// Unit tests for the sceUtilitySavedata back end (psp_savedata.cpp): PSF
// writer/parser, AUTOSAVE/AUTOLOAD round trip, SIZES, FILES, LIST, the list
// modes, READ/WRITEDATA, GETSIZE and bad guest pointers. Standalone: a fake
// guest RAM and a throw-away temp directory; no SDL, no game files.

#include "hle/psp_savedata.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace psp_savedata;
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

// ---- fake guest RAM -------------------------------------------------------
// Small (4MB) mapping: guest 0x08000000 is offset 0, so 0x0A000000 (offset
// 0x02000000) is outside it and exercises the bounds checks.
static constexpr size_t kRam = 0x400000;
static constexpr uint32_t kBase = 0x08000000u;

static constexpr uint32_t P = kBase + 0x1000;      // SceUtilitySavedataParam
static constexpr uint32_t SRC = kBase + 0x10000;   // data to save
static constexpr uint32_t DST = kBase + 0x20000;   // data loaded
static constexpr uint32_t AUX = kBase + 0x30000;   // misc structs
static constexpr uint32_t NAMES = kBase + 0x31000; // saveNameList
static constexpr uint32_t ICON = kBase + 0x32000;  // icon payload
static constexpr uint32_t ENT_S = kBase + 0x40000; // file list: secure
static constexpr uint32_t ENT_N = kBase + 0x41000; // file list: normal
static constexpr uint32_t ENT_Y = kBase + 0x42000; // file list: system

struct Ram {
    std::vector<uint8_t> v;
    Ram() : v(kRam, 0) {}
    uint8_t* at(uint32_t a) { return v.data() + (a & 0x07FFFFFFu); }
    uint32_t u32(uint32_t a) {
        uint32_t x;
        std::memcpy(&x, at(a), 4);
        return x;
    }
    uint64_t u64(uint32_t a) {
        uint64_t x;
        std::memcpy(&x, at(a), 8);
        return x;
    }
    void w32(uint32_t a, uint32_t x) { std::memcpy(at(a), &x, 4); }
    void wstr(uint32_t a, const std::string& s) {
        std::memcpy(at(a), s.c_str(), s.size() + 1);
    }
    std::string str(uint32_t a, size_t cap) {
        const char* p = reinterpret_cast<const char*>(at(a));
        return std::string(p, strnlen(p, cap));
    }
};

static fs::path g_root;
static Options g_opts;

static const char* kGame = "ULJM05999";

static void make_param(Ram& r, uint32_t mode, const char* save = "0000",
                       const char* file = "DATA.BIN") {
    std::memset(r.at(P), 0, off::kParamSize);
    r.w32(P + off::kSize, off::kParamSize);
    r.w32(P + off::kMode, mode);
    r.wstr(P + off::kGameName, kGame);
    r.wstr(P + off::kSaveName, save);
    r.wstr(P + off::kFileName, file);
    r.at(P + off::kKey)[0] = 0x42;  // non-zero key: secure version 0 needs one
}

static void set_data(Ram& r, uint32_t addr, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        r.at(addr)[i] = static_cast<uint8_t>(seed + i * 7);
    }
}

static void set_buf(Ram& r, uint32_t buf, uint32_t buf_size, uint32_t size) {
    r.w32(P + off::kDataBuf, buf);
    r.w32(P + off::kDataBufSize, buf_size);
    r.w32(P + off::kDataSize, size);
}

static Outcome run(Ram& r) { return execute(r.v.data(), r.v.size(), P, g_opts); }

static uint32_t res(const Outcome& o) { return static_cast<uint32_t>(o.result); }

static fs::path save_dir(const char* save) {
    return g_root / (std::string(kGame) + save);
}

static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

static void fill_sfo(Ram& r, const char* title, const char* stitle,
                     const char* detail, uint8_t level) {
    r.wstr(P + off::kSfoTitle, title);
    r.wstr(P + off::kSfoSavedataTitle, stitle);
    r.wstr(P + off::kSfoDetail, detail);
    r.at(P + off::kSfoParentalLevel)[0] = level;
}

// AUTOSAVE `n` bytes of seeded data into `save`; returns the result code.
static uint32_t autosave(Ram& r, const char* save, size_t n, uint8_t seed,
                         const char* title = "Title", const char* detail = "Detail") {
    make_param(r, MODE_AUTOSAVE, save);
    set_data(r, SRC, n, seed);
    set_buf(r, SRC, 0x10000, static_cast<uint32_t>(n));
    fill_sfo(r, title, "SaveTitle", detail, 3);
    return res(run(r));
}

// ---- PSF ------------------------------------------------------------------

static void test_psf_round_trip() {
    Psf a;
    a.set_string("TITLE", "Hello", 128);
    a.set_string("SAVEDATA_DETAIL", "line1\nline2", 1024);
    a.set_int("PARENTAL_LEVEL", 7);
    uint8_t blob[5] = {1, 2, 3, 4, 5};
    a.set_binary("SAVEDATA_PARAMS", blob, 5, 128);
    std::vector<uint8_t> bytes = a.serialize();

    ASSERT_TRUE(bytes.size() > 20, "PSF has a header");
    ASSERT_TRUE(bytes[0] == 0 && bytes[1] == 'P' && bytes[2] == 'S' &&
                    bytes[3] == 'F', "PSF magic is \\0PSF");

    Psf b;
    ASSERT_TRUE(Psf::parse(bytes.data(), bytes.size(), &b), "PSF parses");
    ASSERT_TRUE(b.get_string("TITLE") == "Hello", "TITLE round trips");
    ASSERT_TRUE(b.get_string("SAVEDATA_DETAIL") == "line1\nline2",
                "DETAIL round trips");
    ASSERT_EQ(b.get_int("PARENTAL_LEVEL"), 7, "int round trips");
    const std::vector<uint8_t>* bin = b.get_binary("SAVEDATA_PARAMS");
    ASSERT_TRUE(bin != nullptr && bin->size() == 5 && (*bin)[4] == 5,
                "binary round trips with its used length");
    ASSERT_TRUE(!b.has("NOPE") && b.get_string("NOPE").empty() &&
                    b.get_int("NOPE") == 0, "missing keys read as empty");

    // Serialising again is byte-identical (keys are kept sorted).
    ASSERT_TRUE(b.serialize() == bytes, "re-serialise is stable");

    // Truncated / garbage input is rejected, never read out of bounds.
    Psf c;
    ASSERT_TRUE(!Psf::parse(bytes.data(), 10, &c), "short header rejected");
    ASSERT_TRUE(!Psf::parse(bytes.data(), bytes.size() - 30, &c),
                "truncated data table rejected");
    std::vector<uint8_t> bad = bytes;
    bad[1] = 'X';
    ASSERT_TRUE(!Psf::parse(bad.data(), bad.size(), &c), "bad magic rejected");
    bad = bytes;
    bad[16] = 0xFF;  // index entry count -> huge
    bad[17] = 0xFF;
    ASSERT_TRUE(!Psf::parse(bad.data(), bad.size(), &c),
                "oversized entry count rejected");
}

// ---- core round trips -----------------------------------------------------

static void test_autosave_autoload_round_trip() {
    Ram r;
    ASSERT_EQ(autosave(r, "0000", 0x321, 0x11, "My Title", "My Detail"), 0,
              "AUTOSAVE succeeds");
    ASSERT_EQ(r.u32(P + off::kResult), 0, "result field written at +28");
    ASSERT_TRUE(fs::exists(save_dir("0000") / "DATA.BIN"), "data file exists");
    ASSERT_TRUE(fs::exists(save_dir("0000") / "PARAM.SFO"), "SFO exists");
    ASSERT_EQ(fs::file_size(save_dir("0000") / "DATA.BIN"), 0x321,
              "data file holds exactly dataSize bytes");

    // PARAM.SFO is a real PSF with the expected keys.
    std::vector<uint8_t> sfo = read_file(save_dir("0000") / "PARAM.SFO");
    Psf psf;
    ASSERT_TRUE(Psf::parse(sfo.data(), sfo.size(), &psf), "SFO parses");
    ASSERT_TRUE(psf.get_string("TITLE") == "My Title", "SFO TITLE");
    ASSERT_TRUE(psf.get_string("SAVEDATA_TITLE") == "SaveTitle",
                "SFO SAVEDATA_TITLE");
    ASSERT_TRUE(psf.get_string("SAVEDATA_DETAIL") == "My Detail",
                "SFO SAVEDATA_DETAIL");
    ASSERT_EQ(psf.get_int("PARENTAL_LEVEL"), 3, "SFO PARENTAL_LEVEL");
    ASSERT_TRUE(psf.get_string("CATEGORY") == "MS", "SFO CATEGORY");
    ASSERT_TRUE(psf.get_string("SAVEDATA_DIRECTORY") ==
                    std::string(kGame) + "0000", "SFO SAVEDATA_DIRECTORY");
    ASSERT_TRUE(psf.get_binary("SAVEDATA_FILE_LIST") != nullptr,
                "SFO SAVEDATA_FILE_LIST");

    // Fresh request in "another boot": nothing but names, buffer and mode.
    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "0000");
    set_buf(r2, DST, 0x1000, 0);
    r2.w32(P + off::kDataSize, 0xDEAD);
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "AUTOLOAD succeeds");
    ASSERT_EQ(r2.u32(P + off::kResult), 0, "load result field");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 0x321, "dataSize restored");
    ASSERT_TRUE(std::memcmp(r2.at(DST), r.at(SRC), 0x321) == 0,
                "data bytes round trip");
    ASSERT_TRUE(r2.str(P + off::kSfoTitle, 128) == "My Title", "title loaded");
    ASSERT_TRUE(r2.str(P + off::kSfoSavedataTitle, 128) == "SaveTitle",
                "savedata title loaded");
    ASSERT_TRUE(r2.str(P + off::kSfoDetail, 1024) == "My Detail",
                "detail loaded");
    ASSERT_EQ(r2.at(P + off::kSfoParentalLevel)[0], 3, "parental level");
    ASSERT_EQ(r2.u32(P + off::kBind), 1021, "bind set like PPSSPP");
    ASSERT_TRUE(!o.summary.empty() &&
                    o.summary.find("AUTOLOAD") != std::string::npos &&
                    o.summary.find(kGame) != std::string::npos,
                "summary names mode and game");
}

static void test_autoload_nothing_saved() {
    Ram r;
    make_param(r, MODE_AUTOLOAD, "0007");
    set_buf(r, DST, 0x1000, 0);
    Outcome o = run(r);
    ASSERT_EQ(res(o), ERR_LOAD_NO_DATA, "AUTOLOAD with no save -> NO_DATA");
    ASSERT_EQ(r.u32(P + off::kResult), ERR_LOAD_NO_DATA,
              "NO_DATA written to the param");
    ASSERT_TRUE(!fs::exists(save_dir("0007")), "load creates nothing");
}

static void test_overwrite_keeps_latest() {
    Ram r;
    ASSERT_EQ(autosave(r, "0001", 0x800, 0x10, "T1", "D1"), 0, "first save");
    ASSERT_EQ(autosave(r, "0001", 0x100, 0x77, "T2", "D2"), 0, "second save");
    ASSERT_EQ(fs::file_size(save_dir("0001") / "DATA.BIN"), 0x100,
              "only the latest content is kept");
    int tmp = 0;
    for (const auto& e : fs::directory_iterator(save_dir("0001"))) {
        if (e.path().extension() == ".tmp") tmp++;
    }
    ASSERT_EQ(tmp, 0, "no temp files left behind");

    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "0001");
    set_buf(r2, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r2)), 0, "load after overwrite");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 0x100, "latest dataSize");
    ASSERT_TRUE(std::memcmp(r2.at(DST), r.at(SRC), 0x100) == 0,
                "latest bytes");
    ASSERT_TRUE(r2.str(P + off::kSfoTitle, 128) == "T2", "latest title");
}

static void test_icon_files() {
    Ram r;
    make_param(r, MODE_AUTOSAVE, "0002");
    set_data(r, SRC, 64, 1);
    set_buf(r, SRC, 0x1000, 64);
    fill_sfo(r, "T", "S", "D", 0);
    set_data(r, ICON, 200, 0x55);
    r.w32(P + off::kIcon0 + off::kFdBuf, ICON);
    r.w32(P + off::kIcon0 + off::kFdBufSize, 200);
    r.w32(P + off::kIcon0 + off::kFdSize, 200);
    ASSERT_EQ(res(run(r)), 0, "save with ICON0");
    ASSERT_EQ(fs::file_size(save_dir("0002") / "ICON0.PNG"), 200,
              "ICON0.PNG written");
    ASSERT_TRUE(!fs::exists(save_dir("0002") / "PIC1.PNG"),
                "no PIC1 without data");

    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "0002");
    set_buf(r2, DST, 0x1000, 0);
    r2.w32(P + off::kIcon0 + off::kFdBuf, DST + 0x2000);
    r2.w32(P + off::kIcon0 + off::kFdBufSize, 0x1000);
    ASSERT_EQ(res(run(r2)), 0, "load with ICON0 buffer");
    ASSERT_EQ(r2.u32(P + off::kIcon0 + off::kFdSize), 200, "icon size");
    ASSERT_TRUE(std::memcmp(r2.at(DST + 0x2000), r.at(ICON), 200) == 0,
                "icon bytes");
}

// ---- SIZES / FILES / LIST / GETSIZE ---------------------------------------

static void setup_sizes(Ram& r, const char* save) {
    make_param(r, MODE_SIZES, save);
    r.w32(P + off::kMsFree, AUX);
    r.w32(P + off::kMsData, AUX + 0x100);
    r.w32(P + off::kUtilityData, AUX + 0x200);
    std::memset(r.at(AUX), 0, 0x400);
    r.wstr(AUX + 0x100 + off::kMsDataGameName, kGame);
    r.wstr(AUX + 0x100 + off::kMsDataSaveName, save);
    // Pre-fill msData.info so "zeroed" is observable.
    std::memset(r.at(AUX + 0x100 + off::kMsDataInfo), 0xFF, off::kUsedSize);
}

static void test_sizes_missing_save() {
    Ram r;
    setup_sizes(r, "0009");
    Outcome o = run(r);
    ASSERT_EQ(res(o), ERR_SIZES_NO_DATA, "SIZES on a missing save");
    ASSERT_EQ(r.u32(AUX + off::kMsFreeClusterSize), 32768, "cluster size");
    ASSERT_TRUE(r.u32(AUX + off::kMsFreeFreeClusters) > 0, "free clusters");
    ASSERT_TRUE(r.u32(AUX + off::kMsFreeFreeSpaceKB) > 0, "free KB");
    ASSERT_TRUE(!r.str(AUX + off::kMsFreeFreeSpaceStr, 8).empty(),
                "free space string");
    uint32_t info = AUX + 0x100 + off::kMsDataInfo;
    ASSERT_EQ(r.u32(info + off::kUsedClusters), 0, "msData used clusters zero");
    ASSERT_EQ(r.u32(info + off::kUsedSpaceKB), 0, "msData used KB zero");
    ASSERT_EQ(r.u32(info + off::kUsedSpace32KB), 0, "msData used32 KB zero");
    ASSERT_TRUE(r.str(info + off::kUsedSpaceStr, 8).empty(), "used str empty");
}

static void test_sizes_existing_save() {
    Ram r;
    ASSERT_EQ(autosave(r, "0003", 0x2000, 3), 0, "seed save");
    Ram r2;
    setup_sizes(r2, "0003");
    r2.w32(P + off::kDataSize, 0x2000);  // utilityData is computed from this
    r2.wstr(P + off::kFileName, "DATA.BIN");
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "SIZES on an existing save");
    uint32_t info = AUX + 0x100 + off::kMsDataInfo;
    ASSERT_TRUE(r2.u32(info + off::kUsedClusters) >= 2,
                "data + SFO occupy at least two clusters");
    ASSERT_EQ(r2.u32(info + off::kUsedSpaceKB),
              r2.u32(info + off::kUsedClusters) * 32, "used KB = clusters*32");
    ASSERT_TRUE(!r2.str(info + off::kUsedSpaceStr, 8).empty(), "used string");
    ASSERT_TRUE(r2.u32(AUX + 0x200 + off::kUsedClusters) >= 3,
                "utilityData covers dir + SFO + data");
}

static void test_files_list() {
    Ram r;
    // Secure data file + ICON0, then a non-secure extra file.
    make_param(r, MODE_AUTOSAVE, "0004");
    set_data(r, SRC, 100, 9);
    set_buf(r, SRC, 0x1000, 100);
    fill_sfo(r, "T", "S", "D", 0);
    set_data(r, ICON, 50, 2);
    r.w32(P + off::kIcon0 + off::kFdBuf, ICON);
    r.w32(P + off::kIcon0 + off::kFdBufSize, 50);
    r.w32(P + off::kIcon0 + off::kFdSize, 50);
    ASSERT_EQ(res(run(r)), 0, "seed save");

    make_param(r, MODE_MAKEDATA, "0004", "EXTRA.BIN");
    set_data(r, SRC, 30, 4);
    set_buf(r, SRC, 0x1000, 30);
    ASSERT_EQ(res(run(r)), 0, "non-secure extra file");

    Ram r2;
    make_param(r2, MODE_FILES, "0004");
    r2.w32(P + off::kFileList, AUX);
    r2.w32(AUX + off::kFlMaxSecure, 8);
    r2.w32(AUX + off::kFlMaxNormal, 8);
    r2.w32(AUX + off::kFlMaxSystem, 5);
    r2.w32(AUX + off::kFlSecureEntries, ENT_S);
    r2.w32(AUX + off::kFlNormalEntries, ENT_N);
    r2.w32(AUX + off::kFlSystemEntries, ENT_Y);
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "FILES succeeds");
    ASSERT_EQ(r2.u32(AUX + off::kFlResultSecure), 1, "one secure entry");
    ASSERT_EQ(r2.u32(AUX + off::kFlResultNormal), 1, "one normal entry");
    ASSERT_EQ(r2.u32(AUX + off::kFlResultSystem), 2,
              "two system entries (ICON0.PNG, PARAM.SFO)");
    ASSERT_TRUE(r2.str(ENT_S + off::kFlEntryName, 16) == "DATA.BIN",
                "secure entry is the data file");
    ASSERT_EQ(r2.u64(ENT_S + off::kFlEntrySize64), 100, "secure size");
    ASSERT_TRUE(r2.str(ENT_N + off::kFlEntryName, 16) == "EXTRA.BIN",
                "normal entry is the extra file");
    ASSERT_EQ(r2.u64(ENT_N + off::kFlEntrySize64), 30, "normal size");
    std::string s0 = r2.str(ENT_Y + off::kFlEntryName, 16);
    std::string s1 = r2.str(ENT_Y + off::kFlEntryBytes + off::kFlEntryName, 16);
    ASSERT_TRUE((s0 == "ICON0.PNG" && s1 == "PARAM.SFO") ||
                    (s0 == "PARAM.SFO" && s1 == "ICON0.PNG"),
                "system entries are ICON0.PNG and PARAM.SFO");
    ASSERT_EQ(r2.u32(ENT_S + off::kFlEntryMode), 0x21FF, "entry st_mode");

    // Missing save -> RW_NO_DATA.
    Ram r3;
    make_param(r3, MODE_FILES, "0099");
    r3.w32(P + off::kFileList, AUX);
    r3.w32(AUX + off::kFlMaxSecure, 8);
    ASSERT_EQ(res(run(r3)), ERR_RW_NO_DATA, "FILES on a missing save");

    // Too many secure entries -> BAD_PARAMS.
    Ram r4;
    make_param(r4, MODE_FILES, "0004");
    r4.w32(P + off::kFileList, AUX);
    r4.w32(AUX + off::kFlMaxSecure, 500);
    r4.w32(AUX + off::kFlSecureEntries, ENT_S);
    ASSERT_EQ(res(run(r4)), ERR_RW_BAD_PARAMS, "FILES maxSecure > 99");
}

static void test_list_mode() {
    Ram r;
    ASSERT_EQ(autosave(r, "0010", 16, 1), 0, "seed 0010");
    ASSERT_EQ(autosave(r, "0011", 16, 2), 0, "seed 0011");
    Ram r2;
    make_param(r2, MODE_LIST, "????");
    r2.w32(P + off::kIdList, AUX);
    r2.w32(AUX + off::kIdMaxCount, 8);
    r2.w32(AUX + off::kIdEntries, ENT_N);
    ASSERT_EQ(res(run(r2)), 0, "LIST succeeds");
    // Other tests leave saves with other names; count only ours.
    uint32_t n = r2.u32(AUX + off::kIdResultCount);
    int found10 = 0, found11 = 0;
    for (uint32_t i = 0; i < n; i++) {
        std::string name =
            r2.str(ENT_N + i * off::kIdEntrySize + off::kIdEntryName, 20);
        if (name == "0010") found10++;
        if (name == "0011") found11++;
        ASSERT_EQ(r2.u32(ENT_N + i * off::kIdEntrySize + off::kIdEntryMode),
                  0x11FF, "list entry st_mode");
    }
    ASSERT_TRUE(found10 == 1 && found11 == 1, "LIST finds both saves by name");

    // maxCount caps the result.
    Ram r3;
    make_param(r3, MODE_LIST, "????");
    r3.w32(P + off::kIdList, AUX);
    r3.w32(AUX + off::kIdMaxCount, 1);
    r3.w32(AUX + off::kIdEntries, ENT_N);
    ASSERT_EQ(res(run(r3)), 0, "LIST with maxCount 1");
    ASSERT_EQ(r3.u32(AUX + off::kIdResultCount), 1, "capped at maxCount");
}

static void set_name_list(Ram& r, const std::vector<std::string>& names) {
    std::memset(r.at(NAMES), 0, 20 * (names.size() + 1));
    for (size_t i = 0; i < names.size(); i++) {
        r.wstr(NAMES + static_cast<uint32_t>(i) * 20, names[i]);
    }
    r.w32(P + off::kSaveNameList, NAMES);
}

static void test_listsave_listload() {
    Ram r;
    // LISTSAVE, focus NAME with saveName "L1": goes to L1.
    make_param(r, MODE_LISTSAVE, "L1");
    set_name_list(r, {"L0", "L1", "L2"});
    set_data(r, SRC, 40, 0x21);
    set_buf(r, SRC, 0x1000, 40);
    fill_sfo(r, "T", "S", "D", 0);
    ASSERT_EQ(res(run(r)), 0, "LISTSAVE succeeds");
    ASSERT_TRUE(fs::exists(save_dir("L1") / "DATA.BIN"),
                "LISTSAVE wrote the focused entry");
    ASSERT_TRUE(!fs::exists(save_dir("L0")) && !fs::exists(save_dir("L2")),
                "other entries untouched");

    // LISTSAVE with saveName not in the list: first entry.
    make_param(r, MODE_LISTSAVE, "zz");
    set_name_list(r, {"L0", "L1", "L2"});
    set_buf(r, SRC, 0x1000, 40);
    fill_sfo(r, "T", "S", "D", 0);
    ASSERT_EQ(res(run(r)), 0, "LISTSAVE without a matching name");
    ASSERT_TRUE(fs::exists(save_dir("L0") / "DATA.BIN"),
                "falls back to the first entry");
    ASSERT_TRUE(r.str(P + off::kSaveName, 20) == "L0", "chosen name copied back");

    // Make L0 older than L1; LISTLOAD must take the newest existing (L1).
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(save_dir("L0") / "PARAM.SFO", old_time);
    fs::last_write_time(save_dir("L0") / "DATA.BIN", old_time);
    fs::last_write_time(save_dir("L0"), old_time);

    Ram r2;
    make_param(r2, MODE_LISTLOAD, "L2");
    set_name_list(r2, {"L0", "L1", "L2"});
    set_buf(r2, DST, 0x1000, 0);
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "LISTLOAD succeeds");
    ASSERT_TRUE(r2.str(P + off::kSaveName, 20) == "L1",
                "LISTLOAD picked the newest save");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 40, "LISTLOAD dataSize");

    // Nothing existing in the list -> NO_DATA.
    Ram r3;
    make_param(r3, MODE_LISTLOAD, "N0");
    set_name_list(r3, {"N0", "N1"});
    set_buf(r3, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r3)), ERR_LOAD_NO_DATA, "LISTLOAD with no saves");
}

static void test_read_write_data() {
    Ram r;
    ASSERT_EQ(autosave(r, "0020", 16, 5), 0, "seed save");
    make_param(r, MODE_WRITEDATA, "0020", "EXTRA.BIN");
    set_data(r, SRC, 77, 0x30);
    set_buf(r, SRC, 0x1000, 77);
    ASSERT_EQ(res(run(r)), 0, "WRITEDATA");
    ASSERT_EQ(fs::file_size(save_dir("0020") / "EXTRA.BIN"), 77,
              "WRITEDATA file size");

    Ram r2;
    make_param(r2, MODE_READDATA, "0020", "EXTRA.BIN");
    set_buf(r2, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r2)), 0, "READDATA");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 77, "READDATA size");
    ASSERT_TRUE(std::memcmp(r2.at(DST), r.at(SRC), 77) == 0, "READDATA bytes");

    // WRITEDATA kept the original data file and its SFO fields.
    ASSERT_EQ(fs::file_size(save_dir("0020") / "DATA.BIN"), 16,
              "original data file untouched");
    std::vector<uint8_t> sfo = read_file(save_dir("0020") / "PARAM.SFO");
    Psf psf;
    ASSERT_TRUE(Psf::parse(sfo.data(), sfo.size(), &psf) &&
                    psf.get_string("TITLE") == "Title",
                "SFO title kept by a title-less write");

    Ram r3;
    make_param(r3, MODE_READDATA, "0021", "EXTRA.BIN");
    set_buf(r3, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r3)), ERR_RW_NO_DATA, "READDATA missing save");
    make_param(r3, MODE_READDATA, "0020", "NOPE.BIN");
    ASSERT_EQ(res(run(r3)), ERR_RW_FILE_NOT_FOUND, "READDATA missing file");

    // The *SECURE variants behave the same (plaintext).
    make_param(r3, MODE_WRITEDATASECURE, "0020", "SEC.BIN");
    set_data(r3, SRC, 20, 0x60);
    set_buf(r3, SRC, 0x1000, 20);
    ASSERT_EQ(res(run(r3)), 0, "WRITEDATASECURE");
    make_param(r3, MODE_READDATASECURE, "0020", "SEC.BIN");
    set_buf(r3, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r3)), 0, "READDATASECURE");
    ASSERT_EQ(r3.u32(P + off::kDataSize), 20, "READDATASECURE size");
}

static void test_getsize() {
    Ram r;
    ASSERT_EQ(autosave(r, "0030", 100, 5), 0, "seed save");

    auto setup = [&](Ram& g, const char* save) {
        make_param(g, MODE_GETSIZE, save);
        g.w32(P + off::kSizeInfo, AUX);
        std::memset(g.at(AUX), 0, 0x200);
        g.w32(AUX + off::kSiNumNormal, 1);
        g.w32(AUX + off::kSiNormalEntries, AUX + 0x100);
        uint64_t sz = 5000;
        std::memcpy(g.at(AUX + 0x100 + off::kSiEntrySize64), &sz, 8);
        g.wstr(AUX + 0x100 + off::kSiEntryName, "DATA.BIN");
    };
    Ram r2;
    setup(r2, "0030");
    ASSERT_EQ(res(run(r2)), 0, "GETSIZE on an existing save");
    ASSERT_EQ(r2.u32(AUX + off::kSiSectorSize), 32768, "sector size");
    ASSERT_TRUE(r2.u32(AUX + off::kSiFreeSectors) > 0, "free sectors");
    ASSERT_EQ(r2.u32(AUX + off::kSiNeededKB), 0, "plenty of space");

    Ram r3;
    setup(r3, "0031");
    ASSERT_EQ(res(run(r3)), ERR_RW_NO_DATA, "GETSIZE on a missing save");
    ASSERT_EQ(r3.u32(AUX + off::kSiSectorSize), 32768,
              "sizeInfo still filled for a missing save");
}

// ---- robustness -----------------------------------------------------------

static void test_bad_pointers() {
    Ram r;
    // dataBuf outside guest RAM (offset 0x02000000 > 4MB): error, no crash.
    make_param(r, MODE_AUTOSAVE, "0040");
    set_buf(r, 0x0A000000u, 0x1000, 16);
    fill_sfo(r, "T", "S", "D", 0);
    ASSERT_EQ(res(run(r)), ERR_SAVE_PARAM, "AUTOSAVE with a bad dataBuf");

    // A buffer that starts inside but runs past the end of RAM.
    make_param(r, MODE_AUTOSAVE, "0040");
    set_buf(r, kBase + static_cast<uint32_t>(kRam) - 8, 0x1000, 0x1000);
    fill_sfo(r, "T", "S", "D", 0);
    ASSERT_EQ(res(run(r)), ERR_SAVE_PARAM, "AUTOSAVE with an overrunning buf");

    // Good save, then load into a bad buffer.
    ASSERT_EQ(autosave(r, "0041", 64, 8), 0, "seed save");
    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "0041");
    set_buf(r2, 0x0A000000u, 0x1000, 0);
    ASSERT_EQ(res(run(r2)), ERR_LOAD_DATA_BROKEN, "AUTOLOAD with a bad dataBuf");

    // dataBufSize smaller than the data: BROKEN, buffer untouched.
    make_param(r2, MODE_AUTOLOAD, "0041");
    set_buf(r2, DST, 16, 0);
    std::memset(r2.at(DST), 0xEE, 64);
    ASSERT_EQ(res(run(r2)), ERR_LOAD_DATA_BROKEN, "AUTOLOAD into a small buffer");
    ASSERT_EQ(r2.at(DST)[0], 0xEE, "small buffer left untouched");

    // dataSize > dataBufSize on save.
    make_param(r2, MODE_AUTOSAVE, "0042");
    set_buf(r2, SRC, 16, 32);
    ASSERT_EQ(res(run(r2)), ERR_RW_BAD_PARAMS, "dataSize > dataBufSize");

    // Bad icon buffer.
    make_param(r2, MODE_AUTOSAVE, "0042");
    set_buf(r2, SRC, 0x1000, 16);
    r2.w32(P + off::kIcon0 + off::kFdBuf, 0x0A000000u);
    r2.w32(P + off::kIcon0 + off::kFdBufSize, 100);
    r2.w32(P + off::kIcon0 + off::kFdSize, 100);
    fill_sfo(r2, "T", "S", "D", 0);
    Outcome oi = run(r2);
    ASSERT_TRUE(res(oi) != 0, "bad ICON0 buffer is an error");

    // Param itself outside RAM / bad size / path traversal.
    Outcome o = execute(r.v.data(), r.v.size(), 0x0A000000u, g_opts);
    ASSERT_EQ(res(o), ERR_UTILITY_INVALID_ADDRESS, "param outside RAM");
    o = execute(r.v.data(), r.v.size(), 0, g_opts);
    ASSERT_EQ(res(o), ERR_UTILITY_INVALID_ADDRESS, "null param");
    make_param(r, MODE_AUTOLOAD, "0043");
    r.w32(P + off::kSize, 100);
    ASSERT_EQ(res(run(r)), ERR_UTILITY_INVALID_PARAM_SIZE, "bad param size");
    make_param(r, MODE_AUTOLOAD, "../evil");
    ASSERT_EQ(res(run(r)), ERR_UTILITY_INVALID_PARAM_SIZE, "path traversal");
    ASSERT_TRUE(!fs::exists(g_root.parent_path() / "evil"), "nothing escaped");

    // Name list / struct pointers outside RAM.
    make_param(r, MODE_LISTLOAD, "x");
    r.w32(P + off::kSaveNameList, 0x0A000000u);
    set_buf(r, DST, 0x1000, 0);
    (void)run(r);  // must not crash; falls back to saveName
    make_param(r, MODE_FILES, "0041");
    r.w32(P + off::kFileList, 0x0A000000u);
    ASSERT_EQ(static_cast<int32_t>(res(run(r))), -1, "FILES with bad fileList");
    make_param(r, MODE_LIST, "????");
    r.w32(P + off::kIdList, 0x0A000000u);
    (void)run(r);
    make_param(r, MODE_SIZES, "0041");
    r.w32(P + off::kMsFree, 0x0A000000u);
    r.w32(P + off::kMsData, 0x0A000000u);
    r.w32(P + off::kUtilityData, 0x0A000000u);
    (void)run(r);
    make_param(r, MODE_GETSIZE, "0041");
    r.w32(P + off::kSizeInfo, 0x0A000000u);
    (void)run(r);
    ASSERT_TRUE(true, "bad struct pointers do not crash");
}

static void test_handles_mode() {
    const uint32_t yes[] = {0, 1, 2, 3, 4, 5, 8, 11, 12, 13, 14, 15, 16, 17, 18, 22};
    for (uint32_t m : yes) ASSERT_TRUE(handles_mode(m), "mode is handled");
    const uint32_t no[] = {6, 7, 9, 10, 19, 20, 21, 23, 99};
    for (uint32_t m : no) ASSERT_TRUE(!handles_mode(m), "mode is not handled");
}

int main() {
    g_root = fs::temp_directory_path() /
             ("psp_savedata_test_" +
              std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch().count()));
    fs::remove_all(g_root);  // our own fresh temp dir name; nothing else
    g_opts.root = (g_root / "SAVEDATA").string();
    g_root = g_root / "SAVEDATA";  // saves live under here
    fs::create_directories(g_root.parent_path());

    test_psf_round_trip();
    test_handles_mode();
    test_autosave_autoload_round_trip();
    test_autoload_nothing_saved();
    test_overwrite_keeps_latest();
    test_icon_files();
    test_sizes_missing_save();
    test_sizes_existing_save();
    test_files_list();
    test_list_mode();
    test_listsave_listload();
    test_read_write_data();
    test_getsize();
    test_bad_pointers();

    std::error_code ec;
    fs::remove_all(g_root.parent_path(), ec);
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
