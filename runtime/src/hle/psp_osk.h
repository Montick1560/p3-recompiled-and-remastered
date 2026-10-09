// sceUtilityOsk back end: on-screen keyboard completion without a UI.
//
// Ported from PPSSPP (GPL-2.0-or-later):
//   Core/Dialog/PSPOskDialog.cpp (WriteOutput, FieldMaxLength, the
//     NativeKeyboard completion path) and Core/Dialog/PSPDialog.cpp
//     (CheckRequest sizes).
//
// Pure C++: no SDL, no scheduler, no guest-RAM globals. The guest
// SceUtilityOskParams is read and written in place through a bounds-checked
// view of guest RAM (addresses are masked with 0x07FFFFFF like the emitted
// code does), so a bad guest pointer yields no write instead of a host
// crash. The HLE glue (dialog status machine) lives in psp_hle_utility.cpp.
//
// There is no keyboard UI: the result text comes from the environment
// (PSPRECOMP_OSK_TEXT, default "Hero" — Patapon 3 rejects names shorter
// than 2 characters), mirroring PPSSPP's native-input-box path
// (PSPOskDialog::NativeKeyboard): the text is converted UTF-8 -> UCS-2,
// truncated to the field limit, and WriteOutput lands it in the guest
// buffer with field result = PSP_UTILITY_OSK_RESULT_CHANGED.
//
// Deliberate deviation: PPSSPP never writes SceUtilityOskParams.state;
// completion here sets it to PSP_UTILITY_OSK_DIALOG_FINISHED so a game that
// reads the field sees the terminal "done" value.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace psp_osk {
// PPSSPP Core/HLE/ErrorCodes.h (subset used here).
enum : uint32_t {
    ERR_UTILITY_INVALID_STATUS = 0x80110001u,
    ERR_UTILITY_INVALID_ADDRESS = 0x80110002u,
    ERR_UTILITY_INVALID_PARAM_SIZE = 0x80110004u,
};

// SceUtilityOskResult (PPSSPP Core/Dialog/PSPOskDialog.h).
enum : int32_t {
    RESULT_UNCHANGED = 0,
    RESULT_CANCELLED = 1,
    RESULT_CHANGED = 2,
};

// SceUtilityOskState (PPSSPP Core/Dialog/PSPOskDialog.h).
enum : int32_t {
    STATE_NONE = 0,
    STATE_INITING = 1,
    STATE_INITED = 2,
    STATE_VISIBLE = 3,
    STATE_QUIT = 4,
    STATE_FINISHED = 5,
};

// Byte offsets into the guest structs (PPSSPP Core/Dialog/PSPOskDialog.h
// and Core/Dialog/PSPDialog.h; pspUtilityDialogCommon is 48 bytes).
namespace off {
// SceUtilityOskParams
constexpr uint32_t kSize = 0;      // pspUtilityDialogCommon.size
constexpr uint32_t kResult = 28;   // pspUtilityDialogCommon.result
constexpr uint32_t kFieldCount = 48;  // s32
constexpr uint32_t kFields = 52;      // ptr to SceUtilityOskData[]
constexpr uint32_t kState = 56;       // s32 SceUtilityOskState
constexpr uint32_t kUnk60 = 60;
// PSPDialog::CheckRequest sizes for the OSK: { 0x40, 0x44 }.
constexpr uint32_t kParamsSizeV1 = 0x40;
constexpr uint32_t kParamsSizeV2 = 0x44;
// SceUtilityOskData (52 bytes per field)
constexpr uint32_t kDataLanguage = 8;        // s32 SceUtilityOskInputLanguage
constexpr uint32_t kDataInputType = 16;      // s32 flags
constexpr uint32_t kDataLines = 20;          // s32
constexpr uint32_t kDataDesc = 28;           // ptr to u16 string
constexpr uint32_t kDataIntext = 32;         // ptr to u16 string
constexpr uint32_t kDataOuttextLength = 36;  // u32, u16 units incl. terminator
constexpr uint32_t kDataOuttext = 40;        // ptr to u16 buffer
constexpr uint32_t kDataResult = 44;         // s32 SceUtilityOskResult
constexpr uint32_t kDataOuttextLimit = 48;   // u32, chars excl. terminator
constexpr uint32_t kDataSize = 52;
}  // namespace off

// Fallback result text when PSPRECOMP_OSK_TEXT is unset or empty. Patapon 3
// requires >= 2 characters, so the default satisfies that.
constexpr const char* kDefaultText = "Hero";

// Environment value -> result text: `env` when non-null and non-empty,
// else kDefaultText.
std::string resolve_text(const char* env);

// UTF-8 -> UTF-16, BMP only: code points <= 0xFFFF (surrogates excluded)
// are kept; astral code points and invalid sequences are skipped.
std::u16string utf8_to_utf16(const std::string& s);

// PPSSPP PSPOskDialog::FieldMaxLength: maximum characters written to the
// output buffer, excluding the terminator.
uint32_t field_max_length(uint32_t outtextlength, uint32_t outtextlimit);
// PPSSPP PSPOskDialog completion (NativeKeyboard success + WriteOutput):
// field 0's outtext gets `text` as UTF-16LE, NUL-terminated, truncated to
// field_max_length(outtextlength, outtextlimit); base.result = 0;
// fields[0].result = RESULT_CHANGED; params state = STATE_FINISHED.
// Only the text and its terminator are written, not the rest of the buffer.
// fields[0].desc (UCS-2) is read back for the summary. Never reads or
// writes outside [rdram, rdram + ram_size); an invalid params/fields/outtext
// pointer degrades to a partial (or no) write, like PPSSPP.
// Returns a one-line summary: field0 desc="<desc as utf8>" -> "<text>".
std::string complete(uint8_t* rdram, size_t ram_size, uint32_t params_addr,
                     const std::string& text);

}  // namespace psp_osk