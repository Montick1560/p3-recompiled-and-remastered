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

# Fake PSP disc pieces for tests 9-11: a PARAM.SFO with DISC_ID / TITLE and an overlay file
# with the MWo3 header id at offset 4.
make_sfo() {   # $1 = out file, $2 = DISC_ID
    python -I - "$1" "$2" <<'PY'
import struct, sys
out, disc = sys.argv[1], sys.argv[2]
entries = [("DISC_ID", disc), ("TITLE", "PATAPON 3")]
keys = b""; data = b""; index = b""
for k, v in entries:
    val = v.encode() + b"\0"; size = (len(val) + 3) // 4 * 4
    index += struct.pack("<HHIII", len(keys), 0x0204, len(val), size, len(data))
    keys += k.encode() + b"\0"; data += val.ljust(size, b"\0")
keys = keys.ljust((len(keys) + 3) // 4 * 4, b"\0")
kstart = 20 + len(index)
open(out, "wb").write(b"\0PSF" + struct.pack("<IIII", 0x0101, kstart, kstart + len(keys), len(entries)) + index + keys + data)
PY
}
make_ovl() { printf 'MWo3' >"$1"; printf "\\x$(printf %02x "$2")\\x00\\x00\\x00" >>"$1"; head -c 64 /dev/zero >>"$1"; }

# 9. build writes release.json next to the exe (id from the variant, disc id, EBOOT hash).
fake_deco "$TMP/rel" yes
mkdir -p "$TMP/rel/disc0/PSP_GAME/SYSDIR"
echo "fake eboot" >"$TMP/rel/disc0/PSP_GAME/SYSDIR/EBOOT.BIN"
make_sfo "$TMP/rel/disc0/PSP_GAME/PARAM.SFO" INFN00001
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/rel.log" DECO="$TMP/rel" bash "$P3" build 2>&1)"; rc=$?
want="$(sha256sum "$TMP/rel/disc0/PSP_GAME/SYSDIR/EBOOT.BIN" | cut -d' ' -f1)"
rel="$(cat "$TMP/rel/build/rtw/release.json" 2>/dev/null)"
if [[ $rc -eq 0 && "$rel" == *'"id": "dxd-1.1.4"'* && "$rel" == *'"disc_id": "INFN00001"'* && "$rel" == *"\"eboot_sha256\": \"$want\""* && "$rel" == *'"saves"'* && "$rel" == *'"id": "UCES01421"'* && "$rel" == *'"last_slot": 10'* ]]; then
    pass "build writes release.json"
else
    fail "build writes release.json (rc=$rc, out: $out, json: $rel)"
fi

# 10. Without EBOOT.BIN / PARAM.SFO in disc0 the build still succeeds and says why there is no release.json.
fake_deco "$TMP/norel" yes
out="$(PATH="$TMP/bin:$PATH" CMAKE_LOG="$TMP/norel.log" DECO="$TMP/norel" bash "$P3" build 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"release.json not written"* && ! -f "$TMP/norel/build/rtw/release.json" ]]; then
    pass "no disc0 -> no release.json, build ok"
else
    fail "no disc0 -> no release.json, build ok (rc=$rc, out: $out)"
fi

# 11. Overlay banks come from the MWo3 header id, not the file name, and the folder may be "Overlay".
mkdir -p "$TMP/van/PSP_GAME/USRDIR/Overlay"
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Azito.bin" 2
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Mission.bin" 3
make_ovl "$TMP/van/PSP_GAME/USRDIR/Overlay/OL_Title.bin" 1
out="$(P3_DISC0="$TMP/van" bash "$P3" overlay-banks 2>&1)"; rc=$?
if [[ $rc -eq 0 && "$out" == *"OL_Azito.bin Mission"* && "$out" == *"OL_Mission.bin Title"* && "$out" == *"OL_Title.bin Azito"* ]]; then
    pass "overlay banks by header id"
else
    fail "overlay banks by header id (rc=$rc, out: $out)"
fi

echo "$fails failure(s)"
exit $((fails > 0))
