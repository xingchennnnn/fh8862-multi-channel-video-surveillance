#ifndef RTSP_LIVE555_H
#define RTSP_LIVE555_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_STREAM_MAIN 0
#define RTSP_STREAM_SUB  1

/*
 * 启动 live555 RTSP 服务器。
 * 创建 RTSPServer，注册 /main 和 /sub 两个会话，
 * 然后进入事件循环 (阻塞)。
 *
 * 参数:
 *   port - RTSP 监听端口 (建议 8554)
 *
 * 返回值:
 *   0  - 成功启动并正常退出
 *   -1 - 启动失败
 */
int rtsp_live555_start(int port);

/*
 * 获取当前连接的 RTSP 客户端数量。
 * 可在任意线程调用。
 */
int rtsp_live555_get_client_count(void);

/*
 * 停止 live555 RTSP 服务器。
 * 设置事件循环退出标志，关闭帧队列。
 *
 * 线程安全: 可在任意线程调用 (用于信号处理)。
 */
void rtsp_live555_stop(void);

/*
 * 向指定码流的帧队列推送一帧 H.264 数据。
 *
 * 由编码器线程调用。
 *
 * 参数:
 *   stream_id    - RTSP_STREAM_MAIN(0) 或 RTSP_STREAM_SUB(1)
 *   data         - H.264 Annex-B 格式码流数据 (带 00 00 00 01 起始码)
 *   len          - 数据长度 (字节)
 *   is_key_frame - 是否关键帧 (1=IDR, 0=P帧)
 */
void rtsp_live555_push_frame(int stream_id,
                             const unsigned char *data,
                             int len,
                             int is_key_frame);

#ifdef __cplusplus
}
#endif

#endif
