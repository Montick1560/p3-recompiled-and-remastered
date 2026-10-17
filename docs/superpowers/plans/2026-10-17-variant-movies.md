# Movies in the 1.1.5b and vanilla builds — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Movies (opening cinematic, title intro, story cutscenes) play in the DxD 1.1.5b and vanilla
UCES-01421 builds as they already do in the DxD 1.1.4 build.

**Architecture:** Bug fix in the build orchestration, not in the runtime. `p3.sh build` never passes
`PSPRECOMP_FFMPEG_DIR` to CMake; the 1.1.4 build folder (`build/rtw`) only has it because it was
once set by hand in its CMake cache, while the folders created by `P3_VARIANT` builds
(`build/v115b/rtw`, `build/vanilla/rtw`) were configured with it empty, which compiles the runtime
without FFmpeg (`PSPRECOMP_HAVE_FFMPEG` undefined -> no video decode, black movies, audio still
plays). The fix makes `p3.sh build` pass the FFmpeg package and refuse to build a game exe without
one, then reconfigures the two variant builds.

**Tech Stack:** Bash (Git Bash on Windows), CMake + Ninja + llvm-mingw clang, FFmpeg 7.1.1 LGPL
shared package, Python 3 test drivers (`build/tools/p3drive.py`).

**Spec:** none (bug report from the user, 2026-10-17: "tanto a 1.1.5b y vanilla le hacen falta las
cinematicas, no se ven"). The diagnosis below is the spec.

## Diagnosis (done 2026-10-17, read before starting)

- The user's play logs: `build/logs/jugar.log` (1.1.4) has
  `[HLE] sceMpegAvcDecodeYCbCr: first picture 480x272 (ffmpeg=1)`;
  `build/logs/jugar_115b.log` and `build/logs/jugar_vanilla.log` have
  `[HLE] sceMpegAvcDecodeYCbCr: first picture 0x0 (ffmpeg=0)`. The PSMF/ringbuffer path runs the
  same in all three (same `sceMpegCreate` / `QueryStreamOffset` / `RegistStream` sequence); only the
  decoder is missing.
- `grep PSPRECOMP_FFMPEG_DIR build/{rtw,v115b/rtw,vanilla/rtw}/CMakeCache.txt`:
  `build/rtw` -> `C:/Users/torso/Documents/deco/tools/ffmpeg-7.1.1/ffmpeg-n7.1.1-54-g6400860b9d-win64-lgpl-shared-7.1`;
  both variant folders -> empty. Every other `PSPRECOMP_*` cache entry differs only by its folder.
- `runtime/CMakeLists.txt:283-308`: `PSPRECOMP_FFMPEG_DIR` empty -> `message(STATUS "FFmpeg: disabled")`
  and no `psp_ffmpeg` target; set -> links avcodec/avutil/swscale, defines `PSPRECOMP_HAVE_FFMPEG=1`
  and copies the FFmpeg DLLs next to the exe after each build (`psp_ffmpeg_copy_dlls`).
- `games/patapon3/scripts/p3.sh` `cmd_build` passes SDL2, zlib, banks and icon, never FFmpeg.

## Global Constraints

- Repo `C:/Users/torso/Documents/deco/psprecomp`, branch `patapon3-windows`; commit author is the
  repo-local identity "Patapon Community"; commit messages end with `Verified: ...` and
  `Agent: ...` lines and NO `Co-Authored-By` line. Do not push (the user decides when).
- Code and commits in English; talk to the user in Spanish.
- Never kill or disturb the user's game: it listens on port 9999. Test instances use port 9998
  (`PSPRECOMP_DEBUG_PORT=9998`); before running anything check
  `netstat -ano | grep -E ":9999 .*LISTEN"` and stop only exes under `build\v115b\` or `build\vanilla\`.
- Do not touch `build/rtw` beyond what Task 1 Step 6 says (it is the exe `jugar.bat` runs), and never
  write to `DECO/savedata*` (the user's saves); test runs copy saves into `build/tmp_saves/<tag>`.
- FFmpeg package path (the only one installed):
  `C:/Users/torso/Documents/deco/tools/ffmpeg-7.1.1/ffmpeg-n7.1.1-54-g6400860b9d-win64-lgpl-shared-7.1`
  (contains `include/`, `lib/*.dll.a`, `bin/*.dll`).
- Variant builds (from the 2026-10-17 session, commit 0d6d0eb): `P3_VARIANT=v115b` with
  `P3_ISO=/c/Users/torso/Downloads/INFN00001.iso P3_EBOOT=/c/Users/torso/Documents/deco/build/v115b/EBOOT_dec.BIN`;
  `P3_VARIANT=vanilla` with `P3_ISO="/c/Users/torso/Downloads/Patapon 3 (Europe).iso"
  P3_EBOOT=/c/Users/torso/Documents/deco/build/vanilla/EBOOT_dec.BIN`. Only the `build` step is
  needed here (analysis, banks and generated C++ are already in place).

## Review Focus

- A machine with no FFmpeg package: `p3.sh build` must stop with a message that names the expected
  folder and the `P3_FFMPEG` override, not silently build an exe without movies (Task 1 test 7).
- `P3_FFMPEG` pointing at a folder without `include/libavcodec`: refused with the same kind of
  message, not passed to CMake to fail later with a compiler error (Task 1 test 8).
- An existing build folder whose cache already has the right value (`build/rtw`): reconfiguring
  must keep it identical, so `jugar.bat`'s exe does not change behaviour (Task 1 Step 6).
- The FFmpeg DLLs must sit next to each variant exe (CMake copies them post-build); an exe that
  links avcodec but cannot find `avcodec-61.dll` would not start at all (Task 2 Step 2).
- A movie that decodes but never reaches the screen (texture upload/present path): checked with a
  screenshot during the opening movie, not only the log line (Task 2 Step 4).

---

### Task 1: `p3.sh build` passes FFmpeg and refuses to build without it

**Files:**
- Modify: `games/patapon3/scripts/p3.sh` (variables block near the top; `cmd_build`)
- Test: `games/patapon3/scripts/test_p3.sh` (append tests 7 and 8 before the summary)
- Modify: `README.md` (the Windows line that mentions `p3.sh all`)

**Interfaces:**
- Consumes: `P3_VARIANT` folders from 0d6d0eb (`$BUILD`, `$OUT`, `bank_dirs`).
- Produces: env override `P3_FFMPEG=<FFmpeg package root>`; default = the single directory under
  `$DECO/tools/ffmpeg-7.1.1/`; `cmd_build` passes `-DPSPRECOMP_FFMPEG_DIR=<windows-style path>`.

- [ ] **Step 1: Write the failing tests.** Append to `test_p3.sh`, right before
  `echo "$fails failure(s)"`. They use a fake `cmake` on `PATH` that records its arguments, so no
  real build runs:

```bash
# Fake toolchain for the build tests: cmake records its arguments and creates the -B folder.
mkdir -p "$TMP/bin"
cat >"$TMP/bin/cmake" <<'SH'
#!/usr/bin/env bash
echo "$*" >>"$CMAKE_LOG"
prev=""; for a in "$@"; do [[ "$prev" == "-B" ]] && mkdir -p "$a"; prev="$a"; done
exit 0
SH
chmod +x "$TMP/bin/cmake"
fake_deco() {   # $1 = deco root; with $2 = yes, also an FFmpeg package
    mkdir -p "$1/build/p3_output" "$1/tools/SDL2-2.32.10/x86_64-w64-mingw32/bin" "$1/tools/zlib-1.3.1"
    touch "$1/build/p3_output/CMakeLists.txt" "$1/tools/SDL2-2.32.10/x86_64-w64-mingw32/bin/SDL2.dll"
    if [[ "${2:-}" == yes ]]; then mkdir -p "$1/tools/ffmpeg-7.1.1/ffmpeg-x-lgpl-shared/include/libavcodec"; fi
}

# 7. build passes the FFmpeg package to CMake; without one it stops and says how to fix it.
fake_deco "$TMP/ff" yes
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/ff.log" DECO="$TMP/ff" bash "$P3" build 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$(cat "$TMP/ff.log")" == *"-DPSPRECOMP_FFMPEG_DIR="*"ffmpeg-x-lgpl-shared"* ]]; then
    pass "build passes FFmpeg"
else
    fail "build passes FFmpeg (rc=$rc, out: $out, cmake: $(cat "$TMP/ff.log" 2>/dev/null))"
fi
fake_deco "$TMP/noff"
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/noff.log" DECO="$TMP/noff" bash "$P3" build 2>&1)"; rc=$?
if [[ $rc -ne 0 && "$out" == *"FFmpeg"* && "$out" == *"P3_FFMPEG"* && ! -s "$TMP/noff.log" ]]; then
    pass "build without FFmpeg refused"
else
    fail "build without FFmpeg refused (rc=$rc, out: $out)"
fi

# 8. A P3_FFMPEG folder that is not an FFmpeg package is refused before CMake runs.
mkdir -p "$TMP/notff"
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/bad.log" DECO="$TMP/ff" P3_FFMPEG="$TMP/notff" bash "$P3" build 2>&1)"; rc=$?
if [[ $rc -ne 0 && "$out" == *"libavcodec"* && ! -s "$TMP/bad.log" ]]; then
    pass "bad P3_FFMPEG refused"
else
    fail "bad P3_FFMPEG refused (rc=$rc, out: $out)"
fi
```

- [ ] **Step 2: Run the tests and see 7 and 8 fail.**
  Run: `cd /c/Users/torso/Documents/deco/psprecomp && bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -6`
  Expected: tests 1-6 PASS; "FAIL: build passes FFmpeg" (no `-DPSPRECOMP_FFMPEG_DIR` in the cmake
  log), "FAIL: build without FFmpeg refused" (rc 0), "FAIL: bad P3_FFMPEG refused"; `3 failure(s)`.
  (Test 3 extracts the real ISO into a temp folder and takes about a minute.)

- [ ] **Step 3: Implement.** In `p3.sh`, below `ZLIB_SRC=...` add:

```bash
# FFmpeg LGPL shared package (include/, lib/, bin/): without it the runtime has no video decode
# and every movie stays black. P3_FFMPEG overrides the one unpacked under tools/ffmpeg-7.1.1/.
P3_FFMPEG="${P3_FFMPEG:-$(ls -d "$TOOLS"/ffmpeg-7.1.1/*/ 2>/dev/null | head -1)}"
P3_FFMPEG="${P3_FFMPEG%/}"
```

  Update the header comment line `# Env overrides: DECO (project root), P3_ISO, P3_EBOOT, JAVA_HOME, P3_ICON.`
  to `# Env overrides: DECO (project root), P3_ISO, P3_EBOOT, JAVA_HOME, P3_ICON, P3_FFMPEG.`

  In `cmd_build`, after the `ZLIB_SRC` check add:

```bash
    [[ -n "$P3_FFMPEG" ]] || die "FFmpeg package not found under $TOOLS/ffmpeg-7.1.1/ (movies need it; set P3_FFMPEG=/path/to/ffmpeg-...-lgpl-shared)"
    [[ -d "$P3_FFMPEG/include/libavcodec" ]] || die "P3_FFMPEG='$P3_FFMPEG' is not an FFmpeg package (no include/libavcodec)"
```

  and add this argument to the `cmake -G Ninja ...` configure line, next to
  `-DFETCHCONTENT_SOURCE_DIR_ZLIB="$ZLIB_SRC"`:

```bash
        -DPSPRECOMP_FFMPEG_DIR="$(cygpath -m "$P3_FFMPEG" 2>/dev/null || echo "$P3_FFMPEG")" \
```

- [ ] **Step 4: Run the tests again.**
  Run: `bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -10`
  Expected: 9 PASS lines (1-6, "build passes FFmpeg", "build without FFmpeg refused",
  "bad P3_FFMPEG refused"), `0 failure(s)`.

- [ ] **Step 5: README.** In `README.md`, in the Windows bullet that says
  "Patapon 3 has a one-command pipeline: `games/patapon3/scripts/p3.sh all`", add after that sentence:
  "Movies need the FFmpeg LGPL shared package unpacked under `$DECO/tools/ffmpeg-7.1.1/` (or
  `P3_FFMPEG=<package root>`); `p3.sh build` refuses to build without it."

- [ ] **Step 6: The 1.1.4 folder stays the same.** Check the user's game is not running
  (`netstat -ano | grep -E ":9999 .*LISTEN"` prints nothing), then run
  `bash games/patapon3/scripts/p3.sh build` (no `P3_VARIANT`). Expected: the CMake configure
  succeeds, `grep PSPRECOMP_FFMPEG_DIR /c/Users/torso/Documents/deco/build/rtw/CMakeCache.txt` shows
  the same path as before, and Ninja reports `no work to do` for `psprecomp_runtime` or relinks only
  (no recompilation of the runtime sources).

- [ ] **Step 7: Commit.**

```bash
git add games/patapon3/scripts/p3.sh games/patapon3/scripts/test_p3.sh README.md
git commit -F - <<'EOF'
fix(p3): build passes the FFmpeg package and refuses to build without it

The build folders created with P3_VARIANT (DxD 1.1.5b, vanilla) were configured with an empty
PSPRECOMP_FFMPEG_DIR, so their runtime had no video decode and every movie stayed black
("first picture 0x0 (ffmpeg=0)"); build/rtw only worked because its cache had been set by hand.
p3.sh now passes the package under tools/ffmpeg-7.1.1/ (P3_FFMPEG overrides) and stops with a
message when there is none.

Verified: test_p3.sh 9/9 (3 new: FFmpeg passed, missing FFmpeg refused, bad P3_FFMPEG refused);
build/rtw reconfigured with the same FFmpeg path
Agent: <model> (<skill>)
EOF
```

### Task 2: Rebuild the two variant exes and see their movies

**Files:**
- Create: `C:/Users/torso/Documents/deco/build/tools/movie_check.py` (DECO build tool, not in the repo)
- Modify: none in the repo (the variant build folders are regenerated by `p3.sh build`)

**Interfaces:**
- Consumes: Task 1's `p3.sh build`; `build/tools/p3drive.py` (`Instance`, `D`, `B`; env
  `P3DRIVE_EXE` = exe path relative to `build/rtw`, `P3DRIVE_DISC0` = disc0 folder,
  `P3DRIVE_SAVE` = save folder to copy).
- Produces: `movie_check.py <variant> <tag>` -> exit 0 and prints `MOVIE OK` when the opening movie
  decodes with FFmpeg and the frame on screen is not black.

- [ ] **Step 1: Rebuild both variants.** Stop leftover test instances of the variants only:
  `powershell -NoProfile -Command "Get-Process psprecomp_runtime -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '*\\v115b\\*' -or \$_.Path -like '*\\vanilla\\*' } | Stop-Process -Force"`.
  Then:

```bash
cd /c/Users/torso/Documents/deco/psprecomp
P3_VARIANT=v115b P3_ISO=/c/Users/torso/Downloads/INFN00001.iso \
  P3_EBOOT=/c/Users/torso/Documents/deco/build/v115b/EBOOT_dec.BIN bash games/patapon3/scripts/p3.sh build 2>&1 | tail -3
P3_VARIANT=vanilla P3_ISO="/c/Users/torso/Downloads/Patapon 3 (Europe).iso" \
  P3_EBOOT=/c/Users/torso/Documents/deco/build/vanilla/EBOOT_dec.BIN bash games/patapon3/scripts/p3.sh build 2>&1 | tail -3
```

  Expected: both finish with a link of `psprecomp_runtime.exe` (the runtime sources recompile once
  because `PSPRECOMP_HAVE_FFMPEG` is now defined), and
  `grep PSPRECOMP_FFMPEG_DIR /c/Users/torso/Documents/deco/build/{v115b,vanilla}/rtw/CMakeCache.txt`
  shows the FFmpeg path in both.

- [ ] **Step 2: Static check (fails before Step 1, passes after).** For each variant:

```bash
for v in v115b vanilla; do
  llvm-objdump -p /c/Users/torso/Documents/deco/build/$v/rtw/psprecomp_runtime.exe | grep -c "avcodec-61.dll"
  ls /c/Users/torso/Documents/deco/build/$v/rtw/ | grep -E "avcodec|avutil|swscale|swresample"
done
```

  Expected per variant: count `1` (the exe imports avcodec) and the four DLLs listed next to the exe.

- [ ] **Step 3: Write the movie check driver** `C:/Users/torso/Documents/deco/build/tools/movie_check.py`:

```python
"""movie_check.py <variant> <tag>: boot build/<variant>/rtw on port 9998 to the opening movie and
check it is really shown: the log has "first picture <W>x<H> (ffmpeg=1)" and a screenshot taken
during the movie is not black. Prints MOVIE OK / MOVIE FAIL <why>; exit code 0 / 1.

Saves: v115b uses a copy of build/tour_save (DxD); vanilla starts from an empty save dir, so the
first-run prompts are answered (language, autosave notice, create system data = Yes, install = No).
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image  # noqa: E402
from p3drive import Instance, B  # noqa: E402

variant, tag = sys.argv[1], sys.argv[2]
os.environ['P3DRIVE_EXE'] = f'../{variant}/rtw/psprecomp_runtime.exe'
os.environ['P3DRIVE_DISC0'] = f'{B}/{variant}/disc0'
if variant == 'vanilla':
    os.makedirs(f'{B}/tmp_saves/empty', exist_ok=True)
    os.environ['P3DRIVE_SAVE'] = f'{B}/tmp_saves/empty'
else:
    os.environ['P3DRIVE_SAVE'] = f'{B}/tour_save'

a = Instance(tag, port=9998)
a.launch()
ok, why = False, 'did not reach the movie'
try:
    a.wait_for('DATAMS.BND', 90)
    time.sleep(8)
    if variant == 'vanilla':
        # English is first in the vanilla list; Espanol is 4 rows down.
        for k, w in [('D', .5), ('D', .5), ('D', .5), ('D', .5), ('X', 6), ('X', 8),
                     ('U', 1), ('X', 10), ('D', 1), ('X', 15), ('START', 2)]:
            a.press(k, w)
        for _ in range(12):           # PSN terms pages, then the accept prompt
            a.press('X', 1.5)
        a.press('D', 1)
        a.press('X', 2)
    else:
        for _ in range(5):            # same as p3drive.boot_to_hideout's first half
            a.press('D', 0.5)
        a.press('X', 3)
        a.press('X', 10)
        a.press('START', 4)
        a.press('D', 1)
        a.press('X', 2)
    if a.wait_for('sceMpegAvcCsc', 120):
        time.sleep(6)
        a.shot('movie')
        log = open(a.log, encoding='utf-8', errors='replace').read()
        m = re.search(r'first picture (\d+)x(\d+) \(ffmpeg=(\d)\)', log)
        im = Image.open(f'{a.shots}/movie.tga').convert('L')
        mean = sum(im.getdata()) / (im.width * im.height)
        if not m or m.group(3) != '1' or m.group(1) == '0':
            why = f'decoder: {m.group(0) if m else "no first picture line"}'
        elif mean < 8:
            why = f'screen is black (mean {mean:.1f})'
        else:
            ok, why = True, f'{m.group(0)}, screen mean {mean:.1f}'
finally:
    a.kill()
print(('MOVIE OK ' if ok else 'MOVIE FAIL ') + why)
sys.exit(0 if ok else 1)
```

- [ ] **Step 4: Run it for both variants.**

```bash
cd /c/Users/torso/Documents/deco
python -I build/tools/movie_check.py v115b mv115b
python -I build/tools/movie_check.py vanilla mvvan
```

  Expected: `MOVIE OK first picture 480x272 (ffmpeg=1), screen mean <number above 8>` for both.
  Open `build/shots/mv115b/movie.tga` and `build/shots/mvvan/movie.tga` (convert with PIL if the
  viewer cannot read TGA) and look at them: a movie frame, not a menu or a black screen. If the
  vanilla sequence lands somewhere else (a prompt not listed above), take `a.shot()` screenshots
  after each press to find where it diverges and fix the key list; do not loosen the checks.

- [ ] **Step 5: 1.1.4 still plays its movie.** The graphics tour boots the play exe (`build/rtw`)
  through the opening movie before its first checkpoint, so run only up to that checkpoint:

```bash
cd /c/Users/torso/Documents/deco
TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py \
  --only rt --to hideout_cuartel --out C:/Users/torso/Documents/deco/build/tour/movies_t1
grep -a "first picture" build/logs/tour_rt.log
```

  Expected: the tour reaches `00 hideout_cuartel` and the grep prints
  `[HLE] sceMpegAvcDecodeYCbCr: first picture 480x272 (ffmpeg=1)`.

- [ ] **Step 6: Tell the user (Spanish)** that the movies decode in both builds, with the two
  screenshots, and ask them to try `build\v115b\jugar_115b.bat` and `build\vanilla\jugar_vanilla.bat`
  (opening movie, and in vanilla the story movie after "Nuevo juego"). Nothing to commit in this task
  (`build/tools` is outside the repo); add a line to the end of this plan under `## Findings` with
  the results and commit the plan:
  `git add docs/superpowers/plans/2026-10-17-variant-movies.md && git commit` with message
  `docs(plan): variant movies findings` and the `Verified:` / `Agent:` lines.

## Findings

- 2026-10-17 (executed inline): `p3.sh build` now passes FFmpeg (cba8b13, test_p3.sh 9/9). Both variant
  exes import `avcodec-61.dll` and play the opening movie: `movie_check.py v115b` and `vanilla` ->
  `MOVIE OK first picture 480x272 (ffmpeg=1)`, lit pixels 1.1% (JAPAN Studio logo) / ~80% (cinematic,
  checked by eye). 1.1.4 tour to `hideout_cuartel` still shows `first picture 480x272 (ffmpeg=1)`.
- Rulings: (1) the `ls -d` default needed `|| true` (set -euo pipefail killed p3.sh with no message
  when tools/ffmpeg-7.1.1/ was missing). (2) The movie check uses lit pixels (luma > 40) >= 0.5% in one
  of two shots (+6 s, +25 s) instead of mean luma >= 8: the movie opens on a logo over black (mean 1.2).
  (3) `build/v115b` main output and its three overlay banks were stale by fingerprint (generated before
  fbdb24a, an analyze-only change); regenerated with the current emitter into a scratch folder, all
  code files identical, so only `fingerprint.json`, `include/recomp_fingerprint.h` and
  `recompile_report.json` were copied in. (4) `build/rtw` (1.1.4, the exe `jugar.bat` runs) has the
  same stale fingerprint, so its configure stops before the FFmpeg line; its cache value is unchanged
  and its exe was not rebuilt (md5 6e1c006c...). Rebuilding 1.1.4 later needs the same metadata
  refresh (or `p3.sh recompile` + `overlays`), which also picks up ada529e in the play exe.
