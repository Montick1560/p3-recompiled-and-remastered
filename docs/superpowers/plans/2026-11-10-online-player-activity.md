# Online Player Activity on the Rooms Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the public rooms page shows what each player in an online mission is doing — ready, watching a scene,
reading tips, opening chests, viewing results, loading — using the game's own "tweet" status.

**Architecture:**
- The game already shares this status between players: mission scripts call `multi_setTweet(code)` (390 uses) and
  draw the partners' codes as bubbles (`multi_getTweet` + `callMessageWindow`, msg ids 117-122: "El jugador está
  listo / mirando un evento / mirando consejos / revisando cofres del tesoro / revisando resultados / cargando datos").
- Own code in RAM (Ghidra, EBOOT, same in the three releases): table = rd32(rd32(rd32(rd32(0x08AABD94)+0x78)+100)+0x18C),
  own slot i = rd32(rd32(rd32(0x08AB9560)+0x44)+0x28) (< 8), code = rd32(table + i*0x14 + 0x30). setTweet only
  stores while online (FUN_08986CB4: rd32(rd32(0x08AABD94)+0x78)+0x44 -> +0x24 byte).
- Existing path, extended by one field: `p3_presence_read` (pure RAM read) -> `p3_online_status` JSON in the
  `ping` (sent on change) -> `infra_server.clean_status` (allow-list) -> `salas.json` roster -> page
  (`beta_report_server.py`, textContent only).

**Tech Stack:** C++20 game module + ctest (test_p3_presence, test_p3_presence_text); Python server tools with their
selftests; lab two.py (ports 9997/9998, local infra server).

**Spec:** the user, 10 Nov 2026: "Dale 1" — option 1 of my list: "Estado de los jugadores online … sabríamos en
tiempo real qué está haciendo cada uno (listo, viendo consejos, en los cofres…) para la página de salas".

## Global Constraints
- Client-sent text stays untrusted: the server accepts only the six code words, the page renders with textContent.
- Old clients (no field) and old servers (ignore the field) keep working.
- Branch online-activity (from fx-probe HEAD, worktree build/wt_fx, build build/rtw_fx). No push, no deploy to the
  tester server without the user. Commits as Patapon Community. Never port 9999; no desktop capture.

## Review Focus
1. A bad pointer chain (offline, title screen, mid-load) must give "no activity", never a crash or a stale code.
2. A code outside 1..6 (or a modded client) must not reach the page.
3. Solo play (not online) sends no activity.

---

### Task 1: Live recon of the code
- [ ] fx60/tweet.py PORT: read the chain above every 0.5 s and print changes (with the online byte).
- [ ] Lab host in a room (fx60/pg_go.sh-like route, any mission): record the code at the room, loading, tips,
  mission start, results, chest. Expected: 6 cargando / 3 consejos / 1 listo / 5 / 4 at those points; 0 offline.

### Task 2: Read it in the client (TDD)
- [ ] test_p3_presence: fake chain -> activity == 4; own slot 8 -> 0; online byte 0 -> 0; broken link -> 0.
- [ ] P3Presence gains `uint8_t activity` (0 = none); p3_presence_read fills it; operator== includes it.
- [ ] test_p3_presence_text: activity 4 -> status["activity"] == "chests"; 0 -> no key.
- [ ] p3_online_status adds "activity": ready|event|tips|chests|results|loading.

### Task 3: Server + page (TDD)
- [ ] infra_server selftest: clean_status keeps "activity" only for the six words; the roster entry carries it.
- [ ] beta_report_server selftest: the roster line shows the Spanish label (Listo, Viendo una escena, Leyendo
  consejos, Abriendo cofres, Viendo resultados, Cargando) via textContent.

### Task 4: Live end to end + hand-off
- [ ] Two lab instances in one room through the local infra server: salas.json shows each member's activity
  changing (tips -> ready -> results/chests). ctest + both selftests green.
- [ ] Notes for deployment (client exe + infra_server.py + beta_report_server.py), CHANGELOG line, ledger, memory.
