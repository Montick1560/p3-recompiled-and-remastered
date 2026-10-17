# Launcher part 3 — PPSSPP saves (import / export, chosen save) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Execution chosen by the user:** each task is handed to **DeepSeek v4.1 flash at max effort**
> (`python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo <repo> --model "deepseek/deepseek-flash#max"
> --msg "<no-commit message>"`) as a self-contained brief; Claude reads every diff, re-runs every gate,
> commits and decides. DeepSeek never commits, never pushes, never edits outside the task's files.
> The psprecomp chain (Tasks 1-3) and the launcher chain (Tasks 4-7) are independent and may run
> in parallel; Task 8 (end to end) needs both.

**Goal:** The player imports their PPSSPP Patapon 3 saves (EU or US, encrypted) from the launcher,
chooses which one the game's Continue loads, and can export a slot back to PPSSPP.

**Architecture:** The runtime's savedata module decrypts encrypted saves on load exactly like PPSSPP
(`SAVEDATA_PARAMS` crypt mode + the key the game passes, PPSSPP's sceChnnlsv cipher vendored next to
libkirk) and lets `PSPRECOMP_SAVE_SLOT` pick the entry `LISTLOAD` loads. `release.json` gains the game's
save facts. The launcher lists the slots of a savedata folder (PARAM.SFO + ICON0), copies saves between
the port and PPSSPP's `PSP/SAVEDATA` (staged in `.partial`, renamed into place, replaced folders moved to
`backups/`), rewrites `SAVEDATA_DIRECTORY` when a US save becomes EU, remembers the chosen slot and passes
it to the game.

**Tech Stack:** C++17 runtime (CMake + ctest, clang 22 llvm-mingw), vendored PPSSPP code (GPL-2.0+)
+ libkirk (GPL-3.0+), Python 3 (`-I`), Bash (Git Bash), Rust 1.96 + Tauri 2 (launcher), React 19 + TSX
+ Vitest.

**Spec:** `docs/superpowers/specs/2026-10-17-launcher-part3-design.md` (read it first).

## Global Constraints

- Save facts (all three releases): game save id `UCES01421`, importable foreign id `UCUS98751`,
  system data `_DATA00`, game slots `_DATA01`..`_DATA10` (prefix `_DATA`, first 1, last 10), other data
  `_DATA50`, `_DATA99`.
- Crypt mode from `SAVEDATA_PARAMS[0]`: `0x00` -> 0 (plain), `0x01` -> 1, `0x21` -> 3, `0x41` -> 5,
  any other non-zero -> 1 (PPSSPP `GetSaveCryptMode`).
- The port keeps writing plain saves (unchanged `save()`); only loading learns to decrypt.
- `PSPRECOMP_SAVE_SLOT=<save name>` (e.g. `_DATA04`) only changes which existing list entry
  `LISTLOAD` loads; every other mode ignores it.
- Launcher-only `settings.ini` keys, section `[launcher]`: `version` (exists), `ppsspp_savedata`
  (normalised PPSSPP `SAVEDATA` folder), `save_<release-id>` (chosen slot suffix, e.g. `_DATA04`).
  Contract v1 is otherwise unchanged; the game ignores `[launcher]`.
- Nothing is ever deleted: a replaced save folder is first moved to
  `backups/<UTC YYYYMMDD-HHMMSS>/<side>/<folder>` under the portable folder (`side` = `port-<id>` or
  `ppsspp`). The only thing written into PPSSPP's folder is the exported save (and its transient
  `<folder>.partial`).
- No game file (ISO, EBOOT, save) is ever committed. Tests that need the user's files skip when absent.
- Never write `DECO/savedata*`, `E:\Respaldo` or any real PPSSPP folder from a test; the E2E reads
  `E:\Respaldo\PPSSPP` only through the launcher's import (a copy) and exports to a scratch PPSSPP.
- Nothing is pushed. psprecomp commits on `patapon3-windows`, launcher commits on `main` of
  `DECO/launcher`. Author `Patapon Community` (psprecomp: repo config already set; launcher:
  `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit ...`).
  **No `Co-Authored-By` trailer.** Commit bodies end with `Verified: <gates and numbers>` and
  `Agent: <model that wrote the code>`.
- Test runs of the game use debug port **9998** (`PSPRECOMP_DEBUG_PORT=9998`), never 9999.
- Strings shown to the player live in `launcher/lang/es.json` and `launcher/lang/en.json`.
- Code, comments and commits in English.

## Review Focus

1. The game's data buffer is sized for the **plain** save (178,848 bytes); an encrypted file is 16
   bytes longer. The buffer-size check must run after decryption or every PPSSPP save fails with
   `LOAD_DATA_BROKEN` (Task 2 test `test_load_encrypted_save` uses a buffer of exactly the plain size).
2. The user's real `DATA04` has a MAC that does not match its SFO hash: loading must retry without the
   hash like PPSSPP, not fail (Task 1 real-save test, Task 2 `test_load_encrypted_bad_hash`).
3. Folder names that are not saves (`UCES01421_DATA01 - copia`, `UCES01421_DATA04.partial`, `.rar`
   files, other games' saves) must never be listed, imported or overwritten (Task 5 tests).
4. Exporting to PPSSPP on another drive (`E:`) while backups live on `C:`: moving the replaced
   PPSSPP folder into `backups/` must work across volumes (copy, then remove) and never lose it
   (Task 5 `move_dir` test with a forced copy path).
5. A crash mid-copy leaves `<folder>.partial`: it must never count as a save and is cleaned on the
   next start / transfer (Task 5 tests; Task 6 startup cleanup).

## File map

psprecomp (`DECO/psprecomp`):
- `runtime/third_party/kirk/Core/HLE/sceChnnlsv.{h,cpp}`, `runtime/third_party/kirk/vendor_chnnlsv.py`,
  `runtime/third_party/kirk/README.md` — vendored savedata cipher (Task 1).
- `runtime/include/hle/psp_save_crypt.h`, `runtime/src/hle/psp_save_crypt.cpp`,
  `runtime/tests/test_save_crypt.cpp` — decrypt / encrypt wrapper + ctest `save_crypt_tests` (Task 1).
- `runtime/CMakeLists.txt` — kirk library source, runtime source, tests (Tasks 1, 2).
- `runtime/include/hle/psp_savedata.h`, `runtime/src/hle/psp_savedata.cpp`,
  `runtime/src/hle/psp_hle_utility.cpp`, `runtime/tests/test_savedata.cpp` — encrypted load, chosen
  slot, FILES size, notes (Task 2).
- `ARCHITECTURE.md`, `README.md`, `docs/ENV_FLAGS.md` — docs (Task 2).
- `scripts/release_info.py`, `games/patapon3/release_saves.json`, `games/patapon3/scripts/p3.sh`,
  `games/patapon3/scripts/test_p3.sh` — `release.json` `saves` (Task 3).

launcher (`DECO/launcher`):
- `src-tauri/src/sfo.rs`, `src-tauri/src/releases.rs` — SFO entries / in-place string, `saves` config (Task 4).
- `src-tauri/src/saves.rs`, `src-tauri/src/ppsspp.rs`, `src-tauri/src/settings.rs` — listing, transfer,
  backups, PPSSPP folder, launcher keys (Task 5).
- `src-tauri/src/lib.rs`, `src-tauri/src/launch.rs` — commands, startup cleanup, `PSPRECOMP_SAVE_SLOT` (Task 6).
- `src/types.ts`, `src/api.ts`, `src/App.tsx`, `src/screens/{Saves,ImportSaves,Choice,MainMenu}.tsx`,
  `src/components/SaveRow.tsx`, `src/styles.css`, `lang/{es,en}.json`, tests, `README.md` — UI (Task 7).

Gates used below (exact commands):
- runtime: `cmake --build C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -2 && ctest --test-dir C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -3`
- test_p3: `cd /c/Users/torso/Documents/deco/psprecomp && bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -3`
- launcher Rust: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
- launcher UI: `cd /c/Users/torso/Documents/deco/launcher && npx vitest run 2>&1 | tail -5` and `npx tsc --noEmit`
- 1.1.4 tour: `cd /c/Users/torso/Documents/deco && TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/<name>` then `python -I build/tools/rt_regress.py build/tour/rebuild114_t1 build/tour/<name>` → `RT_REGRESS PASS`.

Baseline before Task 1 (record the real numbers in the Task 1 report): ctest 74/74, test_p3 12/12,
launcher cargo 44 passed + 1 ignored, Vitest 23/23.

---

### Task 1: Vendored savedata cipher + `psp_save_crypt` (runtime)

**Files (psprecomp):**
- Create: `runtime/third_party/kirk/vendor_chnnlsv.py`, `runtime/third_party/kirk/Core/HLE/sceChnnlsv.h`
  (verbatim copy), `runtime/third_party/kirk/Core/HLE/sceChnnlsv.cpp` (generated by the script)
- Create: `runtime/include/hle/psp_save_crypt.h`, `runtime/src/hle/psp_save_crypt.cpp`,
  `runtime/tests/test_save_crypt.cpp`
- Modify: `runtime/CMakeLists.txt` (kirk library gains the cipher; new test), `runtime/third_party/kirk/README.md`

**Interfaces:**
- Produces (namespace `psp_save_crypt`, header `hle/psp_save_crypt.h`):
  `int mode_from_params(const uint8_t* params, size_t size);`
  `int request_mode(bool full_request, uint32_t secure_version, uint32_t sdk_version);`
  `int decrypt(int mode, const std::vector<uint8_t>& file, const uint8_t* key, const uint8_t* expected_hash, std::vector<uint8_t>* out);`
  `int encrypt(int mode, const std::vector<uint8_t>& plain, const uint8_t* key, std::vector<uint8_t>* out, uint8_t hash[16]);`
  `decrypt` returns 0 or PPSSPP's negative `DecryptData` code (-1 file too short, -8 hash mismatch).
  The `kirk` CMake library now also provides `Core/HLE/sceChnnlsv.h` (include dirs are PUBLIC).

- [ ] **Step 1: Vendor script.** Create `runtime/third_party/kirk/vendor_chnnlsv.py`:

```python
"""Copy PPSSPP's Core/HLE/sceChnnlsv.cpp without its HLE wrappers (guest-pointer sceSd* entry
points, module tables, Register_*): only the savedata cipher stays.
Usage: python -I vendor_chnnlsv.py <ppsspp sceChnnlsv.cpp> <out .cpp>"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8", newline="").read().replace("\r\n", "\n")
old_inc = ('#include "Core/MemMapHelpers.h"\n#include "Core/HLE/HLE.h"\n'
           '#include "Core/HLE/FunctionWrappers.h"\n\n#include "Core/HLE/sceChnnlsv.h"\n'
           '#include "Core/HLE/sceKernel.h"\n')
new_inc = ('// psprecomp: HLE wrappers (guest-pointer sceSd* entry points, module tables, Register_*)\n'
           '// removed; only the savedata cipher remains. See third_party/kirk/README.md.\n'
           '#include <cstring>\n\n'
           '#include "Common/CommonTypes.h"\n'
           '#include "Common/Swap.h"\n'
           '#include "Core/HLE/sceChnnlsv.h"\n')
assert old_inc in s, "PPSSPP include block changed"
s = s.replace(old_inc, new_inc)
s, n = re.subn(r"static int sceSd\w+\(u32[^\n]*\{\n(?:.*\n)*?\}\n\n", "", s)
assert n == 6, f"expected 6 guest-pointer wrappers, removed {n}"
s = s[:s.index("const HLEFunction sceChnnlsv[]")].rstrip("\n") + "\n"
for bad in ("Memory::", "hleLog", "PSPPointer", "RegisterHLEModule", "_LOG("):
    assert bad not in s, bad
open(dst, "w", encoding="utf-8", newline="\n").write(s)
print(f"{dst}: {s.count(chr(10))} lines")
```

- [ ] **Step 2: Vendor the files.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp/runtime/third_party/kirk
mkdir -p Core/HLE
cp C:/Users/torso/Documents/deco/tools/ppsspp-src/Core/HLE/sceChnnlsv.h Core/HLE/sceChnnlsv.h
python -I vendor_chnnlsv.py C:/Users/torso/Documents/deco/tools/ppsspp-src/Core/HLE/sceChnnlsv.cpp Core/HLE/sceChnnlsv.cpp
git -C C:/Users/torso/Documents/deco/tools/ppsspp-src log -1 --format=%H
```
  Expected: `Core/HLE/sceChnnlsv.cpp: 478 lines`; the commit printed is `c8814f545f32a262884e427616281a286c697b32`
  (write the real one into the README in Step 7). `sceChnnlsv.h` is byte-identical to PPSSPP's.

- [ ] **Step 3: Header.** Create `runtime/include/hle/psp_save_crypt.h`:

```cpp
// PSP savedata encryption (the sceChnnlsv cipher), for loading saves PPSSPP or a real PSP wrote
// encrypted. DecryptData / EncryptData come from PPSSPP Core/Dialog/SavedataParam.cpp
// (GPL-2.0-or-later, Copyright (c) 2012- PPSSPP Project); the cipher itself is the vendored
// third_party/kirk/Core/HLE/sceChnnlsv.cpp. Pure: no guest memory, no files, no logging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp_save_crypt {

// Crypt mode a save was written with, from the first byte of its PARAM.SFO SAVEDATA_PARAMS
// (PPSSPP SavedataParam::GetSaveCryptMode): 0 plain, 1, 3 or 5. Empty params -> 0.
int mode_from_params(const uint8_t* params, size_t size);

// Mode a request implies (PPSSPP SavedataParam::DetermineCryptMode): `full_request` is a
// 1536-byte SceUtilitySavedataParam, `sdk_version` the game's compiled SDK version.
int request_mode(bool full_request, uint32_t secure_version, uint32_t sdk_version);

// Decrypts a whole encrypted save file. `key` is the request's 16-byte key or nullptr (mode 1);
// `expected_hash` the 16-byte hash from SAVEDATA_FILE_LIST or nullptr (no check). Returns 0 and
// the plain bytes in *out, else PPSSPP's negative DecryptData code (-8 = hash mismatch).
int decrypt(int mode, const std::vector<uint8_t>& file, const uint8_t* key,
            const uint8_t* expected_hash, std::vector<uint8_t>* out);

// Encrypts plain bytes the way PPSSPP saves them (used by the tests). Returns 0, the file bytes
// in *out and the 16-byte hash for SAVEDATA_FILE_LIST in hash.
int encrypt(int mode, const std::vector<uint8_t>& plain, const uint8_t* key,
            std::vector<uint8_t>* out, uint8_t hash[16]);

}  // namespace psp_save_crypt
```

- [ ] **Step 4: Failing test.** Create `runtime/tests/test_save_crypt.cpp`:

```cpp
// Unit tests for psp_save_crypt (PPSSPP savedata cipher): crypt-mode tables, encrypt/decrypt
// round trips, hash check, short files, and the user's real PPSSPP save when it is on this PC.
#include "hle/psp_save_crypt.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace psp_save_crypt;

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

static const uint8_t kKey[16] = {0x44, 0x39, 0x56, 0x5a, 0x47, 0x32, 0x32, 0x59,
                                 0x4c, 0x57, 0x53, 0x4a, 0x47, 0x4e, 0x4c, 0x9a};

static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

static std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void test_mode_tables() {
    const uint8_t p0[] = {0x00}, p1[] = {0x01}, p3[] = {0x21}, p5[] = {0x41}, px[] = {0x7F};
    ASSERT_TRUE(mode_from_params(nullptr, 0) == 0, "no params -> plain");
    ASSERT_TRUE(mode_from_params(p0, 1) == 0, "0x00 -> plain");
    ASSERT_TRUE(mode_from_params(p1, 1) == 1, "0x01 -> mode 1");
    ASSERT_TRUE(mode_from_params(p3, 1) == 3, "0x21 -> mode 3");
    ASSERT_TRUE(mode_from_params(p5, 1) == 5, "0x41 -> mode 5");
    ASSERT_TRUE(mode_from_params(px, 1) == 1, "unknown non-zero -> mode 1");
    ASSERT_TRUE(request_mode(false, 0, 0x06060010) == 1, "short request -> mode 1");
    ASSERT_TRUE(request_mode(true, 0, 0x06060010) == 5, "secureVersion 0, new SDK -> 5");
    ASSERT_TRUE(request_mode(true, 0, 0x02050000) == 3, "secureVersion 0, old SDK -> 3");
    ASSERT_TRUE(request_mode(true, 1, 0x06060010) == 1, "secureVersion 1 -> 1");
    ASSERT_TRUE(request_mode(true, 2, 0x06060010) == 3, "secureVersion 2 -> 3");
    ASSERT_TRUE(request_mode(true, 3, 0x06060010) == 5, "secureVersion 3, new SDK -> 5");
    ASSERT_TRUE(request_mode(true, 3, 0x02050000) == 1, "secureVersion 3, old SDK -> 1");
}

static void test_round_trips() {
    for (int mode : {1, 3, 5}) {
        for (size_t n : {size_t{1}, size_t{16}, size_t{5000}, size_t{178848}}) {
            std::vector<uint8_t> plain = pattern(n), enc, dec;
            uint8_t hash[16];
            const uint8_t* key = mode > 1 ? kKey : nullptr;
            int e = encrypt(mode, plain, key, &enc, hash);
            ASSERT_TRUE(e == 0 && enc.size() == n + 16, "encrypt adds the 16-byte IV");
            ASSERT_TRUE(enc != plain, "encrypted bytes differ");
            int d = decrypt(mode, enc, key, hash, &dec);
            ASSERT_TRUE(d == 0 && dec == plain, "decrypt with the hash returns the plain bytes");
            dec.clear();
            ASSERT_TRUE(decrypt(mode, enc, key, nullptr, &dec) == 0 && dec == plain,
                        "decrypt without a hash too");
        }
    }
}

static void test_hash_and_short_files() {
    std::vector<uint8_t> plain = pattern(4096), enc, dec;
    uint8_t hash[16];
    ASSERT_TRUE(encrypt(5, plain, kKey, &enc, hash) == 0, "encrypt");
    uint8_t wrong[16];
    std::memset(wrong, 0xEE, sizeof(wrong));
    ASSERT_TRUE(decrypt(5, enc, kKey, wrong, &dec) == -8, "wrong hash -> -8");
    ASSERT_TRUE(dec.empty(), "nothing returned on failure");
    std::vector<uint8_t> sixteen(enc.begin(), enc.begin() + 16);
    ASSERT_TRUE(decrypt(5, sixteen, kKey, nullptr, &dec) == -1, "16 bytes (IV only) -> -1");
    ASSERT_TRUE(decrypt(5, std::vector<uint8_t>(), kKey, nullptr, &dec) == -1, "empty -> -1");
}

// The user's PPSSPP save (decrypted once with PPSSPP's code on 2026-10-16). No game files in the
// repo: skipped when the folder is not on this PC.
static void test_real_save() {
    const fs::path deco = PSP_DECO_DIR;
    const fs::path enc_path = deco / "build/save_main/original_DATA04/SECURE.BIN";
    const fs::path ref_path = deco / "build/save_main/dec_DATA04.bin";
    if (!fs::exists(enc_path) || !fs::exists(ref_path)) {
        std::printf("SKIP: %s not on this PC\n", enc_path.string().c_str());
        return;
    }
    std::vector<uint8_t> enc = slurp(enc_path), ref = slurp(ref_path), dec;
    ASSERT_TRUE(enc.size() == 178864 && ref.size() == 178848, "real save sizes");
    ASSERT_TRUE(decrypt(5, enc, kKey, nullptr, &dec) == 0, "real save decrypts");
    ASSERT_TRUE(dec == ref, "real save matches PPSSPP's decryption byte for byte");
}

int main() {
    test_mode_tables();
    test_round_trips();
    test_hash_and_short_files();
    test_real_save();
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
```

  And in `runtime/CMakeLists.txt`, right after the `eboot_loader_tests` block (line ~665):

```cmake
# Savedata cipher (PPSSPP sceChnnlsv, vendored in kirk) behind psp_save_crypt: round trips, hash
# check, and the user's PPSSPP save when it is on this PC (skipped otherwise).
add_executable(test_save_crypt tests/test_save_crypt.cpp src/hle/psp_save_crypt.cpp)
target_include_directories(test_save_crypt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_save_crypt PRIVATE kirk)
target_compile_definitions(test_save_crypt PRIVATE PSP_DECO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(test_save_crypt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME save_crypt_tests COMMAND test_save_crypt)
```

  Run: `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_save_crypt 2>&1 | tail -3`
  Expected: FAIL to build (`psp_save_crypt.cpp` does not exist).

- [ ] **Step 5: Implementation.** Create `runtime/src/hle/psp_save_crypt.cpp`:

```cpp
// See psp_save_crypt.h. DecryptData / EncryptData / GetSDKMainVersion are PPSSPP's
// (Core/Dialog/SavedataParam.cpp), unchanged apart from being free functions.
#include "hle/psp_save_crypt.h"

#include <cstring>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/HLE/sceChnnlsv.h"

namespace psp_save_crypt {
namespace {

void init_kirk_once() {
    static std::once_flag once;
    std::call_once(once, [] { kirk_init(__ChnnlsvKirkState()); });
}

int align16(int n) { return (n + 15) & ~15; }

int GetSDKMainVersion(int sdkVersion) {
    if (sdkVersion > 0x0307FFFF) return 6;
    if (sdkVersion > 0x0300FFFF) return 5;
    if (sdkVersion > 0x0206FFFF) return 4;
    if (sdkVersion > 0x0205FFFF) return 3;
    if (sdkVersion >= 0x02000000) return 2;
    if (sdkVersion >= 0x01000000) return 1;
    return 0;
}

// Note: The work is done in-place, hence the memmove etc.
int EncryptData(unsigned int mode, unsigned char *data, int *dataLen, int *alignedLen, unsigned char *hash, const u8 *cryptkey) {
	pspChnnlsvContext1 ctx1{};
	pspChnnlsvContext2 ctx2{};

	/* Make room for the IV in front of the data. */
	memmove(data + 0x10, data, *alignedLen);

	/* Set up buffers */
	memset(hash, 0, 0x10);

	// Zero out the IV before we begin.
	memset(data, 0, 0x10);

	/* Build the 0x10-byte IV and setup encryption */
	if (sceSdCipherInit(ctx2, mode, 1, data, cryptkey) < 0)
		return -1;
	if (sceSdMacInit(ctx1, mode) < 0)
		return -2;
	if (sceSdMacUpdate(ctx1, data, 0x10) < 0)
		return -3;
	if (sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0)
		return -4;

	/* Clear any extra bytes left from the previous steps */
	memset(data + 0x10 + *dataLen, 0, *alignedLen - *dataLen);

	/* Encrypt the data */
	if (sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0)
		return -5;

	/* Verify encryption */
	if (sceSdCipherFinal(ctx2) < 0)
		return -6;

	/* Build the file hash from this PSP */
	if (sceSdMacFinal(ctx1, hash, cryptkey) < 0)
		return -7;

	/* Adjust sizes to account for IV */
	*alignedLen += 0x10;
	*dataLen += 0x10;

	/* All done */
	return 0;
}

// Note: The work is done in-place, hence the memmove etc.
int DecryptData(unsigned int mode, unsigned char *data, int *dataLen, int *alignedLen, const u8 *cryptkey, const u8 *expectedHash) {
	pspChnnlsvContext1 ctx1{};
	pspChnnlsvContext2 ctx2{};

	/* Need a 16-byte IV plus some data */
	if (*alignedLen <= 0x10)
		return -1;
	*dataLen -= 0x10;
	*alignedLen -= 0x10;

	/* Perform the magic */
	if (sceSdMacInit(ctx1, mode) < 0)
		return -2;
	if (sceSdCipherInit(ctx2, mode, 2, data, cryptkey) < 0)
		return -3;
	if (sceSdMacUpdate(ctx1, data, 0x10) < 0)
		return -4;
	if (sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0)
		return -5;
	if (sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0)
		return -6;

	/* Verify that it decrypted correctly */
	if (sceSdCipherFinal(ctx2) < 0)
		return -7;

	if (expectedHash) {
		u8 hash[16];
		if (sceSdMacFinal(ctx1, hash, cryptkey) < 0)
			return -7;
		if (memcmp(hash, expectedHash, sizeof(hash)) != 0)
			return -8;
	}

	/* The decrypted data starts at data + 0x10, so shift it back. */
	memmove(data, data + 0x10, *dataLen);
	return 0;
}

}  // namespace

int mode_from_params(const uint8_t* params, size_t size) {
    if (params == nullptr || size == 0) return 0;
    switch (params[0]) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0x21: return 3;
        case 0x41: return 5;
        default: return 1;  // PPSSPP: "Well, it's not zero, so yes."
    }
}

int request_mode(bool full_request, uint32_t secure_version, uint32_t sdk_version) {
    if (!full_request) return 1;
    const bool new_hash = GetSDKMainVersion(static_cast<int>(sdk_version)) >= 4;
    switch (secure_version) {
        case 0: return new_hash ? 5 : 3;
        case 2: return 3;
        case 3: return new_hash ? 5 : 1;
        default: return 1;
    }
}

int decrypt(int mode, const std::vector<uint8_t>& file, const uint8_t* key,
            const uint8_t* expected_hash, std::vector<uint8_t>* out) {
    init_kirk_once();
    if (file.size() > 0x7FFFFFF0u) return -1;
    int len = static_cast<int>(file.size());
    int aligned = align16(len);
    std::vector<uint8_t> buf(static_cast<size_t>(aligned), 0);
    if (len > 0) std::memcpy(buf.data(), file.data(), file.size());
    uint8_t k[16];
    if (key != nullptr) std::memcpy(k, key, 16);
    int err = DecryptData(static_cast<unsigned>(mode), buf.data(), &len, &aligned,
                          key != nullptr ? k : nullptr, expected_hash);
    if (err != 0) return err;
    out->assign(buf.begin(), buf.begin() + len);
    return 0;
}

int encrypt(int mode, const std::vector<uint8_t>& plain, const uint8_t* key,
            std::vector<uint8_t>* out, uint8_t hash[16]) {
    init_kirk_once();
    if (plain.size() > 0x7FFFFFE0u) return -1;
    int len = static_cast<int>(plain.size());
    int aligned = align16(len);
    std::vector<uint8_t> buf(static_cast<size_t>(aligned) + 0x10, 0);
    if (len > 0) std::memcpy(buf.data(), plain.data(), plain.size());
    uint8_t k[16];
    if (key != nullptr) std::memcpy(k, key, 16);
    int err = EncryptData(static_cast<unsigned>(mode), buf.data(), &len, &aligned, hash,
                          key != nullptr ? k : nullptr);
    if (err != 0) return err;
    out->assign(buf.begin(), buf.begin() + len);
    return 0;
}

}  // namespace psp_save_crypt
```

  In `runtime/CMakeLists.txt`, the kirk library (line ~290) becomes:

```cmake
add_library(kirk STATIC ${KIRK_C_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/Core/ELF/PrxDecrypter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/Core/HLE/sceChnnlsv.cpp)
```
  and the comment above it gains one line: `# Also PPSSPP's savedata cipher (Core/HLE/sceChnnlsv.cpp), used by src/hle/psp_save_crypt.cpp.`

- [ ] **Step 6: Tests pass.** Run the runtime gate. Expected: `test_save_crypt` prints
  `... tests run, 0 failures` (no `SKIP` line on the user's PC: the real save is there), ctest
  **75/75** (74 + `save_crypt_tests`). Paste the real numbers.

- [ ] **Step 7: README.** In `runtime/third_party/kirk/README.md`: add to the copied files
  `Core/HLE/sceChnnlsv.h` (verbatim) and `Core/HLE/sceChnnlsv.cpp` (generated by
  `vendor_chnnlsv.py` from PPSSPP commit `<commit from Step 2>`: the six guest-pointer `sceSd*`
  wrappers, the module tables and `Register_*` removed, includes replaced by the shims; the cipher is
  unchanged); licence line `Core/HLE/sceChnnlsv.*: PPSSPP, GPL-2.0-or-later`; and replace the
  "psprecomp uses only `pspDecryptPRX`" paragraph with: psprecomp uses `pspDecryptPRX` (boot file, see
  above) and the `sceSd*` cipher through `src/hle/psp_save_crypt.cpp` to read saves PPSSPP or a PSP
  wrote encrypted (decrypted in memory; saves are always written in plaintext).

- [ ] **Step 8: Report (no commit).** Report: files, the commit id of Step 2, test output,
  ctest count. Claude commits: `feat(runtime): vendored PPSSPP savedata cipher (psp_save_crypt)`.

---

### Task 2: Savedata loads encrypted saves; `PSPRECOMP_SAVE_SLOT` (runtime)

**Files (psprecomp):**
- Modify: `runtime/include/hle/psp_savedata.h` (header comment, `Options`, `Outcome`)
- Modify: `runtime/src/hle/psp_savedata.cpp` (`load()`, `list_load()`, `files_list()`, notes, includes)
- Modify: `runtime/src/hle/psp_hle_utility.cpp:1305-1315` (preferred save, print notes)
- Modify: `runtime/CMakeLists.txt` (runtime source list near line 213; `test_savedata` and
  `test_gamedata_install` gain `src/hle/psp_save_crypt.cpp` and link `kirk`)
- Modify: `runtime/tests/test_savedata.cpp` (new tests)
- Modify: `ARCHITECTURE.md` (Savedata row), `README.md`, `docs/ENV_FLAGS.md`

**Interfaces:**
- Consumes: Task 1 `psp_save_crypt::{mode_from_params, request_mode, decrypt, encrypt}`.
- Produces: `psp_savedata::Options::preferred_save` (`std::string`, empty = none);
  `psp_savedata::Outcome::notes` (`std::vector<std::string>`, one log line each, printed by the
  utility as `[SAVEDATA] <note>`); env var `PSPRECOMP_SAVE_SLOT`.

- [ ] **Step 1: Failing tests.** In `runtime/tests/test_savedata.cpp`: add `#include "hle/psp_save_crypt.h"`
  after the savedata include, then these helpers and tests before `int main()`:

```cpp
// ---- encrypted saves (PPSSPP / real PSP) ------------------------------------

static void write_all(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

static bool has_note(const Outcome& o, const char* a, const char* b = "") {
    for (const std::string& n : o.notes) {
        if (n.find(a) != std::string::npos && n.find(b) != std::string::npos) return true;
    }
    return false;
}

// Turns the plain save `save` (written by AUTOSAVE, data file DATA.BIN) into what PPSSPP writes
// with encryption on: DATA.BIN encrypted in mode 5 with make_param's key, SAVEDATA_PARAMS[0] =
// 0x41 and the file-list hash (the right one, or garbage with good_hash = false).
static void encrypt_save(const char* save, bool good_hash) {
    const fs::path dir = save_dir(save);
    std::vector<uint8_t> plain = read_file(dir / "DATA.BIN");
    uint8_t key[16] = {0x42};
    std::vector<uint8_t> enc;
    uint8_t hash[16];
    ASSERT_EQ(psp_save_crypt::encrypt(5, plain, key, &enc, hash), 0, "test encryption");
    write_all(dir / "DATA.BIN", enc);
    std::vector<uint8_t> raw = read_file(dir / "PARAM.SFO");
    Psf sfo;
    ASSERT_TRUE(Psf::parse(raw.data(), raw.size(), &sfo), "seed SFO parses");
    std::vector<uint8_t> params(128, 0);
    params[0] = 0x41;
    sfo.set_binary("SAVEDATA_PARAMS", params.data(), params.size(), 128);
    std::vector<uint8_t> list = *sfo.get_binary("SAVEDATA_FILE_LIST");
    ASSERT_TRUE(std::strncmp(reinterpret_cast<const char*>(list.data()), "DATA.BIN", 13) == 0,
                "file list entry 0 is DATA.BIN");
    if (good_hash) std::memcpy(list.data() + 13, hash, 16);
    else std::memset(list.data() + 13, 0xEE, 16);
    sfo.set_binary("SAVEDATA_FILE_LIST", list.data(), list.size(),
                   static_cast<uint32_t>(list.size()));
    write_all(dir / "PARAM.SFO", sfo.serialize());
}

static void test_load_encrypted_save() {
    Ram r;
    ASSERT_EQ(autosave(r, "E1", 0x1234, 0x5A), 0, "seed plain save");
    std::vector<uint8_t> plain = read_file(save_dir("E1") / "DATA.BIN");
    encrypt_save("E1", true);
    ASSERT_EQ(fs::file_size(save_dir("E1") / "DATA.BIN"), 0x1234 + 16,
              "the encrypted file is 16 bytes longer");

    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "E1");
    set_buf(r2, DST, 0x1234, 0);  // exactly the plain size, like the game's own buffer
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "AUTOLOAD of an encrypted save");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 0x1234, "dataSize is the plain size");
    ASSERT_TRUE(std::memcmp(r2.at(DST), plain.data(), plain.size()) == 0,
                "the game gets the plain bytes");
    ASSERT_TRUE(has_note(o, "decrypted", "mode 5"), "decryption is reported");
    ASSERT_TRUE(!has_note(o, "hash mismatch"), "the right hash is accepted");

    // Saving over it writes plain again (the save path is unchanged).
    ASSERT_EQ(autosave(r, "E1", 0x200, 0x33), 0, "save over the encrypted save");
    std::vector<uint8_t> raw = read_file(save_dir("E1") / "PARAM.SFO");
    Psf sfo;
    ASSERT_TRUE(Psf::parse(raw.data(), raw.size(), &sfo), "SFO parses after the save");
    const std::vector<uint8_t>* params = sfo.get_binary("SAVEDATA_PARAMS");
    ASSERT_TRUE(params != nullptr && !params->empty() && (*params)[0] == 0,
                "SAVEDATA_PARAMS zeroed again: plain");
    Ram r3;
    make_param(r3, MODE_AUTOLOAD, "E1");
    set_buf(r3, DST, 0x1000, 0);
    Outcome o3 = run(r3);
    ASSERT_EQ(res(o3), 0, "the re-saved plain save loads");
    ASSERT_EQ(r3.u32(P + off::kDataSize), 0x200, "plain size after the re-save");
    ASSERT_TRUE(!has_note(o3, "decrypted"), "nothing decrypted for a plain save");
}

static void test_load_encrypted_bad_hash() {
    Ram r;
    ASSERT_EQ(autosave(r, "E2", 0x800, 0x21), 0, "seed plain save");
    std::vector<uint8_t> plain = read_file(save_dir("E2") / "DATA.BIN");
    encrypt_save("E2", false);
    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "E2");
    set_buf(r2, DST, 0x800, 0);
    Outcome o = run(r2);
    ASSERT_EQ(res(o), 0, "a hash mismatch still loads (PPSSPP retries without it)");
    ASSERT_TRUE(std::memcmp(r2.at(DST), plain.data(), plain.size()) == 0, "plain bytes");
    ASSERT_TRUE(has_note(o, "decrypted", "hash mismatch ignored"), "the mismatch is reported");
}

static void test_load_encrypted_too_short() {
    Ram r;
    ASSERT_EQ(autosave(r, "E3", 0x100, 0x10), 0, "seed plain save");
    encrypt_save("E3", true);
    std::vector<uint8_t> enc = read_file(save_dir("E3") / "DATA.BIN");
    write_all(save_dir("E3") / "DATA.BIN", std::vector<uint8_t>(enc.begin(), enc.begin() + 16));
    Ram r2;
    make_param(r2, MODE_AUTOLOAD, "E3");
    set_buf(r2, DST, 0x1000, 0);
    std::memset(r2.at(DST), 0xCC, 16);
    Outcome o = run(r2);
    ASSERT_EQ(res(o), ERR_LOAD_DATA_BROKEN, "an IV-only file is broken data");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 0, "dataSize stays 0");
    ASSERT_EQ(r2.at(DST)[0], 0xCC, "the game's buffer is untouched (never ciphertext)");
    ASSERT_TRUE(has_note(o, "cannot decrypt"), "the failure is reported");
    Ram r3;
    make_param(r3, MODE_READDATASECURE, "E3");
    set_buf(r3, DST, 0x1000, 0);
    ASSERT_EQ(res(run(r3)), ERR_RW_DATA_BROKEN, "READDATASECURE maps it to RW_DATA_BROKEN");
}

static void test_files_list_encrypted_size() {
    Ram r;
    ASSERT_EQ(autosave(r, "E4", 100, 0x44), 0, "seed plain save");
    encrypt_save("E4", true);
    Ram r2;
    make_param(r2, MODE_FILES, "E4");
    r2.w32(P + off::kFileList, AUX);
    r2.w32(AUX + off::kFlMaxSecure, 8);
    r2.w32(AUX + off::kFlMaxNormal, 8);
    r2.w32(AUX + off::kFlMaxSystem, 5);
    r2.w32(AUX + off::kFlSecureEntries, ENT_S);
    r2.w32(AUX + off::kFlNormalEntries, ENT_N);
    r2.w32(AUX + off::kFlSystemEntries, ENT_Y);
    ASSERT_EQ(res(run(r2)), 0, "FILES on an encrypted save");
    ASSERT_EQ(r2.u32(AUX + off::kFlResultSecure), 1, "one secure entry");
    ASSERT_EQ(r2.u64(ENT_S + off::kFlEntrySize64), 100,
              "the secure file reports its plain size (file size - 16), like PPSSPP");
}

static void test_listload_preferred_save() {
    Ram r;
    ASSERT_EQ(autosave(r, "Q0", 40, 0x01), 0, "older save");
    ASSERT_EQ(autosave(r, "Q1", 50, 0x02), 0, "newer save");
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(save_dir("Q0") / "PARAM.SFO", old_time);
    fs::last_write_time(save_dir("Q0") / "DATA.BIN", old_time);
    fs::last_write_time(save_dir("Q0"), old_time);

    Options chosen = g_opts;
    chosen.preferred_save = "Q0";
    Ram r2;
    make_param(r2, MODE_LISTLOAD, "Q1");
    set_name_list(r2, {"Q0", "Q1"});
    set_buf(r2, DST, 0x1000, 0);
    Outcome o = execute(r2.v.data(), r2.v.size(), P, chosen);
    ASSERT_EQ(res(o), 0, "LISTLOAD with a chosen save");
    ASSERT_TRUE(r2.str(P + off::kSaveName, 20) == "Q0", "the chosen (older) save is loaded");
    ASSERT_EQ(r2.u32(P + off::kDataSize), 40, "its data");
    ASSERT_TRUE(has_note(o, "LISTLOAD picks Q0", "PSPRECOMP_SAVE_SLOT"), "the pick is reported");

    Options missing = g_opts;
    missing.preferred_save = "Q9";
    Ram r3;
    make_param(r3, MODE_LISTLOAD, "Q1");
    set_name_list(r3, {"Q0", "Q1"});
    set_buf(r3, DST, 0x1000, 0);
    ASSERT_EQ(res(execute(r3.v.data(), r3.v.size(), P, missing)), 0, "LISTLOAD, chosen save missing");
    ASSERT_TRUE(r3.str(P + off::kSaveName, 20) == "Q1", "falls back to the newest");

    Ram r4;
    make_param(r4, MODE_AUTOLOAD, "Q1");
    set_buf(r4, DST, 0x1000, 0);
    ASSERT_EQ(res(execute(r4.v.data(), r4.v.size(), P, chosen)), 0, "AUTOLOAD ignores the choice");
    ASSERT_EQ(r4.u32(P + off::kDataSize), 50, "AUTOLOAD loads the named save");
}
```
  and call them at the end of `main()`'s test list (after `test_bad_pointers();`):

```cpp
    test_load_encrypted_save();
    test_load_encrypted_bad_hash();
    test_load_encrypted_too_short();
    test_files_list_encrypted_size();
    test_listload_preferred_save();
```
  In `runtime/CMakeLists.txt` change the two test targets that compile `psp_savedata.cpp`:

```cmake
add_executable(test_savedata tests/test_savedata.cpp src/hle/psp_savedata.cpp src/hle/psp_save_crypt.cpp src/psp_host_io.cpp)
target_link_libraries(test_savedata PRIVATE kirk)
```
  (keep its include/compile-option lines), and in `test_gamedata_install` add
  `src/hle/psp_save_crypt.cpp` after `src/hle/psp_savedata.cpp` plus
  `target_link_libraries(test_gamedata_install PRIVATE kirk)` (if the target already has a
  `target_link_libraries`, add `kirk` to it). In the runtime's own source list (line ~213) add
  `src/hle/psp_save_crypt.cpp` right after `src/hle/psp_savedata.cpp` (the runtime already links `kirk`).

  Run: `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_savedata 2>&1 | tail -3`
  Expected: FAIL to compile (`Outcome` has no `notes`, `Options` has no `preferred_save`).

- [ ] **Step 2: Header.** In `runtime/include/hle/psp_savedata.h`:
  - Replace the first deviation bullet ("Savedata encryption/hashing is not ported. ...") with:

```cpp
//  - Encrypted saves (written by PPSSPP or a real PSP: PARAM.SFO SAVEDATA_PARAMS != 0) are
//    decrypted on load in the secure modes with the request's key (psp_save_crypt, PPSSPP's
//    LoadCryptedSave retries included). Saves are always written in plaintext:
//    SAVEDATA_FILE_LIST still records which files are secure, its hash and SAVEDATA_PARAMS are
//    zero. Saves written here therefore load in PPSSPP but not on real hardware.
```
  - In the LISTLOAD bullet, after "(no existing save -> LOAD_NO_DATA)" add
    "; Options::preferred_save (PSPRECOMP_SAVE_SLOT) picks an existing entry instead".
  - `struct Options` gains, after `sdk_version`:

```cpp
    // LISTLOAD loads this list entry (a save name such as "_DATA04") when it exists, instead
    // of the newest one; empty = newest. Other modes ignore it. ($PSPRECOMP_SAVE_SLOT)
    std::string preferred_save;
```
  - `struct Outcome` gains, after `summary`:

```cpp
    std::vector<std::string> notes;  // extra log lines (decryption, the chosen save)
```

- [ ] **Step 3: Load path.** In `runtime/src/hle/psp_savedata.cpp`:
  - add `#include "hle/psp_save_crypt.h"` after the savedata include;
  - in `class Savedata`, public section after `used_save()`, add
    `const std::vector<std::string>& notes() const { return notes_; }`, and next to the other members
    at the bottom add `std::vector<std::string> notes_;`;
  - in `load()`, replace the whole `if (!file_.empty()) { ... }` block that reads the data file
    (from `uint64_t fsize = fs::file_size(file_path, ec);` to the closing brace after
    `set_field32(off::kDataSize, ...)`) with:

```cpp
        if (!file_.empty()) {
            uint64_t fsize = fs::file_size(file_path, ec);
            if (ec || fsize == 0) return ERR_LOAD_NO_DATA;  // nothing to read
            set_field_str(off::kSaveName, 20, save_dir);

            std::vector<uint8_t> bytes;
            if (!read_file(file_path, fsize, &bytes) || bytes.empty()) {
                return ERR_LOAD_NO_DATA;
            }
            // Decrypt before the size check: the game's buffer fits the plain data, which is
            // 16 bytes shorter than an encrypted file.
            if (secure) {
                int32_t r = decrypt_if_encrypted(dir, save_dir, &bytes);
                if (r != 0) return r;
            }
            uint32_t data_buf = field32(off::kDataBuf);
            uint32_t buf_size = field32(off::kDataBufSize);
            if (data_buf != 0) {
                if (bytes.size() > buf_size) return ERR_LOAD_DATA_BROKEN;
                uint8_t* dst = m_.range(data_buf, bytes.size());
                if (dst == nullptr) return ERR_LOAD_DATA_BROKEN;
                std::memcpy(dst, bytes.data(), bytes.size());
            }
            set_field32(off::kDataSize, static_cast<uint32_t>(bytes.size()));
        }
```
  - add these private members right before `int32_t load(...)`:

```cpp
    // The SAVEDATA_FILE_LIST hash recorded for the data file (PPSSPP GetExpectedHash).
    bool expected_hash(const Psf& sfo, uint8_t hash[16]) const {
        const std::vector<uint8_t>* list = sfo.get_binary("SAVEDATA_FILE_LIST");
        if (list == nullptr || file_.empty()) return false;
        for (size_t o = 0; o + kFileListEntrySize <= list->size(); o += kFileListEntrySize) {
            const char* name = reinterpret_cast<const char*>(list->data() + o);
            if (std::strncmp(name, file_.c_str(), 13) == 0) {
                std::memcpy(hash, list->data() + o + 13, 16);
                return true;
            }
        }
        return false;
    }

    // PPSSPP SavedataParam::LoadCryptedSave: the save's PARAM.SFO says whether (and with which
    // mode) the data file is encrypted. Tries the file's mode, then the mode the request implies,
    // then the file's mode without the hash check. Plain saves are left alone.
    int32_t decrypt_if_encrypted(const fs::path& dir, const std::string& save_dir,
                                 std::vector<uint8_t>* bytes) {
        Psf sfo;
        std::vector<uint8_t> raw;
        if (!read_file(dir / kSfoName, 16 * 1024 * 1024, &raw) || raw.empty() ||
            !Psf::parse(raw.data(), raw.size(), &sfo)) {
            return 0;  // the LoadSFO step below reports a broken PARAM.SFO
        }
        const std::vector<uint8_t>* params = sfo.get_binary("SAVEDATA_PARAMS");
        const int file_mode = params == nullptr ? 0
            : psp_save_crypt::mode_from_params(params->data(), params->size());
        if (file_mode == 0) return 0;
        const int detected = psp_save_crypt::request_mode(
            psize_ >= off::kParamSize, field32(off::kSecureVersion), opts_.sdk_version);
        uint8_t key[16] = {};
        if (in_param(off::kKey, 16)) {
            const uint8_t* k = m_.range(p_ + off::kKey, 16);
            if (k != nullptr) std::memcpy(key, k, 16);
        }
        uint8_t hash[16] = {};
        const bool has_hash = expected_hash(sfo, hash);
        std::vector<uint8_t> plain;
        auto attempt = [&](int mode, bool use_hash) {
            plain.clear();
            return psp_save_crypt::decrypt(mode, *bytes, mode > 1 ? key : nullptr,
                                           use_hash ? hash : nullptr, &plain);
        };
        int used = file_mode;
        bool hash_ignored = false;
        int err = attempt(file_mode, has_hash);
        if (err != 0 && detected != file_mode) {
            used = detected;
            err = attempt(detected, has_hash);
        }
        if (err != 0 && has_hash) {
            used = file_mode;
            hash_ignored = true;
            err = attempt(file_mode, false);
        }
        const std::string what = dir_name(save_dir) + "/" + file_;
        if (err != 0) {
            notes_.push_back("cannot decrypt " + what + " (mode " + std::to_string(file_mode) +
                             ", error " + std::to_string(err) + ")");
            return static_cast<int32_t>(ERR_LOAD_DATA_BROKEN);
        }
        notes_.push_back("decrypted " + what + " mode " + std::to_string(used) + " (" +
                         std::to_string(bytes->size()) + " -> " + std::to_string(plain.size()) +
                         " bytes" + (hash_ignored ? ", hash mismatch ignored like PPSSPP" : "") +
                         ")");
        bytes->swap(plain);
        return 0;
    }
```

- [ ] **Step 4: Chosen save, FILES size, notes out.**
  - `list_load()`: right after `if (items.empty()) return ERR_LOAD_NO_DATA;` insert:

```cpp
        if (!opts_.preferred_save.empty()) {
            for (const ListItem& it : items) {
                if (it.name == opts_.preferred_save) {
                    notes_.push_back("LISTLOAD picks " + it.name + " (PSPRECOMP_SAVE_SLOT)");
                    return load(it.name, true, false);
                }
            }
        }
```
  - `files_list()`: after `secure_names(sfo, &secure);` add

```cpp
        // PPSSPP GetFilesList: an encrypted save's secure files report their plain size.
        const std::vector<uint8_t>* params = sfo.get_binary("SAVEDATA_PARAMS");
        const bool crypted = params != nullptr &&
            psp_save_crypt::mode_from_params(params->data(), params->size()) != 0;
```
    in the loop, introduce `bool is_secure = false;` next to `uint32_t* count = &n_normal;`, set
    `is_secure = true;` in the `secure` branch, and replace
    `m_.wr64(entry + off::kFlEntrySize64, f.size);` with

```cpp
            uint64_t size = f.size;
            if (crypted && is_secure && size >= 0x10) size -= 0x10;
            m_.wr64(entry + off::kFlEntrySize64, size);
```
  - `execute()`: replace the last two lines (`int32_t result = sd.run();` /
    `return finish(result, mode, sd.game(), sd.used_save());`) with

```cpp
    int32_t result = sd.run();
    out.notes = sd.notes();
    return finish(result, mode, sd.game(), sd.used_save());
```
  - `runtime/src/hle/psp_hle_utility.cpp` `savedata_complete()`: after
    `opts.sdk_version = psp_kernel_compiled_sdk_version();` add

```cpp
        // The launcher's LOAD screen: which save the game's Continue (LISTLOAD) loads.
        if (const char* slot = std::getenv("PSPRECOMP_SAVE_SLOT")) opts.preferred_save = slot;
```
    and after the existing `fprintf(stderr, "[HLE] sceUtilitySavedata %s\n", ...)` add

```cpp
        for (const std::string& note : out.notes) {
            fprintf(stderr, "[SAVEDATA] %s\n", note.c_str());
        }
```
    (add `#include <cstdlib>` if the file does not include it yet).

- [ ] **Step 5: Tests pass.** Runtime gate. Expected: `test_savedata` `... 0 failures`, ctest **75/75**.
  Paste the real numbers.

- [ ] **Step 6: Docs.**
  - `ARCHITECTURE.md` Savedata row: replace "Data is stored in plaintext (PSP savedata encryption is not
    ported)." with "Saves are written in plaintext; encrypted saves (PPSSPP / PSP, `SAVEDATA_PARAMS`
    non-zero) are decrypted on load with the request key through `hle/psp_save_crypt` (PPSSPP's
    sceChnnlsv cipher vendored in `third_party/kirk`)." and after "list modes auto-pick" add
    "(`PSPRECOMP_SAVE_SLOT` chooses the entry `LISTLOAD` loads; the launcher's Saves screen sets it)".
  - `README.md`: where Patapon 3 saves / `PSPRECOMP_SAVEDATA` are described, add one sentence: PPSSPP
    saves (encrypted) load as they are when copied into the savedata folder; saves are written plain,
    which PPSSPP also reads. (Use `grep -n "SAVEDATA\|savedata" README.md` to find the place.)
  - `docs/ENV_FLAGS.md`: after the `PSPRECOMP_SAVEDATA` section add, in the same format:

```markdown
### `PSPRECOMP_SAVE_SLOT`

Save name (the part after the game id, e.g. `_DATA04`) that `LISTLOAD` loads when it is in the
game's list and exists, instead of the newest save. Patapon 3's Continue uses `LISTLOAD`; the
launcher's Saves screen sets this variable. Unset or missing save: the newest, as before. Logs
`[SAVEDATA] LISTLOAD picks _DATA04 (PSPRECOMP_SAVE_SLOT)`.
```

- [ ] **Step 7: Report (no commit).** Files, test output, ctest count. Claude commits:
  `feat(runtime): load encrypted PPSSPP saves; PSPRECOMP_SAVE_SLOT picks the LISTLOAD save`.

---

### Task 3: `release.json` save facts; rebuild the three exes (psprecomp)

**Files (psprecomp):**
- Create: `games/patapon3/release_saves.json`
- Modify: `scripts/release_info.py`, `games/patapon3/scripts/p3.sh:173-174`,
  `games/patapon3/scripts/test_p3.sh` (test 9)

**Interfaces:**
- Produces: `release.json` key `"saves": {"id", "import_from", "system", "slot_prefix",
  "first_slot", "last_slot"}` in `DECO/build/rtw`, `DECO/build/v115b/rtw`, `DECO/build/vanilla/rtw`.

- [ ] **Step 1: Failing test.** In `games/patapon3/scripts/test_p3.sh` test 9, extend the condition
  with `&& "$rel" == *'"saves"'* && "$rel" == *'"id": "UCES01421"'* && "$rel" == *'"last_slot": 10'*`
  (keep everything else). Run the test_p3 gate. Expected: `1 failure(s)` (test 9).

- [ ] **Step 2: Implementation.** Create `games/patapon3/release_saves.json`:

```json
{
  "id": "UCES01421",
  "import_from": ["UCUS98751"],
  "system": "_DATA00",
  "slot_prefix": "_DATA",
  "first_slot": 1,
  "last_slot": 10
}
```
  `scripts/release_info.py`: usage line becomes
  `Usage: python -I scripts/release_info.py <disc0> <id> <name> <out.json> [<saves.json>]` and add to the
  docstring: `saves.json (optional): the game's save facts for the launcher, copied as "saves".`;
  `main()` accepts 5 or 6 arguments:

```python
    if len(sys.argv) not in (5, 6):
        print(__doc__, file=sys.stderr)
        return 2
    disc0, rid, name, out = sys.argv[1:5]
```
  and before `with open(out, "w", ...)`:

```python
    if len(sys.argv) == 6:
        with open(sys.argv[5], encoding="utf-8") as f:
            info["saves"] = json.load(f)
```
  `p3.sh` line 174 passes the file as the 5th argument:
  `python -I "$REPO/scripts/release_info.py" "$DISC0" "$RELEASE_ID" "$RELEASE_NAME" "$BUILD/release.json" "$REPO/games/patapon3/release_saves.json"`

- [ ] **Step 3: Tests pass.** test_p3 gate → `0 failure(s)` (12 PASS).

- [ ] **Step 4: Rebuild the three exes** (they link the Task 2 runtime):

```bash
cd /c/Users/torso/Documents/deco/psprecomp
bash games/patapon3/scripts/p3.sh build 2>&1 | tail -3
P3_VARIANT=v115b bash games/patapon3/scripts/p3.sh build 2>&1 | tail -3
P3_VARIANT=vanilla bash games/patapon3/scripts/p3.sh build 2>&1 | tail -3
grep -c '"last_slot": 10' C:/Users/torso/Documents/deco/build/rtw/release.json C:/Users/torso/Documents/deco/build/v115b/rtw/release.json C:/Users/torso/Documents/deco/build/vanilla/rtw/release.json
```
  Expected: three `release.json: <id> (<disc id>)` lines, each grep count `1`.

- [ ] **Step 5: Regression (Claude runs it).** Runtime gate (ctest 75/75); 1.1.4 tour `p3_t3` vs
  `rebuild114_t1` → `RT_REGRESS PASS`. Claude commits:
  `feat(p3): release.json carries the game's save facts` with the numbers.

---

### Task 4: Launcher — SFO entries, in-place string, `saves` in `release.json`

**Files (launcher):**
- Modify: `src-tauri/src/sfo.rs`, `src-tauri/src/releases.rs`, `src-tauri/src/lib.rs` (test helper only)

**Interfaces:**
- Produces: `sfo::Entry { key: String, fmt: u16, len: usize, max: usize, start: usize }`,
  `sfo::entries(&[u8]) -> Option<Vec<Entry>>`, `sfo::raw(&[u8], &str) -> Option<&[u8]>`,
  `sfo::is_encrypted(&[u8]) -> bool`, `sfo::set_string(&[u8], key: &str, value: &str) -> Option<Vec<u8>>`;
  test helpers `sfo::build_raw(&[(&str, u16, Vec<u8>, u32)]) -> Vec<u8>` and
  `sfo::save_sfo(dir: &str, title: &str, detail: &str, params0: u8) -> Vec<u8>` (`#[cfg(test)] pub`).
  `releases::SavesCfg { id: String, import_from: Vec<String>, system: String, slot_prefix: String,
  first_slot: u32, last_slot: u32 }`; `releases::Release` gains `#[serde(default)] pub saves: Option<SavesCfg>`.

- [ ] **Step 1: Failing tests.** Add to the `tests` module of `src-tauri/src/sfo.rs`:

```rust
    #[test] fn binary_and_encrypted_flag() {
        let plain = save_sfo("UCES01421_DATA04", "Datos guardados", "Héroe: Edson\nClase: Yumiyacha Nv 35", 0x00);
        let enc = save_sfo("UCES01421_DATA04", "Datos guardados", "x", 0x41);
        assert!(!is_encrypted(&plain));
        assert!(is_encrypted(&enc));
        assert_eq!(raw(&enc, "SAVEDATA_PARAMS").unwrap().len(), 128);
        assert!(!is_encrypted(&build(&[("TITLE", "no params")])), "no SAVEDATA_PARAMS -> plain");
        let m = parse(&plain).unwrap();
        assert_eq!(m["SAVEDATA_DETAIL"], "Héroe: Edson\nClase: Yumiyacha Nv 35");
        assert_eq!(m["SAVEDATA_DIRECTORY"], "UCES01421_DATA04");
    }
    #[test] fn set_string_keeps_every_other_byte() {
        let b = save_sfo("UCUS98751_DATA02", "Save data", "Uberhero: MoMo", 0x41);
        let nb = set_string(&b, "SAVEDATA_DIRECTORY", "UCES01421_DATA02").unwrap();
        assert_eq!(nb.len(), b.len());
        assert_eq!(parse(&nb).unwrap()["SAVEDATA_DIRECTORY"], "UCES01421_DATA02");
        assert_eq!(parse(&nb).unwrap()["SAVEDATA_DETAIL"], "Uberhero: MoMo");
        let e = entries(&b).unwrap().into_iter().find(|e| e.key == "SAVEDATA_DIRECTORY").unwrap();
        let differ: Vec<usize> = (0..b.len()).filter(|&i| b[i] != nb[i]).collect();
        assert!(differ.iter().all(|&i| (e.start..e.start + e.max).contains(&i)), "only the value bytes change: {differ:?}");
        let shorter = set_string(&b, "SAVEDATA_DIRECTORY", "UCES01421_DATA5").unwrap();
        let e2 = entries(&shorter).unwrap().into_iter().find(|e| e.key == "SAVEDATA_DIRECTORY").unwrap();
        assert_eq!(e2.len, "UCES01421_DATA5".len() + 1, "len follows the new value");
        assert_eq!(&shorter[e2.start..e2.start + e2.max], &{ let mut v = b"UCES01421_DATA5".to_vec(); v.resize(e2.max, 0); v }[..], "zero padded");
        assert!(set_string(&b, "SAVEDATA_DIRECTORY", &"X".repeat(64)).is_none(), "64 + NUL does not fit 64");
        assert!(set_string(&b, "NOPE", "x").is_none());
        assert!(set_string(&b, "SAVEDATA_PARAMS", "x").is_none(), "not a string");
    }
    #[test] fn real_save_sfo_when_present() {
        let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build/save_main/original_DATA04/PARAM.SFO");
        let Ok(b) = std::fs::read(&p) else { eprintln!("SKIP: {} not on this PC", p.display()); return };
        assert!(is_encrypted(&b));
        let nb = set_string(&b, "SAVEDATA_DIRECTORY", "UCES01421_DATA07").unwrap();
        assert_eq!(nb.len(), b.len());
        let m = parse(&nb).unwrap();
        assert_eq!(m["SAVEDATA_DIRECTORY"], "UCES01421_DATA07");
        assert!(m["SAVEDATA_DETAIL"].contains("Yumiyacha"));
    }
```
  Add to the `tests` module of `src-tauri/src/releases.rs`:

```rust
    #[test] fn saves_config_is_optional() {
        let old: Release = serde_json::from_str(r#"{"id":"a","name":"A","disc_id":"X","eboot_sha256":"0"}"#).unwrap();
        assert_eq!(old.saves, None);
        let new: Release = serde_json::from_str(r#"{"id":"a","name":"A","disc_id":"X","eboot_sha256":"0",
            "saves":{"id":"UCES01421","import_from":["UCUS98751"],"system":"_DATA00","slot_prefix":"_DATA","first_slot":1,"last_slot":10}}"#).unwrap();
        let s = new.saves.unwrap();
        assert_eq!((s.id.as_str(), s.import_from.len(), s.system.as_str(), s.slot_prefix.as_str(), s.first_slot, s.last_slot),
                   ("UCES01421", 1, "_DATA00", "_DATA", 1, 10));
    }
    #[test] fn real_release_json_has_saves_when_present() {
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build/rtw/release.json");
        let Ok(t) = std::fs::read_to_string(&p) else { eprintln!("SKIP: {} not on this PC", p.display()); return };
        let r: Release = serde_json::from_str(&t).unwrap();
        if r.saves.is_none() { eprintln!("SKIP: {} predates Task 3", p.display()); return; }
        assert_eq!(r.saves.unwrap().id, "UCES01421");
    }
```
  Run the launcher Rust gate. Expected: compile errors (`save_sfo` / `is_encrypted` / `set_string` not found, no `saves` field).

- [ ] **Step 2: `sfo.rs`.** Replace `parse` with an `entries`-based version and add the new functions
  (keep the module doc comment; extend it to "PARAM.SFO reader (PSP system file): key -> value for
  UTF-8 strings and 32-bit integers, raw values, and in-place string edits."):

```rust
/// One index entry of a PARAM.SFO: its key, format and where its value lives in the file.
#[derive(Debug, Clone)]
pub struct Entry { pub key: String, pub fmt: u16, pub len: usize, pub max: usize, pub start: usize, index: usize }

pub fn entries(b: &[u8]) -> Option<Vec<Entry>> {
    if b.len() < 20 || &b[0..4] != b"\0PSF" { return None; }
    let rd16 = |o: usize| b.get(o..o + 2).map(|s| u16::from_le_bytes([s[0], s[1]]));
    let rd32 = |o: usize| b.get(o..o + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]));
    let (keys, data, n) = (rd32(8)? as usize, rd32(12)? as usize, rd32(16)? as usize);
    let mut out = Vec::new();
    for i in 0..n.min(256) {
        let e = 20 + i * 16;
        let (koff, fmt, len, max, doff) = (rd16(e)? as usize, rd16(e + 2)?, rd32(e + 4)? as usize,
                                           rd32(e + 8)? as usize, rd32(e + 12)? as usize);
        let kstart = keys.checked_add(koff)?;
        let kend = kstart + b.get(kstart..)?.iter().position(|&c| c == 0)?;
        let start = data.checked_add(doff)?;
        b.get(start..start.checked_add(len)?)?;
        out.push(Entry { key: String::from_utf8_lossy(&b[kstart..kend]).into_owned(), fmt, len, max, start, index: e });
    }
    Some(out)
}

pub fn parse(b: &[u8]) -> Option<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    for e in entries(b)? {
        let v = &b[e.start..e.start + e.len];
        let val = match e.fmt {
            0x0404 if e.len >= 4 => u32::from_le_bytes([v[0], v[1], v[2], v[3]]).to_string(),
            _ => String::from_utf8_lossy(v).trim_end_matches('\0').to_string(),
        };
        out.insert(e.key, val);
    }
    Some(out)
}

/// The stored bytes of one value (for binary keys such as SAVEDATA_PARAMS).
pub fn raw<'a>(b: &'a [u8], key: &str) -> Option<&'a [u8]> {
    let e = entries(b)?.into_iter().find(|e| e.key == key)?;
    b.get(e.start..e.start + e.len)
}

/// PPSSPP's test: a save whose SAVEDATA_PARAMS starts with a non-zero byte has an encrypted data file.
pub fn is_encrypted(b: &[u8]) -> bool {
    raw(b, "SAVEDATA_PARAMS").and_then(|v| v.first().copied()).is_some_and(|x| x != 0)
}

/// Rewrites a UTF-8 string value inside its reserved space: the file keeps its size and every
/// byte outside that value. None if the key is missing, not a string, or `value` + NUL does not fit.
pub fn set_string(b: &[u8], key: &str, value: &str) -> Option<Vec<u8>> {
    let e = entries(b)?.into_iter().find(|e| e.key == key && e.fmt == 0x0204)?;
    let need = value.len() + 1;
    if need > e.max || e.start + e.max > b.len() { return None; }
    let mut out = b.to_vec();
    out[e.start..e.start + e.max].fill(0);
    out[e.start..e.start + value.len()].copy_from_slice(value.as_bytes());
    out[e.index + 4..e.index + 8].copy_from_slice(&(need as u32).to_le_bytes());
    Some(out)
}
```
  Replace the test-only `build` with a generic builder plus the old signature on top of it, and add
  `save_sfo`:

```rust
/// A PARAM.SFO from (key, fmt, value bytes, reserved length) entries, for the tests.
#[cfg(test)]
pub fn build_raw(entries: &[(&str, u16, Vec<u8>, u32)]) -> Vec<u8> {
    let (mut keys, mut data, mut index) = (Vec::new(), Vec::new(), Vec::new());
    for (k, fmt, v, max) in entries {
        let koff = keys.len() as u16;
        keys.extend_from_slice(k.as_bytes());
        keys.push(0);
        let doff = data.len() as u32;
        let len = v.len() as u32;
        let mut val = v.clone();
        val.resize(*max as usize, 0);
        data.extend_from_slice(&val);
        for part in [&koff.to_le_bytes()[..], &fmt.to_le_bytes(), &len.to_le_bytes(), &max.to_le_bytes(), &doff.to_le_bytes()] {
            index.extend_from_slice(part);
        }
    }
    while keys.len() % 4 != 0 { keys.push(0); }
    let key_start = 20 + index.len() as u32;
    let data_start = key_start + keys.len() as u32;
    let mut b = b"\0PSF".to_vec();
    for v in [0x0101u32, key_start, data_start, entries.len() as u32] { b.extend_from_slice(&v.to_le_bytes()); }
    b.extend(index);
    b.extend(keys);
    b.extend(data);
    b
}

/// A PARAM.SFO holding UTF-8 strings only (fmt 0x0204), for the tests.
#[cfg(test)]
pub fn build(entries: &[(&str, &str)]) -> Vec<u8> {
    let raw: Vec<(&str, u16, Vec<u8>, u32)> = entries.iter().map(|(k, v)| {
        let mut b = v.as_bytes().to_vec();
        b.push(0);
        let max = (b.len() as u32).div_ceil(4) * 4;
        (*k, 0x0204u16, b, max)
    }).collect();
    build_raw(&raw)
}

/// A save's PARAM.SFO like the game writes it (SAVEDATA_DIRECTORY reserved 64 bytes, 128-byte
/// SAVEDATA_PARAMS starting with `params0`: 0x00 plain, 0x41 PPSSPP mode 5), for the tests.
#[cfg(test)]
pub fn save_sfo(dir: &str, title: &str, detail: &str, params0: u8) -> Vec<u8> {
    let s = |v: &str| { let mut b = v.as_bytes().to_vec(); b.push(0); b };
    let mut params = vec![0u8; 128];
    params[0] = params0;
    build_raw(&[("CATEGORY", 0x0204, s("MS"), 4), ("SAVEDATA_DETAIL", 0x0204, s(detail), 1024),
                ("SAVEDATA_DIRECTORY", 0x0204, s(dir), 64), ("SAVEDATA_PARAMS", 0x0004, params, 128),
                ("SAVEDATA_TITLE", 0x0204, s(title), 128), ("TITLE", 0x0204, s("Patapon™3"), 128)])
}
```

- [ ] **Step 3: `releases.rs`.** Add above `Release`:

```rust
/// The game's save facts (release.json "saves", written by psprecomp's p3.sh): the save id the
/// game uses, ids whose saves can be imported by renaming them, the system-data name and the
/// game's slot names <slot_prefix><NN> for NN in first_slot..=last_slot.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SavesCfg {
    pub id: String,
    #[serde(default)] pub import_from: Vec<String>,
    pub system: String,
    pub slot_prefix: String,
    pub first_slot: u32,
    pub last_slot: u32,
}
```
  and `Release` becomes
  `pub struct Release { pub id: String, pub name: String, pub disc_id: String, pub eboot_sha256: String, #[serde(default)] pub saves: Option<SavesCfg> }`.
  Every struct literal of `Release` gains `saves: None` (`releases.rs` test `rel()`, `lib.rs` test `put()`).

- [ ] **Step 4: Tests pass.** Launcher Rust gate → all pass (44 + 5 new = 49 passed, 1 ignored). Paste the line.

- [ ] **Step 5: Report (no commit).** Claude commits: `feat(launcher): PARAM.SFO edits and release save facts`.

---

### Task 5: Launcher — `saves.rs`, `ppsspp.rs`, launcher settings keys

**Files (launcher):**
- Create: `src-tauri/src/saves.rs`, `src-tauri/src/ppsspp.rs`
- Modify: `src-tauri/src/lib.rs` (`mod saves; mod ppsspp;` only), `src-tauri/src/settings.rs`

**Interfaces:**
- Consumes: Task 4 `sfo::{parse, is_encrypted, set_string, save_sfo}`, `releases::SavesCfg`.
- Produces:
  - `saves::Kind { Slot, System, Other }` (serde `"slot" | "system" | "other"`),
    `saves::SaveEntry { folder, suffix, kind, slot: Option<u32>, title, detail, converted, encrypted, damaged, modified: u64 }`;
  - `saves::slot_suffix(&SavesCfg, u32) -> String`, `saves::valid_suffix(&str) -> bool`,
    `saves::classify(&SavesCfg, &str) -> (Kind, Option<u32>)`,
    `saves::list(dir: &Path, cfg: &SavesCfg, foreign: bool) -> Vec<SaveEntry>`,
    `saves::free_slot(dir: &Path, cfg: &SavesCfg) -> Option<u32>`,
    `saves::utc_stamp(secs: u64) -> String`, `saves::backup_dir(root: &Path, side: &str, now: SystemTime) -> PathBuf`,
    `saves::TransferError { Exists, Io(String) }`,
    `saves::transfer(src: &Path, dest_dir: &Path, target: &str, replace: bool, backup: &Path) -> Result<(), TransferError>`,
    `saves::cleanup_partials(dir: &Path)`, `saves::base64(&[u8]) -> String`;
  - `ppsspp::normalize(&Path) -> Option<PathBuf>`, `ppsspp::default_candidates(&dyn Fn(&str) -> Option<String>) -> Vec<PathBuf>`,
    `ppsspp::find(remembered: Option<&Path>, env: &dyn Fn(&str) -> Option<String>) -> Option<PathBuf>`;
  - `settings::launcher_get(root: &Path, key: &str) -> Option<String>`,
    `settings::launcher_set(root: &Path, key: &str, value: &str) -> Result<(), String>`.

- [ ] **Step 1: `saves.rs` with its tests.** Create `src-tauri/src/saves.rs`:

```rust
//! The game's saves in one savedata folder (the port's versions/<id>/savedata or PPSSPP's
//! PSP/SAVEDATA): listed like the game's LOAD screen, and copied between the two. A copy is staged
//! in <folder>.partial and renamed into place; whatever it replaces is first moved to backups/.
//! Nothing here deletes a save.
use crate::releases::SavesCfg;
use crate::sfo;
use serde::Serialize;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Kind { Slot, System, Other }

#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct SaveEntry {
    pub folder: String,      // UCUS98751_DATA02
    pub suffix: String,      // _DATA02
    pub kind: Kind,
    pub slot: Option<u32>,   // 2 for a game slot
    pub title: String,       // SAVEDATA_TITLE
    pub detail: String,      // SAVEDATA_DETAIL (Hero / Class Lv / Play time / Last mission)
    pub converted: bool,     // an import_from id (US): renamed to the release's id on import
    pub encrypted: bool,     // SAVEDATA_PARAMS[0] != 0 (PPSSPP / PSP); the game decrypts it
    pub damaged: bool,       // no readable PARAM.SFO: listed, never copied
    pub modified: u64,       // PARAM.SFO modification time, seconds since 1970 (0 unknown)
}

pub fn slot_suffix(cfg: &SavesCfg, n: u32) -> String { format!("{}{n:02}", cfg.slot_prefix) }

/// A folder-name tail the game writes (`_DATA04`): letters, digits and `_` only, at most 20.
pub fn valid_suffix(s: &str) -> bool {
    !s.is_empty() && s.len() <= 20 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
}

pub fn classify(cfg: &SavesCfg, suffix: &str) -> (Kind, Option<u32>) {
    if suffix == cfg.system { return (Kind::System, None); }
    let slot = suffix.strip_prefix(cfg.slot_prefix.as_str())
        .filter(|d| d.len() == 2 && d.chars().all(|c| c.is_ascii_digit()))
        .and_then(|d| d.parse::<u32>().ok())
        .filter(|n| (cfg.first_slot..=cfg.last_slot).contains(n));
    match slot { Some(n) => (Kind::Slot, Some(n)), None => (Kind::Other, None) }
}

fn modified(p: &Path) -> u64 {
    std::fs::metadata(p).and_then(|m| m.modified()).ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs())
}

/// The saves of `cfg.id` (with `foreign`, also of `cfg.import_from`) in `dir`: game slots by
/// number, then the system data, then the rest by name. Anything else in the folder (other
/// games, "<save> - copia", "<save>.partial", files) is ignored.
pub fn list(dir: &Path, cfg: &SavesCfg, foreign: bool) -> Vec<SaveEntry> {
    let mut ids = vec![cfg.id.as_str()];
    if foreign { ids.extend(cfg.import_from.iter().map(String::as_str)); }
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir) else { return out };
    for e in rd.flatten() {
        if !e.path().is_dir() { continue; }
        let folder = e.file_name().to_string_lossy().into_owned();
        let Some((id, suffix)) = ids.iter().find_map(|id| folder.strip_prefix(id).map(|s| (*id, s.to_string()))) else { continue };
        if !valid_suffix(&suffix) { continue; }
        let (kind, slot) = classify(cfg, &suffix);
        let sfo_path = e.path().join("PARAM.SFO");
        let bytes = std::fs::read(&sfo_path).ok();
        let map = bytes.as_deref().and_then(sfo::parse);
        out.push(SaveEntry {
            title: map.as_ref().and_then(|m| m.get("SAVEDATA_TITLE").cloned()).unwrap_or_default(),
            detail: map.as_ref().and_then(|m| m.get("SAVEDATA_DETAIL").cloned()).unwrap_or_default(),
            converted: id != cfg.id,
            encrypted: bytes.as_deref().is_some_and(sfo::is_encrypted),
            damaged: map.is_none(),
            modified: modified(&sfo_path),
            folder, suffix, kind, slot,
        });
    }
    let rank = |e: &SaveEntry| match e.kind { Kind::Slot => 0, Kind::System => 1, Kind::Other => 2 };
    out.sort_by(|a, b| (rank(a), a.slot, a.converted, &a.folder).cmp(&(rank(b), b.slot, b.converted, &b.folder)));
    out
}

/// First game slot with no folder in `dir`.
pub fn free_slot(dir: &Path, cfg: &SavesCfg) -> Option<u32> {
    (cfg.first_slot..=cfg.last_slot).find(|n| !dir.join(format!("{}{}", cfg.id, slot_suffix(cfg, *n))).exists())
}

/// `YYYYMMDD-HHMMSS` in UTC (civil-from-days, no time-zone database needed).
pub fn utc_stamp(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}{m:02}{d:02}-{:02}{:02}{:02}", rem / 3600, rem % 3600 / 60, rem % 60)
}

/// `<root>/backups/<stamp>/<side>`, a folder that does not exist yet (`<stamp>-2`, `-3`... when
/// several backups happen within the same second).
pub fn backup_dir(root: &Path, side: &str, now: SystemTime) -> PathBuf {
    let stamp = utc_stamp(now.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs()));
    let mut n = 1;
    loop {
        let name = if n == 1 { stamp.clone() } else { format!("{stamp}-{n}") };
        let p = root.join("backups").join(name).join(side);
        if !p.exists() { return p; }
        n += 1;
    }
}

fn copy_dir(from: &Path, to: &Path) -> io::Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from)? {
        let e = e?;
        let ty = e.file_type()?;
        let dst = to.join(e.file_name());
        if ty.is_dir() { copy_dir(&e.path(), &dst)?; } else if ty.is_file() { std::fs::copy(e.path(), &dst)?; }
    }
    Ok(())
}

fn tree_size(p: &Path) -> io::Result<u64> {
    let mut n = 0;
    for e in std::fs::read_dir(p)? {
        let e = e?;
        n += if e.file_type()?.is_dir() { tree_size(&e.path())? } else { e.metadata()?.len() };
    }
    Ok(n)
}

/// Moves a folder; across volumes (a PPSSPP folder on E: into backups/ on C:) it copies, checks
/// the copy holds as many bytes, and only then removes the original. `force_copy` is for the tests.
fn move_dir(from: &Path, to: &Path, force_copy: bool) -> io::Result<()> {
    if let Some(parent) = to.parent() { std::fs::create_dir_all(parent)?; }
    if !force_copy && std::fs::rename(from, to).is_ok() { return Ok(()); }
    copy_dir(from, to)?;
    if tree_size(from)? != tree_size(to)? {
        return Err(io::Error::other(format!("copy of {} is incomplete; original kept", from.display())));
    }
    std::fs::remove_dir_all(from)
}

/// Rewrites PARAM.SFO's SAVEDATA_DIRECTORY when it names another folder (a US save becoming EU).
fn fix_directory(dir: &Path, target: &str) -> io::Result<()> {
    let p = dir.join("PARAM.SFO");
    let b = std::fs::read(&p)?;
    let bad = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_string());
    let m = sfo::parse(&b).ok_or_else(|| bad("PARAM.SFO cannot be read"))?;
    match m.get("SAVEDATA_DIRECTORY") {
        Some(d) if d != target => {
            let nb = sfo::set_string(&b, "SAVEDATA_DIRECTORY", target).ok_or_else(|| bad("SAVEDATA_DIRECTORY does not fit"))?;
            std::fs::write(&p, nb)
        }
        _ => Ok(()),
    }
}

#[derive(Debug, PartialEq)]
pub enum TransferError { Exists, Io(String) }

/// Copies the save folder `src` to `dest_dir/<target>`. An existing `<target>` is refused unless
/// `replace`; then it is moved to `backup/<target>` first. The copy is staged in
/// `<target>.partial` (SAVEDATA_DIRECTORY rewritten when the name changes) and renamed into place,
/// so a failure never leaves a half save under the real name.
pub fn transfer(src: &Path, dest_dir: &Path, target: &str, replace: bool, backup: &Path) -> Result<(), TransferError> {
    transfer_with(src, dest_dir, target, replace, backup, false)
}

fn transfer_with(src: &Path, dest_dir: &Path, target: &str, replace: bool, backup: &Path, force_copy: bool) -> Result<(), TransferError> {
    let io = |e: io::Error| TransferError::Io(e.to_string());
    if target.is_empty() { return Err(TransferError::Io("empty save folder name".into())); }
    if !target.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return Err(TransferError::Io(format!("bad save folder name {target:?}")));
    }
    let dest = dest_dir.join(target);
    if dest.exists() && !replace { return Err(TransferError::Exists); }
    if !src.join("PARAM.SFO").is_file() {
        return Err(TransferError::Io(format!("{} has no PARAM.SFO", src.display())));
    }
    std::fs::create_dir_all(dest_dir).map_err(io)?;
    let partial = dest_dir.join(format!("{target}.partial"));
    if partial.exists() { std::fs::remove_dir_all(&partial).map_err(io)?; }
    if let Err(e) = copy_dir(src, &partial).and_then(|()| fix_directory(&partial, target)) {
        std::fs::remove_dir_all(&partial).ok();
        return Err(io(e));
    }
    if dest.exists() {
        if let Err(e) = move_dir(&dest, &backup.join(target), force_copy) {
            std::fs::remove_dir_all(&partial).ok();
            return Err(io(e));
        }
    }
    std::fs::rename(&partial, &dest).map_err(io)
}

/// Deletes copies a crash left half done (`*.partial` folders) in one savedata folder.
pub fn cleanup_partials(dir: &Path) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        if e.file_name().to_string_lossy().ends_with(".partial") && e.path().is_dir() {
            std::fs::remove_dir_all(e.path()).ok();
        }
    }
}

/// Standard base64 (for ICON0.PNG as a data: URL in the UI).
pub fn base64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(b.len().div_ceil(3) * 4);
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 { s.push(if i <= c.len() { T[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' }); }
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg() -> SavesCfg {
        SavesCfg { id: "UCES01421".into(), import_from: vec!["UCUS98751".into()], system: "_DATA00".into(),
                   slot_prefix: "_DATA".into(), first_slot: 1, last_slot: 10 }
    }
    fn put(dir: &Path, folder: &str, detail: &str, params0: u8) {
        std::fs::create_dir_all(dir.join(folder)).unwrap();
        std::fs::write(dir.join(folder).join("PARAM.SFO"), sfo::save_sfo(folder, "Datos guardados", detail, params0)).unwrap();
        std::fs::write(dir.join(folder).join("SECURE.BIN"), detail.as_bytes()).unwrap();
        std::fs::write(dir.join(folder).join("ICON0.PNG"), b"\x89PNG").unwrap();
    }

    #[test] fn suffixes_and_kinds() {
        let c = cfg();
        assert_eq!(slot_suffix(&c, 4), "_DATA04");
        assert_eq!(classify(&c, "_DATA04"), (Kind::Slot, Some(4)));
        assert_eq!(classify(&c, "_DATA10"), (Kind::Slot, Some(10)));
        assert_eq!(classify(&c, "_DATA00"), (Kind::System, None));
        assert_eq!(classify(&c, "_DATA11"), (Kind::Other, None));
        assert_eq!(classify(&c, "_DATA99"), (Kind::Other, None));
        assert_eq!(classify(&c, "_DATA4"), (Kind::Other, None));
        for ok in ["_DATA04", "_DATA99", "DATA"] { assert!(valid_suffix(ok), "{ok}"); }
        for bad in ["", "_DATA01 - copia", "_DATA04.partial", "_DATA/..", "_DATA\\x", "_DATA:1", &"x".repeat(21)] {
            assert!(!valid_suffix(bad), "{bad}");
        }
    }
    #[test] fn list_like_the_load_screen() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path();
        put(p, "UCES01421_DATA04", "Héroe: Edson\nClase: Yumiyacha Nv 35", 0x41);
        put(p, "UCES01421_DATA01", "Héroe: Hero", 0x00);
        put(p, "UCES01421_DATA00", " ", 0x00);
        put(p, "UCES01421_DATA99", "x", 0x00);
        put(p, "UCUS98751_DATA02", "Uberhero: MoMo", 0x41);
        put(p, "UCES01421_DATA01 - copia", "copy", 0x00);
        put(p, "UCES01177_DATA03", "Patapon 2", 0x41);
        std::fs::create_dir_all(p.join("UCES01421_DATA05.partial")).unwrap();
        std::fs::write(p.join("UCES01421_DATA02.rar"), b"rar").unwrap();
        std::fs::create_dir_all(p.join("UCES01421_DATA06")).unwrap();  // no PARAM.SFO
        let own: Vec<String> = list(p, &cfg(), false).into_iter().map(|e| e.folder).collect();
        assert_eq!(own, ["UCES01421_DATA01", "UCES01421_DATA04", "UCES01421_DATA06", "UCES01421_DATA00", "UCES01421_DATA99"]);
        let all = list(p, &cfg(), true);
        let names: Vec<&str> = all.iter().map(|e| e.folder.as_str()).collect();
        assert_eq!(names, ["UCES01421_DATA01", "UCUS98751_DATA02", "UCES01421_DATA04", "UCES01421_DATA06", "UCES01421_DATA00", "UCES01421_DATA99"]);
        let us = &all[1];
        assert!(us.converted && us.encrypted && !us.damaged);
        assert_eq!((us.kind, us.slot, us.suffix.as_str(), us.detail.as_str()), (Kind::Slot, Some(2), "_DATA02", "Uberhero: MoMo"));
        let d4 = &all[2];
        assert!(d4.encrypted && !d4.converted && d4.modified > 0);
        assert_eq!(d4.title, "Datos guardados");
        assert!(all[3].damaged, "no PARAM.SFO -> damaged");
        assert_eq!((all[4].kind, all[5].kind), (Kind::System, Kind::Other));
        assert!(list(&p.join("missing"), &cfg(), true).is_empty());
    }
    #[test] fn free_slot_is_the_first_hole() {
        let d = tempfile::tempdir().unwrap();
        assert_eq!(free_slot(d.path(), &cfg()), Some(1));
        put(d.path(), "UCES01421_DATA01", "a", 0);
        put(d.path(), "UCES01421_DATA02", "b", 0);
        put(d.path(), "UCES01421_DATA04", "c", 0);
        assert_eq!(free_slot(d.path(), &cfg()), Some(3));
        for n in 1..=10 { put(d.path(), &format!("UCES01421_DATA{n:02}"), "x", 0); }
        assert_eq!(free_slot(d.path(), &cfg()), None);
    }
    #[test] fn stamps_and_backup_names() {
        assert_eq!(utc_stamp(0), "19700101-000000");
        assert_eq!(utc_stamp(1_700_000_000), "20231114-221320");
        assert_eq!(utc_stamp(951_782_400), "20000229-000000");
        let d = tempfile::tempdir().unwrap();
        let t = UNIX_EPOCH + std::time::Duration::from_secs(1_700_000_000);
        let a = backup_dir(d.path(), "ppsspp", t);
        assert!(a.ends_with("backups/20231114-221320/ppsspp"));
        std::fs::create_dir_all(&a).unwrap();
        assert!(backup_dir(d.path(), "ppsspp", t).ends_with("backups/20231114-221320-2/ppsspp"));
        assert!(backup_dir(d.path(), "port-dxd-1.1.4", t).ends_with("backups/20231114-221320/port-dxd-1.1.4"));
    }
    #[test] fn import_us_save_renames_and_keeps_the_data() {
        let d = tempfile::tempdir().unwrap();
        let (pp, port, bk) = (d.path().join("pp"), d.path().join("port"), d.path().join("bk"));
        put(&pp, "UCUS98751_DATA02", "Uberhero: MoMo", 0x41);
        transfer(&pp.join("UCUS98751_DATA02"), &port, "UCES01421_DATA02", false, &bk).unwrap();
        let sfo_b = std::fs::read(port.join("UCES01421_DATA02/PARAM.SFO")).unwrap();
        assert_eq!(sfo::parse(&sfo_b).unwrap()["SAVEDATA_DIRECTORY"], "UCES01421_DATA02");
        assert!(sfo::is_encrypted(&sfo_b), "still encrypted: the game decrypts it");
        assert_eq!(std::fs::read(port.join("UCES01421_DATA02/SECURE.BIN")).unwrap(), b"Uberhero: MoMo");
        assert!(port.join("UCES01421_DATA02/ICON0.PNG").is_file());
        assert!(pp.join("UCUS98751_DATA02/SECURE.BIN").is_file(), "the source is untouched");
        assert!(!port.join("UCES01421_DATA02.partial").exists());
        assert!(!bk.exists(), "nothing replaced, no backup");
    }
    #[test] fn existing_target_needs_replace_and_is_backed_up() {
        let d = tempfile::tempdir().unwrap();
        let (pp, port, bk) = (d.path().join("pp"), d.path().join("port"), d.path().join("bk"));
        put(&pp, "UCES01421_DATA04", "new", 0x41);
        put(&port, "UCES01421_DATA04", "old", 0x00);
        assert_eq!(transfer(&pp.join("UCES01421_DATA04"), &port, "UCES01421_DATA04", false, &bk), Err(TransferError::Exists));
        assert_eq!(std::fs::read(port.join("UCES01421_DATA04/SECURE.BIN")).unwrap(), b"old", "refused: untouched");
        transfer(&pp.join("UCES01421_DATA04"), &port, "UCES01421_DATA04", true, &bk).unwrap();
        assert_eq!(std::fs::read(port.join("UCES01421_DATA04/SECURE.BIN")).unwrap(), b"new");
        assert_eq!(std::fs::read(bk.join("UCES01421_DATA04/SECURE.BIN")).unwrap(), b"old", "the old save is in the backup");
    }
    #[test] fn cross_volume_backup_copies_then_removes() {
        let d = tempfile::tempdir().unwrap();
        let (port, pp, bk) = (d.path().join("port"), d.path().join("pp"), d.path().join("bk"));
        put(&port, "UCES01421_DATA03", "mine", 0x00);
        put(&pp, "UCES01421_DATA03", "theirs", 0x41);
        transfer_with(&port.join("UCES01421_DATA03"), &pp, "UCES01421_DATA03", true, &bk, true).unwrap();
        assert_eq!(std::fs::read(pp.join("UCES01421_DATA03/SECURE.BIN")).unwrap(), b"mine");
        assert_eq!(std::fs::read(bk.join("UCES01421_DATA03/SECURE.BIN")).unwrap(), b"theirs");
        assert!(bk.join("UCES01421_DATA03/PARAM.SFO").is_file() && bk.join("UCES01421_DATA03/ICON0.PNG").is_file());
    }
    #[test] fn failures_leave_no_partial_and_bad_names_are_refused() {
        let d = tempfile::tempdir().unwrap();
        let (src, port, bk) = (d.path().join("src"), d.path().join("port"), d.path().join("bk"));
        std::fs::create_dir_all(src.join("UCES01421_DATA01")).unwrap();  // no PARAM.SFO
        assert!(matches!(transfer(&src.join("UCES01421_DATA01"), &port, "UCES01421_DATA01", false, &bk), Err(TransferError::Io(_))));
        put(&src, "UCES01421_DATA02", "x", 0);
        for bad in ["../x", "UCES01421_DATA02/..", "a b", "UCES01421_DATA02.partial", ""] {
            assert!(matches!(transfer(&src.join("UCES01421_DATA02"), &port, bad, false, &bk), Err(TransferError::Io(_))), "{bad}");
        }
        std::fs::write(src.join("UCES01421_DATA02/PARAM.SFO"), b"garbage").unwrap();
        assert!(matches!(transfer(&src.join("UCES01421_DATA02"), &port, "UCES01421_DATA07", false, &bk), Err(TransferError::Io(_))));
        assert!(!port.join("UCES01421_DATA07.partial").exists() && !port.join("UCES01421_DATA07").exists());
    }
    #[test] fn leftover_partials_are_cleaned() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "UCES01421_DATA01", "keep", 0);
        std::fs::create_dir_all(d.path().join("UCES01421_DATA02.partial/sub")).unwrap();
        cleanup_partials(d.path());
        assert!(!d.path().join("UCES01421_DATA02.partial").exists());
        assert!(d.path().join("UCES01421_DATA01/SECURE.BIN").is_file());
        cleanup_partials(&d.path().join("missing"));
    }
    #[test] fn base64_vectors() {
        for (i, o) in [("", ""), ("f", "Zg=="), ("fo", "Zm8="), ("foo", "Zm9v"), ("foobar", "Zm9vYmFy")] {
            assert_eq!(base64(i.as_bytes()), o);
        }
    }
}
```
  A `.partial` or `..` target fails the character check in `transfer_with`.

- [ ] **Step 2: `ppsspp.rs` with its tests.** Create `src-tauri/src/ppsspp.rs`:

```rust
//! PPSSPP's save folder (PSP/SAVEDATA of its memory stick): normalising a folder the player
//! picked, and where an installed PPSSPP keeps it by default.
use std::path::{Path, PathBuf};

/// The SAVEDATA folder that `p` stands for: SAVEDATA itself, or PPSSPP's own folder, its
/// memstick, or its PSP folder (the first of SAVEDATA, PSP/SAVEDATA, memstick/PSP/SAVEDATA under `p`).
pub fn normalize(p: &Path) -> Option<PathBuf> {
    let named = p.file_name().is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case("SAVEDATA"));
    if named && p.is_dir() { return Some(p.to_path_buf()); }
    ["SAVEDATA", "PSP/SAVEDATA", "memstick/PSP/SAVEDATA"].iter().map(|s| p.join(s)).find(|c| c.is_dir())
}

/// Default memory-stick folders of an installed PPSSPP, most likely first.
pub fn default_candidates(env: &dyn Fn(&str) -> Option<String>) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if cfg!(windows) {
        if let Some(home) = env("USERPROFILE") {
            out.push(Path::new(&home).join("Documents").join("PPSSPP"));
            out.push(Path::new(&home).join("OneDrive").join("Documents").join("PPSSPP"));
        }
    } else {
        if let Some(x) = env("XDG_CONFIG_HOME") { out.push(Path::new(&x).join("ppsspp")); }
        if let Some(home) = env("HOME") { out.push(Path::new(&home).join(".config").join("ppsspp")); }
    }
    out
}

/// The remembered folder while it still exists, else the first default that has a SAVEDATA.
pub fn find(remembered: Option<&Path>, env: &dyn Fn(&str) -> Option<String>) -> Option<PathBuf> {
    remembered.and_then(normalize).or_else(|| default_candidates(env).iter().find_map(|c| normalize(c)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn four_shapes_normalise_to_savedata() {
        let d = tempfile::tempdir().unwrap();
        let portable = d.path().join("ppsspp_win");
        let sd = portable.join("memstick/PSP/SAVEDATA");
        std::fs::create_dir_all(&sd).unwrap();
        for p in [portable.clone(), portable.join("memstick"), portable.join("memstick/PSP"), sd.clone()] {
            assert_eq!(normalize(&p).as_deref(), Some(sd.as_path()), "{}", p.display());
        }
        assert_eq!(normalize(&d.path().join("nothing")), None);
        std::fs::create_dir_all(d.path().join("empty")).unwrap();
        assert_eq!(normalize(&d.path().join("empty")), None);
    }
    #[test] fn defaults_and_remembered() {
        let d = tempfile::tempdir().unwrap();
        let home = d.path().to_string_lossy().into_owned();
        let env = |k: &str| match k { "USERPROFILE" | "HOME" => Some(home.clone()), _ => None };
        assert_eq!(find(None, &env), None, "nothing installed");
        let installed = if cfg!(windows) { d.path().join("Documents/PPSSPP/PSP/SAVEDATA") } else { d.path().join(".config/ppsspp/PSP/SAVEDATA") };
        std::fs::create_dir_all(&installed).unwrap();
        assert_eq!(find(None, &env).as_deref(), Some(installed.as_path()));
        let other = d.path().join("E/Respaldo/PPSSPP/memstick/PSP/SAVEDATA");
        std::fs::create_dir_all(&other).unwrap();
        assert_eq!(find(Some(&other), &env).as_deref(), Some(other.as_path()), "remembered wins");
        assert_eq!(find(Some(&d.path().join("gone")), &env).as_deref(), Some(installed.as_path()), "gone -> default");
    }
}
```

- [ ] **Step 3: `settings.rs` launcher keys.** Replace `last_version` / `set_last_version` with:

```rust
/// A launcher-only value from settings.ini's [launcher] section (the game ignores it).
pub fn launcher_get(root: &Path, key: &str) -> Option<String> {
    let text = std::fs::read_to_string(ini_path(root)).ok()?;
    IniFile::parse(&text).get("launcher", key).filter(|v| !v.is_empty())
}

pub fn launcher_set(root: &Path, key: &str, value: &str) -> Result<(), String> {
    let (defs, _) = catalogs(root);
    let p = ini_path(root);
    let text = std::fs::read_to_string(&p).unwrap_or_else(|_| default_ini(&defs));
    let mut f = IniFile::parse(&text);
    f.set("launcher", key, value);
    write_atomic(&p, &f.to_string())
}

pub fn last_version(root: &Path) -> Option<String> { launcher_get(root, "version") }

pub fn set_last_version(root: &Path, id: &str) -> Result<(), String> { launcher_set(root, "version", id) }
```
  and add to its tests:

```rust
    #[test] fn launcher_keys_live_together() {
        let d = root();
        load(d.path()).unwrap();
        set_last_version(d.path(), "dxd-1.1.4").unwrap();
        launcher_set(d.path(), "save_dxd-1.1.4", "_DATA04").unwrap();
        launcher_set(d.path(), "ppsspp_savedata", "E:\\Respaldo\\PPSSPP\\memstick\\PSP\\SAVEDATA").unwrap();
        assert_eq!(launcher_get(d.path(), "save_dxd-1.1.4").as_deref(), Some("_DATA04"));
        assert_eq!(launcher_get(d.path(), "ppsspp_savedata").as_deref(), Some("E:\\Respaldo\\PPSSPP\\memstick\\PSP\\SAVEDATA"));
        assert_eq!(last_version(d.path()).as_deref(), Some("dxd-1.1.4"));
        assert_eq!(launcher_get(d.path(), "missing"), None);
        assert_eq!(load(d.path()).unwrap()["game.fps"], "30", "options still load");
    }
```
  If `IniFile::set` / `get` cannot hold a key with `-` and `.` (e.g. `save_dxd-1.1.4`) or a value
  with `\` and `:`, stop BLOCKED with the failing assertion instead of changing the key format.
  In `lib.rs` add `mod ppsspp;` and `mod saves;` to the module list (nothing else in this task).

- [ ] **Step 4: Tests pass.** Launcher Rust gate → 62 passed (49 + saves 10 + ppsspp 2 + settings 1),
  1 ignored, 0 failed. `cargo build` warnings about unused functions are expected until Task 6.

- [ ] **Step 5: Report (no commit).** Claude commits: `feat(launcher): save slots, PPSSPP folder, transfers with backups`.

---

### Task 6: Launcher — save commands, startup cleanup, `PSPRECOMP_SAVE_SLOT`

**Files (launcher):**
- Modify: `src-tauri/src/lib.rs`, `src-tauri/src/launch.rs`

**Interfaces:**
- Consumes: Task 4 `releases::{load, SavesCfg}`, Task 5 `saves::*`, `ppsspp::find/normalize`,
  `settings::launcher_get/launcher_set`.
- Produces Tauri commands (JS names, args camelCase):
  - `list_saves(id) -> SavesPayload { supported: bool, slots: SaveView[], all_slots: string[], chosen: string|null, continues: string|null, free: string|null }`
  - `choose_save(id, suffix) -> ()`
  - `ppsspp_folder() -> string|null`, `choose_ppsspp_folder(path) -> string` (Err `"not_found"`)
  - `list_ppsspp_saves(id) -> SaveView[]` (Err `"not_found"`)
  - `import_save(id, folder, targetSuffix, replace) -> string` (the new folder; Err `"exists"` or a message)
  - `export_save(id, suffix, replace) -> string` (the PPSSPP folder written; Err `"exists"`, `"not_found"` or a message)
  - `SaveView` = `SaveEntry` fields flattened + `icon: string` (`data:image/png;base64,...` or `""`).
  - `launch::game_env` adds `PSPRECOMP_SAVE_SLOT=<suffix>` when `[launcher] save_<id>` names an existing slot.

- [ ] **Step 1: Failing tests.** In `lib.rs` tests add:

```rust
    fn put_saves_release(root: &std::path::Path, id: &str) {
        std::fs::create_dir_all(releases::release_dir(root, id)).unwrap();
        let r = releases::Release { id: id.into(), name: id.into(), disc_id: "X".into(), eboot_sha256: "0".into(),
            saves: Some(releases::SavesCfg { id: "UCES01421".into(), import_from: vec!["UCUS98751".into()], system: "_DATA00".into(),
                                             slot_prefix: "_DATA".into(), first_slot: 1, last_slot: 10 }) };
        std::fs::write(releases::release_dir(root, id).join("release.json"), serde_json::to_string(&r).unwrap()).unwrap();
        std::fs::create_dir_all(install::version_dir(root, id)).unwrap();
        std::fs::write(install::version_dir(root, id).join("installed.json"), b"{}").unwrap();
    }
    fn put_save(dir: &std::path::Path, folder: &str, detail: &str, params0: u8, age_secs: u64) {
        std::fs::create_dir_all(dir.join(folder)).unwrap();
        let p = dir.join(folder).join("PARAM.SFO");
        std::fs::write(&p, sfo::save_sfo(folder, "Datos guardados", detail, params0)).unwrap();
        std::fs::write(dir.join(folder).join("ICON0.PNG"), b"PNG").unwrap();
        let t = std::time::SystemTime::now() - std::time::Duration::from_secs(age_secs);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(t).unwrap();
    }
    #[test] fn saves_payload_continue_rules() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        put_saves_release(root, "dxd-1.1.4");
        let sd = install::savedata(root, "dxd-1.1.4");
        put_save(&sd, "UCES01421_DATA01", "Héroe: Hero", 0, 100);
        put_save(&sd, "UCES01421_DATA04", "Héroe: Edson", 0x41, 5000);
        put_save(&sd, "UCES01421_DATA00", " ", 0, 10);
        let p = saves_payload(root, "dxd-1.1.4").unwrap();
        assert!(p.supported);
        assert_eq!(p.all_slots.len(), 10);
        assert_eq!(p.all_slots[0], "_DATA01");
        assert_eq!(p.continues.as_deref(), Some("_DATA01"), "newest slot when none is chosen");
        assert_eq!(p.free.as_deref(), Some("_DATA02"));
        assert!(p.slots.iter().all(|s| s.icon == format!("data:image/png;base64,{}", saves::base64(b"PNG"))));
        settings::launcher_set(root, "save_dxd-1.1.4", "_DATA04").unwrap();
        let p = saves_payload(root, "dxd-1.1.4").unwrap();
        assert_eq!((p.chosen.as_deref(), p.continues.as_deref()), (Some("_DATA04"), Some("_DATA04")));
        settings::launcher_set(root, "save_dxd-1.1.4", "_DATA07").unwrap();
        assert_eq!(saves_payload(root, "dxd-1.1.4").unwrap().continues.as_deref(), Some("_DATA01"), "chosen slot gone -> newest");
        put(root, "vanilla-eu", true);
        assert!(!saves_payload(root, "vanilla-eu").unwrap().supported, "release.json without saves");
        assert!(saves_payload(root, "../x").is_err());
    }
    #[test] fn import_and_export_commands() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        put_saves_release(root, "dxd-1.1.4");
        let pp = root.join("pp/memstick/PSP/SAVEDATA");
        put_save(&pp, "UCUS98751_DATA02", "Uberhero: MoMo", 0x41, 10);
        put_save(&pp, "UCES01421_DATA04", "Héroe: Edson", 0x41, 10);
        assert_eq!(do_import(root, &pp, "dxd-1.1.4", "UCUS98751_DATA02", "_DATA02", false).unwrap(), "UCES01421_DATA02");
        assert_eq!(settings::launcher_get(root, "save_dxd-1.1.4").as_deref(), Some("_DATA02"), "an imported slot becomes the chosen one");
        assert_eq!(do_import(root, &pp, "dxd-1.1.4", "UCUS98751_DATA02", "_DATA02", false), Err("exists".to_string()));
        assert!(do_import(root, &pp, "dxd-1.1.4", "UCES01177_DATA03", "_DATA03", false).is_err(), "not a listed save");
        assert!(do_import(root, &pp, "dxd-1.1.4", "UCUS98751_DATA02", "_DATA02/..", true).is_err());
        assert_eq!(do_export(root, &pp, "dxd-1.1.4", "_DATA02", false).unwrap(), "UCES01421_DATA02");
        assert!(pp.join("UCES01421_DATA02/PARAM.SFO").is_file());
        put_save(&install::savedata(root, "dxd-1.1.4"), "UCES01421_DATA04", "mine", 0, 10);
        assert_eq!(do_export(root, &pp, "dxd-1.1.4", "_DATA04", false), Err("exists".to_string()));
        do_export(root, &pp, "dxd-1.1.4", "_DATA04", true).unwrap();
        let backups: Vec<_> = walk(&root.join("backups"));
        assert!(backups.iter().any(|p| p.ends_with("ppsspp/UCES01421_DATA04/PARAM.SFO")), "PPSSPP's old DATA04 is in backups: {backups:?}");
        assert!(do_export(root, &pp, "dxd-1.1.4", "_DATA09", false).is_err(), "no such slot in the port");
    }
    fn walk(p: &std::path::Path) -> Vec<std::path::PathBuf> {
        let mut out = Vec::new();
        if let Ok(rd) = std::fs::read_dir(p) {
            for e in rd.flatten() { if e.path().is_dir() { out.extend(walk(&e.path())); } else { out.push(e.path()); } }
        }
        out
    }
```
  In `launch.rs` tests add:

```rust
    #[test] fn chosen_save_reaches_the_game() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        let id = "dxd-1.1.4";
        std::fs::create_dir_all(crate::releases::release_dir(root, id)).unwrap();
        std::fs::write(crate::releases::release_dir(root, id).join("release.json"),
            r#"{"id":"dxd-1.1.4","name":"P3","disc_id":"X","eboot_sha256":"0","saves":{"id":"UCES01421","system":"_DATA00","slot_prefix":"_DATA","first_slot":1,"last_slot":10}}"#).unwrap();
        let has = |env: &[(String, String)]| env.iter().find(|(k, _)| k == "PSPRECOMP_SAVE_SLOT").map(|(_, v)| v.clone());
        assert_eq!(has(&game_env(root, id)), None, "nothing chosen");
        crate::settings::launcher_set(root, "save_dxd-1.1.4", "_DATA04").unwrap();
        assert_eq!(has(&game_env(root, id)), None, "chosen slot does not exist");
        std::fs::create_dir_all(crate::install::savedata(root, id).join("UCES01421_DATA04")).unwrap();
        assert_eq!(has(&game_env(root, id)).as_deref(), Some("_DATA04"));
        crate::settings::launcher_set(root, "save_dxd-1.1.4", "../x").unwrap();
        assert_eq!(has(&game_env(root, id)), None, "a bad value never reaches the game");
    }
```
  Run the launcher Rust gate. Expected: compile errors (`saves_payload`, `do_import`, `do_export` missing).

- [ ] **Step 2: `lib.rs`.** Add (after `open_save_folder`):

```rust
#[derive(Serialize)]
struct SaveView { #[serde(flatten)] entry: saves::SaveEntry, icon: String }

fn view(dir: &std::path::Path, e: saves::SaveEntry) -> SaveView {
    let icon = std::fs::read(dir.join(&e.folder).join("ICON0.PNG"))
        .map(|b| format!("data:image/png;base64,{}", saves::base64(&b))).unwrap_or_default();
    SaveView { entry: e, icon }
}

#[derive(Serialize)]
struct SavesPayload { supported: bool, slots: Vec<SaveView>, all_slots: Vec<String>, chosen: Option<String>,
                      continues: Option<String>, free: Option<String> }

/// The release's save facts; Ok(None) when its release.json has none (an older build).
fn release_saves(root: &std::path::Path, id: &str) -> Result<Option<releases::SavesCfg>, String> {
    if !releases::valid_id(id) { return Err(format!("bad release id {id}")); }
    Ok(releases::load(root).into_iter().find(|r| r.id == id).and_then(|r| r.saves))
}

fn chosen_key(id: &str) -> String { format!("save_{id}") }

fn ppsspp_dir(root: &std::path::Path) -> Option<std::path::PathBuf> {
    let remembered = settings::launcher_get(root, "ppsspp_savedata").map(std::path::PathBuf::from);
    ppsspp::find(remembered.as_deref(), &|k| std::env::var(k).ok())
}

/// What the game's Continue loads: the chosen slot while it exists, else the newest slot (the
/// runtime's LISTLOAD rule: newest PARAM.SFO).
fn continues(list: &[saves::SaveEntry], chosen: Option<&str>) -> Option<String> {
    let slots = || list.iter().filter(|e| e.kind == saves::Kind::Slot && !e.damaged);
    chosen.and_then(|c| slots().find(|e| e.suffix == c)).or_else(|| slots().max_by_key(|e| e.modified))
        .map(|e| e.suffix.clone())
}

fn saves_payload(root: &std::path::Path, id: &str) -> Result<SavesPayload, String> {
    let Some(cfg) = release_saves(root, id)? else {
        return Ok(SavesPayload { supported: false, slots: vec![], all_slots: vec![], chosen: None, continues: None, free: None });
    };
    let dir = install::savedata(root, id);
    let list = saves::list(&dir, &cfg, false);
    let chosen = settings::launcher_get(root, &chosen_key(id));
    let cont = continues(&list, chosen.as_deref());
    Ok(SavesPayload {
        supported: true,
        all_slots: (cfg.first_slot..=cfg.last_slot).map(|n| saves::slot_suffix(&cfg, n)).collect(),
        free: saves::free_slot(&dir, &cfg).map(|n| saves::slot_suffix(&cfg, n)),
        slots: list.into_iter().map(|e| view(&dir, e)).collect(),
        chosen, continues: cont,
    })
}

fn transfer_err(e: saves::TransferError) -> String {
    match e { saves::TransferError::Exists => "exists".into(), saves::TransferError::Io(m) => m }
}

fn do_import(root: &std::path::Path, pp: &std::path::Path, id: &str, folder: &str, target_suffix: &str, replace: bool) -> Result<String, String> {
    let cfg = release_saves(root, id)?.ok_or("this release has no save facts")?;
    if !saves::list(pp, &cfg, true).iter().any(|e| e.folder == folder && !e.damaged) {
        return Err(format!("{folder} is not a Patapon 3 save in {}", pp.display()));
    }
    if !saves::valid_suffix(target_suffix) { return Err(format!("bad slot {target_suffix:?}")); }
    let target = format!("{}{target_suffix}", cfg.id);
    let backup = saves::backup_dir(root, &format!("port-{id}"), std::time::SystemTime::now());
    saves::transfer(&pp.join(folder), &install::savedata(root, id), &target, replace, &backup).map_err(transfer_err)?;
    if saves::classify(&cfg, target_suffix).0 == saves::Kind::Slot {
        settings::launcher_set(root, &chosen_key(id), target_suffix)?;
    }
    Ok(target)
}

fn do_export(root: &std::path::Path, pp: &std::path::Path, id: &str, suffix: &str, replace: bool) -> Result<String, String> {
    let cfg = release_saves(root, id)?.ok_or("this release has no save facts")?;
    let port = install::savedata(root, id);
    let entry = saves::list(&port, &cfg, false).into_iter().find(|e| e.suffix == suffix && !e.damaged)
        .ok_or_else(|| format!("no save {suffix} in this release"))?;
    let backup = saves::backup_dir(root, "ppsspp", std::time::SystemTime::now());
    saves::transfer(&port.join(&entry.folder), pp, &entry.folder, replace, &backup).map_err(transfer_err)?;
    Ok(entry.folder)
}

#[tauri::command]
fn list_saves(id: String) -> Result<SavesPayload, String> { saves_payload(&settings::root_dir(), &id) }

#[tauri::command]
fn choose_save(id: String, suffix: String) -> Result<(), String> {
    if !saves::valid_suffix(&suffix) { return Err(format!("bad slot {suffix:?}")); }
    release_saves(&settings::root_dir(), &id)?;
    settings::launcher_set(&settings::root_dir(), &chosen_key(&id), &suffix)
}

#[tauri::command]
fn ppsspp_folder() -> Option<String> { ppsspp_dir(&settings::root_dir()).map(|p| p.to_string_lossy().into_owned()) }

#[tauri::command]
fn choose_ppsspp_folder(path: String) -> Result<String, String> {
    let dir = ppsspp::normalize(std::path::Path::new(&path)).ok_or("not_found")?;
    let s = dir.to_string_lossy().into_owned();
    settings::launcher_set(&settings::root_dir(), "ppsspp_savedata", &s)?;
    Ok(s)
}

#[tauri::command]
fn list_ppsspp_saves(id: String) -> Result<Vec<SaveView>, String> {
    let root = settings::root_dir();
    let cfg = release_saves(&root, &id)?.ok_or("this release has no save facts")?;
    let pp = ppsspp_dir(&root).ok_or("not_found")?;
    Ok(saves::list(&pp, &cfg, true).into_iter().map(|e| view(&pp, e)).collect())
}

#[tauri::command]
async fn import_save(id: String, folder: String, target_suffix: String, replace: bool) -> Result<String, String> {
    let root = settings::root_dir();
    let pp = ppsspp_dir(&root).ok_or("not_found")?;
    tauri::async_runtime::spawn_blocking(move || do_import(&root, &pp, &id, &folder, &target_suffix, replace))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn export_save(id: String, suffix: String, replace: bool) -> Result<String, String> {
    let root = settings::root_dir();
    let pp = ppsspp_dir(&root).ok_or("not_found")?;
    tauri::async_runtime::spawn_blocking(move || do_export(&root, &pp, &id, &suffix, replace))
        .await.map_err(|e| e.to_string())?
}
```
  Register the seven new commands in `generate_handler![...]` (`list_saves, choose_save, ppsspp_folder,
  choose_ppsspp_folder, list_ppsspp_saves, import_save, export_save`). In `setup`, after
  `install::cleanup_partials(...)`:

```rust
            let root = settings::root_dir();
            for id in install::installed(&root) { saves::cleanup_partials(&install::savedata(&root, &id)); }
```

- [ ] **Step 3: `launch.rs`.** `game_env` becomes:

```rust
/// The release's folders, as environment variables (they take precedence over settings.ini),
/// plus the save the player chose in Saves (PSPRECOMP_SAVE_SLOT) while that slot exists.
pub fn game_env(root: &Path, id: &str) -> Vec<(String, String)> {
    let mut env = vec![
        ("PSPRECOMP_DISC0".into(), crate::install::disc0(root, id).to_string_lossy().into_owned()),
        ("PSPRECOMP_SAVEDATA".into(), crate::install::savedata(root, id).to_string_lossy().into_owned()),
    ];
    let cfg = crate::releases::load(root).into_iter().find(|r| r.id == id).and_then(|r| r.saves);
    if let (Some(cfg), Some(slot)) = (cfg, crate::settings::launcher_get(root, &format!("save_{id}"))) {
        if crate::saves::valid_suffix(&slot) && crate::install::savedata(root, id).join(format!("{}{slot}", cfg.id)).is_dir() {
            env.push(("PSPRECOMP_SAVE_SLOT".into(), slot));
        }
    }
    env
}
```

- [ ] **Step 4: Tests pass.** Launcher Rust gate → 65 passed, 1 ignored. `cargo build -q` has no warnings from
  `saves.rs` / `ppsspp.rs` any more (the warning count is part of the report).

- [ ] **Step 5: Report (no commit).** Claude commits: `feat(launcher): save commands; the chosen save reaches the game`.

---

### Task 7: Launcher UI — Saves, Import from PPSSPP, main menu

**Files (launcher):**
- Create: `src/components/SaveRow.tsx`, `src/screens/Choice.tsx`, `src/screens/Saves.tsx`,
  `src/screens/ImportSaves.tsx`, `src/screens/Saves.test.tsx`, `src/screens/ImportSaves.test.tsx`
- Modify: `src/types.ts`, `src/api.ts`, `src/App.tsx`, `src/screens/MainMenu.tsx`,
  `src/screens/MainMenu.test.tsx`, `src/screens/Start.test.tsx` (api mock), `src/styles.css`,
  `lang/es.json`, `lang/en.json`, `README.md`

**Interfaces:**
- Consumes: Task 6 commands.
- Produces: `MainMenu` props `onSaves?: () => void` (entry shown only when given) and
  `saveLine?: string` (shown under the menu); screens `Saves` and `ImportSaves` (props below).

- [ ] **Step 1: Strings.** Add to `lang/es.json`:

```json
  "menu.saves": "Partidas",
  "menu.save_line": "Partida: {line}",
  "saves.title": "PARTIDAS",
  "saves.empty": "Vacía",
  "saves.continues": "★ Continuar carga esta",
  "saves.extra": "También: {names}",
  "saves.system": "datos de sistema",
  "saves.import": "Importar de PPSSPP",
  "saves.open_folder": "Abrir carpeta de partidas",
  "saves.slot_continue": "Continuar con esta partida",
  "saves.slot_export": "Exportar a PPSSPP",
  "saves.cancel": "Cancelar",
  "saves.ok": "Aceptar",
  "saves.chosen": "Al pulsar Continuar en el juego se cargará {slot}.",
  "saves.exported": "Exportada a PPSSPP como {folder}.",
  "saves.export_exists": "PPSSPP ya tiene una partida en {slot}. ¿Reemplazarla? (se guarda una copia)",
  "saves.replace": "Reemplazar (se guarda una copia)",
  "saves.use_free": "Usar la ranura libre {slot}",
  "saves.slot_taken": "La ranura {slot} ya tiene una partida: {detail}",
  "saves.imported": "Importada en {slot}.",
  "saves.converted": "EE. UU. → se convierte",
  "saves.damaged": "Dañada",
  "saves.ppsspp_title": "PARTIDAS DE PPSSPP",
  "saves.ppsspp_missing": "No encontré la carpeta de PPSSPP.",
  "saves.ppsspp_choose": "Elegir la carpeta de PPSSPP",
  "saves.ppsspp_change": "Cambiar de carpeta",
  "saves.ppsspp_bad": "En esa carpeta no hay partidas de PSP (PSP/SAVEDATA).",
  "saves.ppsspp_none": "No hay partidas de Patapon 3 en {path}.",
  "saves.error": "No se pudo copiar la partida: {message}"
```
  and to `lang/en.json`:

```json
  "menu.saves": "Saves",
  "menu.save_line": "Save: {line}",
  "saves.title": "SAVES",
  "saves.empty": "Empty",
  "saves.continues": "★ Continue loads this one",
  "saves.extra": "Also: {names}",
  "saves.system": "system data",
  "saves.import": "Import from PPSSPP",
  "saves.open_folder": "Open the save folder",
  "saves.slot_continue": "Continue with this save",
  "saves.slot_export": "Export to PPSSPP",
  "saves.cancel": "Cancel",
  "saves.ok": "OK",
  "saves.chosen": "Continue in the game will load {slot}.",
  "saves.exported": "Exported to PPSSPP as {folder}.",
  "saves.export_exists": "PPSSPP already has a save in {slot}. Replace it? (a copy is kept)",
  "saves.replace": "Replace (a copy is kept)",
  "saves.use_free": "Use the free slot {slot}",
  "saves.slot_taken": "Slot {slot} already has a save: {detail}",
  "saves.imported": "Imported into {slot}.",
  "saves.converted": "USA → converted",
  "saves.damaged": "Damaged",
  "saves.ppsspp_title": "PPSSPP SAVES",
  "saves.ppsspp_missing": "The PPSSPP folder was not found.",
  "saves.ppsspp_choose": "Choose the PPSSPP folder",
  "saves.ppsspp_change": "Change folder",
  "saves.ppsspp_bad": "That folder has no PSP saves (PSP/SAVEDATA).",
  "saves.ppsspp_none": "There are no Patapon 3 saves in {path}.",
  "saves.error": "The save could not be copied: {message}"
```
  Remove `options.open_saves` from both files (the action moves to Saves).

- [ ] **Step 2: Types and api.** `src/types.ts`: `Release` gains `saves?: SavesCfg | null` and add:

```ts
export type SavesCfg = { id: string; import_from: string[]; system: string; slot_prefix: string; first_slot: number; last_slot: number };
export type SaveKind = "slot" | "system" | "other";
// One save folder as the launcher lists it (PARAM.SFO + ICON0.PNG as a data: URL).
export type SaveEntry = { folder: string; suffix: string; kind: SaveKind; slot: number | null; title: string; detail: string;
  converted: boolean; encrypted: boolean; damaged: boolean; modified: number; icon: string };
export type SavesPayload = { supported: boolean; slots: SaveEntry[]; all_slots: string[]; chosen: string | null;
  continues: string | null; free: string | null };
```
  `src/api.ts` gains (import the new types):

```ts
  listSaves: (id: string) => invoke<SavesPayload>("list_saves", { id }),
  chooseSave: (id: string, suffix: string) => invoke<void>("choose_save", { id, suffix }),
  ppssppFolder: () => invoke<string | null>("ppsspp_folder"),
  choosePpssppFolder: (path: string) => invoke<string>("choose_ppsspp_folder", { path }),
  listPpssppSaves: (id: string) => invoke<SaveEntry[]>("list_ppsspp_saves", { id }),
  importSave: (id: string, folder: string, targetSuffix: string, replace: boolean) =>
    invoke<string>("import_save", { id, folder, targetSuffix, replace }),
  exportSave: (id: string, suffix: string, replace: boolean) => invoke<string>("export_save", { id, suffix, replace }),
  pickFolder: async (): Promise<string | null> => {
    const { open } = await import("@tauri-apps/plugin-dialog");
    const p = await open({ multiple: false, directory: true });
    return typeof p === "string" ? p : null;
  },
```

- [ ] **Step 3: Shared pieces.** `src/components/SaveRow.tsx`:

```tsx
import type { SaveEntry } from "../types";

// "_DATA04" -> "DATA04", the name the player sees.
export const slotLabel = (suffix: string) => suffix.replace(/^_/, "");
export const firstLine = (detail: string) => detail.split("\n").map((l) => l.trim()).find((l) => l) ?? "";

type Props = { suffix: string; save?: SaveEntry; selected: boolean; star?: string; tags?: string[]; emptyLabel: string;
  onHover(): void; onClick(): void };

// One slot like the game's LOAD screen: icon, slot name, title and detail lines.
export function SaveRow(p: Props) {
  return (
    <li role="option" aria-selected={p.selected} className={`save-row${p.selected ? " selected" : ""}${p.save ? "" : " empty"}`}
        data-testid={`save-${slotLabel(p.suffix)}`} onMouseEnter={p.onHover} onClick={p.onClick}>
      <div className="save-icon">{p.save?.icon ? <img src={p.save.icon} alt="" /> : null}</div>
      <div className="save-text">
        <div className="save-head">
          <span className="save-slot">{slotLabel(p.suffix)}</span>
          {p.save && <span className="save-title">{p.save.title}</span>}
          {(p.tags ?? []).map((t) => <span key={t} className="save-tag">{t}</span>)}
          {p.star && <span className="save-star">{p.star}</span>}
        </div>
        {p.save ? <pre className="save-detail">{p.save.detail.trim()}</pre> : <p className="save-detail muted">{p.emptyLabel}</p>}
      </div>
    </li>
  );
}
```
  `src/screens/Choice.tsx`:

```tsx
import { PromptBar } from "../components/PromptBar";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";

type Props = { strings: Record<string, string>; text: string; options: { id: string; label: string; run(): void }[]; onBack(): void };

// A message with a few choices (slot menu, replace?, done). Mounted alone, so it owns navigation.
export function Choice(p: Props) {
  const nav = useListNav(p.options.length, (i) => p.options[i].run(), p.onBack);
  return (
    <div className="screen notice" data-testid="choice">
      <p className="panel notice-text">{p.text}</p>
      <ul className="menu" role="listbox">
        {p.options.map((o, i) => (
          <li key={o.id} role="option" aria-selected={nav.index === i} className={nav.index === i ? "selected" : ""}
              onMouseEnter={() => nav.setIndex(i)} onClick={() => o.run()}>{o.label}</li>
        ))}
      </ul>
      <PromptBar device={nav.device} items={[{ id: "select", label: t(p.strings, "prompt.select") },
        { id: "confirm", label: t(p.strings, "prompt.confirm") }, { id: "back", label: t(p.strings, "prompt.back") }]} />
    </div>
  );
}
```

- [ ] **Step 4: Failing UI tests.** `src/screens/Saves.test.tsx`:

```tsx
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
const api = vi.hoisted(() => ({ listSaves: vi.fn(), chooseSave: vi.fn(), exportSave: vi.fn(), openSaveFolder: vi.fn() }));
vi.mock("../api", () => ({ api }));
import { Saves } from "./Saves";

const strings = { "saves.title": "PARTIDAS", "saves.empty": "Vacía", "saves.continues": "★ Continuar carga esta",
  "saves.import": "Importar de PPSSPP", "saves.open_folder": "Abrir carpeta", "prompt.back": "Volver",
  "saves.slot_continue": "Continuar con esta partida", "saves.slot_export": "Exportar a PPSSPP", "saves.cancel": "Cancelar",
  "saves.ok": "Aceptar", "saves.chosen": "Se cargará {slot}.", "saves.exported": "Exportada como {folder}.",
  "saves.export_exists": "PPSSPP ya tiene {slot}. ¿Reemplazar?", "saves.replace": "Reemplazar", "saves.error": "Error: {message}",
  "saves.extra": "También: {names}", "saves.system": "datos de sistema" };
const save = (suffix: string, detail: string, modified = 1) => ({ folder: `UCES01421${suffix}`, suffix, kind: "slot", slot: Number(suffix.slice(-2)),
  title: "Datos guardados", detail, converted: false, encrypted: false, damaged: false, modified, icon: "" });
const payload = (chosen: string | null, continues: string) => ({ supported: true, chosen, continues, free: "_DATA02",
  all_slots: Array.from({ length: 10 }, (_, i) => `_DATA${String(i + 1).padStart(2, "0")}`),
  slots: [save("_DATA01", "Héroe: Hero\nClase: Tondenga Nv 7"), save("_DATA04", "Héroe: Edson\nClase: Yumiyacha Nv 35"),
    { ...save("_DATA00", " "), kind: "system", slot: null }] });

beforeEach(() => { vi.clearAllMocks(); api.listSaves.mockResolvedValue(payload(null, "_DATA01")); });

test("ten slots like the LOAD screen, the star on the save Continue loads", async () => {
  render(<Saves strings={strings} releaseId="dxd-1.1.4" message="" onBack={vi.fn()} onImport={vi.fn()} />);
  expect(await screen.findByText(/Clase: Yumiyacha Nv 35/)).toBeInTheDocument();
  expect(screen.getAllByText("Vacía")).toHaveLength(8);
  expect(screen.getByTestId("save-DATA01")).toHaveTextContent("★ Continuar carga esta");
  expect(screen.getByTestId("save-DATA04")).not.toHaveTextContent("★");
  expect(screen.getByText("También: datos de sistema")).toBeInTheDocument();
});

test("Confirm on a slot -> continue with it", async () => {
  api.chooseSave.mockResolvedValue(undefined);
  render(<Saves strings={strings} releaseId="dxd-1.1.4" message="" onBack={vi.fn()} onImport={vi.fn()} />);
  await screen.findByTestId("save-DATA04");
  await userEvent.keyboard("{ArrowDown}{ArrowDown}{ArrowDown}{Enter}");
  expect(await screen.findByText("Continuar con esta partida")).toBeInTheDocument();
  api.listSaves.mockResolvedValue(payload("_DATA04", "_DATA04"));
  await userEvent.keyboard("{Enter}");
  expect(api.chooseSave).toHaveBeenCalledWith("dxd-1.1.4", "_DATA04");
  expect(await screen.findByText("Se cargará DATA04.")).toBeInTheDocument();
});

test("export asks before replacing PPSSPP's save", async () => {
  api.exportSave.mockRejectedValueOnce("exists").mockResolvedValueOnce("UCES01421_DATA01");
  render(<Saves strings={strings} releaseId="dxd-1.1.4" message="" onBack={vi.fn()} onImport={vi.fn()} />);
  await userEvent.click(await screen.findByTestId("save-DATA01"));
  await userEvent.click(await screen.findByText("Exportar a PPSSPP"));
  expect(api.exportSave).toHaveBeenCalledWith("dxd-1.1.4", "_DATA01", false);
  await userEvent.click(await screen.findByText("Reemplazar"));
  expect(api.exportSave).toHaveBeenLastCalledWith("dxd-1.1.4", "_DATA01", true);
  expect(await screen.findByText("Exportada como UCES01421_DATA01.")).toBeInTheDocument();
});

test("empty slots do nothing; actions and Back work", async () => {
  const onBack = vi.fn(), onImport = vi.fn();
  render(<Saves strings={strings} releaseId="dxd-1.1.4" message="Importada en DATA04." onBack={onBack} onImport={onImport} />);
  expect(screen.getByText("Importada en DATA04.")).toBeInTheDocument();
  await userEvent.click(await screen.findByTestId("save-DATA02"));
  expect(screen.queryByTestId("choice")).toBeNull();
  await userEvent.click(screen.getByText("Importar de PPSSPP"));
  expect(onImport).toHaveBeenCalledOnce();
  await userEvent.click(screen.getByText("Abrir carpeta"));
  expect(api.openSaveFolder).toHaveBeenCalledWith("dxd-1.1.4");
  await userEvent.keyboard("{Escape}");
  expect(onBack).toHaveBeenCalledOnce();
});
```
  `src/screens/ImportSaves.test.tsx`:

```tsx
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
const api = vi.hoisted(() => ({ ppssppFolder: vi.fn(), choosePpssppFolder: vi.fn(), listPpssppSaves: vi.fn(),
  listSaves: vi.fn(), importSave: vi.fn(), pickFolder: vi.fn() }));
vi.mock("../api", () => ({ api }));
import { ImportSaves } from "./ImportSaves";

const strings = { "saves.ppsspp_title": "PARTIDAS DE PPSSPP", "saves.ppsspp_missing": "No encontré PPSSPP.",
  "saves.ppsspp_choose": "Elegir carpeta", "saves.ppsspp_change": "Cambiar de carpeta", "saves.ppsspp_bad": "Sin partidas PSP.",
  "saves.ppsspp_none": "Nada en {path}.", "saves.converted": "EE. UU. → se convierte", "saves.damaged": "Dañada",
  "saves.replace": "Reemplazar", "saves.use_free": "Usar {slot}", "saves.cancel": "Cancelar", "prompt.back": "Volver",
  "saves.slot_taken": "{slot} ocupada: {detail}", "saves.imported": "Importada en {slot}.", "saves.error": "Error: {message}",
  "saves.empty": "Vacía" };
const entry = (folder: string, detail: string, converted = false) => ({ folder, suffix: folder.slice(9), kind: "slot",
  slot: Number(folder.slice(-2)), title: "Datos", detail, converted, encrypted: true, damaged: false, modified: 1, icon: "" });
const port = (taken: string[]) => ({ supported: true, chosen: null, continues: null, free: "_DATA03",
  all_slots: [], slots: taken.map((s) => entry(`UCES01421${s}`, "Héroe: Hero")) });

beforeEach(() => vi.clearAllMocks());

test("no PPSSPP folder: choose one", async () => {
  api.ppssppFolder.mockResolvedValueOnce(null).mockResolvedValue("E:/PPSSPP/memstick/PSP/SAVEDATA");
  api.pickFolder.mockResolvedValue("E:/PPSSPP");
  api.choosePpssppFolder.mockResolvedValue("E:/PPSSPP/memstick/PSP/SAVEDATA");
  api.listPpssppSaves.mockResolvedValue([entry("UCES01421_DATA04", "Héroe: Edson")]);
  render(<ImportSaves strings={strings} releaseId="dxd-1.1.4" onDone={vi.fn()} onBack={vi.fn()} />);
  expect(await screen.findByText("No encontré PPSSPP.")).toBeInTheDocument();
  await userEvent.click(screen.getByText("Elegir carpeta"));
  expect(api.choosePpssppFolder).toHaveBeenCalledWith("E:/PPSSPP");
  expect(await screen.findByText(/Héroe: Edson/)).toBeInTheDocument();
});

test("a bad folder says so", async () => {
  api.ppssppFolder.mockResolvedValue(null);
  api.pickFolder.mockResolvedValue("C:/Users");
  api.choosePpssppFolder.mockRejectedValue("not_found");
  render(<ImportSaves strings={strings} releaseId="dxd-1.1.4" onDone={vi.fn()} onBack={vi.fn()} />);
  await userEvent.click(await screen.findByText("Elegir carpeta"));
  expect(await screen.findByText("Sin partidas PSP.")).toBeInTheDocument();
});

test("US save into a free slot when its slot is taken", async () => {
  api.ppssppFolder.mockResolvedValue("E:/pp");
  api.listPpssppSaves.mockResolvedValue([entry("UCUS98751_DATA02", "Uberhero: MoMo", true)]);
  api.listSaves.mockResolvedValue(port(["_DATA02"]));
  api.importSave.mockResolvedValue("UCES01421_DATA03");
  const onDone = vi.fn();
  render(<ImportSaves strings={strings} releaseId="dxd-1.1.4" onDone={onDone} onBack={vi.fn()} />);
  expect(await screen.findByText("EE. UU. → se convierte")).toBeInTheDocument();
  await userEvent.click(screen.getByTestId("save-DATA02"));
  expect(await screen.findByText("DATA02 ocupada: Héroe: Hero")).toBeInTheDocument();
  await userEvent.click(screen.getByText("Usar DATA03"));
  expect(api.importSave).toHaveBeenCalledWith("dxd-1.1.4", "UCUS98751_DATA02", "_DATA03", false);
  expect(onDone).toHaveBeenCalledWith("Importada en DATA03.");
});

test("replace keeps a copy; a free slot imports straight away", async () => {
  api.ppssppFolder.mockResolvedValue("E:/pp");
  api.listPpssppSaves.mockResolvedValue([entry("UCES01421_DATA04", "Héroe: Edson"), entry("UCES01421_DATA05", "Héroe: Lobo")]);
  api.listSaves.mockResolvedValue(port(["_DATA04"]));
  api.importSave.mockResolvedValue("UCES01421_DATA04");
  const onDone = vi.fn();
  render(<ImportSaves strings={strings} releaseId="dxd-1.1.4" onDone={onDone} onBack={vi.fn()} />);
  await userEvent.click(await screen.findByTestId("save-DATA04"));
  await userEvent.click(await screen.findByText("Reemplazar"));
  expect(api.importSave).toHaveBeenCalledWith("dxd-1.1.4", "UCES01421_DATA04", "_DATA04", true);
  api.importSave.mockResolvedValue("UCES01421_DATA05");
  render(<ImportSaves strings={strings} releaseId="dxd-1.1.4" onDone={onDone} onBack={vi.fn()} />);
  await userEvent.click((await screen.findAllByTestId("save-DATA05"))[0]);
  expect(api.importSave).toHaveBeenLastCalledWith("dxd-1.1.4", "UCES01421_DATA05", "_DATA05", false);
});
```
  `src/screens/MainMenu.test.tsx` gains:

```tsx
test("Saves entry and the chosen save line", async () => {
  const onSaves = vi.fn();
  render(<MainMenu strings={{ ...strings, "menu.saves": "Partidas", "menu.save_line": "Partida: {line}" }} logo="" background=""
    onStart={vi.fn()} onOptions={vi.fn()} onExit={vi.fn()} busy={false} onSaves={onSaves} saveLine="Héroe: Edson" />);
  await userEvent.keyboard("{ArrowDown}{Enter}");
  expect(onSaves).toHaveBeenCalledOnce();
  expect(screen.getByText("Partida: Héroe: Edson")).toBeInTheDocument();
});
```
  `src/screens/Start.test.tsx`: the hoisted `api` mock gains
  `listSaves: vi.fn(async () => ({ supported: false, slots: [], all_slots: [], chosen: null, continues: null, free: null })),`.
  Run the launcher UI gate. Expected: the new tests fail (modules missing).

- [ ] **Step 5: `Saves.tsx`.**

```tsx
import { useCallback, useEffect, useState } from "react";
import { api } from "../api";
import { PromptBar } from "../components/PromptBar";
import { SaveRow, slotLabel } from "../components/SaveRow";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";
import type { SaveEntry, SavesPayload } from "../types";
import { Choice } from "./Choice";

type Props = { strings: Record<string, string>; releaseId: string; message: string; onBack(): void; onImport(): void };
type Pending = { text: string; options: { id: string; label: string; run(): void }[] };

// The release's ten game slots like the game's LOAD screen. Confirm on a save: continue with it
// (the game's Continue loads it) or export it to PPSSPP.
export function Saves(p: Props) {
  const [data, setData] = useState<SavesPayload | null>(null);
  const [pending, setPending] = useState<Pending | null>(null);
  const [message, setMessage] = useState(p.message);
  const s = p.strings;
  const reload = useCallback(async () => { try { setData(await api.listSaves(p.releaseId)); } catch { setData(null); } }, [p.releaseId]);
  useEffect(() => { void reload(); }, [reload]);

  const slots = data?.all_slots ?? [];
  const bySuffix = new Map((data?.slots ?? []).map((e) => [e.suffix, e] as const));
  const extras = (data?.slots ?? []).filter((e) => e.kind !== "slot");
  const done = (text: string) => setPending({ text, options: [{ id: "ok", label: t(s, "saves.ok"), run: () => setPending(null) }] });
  const fail = (e: unknown) => done(t(s, "saves.error", { message: String(e) }));

  const exportSave = async (save: SaveEntry, replace: boolean) => {
    try {
      const folder = await api.exportSave(p.releaseId, save.suffix, replace);
      done(t(s, "saves.exported", { folder }));
    } catch (e) {
      if (String(e) === "exists" && !replace) {
        setPending({ text: t(s, "saves.export_exists", { slot: slotLabel(save.suffix) }), options: [
          { id: "replace", label: t(s, "saves.replace"), run: () => { void exportSave(save, true); } },
          { id: "cancel", label: t(s, "saves.cancel"), run: () => setPending(null) }] });
      } else fail(e);
    }
  };
  const openSlot = (suffix: string) => {
    const save = bySuffix.get(suffix);
    if (!save || save.damaged) return;
    setPending({ text: `${slotLabel(suffix)} — ${save.detail.trim()}`, options: [
      { id: "continue", label: t(s, "saves.slot_continue"), run: () => {
        api.chooseSave(p.releaseId, suffix).then(async () => { await reload(); setMessage(t(s, "saves.chosen", { slot: slotLabel(suffix) })); setPending(null); }).catch(fail);
      } },
      { id: "export", label: t(s, "saves.slot_export"), run: () => { void exportSave(save, false); } },
      { id: "cancel", label: t(s, "saves.cancel"), run: () => setPending(null) }] });
  };
  const actions = [
    { id: "import", label: t(s, "saves.import"), run: p.onImport },
    { id: "open", label: t(s, "saves.open_folder"), run: () => { void api.openSaveFolder(p.releaseId); } },
    { id: "back", label: t(s, "prompt.back"), run: p.onBack },
  ];
  const run = (i: number) => (i < slots.length ? openSlot(slots[i]) : actions[i - slots.length]?.run());

  if (pending) return <Choice key={pending.text} strings={s} text={pending.text} options={pending.options} onBack={() => setPending(null)} />;
  return <SavesList strings={s} slots={slots} bySuffix={bySuffix} continues={data?.continues ?? null} extras={extras}
    message={message} actions={actions} run={run} onBack={p.onBack} />;
}

type ListProps = { strings: Record<string, string>; slots: string[]; bySuffix: Map<string, SaveEntry>; continues: string | null;
  extras: SaveEntry[]; message: string; actions: { id: string; label: string }[]; run(i: number): void; onBack(): void };

// Split out so the navigation hook is only mounted while the list is on screen.
function SavesList(p: ListProps) {
  const nav = useListNav(p.slots.length + p.actions.length, p.run, p.onBack);
  const s = p.strings;
  const extraNames = p.extras.map((e) => (e.kind === "system" ? t(s, "saves.system") : slotLabel(e.suffix))).join(", ");
  return (
    <div className="screen saves">
      <h1 className="title">{t(s, "saves.title")}</h1>
      {p.message && <p className="saves-message" role="status">{p.message}</p>}
      <ul className="panel save-list" role="listbox">
        {p.slots.map((suffix, i) => (
          <SaveRow key={suffix} suffix={suffix} save={p.bySuffix.get(suffix)} selected={nav.index === i}
            star={p.continues === suffix ? t(s, "saves.continues") : undefined} emptyLabel={t(s, "saves.empty")}
            onHover={() => nav.setIndex(i)} onClick={() => p.run(i)} />
        ))}
      </ul>
      {extraNames && <p className="muted">{t(s, "saves.extra", { names: extraNames })}</p>}
      <ul className="menu save-actions" role="listbox">
        {p.actions.map((a, i) => (
          <li key={a.id} role="option" aria-selected={nav.index === p.slots.length + i}
              className={nav.index === p.slots.length + i ? "selected" : ""}
              onMouseEnter={() => nav.setIndex(p.slots.length + i)} onClick={() => p.run(p.slots.length + i)}>{a.label}</li>
        ))}
      </ul>
      <PromptBar device={nav.device} items={[{ id: "select", label: t(s, "prompt.select") },
        { id: "confirm", label: t(s, "prompt.confirm") }, { id: "back", label: t(s, "prompt.back") }]} />
    </div>
  );
}
```

- [ ] **Step 6: `ImportSaves.tsx`.**

```tsx
import { useCallback, useEffect, useState } from "react";
import { api } from "../api";
import { PromptBar } from "../components/PromptBar";
import { SaveRow, firstLine, slotLabel } from "../components/SaveRow";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";
import type { SaveEntry } from "../types";
import { Choice } from "./Choice";

type Props = { strings: Record<string, string>; releaseId: string; onDone(message: string): void; onBack(): void };
type Pending = { text: string; options: { id: string; label: string; run(): void }[] };

// PPSSPP's Patapon 3 saves (EU and US). Picking one copies it into the same slot of this
// release, asking first when that slot is taken.
export function ImportSaves(p: Props) {
  const s = p.strings;
  const [folder, setFolder] = useState<string | null | undefined>(undefined);
  const [list, setList] = useState<SaveEntry[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [error, setError] = useState("");

  const load = useCallback(async () => {
    const f = await api.ppssppFolder().catch(() => null);
    setFolder(f);
    setList(f ? await api.listPpssppSaves(p.releaseId).catch(() => []) : []);
  }, [p.releaseId]);
  useEffect(() => { void load(); }, [load]);

  const choose = async () => {
    const path = await api.pickFolder().catch(() => null);
    if (!path) return;
    try { await api.choosePpssppFolder(path); setError(""); await load(); } catch { setError(t(s, "saves.ppsspp_bad")); }
  };
  const doImport = async (src: SaveEntry, target: string, replace: boolean) => {
    try {
      await api.importSave(p.releaseId, src.folder, target, replace);
      p.onDone(t(s, "saves.imported", { slot: slotLabel(target) }));
    } catch (e) {
      setPending({ text: t(s, "saves.error", { message: String(e) }), options: [{ id: "ok", label: t(s, "prompt.back"), run: () => setPending(null) }] });
    }
  };
  const pick = async (src: SaveEntry) => {
    if (src.damaged) return;
    const port = await api.listSaves(p.releaseId).catch(() => null);
    const taken = port?.slots.find((e) => e.suffix === src.suffix);
    if (!taken) { void doImport(src, src.suffix, false); return; }
    const options = [{ id: "replace", label: t(s, "saves.replace"), run: () => { void doImport(src, src.suffix, true); } }];
    if (src.kind === "slot" && port?.free) {
      const free = port.free;
      options.push({ id: "free", label: t(s, "saves.use_free", { slot: slotLabel(free) }), run: () => { void doImport(src, free, false); } });
    }
    options.push({ id: "cancel", label: t(s, "saves.cancel"), run: () => setPending(null) });
    setPending({ text: t(s, "saves.slot_taken", { slot: slotLabel(src.suffix), detail: firstLine(taken.detail) }), options });
  };

  if (pending) return <Choice key={pending.text} strings={s} text={pending.text} options={pending.options} onBack={() => setPending(null)} />;
  if (folder === undefined) return <div className="screen" />;
  if (folder === null) {
    return <Choice key={`missing${error}`} strings={s} text={error || t(s, "saves.ppsspp_missing")} onBack={p.onBack} options={[
      { id: "choose", label: t(s, "saves.ppsspp_choose"), run: () => { void choose(); } },
      { id: "back", label: t(s, "prompt.back"), run: p.onBack }]} />;
  }
  return <ImportList strings={s} folder={folder} list={list} error={error} onPick={(e) => { void pick(e); }}
    onChange={() => { void choose(); }} onBack={p.onBack} />;
}

type ListProps = { strings: Record<string, string>; folder: string; list: SaveEntry[]; error: string;
  onPick(e: SaveEntry): void; onChange(): void; onBack(): void };

function ImportList(p: ListProps) {
  const s = p.strings;
  const actions = [{ id: "change", label: t(s, "saves.ppsspp_change"), run: p.onChange }, { id: "back", label: t(s, "prompt.back"), run: p.onBack }];
  const run = (i: number) => (i < p.list.length ? p.onPick(p.list[i]) : actions[i - p.list.length]?.run());
  const nav = useListNav(p.list.length + actions.length, run, p.onBack);
  return (
    <div className="screen saves">
      <h1 className="title">{t(s, "saves.ppsspp_title")}</h1>
      <p className="muted saves-path">{p.folder}</p>
      {p.error && <p className="saves-message" role="alert">{p.error}</p>}
      {p.list.length === 0 && <p className="saves-message">{t(s, "saves.ppsspp_none", { path: p.folder })}</p>}
      <ul className="panel save-list" role="listbox">
        {p.list.map((e, i) => (
          <SaveRow key={e.folder} suffix={e.suffix} save={e} selected={nav.index === i} emptyLabel={t(s, "saves.empty")}
            tags={[...(e.converted ? [t(s, "saves.converted")] : []), ...(e.damaged ? [t(s, "saves.damaged")] : [])]}
            onHover={() => nav.setIndex(i)} onClick={() => run(i)} />
        ))}
      </ul>
      <ul className="menu save-actions" role="listbox">
        {actions.map((a, i) => (
          <li key={a.id} role="option" aria-selected={nav.index === p.list.length + i}
              className={nav.index === p.list.length + i ? "selected" : ""}
              onMouseEnter={() => nav.setIndex(p.list.length + i)} onClick={() => a.run()}>{a.label}</li>
        ))}
      </ul>
      <PromptBar device={nav.device} items={[{ id: "select", label: t(s, "prompt.select") },
        { id: "confirm", label: t(s, "prompt.confirm") }, { id: "back", label: t(s, "prompt.back") }]} />
    </div>
  );
}
```

- [ ] **Step 7: Main menu + App.** `MainMenu.tsx`: props gain `onSaves?: () => void; saveLine?: string`;
  `entries` becomes

```tsx
  const entries = [
    { id: "start", label: t(p.strings, "menu.start"), run: () => { if (!p.busy) p.onStart(); } },
    ...(p.onSaves ? [{ id: "saves", label: t(p.strings, "menu.saves"), run: p.onSaves }] : []),
    { id: "options", label: t(p.strings, "menu.options"), run: p.onOptions },
    { id: "exit", label: t(p.strings, "menu.exit"), run: p.onExit },
  ];
```
  and after the `</ul>` of the menu:
  `{p.saveLine && <p className="save-line">{t(p.strings, "menu.save_line", { line: p.saveLine })}</p>}`.

  `App.tsx`:
  - `type Screen` gains `"saves" | "import"`; new state `const [savesMsg, setSavesMsg] = useState("");`
    and `const [saveLine, setSaveLine] = useState("");`;
  - imports `Saves`, `ImportSaves`, `firstLine`;
  - `const current = versions?.releases.find((r) => r.id === versions?.current);`
    `const hasSaves = !!current?.saves;`
  - an effect that refreshes the menu's save line:

```tsx
  useEffect(() => {
    const id = versions?.current;
    if (screen !== "menu" || !id || !hasSaves) { setSaveLine(""); return; }
    api.listSaves(id).then((p) => {
      const s = p.slots.find((e) => e.suffix === p.continues);
      setSaveLine(s ? firstLine(s.detail) : "");
    }).catch(() => setSaveLine(""));
  }, [screen, versions, hasSaves]);
```
  - Options `actions` keeps only `add_iso` (drop `open_saves`);
  - screens:

```tsx
  } else if (screen === "saves" && versions?.current) {
    body = <Saves strings={strings} releaseId={versions.current} message={savesMsg}
      onBack={() => { setSavesMsg(""); toMenu(); }} onImport={() => setScreen("import")} />;
  } else if (screen === "import" && versions?.current) {
    body = <ImportSaves strings={strings} releaseId={versions.current}
      onDone={(m) => { setSavesMsg(m); setScreen("saves"); }} onBack={() => setScreen("saves")} />;
```
    (before the final `else`), and the `MainMenu` element gains
    `onSaves={hasSaves ? () => { setSavesMsg(""); setScreen("saves"); } : undefined} saveLine={saveLine}`.

- [ ] **Step 8: Styles.** Append to `src/styles.css`:

```css
.saves { justify-content: flex-start; padding-top: 5vh; gap: 1.2vh; }
.save-list { list-style: none; margin: 0; max-height: 62vh; overflow-y: auto; width: min(1100px, 90vw); }
.save-row { display: flex; gap: 1em; align-items: center; padding: 0.5em 0.8em; cursor: pointer; border-radius: 6px; }
.save-row.selected { background: var(--c-accent); color: #000; }
.save-row.empty { opacity: 0.6; }
.save-icon { width: 144px; height: 80px; flex: none; background: rgba(0, 0, 0, 0.15); border-radius: 4px; overflow: hidden; }
.save-icon img { width: 100%; height: 100%; object-fit: cover; }
.save-head { display: flex; gap: 0.8em; align-items: baseline; font-size: clamp(16px, 2.4vh, 28px); }
.save-slot { font-weight: bold; }
.save-tag { font-size: 0.75em; border: 1px solid currentColor; border-radius: 4px; padding: 0 0.4em; }
.save-star { margin-left: auto; }
.save-detail { margin: 0.2em 0 0; font: inherit; font-size: clamp(13px, 1.9vh, 22px); white-space: pre-wrap; }
.save-actions li { font-size: clamp(18px, 3vh, 36px); padding: 0.2em 1em; }
.saves-message, .save-line { margin: 0; font-size: clamp(15px, 2.2vh, 26px); }
.saves-path { margin: 0; font-size: clamp(12px, 1.6vh, 18px); }
```

- [ ] **Step 9: README.** In the launcher `README.md` (Spanish part, after "Primera vez"), add a section:

```markdown
## Partidas

Menú principal → **Partidas**: las diez ranuras del juego como en su pantalla de CARGAR. Confirmar
sobre una partida → "Continuar con esta partida" (la que cargará *Continuar* en el juego; queda en
`settings.ini` como `[launcher] save_<id>`) o "Exportar a PPSSPP". **Importar de PPSSPP** busca la
carpeta de PPSSPP (o la eliges: vale la carpeta de PPSSPP, `memstick`, `PSP` o `SAVEDATA`) y copia
la partida elegida a su misma ranura; las de EE. UU. (UCUS98751) se convierten a la europea. Las
partidas cifradas de PPSSPP se copian tal cual: el juego las descifra al cargarlas. Nunca se borra
nada: lo que se reemplaza va antes a `backups/<fecha>/`.
```
  and the same in the English part (translate it).

- [ ] **Step 10: Tests pass.** Launcher UI gate: Vitest all pass (23 + 9 new = 32; paste the real
  count) and `tsc --noEmit` clean. Launcher Rust gate still green.

- [ ] **Step 11: Report (no commit).** Claude commits: `feat(launcher): Saves screen, PPSSPP import / export, chosen save`.

---

### Task 8: Portable package, end to end, Findings (Claude)

**Files:** `docs/superpowers/plans/2026-10-17-launcher-part3.md` (`## Findings`), launcher fixes found on the way.

- [ ] **Step 1: Package.** `cd /c/Users/torso/Documents/deco/launcher && npm run tauri build -- --no-bundle`,
  then `node scripts/make-portable.mjs --out C:/Users/torso/Documents/deco/build/e2e_portable3
  --game dxd-1.1.4=C:/Users/torso/Documents/deco/build/rtw --game dxd-1.1.5b=C:/Users/torso/Documents/deco/build/v115b/rtw
  --game vanilla-eu=C:/Users/torso/Documents/deco/build/vanilla/rtw` (check `make-portable.mjs --help` for
  the exact flags of part 2), install DxD 1.1.4 and vanilla from their ISOs through the launcher.
- [ ] **Step 2: Scratch PPSSPP.** Copy `DECO/build/ppsspp_mp` to `DECO/build/e2e_ppsspp` (its own
  `memstick`); never use `E:` as an export target.
- [ ] **Step 3: E2E (port 9998, `PSPRECOMP_DEBUG_PORT=9998` inherited from the launcher).**
  1. Saves → Import from PPSSPP → choose `E:\Respaldo\PPSSPP` → the list shows UCES01421_DATA00..10 and
     UCUS98751_DATA00..06 (US tagged), `.rar` files and other games absent.
  2. Import `UCES01421_DATA04` (encrypted) into dxd-1.1.4 → "Importada en DATA04." → Start → Español EU →
     Continue → the hideout shows Edson Yumiyacha Nv 35; `logs/game.log` has
     `[SAVEDATA] decrypted ...UCES01421_DATA04/SECURE.BIN mode 5 (178864 -> 178848 bytes, hash mismatch ignored like PPSSPP)`
     and `[SAVEDATA] LISTLOAD picks _DATA04`.
  3. Import `UCES01421_DATA05` too (newer save), choose DATA04 in Saves → Continue still loads Edson
     Yumiyacha Nv 35 (without the choice it would load DATA05).
  4. In vanilla-eu, import `UCUS98751_DATA02` (MoMo, Tondenga Lv 19) → Continue → record whether the
     hideout shows MoMo. If the game rejects it, the US saves become "not compatible" (spec §1.3) and
     the Findings say so.
  5. Choose the scratch PPSSPP folder, export DATA04 → the folder appears; export again → "already has"
     → Replace → `backups/<stamp>/ppsspp/UCES01421_DATA04` exists; the exported save loads in the
     scratch PPSSPP (screenshot of its hideout or of its save list).
  6. Leave a fake `UCES01421_DATA09.partial` in `versions/dxd-1.1.4/savedata`, restart the launcher → gone.
- [ ] **Step 4: Regression.** ctest 75/75, test_p3 12/12, launcher cargo + Vitest green, 1.1.4 tour PASS
  (Task 3), `jugar.bat`'s save folder untouched (`git status`-style listing of `DECO/savedata` before/after).
- [ ] **Step 5: Findings + memory.** Write `## Findings` (commits, E2E table, plan defects) at the end of
  this plan; commit `docs(plan): launcher part 3 findings`. Nothing pushed; ask the user to try it.
