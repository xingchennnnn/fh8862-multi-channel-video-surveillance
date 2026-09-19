#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>

#include "control_server.h"
#include "sensor.h"

/*
 * 外部全局变量 — 在 main.c 中定义，用于跨文件访问 OSD 和录像状态。
 *
 * g_osd_user_color_r/g/b : OSD 文字用户颜色 (outline模式, 0-255)
 * g_osd_invert_color_r/g/b : OSD 文字反色颜色 (invert模式, 自动计算)
 * g_mask_color_r/g/b    : OSD 遮罩颜色 (0-255)
 * g_osd_text            : OSD 自定义文字内容
 * g_osd_text_en         : OSD 文字行使能
 * g_osd_invert_en       : OSD 反色使能 (0=关, 1=按字符, 2=按行)
 * g_record_active       : 录像是否进行中
 * g_record_duration     : 录像剩余秒数
 * g_led_mode            : LED 模式 (0=灭, 1=亮, 2=闪烁)
 * g_rotation            : 旋转模式 (0=正常, 1=旋转180)
 * g_ctrl_listen_port    : 控制端口
 */

/* OSD 相关全局变量 */
extern int g_osd_user_color_r;
extern int g_osd_user_color_g;
extern int g_osd_user_color_b;
extern int g_osd_invert_color_r;
extern int g_osd_invert_color_g;
extern int g_osd_invert_color_b;
extern int g_mask_color_r;
extern int g_mask_color_g;
extern int g_mask_color_b;
extern char g_osd_text[128];
extern int g_osd_text_en;
extern int g_osd_invert_en;

/* 录像相关全局变量 */
extern int g_record_active;
extern int g_record_duration;

/* LED 相关全局变量 */
extern int g_led_mode;
extern int g_led_physical_mode;
extern int g_led_blink_state;
extern int g_led_thread_running;
extern int g_led_auto_mode;
extern int g_led_record_refcount;
extern void *led_blink_thread(void *);
extern int g_rotation;

/* OSD 层ID (sample_set_osd 使用的) */
extern int g_osd_layer_id;

/* 函数声明 - 在 main.c 中实现 */
extern int sample_set_osd(int chan);
extern int sample_update_osd_color(int chan, int layer_id, int r, int g, int b);
extern int sample_update_osd_invert(int chan, int layer_id, int invert_en);
extern int sample_update_osd_text(int chan, int layer_id, int line_id, const char *text, int enable);
extern int mask_set_region(int idx, int enable, int x, int y, int w, int h);
extern int mask_del_region(int idx);
extern int mask_set_type(int type);
extern int mask_set_mosaic_size(int size);
extern int mask_set_color(int r, int g, int b);
extern int mask_set_master_enable(int en);

static int g_ctrl_running = 0;
static int g_ctrl_fd = -1;
static pthread_t g_ctrl_thread;

/* 去除字符串末尾的 \r\n */
static void trim(char *s)
{
	char *p = s + strlen(s) - 1;
	while (p >= s && (*p == '\r' || *p == '\n'))
	{
		*p = '\0';
		p--;
	}
}

/* 解析并执行控制命令 */
static void handle_command(const char *cmd)
{
	char buf[256];
	strncpy(buf, cmd, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	trim(buf);

	if (strncmp(buf, "ROTATE", 6) == 0)
	{
		int val;
		if (sscanf(buf + 6, "%d", &val) == 1)
		{
			g_rotation = val;
			if (val == 1)
			{
				/* 旋转180度: mirror=1, flip=1 */
				isp_set_mirrorflip(3);
				printf("[CTRL] Rotate 180: ON (mirror+flip)\n");
			}
			else
			{
				/* 恢复正常 */
				isp_set_mirrorflip(0);
				printf("[CTRL] Rotate 180: OFF\n");
			}
		}
	}
	else if (strncmp(buf, "OSD_COLOR", 9) == 0)
	{
		int r = 255, g = 255, b = 255;
		sscanf(buf + 9, "%d %d %d", &r, &g, &b);
		if (r < 0) r = 0;
		if (r > 255) r = 255;
		if (g < 0) g = 0;
		if (g > 255) g = 255;
		if (b < 0) b = 0;
		if (b > 255) b = 255;
		g_osd_user_color_r = r;
		g_osd_user_color_g = g;
		g_osd_user_color_b = b;
		sample_update_osd_color(0, g_osd_layer_id, r, g, b);
		printf("[CTRL] OSD color set to R=%d G=%d B=%d\n", r, g, b);
	}
	else if (strncmp(buf, "MASK_COLOR", 10) == 0)
	{
		int r = 0, g = 0, b = 0;
		sscanf(buf + 10, "%d %d %d", &r, &g, &b);
		if (r < 0) r = 0;
		if (r > 255) r = 255;
		if (g < 0) g = 0;
		if (g > 255) g = 255;
		if (b < 0) b = 0;
		if (b > 255) b = 255;
		mask_set_color(r, g, b);
		printf("[CTRL] Mask color set to R=%d G=%d B=%d\n", r, g, b);
	}
	else if (strncmp(buf, "MASK_ENABLE", 11) == 0)
	{
		int en = 0;
		sscanf(buf + 11, "%d", &en);
		mask_set_master_enable(en);
		printf("[CTRL] Mask master enable: %d\n", en);
	}
	else if (strncmp(buf, "MASK_REGION_SET", 15) == 0)
	{
		int idx, en, x, y, w, h;
		if (sscanf(buf + 15, "%d %d %d %d %d %d", &idx, &en, &x, &y, &w, &h) == 6)
		{
			mask_set_region(idx, en, x, y, w, h);
			printf("[CTRL] Mask region %d set: en=%d (%d,%d %dx%d)\n", idx, en, x, y, w, h);
		}
	}
	else if (strncmp(buf, "MASK_REGION_DEL", 15) == 0)
	{
		int idx;
		if (sscanf(buf + 15, "%d", &idx) == 1)
		{
			mask_del_region(idx);
			printf("[CTRL] Mask region %d deleted\n", idx);
		}
	}
	else if (strncmp(buf, "MASK_TYPE", 9) == 0)
	{
		int type;
		if (sscanf(buf + 9, "%d", &type) == 1)
		{
			mask_set_type(type);
			printf("[CTRL] Mask type: %d (0=solid, 1=mosaic)\n", type);
		}
	}
	else if (strncmp(buf, "MASK_MOSAIC_SIZE", 16) == 0)
	{
		int size;
		if (sscanf(buf + 16, "%d", &size) == 1)
		{
			mask_set_mosaic_size(size);
			printf("[CTRL] Mask mosaic size: %d\n", size);
		}
	}
	else if (strncmp(buf, "OSD_TEXT_EN", 11) == 0)
	{
		int en = 1;
		sscanf(buf + 11, "%d", &en);
		g_osd_text_en = en;
		sample_update_osd_text(0, g_osd_layer_id, 0, g_osd_text, en);
		printf("[CTRL] OSD text enable: %d\n", en);
	}
	else if (strncmp(buf, "OSD_TEXT ", 9) == 0)
	{
		/* OSD_TEXT 后跟文字内容 (GB2312 原始字节，仅支持 ASCII/英文) */
		const char *text = buf + 9;
		while (*text == ' ') text++;
		strncpy(g_osd_text, text, sizeof(g_osd_text) - 1);
		g_osd_text[sizeof(g_osd_text) - 1] = '\0';
		g_osd_text_en = 1;
		sample_update_osd_text(0, g_osd_layer_id, 0, g_osd_text, 1);
		printf("[CTRL] OSD text set to: %s\n", g_osd_text);
	}
	else if (strncmp(buf, "OSD_TEXT_HEX ", 13) == 0)
	{
		/* 接收十六进制编码的 GB2312 字符串，解码后直接使用 */
		const char *hex = buf + 13;
		while (*hex == ' ') hex++;
		int hex_len = strlen(hex);
		int i, byte_idx = 0;
		for (i = 0; i + 1 < hex_len && byte_idx < (int)sizeof(g_osd_text) - 1; i += 2)
		{
			unsigned int val;
			if (sscanf(hex + i, "%2x", &val) == 1)
			{
				g_osd_text[byte_idx++] = (char)(val & 0xFF);
			}
		}
		g_osd_text[byte_idx] = '\0';
		g_osd_text_en = 1;
		sample_update_osd_text(0, g_osd_layer_id, 0, g_osd_text, 1);
		printf("[CTRL] OSD text set (hex, %d bytes)\n", byte_idx);
	}
	else if (strncmp(buf, "OSD_INVERT", 10) == 0)
	{
		int val;
		if (sscanf(buf + 10, "%d", &val) == 1)
		{
			/* 网页: 0=关闭, 1=按行反色。硬件: 0=关, 2=按行 */
			int hw_val = (val == 1) ? 2 : 0;
			g_osd_invert_en = hw_val;
			sample_update_osd_invert(0, g_osd_layer_id, hw_val);
			printf("[CTRL] OSD invert set to: %d (hw=%d)\n", val, hw_val);
		}
	}
	else if (strncmp(buf, "RECORD_START", 12) == 0)
	{
		int duration = 60;
		sscanf(buf + 12, "%d", &duration);
		if (duration <= 0) duration = 60;
		if (duration > 3600) duration = 3600;
		if (!g_record_active)
			control_led_record_begin();
		g_record_duration = duration;
		g_record_active = 1;
		printf("[CTRL] Recording started, duration=%d seconds\n", duration);
	}
	else if (strncmp(buf, "RECORD_STOP", 11) == 0)
	{
		if (g_record_active)
			control_led_record_end();
		g_record_active = 0;
		g_record_duration = 0;
		printf("[CTRL] Recording stopped\n");
	}
	else if (strncmp(buf, "LED_RECORD_START", 16) == 0)
	{
		control_led_record_begin();
		printf("[CTRL] LED record override started\n");
	}
	else if (strncmp(buf, "LED_RECORD_STOP", 15) == 0)
	{
		control_led_record_end();
		printf("[CTRL] LED record override stopped\n");
	}
	else if (strncmp(buf, "TIME_SYNC", 9) == 0)
	{
		long long timestamp = 0;
		if (sscanf(buf + 9, "%lld", &timestamp) == 1)
		{
			control_time_sync(timestamp);
			printf("[CTRL] Time synced to: %lld\n", timestamp);
		}
	}
	else if (strncmp(buf, "LED", 3) == 0)
	{
		int mode;
		if (sscanf(buf + 3, "%d", &mode) == 1)
		{
			if (mode == 3)
			{
				/* LED 3 = 恢复自动模式 */
				g_led_auto_mode = 1;
				printf("[CTRL] LED: resume auto mode\n");
			}
			else
			{
				g_led_auto_mode = 0;  /* 手动模式覆盖 */
				control_led_set(mode);
				printf("[CTRL] LED mode set to: %d (manual)\n", mode);
			}
		}
	}
	else
	{
		printf("[CTRL] Unknown command: %s\n", buf);
	}
}

/*
 * LED 控制: 通过 /dev/helloworld 驱动 ioctl 控制 GPIO43。
 * g_led_mode 是自动/手动流程的基础状态；录像期间物理 LED 强制闪烁，
 * 但 control_led_set 仍会更新基础状态，录像结束后恢复到该状态。
 */
#define LED_IOC_MAGIC 'L'
#define SET_LED_OFF    _IO(LED_IOC_MAGIC, 1)
#define SET_LED_ON     _IO(LED_IOC_MAGIC, 2)

static int led_apply_physical(int mode)
{
	int fd;
	if (mode != 1 && mode != 2)
		mode = 0;

	fd = open("/dev/helloworld", O_RDWR);
	if (fd < 0)
	{
		printf("[CTRL] LED: cannot open /dev/helloworld (%s)\n", strerror(errno));
		return -1;
	}

	g_led_physical_mode = mode;
	if (mode == 2)
	{
		ioctl(fd, SET_LED_ON);
		if (!g_led_thread_running)
		{
			g_led_thread_running = 1;
			{
				pthread_attr_t attr;
				pthread_t tid;
				pthread_attr_init(&attr);
				pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
				pthread_attr_setstacksize(&attr, 32 * 1024);
				pthread_create(&tid, &attr, led_blink_thread, NULL);
				pthread_attr_destroy(&attr);
			}
			printf("[CTRL] LED: blink thread started\n");
		}
	}
	else
	{
		g_led_thread_running = 0;
		if (mode == 1)
			ioctl(fd, SET_LED_ON);
		else
			ioctl(fd, SET_LED_OFF);
	}

	close(fd);
	printf("[CTRL] LED: physical mode=%d\n", mode);
	return 0;
}

int control_led_set(int mode)
{
	if (mode != 1 && mode != 2)
		mode = 0;

	g_led_mode = mode;
	if (g_led_record_refcount > 0)
	{
		printf("[CTRL] LED: base mode=%d, recording override keeps blinking (refs=%d)\n",
		       mode, g_led_record_refcount);
		if (g_led_physical_mode != 2)
			return led_apply_physical(2);
		return 0;
	}

	printf("[CTRL] LED: base mode=%d\n", mode);
	return led_apply_physical(mode);
}

int control_led_record_begin(void)
{
	if (g_led_record_refcount < 0)
		g_led_record_refcount = 0;

	g_led_record_refcount++;
	printf("[CTRL] LED: record override begin, refs=%d, base=%d\n",
	       g_led_record_refcount, g_led_mode);

	if (g_led_physical_mode != 2)
		return led_apply_physical(2);
	return 0;
}

int control_led_record_end(void)
{
	if (g_led_record_refcount <= 0)
	{
		g_led_record_refcount = 0;
		printf("[CTRL] LED: record override stop ignored, refs=0\n");
		return 0;
	}

	g_led_record_refcount--;
	printf("[CTRL] LED: record override end, refs=%d, restore base=%d\n",
	       g_led_record_refcount, g_led_mode);

	if (g_led_record_refcount == 0)
		return led_apply_physical(g_led_mode);
	return 0;
}


/*
 * 时间同步: 将前端传来的 Unix 毫秒时间戳设置到板端系统时间
 *
 * 线程安全注意事项:
 *   - 使用 setenv() 而非 putenv()，因为 putenv() 不复制字符串, 会将
 *     字符串字面量指针直接存入环境变量表, 后续环境变量操作可能尝试
 *     free() 该指针导致 SIGSEGV (字符串字面量在只读 .rodata 段)
 *   - 不使用 system() 因为它不是线程安全的 (会篡改进程信号处理),
 *     改用 fork+exec 执行 hwclock
 *   - 跳变系统时钟可能影响 RTSP 推流的 PTS 计算 (gettimeofday),
 *     但这是时钟同步的本质特征, 无法避免
 */
int control_time_sync(long long timestamp_ms)
{
	struct timespec ts;
	int ret;

	ts.tv_sec = (time_t)(timestamp_ms / 1000);
	ts.tv_nsec = (long)((timestamp_ms % 1000) * 1000000);

	/* 方法1: clock_settime (POSIX) */
	ret = clock_settime(CLOCK_REALTIME, &ts);
	if (ret != 0)
	{
		/* 方法2: settimeofday (BSD) */
		struct timeval tv;
		tv.tv_sec = ts.tv_sec;
		tv.tv_usec = ts.tv_nsec / 1000;
		ret = settimeofday(&tv, NULL);
	}

	if (ret != 0)
	{
		printf("[CTRL] time sync failed: %s\n", strerror(errno));
		return -1;
	}

	/* 设置时区为中国标准时间 UTC+8
	 * 使用 setenv 而非 putenv: setenv 会在堆上分配副本,
	 * 避免 putenv 直接将 .rodata 指针存入环境表导致后续 free 崩溃 */
	setenv("TZ", "CST-8", 1);
	tzset();
	printf("[CTRL] TZ set to CST-8\n");

	/* 同步硬件 RTC: 用 fork+exec 代替 system() 以保证线程安全
	 * system() 会临时修改 SIGCHLD/SIGINT/SIGQUIT 信号处理,
	 * 在多线程程序中可能导致其他线程收到意外的信号 */
	{
		pid_t pid = fork();
		if (pid == 0)
		{
			/* 子进程: 重定向 stderr 到 /dev/null, 执行 hwclock -w */
			int null_fd = open("/dev/null", O_WRONLY);
			if (null_fd >= 0)
			{
				dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}
			execl("/sbin/hwclock", "hwclock", "-w", (char *)NULL);
			_exit(1);
		}
		else if (pid > 0)
		{
			waitpid(pid, NULL, 0);
		}
	}

	printf("[CTRL] Time synced: %ld\n", (long)ts.tv_sec);
	return 0;
}

/* 控制服务器工作线程 */
static void *control_thread_func(void *arg)
{
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);
	char buf[512];
	int n;

	(void)arg;

	while (g_ctrl_running)
	{
		client_fd = accept(g_ctrl_fd, (struct sockaddr *)&client_addr, &addr_len);
		if (client_fd < 0)
		{
			if (!g_ctrl_running) break;
			usleep(100000);
			continue;
		}

		printf("[CTRL] Client connected from %s:%d\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		/* 读取客户端命令，每行一条 */
		memset(buf, 0, sizeof(buf));
		n = read(client_fd, buf, sizeof(buf) - 1);
		if (n > 0)
		{
			buf[n] = '\0';

			/* 处理多行命令 */
			char *line = strtok(buf, "\n");
			while (line != NULL)
			{
				handle_command(line);
				line = strtok(NULL, "\n");
			}

			/* 返回 OK */
			write(client_fd, "OK\n", 3);
		}

		close(client_fd);
	}

	return NULL;
}

int control_server_start(int port)
{
	struct sockaddr_in addr;
	int opt = 1;

	if (g_ctrl_running)
	{
		return 0;
	}

	g_ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_ctrl_fd < 0)
	{
		printf("[CTRL] Failed to create socket: %s\n", strerror(errno));
		return -1;
	}

	setsockopt(g_ctrl_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(g_ctrl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		printf("[CTRL] Failed to bind port %d: %s\n", port, strerror(errno));
		close(g_ctrl_fd);
		return -1;
	}

	if (listen(g_ctrl_fd, 5) < 0)
	{
		printf("[CTRL] Failed to listen: %s\n", strerror(errno));
		close(g_ctrl_fd);
		return -1;
	}

	g_ctrl_running = 1;

	{
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 64 * 1024); /* 64KB, 避免 OOM */
		if (pthread_create(&g_ctrl_thread, &attr, control_thread_func, NULL) != 0)
		{
			printf("[CTRL] Failed to create control thread\n");
			close(g_ctrl_fd);
			g_ctrl_running = 0;
			return -1;
		}
		pthread_attr_destroy(&attr);
	}

	printf("[CTRL] Control server started on port %d\n", port);
	return 0;
}

void control_server_stop(void)
{
	g_ctrl_running = 0;

	if (g_ctrl_fd >= 0)
	{
		shutdown(g_ctrl_fd, SHUT_RDWR);
		close(g_ctrl_fd);
		g_ctrl_fd = -1;
	}
}
