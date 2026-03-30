#include "./fileserver/fileserver.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

static int resolveThreadPoolSize() {
    const char* fixed = std::getenv("THREAD_POOL_SIZE");
    if (fixed != nullptr) {
        int v = std::atoi(fixed);
        if (v > 0) {
            return v;
        }
    }

    unsigned int cpu = std::thread::hardware_concurrency();
    int base = (cpu == 0U) ? 4 : static_cast<int>(cpu);

    int factor = 1;
    const char* mode = std::getenv("THREAD_POOL_MODE");
    if (mode != nullptr) {
        if (std::strcmp(mode, "2x") == 0 || std::strcmp(mode, "double") == 0) {
            factor = 2;
        }
    }

    return std::max(1, base * factor);
}

int main() {
    WebServer webserver;

    int ret = webserver.createThreadPool(resolveThreadPoolSize());
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -1;
    }

    int port = 8888;
    ret = webserver.createListenFd(port);
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -2;
    }

    ret = webserver.createEpoll();
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -3;
    }

    ret = webserver.epollAddListenFd();
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -4;
    }

    ret = webserver.waitEpoll();
    if (ret != 0) {
        logStream("error") << "error" << std::endl;
        return -5;
    }

    return 0;
}
