#include "psp_subintr.h"

// Semantics and error codes follow PPSSPP Core/HLE/sceKernelInterrupt.cpp
// (sceKernelRegisterSubIntrHandler / Release / Enable / Disable).

PspSubIntrTable::Slot* PspSubIntrTable::slot(int intr, int sub) {
    if (intr < 0 || intr >= PSP_NUM_INTERRUPTS) return nullptr;
    if (sub < 0 || sub >= PSP_NUM_SUBINTERRUPTS) return nullptr;
    return &slots_[intr][sub];
}

int PspSubIntrTable::register_handler(int intr, int sub, uint32_t handler,
                                      uint32_t arg) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* s = slot(intr, sub);
    if (!s) return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE);
    if (s->registered) return static_cast<int>(SCE_KERNEL_ERROR_FOUND_HANDLER);
    *s = Slot{true, false, handler, arg};
    return 0;
}

int PspSubIntrTable::release(int intr, int sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* s = slot(intr, sub);
    if (!s) return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE);
    if (!s->registered) return static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER);
    *s = Slot{};
    return 0;
}

int PspSubIntrTable::enable(int intr, int sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* s = slot(intr, sub);
    if (!s) return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE);
    if (!s->registered) return static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER);
    s->enabled = true;
    return 0;
}

int PspSubIntrTable::disable(int intr, int sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* s = slot(intr, sub);
    if (!s) return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE);
    if (!s->registered) return static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER);
    s->enabled = false;
    return 0;
}

std::vector<PspSubIntrHandler> PspSubIntrTable::enabled(int intr) const {
    std::vector<PspSubIntrHandler> out;
    if (intr < 0 || intr >= PSP_NUM_INTERRUPTS) return out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int sub = 0; sub < PSP_NUM_SUBINTERRUPTS; sub++) {
        const Slot& s = slots_[intr][sub];
        if (s.registered && s.enabled) out.push_back({sub, s.handler, s.arg});
    }
    return out;
}
