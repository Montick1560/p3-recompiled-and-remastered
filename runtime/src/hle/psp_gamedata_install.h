// sceUtilityGamedataInstall back end: data-install copy engine plus the
// Memory Stick free-space descriptor.
//
// Ported from PPSSPP (GPL-2.0-or-later):
//   Core/Dialog/PSPGamedataInstallDialog.cpp (copy loop, progress, SFO,
//     destination naming) and Core/HLE/sceIo.cpp (DeviceSize devctl
//     0x02425818 / 0x02425823).
//
// Pure C++: no SDL, no scheduler, no guest RAM. The HLE glue lives in
// psp_hle_utility.cpp (dialog status machine) and psp_hle_io.cpp (ms0:
// mount, devctl); it drives Installer across Update calls and reads the
// guest SceUtilityGamedataInstallParam itself.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace psp_gamedata_install {

// PPSSPP Core/HLE/ErrorCodes.h (subset used here).
enum : uint32_t {
    ERR_UTILITY_INVALID_STATUS = 0x80110001u,
    ERR_UTILITY_INVALID_ADDRESS = 0x80110002u,
    ERR_UTILITY_INVALID_PARAM_SIZE = 0x80110004u,
    ERR_UTILITY_GAMEDATA_INVALID_MODE = 0x80111908u,
    ERR_MEMSTICK_DEVCTL_BAD_PARAMS = 0x80220081u,
};

// Byte offsets into the guest SceUtilityGamedataInstallParam
// (PPSSPP Core/Dialog/PSPGamedataInstallDialog.h). The struct is 1432
// bytes in its newest form; 1424 is also accepted (CheckRequest sizes
// {1424, 1432}) — every field this back end touches ends before 1384,
// so both sizes expose them.
namespace off {
constexpr uint32_t kSize = 0;              // u32 struct size
constexpr uint32_t kResult = 28;           // pspUtilityDialogCommon.result
constexpr uint32_t kMode = 48;             // s32 mode
constexpr uint32_t kGameName = 52;         // char[13]
constexpr uint32_t kDataName = 68;         // char[20]
constexpr uint32_t kSfoTitle = 88;         // char[128]
constexpr uint32_t kSfoSavedataTitle = 216;  // char[128]
constexpr uint32_t kSfoDetail = 344;       // char[1024]
constexpr uint32_t kSfoParentalLevel = 1368;  // u8
constexpr uint32_t kProgress = 1372;       // s32 install progress 0..100
constexpr uint32_t kUnknownResult1 = 1376;  // u32: files installed
constexpr uint32_t kUnknownResult2 = 1380;  // u32: files installed
constexpr uint32_t kSizeV1 = 1424;
constexpr uint32_t kSizeV2 = 1432;
}  // namespace off

// Mode: 1 shows a progress bar, anything else proceeds silently; >= 2 is
// rejected with ERR_UTILITY_GAMEDATA_INVALID_MODE.
constexpr int32_t kModeShowProgress = 1;

// ---- Virtual Memory Stick ------------------------------------------------
// The savedata back end reports a virtual 4 GB card (psp_savedata.cpp:
// kMemStickBytes); data-install free-space queries report the same card.
constexpr uint64_t kVirtualCardBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint32_t kSectorSize = 0x200;
constexpr uint32_t kMemStickSectorSize = 32 * 1024;
constexpr uint32_t kSectorCount = kMemStickSectorSize / kSectorSize;  // 64

// sceIo.cpp `DeviceSize` (devctl 0x02425818 payload, 20 bytes).
struct DeviceSize {
    uint32_t max_clusters = 0;
    uint32_t free_clusters = 0;
    uint32_t max_sectors = 0;
    uint32_t sector_size = kSectorSize;
    uint32_t sector_count = kSectorCount;
};

// PPSSPP sceIo.cpp 0x02425818 formula: 95% of the card is usable, and the
// whole usable area is reported free.
DeviceSize ms_device_size(uint64_t free_bytes = kVirtualCardBytes);

// ---- Destination naming --------------------------------------------------
// PPSSPP GetGameDataInstallFileName: saveBasePath ("ms0:/PSP/SAVEDATA/")
// + gameName + dataName + "/" + filename. The host equivalents below join
// onto the savedata root ($PSPRECOMP_SAVEDATA or ./SAVEDATA).
std::string install_dir_name(const std::string& game_name,
                             const std::string& data_name);
std::string install_dir(const std::string& savedata_root,
                        const std::string& game_name,
                        const std::string& data_name);
std::string install_file_path(const std::string& savedata_root,
                              const std::string& game_name,
                              const std::string& data_name,
                              const std::string& filename);

// PARAM.SFO contents (PPSSPP WriteSfoFile), merged over any PARAM.SFO the
// destination already holds.
struct SfoParams {
    std::string title;
    std::string savedata_title;
    std::string detail;
    int32_t parental_level = 0;
};

// ---- Chunked copy engine -------------------------------------------------
// PPSSPP copies GAMEDATA_READS_PER_UPDATE (20) x GAMEDATA_BYTES_PER_READ
// (32 KiB) per Update so progress advances without blocking one HLE call
// for seconds; one Update with no file open only opens the next file.
constexpr uint32_t kBytesPerRead = 32768;
constexpr uint32_t kReadsPerUpdate = 20;

class Installer {
 public:
    Installer() = default;
    Installer(const std::string& src_dir, const std::string& dst_dir,
              const SfoParams& sfo);

    // False when the source directory holds no regular files (PPSSPP
    // InitStart then fails the install).
    bool ok() const { return ok_; }
    // True once every file is copied and PARAM.SFO is written.
    bool finished() const { return finished_; }
    // 0..100 (PPSSPP UpdateProgress; 100 when there is nothing to copy).
    int progress() const;
    // One Update quantum: open the next file, or copy the next chunk run.
    // Never throws; an unreadable file is skipped like PPSSPP does.
    void step();
    int files_done() const { return read_files_; }
    int file_count() const { return static_cast<int>(files_.size()); }
    uint64_t total_bytes() const { return total_bytes_; }
    uint64_t copied_bytes() const { return copied_bytes_; }

 private:
    void open_next();
    void copy_chunks();
    void close_current();
    void write_sfo();

    std::string src_dir_;
    std::string dst_dir_;
    std::string dir_name_;  // gameName+dataName (last component of dst_dir_)
    SfoParams sfo_;
    std::vector<std::string> files_;
    std::vector<uint64_t> sizes_;
    uint64_t total_bytes_ = 0;
    uint64_t copied_bytes_ = 0;
    int read_files_ = 0;
    size_t index_ = 0;
    bool file_open_ = false;
    uint64_t file_left_ = 0;
    std::ifstream in_;
    std::ofstream out_;
    bool ok_ = false;
    bool finished_ = false;
};

}  // namespace psp_gamedata_install
