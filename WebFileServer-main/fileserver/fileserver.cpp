/**
 * @file fileserver.cpp
 * @brief WebServer 实现：监听套接字、epoll 主循环、线程池派发、信号/管道（预留）
 *
 * 职责划分：
 * - 本文件只负责「基础设施 + 事件发现 + 投递到线程池」
 * - 具体 HTTP/文件逻辑在 event/myevent.cpp 的 AcceptConn / HandleRecv / HandleSend 中
 */

#include "fileserver.h"

WebServer::WebServer(){
    // 成员默认值在类内或首次使用前初始化；
}

WebServer::~WebServer(){
}

/**
 * @brief 创建监听套接字并 bind + listen
 * @param port 监听端口（主机字节序，内部会 htons）
 * @param ip  若非 nullptr 则绑定到该 IP；否则 INADDR_ANY（本机所有网卡）
 * @return 0 成功；负数表示失败步骤（-1 socket，-2 reuseaddr，-3 bind，-4 listen）
 */
int WebServer::createListenFd(int port, const char* ip){
    // 指定地址
    bzero(&m_serverAddr, sizeof(m_serverAddr));
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(port);
    // 根据传入的 ip 确定是否指定 ip 地址
    if(ip != nullptr){
        // 绑定到特定 IP
        m_serverAddr.sin_addr.s_addr = inet_addr(ip);
    }else{
        // 绑定到所有 IP
        m_serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    // 创建 TCP 套接字
    m_listenfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(m_listenfd < 0){
        std::cout << outHead("error") << "套接字创建失败" << std::endl;
        return -1;
    }

    int ret = 0;        // 保存函数执行的返回结果

    // 设置地址可重用，避免 TIME_WAIT 等导致短时间内无法再次 bind 同一端口
    int reuseAddr = 1;
    ret = setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
    if(ret != 0){
        std::cout << outHead("error") << "套接字设置地址重用失败" << std::endl;
        return -2;
    }

    // 绑定地址信息
    ret = bind(m_listenfd, (sockaddr*)&m_serverAddr, sizeof(m_serverAddr));
    if(ret != 0){
        std::cout << outHead("error") << "套接字绑定地址失败" << std::endl;
        return -3;
    }

    // 开启监听，backlog=5（等待 accept 的连接队列长度）
    ret = listen(m_listenfd, 5);
    if(ret != 0){
        std::cout << outHead("error") << "套接字开启监听失败" << std::endl;
        return -4;
    }

    return 0;
}

/**
 * @brief 创建 epoll 实例，后续所有 fd 都注册到 m_epollfd
 * @return 0 成功；-1 创建失败
 */
int WebServer::createEpoll(){
    // 参数为内核要监听的文件描述符数量（仅是提示）
    m_epollfd = epoll_create(100);
    if(m_epollfd < 0){
        std::cout << outHead("error") << "创建 epoll 失败" << std::endl;
        return -1;
    }
    return 0;
}

/**
 * @brief 将监听套接字加入 epoll：非阻塞 + 水平/边沿由 addWaitFd 第三个参数决定（此处为边沿触发）
 * @note 监听 fd 未使用 EPOLLONESHOT，与客户端连接 fd 不同
 * @return 0 成功；-1 添加失败
 */
int WebServer::epollAddListenFd(){
    // ListenFd 设为非阻塞，便于与 epoll 配合
    setNonBlocking(m_listenfd);
    // 边缘触发：有新连接时通知一次，避免 accept 交给工作线程前事件风暴
    int ret = addWaitFd(m_epollfd, m_listenfd, true, false);
    if(ret != 0){
        std::cout << outHead("error") << "添加监控 Listen 套接字失败" << std::endl;
        return -1;
    }
    std::cout << outHead("info") << "epoll 中添加监听套接字成功" << std::endl;
    return 0;
}

/**
 * @brief 创建一对全双工 Unix 域套接字，读端加入 epoll，用于「信号处理函数 → 主循环」的异步通知（统一事件源）
 * @note 需配合 addHandleSig / setSigHandler 使用；main 中若未调用则管道可能未启用
 * @return 0 成功；负数各步失败
 */
int WebServer::epollAddEventPipe(){
    int ret = socketpair(PF_UNIX, SOCK_STREAM, IPPROTO_TCP, eventHandlerPipe);
    if(ret != 0){
        std::cout << outHead("error") << "创建双向管道失败" << std::endl;
        return -1;
    }
    ret = setNonBlocking(eventHandlerPipe[0]);
    if(ret != 0){
        std::cout << outHead("error") << "设置 pipe[0] 非阻塞失败" << std::endl;
        return -2;
    }
    ret = setNonBlocking(eventHandlerPipe[1]);
    if(ret != 0){
        std::cout << outHead("error") << "设置 pipe[1] 非阻塞失败" << std::endl;
        return -3;
    }
    // 只监听读端：信号处理里往 pipe[1] 写，主线程在 pipe[0] 上收到 EPOLLIN
    ret = addWaitFd(m_epollfd, eventHandlerPipe[0]);
    if(ret != 0){
        std::cout << outHead("error") << "添加监控 pipe[0] 失败" << std::endl;
        return -4;
    }
    return 0;
}


/**
 * @brief 注册信号处理函数到指定信号
 * @param signo 若为 -1，则一次性注册 SIGINT / SIGTERM / SIGALRM；否则注册单个 signo
 * @return 0 成功；-1 sigaction 失败
 */
int WebServer::addHandleSig(int signo){
    int ret = 0;
    // 当参数 signo 为 -1 时，表示添加对默认信号的处理
    if(signo == -1){
        // 处理 SIGINT 信号（如 Ctrl+C）
        struct sigaction actINT;
        actINT.sa_handler = setSigHandler;
        sigfillset(&actINT.sa_mask);
        actINT.sa_flags = 0;
        ret = sigaction(SIGINT, &actINT, nullptr);
        if(ret != 0){
            std::cout << outHead("error") << "SIGINT 指定信号处理函数失败" << std::endl;
            return -1;
        }
        // 处理 SIGTERM 信号
        struct sigaction actTERM;
        actTERM.sa_handler = setSigHandler;
        sigfillset(&actTERM.sa_mask);
        actTERM.sa_flags = 0;
        ret = sigaction(SIGTERM, &actTERM, nullptr);
        if(ret != 0){
            std::cout << outHead("error") << "SIGTERM 指定信号处理函数失败" << std::endl;
            return -1;
        }
        // 处理 SIGALRM 信号
        struct sigaction actALRM;
        actALRM.sa_handler = setSigHandler;
        sigfillset(&actALRM.sa_mask);
        actALRM.sa_flags = 0;
        ret = sigaction(SIGALRM, &actALRM, nullptr);
        if(ret != 0){
            std::cout << outHead("error") << "SIGALRM 指定信号处理函数失败" << std::endl;
            return -1;
        }
        return 0;
    }

    // 参数 signo 不为 -1 时，表示只注册某一个信号
    struct sigaction act;
    act.sa_handler = setSigHandler;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    ret = sigaction(signo, &act, nullptr);
    if(ret != 0){
        std::cout << outHead("error") << "指定信号处理函数失败" << std::endl;
        return -1;
    }
    return 0;
}


/**
 * @brief 异步信号安全路径：SIGINT/SIGTERM 置停止标志；其它信号通过管道通知 epoll
 * @note 此处用 (signo & SIGINT) | (signo & SIGTERM) 判断退出意图
 */
void WebServer::setSigHandler(int signo){
    if((signo & SIGINT) | (signo & SIGTERM)){
        isStop = true;
        return;
    }
    // 其他信号 → 通过管道安全地传递给主循环
    int saveErrno = errno;
    int msg = signo;
    int ret = send(eventHandlerPipe[1], &msg, 1, 0);
    if(ret != 0){
        std::cout << outHead("error") << "信号处理失败" << std::endl;
    }
    errno = saveErrno;
}

/**
 * @brief 主线程事件循环：epoll_wait → 封装成 EventBase → 线程池 appendEvent
 *
 * 事件映射：
 * - m_listenfd 可读          → AcceptConn（accept 新连接并加入 epoll）
 * - eventHandlerPipe[0] 可读 → 预留「信号事件」（当前未 new 具体事件对象，event 仍为 nullptr 则跳过）
 * - 其它 fd 可读             → HandleRecv（读请求）
 * - 其它 fd 可写             → HandleSend（发响应）
 *
 * @return 正常退出循环返回 0；epoll_wait 致命错误返回 -1
 */
int WebServer::waitEpoll(){
    isStop = false;

    EventBase *event = nullptr;

    while(!isStop){
        int resNum = epoll_wait(m_epollfd, resEvents, MAX_RESEVENT_SIZE, -1);
        // 被信号中断时 errno==EINTR，属正常情况，不应当作错误退出
        if(resNum < 0 && errno != EINTR ){
            std::cout << outHead("error") << "epoll_wait 执行错误" << std::endl;
            return -1;
        }
        std::string eventType;
        for(int i = 0; i < resNum; ++i){
            int resfd = resEvents[i].data.fd;
            if(resfd == m_listenfd){
                std::cout << outHead("info") << "有新的连接请求" << std::endl;
                event = new AcceptConn(m_listenfd, m_epollfd);
                eventType = "新连接事件";
            }else if((resfd == eventHandlerPipe[0]) && (resEvents[i].events & EPOLLIN)){
                // 管道读端有数据 → 有信号到达
                eventType = "新信号事件";
            }else if(resEvents[i].events & EPOLLIN){
                event = new HandleRecv(resEvents[i].data.fd, m_epollfd);
                eventType = "新可读事件";

            }else if(resEvents[i].events & EPOLLOUT){
                event = new HandleSend(resEvents[i].data.fd, m_epollfd);
                eventType = "新可写事件";
            }
            if(event == nullptr){
                continue;
            }
            // 工作线程执行 process() 后 delete 该事件对象
            threadPool->appendEvent(event, eventType);

            event = nullptr;
        }
    }
    return 0;
}

/**
 * @brief 创建工作线程池，供 waitEpoll 中投递事件使用
 * @param threadNum 线程数量
 * @return 0 成功；-1 创建失败（new 抛异常或指针为空）
 */
int WebServer::createThreadPool(int threadNum){
    try{
        threadPool = new ThreadPool(threadNum);
    }catch(std::runtime_error &err){
        std::cout << err.what() << std::endl;
    }
    if(threadPool == nullptr){
        std::cout << outHead("error") << "线程池创建失败" << std::endl;
        return -1;
    }
    return 0;
}

// ---------- 静态成员定义（在类外分配存储，并给出初值）----------

/** epoll 实例 fd，-1 表示尚未 createEpoll */
int WebServer::m_epollfd = -1;

/** 为 true 时 waitEpoll 主循环结束（如收到 SIGINT/SIGTERM 时 setSigHandler 会置位） */
bool WebServer::isStop = false;

/** 信号通知管道：与 epollAddEventPipe 配对使用 */
int WebServer::eventHandlerPipe[2] = {-1, -1};
