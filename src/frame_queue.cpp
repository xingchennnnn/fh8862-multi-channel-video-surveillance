#include "frame_queue.h"
#include <stdlib.h>
#include <string.h>

FrameQueue::FrameQueue(unsigned int maxSize)
    : m_maxSize(maxSize), m_shutdown(false)
{
    pthread_mutex_init(&m_mutex, NULL);
}

FrameQueue::~FrameQueue()
{
    pthread_mutex_lock(&m_mutex);
    m_shutdown = true;
    /* 清空队列中剩余帧 */
    while (!m_queue.empty())
    {
        EncodedFrame &f = m_queue.front();
        free(f.data);
        m_queue.pop();
    }
    pthread_mutex_unlock(&m_mutex);
    pthread_mutex_destroy(&m_mutex);
}

bool FrameQueue::push(const unsigned char *data, int size,
                      struct timeval pts, bool is_key_frame)
{
    if (!data || size <= 0)
    {
        return false;
    }

    pthread_mutex_lock(&m_mutex);

    if (m_shutdown)
    {
        pthread_mutex_unlock(&m_mutex);
        return false;
    }

    /* 队列满: 丢最旧帧 */
    while (m_queue.size() >= m_maxSize)
    {
        EncodedFrame &old = m_queue.front();
        free(old.data);
        m_queue.pop();
    }

    EncodedFrame frame;
    frame.data = (unsigned char *)malloc(size);
    if (!frame.data)
    {
        pthread_mutex_unlock(&m_mutex);
        return false;
    }

    memcpy(frame.data, data, size);
    frame.size = size;
    frame.pts = pts;
    frame.is_key_frame = is_key_frame;

    m_queue.push(frame);
    pthread_mutex_unlock(&m_mutex);
    return true;
}

bool FrameQueue::pop(EncodedFrame &frame)
{
    pthread_mutex_lock(&m_mutex);

    if (m_queue.empty())
    {
        pthread_mutex_unlock(&m_mutex);
        return false;
    }

    frame = m_queue.front();
    m_queue.pop();
    pthread_mutex_unlock(&m_mutex);
    return true;
}

void FrameQueue::releaseFrame(EncodedFrame &frame)
{
    if (frame.data)
    {
        free(frame.data);
        frame.data = NULL;
    }
    frame.size = 0;
}

void FrameQueue::shutdown()
{
    pthread_mutex_lock(&m_mutex);
    m_shutdown = true;
    pthread_mutex_unlock(&m_mutex);
}

bool FrameQueue::isShutdown() const
{
    return m_shutdown;
}
