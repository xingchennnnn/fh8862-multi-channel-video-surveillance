#ifndef H264_SUBSESSION_H
#define H264_SUBSESSION_H

#include <OnDemandServerMediaSubsession.hh>

class FrameQueue;

class H264LiveServerMediaSubsession : public OnDemandServerMediaSubsession {
public:
    static H264LiveServerMediaSubsession *
    createNew(UsageEnvironment &env, FrameQueue *queue,
              unsigned estBitrateKbps = 2048);

protected:
    H264LiveServerMediaSubsession(UsageEnvironment &env,
                                   FrameQueue *queue,
                                   unsigned estBitrateKbps);
    virtual ~H264LiveServerMediaSubsession();

private:
    virtual FramedSource *createNewStreamSource(
        unsigned clientSessionId, unsigned &estBitrate) override;

    virtual RTPSink *createNewRTPSink(
        Groupsock *rtpGroupsock,
        unsigned char rtpPayloadTypeIfDynamic,
        FramedSource *inputSource) override;

    FrameQueue *m_queue;
    unsigned    m_estBitrateKbps;
};

#endif
