#include "psp_ge_texpatch.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kMaxPatchers = 4;
constexpr size_t kMemoMax = 512;

GeTexPatcher g_patchers[kMaxPatchers];
int g_count = 0;
uint32_t g_latched[kMaxPatchers] = {};
// content hash -> index of the patcher that recognised it, -1 = none. GE thread only.
std::unordered_map<uint32_t, int> g_memo;
int g_scale = 1;

}  // namespace

void ge_texpatch_register(const GeTexPatcher& p) {
    if (g_count < kMaxPatchers) g_patchers[g_count++] = p;
}

bool ge_texpatch_active() { return g_count > 0; }

void ge_texpatch_begin_list() {
    for (int i = 0; i < g_count; i++) g_latched[i] = g_patchers[i].variant() & 0x0FFFFFFFu;
}

uint32_t ge_texpatch_variant(const uint8_t* rdram, const GeTexDecodeParams& p, uint32_t content_hash) {
    int idx = -1;
    bool any_candidate = false;
    for (int i = 0; i < g_count; i++) any_candidate |= g_patchers[i].candidate(p);
    if (!any_candidate) return 0;
    auto it = g_memo.find(content_hash);
    if (it != g_memo.end()) {
        idx = it->second;
    } else {
        std::vector<uint8_t> rgba;
        if (ge_decode_texture(rdram, p, rgba)) {
            for (int i = 0; i < g_count && idx < 0; i++) {
                if (g_patchers[i].candidate(p) && g_patchers[i].recognise(p, rgba.data())) idx = i;
            }
        }
        if (g_memo.size() >= kMemoMax) g_memo.clear();
        g_memo[content_hash] = idx;
    }
    if (idx < 0 || g_latched[idx] == 0) return 0;
    return (static_cast<uint32_t>(idx + 1) << 28) | g_latched[idx];
}

void ge_texpatch_apply(uint32_t variant, const GeTexDecodeParams& p, RgbaImage& img) {
    const int idx = static_cast<int>(variant >> 28) - 1;
    if (idx < 0 || idx >= g_count) return;
    g_patchers[idx].apply(variant & 0x0FFFFFFFu, p, img);
}

void ge_texpatch_set_scale(int render_scale) { g_scale = std::clamp(render_scale, 1, 4); }

int ge_texpatch_scale() { return g_scale; }
