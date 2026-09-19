#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动 TCP 控制服务器线程，监听指定端口。
 * 用于接收来自 Web 前端的控制指令（旋转、OSD颜色、录像、时间同步等）。
 *
 * 参数: port - 监听端口 (建议 9999)
 * 返回值: 0=成功, -1=失败
 */
int control_server_start(int port);

/*
 * 停止控制服务器。
 */
void control_server_stop(void);

/*
 * 设置 LED 模式。
 * mode: 0=灭, 1=亮, 2=闪烁
 * 录像期间只更新基础状态，物理 LED 保持闪烁；录像结束后恢复该基础状态。
 */
int control_led_set(int mode);

/*
 * 录像 LED 覆盖控制。
 * begin 后物理 LED 强制闪烁；end 到引用计数归零后恢复 control_led_set 记录的基础状态。
 */
int control_led_record_begin(void);
int control_led_record_end(void);

/*
 * 同步板端系统时间。
 * timestamp_ms: Unix 毫秒时间戳
 */
int control_time_sync(long long timestamp_ms);

/* Mask 管理 — 纯色/马赛克遮挡区域 CRUD */
int mask_set_region(int idx, int enable, int x, int y, int w, int h);
int mask_del_region(int idx);
int mask_set_type(int type);
int mask_set_mosaic_size(int size);
int mask_set_color(int r, int g, int b);
int mask_set_master_enable(int en);

#ifdef __cplusplus
}
#endif

#endif
