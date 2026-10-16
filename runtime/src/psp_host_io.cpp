#include "psp_host_io.h"

#include <condition_variable>
#include <mutex>

namespace {
std::mutex g_m;
std::condition_variable g_cv;
int g_in_flight = 0;
}  // namespace

void psp_host_write_begin() {
    std::lock_guard<std::mutex> lock(g_m);
    g_in_flight++;
}

void psp_host_write_end() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_in_flight--;
    }
    g_cv.notify_all();
}

int psp_host_writes_in_flight() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_in_flight;
}

bool psp_host_writes_wait_idle(std::chrono::milliseconds limit) {
    std::unique_lock<std::mutex> lock(g_m);
    return g_cv.wait_for(lock, limit, [] { return g_in_flight == 0; });
}
