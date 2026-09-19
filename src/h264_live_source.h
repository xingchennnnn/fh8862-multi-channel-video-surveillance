#ifndef H264_LIVE_SOURCE_H
#define H264_LIVE_SOURCE_H

#include <FramedSource.hh>

class FrameQueue;

class H264LiveSource : public FramedSource {
public:
    static H264LiveSource *createNew(UsageEnvironment &env,
                                      FrameQueue *queue);

protected:
    H264LiveSource(UsageEnvironment &env, FrameQueue *queue);
    virtual ~H264LiveSource();

private:
    virtual void doGetNextFrame() override;
    static void deliverFrameCallback(void *clientData);
    void deliverFrame();

    FrameQueue *m_queue;

    /* 分片交付: 单帧超过 fMaxSize 时，分多次 doGetNextFrame() 交付 */
    unsigned char *m_currentData;
    int            m_currentSize;
    int            m_currentOffset;
    struct timeval m_currentPts;
};

#endif
