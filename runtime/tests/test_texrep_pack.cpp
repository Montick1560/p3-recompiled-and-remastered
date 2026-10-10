// Unit tests for TexrepPack: textures.ini parsing and PPSSPP's LookupWildcard
// order (GPU/Common/TextureReplacer.cpp).
#include "psp_texrep_pack.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const char* kIni =
    "# comment\n"
    "[options]\n"
    "version = 1\n"
    "hash = xxh64\n"
    "ignoreAddress = True\n"
    "ignoreMipmap = True\n"
    "reduceHash = True\n"
    "[games]\n"
    "UCES01421 = textures.ini\n"
    "[hashes]\n"
    "; TEXTURES: Start\n"
    "00000000098b1cca0ddf97bd = effects/00000000098b1cca0ddf97bd.png\n"
    "000000001111111122222222 = ui\\back.png\n"
    "0000000033333333 = clutonly.png\n"
    "000000004444444455555555 =\n"
    "000000006666666677777777 = ../escape.png\n"
    "000000008888888899999999_1 = level1.png\n"
    "[filtering]\n"
    "00000000098b1cca0ddf97bd = nearest\n"
    "[reducehashranges]\n"
    "512,512 = 0.25\n"
    "[hashranges]\n"
    "0x08800000,512,512 = 480,272\n";

static TexrepPack loaded() {
    TexrepPack p;
    std::string err;
    const std::vector<std::string> root = {"aaaaaaaabbbbbbbbcccccccc.png", "readme.md", "abc.png"};
    CHECK(p.load(kIni, root, &err), "pack loads");
    return p;
}

static void options() {
    const TexrepPack p = loaded();
    CHECK(p.ignore_address() && p.reduce_hash() && !p.xxh32(), "options parsed");
    CHECK(p.reduce_for(512, 512) == 0.25f, "reducehashranges entry");
    CHECK(p.reduce_for(256, 256) == 0.5f, "reduce default 0.5");
    int w = 0, h = 0;
    CHECK(p.hash_range(0x08800000u, 512, 512, &w, &h) && w == 480 && h == 272, "hashrange entry");
    CHECK(!p.hash_range(0x08800010u, 512, 512, &w, &h) && w == 512 && h == 512, "no hashrange keeps size");
}

static void lookups() {
    const TexrepPack p = loaded();
    // Exact key (address already zero in the pack).
    TexrepFind f = p.find({0x00000000098B1CCAULL, 0x0DDF97BDu});
    CHECK(f.found && !f.ignored && f.path == "effects/00000000098b1cca0ddf97bd.png", "exact");
    // ignoreAddress: the address half of a live key is zeroed first.
    f = p.find({0x09A0000000000000ULL | 0x098B1CCAULL, 0x0DDF97BDu});
    CHECK(f.found && f.path == "effects/00000000098b1cca0ddf97bd.png", "address ignored");
    CHECK(p.find({0x11111111ULL, 0x22222222u}).path == "ui/back.png", "backslash -> slash");
    // Partial key "0000000033333333" = (cachekey, hash 0): CLUT-only wildcard.
    f = p.find({0x33333333ULL, 0xDEADBEEFu});
    CHECK(f.found && f.path == "clutonly.png", "clut-only wildcard (hash 0)");
    f = p.find({0x44444444ULL, 0x55555555u});
    CHECK(f.found && f.ignored, "empty value = ignored");
    CHECK(!p.find({0x66666666ULL, 0x77777777u}).found, "'..' path rejected");
    f = p.find({0x88888888ULL, 0x99999999u});
    CHECK(f.found && f.ignored, "level-1-only entry = ignored (PPSSPP ComputeAliasMap breaks at mip 0)");
    CHECK(p.find({0xAAAAAAAABBBBBBBBULL & 0xFFFFFFFFULL, 0xCCCCCCCCu}).found == false,
          "root file key has address aaaaaaaa: not found once the address is zeroed");
    CHECK(p.alias_count() >= 5, "aliases counted");
    CHECK(p.filter({0x098B1CCAULL, 0x0DDF97BDu}) == TexrepFilter::Nearest, "filtering entry");
    CHECK(p.filter({0x1ULL, 0x2u}) == TexrepFilter::None, "no filtering entry");
}

static void root_files_without_ignore_address() {
    TexrepPack p;
    std::string err;
    CHECK(p.load("[options]\nhash = xxh64\n", {"aaaaaaaabbbbbbbbcccccccc.png", "x.png"}, &err), "root-only pack");
    TexrepFind f = p.find({0xAAAAAAAABBBBBBBBULL, 0xCCCCCCCCu});
    CHECK(f.found && f.path == "aaaaaaaabbbbbbbbcccccccc.png", "hash-named root file");
    // Wildcard (0, hash): any key with that data hash.
    TexrepPack q;
    CHECK(q.load("[options]\nhash = xxh64\n[hashes]\n0000000000000000abcdef01 = any.png\n", {}, &err), "data-hash-only pack");
    CHECK(q.find({0x0912345000000506ULL, 0xABCDEF01u}).path == "any.png", "data-hash-only wildcard");
}

static void bad_packs() {
    TexrepPack p;
    std::string err;
    CHECK(!p.load("[options]\nhash = quick\n", {}, &err), "quick hash rejected");
    CHECK(!err.empty(), "error text set");
    CHECK(!p.load("[hashes]\n000000001111111122222222 = a.png\n", {}, &err), "missing hash type rejected");
}

int main() {
    options();
    lookups();
    root_files_without_ignore_address();
    bad_packs();
    if (failures == 0) std::printf("test_texrep_pack: all passed\n");
    return failures == 0 ? 0 : 1;
}
