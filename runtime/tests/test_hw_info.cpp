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

static void gpu_two_adapters() {
    const std::string s = psp_gpu_format({{"NVIDIA GeForce RTX 3050 Laptop GPU", "32.0.15.6094", 4096, "11_1", false},
                                          {"Intel(R) UHD Graphics", "31.0.101.4502", 128, "11_1", false}});
    CHECK(s == "[RT] gpu adapters: NVIDIA GeForce RTX 3050 Laptop GPU (driver 32.0.15.6094, 4096 MB, D3D 11_1); "
               "Intel(R) UHD Graphics (driver 31.0.101.4502, 128 MB, D3D 11_1)",
          s.c_str());
}

static void gpu_unknowns_and_software() {
    const std::string s = psp_gpu_format({{"Microsoft Basic Render Driver", "", 0, "", true}});
    CHECK(s == "[RT] gpu adapters: Microsoft Basic Render Driver (software, driver ?, ? MB, D3D ?)", s.c_str());
}

static void gpu_none() {
    const std::string s = psp_gpu_format({});
    CHECK(s == "[RT] gpu adapters: ?", s.c_str());
}

static void gpu_one_line() {
    const std::string s = psp_gpu_format({{"Radeon\n(TM)\r Graphics", "1.2.3.4", 512, "10_0", false}});
    CHECK(s == "[RT] gpu adapters: Radeon (TM) Graphics (driver 1.2.3.4, 512 MB, D3D 10_0)", s.c_str());
}

// The PC class sent with frame_pace: the launcher's rule (gpu.rs scale_for): a hardware adapter with 2 GB of its
// own memory is dedicated; other hardware is integrated; only software adapters -> software; none -> unknown.
static void gpu_class_of_pc() {
    CHECK(psp_gpu_class({}) == 0, "no adapters -> unknown");
    CHECK(psp_gpu_class({{"Microsoft Basic Render Driver", "", 0, "", true}}) == 3, "software only");
    CHECK(psp_gpu_class({{"Intel HD 4000", "", 128, "11_0", false}, {"Microsoft Basic Render Driver", "", 0, "", true}})
              == 1, "integrated beside the software adapter");
    CHECK(psp_gpu_class({{"Intel UHD", "", 128, "12_1", false}, {"GeForce MX130", "", 2048, "12_1", false}}) == 2,
          "a 2 GB card makes it dedicated");
    CHECK(psp_gpu_class({{"Radeon Vega", "", 1024, "12_1", false}}) == 1, "1 GB of carve-out is integrated");
}

int main() {
    formats_every_field();
    unknown_fields_say_so();
    one_line_always();
    windows_name_from_build();
    gpu_two_adapters();
    gpu_unknowns_and_software();
    gpu_none();
    gpu_one_line();
    gpu_class_of_pc();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hw_info: all tests passed\n");
    return 0;
}
