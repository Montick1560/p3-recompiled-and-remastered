// psp_host_io.h — host file writes in flight (savedata, memory-stick files, data install,
// ad hoc config). Shutdown waits for them before it ends the process: a save written halfway
// is worse than a slower exit.
#pragma once

#include <chrono>

void psp_host_write_begin();
void psp_host_write_end();
int psp_host_writes_in_flight();
bool psp_host_writes_wait_idle(std::chrono::milliseconds limit);

struct PspHostWriteScope {
    PspHostWriteScope() { psp_host_write_begin(); }
    ~PspHostWriteScope() { psp_host_write_end(); }
    PspHostWriteScope(const PspHostWriteScope&) = delete;
    PspHostWriteScope& operator=(const PspHostWriteScope&) = delete;
};
