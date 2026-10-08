#!/usr/bin/env bash
# Patapon 3 build orchestration (Windows / Git Bash).
#   p3.sh extract | analyze | recompile | build | run [seconds] | all
# Env overrides: DECO (project root), P3_ISO, P3_EBOOT, JAVA_HOME.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DECO="${DECO:-$(cd "$REPO/.." && pwd)}"
P3_ISO="${P3_ISO:-$DECO/INFN00001.iso}"
P3_EBOOT="${P3_EBOOT:-$DECO/INFN00001_EBOOT.BIN}"
JAVA_HOME="${JAVA_HOME:-/c/Program Files/Java/jdk-26.0.1}"
TOOLS="$DECO/tools"
GHIDRA="$TOOLS/ghidra_12.0.2_PUBLIC"
SDL2_PKG="$TOOLS/SDL2-2.32.10/x86_64-w64-mingw32"
ZLIB_SRC="$TOOLS/zlib-1.3.1"
DISC0="$DECO/disc0"
ANALYSIS="$REPO/data/patapon3/analysis.json"
OUT="$DECO/build/p3_output"
BUILD="$DECO/build/rtw"
LOGS="$DECO/build/logs"

die() { echo "p3.sh: error: $*" >&2; exit 1; }
need_file() { [[ -f "$1" ]] || die "$2"; }

cmd_extract() {
    need_file "$P3_ISO" "ISO not found at '$P3_ISO' (set P3_ISO=/path/to/your.iso)"
    if [[ -f "$DISC0/PSP_GAME/PARAM.SFO" ]]; then
        echo "disc0 already extracted at $DISC0 (delete it to re-extract)"
        return
    fi
    python -I "$REPO/scripts/iso_extract.py" "$P3_ISO" "$DISC0"
}

cmd_analyze() {
    need_file "$P3_EBOOT" "decrypted EBOOT not found at '$P3_EBOOT'. Dump it with PPSSPP: Settings > Tools > Developer tools > 'Dump decrypted EBOOT.BIN on game boot', boot the game, copy memstick/PSP/SYSTEM/DUMP/*.BIN there (or set P3_EBOOT)"
    [[ -x "$JAVA_HOME/bin/java.exe" || -x "$JAVA_HOME/bin/java" ]] || die "Java not found in JAVA_HOME='$JAVA_HOME' (Ghidra needs JDK 21+)"
    [[ -d "$GHIDRA/Ghidra/Extensions/ghidra-allegrex" ]] || die "Ghidra 12.0.2 + ghidra-allegrex v21.3 not found under $GHIDRA"
    [[ -f "$REPO/data/niddb/ppsspp_niddb.xml" ]] || bash "$REPO/scripts/fetch-niddb.sh"
    mkdir -p "$(dirname "$ANALYSIS")"
    (cd "$REPO" && cargo build --release -q)
    # psprecomp resolves data/niddb/ relative to the working directory.
    (cd "$REPO" && PATH="$JAVA_HOME/bin:$PATH" JAVA_HOME="$JAVA_HOME" \
        ./target/release/psprecomp analyze --ghidra-dir "$GHIDRA" \
        --output "$ANALYSIS" "$P3_EBOOT")
}

cmd_recompile() {
    need_file "$ANALYSIS" "analysis.json missing — run: p3.sh analyze"
    (cd "$REPO" && cargo build --release -q)
    rm -rf "$OUT"   # stale batch_*.cpp files break the link (CMake globs them)
    (cd "$REPO" && PSPRECOMP_CROSS_MID=1 ./target/release/psprecomp recompile "$ANALYSIS" \
        --config games/patapon3/game.toml -o "$OUT" 2>&1 | tail -1)
}

cmd_build() {
    [[ -f "$OUT/CMakeLists.txt" ]] || die "generated C++ missing — run: p3.sh recompile"
    [[ -d "$SDL2_PKG" ]] || die "SDL2 mingw package not found at $SDL2_PKG"
    [[ -d "$ZLIB_SRC" ]] || die "zlib source not found at $ZLIB_SRC"
    cmake -G Ninja -B "$BUILD" -S "$REPO/runtime" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DPSPRECOMP_GAME=patapon3 -DPSPRECOMP_OUTPUT_DIR="$OUT" \
        -DSDL2_DIR="$SDL2_PKG/lib/cmake/SDL2" \
        -DFETCHCONTENT_SOURCE_DIR_ZLIB="$ZLIB_SRC" >/dev/null
    cmake --build "$BUILD" --target psprecomp_runtime
    cp -u "$SDL2_PKG/bin/SDL2.dll" "$BUILD/"
}

cmd_run() {
    local secs="${1:-60}"
    need_file "$BUILD/psprecomp_runtime.exe" "runtime not built — run: p3.sh build"
    [[ -d "$DISC0/PSP_GAME" ]] || die "disc0 missing — run: p3.sh extract"
    mkdir -p "$LOGS"
    local log="$LOGS/run-$(date +%Y%m%d-%H%M%S).log"
    echo "running for ${secs}s, log: $log"
    (cd "$BUILD" && PSPRECOMP_DISC0="$DISC0" timeout "$secs" ./psprecomp_runtime.exe >"$log" 2>&1) || true
    if grep -q 'Permission denied' "$log" && [[ $(wc -c <"$log") -lt 200 ]]; then
        # Windows Smart App Control blocks some unsigned builds by file hash;
        # a relink yields a new file that is usually allowed. Retry once.
        echo "exe blocked by Windows (Smart App Control?) — relinking and retrying once"
        rm -f "$BUILD/psprecomp_runtime.exe"
        cmake --build "$BUILD" --target psprecomp_runtime >/dev/null
        (cd "$BUILD" && PSPRECOMP_DISC0="$DISC0" timeout "$secs" ./psprecomp_runtime.exe >"$log" 2>&1) || true
    fi
    ln -sf "$log" "$LOGS/latest.log" 2>/dev/null || cp "$log" "$LOGS/latest.log"
    grep -E '\[PRESENT\] Frame' "$log" | tail -1 || true
    echo "log size: $(wc -c <"$log") bytes"
}

case "${1:-}" in
    extract)   cmd_extract ;;
    analyze)   cmd_analyze ;;
    recompile) cmd_recompile ;;
    build)     cmd_build ;;
    run)       shift; cmd_run "${1:-60}" ;;
    all)       cmd_extract; cmd_analyze; cmd_recompile; cmd_build; cmd_run 60 ;;
    *) echo "usage: $0 {extract|analyze|recompile|build|run [seconds]|all}" >&2; exit 2 ;;
esac
