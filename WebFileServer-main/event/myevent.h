#ifndef MYEVENT_H
#define MYEVENT_H

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <vector>

#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../message/message.h"
#include "../utils/utils.h"

class EventBase {
public:
    EventBase() {}
    virtual ~EventBase() {}
    virtual void process() = 0;

protected:
    static std::unordered_map<int, Request> requestStatus;
    static std::unordered_map<int, Response> responseStatus;
    static std::mutex requestMutex;
    static std::mutex responseMutex;
};

class AcceptConn : public EventBase {
public:
    AcceptConn(int listenFd, int epollFd) : m_listenFd(listenFd), m_epollFd(epollFd) {}
    ~AcceptConn() override {}
    void process() override;

private:
    int m_listenFd;
    int m_epollFd;
    int accetpFd;
    sockaddr_in clientAddr;
    socklen_t clientAddrLen;
};

class HandleSig : public EventBase {
public:
    explicit HandleSig(int epollFd) : EventBase(), m_epollFd(epollFd) {}
    ~HandleSig() override {}
    void process() override {}

private:
    int m_epollFd;
};

class HandleRecv : public EventBase {
public:
    HandleRecv(int clientFd, int epollFd) : m_clientFd(clientFd), m_epollFd(epollFd) {}
    ~HandleRecv() override {}
    void process() override;

private:
    int m_clientFd;
    int m_epollFd;
};

class HandleSend : public EventBase {
public:
    HandleSend(int clientFd, int epollFd) : m_clientFd(clientFd), m_epollFd(epollFd) {}
    ~HandleSend() override {}

    void process() override;

    std::string getStatusLine(const std::string& httpVersion,
                              const std::string& statusCode,
                              const std::string& statusDes);
    void getFileListPage(std::string& fileListHtml);
    void getFileVec(const std::string dirName, std::vector<std::string>& resVec);
    std::string getMessageHeader(const std::string contentLength,
                                 const std::string contentType,
                                 const std::string redirectLoction = "",
                                 const std::string contentRange = "");

private:
    int m_clientFd;
    int m_epollFd;
};

#endif
