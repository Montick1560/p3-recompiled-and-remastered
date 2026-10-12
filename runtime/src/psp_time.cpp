#include "psp_time.h"

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int64_t psp_audio_pace_us(int64_t samples) {
    return samples * 1000000LL / 44100;
}

std::chrono::steady_clock::time_point AudioPaceClock::schedule(
    std::chrono::steady_clock::time_point now, int64_t samples
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto grain = std::chrono::microseconds(psp_audio_pace_us(samples));
    // Idle gap or a producer that fell a full grain behind: resync, don't
    // accumulate unbounded debt.
    if (next_ == std::chrono::steady_clock::time_point{} || now - next_ > grain) {
        next_ = now;
    }
    next_ += grain;  // the grain plays while the caller works on the next one
    return next_;
}

void psp_sleep_until(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+) lands within
    // ~0.5 ms without touching the global system timer, unlike Sleep(), which
    // quantizes to ~15.6 ms. One timer per call: audio grains come from several
    // guest threads and a shared timer would let one caller's SetWaitableTimer
    // clobber another's wait.
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        for (;;) {
            const auto remain = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remain <= 0) break;
            LARGE_INTEGER due;
            due.QuadPart = -(remain / 100);  // relative, 100 ns units
            if (due.QuadPart == 0) due.QuadPart = -1;
            if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;
            WaitForSingleObject(timer, INFINITE);
        }
        CloseHandle(timer);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return;
    }
#endif
    // No high-resolution timer (older Windows) or non-Windows: sleep while far,
    // yield-spin the tail so the call never returns early.
    for (;;) {
        const auto remain = deadline - std::chrono::steady_clock::now();
        if (remain <= std::chrono::microseconds(0)) return;
        if (remain > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(remain - std::chrono::milliseconds(1));
        } else {
            std::this_thread::yield();
        }
    }
}
