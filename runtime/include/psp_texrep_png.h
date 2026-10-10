#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

/// Decode PNG bytes to tightly packed RGBA8 rows (top row first). False on any
/// error (nothing is thrown, `rgba` is left unspecified).
bool ge_texrep_decode_png(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint8_t>* rgba);
