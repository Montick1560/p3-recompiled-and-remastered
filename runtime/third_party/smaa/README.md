# SMAA 1.0 (vendored)

Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez, MIT (see `LICENSE`), from github.com/iryoku/smaa (master, fetched 2026-11-04): `SMAA.hlsl` and
`Textures/AreaTex.h`, `Textures/SearchTex.h`, unchanged. Used by `src/psp_present_aa.cpp` (`PSPRECOMP_AA=smaa`):
`SMAA.hlsl` is embedded in the exe (cmake/embed_files.cmake) and compiled as GLSL (`SMAA_GLSL_3`).
