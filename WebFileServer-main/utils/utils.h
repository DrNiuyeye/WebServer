#ifndef UTILS_H
#define UTILS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <string>

#include <sys/epoll.h>
#include <sys/fcntl.h>
#include <sys/time.h>

std::string outHead(const std::string& logType);

// Logging control: info/log are muted by default, error/stat always print.
std::ostream& logStream(const std::string& logType);
void setInfoLogEnabled(bool enabled);
bool isInfoLogEnabled();

struct RuntimeStatsSnapshot {
    uint64_t epollWakeups;
    uint64_t epollEvents;
    uint64_t eventDispatched;
    uint64_t acceptOk;
    uint64_t recvEvents;
    uint64_t sendEvents;
    uint64_t errorLogs;
};

void statIncEpollWakeups(uint64_t n = 1);
void statIncEpollEvents(uint64_t n = 1);
void statIncEventDispatched(uint64_t n = 1);
void statIncAcceptOk(uint64_t n = 1);
void statIncRecvEvents(uint64_t n = 1);
void statIncSendEvents(uint64_t n = 1);
RuntimeStatsSnapshot statConsumeSnapshot();

int addWaitFd(int epollFd, int newFd, bool edgeTrigger = false, bool isOneshot = false);
int modifyWaitFd(int epollFd, int modFd, bool edgeTrigger = false, bool resetOneshot = false, bool addEpollout = false);
int deleteWaitFd(int epollFd, int deleteFd);
int setNonBlocking(int fd);

#endif
