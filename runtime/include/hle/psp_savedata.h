// Ported from PPSSPP (GPL-2.0-or-later), Core/Dialog/SavedataParam.cpp and
// Core/Dialog/PSPSaveDialog.cpp. Copyright (c) 2012- PPSSPP Project.
//
// sceUtilitySavedata back end. Pure C++: no PPSSPP framework, no scheduler,
// no logging. The guest SceUtilitySavedataParam is read and written in place
// through a bounds-checked view of guest RAM (addresses are masked with
// 0x07FFFFFF like the emitted code does), so a bad guest pointer yields a PSP
// error code instead of a host crash. Saves live on the host file system in a
// memory-stick-like layout:
//
//   <root>/<gameName><saveName>/<fileName>      the data file (plaintext)
//   <root>/<gameName><saveName>/PARAM.SFO       real PSF with TITLE, ...
//   <root>/<gameName><saveName>/ICON0.PNG, ICON1.PMF, PIC1.PNG, SND0.AT3
//
// Modes: 0 AUTOLOAD, 1 AUTOSAVE, 2 LOAD, 3 SAVE, 4 LISTLOAD, 5 LISTSAVE,
// 8 SIZES, 11 LIST, 12 FILES, 13 MAKEDATASECURE, 14 MAKEDATA,
// 15 READDATASECURE, 16 READDATA, 17 WRITEDATASECURE, 18 WRITEDATA,
// 22 GETSIZE. Every other mode is not handled here (handles_mode() == false).
//
// Deviations from PPSSPP (all deliberate):
//  - Savedata encryption/hashing is not ported. Data files are stored in
//    plaintext in every mode (the *SECURE modes read/write plaintext too);
//    SAVEDATA_FILE_LIST still records which files are secure and the hash
//    field is zero. Saves are therefore not loadable on real hardware.
//  - Everything is synchronous: no 200ms init delay, no IO thread.
//  - There is no UI. LISTLOAD loads the newest existing save in saveNameList
//    (no existing save -> LOAD_NO_DATA); LISTSAVE picks the entry the
//    `focus` field selects (NAME: the entry equal to saveName, else the first).
//    LOAD/SAVE (2/3) run without a confirmation prompt.
//  - The memory stick is a virtual 4GB card; free space is that minus the
//    bytes the game's saves already use. File creation/access times are the
//    host modification time (std::filesystem has no portable ctime).
//  - Writes go to "<file>.tmp" in the same directory and are renamed over the
//    target, so a crash never leaves a half-written file (no fsync).
//  - Names with path separators or "."/".." components are rejected with
//    SCE_ERROR_UTILITY_INVALID_PARAM_SIZE (as PPSSPP does at InitStart).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace psp_savedata {

// PPSSPP Core/HLE/ErrorCodes.h
enum : uint32_t {
    ERR_UTILITY_INVALID_ADDRESS = 0x80110002u,
    ERR_UTILITY_INVALID_PARAM_SIZE = 0x80110004u,
    ERR_LOAD_DATA_BROKEN = 0x80110306u,
    ERR_LOAD_NO_DATA = 0x80110307u,
    ERR_LOAD_PARAM = 0x80110308u,
    ERR_LOAD_FILE_NOT_FOUND = 0x80110309u,
    ERR_RW_MEMSTICK_FULL = 0x80110323u,
    ERR_RW_DATA_BROKEN = 0x80110326u,
    ERR_RW_NO_DATA = 0x80110327u,
    ERR_RW_BAD_PARAMS = 0x80110328u,
    ERR_RW_FILE_NOT_FOUND = 0x80110329u,
    ERR_SAVE_MS_NOSPACE = 0x80110383u,
    ERR_SAVE_ACCESS_ERROR = 0x80110385u,
    ERR_SAVE_PARAM = 0x80110388u,
    ERR_SIZES_NO_DATA = 0x801103C7u,
};

// SceUtilitySavedataType (PPSSPP SavedataParam.h)
enum Mode : uint32_t {
    MODE_AUTOLOAD = 0,
    MODE_AUTOSAVE = 1,
    MODE_LOAD = 2,
    MODE_SAVE = 3,
    MODE_LISTLOAD = 4,
    MODE_LISTSAVE = 5,
    MODE_SIZES = 8,
    MODE_LIST = 11,
    MODE_FILES = 12,
    MODE_MAKEDATASECURE = 13,
    MODE_MAKEDATA = 14,
    MODE_READDATASECURE = 15,
    MODE_READDATA = 16,
    MODE_WRITEDATASECURE = 17,
    MODE_WRITEDATA = 18,
    MODE_GETSIZE = 22,
};

// SceUtilitySavedataFocus
enum Focus : uint32_t {
    FOCUS_NAME = 0,
    FOCUS_FIRSTLIST = 1,
    FOCUS_LASTLIST = 2,
    FOCUS_LATEST = 3,
    FOCUS_OLDEST = 4,
    FOCUS_FIRSTDATA = 5,
    FOCUS_LASTDATA = 6,
    FOCUS_FIRSTEMPTY = 7,
    FOCUS_LASTEMPTY = 8,
};

// Byte offsets into the guest structs (PPSSPP SavedataParam.h layouts; the
// request is 1536 bytes in its newest form).
namespace off {
// SceUtilitySavedataParam
constexpr uint32_t kSize = 0;
constexpr uint32_t kResult = 28;  // pspUtilityDialogCommon.result
constexpr uint32_t kMode = 48;
constexpr uint32_t kBind = 52;
constexpr uint32_t kOverwriteMode = 56;
constexpr uint32_t kGameName = 60;       // char[13]
constexpr uint32_t kSaveName = 76;       // char[20]
constexpr uint32_t kSaveNameList = 96;   // ptr to char[20] array, "" ends it
constexpr uint32_t kFileName = 100;      // char[13]
constexpr uint32_t kDataBuf = 116;
constexpr uint32_t kDataBufSize = 120;
constexpr uint32_t kDataSize = 124;
constexpr uint32_t kSfoTitle = 128;        // char[0x80]
constexpr uint32_t kSfoSavedataTitle = 256;  // char[0x80]
constexpr uint32_t kSfoDetail = 384;       // char[0x400]
constexpr uint32_t kSfoParentalLevel = 1408;
constexpr uint32_t kIcon0 = 1412;  // PspUtilitySavedataFileData (16 bytes)
constexpr uint32_t kIcon1 = 1428;
constexpr uint32_t kPic1 = 1444;
constexpr uint32_t kSnd0 = 1460;
constexpr uint32_t kNewData = 1476;
constexpr uint32_t kFocus = 1480;
constexpr uint32_t kAbortStatus = 1484;
constexpr uint32_t kMsFree = 1488;
constexpr uint32_t kMsData = 1492;
constexpr uint32_t kUtilityData = 1496;
constexpr uint32_t kKey = 1500;  // u8[16]
constexpr uint32_t kSecureVersion = 1516;
constexpr uint32_t kMultiStatus = 1520;
constexpr uint32_t kIdList = 1524;
constexpr uint32_t kFileList = 1528;
constexpr uint32_t kSizeInfo = 1532;
constexpr uint32_t kParamSize = 1536;

// PspUtilitySavedataFileData
constexpr uint32_t kFdBuf = 0;
constexpr uint32_t kFdBufSize = 4;
constexpr uint32_t kFdSize = 8;

// SceUtilitySavedataMsFreeInfo (20 bytes)
constexpr uint32_t kMsFreeClusterSize = 0;
constexpr uint32_t kMsFreeFreeClusters = 4;
constexpr uint32_t kMsFreeFreeSpaceKB = 8;
constexpr uint32_t kMsFreeFreeSpaceStr = 12;  // char[8]
constexpr uint32_t kMsFreeSize = 20;

// SceUtilitySavedataUsedDataInfo (28 bytes)
constexpr uint32_t kUsedClusters = 0;
constexpr uint32_t kUsedSpaceKB = 4;
constexpr uint32_t kUsedSpaceStr = 8;  // char[8]
constexpr uint32_t kUsedSpace32KB = 16;
constexpr uint32_t kUsedSpace32Str = 20;  // char[8]
constexpr uint32_t kUsedSize = 28;

// SceUtilitySavedataMsDataInfo (64 bytes)
constexpr uint32_t kMsDataGameName = 0;  // char[13]
constexpr uint32_t kMsDataSaveName = 16;  // char[20]
constexpr uint32_t kMsDataInfo = 36;      // UsedDataInfo
constexpr uint32_t kMsDataSize = 64;

// SceUtilitySavedataIdListInfo / Entry (72 bytes)
constexpr uint32_t kIdMaxCount = 0;
constexpr uint32_t kIdResultCount = 4;
constexpr uint32_t kIdEntries = 8;
constexpr uint32_t kIdEntryMode = 0;
constexpr uint32_t kIdEntryCtime = 4;  // ScePspDateTime (16 bytes)
constexpr uint32_t kIdEntryAtime = 20;
constexpr uint32_t kIdEntryMtime = 36;
constexpr uint32_t kIdEntryName = 52;  // char[20]
constexpr uint32_t kIdEntrySize = 72;

// SceUtilitySavedataFileListInfo / Entry (80 bytes)
constexpr uint32_t kFlMaxSecure = 0;
constexpr uint32_t kFlMaxNormal = 4;
constexpr uint32_t kFlMaxSystem = 8;
constexpr uint32_t kFlResultSecure = 12;
constexpr uint32_t kFlResultNormal = 16;
constexpr uint32_t kFlResultSystem = 20;
constexpr uint32_t kFlSecureEntries = 24;
constexpr uint32_t kFlNormalEntries = 28;
constexpr uint32_t kFlSystemEntries = 32;
constexpr uint32_t kFlEntryMode = 0;
constexpr uint32_t kFlEntrySize64 = 8;  // u64 st_size
constexpr uint32_t kFlEntryCtime = 16;
constexpr uint32_t kFlEntryAtime = 32;
constexpr uint32_t kFlEntryMtime = 48;
constexpr uint32_t kFlEntryName = 64;  // char[16]
constexpr uint32_t kFlEntryBytes = 80;

// PspUtilitySavedataSizeInfo (60 bytes) / SizeEntry (24 bytes)
constexpr uint32_t kSiNumSecure = 0;
constexpr uint32_t kSiNumNormal = 4;
constexpr uint32_t kSiSecureEntries = 8;
constexpr uint32_t kSiNormalEntries = 12;
constexpr uint32_t kSiSectorSize = 16;
constexpr uint32_t kSiFreeSectors = 20;
constexpr uint32_t kSiFreeKB = 24;
constexpr uint32_t kSiFreeString = 28;  // char[8]
constexpr uint32_t kSiNeededKB = 36;
constexpr uint32_t kSiNeededString = 40;
constexpr uint32_t kSiOverwriteKB = 48;
constexpr uint32_t kSiOverwriteString = 52;
constexpr uint32_t kSiEntrySize64 = 0;  // u64 size
constexpr uint32_t kSiEntryName = 8;    // char[16]
constexpr uint32_t kSiEntryBytes = 24;
}  // namespace off

// PSF (PARAM.SFO) container: header, sorted key table, index, data table.
class Psf {
public:
    enum Type : uint16_t {
        TYPE_BINARY = 0x0004,
        TYPE_STRING = 0x0204,  // NUL-terminated UTF-8
        TYPE_INT = 0x0404,
    };

    struct Value {
        Type type = TYPE_BINARY;
        std::vector<uint8_t> data;  // bytes in use (string includes the NUL)
        uint32_t max_len = 0;       // reserved bytes in the data table
    };

    void set_string(const std::string& key, const std::string& val,
                    uint32_t max_len);
    void set_int(const std::string& key, int32_t val);
    void set_binary(const std::string& key, const uint8_t* data, size_t len,
                    uint32_t max_len);

    bool has(const std::string& key) const;
    std::string get_string(const std::string& key) const;
    int32_t get_int(const std::string& key) const;
    // nullptr when the key is absent.
    const std::vector<uint8_t>* get_binary(const std::string& key) const;

    std::vector<uint8_t> serialize() const;
    // Fully bounds-checked; false on any malformed input.
    static bool parse(const uint8_t* data, size_t size, Psf* out);

private:
    std::map<std::string, Value> values_;
};

struct Options {
    // Host save root; empty = default_root().
    std::string root;
    // Compiled SDK version (sceKernelSetCompiledSdkVersion*); 0 = unknown.
    uint32_t sdk_version = 0;
};

struct Outcome {
    int32_t result = 0;     // also written to pspUtilityDialogCommon.result
    std::string summary;    // one log line: mode, game+save name, result
};

// True for the modes execute() implements.
bool handles_mode(uint32_t mode);

// $PSPRECOMP_SAVEDATA if set and non-empty, else "SAVEDATA" (relative to the
// current working directory).
std::string default_root();

// Runs the operation selected by param->mode. `rdram`/`ram_size` describe the
// guest RAM mapping (0x08000000 bytes for the real rdram); `param_addr` is the
// guest address of the SceUtilitySavedataParam. Never throws and never reads
// or writes outside [rdram, rdram+ram_size).
Outcome execute(uint8_t* rdram, size_t ram_size, uint32_t param_addr,
                const Options& opts);

}  // namespace psp_savedata
