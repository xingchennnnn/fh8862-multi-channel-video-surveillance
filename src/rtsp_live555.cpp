#include "rtsp_live555.h"
#include "frame_queue.h"
#include "h264_subsession.h"
#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>

/* ============================================================================
 * 全局状态
 * ============================================================================ */

static TaskScheduler    *g_scheduler    = NULL;
static UsageEnvironment *g_env          = NULL;
static RTSPServer       *g_rtspServer   = NULL;
static FrameQueue       *g_mainQueue    = NULL;
static FrameQueue       *g_subQueue     = NULL;
static char              g_eventLoopExit = 0;
int                      g_rtsp_client_count = 0;

/* ============================================================================
 * C API 实现
 * ============================================================================ */

int rtsp_live555_start(int port)
{
    if (g_scheduler != NULL)
    {
        return 0; /* 已启动 */
    }

    /* 步骤 1: 创建任务调度器和运行环境 */
    g_scheduler = BasicTaskScheduler::createNew();
    if (!g_scheduler)
    {
        return -1;
    }

    g_env = BasicUsageEnvironment::createNew(*g_scheduler);
    if (!g_env)
    {
        delete g_scheduler;
        g_scheduler = NULL;
        return -1;
    }

    /*
     * 增大 OutPacketBuffer::maxSize，避免主码流大帧触发不必要的分片。
     * 默认值 100KB 对 720p I 帧可能不够; 设为 2MB 留足余量。
     */
    OutPacketBuffer::increaseMaxSizeTo(2 * 1024 * 1024);

    /* 步骤 2: 创建帧队列 */
    /*
     * 主码流 1280×720 2Mbps I 帧可达 50-100KB+，需大量 RTP 包发送。
     * Live555 事件循环发送大量 RTP 包时，编码器线程持续生产，
     * 队列过小会导致 I 帧被丢弃，P 帧失去参考帧 → 花屏。
     * 50 帧 ≈ 2 秒缓冲，给予足够的消费时间。
     */
    g_mainQueue = new FrameQueue(50); /* 主码流: 50 帧缓冲 (~2s) */
    /*
     * 子码流 704×576 700kbps 帧小、发送快，30 帧已足够防御。
     */
    g_subQueue  = new FrameQueue(30); /* 子码流: 30 帧缓冲 (~1.2s) */

    /* 步骤 3: 创建 RTSP 服务器 */
    g_rtspServer = RTSPServer::createNew(*g_env, port, NULL);
    if (!g_rtspServer)
    {
        *g_env << "Failed to create RTSP server: "
               << g_env->getResultMsg() << "\n";
        delete g_mainQueue;
        delete g_subQueue;
        g_env->reclaim();
        delete g_scheduler;
        g_scheduler = NULL;
        g_env = NULL;
        return -1;
    }

    /* 步骤 4: 注册主码流会话 "/main" — 1280×720, ~2Mbps */
    {
        ServerMediaSession *sms = ServerMediaSession::createNew(
            *g_env, "main", "main",
            "FH8862 Main Stream (1280x720 H.264)");
        sms->addSubsession(
            H264LiveServerMediaSubsession::createNew(*g_env, g_mainQueue, 2048));
        g_rtspServer->addServerMediaSession(sms);

        char *url = g_rtspServer->rtspURL(sms);
        *g_env << "Main stream URL: " << url << "\n";
        delete[] url;
    }

    /* 步骤 5: 注册子码流会话 "/sub" — 704×576, ~700kbps */
    {
        ServerMediaSession *sms = ServerMediaSession::createNew(
            *g_env, "sub", "sub",
            "FH8862 Sub Stream (704x576 H.264)");
        sms->addSubsession(
            H264LiveServerMediaSubsession::createNew(*g_env, g_subQueue, 700));
        g_rtspServer->addServerMediaSession(sms);

        char *url = g_rtspServer->rtspURL(sms);
        *g_env << "Sub stream URL: " << url << "\n";
        delete[] url;
    }

    /* 步骤 6: 进入事件循环 (阻塞, 直到 g_eventLoopExit 被设为非零) */
    g_eventLoopExit = 0;
    g_env->taskScheduler().doEventLoop(&g_eventLoopExit);

    /* 事件循环退出后的清理 */
    Medium::close(g_rtspServer);
    g_rtspServer = NULL;

    g_mainQueue->shutdown();
    g_subQueue->shutdown();
    delete g_mainQueue;
    delete g_subQueue;
    g_mainQueue = NULL;
    g_subQueue = NULL;

    g_env->reclaim();
    g_env = NULL;
    delete g_scheduler;
    g_scheduler = NULL;

    return 0;
}

int rtsp_live555_get_client_count(void)
{
    return __atomic_load_n(&g_rtsp_client_count, __ATOMIC_RELAXED);
}

void rtsp_live555_stop(void)
{
    /* 设置退出标志，使 doEventLoop 返回 */
    g_eventLoopExit = 1;
}

void rtsp_live555_push_frame(int stream_id,
                             const unsigned char *data,
                             int len,
                             int is_key_frame)
{
    FrameQueue *queue = NULL;
    struct timeval now;

    if (!data || len <= 0)
    {
        return;
    }

    if (stream_id == RTSP_STREAM_MAIN)
    {
        queue = g_mainQueue;
    }
    else if (stream_id == RTSP_STREAM_SUB)
    {
        queue = g_subQueue;
    }

    if (!queue || queue->isShutdown())
    {
        return;
    }

    gettimeofday(&now, NULL);
    queue->push(data, len, now, is_key_frame != 0);
}
