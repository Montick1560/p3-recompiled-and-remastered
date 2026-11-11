// psp_module_of: names the module an address belongs to, for the [CRASH] line.
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void psp_module_of(uintptr_t addr, char* out, size_t size);
void psp_fault_frames(const void* context, char* out, size_t size);

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static std::string module_of(uintptr_t addr) {
    char buf[128];
    psp_module_of(addr, buf, sizeof buf);
    return buf;
}

#ifdef _WIN32
// A null call caught by a vectored handler: the frames are taken from the faulting context, then the handler
// returns to the caller as if the call had returned.
static char g_frames[1024];
static char g_tiny[4];
static LONG CALLBACK null_call_handler(EXCEPTION_POINTERS* info) {
    CONTEXT* c = info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || c->Rip != 0) return EXCEPTION_CONTINUE_SEARCH;
    psp_fault_frames(c, g_frames, sizeof g_frames);
    psp_fault_frames(c, g_tiny, sizeof g_tiny);
    c->Rip = *reinterpret_cast<DWORD64*>(c->Rsp);
    c->Rsp += 8;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline)) static int call_null() {
    void (*volatile f)() = nullptr;
    f();
    return 7;
}

static unsigned long long exe_offset(const std::string& frame) {
    return std::strtoull(frame.c_str() + 6, nullptr, 16);  // after "exe+0x"
}

static void fault_frames_tests() {
    void* h = AddVectoredExceptionHandler(1, null_call_handler);
    const int r = call_null();
    RemoveVectoredExceptionHandler(h);
    CHECK(r == 7, "the handler returned to the caller");
    std::printf("fault frames: %s\n", g_frames);
    std::vector<std::string> frames;
    for (const char* p = g_frames; *p;) {
        const char* end = std::strchr(p, ' ');
        frames.emplace_back(p, end ? end : p + std::strlen(p));
        p = end ? end + 1 : p + std::strlen(p);
    }
    CHECK(frames.size() >= 3, "fault_frames_find_the_caller_of_a_null_call: at least 3 frames");
    CHECK(!frames.empty() && frames[0] == "?", "the faulting address 0 is in no module");
    const unsigned long long fn = exe_offset(module_of(reinterpret_cast<uintptr_t>(&call_null)));
    CHECK(frames.size() > 1 && frames[1].rfind("exe+0x", 0) == 0 && exe_offset(frames[1]) > fn &&
              exe_offset(frames[1]) < fn + 0x100,
          "the second frame is inside the function that called address 0");
    CHECK(std::strlen(g_tiny) == 3, "fault_frames_short_buffer: cut and terminated");

    CONTEXT bad = {};
    bad.Rip = 0;
    bad.Rsp = 0x10;
    char out[64];
    psp_fault_frames(&bad, out, sizeof out);
    CHECK(std::string(out) == "?", "fault_frames_stop_at_a_bad_stack");
}
#endif

int main() {
    CHECK(module_of(1) == "?", "no module holds address 1");
    CHECK(module_of(0) == "?", "no module holds address 0");
#ifdef _WIN32
    const std::string self = module_of(reinterpret_cast<uintptr_t>(&module_of));
    CHECK(self.rfind("exe+0x", 0) == 0, "our own code is exe+0x...");
    const uintptr_t sleep_addr = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep"));
    std::string kernel = module_of(sleep_addr);
    for (char& c : kernel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    CHECK(kernel.rfind("kernel32.dll+0x", 0) == 0, "a system DLL by name, no path");
    char tiny[4];
    psp_module_of(sleep_addr, tiny, sizeof tiny);
    CHECK(std::strlen(tiny) == 3, "a short buffer is cut and stays terminated");
    fault_frames_tests();
#endif
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("backtrace: all passed\n");
    return 0;
}
