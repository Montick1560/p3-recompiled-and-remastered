# Beta 1 of infrastructure mode (25 invited players) — design

## Intent
The online (infrastructure) mode works on one PC: lobby, shared hideouts, co-op, VS between parties. What one PC
cannot show is real use: real latency, many players at once, people in other places, 2v2 with real input. The user
invites about 25 people who already own the game ISO to use the online mode hard for one weekend, to see how the
server and the online mode behave, and to collect problems with enough data to fix them.

## Window (decided by the user)
- Opens **Friday 23 Oct 2026, 21:00** CDMX (UTC-6, no DST) = `2026-10-24T03:00:00Z`.
- Closes **Sunday 25 Oct 2026, 23:59** CDMX = `2026-10-26T05:59:59Z`.
- After it closes: the beta launcher does not open the game and nothing connects to the servers until another round
  (another round ships a new beta package).

## Goals
1. **Only invited people play.** Each invitee gets a personal invite code by email. A code works for one person on up
   to 2 PCs at once (my call). Codes can be disabled live without restarting anything.
2. **The window is enforced in three layers**: the server (authoritative: refuses logins outside the window), the
   launcher (asks the server whether the round is open; does not start the game when it is closed), and the beta
   runtime (the end date is built in; after it, it refuses to start).
3. **Clear refusals.** A wrong or disabled code, an outdated runtime, or a closed window each get their own Spanish
   message in the launcher, before the game starts.
4. **The server is ready for public use**: a protocol/version check in the login (older runtimes get "actualiza"),
   rooms per address capped, capacity for 25-50 players with margin, proven by a load test with synthetic clients.
5. **Observe the beta**: the server records sessions, rooms, co-op/VS starts and ends, errors and disconnects with
   their cause, per invite code (no other personal data), in SQLite. A report script turns it into a summary.
6. **Problem reports, two doors, one store**:
   - a "Reportar problema" button in the launcher: short form (what happened, roughly when, mode: hideout / co-op /
     VS / other) plus, attached automatically, the tail of `logs/game.log` (and `game.prev.log`), runtime version,
     nick, invite code and machine id;
   - `report.montick.app`: a basic web form (same fields, no log) for when the launcher does not open or from a phone.
   Both store into the same SQLite. The user reads them with a local script on the server machine (not exposed).
7. **Distribution**: one zip of the portable launcher (the three game releases with the beta runtime) on Cloudflare
   R2 behind `descargas.montick.app`, its SHA-256 in the email. One Gmail **draft** per invitee with their code,
   which the user reviews and sends.

## Non-goals
- Player accounts, passwords, cloud saves, progress storage (later rounds, if the beta goes well).
- Changing the game (the 60 s idle rule stays).
- Anti-cheat or protection against a determined person changing their clock or patching the exe: the server window
  is the real lock; the launcher and exe checks stop normal use.
- A public dashboard. The summary is for the user.

## Anti-goals (would make it a failure even if it works)
- An invitee cannot play during the window because of the new gates (wrong clock zone, a code typed with spaces or
  lowercase, a server restart). Codes are case-insensitive and ignore spaces and dashes; all window checks use UTC.
- Reports without the log, so problems cannot be diagnosed.
- The user's own local testing (e2e with the local server, two.py, rt_regress) breaks: with no beta configuration the
  server and runtime behave as today.
- Sending anything on the user's behalf: emails are drafts only; nothing is pushed or published without the user.

## Constraints
- Today is Friday 23 Oct; the beta opens at 21:00 the same day. The plan is ordered so the must-haves (gates, codes,
  package, emails) are done first; reports and metrics follow; the load test and a two-PC smoke test run before
  opening.
- The public server runs on the user's other machine (Windows user `tester`) behind the Cloudflare Tunnel
  `patapon-online` (`online.montick.app` -> the WebSocket listener). Claude cannot reach that machine: copying files,
  adding the tunnel hostname `report.montick.app`, creating the R2 bucket and custom domain are user steps, written as
  a numbered runbook in Spanish.
- Python stdlib only on the server side (sqlite3 is in the stdlib). Launcher: Tauri 2 + React; Rust already has
  `ureq` and `sha2`. Runtime: C++20.
- Commits authored "Patapon Community", no Claude co-author trailer. Native execution (no helper models, no
  subagents).
- This PC's disk stalls writes for seconds: e2e runs after `build/infra/diskquiet.py`.

## How (decided)
- **Server config file** (`--beta <file.json>`; absent = no gating, as today): window start/end (UTC), minimum
  protocol version, invite codes as SHA-256 hashes with a label (e.g. "invitado 07") and an enabled flag, staff codes
  that also work outside the window (the user and test instances), max connections per code (2). The server re-reads
  the file when it changes (disable a code live).
- **Login fields**: the runtime adds `proto` (an integer, bumped when the wire protocol changes; this beta = 2) and
  `invite` (from `PSPRECOMP_INFRA_INVITE`, which the launcher sets). Refusals use distinct error codes the launcher
  and logs can name.
- **Beta HTTP endpoints on the existing WebSocket listener** (so `online.montick.app` serves them, no new tunnel
  route): `GET /beta` -> `{open, starts, ends, min_proto, now}`; `POST /beta/check {code}` -> ok / unknown / disabled /
  closed. Rate-limited per address.
- **Runtime end date** is a build option (CMake `PSPRECOMP_BETA_UNTIL`, ISO UTC). Unset = no limit. When past: a
  Spanish message box and exit code 12 ("beta ended"), which the launcher maps to its "the beta ended" screen.
- **Launcher**: first run of the beta asks for the code (saved in settings.ini `[online] invite`), validates it with
  `/beta/check`; the Start button asks `/beta` first; unreachable server inside the embedded window = start allowed
  (offline single player still works during the window); past the embedded end date = blocked regardless.
- **Metrics**: `beta.db` (SQLite, WAL) next to the server; tables for sessions, rooms, events, errors. Written from
  the server's existing log thread pattern (a queue + one writer thread) so a slow disk never blocks the server.
- **Report service**: `tools/beta_report_server.py`, stdlib HTTP on `127.0.0.1:27322`, routes `GET /` (form, Spanish),
  `POST /report` (JSON; log tail up to 512 KiB, gzip+base64), same `beta.db`. Per-address rate limit. The tunnel maps
  `report.montick.app` to it. `tools/beta_summary.py` writes a local HTML summary (reports + metrics).
- **Load test**: `tools/infra_loadtest.py` drives N synthetic clients (login with staff codes, hideout rooms of 4,
  VS rooms across parties, datagrams at the game's rate) against the local server and, before opening, briefly
  against the public one.

## Left to the builder
File layout inside the server, exact table columns, exact error code numbers (inside the PSP NP range already
used), the launcher screens' look (follow the existing theme and screens), the code alphabet (no ambiguous
characters), the summary's layout.

## Acceptance
- Local: server selftest covers codes, window, version, limits, beta endpoints, metrics rows; runtime ctest green;
  launcher `npm test` + `cargo test` green; e2e co-op with the gating on (staff code) and off.
- Load test: 50 synthetic clients for 10 minutes against the local server with no errors and the per-client datagram
  delay p99 under 50 ms on loopback; a 25-client burst against the public server works before opening.
- Two real PCs (the user and one invitee or the user's second machine) connect through the public server with their
  codes before 21:00.
- Package on R2, SHA-256 matches, 25 drafts in Gmail, runbook done by the user.
