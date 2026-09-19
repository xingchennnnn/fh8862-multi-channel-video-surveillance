#include "h264_subsession.h"
#include "h264_live_source.h"
#include "frame_queue.h"
#include <H264VideoStreamFramer.hh>
#include <H264VideoRTPSink.hh>

extern int g_rtsp_client_count;

H264LiveServerMediaSubsession *
H264LiveServerMediaSubsession::createNew(UsageEnvironment &env,
                                          FrameQueue *queue,
                                          unsigned estBitrateKbps)
{
    return new H264LiveServerMediaSubsession(env, queue, estBitrateKbps);
}

H264LiveServerMediaSubsession::H264LiveServerMediaSubsession(
    UsageEnvironment &env, FrameQueue *queue, unsigned estBitrateKbps)
    : OnDemandServerMediaSubsession(env, True /* reuseFirstSource */),
      m_queue(queue), m_estBitrateKbps(estBitrateKbps)
{
}

H264LiveServerMediaSubsession::~H264LiveServerMediaSubsession()
{
}

FramedSource *
H264LiveServerMediaSubsession::createNewStreamSource(
    unsigned /* clientSessionId */, unsigned &estBitrate)
{
    estBitrate = m_estBitrateKbps;

    __atomic_add_fetch(&g_rtsp_client_count, 1, __ATOMIC_RELAXED);

    H264LiveSource *liveSource =
        H264LiveSource::createNew(envir(), m_queue);

    /*
     * H264VideoStreamFramer 负责:
     *   1. 解析 Annex-B 字节流中的 NALU
     *   2. 缓存 SPS/PPS (用于 H264VideoRTPSink 生成 SDP)
     *   3. 检测帧边界 (用于设置 RTP marker bit)
     * includeStartCodeInOutput=False: 输出纯 NALU (不带起始码)，H264VideoRTPSink 预期此格式
     */
    return H264VideoStreamFramer::createNew(envir(), liveSource,
                                             False,   /* includeStartCodeInOutput */
                                             False); /* insertAccessUnitDelimiters */
}

RTPSink *
H264LiveServerMediaSubsession::createNewRTPSink(
    Groupsock *rtpGroupsock,
    unsigned char rtpPayloadTypeIfDynamic,
    FramedSource * /*inputSource*/)
{
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock,
                                        rtpPayloadTypeIfDynamic);
}
