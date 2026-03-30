#include "utils.h"

#include <streambuf>

namespace {
class NullBuffer : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
};

NullBuffer g_nullBuffer;
std::ostream g_nullStream(&g_nullBuffer);
std::atomic<bool> g_infoLogEnabled(false);

std::atomic<uint64_t> g_statEpollWakeups(0);
std::atomic<uint64_t> g_statEpollEvents(0);
std::atomic<uint64_t> g_statEventDispatched(0);
std::atomic<uint64_t> g_statAcceptOk(0);
std::atomic<uint64_t> g_statRecvEvents(0);
std::atomic<uint64_t> g_statSendEvents(0);
std::atomic<uint64_t> g_statErrorLogs(0);
} // namespace

std::string outHead(const std::string& logType) {
    auto now = std::chrono::system_clock::now();
    time_t tt = std::chrono::system_clock::to_time_t(now);
    auto* timeTm = localtime(&tt);

    struct timeval timeUsec;
    gettimeofday(&timeUsec, nullptr);

    char strTime[64] = {0};
    sprintf(strTime,
            "%02d:%02d:%02d.%05ld %d-%02d-%02d",
            timeTm->tm_hour,
            timeTm->tm_min,
            timeTm->tm_sec,
            timeUsec.tv_usec,
            timeTm->tm_year + 1900,
            timeTm->tm_mon + 1,
            timeTm->tm_mday);

    std::string out = strTime;
    if (logType == "init") {
        out += " [init]: ";
    } else if (logType == "error") {
        out += " [erro]: ";
    } else if (logType == "stat") {
        out += " [stat]: ";
    } else {
        out += " [info]: ";
    }
    return out;
}

std::ostream& logStream(const std::string& logType) {
    if (logType == "error") {
        g_statErrorLogs.fetch_add(1, std::memory_order_relaxed);
        std::cout << outHead("error");
        return std::cout;
    }
    if (logType == "stat") {
        std::cout << outHead("stat");
        return std::cout;
    }
    if (!g_infoLogEnabled.load(std::memory_order_relaxed)) {
        return g_nullStream;
    }
    std::cout << outHead(logType);
    return std::cout;
}

void setInfoLogEnabled(bool enabled) {
    g_infoLogEnabled.store(enabled, std::memory_order_relaxed);
}

bool isInfoLogEnabled() {
    return g_infoLogEnabled.load(std::memory_order_relaxed);
}

void statIncEpollWakeups(uint64_t n) { g_statEpollWakeups.fetch_add(n, std::memory_order_relaxed); }
void statIncEpollEvents(uint64_t n) { g_statEpollEvents.fetch_add(n, std::memory_order_relaxed); }
void statIncEventDispatched(uint64_t n) { g_statEventDispatched.fetch_add(n, std::memory_order_relaxed); }
void statIncAcceptOk(uint64_t n) { g_statAcceptOk.fetch_add(n, std::memory_order_relaxed); }
void statIncRecvEvents(uint64_t n) { g_statRecvEvents.fetch_add(n, std::memory_order_relaxed); }
void statIncSendEvents(uint64_t n) { g_statSendEvents.fetch_add(n, std::memory_order_relaxed); }

RuntimeStatsSnapshot statConsumeSnapshot() {
    RuntimeStatsSnapshot s;
    s.epollWakeups = g_statEpollWakeups.exchange(0, std::memory_order_relaxed);
    s.epollEvents = g_statEpollEvents.exchange(0, std::memory_order_relaxed);
    s.eventDispatched = g_statEventDispatched.exchange(0, std::memory_order_relaxed);
    s.acceptOk = g_statAcceptOk.exchange(0, std::memory_order_relaxed);
    s.recvEvents = g_statRecvEvents.exchange(0, std::memory_order_relaxed);
    s.sendEvents = g_statSendEvents.exchange(0, std::memory_order_relaxed);
    s.errorLogs = g_statErrorLogs.exchange(0, std::memory_order_relaxed);
    return s;
}

int addWaitFd(int epollFd, int newFd, bool edgeTrigger, bool isOneshot) {
    epoll_event event;
    event.data.fd = newFd;
    event.events = EPOLLIN;
    if (edgeTrigger) event.events |= EPOLLET;
    if (isOneshot) event.events |= EPOLLONESHOT;

    int ret = epoll_ctl(epollFd, EPOLL_CTL_ADD, newFd, &event);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }
    return 0;
}

int modifyWaitFd(int epollFd, int modFd, bool edgeTrigger, bool resetOneshot, bool addEpollout) {
    epoll_event event;
    event.data.fd = modFd;
    event.events = EPOLLIN;

    if (edgeTrigger) event.events |= EPOLLET;
    if (resetOneshot) event.events |= EPOLLONESHOT;
    if (addEpollout) event.events |= EPOLLOUT;

    int ret = epoll_ctl(epollFd, EPOLL_CTL_MOD, modFd, &event);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }
    return 0;
}

int deleteWaitFd(int epollFd, int deleteFd) {
    int ret = epoll_ctl(epollFd, EPOLL_CTL_DEL, deleteFd, nullptr);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }
    return 0;
}

int setNonBlocking(int fd) {
    int oldFlag = fcntl(fd, F_GETFL);
    int ret = fcntl(fd, F_SETFL, oldFlag | O_NONBLOCK);
    if (ret != 0) return -1;
    return 0;
}
