#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <pthread.h>
#include <queue>
#include <sys/time.h>

struct EncodedFrame {
    unsigned char *data;
    int            size;
    struct timeval pts;
    bool           is_key_frame;
};

class FrameQueue {
public:
    explicit FrameQueue(unsigned int maxSize = 30);
    ~FrameQueue();

    bool push(const unsigned char *data, int size,
              struct timeval pts, bool is_key_frame);
    bool pop(EncodedFrame &frame);
    void releaseFrame(EncodedFrame &frame);
    void shutdown();
    bool isShutdown() const;

private:
    std::queue<EncodedFrame> m_queue;
    pthread_mutex_t          m_mutex;
    unsigned int             m_maxSize;
    bool                     m_shutdown;
};

#endif
