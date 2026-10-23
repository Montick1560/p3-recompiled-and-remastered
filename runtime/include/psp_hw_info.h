#pragma once
// One log line with the player's hardware, written once at start after the GL context exists
// ("[RT] hardware: CPU ..., RAM ..., OS ..., GPU ..., GL ..., display ..."). Problem reports of the online beta
// carry the game log, so each report says what it ran on. Nothing personal: model names and sizes only.
#include <string>

struct PspHwInfo {
    std::string cpu;           // e.g. "AMD Ryzen 5 4500 6-Core Processor"
    int threads = 0;           // logical processors
    int ram_mb = 0;
    std::string os;            // psp_hw_windows_name(...) on Windows
    std::string gpu_vendor;    // GL_VENDOR
    std::string gpu_renderer;  // GL_RENDERER
    std::string gl_version;    // GL_VERSION
    int display_w = 0, display_h = 0, display_hz = 0;  // desktop mode of the window's display
};

// The log line (no trailing newline). Unknown fields print "?"; control characters become spaces and runs of
// spaces collapse, so it is always one line.
std::string psp_hw_format(const PspHwInfo& h);

// "Windows 11 (build 26200)": Windows 11 still reports version 10.0, told apart by build >= 22000.
std::string psp_hw_windows_name(unsigned major, unsigned minor, unsigned build);

// CPU name and OS from the system (Windows: registry and RtlGetVersion; elsewhere empty). The SDL/GL fields are
// filled by the caller, which owns the GL context.
void psp_hw_fill_system(PspHwInfo* h);
