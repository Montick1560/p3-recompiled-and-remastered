#!/usr/bin/env bash
# Behaviour tests for p3.sh (Git Bash). Uses the real ISO for the extract test.
#   bash games/patapon3/scripts/test_p3.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P3="$HERE/p3.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; fails=$((fails + 1)); }

# 1. A runtime that exits with an error must fail `run` with a clear message.
mkdir -p "$TMP/disc0/PSP_GAME"
out="$(P3_RUNTIME=/usr/bin/false P3_DISC0="$TMP/disc0" bash "$P3" run 5 2>&1)"; rc=$?
if [[ $rc -ne 0 && "$out" == *"runtime exited with status 1"* ]]; then
    pass "failing runtime fails run"
else
    fail "failing runtime fails run (rc=$rc, out: $out)"
fi

# 2. A runtime that is still running at the timeout (status 124) is a success.
out="$(P3_RUNTIME=/usr/bin/sleep P3_RUNTIME_ARGS=30 P3_DISC0="$TMP/disc0" bash "$P3" run 2 2>&1)"; rc=$?
if [[ $rc -eq 0 ]]; then pass "timeout is success"; else fail "timeout is success (rc=$rc, out: $out)"; fi

# 3. A partial extraction (no completion marker) must be redone, not skipped.
mkdir -p "$TMP/partial/PSP_GAME" && echo stub >"$TMP/partial/PSP_GAME/PARAM.SFO"
out="$(P3_DISC0="$TMP/partial" bash "$P3" extract 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" != *"already extracted"* && -f "$TMP/partial/PSP_GAME/USRDIR/DATAMS.HED" ]]; then
    pass "partial extract is redone"
else
    fail "partial extract is redone (rc=$rc, out: $out)"
fi

# 4. A completed extraction is skipped on the next call, even without the ISO.
out="$(P3_ISO=/no/such.iso P3_DISC0="$TMP/partial" bash "$P3" extract 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"already extracted"* ]]; then
    pass "complete extract is skipped"
else
    fail "complete extract is skipped (rc=$rc, out: $out)"
fi

# 5. P3_VARIANT keeps another release in build/<variant>/, apart from the default one.
mkdir -p "$TMP/deco/build/v9/disc0" && touch "$TMP/deco/build/v9/disc0/.complete"
out="$(DECO="$TMP/deco" P3_VARIANT=v9 P3_ISO=/no/such.iso bash "$P3" extract 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"already extracted at $TMP/deco/build/v9/disc0"* ]]; then
    pass "variant uses its own folder"
else
    fail "variant uses its own folder (rc=$rc, out: $out)"
fi

# 6. A variant name that could escape build/ is refused.
out="$(DECO="$TMP/deco" P3_VARIANT=../x bash "$P3" extract 2>&1)"; rc=$?
if [[ $rc -ne 0 && "$out" == *"bad P3_VARIANT"* ]]; then
    pass "bad variant refused"
else
    fail "bad variant refused (rc=$rc, out: $out)"
fi

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

echo "$fails failure(s)"
exit $((fails > 0))
