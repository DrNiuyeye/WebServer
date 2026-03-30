#include "threadpool.h"

namespace {
int tnum = 0;
}

ThreadPool::ThreadPool(int threadNum) : m_threadNum(threadNum), m_threads(nullptr) {
    int ret = pthread_mutex_init(&queueLocker, nullptr);
    if (ret != 0) {
        throw std::runtime_error("mutex init failed");
    }

    ret = sem_init(&queueEventNum, 0, 0);
    if (ret != 0) {
        throw std::runtime_error("semaphore init failed");
    }

    m_threads = new pthread_t[m_threadNum];
    for (int i = 0; i < m_threadNum; ++i) {
        ret = pthread_create(m_threads + i, nullptr, worker, this);
        if (ret != 0) {
            delete[] m_threads;
            m_threads = nullptr;
            throw std::runtime_error("thread create failed");
        }

        ret = pthread_detach(m_threads[i]);
        ++tnum;
        usleep(1000);
        if (ret != 0) {
            delete[] m_threads;
            m_threads = nullptr;
            throw std::runtime_error("thread detach failed");
        }
    }
}

ThreadPool::~ThreadPool() {
    pthread_mutex_destroy(&queueLocker);
    sem_destroy(&queueEventNum);
    delete[] m_threads;
}

int ThreadPool::appendEvent(EventBase* event, const std::string eventType) {
    (void)eventType;

    int ret = pthread_mutex_lock(&queueLocker);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    m_workQueue.push(event);
    logStream("info") << "info" << std::endl;

    ret = pthread_mutex_unlock(&queueLocker);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -2;
    }

    ret = sem_post(&queueEventNum);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -3;
    }

    return 0;
}

size_t ThreadPool::pendingEventCount() {
    size_t count = 0;
    int ret = pthread_mutex_lock(&queueLocker);
    if (ret != 0) {
        return 0;
    }

    count = m_workQueue.size();
    pthread_mutex_unlock(&queueLocker);
    return count;
}

void* ThreadPool::worker(void* arg) {
    ThreadPool* thiz = static_cast<ThreadPool*>(arg);
    thiz->run();
    return thiz;
}

void ThreadPool::run() {
    int threadN = tnum;
    (void)threadN;
    logStream("info") << "info" << std::endl;

    while (1) {
        int ret = sem_wait(&queueEventNum);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return;
        }

        logStream("log") << "log" << std::endl;

        ret = pthread_mutex_lock(&queueLocker);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return;
        }

        EventBase* curEvent = m_workQueue.front();
        m_workQueue.pop();

        ret = pthread_mutex_unlock(&queueLocker);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return;
        }

        if (curEvent == nullptr) {
            continue;
        }

        logStream("info") << "info" << std::endl;
        curEvent->process();
        logStream("info") << "info" << std::endl;
        delete curEvent;
    }
}
