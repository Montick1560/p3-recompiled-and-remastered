// psp_eboot_loader.h — [image] source = "disc" (games/<id>/game.toml): the main memory image is
// read from the boot file in disc0 at boot instead of being compiled into the exe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PspImageSegment { uint32_t vaddr; uint32_t filesz; uint32_t memsz; };
struct PspExpectedImage { const PspImageSegment* segments; uint32_t count; uint64_t fnv1a64; };

enum class EbootStatus { Ok, Missing, DecryptFailed, NotElf, LayoutMismatch, HashMismatch };

// FNV-1a-64, the same function psp_emitter::bank::fnv1a64 computes at recompile time.
uint64_t psp_eboot_fnv1a64(const uint8_t* p, size_t n);
const char* psp_eboot_status_name(EbootStatus s);

// Turns the boot file's bytes (encrypted "~PSP" or a plain ELF) into the memory image the code
// was recompiled from: each expected PT_LOAD's file bytes then zeros up to memsz, concatenated.
// `detail` says "decrypted" / "plain ELF" on success, the reason otherwise; `image` is empty
// unless the result is Ok.
EbootStatus psp_eboot_build_image(std::vector<uint8_t> file, const PspExpectedImage& want,
                                  std::vector<uint8_t>& image, std::string& detail);

// Host path of a guest boot path: "<disc0>/PSP_GAME/SYSDIR/EBOOT.BIN" for
// "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN".
std::string psp_eboot_host_path(const std::string& disc0, const std::string& guest_boot_path);

// main() sets the boot file before psp_init_data_sections(); embedded builds never read it.
void psp_eboot_set_boot_file(const std::string& host_path);

// Reads the boot file, builds and checks the image and copies it into rdram. On any failure:
// one "[EBOOT] ..." line on stderr and exit code 11 (docs/contracts/settings-v1.md).
void psp_eboot_load_or_exit(uint8_t* rdram, const PspExpectedImage& want);
