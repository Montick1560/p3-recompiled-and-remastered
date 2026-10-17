# Patapon 3 community launcher — Part 2: first run with the player's ISO (design)

Date: 2026-10-17. Status: approved in conversation (sections 1-2), awaiting written-spec review.
Author: Claude Opus 5.5 with the user (brainstorming). Builder for the plan: DeepSeek flash
(`deepseek/deepseek-flash` through OpenCode, the user's own key), Claude designs, reviews and
verifies. Follows `2026-10-17-launcher-part1-design.md` (decomposition item 2).

## Goal

"Simple as fuck": the player only has to care about having the right ISO and their saves.
They drop their Patapon 3 ISO on the launcher once; everything else (which release it is, which
exe runs it, the EBOOT decryption, the folders) happens on its own and stays invisible.
Supported releases: **DxD 1.1.4**, **DxD 1.1.5b** and **vanilla EU UCES01421 v1.01**.

### Decisions taken (conversation 2026-10-17)

- **Several releases installed side by side**, each with its own `disc0` and `savedata`. A
  `Version ◂ ▸` selector appears in the main menu only when two or more are installed.
- **The exe no longer carries the EBOOT image.** It reads the player's (encrypted) `EBOOT.BIN`
  from `disc0` at boot, decrypts it in memory and loads it. No decrypted EBOOT is written to disk.
- **The launcher extracts the ISO** to a per-release `disc0` (the runtime keeps reading plain
  files; afterwards the ISO can be moved or deleted).
- **All three exes ship in the portable package** (`game/<release-id>/`, about 37 MB each); no
  downloads.
- **Saves stay minimal in part 2:** a `savedata` folder per release and an "Open the save folder"
  button. Importing / converting PPSSPP saves remains part 3.

### Facts verified before the design (2026-10-17)

- The `EBOOT.BIN` of all three ISOs is encrypted (`~PSP`, tags `0xD9160BF0` for both DxD releases,
  `0xD91613F0` for vanilla, `decrypt_mode` 9); `BOOT.BIN` is zero-filled.
- PPSSPP's `Core/ELF/PrxDecrypter.cpp` + `ext/libkirk` (GPL-2.0, same licence as psprecomp),
  compiled on their own with four small shim headers, decrypt all three to a 2,719,616-byte ELF
  that is byte-identical to the first 2,719,616 bytes of the user's PPSSPP dumps (the dumps carry
  336 trailing bytes of buffer garbage outside the ELF). Spike: `DECO/build/spike_decrypt/`
  (throwaway).
- Today the whole image is one `static const uint8_t seg_08804000[3941504]` in the generated
  `data_sections.cpp`, copied by `psp_init_data_sections()` (`runtime/src/main.cpp:231`). In all
  three releases it equals the single PT_LOAD's file bytes (offset `0xb4`, vaddr `0x08804000`,
  filesz `0x297860`) followed by zeros up to memsz `0x3C2480` (DeepSeek audit,
  `DECO/build/spike_decrypt/AUDIT_deepseek.md` §1.4). No other generated or runtime code reads the
  array. Everything else release-specific (functions, dispatch, constructors, imports, patch
  words) is compiled code: each release keeps its own exe.
- **Existing vanilla bug found by the audits:** the three vanilla overlay files carry other
  content under the DxD names (`MWo3` header id at offset 4: vanilla `OL_Azito.bin` = 2 Mission,
  `OL_Mission.bin` = 3 Title, `OL_Title.bin` = 1 Azito; DxD: Azito 1, Mission 2, Title 3).
  `p3.sh overlays` names banks by file name, so `game.toml` `force_entries` (looked up by bank
  name) land in the wrong overlay for vanilla. The runtime still activates banks by content hash,
  which is why vanilla boots and plays.

## 1. What the player sees

1. **First run** (no release installed): one screen, "Drop your Patapon 3 ISO here (or click to
   browse)". Keyboard / gamepad: Confirm opens the file picker.
2. **After dropping it:** "Patapon 3 DxD 1.1.5b ✓ — preparing…" with a progress bar and Cancel.
   No questions. When it finishes, the normal main menu (Start · Options · Exit) appears.
3. **Wrong ISO:** one human message, then back to the drop screen:
   - another Patapon 3 release: "This ISO is «PATAPON 3» UCUS98751 v1.00. Supported for now:
     DxD 1.1.4, DxD 1.1.5b, Europe UCES01421." (title, disc id and version read from the ISO's
     `PARAM.SFO`; no hard-coded table of other releases);
   - not a PSP ISO / unreadable: "This file is not a PSP ISO." / the OS error, in plain words.
4. **Afterwards nothing is asked again.** The ISO may be deleted or moved.
5. **Another release:** Options › General › "Add another ISO" (same flow). With two or more
   installed the main menu shows `Version ◂ ▸` (left / right, remembered). With one, never.
6. **Saves:** Options › General › "Open the save folder" opens the current release's `savedata`.
7. **Damaged or mixed files** (the exe's EBOOT check fails, exit code 11): "The files of this
   version are damaged or mixed up. Add the ISO again." with a button that restarts the flow for
   that release.

All strings in `lang/es.json` and `lang/en.json`.

## 2. Portable folder layout (part 2)

```
Patapon3Recomp/
  launcher(.exe)  settings.ini  lang/  themes/  logs/  textures/
  game/dxd-1.1.4/    psprecomp_runtime(.exe) + DLLs + game_options.json + release.json
  game/dxd-1.1.5b/   (same)
  game/vanilla-eu/   (same)
  versions/<release-id>/disc0/          extracted ISO (EBOOT.BIN stays encrypted)
  versions/<release-id>/savedata/
  versions/<release-id>/installed.json  written last: {"release": "<id>", "iso_name": "...", "date": "..."}
```

- A release counts as installed only when `versions/<id>/installed.json` exists (written after the
  extraction is complete and renamed into place). The marker lives outside `disc0`, so a developer
  folder can link `disc0` to an existing extraction without writing into it.
- `settings.ini` gains `[launcher] version = <release-id>` (last played; the game ignores the
  section). `[paths] disc0/savedata` stay in the contract for running the game without the
  launcher; the launcher passes `PSPRECOMP_DISC0` and `PSPRECOMP_SAVEDATA` for the chosen release
  as environment variables, which already take precedence over the ini (contract v1 unchanged).
- Part 1's single `game/` folder becomes `game/<release-id>/`. A part-1 portable folder (`game/`
  with an exe directly inside) is not migrated: make-portable rebuilds the folder.

## 3. `release.json` (one per exe, written by the build)

```json
{ "id": "dxd-1.1.5b", "name": "Patapon 3 DxD 1.1.5b", "disc_id": "INFN00001",
  "eboot_sha256": "<sha-256 of the encrypted PSP_GAME/SYSDIR/EBOOT.BIN>" }
```

- The launcher identifies an ISO by the SHA-256 of its encrypted `EBOOT.BIN` (read straight out of
  the ISO, before extracting) against every `game/*/release.json`. Adding a release = adding a
  folder; the launcher has no release table.
- Ids: default build -> `dxd-1.1.4`, `P3_VARIANT=v115b` -> `dxd-1.1.5b`, `P3_VARIANT=vanilla` ->
  `vanilla-eu` (mapping kept in `p3.sh`, next to the variant handling).

## 4. Game side (`psprecomp`)

1. **Per-game opt-in** (`games/patapon3/game.toml`): `[image] source = "disc"`. The default stays
   `"embedded"` (today's behaviour), so other games — Patapon 1 is a relocated PRX, which this
   loader does not handle — are untouched. Only ET_EXEC images can use `"disc"`; the recompiler
   refuses the option otherwise.
2. **Emitter** (`crates/psp-emitter/src/data.rs`): with `source = "disc"`, `data_sections.cpp` no
   longer embeds bytes. It emits the expected segment table (vaddr, memsz) and the FNV-1a-64 of the
   full image (file bytes + zero tail, the same bytes it embeds today) as `psp_expected_image`, and
   `psp_init_data_sections(rdram)` becomes a call into the loader. The generated file shrinks from
   20.9 MB to a few lines; the exe loses 3.9 MB.
3. **Loader** (`runtime/src/psp_eboot_loader.cpp`, generic, pure parts unit-tested): reads the
   boot file the generated `recomp_game_config.h` names (`RECOMP_BOOT_PATH`,
   `disc0:/PSP_GAME/SYSDIR/EBOOT.BIN`) under the disc0 folder; if it starts with `~PSP` it decrypts it in memory with the
   PPSSPP decrypter; if it is already a plain ELF (developer dumps) it uses it as is. Then it
   checks the ELF (ET_EXEC, PT_LOAD entries matching the expected table), copies each segment and
   zero-fills to memsz, and compares the FNV-1a-64 of the loaded image with `psp_expected_image`.
   Logs `[EBOOT] loaded <path> (decrypted, tag 0x...)` or the reason it refused.
4. **Mismatch / missing file / decrypt failure:** `[EBOOT] ...` line on stderr, **exit code 11**,
   before any window opens. Code 11 joins the exit-code table in `docs/contracts/settings-v1.md`
   (0 quit, 10 back to menu, 11 game files do not match this exe, other = crash).
5. **Decrypter:** `runtime/third_party/kirk/` holds `PrxDecrypter.{h,cpp}` and the libkirk C
   files copied verbatim from PPSSPP (commit recorded in a README with the licence note), plus the
   shim headers (`Common/Common.h`, `CommonTypes.h`, `Swap.h`, `Log.h`). Only `pspDecryptPRX` is
   used.
6. **Build** (`games/patapon3/scripts/p3.sh build`): writes `release.json` next to the exe (id
   from the variant mapping, `disc_id` from `PARAM.SFO`, `eboot_sha256` of the encrypted
   `EBOOT.BIN` in that build's `disc0`).
7. **Overlay mapping fix** (`p3.sh overlays`): each overlay file is assigned to its bank by the
   `MWo3` id at offset 4 (1 Azito, 2 Mission, 3 Title), and the overlay folder is found whatever
   its case (`overlay` / `Overlay`). Rebuild vanilla's overlays and exe afterwards.
8. Unchanged: boot order, `jugar.bat`, `jugar_115b.bat`, `jugar_vanilla.bat`, tours and
   `rt_regress` (their `disc0` folders already hold the encrypted `EBOOT.BIN`).
9. **Docs in the same change:** `ARCHITECTURE.md` (boot data flow: the image can come from the
   disc at boot; the `[image]` option), `README.md` (Patapon 3 needs `EBOOT.BIN` in `disc0`, which
   any extraction has), `docs/contracts/settings-v1.md` (exit code 11).

## 5. Launcher side (`DECO/launcher`)

1. **`iso.rs`:** ISO9660 reader (port of `scripts/iso_extract.py`: PVD, directory records, plain
   ISO9660, no Joliet / Rock Ridge): read one file by path, extract everything with a progress
   callback and a cancel flag.
2. **`releases.rs`:** load `game/*/release.json`; `identify(iso)` -> `Known(release)` /
   `OtherRelease { title, disc_id, version }` (from `PARAM.SFO`) / `NotPspIso` / `Io(error)`.
3. **`install.rs`:** extract to `versions/<id>/disc0.partial`, rename to `disc0`, create
   `savedata/`, write `installed.json` last. Cancel or failure deletes the `.partial` folder; a
   leftover `.partial` from a crash is deleted and redone. An existing install of the same release
   is replaced only after the new extraction succeeded. Disk-full and permission errors become the
   plain message of §1.3.
4. **Commands / UI:** `list_versions`, `identify_iso`, `install_iso` (progress events, cancel),
   `open_save_folder`, `start_game` taking the release id. Screens: FirstRun (drop zone via
   Tauri's drag-and-drop event + file picker via `tauri-plugin-dialog`), Installing, IsoError; main
   menu `Version ◂ ▸` when more than one is installed; Options › General "Add another ISO" and
   "Open the save folder". Keyboard, mouse and gamepad navigation as in part 1.
5. **Start:** runs `game/<id>/psprecomp_runtime(.exe)` with `PSPRECOMP_DISC0` /
   `PSPRECOMP_SAVEDATA` pointing at `versions/<id>/`; exit 11 -> the message of §1.7.
6. **`scripts/make-portable.mjs`:** takes one `--game <id>=<build folder>` per release and copies
   each into `game/<id>/` with its `release.json`; `--data` creates `versions/<id>/disc0` as a link
   to the developer's extraction and writes `installed.json` (never writes into the linked disc0).

## 6. Testing and success criteria

- **ctest (game):** loader with synthetic ELFs (good image; wrong hash; truncated file; not an
  ELF; wrong PT_LOAD layout); FNV of the image equals the emitter's value for a fixture. The real
  decrypt test runs only when the three `disc0` folders exist on the PC (skipped otherwise; no
  game files in the repo): each decrypts and its image hash equals its build's
  `psp_expected_image`.
- **cargo test (psprecomp):** the emitter writes the table and hash, no byte arrays.
- **cargo test (launcher):** ISO reader against an ISO9660 image built in the test (nested
  folders, file larger than one extent, read by path, extract, cancel, leftover `.partial`);
  `identify` against fake `release.json` files and a fake `PARAM.SFO`; install marker logic;
  exit code 11 mapping.
- **Vitest:** FirstRun / Installing / IsoError screens, the version selector hidden with one
  release, focus and navigation.
- **Regression:** ctest all green; 1.1.4 graphics tour `RT_REGRESS PASS` against
  `build/tour/rebuild114_t1` with the new exe (no embedded image); v115b and vanilla still play
  their opening movie (`build/tools/movie_check.py`).
- **End to end on the user's PC (port 9998):** from a fresh portable folder, each of the three
  ISOs (`DECO/INFN00001.iso`, `Downloads/INFN00001.iso`, `Downloads/Patapon 3 (Europe).iso`) goes
  through drop -> install -> Start and reaches the title screen; with two installed the selector
  switches release and each boots its own; a wrong ISO shows the message; a corrupted
  `versions/<id>/disc0/PSP_GAME/SYSDIR/EBOOT.BIN` gives the exit-11 message.
- **Part 2 is done when** the end-to-end list passes, the regression gates pass, nothing is
  pushed, and the user has tried it and approved it.

## Out of scope

Importing / converting saves (part 3); HD texture packs per release (the `texture_pack` key keeps
pointing at the DxD pack); removing an installed release from the UI (delete the folder);
reading the ISO without extracting; Linux packaging (part 7); other regions (US, JP).
