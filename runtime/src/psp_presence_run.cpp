#include "psp_presence_run.h"

#include "psp_discord.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>

namespace {

constexpr int64_t kReconnectMs = 30000;

PspPresenceSource g_source{nullptr, nullptr};

struct Run {
    const uint8_t* rdram;
    std::string lang;
    int64_t start_unix;
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
};
Run* g_run = nullptr;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void run_main(Run* r) {
    DiscordIpc ipc;
    nlohmann::json last;                 // null until something was sent in this connection
    int64_t last_send = 0, last_try = 0;
    bool tried = false;
    for (;;) {
        const int64_t t = now_ms();
        try {  // nothing may escape the thread (std::terminate): a throw counts as a failed send
            if (!ipc.connected() && (!tried || t - last_try >= kReconnectMs)) {
                tried = true;
                last_try = t;
                if (ipc.connect(g_source.client_id)) {
                    std::fprintf(stderr, "[DISCORD] connected\n");
                    last = nlohmann::json();  // a fresh Discord knows nothing: send the current state
                    last_send = t - kPresenceMinSendGapMs;
                }
            }
            if (ipc.connected() && !ipc.alive()) {  // Discord closed: notice now, not at the next change
                std::fprintf(stderr, "[DISCORD] lost, retrying in 30 s\n");
                last_try = t;
            }
            if (ipc.connected()) {
                const nlohmann::json now = g_source.activity(r->rdram, r->lang.c_str(), r->start_unix);
                if (!now.is_null() && psp_presence_should_send(last, now, t - last_send)) {
                    last_send = t;
                    last = now;  // also after a failure: the same activity is not resent until it changes
                    if (!ipc.set_activity(now)) {
                        std::fprintf(stderr, "[DISCORD] lost, retrying in 30 s\n");
                        last_try = t;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[DISCORD] error: %s; retrying in 30 s\n", e.what());
            ipc.close();
            last_try = t;
        }
        std::unique_lock<std::mutex> lk(r->m);
        if (r->cv.wait_for(lk, std::chrono::seconds(1), [r] { return r->stop; })) break;
    }
    try {
        if (ipc.connected()) ipc.set_activity(nullptr);  // the profile clears when the game quits
    } catch (...) {
    }
    ipc.close();
}

}  // namespace

void psp_presence_register(const PspPresenceSource& s) { g_source = s; }

bool psp_presence_should_send(const nlohmann::json& last, const nlohmann::json& now, int64_t ms_since_last) {
    return now != last && ms_since_last >= kPresenceMinSendGapMs;
}

bool psp_presence_start(const uint8_t* rdram, const std::string& lang) {
    const char* kill = std::getenv("PSPRECOMP_DISCORD");
    if (kill && std::string(kill) == "0") { std::fprintf(stderr, "[DISCORD] off (PSPRECOMP_DISCORD=0)\n"); return false; }
    if (!g_source.client_id || !g_source.client_id[0] || !g_source.activity) {
        std::fprintf(stderr, "[DISCORD] off: no client id registered\n");
        return false;
    }
    psp_presence_stop();
    Run* r = new Run;
    r->rdram = rdram;
    r->lang = lang == "en" ? "en" : "es";
    r->start_unix = static_cast<int64_t>(std::time(nullptr));
    r->thread = std::thread(run_main, r);
    g_run = r;
    std::fprintf(stderr, "[DISCORD] on (looking for Discord every 30 s)\n");
    return true;
}

void psp_presence_stop() {
    Run* r = g_run;
    g_run = nullptr;
    if (!r) return;
    { std::lock_guard<std::mutex> lk(r->m); r->stop = true; }
    r->cv.notify_all();
    r->thread.join();
    delete r;
}
