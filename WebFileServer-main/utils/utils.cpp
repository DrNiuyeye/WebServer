/**
 * @file utils.cpp
 * @brief 通用工具：日志前缀、epoll 注册/修改/删除、fd 非阻塞
 *
 * 与业务解耦，供 fileserver、myevent 等模块调用。
 */

#include "utils.h"

/**
 * @brief 生成日志行前缀：本地时间 + 微秒 + 级别标签
 * @param logType "init" | "error" | 其它（均按 info 处理）
 * @return 形如 "16:31:15.69568 2025-05-10 [info]: "（error 分支文案为 [erro]，与历史拼写一致）
 *
 * @note 使用 localtime()，多线程同时打日志时非线程安全；高并发可改为 localtime_r 或统一日志锁
 */
std::string outHead(const std::string logType){
    auto now = std::chrono::system_clock::now();
    time_t tt = std::chrono::system_clock::to_time_t(now);
    auto time_tm = localtime(&tt);

    struct timeval time_usec;
    gettimeofday(&time_usec, NULL);

    char strTime[30] = { 0 };
    // 时:分:秒.微秒(取前 5 位显示) 年-月-日
    sprintf(strTime, "%02d:%02d:%02d.%05ld %d-%02d-%02d",
            time_tm->tm_hour, time_tm->tm_min, time_tm->tm_sec, time_usec.tv_usec,
            time_tm->tm_year + 1900, time_tm->tm_mon + 1, time_tm->tm_mday);

    std::string outStr;
    outStr += strTime;
    if(logType == "init"){
        outStr += " [init]: ";
    }else if(logType == "error"){
        outStr += " [erro]: ";
    }else{
        outStr += " [info]: ";
    }
    return outStr;
}


/**
 * @brief 将 fd 加入 epoll，默认监听可读 EPOLLIN
 * @param epollFd     epoll 实例
 * @param newFd       待监视的 fd
 * @param edgeTrigger true 则或上 EPOLLET（边沿触发）
 * @param isOneshot   true 则或上 EPOLLONESHOT（触发一次后需 epoll_ctl MOD 重新启用）
 * @return 0 成功；-1 失败（epoll_ctl 返回 -1，本函数未区分 errno）
 */
int addWaitFd(int epollFd, int newFd, bool edgeTrigger, bool isOneshot){
    epoll_event event;
    event.data.fd = newFd;

    event.events = EPOLLIN;
    if(edgeTrigger){
        event.events |= EPOLLET;
    }
    if(isOneshot){
        event.events |= EPOLLONESHOT;
    }

    int ret = epoll_ctl(epollFd, EPOLL_CTL_ADD, newFd, &event);
    if(ret != 0){
        std::cout << outHead("error") << "添加文件描述符失败" << std::endl;
        return -1;
    }
    return 0;
}

/**
 * @brief 修改已在 epoll 中的 fd 的监听事件（EPOLL_CTL_MOD）
 * @param modFd        已注册的 fd
 * @param edgeTrigger  是否 EPOLLET
 * @param resetOneshot 是否带上 EPOLLONESHOT（处理完一次事件后需再次 MOD 才能再收到）
 * @param addEpollout  true 则同时监听 EPOLLOUT（可写），用于触发 HandleSend
 * @return 0 成功；-1 失败
 *
 * @note 每次 MOD 都会从「仅 EPOLLIN」重新组合标志位；本项目中通常 IN 与 OUT 同时需要时一并设置
 */
int modifyWaitFd(int epollFd, int modFd, bool edgeTrigger, bool resetOneshot, bool addEpollout){
    epoll_event event;
    event.data.fd = modFd;

    event.events = EPOLLIN;

    if(edgeTrigger){
        event.events |= EPOLLET;
    }
    if(resetOneshot){
        event.events |= EPOLLONESHOT;
    }
    if(addEpollout){
        event.events |= EPOLLOUT;
    }

    int ret = epoll_ctl(epollFd, EPOLL_CTL_MOD, modFd, &event);
    if(ret != 0){
        std::cout << outHead("error") << "修改文件描述符失败" << std::endl;
        return -1;
    }
    return 0;
}

/**
 * @brief 从 epoll 实例中移除对某 fd 的监视（EPOLL_CTL_DEL）
 * @return 0 成功；-1 失败
 */
int deleteWaitFd(int epollFd, int deleteFd){
    int ret = epoll_ctl(epollFd, EPOLL_CTL_DEL, deleteFd, nullptr);
    if(ret != 0){
        std::cout << outHead("error") << "删除监听的文件描述符失败" << std::endl;
        return -1;
    }
    return 0;
}

/**
 * @brief 为 fd 设置 O_NONBLOCK，与 epoll 配合：read/write 在无数据/缓冲区满时返回 EAGAIN 而非阻塞
 * @return 0 成功；-1 失败（如 fd 无效）
 */
int setNonBlocking(int fd){
    int oldFlag = fcntl(fd, F_GETFL);
    int ret = fcntl(fd, F_SETFL, oldFlag | O_NONBLOCK);
    if(ret != 0){
        return -1;
    }
    return 0;
}
