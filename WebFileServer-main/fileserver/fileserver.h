#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <errno.h>
#include <signal.h>
#include <stdexcept>
#include <sys/types.h>

#include "../threadpool/threadpool.h"

#define MAX_RESEVENT_SIZE 1024

class WebServer {
public:
    WebServer();

    int createListenFd(int port, const char* ip = nullptr);
    int createEpoll();
    int epollAddListenFd();
    int epollAddEventPipe();
    int addHandleSig(int signo = -1);
    static void setSigHandler(int signo);
    int waitEpoll();
    int createThreadPool(int threadNum = 8);

    ~WebServer();

private:
    int m_listenfd;
    sockaddr_in m_serverAddr;
    static int m_epollfd;
    static bool isStop;
    static int eventHandlerPipe[2];

    epoll_event resEvents[MAX_RESEVENT_SIZE];
    ThreadPool* threadPool;
};

#endif
