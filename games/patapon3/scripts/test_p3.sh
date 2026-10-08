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

echo "$fails failure(s)"
exit $((fails > 0))
