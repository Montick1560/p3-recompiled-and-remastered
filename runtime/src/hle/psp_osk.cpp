// See psp_osk.h for the porting notes and the deliberate deviations.

#include "psp_osk.h"

#include <cstring>

namespace psp_osk {

namespace {

// Bounds-checked view of guest RAM: addresses are masked with 0x07FFFFFF
// like the emitted code does, and address 0 is never valid (PPSSPP
// PSPPointer::IsValid). Modeled on psp_savedata.cpp's Mem.
struct Mem {
    uint8_t* base = nullptr;
    size_t size = 0;

    uint8_t* range(uint32_t addr, uint64_t len) const {
        if (base == nullptr || addr == 0) {
            return nullptr;
        }
        uint64_t off = addr & 0x07FFFFFFu;
        if (off > size || len > size - off) {
            return nullptr;
        }
        return base + off;
    }
    bool valid(uint32_t addr, uint64_t len) const {
        return range(addr, len) != nullptr;
    }
    uint16_t rd16(uint32_t addr) const {
        const uint8_t* p = range(addr, 2);
        if (p == nullptr) return 0;
        uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    uint32_t rd32(uint32_t addr) const {
        const uint8_t* p = range(addr, 4);
        if (p == nullptr) return 0;
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    void wr16(uint32_t addr, uint16_t v) const {
        uint8_t* p = range(addr, 2);
        if (p != nullptr) std::memcpy(p, &v, 2);
    }
    void wr32(uint32_t addr, uint32_t v) const {
        uint8_t* p = range(addr, 4);
        if (p != nullptr) std::memcpy(p, &v, 4);
    }
};

void utf8_push(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}
// PPSSPP PSPOskDialog::ConvertUCS2ToUTF8 (pointer variant): guest UCS-2 ->
// UTF-8, capped at 2047 bytes out, stopping at the NUL terminator or the
// end of the valid range.
std::string ucs2_to_utf8(const Mem& mem, uint32_t addr) {
    std::string out;
    if (addr == 0) {
        return out;
    }
    const size_t maxLength = 2047;
    for (uint32_t a = addr; mem.valid(a, 2); a += 2) {
        const uint16_t c = mem.rd16(a);
        if (c == 0) {
            break;
        }
        const size_t bytes = c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
        if (out.size() + bytes > maxLength) {
            break;
        }
        utf8_push(out, c);
    }
    return out;
}

// UTF-16 -> UTF-8 for the summary line (same BMP encoding as above).
std::string utf16_to_utf8(const std::u16string& s) {
    std::string out;
    for (char16_t ch : s) {
        utf8_push(out, static_cast<uint16_t>(ch));
    }
    return out;
}

}  // namespace

std::string resolve_text(const char* env) {
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return kDefaultText;
}
std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t b = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (b < 0x80) {
            cp = b;
            len = 1;
        } else if (b >= 0xC2 && b <= 0xDF) {
            cp = b & 0x1F;
            len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            cp = b & 0x0F;
            len = 3;
        } else if (b >= 0xF0 && b <= 0xF4) {
            cp = b & 0x07;
            len = 4;  // astral plane: consumed but not representable
        } else {
            ++i;  // invalid lead byte
            continue;
        }
        bool ok = i + len <= s.size();
        for (size_t j = 1; ok && j < len; j++) {
            const uint8_t cb = static_cast<uint8_t>(s[i + j]);
            if ((cb & 0xC0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (cb & 0x3F);
            }
        }
        if (!ok) {
            ++i;  // truncated/invalid sequence: resync at the next byte
            continue;
        }
        i += len;
        if (len == 4) {
            continue;  // beyond the BMP
        }
        // Overlong encodings and UTF-16 surrogate values are not valid.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800)) {
            continue;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }
        out.push_back(static_cast<char16_t>(cp));
    }
    return out;
}

uint32_t field_max_length(uint32_t outtextlength, uint32_t outtextlimit) {
    // PPSSPP PSPOskDialog::FieldMaxLength.
    if (outtextlength == 0) {
        return 0;
    }
    if (outtextlimit > outtextlength - 1 || outtextlimit == 0) {
        return outtextlength - 1;
    }
    return outtextlimit;
}
std::string complete(uint8_t* rdram, size_t ram_size, uint32_t params_addr,
                     const std::string& text) {
    const Mem mem{rdram, ram_size};
    // The glue validated these at InitStart (PPSSPP CheckRequest/Init);
    // revalidate defensively: a corrupt guest write must not escape the RAM.
    if (!mem.valid(params_addr, off::kParamsSizeV1)) {
        return "field0 error: bad params pointer";
    }
    const uint32_t field = mem.rd32(params_addr + off::kFields);  // field 0
    if (!mem.valid(field, off::kDataSize)) {
        return "field0 error: bad fields pointer";
    }

    const uint32_t outtextlength = mem.rd32(field + off::kDataOuttextLength);
    const uint32_t outtextlimit = mem.rd32(field + off::kDataOuttextLimit);
    const uint32_t outtext = mem.rd32(field + off::kDataOuttext);

    // PPSSPP NativeKeyboard success path: convert, then truncate to the
    // field's maximum.
    std::u16string input = utf8_to_utf16(text);
    const uint32_t max_len = field_max_length(outtextlength, outtextlimit);
    if (input.size() > max_len) {
        input.erase(max_len);
    }

    // PPSSPP WriteOutput: only the text and its terminator, not the rest
    // of the buffer. A bad outtext pointer skips just the text write.
    size_t end = outtextlength;
    if (end > input.size()) {
        end = input.size() + 1;
    }
    if (end != 0 && mem.valid(outtext, end * sizeof(uint16_t))) {
        for (size_t i = 0; i < end; ++i) {
            uint16_t value = 0;
            if (i < max_len && i < input.size()) {
                value = input[i];
            }
            mem.wr16(outtext + static_cast<uint32_t>(i) * 2, value);
        }
    }

    mem.wr32(params_addr + off::kResult, 0);              // base.result
    mem.wr32(field + off::kDataResult, RESULT_CHANGED);   // field result
    mem.wr32(params_addr + off::kState, STATE_FINISHED);  // params state

    const std::string desc =
        ucs2_to_utf8(mem, mem.rd32(field + off::kDataDesc));
    return "field0 desc=\"" + desc + "\" -> \"" + utf16_to_utf8(input) +
           "\"";
}

}  // namespace psp_osk