// psp_eboot_loader.cpp — [image] source = "disc": the main memory image comes from the boot file
// in disc0 (PSP_GAME/SYSDIR/EBOOT.BIN), not from the exe. An encrypted ("~PSP") file is
// decrypted in memory with PPSSPP's PRX decrypter (third_party/kirk); a plain ELF (a developer
// dump) is used as it is. The result must equal the image the code was recompiled from.
#include "psp_eboot_loader.h"

#include "Core/ELF/PrxDecrypter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

std::string g_boot_file;

uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}
uint16_t rd16(const std::vector<uint8_t>& b, size_t o) {
    return uint16_t(b[o] | b[o + 1] << 8);
}

}  // namespace

uint64_t psp_eboot_fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

const char* psp_eboot_status_name(EbootStatus s) {
    switch (s) {
        case EbootStatus::Ok: return "ok";
        case EbootStatus::Missing: return "missing";
        case EbootStatus::DecryptFailed: return "decrypt failed";
        case EbootStatus::NotElf: return "not an ELF image";
        case EbootStatus::LayoutMismatch: return "segment layout differs";
        case EbootStatus::HashMismatch: return "image differs from the recompiled one";
    }
    return "?";
}

EbootStatus psp_eboot_build_image(std::vector<uint8_t> file, const PspExpectedImage& want,
                                  std::vector<uint8_t>& image, std::string& detail) {
    image.clear();
    std::string kind = "plain ELF";
    if (file.size() >= 4 && std::memcmp(file.data(), "~PSP", 4) == 0) {
        std::vector<uint8_t> out(file.size());
        int n = pspDecryptPRX(file.data(), out.data(), u32(file.size()));
        if (n <= 0) {
            detail = "pspDecryptPRX returned " + std::to_string(n);
            return EbootStatus::DecryptFailed;
        }
        out.resize(size_t(n));
        if (out.size() >= 2 && out[0] == 0x1F && out[1] == 0x8B) {
            detail = "gzip-compressed boot file (not supported)";
            return EbootStatus::DecryptFailed;
        }
        file.swap(out);
        kind = "decrypted";
    }
    if (file.size() < 52 || std::memcmp(file.data(), "\x7F" "ELF", 4) != 0 || file[4] != 1 || file[5] != 1) {
        detail = "no ELF32 little-endian header";
        return EbootStatus::NotElf;
    }
    if (rd16(file, 16) != 2) {
        detail = "not ET_EXEC";
        return EbootStatus::NotElf;
    }
    const uint32_t phoff = rd32(file, 28);
    const uint16_t phentsize = rd16(file, 42), phnum = rd16(file, 44);
    if (phentsize < 32 || uint64_t(phoff) + uint64_t(phnum) * phentsize > file.size()) {
        detail = "program headers past the end of the file";
        return EbootStatus::NotElf;
    }
    uint32_t seen = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const size_t ph = phoff + size_t(i) * phentsize;
        if (rd32(file, ph) != 1) continue;  // PT_LOAD only
        const uint32_t off = rd32(file, ph + 4), vaddr = rd32(file, ph + 8);
        const uint32_t filesz = rd32(file, ph + 16), memsz = rd32(file, ph + 20);
        if (seen >= want.count) {
            detail = "more PT_LOAD segments than expected";
            image.clear();
            return EbootStatus::LayoutMismatch;
        }
        const PspImageSegment& w = want.segments[seen];
        if (vaddr != w.vaddr || filesz != w.filesz || memsz != w.memsz) {
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "segment %u: vaddr 0x%08X filesz 0x%X memsz 0x%X, expected 0x%08X 0x%X 0x%X",
                          seen, vaddr, filesz, memsz, w.vaddr, w.filesz, w.memsz);
            detail = buf;
            image.clear();
            return EbootStatus::LayoutMismatch;
        }
        if (uint64_t(off) + filesz > file.size() || filesz > memsz) {
            detail = "segment past the end of the file";
            image.clear();
            return EbootStatus::NotElf;
        }
        image.insert(image.end(), file.begin() + off, file.begin() + off + filesz);
        image.insert(image.end(), memsz - filesz, uint8_t(0));
        seen++;
    }
    if (seen != want.count) {
        detail = "fewer PT_LOAD segments than expected";
        image.clear();
        return EbootStatus::LayoutMismatch;
    }
    const uint64_t h = psp_eboot_fnv1a64(image.data(), image.size());
    if (h != want.fnv1a64) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "fnv 0x%016llX, expected 0x%016llX",
                      (unsigned long long)h, (unsigned long long)want.fnv1a64);
        detail = buf;
        image.clear();
        return EbootStatus::HashMismatch;
    }
    detail = kind;
    return EbootStatus::Ok;
}

std::string psp_eboot_host_path(const std::string& disc0, const std::string& guest_boot_path) {
    const size_t p = guest_boot_path.find(":/");
    const std::string rel = p == std::string::npos ? guest_boot_path : guest_boot_path.substr(p + 2);
    std::string base = disc0;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    return base + "/" + rel;
}

void psp_eboot_set_boot_file(const std::string& host_path) { g_boot_file = host_path; }

void psp_eboot_load_or_exit(uint8_t* rdram, const PspExpectedImage& want) {
    EbootStatus st = EbootStatus::Missing;
    std::string detail = "cannot open the file";
    std::vector<uint8_t> image;
    std::ifstream f(g_boot_file, std::ios::binary);
    if (f) {
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        st = psp_eboot_build_image(std::move(file), want, image, detail);
    }
    if (st != EbootStatus::Ok) {
        std::fprintf(stderr, "[EBOOT] %s: %s (%s); exit 11\n", g_boot_file.c_str(),
                     psp_eboot_status_name(st), detail.c_str());
        std::fflush(stderr);
        // quick_exit, like the fast shutdown (psp_exit.h): std::exit would run static destructors
        // while the debug-socket thread is still joinable and abort instead of returning 11.
        std::quick_exit(11);
    }
    size_t at = 0;
    for (uint32_t i = 0; i < want.count; i++) {
        std::memcpy(rdram + (want.segments[i].vaddr & 0x07FFFFFFU), image.data() + at, want.segments[i].memsz);
        at += want.segments[i].memsz;
    }
    std::fprintf(stderr, "[EBOOT] loaded %s (%s, %zu bytes)\n", g_boot_file.c_str(), detail.c_str(), image.size());
}
