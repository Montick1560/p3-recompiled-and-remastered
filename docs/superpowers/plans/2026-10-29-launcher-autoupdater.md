# Launcher Auto-Updater with Mandatory Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** From update 6 on, the launcher installs every new package by itself, and an update marked mandatory blocks **Iniciar** until it is installed, so the public beta never again depends on players downloading a zip by hand.

**Architecture:** A signed manifest `latest.json` on R2 (`descargas.montick.app/update/latest.json`) names the newest package (`version`, zip `url`, `sha256`, `size`, `notes`) and the oldest version still allowed to play (`min_version`). The launcher checks it in the background at start and again when Iniciar is pressed. `local < min_version` → mandatory: only **Actualizar** is offered and Start stays blocked. `min_version ≤ local < version` → optional banner (Actualizar / Después). The download is checked against size + SHA-256 and the manifest against an ed25519 signature. The zip is staged in `updates/`, and files are swapped one by one through a journal that a crash or power cut rolls back on the next start (the running `launcher.exe` is renamed, which Windows allows). Then the launcher restarts. We write our own updater instead of `tauri-plugin-updater` because that plugin only updates NSIS/MSI installs, and the beta ships as a portable zip (`tauri build --no-bundle`).

**Tech Stack:** Tauri 2 launcher (Rust `src-tauri`, React + vitest `src`), `ureq` + `sha2` (already dependencies), new crates `ed25519-dalek = "2"`, `zip = { version = "2", default-features = false, features = ["deflate"] }`, `semver = "1"`; psprecomp runtime only for the beta exes' end date; Python `http.server` for the end-to-end test.

**Spec:** none written. Sources: the user's request on 2026-10-29 ("ya prácticamente es beta pública, así que requeriremos autoupdater con actualizaciones obligatorias, reescribe el plan pero aún no ejecutes"); it replaces Tasks B3–B6 of `2026-10-25-p3-beta-extension-and-autoupdater.md` (its Part A, B1 and B2 shipped as u5). Rulings made without a spec are provisional.

## Decisions for the user (before executing)

1. **New built-in end date.** Today the u5 exes and launcher stop at **Sun 1 Nov 23:59 CDMX** (`2026-11-02T05:59:59Z`) no matter what. With mandatory updates that date becomes only a safety net for copies that never go online. Proposal: **Thu 31 Dec 2026 23:59 CDMX** (`2027-01-01T05:59:59Z`), moved forward by later updates. Task 5 uses whatever the user picks.
2. **u6 must be published before Sun 1 Nov 23:59 CDMX.** u5 has no updater, so u6 is still one last manual download. If it goes up after the u5 deadline, players are locked out until they download it.
3. **The signing key** (`build/beta/claves/update_ed25519.key`, outside every repo) needs a backup copy kept by the user. Losing it costs one more manual download for every player.

## Global Constraints

- No push. Launcher commits on a new local branch **`updater` from `auto-scale`** (so u6 also carries the automatic resolution). psprecomp beta exes are built from **`ge-batching`** (contains u5's `build-2026-10-25` + batching + CPU cuts). Author from repo config, no Co-Authored-By, `Verified:` / `Agent:` lines.
- The updater never deletes or overwrites player data. Protected tops: `versions/` (ISO install + saves), `settings.ini`, `logs/`, `textures/`, `updates/`, `psprecomp_adhoc_mac.txt`. It only writes files present in the package and never removes a file the package does not name.
- Every network failure means "no update info": the launcher keeps working and Start is allowed. Only a verified manifest can block Start.
- Player-facing text in Spanish (`lang/es.json`) and English (`lang/en.json`), through `t(strings, key)` like the rest of the UI.
- `make_beta_exes.sh` reconfigures the play trees (`build/rtw`, `build/v115b/rtw`, `build/vanilla/rtw`) and puts them back. Run it only while the user's game is closed. Run `build/infra/diskquiet.py` first.
- Tests on this PC: `cargo test` (launcher `src-tauri`, today 142 + 4 ignored) and `npm test -- --run` (today 99), both all PASS after every task. End-to-end runs go in `E:\p3_upd_e2e` (E: has ~78 GB free).
- Add the player-visible lines to `build/beta/CHANGELOG.md` → "Actualización 6".

## Review Focus

1. **Crash or power cut during the file swap** must leave a launcher that starts and a consistent package. A journal `updates/apply.json` lists every renamed file before the swap starts; on start, an unfinished journal is rolled back. Pinned by `cleanup_rolls_back_unfinished_apply` (Task 2).
2. **Antivirus or the game holding a file** (common on Windows right after a download) → the swap fails, everything is put back, the player gets a message, and the next start retries. Pinned by `apply_rolls_back_on_locked_file` (Task 2) and the "game running" refusal (Task 4).
3. **No internet / R2 down / Cloudflare serving a stale manifest** → never blocks Start; a stale or replayed signed manifest can never downgrade (`is_newer` + `min_version ≤ version` check). Pinned by `offline_never_blocks`, `older_or_equal_not_offered`, `replayed_old_manifest_ignored` (Task 1).
4. **A package in a read-only place** (e.g. extracted under `C:\Program Files`) → clean failure with "Mueve la carpeta Patapon3Recomp a Documentos y vuelve a intentarlo", nothing half-written. Pinned by `apply_fails_cleanly_when_root_read_only` (Task 2).
5. **A malicious or broken zip** (`..`, absolute paths, entries under protected tops) is rejected before anything is extracted. Pinned by `stage_rejects_zip_slip_and_protected` (Task 2).

---

### Task 1: Manifest, signature and the update decision (`update.rs`)

**Files:**
- Create: `launcher/src-tauri/src/update.rs`
- Modify: `launcher/src-tauri/Cargo.toml` (`ed25519-dalek`, `semver`), `launcher/src-tauri/src/lib.rs` (`mod update;`)

**Interfaces (produces):**
- `pub const MANIFEST_URL: &str = "https://descargas.montick.app/update/latest.json";` overridden by runtime env `P3_UPDATE_URL` (e2e). The signature check always applies.
- `pub const PUBKEY_HEX: &str` = compile-time env `P3_UPDATE_PUBKEY` if set, else the production key (filled by Task 3).
- `#[derive(Serialize, Deserialize, Clone)] pub struct ManifestBody { pub version: String, pub min_version: String, pub url: String, pub sha256: String, pub size: u64, pub notes: String }`. The field order is fixed, because the signature covers `serde_json::to_vec(&body)`.
- `#[derive(Deserialize)] pub struct Manifest { #[serde(flatten)] pub body: ManifestBody, pub sig: String }` (sig = 128 hex chars).
- `pub fn verify(m: &Manifest, pubkey_hex: &str) -> Result<(), String>` (hex decoded by a private 6-line helper, no `hex` crate).
- `#[derive(Serialize, Clone, PartialEq, Debug)] #[serde(tag = "kind")] pub enum UpdateState { UpToDate, Optional { body: ManifestBody }, Required { body: ManifestBody } }`
- `pub fn decide(body: &ManifestBody, local: &str) -> UpdateState`: semver with pre-release order (`0.3.0-beta.2 > 0.3.0-beta.1`, `0.3.0 > 0.3.0-beta.9`). `local >= version` → UpToDate; `local < min_version` → Required; else Optional. A body with `min_version > version` or an unparsable version → UpToDate.
- `pub fn check(http: &dyn crate::beta::BetaHttp, local: &str, now_unix: i64) -> UpdateState`: GET `<url>?t=<now_unix>` (cache-buster), parse, verify, decide. Any failure → `UpToDate`.

- [ ] **Step 1: Failing tests** (a keypair generated inside the test with `SigningKey::generate`): `verify_accepts_good_signature`, `bad_signature_rejected` (other key; one `notes` byte changed), `older_or_equal_not_offered`, `prerelease_order`, `required_below_min_version`, `optional_between_min_and_latest`, `replayed_old_manifest_ignored` (a valid manifest for a version older than local → UpToDate), `min_above_version_is_ignored`, `offline_never_blocks` (HTTP error / garbage JSON / bad sig → UpToDate), `check_url_has_cache_buster`.
- [ ] **Step 2:** `cargo test update::` → FAIL (module missing).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `cargo test` → all PASS (142 + the new ones).
- [ ] **Step 5:** Commit `feat(launcher): signed update manifest and mandatory/optional decision`.

### Task 2: Download, stage, swap with journal (`update_apply.rs`)

**Files:**
- Create: `launcher/src-tauri/src/update_apply.rs`
- Modify: `launcher/src-tauri/Cargo.toml` (`zip`), `lib.rs` (`mod update_apply;`)

**Interfaces (produces):**
- `pub const PROTECTED: &[&str] = &["versions", "settings.ini", "logs", "textures", "updates", "psprecomp_adhoc_mac.txt"];`
- `pub fn download(http_get: &dyn Fn(&str) -> Result<Box<dyn std::io::Read>, String>, url: &str, sha256: &str, size: u64, dest: &Path, progress: &dyn Fn(u64, u64)) -> Result<(), String>`: streams to `dest.part`, rejects on size or hash mismatch (deletes the part), renames to `dest`.
- `pub fn stage(zip: &Path, staging: &Path) -> Result<Vec<PathBuf>, String>`: strips the leading `Patapon3Recomp/`, rejects absolute paths, `..`, and entries under `PROTECTED`, before writing anything. Returns relative file paths.
- `pub fn apply(root: &Path, staging: &Path, files: &[PathBuf]) -> Result<(), String>`: writes `updates/apply.json` (`{"files": [...], "done": false}`), then for each file renames the old one to `<file>.old` (if present) and moves the new one in. On any error it undoes every step in reverse and deletes the journal. At the end it checks that `launcher.exe` exists with the staged size, marks the journal done and deletes the `.old` files it can (the running `launcher.exe.old` stays). A permission error on the first write → `Err` with the "Mueve la carpeta…" text.
- `pub fn cleanup_on_start(root: &Path)`: an unfinished journal → roll back (restore every `.old`); a finished one → delete leftover `.old` files and `updates/`. "In use" errors are ignored and retried on the next start.

- [ ] **Step 1: Failing tests** (`tempfile`, zips built in the test with `zip::ZipWriter`): `download_rejects_wrong_sha_or_size`, `download_reports_progress`, `stage_rejects_zip_slip_and_protected` (`../x`, `C:/x`, `Patapon3Recomp/versions/a`, `settings.ini` → Err and nothing written), `apply_replaces_files_and_keeps_protected` (versions/, settings.ini, logs/, an extra user theme folder byte-identical), `apply_rolls_back_on_locked_file` (a target opened without share-delete → Err, every original back), `apply_fails_cleanly_when_root_read_only`, `cleanup_rolls_back_unfinished_apply`, `cleanup_removes_old_and_updates`.
- [ ] **Step 2:** `cargo test update_apply::` → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `cargo test` → all PASS.
- [ ] **Step 5:** Commit `feat(launcher): staged update apply with journal and rollback`.

### Task 3: Signing tool and production key

**Files:**
- Create: `launcher/src-tauri/src/bin/p3-sign-update.rs`; Modify `Cargo.toml` (`default-run = "p3-launcher"`, the package's own bin, so `tauri build` keeps building the launcher)
- Create (outside every repo, never printed or committed): `build/beta/claves/update_ed25519.key`

**Interfaces:**
- `p3-sign-update keygen <secret_out>` writes the 32-byte secret and prints the public key hex.
- `p3-sign-update sign <zip> --version V --min-version M --url U --notes FILE --key <secret> --out latest.json` computes size + SHA-256 and signs `update::ManifestBody` (shared struct, so the bytes are identical).

- [ ] **Step 1: Failing test:** `signed_manifest_verifies_with_update_rs` (keygen in a tempdir → sign a small file → `update::verify` OK → `update::decide` gives the expected state).
- [ ] **Step 2–4:** FAIL → implement → `cargo test` PASS.
- [ ] **Step 5:** Generate the real key, put its public hex in `PUBKEY_HEX`, `cargo test` PASS. Commit `feat(launcher): p3-sign-update tool and production update key`. Tell the user where the key is and to back it up.

### Task 4: Commands and screens

**Files:**
- Modify: `launcher/src-tauri/src/lib.rs`: `cleanup_on_start(&root)` first thing in `run()`; commands `check_update() -> update::UpdateState` (spawn_blocking, like `beta_gate`), `install_update() -> Result<(), String>` (refuses with key `update.close_game` while `launch::LaunchGuard` says a game is running; emits `update-progress` `{done, total}`), `restart_launcher()`.
- Modify: `launcher/src/api.ts` (`checkUpdate`, `installUpdate`, `restartLauncher`, `onUpdateProgress`), `launcher/src/App.tsx`, `launcher/src/types.ts`, `lang/es.json`, `lang/en.json`
- Create: `launcher/src/screens/Update.tsx`, `launcher/src/screens/Update.test.tsx`

**Behaviour:**
- At launcher start: `checkUpdate()` in the background. `Optional` → banner on the main menu "Actualización {version} disponible" with **Actualizar** / **Después**. `Required` → the Update screen ("Hay una actualización obligatoria para seguir jugando", notes, **Actualizar**; no Después).
- In `start()` (App.tsx), before `api.betaGate()`: `checkUpdate()` again; `Required` → Update screen, no game start.
- Install: progress bar → "Reiniciando…" → `restartLauncher()`. Error → the message plus "Si sigue fallando, descárgala en https://beta.montick.app"; an Optional update returns to the menu, a Required one stays on the Update screen.
- Everything reachable by pad and keyboard (`useNav`), like the other screens.

- [ ] **Step 1: Failing tests** (`Update.test.tsx`, `App.test.tsx`): `optional banner offers Actualizar and Después`, `required update shows no Después and blocks Start`, `Start re-checks and opens the Update screen when required`, `install error shows the manual download link and keeps Start blocked when required`, `progress bar follows update-progress`, `no banner when up to date`, `pad and keyboard reach every button`.
- [ ] **Step 2:** `npm test -- --run` → the new ones FAIL.
- [ ] **Step 3:** Implement (Rust commands + UI + both lang files).
- [ ] **Step 4:** `npm test -- --run` and `cargo test` → all PASS.
- [ ] **Step 5:** Commit `feat(launcher): update banner, mandatory update screen and restart`.

### Task 5: u6 build (launcher 0.3.0-beta.1, new end date, game exes)

**Files:**
- Modify: launcher `package.json`, `src-tauri/Cargo.toml`, `src-tauri/tauri.conf.json` → `0.3.0-beta.1`; `src-tauri/src/beta.rs` `ENDS` → the date from Decision 1
- Modify (not in git): `build/beta/make_beta_exes.sh` `UNTIL` → same date
- Create: `build/beta/patapon3-beta1-u6.zip` + `.sha256`

- [ ] **Step 1:** Version + `ENDS`; grep the launcher for any other copy of the old date; `cargo test` + `npm test -- --run` PASS. Commit `chore(launcher): 0.3.0-beta.1 (beta update 6)`.
- [ ] **Step 2:** Players' launcher build: `npm run tauri build -- --no-bundle` with `P3_UPDATE_PUBKEY` unset and no `P3_NO_BETA_GATE`.
- [ ] **Step 3:** Game exes: psprecomp on `ge-batching`, game closed, diskquiet, `sh build/beta/make_beta_exes.sh`. Expected per release: `plays until <new date>`, `debug_menu options left: 0`, plain exe back with 0 matches.
- [ ] **Step 4:** Assemble `build/beta/Patapon3Recomp` as for u5 (`PAQUETE.md`), then `python -I build/beta/make_zip.py build/beta/Patapon3Recomp build/beta/patapon3-beta1-u6.zip`. Expected: zip + SHA-256 printed.

### Task 6: End-to-end update test

- [ ] **Step 1: Setup.** Unzip u6 to `E:\p3_upd_e2e`; install the ISO there through the launcher; create a save, `settings.ini` and a user theme; record SHA-256 of everything under the protected tops.
- [ ] **Step 2: Fake next versions.** Build two packages from u6 with a visibly different launcher version: `0.3.0-beta.2` (optional: `min_version 0.3.0-beta.1`) and `0.3.0-beta.3` (mandatory: `min_version 0.3.0-beta.3`). Sign each with a **test key** (launcher built with `P3_UPDATE_PUBKEY` = test public key); serve from a scratch dir with `python -m http.server`; start with `P3_UPDATE_URL=http://127.0.0.1:<port>/latest.json`.
- [ ] **Step 3: Happy paths.** Optional: banner → Después → plays; banner → Actualizar → progress → restart → corner shows `0.3.0-beta.2`. Mandatory: Update screen, Start blocked; Actualizar → `0.3.0-beta.3`. After each: protected hashes identical, the game starts, and the next start removes `launcher.exe.old` and `updates/`.
- [ ] **Step 4: Failures.** Truncated zip → error + manual link, launcher unchanged and working; game running → "Cierra el juego" refusal; server down → no banner, Start works; process killed in the middle of the swap (journal left unfinished) → next start rolls back and the launcher works.
- [ ] **Step 5:** Delete `E:\p3_upd_e2e` only after recording the results (it is our own test folder).

### Task 7: Publishing guide and notes

- [ ] **Step 1:** `build/beta/PUBLICAR_ACTUALIZACION.md` (Spanish): build → `p3-sign-update sign` (with the real key) → upload the zip **first** → upload `update/latest.json` with `Cache-Control: no-cache` → open the URL in a browser to check. How to make an update mandatory (`--min-version` = the new version). First publish: `latest.json` points at u6 itself (`min_version` = `0.3.0-beta.1`).
- [ ] **Step 2:** `build/beta/ACTUALIZACION_6.md` (same layout as u5) and the changelog lines: "El launcher se actualiza solo" and "Las actualizaciones obligatorias se instalan antes de jugar".
- [ ] **Step 3:** Update the memories `p3-beta-1.md` and `user-changelog.md`; mark B3–B6 of the 25 Oct plan as superseded by this plan.
