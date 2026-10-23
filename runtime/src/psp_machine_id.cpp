#include "psp_machine_id.h"

#ifdef _WIN32
#include <windows.h>

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000  // missing from older MinGW headers
#endif

static std::string utf8_of(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::string psp_machine_id() {
    wchar_t guid[128];
    DWORD size = sizeof(guid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\Microsoft\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &size) == ERROR_SUCCESS) {
        std::string id = utf8_of(guid);
        if (!id.empty()) return id;
    }
    wchar_t name[256];
    DWORD len = sizeof(name) / sizeof(name[0]);
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &len)) return utf8_of(name);
    return {};
}
#else
#include <fstream>
#include <unistd.h>

static std::string first_line(const char* path) {
    std::ifstream in(path);
    std::string line;
    if (in) std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

std::string psp_machine_id() {
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::string id = first_line(path);
        if (!id.empty()) return id;
    }
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) return host;
    return {};
}
#endif
