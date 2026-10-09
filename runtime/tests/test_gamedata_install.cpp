// Unit tests for the sceUtilityGamedataInstall back end
// (psp_gamedata_install.cpp): destination naming, the chunked copy-progress
// state machine over a temp directory with fake INSDIR files, the PARAM.SFO
// contents, and the Memory Stick devctl descriptor. Standalone: host
// filesystem temp directories only; no SDL, no guest RAM, no game files.

#include "psp_gamedata_install.h"

#include "hle/psp_savedata.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace gdi = psp_gamedata_install;
namespace fs = std::filesystem;

static int failures = 0;
static int tests_run = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        long long a_ = static_cast<long long>(actual); \
        long long e_ = static_cast<long long>(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, "FAIL: %s: got 0x%llX, expected 0x%llX " \
                "(line %d)\n", msg, static_cast<unsigned long long>(a_), \
                static_cast<unsigned long long>(e_), __LINE__); \
            failures++; \
        } \
    } while (0)

static fs::path g_tmp;

static void write_bytes(const fs::path& p, size_t n, uint8_t seed) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < n; i++) {
        char c = static_cast<char>(seed + i * 7);
        f.write(&c, 1);
    }
}

static std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

static void test_destination_naming() {
    // PPSSPP GetGameDataInstallFileName: saveBasePath + gameName + dataName
    // + "/" + filename (no separator between the two names).
    ASSERT_TRUE(gdi::install_dir_name("ULJM05999", "DATA") == "ULJM05999DATA",
                "dir name concatenates game+data");
    std::string dir = gdi::install_dir("/ms/SAVEDATA", "ULJM05999", "DATA");
    ASSERT_TRUE(dir == (fs::path("/ms/SAVEDATA") / "ULJM05999DATA").string(),
                "dir joins the savedata root");
    std::string fp = gdi::install_file_path("/ms/SAVEDATA", "ULJM05999",
                                            "DATA", "DATAMS.BND");
    ASSERT_TRUE(fp ==
                    (fs::path("/ms/SAVEDATA") / "ULJM05999DATA" / "DATAMS.BND")
                        .string(),
                "file path joins dir + filename");
}

static void test_param_layout() {
    // Guest SceUtilityGamedataInstallParam offsets (PPSSPP
    // PSPGamedataInstallDialog.h): common(48) + mode(4) + gameName[13] +
    // ignore1[3] + dataName[20] = sfoParam at 88; title[128] +
    // savedataTitle[128] + detail[1024] + parentalLevel ends at 1372.
    ASSERT_EQ(gdi::off::kResult, 28u, "common.result offset");
    ASSERT_EQ(gdi::off::kMode, 48u, "mode offset");
    ASSERT_EQ(gdi::off::kGameName, 52u, "gameName offset");
    ASSERT_EQ(gdi::off::kDataName, 68u, "dataName offset");
    ASSERT_EQ(gdi::off::kSfoTitle, 88u, "sfo title offset");
    ASSERT_EQ(gdi::off::kSfoSavedataTitle, 216u, "sfo savedataTitle offset");
    ASSERT_EQ(gdi::off::kSfoDetail, 344u, "sfo detail offset");
    ASSERT_EQ(gdi::off::kSfoParentalLevel, 1368u, "parentalLevel offset");
    ASSERT_EQ(gdi::off::kProgress, 1372u, "progress offset");
    ASSERT_EQ(gdi::off::kUnknownResult1, 1376u, "unknownResult1 offset");
    ASSERT_EQ(gdi::off::kUnknownResult2, 1380u, "unknownResult2 offset");
    ASSERT_EQ(gdi::off::kSizeV2, 1432u, "current struct size");
}

static void test_ms_device_size() {
    // PPSSPP sceIo.cpp 0x02425818 on a virtual 4 GB card: sector 512 B,
    // 64 sectors per cluster, 95% usable, everything free.
    gdi::DeviceSize d = gdi::ms_device_size();
    ASSERT_EQ(d.sector_size, 0x200u, "sector size");
    ASSERT_EQ(d.sector_count, 64u, "sectors per cluster");
    uint64_t per_cluster = uint64_t{0x200} * 64;
    uint32_t want =
        static_cast<uint32_t>((gdi::kVirtualCardBytes * 95 / 100) /
                              per_cluster);
    ASSERT_EQ(d.max_clusters, want, "max clusters");
    ASSERT_EQ(d.free_clusters, want, "free clusters");
    ASSERT_EQ(d.max_sectors, want, "max sectors");
    ASSERT_TRUE(want > 100000, "plenty of clusters");
    uint64_t free_bytes = uint64_t{d.free_clusters} * per_cluster;
    ASSERT_TRUE(free_bytes >= 1024ull * 1024 * 1024, ">= 1 GB free");
    ASSERT_TRUE(gdi::kVirtualCardBytes == 4ull * 1024 * 1024 * 1024,
                "4 GB virtual card");
}

static void test_empty_source() {
    fs::path src = g_tmp / "empty_insdir";
    fs::create_directories(src);
    gdi::SfoParams sfo;
    gdi::Installer inst(src.string(), (g_tmp / "dst").string(), sfo);
    ASSERT_TRUE(!inst.ok(), "no files -> not ok");
    ASSERT_TRUE(inst.file_count() == 0, "no files counted");
}

static void test_missing_source() {
    gdi::SfoParams sfo;
    gdi::Installer inst((g_tmp / "nope").string(),
                        (g_tmp / "dst2").string(), sfo);
    ASSERT_TRUE(!inst.ok(), "missing dir -> not ok");
}

static void test_copy_progress_and_sfo() {
    fs::path src = g_tmp / "insdir";
    fs::path dst = g_tmp / "SAVEDATA" / "ULJM05999DATA";
    fs::create_directories(src);
    write_bytes(src / "a.bin", 10, 0x11);
    write_bytes(src / "b.bin", 100 * 1024, 0x22);
    // > 640 KiB (one Update quantum: 20 x 32 KiB) so the copy must span
    // several steps.
    write_bytes(src / "c.bin", 2 * 1024 * 1024, 0x33);

    gdi::SfoParams sfo;
    sfo.title = "Patapon 3 Install";
    sfo.savedata_title = "Install Data";
    sfo.detail = "Game data installed from UMD.";
    sfo.parental_level = 3;
    gdi::Installer inst(src.string(), dst.string(), sfo);
    ASSERT_TRUE(inst.ok(), "fake INSDIR is ok");
    ASSERT_EQ(inst.file_count(), 3, "three files found");
    ASSERT_EQ(inst.progress(), 0, "progress starts at 0");
    ASSERT_TRUE(!inst.finished(), "not finished yet");

    int last = 0;
    int steps = 0;
    while (!inst.finished() && steps < 10000) {
        inst.step();
        int p = inst.progress();
        ASSERT_TRUE(p >= last, "progress never goes backwards");
        last = p;
        steps++;
    }
    ASSERT_TRUE(inst.finished(), "copy finishes");
    ASSERT_EQ(inst.progress(), 100, "progress ends at 100");
    ASSERT_EQ(inst.files_done(), 3, "all files done");
    ASSERT_TRUE(steps > 3, "big file spans several steps");
    ASSERT_EQ(inst.copied_bytes(), inst.total_bytes(), "all bytes copied");
    ASSERT_EQ(inst.total_bytes(), 10u + 100u * 1024 + 2u * 1024 * 1024,
              "total size");

    for (const char* name : {"a.bin", "b.bin", "c.bin"}) {
        ASSERT_TRUE(read_all(src / name) == read_all(dst / name),
                    "installed file is byte-identical");
    }

    // PARAM.SFO merged like PPSSPP WriteSfoFile.
    fs::path sfo_path = dst / "PARAM.SFO";
    ASSERT_TRUE(fs::is_regular_file(sfo_path), "PARAM.SFO written");
    std::vector<uint8_t> raw = read_all(sfo_path);
    psp_savedata::Psf parsed;
    ASSERT_TRUE(psp_savedata::Psf::parse(raw.data(), raw.size(), &parsed),
                "PARAM.SFO parses");
    ASSERT_TRUE(parsed.get_string("TITLE") == sfo.title, "SFO TITLE");
    ASSERT_TRUE(parsed.get_string("SAVEDATA_TITLE") == sfo.savedata_title,
                "SFO SAVEDATA_TITLE");
    ASSERT_TRUE(parsed.get_string("SAVEDATA_DETAIL") == sfo.detail,
                "SFO SAVEDATA_DETAIL");
    ASSERT_EQ(parsed.get_int("PARENTAL_LEVEL"), 3, "SFO PARENTAL_LEVEL");
    ASSERT_TRUE(parsed.get_string("CATEGORY") == "MS", "SFO CATEGORY");
    ASSERT_TRUE(parsed.get_string("SAVEDATA_DIRECTORY") == "ULJM05999DATA",
                "SFO SAVEDATA_DIRECTORY");
}

int main() {
    g_tmp = fs::temp_directory_path() /
            ("psp_gamedata_test_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    fs::remove_all(g_tmp);  // our own fresh temp dir name; nothing else
    fs::create_directories(g_tmp);

    test_destination_naming();
    test_param_layout();
    test_ms_device_size();
    test_empty_source();
    test_missing_source();
    test_copy_progress_and_sfo();

    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
