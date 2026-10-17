# Launcher part 2 — first run with the player's ISO — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Execution chosen by the user:** each task is handed to **DeepSeek flash** (`deepseek/deepseek-flash`
> through OpenCode, `python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo <repo>
> --model deepseek/deepseek-flash`) as a self-contained brief; Claude reads every diff, re-runs every
> gate and decides. DeepSeek never pushes, never edits outside the task's files.

**Goal:** The player drops their Patapon 3 ISO (DxD 1.1.4, DxD 1.1.5b or vanilla EU) on the launcher
once; it is recognised, extracted to its own folder, and the matching exe boots it by decrypting
`EBOOT.BIN` in memory — the exe no longer carries the EBOOT image.

**Architecture:** psprecomp gets a per-game `[image] source = "disc"` option: the emitter stops
embedding the 3.9 MB image and emits only its expected segment table + FNV-1a-64; a generic runtime
loader reads the boot file from disc0, decrypts `~PSP` with PPSSPP's vendored PRX decrypter, checks
the image and exits with code 11 on any mismatch. `p3.sh build` writes `release.json` (id, name,
disc id, SHA-256 of the encrypted EBOOT) next to each exe. The Tauri launcher reads those files,
identifies an ISO by its EBOOT hash, extracts it to `versions/<id>/disc0` with its own ISO9660
reader, and starts `game/<id>/` with `PSPRECOMP_DISC0` / `PSPRECOMP_SAVEDATA` for that release.

**Tech Stack:** Rust 1.96 (psprecomp crates, Tauri 2 launcher), C++17 runtime (CMake + ctest,
clang 22 llvm-mingw), vendored PPSSPP `PrxDecrypter` + `libkirk` (GPL-2.0), React 19 + TSX +
Vitest, Git Bash, Python 3 (`-I`).

**Spec:** `docs/superpowers/specs/2026-10-17-launcher-part2-design.md` (read it first).

## Global Constraints

- Releases and ids: DxD 1.1.4 = `dxd-1.1.4` (default build, `DECO/build`), DxD 1.1.5b = `dxd-1.1.5b`
  (`P3_VARIANT=v115b`, `DECO/build/v115b`), vanilla EU UCES01421 = `vanilla-eu`
  (`P3_VARIANT=vanilla`, `DECO/build/vanilla`).
- Portable layout: `game/<release-id>/` (exe, DLLs, `game_options.json`, `release.json`),
  `versions/<release-id>/{disc0, savedata, installed.json}`; a release counts as installed only
  when `installed.json` exists; `installed.json` is written last.
- Exit codes: 0 quit, 10 back to menu, **11 game files do not match this exe**, other = crash.
- The launcher passes `PSPRECOMP_DISC0` / `PSPRECOMP_SAVEDATA` as environment variables;
  `settings.ini` contract v1 is unchanged except the launcher-only `[launcher] version = <id>`.
- `[image] source` default is `"embedded"`; only ET_EXEC images may use `"disc"`.
- No game file (ISO, EBOOT, dump) is ever committed. Tests that need them skip when absent.
- Unchanged: `jugar.bat`, `jugar_115b.bat`, `jugar_vanilla.bat`, tours, `rt_regress`, the user's
  `DECO/savedata*` folders, `DECO/disc0`, `build/estable/`.
- Nothing is pushed. psprecomp commits on branch `patapon3-windows`, launcher commits on `main` of
  `DECO/launcher`. Author `Patapon Community` (psprecomp: repo config already set; launcher:
  `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit ...`).
  **No `Co-Authored-By` trailer.** Commit bodies end with `Verified: <gates and numbers>` and
  `Agent: <model that wrote the code>`.
- Test runs of the game use debug port **9998** (`PSPRECOMP_DEBUG_PORT=9998`), never 9999.
- Strings shown to the player live in `launcher/lang/es.json` and `launcher/lang/en.json`.
- Code, comments and commits in English.

## Review Focus

1. A rebuild of the portable folder must not delete a player's installed releases or saves
   (`versions/`, `settings.ini`) — make-portable only replaces `game/`, `lang/`, `themes/` and the
   launcher (Task 9 test).
2. An ISO with hostile names (`..`, `/`, `\`, `:`) or a directory loop must never write outside
   `versions/<id>/disc0.partial` (Task 5 tests).
3. Cancelling, a failed extraction or a crash mid-install must leave the previous install (if any)
   playable and no half folder counted as installed (Task 6 tests).
4. A developer `versions/<id>/disc0` link (junction) must never be deleted through or replaced
   (Task 6 test, Task 9 test).
5. The wrong exe/disc0 pairing (1.1.4 exe on 1.1.5b files) must stop with exit 11 before a window
   opens, not boot into a broken game (Task 3 step).

---

## File map

psprecomp (`DECO/psprecomp`):
- `crates/psp-cli/src/config.rs` — `[image] source` (Task 1).
- `crates/psp-emitter/src/data.rs`, `crates/psp-emitter/src/lib.rs` — disc-mode emission (Task 1).
- `crates/psp-cli/src/recompile.rs` — chooses the emission, refuses PRX (Task 1).
- `runtime/third_party/kirk/` — vendored decrypter + shims + README (Task 2).
- `runtime/include/psp_eboot_loader.h`, `runtime/src/psp_eboot_loader.cpp` — loader (Task 2).
- `runtime/tests/test_eboot_loader.cpp` — ctest `eboot_loader_tests` (Task 2).
- `runtime/CMakeLists.txt` — kirk library, loader source, test (Task 2).
- `runtime/src/main.cpp` — boot file path for the loader (Task 3).
- `runtime/src/psp_settings.cpp`, `runtime/tests/test_settings.cpp` — `[launcher]` is silent (Task 3).
- `games/patapon3/game.toml` — `[image] source = "disc"` (Task 3).
- `ARCHITECTURE.md`, `README.md`, `docs/contracts/settings-v1.md` — docs (Task 3).
- `games/patapon3/scripts/p3.sh`, `games/patapon3/scripts/test_p3.sh`,
  `scripts/release_info.py` — overlay banks by header id, `release.json` (Task 4).

launcher (`DECO/launcher`):
- `src-tauri/src/iso.rs`, `src-tauri/src/sfo.rs` — ISO9660 + PARAM.SFO readers (Task 5).
- `src-tauri/src/releases.rs`, `src-tauri/src/install.rs` — identify + install (Task 6).
- `src-tauri/src/launch.rs`, `src-tauri/src/settings.rs`, `src-tauri/src/lib.rs`,
  `src-tauri/Cargo.toml`, `src-tauri/capabilities/default.json` — commands (Task 7).
- `src/types.ts`, `src/api.ts`, `src/App.tsx`, `src/nav/useNav.ts`, `src/screens/FirstRun.tsx`,
  `src/screens/Installing.tsx`, `src/screens/MainMenu.tsx`, `src/screens/Options.tsx`,
  `src/styles.css`, `lang/es.json`, `lang/en.json`, tests — UI (Task 8).
- `scripts/make-portable.mjs`, `scripts/e2e_keys.py`, `README.md` — packaging + E2E (Task 9).

Gates used below (exact commands):
- psprecomp Rust: `cd /c/Users/torso/Documents/deco/psprecomp && cargo test -q -p psp-emitter -p psp-cli 2>&1 | grep -E "^test result|FAILED|panicked"`
- runtime: `cmake --build C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -2 && ctest --test-dir C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -3`
- launcher Rust: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
- launcher UI: `cd /c/Users/torso/Documents/deco/launcher && npx vitest run 2>&1 | tail -5` and `npx tsc --noEmit`
- 1.1.4 tour: `cd /c/Users/torso/Documents/deco && TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/<name>` then `python -I build/tools/rt_regress.py build/tour/rebuild114_t1 build/tour/<name>` → `RT_REGRESS PASS`.

---

### Task 1: `[image] source = "disc"` in the recompiler

**Files:**
- Modify: `crates/psp-cli/src/config.rs` (struct `GameConfig`, new `ImageSection`, new method, tests)
- Modify: `crates/psp-emitter/src/data.rs` (new function + tests)
- Modify: `crates/psp-emitter/src/lib.rs:33` (export)
- Modify: `crates/psp-cli/src/recompile.rs` (`write_output_files` and its caller near line 746)

**Interfaces:**
- Produces: `GameConfig::image_from_disc(&self) -> anyhow::Result<bool>`;
  `psp_emitter::emit_data_sections_from_disc(segments: &[JsonSegment]) -> anyhow::Result<String>`.
- Produces (generated C++ contract, consumed by Task 2's header): the disc-mode
  `data_sections.cpp` includes `"psp_eboot_loader.h"`, defines
  `static const PspImageSegment k_image_segments[N]` with rows `{vaddr, filesz, memsz}`,
  `extern const PspExpectedImage psp_expected_image = {k_image_segments, NU, 0x...ULL};` and
  `void psp_init_data_sections(uint8_t* rdram) { psp_eboot_load_or_exit(rdram, psp_expected_image); }`.
  The hash is `psp_emitter::bank::fnv1a64` over every segment's decoded `data_b64` bytes
  (length `p_memsz`, BSS zeros included), concatenated in segment order.

- [ ] **Step 1: Failing config test.** In `crates/psp-cli/src/config.rs`, inside `mod tests`, add:

```rust
    #[test]
    fn image_source_defaults_to_embedded() {
        let c: GameConfig = toml::from_str("[game]\nid = \"x\"\n").unwrap();
        assert!(!c.image_from_disc().unwrap());
        let e: GameConfig = toml::from_str("[image]\nsource = \"embedded\"\n").unwrap();
        assert!(!e.image_from_disc().unwrap());
        let d: GameConfig = toml::from_str("[image]\nsource = \"disc\"\n").unwrap();
        assert!(d.image_from_disc().unwrap());
        let bad: GameConfig = toml::from_str("[image]\nsource = \"cloud\"\n").unwrap();
        assert!(bad.image_from_disc().is_err());
    }
```

- [ ] **Step 2: Failing emitter tests.** In `crates/psp-emitter/src/data.rs`, inside `mod tests`, add:

```rust
    #[test]
    fn disc_mode_emits_table_and_hash_not_bytes() {
        use psp_parser::analysis_json::JsonSegment;
        let bytes = vec![1u8, 2, 3, 0, 0, 0];
        let seg = JsonSegment {
            p_vaddr: "0x08804000".into(),
            p_filesz: 3,
            p_memsz: 6,
            p_flags: 7,
            data_b64: B64.encode(&bytes),
        };
        let out = emit_data_sections_from_disc(&[seg]).unwrap();
        assert!(!out.contains("static const uint8_t"), "no byte arrays in disc mode");
        assert!(out.contains("#include \"psp_eboot_loader.h\""));
        assert!(out.contains("{0x08804000U, 0x00000003U, 0x00000006U},"));
        assert!(out.contains(&format!("0x{:016X}ULL", crate::bank::fnv1a64(&bytes))));
        assert!(out.contains("psp_eboot_load_or_exit(rdram, psp_expected_image);"));
    }

    #[test]
    fn disc_mode_rejects_bad_segments() {
        use psp_parser::analysis_json::JsonSegment;
        assert!(emit_data_sections_from_disc(&[]).is_err(), "no segment");
        let short = JsonSegment {
            p_vaddr: "0x08804000".into(),
            p_filesz: 3,
            p_memsz: 8,
            p_flags: 7,
            data_b64: B64.encode([1u8, 2, 3]),
        };
        assert!(emit_data_sections_from_disc(&[short]).is_err(), "payload shorter than p_memsz");
    }
```

- [ ] **Step 3: Run, expect compile failure** (functions missing).
  Run: `cd /c/Users/torso/Documents/deco/psprecomp && cargo test -q -p psp-emitter -p psp-cli 2>&1 | grep -E "error\[|cannot find" | head`
  Expected: errors naming `image_from_disc` and `emit_data_sections_from_disc`.

- [ ] **Step 4: Config.** In `config.rs`: add to `GameConfig`, after the `overlays` field:

```rust
    /// `[image]` where the runtime gets the main memory image.
    #[serde(default)]
    pub image: Option<ImageSection>,
```

  Add next to `RuntimeSection`:

```rust
/// `[image]` — where the runtime gets the main memory image.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct ImageSection {
    /// "embedded" (default): the image is compiled into the exe (data_sections.cpp arrays).
    /// "disc": the exe reads the boot file from disc0 at boot, decrypts it when needed and checks
    /// it against the image it was recompiled from (runtime/src/psp_eboot_loader.cpp).
    #[serde(default)]
    pub source: Option<String>,
}
```

  Add next to `asset_layer_is_bnd`:

```rust
    /// True when `[image] source = "disc"`.
    pub fn image_from_disc(&self) -> anyhow::Result<bool> {
        match self.image.as_ref().and_then(|i| i.source.as_deref()) {
            None | Some("embedded") => Ok(false),
            Some("disc") => Ok(true),
            Some(other) => anyhow::bail!(
                "[image] source must be \"embedded\" or \"disc\", got {other:?}"
            ),
        }
    }
```

  Also add to the schema comment at the top of the file, after the `[runtime]` block:

```
//!   [image]
//!   source = "embedded"               # embedded|disc ; "disc" = load the boot file at run time
```

- [ ] **Step 5: Emitter.** In `data.rs`, after `emit_data_sections`, add:

```rust
/// `[image] source = "disc"`: no byte arrays. Emits the expected PT_LOAD table and the FNV-1a-64
/// of the image (each segment's bytes, BSS zeros included, concatenated in table order) for
/// `psp_eboot_load_or_exit` (runtime/src/psp_eboot_loader.cpp), which reads the boot file from
/// disc0 at run time and refuses an image that differs from the one recompiled here.
pub fn emit_data_sections_from_disc(segments: &[JsonSegment]) -> anyhow::Result<String> {
    if segments.is_empty() {
        anyhow::bail!("[image] source = \"disc\" needs at least one PT_LOAD segment");
    }
    let mut image = Vec::new();
    let mut rows = String::new();
    for seg in segments {
        let vaddr: u32 = u64::from_str_radix(
            seg.p_vaddr.trim_start_matches("0x"), 16
        ).unwrap_or(0) as u32;
        let raw = B64.decode(&seg.data_b64)
            .map_err(|e| anyhow::anyhow!("base64 decode failed for seg 0x{vaddr:08X}: {e}"))?;
        if raw.len() as u64 != seg.p_memsz {
            anyhow::bail!("seg 0x{vaddr:08X}: {} bytes, expected p_memsz {}", raw.len(), seg.p_memsz);
        }
        rows.push_str(&format!(
            "    {{0x{vaddr:08X}U, 0x{:08X}U, 0x{:08X}U}},\n", seg.p_filesz, seg.p_memsz
        ));
        image.extend_from_slice(&raw);
    }
    let n = segments.len();
    let hash = crate::bank::fnv1a64(&image);
    let mut out = String::new();
    out.push_str("// Generated by psprecomp — do not edit\n");
    out.push_str("// [image] source = \"disc\": the image is read from the boot file at run time\n");
    out.push_str("// (runtime/src/psp_eboot_loader.cpp); none of it is compiled in.\n");
    out.push_str("#include <cstdint>\n");
    out.push_str("#include \"psp_eboot_loader.h\"\n\n");
    out.push_str(&format!("static const PspImageSegment k_image_segments[{n}] = {{\n{rows}}};\n"));
    out.push_str(&format!(
        "extern const PspExpectedImage psp_expected_image = {{k_image_segments, {n}U, 0x{hash:016X}ULL}};\n\n"
    ));
    out.push_str("void psp_init_data_sections(uint8_t* rdram) {\n");
    out.push_str("    psp_eboot_load_or_exit(rdram, psp_expected_image);\n");
    out.push_str("}\n");
    Ok(out)
}
```

  In `crates/psp-emitter/src/lib.rs` line 33 change `pub use data::emit_data_sections;` to
  `pub use data::{emit_data_sections, emit_data_sections_from_disc};`.

- [ ] **Step 6: recompile.rs.** Add a parameter `image_from_disc: bool` as the last parameter of
  `write_output_files`, and replace its `data_sections.cpp` write with:

```rust
    let data_sections = if image_from_disc {
        if analysis.prx.is_some() {
            anyhow::bail!(
                "[image] source = \"disc\" needs an ET_EXEC image; this module is a relocatable PRX"
            );
        }
        psp_emitter::emit_data_sections_from_disc(&analysis.segments)?
    } else {
        emit_data_sections(&analysis.segments)?
    };
    std::fs::write(output_dir.join("data_sections.cpp"), data_sections)?;
```

  At the caller (the `else` branch near line 746) pass `prep.config.image_from_disc()?` as the new
  last argument.

- [ ] **Step 7: Gates.** Run the psprecomp Rust gate. Expected: every `test result: ok`, 0 failed,
  the three new tests listed (`cargo test -q -p psp-emitter -p psp-cli -- image_source disc_mode 2>&1 | grep -c "ok"` ≥ 3).
  Also `cargo build --release -q` (no warnings added: `cargo build --release 2>&1 | grep -c warning` unchanged from before the task).

- [ ] **Step 8: Commit** (psprecomp):

```bash
git add crates/psp-cli/src/config.rs crates/psp-emitter/src/data.rs crates/psp-emitter/src/lib.rs crates/psp-cli/src/recompile.rs
git commit -m "feat(emitter): [image] source = \"disc\" emits the image table and hash, not its bytes" -m "Verified: cargo test psp-emitter + psp-cli all ok (3 new tests)" -m "Agent: <model>"
```

---

### Task 2: Vendored PRX decrypter + EBOOT image loader (runtime)

**Files:**
- Create: `runtime/third_party/kirk/Core/ELF/PrxDecrypter.h`, `.../PrxDecrypter.cpp` (copied verbatim)
- Create: `runtime/third_party/kirk/ext/libkirk/*` (the `.c`/`.h` files copied verbatim; not the vcxproj/CMakeLists)
- Create: `runtime/third_party/kirk/shim/Common/{Common.h,CommonTypes.h,Swap.h,Log.h}`
- Create: `runtime/third_party/kirk/README.md`
- Create: `runtime/include/psp_eboot_loader.h`, `runtime/src/psp_eboot_loader.cpp`
- Create: `runtime/tests/test_eboot_loader.cpp`
- Modify: `runtime/CMakeLists.txt` (kirk library after the xxhash block near line 279; loader in the
  runtime source list next to `src/psp_settings.cpp` near line 146; link `kirk` where `xxhash` is
  linked near line 404; the test next to `test_settings` near line 640)

**Interfaces:**
- Consumes: the generated-C++ contract of Task 1.
- Produces (`psp_eboot_loader.h`, exactly):

```cpp
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
```

- [ ] **Step 1: Vendor the decrypter.** Copy, byte for byte:

```bash
cd /c/Users/torso/Documents/deco
K=psprecomp/runtime/third_party/kirk
mkdir -p $K/Core/ELF $K/ext/libkirk $K/shim/Common
cp tools/ppsspp-decrypt/src/Core/ELF/PrxDecrypter.h tools/ppsspp-decrypt/src/Core/ELF/PrxDecrypter.cpp $K/Core/ELF/
cp tools/ppsspp-decrypt/src/ext/libkirk/*.c tools/ppsspp-decrypt/src/ext/libkirk/*.h $K/ext/libkirk/
cp build/spike_decrypt/shim/Common/*.h $K/shim/Common/
git -C tools/ppsspp-decrypt/src log -1 --format=%H   # record this hash in the README
```

  The four shims (already proven by the spike, `build/spike_decrypt/`) are:
  `CommonTypes.h` (`u8..u64`, `s8..s64` typedefs over `<cstdint>`), `Common.h` (includes
  `CommonTypes.h`; `_dbg_assert_` / `_dbg_assert_msg_` as no-ops), `Swap.h` (`u32_le`, `u16_le`,
  `s32_le`, `s16_le`, `u64_le` as plain little-endian typedefs), `Log.h` (`INFO_LOG`, `ERROR_LOG`,
  `WARN_LOG`, `DEBUG_LOG`, `NOTICE_LOG`, `VERBOSE_LOG` as no-ops). Add a first-line comment to each
  shim: `// Shim for the vendored PPSSPP PRX decrypter (third_party/kirk/README.md).`

  `README.md`:

```markdown
# PPSSPP PRX decrypter (vendored)

`Core/ELF/PrxDecrypter.{h,cpp}` and `ext/libkirk/` are copied unmodified from PPSSPP
(https://github.com/hrydgard/ppsspp, commit <hash from the command above>). License: GNU GPL 2.0
(same as psprecomp); the copyright headers are kept in every file. `shim/Common/` replaces the
four PPSSPP `Common/` headers these files include (types, little-endian typedefs, no-op logging).

psprecomp uses only `pspDecryptPRX`, to decrypt a game's `~PSP` boot file in memory at boot
(`runtime/src/psp_eboot_loader.cpp`, `[image] source = "disc"`). Nothing decrypted is written
to disk.
```

- [ ] **Step 2: CMake library.** After the xxhash block (`add_library(xxhash ...)` +2 lines) add:

```cmake
# Vendored PPSSPP PRX decrypter (GPL-2.0; third_party/kirk/README.md): decrypts a "~PSP" boot
# file in memory for [image] source = "disc" builds (psp_eboot_loader.cpp).
file(GLOB KIRK_C_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/ext/libkirk/*.c)
add_library(kirk STATIC ${KIRK_C_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/Core/ELF/PrxDecrypter.cpp)
target_include_directories(kirk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/ext)
target_compile_options(kirk PRIVATE -w -O2)
```

  Add `src/psp_eboot_loader.cpp` to the runtime source list right after `src/psp_settings.cpp`,
  and `kirk` to the runtime's `target_link_libraries` list right after `xxhash`.
  After the `test_settings` block add:

```cmake
# EBOOT image loader ([image] source = "disc"): synthetic ELFs, plus the real EBOOTs of the
# three Patapon 3 releases when they are on this PC (skipped otherwise; never committed).
add_executable(test_eboot_loader tests/test_eboot_loader.cpp src/psp_eboot_loader.cpp)
target_include_directories(test_eboot_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_eboot_loader PRIVATE kirk)
target_compile_definitions(test_eboot_loader PRIVATE PSP_DECO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(test_eboot_loader PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME eboot_loader_tests COMMAND test_eboot_loader)
```

- [ ] **Step 3: Failing test.** Create `runtime/tests/test_eboot_loader.cpp`:

```cpp
// Unit tests for psp_eboot_loader ([image] source = "disc"): image building from synthetic ELFs,
// and the three real Patapon 3 EBOOTs (encrypted == PPSSPP dump) when they are on this PC.
#include "psp_eboot_loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ELF32 LE ET_EXEC with one PT_LOAD whose bytes start at file offset 0x54
// (52-byte header + one 32-byte program header).
static std::vector<uint8_t> make_elf(uint32_t vaddr, const std::vector<uint8_t>& bytes, uint32_t memsz) {
    std::vector<uint8_t> f(0x54, 0);
    auto w16 = [&](size_t o, uint16_t v) { f[o] = uint8_t(v); f[o + 1] = uint8_t(v >> 8); };
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) f[o + i] = uint8_t(v >> (8 * i)); };
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F'; f[4] = 1; f[5] = 1; f[6] = 1;
    w16(16, 2); w16(18, 8); w32(20, 1); w32(28, 52); w16(40, 52); w16(42, 32); w16(44, 1);
    w32(52, 1); w32(56, 0x54); w32(60, vaddr); w32(64, vaddr);
    w32(68, uint32_t(bytes.size())); w32(72, memsz);
    f.insert(f.end(), bytes.begin(), bytes.end());
    return f;
}

static const std::vector<uint8_t> kBytes = {1, 2, 3, 4};
static const std::vector<uint8_t> kImage = {1, 2, 3, 4, 0, 0, 0, 0};
static const PspImageSegment kSeg{0x08804000U, 4, 8};
static PspExpectedImage want() { return {&kSeg, 1, psp_eboot_fnv1a64(kImage.data(), kImage.size())}; }

static void fnv_vectors() {
    CHECK(psp_eboot_fnv1a64(nullptr, 0) == 0xCBF29CE484222325ULL, "fnv of empty");
    CHECK(psp_eboot_fnv1a64(reinterpret_cast<const uint8_t*>("a"), 1) == 0xAF63DC4C8601EC8CULL, "fnv of a");
}

static void plain_elf_loads() {
    std::vector<uint8_t> img; std::string d;
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, kBytes, 8), want(), img, d) == EbootStatus::Ok, "plain ELF ok");
    CHECK(img == kImage, "file bytes then zeros up to memsz");
    CHECK(d == "plain ELF", "detail plain ELF");
}

static void refusals() {
    std::vector<uint8_t> img; std::string d;
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, {1, 2, 3, 5}, 8), want(), img, d) == EbootStatus::HashMismatch, "other bytes -> hash mismatch");
    CHECK(img.empty(), "no image on mismatch");
    CHECK(psp_eboot_build_image(make_elf(0x08900000U, kBytes, 8), want(), img, d) == EbootStatus::LayoutMismatch, "other vaddr");
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, kBytes, 9), want(), img, d) == EbootStatus::LayoutMismatch, "other memsz");
    auto cut = make_elf(0x08804000U, kBytes, 8); cut.resize(cut.size() - 2);
    CHECK(psp_eboot_build_image(cut, want(), img, d) == EbootStatus::NotElf, "truncated segment");
    CHECK(psp_eboot_build_image(std::vector<uint8_t>(100, 'x'), want(), img, d) == EbootStatus::NotElf, "not an ELF");
    CHECK(psp_eboot_build_image({}, want(), img, d) == EbootStatus::NotElf, "empty file");
    auto exe = make_elf(0x08804000U, kBytes, 8); exe[16] = 3;   // ET_DYN
    CHECK(psp_eboot_build_image(exe, want(), img, d) == EbootStatus::NotElf, "not ET_EXEC");
    std::vector<uint8_t> psp(0x200, 0); std::memcpy(psp.data(), "~PSP", 4);
    CHECK(psp_eboot_build_image(psp, want(), img, d) == EbootStatus::DecryptFailed, "garbage ~PSP");
}

static void host_paths() {
    CHECK(psp_eboot_host_path("C:/P 3/versions/x/disc0", "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN")
          == "C:/P 3/versions/x/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "disc0 path");
    CHECK(psp_eboot_host_path("D:\\d\\disc0\\", "disc0:/PSP_GAME/SYSDIR/BOOT.BIN")
          == "D:\\d\\disc0/PSP_GAME/SYSDIR/BOOT.BIN", "trailing separator dropped");
}

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The single PT_LOAD of all three releases: file offset 0xb4, vaddr 0x08804000, filesz 0x297860,
// memsz 0x3C2480 (build/spike_decrypt/AUDIT_deepseek.md section 1.4).
static void real_releases() {
    const std::string deco = PSP_DECO_DIR;
    const char* pairs[][2] = {
        {"/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/INFN00001_EBOOT.BIN"},
        {"/build/v115b/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/build/v115b/EBOOT_dec.BIN"},
        {"/build/vanilla/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/build/vanilla/EBOOT_dec.BIN"},
    };
    for (auto& p : pairs) {
        std::vector<uint8_t> enc = slurp(deco + p[0]), dump = slurp(deco + p[1]);
        if (enc.size() < 0x200 || dump.size() < 0xB4 + 0x297860) {
            std::printf("SKIP real EBOOT %s (not on this PC)\n", p[0]);
            continue;
        }
        std::vector<uint8_t> ref(dump.begin() + 0xB4, dump.begin() + 0xB4 + 0x297860);
        ref.resize(0x3C2480, 0);
        PspImageSegment seg{0x08804000U, 0x297860U, 0x3C2480U};
        PspExpectedImage w{&seg, 1, psp_eboot_fnv1a64(ref.data(), ref.size())};
        std::vector<uint8_t> a, b; std::string da, db;
        CHECK(psp_eboot_build_image(enc, w, a, da) == EbootStatus::Ok && da == "decrypted", p[0]);
        CHECK(psp_eboot_build_image(dump, w, b, db) == EbootStatus::Ok && db == "plain ELF", p[1]);
        CHECK(a == ref && b == ref, "decrypted image equals the PPSSPP dump's image");
    }
}

int main() {
    fnv_vectors();
    plain_elf_loads();
    refusals();
    host_paths();
    real_releases();
    if (failures == 0) std::printf("test_eboot_loader: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Run, expect failure** (no loader source yet):
  `cmake -S C:/Users/torso/Documents/deco/psprecomp/runtime -B C:/Users/torso/Documents/deco/build/rtw >/dev/null; cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_eboot_loader 2>&1 | tail -3`
  Expected: error, `psp_eboot_loader.cpp` missing / undefined symbols.

- [ ] **Step 5: Loader.** Create `runtime/include/psp_eboot_loader.h` with the exact text in
  **Interfaces** above, and `runtime/src/psp_eboot_loader.cpp`:

```cpp
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
        std::exit(11);
    }
    size_t at = 0;
    for (uint32_t i = 0; i < want.count; i++) {
        std::memcpy(rdram + (want.segments[i].vaddr & 0x07FFFFFFU), image.data() + at, want.segments[i].memsz);
        at += want.segments[i].memsz;
    }
    std::fprintf(stderr, "[EBOOT] loaded %s (%s, %zu bytes)\n", g_boot_file.c_str(), detail.c_str(), image.size());
}
```

- [ ] **Step 6: Gate.** `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_eboot_loader 2>&1 | tail -2 && C:/Users/torso/Documents/deco/build/rtw/test_eboot_loader.exe`
  Expected: `test_eboot_loader: all passed` and **no** `SKIP` line on the user's PC (all three
  releases present). Then the runtime gate: build succeeds (the main exe still embeds the image —
  `game.toml` is untouched until Task 3), ctest **74/74** (73 + `eboot_loader_tests`).

- [ ] **Step 7: Commit** (psprecomp): all files of this task.
  Message: `feat(runtime): EBOOT image loader with the vendored PPSSPP PRX decrypter` + `Verified:` (test output, ctest 74/74) + `Agent:`.

---

### Task 3: Patapon 3 loads its image from disc0 (wiring, 1.1.4 rebuild, docs)

**Files:**
- Modify: `runtime/src/main.cpp` (include + one call before `psp_init_data_sections(rdram);`, line ~231)
- Modify: `runtime/src/psp_settings.cpp` (the unknown-key loop, line ~183)
- Modify: `runtime/tests/test_settings.cpp` (new test function + call in `main`)
- Modify: `games/patapon3/game.toml` (new `[image]` section after `[boot]`)
- Modify: `ARCHITECTURE.md:57`, `README.md`, `docs/contracts/settings-v1.md` (exit codes + `[launcher]`)

**Interfaces:**
- Consumes: `psp_eboot_set_boot_file`, `psp_eboot_host_path` (Task 2); `[image]` (Task 1).
- Produces: exit code 11 behaviour of the 1.1.4 exe (consumed by the launcher, Task 7).

- [ ] **Step 1: Failing settings test.** In `runtime/tests/test_settings.cpp` add before `int main`:

```cpp
// [launcher] belongs to the launcher (last played release): the game ignores it silently.
static void launcher_section_is_silent() {
    const SettingsResolution r = psp_settings_resolve("[launcher]\nversion = dxd-1.1.5b\n", kCatalog,
                                                      "C:/P", kNoEnv);
    CHECK(!logged(r, "launcher"), "[launcher] not logged as unknown");
}
```
  and call `launcher_section_is_silent();` in `main` after `args_rules();`. Build + run
  `test_settings`: expected `FAIL: [launcher] not logged as unknown`.

- [ ] **Step 2: Fix.** In `psp_settings.cpp`, first line inside `for (const auto& s : ini.values) {`:

```cpp
        if (s.first == "launcher") continue;  // launcher-only section (docs/contracts/settings-v1.md)
```
  Run `test_settings`: `all passed`.

- [ ] **Step 3: main.cpp.** Add `#include "psp_eboot_loader.h"` with the other runtime includes and,
  directly before `    // 4. RUNTIME-03: Copy .data/.rodata into rdram`, add:

```cpp
    // [image] source = "disc" builds read the image from the boot file (psp_eboot_loader.cpp);
    // embedded builds never open it.
    psp_eboot_set_boot_file(psp_eboot_host_path(disc0_path, RECOMP_BOOT_PATH));
```

- [ ] **Step 4: game.toml.** After the `[boot]` section add:

```toml
[image]
# The EBOOT image is not compiled in: the exe reads PSP_GAME/SYSDIR/EBOOT.BIN from disc0 at boot,
# decrypts it in memory (runtime/third_party/kirk) and checks it against the image it was
# recompiled from (exit code 11 otherwise). Any extraction of the player's ISO has that file.
source = "disc"
```

- [ ] **Step 5: Rebuild 1.1.4 and check the generated file.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
bash games/patapon3/scripts/p3.sh recompile && bash games/patapon3/scripts/p3.sh build 2>&1 | tail -2
wc -c ../build/p3_output/data_sections.cpp          # expected: under 2000 bytes
grep -c "static const uint8_t" ../build/p3_output/data_sections.cpp   # expected: 0
ls -l ../build/rtw/psprecomp_runtime.exe            # expected: about 3.9 MB smaller than 36649472
```

- [ ] **Step 6: Boot checks (port 9998).**

```bash
cd /c/Users/torso/Documents/deco/build/rtw
PSPRECOMP_DEBUG_PORT=9998 PSPRECOMP_DISC0=C:/Users/torso/Documents/deco/disc0 timeout 25 ./psprecomp_runtime.exe > ../logs/p2_t3_ok.log 2>&1; echo "rc=$?"
grep -a "\[EBOOT\]" ../logs/p2_t3_ok.log
# expected: rc=124 (still running) and "[EBOOT] loaded .../disc0/PSP_GAME/SYSDIR/EBOOT.BIN (decrypted, 3941504 bytes)"
PSPRECOMP_DEBUG_PORT=9998 PSPRECOMP_DISC0=C:/Users/torso/Documents/deco/build/v115b/disc0 timeout 25 ./psprecomp_runtime.exe > ../logs/p2_t3_bad.log 2>&1; echo "rc=$?"
grep -a "\[EBOOT\]" ../logs/p2_t3_bad.log
# expected: rc=11 and "[EBOOT] ...: image differs from the recompiled one (fnv ...); exit 11", no window opened
PSPRECOMP_DEBUG_PORT=9998 PSPRECOMP_DISC0=C:/no/such/disc0 timeout 25 ./psprecomp_runtime.exe > ../logs/p2_t3_none.log 2>&1; echo "rc=$?"
# expected: rc=11, "[EBOOT] ...: missing (cannot open the file); exit 11"
```

- [ ] **Step 7: Regression gates.** Runtime gate (ctest 74/74, `settings_tests` passes); 1.1.4 tour
  `p2_t3` vs `rebuild114_t1`: `RT_REGRESS PASS`; `grep -a "first picture" C:/Users/torso/Documents/deco/build/logs/tour_rt.log`
  still shows `(ffmpeg=1)`.

- [ ] **Step 8: Docs.**
  - `ARCHITECTURE.md:57` row becomes: `| data_sections.cpp | .data/.rodata/.bss as byte arrays (addresses masked with 0x07FFFFFFU); with [image] source = "disc", only the expected PT_LOAD table + FNV-1a-64 and a call into runtime/src/psp_eboot_loader.cpp, which reads, decrypts (vendored PPSSPP PrxDecrypter) and checks the boot file at boot |` (keep the table's column style). Add one paragraph under the runtime boot description: why — the exe no longer carries the game's bytes; the player's disc supplies them, and a mismatching disc stops with exit 11.
  - `README.md`: in the Patapon 3 section, one sentence: the Patapon 3 build reads `PSP_GAME/SYSDIR/EBOOT.BIN` from the extracted disc at boot (encrypted is fine), so the PPSSPP EBOOT dump is only needed to *build*, not to play.
  - `docs/contracts/settings-v1.md`: exit-code table gains `| 11 | the game files in disc0 do not match this exe (the launcher asks to add the ISO again) |` before "anything else"; after the defaults block, a line: "`[launcher]` (e.g. `version = dxd-1.1.5b`, the last played release) belongs to the launcher; the game ignores the section without logging."

- [ ] **Step 9: Commit** (psprecomp): all files of this task. Message:
  `feat(p3): Patapon 3 loads its EBOOT image from disc0 at boot (exit 11 on mismatch)` +
  `Verified:` (data_sections size, exe size, three boot checks with rc, ctest 74/74, tour PASS) + `Agent:`.

---

### Task 4: `p3.sh` — overlay banks by header id, `release.json`; rebuild 1.1.5b and vanilla

**Files:**
- Create: `scripts/release_info.py`
- Modify: `games/patapon3/scripts/p3.sh` (variables block, `cmd_overlays`, `cmd_build`, new
  `overlay-banks` command, header usage line, `case`)
- Modify: `games/patapon3/scripts/test_p3.sh` (tests 9-11 before the summary)

**Interfaces:**
- Produces: `<build>/rtw/release.json` = `{"id", "name", "disc_id", "eboot_sha256"}` (lowercase hex),
  consumed by the launcher (Tasks 6, 9).
- Produces: `p3.sh overlay-banks` prints one line per overlay file: `<file name> <Bank>`.

- [ ] **Step 1: Failing tests.** Append to `test_p3.sh`, before `echo "$fails failure(s)"`:

```bash
# Fake PSP disc pieces for tests 9-11: a PARAM.SFO with DISC_ID / TITLE and an overlay file
# with the MWo3 header id at offset 4.
make_sfo() {   # $1 = out file, $2 = DISC_ID
    python -I - "$1" "$2" <<'PY'
import struct, sys
out, disc = sys.argv[1], sys.argv[2]
entries = [("DISC_ID", disc), ("TITLE", "PATAPON 3")]
keys = b""; data = b""; index = b""
for k, v in entries:
    val = v.encode() + b"\0"; size = (len(val) + 3) // 4 * 4
    index += struct.pack("<HHIII", len(keys), 0x0204, len(val), size, len(data))
    keys += k.encode() + b"\0"; data += val.ljust(size, b"\0")
keys = keys.ljust((len(keys) + 3) // 4 * 4, b"\0")
kstart = 20 + len(index)
open(out, "wb").write(b"\0PSF" + struct.pack("<IIII", 0x0101, kstart, kstart + len(keys), len(entries)) + index + keys + data)
PY
}
make_ovl() { printf 'MWo3' >"$1"; printf "\\x$(printf %02x "$2")\\x00\\x00\\x00" >>"$1"; head -c 64 /dev/zero >>"$1"; }

# 9. build writes release.json next to the exe (id from the variant, disc id, EBOOT hash).
fake_deco "$TMP/rel" yes
mkdir -p "$TMP/rel/disc0/PSP_GAME/SYSDIR"
echo "fake eboot" >"$TMP/rel/disc0/PSP_GAME/SYSDIR/EBOOT.BIN"
make_sfo "$TMP/rel/disc0/PSP_GAME/PARAM.SFO" INFN00001
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/rel.log" DECO="$TMP/rel" bash "$P3" build 2>&1)"; rc=$?
want="$(sha256sum "$TMP/rel/disc0/PSP_GAME/SYSDIR/EBOOT.BIN" | cut -d' ' -f1)"
rel="$(cat "$TMP/rel/build/rtw/release.json" 2>/dev/null)"
if [[ $rc -eq 0 && "$rel" == *'"id": "dxd-1.1.4"'* && "$rel" == *'"disc_id": "INFN00001"'* && "$rel" == *"\"eboot_sha256\": \"$want\""* ]]; then
    pass "build writes release.json"
else
    fail "build writes release.json (rc=$rc, out: $out, json: $rel)"
fi

# 10. Without EBOOT.BIN / PARAM.SFO in disc0 the build still succeeds and says why there is no release.json.
fake_deco "$TMP/norel" yes
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/norel.log" DECO="$TMP/norel" bash "$P3" build 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"release.json not written"* && ! -f "$TMP/norel/build/rtw/release.json" ]]; then
    pass "no disc0 -> no release.json, build ok"
else
    fail "no disc0 -> no release.json, build ok (rc=$rc, out: $out)"
fi

# 11. Overlay banks come from the MWo3 header id, not the file name, and the folder may be "Overlay".
mkdir -p "$TMP/van/PSP_GAME/USRDIR/Overlay"
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Azito.bin" 2
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Mission.bin" 3
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Title.bin" 1
out="$(P3_DISC0="$TMP/van" bash "$P3" overlay-banks 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"OL_Azito.bin Mission"* && "$out" == *"OL_Mission.bin Title"* && "$out" == *"OL_Title.bin Azito"* ]]; then
    pass "overlay banks by header id"
else
    fail "overlay banks by header id (rc=$rc, out: $out)"
fi
```

  Run: `cd /c/Users/torso/Documents/deco/psprecomp && bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -5`
  Expected: tests 1-8 PASS, 9-11 FAIL, `3 failure(s)`.

- [ ] **Step 2: `scripts/release_info.py`:**

```python
#!/usr/bin/env python3
"""Write release.json for a game build: which disc this exe was built for.

Usage: python -I scripts/release_info.py <disc0> <id> <name> <out.json>
The launcher recognises a player's ISO by eboot_sha256, the SHA-256 of the (encrypted)
PSP_GAME/SYSDIR/EBOOT.BIN; disc_id comes from PSP_GAME/PARAM.SFO.
"""
import hashlib
import json
import os
import struct
import sys


def sfo_strings(data: bytes) -> dict:
    if data[:4] != b"\0PSF":
        raise ValueError("PARAM.SFO has no PSF header")
    kstart, dstart, count = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(count):
        koff, fmt, length, _max, doff = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[kstart + koff:data.index(b"\0", kstart + koff)].decode()
        if fmt in (0x0004, 0x0204):
            out[key] = data[dstart + doff:dstart + doff + length].rstrip(b"\0").decode("utf-8", "replace")
    return out


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    disc0, rid, name, out = sys.argv[1:]
    with open(os.path.join(disc0, "PSP_GAME", "SYSDIR", "EBOOT.BIN"), "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(disc0, "PSP_GAME", "PARAM.SFO"), "rb") as f:
        sfo = sfo_strings(f.read())
    info = {"id": rid, "name": name, "disc_id": sfo.get("DISC_ID", ""), "eboot_sha256": sha}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    print(f"release.json: {rid} ({info['disc_id']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: p3.sh.**
  - Header usage line: `#   p3.sh extract | analyze | recompile | overlays | overlay-banks | build | run [seconds] | all`.
  - After the `VARIANT` / `WORK` block add:

```bash
# Release id and name written to release.json (the launcher picks game/<id>/ by them).
case "$VARIANT" in
    "")      RELEASE_ID="dxd-1.1.4";  RELEASE_NAME="Patapon 3 DxD 1.1.4" ;;
    v115b)   RELEASE_ID="dxd-1.1.5b"; RELEASE_NAME="Patapon 3 DxD 1.1.5b" ;;
    vanilla) RELEASE_ID="vanilla-eu"; RELEASE_NAME="Patapon 3 (Europe)" ;;
    *)       RELEASE_ID="$VARIANT";   RELEASE_NAME="Patapon 3 ($VARIANT)" ;;
esac
```

  - After `need_file()` add:

```bash
# The overlay folder's case differs between releases (DxD: overlay, vanilla: Overlay).
overlay_dir() {
    local d
    for d in "$DISC0/PSP_GAME/USRDIR/overlay" "$DISC0/PSP_GAME/USRDIR/Overlay"; do
        if [[ -d "$d" ]]; then echo "$d"; return 0; fi
    done
    return 1
}

# Bank of an overlay file from its "MWo3" header id (offset 4): 1 Azito, 2 Mission, 3 Title.
# Vanilla ships the three files under each other's names, so the file name is never trusted.
overlay_bank() {
    [[ "$(head -c 4 "$1")" == "MWo3" ]] || return 1
    case "$(od -An -tu4 -j4 -N4 "$1" | tr -d ' ')" in
        1) echo Azito ;; 2) echo Mission ;; 3) echo Title ;; *) return 1 ;;
    esac
}

cmd_overlay_banks() {
    local dir f bank
    dir="$(overlay_dir)" || die "no PSP_GAME/USRDIR/overlay folder in $DISC0 — run: p3.sh extract"
    for f in "$dir"/OL_*.bin; do
        bank="$(overlay_bank "$f")" || die "$f is not a known MWo3 overlay"
        echo "$(basename "$f") $bank"
    done
}
```

  - `cmd_overlays`: replace the `[[ -d "$DISC0/PSP_GAME/USRDIR/overlay" ]] || die ...` line with
    `local dir; dir="$(overlay_dir)" || die "disc0 missing — run: p3.sh extract"` and replace the
    `for name in "${OVERLAYS[@]}"; do ... overlay_elf.py ... "$DISC0/PSP_GAME/USRDIR/overlay/OL_$name.bin" "OL_$name.bin" ...`
    head of the loop with a loop over the files:

```bash
    local f name lower
    for f in "$dir"/OL_*.bin; do
        name="$(overlay_bank "$f")" || die "$f is not a known MWo3 overlay"
        lower="$(echo "$name" | tr '[:upper:]' '[:lower:]')"
        # The ELF section keeps the file's own name; the bank follows the content.
        python -I "$REPO/scripts/overlay_elf.py" "$P3_EBOOT" "$f" "$(basename "$f")" "$OVL/$lower.elf"
```
    (the rest of the loop body — analyze into `$DATA/ov_$lower`, recompile `--bank "ov$name"` — is
    unchanged; remove the old `local name lower` line).
  - `cmd_build`, after `cp -u "$SDL2_PKG/bin/SDL2.dll" "$BUILD/"`:

```bash
    if [[ -f "$DISC0/PSP_GAME/SYSDIR/EBOOT.BIN" && -f "$DISC0/PSP_GAME/PARAM.SFO" ]]; then
        python -I "$REPO/scripts/release_info.py" "$DISC0" "$RELEASE_ID" "$RELEASE_NAME" "$BUILD/release.json"
    else
        echo "p3.sh: warning: no EBOOT.BIN / PARAM.SFO in $DISC0, release.json not written" >&2
    fi
```
  - `case`: add `    overlay-banks) cmd_overlay_banks ;;` and the usage message gains `overlay-banks`.

  Note for the fake-toolchain tests: `cmake --build` and `cp -u` must still work with the fake
  `cmake`; `fake_deco` creates `SDL2.dll`. If `cp -u` fails because `$BUILD` is absent, the fake
  `cmake` already creates the `-B` folder (test 7 proves it).

- [ ] **Step 4: Tests pass.** `bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -3` → `0 failure(s)` (11 PASS).
  On the real discs: `bash games/patapon3/scripts/p3.sh overlay-banks` → `OL_Azito.bin Azito`,
  `OL_Mission.bin Mission`, `OL_Title.bin Title`; `P3_VARIANT=vanilla bash games/patapon3/scripts/p3.sh overlay-banks`
  → `OL_Azito.bin Mission`, `OL_Mission.bin Title`, `OL_Title.bin Azito`.

- [ ] **Step 5: Rebuild the three releases** (each writes its `release.json`; v115b and vanilla
  now load from disc too). Run in the background, one after another:

```bash
cd /c/Users/torso/Documents/deco/psprecomp
bash games/patapon3/scripts/p3.sh build 2>&1 | tail -2        # 1.1.4: code unchanged, adds release.json
P3_VARIANT=v115b P3_EBOOT=C:/Users/torso/Documents/deco/build/v115b/EBOOT_dec.BIN bash -c 'bash games/patapon3/scripts/p3.sh recompile && bash games/patapon3/scripts/p3.sh build' 2>&1 | tail -2
P3_VARIANT=vanilla P3_EBOOT=C:/Users/torso/Documents/deco/build/vanilla/EBOOT_dec.BIN bash -c 'bash games/patapon3/scripts/p3.sh overlays && bash games/patapon3/scripts/p3.sh recompile && bash games/patapon3/scripts/p3.sh build' 2>&1 | tail -2
cat ../build/rtw/release.json ../build/v115b/rtw/release.json ../build/vanilla/rtw/release.json
```
  Expected: three JSON files with ids `dxd-1.1.4`, `dxd-1.1.5b`, `vanilla-eu`; disc ids
  `INFN00001`, `INFN00001`, `UCES01421`; three different `eboot_sha256`.
  Vanilla banks now hold the right content: `grep -c "" ../build/vanilla/ovl/ovAzito_output/bank_dispatch.cpp`
  is close to the 1.1.4 `ovAzito` (3425 lines), not 21736.

- [ ] **Step 6: Boot checks.** `cd /c/Users/torso/Documents/deco && python -I build/tools/movie_check.py v115b p2t4`
  and `python -I build/tools/movie_check.py vanilla p2t4` → both `MOVIE OK`; each variant's log has
  `[EBOOT] loaded ... (decrypted, 3941504 bytes)`. Vanilla to the title screen with the
  `LOOKUP_MISS` count no higher than before the fix (`grep -ac LOOKUP_MISS` on the old
  `build/vanilla/tour114.log` vs the new boot log; write both numbers in the commit).

- [ ] **Step 7: Commit** (psprecomp): `scripts/release_info.py`, `p3.sh`, `test_p3.sh`. Message:
  `fix(p3): overlay banks follow the MWo3 header id; build writes release.json` + `Verified:`
  (test_p3 11/11, overlay-banks output for DxD and vanilla, three release.json, movie checks,
  LOOKUP_MISS before/after) + `Agent:`.

---

### Task 5: Launcher — ISO9660 and PARAM.SFO readers

**Files (launcher, `src-tauri/src/`):**
- Create: `iso.rs` (reader + `#[cfg(test)] pub mod testiso` image builder + tests)
- Create: `sfo.rs` (parser + `#[cfg(test)] pub fn build` + tests)
- Modify: `lib.rs` (add `mod iso;` and `mod sfo;` to the module list)

**Interfaces:**
- Produces: `iso::Iso::open(&Path) -> io::Result<Iso>` (`ErrorKind::InvalidData` = not an ISO9660
  image); `Iso::entries(&mut self) -> io::Result<Vec<iso::Entry>>`;
  `Iso::read_file(&mut self, path: &str) -> io::Result<Option<Vec<u8>>>` (case-insensitive,
  `/`-separated); `Iso::extract_all(&mut self, out: &Path, cancel: &AtomicBool,
  progress: &mut dyn FnMut(u64, u64)) -> io::Result<()>` (`ErrorKind::Interrupted` when cancelled).
  `iso::Entry { path: String, lba: u32, size: u32, is_dir: bool }`.
- Produces (tests only): `iso::testiso::{Node, build(Vec<Node>) -> Vec<u8>, psp_iso(eboot: &[u8], sfo: &[u8]) -> Vec<u8>, write_tmp(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf}`.
- Produces: `sfo::parse(&[u8]) -> Option<BTreeMap<String, String>>`; tests: `sfo::build(&[(&str, &str)]) -> Vec<u8>`.

- [ ] **Step 1: Write `sfo.rs` with its tests first.**

```rust
//! PARAM.SFO reader (PSP system file): key -> value for UTF-8 strings and 32-bit integers.
use std::collections::BTreeMap;

pub fn parse(b: &[u8]) -> Option<BTreeMap<String, String>> {
    if b.len() < 20 || &b[0..4] != b"\0PSF" { return None; }
    let rd16 = |o: usize| b.get(o..o + 2).map(|s| u16::from_le_bytes([s[0], s[1]]));
    let rd32 = |o: usize| b.get(o..o + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]));
    let (keys, data, n) = (rd32(8)? as usize, rd32(12)? as usize, rd32(16)? as usize);
    let mut out = BTreeMap::new();
    for i in 0..n.min(256) {
        let e = 20 + i * 16;
        let (koff, fmt, len, doff) = (rd16(e)? as usize, rd16(e + 2)?, rd32(e + 4)? as usize, rd32(e + 12)? as usize);
        let kstart = keys.checked_add(koff)?;
        let kend = kstart + b.get(kstart..)?.iter().position(|&c| c == 0)?;
        let key = String::from_utf8_lossy(&b[kstart..kend]).into_owned();
        let start = data.checked_add(doff)?;
        let v = b.get(start..start.checked_add(len)?)?;
        let val = match fmt {
            0x0404 if len >= 4 => u32::from_le_bytes([v[0], v[1], v[2], v[3]]).to_string(),
            _ => String::from_utf8_lossy(v).trim_end_matches('\0').to_string(),
        };
        out.insert(key, val);
    }
    Some(out)
}

/// A PARAM.SFO holding UTF-8 strings only (fmt 0x0204), for the tests.
#[cfg(test)]
pub fn build(entries: &[(&str, &str)]) -> Vec<u8> {
    let (mut keys, mut data, mut index) = (Vec::new(), Vec::new(), Vec::new());
    for (k, v) in entries {
        let koff = keys.len() as u16;
        keys.extend_from_slice(k.as_bytes());
        keys.push(0);
        let doff = data.len() as u32;
        let mut val = v.as_bytes().to_vec();
        val.push(0);
        let len = val.len() as u32;
        let max = len.div_ceil(4) * 4;
        val.resize(max as usize, 0);
        data.extend_from_slice(&val);
        for part in [&koff.to_le_bytes()[..], &0x0204u16.to_le_bytes(), &len.to_le_bytes(), &max.to_le_bytes(), &doff.to_le_bytes()] {
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

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn round_trip() {
        let m = parse(&build(&[("DISC_ID", "UCUS98751"), ("DISC_VERSION", "1.00"), ("TITLE", "PATAPON 3")])).unwrap();
        assert_eq!(m["DISC_ID"], "UCUS98751");
        assert_eq!(m["DISC_VERSION"], "1.00");
        assert_eq!(m["TITLE"], "PATAPON 3");
    }
    #[test] fn garbage_is_none() {
        assert!(parse(b"").is_none());
        assert!(parse(&[0u8; 64]).is_none());
        let mut b = build(&[("TITLE", "X")]);
        b.truncate(30);
        assert!(parse(&b).is_none(), "truncated index");
    }
    #[test] fn real_param_sfo_when_present() {
        let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../disc0/PSP_GAME/PARAM.SFO");
        let Ok(b) = std::fs::read(&p) else { eprintln!("SKIP: {} not on this PC", p.display()); return };
        assert_eq!(parse(&b).unwrap()["DISC_ID"], "INFN00001");
    }
}
```

- [ ] **Step 2: Write `iso.rs` tests + builder (the reader functions as `todo!()` stubs first).**
  The test builder (put at the end of `iso.rs`):

```rust
#[cfg(test)]
pub mod testiso {
    //! Small ISO9660 images built in memory for the tests: sectors 0-15 empty, 16 PVD,
    //! 17 terminator, then file data and one or more sectors per directory.
    use std::path::{Path, PathBuf};
    pub enum Node { File(&'static str, Vec<u8>), Dir(&'static str, Vec<Node>) }
    const S: usize = 2048;

    fn rec(name: &[u8], lba: u32, size: u32, dir: bool) -> Vec<u8> {
        let mut len = 33 + name.len();
        if len % 2 == 1 { len += 1; }
        let mut r = vec![0u8; len];
        r[0] = len as u8;
        r[2..6].copy_from_slice(&lba.to_le_bytes());
        r[6..10].copy_from_slice(&lba.to_be_bytes());
        r[10..14].copy_from_slice(&size.to_le_bytes());
        r[14..18].copy_from_slice(&size.to_be_bytes());
        r[25] = if dir { 2 } else { 0 };
        r[32] = name.len() as u8;
        r[33..33 + name.len()].copy_from_slice(name);
        r
    }
    fn pad(img: &mut Vec<u8>) { let n = img.len().div_ceil(S) * S; img.resize(n, 0); }
    fn put_dir(nodes: &[Node], img: &mut Vec<u8>) -> (u32, u32) {
        // "." and ".." point nowhere useful: the reader skips them.
        let mut recs = vec![rec(&[0], 0, S as u32, true), rec(&[1], 0, S as u32, true)];
        for n in nodes {
            match n {
                Node::File(name, data) => {
                    pad(img);
                    let lba = (img.len() / S) as u32;
                    img.extend_from_slice(data);
                    recs.push(rec(format!("{name};1").as_bytes(), lba, data.len() as u32, false));
                }
                Node::Dir(name, kids) => {
                    let (lba, size) = put_dir(kids, img);
                    recs.push(rec(name.as_bytes(), lba, size, true));
                }
            }
        }
        pad(img);
        let lba = (img.len() / S) as u32;
        let mut sector: Vec<u8> = Vec::new();
        for r in recs {
            if sector.len() % S + r.len() > S { sector.resize(sector.len().div_ceil(S) * S, 0); }
            sector.extend_from_slice(&r);
        }
        let size = sector.len().div_ceil(S) * S;
        sector.resize(size, 0);
        img.extend_from_slice(&sector);
        (lba, size as u32)
    }
    pub fn build(root: Vec<Node>) -> Vec<u8> {
        let mut img = vec![0u8; 18 * S];
        let (lba, size) = put_dir(&root, &mut img);
        let r = rec(&[0], lba, size, true);
        let pvd = &mut img[16 * S..17 * S];
        pvd[0] = 1;
        pvd[1..6].copy_from_slice(b"CD001");
        pvd[6] = 1;
        pvd[156..156 + r.len()].copy_from_slice(&r);
        let term = &mut img[17 * S..18 * S];
        term[0] = 255;
        term[1..6].copy_from_slice(b"CD001");
        term[6] = 1;
        img
    }
    /// A PSP-like disc: PSP_GAME/{PARAM.SFO, SYSDIR/EBOOT.BIN, USRDIR/DATA.BIN (5000 bytes)}.
    pub fn psp_iso(eboot: &[u8], sfo: &[u8]) -> Vec<u8> {
        let big: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
        build(vec![Node::Dir("PSP_GAME", vec![
            Node::File("PARAM.SFO", sfo.to_vec()),
            Node::Dir("SYSDIR", vec![Node::File("EBOOT.BIN", eboot.to_vec())]),
            Node::Dir("USRDIR", vec![Node::File("DATA.BIN", big)]),
        ])])
    }
    pub fn write_tmp(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
        let p = dir.join(name);
        std::fs::write(&p, bytes).unwrap();
        p
    }
}
```

  The tests (in `iso.rs`, `#[cfg(test)] mod tests`):

```rust
#[cfg(test)]
mod tests {
    use super::testiso::{build, psp_iso, write_tmp, Node};
    use super::*;

    #[test] fn lists_and_reads_nested_files() {
        let d = tempfile::tempdir().unwrap();
        let p = write_tmp(d.path(), "a.iso", &psp_iso(b"EBOOT", b"SFO"));
        let mut iso = Iso::open(&p).unwrap();
        let paths: Vec<String> = iso.entries().unwrap().into_iter().map(|e| e.path).collect();
        assert!(paths.contains(&"PSP_GAME/SYSDIR/EBOOT.BIN".to_string()), "{paths:?}");
        assert!(paths.contains(&"PSP_GAME/USRDIR".to_string()));
        assert_eq!(iso.read_file("psp_game/sysdir/eboot.bin").unwrap().unwrap(), b"EBOOT");
        assert!(iso.read_file("PSP_GAME/NOPE.BIN").unwrap().is_none());
    }
    #[test] fn extracts_everything_with_progress() {
        let d = tempfile::tempdir().unwrap();
        let p = write_tmp(d.path(), "a.iso", &psp_iso(b"EBOOT", b"SFO"));
        let out = d.path().join("out");
        let mut last = (0, 0);
        Iso::open(&p).unwrap().extract_all(&out, &AtomicBool::new(false), &mut |a, b| last = (a, b)).unwrap();
        assert_eq!(std::fs::read(out.join("PSP_GAME/SYSDIR/EBOOT.BIN")).unwrap(), b"EBOOT");
        let big = std::fs::read(out.join("PSP_GAME/USRDIR/DATA.BIN")).unwrap();
        assert_eq!(big.len(), 5000);
        assert_eq!(big[4999], (4999 % 251) as u8, "file larger than one sector intact");
        assert_eq!(last, (5000 + 5 + 3, 5000 + 5 + 3), "progress ends at total bytes");
    }
    #[test] fn cancel_stops_with_interrupted() {
        let d = tempfile::tempdir().unwrap();
        let p = write_tmp(d.path(), "a.iso", &psp_iso(b"EBOOT", b"SFO"));
        let err = Iso::open(&p).unwrap()
            .extract_all(&d.path().join("out"), &AtomicBool::new(true), &mut |_, _| {}).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::Interrupted);
    }
    #[test] fn not_an_iso_is_invalid_data() {
        let d = tempfile::tempdir().unwrap();
        for bytes in [vec![0u8; 40000], b"tiny".to_vec()] {
            let p = write_tmp(d.path(), "x.iso", &bytes);
            assert_eq!(Iso::open(&p).err().unwrap().kind(), io::ErrorKind::InvalidData);
        }
    }
    #[test] fn hostile_names_are_refused() {
        let d = tempfile::tempdir().unwrap();
        for name in ["..", "A/B", "C:X", "A\\B"] {
            let p = write_tmp(d.path(), "h.iso", &build(vec![Node::File(name, b"x".to_vec())]));
            let out = d.path().join("out");
            let r = Iso::open(&p).unwrap().extract_all(&out, &AtomicBool::new(false), &mut |_, _| {});
            assert_eq!(r.unwrap_err().kind(), io::ErrorKind::InvalidData, "{name}");
            assert!(!d.path().join("x").exists() && !d.path().join("B").exists());
        }
    }
    #[test] fn deep_nesting_is_refused() {
        // The loop guard: a directory chain deeper than 16 (or a record pointing back) stops.
        let mut n = Node::Dir("D", vec![]);
        for _ in 0..20 { n = Node::Dir("D", vec![n]); }
        let d = tempfile::tempdir().unwrap();
        let p = write_tmp(d.path(), "deep.iso", &build(vec![n]));
        assert_eq!(Iso::open(&p).unwrap().entries().unwrap_err().kind(), io::ErrorKind::InvalidData);
    }
}
```

  Run the launcher Rust gate. Expected: the `sfo` tests pass, the `iso` tests panic at `todo!()`.

- [ ] **Step 3: Implement the reader** (top of `iso.rs`, replacing the stubs):

```rust
//! Plain ISO9660 reader for PSP UMD images (no Joliet / Rock Ridge), the Rust port of
//! psprecomp/scripts/iso_extract.py: read one file by path, or extract everything.
use std::collections::HashSet;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

const SECTOR: u64 = 2048;

#[derive(Debug, Clone, PartialEq)]
pub struct Entry { pub path: String, pub lba: u32, pub size: u32, pub is_dir: bool }

pub struct Iso { file: File, root: (u32, u32) }

fn bad(msg: impl Into<String>) -> io::Error { io::Error::new(io::ErrorKind::InvalidData, msg.into()) }
fn le32(b: &[u8], o: usize) -> u32 { u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) }

impl Iso {
    pub fn open(path: &Path) -> io::Result<Iso> {
        let mut file = File::open(path)?;
        let mut pvd = [0u8; 2048];
        file.seek(SeekFrom::Start(16 * SECTOR))?;
        file.read_exact(&mut pvd).map_err(|_| bad("not an ISO9660 image"))?;
        if pvd[0] != 1 || &pvd[1..6] != b"CD001" { return Err(bad("not an ISO9660 image")); }
        Ok(Iso { file, root: (le32(&pvd, 158), le32(&pvd, 166)) })
    }

    fn read_at(&mut self, lba: u32, size: usize) -> io::Result<Vec<u8>> {
        let mut buf = vec![0u8; size];
        self.file.seek(SeekFrom::Start(lba as u64 * SECTOR))?;
        self.file.read_exact(&mut buf).map_err(|_| bad("truncated ISO image"))?;
        Ok(buf)
    }

    /// Every file and folder, parents before children, paths joined with '/'.
    pub fn entries(&mut self) -> io::Result<Vec<Entry>> {
        let (mut out, mut seen) = (Vec::new(), HashSet::new());
        let (lba, size) = self.root;
        self.walk(lba, size, "", 0, &mut seen, &mut out)?;
        Ok(out)
    }

    fn walk(&mut self, lba: u32, size: u32, prefix: &str, depth: u32,
            seen: &mut HashSet<u32>, out: &mut Vec<Entry>) -> io::Result<()> {
        if depth > 16 || !seen.insert(lba) { return Err(bad("directory loop in ISO image")); }
        if size > 16 << 20 { return Err(bad("directory record too large")); }
        let data = self.read_at(lba, size as usize)?;
        let mut i = 0usize;
        while i < data.len() {
            let len = data[i] as usize;
            if len == 0 { i = (i / SECTOR as usize + 1) * SECTOR as usize; continue; } // records never straddle sectors
            if len < 34 || i + len > data.len() { return Err(bad("bad directory record")); }
            let rec = &data[i..i + len];
            i += len;
            let name_len = rec[32] as usize;
            if 33 + name_len > len { return Err(bad("bad directory record")); }
            let raw = &rec[33..33 + name_len];
            if raw == [0] || raw == [1] { continue; }
            let flags = rec[25];
            if flags & 0x80 != 0 { return Err(bad("multi-extent files are not supported")); }
            let is_dir = flags & 2 != 0;
            let name = std::str::from_utf8(raw).map_err(|_| bad("file name is not UTF-8"))?;
            let name = name.split(';').next().unwrap_or("");
            let name = if is_dir { name } else { name.strip_suffix('.').unwrap_or(name) };
            if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', ':']) {
                return Err(bad(format!("unsafe file name {name:?} in ISO image")));
            }
            let path = if prefix.is_empty() { name.to_string() } else { format!("{prefix}/{name}") };
            let (elba, esize) = (le32(rec, 2), le32(rec, 10));
            out.push(Entry { path: path.clone(), lba: elba, size: esize, is_dir });
            if is_dir { self.walk(elba, esize, &path, depth + 1, seen, out)?; }
        }
        Ok(())
    }

    /// One file's bytes by path (case-insensitive, '/'-separated); None when absent.
    pub fn read_file(&mut self, path: &str) -> io::Result<Option<Vec<u8>>> {
        let want = path.to_ascii_lowercase();
        let Some(e) = self.entries()?.into_iter().find(|e| !e.is_dir && e.path.to_ascii_lowercase() == want)
        else { return Ok(None) };
        Ok(Some(self.read_at(e.lba, e.size as usize)?))
    }

    /// Extracts everything under `out` (created). `progress(done, total)` counts file bytes; a set
    /// `cancel` stops between chunks with ErrorKind::Interrupted. Names were checked by `entries`.
    pub fn extract_all(&mut self, out: &Path, cancel: &AtomicBool,
                       progress: &mut dyn FnMut(u64, u64)) -> io::Result<()> {
        let entries = self.entries()?;
        let total: u64 = entries.iter().filter(|e| !e.is_dir).map(|e| e.size as u64).sum();
        let mut done = 0u64;
        std::fs::create_dir_all(out)?;
        let mut buf = vec![0u8; 1 << 22];
        for e in &entries {
            if cancel.load(Ordering::Relaxed) { return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled")); }
            let dst = out.join(&e.path);
            if e.is_dir { std::fs::create_dir_all(&dst)?; continue; }
            self.file.seek(SeekFrom::Start(e.lba as u64 * SECTOR))?;
            let mut w = File::create(&dst)?;
            let mut left = e.size as u64;
            while left > 0 {
                if cancel.load(Ordering::Relaxed) { return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled")); }
                let n = left.min(buf.len() as u64) as usize;
                self.file.read_exact(&mut buf[..n])
                    .map_err(|_| bad(format!("truncated ISO image while reading {}", e.path)))?;
                w.write_all(&buf[..n])?;
                left -= n as u64;
                done += n as u64;
                progress(done, total);
            }
        }
        progress(total, total);
        Ok(())
    }
}
```

  Note: the PVD root record starts at byte 156, so its extent LBA is at 156+2 = 158 and its size
  at 156+10 = 166.

- [ ] **Step 4: Gate.** Launcher Rust gate: all pass (part 1's tests + 3 `sfo` + 6 `iso`).
  `cargo build` shows no new warnings (`#[allow(dead_code)]` is not allowed; Task 6 uses everything).
  If `dead_code` warnings appear for not-yet-used items, they are expected until Task 6 — say so
  in the report, do not silence them.

- [ ] **Step 5: Commit** (launcher): `feat: ISO9660 and PARAM.SFO readers` + `Verified:` + `Agent:`.

---

### Task 6: Launcher — release identification and installation

**Files (launcher, `src-tauri/`):**
- Create: `src/releases.rs`, `src/install.rs` (each with tests)
- Modify: `src/lib.rs` (`mod releases;`, `mod install;`), `Cargo.toml` (`sha2 = "0.10"` under `[dependencies]`)

**Interfaces:**
- Consumes: `iso::Iso`, `sfo::parse`, `iso::testiso` (Task 5).
- Produces:
  - `releases::Release { id, name, disc_id, eboot_sha256 }` (serde, `Clone`, `PartialEq`);
    `releases::load(root: &Path) -> Vec<Release>` (sorted by id; skips unreadable files and files
    whose `id` differs from their folder name); `releases::valid_id(&str) -> bool`;
    `releases::release_dir(root, id) -> PathBuf` (= `root/game/<id>`);
    `releases::identify(iso: &Path, releases: &[Release]) -> releases::Identity`;
    `Identity` = serde `tag = "kind"`: `Known { id, name }`, `OtherRelease { title, disc_id, version }`,
    `NotPspIso`, `Io { message }`; `releases::sha256_hex(&[u8]) -> String` (lowercase).
  - `install::version_dir(root, id)`, `install::disc0(root, id)`, `install::savedata(root, id)`
    (all `PathBuf`), `install::is_installed(root, id) -> bool`, `install::installed(root) -> Vec<String>`
    (sorted), `install::cleanup_partials(root)`, `install::install(root, iso, id, cancel: &AtomicBool,
    progress: &mut dyn FnMut(u64, u64)) -> io::Result<()>`.

- [ ] **Step 1: `releases.rs` tests** (write the module with `todo!()` bodies, then these tests):

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::iso::testiso::{psp_iso, write_tmp};

    fn put_release(root: &Path, folder: &str, r: &Release) {
        std::fs::create_dir_all(root.join("game").join(folder)).unwrap();
        std::fs::write(root.join("game").join(folder).join("release.json"), serde_json::to_string(r).unwrap()).unwrap();
    }
    fn rel(id: &str, eboot: &[u8]) -> Release {
        Release { id: id.into(), name: format!("Patapon 3 {id}"), disc_id: "INFN00001".into(), eboot_sha256: sha256_hex(eboot) }
    }

    #[test] fn sha256_known_vector() {
        assert_eq!(sha256_hex(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    #[test] fn load_reads_valid_folders_only() {
        let d = tempfile::tempdir().unwrap();
        put_release(d.path(), "dxd-1.1.5b", &rel("dxd-1.1.5b", b"B"));
        put_release(d.path(), "dxd-1.1.4", &rel("dxd-1.1.4", b"A"));
        put_release(d.path(), "wrong-folder", &rel("vanilla-eu", b"C"));
        std::fs::create_dir_all(d.path().join("game/broken")).unwrap();
        std::fs::write(d.path().join("game/broken/release.json"), "{").unwrap();
        let ids: Vec<String> = load(d.path()).into_iter().map(|r| r.id).collect();
        assert_eq!(ids, ["dxd-1.1.4", "dxd-1.1.5b"]);
        assert!(load(&d.path().join("nothing")).is_empty());
    }
    #[test] fn ids_are_plain_names() {
        for ok in ["dxd-1.1.4", "vanilla-eu", "a_b"] { assert!(valid_id(ok), "{ok}"); }
        for bad in ["", ".", "..", "../x", "a/b", "a\\b", "c:"] { assert!(!valid_id(bad), "{bad}"); }
    }
    #[test] fn identify_known_other_and_not_psp() {
        let d = tempfile::tempdir().unwrap();
        let releases = vec![rel("dxd-1.1.4", b"EBOOT-A")];
        let sfo = crate::sfo::build(&[("DISC_ID", "UCUS98751"), ("DISC_VERSION", "1.00"), ("TITLE", "PATAPON 3")]);
        let known = write_tmp(d.path(), "k.iso", &psp_iso(b"EBOOT-A", &sfo));
        assert_eq!(identify(&known, &releases), Identity::Known { id: "dxd-1.1.4".into(), name: "Patapon 3 dxd-1.1.4".into() });
        let other = write_tmp(d.path(), "o.iso", &psp_iso(b"EBOOT-US", &sfo));
        assert_eq!(identify(&other, &releases), Identity::OtherRelease {
            title: "PATAPON 3".into(), disc_id: "UCUS98751".into(), version: "1.00".into() });
        let zeros = write_tmp(d.path(), "z.iso", &vec![0u8; 40000]);
        assert_eq!(identify(&zeros, &releases), Identity::NotPspIso);
        let no_eboot = write_tmp(d.path(), "n.iso", &crate::iso::testiso::build(vec![]));
        assert_eq!(identify(&no_eboot, &releases), Identity::NotPspIso);
        assert!(matches!(identify(&d.path().join("missing.iso"), &releases), Identity::Io { .. }));
    }
    /// The user's three ISOs against the three builds' release.json (run with --ignored).
    #[test] #[ignore] fn real_isos() {
        let deco = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let releases: Vec<Release> = ["build/rtw", "build/v115b/rtw", "build/vanilla/rtw"].iter()
            .map(|b| serde_json::from_str(&std::fs::read_to_string(deco.join(b).join("release.json")).unwrap()).unwrap())
            .collect();
        for (iso, id) in [(deco.join("INFN00001.iso"), "dxd-1.1.4"),
                          (Path::new("C:/Users/torso/Downloads/INFN00001.iso").to_path_buf(), "dxd-1.1.5b"),
                          (Path::new("C:/Users/torso/Downloads/Patapon 3 (Europe).iso").to_path_buf(), "vanilla-eu")] {
            match identify(&iso, &releases) {
                Identity::Known { id: got, .. } => assert_eq!(got, id, "{}", iso.display()),
                other => panic!("{}: {other:?}", iso.display()),
            }
        }
    }
}
```

- [ ] **Step 2: `install.rs` tests:**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::iso::testiso::{psp_iso, write_tmp};
    use std::sync::atomic::AtomicBool;

    fn go(root: &Path, iso: &Path, cancel: bool) -> io::Result<()> {
        install(root, iso, "dxd-1.1.4", &AtomicBool::new(cancel), &mut |_, _| {})
    }

    #[test] fn installs_disc0_savedata_and_marker() {
        let d = tempfile::tempdir().unwrap();
        let iso = write_tmp(d.path(), "a.iso", &psp_iso(b"EBOOT", b"SFO"));
        let root = d.path().join("root");
        go(&root, &iso, false).unwrap();
        assert_eq!(std::fs::read(disc0(&root, "dxd-1.1.4").join("PSP_GAME/SYSDIR/EBOOT.BIN")).unwrap(), b"EBOOT");
        assert!(savedata(&root, "dxd-1.1.4").is_dir());
        assert!(is_installed(&root, "dxd-1.1.4"));
        assert_eq!(installed(&root), ["dxd-1.1.4"]);
        let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(version_dir(&root, "dxd-1.1.4").join("installed.json")).unwrap()).unwrap();
        assert_eq!(m["release"], "dxd-1.1.4");
        assert_eq!(m["iso_name"], "a.iso");
    }
    #[test] fn cancel_leaves_nothing_installed() {
        let d = tempfile::tempdir().unwrap();
        let iso = write_tmp(d.path(), "a.iso", &psp_iso(b"EBOOT", b"SFO"));
        let root = d.path().join("root");
        assert_eq!(go(&root, &iso, true).unwrap_err().kind(), io::ErrorKind::Interrupted);
        assert!(!is_installed(&root, "dxd-1.1.4"));
        assert!(!version_dir(&root, "dxd-1.1.4").join("disc0.partial").exists());
        assert!(installed(&root).is_empty());
    }
    #[test] fn reinstall_replaces_disc0_and_keeps_saves() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        go(&root, &write_tmp(d.path(), "a.iso", &psp_iso(b"OLD", b"SFO")), false).unwrap();
        std::fs::write(savedata(&root, "dxd-1.1.4").join("DATA04.BIN"), b"save").unwrap();
        std::fs::write(disc0(&root, "dxd-1.1.4").join("stale.txt"), b"x").unwrap();
        go(&root, &write_tmp(d.path(), "b.iso", &psp_iso(b"NEW", b"SFO")), false).unwrap();
        assert_eq!(std::fs::read(disc0(&root, "dxd-1.1.4").join("PSP_GAME/SYSDIR/EBOOT.BIN")).unwrap(), b"NEW");
        assert!(!disc0(&root, "dxd-1.1.4").join("stale.txt").exists());
        assert_eq!(std::fs::read(savedata(&root, "dxd-1.1.4").join("DATA04.BIN")).unwrap(), b"save");
    }
    #[test] fn failed_reinstall_keeps_the_previous_one() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        go(&root, &write_tmp(d.path(), "a.iso", &psp_iso(b"OLD", b"SFO")), false).unwrap();
        let bad = write_tmp(d.path(), "bad.iso", &vec![0u8; 40000]);
        assert!(go(&root, &bad, false).is_err());
        assert!(is_installed(&root, "dxd-1.1.4"));
        assert_eq!(std::fs::read(disc0(&root, "dxd-1.1.4").join("PSP_GAME/SYSDIR/EBOOT.BIN")).unwrap(), b"OLD");
    }
    #[test] fn leftovers_are_cleaned_and_bad_ids_refused() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        std::fs::create_dir_all(version_dir(&root, "x").join("disc0.partial/PSP_GAME")).unwrap();
        cleanup_partials(&root);
        assert!(!version_dir(&root, "x").join("disc0.partial").exists());
        let iso = write_tmp(d.path(), "a.iso", &psp_iso(b"E", b"S"));
        let err = install(&root, &iso, "../evil", &AtomicBool::new(false), &mut |_, _| {}).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(!d.path().join("evil").exists());
    }
    #[cfg(windows)]
    #[test] fn a_linked_disc0_is_never_replaced() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path().join("root");
        let real = d.path().join("dev_disc0");
        std::fs::create_dir_all(&real).unwrap();
        std::fs::write(real.join("keep.txt"), b"k").unwrap();
        std::fs::create_dir_all(version_dir(&root, "dxd-1.1.4")).unwrap();
        let st = std::process::Command::new("cmd").args(["/C", "mklink", "/J"])
            .arg(disc0(&root, "dxd-1.1.4")).arg(&real).status().unwrap();
        assert!(st.success());
        let iso = write_tmp(d.path(), "a.iso", &psp_iso(b"E", b"S"));
        assert!(go(&root, &iso, false).is_err());
        assert_eq!(std::fs::read(real.join("keep.txt")).unwrap(), b"k", "link target untouched");
    }
}
```

  Run the launcher Rust gate: the new tests fail (`todo!()`).

- [ ] **Step 3: Implement `releases.rs`:**

```rust
//! The game releases this package ships (game/<id>/release.json, written by psprecomp's
//! `p3.sh build`) and recognising a player's ISO among them by the SHA-256 of its EBOOT.BIN.
use crate::{iso::Iso, sfo};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::io::ErrorKind;
use std::path::{Path, PathBuf};

pub const EBOOT: &str = "PSP_GAME/SYSDIR/EBOOT.BIN";
pub const SFO: &str = "PSP_GAME/PARAM.SFO";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Release { pub id: String, pub name: String, pub disc_id: String, pub eboot_sha256: String }

pub fn release_dir(root: &Path, id: &str) -> PathBuf { root.join("game").join(id) }

pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id != "." && id != ".."
        && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '.' || c == '_')
}

pub fn sha256_hex(b: &[u8]) -> String { Sha256::digest(b).iter().map(|x| format!("{x:02x}")).collect() }

/// Every game/<id>/release.json whose id matches its folder, sorted by id.
pub fn load(root: &Path) -> Vec<Release> {
    let mut out = Vec::new();
    let Ok(dir) = std::fs::read_dir(root.join("game")) else { return out };
    for e in dir.flatten() {
        let Ok(text) = std::fs::read_to_string(e.path().join("release.json")) else { continue };
        let Ok(r) = serde_json::from_str::<Release>(&text) else { continue };
        if valid_id(&r.id) && e.file_name().to_string_lossy() == r.id { out.push(r); }
    }
    out.sort_by(|a, b| a.id.cmp(&b.id));
    out
}

#[derive(Debug, Serialize, PartialEq)]
#[serde(tag = "kind")]
pub enum Identity {
    Known { id: String, name: String },
    OtherRelease { title: String, disc_id: String, version: String },
    NotPspIso,
    Io { message: String },
}

pub fn identify(iso_path: &Path, releases: &[Release]) -> Identity {
    let io = |e: std::io::Error| if e.kind() == ErrorKind::InvalidData { Identity::NotPspIso }
                                 else { Identity::Io { message: e.to_string() } };
    let mut iso = match Iso::open(iso_path) { Ok(i) => i, Err(e) => return io(e) };
    let eboot = match iso.read_file(EBOOT) {
        Ok(Some(b)) => b,
        Ok(None) => return Identity::NotPspIso,
        Err(e) => return io(e),
    };
    let hash = sha256_hex(&eboot);
    if let Some(r) = releases.iter().find(|r| r.eboot_sha256.eq_ignore_ascii_case(&hash)) {
        return Identity::Known { id: r.id.clone(), name: r.name.clone() };
    }
    match iso.read_file(SFO).ok().flatten().and_then(|b| sfo::parse(&b)) {
        Some(m) => Identity::OtherRelease {
            title: m.get("TITLE").cloned().unwrap_or_default(),
            disc_id: m.get("DISC_ID").cloned().unwrap_or_default(),
            version: m.get("DISC_VERSION").cloned().unwrap_or_default(),
        },
        None => Identity::NotPspIso,
    }
}
```

- [ ] **Step 4: Implement `install.rs`:**

```rust
//! Installing a release from the player's ISO: versions/<id>/{disc0, savedata, installed.json}.
//! A release counts as installed only once installed.json exists; it is written last.
use crate::iso::Iso;
use crate::releases::valid_id;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

pub fn version_dir(root: &Path, id: &str) -> PathBuf { root.join("versions").join(id) }
pub fn disc0(root: &Path, id: &str) -> PathBuf { version_dir(root, id).join("disc0") }
pub fn savedata(root: &Path, id: &str) -> PathBuf { version_dir(root, id).join("savedata") }
pub fn is_installed(root: &Path, id: &str) -> bool { version_dir(root, id).join("installed.json").is_file() }

/// Ids of the installed releases, sorted.
pub fn installed(root: &Path) -> Vec<String> {
    let mut out: Vec<String> = std::fs::read_dir(root.join("versions")).map(|d| d.flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|id| valid_id(id) && is_installed(root, id)).collect()).unwrap_or_default();
    out.sort();
    out
}

/// Deletes what a crash left half done (disc0.partial / disc0.old).
pub fn cleanup_partials(root: &Path) {
    let Ok(dir) = std::fs::read_dir(root.join("versions")) else { return };
    for e in dir.flatten() {
        for name in ["disc0.partial", "disc0.old"] {
            let p = e.path().join(name);
            if p.is_dir() && !is_link(&p) { std::fs::remove_dir_all(&p).ok(); }
        }
    }
}

/// True for a symlink or a Windows junction (read_link succeeds for both).
fn is_link(p: &Path) -> bool { std::fs::read_link(p).is_ok() }

pub fn install(root: &Path, iso_path: &Path, id: &str, cancel: &AtomicBool,
               progress: &mut dyn FnMut(u64, u64)) -> io::Result<()> {
    if !valid_id(id) { return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("bad release id {id:?}"))); }
    let vdir = version_dir(root, id);
    let target = vdir.join("disc0");
    if is_link(&target) {
        return Err(io::Error::other(format!("{} is a link to another folder; not replacing it", target.display())));
    }
    std::fs::create_dir_all(&vdir)?;
    let partial = vdir.join("disc0.partial");
    if partial.exists() { std::fs::remove_dir_all(&partial)?; }
    if let Err(e) = Iso::open(iso_path).and_then(|mut iso| iso.extract_all(&partial, cancel, progress)) {
        std::fs::remove_dir_all(&partial).ok();
        return Err(e);
    }
    // Swap in the new extraction only now, so a failure above leaves the previous one playable.
    let old = vdir.join("disc0.old");
    if old.exists() { std::fs::remove_dir_all(&old)?; }
    if target.exists() { std::fs::rename(&target, &old)?; }
    std::fs::rename(&partial, &target)?;
    if old.exists() { std::fs::remove_dir_all(&old).ok(); }
    std::fs::create_dir_all(vdir.join("savedata"))?;
    let marker = serde_json::json!({
        "release": id,
        "iso_name": iso_path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
        "date": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
    });
    let tmp = vdir.join("installed.json.tmp");
    std::fs::write(&tmp, serde_json::to_string_pretty(&marker).map_err(io::Error::other)?)?;
    std::fs::rename(&tmp, vdir.join("installed.json"))
}
```

- [ ] **Step 5: Gates.** Launcher Rust gate: all pass (the Windows junction test included).
  Then the real-ISO check: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q real_isos -- --ignored 2>&1 | tail -3`
  → `test result: ok. 1 passed` (needs Task 4's three `release.json`).

- [ ] **Step 6: Commit** (launcher): `feat: identify the player's ISO and install it per release` + `Verified:` + `Agent:`.

---

### Task 7: Launcher — commands, per-release start, exit 11, last version

**Files (launcher, `src-tauri/`):**
- Modify: `src/launch.rs` (exe per release, env, `Outcome::Mismatch`, tests)
- Modify: `src/settings.rs` (catalog path, `last_version`, `set_last_version`, atomic write helper, tests)
- Modify: `src/lib.rs` (new commands, plugin, startup cleanup, tests)
- Modify: `Cargo.toml` (`tauri-plugin-dialog = "2"`), `capabilities/default.json` (`"dialog:allow-open"`)

**Interfaces:**
- Consumes: `releases::{load, release_dir, identify, Identity, Release}`, `install::*` (Task 6).
- Produces (Tauri commands, JS names in quotes; consumed by Task 8):
  `"list_versions"() -> { releases: Release[], installed: string[], current: string | null }`;
  `"set_version"({ id })`; `"identify_iso"({ path }) -> Identity`;
  `"install_iso"({ path, id })` (emits event `"install_progress"` `{ done, total }`; rejects with
  `"cancelled"` when cancelled, otherwise with the error text); `"cancel_install"()`;
  `"open_save_folder"({ id })`; `"check_install"({ id }) -> { exe, disc0, ascii_path }`;
  `"start_game"({ id }) -> Outcome` with `Outcome = Menu | Quit | Mismatch | Crash { code }`.

- [ ] **Step 1: Tests first.** In `launch.rs` replace the tests `outcomes`, `exe_name_per_os` and
  `install_checks` with:

```rust
    #[test] fn outcomes() {
        assert_eq!(outcome_from_code(Some(10)), Outcome::Menu);
        assert_eq!(outcome_from_code(Some(0)), Outcome::Quit);
        assert_eq!(outcome_from_code(Some(11)), Outcome::Mismatch);
        assert_eq!(outcome_from_code(Some(3)), Outcome::Crash { code: Some(3) });
        assert_eq!(outcome_from_code(None), Outcome::Crash { code: None });
    }
    #[test] fn exe_name_per_os_and_release() {
        let p = game_exe(Path::new("R"), "dxd-1.1.4");
        if cfg!(windows) { assert!(p.ends_with("game/dxd-1.1.4/psprecomp_runtime.exe")); }
        else { assert!(p.ends_with("game/dxd-1.1.4/psprecomp_runtime")); }
    }
    #[test] fn env_points_at_the_release_folders() {
        let env = game_env(Path::new("C:/Juegos/Patapon 3"), "vanilla-eu");
        assert_eq!(env[0].0, "PSPRECOMP_DISC0");
        assert!(env[0].1.replace('\\', "/").ends_with("Patapon 3/versions/vanilla-eu/disc0"));
        assert_eq!(env[1].0, "PSPRECOMP_SAVEDATA");
        assert!(env[1].1.replace('\\', "/").ends_with("versions/vanilla-eu/savedata"));
    }
    #[test] fn install_checks() {
        let d = tempfile::tempdir().unwrap();
        let id = "dxd-1.1.4";
        assert_eq!(check_install(d.path(), id), InstallCheck { exe: false, disc0: false, ascii_path: path_is_ascii(d.path()) });
        std::fs::create_dir_all(crate::releases::release_dir(d.path(), id)).unwrap();
        std::fs::write(game_exe(d.path(), id), b"x").unwrap();
        std::fs::create_dir_all(crate::install::disc0(d.path(), id)).unwrap();
        assert!(!check_install(d.path(), id).disc0, "disc0 without installed.json is not installed");
        std::fs::write(crate::install::version_dir(d.path(), id).join("installed.json"), b"{}").unwrap();
        let c = check_install(d.path(), id);
        assert!(c.exe && c.disc0);
        assert!(!path_is_ascii(Path::new("C:/Juegos/Ñandú")));
    }
```

  In `settings.rs` tests add:

```rust
    #[test] fn last_version_round_trip_keeps_the_file() {
        let d = root();
        load(d.path()).unwrap();
        assert_eq!(last_version(d.path()), None);
        set_last_version(d.path(), "dxd-1.1.5b").unwrap();
        assert_eq!(last_version(d.path()).as_deref(), Some("dxd-1.1.5b"));
        let t = std::fs::read_to_string(d.path().join("settings.ini")).unwrap();
        assert!(t.contains("[launcher]") && t.contains("version = dxd-1.1.5b") && t.contains("[display]"));
        assert_eq!(load(d.path()).unwrap()["game.fps"], "30", "options still load");
    }
    #[test] fn catalog_comes_from_a_release_folder() {
        let d = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(d.path().join("game/dxd-1.1.4")).unwrap();
        std::fs::copy(format!("{}/../../psprecomp/games/patapon3/game_options.json", env!("CARGO_MANIFEST_DIR")),
                      d.path().join("game/dxd-1.1.4/game_options.json")).unwrap();
        let (defs, warnings) = catalogs(d.path());
        assert!(warnings.is_empty(), "{warnings:?}");
        assert!(defs.iter().any(|o| o.section == "game"));
    }
```

  In `lib.rs` add at the end:

```rust
#[cfg(test)]
mod tests {
    use super::*;
    fn put(root: &std::path::Path, id: &str, installed: bool) {
        std::fs::create_dir_all(releases::release_dir(root, id)).unwrap();
        let r = releases::Release { id: id.into(), name: id.into(), disc_id: "X".into(), eboot_sha256: "0".into() };
        std::fs::write(releases::release_dir(root, id).join("release.json"), serde_json::to_string(&r).unwrap()).unwrap();
        if installed {
            std::fs::create_dir_all(install::version_dir(root, id)).unwrap();
            std::fs::write(install::version_dir(root, id).join("installed.json"), b"{}").unwrap();
        }
    }
    #[test] fn current_version_rules() {
        let d = tempfile::tempdir().unwrap();
        assert_eq!(versions_payload(d.path()).current, None);
        put(d.path(), "dxd-1.1.4", true);
        put(d.path(), "vanilla-eu", true);
        put(d.path(), "dxd-1.1.5b", false);
        let v = versions_payload(d.path());
        assert_eq!(v.installed, ["dxd-1.1.4", "vanilla-eu"]);
        assert_eq!(v.current.as_deref(), Some("dxd-1.1.4"), "first installed when none chosen");
        settings::set_last_version(d.path(), "vanilla-eu").unwrap();
        assert_eq!(versions_payload(d.path()).current.as_deref(), Some("vanilla-eu"));
        settings::set_last_version(d.path(), "dxd-1.1.5b").unwrap();
        assert_eq!(versions_payload(d.path()).current.as_deref(), Some("dxd-1.1.4"), "not installed -> first installed");
        std::fs::create_dir_all(install::version_dir(d.path(), "orphan")).unwrap();
        std::fs::write(install::version_dir(d.path(), "orphan").join("installed.json"), b"{}").unwrap();
        assert!(!versions_payload(d.path()).installed.contains(&"orphan".to_string()), "no exe for it");
    }
}
```

  Run the launcher Rust gate: compile errors (new names missing).

- [ ] **Step 2: launch.rs.** Replace `game_exe`, `check_install`, `Outcome`, `outcome_from_code` and
  `run_game` with:

```rust
pub fn game_exe(root: &Path, id: &str) -> PathBuf {
    crate::releases::release_dir(root, id)
        .join(if cfg!(windows) { "psprecomp_runtime.exe" } else { "psprecomp_runtime" })
}

pub fn check_install(root: &Path, id: &str) -> InstallCheck {
    InstallCheck {
        exe: game_exe(root, id).is_file(),
        disc0: crate::install::is_installed(root, id) && crate::install::disc0(root, id).is_dir(),
        ascii_path: path_is_ascii(root),
    }
}

#[derive(Serialize, Debug, PartialEq)]
#[serde(tag = "kind")]
pub enum Outcome { Menu, Quit, Mismatch, Crash { code: Option<i32> } }

/// 0 quit, 10 back to the menu, 11 the disc0 files do not match this exe (settings-v1.md).
pub fn outcome_from_code(code: Option<i32>) -> Outcome {
    match code {
        Some(10) => Outcome::Menu,
        Some(0) => Outcome::Quit,
        Some(11) => Outcome::Mismatch,
        c => Outcome::Crash { code: c },
    }
}

/// The release's folders, as environment variables (they take precedence over settings.ini).
pub fn game_env(root: &Path, id: &str) -> Vec<(String, String)> {
    vec![
        ("PSPRECOMP_DISC0".into(), crate::install::disc0(root, id).to_string_lossy().into_owned()),
        ("PSPRECOMP_SAVEDATA".into(), crate::install::savedata(root, id).to_string_lossy().into_owned()),
    ]
}
```
  and in `run_game(root: &Path, id: &str)`: after `rotate_logs(root)?;` add
  `std::fs::create_dir_all(crate::install::savedata(root, id))?;`, build the command with
  `Command::new(game_exe(root, id)).args(game_args(root)).envs(game_env(root, id)).current_dir(crate::releases::release_dir(root, id))`.
  Update the module comment's code list to `0 quit, 10 back to menu, 11 files mismatch, other crash`.

- [ ] **Step 3: settings.rs.** Replace the catalog read in `catalogs` with a lookup through:

```rust
/// game_options.json of the packaged releases (every copy is the same game's catalog): the
/// first game/<id>/game_options.json by name, else game/game_options.json.
fn catalog_path(root: &Path) -> PathBuf {
    let mut dirs: Vec<PathBuf> = std::fs::read_dir(root.join("game"))
        .map(|d| d.flatten().map(|e| e.path()).filter(|p| p.join("game_options.json").is_file()).collect())
        .unwrap_or_default();
    dirs.sort();
    dirs.first().map(|d| d.join("game_options.json")).unwrap_or_else(|| root.join("game").join("game_options.json"))
}
```
  (`catalogs` reads `catalog_path(root)`; its warning text becomes `format!("{}: {e}", p.display())`).
  Move the tmp-write-rename tail of `save` into
  `fn write_atomic(p: &Path, text: &str) -> Result<(), String>` and use it from `save`. Add:

```rust
/// The last played release (`[launcher] version`), launcher-only (settings-v1.md).
pub fn last_version(root: &Path) -> Option<String> {
    let text = std::fs::read_to_string(ini_path(root)).ok()?;
    IniFile::parse(&text).get("launcher", "version").filter(|v| !v.is_empty())
}

pub fn set_last_version(root: &Path, id: &str) -> Result<(), String> {
    let (defs, _) = catalogs(root);
    let p = ini_path(root);
    let text = std::fs::read_to_string(&p).unwrap_or_else(|_| default_ini(&defs));
    let mut f = IniFile::parse(&text);
    f.set("launcher", "version", id);
    write_atomic(&p, &f.to_string())
}
```

- [ ] **Step 4: lib.rs.** Add `use std::sync::{atomic::{AtomicBool, Ordering}, Arc};`,
  `use tauri::Emitter;`, the modules, and:

```rust
#[derive(Serialize)]
struct VersionsPayload { releases: Vec<releases::Release>, installed: Vec<String>, current: Option<String> }

/// Installed = installed.json present AND this package has the exe for it; current = the last
/// played one when still installed, else the first installed.
fn versions_payload(root: &std::path::Path) -> VersionsPayload {
    let releases = releases::load(root);
    let installed: Vec<String> = install::installed(root).into_iter()
        .filter(|id| releases.iter().any(|r| &r.id == id)).collect();
    let current = settings::last_version(root).filter(|v| installed.contains(v))
        .or_else(|| installed.first().cloned());
    VersionsPayload { releases, installed, current }
}

#[derive(Default)]
struct InstallState { cancel: Arc<AtomicBool>, running: AtomicBool }

#[derive(Clone, Serialize)]
struct Progress { done: u64, total: u64 }

#[tauri::command]
fn list_versions() -> VersionsPayload { versions_payload(&settings::root_dir()) }

#[tauri::command]
fn set_version(id: String) -> Result<(), String> {
    let root = settings::root_dir();
    if !versions_payload(&root).installed.contains(&id) { return Err(format!("{id} is not installed")); }
    settings::set_last_version(&root, &id)
}

#[tauri::command]
async fn identify_iso(path: String) -> releases::Identity {
    let root = settings::root_dir();
    tauri::async_runtime::spawn_blocking(move || releases::identify(std::path::Path::new(&path), &releases::load(&root)))
        .await
        .unwrap_or_else(|e| releases::Identity::Io { message: e.to_string() })
}

#[tauri::command]
async fn install_iso(app: AppHandle, state: State<'_, InstallState>, path: String, id: String) -> Result<(), String> {
    let root = settings::root_dir();
    if !releases::load(&root).iter().any(|r| r.id == id) { return Err(format!("unknown release {id}")); }
    if state.running.swap(true, Ordering::SeqCst) { return Err("already installing".into()); }
    state.cancel.store(false, Ordering::SeqCst);
    let (cancel, root2, id2) = (state.cancel.clone(), root.clone(), id.clone());
    let r = tauri::async_runtime::spawn_blocking(move || {
        let mut last = std::time::Instant::now();
        install::install(&root2, std::path::Path::new(&path), &id2, &cancel, &mut |done, total| {
            if done == total || last.elapsed() >= std::time::Duration::from_millis(100) {
                last = std::time::Instant::now();
                app.emit("install_progress", Progress { done, total }).ok();
            }
        })
    }).await;
    state.running.store(false, Ordering::SeqCst);
    match r {
        Ok(Ok(())) => settings::set_last_version(&root, &id),
        Ok(Err(e)) if e.kind() == std::io::ErrorKind::Interrupted => Err("cancelled".into()),
        Ok(Err(e)) => Err(e.to_string()),
        Err(e) => Err(e.to_string()),
    }
}

#[tauri::command]
fn cancel_install(state: State<'_, InstallState>) { state.cancel.store(true, Ordering::SeqCst); }

fn open_path(p: &std::path::Path) -> Result<(), String> {
    let opener = if cfg!(windows) { "explorer" } else { "xdg-open" };
    std::process::Command::new(opener).arg(p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

#[tauri::command]
fn open_save_folder(id: String) -> Result<(), String> {
    if !releases::valid_id(&id) { return Err(format!("bad release id {id}")); }
    let dir = install::savedata(&settings::root_dir(), &id);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    open_path(&dir)
}
```
  Change `check_install` to take `id: String` and call `launch::check_install(&root, &id)`;
  `start_game` gains `id: String` (reject `!releases::valid_id(&id)` with an error) and calls
  `launch::run_game(&root, &id)`; `open_log` uses `open_path`. In `run()`: `.plugin(tauri_plugin_dialog::init())`,
  `.manage(InstallState::default())`, `install::cleanup_partials(&settings::root_dir());` first in
  `setup`, and the handler list gains `list_versions, set_version, identify_iso, install_iso,
  cancel_install, open_save_folder`. `capabilities/default.json` permissions become
  `["core:default", "core:window:allow-close", "dialog:allow-open"]`.

- [ ] **Step 5: Gates.** Launcher Rust gate: all pass. `cargo build 2>&1 | grep -c warning` → 0
  new warnings (the dead-code warnings from Task 5/6 are gone now).

- [ ] **Step 6: Commit** (launcher): `feat: per-release start, install commands, exit 11, last version` + `Verified:` + `Agent:`.

---

### Task 8: Launcher UI — first run, installing, version selector, Options actions

**Files (launcher):**
- Modify: `src/types.ts`, `src/api.ts`, `src/App.tsx`, `src/nav/useNav.ts`,
  `src/screens/MainMenu.tsx`, `src/screens/Options.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json`
- Create: `src/screens/FirstRun.tsx`, `src/screens/Installing.tsx`, `src/screens/FirstRun.test.tsx`
- Modify tests: `src/screens/Start.test.tsx` (mocks gain the new api functions),
  `src/screens/MainMenu.test.tsx`, `src/screens/Options.test.tsx` (one new test each)
- `package.json`: `npm install --save-exact @tauri-apps/plugin-dialog@2` (record the version in the report)

**Interfaces:**
- Consumes: the Tauri commands of Task 7.
- Produces: `useListNav(count, onConfirm, onBack?, onSide?: (dir: 1 | -1) => void)` (left/right);
  `MainMenu` prop `version?: { label: string; onStep(dir: 1 | -1): void }`;
  `Options` prop `actions?: { id: string; label: string; run(): void }[]` (shown on the General tab
  after the option rows).

- [ ] **Step 1: Strings.** Add to `lang/es.json`:

```json
  "firstrun.title": "Arrastra aquí tu ISO de Patapon 3",
  "firstrun.hint": "o pulsa Confirmar para buscarla",
  "firstrun.browse": "Buscar ISO",
  "install.preparing": "{name} ✓ — preparando…",
  "install.cancel": "Cancelar",
  "iso.other": "Esta ISO es «{title}» {disc_id} v{version}. Por ahora funcionan: {supported}.",
  "iso.not_psp": "Este archivo no es una ISO de PSP.",
  "iso.io": "No se pudo leer la ISO: {message}",
  "iso.install_failed": "No se pudo preparar el juego: {message}",
  "menu.version": "Versión",
  "options.add_iso": "Añadir otra ISO",
  "options.open_saves": "Abrir carpeta de saves",
  "start.mismatch": "Los archivos de esta versión están dañados o mezclados. Vuelve a añadir la ISO."
```
  and to `lang/en.json`:

```json
  "firstrun.title": "Drop your Patapon 3 ISO here",
  "firstrun.hint": "or press Confirm to browse for it",
  "firstrun.browse": "Browse for the ISO",
  "install.preparing": "{name} ✓ — preparing…",
  "install.cancel": "Cancel",
  "iso.other": "This ISO is «{title}» {disc_id} v{version}. Supported for now: {supported}.",
  "iso.not_psp": "This file is not a PSP ISO.",
  "iso.io": "The ISO could not be read: {message}",
  "iso.install_failed": "The game could not be prepared: {message}",
  "menu.version": "Version",
  "options.add_iso": "Add another ISO",
  "options.open_saves": "Open the save folder",
  "start.mismatch": "The files of this version are damaged or mixed up. Add the ISO again."
```
  Change `start.missing_disc0` (es) to `"No se encontraron los datos de esta versión. Vuelve a añadir la ISO."`
  and (en) to `"The data of this version is missing. Add the ISO again."`.

- [ ] **Step 2: Types and api.** `types.ts` gains:

```ts
export type Release = { id: string; name: string; disc_id: string; eboot_sha256: string };
export type Versions = { releases: Release[]; installed: string[]; current: string | null };
export type Identity =
  | { kind: "Known"; id: string; name: string }
  | { kind: "OtherRelease"; title: string; disc_id: string; version: string }
  | { kind: "NotPspIso" }
  | { kind: "Io"; message: string };
```
  and `Outcome` becomes `{ kind: "Menu" } | { kind: "Quit" } | { kind: "Mismatch" } | { kind: "Crash"; code: number | null }`.
  `api.ts`: `checkInstall: (id: string) => invoke<InstallCheck>("check_install", { id })`,
  `startGame: (id: string) => invoke<Outcome>("start_game", { id })`, plus:

```ts
  listVersions: () => invoke<Versions>("list_versions"),
  setVersion: (id: string) => invoke<void>("set_version", { id }),
  identifyIso: (path: string) => invoke<Identity>("identify_iso", { path }),
  installIso: (path: string, id: string) => invoke<void>("install_iso", { path, id }),
  cancelInstall: () => invoke<void>("cancel_install"),
  openSaveFolder: (id: string) => invoke<void>("open_save_folder", { id }),
  pickIso: async (): Promise<string | null> => {
    const { open } = await import("@tauri-apps/plugin-dialog");
    const p = await open({ multiple: false, directory: false, filters: [{ name: "ISO", extensions: ["iso", "ISO"] }] });
    return typeof p === "string" ? p : null;
  },
```

- [ ] **Step 3: Failing tests.** Create `src/screens/FirstRun.test.tsx`:

```tsx
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ close: vi.fn() }) }));
vi.mock("@tauri-apps/api/webview", () => ({ getCurrentWebview: () => ({ onDragDropEvent: vi.fn(async () => () => {}) }) }));
vi.mock("@tauri-apps/api/core", () => ({ convertFileSrc: (p: string) => p, invoke: vi.fn() }));
const strings = {
  "menu.start": "Iniciar", "menu.options": "Opciones", "menu.exit": "Salir", "menu.version": "Versión",
  "firstrun.title": "Arrastra aquí tu ISO", "firstrun.hint": "o Confirmar", "firstrun.browse": "Buscar ISO",
  "install.preparing": "{name} ✓ — preparando…", "install.cancel": "Cancelar",
  "iso.other": "Esta ISO es «{title}» {disc_id} v{version}. Por ahora funcionan: {supported}.",
  "iso.not_psp": "No es una ISO de PSP.", "start.mismatch": "Archivos mezclados",
};
const rel = (id: string) => ({ id, name: `P3 ${id}`, disc_id: "X", eboot_sha256: "0" });
const api = vi.hoisted(() => ({
  getOptions: vi.fn(async () => ({ options: [], values: { "general.language": "es" }, warnings: [] })),
  getLang: vi.fn(async () => ({})),
  getTheme: vi.fn(async () => ({ colors: {}, background: "", logo: "" })),
  saveSettings: vi.fn(), openLog: vi.fn(), checkInstall: vi.fn(), startGame: vi.fn(),
  listVersions: vi.fn(), setVersion: vi.fn(async () => {}), identifyIso: vi.fn(), installIso: vi.fn(),
  cancelInstall: vi.fn(), openSaveFolder: vi.fn(), pickIso: vi.fn(),
}));
vi.mock("../api", () => ({ api }));
import { App } from "../App";

beforeEach(() => { api.getLang.mockResolvedValue(strings); });

test("nothing installed: drop screen; a known ISO installs and opens the menu", async () => {
  api.listVersions.mockResolvedValueOnce({ releases: [rel("dxd-1.1.4")], installed: [], current: null })
    .mockResolvedValue({ releases: [rel("dxd-1.1.4")], installed: ["dxd-1.1.4"], current: "dxd-1.1.4" });
  api.pickIso.mockResolvedValue("C:/isos/p3.iso");
  api.identifyIso.mockResolvedValue({ kind: "Known", id: "dxd-1.1.4", name: "P3 dxd-1.1.4" });
  api.installIso.mockResolvedValue(undefined);
  render(<App />);
  expect(await screen.findByText("Arrastra aquí tu ISO")).toBeInTheDocument();
  await userEvent.keyboard("{Enter}");
  await waitFor(() => expect(api.installIso).toHaveBeenCalledWith("C:/isos/p3.iso", "dxd-1.1.4"));
  expect(await screen.findByText("Iniciar")).toBeInTheDocument();
  expect(screen.queryByTestId("version-picker")).toBeNull();
});

test("another release is explained with the supported list", async () => {
  api.listVersions.mockResolvedValue({ releases: [rel("dxd-1.1.4"), rel("vanilla-eu")], installed: [], current: null });
  api.pickIso.mockResolvedValue("C:/isos/us.iso");
  api.identifyIso.mockResolvedValue({ kind: "OtherRelease", title: "PATAPON 3", disc_id: "UCUS98751", version: "1.00" });
  render(<App />);
  await screen.findByText("Arrastra aquí tu ISO");
  await userEvent.keyboard("{Enter}");
  expect(await screen.findByText("Esta ISO es «PATAPON 3» UCUS98751 v1.00. Por ahora funcionan: P3 dxd-1.1.4, P3 vanilla-eu."))
    .toBeInTheDocument();
  expect(api.installIso).not.toHaveBeenCalled();
});

test("not a PSP ISO", async () => {
  api.listVersions.mockResolvedValue({ releases: [rel("dxd-1.1.4")], installed: [], current: null });
  api.pickIso.mockResolvedValue("C:/x.zip");
  api.identifyIso.mockResolvedValue({ kind: "NotPspIso" });
  render(<App />);
  await screen.findByText("Arrastra aquí tu ISO");
  await userEvent.keyboard("{Enter}");
  expect(await screen.findByText("No es una ISO de PSP.")).toBeInTheDocument();
});

test("two installed: the selector switches release with the arrows", async () => {
  api.listVersions.mockResolvedValue({ releases: [rel("dxd-1.1.4"), rel("vanilla-eu")], installed: ["dxd-1.1.4", "vanilla-eu"], current: "dxd-1.1.4" });
  render(<App />);
  expect(await screen.findByTestId("version-picker")).toHaveTextContent("P3 dxd-1.1.4");
  await userEvent.keyboard("{ArrowRight}");
  await waitFor(() => expect(api.setVersion).toHaveBeenCalledWith("vanilla-eu"));
});

test("exit 11 sends the player back to the drop screen with the reason", async () => {
  api.listVersions.mockResolvedValue({ releases: [rel("dxd-1.1.4")], installed: ["dxd-1.1.4"], current: "dxd-1.1.4" });
  api.checkInstall.mockResolvedValue({ exe: true, disc0: true, ascii_path: true });
  api.startGame.mockResolvedValue({ kind: "Mismatch" });
  render(<App />);
  await userEvent.click(await screen.findByText("Iniciar"));
  await waitFor(() => expect(api.startGame).toHaveBeenCalledWith("dxd-1.1.4"));
  expect(await screen.findByText("Archivos mezclados")).toBeInTheDocument();
  expect(screen.getByText("Arrastra aquí tu ISO")).toBeInTheDocument();
});
```
  In `Start.test.tsx` add to the hoisted `api` object:
  `listVersions: vi.fn(async () => ({ releases: [{ id: "dxd-1.1.4", name: "P3", disc_id: "X", eboot_sha256: "0" }], installed: ["dxd-1.1.4"], current: "dxd-1.1.4" })), setVersion: vi.fn(), identifyIso: vi.fn(), installIso: vi.fn(), cancelInstall: vi.fn(), openSaveFolder: vi.fn(), pickIso: vi.fn(),`
  and the `@tauri-apps/api/webview` mock line above; its existing tests must keep passing unchanged
  otherwise (they now also check `api.startGame` was called with `"dxd-1.1.4"` where they check
  it was called).
  In `Options.test.tsx` add a test: render `Options` with `actions={[{ id: "add_iso", label: "Añadir otra ISO", run }]}`
  and defs that put one row on the general tab; press `{ArrowDown}` then `{Enter}`; expect `run`
  called once and `onSaved` not called. In `MainMenu.test.tsx` add a test: with
  `version={{ label: "DxD 1.1.5b", onStep }}`, the text `DxD 1.1.5b` is shown and `{ArrowLeft}`
  calls `onStep(-1)`; without `version` there is no `version-picker` test id (the existing three
  tests keep passing as they are).
  Run the UI gate: the new tests fail.

- [ ] **Step 4: `useNav.ts`.** Signature `useListNav(count: number, onConfirm: (i: number) => void, onBack?: () => void, onSide?: (dir: 1 | -1) => void)`;
  `ref.current = { index, count, onConfirm, onBack, onSide };` and in the handler, after the `down` case:

```ts
    else if (a === "left") ref.current.onSide?.(-1);
    else if (a === "right") ref.current.onSide?.(1);
```

- [ ] **Step 5: `FirstRun.tsx`:**

```tsx
import { useEffect, useRef, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { api } from "../api";
import { PromptBar } from "../components/PromptBar";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";

type Props = { strings: Record<string, string>; message: string; onPick(path: string): void; onBack?: () => void };

// "Drop your ISO here": a dropped file or the file picker (Confirm / click) gives the ISO path.
export function FirstRun(p: Props) {
  const [hover, setHover] = useState(false);
  const pick = useRef(p.onPick);
  pick.current = p.onPick;
  const browse = async () => {
    const path = await api.pickIso().catch(() => null);
    if (path) pick.current(path);
  };
  const nav = useListNav(1, () => { void browse(); }, p.onBack);
  useEffect(() => {
    let un: (() => void) | null = null;
    let closed = false;
    getCurrentWebview().onDragDropEvent((ev) => {
      const e = ev.payload;
      if (e.type === "enter" || e.type === "over") setHover(true);
      else if (e.type === "leave") setHover(false);
      else if (e.type === "drop") { setHover(false); if (e.paths.length > 0) pick.current(e.paths[0]); }
    }).then((u) => { if (closed) u(); else un = u; }).catch(() => {});
    return () => { closed = true; un?.(); };
  }, []);
  return (
    <div className={`screen first-run${hover ? " drop-hover" : ""}`} data-testid="first-run">
      <div className="panel drop-zone" role="button" onClick={() => { void browse(); }}>
        <h1 className="title">{t(p.strings, "firstrun.title")}</h1>
        <p>{t(p.strings, "firstrun.hint")}</p>
      </div>
      {p.message && <p className="panel notice-text" role="alert">{p.message}</p>}
      <PromptBar device={nav.device} items={[
        { id: "confirm", label: t(p.strings, "firstrun.browse") },
        ...(p.onBack ? [{ id: "back", label: t(p.strings, "prompt.back") }] : []),
      ]} />
    </div>
  );
}
```

- [ ] **Step 6: `Installing.tsx`:**

```tsx
import { useEffect, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { PromptBar } from "../components/PromptBar";
import { t } from "../i18n";
import { useListNav } from "../nav/useNav";

type Props = { strings: Record<string, string>; name: string; onCancel(): void };

// Extraction progress ("install_progress" events from install_iso) with a Cancel entry.
export function Installing(p: Props) {
  const [pct, setPct] = useState(0);
  useEffect(() => {
    let un: (() => void) | null = null;
    let closed = false;
    listen<{ done: number; total: number }>("install_progress", (e) =>
      setPct(e.payload.total > 0 ? Math.floor((100 * e.payload.done) / e.payload.total) : 0))
      .then((u) => { if (closed) u(); else un = u; }).catch(() => {});
    return () => { closed = true; un?.(); };
  }, []);
  const nav = useListNav(1, () => p.onCancel(), p.onCancel);
  return (
    <div className="screen installing" data-testid="installing">
      <p className="panel notice-text">{t(p.strings, "install.preparing", { name: p.name })}</p>
      <progress className="install-progress" max={100} value={pct} />
      <ul className="menu" role="listbox">
        <li role="option" aria-selected className="selected" onClick={p.onCancel}>{t(p.strings, "install.cancel")}</li>
      </ul>
      <PromptBar device={nav.device} items={[{ id: "confirm", label: t(p.strings, "install.cancel") }]} />
    </div>
  );
}
```

- [ ] **Step 7: `MainMenu.tsx`.** Add `version?: { label: string; onStep(dir: 1 | -1): void }` to
  `Props`; `useListNav(entries.length, (i) => entries[i].run(), undefined, p.version ? (d) => p.version!.onStep(d) : undefined)`;
  render, just above `<ul className="menu" ...>`:

```tsx
      {p.version && (
        <p className="version-picker" data-testid="version-picker">
          {t(p.strings, "menu.version")}:{" "}
          <button className="arrow" onClick={() => p.version!.onStep(-1)}>◂</button>
          {" "}{p.version.label}{" "}
          <button className="arrow" onClick={() => p.version!.onStep(1)}>▸</button>
        </p>
      )}
```

- [ ] **Step 8: `Options.tsx`.** Add `actions?: { id: string; label: string; run(): void }[]` to
  `Props`. After `const rows = ...`: `const actions = TABS[tab] === "general" ? (p.actions ?? []) : [];`
  and `const total = rows.length + actions.length;`. In `handle`: the `up`/`down` case uses
  `if (total) setRow((row + (a === "down" ? 1 : total - 1)) % total);`; the `confirm` case starts with
  `if (row >= rows.length) { actions[row - rows.length]?.run(); break; }`. In the option list,
  after the `rows.map(...)`, render:

```tsx
          {actions.map((x, i) => (
            <li key={x.id} role="option" aria-selected={row === rows.length + i}
                className={`action${row === rows.length + i ? " selected" : ""}`}
                onMouseEnter={() => setRow(rows.length + i)} onClick={() => x.run()}>{x.label}</li>
          ))}
```

- [ ] **Step 9: `App.tsx`.** Add `type Screen = "loading" | "firstrun" | "installing" | "menu" | "options" | "notice" | "crash";`
  (initial `"loading"`, rendered as an empty `<div className="screen" />`), state
  `versions: Versions | null`, `isoMsg: string`, `installName: string`, and:

```tsx
  const loadVersions = useCallback(async () => {
    try { const v = await api.listVersions(); setVersions(v); return v; } catch { return null; }
  }, []);

  useEffect(() => {
    void loadVersions().then((v) => setScreen(v && v.installed.length > 0 ? "menu" : "firstrun"));
  }, [loadVersions]);

  const releaseName = (id: string | null) => versions?.releases.find((r) => r.id === id)?.name ?? id ?? "";

  const toFirstRun = (msg: string) => { setIsoMsg(msg); setScreen("firstrun"); };

  const pickIso = async (path: string) => {
    setIsoMsg("");
    let id: Identity;
    try { id = await api.identifyIso(path); } catch (e) { id = { kind: "Io", message: String(e) }; }
    if (id.kind === "Known") {
      setInstallName(id.name);
      setScreen("installing");
      try {
        await api.installIso(path, id.id);
        await loadVersions();
        setScreen("menu");
      } catch (e) {
        toFirstRun(String(e) === "cancelled" ? "" : t(strings, "iso.install_failed", { message: String(e) }));
      }
      return;
    }
    if (id.kind === "OtherRelease") {
      toFirstRun(t(strings, "iso.other", { title: id.title, disc_id: id.disc_id, version: id.version,
        supported: (versions?.releases ?? []).map((r) => r.name).join(", ") }));
    } else if (id.kind === "NotPspIso") {
      toFirstRun(t(strings, "iso.not_psp"));
    } else {
      toFirstRun(t(strings, "iso.io", { message: id.message }));
    }
  };

  const stepVersion = async (dir: 1 | -1) => {
    if (!versions || versions.installed.length < 2) return;
    const i = Math.max(0, versions.installed.indexOf(versions.current ?? ""));
    const next = versions.installed[(i + dir + versions.installed.length) % versions.installed.length];
    await api.setVersion(next).catch(() => {});
    await loadVersions();
  };
```
  In `start`: `const id = versions?.current; if (!id) { toFirstRun(""); return; }` before the
  install check (inside the existing `try`, after `setBusy(true)`), `api.checkInstall(id)`,
  `api.startGame(id)`, and handle `o.kind === "Mismatch"` with `toFirstRun(t(strings, "start.mismatch"))`.
  Rendering: `"firstrun"` → `<FirstRun strings={strings} message={isoMsg} onPick={(p) => { void pickIso(p); }} onBack={versions && versions.installed.length > 0 ? toMenu : undefined} />`;
  `"installing"` → `<Installing strings={strings} name={installName} onCancel={() => { void api.cancelInstall(); }} />`;
  `MainMenu` gets `version={versions && versions.installed.length > 1 ? { label: releaseName(versions.current), onStep: (d) => { void stepVersion(d); } } : undefined}`;
  `Options` gets `actions={[{ id: "add_iso", label: t(strings, "options.add_iso"), run: () => toFirstRun("") },
  { id: "open_saves", label: t(strings, "options.open_saves"), run: () => { if (versions?.current) void api.openSaveFolder(versions.current); } }]}`.

- [ ] **Step 10: `styles.css`.** Using the existing theme variables only, add rules for
  `.first-run .drop-zone` (dashed 2px border in the accent color, generous padding, centered text,
  `cursor: pointer`), `.first-run.drop-hover .drop-zone` (solid border, slightly lighter panel),
  `.install-progress` (full panel width, 1.2rem high), `.version-picker` (centered, `.arrow` buttons
  borderless and transparent) and `.option-list .action` (same layout as an option row). No new
  colors.

- [ ] **Step 11: Gates.** UI gate: Vitest all pass (part 1's + 5 FirstRun + 1 Options + 1 MainMenu),
  `npx tsc --noEmit` clean; launcher Rust gate still green; `npm run tauri build -- --no-bundle`
  succeeds.

- [ ] **Step 12: Commit** (launcher): `feat: first run with the player's ISO, version selector, save folder` + `Verified:` + `Agent:`.

---

### Task 9: Portable package with three releases, end to end, Findings

**Files:**
- Modify: `launcher/scripts/make-portable.mjs`, `launcher/README.md`
- Create: `launcher/scripts/e2e_keys.py` (key injection for the E2E; kept in the repo this time)
- Create: `launcher/scripts/make-portable.test.mjs` (node:test)
- Modify: `psprecomp/docs/superpowers/plans/2026-10-17-launcher-part2.md` (`## Findings`)

**Interfaces:**
- Consumes: `release.json` (Task 4), the launcher build (Task 8).
- Produces: `node scripts/make-portable.mjs --game <id>=<build rtw folder> [...] [--dev-disc0 <id>=<disc0>] [--textures <folder>] [--out <folder>]`.

- [ ] **Step 1: Failing test** `scripts/make-portable.test.mjs` (run with `node --test scripts/`):

```js
// make-portable must never delete a player's installed releases, saves or settings.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const win = process.platform === "win32";
const exe = win ? "psprecomp_runtime.exe" : "psprecomp_runtime";

function fakeGame(dir, id) {
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, exe), "exe");
  writeFileSync(join(dir, "game_options.json"), "{}");
  writeFileSync(join(dir, "release.json"), JSON.stringify({ id, name: id, disc_id: "X", eboot_sha256: "0" }));
  for (const dll of ["libc++.dll", "libunwind.dll"]) writeFileSync(join(dir, dll), "dll");
}

test("packages every release and keeps versions/ and settings.ini", () => {
  const t = mkdtempSync(join(tmpdir(), "p3mp-"));
  fakeGame(join(t, "a"), "dxd-1.1.4");
  fakeGame(join(t, "b"), "vanilla-eu");
  const out = join(t, "out");
  mkdirSync(join(out, "versions", "dxd-1.1.4", "savedata"), { recursive: true });
  writeFileSync(join(out, "versions", "dxd-1.1.4", "savedata", "DATA04.BIN"), "save");
  writeFileSync(join(out, "settings.ini"), "[launcher]\nversion = dxd-1.1.4\n");
  execFileSync("node", ["scripts/make-portable.mjs", "--game", `dxd-1.1.4=${join(t, "a")}`,
    "--game", `vanilla-eu=${join(t, "b")}`, "--out", out, "--skip-launcher"]);
  assert.ok(existsSync(join(out, "game", "dxd-1.1.4", exe)));
  assert.ok(existsSync(join(out, "game", "vanilla-eu", "release.json")));
  assert.equal(readFileSync(join(out, "versions", "dxd-1.1.4", "savedata", "DATA04.BIN"), "utf8"), "save");
  assert.match(readFileSync(join(out, "settings.ini"), "utf8"), /version = dxd-1.1.4/);
});

test("refuses a folder whose release.json names another id", () => {
  const t = mkdtempSync(join(tmpdir(), "p3mp-"));
  fakeGame(join(t, "a"), "dxd-1.1.4");
  assert.throws(() => execFileSync("node", ["scripts/make-portable.mjs", "--game", `vanilla-eu=${join(t, "a")}`,
    "--out", join(t, "out"), "--skip-launcher"], { stdio: "pipe" }));
});
```
  (`--skip-launcher` is a test-only switch: do not require/copy the launcher exe.) Run:
  `cd /c/Users/torso/Documents/deco/launcher && node --test scripts/` → fails.

- [ ] **Step 2: Rewrite `make-portable.mjs`:**

```js
// Assembles the portable folder: launcher, one game/<release-id>/ per release (exe + DLLs +
// game_options.json + release.json), lang, themes. Default output: portable/Patapon3Recomp
// (outside Vite's dist/, which every build empties). Only launcher, game/, lang/ and themes/ are
// replaced: versions/ (installed releases and saves), settings.ini and logs/ are the player's.
//   --game <id>=<folder>       once per release; the folder holds the exe and its release.json
//   --dev-disc0 <id>=<disc0>   developer shortcut: versions/<id>/disc0 links to an existing
//                              extraction (junction on Windows) and installed.json is written
//   --textures <folder>        links textures/ to an HD texture pack folder
import { cpSync, existsSync, lstatSync, mkdirSync, readFileSync, readdirSync, rmSync, symlinkSync, unlinkSync, writeFileSync } from "node:fs";
import { delimiter, join, resolve } from "node:path";

const all = (name) => process.argv.flatMap((a, i) => (a === name ? [process.argv[i + 1]] : []));
const fail = (msg) => { console.error(msg); process.exit(1); };
const pairs = (name) => all(name).map((s) => {
  const i = (s ?? "").indexOf("=");
  if (i <= 0) fail(`${name} wants <id>=<folder>, got ${s}`);
  return [s.slice(0, i), resolve(s.slice(i + 1))];
});
const validId = (id) => /^[A-Za-z0-9._-]+$/.test(id) && id !== "." && id !== "..";
const win = process.platform === "win32";
const exe = win ? "psprecomp_runtime.exe" : "psprecomp_runtime";
const games = pairs("--game");
const devDisc0 = pairs("--dev-disc0");
const textures = all("--textures")[0] ? resolve(all("--textures")[0]) : undefined;
const out = resolve(all("--out")[0] ?? "portable/Patapon3Recomp");
const skipLauncher = process.argv.includes("--skip-launcher");

if (games.length === 0) fail("give at least one --game <id>=<folder>");
for (const [id, dir] of games) {
  if (!validId(id)) fail(`bad release id ${id}`);
  if (!existsSync(join(dir, exe))) fail(`no ${exe} in ${dir}`);
  if (!existsSync(join(dir, "release.json"))) fail(`no release.json in ${dir} (psprecomp p3.sh build writes it)`);
  const rel = JSON.parse(readFileSync(join(dir, "release.json"), "utf8"));
  if (rel.id !== id) fail(`${dir}/release.json is ${rel.id}, not ${id}`);
}
const launcher = join("src-tauri", "target", "release", win ? "p3-launcher.exe" : "p3-launcher");
if (!skipLauncher && !existsSync(launcher)) fail("build the launcher first: npm run tauri build -- --no-bundle");

// Unlink data links first so nothing ever walks into the player's or developer's folders.
const unlinkIfLink = (p) => { try { if (lstatSync(p).isSymbolicLink()) unlinkSync(p); } catch { /* absent */ } };
unlinkIfLink(join(out, "textures"));
unlinkIfLink(join(out, "disc0"));   // part 1 layout
mkdirSync(out, { recursive: true });
for (const name of ["game", "lang", "themes"]) rmSync(join(out, name), { recursive: true, force: true });
mkdirSync(join(out, "logs"), { recursive: true });
if (!skipLauncher) cpSync(launcher, join(out, win ? "launcher.exe" : "launcher"));

for (const [id, dir] of games) {
  const dst = join(out, "game", id);
  mkdirSync(dst, { recursive: true });
  for (const f of readdirSync(dir)) {
    if (f === exe || f.endsWith(".dll") || f === "game_options.json" || f === "release.json") cpSync(join(dir, f), join(dst, f));
  }
  // The MinGW-clang build also needs the toolchain's C++ runtime DLLs, which the build PC finds
  // on PATH but a player's PC does not: copy them from PATH when the game folder lacks them.
  if (win) {
    for (const dll of ["libc++.dll", "libunwind.dll"]) {
      if (existsSync(join(dst, dll))) continue;
      const p = (process.env.PATH ?? "").split(delimiter).find((d) => d && existsSync(join(d, dll)));
      if (p) cpSync(join(p, dll), join(dst, dll));
      else console.warn(`warning: ${dll} not found on PATH; the game may not start on other PCs`);
    }
  }
}
cpSync("lang", join(out, "lang"), { recursive: true });
cpSync("themes", join(out, "themes"), { recursive: true });
if (textures && existsSync(textures)) symlinkSync(textures, join(out, "textures"), win ? "junction" : "dir");

for (const [id, disc0] of devDisc0) {
  if (!validId(id)) fail(`bad release id ${id}`);
  const vdir = join(out, "versions", id);
  const link = join(vdir, "disc0");
  let isLink = false;
  try { isLink = lstatSync(link).isSymbolicLink(); } catch { /* absent */ }
  if (existsSync(link) && !isLink) { console.warn(`warning: ${id} is installed for real in ${vdir}; not linking`); continue; }
  if (isLink) unlinkSync(link);
  mkdirSync(join(vdir, "savedata"), { recursive: true });
  symlinkSync(disc0, link, win ? "junction" : "dir");
  writeFileSync(join(vdir, "installed.json"), JSON.stringify({ release: id, iso_name: "(developer link)", date: 0 }, null, 2));
}
console.log(`portable folder: ${out}`);
```
  Run `node --test scripts/` → 2 pass.

- [ ] **Step 3: `scripts/e2e_keys.py`** (Windows only; real scan codes — part 1 Findings: scan code
  0 never reaches SDL2 as F1):

```python
#!/usr/bin/env python3
"""Key injection for the launcher E2E (Windows). Real scan codes: SDL2 maps keys by scan code.

  python -I scripts/e2e_keys.py focus "<window title>"
  python -I scripts/e2e_keys.py key enter|esc|up|down|left|right|f1 [count]
  python -I scripts/e2e_keys.py type "<text>"        (file dialogs: types the path)
"""
import ctypes
import sys
import time

user32 = ctypes.windll.user32
VK = {"enter": 0x0D, "esc": 0x1B, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "f1": 0x70}
EXTENDED = {0x26, 0x28, 0x25, 0x27}


def press(vk: int) -> None:
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = 1 if vk in EXTENDED else 0  # KEYEVENTF_EXTENDEDKEY
    user32.keybd_event(vk, scan, flags, 0)
    time.sleep(0.05)
    user32.keybd_event(vk, scan, flags | 2, 0)  # KEYEVENTF_KEYUP
    time.sleep(0.15)


def type_text(text: str) -> None:
    for ch in text:
        r = user32.VkKeyScanW(ord(ch))
        vk, shift = r & 0xFF, (r >> 8) & 1
        if shift:
            user32.keybd_event(0x10, user32.MapVirtualKeyW(0x10, 0), 0, 0)
        press(vk)
        if shift:
            user32.keybd_event(0x10, user32.MapVirtualKeyW(0x10, 0), 2, 0)


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "focus":
        hwnd = user32.FindWindowW(None, sys.argv[2])
        if not hwnd:
            print(f"no window titled {sys.argv[2]!r}")
            return 1
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        return 0
    if cmd == "key":
        for _ in range(int(sys.argv[3]) if len(sys.argv) > 3 else 1):
            press(VK[sys.argv[2]])
        return 0
    if cmd == "type":
        type_text(sys.argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Build the package** (fresh folder, no `--dev-disc0`, so the real first run happens):

```bash
cd /c/Users/torso/Documents/deco/launcher
npm run tauri build -- --no-bundle 2>&1 | tail -2
D=C:/Users/torso/Documents/deco
node scripts/make-portable.mjs --game dxd-1.1.4=$D/build/rtw --game dxd-1.1.5b=$D/build/v115b/rtw \
  --game vanilla-eu=$D/build/vanilla/rtw --textures $D/PPSSPP/memstick/PSP/TEXTURES --out $D/build/e2e_portable
```

- [ ] **Step 5: End to end** (Claude drives; game debug port 9998 through the environment the
  launcher passes on: start the launcher with `PSPRECOMP_DEBUG_PORT=9998`). For each ISO in order
  `C:/Users/torso/Documents/deco/INFN00001.iso` (1.1.4), `C:/Users/torso/Downloads/INFN00001.iso`
  (1.1.5b), `C:/Users/torso/Downloads/Patapon 3 (Europe).iso` (vanilla):
  1. First ISO: the launcher opens on the drop screen. `e2e_keys.py focus "Patapon 3"`, `key enter`
     opens the file picker; `type "<iso path>"`, `key enter`. Later ISOs: Options (`key down`,
     `key enter`), General tab, `key down` to "Añadir otra ISO", `key enter`, then the same picker steps.
  2. Wait until `build/e2e_portable/versions/<id>/installed.json` exists (poll with a Monitor
     until-loop, ≤ 3 min); the menu is back.
  3. With two or more installed: `key left`/`key right` until the selector shows the release just
     installed (check `[launcher] version` in `settings.ini`).
  4. `key enter` on Iniciar. After 40 s: `logs/game.log` has `[EBOOT] loaded .../versions/<id>/disc0/PSP_GAME/SYSDIR/EBOOT.BIN (decrypted, 3941504 bytes)`;
     a capture through the debug socket (`S <png>` on 9998, `build/tools/p3key.py`) shows the title
     or opening movie (look at it).
  5. `key f1`, `key left`, `key enter` → exit 10 → the launcher menu is back.
  Then: (a) a wrong file (`C:/Users/torso/Documents/deco/ppsspp_win.zip`) through "Añadir otra ISO"
  → the "no es una ISO de PSP" message (desktop screenshot); (b) flip one byte of
  `versions/dxd-1.1.4/disc0/PSP_GAME/SYSDIR/EBOOT.BIN` (keep a copy), Start → the drop screen with
  the mismatch message, `logs/game.log` ends with `[EBOOT] ...; exit 11`; restore the file;
  (c) cancel an install halfway (Esc on the progress screen) → no `installed.json` for it, no
  `disc0.partial` left, the other releases still start.
  If the file picker cannot be driven by keys, stop there and ask the user to do steps 1 by hand.

- [ ] **Step 6: Regression.** psprecomp: ctest 74/74, test_p3 11/11, 1.1.4 tour `p2_t9` vs
  `rebuild114_t1` `RT_REGRESS PASS`; launcher: Rust gate, UI gate, `node --test scripts/`.
  `jugar.bat` still starts 1.1.4 from `DECO/disc0` (boot 20 s on port 9998 with the same
  environment `jugar.bat` sets, `[EBOOT] loaded`).

- [ ] **Step 7: README.** `launcher/README.md`: the new make-portable arguments, the folder layout
  (`game/<id>/`, `versions/<id>/`), and that a rebuild keeps `versions/`, `settings.ini` and `logs/`.

- [ ] **Step 8: Findings + commits.** Write `## Findings` at the end of this plan: E2E results per
  ISO (install time, boot line, screenshots looked at), the three refusal checks, what needed the
  user, any plan defect fixed on the way. Commit launcher (`feat: portable package with one folder
  per release; E2E key driver`) and psprecomp (`docs(plan): launcher part 2 findings`), each with
  `Verified:` and `Agent:`. Update memory `p3-launcher-part1.md`/new `p3-launcher-part2.md`. Nothing
  is pushed; the user plays and approves.

---

## Findings (2026-10-17, execution)

Built by DeepSeek flash (OpenCode, the user's API key) from one brief per task; Claude read every
diff, re-ran the gates and committed. Task 1 was written by Claude (the OpenCode deny list still froze
`crates/` from the closed relevo until the user re-ran its setup).

Commits — psprecomp (`patapon3-windows`): 1d17b60 (T1), fdf260b (T2), 85018c6 (T3), fc788f7 (exit 11
fix), 92af841 (T4). Launcher (`main`): 52b71e7 (T5), 566a484 (T6), 84d70ad (T7), ca4110d (T8),
3834511 + 04411ff (T9). Nothing pushed.

**End to end** (fresh `build/e2e_portable`, launcher with `PSPRECOMP_DEBUG_PORT=9998`):

| ISO | release | install | boot line | looked at |
|---|---|---|---|---|
| `DECO/INFN00001.iso` | dxd-1.1.4 | ok (the injected path came out garbled, see 7; the ISO was then picked by hand) | `[EBOOT] loaded ...\versions\dxd-1.1.4\disc0/PSP_GAME/SYSDIR/EBOOT.BIN (decrypted, 3941504 bytes)` | DxD language menu (debug-socket capture) |
| `Downloads/INFN00001.iso` | dxd-1.1.5b | < 7 s (ISO in the file cache) | same, `dxd-1.1.5b` | DxD language menu; menu shows `Versión ◂ Patapon 3 DxD 1.1.5b ▸` |
| `Downloads/Patapon 3 (Europe).iso` | vanilla-eu | ~1 s (cached) | same, `vanilla-eu` | vanilla's own language list (English first) |

- F1 → left → Enter returned to the launcher menu each time (exit 10). `▸` from vanilla-eu wrapped to
  dxd-1.1.4, `settings.ini` `[launcher] version = dxd-1.1.4`, and Start booted the 1.1.4 disc0.
- Refusals: `ppsspp_win.zip` → "Este archivo no es una ISO de PSP."; one byte flipped in the installed
  1.1.4 EBOOT.BIN → `[EBOOT] ... decrypt failed (pspDecryptPRX returned -4); exit 11` and the drop screen
  with "Los archivos de esta versión están dañados o mezclados..." (file restored afterwards).
- Cancel by hand could not be exercised: with the ISO in the file cache an install takes about a
  second, and the Enter meant for Cancel landed on the menu's Start. Cancel, failed reinstall and
  leftover `.partial` stay covered by the Rust tests (Task 6).
- Re-running make-portable over the E2E folder kept the three installs, `settings.ini` and the saves.

**Regression:** ctest 74/74; test_p3 12/12; launcher cargo 44 + 1 ignored (`real_isos` passes with
`--ignored`: the three ISOs identified by their EBOOT hash), Vitest 23/23, `node --test` 2/2;
1.1.4 tours `p2_t3c` and `p2_t9` vs `rebuild114_t1` RT_REGRESS PASS; v115b and vanilla MOVIE OK;
`jugar.bat`'s environment boots 1.1.4 from `DECO/disc0` (`[EBOOT] loaded`).

**Plan defects fixed on the way:**
1. The disc-mode `data_sections.cpp` includes `psp_eboot_loader.h`, but the generated recomp target
   did not see `runtime/include` (DeepSeek stopped BLOCKED with the diagnosis); the runtime CMake now
   adds it to the aliased recomp target.
2. `std::exit(11)` ran static destructors with the debug-socket thread joinable and aborted
   (0xC0000409, rc 127): the launcher would have shown a crash. Now `std::quick_exit(11)`, like the
   fast shutdown. Found by DeepSeek re-checking Task 3; the first T3 commit message wrongly claimed
   rc=11 from the log text.
3. `kirk_engine.c` is GPL-3.0-or-later, not GPL-2.0 as the plan said: the kirk README and the main
   README say a runtime exe is distributed under GPL-3.0.
4. `node --test scripts/` does not expand a folder on Node 24 (use `scripts/*.test.mjs`), and Vitest
   collected the node:test file: `vite.config.ts` now limits Vitest to `src/`.
5. test_p3 has 12 checks after the task (the plan said 11; the old suite had 9, not 8).
6. v115b's overlay banks predated the Task 1 emitter commit, so its build hit the stale-bank guard;
   DeepSeek re-ran `p3.sh overlays` for v115b (no guard relaxed).
7. E2E: typing paths key by key broke with Caps Lock and the Spanish layout (`\` needs AltGr) →
   `e2e_keys.py type` pastes; make-portable deleted `game/ lang/ themes/` before copying a locked
   `launcher.exe` → it now copies the launcher first.

**Open / for later:** the three `game/<id>/` folders weigh 307 MB together (FFmpeg DLLs in each);
sharing the DLLs or one exe folder per release with links is a packaging question for part 7. The
first-run hint text is small and the dashed drop-zone border is faint at 1080p (cosmetic, part 5 themes).
