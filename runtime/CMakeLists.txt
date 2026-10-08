cmake_minimum_required(VERSION 3.20)
project(psprecomp_runtime CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # Windows (MinGW clang): SDL2 from the official SDL2-devel-*-mingw package
    # (pass -DSDL2_DIR=<pkg>/x86_64-w64-mingw32/lib/cmake/SDL2); zlib is built
    # from source since MinGW toolchains do not ship it.
    find_package(SDL2 REQUIRED CONFIG)
    set(PSP_SDL2_LIBS SDL2::SDL2)
    add_compile_definitions(SDL_MAIN_HANDLED)
    include(FetchContent)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    set(PSP_SOCKET_LIBS ws2_32)
else()
    # Find SDL2 via pkg-config (Homebrew installs pkg-config file)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(PSP_SDL2_LIBS PkgConfig::SDL2)

    # Find zlib (system zlib on macOS; pkg-config-discoverable on Linux)
    find_package(ZLIB REQUIRED)
    set(PSP_SOCKET_LIBS)
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# ── Recompiled game output directory (issue #47 Phase 1) ────────────────────
# The runtime is game-agnostic: it builds against ANY `psprecomp recompile`
# output directory. Default preserves the historical in-repo layout
# (../output, Patapon). Another game's output:
#   cmake -B build-hack -S runtime -DPSPRECOMP_OUTPUT_DIR=/abs/path/hack_output
set(PSPRECOMP_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../output" CACHE PATH
    "Recompiled game output directory (psprecomp recompile -o <dir>)")
get_filename_component(PSPRECOMP_OUTPUT_DIR "${PSPRECOMP_OUTPUT_DIR}" ABSOLUTE)
message(STATUS "psprecomp: output dir = ${PSPRECOMP_OUTPUT_DIR}")

if(NOT EXISTS "${PSPRECOMP_OUTPUT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "PSPRECOMP_OUTPUT_DIR does not contain a recompiled game "
        "(${PSPRECOMP_OUTPUT_DIR}/CMakeLists.txt not found).\n"
        "Generate it first:\n"
        "    PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile "
        "<analysis.json> -o ${PSPRECOMP_OUTPUT_DIR}\n"
        "or point at an existing output:\n"
        "    cmake ... -DPSPRECOMP_OUTPUT_DIR=/abs/path/to/output")
endif()

# ── Build fingerprint staleness check (issue #36) ───────────────────────────
# output/ is gitignored; building against a stale output/ after an emitter
# change silently invalidates all conclusions. At configure time, recompute
# the emitter-source content hash and compare with output/fingerprint.json
# (written by `psprecomp recompile`). Recipe: crates/psp-cli/src/fingerprint.rs
# and runtime/cmake/check_fingerprint.py (kept in sync; recipe version 1).
option(PSPRECOMP_ALLOW_STALE
    "Allow configuring against a stale output/ (fingerprint mismatch becomes a warning)" OFF)

set(PSPRECOMP_REGEN_CMD
    "PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json --config games/<id>/game.toml -o ${PSPRECOMP_OUTPUT_DIR}")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_fingerprint.py
            --fingerprint ${PSPRECOMP_OUTPUT_DIR}/fingerprint.json
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/..
        RESULT_VARIABLE PSPRECOMP_FP_RESULT
        OUTPUT_VARIABLE PSPRECOMP_FP_OUTPUT
        ERROR_VARIABLE PSPRECOMP_FP_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(PSPRECOMP_FP_RESULT EQUAL 0)
        message(STATUS "${PSPRECOMP_FP_OUTPUT}")
    elseif(PSPRECOMP_FP_RESULT EQUAL 2)
        if(PSPRECOMP_ALLOW_STALE)
            message(WARNING
                "BUILDING AGAINST STALE output/ (PSPRECOMP_ALLOW_STALE=ON):\n"
                "${PSPRECOMP_FP_OUTPUT}\n"
                "Runtime behavior will NOT reflect the current emitter sources.")
        else()
            message(FATAL_ERROR
                "output/ is stale relative to the emitter sources — run:\n"
                "    ${PSPRECOMP_REGEN_CMD}\n"
                "${PSPRECOMP_FP_OUTPUT}\n"
                "(override with -DPSPRECOMP_ALLOW_STALE=ON at your own risk)")
        endif()
    elseif(PSPRECOMP_FP_RESULT EQUAL 3)
        message(WARNING
            "output/fingerprint.json not found (output/ predates issue #36). "
            "Staleness cannot be checked — regenerate to enable it:\n"
            "    ${PSPRECOMP_REGEN_CMD}")
    else()
        message(WARNING
            "Fingerprint check could not verify output/ freshness "
            "(exit ${PSPRECOMP_FP_RESULT}):\n"
            "${PSPRECOMP_FP_OUTPUT}${PSPRECOMP_FP_ERROR}")
    endif()
else()
    message(WARNING
        "Python3 not found — skipping the output/ staleness check (issue #36).")
endif()

# Build the recompiler output as a subdirectory.
# The generated CMakeLists.txt produces lib{module_name}_recomp.a and the
# stable alias psp::recomp that the runtime links (issue #47 Phase 1).
add_subdirectory(${PSPRECOMP_OUTPUT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/recomp_build)

if(NOT TARGET psp::recomp)
    message(FATAL_ERROR
        "${PSPRECOMP_OUTPUT_DIR} does not define the psp::recomp ALIAS — "
        "it was generated by a psprecomp older than issue #47 Phase 1. "
        "Regenerate it:\n    ${PSPRECOMP_REGEN_CMD}")
endif()

# Runtime sources
set(RUNTIME_SOURCES
    src/main.cpp
    src/psp_memory.cpp
    src/psp_dispatch.cpp
    src/psp_overlay.cpp
    src/psp_audio_mixer.cpp
    src/psp_audio_out.cpp
    src/psp_subintr.cpp
    src/psp_scheduler.cpp
    src/psp_render_queue.cpp
    src/psp_event_loop.cpp
    src/psp_ge.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_texture.cpp
    src/psp_ge_texdecode.cpp
    src/psp_ge_shader.cpp
    src/psp_ge_draw.cpp
    src/psp_ge_viewport.cpp
    src/psp_ge_test.cpp
    src/hle/psp_hle_dispatch.cpp
    src/hle/psp_hle_kernel_thread.cpp
    src/hle/psp_hle_kernel_memory.cpp
    src/hle/psp_hle_kernel_sema.cpp
    src/hle/psp_hle_kernel_mutex.cpp
    src/hle/psp_hle_kernel_lwmutex.cpp
    src/hle/psp_hle_kernel_eventflag.cpp
    src/hle/psp_hle_io.cpp
    src/hle/psp_hle_display.cpp
    src/hle/psp_hle_intr.cpp
    src/hle/psp_hle_ge.cpp
    src/hle/psp_hle_power.cpp
    src/hle/psp_hle_ctrl.cpp
    src/hle/psp_hle_utility.cpp
    src/hle/psp_hle_sas.cpp
    src/hle/psp_sas_core.cpp
    src/hle/psp_savedata.cpp
    src/psp_vfpu_core.cpp
    src/psp_vfpu_arith.cpp
    src/psp_vfpu_trig.cpp
    src/psp_vfpu_matrix.cpp
    src/psp_vfpu_mem.cpp
    src/psp_vfpu_convert.cpp
    src/psp_vfpu_misc.cpp
    src/glad.c
    src/psp_debug_socket.cpp
)

# ── Game-module selection (issues #46/#47 Phase 4) ──────────────────────────
# Everything address-keyed or title-specific lives in games/<id>/runtime/,
# compiled in only when selected here (exactly one strong psp_game_module()
# definition per build). Default "patapon" preserves the documented Patapon
# build commands byte-for-byte; "none" builds the pure generic runtime with
# ZERO game hooks compiled in or registered.
#   cmake -B runtime/build -S runtime                       # Patapon (default)
#   cmake -B runtime/build-none -S runtime -DPSPRECOMP_GAME=none   # generic
set(PSPRECOMP_GAME "patapon" CACHE STRING
    "games/<id> module to compile in ('none' = generic build, zero game hooks)")
if(PSPRECOMP_GAME STREQUAL "none" OR PSPRECOMP_GAME STREQUAL "")
    list(APPEND RUNTIME_SOURCES src/psp_game_default.cpp)
    message(STATUS "psprecomp: game module = (none — generic build)")
else()
    # CONFIGURE_DEPENDS: newly added hook files are picked up without a manual
    # reconfigure (plan risk R7 — same trap class as the batch_*.cpp glob).
    file(GLOB PSP_GAME_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/runtime/*.cpp)
    if(NOT PSP_GAME_SOURCES)
        message(FATAL_ERROR
            "games/${PSPRECOMP_GAME}/runtime/ has no sources — unknown game id "
            "'${PSPRECOMP_GAME}' (use -DPSPRECOMP_GAME=none for a generic build).")
    endif()
    list(APPEND RUNTIME_SOURCES ${PSP_GAME_SOURCES})
    # Game-private headers (e.g. games/patapon/runtime/patapon_hooks.h,
    # asset_bnd.h) resolve only when the game module is compiled in — the
    # generic build cannot even include them by construction.
    set(PSP_GAME_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/runtime)
    message(STATUS "psprecomp: game module = ${PSPRECOMP_GAME}")
endif()

add_executable(psprecomp_runtime ${RUNTIME_SOURCES})

# ── Code-overlay banks (M2) ────────────────────────────────────────────────
# Each `psprecomp recompile --bank <name>` output dir defines an OBJECT
# library <name> whose objects self-register with psp_overlay.cpp.
set(PSPRECOMP_BANK_DIRS "" CACHE STRING
    "Overlay bank output directories (recompile --bank), ';'-separated")
set(_bank_index 0)
foreach(_bank_dir IN LISTS PSPRECOMP_BANK_DIRS)
    get_filename_component(_bank_dir "${_bank_dir}" ABSOLUTE)
    if(NOT EXISTS "${_bank_dir}/bank_dispatch.cpp")
        message(FATAL_ERROR "${_bank_dir} is not a recompile --bank output")
    endif()
    # A bank generated by an older emitter must not be linked: same staleness
    # check as the main output, and its recomp.h (context/struct layout) must
    # be byte-identical to the main one.
    if(Python3_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_fingerprint.py
                --fingerprint ${_bank_dir}/fingerprint.json
                --repo-root ${CMAKE_CURRENT_SOURCE_DIR}/..
            RESULT_VARIABLE _bank_fp_result
            OUTPUT_VARIABLE _bank_fp_output
            ERROR_VARIABLE _bank_fp_error)
        if(_bank_fp_result EQUAL 2 AND NOT PSPRECOMP_ALLOW_STALE)
            message(FATAL_ERROR
                "overlay bank ${_bank_dir} is stale relative to the emitter sources — "
                "regenerate the banks (games/<id>/scripts: overlays step).
${_bank_fp_output}")
        endif()
    endif()
    file(SHA256 "${_bank_dir}/include/recomp.h" _bank_recomp_h)
    file(SHA256 "${PSPRECOMP_OUTPUT_DIR}/include/recomp.h" _main_recomp_h)
    if(NOT _bank_recomp_h STREQUAL _main_recomp_h)
        message(FATAL_ERROR
            "overlay bank ${_bank_dir} was generated with a different recomp.h than "
            "${PSPRECOMP_OUTPUT_DIR} — regenerate the main output and the banks together.")
    endif()
    add_subdirectory(${_bank_dir} ${CMAKE_CURRENT_BINARY_DIR}/bank_${_bank_index})
    file(STRINGS "${_bank_dir}/CMakeLists.txt" _bank_line REGEX "^add_library\\(")
    string(REGEX REPLACE "^add_library\\(([A-Za-z0-9_]+) OBJECT.*" "\\1" _bank_target "${_bank_line}")
    target_link_libraries(psprecomp_runtime PRIVATE ${_bank_target})
    message(STATUS "psprecomp: overlay bank ${_bank_target} from ${_bank_dir}")
    math(EXPR _bank_index "${_bank_index} + 1")
endforeach()

target_include_directories(psprecomp_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
    ${PSPRECOMP_OUTPUT_DIR}          # funcs.h
    ${SDL2_INCLUDE_DIRS}
    ${PSP_GAME_INCLUDE_DIR}          # game-private headers (empty when generic)
)

target_link_libraries(psprecomp_runtime PRIVATE
    psp::recomp
    ${PSP_SDL2_LIBS}
    OpenGL::GL
    ZLIB::ZLIB
    pthread
    ${PSP_SOCKET_LIBS}
)

# Suppress OpenGL deprecation warnings on macOS
target_compile_definitions(psprecomp_runtime PRIVATE
    GL_SILENCE_DEPRECATION
)

target_compile_options(psprecomp_runtime PRIVATE
    $<IF:$<CONFIG:Debug>,-O0,-O2>
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# VFPU unit test target (links only VFPU sources, no SDL2/GL dependency)
set(VFPU_TEST_SOURCES
    tests/test_vfpu.cpp
    src/psp_vfpu_core.cpp
    src/psp_vfpu_arith.cpp
    src/psp_vfpu_trig.cpp
    src/psp_vfpu_matrix.cpp
    src/psp_vfpu_mem.cpp
    src/psp_vfpu_convert.cpp
    src/psp_vfpu_misc.cpp
)

add_executable(test_vfpu ${VFPU_TEST_SOURCES})

target_include_directories(test_vfpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
)

target_compile_options(test_vfpu PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_compile_definitions(test_vfpu PRIVATE
    GL_SILENCE_DEPRECATION
)

enable_testing()
add_test(NAME vfpu_tests COMMAND test_vfpu)

# Debug socket v2 unit test target (issue #35). Links ONLY the socket
# layer -- the I/S commands reach game state through the PspDebugHooks
# seams, so no SDL2/GL/recomp dependency is needed.
set(DEBUG_SOCKET_TEST_SOURCES
    tests/test_debug_socket.cpp
    src/psp_debug_socket.cpp
)

add_executable(test_debug_socket ${DEBUG_SOCKET_TEST_SOURCES})

target_include_directories(test_debug_socket PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_debug_socket PRIVATE
    pthread
    ${PSP_SOCKET_LIBS}
)

target_compile_options(test_debug_socket PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME debug_socket_tests COMMAND test_debug_socket)

# Refer*Status layout unit test target (dothack-L6 fix). The header under
# test (hle/psp_hle_refer_info.h) is pure layout/encoding logic, so the
# test links nothing beyond its own translation unit (test_vfpu convention:
# no SDL2/GL).
add_executable(test_refer_info tests/test_refer_info.cpp)

target_include_directories(test_refer_info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (psp_mem_read/write)
)

target_compile_options(test_refer_info PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME refer_info_tests COMMAND test_refer_info)

# Volatile-memory lock protocol unit test (hle/psp_hle_volatile_mem.h):
# pure protocol logic over guest memory, test_refer_info convention.
add_executable(test_volatile_mem tests/test_volatile_mem.cpp)

target_include_directories(test_volatile_mem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (psp_mem_read/write)
)

target_compile_options(test_volatile_mem PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME volatile_mem_tests COMMAND test_volatile_mem)

# Code-overlay bank resolution unit test (psp_overlay.cpp): links only the
# resolver — no SDL/GL/scheduler.
add_executable(test_overlay tests/test_overlay.cpp src/psp_overlay.cpp)

target_include_directories(test_overlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (PspOverlayBank, FuncPtr)
)

target_compile_options(test_overlay PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME overlay_tests COMMAND test_overlay)

# Pure GE texture decoder unit test (stride, swizzle, CLUT transform): no GL.
add_executable(test_ge_texdecode tests/test_ge_texdecode.cpp src/psp_ge_texdecode.cpp)

target_include_directories(test_ge_texdecode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include
)

target_compile_options(test_ge_texdecode PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_texdecode_tests COMMAND test_ge_texdecode)

# sceAudio output mixer unit test (pure: FIFOs, volume, saturation) — no SDL.
add_executable(test_audio_mixer tests/test_audio_mixer.cpp src/psp_audio_mixer.cpp)

target_include_directories(test_audio_mixer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_audio_mixer PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME audio_mixer_tests COMMAND test_audio_mixer)

# Sub-interrupt handler table unit test (pure bookkeeping, no guest code).
add_executable(test_subintr tests/test_subintr.cpp src/psp_subintr.cpp)

target_include_directories(test_subintr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_subintr PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME subintr_tests COMMAND test_subintr)

# SAS synthesis core unit test (pure: VAG decode, ADSR, mixing, bad-pointer
# safety) -- no SDL, no game files.
add_executable(test_sas_core tests/test_sas_core.cpp src/hle/psp_sas_core.cpp)

target_include_directories(test_sas_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_sas_core PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME sas_core_tests COMMAND test_sas_core)

# sceUtilitySavedata back end unit test (pure: PSF writer/parser, savedata
# modes against a temp directory and a fake guest RAM) -- no SDL, no game files.
add_executable(test_savedata tests/test_savedata.cpp src/hle/psp_savedata.cpp)

target_include_directories(test_savedata PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_savedata PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME savedata_tests COMMAND test_savedata)

# GE vertex transform unit test target (dothack-L6c fix). Links the real
# psp_ge_vertex.cpp and calls ge_transform_vertices directly to verify
# through-mode UV texel->[0,1] normalization. No SDL2/GL/scheduler deps
# (the function is pure float math); only needs recomp.h for the TU.
add_executable(test_ge_vertex
    tests/test_ge_vertex.cpp
    src/psp_ge_vertex.cpp
)

target_include_directories(test_ge_vertex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (psp_mem_read/write)
)

target_compile_options(test_ge_vertex PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_vertex_tests COMMAND test_ge_vertex)

# GE viewport/depth-range mapping unit test (issue #23). Pure math: links
# psp_ge_viewport.cpp only -- no SDL2/GL/scheduler deps.
add_executable(test_ge_viewport
    tests/test_ge_viewport.cpp
    src/psp_ge_viewport.cpp
)

target_include_directories(test_ge_viewport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_ge_viewport PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_viewport_tests COMMAND test_ge_viewport)

# BND parser unit test target (mirrors test_vfpu convention: no SDL2/GL,
# links the parser plus psp_memory + system zlib only). The BND layer is
# part of the Patapon game module (issue #47 Phase 5), so the target only
# exists when that module is selected.
if(PSPRECOMP_GAME STREQUAL "patapon")
    set(BND_TEST_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon/tests/test_asset_bnd.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon/runtime/asset_bnd.cpp
        src/psp_memory.cpp
    )

    add_executable(test_asset_bnd ${BND_TEST_SOURCES})

    target_include_directories(test_asset_bnd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
        ${PSP_GAME_INCLUDE_DIR}          # asset_bnd.h
    )

    target_link_libraries(test_asset_bnd PRIVATE
        ZLIB::ZLIB
        pthread
    )

    target_compile_options(test_asset_bnd PRIVATE
        -O2
        -Wall
        -Wextra
        -Wno-unused-parameter
    )

    target_compile_definitions(test_asset_bnd PRIVATE
        GL_SILENCE_DEPRECATION
    )

    add_test(NAME bnd_tests COMMAND test_asset_bnd)
endif()
