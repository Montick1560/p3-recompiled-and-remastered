#include "psp_handoff.h"

#ifdef _WIN32
#include <cstdlib>
#include <string>
#include <windows.h>

static HANDLE g_ready = nullptr;  // set while a handoff is under way

bool psp_handoff_begin() {
    const char* base = std::getenv("PSPRECOMP_HANDOFF");
    if (!base || !*base || g_ready) return false;
    const std::string prefix = std::string("Local\\") + base;
    HANDLE leaving = OpenEventA(EVENT_MODIFY_STATE, FALSE, (prefix + "-leaving").c_str());
    if (!leaving) return false;
    g_ready = OpenEventA(SYNCHRONIZE, FALSE, (prefix + "-ready").c_str());
    if (g_ready) {
        // This process is in front now, so it may hand the foreground over; the launcher's window could not take it otherwise.
        AllowSetForegroundWindow(ASFW_ANY);
        SetEvent(leaving);
    }
    CloseHandle(leaving);
    return g_ready != nullptr;
}

bool psp_handoff_finish(std::chrono::milliseconds limit) {
    if (!g_ready) return false;
    WaitForSingleObject(g_ready, static_cast<DWORD>(limit.count()));
    CloseHandle(g_ready);
    g_ready = nullptr;
    return true;
}
#else
bool psp_handoff_begin() { return false; }
bool psp_handoff_finish(std::chrono::milliseconds) { return false; }
#endif
