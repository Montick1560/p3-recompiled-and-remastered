// Unit tests for the sceUtilityOsk back end (psp_osk.cpp): guest struct
// layout offsets, UTF-8 -> UTF-16 conversion, the completion writes
// (UTF-16LE text + terminator, truncation at outtextlength/outtextlimit,
// field/common result, params state), the default-text rule, and bad
// guest-pointer safety. Standalone: a fake guest RAM only; no SDL, no
// game files.

#include "psp_osk.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace osk = psp_osk;

static int failures = 0;
static int tests_run = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        long long a_ = static_cast<long long>(actual); \
        long long e_ = static_cast<long long>(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, "FAIL: %s: got 0x%llX, expected 0x%llX " \
                "(line %d)\n", msg, static_cast<unsigned long long>(a_), \
                static_cast<unsigned long long>(e_), __LINE__); \
            failures++; \
        } \
    } while (0)

#define ASSERT_STR(actual, expected, msg) \
    do { \
        tests_run++; \
        std::string a_ = (actual); \
        if (a_ != (expected)) { \
            std::fprintf(stderr, "FAIL: %s: got \"%s\", expected \"%s\" " \
                "(line %d)\n", msg, a_.c_str(), expected, __LINE__); \
            failures++; \
        } \
    } while (0)
// ---- Fake guest RAM ------------------------------------------------------
// Guest addresses are 0x08000000-based; the back end masks them with
// 0x07FFFFFF, so 0x08001000 lands at byte 0x1000 of the fake.
static constexpr uint32_t kRamSize = 0x10000;
static constexpr uint32_t kParams = 0x08001000;
static constexpr uint32_t kFields = 0x08001200;
static constexpr uint32_t kDesc = 0x08001300;
static constexpr uint32_t kIntext = 0x08001380;
static constexpr uint32_t kOuttext = 0x08001400;

struct Guest {
    std::vector<uint8_t> ram;

    Guest() : ram(kRamSize, 0) {}

    static uint32_t at(uint32_t addr) { return addr & 0x07FFFFFFu; }

    void wr16(uint32_t addr, uint16_t v) {
        uint32_t o = at(addr);
        ram[o] = static_cast<uint8_t>(v & 0xFF);
        ram[o + 1] = static_cast<uint8_t>(v >> 8);
    }
    void wr32(uint32_t addr, uint32_t v) {
        std::memcpy(ram.data() + at(addr), &v, 4);
    }
    uint16_t rd16(uint32_t addr) const {
        uint16_t v;
        std::memcpy(&v, ram.data() + at(addr), 2);
        return v;
    }
    uint32_t rd32(uint32_t addr) const {
        uint32_t v;
        std::memcpy(&v, ram.data() + at(addr), 4);
        return v;
    }
    // NUL-terminated UCS-2 string.
    void ucs2(uint32_t addr, const std::u16string& s) {
        for (size_t i = 0; i < s.size(); i++) {
            wr16(addr + 2 * static_cast<uint32_t>(i), s[i]);
        }
        wr16(addr + 2 * static_cast<uint32_t>(s.size()), 0);
    }
    void fill(uint32_t addr, uint8_t v, size_t n) {
        std::memset(ram.data() + at(addr), v, n);
    }
};

// A valid one-field SceUtilityOskParams with desc "Hero name".
static void make_params(Guest& g, uint32_t outtextlength,
                        uint32_t outtextlimit) {
    g.wr32(kParams + osk::off::kSize, osk::off::kParamsSizeV1);
    g.wr32(kParams + osk::off::kFieldCount, 1);
    g.wr32(kParams + osk::off::kFields, kFields);
    g.wr32(kFields + osk::off::kDataDesc, kDesc);
    g.wr32(kFields + osk::off::kDataIntext, kIntext);
    g.wr32(kFields + osk::off::kDataOuttextLength, outtextlength);
    g.wr32(kFields + osk::off::kDataOuttext, kOuttext);
    g.wr32(kFields + osk::off::kDataOuttextLimit, outtextlimit);
    g.ucs2(kDesc, u"Hero name");
}
static void test_layout_offsets() {
    // PPSSPP Core/Dialog/PSPOskDialog.h field layout.
    ASSERT_EQ(osk::off::kResult, 28u, "common.result offset");
    ASSERT_EQ(osk::off::kFieldCount, 48u, "fieldCount offset");
    ASSERT_EQ(osk::off::kFields, 52u, "fields offset");
    ASSERT_EQ(osk::off::kState, 56u, "state offset");
    ASSERT_EQ(osk::off::kUnk60, 60u, "unk_60 offset");
    ASSERT_EQ(osk::off::kParamsSizeV1, 0x40u, "params size v1");
    ASSERT_EQ(osk::off::kParamsSizeV2, 0x44u, "params size v2");
    ASSERT_EQ(osk::off::kDataDesc, 28u, "desc offset");
    ASSERT_EQ(osk::off::kDataIntext, 32u, "intext offset");
    ASSERT_EQ(osk::off::kDataOuttextLength, 36u, "outtextlength offset");
    ASSERT_EQ(osk::off::kDataOuttext, 40u, "outtext offset");
    ASSERT_EQ(osk::off::kDataResult, 44u, "field result offset");
    ASSERT_EQ(osk::off::kDataOuttextLimit, 48u, "outtextlimit offset");
    ASSERT_EQ(osk::off::kDataSize, 52u, "SceUtilityOskData size");
    ASSERT_EQ(osk::RESULT_CHANGED, 2, "RESULT_CHANGED value");
    ASSERT_EQ(osk::STATE_FINISHED, 5, "STATE_FINISHED value");
    ASSERT_TRUE(std::string(osk::kDefaultText) == "Hero", "default text");
}
static void test_utf8_to_utf16() {
    std::u16string ascii = osk::utf8_to_utf16("Hero");
    ASSERT_EQ(ascii.size(), 4u, "ascii length");
    ASSERT_TRUE(ascii == u"Hero", "ascii content");

    // U+00E9 e-acute (2-byte), U+3042 HIRAGANA A (3-byte).
    std::u16string mixed = osk::utf8_to_utf16("A\xC3\xA9\xE3\x81\x82Z");
    ASSERT_EQ(mixed.size(), 4u, "mixed length");
    ASSERT_EQ(mixed[0], u'A', "ascii in mixed");
    ASSERT_EQ(mixed[1], 0x00E9, "2-byte sequence");
    ASSERT_EQ(mixed[2], 0x3042, "3-byte sequence");
    ASSERT_EQ(mixed[3], u'Z', "trailing ascii");

    // Astral code points (U+1F600, 4-byte) are beyond the BMP: skipped.
    ASSERT_EQ(osk::utf8_to_utf16("\xF0\x9F\x98\x80").size(), 0u,
              "astral skipped");
    // A UTF-16 surrogate value encoded in UTF-8 is invalid: skipped.
    ASSERT_EQ(osk::utf8_to_utf16("\xED\xA0\x80").size(), 0u,
              "surrogate skipped");
    // Bad bytes are skipped, not fatal: FF, 'e', truncated C3, 'x'.
    ASSERT_TRUE(osk::utf8_to_utf16("\xFF" "e" "\xC3" "x") == u"ex",
                "invalid sequences skipped");
}

static void test_field_max_length() {
    // PPSSPP PSPOskDialog::FieldMaxLength.
    ASSERT_EQ(osk::field_max_length(0, 0), 0u, "zero length");
    ASSERT_EQ(osk::field_max_length(10, 0), 9u, "limit 0 -> length-1");
    ASSERT_EQ(osk::field_max_length(10, 12), 9u, "limit > length-1");
    ASSERT_EQ(osk::field_max_length(10, 9), 9u, "limit == length-1");
    ASSERT_EQ(osk::field_max_length(10, 5), 5u, "limit within range");
    ASSERT_EQ(osk::field_max_length(1, 0), 0u, "length 1 -> NUL only");
}
static void test_completion_writes() {
    Guest g;
    make_params(g, 24, 0);  // room for 23 chars + terminator
    g.fill(kOuttext, 0xAA, 48);  // canary over the whole output buffer
    std::string summary =
        osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");

    // UTF-16LE text with terminator, nothing past it.
    ASSERT_EQ(g.rd16(kOuttext + 0), u'H', "char 0");
    ASSERT_EQ(g.rd16(kOuttext + 2), u'e', "char 1");
    ASSERT_EQ(g.rd16(kOuttext + 4), u'r', "char 2");
    ASSERT_EQ(g.rd16(kOuttext + 6), u'o', "char 3");
    ASSERT_EQ(g.rd16(kOuttext + 8), 0, "NUL terminator");
    ASSERT_EQ(g.rd16(kOuttext + 10), 0xAAAA, "buffer past terminator kept");

    // PPSSPP WriteOutput: base.result = 0, field result = CHANGED.
    ASSERT_EQ(g.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "field result CHANGED");
    ASSERT_EQ(g.rd32(kParams + osk::off::kResult), 0, "common result 0");
    ASSERT_EQ(g.rd32(kParams + osk::off::kState), osk::STATE_FINISHED,
              "params state FINISHED");

    ASSERT_STR(summary, "field0 desc=\"Hero name\" -> \"Hero\"",
               "summary line");
}

static void test_completion_non_ascii() {
    Guest g;
    make_params(g, 24, 0);
    // U+540D U+524D ("name" kanji) as desc; text with U+00E9.
    std::u16string jp;
    jp.push_back(0x540D);
    jp.push_back(0x524D);
    g.ucs2(kDesc, jp);
    std::string summary =
        osk::complete(g.ram.data(), g.ram.size(), kParams, "H\xC3\xA9ro");
    ASSERT_EQ(g.rd16(kOuttext + 2), 0x00E9, "2-byte char written");
    ASSERT_EQ(g.rd16(kOuttext + 8), 0, "NUL after 4 chars");
    ASSERT_STR(summary,
               "field0 desc=\"\xE5\x90\x8D\xE5\x89\x8D\" -> \"H\xC3\xA9ro\"",
               "summary converts ucs2 desc to utf8");
}
static void test_truncation_at_length() {
    // outtextlength caps the field: 5 -> 4 chars + terminator.
    Guest g;
    make_params(g, 5, 0);
    g.fill(kOuttext, 0xAA, 10);
    std::string summary =
        osk::complete(g.ram.data(), g.ram.size(), kParams, "HeroHero");
    ASSERT_EQ(g.rd16(kOuttext + 0), u'H', "truncated char 0");
    ASSERT_EQ(g.rd16(kOuttext + 6), u'o', "truncated char 3");
    ASSERT_EQ(g.rd16(kOuttext + 8), 0, "terminator at length-1");
    ASSERT_STR(summary, "field0 desc=\"Hero name\" -> \"Hero\"",
               "summary shows the truncated text");
    ASSERT_EQ(g.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "field result CHANGED");
}

static void test_truncation_at_limit() {
    // outtextlimit < outtextlength-1 wins (PPSSPP FieldMaxLength).
    Guest g;
    make_params(g, 10, 3);
    g.fill(kOuttext, 0xAA, 20);
    osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");
    ASSERT_EQ(g.rd16(kOuttext + 0), u'H', "char 0");
    ASSERT_EQ(g.rd16(kOuttext + 2), u'e', "char 1");
    ASSERT_EQ(g.rd16(kOuttext + 4), u'r', "char 2");
    ASSERT_EQ(g.rd16(kOuttext + 6), 0, "terminator after limit chars");
    ASSERT_EQ(g.rd16(kOuttext + 8), 0xAAAA, "rest of buffer kept");
}

static void test_default_text() {
    ASSERT_TRUE(osk::resolve_text(nullptr) == "Hero", "null env -> default");
    ASSERT_TRUE(osk::resolve_text("") == "Hero", "empty env -> default");
    ASSERT_TRUE(osk::resolve_text("Pata") == "Pata", "env used verbatim");
    // Patapon 3 rejects names shorter than 2 characters.
    ASSERT_TRUE(osk::resolve_text(nullptr).size() >= 2,
                "default is 2+ characters");
}

static void test_empty_text() {
    Guest g;
    make_params(g, 4, 0);
    g.fill(kOuttext, 0xAA, 8);
    osk::complete(g.ram.data(), g.ram.size(), kParams, "");
    ASSERT_EQ(g.rd16(kOuttext), 0, "empty text writes only the terminator");
    ASSERT_EQ(g.rd16(kOuttext + 2), 0xAAAA, "rest untouched");
    ASSERT_EQ(g.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "result still CHANGED");
}
static void test_zero_outtextlength() {
    Guest g;
    make_params(g, 0, 0);
    g.fill(kOuttext, 0xAA, 8);
    osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");
    ASSERT_EQ(g.rd16(kOuttext), 0xAAAA, "no text when outtextlength is 0");
    // PPSSPP writes the result fields even when there is no text write.
    ASSERT_EQ(g.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "field result still written");
    ASSERT_EQ(g.rd32(kParams + osk::off::kResult), 0,
              "common result still written");
}

static void test_bad_params_pointer() {
    Guest g;
    std::string s0 = osk::complete(g.ram.data(), g.ram.size(), 0, "Hero");
    ASSERT_TRUE(s0.find("error") != std::string::npos, "null params error");
    // Outside the guest view (0x0BFFFF00 masks to 0x3FFFF00 > ram size).
    std::string s1 =
        osk::complete(g.ram.data(), g.ram.size(), 0x0BFFFF00u, "Hero");
    ASSERT_TRUE(s1.find("error") != std::string::npos,
                "out-of-range params error");
}
static void test_bad_fields_pointer() {
    Guest g;
    make_params(g, 24, 0);
    g.wr32(kParams + osk::off::kFields, 0);
    g.wr32(kParams + osk::off::kResult, 0xDEADBEEF);  // canary: must survive
    std::string s = osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");
    ASSERT_TRUE(s.find("error") != std::string::npos, "null fields error");
    ASSERT_EQ(g.rd32(kParams + osk::off::kResult), 0xDEADBEEF,
              "nothing written with null fields");
}

static void test_bad_outtext_pointer() {
    // PPSSPP: a bad outtext skips the text write but the result fields
    // are still written.
    Guest g;
    make_params(g, 24, 0);
    g.wr32(kFields + osk::off::kDataOuttext, 0);
    osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");
    ASSERT_EQ(g.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "field result written despite null outtext");
    ASSERT_EQ(g.rd32(kParams + osk::off::kResult), 0,
              "common result written despite null outtext");

    Guest g2;
    make_params(g2, 24, 0);
    g2.wr32(kFields + osk::off::kDataOuttext, 0x0BFFF000u);  // out of range
    osk::complete(g2.ram.data(), g2.ram.size(), kParams, "Hero");
    ASSERT_EQ(g2.rd32(kFields + osk::off::kDataResult), osk::RESULT_CHANGED,
              "field result written despite out-of-range outtext");
}

static void test_null_desc() {
    Guest g;
    make_params(g, 24, 0);
    g.wr32(kFields + osk::off::kDataDesc, 0);
    std::string s = osk::complete(g.ram.data(), g.ram.size(), kParams, "Hero");
    ASSERT_STR(s, "field0 desc=\"\" -> \"Hero\"", "null desc logs empty");
}
int main() {
    test_layout_offsets();
    test_utf8_to_utf16();
    test_field_max_length();
    test_completion_writes();
    test_completion_non_ascii();
    test_truncation_at_length();
    test_truncation_at_limit();
    test_default_text();
    test_empty_text();
    test_zero_outtextlength();
    test_bad_params_pointer();
    test_bad_fields_pointer();
    test_bad_outtext_pointer();
    test_null_desc();

    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}