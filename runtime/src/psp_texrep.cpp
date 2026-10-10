#include "psp_texrep.h"

#include "psp_memory.h"
#include "psp_texrep_png.h"
#include "psp_texrep_queue.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
GeClutSnapshot g_clut{};
TexrepPack g_pack;
bool g_enabled = false;
bool g_dump = false;
std::string g_dir;
std::unordered_set<std::string> g_logged;  // DUMP: keys already printed

// Pack images decode on a worker thread (TexrepImageQueue); created only when
// a pack is loaded. 256 MiB LRU byte budget.
std::unique_ptr<TexrepImageQueue> g_images;
constexpr size_t kImageBudget = 256u << 20;

bool decode_file(const std::string& rel, int* w, int* h, std::vector<uint8_t>* rgba) {
    const auto t0 = std::chrono::steady_clock::now();
    std::ifstream f(std::filesystem::u8path(g_dir + "/" + rel), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const bool ok = !bytes.empty() && ge_texrep_decode_png(bytes.data(), bytes.size(), w, h, rgba);
    if (!ok) {
        std::fprintf(stderr, "[TEXREP] cannot load %s (keeping the original texture)\n", rel.c_str());
    } else if (g_dump) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[TEXREP] loaded %s %dx%d in %.1f ms\n", rel.c_str(), *w, *h, ms);
    }
    return ok;
}
}  // namespace

void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data) {
    const uint32_t bytes = ge_clut_load_bytes(loadclut_data);
    const uint32_t off = clut_addr & PSP_ADDR_MASK;
    const bool valid = clut_addr != 0 && static_cast<uint64_t>(off) + bytes <= PSP_MEM_SIZE;
    ge_clut_snapshot_load(g_clut, valid ? rdram + off : nullptr, bytes);
}

const GeClutSnapshot& ge_texrep_clut() { return g_clut; }

void ge_texrep_init() {
    g_enabled = false;
    g_logged.clear();
    const char* dir = std::getenv("PSPRECOMP_TEXTURES");
    if (!dir || !dir[0]) return;
    const char* dump = std::getenv("PSPRECOMP_TEXTURES_DUMP");
    g_dump = dump && dump[0] && dump[0] != '0';
    g_dir = dir;
    std::replace(g_dir.begin(), g_dir.end(), '\\', '/');
    while (!g_dir.empty() && g_dir.back() == '/') g_dir.pop_back();
    std::ifstream f(std::filesystem::u8path(g_dir + "/textures.ini"), std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[TEXREP] disabled: no textures.ini in %s\n", g_dir.c_str());
        return;
    }
    std::stringstream text;
    text << f.rdbuf();
    std::vector<std::string> root;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(g_dir), ec)) {
        if (e.is_regular_file(ec)) root.push_back(e.path().filename().u8string());
    }
    std::string err;
    if (!g_pack.load(text.str(), root, &err)) {
        std::fprintf(stderr, "[TEXREP] disabled: %s\n", err.c_str());
        return;
    }
    g_enabled = true;
    g_images = std::make_unique<TexrepImageQueue>(decode_file, kImageBudget);
    std::fprintf(stderr, "[TEXREP] pack %s: %zu textures, hash=%s ignoreAddress=%d reduceHash=%d\n",
                 g_dir.c_str(), g_pack.alias_count(), g_pack.xxh32() ? "xxh32" : "xxh64",
                 g_pack.ignore_address() ? 1 : 0, g_pack.reduce_hash() ? 1 : 0);
}

bool ge_texrep_enabled() { return g_enabled; }

GeTexrepKey ge_texrep_key(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v) {
    const GeTexrepParams p = ge_texrep_params(s.tex_addr[0], s.tex_bufw[0], s.tex_size[0],
                                              s.tex_format, s.tex_mode);
    int w = p.w, h = p.h;
    if (!g_pack.hash_range(p.addr, p.w, p.h, &w, &h)) {
        const bool through = (s.vertex_type & 0x00800000u) != 0;
        h = ge_texrep_hash_height(h, p.fmt, p.swizzled, ge_texrep_max_seen_v(through, draw_max_v));
    }
    const float reduce = g_pack.reduce_hash() ? g_pack.reduce_for(w, h) : 1.0f;
    const uint32_t off = p.addr & PSP_ADDR_MASK;
    const uint64_t avail = off < PSP_MEM_SIZE ? PSP_MEM_SIZE - off : 0;
    const uint32_t data = ge_texrep_data_hash(rdram + off, avail, p.bufw, w, h, p.fmt, reduce,
                                              g_pack.xxh32());
    const uint32_t cluthash =
        (p.fmt >= 4 && p.fmt <= 7) ? ge_texrep_cluthash(g_clut, s.clut_format) : 0;
    return {ge_texrep_cachekey(p.addr, p.fmt, p.dim, cluthash), data};
}

TexrepFind ge_texrep_lookup(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                            GeTexrepKey* key_out) {
    const GeTexrepKey key = ge_texrep_key(rdram, s, draw_max_v);
    if (key_out) *key_out = key;
    const TexrepFind f = g_pack.find(key);
    if (g_dump) {
        const std::string name = ge_texrep_key_name(key);
        if (g_logged.insert(name).second) {
            const GeTexrepParams p = ge_texrep_params(s.tex_addr[0], s.tex_bufw[0], s.tex_size[0],
                                                      s.tex_format, s.tex_mode);
            std::fprintf(stderr, "[TEXREP] key=%s %dx%d fmt=%d %s\n", name.c_str(), p.w, p.h, p.fmt,
                         f.ignored ? "ignored" : (f.found ? "hit" : "miss"));
        }
    }
    return f;
}

GeTexrepResult ge_texrep_replacement(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                                    GeTexrepImage* out) {
    if (!g_enabled) return GeTexrepResult::None;
    GeTexrepKey key{};
    const TexrepFind f = ge_texrep_lookup(rdram, s, draw_max_v, &key);
    if (!f.found || f.ignored) return GeTexrepResult::None;
    const TexrepImage* img = nullptr;
    switch (g_images->request(f.path, &img)) {
    case TexrepImageQueue::State::Pending:
        return GeTexrepResult::Pending;
    case TexrepImageQueue::State::Failed:
        return GeTexrepResult::None;
    case TexrepImageQueue::State::Ready:
        break;
    }
    out->w = img->w;
    out->h = img->h;
    out->rgba = img->rgba.data();
    out->filter = g_pack.filter(key);
    return GeTexrepResult::Ready;
}

uint64_t ge_texrep_epoch() { return g_images ? g_images->ready_epoch() : 0; }
