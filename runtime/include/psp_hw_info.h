#pragma once
// One log line with the player's hardware, written once at start after the GL context exists
// ("[RT] hardware: CPU ..., RAM ..., OS ..., GPU ..., GL ..., display ..."). Problem reports of the online beta
// carry the game log, so each report says what it ran on. Nothing personal: model names and sizes only.
#include <string>
#include <vector>

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

// One GPU as DXGI lists it. Printed as "[RT] gpu adapters: ..." right after SDL_Init, before any GL call, so a
// PC whose OpenGL context fails still says which GPU, driver and Direct3D level it has (ANGLE needs D3D 10_0+).
struct PspGpuAdapter {
    std::string name;       // DXGI_ADAPTER_DESC1.Description
    std::string driver;     // user-mode driver version "32.0.15.6094", "" unknown
    int vram_mb = 0;        // DedicatedVideoMemory
    std::string d3d;        // highest D3D11 feature level "11_1", "10_0", "" unknown
    bool software = false;  // DXGI_ADAPTER_FLAG_SOFTWARE (Microsoft Basic Render Driver)
};

// "[RT] gpu adapters: <name> (driver <v>, <n> MB, D3D <level>); ..." ("software, " first for a software adapter);
// unknown fields print "?", no adapters prints "?". Always one line.
std::string psp_gpu_format(const std::vector<PspGpuAdapter>& adapters);

// Every adapter from DXGI (Windows); empty elsewhere or when DXGI fails. psp_gpu_adapters.cpp.
std::vector<PspGpuAdapter> psp_gpu_adapters();
