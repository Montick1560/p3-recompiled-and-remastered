# Unique player identity online (player 2 is not player 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In an infrastructure (and ad hoc) session every player has their own identity, so the visitor is "2.<hero>" in a mission instead of a second "1.<hero>", even when two machines run copies of the same game folder.

**Architecture:** Patapon 3 tells players apart by `sceOpenPSIDGetOpenPSID`, which the runtime builds from the ad hoc MAC (fix 4db6937, PPSSPP #13231). That MAC comes from `adhoc_mac.txt` in the game folder, so a copied folder (or two instances on one PC) gives two consoles the same OpenPSID and the visitor takes slot 1. The MAC file becomes machine-bound (it records a hash of the machine id and is regenerated on another machine), and the room server refuses a join from a console id already in the room, so a collision gives the game's "could not connect" message instead of two player 1s.

**Tech Stack:** C++20 runtime (`psprecomp/runtime`), ctest in `build/rtw`, Python 3 stdlib server `tools/infra_server.py`, e2e driver `build/infra/two.py` (outside the repo).

**Spec:** `docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md` (infrastructure mode), plus the evidence below.

## Evidence (22 Oct 2026)

- Every two-instance infra e2e so far used one MAC: both logs print `[ADHOC] config: ... mac 50:a0:f3:fc:7b:60` (same `build/rtw/psprecomp_adhoc_mac.txt`).
  In the mission the visitor's screen was identical to the host's: its own hero labelled `1.Edson` (`build/infra/pub2/ms_B.png`, `fight_B.png`).
- Same mission with `PSPRECOMP_ADHOC_MAC` 02:00:00:00:00:0a / 0b (`build/infra/mac1`): the visitor's own hero is `2.Edson`, its HUD icons swap order (`mac1/compare_ms.png`, `mac1/compare_fight.png`; top row distinct MACs, bottom row same MAC).
- The deployed portable folders carry `adhoc_mac.txt` at their root (`build/e2e_portable3/adhoc_mac.txt`, launcher option `online.mac_file`, default `adhoc_mac.txt`), so copying the folder to another machine copies the identity.
- Saves are not bound to the OpenPSID: PPSSPP saves (made with PPSSPP's MAC) load in the port with another MAC. Changing the MAC once is safe.

## Global Constraints

- Work continues on the local branch `infra-mode` in `psprecomp/` (not pushed). Commits authored as configured ("Patapon Community"), no Claude co-author trailer, message ends with `Verified: ...`. Nothing is pushed without the user's OK (next push branch `push-p3-32`).
- `PSPRECOMP_ADHOC_MAC` keeps winning over the file (tests and two-instance setups rely on it).
- Never touch the user's game on port 9999; test instances on 9996-9998, killed by path, never by image name.
- 30 fps `rt_regress.py` PASS and ctest green at the end of every task that touches the runtime.
- Docs in the same change: `docs/ENV_FLAGS.md` (MAC file format, new env var), `README.md` (identity note).

## Review Focus

1. **Legacy MAC file** (one line, no hash: every existing install, including the user's main PC): regenerated once with the machine hash; the game, ad hoc and saves keep working. Test `legacy_mac_file_regenerated` (Task 1).
2. **Folder copied to a second machine**: the second machine gets a different MAC on its first start, the first machine keeps its own. Test `copied_file_other_machine_regenerated` (Task 1) and e2e (Task 3).
3. **Read-only or missing folder for the MAC file**: a MAC is still produced in memory, nothing crashes, the next start regenerates. Test `unwritable_mac_file_still_gives_mac` (Task 1).
4. **Old runtime without `console` in its login**: the server does not refuse it. Selftest case (Task 2).
5. **Two instances on one PC without `PSPRECOMP_ADHOC_MAC`** (same machine, same file): they share the MAC; the server refuses the second join instead of making two player 1s, and the runtime logs why. Selftest (Task 2) and e2e `SAME_MAC=1` (Task 3).

---

### Task 1: Machine-bound ad hoc MAC

**Files:**
- Modify: `runtime/include/net/adhoc_config.h`, `runtime/src/net/adhoc_config.cpp`
- Create: `runtime/include/psp_machine_id.h`, `runtime/src/psp_machine_id.cpp` (host query, Windows + POSIX)
- Modify: `runtime/src/hle/psp_hle_net.cpp:39-50` (`psp_net_config()` passes the machine id), `runtime/CMakeLists.txt` (new source; `test_adhoc_config` unchanged sources plus nothing host-specific)
- Test: `runtime/tests/test_adhoc_config.cpp`

**Interfaces:**
- Produces:
  - `std::string adhoc_machine_hash(const std::string& machine_id);` — FNV-1a 64 of the id as 16 lowercase hex digits; an empty id hashes the string `"unknown"`.
  - `AdhocConfig adhoc_config_load(const std::function<const char*(const char*)>& getenv_fn, const std::string& mac_file, uint32_t random_seed, const std::string& machine_id);` — the new 4th parameter; `PSPRECOMP_ADHOC_MACHINE_ID` (env, testing aid) replaces `machine_id` when set and non-empty.
  - MAC file format: one line `aa:bb:cc:dd:ee:ff <hash16>`. The file's MAC is used only when its hash equals `adhoc_machine_hash(machine_id)`; otherwise (no hash, other hash, unparsable) a new MAC is generated (`generate_mac(random_seed ^ low 32 bits of the hash)`) and the file rewritten. Log `[ADHOC] new MAC for this machine (file was from another machine or older)` when an existing file is replaced.
  - `std::string psp_machine_id();` (`psp_machine_id.h`) — Windows: `RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, ...)` as UTF-8, falling back to `GetComputerNameExW(ComputerNamePhysicalDnsHostname)`; POSIX: first line of `/etc/machine-id`, else `/var/lib/dbus/machine-id`, else `gethostname`. Empty string if all fail. Never logged raw (only its hash, in `[ADHOC] config: ... machine <hash16>`).

- [ ] **Step 1: Failing tests** in `test_adhoc_config.cpp` (temp files under the test's working directory, like the existing file tests):
  - `mac_file_keeps_mac_on_same_machine`: load with machine `"pc-1"` writes the file; a second load with `"pc-1"` and another seed returns the same MAC; the file's line ends with `adhoc_machine_hash("pc-1")`.
  - `copied_file_other_machine_regenerated`: file written for `"pc-1"`; load with `"pc-2"` returns a different MAC and rewrites the file with `adhoc_machine_hash("pc-2")`; loading again with `"pc-1"` regenerates once more (the copy moved back) — never the same MAC for two machine ids with the same seed.
  - `legacy_mac_file_regenerated`: file containing only `02:00:00:00:00:0a\n` → the returned MAC differs from `02:00:00:00:00:0a` and the file gains the hash.
  - `env_mac_wins`: `PSPRECOMP_ADHOC_MAC=02:11:22:33:44:55` with any file and machine → that MAC, file untouched.
  - `machine_id_env_override`: `PSPRECOMP_ADHOC_MACHINE_ID=lab-b` with machine argument `"pc-1"` → file hash is `adhoc_machine_hash("lab-b")`.
  - `unwritable_mac_file_still_gives_mac`: `mac_file` inside a directory that does not exist → a valid unicast MAC (first byte `& 3 == 0`, not all zero), no exception.
  - `machine_hash_stable`: `adhoc_machine_hash("pc-1")` is 16 hex chars, equals itself across calls, differs from `adhoc_machine_hash("pc-2")`; `adhoc_machine_hash("") == adhoc_machine_hash("unknown")`.
  - Existing calls in the file gain the 4th argument `"test-machine"`.
- [ ] **Step 2:** `ninja test_adhoc_config && ctest -R adhoc_config` → build fails (4th parameter missing).
- [ ] **Step 3: Implement** the hash, the file format and `psp_machine_id()`; `psp_net_config()` calls `adhoc_config_load(..., seed, psp_machine_id())`. Link `advapi32` on Windows for `RegGetValueW` if the toolchain does not already (`PSP_SOCKET_LIBS` block in `runtime/CMakeLists.txt`).
- [ ] **Step 4:** same command → PASS; `ninja && ctest` all green; on this PC start one test instance (port 9998) through infrastructure mode or ad hoc and check its log shows `[ADHOC] new MAC for this machine` once and a different MAC from `50:a0:f3:fc:7b:60` (the old shared file), then a second start keeps it.
- [ ] **Step 5:** 30 fps `rt_regress.py` vs `build/tour/wait30` → PASS.
- [ ] **Step 6: Commit** (`fix(net): the ad hoc MAC belongs to the machine, not to a copied folder`).

### Task 2: The room server refuses a console id already in the room

**Files:**
- Modify: `tools/infra_server.py` (login, join, selftest)
- Modify: `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp` (login carries extra fields)
- Modify: `runtime/src/hle/psp_hle_np_matching2.cpp` (`ensure_connected()` sends the console id; clear log on the refusal)
- Test: `python -I tools/infra_server.py --selftest`; `runtime/tests/test_infra_client.cpp`

**Interfaces:**
- Produces:
  - Login request gains optional `"console"`: 12 lowercase hex digits (the 6-byte MAC that starts the OpenPSID). Server validates `[0-9a-f]{12}` (else BAD_REQUEST) and stores it on the session; absent = no check.
  - `join` into a room where a member's session has the same `console` → error `DUPLICATE_CONSOLE = 0x80550D07` (the FORBIDDEN code the game already handles) and log `<label> join room <id> refused: same console id as <member label>`.
  - `bool Client::connect(const std::string& online_id, int timeout_ms, const nlohmann::json& login_extra = nlohmann::json::object());` — the extra fields are merged into the login request.
  - Matching2: `ensure_connected()` passes `{"console": <hex of psp_net_config().mac>}`; a JoinRoom answer with error `0x80550D07` logs `[NP2] join refused: another player in the room has this console id (copied adhoc_mac.txt or same PSPRECOMP_ADHOC_MAC)`.
- Consumes: Task 1's MAC (via `psp_net_config()`).

- [ ] **Step 1: Failing tests:**
  - selftest: two logins with `console` `"02000000000a"` → A creates, B with the same console joins → error `0x80550D07`; C with `"02000000000b"` joins → ok; D logging in without `console` joins a room whose members all have consoles → ok (old runtime); login with `console` `"xyz"` → `BAD_REQUEST`.
  - `test_infra_client.cpp` `login_carries_extra_fields`: `connect("P3A", 2000, {{"console", "02000000000a"}})` → the fake server's first frame has `op == "login"` and `console == "02000000000a"`.
- [ ] **Step 2:** `python -I tools/infra_server.py --selftest` and `ctest -R infra_client` → both fail.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4:** selftest `selftest OK` (run it 3 times: the server selftest has had timing flakes); `ninja && ctest` green.
- [ ] **Step 5:** 30 fps `rt_regress.py` → PASS. **Commit** (`fix(net): a room refuses a second player with the same console id`).

### Task 3: Prove it in the game, document, deploy

**Files:**
- Modify (outside the repo): `build/infra/two.py` — `MAC_FILE_TEST=1` mode: no `PSPRECOMP_ADHOC_MAC`; each instance gets its own copy of one legacy MAC file (`OUTDIR/macA/adhoc_mac.txt`, `OUTDIR/macB/adhoc_mac.txt`, both `50:a0:f3:fc:7b:60`) through `PSPRECOMP_ADHOC_MAC_FILE`, and `PSPRECOMP_ADHOC_MACHINE_ID` `lab-A` / `lab-B` (two machines that copied one folder). `SAME_MAC=1` already exists (no per-instance MAC).
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_ADHOC_MAC_FILE` format line, new `PSPRECOMP_ADHOC_MACHINE_ID`), `README.md` (one sentence: each machine gets its own identity; two instances on one PC need `PSPRECOMP_ADHOC_MAC`), `docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md` (identity paragraph with the evidence above).
- Hand-off note: `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-23.md` (check the name is free first; use the next letter if not), in Spanish.

- [ ] **Step 1: e2e, copied folder:** `MAC_FILE_TEST=1 python -I two.py build/infra/id1`, then `host_join.sh id1` and `mission.sh id1`. Expected: the two logs print different MACs (neither `50:a0:f3:fc:7b:60`), B joins, and in the mission B's own hero is `2.Edson` with the swapped HUD order, as in `mac1/compare_ms.png` top row (screenshots `ms_*`, `fight_*` after `* D PPPO---- 2`).
- [ ] **Step 2: e2e, same identity:** `SAME_MAC=1 python -I two.py build/infra/id2`, `host_join.sh id2`. Expected: server log `join room ... refused: same console id`, B's log `[NP2] join refused`, B shows the game's "No se ha podido conectar con el escondite" and returns to its hideout (screenshot `j1_B`); nobody ends up as a second player 1.
- [ ] **Step 3: Regression:** ad hoc smoke `ADHOC=1 python -I two.py build/infra/idA` (A hosts ad hoc, B lists `Escondite de [UH] Edson`); `ninja && ctest` green; 30 fps `rt_regress.py` PASS.
- [ ] **Step 4: Deploy:** back up the three variants' `psprecomp_runtime.exe` of `build/e2e_portable` and `build/e2e_portable3` into `build/backup_pre_identity_<date>/`, build `build/v115b/rtw` and `build/vanilla/rtw`, copy, `cmp`. Write the hand-off: what was wrong (same OpenPSID from a copied `adhoc_mac.txt`), that each machine generates a new MAC once on its first start after the update, and that the server on the `tester` machine must be replaced with the new `tools/infra_server.py` (copy it over `C:\patapon-server\infra_server.py` and restart `run_infra_server.bat`) for the duplicate guard.
- [ ] **Step 5: Commit** docs (`docs: identity per machine for online play`), with the evidence paths in `Verified:`. Ask the user before any push.
