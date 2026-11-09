// psp_module_of: names the module an address belongs to, for the [CRASH] line.
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void psp_module_of(uintptr_t addr, char* out, size_t size);

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static std::string module_of(uintptr_t addr) {
    char buf[128];
    psp_module_of(addr, buf, sizeof buf);
    return buf;
}

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
#endif
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("backtrace: all passed\n");
    return 0;
}
