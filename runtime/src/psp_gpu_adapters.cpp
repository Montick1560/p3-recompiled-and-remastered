// psp_gpu_adapters: the GPUs as DXGI lists them (psp_hw_info.h). Needs no OpenGL, so it runs before the context.
#include "psp_hw_info.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

static std::string utf8(const wchar_t* w) {
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf - 1, nullptr, nullptr);
    return buf;
}

// Highest D3D11 feature level; "" when no device can be made. A D3D11.0 runtime rejects 11_1 with E_INVALIDARG.
static std::string feature_level(IDXGIAdapter1* adapter) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                               D3D_FEATURE_LEVEL_9_1};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 7, D3D11_SDK_VERSION,
                                   nullptr, &got, nullptr);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 6, D3D11_SDK_VERSION,
                               nullptr, &got, nullptr);
    if (FAILED(hr)) return "";
    return std::to_string((got >> 12) & 0xF) + "_" + std::to_string((got >> 8) & 0xF);
}

std::vector<PspGpuAdapter> psp_gpu_adapters() {
    std::vector<PspGpuAdapter> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return out;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            PspGpuAdapter a;
            a.name = utf8(d.Description);
            a.vram_mb = static_cast<int>(d.DedicatedVideoMemory >> 20);
            a.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                a.driver = std::to_string(HIWORD(umd.HighPart)) + "." + std::to_string(LOWORD(umd.HighPart)) + "." +
                           std::to_string(HIWORD(umd.LowPart)) + "." + std::to_string(LOWORD(umd.LowPart));
            a.d3d = feature_level(adapter);
            out.push_back(a);
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}
#else
std::vector<PspGpuAdapter> psp_gpu_adapters() { return {}; }
#endif
