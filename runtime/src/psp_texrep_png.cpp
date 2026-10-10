#include "psp_texrep_png.h"

#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

bool ge_texrep_decode_png(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint8_t>* rgba) {
    if (data == nullptr || size == 0 || size > 0x7FFFFFFF) return false;
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), w, h, &comp, 4);
    if (px == nullptr) return false;
    rgba->resize(static_cast<size_t>(*w) * static_cast<size_t>(*h) * 4);
    std::memcpy(rgba->data(), px, rgba->size());
    stbi_image_free(px);
    return true;
}
