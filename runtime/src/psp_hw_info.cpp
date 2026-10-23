#include "psp_hw_info.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

std::string clean(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "?" : out;
}

std::string num(int v) { return v > 0 ? std::to_string(v) : "?"; }

}  // namespace

std::string psp_hw_format(const PspHwInfo& h) {
    std::string gpu = clean(h.gpu_vendor + " " + h.gpu_renderer);
    std::string display = "?";
    if (h.display_w > 0 && h.display_h > 0) {
        display = std::to_string(h.display_w) + "x" + std::to_string(h.display_h);
        if (h.display_hz > 0) display += " " + std::to_string(h.display_hz) + " Hz";
    }
    return "[RT] hardware: CPU " + clean(h.cpu) + " (" + num(h.threads) + " threads), RAM " + num(h.ram_mb) +
           " MB, OS " + clean(h.os) + ", GPU " + gpu + ", GL " + clean(h.gl_version) + ", display " + display;
}

std::string psp_hw_windows_name(unsigned major, unsigned minor, unsigned build) {
    char buf[64];
    if (major == 10 && minor == 0)
        std::snprintf(buf, sizeof buf, "Windows %d (build %u)", build >= 22000 ? 11 : 10, build);
    else
        std::snprintf(buf, sizeof buf, "Windows %u.%u (build %u)", major, minor, build);
    return buf;
}

void psp_hw_fill_system(PspHwInfo* h) {
#ifdef _WIN32
    char name[256] = {};
    DWORD size = sizeof name;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
        h->cpu = name;
    // GetVersionEx lies to unmanifested programs; RtlGetVersion does not.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        OSVERSIONINFOW v = {};
        v.dwOSVersionInfoSize = sizeof v;
        if (fn && fn(&v) == 0) h->os = psp_hw_windows_name(v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
#else
    (void)h;
#endif
}
