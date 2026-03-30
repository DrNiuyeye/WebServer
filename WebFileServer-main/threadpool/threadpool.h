#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <cstddef>
#include <queue>
#include <stdexcept>

#include <pthread.h>
#include <semaphore.h>

#include "../event/myevent.h"

class ThreadPool {
public:
    explicit ThreadPool(int threadNum);
    ~ThreadPool();

    int appendEvent(EventBase* event, const std::string eventType);
    size_t pendingEventCount();

private:
    static void* worker(void* arg);
    void run();

    int m_threadNum;
    pthread_t* m_threads;
    std::queue<EventBase*> m_workQueue;
    pthread_mutex_t queueLocker;
    sem_t queueEventNum;
};

#endif
