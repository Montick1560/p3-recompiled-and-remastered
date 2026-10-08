#pragma once
// PSP sub-interrupt handler table (sceKernelRegisterSubIntrHandler & co).
// Pure bookkeeping: which guest handler is registered/enabled per
// (interrupt, sub-interrupt). The HLE layer dispatches the enabled
// handlers, e.g. the vblank ones once per display frame.

#include <cstdint>
#include <mutex>
#include <vector>

constexpr int PSP_NUM_INTERRUPTS = 67;
constexpr int PSP_NUM_SUBINTERRUPTS = 32;
constexpr int PSP_INTR_VBLANK = 30;  // PSP_VBLANK_INT (0x1E)

constexpr uint32_t SCE_KERNEL_ERROR_ILLEGAL_INTRCODE = 0x80020065u;
constexpr uint32_t SCE_KERNEL_ERROR_FOUND_HANDLER = 0x80020067u;
constexpr uint32_t SCE_KERNEL_ERROR_NOTFOUND_HANDLER = 0x80020068u;

struct PspSubIntrHandler {
    int sub;
    uint32_t handler;  // guest function: handler(sub, arg)
    uint32_t arg;
};

class PspSubIntrTable {
public:
    /// Returns 0 or a SCE_KERNEL_ERROR_* code (as int).
    int register_handler(int intr, int sub, uint32_t handler, uint32_t arg);
    int release(int intr, int sub);
    int enable(int intr, int sub);
    int disable(int intr, int sub);

    /// Enabled handlers of `intr`, lowest sub-interrupt first.
    std::vector<PspSubIntrHandler> enabled(int intr) const;

private:
    struct Slot {
        bool registered = false;
        bool enabled = false;
        uint32_t handler = 0;
        uint32_t arg = 0;
    };
    Slot* slot(int intr, int sub);

    mutable std::mutex mutex_;
    Slot slots_[PSP_NUM_INTERRUPTS][PSP_NUM_SUBINTERRUPTS];
};
