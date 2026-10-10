#pragma once

/// The guest CPU: the PSP runs one thread at a time and its games rely on it
/// (allocators, task lists and loaders without locks). Guest threads here are
/// host threads, so a guest thread must own this token while it runs guest
/// code. HLE calls run without it (they may block), and the scheduler hands it
/// to the waiting thread with the best PSP priority (lowest number).
/// PSPRECOMP_NO_CPU_LOCK=1 disables it (free-threaded, for A/B comparison).

/// The scheduler tells the lock how to find the calling thread's PSP
/// priority and id (host threads report 0 / -1 by default).
void psp_cpu_set_thread_info(int (*priority)(), int (*id)());

/// Take the CPU for this host thread (no-op if it already owns it).
/// `front`: among equal-priority waiters, go first (a thread coming back
/// from a non-blocking HLE call keeps running, like on the PSP).
void psp_cpu_acquire(bool front = false);

/// Give the CPU back (no-op if this host thread does not own it).
void psp_cpu_release();

/// Give the CPU back for a non-blocking HLE call: only threads of strictly
/// better priority may run meanwhile (PSP syscalls do not let worse threads
/// in) until this thread blocks (psp_cpu_block_begin) or 2 ms pass.
void psp_cpu_release_reserved();

/// An HLE call is about to block (wait, delay, sleep): drop this thread's
/// reservation so any ready thread may run, like a PSP thread going to WAIT.
void psp_cpu_block_begin();

/// True when this host thread owns the CPU.
bool psp_cpu_held();

/// At a guest loop back-edge: let a waiting thread of better or equal
/// priority run, then take the CPU back. Cheap when nobody waits.
void psp_cpu_yield_if_contended();

/// Releases the CPU for the scope if this thread owns it (HLE calls).
struct PspCpuReleaseScope {
    bool was_held;
    PspCpuReleaseScope() : was_held(psp_cpu_held()) { if (was_held) psp_cpu_release_reserved(); }
    ~PspCpuReleaseScope() { if (was_held) psp_cpu_acquire(/*front=*/true); }
    PspCpuReleaseScope(const PspCpuReleaseScope&) = delete;
    PspCpuReleaseScope& operator=(const PspCpuReleaseScope&) = delete;
};

/// Holds the CPU for the scope if this thread does not own it yet (guest
/// callbacks run from HLE code or host threads, thread entry, module_start).
struct PspCpuAcquireScope {
    bool took;
    PspCpuAcquireScope() : took(!psp_cpu_held()) { if (took) psp_cpu_acquire(); }
    ~PspCpuAcquireScope() { if (took) psp_cpu_release(); }
    PspCpuAcquireScope(const PspCpuAcquireScope&) = delete;
    PspCpuAcquireScope& operator=(const PspCpuAcquireScope&) = delete;
};
