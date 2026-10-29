# Beta extension (u5) + public beta with launcher auto-update (u6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep beta 1 playable until **Sunday 1 Nov 2026 23:59 CDMX** without a gap tonight (u5), then open it to
everyone without invite codes and ship a launcher that installs future packages by itself (u6, the last manual
download).

**Architecture:** The end date lives in three places: the server's `beta_config.json` (read live), the launcher
(`beta.rs` `ENDS`, compiled in) and the three game exes (`PSPRECOMP_BETA_UNTIL`, compiled in). u4 dies at
2026-10-26T05:59:59Z whatever the server says, so **Part A** rebuilds all three with the new date and nothing else.
**Part B** adds a `public` switch to the server's beta gate, drops the code requirement from the launcher, and adds
an updater: the launcher reads a signed `latest.json` from R2 (`descargas.montick.app`), downloads the package zip,
checks SHA-256 + ed25519 signature, stages it, swaps files with rollback (renaming its own running exe), restarts.

**Status (25 Oct 19:50 CDMX):** the user asked for the public beta **tonight**, plus a nickname prompt (after the ISO
install, and at Start while the nickname is "Patapon"). Part A, Task B1 (server public mode) and Task B2 (launcher
without codes) shipped together as u5: psprecomp 563ac5b + 792e9c1, launcher 9930977 + 1a1406e (0.2.0-beta.6),
`build/beta/ACTUALIZACION_5.md`. u6 = Tasks B3–B6 only (the updater).

**Tech Stack:** C++ runtime (CMake/ninja), Python 3 server (`tools/infra_server.py`, `tools/beta_report_server.py`),
Tauri 2 launcher (Rust + React/TS, vitest), Cloudflare R2 + custom domain.

**Spec:** this document (decisions taken in chat 25 Oct 2026 with the user: "u5 rápido hoy" + "sin códigos");
background `docs/superpowers/specs/2026-10-23-p3-beta-1-design.md`, `docs/superpowers/plans/2026-10-23-p3-beta-1.md`.

## Global Constraints

- New end: `2026-11-02T05:59:59Z` = Sun 1 Nov 2026 23:59:59 CDMX (CDMX is UTC−6 all year; Mexico dropped DST in 2022).
- Start of the window unchanged: `2026-10-24T03:00:00Z`.
- Branches: psprecomp local `build-2026-10-25`; launcher local `beta-1`. **No push** without the user's OK (next psprecomp
  push branch `push-p3-33`; launcher `beta-1` -> `origin/main`).
- Commit author "Patapon Community"; no Claude co-author trailer (user rule, overrides the harness attribution).
- Never print or commit invite/staff codes, the update signing key, or anything from `beta.db`.
- Players' launcher is built **without** `P3_NO_BETA_GATE`, and **after** `cargo test` (cargo test rebuilds the exe).
- Packages never contain `versions/`, `settings.ini`, `logs/` (`build/beta/make_zip.py` refuses them).
- Versions: u5 = launcher `0.2.0-beta.6`; u6 = `0.3.0-beta.1`. The launcher version **is** the package version from u6 on
  (every package bumps it, even when only game exes change).
- Player-facing text in Spanish; code, commits, plans in English.
- Tester machine (`tester`) runs `infra_server.py` + `beta_report_server.py`; the user copies files there and restarts
  services. `beta_config.json` is reloaded live (mtime), no restart needed.

## Review Focus

1. **Game (or a second launcher) still running while an update applies** → a file is locked; expected: rollback,
   every file as before, message "Cierra el juego y vuelve a intentarlo". Test in Task B3 (`apply_rolls_back_on_locked_file`).
2. **Offline, R2 down, or Cloudflare serving a stale `latest.json`** → launcher starts and plays exactly as without the
   updater, no error popup; stale = no offer. Tests in B3 (`check_offline_is_none`) and the `?t=` cache-buster in B3.
3. **Install folder not writable** (Program Files, OneDrive "solo en línea", read-only) → clear message with the manual
   download link; nothing half-written. Test in B3 (`apply_fails_cleanly_when_root_read_only`).
4. **Antivirus removes/blocks the new `launcher.exe` right after the swap** → the next start must not leave the player
   with no launcher: apply verifies the new exe exists with the expected size before deleting anything, else rolls
   back; `cleanup_after_update` restores `launcher.old.exe` when `launcher.exe` is missing. Test in B3.
5. **Older or equal version in `latest.json`, or a manifest signed with another key** → never installed (no downgrade,
   no unsigned code on players' PCs). Tests in B3 (`older_version_not_offered`, `bad_signature_rejected`).

---

# Part A — u5 tonight (date only)

Deadline: zip on R2 well before 23:59 tonight. After 23:59 u4 stops starting, but downloading u5 fixes it at any time,
so a late player is not lost — they just need the new zip.

### Task A1: Runtime message without a date

**Files:**
- Modify: `psprecomp/runtime/src/psp_beta.cpp` (`psp_beta_check_at_start`, the `SDL_ShowSimpleMessageBox` text)

- [ ] **Step 1:** Replace the text with a date-free one so future extensions need no text edit:
  `"La beta de Patapon 3 termin\xC3\xB3. Descarga la versi\xC3\xB3n nueva en https://beta.montick.app"`.
- [ ] **Step 2:** `ninja -C build/rtw psprecomp_runtime` and `ctest --test-dir build/rtw -R beta` → PASS
  (tests keep `2026-10-26T05:59:59Z`: they only test parsing/expiry, do not touch them).
- [ ] **Step 3:** Commit on `build-2026-10-25`: `fix(beta): ended message points to the download page, no fixed date`.

### Task A2: Launcher 0.2.0-beta.6 with the new end

**Files:**
- Modify: `launcher/src-tauri/src/beta.rs:8` (`ENDS`, comment `// Sun 1 Nov 23:59:59 CDMX`), test const `AFTER`
  (`beta.rs` tests) -> `"2026-11-02T06:00:00Z"`
- Modify: version `0.2.0-beta.5` -> `0.2.0-beta.6` in `launcher/src-tauri/Cargo.toml`, `launcher/src-tauri/tauri.conf.json`,
  `launcher/package.json` (Cargo.lock follows on build)

- [ ] **Step 1:** Edit the constants; grep the launcher for other `2026-10-26` uses (`launch.rs`, screens, i18n) and
  update any player-facing date text to "domingo 1 de noviembre a las 23:59".
- [ ] **Step 2:** `cargo test` (src-tauri) and `npm test -- --run` → all PASS.
- [ ] **Step 3:** Commit on `beta-1`: `chore(launcher): 0.2.0-beta.6 (beta 1 update 5): beta runs until Sun 1 Nov 23:59 CDMX`.
- [ ] **Step 4:** Players' build, last: `npm run tauri build -- --no-bundle` (no `P3_NO_BETA_GATE` in the env).

### Task A3: Beta exes, package, smoke

**Files:**
- Modify: `build/beta/make_beta_exes.sh:7` `UNTIL=2026-11-02T05:59:59Z`
- Create: `build/beta/patapon3-beta1-u5.zip` (+ `.sha256`), `build/beta/ACTUALIZACION_5.md`, CHANGELOG entry

- [ ] **Step 1:** Run `sh build/beta/make_beta_exes.sh` (run `build/infra/diskquiet.py` first). Expected per tree:
  `beta exe: 1 match(es) of 2026-11-02T05:59:59Z`, then `plain exe back: 0 match(es)`, `ALL DONE`.
- [ ] **Step 2:** Assemble `build/beta/Patapon3Recomp` as for u4 (`PAQUETE.md`): new `launcher.exe`, `game/<id>/` from
  `build/beta/game/`, exe renamed `Patapon 3 DxD Remastered.exe`, `lang/ skins/ themes/ SDL2.dll` unchanged.
- [ ] **Step 3:** `python -I build/beta/make_zip.py build/beta/Patapon3Recomp build/beta/patapon3-beta1-u5.zip` → prints
  files/bytes/sha256, no "refusing".
- [ ] **Step 4:** Smoke: unzip to `E:\p3_beta1u5_smoke`; launcher stays open 10 s and shows `0.2.0-beta.6`; each game exe
  logs `[RT] beta build: plays until 2026-11-02T05:59:59Z` and `debug socket off`; in a copy with the ISO installed,
  Start launches the game (window titled "Patapon 3 DxD Remastered").
- [ ] **Step 5:** Write `ACTUALIZACION_5.md` (same layout as u4: zip, size, SHA-256, link
  `https://descargas.montick.app/patapon3-beta1-u5.zip`, how to update, text to publish: "la beta se alarga hasta el
  domingo 1 de noviembre a las 23:59 (CDMX); si después de las 23:59 de hoy el juego dice que la beta terminó, baja
  esta actualización").

### Task A4: Server window and sign-up page

**Files:**
- Modify (tester + local copy): `build/beta/beta_config.json` `"ends": "2026-11-02T05:59:59Z"`
- Modify: `psprecomp/tools/beta_report_server.py:317-318` (sign-up text: "del viernes 23 de octubre a las 21:00 al
  domingo 1 de noviembre a las 23:59 (hora CDMX)"), `psprecomp/tools/beta_cola.py:32` (`ENDS`, comment)

- [ ] **Step 1:** `python -I tools/beta_report_server.py --selftest` and `python -I tools/beta_cola.py --selftest` → PASS.
- [ ] **Step 2:** Commit on `build-2026-10-25`: `chore(beta): window runs until Sun 1 Nov 23:59 CDMX`.
- [ ] **Step 3 (user, on `tester`):** edit `beta_config.json` `ends` (live reload: the server log says
  `beta config reloaded`), copy `beta_report_server.py`, restart `reportes_beta.bat`.
- [ ] **Step 4:** Verify from here: `GET https://online.montick.app/beta` → `"open": true, "ends": "2026-11-02T05:59:59Z"`;
  `https://beta.montick.app` shows the new date.
- [ ] **Step 5 (user):** upload `patapon3-beta1-u5.zip` to R2 `patapon-beta`, check the link downloads, publish the text.

---

# Part B — u6: public beta + auto-update

### Task B1: Server `public` mode

**Files:**
- Modify: `psprecomp/tools/infra_server.py` (`load_beta`, `beta_status`, `beta_login`, `http_check`, `beta_selftest`)
- Modify: `psprecomp/tools/beta_report_server.py` (sign-up page: open to everyone, download link instead of the queue
  form when `public`)

**Interfaces:**
- Produces: `beta_config.json` key `"public": bool` (default `false`); `GET /beta` adds `"public": <bool>`.

- [ ] **Step 1: Failing selftest cases** in `beta_selftest`: with `"public": true` and the window open, a login with
  no `invite` → accepted, session non-staff, label `"público"`; a login with an unknown code → accepted as public;
  a known staff code → still staff; window closed → `BETA_CLOSED` for public logins; `max_per_addr` still enforced;
  `GET /beta` → `"public": true`; with `"public": false` every existing case unchanged.
- [ ] **Step 2:** `python -I tools/infra_server.py --selftest` → the new cases FAIL.
- [ ] **Step 3:** Implement: `load_beta` reads `public`; in `beta_login`, when `public` and the code is missing/unknown,
  skip the code checks (keep `min_proto`, window, `max_per_addr`); per-code limit only for known codes.
- [ ] **Step 4:** Selftests of both servers PASS. Commit `feat(beta): public mode, no invite needed`.

### Task B2: Launcher gate without codes

**Files:**
- Modify: `launcher/src-tauri/src/beta.rs` (`Status` + `public: Option<bool>`; `gate`/`start_gate`), `lib.rs`
  (`beta_gate`), `launcher/src/App.tsx` (no Invite screen in the Start flow), `launcher/src/screens/Start.test.tsx`

**Interfaces:**
- Produces: `gate(now_unix, server, has_code) -> Gate` keeps its signature; `Gate::NoCode` is no longer returned when
  the server says `public` or is unreachable inside the built-in window.

- [ ] **Step 1: Failing tests:** `public_server_allows_without_code` (server open+public, no code → `Allowed`);
  `offline_inside_window_allows_without_code`; `public_closed_is_ended`; `past_builtin_end_is_ended_even_if_public`;
  Start.test: Start with no saved code runs the game, Invite screen never shown.
- [ ] **Step 2:** `cargo test beta` / `npm test -- --run Start` → FAIL. **Step 3:** implement (a saved staff code is still
  passed to the game as before). **Step 4:** PASS. **Step 5:** commit `feat(launcher): public beta, no invite code`.

> **Superseded (29 Oct 2026):** Tasks B3–B6 were replaced by `2026-10-29-launcher-autoupdater.md` (signed
> manifest with mandatory updates), executed on launcher branch `updater`.

### Task B3: Updater core (Rust)

**Files:**
- Create: `launcher/src-tauri/src/update.rs` — manifest, version compare, signature (pure, no I/O except HTTP trait)
- Create: `launcher/src-tauri/src/update_apply.rs` — download, stage, apply/rollback, cleanup (filesystem)
- Modify: `launcher/src-tauri/Cargo.toml`: `ed25519-dalek = "2"`, `zip = { version = "2", default-features = false, features = ["deflate"] }`,
  `semver = "1"`, `hex = "0.4"`

**Interfaces:**
- Produces (`update.rs`):
  - `pub const MANIFEST_URL: &str = "https://descargas.montick.app/update/latest.json";` overridable by env
    `P3_UPDATE_URL` (tests/e2e; the signature check still applies).
  - `pub const PUBKEY_HEX: &str` — from compile-time env `P3_UPDATE_PUBKEY` when set, else the production key (Task B5).
  - `#[derive(Serialize, Deserialize)] pub struct ManifestBody { version: String, url: String, sha256: String, size: u64, required: bool, notes: String }`
    (field order fixed: the signature covers `serde_json::to_vec(&body)`).
  - `pub struct Manifest { body: ManifestBody, sig: String /* hex, 64 bytes */ }` — JSON `{ ...body fields, "sig": "..." }`.
  - `pub fn verify(m: &Manifest, pubkey_hex: &str) -> Result<(), String>`
  - `pub fn is_newer(remote: &str, local: &str) -> bool` (semver, pre-release aware: `0.3.0-beta.2 > 0.3.0-beta.1`, `0.3.0 > 0.3.0-beta.9`)
  - `pub fn check(http: &dyn BetaHttp, local_version: &str, now_unix: i64) -> Option<ManifestBody>` — GET
    `MANIFEST_URL?t=<now_unix>` (Cloudflare cache-buster), parse, verify, newer → Some; any failure → None.
- Produces (`update_apply.rs`):
  - `pub fn download(url: &str, expect_sha256: &str, expect_size: u64, dest: &Path, progress: &dyn Fn(u64, u64)) -> Result<(), String>`
  - `pub fn stage(zip: &Path, staging: &Path) -> Result<Vec<PathBuf>, String>` — strips `Patapon3Recomp/`, rejects
    absolute paths, `..`, and protected tops.
  - `pub const PROTECTED: &[&str] = &["versions", "settings.ini", "logs", "savedata", "textures", "updates"];`
  - `pub fn apply(root: &Path, staging: &Path, files: &[PathBuf]) -> Result<(), String>` — for each file: rename the
    old one to `<name>.old` (works on the running `launcher.exe`), move the new one in; on any error undo every step
    in reverse; after the loop verify `launcher.exe` exists with the staged size; only then delete the `.old` files
    it can (the running `launcher.old.exe` stays).
  - `pub fn cleanup_after_update(root: &Path)` — on start: if `launcher.exe` is missing and `launcher.old.exe` exists,
    rename it back; else delete `launcher.old.exe` and `updates/` (ignore "in use", retry next start).
  - Work dir: `<root>/updates/` (`package.zip`, `staging/`).

- [ ] **Step 1: Failing tests** (`tempfile`, a test keypair generated in the test):
  `verify_accepts_good_signature`, `bad_signature_rejected` (other key; one notes byte changed),
  `older_version_not_offered` (equal and older → None), `prerelease_order`, `check_offline_is_none`
  (HTTP error / garbage JSON → None), `check_url_has_cache_buster`, `download_rejects_wrong_sha_or_size`,
  `stage_rejects_zip_slip_and_protected` (`../x`, `C:/x`, `Patapon3Recomp/versions/a`, `settings.ini` → Err),
  `apply_replaces_files_and_keeps_protected` (versions/, settings.ini, savedata untouched byte for byte),
  `apply_rolls_back_on_locked_file` (a file opened without share-delete → Err, every original file back),
  `apply_fails_cleanly_when_root_read_only`, `cleanup_restores_missing_launcher`, `cleanup_removes_old_and_updates`.
- [ ] **Step 2:** `cargo test update` → FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(launcher): updater core: signed manifest, staged apply with rollback`.

### Task B4: Update UI

**Files:**
- Modify: `launcher/src-tauri/src/lib.rs` — commands `check_update() -> Option<UpdateInfo>`,
  `install_update() -> Result<(), String>` (emits `update-progress` `{done, total}`), `restart_launcher()`;
  `cleanup_after_update(root)` first thing in `run()`
- Create: `launcher/src/screens/Update.tsx` + `Update.test.tsx`; Modify `launcher/src/api.ts`, `launcher/src/App.tsx`

**Interfaces:**
- Consumes: B3 `check`, `download`, `stage`, `apply`. `UpdateInfo = { version: string, notes: string, required: boolean, size: number }`.

- [ ] **Step 1: Failing tests** (`Update.test.tsx`): banner "Actualización {version} disponible" with **Actualizar** /
  **Después** reachable by pad and keyboard (useNav); `required` → no "Después" and Start disabled with text
  "Esta actualización es necesaria para jugar"; progress bar during install; on error the message plus
  "Descárgala a mano en https://beta.montick.app" and Start works again; no banner when `check_update` is null.
- [ ] **Step 2:** FAIL. **Step 3:** implement; the check runs once at launcher start in the background, never while the
  game runs; `install_update` refuses while the game process is alive. **Step 4:** `npm test -- --run`, `cargo test` PASS.
- [ ] **Step 5:** commit `feat(launcher): update banner, progress and restart`.

### Task B5: Signing tool + key

**Files:**
- Create: `launcher/src-tauri/src/bin/p3-sign-update.rs`; Modify `Cargo.toml` (`default-run = "p3-launcher"`)
- Create (outside every repo): `build/beta/claves/update_ed25519.key` — **never committed or printed**

**Interfaces:**
- `p3-sign-update keygen <secret_out>` → writes the secret, prints the public key hex (goes into `PUBKEY_HEX`).
- `p3-sign-update sign <zip> --version V --url U --notes FILE [--required] --key <secret> --out latest.json` →
  computes sha256 + size, signs `ManifestBody` with the same serialization as `update.rs` (share the struct).

- [ ] **Step 1: Failing test:** `signed_manifest_verifies_with_update_rs` (keygen in tempdir → sign → `update::verify` OK).
- [ ] **Step 2–4:** implement, PASS. Generate the real key; put its public hex in `PUBKEY_HEX`.
- [ ] **Step 5:** commit `feat(launcher): p3-sign-update tool, production update key`. Tell the user to keep a backup of
  `update_ed25519.key` (losing it means one more manual download for everyone).

### Task B6: u6 package + end-to-end update test

**Files:**
- Create: `build/beta/patapon3-beta1-u6.zip`, `build/beta/ACTUALIZACION_6.md`, `build/beta/PUBLICAR_ACTUALIZACION.md`

- [ ] **Step 1:** Version `0.3.0-beta.1`; players' launcher build (Task A2 Step 4 rules); package + zip as in A3 (game
  exes unchanged from u5 unless something else landed).
- [ ] **Step 2: E2E happy path:** u6 in `E:\p3_upd_e2e` with the ISO installed, a save and `settings.ini`; record hashes of
  `versions/`, `settings.ini`, savedata. Build a fake `0.3.0-beta.2` package (launcher with a visible version change),
  sign it, serve with `python -m http.server` from a scratch dir; start the u6 launcher with
  `P3_UPDATE_URL=http://127.0.0.1:<port>/latest.json`. Expected: banner → Actualizar → progress → restart → corner shows
  `0.3.0-beta.2`; protected hashes identical; next start removes `launcher.old.exe` and `updates/`.
- [ ] **Step 3: E2E failures:** truncated zip → error message, launcher still `0.3.0-beta.1` and works; game running →
  refused with the "Cierra el juego" text; server down → no banner, Start works.
- [ ] **Step 4:** `PUBLICAR_ACTUALIZACION.md` for the user: build → `p3-sign-update sign` → upload the zip **first**,
  then `update/latest.json` with `Cache-Control: no-cache` → check the URL in a browser. First publish: `latest.json`
  points at u6 itself (players on u6 see no offer).
- [ ] **Step 5:** Server go-live (user, on `tester`): `"public": true` in `beta_config.json`, copy the B1 server files,
  restart both services; verify `GET /beta` → `"public": true`; upload u6 + `latest.json`; publish `ACTUALIZACION_6.md`
  text ("abierta a todos; esta es la última vez que descargas a mano").
