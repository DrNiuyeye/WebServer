#include "fileserver.h"
#include <cstring>

WebServer::WebServer() : m_listenfd(-1), m_serverAddr{}, threadPool(nullptr) {}
WebServer::~WebServer() {}

int WebServer::createListenFd(int port, const char* ip) {
    std::memset(&m_serverAddr, 0, sizeof(m_serverAddr));
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(port);
    if (ip != nullptr) {
        m_serverAddr.sin_addr.s_addr = inet_addr(ip);
    } else {
        m_serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    m_listenfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenfd < 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    int ret = 0;
    int reuseAddr = 1;
    ret = setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -2;
    }

    ret = bind(m_listenfd, (sockaddr*)&m_serverAddr, sizeof(m_serverAddr));
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -3;
    }

    // Keep backlog high for bursty accepts.
    ret = listen(m_listenfd, 1024);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -4;
    }

    return 0;
}

int WebServer::createEpoll() {
    m_epollfd = epoll_create(100);
    if (m_epollfd < 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }
    return 0;
}

int WebServer::epollAddListenFd() {
    setNonBlocking(m_listenfd);
    int ret = addWaitFd(m_epollfd, m_listenfd, true, false);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }
    logStream("info") << "info" << std::endl;
    return 0;
}

int WebServer::epollAddEventPipe() {
    int ret = socketpair(PF_UNIX, SOCK_STREAM, IPPROTO_TCP, eventHandlerPipe);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    ret = setNonBlocking(eventHandlerPipe[0]);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -2;
    }

    ret = setNonBlocking(eventHandlerPipe[1]);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -3;
    }

    ret = addWaitFd(m_epollfd, eventHandlerPipe[0]);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -4;
    }

    return 0;
}

int WebServer::addHandleSig(int signo) {
    int ret = 0;

    if (signo == -1) {
        struct sigaction actINT;
        actINT.sa_handler = setSigHandler;
        sigfillset(&actINT.sa_mask);
        actINT.sa_flags = 0;
        ret = sigaction(SIGINT, &actINT, nullptr);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return -1;
        }

        struct sigaction actTERM;
        actTERM.sa_handler = setSigHandler;
        sigfillset(&actTERM.sa_mask);
        actTERM.sa_flags = 0;
        ret = sigaction(SIGTERM, &actTERM, nullptr);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return -1;
        }

        struct sigaction actALRM;
        actALRM.sa_handler = setSigHandler;
        sigfillset(&actALRM.sa_mask);
        actALRM.sa_flags = 0;
        ret = sigaction(SIGALRM, &actALRM, nullptr);
        if (ret != 0) {
            logStream("error") << "error" << std::endl;
            return -1;
        }

        return 0;
    }

    struct sigaction act;
    act.sa_handler = setSigHandler;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    ret = sigaction(signo, &act, nullptr);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    return 0;
}

void WebServer::setSigHandler(int signo) {
    if ((signo & SIGINT) | (signo & SIGTERM)) {
        isStop = true;
        return;
    }

    int saveErrno = errno;
    int msg = signo;
    int ret = send(eventHandlerPipe[1], &msg, 1, 0);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
    }
    errno = saveErrno;
}

int WebServer::waitEpoll() {
    isStop = false;

    EventBase* event = nullptr;
    auto lastStatAt = std::chrono::steady_clock::now();

    while (!isStop) {
        int resNum = epoll_wait(m_epollfd, resEvents, MAX_RESEVENT_SIZE, -1);
        if (resNum < 0 && errno != EINTR) {
            logStream("error") << "error" << std::endl;
            return -1;
        }

        if (resNum > 0) {
            statIncEpollWakeups(1);
            statIncEpollEvents(static_cast<uint64_t>(resNum));
        }

        std::string eventType;
        for (int i = 0; i < resNum; ++i) {
            int resfd = resEvents[i].data.fd;
            if (resfd == m_listenfd) {
                event = new AcceptConn(m_listenfd, m_epollfd);
                eventType = "accept";
            } else if ((resfd == eventHandlerPipe[0]) && (resEvents[i].events & EPOLLIN)) {
                eventType = "signal";
            } else if (resEvents[i].events & EPOLLIN) {
                event = new HandleRecv(resEvents[i].data.fd, m_epollfd);
                eventType = "read";
                statIncRecvEvents(1);
            } else if (resEvents[i].events & EPOLLOUT) {
                event = new HandleSend(resEvents[i].data.fd, m_epollfd);
                eventType = "write";
                statIncSendEvents(1);
            }

            if (event == nullptr) {
                continue;
            }

            threadPool->appendEvent(event, eventType);
            statIncEventDispatched(1);
            event = nullptr;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastStatAt >= std::chrono::seconds(1)) {
            RuntimeStatsSnapshot s = statConsumeSnapshot();
            size_t pending = 0;
            if (threadPool != nullptr) {
                pending = threadPool->pendingEventCount();
            }

            logStream("stat")
                << "1s summary"
                << " epoll_wakeups=" << s.epollWakeups
                << " epoll_events=" << s.epollEvents
                << " dispatched=" << s.eventDispatched
                << " accept_ok=" << s.acceptOk
                << " recv_events=" << s.recvEvents
                << " send_events=" << s.sendEvents
                << " error_logs=" << s.errorLogs
                << " queue_pending=" << pending
                << std::endl;

            lastStatAt = now;
        }
    }

    return 0;
}

int WebServer::createThreadPool(int threadNum) {
    try {
        threadPool = new ThreadPool(threadNum);
    } catch (std::runtime_error&) {
        logStream("error") << "error" << std::endl;
    }

    if (threadPool == nullptr) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    return 0;
}

int WebServer::m_epollfd = -1;
bool WebServer::isStop = false;
int WebServer::eventHandlerPipe[2] = {-1, -1};

