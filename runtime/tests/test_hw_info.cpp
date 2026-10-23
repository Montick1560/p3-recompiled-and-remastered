// psp_hw_info: the one-line hardware summary the runtime logs at start (beta reports carry the game log).
#include "psp_hw_info.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void formats_every_field() {
    PspHwInfo h;
    h.cpu = "  AMD Ryzen 5 4500 6-Core Processor        ";
    h.threads = 12;
    h.ram_mb = 32651;
    h.os = "Windows 11 (build 26200)";
    h.gpu_vendor = "NVIDIA Corporation";
    h.gpu_renderer = "NVIDIA GeForce RTX 3050/PCIe/SSE2";
    h.gl_version = "3.3.0 NVIDIA 610.88";
    h.display_w = 1920;
    h.display_h = 1080;
    h.display_hz = 60;
    const std::string s = psp_hw_format(h);
    CHECK(s == "[RT] hardware: CPU AMD Ryzen 5 4500 6-Core Processor (12 threads), RAM 32651 MB, "
               "OS Windows 11 (build 26200), GPU NVIDIA Corporation NVIDIA GeForce RTX 3050/PCIe/SSE2, "
               "GL 3.3.0 NVIDIA 610.88, display 1920x1080 60 Hz",
          s.c_str());
}

static void unknown_fields_say_so() {
    PspHwInfo h;
    const std::string s = psp_hw_format(h);
    CHECK(s == "[RT] hardware: CPU ? (? threads), RAM ? MB, OS ?, GPU ?, GL ?, display ?", s.c_str());
}

static void one_line_always() {
    PspHwInfo h;
    h.cpu = "Intel\n(R) Core\r i3";
    h.gpu_renderer = "Mesa\nIntel";
    const std::string s = psp_hw_format(h);
    CHECK(s.find('\n') == std::string::npos && s.find('\r') == std::string::npos, s.c_str());
    CHECK(s.find("CPU Intel (R) Core i3") != std::string::npos, s.c_str());
}

static void windows_name_from_build() {
    CHECK(psp_hw_windows_name(10, 0, 26200) == "Windows 11 (build 26200)", "11 from the build number");
    CHECK(psp_hw_windows_name(10, 0, 19045) == "Windows 10 (build 19045)", "10");
    CHECK(psp_hw_windows_name(6, 1, 7601) == "Windows 6.1 (build 7601)", "older: version numbers");
}

int main() {
    formats_every_field();
    unknown_fields_say_so();
    one_line_always();
    windows_name_from_build();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hw_info: all tests passed\n");
    return 0;
}
