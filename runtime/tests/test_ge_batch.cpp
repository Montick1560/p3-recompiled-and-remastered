// GE draw batching: flush rule and triangle-list indices (psp_ge_batch.h). Header-only, no GL.
#include "psp_ge_batch.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static uint32_t w(uint8_t cmd, uint32_t data) { return (uint32_t{cmd} << 24) | data; }

// Same word twice, or a new word, for one command.
static bool same(uint8_t cmd) { return ge_cmd_flushes(cmd, w(cmd, 5), w(cmd, 5)); }
static bool changed(uint8_t cmd) { return ge_cmd_flushes(cmd, w(cmd, 5), w(cmd, 6)); }

static void flush_rule() {
    CHECK(same(GE_CMD_LOADCLUT), "LOADCLUT flushes even with the same word");
    CHECK(same(GE_CMD_TRANSFERSTART), "block transfer always flushes");
    CHECK(same(GE_CMD_TEXFLUSH) && same(GE_CMD_TEXSYNC), "texture flush/sync always flush");
    CHECK(same(GE_CMD_FINISH) && same(GE_CMD_END) && same(GE_CMD_SIGNAL), "list control flushes");
    CHECK(same(GE_CMD_BEZIER) && same(GE_CMD_SPLINE), "patches flush before they draw");
    CHECK(same(GE_CMD_CLEARMODE), "clear mode always flushes");
    CHECK(!changed(GE_CMD_PRIM), "PRIM is what gets batched");
    CHECK(!changed(GE_CMD_VADDR) && !changed(GE_CMD_WORLDMATRIXDATA), "CPU-only state never flushes");
    CHECK(!changed(GE_CMD_MORPHWEIGHT0) && !changed(GE_CMD_TEXSCALEU) && !changed(GE_CMD_LDC3),
          "morph, UV scale and lights are applied on the CPU");
    CHECK(!changed(GE_CMD_JUMP) && !changed(GE_CMD_CALL) && !changed(GE_CMD_RET), "list flow never flushes");
    CHECK(changed(GE_CMD_TEXADDR0) && !same(GE_CMD_TEXADDR0), "draw-time state flushes on change only");
    CHECK(changed(GE_CMD_ALPHABLENDENABLE) && changed(GE_CMD_SCISSOR1), "blend and scissor are draw-time state");
    CHECK(changed(GE_CMD_FRAMEBUFPTR) && changed(GE_CMD_FOGCOLOR), "target and fog colour are draw-time state");
    const uint32_t thr = GE_VTYPE_THROUGH;
    CHECK(!ge_cmd_flushes(GE_CMD_VERTEXTYPE, w(GE_CMD_VERTEXTYPE, 0x11), w(GE_CMD_VERTEXTYPE, 0x12)),
          "a vertex format change alone does not flush");
    CHECK(ge_cmd_flushes(GE_CMD_VERTEXTYPE, w(GE_CMD_VERTEXTYPE, 0x11), w(GE_CMD_VERTEXTYPE, 0x11 | thr)),
          "a through-mode change flushes (viewport and shader depend on it)");
}

static void list_indices() {
    std::vector<uint32_t> i;
    ge_triangle_list_indices(GE_PRIM_TRIANGLE_STRIP, 5, i);
    CHECK(i == std::vector<uint32_t>({0, 1, 2, 2, 1, 3, 2, 3, 4}),
          "strip: odd triangles swap their first two vertices (PPSSPP AddStrip)");
    i.clear();
    ge_triangle_list_indices(GE_PRIM_TRIANGLE_FAN, 5, i);
    CHECK(i == std::vector<uint32_t>({0, 1, 2, 0, 2, 3, 0, 3, 4}), "fan pivots on vertex 0");
    i.clear();
    ge_triangle_list_indices(GE_PRIM_TRIANGLES, 7, i);
    CHECK(i == std::vector<uint32_t>({0, 1, 2, 3, 4, 5}), "list drops the incomplete tail");
    i.clear();
    ge_triangle_list_indices(GE_PRIM_TRIANGLE_STRIP, 2, i);
    CHECK(i.empty(), "a strip shorter than 3 adds nothing");
    i = {9};
    ge_triangle_list_indices(GE_PRIM_TRIANGLES, 3, i);
    CHECK(i == std::vector<uint32_t>({9, 0, 1, 2}), "indices are appended");
}

int main() {
    flush_rule();
    list_indices();
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("ge_batch: all passed\n");
    return 0;
}
