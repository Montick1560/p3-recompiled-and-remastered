# Patapon 3 community launcher — Part 3: saves (import / export PPSSPP saves) (design)

Date: 2026-10-17. Status: approved in conversation, awaiting written-spec review.
Author: Claude Opus 5.5 with the user (brainstorming). Builder for the plan: DeepSeek v4.1 flash
(`deepseek/deepseek-flash` through OpenCode, the user's own key); Claude designs, reviews and
verifies. Follows `2026-10-17-launcher-part2-design.md` (decomposition item 3).

## Goal

The player brings their PPSSPP saves and keeps playing; they can also take a save back to
PPSSPP. No external tools, no keys, nothing ever deleted.

### Decisions taken (conversation 2026-10-17)

- **"Several saves" = the game's own slots.** One `savedata` folder per release (part 2); the
  launcher shows, imports into and exports from the game's slots. No launcher profiles.
- **The runtime decrypts encrypted saves when the game loads them**, exactly like PPSSPP, with
  the key the game itself passes in the savedata request. The launcher has no cryptography and no
  keys: importing is copying (plus a rename for US saves). Writing stays plain, as today.
- **US saves (UCUS98751) are imported into the European / DxD releases** by renaming them; the
  end-to-end test decides whether they really load (otherwise they are listed as not compatible).
- **Two-way:** "Export to PPSSPP" copies a slot into PPSSPP's save folder (PPSSPP reads plain
  saves).

### Facts verified before the design (2026-10-17)

- The port writes plain saves: `SECURE.BIN` in clear, `SAVEDATA_PARAMS` all zero and the
  `SAVEDATA_FILE_LIST` hash zeroed (`runtime/src/hle/psp_savedata.cpp`, `save()`). It cannot read
  encrypted ones (`DECO/savedata/UCES01421_DATA01` once held an encrypted PPSSPP save the port
  could not read).
- PPSSPP decides by the first byte of `SAVEDATA_PARAMS` (`SavedataParam::GetSaveCryptMode`):
  `0x00` plain, `0x01` mode 1, `0x21` mode 3, `0x41` mode 5; a plain save is loaded with
  `LoadNotCryptedSave`, so PPSSPP reads the port's saves as they are.
- All the user's PPSSPP Patapon 3 saves (`E:\Respaldo\PPSSPP\memstick\PSP\SAVEDATA`,
  `UCES01421_DATA00..10`, `UCUS98751_DATA00..06`) are mode 5 (`0x41`).
- With the EU key (`4439565a473232594c57534a474e4c9a`) the US save `UCUS98751_DATA02` decrypts
  with a matching MAC, 178,864 -> 178,848 bytes, the same size as the EU saves: both regions use
  the same key and the same save size. The user's EU `DATA04` MAC does not match its SFO hash;
  decrypting without the check (PPSSPP's retry) gives the save the port already plays
  (`build/save_main/dec_DATA04.bin`, verified in game 2026-10-16).
- The EBOOT names its saves `_DATA00` (system data, "Datos de sistema"), `_DATA01`..`_DATA10`
  (the 10 game slots), `_DATA50` and `_DATA99` (other data). Every Patapon 3 release uses the
  game name `UCES01421` for EU / DxD saves (DxD's disc id is `INFN00001`, its saves are not).
- The game loads saves with `LISTLOAD` (the system's list dialog, `saveNameList`) and reads the
  system data with `AUTOLOAD _DATA00`; `SIZES` / `FILES` are also used.
- A local throwaway tool already decrypts one save with PPSSPP's code
  (`DECO/build/tools/p3save/`: `chnnlsv_core.cpp`, `decrypt_data.inc`, libkirk objects); the
  runtime vendors libkirk since part 2 (`runtime/third_party/kirk/`).

## 1. What the player sees

1. **Main menu:** Start · **Saves** · Options · Exit (Version ◂ ▸ as in part 2).
2. **Saves** (for the selected release), styled like the game's LOAD screen: one row per slot
   `DATA01`..`DATA10` with `ICON0.PNG`, `SAVEDATA_TITLE` and the `SAVEDATA_DETAIL` lines (Hero,
   Class Lv, Play time, Last mission); empty slots show "Empty". "System data" (`_DATA00`) and
   other data (`_DATA50`, `_DATA99`) are a separate small row. Actions: **Import from PPSSPP**,
   **Export to PPSSPP** (on the focused slot), **Open the save folder** (moves here from Options ›
   General).
3. **Import from PPSSPP:**
   - The launcher looks for PPSSPP's `PSP/SAVEDATA` by itself (see §3.3). If nothing is found, or
     the player wants another one, "Choose the PPSSPP folder" (PPSSPP's own folder, `memstick`,
     `PSP` or `SAVEDATA` all work); the choice is remembered.
   - It lists the Patapon 3 saves found there with the same rows. EU saves are plain entries; US
     saves carry the tag "USA → converted". Saves of other games are not shown.
   - Picking one imports it into the slot with the same number. If that slot is taken:
     "Replace (a copy is kept)" or "Use the free slot DATAnn" (first free one; if none, only
     Replace). System data asks the same "Replace?" question.
   - Done: "Imported into DATAnn." The row appears in the slot list.
4. **Export to PPSSPP:** on a slot, copies it to PPSSPP's `SAVEDATA` as `<save id>_DATAnn`. If
   PPSSPP already has that folder: "PPSSPP already has a save in DATAnn. Replace it (a copy is
   kept)?".
5. **Nothing is ever deleted.** Whatever is replaced (in the port or in PPSSPP) is first moved to
   `backups/<YYYYMMDD-HHMMSS>/<side>/<folder>` inside the portable folder (`side` = `port-<id>` or
   `ppsspp`). Nothing is ever written inside PPSSPP's folder except the exported save itself.
6. **Errors** in plain words: PPSSPP folder not found, folder not readable, disk full, a save
   folder without `PARAM.SFO` (listed as "damaged", not importable).

All strings in `lang/es.json` and `lang/en.json`.

## 2. Game side (`psprecomp`)

1. **Encrypted saves on load** (`runtime/src/hle/psp_savedata.cpp`, `load()` and the secure
   read paths): the crypt mode comes from the save's `PARAM.SFO` (`SAVEDATA_PARAMS[0]`, the same
   table as PPSSPP's `GetSaveCryptMode`). Mode 0 -> today's plain path. Otherwise the request must
   be secure and carry a key when the mode needs one (> 1); then `DecryptData(mode, ..., key,
   expected hash from SAVEDATA_FILE_LIST)`; on failure retry with the mode the request implies
   (PPSSPP's `DetermineCryptMode`, using the runtime's `opts_.sdk_version`); on a
   hash mismatch retry without the hash. Success logs
   `[SAVEDATA] decrypted <dir>/<file> mode <n> (<in> -> <out> bytes)`; a failure logs the reason
   and returns `SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN` (0x80110306) instead of handing the
   game ciphertext.
2. **Sizes:** the list / `FILES` / `SIZES` answers report the data size the game will receive
   (encrypted file size minus 16 bytes when the save is encrypted), like PPSSPP.
3. **Saving over an encrypted save** keeps today's behaviour: plain `SECURE.BIN`, zeroed
   `SAVEDATA_PARAMS` and file-list hash. Nothing else in the savedata module changes.
4. **Crypto:** `runtime/third_party/kirk/` gains PPSSPP's savedata cipher: the
   `sceSdMac*` / `sceSdCipher*` core of `Core/HLE/sceChnnlsv.cpp` (with `sceChnnlsv.h`) and
   `SavedataParam::DecryptData` / `EncryptData` / `BuildHash` as free functions in a small
   `psp_save_crypt.{h,cpp}` wrapper, copied from PPSSPP commit `0665ea0` with the licence note in
   the README (GPL-2.0-or-later). `EncryptData` is used only by the tests (round trip). The
   runtime links `kirk` already.
5. **`release.json`** (written by `p3.sh build`) gains the save facts, so the launcher has no
   Patapon 3 knowledge:
   ```json
   "saves": { "id": "UCES01421", "import_from": ["UCUS98751"],
              "system": "_DATA00", "slot_prefix": "_DATA", "first_slot": 1, "last_slot": 10 }
   ```
   Same values for the three releases (kept next to the variant mapping in `p3.sh`). A
   `release.json` without `saves` hides the Saves entry for that release.
6. **Docs in the same change:** `ARCHITECTURE.md` (savedata: encrypted saves decrypted on load),
   `README.md` (PPSSPP saves load as they are), `runtime/third_party/kirk/README.md`.

## 3. Launcher side (`DECO/launcher`)

1. **`sfo.rs`** (exists, read-only today): add typed access (`SAVEDATA_TITLE`, `SAVEDATA_DETAIL`,
   `SAVEDATA_DIRECTORY`, `TITLE`, `SAVEDATA_PARAMS` first byte) and an in-place string update that
   rewrites `SAVEDATA_DIRECTORY` keeping every other byte (the field has a fixed max length, so the
   file size does not change).
2. **`saves.rs`:**
   - `list(dir, saves_cfg, ids)` -> entries `{ folder, slot, kind: slot|system|other, title,
     detail, icon_path, region: same|converted, encrypted, damaged }` for every
     `<id><suffix>` folder of the given ids;
   - `import(src_entry, port_savedata, target_slot, backups)`: copy to
     `<target>.partial`, rewrite `SAVEDATA_DIRECTORY` (and the folder name) when the id or slot
     changes, move a replaced folder to backups, rename `.partial` into place. Encrypted saves are
     copied encrypted (the game decrypts them, §2.1);
   - `export(slot_entry, ppsspp_savedata, backups)`: same steps towards PPSSPP, always named with
     the release's `saves.id`;
   - `free_slot(...)`, backup folder naming, leftover `.partial` cleanup.
3. **PPSSPP folder detection** (`ppsspp.rs`): candidates in order — the remembered
   `[launcher] ppsspp_savedata` from `settings.ini`; Windows `Documents\PPSSPP\PSP\SAVEDATA`
   (installed PPSSPP's default memstick); Linux `~/.config/ppsspp/PSP/SAVEDATA`; then nothing.
   A chosen folder is normalised: `X/SAVEDATA`, `X/PSP/SAVEDATA`, `X/memstick/PSP/SAVEDATA`, the
   first that exists. The game ignores the `[launcher]` section (contract v1 unchanged).
4. **Commands:** `list_saves(id)`, `ppsspp_folder()`, `choose_ppsspp_folder(path)`,
   `list_ppsspp_saves(id)`, `import_save(id, folder, target_slot)`, `export_save(id, slot,
   replace)`, `open_save_folder(id)` (exists). Icons are served to the UI as data URLs (no asset
   scope widening).
5. **UI:** `screens/Saves.tsx` (slot list + actions), `screens/ImportSaves.tsx` (PPSSPP list,
   conflict question), reusing the part 1 focus manager; keyboard, mouse and gamepad as
   everywhere; Options › General loses "Open the save folder".

## 4. Testing and success criteria

- **ctest (game):** round trip — `EncryptData` mode 5 with a test key, write a save folder with
  `SAVEDATA_PARAMS = 0x41`, the load path returns the original bytes; wrong MAC in the file list
  -> still loads (retry without hash); wrong key -> `LOAD_DATA_BROKEN`, never ciphertext; plain
  saves unchanged; sizes reported for an encrypted save. Real-save test (skipped when the files
  are absent, no game files in the repo): `build/save_main/original_DATA04` decrypts to bytes equal
  to `build/save_main/dec_DATA04.bin`.
- **cargo test (launcher):** SFO read / in-place directory rewrite (other bytes identical);
  listing (slots, system, other, damaged, US tagged); import into the same / a free slot /
  replace with backup; export with and without an existing PPSSPP folder; `.partial` cleanup;
  PPSSPP folder normalisation in its four shapes; a release without `saves`.
- **Vitest:** Saves list with empty and full slots, import conflict question, export confirm,
  Saves entry hidden without `saves`, focus / navigation.
- **Regression:** ctest all green; 1.1.4 graphics tour `RT_REGRESS PASS` against
  `build/tour/rebuild114_t1`; the user's `jugar.bat` save (`DECO/savedata`, plain) untouched and
  still loading.
- **End to end on the user's PC (port 9998, fresh portable folder, PPSSPP folder chosen as
  `E:\Respaldo\PPSSPP` — read only, nothing exported there):** import the **encrypted**
  `UCES01421_DATA04` -> Start -> Español EU -> Continue -> the hideout shows Edson Yumiyacha Nv 35;
  import `UCUS98751_DATA02` (MoMo, Tondenga Lv 19) into a free slot -> it loads (if the game
  rejects it, US saves become "not compatible" in the list and the spec records it); export a
  slot to a scratch PPSSPP (a copy of `build/ppsspp_mp` in `build/e2e_ppsspp`, its own
  `memstick`) -> the exported save loads there; replacing creates the backup.
- **Part 3 is done when** the end-to-end list passes, the regression gates pass, nothing is
  pushed, and the user has tried it and approved it.

## Out of scope

Launcher profiles; cloud sync; exporting for a real PSP (would need re-encrypting); other regions
(JP `UCJS10089`); editing save contents; deleting saves from the UI (backups are folders the
player can delete); per-release HD texture packs.
