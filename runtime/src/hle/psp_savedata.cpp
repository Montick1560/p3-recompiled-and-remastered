// Ported from PPSSPP (GPL-2.0-or-later), Core/Dialog/SavedataParam.cpp and
// Core/Dialog/PSPSaveDialog.cpp. Copyright (c) 2012- PPSSPP Project.
// See psp_savedata.h for the list of deliberate deviations.

#include "hle/psp_savedata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace psp_savedata {

// ======================================================================
// PSF (PARAM.SFO)
// ======================================================================

namespace {

constexpr uint32_t kPsfHeaderSize = 20;
constexpr uint32_t kPsfIndexEntrySize = 16;
constexpr uint32_t kPsfMaxEntries = 512;
constexpr size_t kPsfMaxKeyLen = 256;

inline uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

inline uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

inline uint32_t rd32le(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
    uint8_t b[2];
    std::memcpy(b, &v, 2);
    out.insert(out.end(), b, b + 2);
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
}

}  // namespace

void Psf::set_string(const std::string& key, const std::string& val,
                     uint32_t max_len) {
    Value v;
    v.type = TYPE_STRING;
    v.max_len = align4(std::max<uint32_t>(max_len, 4));
    size_t n = std::min<size_t>(val.size(), v.max_len - 1);
    v.data.assign(val.begin(), val.begin() + static_cast<std::ptrdiff_t>(n));
    v.data.push_back(0);
    values_[key] = std::move(v);
}

void Psf::set_int(const std::string& key, int32_t val) {
    Value v;
    v.type = TYPE_INT;
    v.max_len = 4;
    v.data.resize(4);
    std::memcpy(v.data.data(), &val, 4);
    values_[key] = std::move(v);
}

void Psf::set_binary(const std::string& key, const uint8_t* data, size_t len,
                     uint32_t max_len) {
    Value v;
    v.type = TYPE_BINARY;
    v.max_len = align4(max_len);
    size_t n = std::min<size_t>(len, v.max_len);
    if (data != nullptr && n > 0) {
        v.data.assign(data, data + n);
    } else {
        v.data.assign(n, 0);
    }
    values_[key] = std::move(v);
}

bool Psf::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string Psf::get_string(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.type != TYPE_STRING) return "";
    const std::vector<uint8_t>& d = it->second.data;
    size_t n = 0;
    while (n < d.size() && d[n] != 0) n++;
    return std::string(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(n));
}

int32_t Psf::get_int(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end() || it->second.type != TYPE_INT ||
        it->second.data.size() < 4) {
        return 0;
    }
    int32_t v;
    std::memcpy(&v, it->second.data.data(), 4);
    return v;
}

const std::vector<uint8_t>* Psf::get_binary(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second.data;
}

std::vector<uint8_t> Psf::serialize() const {
    // Key table: keys in sorted order (std::map), NUL-terminated.
    std::vector<uint8_t> keys;
    std::vector<uint32_t> key_offs;
    for (const auto& kv : values_) {
        key_offs.push_back(static_cast<uint32_t>(keys.size()));
        keys.insert(keys.end(), kv.first.begin(), kv.first.end());
        keys.push_back(0);
    }
    while (keys.size() % 4 != 0) keys.push_back(0);

    uint32_t n = static_cast<uint32_t>(values_.size());
    uint32_t key_start = kPsfHeaderSize + kPsfIndexEntrySize * n;
    uint32_t data_start = key_start + static_cast<uint32_t>(keys.size());

    std::vector<uint8_t> out;
    out.push_back(0);
    out.push_back('P');
    out.push_back('S');
    out.push_back('F');
    put32(out, 0x00000101u);
    put32(out, key_start);
    put32(out, data_start);
    put32(out, n);

    uint32_t data_off = 0;
    size_t i = 0;
    for (const auto& kv : values_) {
        const Value& v = kv.second;
        put16(out, static_cast<uint16_t>(key_offs[i++]));
        put16(out, static_cast<uint16_t>(v.type));
        put32(out, static_cast<uint32_t>(v.data.size()));
        put32(out, v.max_len);
        put32(out, data_off);
        data_off += v.max_len;
    }
    out.insert(out.end(), keys.begin(), keys.end());
    for (const auto& kv : values_) {
        const Value& v = kv.second;
        out.insert(out.end(), v.data.begin(), v.data.end());
        out.insert(out.end(), v.max_len - v.data.size(), 0);
    }
    return out;
}

bool Psf::parse(const uint8_t* data, size_t size, Psf* out) {
    if (data == nullptr || out == nullptr || size < kPsfHeaderSize) {
        return false;
    }
    if (data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') {
        return false;
    }
    uint32_t key_start = rd32le(data + 8);
    uint32_t data_start = rd32le(data + 12);
    uint32_t n = rd32le(data + 16);
    if (n > kPsfMaxEntries) return false;
    uint64_t index_end = kPsfHeaderSize + uint64_t{kPsfIndexEntrySize} * n;
    if (index_end > size || key_start > size || data_start > size) {
        return false;
    }

    Psf parsed;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = data + kPsfHeaderSize + kPsfIndexEntrySize * i;
        uint32_t key_off = rd16(e);
        uint16_t fmt = rd16(e + 2);
        uint32_t len = rd32le(e + 4);
        uint32_t max_len = rd32le(e + 8);
        uint32_t d_off = rd32le(e + 12);

        uint64_t kpos = uint64_t{key_start} + key_off;
        if (kpos >= size) return false;
        size_t klen = 0;
        while (kpos + klen < size && data[kpos + klen] != 0) {
            if (++klen > kPsfMaxKeyLen) return false;
        }
        if (kpos + klen >= size) return false;  // unterminated
        std::string key(reinterpret_cast<const char*>(data + kpos), klen);

        if (len > max_len) return false;
        uint64_t dpos = uint64_t{data_start} + d_off;
        if (dpos + max_len > size) return false;

        Value v;
        v.max_len = max_len;
        v.data.assign(data + dpos, data + dpos + len);
        if (fmt == TYPE_STRING) {
            v.type = TYPE_STRING;
        } else if (fmt == TYPE_INT) {
            if (len != 4) return false;
            v.type = TYPE_INT;
        } else {
            v.type = TYPE_BINARY;
        }
        parsed.values_[key] = std::move(v);
    }
    *out = std::move(parsed);
    return true;
}

// ======================================================================
// Guest memory access
// ======================================================================

namespace {

// Bounds-checked view of guest RAM; addresses are masked like the emitted
// code does. Address 0 is never valid (matches PSPPointer::IsValid).
struct Mem {
    uint8_t* base = nullptr;
    size_t size = 0;

    uint8_t* range(uint32_t addr, uint64_t len) const {
        if (base == nullptr || addr == 0) return nullptr;
        uint64_t off = addr & 0x07FFFFFFu;
        if (off > size || len > size - off) return nullptr;
        return base + off;
    }
    bool valid(uint32_t addr, uint64_t len) const {
        return range(addr, len) != nullptr;
    }
    uint32_t rd32(uint32_t addr) const {
        const uint8_t* p = range(addr, 4);
        if (p == nullptr) return 0;
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    void wr32(uint32_t addr, uint32_t v) const {
        uint8_t* p = range(addr, 4);
        if (p != nullptr) std::memcpy(p, &v, 4);
    }
    void wr64(uint32_t addr, uint64_t v) const {
        uint8_t* p = range(addr, 8);
        if (p != nullptr) std::memcpy(p, &v, 8);
    }
    uint64_t rd64(uint32_t addr) const {
        const uint8_t* p = range(addr, 8);
        if (p == nullptr) return 0;
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    // Fixed-size NUL-padded char field.
    std::string str(uint32_t addr, size_t cap) const {
        const uint8_t* p = range(addr, cap);
        if (p == nullptr) return "";
        size_t n = 0;
        while (n < cap && p[n] != 0) n++;
        return std::string(reinterpret_cast<const char*>(p), n);
    }
    // strncpy semantics: copy up to cap bytes, zero-pad the rest.
    void wstr(uint32_t addr, size_t cap, const std::string& s) const {
        uint8_t* p = range(addr, cap);
        if (p == nullptr) return;
        size_t n = std::min(cap, s.size());
        std::memcpy(p, s.data(), n);
        std::memset(p + n, 0, cap - n);
    }
    void zero(uint32_t addr, size_t len) const {
        uint8_t* p = range(addr, len);
        if (p != nullptr) std::memset(p, 0, len);
    }
};

// ======================================================================
// Host file helpers
// ======================================================================

const char* const kIcon0Name = "ICON0.PNG";
const char* const kIcon1Name = "ICON1.PMF";
const char* const kPic1Name = "PIC1.PNG";
const char* const kSnd0Name = "SND0.AT3";
const char* const kSfoName = "PARAM.SFO";

constexpr uint64_t kSectorSize = 32 * 1024;
constexpr uint64_t kMemStickBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint32_t kFileListCountMax = 99;
constexpr uint32_t kFileListEntrySize = 32;  // name[13] + hash[16] + pad[3]
constexpr uint32_t kFileListTotalSize = kFileListEntrySize * kFileListCountMax;

bool write_file_atomic(const fs::path& path, const uint8_t* data, size_t n) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (n > 0) {
            f.write(reinterpret_cast<const char*>(data),
                    static_cast<std::streamsize>(n));
        }
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // Some platforms refuse to rename over an existing file.
        std::error_code ec2;
        fs::remove(path, ec2);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

bool read_file(const fs::path& path, uint64_t max_bytes,
               std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->clear();
    std::vector<uint8_t> buf(64 * 1024);
    while (out->size() < max_bytes && f) {
        uint64_t want = std::min<uint64_t>(buf.size(), max_bytes - out->size());
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(want));
        std::streamsize got = f.gcount();
        if (got <= 0) break;
        out->insert(out->end(), buf.begin(), buf.begin() + got);
    }
    return true;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

struct DirItem {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
    fs::file_time_type mtime{};
};

// Sorted directory listing (names only, non-recursive). `exists` is false
// when `p` is not a readable directory.
std::vector<DirItem> list_dir(const fs::path& p, bool* exists) {
    std::vector<DirItem> items;
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        if (exists) *exists = false;
        return items;
    }
    if (exists) *exists = true;
    for (fs::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec)) {
        DirItem d;
        d.name = it->path().filename().string();
        std::error_code e2;
        d.is_dir = it->is_directory(e2);
        if (!d.is_dir) {
            d.size = it->file_size(e2);
            if (e2) d.size = 0;
        }
        d.mtime = it->last_write_time(e2);
        items.push_back(std::move(d));
    }
    std::sort(items.begin(), items.end(),
              [](const DirItem& a, const DirItem& b) { return a.name < b.name; });
    return items;
}

uint64_t recursive_size(const fs::path& p) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) {
            uint64_t s = it->file_size(e2);
            if (!e2) total += s;
        }
    }
    return total;
}

// time_t + microseconds for a host file time.
struct WallTime {
    std::time_t secs = 0;
    uint32_t micros = 0;
};

WallTime to_wall(fs::file_time_type ft) {
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    auto since = sys.time_since_epoch();
    auto s = duration_cast<seconds>(since);
    auto us = duration_cast<microseconds>(since - s);
    if (us.count() < 0) {
        us += seconds(1);
        s -= seconds(1);
    }
    WallTime w;
    w.secs = static_cast<std::time_t>(s.count());
    w.micros = static_cast<uint32_t>(us.count());
    return w;
}

// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond.
void write_psp_datetime(const Mem& m, uint32_t addr, fs::file_time_type ft) {
    uint8_t* p = m.range(addr, 16);
    if (p == nullptr) return;
    WallTime w = to_wall(ft);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &w.secs);
#else
    localtime_r(&w.secs, &t);
#endif
    uint16_t f[6] = {
        static_cast<uint16_t>(t.tm_year + 1900),
        static_cast<uint16_t>(t.tm_mon + 1),
        static_cast<uint16_t>(t.tm_mday),
        static_cast<uint16_t>(t.tm_hour),
        static_cast<uint16_t>(t.tm_min),
        static_cast<uint16_t>(t.tm_sec),
    };
    std::memcpy(p, f, sizeof(f));
    std::memcpy(p + 12, &w.micros, 4);
}

std::string space_text(uint64_t size, bool round_up) {
    static const char* const suffixes[] = {"B", "KB", "MB", "GB"};
    char text[50];
    for (size_t i = 0; i < 4; i++) {
        if (size < 1024) {
            std::snprintf(text, sizeof(text), "%llu %s",
                          static_cast<unsigned long long>(size), suffixes[i]);
            return text;
        }
        size = round_up ? (size + 1023) / 1024 : size / 1024;
    }
    std::snprintf(text, sizeof(text), "%llu TB",
                  static_cast<unsigned long long>(size));
    return text;
}

uint64_t size_normalized(uint64_t size) {
    return (size + kSectorSize - 1) / kSectorSize * kSectorSize;
}

// Wildcard match used by LIST ('*' and '?'), PPSSPP PSPMatch.
bool psp_match(const std::string& text, size_t ti, const std::string& pat,
               size_t pi) {
    if (ti == text.size() && pi == pat.size()) return true;
    if (pi < pat.size() && pat[pi] == '*') {
        if (pi + 1 == pat.size()) return true;
        for (size_t k = ti; k <= text.size(); k++) {
            if (psp_match(text, k, pat, pi + 1)) return true;
        }
        return false;
    }
    if (ti == text.size() || pi == pat.size()) return false;
    if (pat[pi] == '?' || pat[pi] == text[ti]) {
        return psp_match(text, ti + 1, pat, pi + 1);
    }
    return false;
}

bool has_path_traversal(const std::string& s) {
    if (s == "." || s == "..") return true;
    return s.find_first_of("/\\:") != std::string::npos;
}

const char* mode_name(uint32_t mode) {
    static const char* const names[] = {
        "AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE",
        "LISTDELETE", "LISTALLDELETE", "SIZES", "AUTODELETE", "DELETE", "LIST",
        "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
        "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA",
        "GETSIZE",
    };
    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "UNKNOWN";
}

// ======================================================================
// Operations
// ======================================================================

struct FileData {
    uint32_t buf = 0;
    uint32_t buf_size = 0;
    uint32_t size = 0;
};

class Savedata {
public:
    Savedata(const Mem& mem, uint32_t param_addr, uint32_t param_size,
             const Options& opts)
        : m_(mem), p_(param_addr), psize_(param_size), opts_(opts) {
        root_ = opts.root.empty() ? default_root() : opts.root;
        game_ = field_str(off::kGameName, 13);
        save_ = field_str(off::kSaveName, 20);
        if (save_ == "<>") save_.clear();
        file_ = field_str(off::kFileName, 13);
        mode_ = field32(off::kMode);
    }

    // Do the names form safe path components?
    bool names_ok() const {
        return !has_path_traversal(game_) && !has_path_traversal(save_) &&
               !has_path_traversal(file_);
    }

    uint32_t mode() const { return mode_; }
    const std::string& game() const { return game_; }
    const std::string& used_save() const { return used_save_; }

    int32_t run() {
        used_save_ = save_;
        switch (mode_) {
            case MODE_AUTOLOAD:
            case MODE_LOAD:
                return load(save_, true, false);
            case MODE_AUTOSAVE:
            case MODE_SAVE:
                return save(save_, true);
            case MODE_LISTLOAD:
                return list_load();
            case MODE_LISTSAVE:
                return list_save();
            case MODE_SIZES:
                return sizes();
            case MODE_LIST:
                list();
                return 0;
            case MODE_FILES:
                return files_list();
            case MODE_MAKEDATASECURE:
            case MODE_MAKEDATA: {
                int32_t r = save(save_, mode_ == MODE_MAKEDATASECURE);
                if (r == static_cast<int32_t>(ERR_SAVE_MS_NOSPACE)) {
                    r = static_cast<int32_t>(ERR_RW_MEMSTICK_FULL);
                }
                return r;
            }
            case MODE_WRITEDATASECURE:
            case MODE_WRITEDATA:
                return save(save_, mode_ == MODE_WRITEDATASECURE);
            case MODE_READDATASECURE:
            case MODE_READDATA: {
                int32_t r = load(save_, mode_ == MODE_READDATASECURE, true);
                if (r == static_cast<int32_t>(ERR_LOAD_DATA_BROKEN)) {
                    r = static_cast<int32_t>(ERR_RW_DATA_BROKEN);
                }
                if (r == static_cast<int32_t>(ERR_LOAD_NO_DATA)) {
                    r = static_cast<int32_t>(ERR_RW_NO_DATA);
                }
                return r;
            }
            case MODE_GETSIZE:
                return get_size() ? 0 : static_cast<int32_t>(ERR_RW_NO_DATA);
            default:
                return 0;
        }
    }

private:
    // ---- param field access (fields past the caller's struct size read 0) --
    bool in_param(uint32_t off, uint32_t len) const {
        return uint64_t{off} + len <= psize_;
    }
    uint32_t field32(uint32_t off) const {
        return in_param(off, 4) ? m_.rd32(p_ + off) : 0;
    }
    void set_field32(uint32_t off, uint32_t v) const {
        if (in_param(off, 4)) m_.wr32(p_ + off, v);
    }
    std::string field_str(uint32_t off, size_t cap) const {
        return in_param(off, static_cast<uint32_t>(cap))
                   ? m_.str(p_ + off, cap)
                   : std::string();
    }
    void set_field_str(uint32_t off, size_t cap, const std::string& s) const {
        if (in_param(off, static_cast<uint32_t>(cap))) {
            m_.wstr(p_ + off, cap, s);
        }
    }
    FileData file_data(uint32_t off) const {
        FileData d;
        d.buf = field32(off + off::kFdBuf);
        d.buf_size = field32(off + off::kFdBufSize);
        d.size = field32(off + off::kFdSize);
        return d;
    }

    // ---- paths ------------------------------------------------------------
    fs::path root() const { return fs::path(root_); }
    std::string dir_name(const std::string& save_dir) const {
        return game_ + save_dir;
    }
    fs::path save_path(const std::string& save_dir) const {
        return root() / dir_name(save_dir);
    }

    // ---- secure-version gates (SavedataParam::MissingRequiredKey) ---------
    bool has_key() const {
        if (!in_param(off::kKey, 16)) return false;
        const uint8_t* k = m_.range(p_ + off::kKey, 16);
        if (k == nullptr) return false;
        for (int i = 0; i < 16; i++) {
            if (k[i] != 0) return true;
        }
        return false;
    }
    bool missing_required_key() const {
        return psize_ >= off::kParamSize && field32(off::kSecureVersion) != 1 &&
               !has_key();
    }

    // ---- Save -------------------------------------------------------------
    int32_t save(const std::string& save_dir, bool secure) {
        used_save_ = save_dir;
        uint32_t data_size = field32(off::kDataSize);
        uint32_t buf_size = field32(off::kDataBufSize);
        if (data_size > buf_size) return ERR_RW_BAD_PARAMS;

        FileData icon0 = file_data(off::kIcon0);
        FileData icon1 = file_data(off::kIcon1);
        FileData pic1 = file_data(off::kPic1);
        FileData snd0 = file_data(off::kSnd0);
        for (const FileData* fd : {&icon0, &icon1, &pic1, &snd0}) {
            if (fd->buf != 0 && fd->buf_size < fd->size) {
                return ERR_RW_BAD_PARAMS;
            }
        }

        uint32_t secure_version = field32(off::kSecureVersion);
        if (psize_ >= off::kParamSize && secure_version > 3) {
            return ERR_SAVE_PARAM;
        }
        if (secure && missing_required_key()) return ERR_SAVE_PARAM;

        if (dir_name(save_dir).empty() || game_.empty()) return ERR_SAVE_PARAM;

        // Resolve every guest buffer before touching the disk so a bad
        // pointer can never leave a half-written save behind.
        uint32_t data_buf = field32(off::kDataBuf);
        const uint8_t* data = nullptr;
        uint32_t save_size = 0;
        if (data_buf != 0) {
            save_size = (data_size == 0 || data_size > buf_size) ? buf_size
                                                                 : data_size;
            data = m_.range(data_buf, save_size);
            if (data == nullptr) return ERR_SAVE_PARAM;
        }
        const uint8_t* icon0_p = nullptr;
        const uint8_t* icon1_p = nullptr;
        const uint8_t* pic1_p = nullptr;
        const uint8_t* snd0_p = nullptr;
        struct Sub {
            const FileData* fd;
            const uint8_t** out;
        } subs[] = {{&icon0, &icon0_p},
                    {&icon1, &icon1_p},
                    {&pic1, &pic1_p},
                    {&snd0, &snd0_p}};
        for (const Sub& s : subs) {
            if (s.fd->buf == 0) continue;
            *s.out = m_.range(s.fd->buf, s.fd->size);
            if (*s.out == nullptr) return ERR_RW_BAD_PARAMS;
        }

        fs::path dir = save_path(save_dir);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec || !is_dir(dir)) return ERR_SAVE_ACCESS_ERROR;

        // PARAM.SFO
        fs::path sfo_path = dir / kSfoName;
        Psf sfo;
        {
            std::vector<uint8_t> raw;
            if (read_file(sfo_path, 16 * 1024 * 1024, &raw)) {
                if (!Psf::parse(raw.data(), raw.size(), &sfo)) sfo = Psf();
            }
        }
        if (!field_str(off::kSfoTitle, 0x80).empty()) {
            sfo.set_string("TITLE", field_str(off::kSfoTitle, 0x80), 128);
            sfo.set_string("SAVEDATA_TITLE",
                           field_str(off::kSfoSavedataTitle, 0x80), 128);
            sfo.set_string("SAVEDATA_DETAIL", field_str(off::kSfoDetail, 0x400),
                           1024);
            uint32_t level = 0;
            if (in_param(off::kSfoParentalLevel, 1)) {
                const uint8_t* lp = m_.range(p_ + off::kSfoParentalLevel, 1);
                if (lp != nullptr) level = *lp;
            }
            sfo.set_int("PARENTAL_LEVEL", static_cast<int32_t>(level));
            sfo.set_string("CATEGORY", "MS", 4);
            sfo.set_string("SAVEDATA_DIRECTORY", dir_name(save_dir), 64);
        }

        // SAVEDATA_FILE_LIST: name[13], hash[16] (zero: no encryption), pad[3].
        std::vector<uint8_t> list(kFileListTotalSize, 0);
        if (const std::vector<uint8_t>* old = sfo.get_binary("SAVEDATA_FILE_LIST")) {
            std::memcpy(list.data(), old->data(),
                        std::min<size_t>(old->size(), list.size()));
        }
        if (secure && data_buf != 0 && !file_.empty()) {
            for (uint32_t i = 0; i < kFileListCountMax; i++) {
                uint8_t* e = list.data() + i * kFileListEntrySize;
                if (e[0] != 0 &&
                    std::strncmp(reinterpret_cast<const char*>(e), file_.c_str(),
                                 13) != 0) {
                    continue;
                }
                std::memset(e, 0, kFileListEntrySize);
                std::memcpy(e, file_.c_str(), std::min<size_t>(file_.size(), 12));
                break;
            }
        }
        sfo.set_binary("SAVEDATA_FILE_LIST", list.data(), list.size(),
                       kFileListTotalSize);
        // All-zero params mark the save as not encrypted.
        uint8_t zeroes[128] = {};
        sfo.set_binary("SAVEDATA_PARAMS", zeroes, sizeof(zeroes), 128);

        std::vector<uint8_t> sfo_bytes = sfo.serialize();
        if (!write_file_atomic(sfo_path, sfo_bytes.data(), sfo_bytes.size())) {
            return ERR_SAVE_ACCESS_ERROR;
        }

        if (data_buf != 0) {
            // Copy the chosen save name back into the request.
            set_field_str(off::kSaveName, 20, save_dir);
            if (!file_.empty() &&
                !write_file_atomic(dir / file_, data, save_size)) {
                return ERR_SAVE_MS_NOSPACE;
            }
        }

        if (icon0_p != nullptr) {
            write_file_atomic(dir / kIcon0Name, icon0_p, icon0.size);
        }
        if (icon1_p != nullptr && icon1.size > 0) {
            write_file_atomic(dir / kIcon1Name, icon1_p, icon1.size);
        }
        if (pic1_p != nullptr) {
            write_file_atomic(dir / kPic1Name, pic1_p, pic1.size);
        }
        if (snd0_p != nullptr && snd0.size > 0) {
            write_file_atomic(dir / kSnd0Name, snd0_p, snd0.size);
        }
        return 0;
    }

    // ---- Load -------------------------------------------------------------
    void load_sub_file(const fs::path& dir, const char* name,
                       uint32_t fd_off) const {
        uint32_t buf = field32(fd_off + off::kFdBuf);
        if (buf == 0) return;
        uint32_t want = field32(fd_off + off::kFdBufSize);
        // Clamp to the part of the buffer that is inside guest RAM.
        uint64_t moff = buf & 0x07FFFFFFu;
        if (moff >= m_.size) return;
        want = static_cast<uint32_t>(std::min<uint64_t>(want, m_.size - moff));
        std::vector<uint8_t> bytes;
        if (!read_file(dir / name, want, &bytes) || bytes.empty()) return;
        uint8_t* dst = m_.range(buf, bytes.size());
        if (dst == nullptr) return;
        std::memcpy(dst, bytes.data(), bytes.size());
        set_field32(fd_off + off::kFdSize, static_cast<uint32_t>(bytes.size()));
    }

    int32_t load(const std::string& save_dir, bool secure, bool is_rw) {
        used_save_ = save_dir;
        const int32_t no_data = static_cast<int32_t>(
            is_rw ? ERR_RW_NO_DATA : ERR_LOAD_NO_DATA);
        if (save_dir.empty() && game_.empty()) return no_data;
        fs::path dir = save_path(save_dir);
        if (!is_dir(dir)) return no_data;

        fs::path file_path = dir / file_;
        std::error_code ec;
        if (!file_.empty() && !fs::is_regular_file(file_path, ec)) {
            return static_cast<int32_t>(is_rw ? ERR_RW_FILE_NOT_FOUND
                                              : ERR_LOAD_FILE_NOT_FOUND);
        }

        // Forced to zero before loading, in particular in case of error.
        set_field32(off::kDataSize, 0);

        // LoadSaveData
        if (psize_ >= off::kParamSize && field32(off::kSecureVersion) > 3) {
            return ERR_LOAD_PARAM;
        }
        if (secure && missing_required_key()) return ERR_LOAD_PARAM;

        if (!file_.empty()) {
            uint64_t fsize = fs::file_size(file_path, ec);
            if (ec || fsize == 0) return ERR_LOAD_NO_DATA;  // nothing to read
            set_field_str(off::kSaveName, 20, save_dir);

            uint32_t data_buf = field32(off::kDataBuf);
            uint32_t buf_size = field32(off::kDataBufSize);
            uint8_t* dst = nullptr;
            if (data_buf != 0) {
                if (fsize > buf_size) return ERR_LOAD_DATA_BROKEN;
                dst = m_.range(data_buf, fsize);
                if (dst == nullptr) return ERR_LOAD_DATA_BROKEN;
            }
            std::vector<uint8_t> bytes;
            if (!read_file(file_path, fsize, &bytes) || bytes.empty()) {
                return ERR_LOAD_NO_DATA;
            }
            if (dst != nullptr) std::memcpy(dst, bytes.data(), bytes.size());
            set_field32(off::kDataSize, static_cast<uint32_t>(bytes.size()));
        }

        // LoadSFO
        Psf sfo;
        std::vector<uint8_t> raw;
        if (!read_file(dir / kSfoName, 16 * 1024 * 1024, &raw) || raw.empty() ||
            !Psf::parse(raw.data(), raw.size(), &sfo)) {
            return static_cast<int32_t>(is_rw ? ERR_RW_DATA_BROKEN
                                              : ERR_LOAD_DATA_BROKEN);
        }
        set_field_str(off::kSfoTitle, 0x80, sfo.get_string("TITLE"));
        set_field_str(off::kSfoSavedataTitle, 0x80,
                      sfo.get_string("SAVEDATA_TITLE"));
        set_field_str(off::kSfoDetail, 0x400, sfo.get_string("SAVEDATA_DETAIL"));
        if (in_param(off::kSfoParentalLevel, 1)) {
            uint8_t* lp = m_.range(p_ + off::kSfoParentalLevel, 1);
            if (lp != nullptr) {
                *lp = static_cast<uint8_t>(sfo.get_int("PARENTAL_LEVEL"));
            }
        }

        // Unknown to PPSSPP too, but a real PSP always answers this.
        set_field32(off::kBind, 1021);

        load_sub_file(dir, kIcon0Name, off::kIcon0);
        load_sub_file(dir, kIcon1Name, off::kIcon1);
        load_sub_file(dir, kPic1Name, off::kPic1);
        load_sub_file(dir, kSnd0Name, off::kSnd0);
        return 0;
    }

    // ---- save lists (LISTLOAD / LISTSAVE) ---------------------------------
    struct ListItem {
        std::string name;
        bool exists = false;
        uint64_t size = 0;
        fs::file_time_type mtime{};
    };

    // PPSSPP SavedataParam::GetSaveInfo: total size of the files, time of
    // PARAM.SFO (else the first file).
    ListItem save_info(const std::string& save_dir) const {
        ListItem it;
        it.name = save_dir;
        fs::path dir = save_path(save_dir);
        bool exists = false;
        std::vector<DirItem> files = list_dir(dir, &exists);
        it.exists = exists;
        if (!exists) return it;
        std::error_code ec;
        it.mtime = fs::last_write_time(dir, ec);
        bool first = true;
        for (const DirItem& f : files) {
            if (f.is_dir) continue;
            if (first || f.name == kSfoName) {
                it.mtime = f.mtime;
                first = false;
            }
            it.size += f.size;
        }
        return it;
    }

    // Reads saveNameList; false if it is absent or empty (single-save mode).
    // `bad` is set when an entry is not a safe path component.
    bool read_name_list(std::vector<std::string>* names, bool* bad) const {
        uint32_t list = field32(off::kSaveNameList);
        if (list == 0) return false;
        for (uint32_t i = 0; i < 4096; i++) {
            uint32_t a = list + i * 20;
            if (!m_.valid(a, 20)) break;
            std::string n = m_.str(a, 20);
            if (n.empty()) break;
            if (has_path_traversal(n)) {
                *bad = true;
                return false;
            }
            names->push_back(std::move(n));
        }
        return !names->empty();
    }

    // Builds the list the dialog would show. With `list_empty` (save modes)
    // names that have no data are listed too, otherwise only existing saves.
    bool build_list(bool list_empty, std::vector<ListItem>* items) const {
        std::vector<std::string> names;
        bool bad = false;
        if (!read_name_list(&names, &bad)) return false;
        bool dir_exists = false;
        std::vector<DirItem> all = list_dir(root(), &dir_exists);
        for (const std::string& n : names) {
            if (n == "<>") {
                // Wildcard: the first existing save of this game not listed.
                for (const DirItem& d : all) {
                    if (!d.is_dir || d.name.compare(0, game_.size(), game_) != 0) {
                        continue;
                    }
                    std::string sn = d.name.substr(game_.size());
                    bool dup = false;
                    for (const ListItem& li : *items) dup = dup || li.name == sn;
                    if (dup) continue;
                    items->push_back(save_info(sn));
                    break;
                }
                continue;
            }
            ListItem li = save_info(n);
            if (li.exists || list_empty) items->push_back(std::move(li));
        }
        return true;
    }

    // PPSSPP SavedataParam::Get{First,Last,Latest,Oldest,...}Save.
    size_t pick_by_focus(const std::vector<ListItem>& items) const {
        size_t n = items.size();
        switch (field32(off::kFocus)) {
            case FOCUS_NAME:
                for (size_t i = 0; i < n; i++) {
                    if (items[i].name == save_) return i;
                }
                return 0;
            case FOCUS_FIRSTLIST:
                return 0;
            case FOCUS_LASTLIST:
                return n - 1;
            case FOCUS_LATEST: {
                size_t idx = 0;
                bool have = false;
                for (size_t i = 0; i < n; i++) {
                    if (items[i].size == 0) continue;
                    if (!have || items[i].mtime > items[idx].mtime) {
                        idx = i;
                        have = true;
                    }
                }
                return idx;
            }
            case FOCUS_OLDEST: {
                size_t idx = 0;
                bool have = false;
                for (size_t i = 0; i < n; i++) {
                    if (items[i].size == 0) continue;
                    if (!have || items[i].mtime < items[idx].mtime) {
                        idx = i;
                        have = true;
                    }
                }
                return idx;
            }
            case FOCUS_FIRSTDATA:
                for (size_t i = 0; i < n; i++) {
                    if (items[i].size != 0) return i;
                }
                return 0;
            case FOCUS_LASTDATA:
                for (size_t i = n; i > 0; i--) {
                    if (items[i - 1].size != 0) return i - 1;
                }
                return 0;
            case FOCUS_FIRSTEMPTY:
                for (size_t i = 0; i < n; i++) {
                    if (items[i].size == 0) return i;
                }
                return 0;
            case FOCUS_LASTEMPTY:
                for (size_t i = n; i > 0; i--) {
                    if (items[i - 1].size == 0) return i - 1;
                }
                return 0;
            default:
                return 0;
        }
    }

    // No UI: load the newest existing save of the list.
    int32_t list_load() {
        std::vector<ListItem> items;
        if (!build_list(false, &items)) {
            // Single-save form: just the named save.
            return load(save_, true, false);
        }
        if (items.empty()) return ERR_LOAD_NO_DATA;
        size_t best = 0;
        for (size_t i = 1; i < items.size(); i++) {
            if (items[i].mtime > items[best].mtime) best = i;
        }
        return load(items[best].name, true, false);
    }

    // No UI: save into the entry the focus field selects.
    int32_t list_save() {
        std::vector<ListItem> items;
        if (!build_list(true, &items) || items.empty()) {
            return save(save_, true);
        }
        return save(items[pick_by_focus(items)].name, true);
    }

    // ---- SIZES / LIST / FILES / GETSIZE ------------------------------------
    uint64_t game_used_bytes() const {
        uint64_t used = 0;
        bool exists = false;
        for (const DirItem& d : list_dir(root(), &exists)) {
            if (d.is_dir && d.name.compare(0, game_.size(), game_) == 0) {
                used += recursive_size(root() / d.name);
            }
        }
        return used;
    }
    uint64_t free_bytes() const {
        uint64_t used = game_used_bytes();
        return used < kMemStickBytes ? kMemStickBytes - used : 0;
    }

    int32_t sizes() {
        int32_t ret = 0;
        uint32_t ms_free = field32(off::kMsFree);
        uint32_t ms_data = field32(off::kMsData);
        uint32_t utility = field32(off::kUtilityData);

        if (m_.valid(ms_free, off::kMsFreeSize)) {
            uint64_t fb = free_bytes();
            m_.wr32(ms_free + off::kMsFreeClusterSize,
                    static_cast<uint32_t>(kSectorSize));
            m_.wr32(ms_free + off::kMsFreeFreeClusters,
                    static_cast<uint32_t>(fb / kSectorSize));
            m_.wr32(ms_free + off::kMsFreeFreeSpaceKB,
                    static_cast<uint32_t>(fb / 0x400));
            m_.wstr(ms_free + off::kMsFreeFreeSpaceStr, 8,
                    space_text(fb, false));
        }
        if (m_.valid(ms_data, off::kMsDataSize)) {
            std::string g = m_.str(ms_data + off::kMsDataGameName, 13);
            std::string s = m_.str(ms_data + off::kMsDataSaveName, 20);
            uint32_t info = ms_data + off::kMsDataInfo;
            bool bad = has_path_traversal(g) || has_path_traversal(s);
            bool exists = false;
            std::vector<DirItem> listing;
            if (!bad) {
                listing = list_dir(root() / (g + (s == "<>" ? "" : s)), &exists);
            }
            if (exists) {
                uint64_t clusters = 0;
                for (const DirItem& d : listing) {
                    clusters += (d.size + kSectorSize - 1) / kSectorSize;
                }
                uint64_t total = clusters * kSectorSize;
                m_.wr32(info + off::kUsedClusters,
                        static_cast<uint32_t>(clusters));
                m_.wr32(info + off::kUsedSpaceKB,
                        static_cast<uint32_t>(total / 0x400));
                m_.wstr(info + off::kUsedSpaceStr, 8, space_text(total, true));
                m_.wr32(info + off::kUsedSpace32KB,
                        static_cast<uint32_t>(total / 0x400));
                m_.wstr(info + off::kUsedSpace32Str, 8, space_text(total, true));
            } else {
                m_.wr32(info + off::kUsedClusters, 0);
                m_.wr32(info + off::kUsedSpaceKB, 0);
                m_.wstr(info + off::kUsedSpaceStr, 8, "");
                m_.wr32(info + off::kUsedSpace32KB, 0);
                m_.wstr(info + off::kUsedSpace32Str, 8, "");
                ret = static_cast<int32_t>(ERR_SIZES_NO_DATA);
            }
        }
        if (m_.valid(utility, off::kUsedSize)) {
            uint64_t total = 0;
            total += size_normalized(1);  // the directory record
            total += size_normalized(1);  // PARAM.SFO
            if (!file_.empty()) {
                // +16: the encryption overhead real hardware (and PPSSPP's
                // default configuration) accounts for.
                total += size_normalized(uint64_t{field32(off::kDataSize)} + 16);
            }
            total += size_normalized(file_data(off::kIcon0).size);
            total += size_normalized(file_data(off::kIcon1).size);
            total += size_normalized(file_data(off::kPic1).size);
            total += size_normalized(file_data(off::kSnd0).size);
            std::string txt = space_text(total, true);
            m_.wr32(utility + off::kUsedClusters,
                    static_cast<uint32_t>(total / kSectorSize));
            m_.wr32(utility + off::kUsedSpaceKB,
                    static_cast<uint32_t>(total / 0x400));
            m_.wstr(utility + off::kUsedSpaceStr, 8, txt);
            m_.wr32(utility + off::kUsedSpace32KB,
                    static_cast<uint32_t>(total / 0x400));
            m_.wstr(utility + off::kUsedSpace32Str, 8, txt);
        }
        return ret;
    }

    void list() {
        uint32_t id_list = field32(off::kIdList);
        if (!m_.valid(id_list, 12)) return;
        uint32_t max_count = m_.rd32(id_list + off::kIdMaxCount);
        uint32_t entries = m_.rd32(id_list + off::kIdEntries);

        std::string pattern = game_ + save_;
        bool exists = false;
        std::vector<DirItem> matches;
        for (const DirItem& d : list_dir(root(), &exists)) {
            if (matches.size() >= max_count) break;
            if (d.is_dir && psp_match(d.name, 0, pattern, 0)) {
                matches.push_back(d);
            }
        }
        // Only write the entries that fit in guest RAM.
        uint32_t count = 0;
        for (const DirItem& d : matches) {
            uint32_t e = entries + count * off::kIdEntrySize;
            if (entries == 0 || !m_.valid(e, off::kIdEntrySize)) break;
            // The date comes from PARAM.SFO when present.
            fs::file_time_type t = d.mtime;
            std::error_code ec;
            fs::file_time_type st = fs::last_write_time(
                root() / d.name / kSfoName, ec);
            if (!ec) t = st;
            m_.wr32(e + off::kIdEntryMode, 0x11FF);
            write_psp_datetime(m_, e + off::kIdEntryCtime, t);
            write_psp_datetime(m_, e + off::kIdEntryAtime, t);
            write_psp_datetime(m_, e + off::kIdEntryMtime, t);
            // Folder name without the game name (max 20 bytes).
            m_.wstr(e + off::kIdEntryName, 20, d.name.substr(game_.size()));
            count++;
        }
        m_.wr32(id_list + off::kIdResultCount, count);
    }

    // Secure file names recorded in the SFO's SAVEDATA_FILE_LIST.
    static bool secure_names(const Psf& sfo, std::vector<std::string>* out) {
        const std::vector<uint8_t>* list = sfo.get_binary("SAVEDATA_FILE_LIST");
        if (list == nullptr) return true;
        uint32_t count = std::min<uint32_t>(
            kFileListCountMax,
            static_cast<uint32_t>(list->size()) / kFileListEntrySize);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = list->data() + i * kFileListEntrySize;
            if (e[0] == 0) continue;
            size_t n = 0;
            while (n < 13 && e[n] != 0) n++;
            out->emplace_back(reinterpret_cast<const char*>(e), n);
        }
        return true;
    }

    int32_t files_list() {
        uint32_t fl = field32(off::kFileList);
        if (!m_.valid(fl, 36)) return -1;  // PPSSPP: "Should crash."

        uint32_t max_secure = m_.rd32(fl + off::kFlMaxSecure);
        uint32_t max_normal = m_.rd32(fl + off::kFlMaxNormal);
        uint32_t max_system = m_.rd32(fl + off::kFlMaxSystem);
        uint32_t secure_ptr = m_.rd32(fl + off::kFlSecureEntries);
        uint32_t normal_ptr = m_.rd32(fl + off::kFlNormalEntries);
        uint32_t system_ptr = m_.rd32(fl + off::kFlSystemEntries);

        // An entry array is usable only if its pointer is valid.
        auto usable = [&](uint32_t ptr) { return m_.valid(ptr, 4); };
        if (usable(secure_ptr) && max_secure > 99) return ERR_RW_BAD_PARAMS;
        if (usable(normal_ptr) && max_normal > 8192) return ERR_RW_BAD_PARAMS;
        if (opts_.sdk_version >= 0x02060000u && usable(system_ptr) &&
            max_system > 5) {
            return ERR_RW_BAD_PARAMS;
        }

        fs::path dir = save_path(save_);
        bool exists = false;
        std::vector<DirItem> files = list_dir(dir, &exists);
        if (!exists) return ERR_RW_NO_DATA;

        // Even if there are no files, initialize to 0.
        uint32_t n_secure = 0, n_normal = 0, n_system = 0;
        m_.wr32(fl + off::kFlResultSecure, 0);
        m_.wr32(fl + off::kFlResultNormal, 0);
        m_.wr32(fl + off::kFlResultSystem, 0);

        // The SFO's SAVEDATA_FILE_LIST says which entries are secure.
        Psf sfo;
        std::vector<uint8_t> raw;
        if (!read_file(dir / kSfoName, 16 * 1024 * 1024, &raw) || raw.empty() ||
            !Psf::parse(raw.data(), raw.size(), &sfo)) {
            return ERR_RW_DATA_BROKEN;
        }
        std::vector<std::string> secure;
        secure_names(sfo, &secure);

        set_field32(off::kBind, 1021);

        // Does not list directories, nor recurse into them, and ignores
        // files that are not ALL UPPERCASE.
        for (const DirItem& f : files) {
            if (f.is_dir) continue;
            if (f.name.find_first_of("abcdefghijklmnopqrstuvwxyz") !=
                std::string::npos) {
                continue;
            }
            bool is_system = f.name == kIcon0Name || f.name == kIcon1Name ||
                             f.name == kPic1Name || f.name == kSnd0Name ||
                             f.name == kSfoName;
            // Pick the array this file belongs to; a file that does not fit
            // (full, absent, or outside guest RAM) is simply not listed.
            uint32_t* count = &n_normal;
            uint32_t max = max_normal;
            uint32_t ptr = normal_ptr;
            if (is_system) {
                count = &n_system;
                max = max_system;
                ptr = system_ptr;
            } else if (std::find(secure.begin(), secure.end(), f.name) !=
                       secure.end()) {
                count = &n_secure;
                max = max_secure;
                ptr = secure_ptr;
            }
            if (!usable(ptr) || *count >= max) continue;
            uint32_t entry = ptr + *count * off::kFlEntryBytes;
            if (!m_.valid(entry, off::kFlEntryBytes)) continue;
            (*count)++;
            m_.wr32(entry + off::kFlEntryMode, 0x21FF);
            m_.wr32(entry + 4, 0);
            m_.wr64(entry + off::kFlEntrySize64, f.size);
            write_psp_datetime(m_, entry + off::kFlEntryCtime, f.mtime);
            write_psp_datetime(m_, entry + off::kFlEntryAtime, f.mtime);
            write_psp_datetime(m_, entry + off::kFlEntryMtime, f.mtime);
            m_.wstr(entry + off::kFlEntryName, 16, f.name);
            m_.range(entry + off::kFlEntryName + 15, 1)[0] = 0;
        }
        m_.wr32(fl + off::kFlResultSecure, n_secure);
        m_.wr32(fl + off::kFlResultNormal, n_normal);
        m_.wr32(fl + off::kFlResultSystem, n_system);
        return 0;
    }

    // Returns whether the save directory exists.
    bool get_size() {
        fs::path dir = save_path(save_);
        bool exists = false;
        std::vector<DirItem> listing = list_dir(dir, &exists);

        uint32_t si = field32(off::kSizeInfo);
        if (!m_.valid(si, 60)) return exists;

        auto size_of = [&](const std::string& name) -> int64_t {
            for (const DirItem& d : listing) {
                if (d.name == name) return static_cast<int64_t>(d.size);
            }
            return 0;
        };
        int64_t overwrite = 0;
        int64_t write = 0;
        auto walk = [&](uint32_t count_off, uint32_t ptr_off, int64_t extra) {
            int32_t count = static_cast<int32_t>(m_.rd32(si + count_off));
            uint32_t ptr = m_.rd32(si + ptr_off);
            for (int32_t i = 0; i < count && i < 8192; i++) {
                uint32_t e = ptr + static_cast<uint32_t>(i) * off::kSiEntryBytes;
                if (!m_.valid(e, off::kSiEntryBytes)) break;
                overwrite += size_of(m_.str(e + off::kSiEntryName, 16));
                write += static_cast<int64_t>(m_.rd64(e + off::kSiEntrySize64)) +
                         extra;
            }
        };
        walk(off::kSiNumNormal, off::kSiNormalEntries, 0);
        walk(off::kSiNumSecure, off::kSiSecureEntries, 0x10);

        int64_t free_b = static_cast<int64_t>(free_bytes());
        m_.wr32(si + off::kSiSectorSize, static_cast<uint32_t>(kSectorSize));
        m_.wr32(si + off::kSiFreeSectors,
                static_cast<uint32_t>(free_b / static_cast<int64_t>(kSectorSize)));
        m_.wr32(si + off::kSiFreeKB, static_cast<uint32_t>(free_b / 1024));
        m_.wstr(si + off::kSiFreeString, 8,
                space_text(static_cast<uint64_t>(free_b), false));

        if (write - overwrite < free_b) {
            m_.wr32(si + off::kSiNeededKB, 0);
            // Note: this is "needed to overwrite". The strings are left alone
            // when nothing is needed.
            m_.wr32(si + off::kSiOverwriteKB, 0);
        } else {
            int64_t needed = write - free_b;
            m_.wr32(si + off::kSiNeededKB,
                    static_cast<uint32_t>((needed + 1023) / 1024));
            m_.wstr(si + off::kSiNeededString, 8,
                    space_text(static_cast<uint64_t>(needed), true));
            int64_t needed_ow = write - free_b - overwrite;
            m_.wr32(si + off::kSiOverwriteKB,
                    static_cast<uint32_t>((needed_ow + 1023) / 1024));
            m_.wstr(si + off::kSiOverwriteString, 8,
                    space_text(static_cast<uint64_t>(std::max<int64_t>(needed_ow, 0)),
                               true));
        }
        return exists;
    }

    Mem m_;
    uint32_t p_;
    uint32_t psize_;
    Options opts_;
    std::string root_;
    std::string game_, save_, file_;
    std::string used_save_;
    uint32_t mode_ = 0;
};

constexpr uint32_t kSizeV1 = 1480;
constexpr uint32_t kSizeV2 = 1500;
constexpr uint32_t kSizeV3 = 1536;

std::string describe(uint32_t mode, const std::string& game,
                     const std::string& save, int32_t result) {
    char head[96];
    std::snprintf(head, sizeof(head), "mode=%s(%u) ", mode_name(mode), mode);
    char tail[32];
    std::snprintf(tail, sizeof(tail), " result=0x%08X",
                  static_cast<uint32_t>(result));
    return std::string(head) + "game=" + game + " save=" + save + tail;
}

}  // namespace

// ======================================================================
// Public entry points
// ======================================================================

bool handles_mode(uint32_t mode) {
    switch (mode) {
        case MODE_AUTOLOAD:
        case MODE_AUTOSAVE:
        case MODE_LOAD:
        case MODE_SAVE:
        case MODE_LISTLOAD:
        case MODE_LISTSAVE:
        case MODE_SIZES:
        case MODE_LIST:
        case MODE_FILES:
        case MODE_MAKEDATASECURE:
        case MODE_MAKEDATA:
        case MODE_READDATASECURE:
        case MODE_READDATA:
        case MODE_WRITEDATASECURE:
        case MODE_WRITEDATA:
        case MODE_GETSIZE:
            return true;
        default:
            return false;
    }
}

std::string default_root() {
    const char* env = std::getenv("PSPRECOMP_SAVEDATA");
    if (env != nullptr && env[0] != '\0') return env;
    return "SAVEDATA";
}

Outcome execute(uint8_t* rdram, size_t ram_size, uint32_t param_addr,
                const Options& opts) {
    Outcome out;
    Mem mem{rdram, ram_size};

    auto finish = [&](int32_t result, uint32_t mode, const std::string& game,
                      const std::string& save) {
        out.result = result;
        out.summary = describe(mode, game, save, result);
        // pspUtilityDialogCommon.result lives at +28 of the common header.
        if (mem.valid(param_addr, off::kMode)) {
            mem.wr32(param_addr + off::kResult, static_cast<uint32_t>(result));
        }
        return out;
    };

    if (!mem.valid(param_addr, off::kMode + 4)) {
        return finish(static_cast<int32_t>(ERR_UTILITY_INVALID_ADDRESS), 0xFFFF,
                      "", "");
    }
    uint32_t size = mem.rd32(param_addr + off::kSize);
    uint32_t mode = mem.rd32(param_addr + off::kMode);
    if (size != kSizeV1 && size != kSizeV2 && size != kSizeV3) {
        return finish(static_cast<int32_t>(ERR_UTILITY_INVALID_PARAM_SIZE), mode,
                      "", "");
    }
    if (!mem.valid(param_addr, size)) {
        return finish(static_cast<int32_t>(ERR_UTILITY_INVALID_ADDRESS), mode, "",
                      "");
    }

    Savedata sd(mem, param_addr, size, opts);
    if (!sd.names_ok()) {
        return finish(static_cast<int32_t>(ERR_UTILITY_INVALID_PARAM_SIZE), mode,
                      sd.game(), "");
    }
    int32_t result = sd.run();
    return finish(result, mode, sd.game(), sd.used_save());
}

}  // namespace psp_savedata
