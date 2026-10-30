# Launcher file pickers vs. SDL RAWINPUT — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** "Elegir la carpeta de PPSSPP" and the ISO picker open every time, and picking a folder never freezes or closes the launcher, with a pad connected.

**Architecture:** One SDL hint. The launcher's pad thread loads the game's SDL2.dll (2.32.10) with the
GameController subsystem; SDL's RAWINPUT joystick driver breaks the native Windows file dialog that
tauri-plugin-dialog/rfd opens on its own thread. The launcher sets `SDL_JOYSTICK_RAWINPUT=0` before
`SDL_InitSubSystem`; DualSense/DS4 keep working through HIDAPI and Xbox pads through SDL's XInput
driver. The earlier "show before fullscreen" change (launcher `d9365df`) was a coincidence and is reverted.

**Tech Stack:** Rust (Tauri 2, `libloading` SDL2 bindings in `launcher/src-tauri/src/sdl_pad.rs`), rfd 0.16 via tauri-plugin-dialog 2.8.3, Node 24 CDP + PowerShell UI checks.

**Spec:** none — this plan argues from the investigation below (29 Oct 2026, user report "al seleccionar
una carpeta, se rompe, ni se redimensiona el cuadro de diálogo").

### Evidence (what the plan rests on)

| Run (test copy `E:\p3_upd_e2e`, DualSense connected) | Result |
|---|---|
| Crash dump `%LOCALAPPDATA%\CrashDumps\launcher.exe.34000.dmp` (23:09:27, dialog open) | Fail-fast `0xe0464645` in `CoreMessaging` on rfd's dialog thread: focus change → TSF `msctf` → `TextInputHost` → `CoreUISessionCreate` fails. Stacks: `build/tools/launcher_picker/crash_34000_stacks.txt` |
| rfd patched to print HRESULTs, default SDL | first `Show` sometimes OK, every later one `0x80004005` (E_FAIL) in < 1 s; windowed and fullscreen alike |
| all four `SDL2.dll` renamed (pad via gilrs) | 4/4 open + cancel OK (`0x800704C7` = cancelled) |
| SDL restored, env `SDL_JOYSTICK_WGI=0` | 4/4 E_FAIL |
| SDL restored, env `SDL_JOYSTICK_RAWINPUT=0` | 4/4 open + cancel OK, DualSense still `connected` |

The crash itself was not reproduced with synthetic input (UIA focus does not trigger it); it lives on
the same dialog thread in the same process state, so Task 3's user check is what closes it.

## Global Constraints

- Launcher repo `launcher/`, branch `direct-option`; author as configured (Montick1560); no `Co-Authored-By`; no push.
- Never kill the user's game (port 9999) or processes this session did not start; stop only by PID.
- Tests run in the test copy `E:\p3_upd_e2e\Patapon3Recomp` (u7 + ISO installed), never in the user's folders.
- Release signing key `build/beta/claves/update_ed25519.key` stays on disk; never printed or copied.
- `build/beta/latest.json` is u6's — do not overwrite; the u7 manifest is `build/beta/latest_u7.json`.
- Players' text (CHANGELOG) in Spanish; code and commits in English.

## Review Focus

1. Xbox / XInput pad with RAWINPUT off → still navigates the launcher (SDL falls back to its XInput driver). User check in Task 3.
2. Picking a folder by real mouse click and typing into the "Carpeta:" box (the crash path) → launcher stays alive. User check in Task 3.
3. Steam Deck / Steam Input virtual pad (XInput) → unchanged. Not testable here; note in the report.
4. Game start → pad suspend (`SDL_QuitSubSystem`) → game exit → `resume()` re-runs `init()`, so the hint must be set inside `init()`, not once at load. Pinned by the Task 2 test on `init`'s hint list.
5. Second and third picker in one session (the deterministic E_FAIL) → all open. `cycle.sh` with N=4 in Task 2.

---

### Task 1: Revert the coincidental "show before fullscreen" fix

**Files:**
- Modify: `launcher/src-tauri/src/lib.rs:585-588` (via `git revert d9365df`)

**Interfaces:** none.

- [ ] **Step 1: Revert**

```bash
cd launcher && git revert --no-edit d9365df
```
Expected: `lib.rs` back to `if mode != "windowed" { apply_window_mode(&win, mode); }` before `win.show()`; the
revert message is amended to say: "the pickers failed because of SDL RAWINPUT, not the fullscreen order".

- [ ] **Step 2: Run the Rust tests**

Run: `cd launcher/src-tauri && cargo test --release 2>&1 | grep "test result"`
Expected: `164 passed; 0 failed`

### Task 2: Turn SDL's RAWINPUT joystick driver off in the launcher

**Files:**
- Modify: `launcher/src-tauri/src/sdl_pad.rs` (`Api::init`, tests module)

**Interfaces:**
- Produces: `const HINTS: &[(&CStr, &CStr)]` in `sdl_pad.rs` — every hint `init()` sets, in order:
  `("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1")`, `("SDL_JOYSTICK_HIDAPI","1")`, `("SDL_JOYSTICK_RAWINPUT","0")`.

- [ ] **Step 1: Write the failing test** (in `sdl_pad.rs` `mod tests`)

```rust
#[test] fn rawinput_driver_off() {
    // SDL's RAWINPUT driver makes every Windows file dialog after the first fail (E_FAIL) and can
    // crash the launcher inside the dialog (CoreMessaging fail-fast); HIDAPI + XInput cover the pads.
    assert!(HINTS.contains(&(c"SDL_JOYSTICK_RAWINPUT", c"0")));
    assert!(HINTS.contains(&(c"SDL_JOYSTICK_HIDAPI", c"1")));
}
```

- [ ] **Step 2: Run it**

Run: `cargo test --release rawinput_driver_off`
Expected: FAIL to compile — `cannot find value HINTS`.

- [ ] **Step 3: Add `HINTS` and make `init()` loop over it** before `init_sub(SDL_INIT_GAMECONTROLLER)`, replacing the two `set_hint` lines. One-line comment on `HINTS` with the why (as in the test).

- [ ] **Step 4: Run the tests**

Run: `cargo test --release 2>&1 | grep "test result"`
Expected: `165 passed; 0 failed`

- [ ] **Step 5: Build and run the picker check with the DualSense connected**

```bash
cd launcher && npx tauri build --no-bundle
cp src-tauri/target/release/p3-launcher.exe /e/p3_upd_e2e/Patapon3Recomp/launcher.exe
```
Start it (PowerShell, record the PID) with `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223`
and stderr to a log, once with `window_mode = windowed` and once with `fullscreen` in the test copy's settings.ini, then:

Run: `bash build/tools/launcher_picker/cycle.sh <PID> 4 <stderr log>`
Expected: `try 1..4 dialog=<hwnd>` (never `none`) and the log shows `[PAD] connected "DualSense…"`.
(The release build prints no `[RFD-DIAG]` lines, so the counts read 0; the hwnds are the check.)

- [ ] **Step 6: Pad still drives the launcher**

With the launcher focused, press D-pad down + Cross on the DualSense (or ask the user if away), then
Run: `node build/tools/launcher_picker/cdp.mjs "document.body.innerText.slice(0,40)"`
Expected: the screen moved (e.g. `PARTIDAS`).

- [ ] **Step 7: Commit**

```bash
git add src-tauri/src/sdl_pad.rs
git commit -m "fix(launcher): turn SDL's RAWINPUT pad driver off so file pickers work" \
  -m "With it on, every Windows file dialog after the first failed (E_FAIL) and picking a folder could crash the launcher in CoreMessaging. HIDAPI (DualSense/DS4) and XInput still cover the pads." \
  -m "Verified: cycle.sh 4/4 dialogs windowed + fullscreen with a DualSense connected (before: E_FAIL from the 2nd on); cargo test 165"
```

### Task 3: Repackage u7 and hand the user the real-mouse check

**Files:**
- Modify: `build/beta/patapon3-beta1-u7.zip`, `.sha256`, `build/beta/latest_u7.json`
- Modify: `build/beta/ACTUALIZACION_7.md` (SHA, size, launcher commit), `build/beta/CHANGELOG.md` (u7 SHA + "Arreglado" line)
- Modify: `build/beta/u7/Patapon3Recomp/launcher.exe` (the user's test folder: only this file)

**Interfaces:**
- Consumes: `launcher/src-tauri/target/release/p3-launcher.exe` from Task 2.

- [ ] **Step 1: Rebuild the zip from a clean stage** (the user's `u7` folder now holds their settings/logs; `make_zip.py` refuses it, correctly)

Expand the current `patapon3-beta1-u7.zip` into a scratch folder, replace `Patapon3Recomp/launcher.exe`, then
Run: `python build/beta/make_zip.py <stage>/Patapon3Recomp build/beta/patapon3-beta1-u7.zip`
Expected: `88 files, … sha256 <new>`

- [ ] **Step 2: Re-sign**

Run: `launcher/src-tauri/target/release/p3-sign-update.exe sign build/beta/patapon3-beta1-u7.zip --version 0.3.0-beta.2 --min-version 0.3.0-beta.2 --url https://descargas.montick.app/patapon3-beta1-u7.zip --notes build/beta/notas_u7.txt --key build/beta/claves/update_ed25519.key --out build/beta/latest_u7.json`
Expected: same SHA as Step 1.

- [ ] **Step 3: The real u6 launcher accepts it**

Serve the manifest on `127.0.0.1:8765`, put the u6 `launcher.exe` (from `patapon3-beta1-u6.zip`) in the test copy, start it with `P3_UPDATE_URL=http://127.0.0.1:8765/latest.json`, read the screen with `cdp.mjs`, then put the Task 2 exe back.
Expected: `Hay una actualización obligatoria para seguir jugando`.

- [ ] **Step 4: Docs**

Replace the old u7 SHA/size everywhere in `CHANGELOG.md` and `ACTUALIZACION_7.md`; launcher commit → Task 2's. Replace the u7 "Arreglado" line with:
"- Con un mando conectado, «Elegir la carpeta de PPSSPP» (Partidas → Importar de PPSSPP) y el botón para elegir la ISO a veces no abrían nada, y elegir una carpeta podía cerrar el launcher. Ahora abren siempre."

- [ ] **Step 5: Copy the exe to the user's test folder and write the check**

Copy Task 2's exe over `build/beta/u7/Patapon3Recomp/launcher.exe` (nothing else there). User check (in the final message): DualSense on, launcher in pantalla completa → Partidas → Importar de PPSSPP → Elegir la carpeta → click a folder, click into "Carpeta:" and type a letter, Seleccionar carpeta; repeat 3 times; then the same with the Xbox pad if they have one. Pass = the dialog opens every time, never freezes, the launcher never closes.

- [ ] **Step 6: Record**

Append the result to `psprecomp/.superpowers/sdd/2026-10-24-p3-direct-p2p-part2-direct/progress.md` and update memory `p3-direct-p2p.md` (new u7 SHA, launcher commit).
