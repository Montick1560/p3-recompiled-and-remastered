# xBRZ 1.8 (vendored)

Image upscaler by Zenju (HqMAME project), GPL-3.0 (see `LICENSE` and the file headers), as vendored by PPSSPP
(github.com/hrydgard/ppsspp `ext/xbrz`, fetched 2026-11-04). Used by `src/psp_tex_upscale.cpp` for the optional
texture upscaling (`PSPRECOMP_TEX_UPSCALE`). Local change: `xbrz.h` drops PPSSPP's `ppsspp_config.h` platform
include and includes `<cstdint>` directly.
