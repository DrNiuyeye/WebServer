/**
 * @file threadpool.cpp
 * @brief 线程池实现：生产者（主线程 appendEvent）与消费者（工作线程 run）模型
 *
 * 同步原语：
 * - queueLocker：保护 m_workQueue 的互斥锁
 * - queueEventNum：计数信号量，队列非空时唤醒阻塞在 sem_wait 的工作线程
 *
 * 注意：析构时未向工作线程发送退出信号，线程会一直阻塞在 sem_wait（教学代码简化写法）
 */

#include "threadpool.h"

/**
 * @brief 构造线程池：初始化锁与信号量，并创建 m_threadNum 个 detached 工作线程
 * @param threadNum 工作线程数量
 * @throws std::runtime_error 互斥量/信号量初始化失败、线程创建或 detach 失败
 *
 * @note 全局静态变量 tnum 在每个线程创建间隔中递增，供 run() 里打印「线程序号」（非严格线程 ID）
 */
ThreadPool::ThreadPool(int threadNum) : m_threadNum(threadNum){
    int ret = pthread_mutex_init(&queueLocker, nullptr);
    if(ret != 0){
        throw std::runtime_error("初始化互斥量失败");
    }

    // 第二个参数 0 表示线程间共享；初值 0 表示初始队列为空，所有 worker 会阻塞在 sem_wait
    ret = sem_init(&queueEventNum, 0, 0);
    if(ret != 0){
        throw std::runtime_error("初始化信号量失败");
    }

    m_threads = new pthread_t[m_threadNum];
    for(int i = 0; i < m_threadNum; ++i){
        ret = pthread_create(m_threads + i, nullptr, worker, this);
        if(ret != 0){
            delete[] m_threads;
            throw std::runtime_error("线程创建失败");
        }
        // detached：线程结束时资源由系统回收，主线程不再等待
        ret = pthread_detach(m_threads[i]);
        ++tnum;
        usleep(1000);     // 错开递增 tnum，使各线程 run() 中读到的 threadN 尽量不同（仍非严谨编号方式）
        if(ret != 0){
            delete[] m_threads;
            throw std::runtime_error("设置脱离线程失败");
        }
    }
}


ThreadPool::~ThreadPool(){
    pthread_mutex_destroy(&queueLocker);
    sem_destroy(&queueEventNum);
    delete[] m_threads;
}

/**
 * @brief 生产者接口：将事件指针入队并 sem_post 唤醒一个等待中的工作线程
 * @param event     由调用方 new 出来，成功入队后由工作线程 process() 结束再 delete
 * @param eventType 仅用于日志描述（如「新可读事件」）
 * @return 0 成功；-1 加锁失败；-2/-3 为历史分支返回值（解锁/post 的 ret 未单独保存，与加锁 ret 混用）
 */
int ThreadPool::appendEvent(EventBase* event, const std::string eventType){
    int ret = 0;
    ret = pthread_mutex_lock(&queueLocker);
    if(ret != 0){
        std::cout << outHead("error") << "事件队列加锁失败" << std::endl;
        return -1;
    }
    m_workQueue.push(event);
    std::cout << outHead("info") << eventType << "添加成功，线程池事件队列中剩余的事件个数：" << m_workQueue.size() << std::endl;
    ret = pthread_mutex_unlock(&queueLocker);
    if(ret != 0){
        std::cout << outHead("error") << "事件队列解锁失败" << std::endl;
        return -2;
    }
    ret = sem_post(&queueEventNum);
    if(ret != 0){
        std::cout << outHead("error") << "事件队列信号量 post 失败" << std::endl;
        return -3;
    }

    return 0;
}


/**
 * @brief pthread 入口：把 void* 转回 ThreadPool*，调用成员函数 run()
 */
void *ThreadPool::worker(void *arg){
    ThreadPool *thiz = static_cast<ThreadPool*>(arg);
    thiz->run();
    return thiz;
}


/**
 * @brief 工作线程主循环：P 操作等待队列非空 → 加锁取队首 → 解锁 → 执行 process() → delete 事件
 *
 * 与 appendEvent 的配对关系：
 * appendEvent: lock → push → unlock → sem_post
 * run:         sem_wait → lock → pop → unlock → process → delete
 */
void ThreadPool::run(){
    int threadN = tnum;
    std::cout << outHead("info") << "线程 " << threadN << " 正在执行" << std::endl;
    while(1){
        int ret = sem_wait(&queueEventNum);
        if(ret != 0){
            std::cout << outHead("error") << "等待队列事件失败" << std::endl;
            return;
        }
        std::cout << outHead("log") << "线程 " << threadN << " 收到事件" << std::endl;
        // 加锁取任务
        ret = pthread_mutex_lock(&queueLocker);
        if(ret != 0){
            std::cout << outHead("error") << "ThreadPool:run() : 事件队列加锁失败" << std::endl;
            return;
        }
        EventBase* curEvent = m_workQueue.front();
        m_workQueue.pop();

        ret = pthread_mutex_unlock(&queueLocker);
        if(ret != 0){
            std::cout << outHead("error") << "ThreadPool:run() : 事件队列解锁失败" << std::endl;
            return;
        }

        if(curEvent == nullptr){
            continue;
        }
        std::cout << outHead("info") << "线程 " << threadN << " 开始处理事件" << std::endl;
        curEvent->process();
        std::cout << outHead("info") << "线程 " << threadN << " 处理事件完成" << std::endl;
        delete curEvent;
    }
}
