/*
 * ============================================================================
 * RTSP 视频流服务器 — 主程序入口 (Fullhan FH8862)
 * ============================================================================
 *
 * 功能概述:
 *   本程序实现了一个完整的 4K 摄像头视频采集、编码、推流系统。基于 Fullhan FH8862
 *   芯片平台 (ARM Cortex-A7)，通过 MIPI CSI 接口采集传感器图像，经 ISP 处理、
 *   VPSS 缩放后送入硬件 H.264 编码器，最终通过 RTSP 协议对外推流。
 *
 * 数据流管道 (Pipeline):
 *
 *   IMX415/OVOS02D 传感器
 *        │  MIPI CSI (4 lanes)
 *        ▼
 *   ┌─────────┐
 *   │   ISP    │  ISP_MEM_INIT → 设置VI属性 → 注册sensor回调 → 加载ISP参数 → ISP_Run
 *   │ (VICAP)  │  输出: 3840×2160 YUV422, 30fps
 *   └────┬─────┘
 *        │ FH_SYS_Bind(ISP → VPU_VI)
 *        ▼
 *   ┌─────────┐
 *   │  VPSS    │  FH_VPSS_CreateGrp → 主通道(1280×720) + 子通道(704×576)
 *   │ (VPU)    │  缩放 + 帧率控制(25fps) + 背景建模 + OSD叠加
 *   └────┬─────┘
 *        │ FH_SYS_Bind(VPU_VO → ENC)
 *        ▼
 *   ┌─────────┐
 *   │  VENC    │  H.264 Main Profile: CBR码率控制, I帧间隔50
 *   │ (编码器) │  Chan0: 主码流 1280×720@25fps 2Mbps
 *   │          │  Chan1: 子码流 704×576@25fps  700kbps
 *   └────┬─────┘
 *        │ FH_VENC_GetStream_Block()
 *        ▼
 *   ┌─────────────────────────────────────────┐
 *   │      sample_common_get_stream_proc      │  ← 编码推流线程
 *   │                                         │
 *   │  每帧数据两条输出路径:                      │
 *   │  ① dmc_input() → DMC订阅者 (本地录像/   │
 *   │                   PES UDP远程推流)        │
 *   │  ② rtsp_live555_push_frame() → RTSP客户端 │
 *   └─────────────────────────────────────────┘
 *
 * 双码流架构:
 *   - 主码流 (chan0): 1280×720, 2Mbps, 供局域网实时观看
 *   - 子码流 (chan1): 704×576,  700kbps, 供远程或低带宽场景
 *   - RTSP URL:
 *       rtsp://<设备IP>:8554/main  → 主码流 (VLC 播放)
 *       rtsp://<设备IP>:8554/sub   → 子码流
 *
 * 命令行用法:
 *   ./demo                        → 仅启动 RTSP 服务器 (TCP 模式)
 *   ./demo 192.168.1.100          → RTSP + PES UDP 推流到指定 IP:1234
 *   ./demo 192.168.1.100 5678     → RTSP + PES UDP 推流到指定 IP:5678
 *
 *   ⚠ 带 IP 参数时会额外激活 dmc_pes_subscribe()，它使用 libpes.c 中的
 *     静态全局变量 g_frame_length/g_nalu_count/g_stream_element 来缓存
 *     跨回调的分片数据。由于双码流(chan0+chan1)的回调共享这些全局变量，
 *     存在数据覆盖风险，可能导致 5 分钟左右的内存溢出崩溃。
 *
 * 关键宏开关:
 *   ENABLE_LOCAL_RECORD  0/1 — 是否启用本地 MP4 录像 (/home/目录)
 *   ENABLE_VPSS_MOSAIC_MASK 0/1 — 是否启用隐私区域马赛克遮挡
 *
 * 硬件绑定 (FH_SYS_Bind):
 *   Fullhan SDK 的数据流通过"绑定"机制连接各模块，源设备输出直接送入目标设备
 *   输入，无需 CPU 干预，实现零拷贝硬件流水线:
 *     ISP.ch0 → VPU_VI.ch0     (ISP 输出送入 VPU 输入)
 *     VPU_VO.ch0 → ENC.ch0    (主码流通道 → 编码器通道0)
 *     VPU_VO.ch1 → ENC.ch1    (子码流通道 → 编码器通道1)
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

#include "fh_system_mpi.h"
#include "fh_vpu_mpi.h"
#include "fh_venc_mpi.h"
#include "FHAdv_OSD_mpi.h"
#include "fh_vb_mpipara.h"
#include "fh_vb_mpi.h"

#include "rtsp_live555.h"
#include "control_server.h"

#include "libdmc.h"
#include "libdmc_pes.h"
#include "libdmc_record_raw.h"
#include "font_array.h"
#include "sensor.h"

#include "vicap/fh_vicap_mpi.h"

/*
 * 向上对齐宏: 将 addr 对齐到 edge 的整数倍边界
 * 例如 ALIGN_UP(13, 8) = 16, ALIGN_UP(3842, 64) = 3904
 * 用于确保图像行跨度(stride)满足硬件对齐要求
 */
#define ALIGN_UP(addr, edge) ((addr + edge - 1) & ~(edge - 1))
/*
 * 向下对齐宏: 将 addr 向下舍入到 edge 的整数倍边界
 * 例如 ALIGN_BACK(19, 8) = 16, ALIGN_BACK(4095, 4096) = 0
 */
#define ALIGN_BACK(addr, edge) ((edge) * (((addr) / (edge))))

/*
 * 内核驱动 proc 调试接口路径
 * Fullhan SDK 通过 /proc/driver/ 下的虚拟文件与内核驱动通信，
 * 用于配置硬件模块参数和性能追踪。
 */
#define ISP_PROC "/proc/driver/isp"
#define VPU_PROC "/proc/driver/vpu"
#define BGM_PROC "/proc/driver/bgm"
#define ENC_PROC "/proc/driver/enc"
#define JPEG_PROC "/proc/driver/jpeg"
#define TRACE_PROC "/proc/driver/trace"

/*
 * 向内核驱动写入配置命令的封装宏。
 *
 * Fullhan SDK 的驱动参数通过写入 proc 文件系统来配置。此宏封装了
 * open → write → close 的操作，如果设备文件不存在则静默跳过。
 *
 * 典型用法:
 *   WR_PROC_DEV(ENC_PROC, "stm_20000000_40");
 *   → 向编码器驱动写入码流配置 (码率20000000, 帧率40)
 *
 * 在追踪场景中用于标记关键路径耗时:
 *   WR_PROC_DEV(TRACE_PROC, "timing_GetStream_START");  // 开始取流
 *   ...编码器取流操作...
 *   WR_PROC_DEV(TRACE_PROC, "timing_GetStream_END");    // 取流结束
 */
#define WR_PROC_DEV(device, cmd)              \
	do                                        \
	{                                         \
		int _tmp_fd;                          \
		_tmp_fd = open(device, O_RDWR, 0);    \
		if (_tmp_fd >= 0)                     \
		{                                     \
			write(_tmp_fd, cmd, sizeof(cmd)); \
			close(_tmp_fd);                   \
		}                                     \
	} while (0)

/*
 * 信号处理标志。
 *
 * ⚠ g_sig_stop 在信号处理函数中异步写入，主线程中读取。
 *   类型应为 volatile sig_atomic_t 以保证严格符合 C 标准。
 *   当前 int 类型在 ARM Cortex-A7 上对齐读写通常是安全的，但不是标准保证。
 */
static int g_sig_stop = 0;

/*
 * 信号处理回调: 响应 SIGINT(Ctrl+C) / SIGTERM 等终止信号。
 * 仅设置全局标志位，由主循环检测并执行优雅退出。
 *
 * ⚠ SIGKILL (行后文 main 中) 不能被捕获或忽略，
 *   signal(SIGKILL, ...) 调用是无效的静默返回。
 */
static void sample_vlcview_handle_sig(int signo)
{
	g_sig_stop = 1;
	rtsp_live555_stop(); /* 通知 live555 事件循环退出 */
}

/* ISP 传感器操作接口，由 start_isp() 填充回调函数指针并注册到 ISP 驱动 */
struct isp_sensor_if sensor_func;

/*
 * ISP 输入/输出分辨率定义
 *
 * ISP_W0/H0 = 3864×2192: MIPI CSI 输入帧尺寸 (传感器原始数据)
 * ISP_W/H   = 3840×2160: ISP 裁剪后输出尺寸 (标准 4K UHD)
 * ISP_F     = 30:        传感器帧率 (30fps，后续 VPSS 降到 25fps)
 *
 * 裁剪区域: (3864-3840)/2 = 12 列, (2192-2160)/2 = 16 行
 * 去掉边缘的非有效像素区域
 */
#define ISP_W0 3864
#define ISP_H0 2192
#define ISP_W 3840
#define ISP_H 2160
#define ISP_F 30 // 图像帧率

/*
 * 编译期功能开关
 *
 * ENABLE_VPSS_MOSAIC_MASK: 隐私区域马赛克遮挡
 *   0 = 关闭, 1 = 对指定 ROI 区域进行马赛克模糊处理
 *   配置见 main() 中 #if ENABLE_VPSS_MOSAIC_MASK 块
 *
 * OSD_FONT_SIZE: OSD 叠加文字的字号 (像素单位)
 *   32 = 每个字符占 32×32 像素区域
 *
 * ENABLE_LOCAL_RECORD: 本地录像保存
 *   0 = 关闭, 1 = 启用 dmc_record_subscribe() 写入 /home/ 目录
 */
#define ENABLE_VPSS_MOSAIC_MASK 1
#define OSD_FONT_SIZE 32
#define ENABLE_LOCAL_RECORD 0
#define GROUP_ID 0

/* LED 驱动 ioctl */
#define LED_IOC_MAGIC 'L'
#define SET_LED_OFF    _IO(LED_IOC_MAGIC, 1)
#define SET_LED_ON     _IO(LED_IOC_MAGIC, 2)

/*
 * 控制命令相关全局变量 (由 control_server.c 通过 TCP 命令更新)
 */
int g_osd_user_color_r = 255;
int g_osd_user_color_g = 255;
int g_osd_user_color_b = 255;
int g_osd_invert_color_r = 255;
int g_osd_invert_color_g = 255;
int g_osd_invert_color_b = 255;
/*
 * 遮罩(Mask)管理 — 软件层状态，与硬件 FH_VPU_MASK 解耦。
 * 硬件限制: color 和 masaic 是全局字段，所有区域共用。
 */
#define MASK_MAX_REGIONS 8

typedef struct {
	int enable;
	int x, y, w, h;
} mask_region_t;

typedef struct {
	mask_region_t regions[MASK_MAX_REGIONS];
	int type;           /* 0=纯色, 1=马赛克 */
	int mosaic_size;    /* 0=16x16, 1=32x32, 2=64x64 */
	int color_r, color_g, color_b;
	int master_enable;
} mask_state_t;

static mask_state_t g_mask_state;

int g_mask_color_r = 0;
int g_mask_color_g = 255;
int g_mask_color_b = 0;
char g_osd_text[128] = "\xD3\xB0\xC1\xF7\xB6\xD3"; /* GB2312: 影流队 */
int g_osd_text_en = 1;
int g_osd_invert_en = 0;

/* 时间戳格式字符串 (硬件控制字符), 在 sample_set_osd 中初始化 */
FH_CHAR g_osd_time_fmt[32] = {0};
/* 静态文本行 (line2/3), 在 sample_set_osd 中初始化 */
static char g_osd_line2_text[64] = "Hangzhou Dianzi University";
static char g_osd_line3_text[32] = "Hikvision";
int g_record_active = 0;
int g_record_duration = 0;
int g_led_mode = 0;          /* 基础状态: 自动/手动流程期望的 LED 模式 */
int g_led_physical_mode = 0; /* 物理状态: 当前真正输出到 GPIO 的 LED 模式 */
int g_led_blink_state = 0;
int g_led_auto_mode = 1;
int g_led_auto_thread_running = 0;
int g_led_record_refcount = 0;
int g_rotation = 0;
int g_osd_layer_id = 0;

/* OSD 互斥锁 + 反色线程控制 */
static pthread_mutex_t g_osd_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_led_thread_running = 0;
static int g_invert_thread_running = 0;

/* 录像文件句柄 */
static FILE *g_record_file = NULL;
static time_t g_record_start_time = 0;

/*
 * 重新注册所有 4 行 OSD 文本 (在 layer 配置变更后调用)。
 * FHAdv_Osd_Ex_SetText 会重置 layer 配置，所有 text line 需要重新绑定。
 */
static int osd_reapply_all_lines(int layer_id)
{
	FHT_OSD_TextLine_t text_line;
	int ret;

	/* Line 0: 用户自定义文字 */
	memset(&text_line, 0, sizeof(text_line));
	text_line.textInfo = g_osd_text;
	text_line.textEnable = (FH_UINT8)(g_osd_text_en ? 1 : 0);
	text_line.timeOsdEnable = 0;
	text_line.textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line.linePositionX = 20;
	text_line.linePositionY = 20;
	text_line.lineId = 0;
	text_line.enable = (FH_UINT8)(g_osd_text_en ? 1 : 0);
	ret = FHAdv_Osd_SetTextLine(0, 0, (FH_UINT32)layer_id, &text_line);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 1, (FH_UINT32)layer_id, &text_line);

	/* Line 1: 时间戳 (硬件控制字符) */
	memset(&text_line, 0, sizeof(text_line));
	text_line.textInfo = g_osd_time_fmt;
	text_line.textEnable = 1;
	text_line.timeOsdEnable = 0;
	text_line.textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line.linePositionX = 20;
	text_line.linePositionY = 100;
	text_line.lineId = 1;
	text_line.enable = 1;
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 0, (FH_UINT32)layer_id, &text_line);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 1, (FH_UINT32)layer_id, &text_line);

	/* Line 2: 学校名 */
	memset(&text_line, 0, sizeof(text_line));
	text_line.textInfo = g_osd_line2_text;
	text_line.textEnable = 1;
	text_line.timeOsdEnable = 0;
	text_line.textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line.linePositionX = 20;
	text_line.linePositionY = 180;
	text_line.lineId = 2;
	text_line.enable = 1;
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 0, (FH_UINT32)layer_id, &text_line);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 1, (FH_UINT32)layer_id, &text_line);

	/* Line 3: 厂商名 */
	memset(&text_line, 0, sizeof(text_line));
	text_line.textInfo = g_osd_line3_text;
	text_line.textEnable = 1;
	text_line.timeOsdEnable = 0;
	text_line.textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line.linePositionX = 20;
	text_line.linePositionY = 260;
	text_line.lineId = 3;
	text_line.enable = 1;
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 0, (FH_UINT32)layer_id, &text_line);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 1, (FH_UINT32)layer_id, &text_line);

	return ret;
}

/*
 * 动态更新 OSD 文字颜色
 */
int sample_update_osd_color(int chan, int layer_id, int r, int g, int b)
{
	int ret;
	FHT_OSD_CONFIG_t osd_cfg;
	FHT_OSD_Layer_Config_t layer_info;

	pthread_mutex_lock(&g_osd_mutex);

	memset(&osd_cfg, 0, sizeof(osd_cfg));
	memset(&layer_info, 0, sizeof(layer_info));

	osd_cfg.osdRotate = 0;
	osd_cfg.nOsdLayerNum = 1;
	osd_cfg.pOsdLayerInfo = &layer_info;

	layer_info.layerStartX = 0;
	layer_info.layerStartY = 0;
	layer_info.osdSize = OSD_FONT_SIZE;

	layer_info.normalColor.fAlpha = 255;
	if (g_osd_invert_en)
	{
		/* 硬件反色中: 只用白/黑 */
		layer_info.normalColor.fRed = 255;
		layer_info.normalColor.fGreen = 255;
		layer_info.normalColor.fBlue = 255;
		layer_info.invertColor.fAlpha = 255;
		layer_info.invertColor.fRed = 0;
		layer_info.invertColor.fGreen = 0;
		layer_info.invertColor.fBlue = 0;
	}
	else
	{
		layer_info.normalColor.fRed = (FH_UINT8)r;
		layer_info.normalColor.fGreen = (FH_UINT8)g;
		layer_info.normalColor.fBlue = (FH_UINT8)b;
		layer_info.invertColor.fAlpha = 255;
		layer_info.invertColor.fRed = 0;
		layer_info.invertColor.fGreen = 0;
		layer_info.invertColor.fBlue = 0;
	}

	layer_info.edgeColor.fAlpha = 255;
	layer_info.edgeColor.fRed = 0;
	layer_info.edgeColor.fGreen = 0;
	layer_info.edgeColor.fBlue = 0;

	layer_info.bkgColor.fAlpha = 0;
	layer_info.edgePixel = g_osd_invert_en ? 0 : 1;
	layer_info.osdInvertEnable = (FH_UINT8)g_osd_invert_en;
	layer_info.osdInvertThreshold.high_level = 180;
	layer_info.osdInvertThreshold.low_level = 160;
	layer_info.layerFlag = FH_OSD_LAYER_USE_TWO_BUF;
	layer_info.layerId = (FH_UINT8)layer_id;

	ret = FHAdv_Osd_Ex_SetText(0, 0, &osd_cfg);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_Ex_SetText(0, 1, &osd_cfg);
	if (ret != FH_SUCCESS)
	{
		printf("[OSD] color update failed: %d\n", ret);
		pthread_mutex_unlock(&g_osd_mutex);
		return ret;
	}

	/* 重新注册所有 4 行文字 (layer 配置变更后 text line 会失效) */
	osd_reapply_all_lines(layer_id);

	pthread_mutex_unlock(&g_osd_mutex);
	return 0;
}

/*
 * 动态更新 OSD 反色使能
 */
int sample_update_osd_invert(int chan, int layer_id, int invert_en)
{
	int ret;
	FHT_OSD_CONFIG_t osd_cfg;
	FHT_OSD_Layer_Config_t layer_info;

	pthread_mutex_lock(&g_osd_mutex);

	memset(&osd_cfg, 0, sizeof(osd_cfg));
	memset(&layer_info, 0, sizeof(layer_info));

	osd_cfg.osdRotate = 0;
	osd_cfg.nOsdLayerNum = 1;
	osd_cfg.pOsdLayerInfo = &layer_info;

	layer_info.layerStartX = 0;
	layer_info.layerStartY = 0;
	layer_info.osdSize = OSD_FONT_SIZE;

	layer_info.normalColor.fAlpha = 255;
	if (invert_en)
	{
		/* 硬件反色: normalColor=白(暗背景用), invertColor=黑(亮背景用) */
		layer_info.normalColor.fRed = 255;
		layer_info.normalColor.fGreen = 255;
		layer_info.normalColor.fBlue = 255;

		layer_info.invertColor.fAlpha = 255;
		layer_info.invertColor.fRed = 0;
		layer_info.invertColor.fGreen = 0;
		layer_info.invertColor.fBlue = 0;
	}
	else
	{
		layer_info.normalColor.fRed = (FH_UINT8)g_osd_user_color_r;
		layer_info.normalColor.fGreen = (FH_UINT8)g_osd_user_color_g;
		layer_info.normalColor.fBlue = (FH_UINT8)g_osd_user_color_b;

		layer_info.invertColor.fAlpha = 255;
		layer_info.invertColor.fRed = 0;
		layer_info.invertColor.fGreen = 0;
		layer_info.invertColor.fBlue = 0;
	}

	layer_info.edgeColor.fAlpha = 255;
	layer_info.edgeColor.fRed = 0;
	layer_info.edgeColor.fGreen = 0;
	layer_info.edgeColor.fBlue = 0;

	layer_info.bkgColor.fAlpha = 0;
	layer_info.edgePixel = invert_en ? 0 : 1;
	/* 硬件自动反色: 暗背景亮字(白), 亮背景暗字(黑) */
	layer_info.osdInvertEnable = (FH_UINT8)(invert_en ? FH_OSD_INVERT_BY_CHAR : FH_OSD_INVERT_DISABLE);
	layer_info.osdInvertThreshold.high_level = 180;
	layer_info.osdInvertThreshold.low_level = 140;
	layer_info.layerFlag = FH_OSD_LAYER_USE_TWO_BUF;
	layer_info.layerId = (FH_UINT8)layer_id;

	ret = FHAdv_Osd_Ex_SetText(0, 0, &osd_cfg);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_Ex_SetText(0, 1, &osd_cfg);
	if (ret != FH_SUCCESS)
	{
		printf("[OSD] invert update failed: %d\n", ret);
		pthread_mutex_unlock(&g_osd_mutex);
		return ret;
	}

	/* 重新注册所有 4 行文字 (layer 配置变更后 text line 会失效) */
	osd_reapply_all_lines(layer_id);

	pthread_mutex_unlock(&g_osd_mutex);
	return 0;
}

/*
 * 动态更新 OSD 文字内容
 */
int sample_update_osd_text(int chan, int layer_id, int line_id, const char *text, int enable)
{
	int ret;
	FHT_OSD_TextLine_t text_line;

	pthread_mutex_lock(&g_osd_mutex);

	memset(&text_line, 0, sizeof(text_line));

	text_line.textInfo = (FH_CHAR *)text;
	text_line.textEnable = (FH_UINT8)(enable ? 1 : 0);
	text_line.timeOsdEnable = 0;
	text_line.textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line.linePositionX = 20;
	text_line.linePositionY = 20;
	text_line.lineId = (FH_UINT8)line_id;
	text_line.enable = (FH_UINT8)(enable ? 1 : 0);

	ret = FHAdv_Osd_SetTextLine(0, 0, (FH_UINT32)layer_id, &text_line);
	if (ret == FH_SUCCESS)
		ret = FHAdv_Osd_SetTextLine(0, 1, (FH_UINT32)layer_id, &text_line);

	pthread_mutex_unlock(&g_osd_mutex);
	return ret;
}

/*
 * RGB → AYCbCr VPU mask 颜色转换
 * 硬件格式: [A][Y][Cb][Cr] → (Cr<<24)|(Cb<<16)|(Y<<8)|A
 */
static unsigned int rgb_to_mask_color(int r, int g, int b)
{
	int y, cb, cr;
	y  = ( 77 * r + 150 * g +  29 * b) >> 8;
	cb = ((-43 * r -  85 * g + 128 * b) >> 8) + 128;
	cr = ((128 * r - 107 * g -  21 * b) >> 8) + 128;
	if (y  < 0) y  = 0;
	if (y  > 255) y  = 255;
	if (cb < 0) cb = 0;
	if (cb > 255) cb = 255;
	if (cr < 0) cr = 0;
	if (cr > 255) cr = 255;
	return ((unsigned int)y << 16) | ((unsigned int)cb << 8) | (unsigned int)cr;
}

/* ============================================================================
 * Mask 管理 — 软件层状态 → 硬件 FH_VPSS_SetMask
 *
 * FH8862 VPSS mask 硬件限制:
 *   - color 是全局单字段，所有区域共用一种颜色
 *   - masaic 是全局单字段，所有区域共用一种类型(纯色/马赛克)
 *   - 最多 8 个区域 (MAX_MASK_AREA=8)，每区域独立坐标/使能
 * ============================================================================ */

static void mask_state_init(void)
{
	memset(&g_mask_state, 0, sizeof(g_mask_state));
	g_mask_state.type = 0;
	g_mask_state.mosaic_size = 1;
	g_mask_state.color_r = 0;
	g_mask_state.color_g = 255;
	g_mask_state.color_b = 0;
	g_mask_state.master_enable = 1;

	/* 默认区域0: (360, 360) 900x480 */
	g_mask_state.regions[0].enable = 1;
	g_mask_state.regions[0].x = 360;
	g_mask_state.regions[0].y = 360;
	g_mask_state.regions[0].w = 900;
	g_mask_state.regions[0].h = 480;

	/* 默认区域1: (2250, 1140) 900x480 */
	g_mask_state.regions[1].enable = 1;
	g_mask_state.regions[1].x = 750 * 3;
	g_mask_state.regions[1].y = 380 * 3;
	g_mask_state.regions[1].w = 900;
	g_mask_state.regions[1].h = 480;
}

int mask_flush_to_hardware(void)
{
	FH_VPU_MASK hw;
	int i;

	memset(&hw, 0, sizeof(hw));

	if (g_mask_state.master_enable)
	{
		for (i = 0; i < MASK_MAX_REGIONS; i++)
		{
			if (g_mask_state.regions[i].enable)
			{
				hw.mask_enable[i] = 1;
				hw.area_value[i].u32X      = (FH_UINT32)g_mask_state.regions[i].x;
				hw.area_value[i].u32Y      = (FH_UINT32)g_mask_state.regions[i].y;
				hw.area_value[i].u32Width  = (FH_UINT32)g_mask_state.regions[i].w;
				hw.area_value[i].u32Height = (FH_UINT32)g_mask_state.regions[i].h;
			}
		}
	}

	hw.color = rgb_to_mask_color(g_mask_state.color_r,
	                              g_mask_state.color_g,
	                              g_mask_state.color_b);
	hw.masaic.masaic_enable = (FH_UINT32)g_mask_state.type;
	hw.masaic.masaic_size   = (FH_UINT32)g_mask_state.mosaic_size;

	return FH_VPSS_SetMask(0, &hw);
}

int mask_set_region(int idx, int enable, int x, int y, int w, int h)
{
	if (idx < 0 || idx >= MASK_MAX_REGIONS) return -1;
	g_mask_state.regions[idx].enable = enable;
	g_mask_state.regions[idx].x = x;
	g_mask_state.regions[idx].y = y;
	g_mask_state.regions[idx].w = w;
	g_mask_state.regions[idx].h = h;
	return mask_flush_to_hardware();
}

int mask_del_region(int idx)
{
	if (idx < 0 || idx >= MASK_MAX_REGIONS) return -1;
	memset(&g_mask_state.regions[idx], 0, sizeof(mask_region_t));
	return mask_flush_to_hardware();
}

int mask_set_type(int type)
{
	g_mask_state.type = type;
	return mask_flush_to_hardware();
}

int mask_set_mosaic_size(int size)
{
	if (size < 0 || size > 2) return -1;
	g_mask_state.mosaic_size = size;
	return mask_flush_to_hardware();
}

int mask_set_color(int r, int g, int b)
{
	g_mask_state.color_r = r;
	g_mask_state.color_g = g;
	g_mask_state.color_b = b;
	g_mask_color_r = r;
	g_mask_color_g = g;
	g_mask_color_b = b;
	return mask_flush_to_hardware();
}

int mask_set_master_enable(int en)
{
	g_mask_state.master_enable = en;
	return mask_flush_to_hardware();
}


/*
 * 手动自动反色线程
 */
static FH_VOID *auto_invert_thread(FH_VOID *arg)
{
	(void)arg;
	/* 等待编码管道稳定 */
	sleep(3);
	printf("[INVERT] thread started, invert_en=%d\n", g_osd_invert_en);

	while (g_invert_thread_running)
	{
		sleep(1);
		if (!g_invert_thread_running || g_osd_invert_en == 0) continue;

		FH_VPU_YCMEAN ycmean;
		memset(&ycmean, 0, sizeof(ycmean));
		if (FH_VPSS_GetYCmean(GROUP_ID, &ycmean) != 0)
		{
			continue;
		}
		if (!ycmean.ymean.addr || ycmean.ymean.size < 1 ||
		    !ycmean.cmean.addr || ycmean.cmean.size < 2)
		{
			continue;
		}

		unsigned char *y = ycmean.ymean.addr;
		unsigned char *c = ycmean.cmean.addr;
		int avg_y = y[0], avg_cb = c[0], avg_cr = c[1];

		int r = avg_y + ((359 * (avg_cr - 128)) >> 8);
		int g = avg_y - (( 88 * (avg_cb - 128) + 183 * (avg_cr - 128)) >> 8);
		int b = avg_y + ((454 * (avg_cb - 128)) >> 8);
		if (r < 0) r = 0;
		if (r > 255) r = 255;
		if (g < 0) g = 0;
		if (g > 255) g = 255;
		if (b < 0) b = 0;
		if (b > 255) b = 255;

		int inv_r = 255 - r, inv_g = 255 - g, inv_b = 255 - b;
		if (inv_r == g_osd_invert_color_r && inv_g == g_osd_invert_color_g && inv_b == g_osd_invert_color_b) continue;

		g_osd_invert_color_r = inv_r; g_osd_invert_color_g = inv_g; g_osd_invert_color_b = inv_b;
		sample_update_osd_color(0, g_osd_layer_id, inv_r, inv_g, inv_b);
	}
	return NULL;
}
FH_VOID *led_blink_thread(FH_VOID *arg)
{
	(void)arg;
	int fd = -1;
	while (g_led_thread_running && g_led_physical_mode == 2)
	{
		g_led_blink_state = !g_led_blink_state;
		fd = open("/dev/helloworld", O_RDWR);
		if (fd >= 0)
		{
			if (g_led_blink_state)
				ioctl(fd, SET_LED_ON);
			else
				ioctl(fd, SET_LED_OFF);
			close(fd);
		}
		usleep(500000);
	}
	return NULL;
}

/*
 * LED 自动状态机: 根据 RTSP 客户端连接数自动切换 LED 状态。
 *   0 个客户端 (Mediamtx 未拉流) → LED 灭
 *   ≥1 个客户端 (Mediamtx 正在拉流) → LED 亮
 * 当 Web 前端手动发送 LED 命令时，退出自动模式 (g_led_auto_mode=0)。
 * 发送 LED 3 可恢复自动模式。
 */
static FH_VOID *led_auto_thread(FH_VOID *arg)
{
	(void)arg;
	int prev_count = -1;

	while (g_led_auto_thread_running)
	{
		sleep(2);
		if (!g_led_auto_thread_running) break;
		if (!g_led_auto_mode) continue;

		int count = rtsp_live555_get_client_count();
		if (count == prev_count) continue;
		prev_count = count;

		if (count > 0)
		{
			printf("[LED-AUTO] %d RTSP client(s), switching LED on\n", count);
			control_led_set(1);
		}
		else
		{
			printf("[LED-AUTO] no RTSP clients, switching LED off\n");
			control_led_set(0);
		}
	}
	return NULL;
}

/*
 * 编码推流线程控制标志。
 *
 * g_get_stream_stop:
 *   - 主线程(main)写入 1，通知推流线程退出
 *   - 推流线程退出前写入 0，通知主线程已安全退出
 *   - 双向握手: main 等 g_get_stream_stop 变回 0 才继续清理
 *
 * g_get_stream_running:
 *   - 防止重复创建推流线程的守卫
 *
 * ⚠ 这两个变量在主线程和推流线程间无锁访问，存在数据竞争风险。
 *   虽然在 ARM 上对 int 的读写通常原子化，但严格来说是未定义行为。
 */
static int g_get_stream_stop = 0;
static int g_get_stream_running = 0;

/* VPU 组 ID，整个系统只使用一个 VPU 组 */

/*
 * ISP (Image Signal Processor) 初始化 — 摄像头采集管道的起点。
 *
 * 此函数完成从传感器到 ISP 输出的完整初始化流程，
 * 包含 12 个步骤，按严格的依赖顺序执行:
 *
 *   1. 传感器硬件复位               → isp_sensor_reset()
 *   2. ISP 内存池分配               → API_ISP_MemInit()
 *      配置: Online 模式, 输出格式 YUV422 8bit, 尺寸 3840×2160
 *   3. VI 属性设置 (输入尺寸/帧率)  → API_ISP_SetViAttr()
 *   4. 注册传感器回调函数集         → API_ISP_SensorRegCb()
 *      包含: init, set_fmt, read/write reg, AE/AWB 控制等
 *   5. 传感器初始化                 → API_ISP_SensorInit()
 *   6. ISP 内核驱动初始化           → API_ISP_Init()
 *   7. VICAP VI 设备初始化          → FH_VICAP_InitViDev()
 *   8. VICAP VI 属性 + 裁剪区      → FH_VICAP_SetViAttr()
 *      输入 3864×2192, 裁剪至 3840×2160 (裁掉边缘无效像素)
 *   9. [Offline模式] VICAP→ISP绑定 → FH_SYS_Bind()
 *   10. 从 flash 加载 ISP 参数文件  → fopen(SENSOR_PARAM) + API_ISP_LoadIspParam()
 *       ISP 参数是传感器标定数据，包含黑电平/镜头阴影/白平衡/色彩矩阵等
 *   11. 启动 ISP 处理线程          → isp_server_run()
 *       (src/isp.c: 创建独立线程循环调用 API_ISP_Run)
 *
 * 返回值: 0=成功, 非0=失败 (使用 CHECK_RET 宏就近检查)
 */
int start_isp()
{
	int ret;
	int vimod = 0; // 0:online 1:offline (Online=实时流, Offline=从DDR读图调试)
	int vomod = 1; // 1:to vpu 2:to ddr (输出到VPU做后续处理)

	ISP_MEM_INIT stMemInit = {0};
	ISP_VI_ATTR_S vi_attr = {0};

	ISP_PARAM_CONFIG stIsp_para_cfg;
	unsigned int param_addr, param_size;
	char *isp_param_buff;
	Sensor_Init_t initConf = {0};

	FH_VICAP_DEV_ATTR_S stViDev = {0};
	FH_VICAP_VI_ATTR_S stViAttr = {0};

	FILE *param_file;

	/* 步骤1: 传感器硬件复位，确保 MIPI 接口回到初始状态 */
	isp_sensor_reset();

	/* 步骤2: ISP 内存池分配 — 为 ISP 管线分配 DMA 缓冲区
	 *   Online 模式: ISP 直接从 MIPI 输入处理，不经过 DDR
	 *   输出格式: YUV422 8bit，分辨率 3840×2160 */
	stMemInit.enOfflineWorkMode = vimod;
	stMemInit.enIspOutMode = vomod;
	stMemInit.enIspOutFmt = 1; // 422 8bit
	stMemInit.stPicConf.u32Width = ISP_W;
	stMemInit.stPicConf.u32Height = ISP_H;
	ret = API_ISP_MemInit(0, &stMemInit);
	CHECK_RET(ret != 0, ret);

	/* 步骤3: VI (Video Input) 属性 — 定义 ISP 输入的图像格式和尺寸
	 *   Bayer 格式: GBRG (取决于传感器滤色阵列排列) */
	vi_attr.u16InputHeight = ISP_H;
	vi_attr.u16InputWidth = ISP_W;
	vi_attr.u16PicHeight = ISP_H;
	vi_attr.u16PicWidth = ISP_W;
	vi_attr.u16FrameRate = 30;
	vi_attr.enBayerType = BAYER_GBRG;
	ret = API_ISP_SetViAttr(0, &vi_attr);
	CHECK_RET(ret != 0, ret);

	/* 步骤4: 注册传感器回调函数集
	 *   将 sensor.c 中实现的各传感器操作函数指针填入回调表，
	 *   ISP 驱动通过此表调用传感器控制函数，无需硬编码传感器型号 */
	sensor_func.init = sensor_init_imx415;
	sensor_func.set_sns_fmt = sensor_set_fmt_imx415;
	sensor_func.set_sns_reg = sensor_write_reg;
	sensor_func.get_sns_reg = sensor_read_reg;
	sensor_func.set_exposure_ratio = sensor_set_exposure_ratio_imx415;
	sensor_func.get_exposure_ratio = sensor_get_exposure_ratio_imx415;
	sensor_func.get_sensor_attribute = sensor_get_attribute_imx415;
	sensor_func.set_flipmirror = sensor_set_mirror_flip_imx415;
	sensor_func.get_sns_ae_default = GetAEDefault;
	sensor_func.get_sns_ae_info = GetAEInfo;
	sensor_func.set_sns_gain = SetGain;
	sensor_func.set_sns_intt = SetIntt;
	ret = API_ISP_SensorRegCb(0, 0, &sensor_func);
	CHECK_RET(ret != 0, ret);

	/* 步骤5: 传感器初始化 — 通过 I2C 写寄存器配置传感器工作模式 */
	ret = API_ISP_SensorInit(0, &initConf);
	CHECK_RET(ret != 0, ret);

	/* 步骤6: ISP 内核驱动初始化 */
	ret = API_ISP_Init(0);
	CHECK_RET(ret != 0, ret);

	/* 步骤7: VICAP (Video Capture) 设备初始化 */
	stViDev.enWorkMode = vimod;
	stViDev.stSize.u16Width = ISP_W;
	stViDev.stSize.u16Height = ISP_H;
	ret = FH_VICAP_InitViDev(0, &stViDev);
	CHECK_RET(ret != 0, ret);

	/* 步骤8: VICAP VI 属性 + 裁剪
	 *   输入: 3864×2192 (传感器原始)
	 *   裁剪: 开启, 目标 3840×2160 (切掉边缘非有效像素) */
	stViAttr.enWorkMode = vimod;
	stViAttr.stInSize.u16Width = ISP_W0;
	stViAttr.stInSize.u16Height = ISP_H0;
	stViAttr.stCropSize.bCutEnable = 1;
	stViAttr.stCropSize.stRect.u16Width = ISP_W;
	stViAttr.stCropSize.stRect.u16Height = ISP_H;
	ret = FH_VICAP_SetViAttr(0, &stViAttr);
	CHECK_RET(ret != 0, ret);

	/* 步骤9 (仅 Offline 模式): 绑定 VICAP 输出到 ISP 输入
	 *   Online 模式下 ISP 直接从 MIPI 取数据，无需显式绑定 */
	if (vimod == 1)
	{
		FH_BIND_INFO src, dst;
		src.obj_id = FH_OBJ_VICAP;
		src.dev_id = 0;
		src.chn_id = 0;
		dst.obj_id = FH_OBJ_ISP;
		dst.dev_id = 0;
		dst.chn_id = 0;
		FH_SYS_Bind(src, dst);
	}

	/* 步骤10: 加载 ISP 参数文件 (传感器标定数据)
	 *   从 flash 文件系统读取 .bin 格式的 ISP 标定表，包含:
	 *   - 黑电平校正 (Black Level Correction)
	 *   - 镜头阴影校正 (Lens Shading Correction)
	 *   - 自动白平衡参数 (AWB)
	 *   - 色彩校正矩阵 (CCM)
	 *   - Gamma 曲线等 */
	ret = API_ISP_GetBinAddr(0, &stIsp_para_cfg);
	param_size = stIsp_para_cfg.u32BinSize;
	CHECK_RET(ret != 0, ret);

	isp_param_buff = (char *)malloc(param_size);
	param_file = fopen(SENSOR_PARAM, "rb");
	if (NULL == param_file)
	{
		free(isp_param_buff);

		printf("open file failed!\n");
		return -1;
	}

	if (param_size != fread(isp_param_buff, 1, param_size, param_file))
	{
		free(isp_param_buff);
		fclose(param_file);

		printf("open file failed!\n");
		return -1;
	}
	ret = API_ISP_LoadIspParam(0, isp_param_buff);
	CHECK_RET(ret != 0, ret);
	free(isp_param_buff);
	fclose(param_file);

	/*
	 * sensor 稳定等待: GPIO 复位后，sensor 内部 PLL 和 MIPI
	 * 锁相环需要时间锁定。不加延时可能导致 ISP 取流时 sensor
	 * 尚未输出有效 MIPI 帧，引发 API_ISP_Run 连续报错。
	 */
	usleep(500 * 1000);  /* 500ms */

	/* 步骤11: 启动 ISP 处理线程 (src/isp.c)
	 *   在独立线程中循环调用 API_ISP_Run，每帧处理一次
	 *   ⚠ 当前实现为 while(1) 死循环，无退出条件 */
	ret = isp_server_run();
	CHECK_RET(ret != 0, ret);

	return 0;
}

/*
 * ISP 图像参数动态设置。
 *
 * 通过 key 枚举选择要调节的图像属性，调用对应的 isp.c 函数。
 * 允许在程序运行时动态修改图像效果，无需重启。
 *
 * 参数:
 *   key   - 参数类型:
 *           ISP_AE(0)    自动曝光使能,   范围 [0,1]
 *           ISP_AWB(1)   自动白平衡使能, 范围 [0,1]
 *           ISP_COLOR(2) 色度(饱和度),  范围 [1,255]
 *           ISP_BRIGHT(3)亮度等级,       范围 [0,255]
 *           ISP_NR(4)    降噪使能,       范围 [0,1]
 *           ISP_MF(5)    镜像翻转,       范围 [0,3]
 *   param - 参数值 (范围见上)
 *
 * 返回值: 0=成功, 非0=失败
 */
int isp_set_param(int key, int param)
{
	int ret;

	printf("isp set key %d, val %d\n", key, param);

	switch (key)
	{
	case ISP_AE: // AE使能 范围[0,1]
		ret = isp_set_ae(param);
		break;
	case ISP_AWB: // AWB使能 范围[0,1]
		ret = isp_set_awb(param);
		break;
	case ISP_COLOR: // 色度 范围[1,255]
		ret = isp_set_saturation(param);
		break;
	case ISP_BRIGHT: // 亮度 范围[0,255]
		ret = isp_set_bright(param);
		break;
	case ISP_NR: // 降噪 范围[0,1]
		ret = isp_set_nr(param);
		break;
	case ISP_MF: // 镜像 范围[0,3]
		ret = isp_set_mirrorflip(param);
		break;
	default:
		printf("Error: not support the key %d\n", key);
		break;
	}
	CHECK_RET(ret != 0, ret);

	return ret;
}

/*
 * DMC (Data Media Channel) 初始化 — 媒体数据分发。
 *
 * DMC 是 Fullhan SDK 的媒体数据发布/订阅框架，本质是一个进程内的
 * 观察者模式 (Observer Pattern):
 *   - dmc_input()   → 发布者 (Publisher):  将编码数据注入 DMC
 *   - dmc_xxx_subscribe() → 订阅者 (Subscriber): 从 DMC 取数据
 *
 * 双模式架构 (根据命令行参数自动切换):
 *
 *   模式1: ./demo (无参数)
 *     - dmc_init()                          → 初始化 DMC 框架
 *     - dmc_record_subscribe()              → 订阅: 本地 MP4 录像
 *       (仅当 ENABLE_LOCAL_RECORD=1 时编译)
 *     - 此时 dmc_input() 仅触发录像写入
 *
 *   模式2: ./demo <IP> [port]
 *     - 上述初始化 +
 *     - dmc_pes_subscribe()                 → 订阅: PES UDP 远程推流
 *       dst_ip 非空时激活，向目标 IP:port 推送 PES 封装的码流
 *
 *   ⚠ 模式2 存在内存安全隐患:
 *     PES 订阅回调 _pes_input_fn (libdmc_pes.c) 使用静态全局变量
 *     g_frame_length/g_nalu_count/g_stream_element 缓存跨回调分片，
 *     双码流(chan0+chan1)的回调共享这些全局变量，可能发生:
 *       - 子码流数据覆盖主码流缓冲区 → 帧数据损坏
 *       - 帧长度累加错误 → 内存溢出 → 5分钟崩溃
 *
 * 参数:
 *   dst_ip         - PES UDP 目标 IP (NULL 或空串表示不启用)
 *   port           - PES UDP 目标端口 (默认 1234)
 *   max_channel_no - 最大通道数 (2 = 主码流 + 子码流)
 */
int sample_dmc_init(FH_CHAR *dst_ip, FH_UINT32 port, FH_SINT32 max_channel_no)
{
	dmc_init();
#if ENABLE_LOCAL_RECORD
	dmc_record_subscribe(max_channel_no);
#endif

	if (dst_ip != NULL && *dst_ip != 0)
	{
		dmc_pes_subscribe(max_channel_no, dst_ip, port);
	}

	return 0;
}

/*
 * 检测 NALU 数据开头是否已有 H.264 Annex-B 起始码。
 *
 * H.264 Annex-B 规范定义了两种起始码:
 *   - 4 字节: 0x00 0x00 0x00 0x01 (最常见于帧头)
 *   - 3 字节: 0x00 0x00 0x01       (常见于帧内 NALU)
 *
 * Fullhan 编码器输出的 NALU 可能自带也可能不带起始码，
 * 此函数用于判断是否需要补充起始码以保证 Annex-B 格式兼容。
 *
 * 返回值: 起始码长度 (3 或 4), 0=没有起始码
 */
static int sample_h264_start_code_len(const unsigned char *data, int len)
{
	if (data == NULL || len < 3)
	{
		return 0;
	}

	if (len >= 4 &&
		data[0] == 0x00 &&
		data[1] == 0x00 &&
		data[2] == 0x00 &&
		data[3] == 0x01)
	{
		return 4;
	}

	if (data[0] == 0x00 &&
		data[1] == 0x00 &&
		data[2] == 0x01)
	{
		return 3;
	}

	return 0;
}

/*
 * 将编码器输出的多个 NALU 组装成一帧完整 H.264 Annex-B 数据，推送至 RTSP。
 *
 * Fullhan 编码器 FH_VENC_GetStream_Block 输出的是 NALU 列表 (nalu[]数组)，
 * 每个 NALU 可能带也可能不带起始码。RTSP 推流需要完整的 Annex-B 帧格式。
 *
 * 处理流程:
 *   1. 遍历所有 NALU，计算总缓冲区大小
 *      - 已有起始码的 NALU: 直接用原长度
 *      - 无起始码的 NALU:   加 4 字节 (插入 00 00 00 01)
 *   2. 分配帧缓冲区 (每帧一次 malloc/free)
 *   3. 再次遍历，将每个 NALU 拷贝到帧缓冲区，无起始码的补上
 *   4. 确定码流类型 (chan0→主码流, chan1→子码流)
 *   5. 判断帧类型 (I帧 / P帧)
 *   6. 调用 rtsp_live555_push_frame() 推流
 *   7. 释放帧缓冲区
 *
 * 注意: 每帧都 malloc/free，在高帧率下会产生频繁的内存分配开销。
 *       可以考虑使用静态缓冲区或内存池优化。
 */
static void sample_push_h264_stream_to_rtsp(FH_VENC_STREAM *stream)
{
	unsigned int i;
	int total_len = 0;
	int offset = 0;
	int stream_id;
	int is_key_frame;
	unsigned char *frame_buf = NULL;
	static const unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01};

	if (stream == NULL)
	{
		return;
	}

	if (stream->stmtype != FH_STREAM_H264)
	{
		return;
	}

	if (stream->h264_stream.nalu_cnt <= 0)
	{
		return;
	}

	/* 第一趟: 计算总缓冲区大小。
	 * 对每个 NALU 做清洗: 去掉可能已有的起始码、去掉前导零字节
	 * (前导零是 FH8862 编码器的已知问题，旧版 rtsp_server 也有同样处理) */
	for (i = 0; i < stream->h264_stream.nalu_cnt; i++)
	{
		const unsigned char *nalu = (const unsigned char *)stream->h264_stream.nalu[i].start;
		int len = stream->h264_stream.nalu[i].length;

		if (nalu == NULL || len <= 0)
		{
			continue;
		}

		/* 去掉已有的起始码 (3 或 4 字节) */
		int sc_len = sample_h264_start_code_len(nalu, len);
		nalu += sc_len;
		len -= sc_len;

		/* 跳过前导零字节 — 旧版 rtsp_server 的已知 workaround */
		while (len > 0 && nalu[0] == 0x00)
		{
			nalu++;
			len--;
		}

		if (len <= 0)
		{
			continue;
		}

		total_len += 4 + len;  /* 4 字节起始码 + 清洗后的 NALU */
	}

	if (total_len <= 0)
	{
		return;
	}

	frame_buf = (unsigned char *)malloc(total_len);
	if (frame_buf == NULL)
	{
		printf("malloc rtsp frame buffer failed, len=%d\n", total_len);
		return;
	}

	/* 第二趟: 拷贝 NALU，统一加 4 字节起始码 */
	for (i = 0; i < stream->h264_stream.nalu_cnt; i++)
	{
		const unsigned char *nalu = (const unsigned char *)stream->h264_stream.nalu[i].start;
		int len = stream->h264_stream.nalu[i].length;

		if (nalu == NULL || len <= 0)
		{
			continue;
		}

		int sc_len = sample_h264_start_code_len(nalu, len);
		nalu += sc_len;
		len -= sc_len;

		while (len > 0 && nalu[0] == 0x00)
		{
			nalu++;
			len--;
		}

		if (len <= 0)
		{
			continue;
		}

		memcpy(frame_buf + offset, start_code, 4);
		offset += 4;
		memcpy(frame_buf + offset, nalu, len);
		offset += len;
	}

	stream_id = stream->chan == 0 ? RTSP_STREAM_MAIN : RTSP_STREAM_SUB;
	is_key_frame = stream->h264_stream.frame_type == FH_FRAME_I ? 1 : 0;

	rtsp_live555_push_frame(stream_id, frame_buf, offset, is_key_frame);

	free(frame_buf);
}

/*
 * 编码取流线程 — 整个系统的数据心脏。
 *
 * 此线程在独立 pthread 中运行，是数据管道的最后一环:
 *   硬件编码器 → GetStream → 分发到 DMC(录像/PES) 和 RTSP 两路
 *
 * 工作流程 (每帧循环一次):
 *
 *   1. FH_VENC_GetStream_Block() — 阻塞等待编码器输出一帧
 *      FH_STREAM_ALL & ~FH_STREAM_JPEG = 订阅 H.264 + H.265 (排除 MJPEG)
 *      阻塞模式: 无帧时线程休眠，有帧时立即返回，CPU 占用低
 *
 *   2. 根据编码类型分三路分发:
 *
 *      ┌─ H.264 (最常见):
 *      │   - dmc_input(): 按 NALU 分片注入 DMC (每个 NALU 一次调用)
 *      │     * end_flag = 1 标记帧的最后一个 NALU，用于订阅者组装完整帧
 *      │   - sample_push_h264_stream_to_rtsp(): 合并 NALU → RTSP 推送
 *      │
 *      ├─ H.265 (当前项目未启用):
 *      │   - 仅 dmc_input()，无 RTSP 推送 (live555 仅支持 H.264)
 *      │
 *      └─ MJPEG (抓图模式):
 *          - 仅 dmc_input()，单次调用 (mjpeg 不需要分片)
 *
 *   3. FH_VENC_ReleaseStream() — 释放编码器缓冲区
 *      必须调用，否则编码器缓冲区耗尽导致死锁
 *
 * DMC 分片机制:
 *   编码器输出的是 NALU 列表 (nalu[])，每个 NALU 单独调用 dmc_input()。
 *   end_flag 参数标记帧的最后一个 NALU，订阅者用它来判断帧边界:
 *     - end_flag=0: 此 NALU 后还有更多 NALU，订阅者缓存当前数据
 *     - end_flag=1: 帧的最后一个 NALU，订阅者组装完整帧并处理
 *   这就是为什么 PES 模式的 libpes.c 需要静态全局变量来累积跨回调的分片。
 *
 * 退出机制:
 *   主线程设置 *stop = 1 (即 g_get_stream_stop) → while 循环退出
 *   → 线程在返回前设置 *stop = 0 (通知主线程已安全退出)
 *   这个双向握手确保主线程在编码器释放资源前不会提前清理。
 */
FH_VOID *sample_common_get_stream_proc(FH_VOID *arg)
{
	FH_SINT32 ret;
	FH_UINT32 i;
	FH_SINT32 end_flag;
	FH_SINT32 subtype;
	FH_VENC_STREAM stream;
	FH_SINT32 *stop = (FH_SINT32 *)arg;

	while (*stop == 0)
	{
		/* 标记取流起始时刻 (用于 proc 驱动性能追踪) */
		WR_PROC_DEV(TRACE_PROC, "timing_GetStream_START");

		/*
		 * 阻塞获取一帧编码数据。
		 * FH_STREAM_ALL & ~FH_STREAM_JPEG = H.264 + H.265 (排除了 MJPEG/JPEG)
		 * 阻塞模式意味着没有新帧时线程在此休眠，不消耗 CPU。
		 */
		ret = FH_VENC_GetStream_Block(FH_STREAM_ALL & (~(FH_STREAM_JPEG)), &stream);
		WR_PROC_DEV(TRACE_PROC, "timing_EncBlkFinish_xxx");

		if (ret != 0)
		{
			printf("Error(%d - %x): FH_VENC_GetStream_Block(FH_STREAM_ALL & (~(FH_STREAM_JPEG))) failed!\n", ret, ret);
			continue;
		}

		/* ─── H.264 编码帧 ───
		 * 这是项目实际使用的编码格式。每个 NALU 分别注入 DMC,
		 * 然后合并为一帧推送 RTSP。 */
		if (stream.stmtype == FH_STREAM_H264)
		{
			subtype = stream.h264_stream.frame_type == FH_FRAME_I ? DMC_MEDIA_SUBTYPE_IFRAME : DMC_MEDIA_SUBTYPE_PFRAME;
			for (i = 0; i < stream.h264_stream.nalu_cnt; i++)
			{
				end_flag = (i == (stream.h264_stream.nalu_cnt - 1)) ? 1 : 0;
				dmc_input(stream.chan,
						  DMC_MEDIA_TYPE_H264,
						  subtype,
						  stream.h264_stream.time_stamp,
						  stream.h264_stream.nalu[i].start,
						  stream.h264_stream.nalu[i].length,
						  end_flag);
			}

			sample_push_h264_stream_to_rtsp(&stream);
		}

		/* ─── H.265 编码帧 ───
		 * 当前项目未启用 H.265，但保留支持。仅注入 DMC，
		 * RTSP 服务器暂不支持 H.265 推流 (live555 只处理 H.264)。 */
		else if (stream.stmtype == FH_STREAM_H265)
		{
			subtype = stream.h265_stream.frame_type == FH_FRAME_I ? DMC_MEDIA_SUBTYPE_IFRAME : DMC_MEDIA_SUBTYPE_PFRAME;
			for (i = 0; i < stream.h265_stream.nalu_cnt; i++)
			{
				end_flag = (i == (stream.h265_stream.nalu_cnt - 1)) ? 1 : 0;
				dmc_input(stream.chan,
						  DMC_MEDIA_TYPE_H265,
						  subtype,
						  stream.h265_stream.time_stamp,
						  stream.h265_stream.nalu[i].start,
						  stream.h265_stream.nalu[i].length,
						  end_flag);
			}
		}

		/* ─── MJPEG 编码帧 ───
		 * 抓图模式使用。MJPEG 每帧是独立 JPEG，不需要分片。 */
		else if (stream.stmtype == FH_STREAM_MJPEG)
		{
			dmc_input(stream.chan,
					  DMC_MEDIA_TYPE_MJPEG,
					  0,
					  0,
					  stream.mjpeg_stream.start,
					  stream.mjpeg_stream.length,
					  1);
		}

		/* 释放编码流缓冲区 — 必须调用，归还 DMA buffer 给编码器驱动 */
		ret = FH_VENC_ReleaseStream(&stream);
		if (ret)
		{
			printf("Error(%d - %x): FH_VENC_ReleaseStream failed for chan(%d)!\n", ret, ret, stream.chan);
		}
		WR_PROC_DEV(TRACE_PROC, "timing_GetStream_END");

		/* ─── 录像处理: 仅录主码流(chan0), 含OSD ─── */
		if (g_record_active && stream.stmtype == FH_STREAM_H264 && stream.chan == 0)
		{
			if (g_record_file == NULL)
			{
				char path[128];
				g_record_start_time = time(NULL);
				snprintf(path, sizeof(path), "/home/record_%ld.264", (long)g_record_start_time);
				g_record_file = fopen(path, "wb");
				if (g_record_file)
					printf("[REC] Recording started: %s\n", path);
			}

			if (g_record_file)
			{
				unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01};
				unsigned int i;
				for (i = 0; i < stream.h264_stream.nalu_cnt; i++)
				{
					fwrite(start_code, 1, 4, g_record_file);
					fwrite(stream.h264_stream.nalu[i].start, 1,
						   stream.h264_stream.nalu[i].length, g_record_file);
				}
			}

			/* 检查录像时长 */
				if (g_record_duration > 0)
				{
					time_t now = time(NULL);
					if (now - g_record_start_time >= g_record_duration)
					{
						g_record_active = 0;
						g_record_duration = 0;
						control_led_record_end();
						if (g_record_file)
						{
							fclose(g_record_file);
							g_record_file = NULL;
						printf("[REC] Recording completed (duration reached)\n");
					}
				}
			}

			/* 如果录像被外部停止 */
			if (!g_record_active && g_record_file)
			{
				fclose(g_record_file);
				g_record_file = NULL;
				printf("[REC] Recording stopped\n");
			}
		}
		else if (!g_record_active && g_record_file)
		{
			/* 安全关闭 */
			fclose(g_record_file);
			g_record_file = NULL;
		}
	}

	*stop = 0;  /* 双向握手: 通知主线程推流线程已安全退出 */
	return NULL;
}

/*
 * 视频编码器通道配置 (H.264 / H.265 通用)。
 *
 * 此函数完成编码器通道的创建和属性设置。Fullhan 硬件编码器
 * 支持多个独立通道，每个通道可配置不同的分辨率/码率/编码类型。
 *
 * 编码器创建分两步:
 *   1. FH_VENC_CreateChn(): 创建通道 + 分配编码器内部缓冲区
 *      - support_type = H264 | H265 (两种编码类型均可)
 *      - max_size 必须等于 ISP 输出尺寸 (3840×2160)，即使实际编码分辨率更小
 *   2. FH_VENC_SetChnAttr(): 设置编码参数
 *      - 编码类型: H.264 Main Profile
 *      - I帧间隔: 50 (每 50 帧一个 I 帧，即每 2 秒刷新一次关键帧)
 *      - 码率控制: CBR (恒定码率) — 适合网络传输，带宽可控
 *
 * 码率控制策略:
 *   CBR (Constant Bitrate) — 当前使用:
 *     - 码率稳定，适合固定带宽的网络传输
 *     - 通过 maxrate_percent=200 允许瞬时 2× 峰值
 *
 *   VBR (Variable Bitrate) — 已注释保留:
 *     - 根据画面复杂度动态分配码率 (静态画面少码率，动态画面多码率)
 *     - 同等画质下文件更小，但码率波动大
 *
 * 参数:
 *   chan    - 编码器通道号 (0=主码流, 1=子码流)
 *   enc_w/h - 编码分辨率 (主 1280×720, 子 704×576)
 *   bitrate - 目标码率 (bps), 主 2Mbps, 子 700kbps
 *   fps     - 帧率 (25fps)
 *
 * 返回值: 0=成功, 非0=失败
 */
static int sampe_set_venc_cfg(int chan, int enc_w, int enc_h, int bitrate, int fps)
{
	FH_VENC_CHN_CAP cfg_vencmem;
	memset(&cfg_vencmem, 0, sizeof(cfg_vencmem));

	cfg_vencmem.support_type = FH_NORMAL_H264 | FH_NORMAL_H265;
	cfg_vencmem.max_size.u32Width = ISP_W;
	cfg_vencmem.max_size.u32Height = ISP_H;

	int ret = FH_VENC_CreateChn(chan, &cfg_vencmem);
	if (ret != 0)
	{
		return ret;
	}

	FH_VENC_CHN_CONFIG cfg_param;
	memset(&cfg_param, 0, sizeof(cfg_param));

	cfg_param.chn_attr.enc_type = FH_NORMAL_H264;
	cfg_param.chn_attr.h264_attr.profile = H264_PROFILE_MAIN;
	cfg_param.chn_attr.h264_attr.i_frame_intterval = 50;
	cfg_param.chn_attr.h264_attr.size.u32Width = enc_w;
	cfg_param.chn_attr.h264_attr.size.u32Height = enc_h;

	/* ─── VBR 配置 (已注释, 备查) ───
	 * VBR 在允许较大码率波动时比 CBR 画质更好，
	 * 但 RTSP 推流场景下 CBR 的网络适应性更稳定。
	 *
	 * init_qp=35:      初始QP, 越大画面越模糊但码率越低
	 * ImaxQP=42:       I帧最大QP
	 * IminQP=28:       I帧最小QP
	 * maxrate_percent: 最大码率百分比 (200 = 2×目标码率)
	 * IP_QPDelta=3:    I帧与P帧的QP差值 (I帧更清晰)
	 * I_BitProp=5:     I帧码率权重 (5×P帧)
	 * P_BitProp=1:     P帧码率权重
	 * fluctuate_level: 码率波动程度 (0=最小波动)
	 */
	// cfg_param.rc_attr.rc_type = FH_RC_H264_VBR;
	// cfg_param.rc_attr.h264_vbr.bitrate = bitrate;
	// cfg_param.rc_attr.h264_vbr.init_qp = 35;
	// cfg_param.rc_attr.h264_vbr.ImaxQP = 42;
	// cfg_param.rc_attr.h264_vbr.IminQP = 28;
	// cfg_param.rc_attr.h264_vbr.PmaxQP = 42;
	// cfg_param.rc_attr.h264_vbr.PminQP = 28;
	// cfg_param.rc_attr.h264_vbr.FrameRate.frame_count = fps;
	// cfg_param.rc_attr.h264_vbr.FrameRate.frame_time = 1;
	// cfg_param.rc_attr.h264_vbr.maxrate_percent = 200;
	// cfg_param.rc_attr.h264_vbr.IFrmMaxBits = 0;
	// cfg_param.rc_attr.h264_vbr.IP_QPDelta = 3;
	// cfg_param.rc_attr.h264_vbr.I_BitProp = 5;
	// cfg_param.rc_attr.h264_vbr.P_BitProp = 1;
	// cfg_param.rc_attr.h264_vbr.fluctuate_level = 0;

	/* ─── CBR 配置 (当前使用) ───
	 * 以恒定码率编码，网络传输的推荐选择 */
	cfg_param.rc_attr.rc_type = FH_RC_H264_CBR;

	cfg_param.rc_attr.h264_cbr.init_qp = 32;
	cfg_param.rc_attr.h264_cbr.bitrate = bitrate;
	cfg_param.rc_attr.h264_cbr.FrameRate.frame_count = fps;
	cfg_param.rc_attr.h264_cbr.FrameRate.frame_time = 1;
	cfg_param.rc_attr.h264_cbr.maxrate_percent = 200;
	cfg_param.rc_attr.h264_cbr.IFrmMaxBits = 0;
	cfg_param.rc_attr.h264_cbr.IP_QPDelta = 3;
	cfg_param.rc_attr.h264_cbr.I_BitProp = 5;
	cfg_param.rc_attr.h264_cbr.P_BitProp = 1;
	cfg_param.rc_attr.h264_cbr.fluctuate_level = 0;

	return FH_VENC_SetChnAttr(chan, &cfg_param);
}

static int sampe_set_jpeg_cfg(int chan, int enc_w, int enc_h, int quality)
{
	sleep(1);

	static int jpeg_cnt = 0;
	static int jpeg_init = 0;
	int ret = 0;

	if (jpeg_init == 0)
	{
		FH_VENC_CHN_CAP cfg_vencmem;

		cfg_vencmem.support_type = FH_JPEG;
		cfg_vencmem.max_size.u32Width = ISP_W;
		cfg_vencmem.max_size.u32Height = ISP_H;

		ret = FH_VENC_CreateChn(chan, &cfg_vencmem);
		if (ret != 0)
		{
			return ret;
		}

		jpeg_init = 1;
	}
	FH_VENC_CHN_CONFIG cfg_param = {0};

	cfg_param.chn_attr.enc_type = FH_JPEG;
	cfg_param.chn_attr.jpeg_attr.encode_speed = 4;
	cfg_param.chn_attr.jpeg_attr.qp = quality;

	ret = FH_VENC_SetChnAttr(chan, &cfg_param);
	CHECK_RET(ret != 0, ret);

	FH_BIND_INFO src, dst;
	src.obj_id = FH_OBJ_VPU_VO;
	src.dev_id = 0;
	src.chn_id = 0;

	dst.obj_id = FH_OBJ_JPEG;
	dst.dev_id = 0;
	dst.chn_id = chan;

	ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	FH_VENC_STREAM jpeg_stream;

	while (1)
	{
		ret = FH_VENC_GetStream_Timeout(FH_STREAM_JPEG, &jpeg_stream, 1000);
		if (ret == 0)
		{
			break;
		}
	}

	if (jpeg_stream.stmtype == FH_STREAM_JPEG)
	{
		char jpeg_path[50] = {0};
		snprintf(jpeg_path, sizeof(jpeg_path), "/home/jpeg_%d.jpg", jpeg_cnt);
		FILE *jpeg_file = fopen(jpeg_path, "w+");
		if (jpeg_file)
		{
			fwrite(jpeg_stream.jpeg_stream.start, sizeof(char), jpeg_stream.jpeg_stream.length, jpeg_file);
			fclose(jpeg_file);
			printf("get jpeg file %d\n", jpeg_stream.jpeg_stream.length);
		}
		jpeg_cnt++;
	}
	ret = FH_VENC_ReleaseStream(&jpeg_stream);
	CHECK_RET(ret != 0, ret);

	ret = FH_SYS_UnBindbyDst(dst);
	CHECK_RET(ret != 0, ret);

	return 0;
}

int sample_set_osd(int chan)
{
	int ret;
	int graph_ctrl = 0;

	graph_ctrl |= FHT_OSD_GRAPH_CTRL_TOSD_AFTER_VP;
	/* Mask 放在 VPSS 之后，避免整体变灰 */

	/* 初始化OSD */
	ret = FHAdv_Osd_Init(0, FHT_OSD_DEBUG_LEVEL_ERROR, graph_ctrl, 0, 0);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_Init failed with %x\n", ret);
		return ret;
	}

	/* 加载asc字库 */
	FHT_OSD_FontLib_t font_lib;

	font_lib.pLibData = asc16;
	font_lib.libSize = sizeof(asc16);
	ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_ASC, &font_lib);
	if (ret != 0)
	{
		printf("Error: Load ASC font lib failed, ret=%d\n", ret);
		return ret;
	}

	/* 加载gb2312字库 */
	font_lib.pLibData = gb2312;
	font_lib.libSize = sizeof(gb2312);
	ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_CHINESE, &font_lib);
	if (ret != 0)
	{
		printf("Error: Load CHINESE font lib failed, ret=%d\n", ret);
		return ret;
	}

	FHT_OSD_CONFIG_t osd_cfg;
	FHT_OSD_Layer_Config_t pOsdLayerInfo[4];
	FHT_OSD_TextLine_t text_line_cfg[4];
	FH_CHAR text_data[4][128]; /*it should be enough*/
	FH_SINT32 user_defined_time = 0;

	memset(&osd_cfg, 0, sizeof(osd_cfg));
	memset(&pOsdLayerInfo[0], 0, 4 * sizeof(FHT_OSD_Layer_Config_t));
	memset(&text_line_cfg[0], 0, 4 * sizeof(FHT_OSD_TextLine_t));
	memset(&text_data, 0, sizeof(text_data));

	/* 旋转角 */
	osd_cfg.osdRotate = 0;
	osd_cfg.pOsdLayerInfo = &pOsdLayerInfo[0];
	/* 设置text层信息 */
	osd_cfg.nOsdLayerNum = 1; /*因为demo中只显示了一个层*/

	pOsdLayerInfo[0].layerStartX = 0;
	pOsdLayerInfo[0].layerStartY = 0;
	/* pOsdLayerInfo[0].layerMaxWidth = 640; */ /*如果不设置此参数，则使用编码通道的分辨率值申请内存，否则按实际分配大小申请内存*/
	/* pOsdLayerInfo[0].layerMaxHeight = 480; */
	/* 设置字符大小,像素单位 */
	pOsdLayerInfo[0].osdSize = OSD_FONT_SIZE;

	/* 设置字符颜色为动态可配置 */
	pOsdLayerInfo[0].normalColor.fAlpha = 255;
	pOsdLayerInfo[0].normalColor.fRed = (FH_UINT8)g_osd_user_color_r;
	pOsdLayerInfo[0].normalColor.fGreen = (FH_UINT8)g_osd_user_color_g;
	pOsdLayerInfo[0].normalColor.fBlue = (FH_UINT8)g_osd_user_color_b;

	/* 设置字符反色颜色为黑色 */
	pOsdLayerInfo[0].invertColor.fAlpha = 255;
	pOsdLayerInfo[0].invertColor.fRed = 0;
	pOsdLayerInfo[0].invertColor.fGreen = 0;
	pOsdLayerInfo[0].invertColor.fBlue = 0;

	/* 设置字符边缘颜色为黑色 */
	pOsdLayerInfo[0].edgeColor.fAlpha = 255;
	pOsdLayerInfo[0].edgeColor.fRed = 0;
	pOsdLayerInfo[0].edgeColor.fGreen = 0;
	pOsdLayerInfo[0].edgeColor.fBlue = 0;

	/* 不显示背景 */
	pOsdLayerInfo[0].bkgColor.fAlpha = 0;

	/* 反色关闭时使用勾边，开启时关闭勾边 */
	pOsdLayerInfo[0].edgePixel = g_osd_invert_en ? 0 : 1;

	/* 反色使能 */
	pOsdLayerInfo[0].osdInvertEnable = FH_OSD_INVERT_DISABLE; /*disable反色使能*/
	pOsdLayerInfo[0].osdInvertThreshold.high_level = 180;
	pOsdLayerInfo[0].osdInvertThreshold.low_level = 160;
	pOsdLayerInfo[0].layerFlag = FH_OSD_LAYER_USE_TWO_BUF;
	pOsdLayerInfo[0].layerId = 0;
	g_osd_layer_id = 0;

	ret = FHAdv_Osd_Ex_SetText(0, 0, &osd_cfg);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_Ex_SetText chan0 failed with %d\n", ret);
		return ret;
	}
	ret = FHAdv_Osd_Ex_SetText(0, 1, &osd_cfg);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_Ex_SetText chan1 failed with %d\n", ret);
		return ret;
	}
	text_line_cfg[0].textInfo = text_data[0];
	text_line_cfg[1].textInfo = text_data[1];
	text_line_cfg[2].textInfo = text_data[2];
	text_line_cfg[3].textInfo = text_data[3];
	FH_CHAR user_tag_data[] = {
		0xe4,
		0x01, /*FHT_OSD_YEAR4, 4位年,例如2019*/
		'-',
		0xe4,
		0x03, /*FHT_OSD_MONTH2, 2位月份,取值01~12*/
		'-',
		0xe4,
		0x04, /*FHT_OSD_DAY, 2位日期,取值01~31*/
		0x20, /*空格*/
		0xe4,
		0x07, /*FHT_OSD_HOUR24, 24小时制小时,取值00~23*/
		':',
		0xe4,
		0x09, /*FHT_OSD_MINUTE, 2位分钟,取值00~59*/
		':',
		0xe4,
		0x0a, /*FHT_OSD_SECOND, 2位秒,取值00~59*/
		0,	  /*null terminated string*/
	};
#if 1
	sprintf(text_line_cfg[0].textInfo, "%s", g_osd_text);
	text_line_cfg[0].textEnable = 1;						   /* 使能自定义text */
	text_line_cfg[0].timeOsdEnable = 0;						   /* 去使能时间显示 */
	text_line_cfg[0].textLineWidth = (OSD_FONT_SIZE / 2) * 36; /* 每行最多显示36个宽度为32的字符 */
	text_line_cfg[0].linePositionX = 20;					   /* 左上角起始横坐标偏移位置 */
	text_line_cfg[0].linePositionY = 20;					   /* 左上角起始纵坐标偏移位置 */

	text_line_cfg[0].lineId = 0;
	text_line_cfg[0].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[0]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line0 chan0 failed with %d\n", ret);
		return ret;
	}
	ret = FHAdv_Osd_SetTextLine(0, 1, pOsdLayerInfo[0].layerId, &text_line_cfg[0]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line0 chan1 failed with %d\n", ret);
		return ret;
	}
#endif
#if 1
	strcat(text_line_cfg[1].textInfo, user_tag_data);
	/* 保存时间戳格式字符串到全局，供 osd_reapply_all_lines 使用 */
	memcpy(g_osd_time_fmt, user_tag_data, sizeof(user_tag_data));
	text_line_cfg[1].textEnable = 1;						   /* 使能自定义text */
	text_line_cfg[1].timeOsdEnable = 0;						   /* 去使能时间显示 */
	text_line_cfg[1].textLineWidth = (OSD_FONT_SIZE / 2) * 36; /* 每行最多显示36个宽度为32的字符 */
	text_line_cfg[1].linePositionX = 20;					   /* 左上角起始横坐标偏移位置 */
	text_line_cfg[1].linePositionY = 100;					   /* 左上角起始纵坐标偏移位置 */

	text_line_cfg[1].lineId = 1;
	text_line_cfg[1].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[1]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line1 chan0 failed with %d\n", ret);
		return ret;
	}
	ret = FHAdv_Osd_SetTextLine(0, 1, pOsdLayerInfo[0].layerId, &text_line_cfg[1]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line1 chan1 failed with %d\n", ret);
		return ret;
	}
#endif
#if 1
	sprintf(text_line_cfg[2].textInfo, "Hangzhou Dianzi University");
	text_line_cfg[2].textEnable = 1;
	text_line_cfg[2].timeOsdEnable = 0;
	text_line_cfg[2].textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line_cfg[2].linePositionX = 20;
	text_line_cfg[2].linePositionY = 180;
	text_line_cfg[2].lineId = 2;
	text_line_cfg[2].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[2]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line2 chan0 failed with %d\n", ret);
		return ret;
	}
	ret = FHAdv_Osd_SetTextLine(0, 1, pOsdLayerInfo[0].layerId, &text_line_cfg[2]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line2 chan1 failed with %d\n", ret);
		return ret;
	}
#endif
#if 1
	sprintf(text_line_cfg[3].textInfo, "Hikvision");
	text_line_cfg[3].textEnable = 1;
	text_line_cfg[3].timeOsdEnable = 0;
	text_line_cfg[3].textLineWidth = (OSD_FONT_SIZE / 2) * 36;
	text_line_cfg[3].linePositionX = 20;
	text_line_cfg[3].linePositionY = 260;
	text_line_cfg[3].lineId = 3;
	text_line_cfg[3].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[3]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line3 chan0 failed with %d\n", ret);
		return ret;
	}
	ret = FHAdv_Osd_SetTextLine(0, 1, pOsdLayerInfo[0].layerId, &text_line_cfg[3]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line3 chan1 failed with %d\n", ret);
		return ret;
	}
#endif

	return 0;
}

/*
 * 媒体驱动配置 — 初始化 VB (Video Buffer) 内存池。
 *
 * VB 是 Fullhan SDK 的 DMA 缓冲管理器，所有硬件模块 (ISP/VPU/VENC)
 * 通过 VB 池共享图像数据，避免 CPU 拷贝，实现零拷贝流水线。
 *
 * 内存池配置:
 *   Pool[0]: 3840×2160×3 ≈ 24.9 MB × 4 = 99.6 MB   (4K ISP 输入)
 *   Pool[1]: 1920×1080×3 ≈  5.9 MB × 4 = 23.7 MB   (1080p 中间缓冲)
 *   Pool[2]: 1280×720×3  ≈  2.6 MB × 4 = 10.5 MB   (720p 主码流)
 *   Pool[3]: 704×576×3   ≈  1.2 MB × 4 =  4.7 MB   (D1 子码流)
 *
 *   u32BlkCnt = 4: 每个尺寸预留 4 个 buffer，支持流水线深度 4
 *   总内存约 140 MB，用于 DMA 共享缓冲
 *
 * FH_VB_Exit() → FH_VB_SetConf() → FH_VB_Init() 的顺序是 SDK 规范要求，
 * 先退出已存在的 VB (防御性编程)，再按新配置初始化。
 *
 * proc 驱动配置:
 *   ENC_PROC "stm_20000000_40" → 编码器码流缓冲 20Mbps, 帧率40 (预留余量)
 *   JPEG_PROC "frmsize_1_3000000_3000000" → JPEG 帧最大 3MB
 */
FH_VOID sample_common_media_driver_config(FH_VOID)
{
	VB_CONF_S stVbConf;
	FH_SINT32 ret;

	FH_VB_Exit();

	memset(&stVbConf, 0, sizeof(VB_CONF_S));
	stVbConf.u32MaxPoolCnt = 5;
	stVbConf.astCommPool[0].u32BlkSize = 3840 * 2160 * 3;
	stVbConf.astCommPool[0].u32BlkCnt = 4;
	stVbConf.astCommPool[1].u32BlkSize = 1920 * 1080 * 3;
	stVbConf.astCommPool[1].u32BlkCnt = 4;
	stVbConf.astCommPool[2].u32BlkSize = 1280 * 720 * 3;
	stVbConf.astCommPool[2].u32BlkCnt = 4;
	stVbConf.astCommPool[3].u32BlkSize = 704 * 576 * 3;
	stVbConf.astCommPool[3].u32BlkCnt = 4;

	ret = FH_VB_SetConf(&stVbConf);
	if (ret)
	{
		printf("[FH_VB_SetConf] failed with:%x\n", ret);
	}

	ret = FH_VB_Init();
	if (ret)
	{
		printf("[FH_VB_Init] failed with:%x\n", ret);
	}

	WR_PROC_DEV(ENC_PROC, "allchnstm_0_20000000_40");
	WR_PROC_DEV(ENC_PROC, "stm_20000000_40");
	WR_PROC_DEV(JPEG_PROC, "frmsize_1_3000000_3000000");
	WR_PROC_DEV(JPEG_PROC, "jpgstm_12000000_2");
	WR_PROC_DEV(JPEG_PROC, "mjpgstm_12000000_2");
}

/*
 * 主函数 — 系统初始化和生命周期管理。
 *
 * main() 负责按严格的依赖顺序启动所有子系统，并管理程序的优雅退出。
 * 整个启动流程分为 7 个阶段:
 *
 *   Phase 1: 信号处理注册 → 解析命令行参数
 *   Phase 2: 媒体驱动初始化 (VB 内存池 + FH_SYS)
 *   Phase 3: ISP 管道启动 (传感器→ISP→VI)
 *   Phase 4: VPSS 双通道创建 (主码流 1280×720 + 子码流 704×576)
 *   Phase 5: VENC 编码器配置 (CBR H.264) + 硬件绑定
 *   Phase 6: 上层模块启动 (DMC + RTSP + 编码推流线程 + OSD)
 *   Phase 7: ISP 参数微调 (AE/AWB/色度/亮度/降噪/镜像)
 *   Phase 8: 主循环等待退出信号 → 资源清理
 *
 * 硬件绑定关系总结:
 *   ISP.ch0  ──→  VPU_VI.ch0     (ISP输出 → VPU输入)
 *   VPU_VO.ch0 ──→ ENC.ch0      (主码流 → 编码器通道0)
 *   VPU_VO.ch1 ──→ ENC.ch1      (子码流 → 编码器通道1)
 */
int main(int argc, char *argv[])
{
	int ret;
	char *dst_ip;
	unsigned int port;

	/* ─── Phase 1: 信号处理 + 命令行参数 ────────────────────── */

	/*
	 * 注册信号处理器，实现 Ctrl+C 优雅退出。
	 * ⚠ SIGKILL 不能被捕获 (Linux 内核强制)，此调用无实际效果。
	 */
	signal(SIGINT, sample_vlcview_handle_sig);
	signal(SIGQUIT, sample_vlcview_handle_sig);
	signal(SIGKILL, sample_vlcview_handle_sig);
	signal(SIGTERM, sample_vlcview_handle_sig);

	/*
	 * 命令行参数:
	 *   argv[1] = 目标 IP 地址 (可选，存在则激活 PES UDP 推流)
	 *   argv[2] = 目标端口 (可选，默认 1234)
	 *
	 * ⚠ IP 格式未校验，错误格式可能导致 dmc_pes_subscribe 异常运行
	 */
	dst_ip = argc > 1 ? argv[1] : NULL;
	port = argc > 2 ? strtol(argv[2], NULL, 0) : 1234;

	printf("demo_main driver_config\n");

	/* ─── Phase 2: 媒体驱动初始化 ───────────────────────────── */

	/*
	 * VB 内存池配置: 为各分辨率分配 DMA 缓冲区
	 *   - 3840×2160 ×4 = ~100MB (4K ISP)
	 *   - 1920×1080 ×4 = ~24MB  (1080p)
	 *   - 1280×720  ×4 = ~11MB  (主码流)
	 *   - 704×576   ×4 = ~5MB   (子码流)
	 *   总计约 140MB DMA 内存
	 */
	sample_common_media_driver_config();

	/* FH_SYS_Init 必须在使用任何硬件模块前调用，初始化芯片系统层 */
	ret = FH_SYS_Init();
	CHECK_RET(ret != 0, ret);

	/* ─── Phase 3: ISP 管道启动 ──────────────────────────────── */
	printf("start_isp\n");

	start_isp();
	printf("start_isp success\n");

	/* ─── Phase 4: VPSS 双通道创建 ───────────────────────────── */

	/*
	 * VPSS (Video Processing Sub-System) 组配置
	 *   ycmean_en=1:  启用亮度/色度均值统计 (用于场景检测)
	 *   ycmean_ds=16: 统计下采样系数 (每16×16块一个统计值)
	 */
	FH_VPU_SET_GRP_INFO grp_info;
	grp_info.vi_max_size.u32Width = ISP_W;
	grp_info.vi_max_size.u32Height = ISP_H;
	grp_info.ycmean_en = 1;
	grp_info.ycmean_ds = 16;

	ret = FH_VPSS_CreateGrp(GROUP_ID, &grp_info);
	CHECK_RET(ret != 0, ret);

	/* 设置 VPU 输入图像尺寸 (与 ISP 输出一致) */
	FH_VPU_SIZE vi_pic;
	vi_pic.vi_size.u32Width = ISP_W;
	vi_pic.vi_size.u32Height = ISP_H;
	vi_pic.crop_area.crop_en = 0;
	vi_pic.crop_area.vpu_crop_area.u32X = 0;
	vi_pic.crop_area.vpu_crop_area.u32Y = 0;
	vi_pic.crop_area.vpu_crop_area.u32Width = 0;
	vi_pic.crop_area.vpu_crop_area.u32Height = 0;

	ret = FH_VPSS_SetViAttr(GROUP_ID, &vi_pic);
	CHECK_RET(ret != 0, ret);

	/* 使能 VPU，设置工作模式为 ISP 输入 */
	ret = FH_VPSS_Enable(GROUP_ID, VPU_MODE_ISP);
	CHECK_RET(ret != 0, ret)

	/*
	 * ─── 通道0: 主码流通道 ───
	 * 输入: ISP 4K → VPU 缩放至 1280×720 → 输出 720p
	 * 特性: bgm(背景建模)+cpy(拷贝)+sad(运动检测) 全部开启
	 * bufnum=3: 三缓冲，保证流水线不阻塞
	 */
	FH_VPU_CHN_INFO chn_info = {0};
	chn_info.bgm_enable = 1;
	chn_info.cpy_enable = 1;
	chn_info.sad_enable = 1;
	chn_info.bgm_ds = 8;
	chn_info.chn_max_size.u32Width = ISP_W;
	chn_info.chn_max_size.u32Height = ISP_H;
	chn_info.out_mode = VPU_VOMODE_SCAN;
	chn_info.support_mode = 1 << chn_info.out_mode;
	chn_info.bufnum = 3;
	chn_info.max_stride = 0;
	ret = FH_VPSS_CreateChn(GROUP_ID, 0, &chn_info);
	CHECK_RET(ret != 0, ret);

	FH_VPU_CHN_CONFIG chn_attr;
	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.vpu_chn_size.u32Width = 1280;
	chn_attr.vpu_chn_size.u32Height = 720;
	chn_attr.crop_area.crop_en = 0;
	chn_attr.crop_area.vpu_crop_area.u32X = 0;
	chn_attr.crop_area.vpu_crop_area.u32Y = 0;
	chn_attr.crop_area.vpu_crop_area.u32Width = 0;
	chn_attr.crop_area.vpu_crop_area.u32Height = 0;
	chn_attr.offset = 0;
	chn_attr.depth = 1;
	chn_attr.stride = 0;
	ret = FH_VPSS_SetChnAttr(GROUP_ID, 0, &chn_attr); /* 设置缩放后的输出分辨率 1280×720 */
	CHECK_RET(ret != 0, ret);

	/* 主码流帧率控制: 25fps */
	FH_FRAMERATE main_fps;
	main_fps.frame_count = 25;
	main_fps.frame_time = 1;

	ret = FH_VPSS_SetFramectrl(GROUP_ID, 0, &main_fps);
	CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_SetVOMode(GROUP_ID, 0, VPU_VOMODE_SCAN);
	CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_OpenChn(GROUP_ID, 0);
	CHECK_RET(ret != 0, ret);

	/*
	 * ─── 通道1: 子码流通道 ───
	 * 输入: ISP 4K → VPU 缩放至 704×576 → 输出 D1 分辨率
	 * 与主码流独立运行，可同时被不同客户端拉取
	 */
	FH_VPU_CHN_INFO chn_info_sub = {0};
	chn_info_sub.bgm_enable = 1;
	chn_info_sub.cpy_enable = 1;
	chn_info_sub.sad_enable = 1;
	chn_info_sub.bgm_ds = 8;
	chn_info_sub.chn_max_size.u32Width = ISP_W;
	chn_info_sub.chn_max_size.u32Height = ISP_H;
	chn_info_sub.out_mode = VPU_VOMODE_SCAN;
	chn_info_sub.support_mode = 1 << chn_info_sub.out_mode;
	chn_info_sub.bufnum = 3;
	chn_info_sub.max_stride = 0;
	ret = FH_VPSS_CreateChn(GROUP_ID, 1, &chn_info_sub);
	CHECK_RET(ret != 0, ret);

	FH_VPU_CHN_CONFIG chn_attr_sub;
	memset(&chn_attr_sub, 0, sizeof(chn_attr_sub));

	chn_attr_sub.vpu_chn_size.u32Width = 704;
	chn_attr_sub.vpu_chn_size.u32Height = 576;
	chn_attr_sub.crop_area.crop_en = 0;
	chn_attr_sub.offset = 0;
	chn_attr_sub.depth = 1;
	chn_attr_sub.stride = 0;

	ret = FH_VPSS_SetChnAttr(GROUP_ID, 1, &chn_attr_sub); /* 缩放至 704×576 (D1) */
	CHECK_RET(ret != 0, ret);

	/* 子码流帧率控制: 25fps */
	FH_FRAMERATE sub_fps;
	sub_fps.frame_count = 25;
	sub_fps.frame_time = 1;

	ret = FH_VPSS_SetFramectrl(GROUP_ID, 1, &sub_fps);
	CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_SetVOMode(GROUP_ID, 1, VPU_VOMODE_SCAN);
	CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_OpenChn(GROUP_ID, 1);
	CHECK_RET(ret != 0, ret);

	/* ─── Phase 5: VENC 编码器配置 + 硬件绑定 ────────────────── */

	/* 通道0: 主码流 H.264 CBR 1280×720@25fps 2Mbps */
	ret = sampe_set_venc_cfg(0, 1280, 720, 2 * 1024 * 1024, 25);
	CHECK_RET(ret != 0, ret);

	/* 通道1: 子码流 H.264 CBR 704×576@25fps 700kbps */
	ret = sampe_set_venc_cfg(1, 704, 576, 700 * 1024, 25);
	CHECK_RET(ret != 0, ret);

	/*
	 * 硬件绑定: 建立模块间零拷贝数据通路
	 *
	 * 绑定 1: ISP → VPU_VI
	 *   ISP 的输出图像直接送入 VPU 的输入端口，不经过 DDR
	 */
	FH_BIND_INFO src, dst;
	src.obj_id = FH_OBJ_ISP;
	src.dev_id = 0;
	src.chn_id = 0;
	dst.obj_id = FH_OBJ_VPU_VI;
	dst.dev_id = 0;
	dst.chn_id = 0;
	ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/* 启动编码器接收图像 (必须在绑定完成后调用) */
	ret = FH_VENC_StartRecvPic(0);
	CHECK_RET(ret != 0, ret);

	ret = FH_VENC_StartRecvPic(1);
	CHECK_RET(ret != 0, ret);

	/* 绑定 2: VPU_VO.ch0 → ENC.ch0 (主码流) */
	src.obj_id = FH_OBJ_VPU_VO;
	src.dev_id = GROUP_ID;
	src.chn_id = 0;

	dst.obj_id = FH_OBJ_ENC;
	dst.dev_id = 0;
	dst.chn_id = 0;

	ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/* 绑定 3: VPU_VO.ch1 → ENC.ch1 (子码流) */
	src.obj_id = FH_OBJ_VPU_VO;
	src.dev_id = GROUP_ID;
	src.chn_id = 1;

	dst.obj_id = FH_OBJ_ENC;
	dst.dev_id = 0;
	dst.chn_id = 1;

	ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/* ─── Phase 6: 上层模块启动 ──────────────────────────────── */

	/*
	 * DMC 初始化: 根据命令行参数决定是否启用 PES UDP 推流
	 *   ./demo           → 仅本地录像 (如果 ENABLE_LOCAL_RECORD=1)
	 *   ./demo <IP>      → 本地录像 + PES UDP 推到 <IP>:1234
	 *   ./demo <IP> <port> → 本地录像 + PES UDP 推到 <IP>:<port>
	 */
	sample_dmc_init(dst_ip, port, 2);

	/*
	 * 创建编码推流线程。
	 * 必须在 rtsp_live555_start() 之前创建，因为后者会阻塞在事件循环中。
	 *
	 * ⚠ 原代码线程栈大小仅 3KB，修正为 256KB。
	 */
	{
		pthread_attr_t attr;
		pthread_t thread_stream;

		if (!g_get_stream_running)
		{
			g_get_stream_running = 1;
			g_get_stream_stop = 0;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			pthread_attr_setstacksize(&attr, 256 * 1024); /* 修正: 3KB → 256KB */
			pthread_create(&thread_stream, &attr, sample_common_get_stream_proc, &g_get_stream_stop);
		}
	}

	/*
	 * OSD 文字叠加: 在通道0(主码流)上叠加 4 行文字
	 */
#if 1
	ret = sample_set_osd(0);
	CHECK_RET(ret != 0, ret);
#endif

	/* ─── Phase 7: ISP 图像参数微调 ───────────────────────────── */
	isp_set_param(ISP_AE, 1);
	isp_set_param(ISP_AWB, 1);
	isp_set_param(ISP_COLOR, 25);
	isp_set_param(ISP_BRIGHT, 125);
	isp_set_param(ISP_NR, 1);
	isp_set_param(ISP_MF, 0);

#if ENABLE_VPSS_MOSAIC_MASK
	mask_state_init();
	ret = mask_flush_to_hardware();
	CHECK_RET(ret != 0, ret);
#endif

#if 0
	sleep(2);
	ret = sampe_set_jpeg_cfg(1, 3840, 2160, 40);
	CHECK_RET(ret != 0, ret);

	ret = sampe_set_jpeg_cfg(1, 3840, 2160, 80);
	CHECK_RET(ret != 0, ret);
#endif
	{
		time_t start_time = time(NULL);
		(void)start_time;
	}

	printf("RTSP Server starting on port 8554...\n");
	printf("main stream url: rtsp://<board_ip>:8554/main\n");
	printf("sub  stream url: rtsp://<board_ip>:8554/sub\n");

	/* ─── 启动 TCP 控制服务器 (监听端口 9999) ──────────────── */
	control_server_start(9999);

	/* 启动 LED 自动状态机线程 */
	g_led_auto_thread_running = 1;
	{
		pthread_attr_t attr;
		pthread_t tid;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 32 * 1024);
		pthread_create(&tid, &attr, led_auto_thread, NULL);
		pthread_attr_destroy(&attr);
	}

	/* LED 闪烁线程由 control_led_set 按需启动 */

	/* 硬件自动反色, 不需要独立线程 */
	g_invert_thread_running = 0;

	/* ─── Phase 8: live555 RTSP 事件循环 (阻塞) ──────────────── */

	/*
	 * rtsp_live555_start() 创建 RTSPServer 并进入事件循环。
	 * Ctrl+C → SIGINT → sample_vlcview_handle_sig → rtsp_live555_stop()
	 * → 事件循环退出 → rtsp_live555_start() 返回。
	 */
	ret = rtsp_live555_start(8554);
	CHECK_RET(ret != 0, ret);

	/* ─── 资源清理 ─────────────────────────────────────────── */

	g_get_stream_stop = 1;

	FH_VENC_StopRecvPic(0);
	FH_VENC_StopRecvPic(1);

	while (g_get_stream_stop)
	{
		usleep(10000);
	}

	isp_server_stop();  /* ISP线程停止 — 轮询确保线程完全退出 */

	/* ─── 硬件解绑 (初始化逆序，先解绑再销毁) ────────────── */
	{
		FH_BIND_INFO dst;

		/* 解绑 VPU_VO.ch0 → ENC.ch0 (主码流编码器) */
		dst.obj_id = FH_OBJ_ENC;
		dst.dev_id = 0;
		dst.chn_id = 0;
		FH_SYS_UnBindbyDst(dst);

		/* 解绑 VPU_VO.ch1 → ENC.ch1 (子码流编码器) */
		dst.obj_id = FH_OBJ_ENC;
		dst.dev_id = 0;
		dst.chn_id = 1;
		FH_SYS_UnBindbyDst(dst);

		/* 解绑 ISP → VPU_VI (ISP 输出到 VPSS 输入) */
		dst.obj_id = FH_OBJ_VPU_VI;
		dst.dev_id = 0;
		dst.chn_id = 0;
		FH_SYS_UnBindbyDst(dst);

		/* 注销编码器通道 (释放编码器内部缓冲区和 DSP 资源) */
		FH_VENC_DestroyChn(0);
		FH_VENC_DestroyChn(1);

		/* 关闭并注销 VPSS 通道 (必须先 Close 再 Destroy) */
		FH_VPSS_CloseChn(GROUP_ID, 0);
		FH_VPSS_CloseChn(GROUP_ID, 1);
		FH_VPSS_DestroyChn(GROUP_ID, 0);
		FH_VPSS_DestroyChn(GROUP_ID, 1);

		/* 停用并注销 VPSS 组 */
		FH_VPSS_Disable(GROUP_ID);
		FH_VPSS_DestroyGrp(GROUP_ID);
	}

	/* 系统层退出 — 所有绑定和通道已释放，可完整复位硬件模块 */
	FH_SYS_Exit();

	dmc_record_unsubscribe();

	/* 停止控制服务器 */
	control_server_stop();

	/* 停止 LED 自动状态机线程 */
	g_led_auto_thread_running = 0;
	/* 停止 LED 闪烁线程 */
	g_led_thread_running = 0;

	/* 停止自动反色线程 */
	g_invert_thread_running = 0;

	/* 关闭录像文件 */
	if (g_record_file)
	{
		fclose(g_record_file);
		g_record_file = NULL;
	}

	return 0;
}
