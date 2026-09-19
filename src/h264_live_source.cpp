#include "h264_live_source.h"
#include "frame_queue.h"
#include <string.h>

extern int g_rtsp_client_count;

H264LiveSource *H264LiveSource::createNew(UsageEnvironment &env,
                                           FrameQueue *queue)
{
    return new H264LiveSource(env, queue);
}

H264LiveSource::H264LiveSource(UsageEnvironment &env, FrameQueue *queue)
    : FramedSource(env), m_queue(queue),
      m_currentData(NULL), m_currentSize(0), m_currentOffset(0)
{
    m_currentPts.tv_sec = 0;
    m_currentPts.tv_usec = 0;
}

H264LiveSource::~H264LiveSource()
{
    __atomic_sub_fetch(&g_rtsp_client_count, 1, __ATOMIC_RELAXED);
    free(m_currentData);
}

void H264LiveSource::doGetNextFrame()
{
    /* 如果上一帧还有未交付完的数据，继续交付 */
    if (m_currentData != NULL)
    {
        int remaining = m_currentSize - m_currentOffset;
        if (remaining <= (int)fMaxSize)
        {
            fFrameSize = remaining;
            fNumTruncatedBytes = 0;
        }
        else
        {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = remaining - fMaxSize;
        }

        memcpy(fTo, m_currentData + m_currentOffset, fFrameSize);
        fPresentationTime = m_currentPts;
        fDurationInMicroseconds = 0;

        m_currentOffset += fFrameSize;

        if (fNumTruncatedBytes == 0)
        {
            /* 全部交付完成，释放帧数据 */
            free(m_currentData);
            m_currentData = NULL;
            m_currentSize = 0;
            m_currentOffset = 0;
        }

        FramedSource::afterGetting(this);
        return;
    }

    /* 从队列取新帧 */
    EncodedFrame frame;

    if (!m_queue->pop(frame))
    {
        envir().taskScheduler().scheduleDelayedTask(
            10000,
            (TaskFunc *)deliverFrameCallback,
            this);
        return;
    }

    if (!isCurrentlyAwaitingData())
    {
        m_queue->releaseFrame(frame);
        return;
    }

    if (frame.size <= (int)fMaxSize)
    {
        /* 帧完全适配 fMaxSize，一次交付 */
        fFrameSize = frame.size;
        fNumTruncatedBytes = 0;
        memcpy(fTo, frame.data, frame.size);
        fPresentationTime = frame.pts;
        fDurationInMicroseconds = 0;

        m_queue->releaseFrame(frame);
        FramedSource::afterGetting(this);
    }
    else
    {
        /* 帧超过 fMaxSize，分片交付: 第一片通过 fTo，剩余部分保存到
         * m_currentData，供下次 doGetNextFrame() 调用时继续交付 */
        fFrameSize = fMaxSize;
        fNumTruncatedBytes = frame.size - fMaxSize;

        memcpy(fTo, frame.data, fFrameSize);
        fPresentationTime = frame.pts;
        fDurationInMicroseconds = 0;

        /* 保存剩余数据 */
        int remaining = frame.size - fFrameSize;
        m_currentData = (unsigned char *)malloc(remaining);
        if (m_currentData)
        {
            memcpy(m_currentData, frame.data + fFrameSize, remaining);
            m_currentSize = remaining;
            m_currentOffset = 0;
            m_currentPts = frame.pts;
        }

        m_queue->releaseFrame(frame);
        FramedSource::afterGetting(this);
    }
}

void H264LiveSource::deliverFrameCallback(void *clientData)
{
    ((H264LiveSource *)clientData)->deliverFrame();
}

void H264LiveSource::deliverFrame()
{
    /* deliverFrame 和 doGetNextFrame 逻辑相同 */
    doGetNextFrame();
}
