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
    src/psp_backtrace.cpp
    src/psp_cpu.cpp
    src/psp_scheduler.cpp
    src/psp_render_queue.cpp
    src/psp_event_loop.cpp
    src/psp_ge.cpp
    src/psp_ge_transfer.cpp
    src/psp_present.cpp
    src/psp_fpu.cpp
    src/psp_ge_sampler.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_lighting.cpp
    src/psp_ge_texture.cpp
    src/psp_ge_texdecode.cpp
    src/psp_ge_shader.cpp
    src/psp_ge_draw.cpp
    src/psp_ge_viewport.cpp
    src/psp_texrep_hash.cpp
    src/psp_texrep.cpp
    src/psp_texrep_pack.cpp
    src/psp_texrep_png.cpp
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
    src/hle/psp_hle_misc.cpp
    src/hle/psp_hle_ctrl.cpp
    src/hle/psp_hle_utility.cpp
    src/hle/psp_hle_sas.cpp
    src/hle/psp_sas_core.cpp
    src/hle/psp_atrac_ctx.cpp
    src/hle/psp_hle_atrac.cpp
    src/hle/psp_psmf.cpp
    src/hle/psp_hle_psmf.cpp
    src/hle/psp_savedata.cpp
    src/hle/psp_gamedata_install.cpp
    src/hle/psp_osk.cpp
    src/hle/psp_mpeg_demux.cpp
    src/hle/psp_hle_mpeg.cpp
    src/hle/psp_media_engine.cpp
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

# Vendored standalone ATRAC3/ATRAC3plus decoder (PPSSPP ext/at3_standalone,
# LGPL-2.1+; see third_party/at3_standalone/README.txt). Built as a static lib
# and linked into the runtime and the ATRAC unit test.
add_library(at3_standalone STATIC
    third_party/at3_standalone/atrac.cpp
    third_party/at3_standalone/atrac3.cpp
    third_party/at3_standalone/atrac3plus.cpp
    third_party/at3_standalone/atrac3plusdec.cpp
    third_party/at3_standalone/atrac3plusdsp.cpp
    third_party/at3_standalone/get_bits.cpp
    third_party/at3_standalone/compat.cpp
    third_party/at3_standalone/fft.cpp
    third_party/at3_standalone/mem.cpp
)
target_include_directories(at3_standalone PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/at3_standalone
)
# Third-party code: no warnings, always optimised (the decoder is hot).
target_compile_options(at3_standalone PRIVATE -w $<IF:$<CONFIG:Debug>,-O1,-O2>)

# Vendored xxHash (BSD-2; third_party/xxhash/README.txt): texture-replacement
# keys must match PPSSPP's, which hashes with this exact code.
add_library(xxhash STATIC third_party/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
target_compile_options(xxhash PRIVATE -w -O2)

set(PSPRECOMP_FFMPEG_DIR "" CACHE PATH
    "FFmpeg LGPL shared package root (include/, lib/*.dll.a, bin/*.dll). Empty disables video decode.")
if(PSPRECOMP_FFMPEG_DIR)
    add_library(psp_ffmpeg INTERFACE)
    target_include_directories(psp_ffmpeg INTERFACE "${PSPRECOMP_FFMPEG_DIR}/include")
    target_link_libraries(psp_ffmpeg INTERFACE
        "${PSPRECOMP_FFMPEG_DIR}/lib/libavcodec.dll.a"
        "${PSPRECOMP_FFMPEG_DIR}/lib/libavutil.dll.a"
        "${PSPRECOMP_FFMPEG_DIR}/lib/libswscale.dll.a")
    target_compile_definitions(psp_ffmpeg INTERFACE PSPRECOMP_HAVE_FFMPEG=1)
    if(WIN32)
        function(psp_ffmpeg_copy_dlls target)
            file(GLOB _ff_dlls
                "${PSPRECOMP_FFMPEG_DIR}/bin/avcodec-*.dll"
                "${PSPRECOMP_FFMPEG_DIR}/bin/avutil-*.dll"
                "${PSPRECOMP_FFMPEG_DIR}/bin/swscale-*.dll"
                "${PSPRECOMP_FFMPEG_DIR}/bin/swresample-*.dll")
            if(_ff_dlls)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ff_dlls} "$<TARGET_FILE_DIR:${target}>")
            endif()
        endfunction()
    endif()
    message(STATUS "FFmpeg: ${PSPRECOMP_FFMPEG_DIR}")
else()
    message(STATUS "FFmpeg: disabled")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb  # stb_image (texture replacement PNGs)
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
    ${PSPRECOMP_OUTPUT_DIR}          # funcs.h
    ${SDL2_INCLUDE_DIRS}
    ${PSP_GAME_INCLUDE_DIR}          # game-private headers (empty when generic)
)

target_link_libraries(psprecomp_runtime PRIVATE
    psp::recomp
    at3_standalone
    xxhash
    ${PSP_SDL2_LIBS}
    OpenGL::GL
    ZLIB::ZLIB
    pthread
    ${PSP_SOCKET_LIBS}
)
if(TARGET psp_ffmpeg)
    target_link_libraries(psprecomp_runtime PRIVATE psp_ffmpeg)
    if(WIN32)
        psp_ffmpeg_copy_dlls(psprecomp_runtime)
    endif()
endif()

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

# 59.94 Hz vblank clock unit test (header-only integer math).
# GE block transfer (memory path of PPSSPP DoBlockTransfer) on a fake RAM.
add_executable(test_ge_transfer tests/test_ge_transfer.cpp src/psp_ge_transfer.cpp)
target_include_directories(test_ge_transfer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_transfer PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_transfer_tests COMMAND test_ge_transfer)

# Present rectangle (aspect fit) and window scale parsing.
add_executable(test_present tests/test_present.cpp src/psp_present.cpp)
target_include_directories(test_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_present PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME present_tests COMMAND test_present)

add_executable(test_texrep_hash tests/test_texrep_hash.cpp src/psp_texrep_hash.cpp)
target_include_directories(test_texrep_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_texrep_hash PRIVATE xxhash)
target_compile_options(test_texrep_hash PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_hash_tests COMMAND test_texrep_hash)

add_executable(test_texrep_pack tests/test_texrep_pack.cpp src/psp_texrep_pack.cpp)
target_include_directories(test_texrep_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_texrep_pack PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_pack_tests COMMAND test_texrep_pack)

add_executable(test_texrep_png tests/test_texrep_png.cpp src/psp_texrep_png.cpp)
target_include_directories(test_texrep_png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_options(test_texrep_png PRIVATE -O2)
add_test(NAME texrep_png_tests COMMAND test_texrep_png)

# Host FPU mode from FCR31 (flush-to-zero).
add_executable(test_fpu tests/test_fpu.cpp src/psp_fpu.cpp)
target_include_directories(test_fpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_fpu PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME fpu_tests COMMAND test_fpu)

# GE sampler choice (TEXFILTER / TEXWRAP -> GL filter and wrap).
add_executable(test_ge_sampler tests/test_ge_sampler.cpp src/psp_ge_sampler.cpp)
target_include_directories(test_ge_sampler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_sampler PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_sampler_tests COMMAND test_ge_sampler)

add_executable(test_vblank_clock tests/test_vblank_clock.cpp)

target_include_directories(test_vblank_clock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_vblank_clock PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME vblank_clock_tests COMMAND test_vblank_clock)

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

# ATRAC3/ATRAC3plus playback context + sceAtrac HLE glue unit test: pure
# context, the vendored decoder and psp_hle_atrac.cpp; no SDL/GL/scheduler.
add_executable(test_atrac_ctx
    tests/test_atrac_ctx.cpp
    src/hle/psp_atrac_ctx.cpp
    src/hle/psp_hle_atrac.cpp   # real handlers, driven through a stub registry
)

target_include_directories(test_atrac_ctx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (recomp_context, psp_mem_*)
)

target_link_libraries(test_atrac_ctx PRIVATE at3_standalone)

target_compile_options(test_atrac_ctx PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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

add_test(NAME atrac_ctx_tests COMMAND test_atrac_ctx)
add_test(NAME savedata_tests COMMAND test_savedata)

# sceUtilityGamedataInstall back end unit test (pure: destination naming,
# copy-progress state machine over a temp directory with fake INSDIR files,
# PARAM.SFO contents, Memory Stick devctl descriptor) -- no SDL, no game files.
add_executable(test_gamedata_install
    tests/test_gamedata_install.cpp
    src/hle/psp_gamedata_install.cpp
    src/hle/psp_savedata.cpp  # Psf writer/parser for the SFO assertions
)

target_include_directories(test_gamedata_install PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hle  # psp_gamedata_install.h
)

target_compile_options(test_gamedata_install PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME gamedata_install_tests COMMAND test_gamedata_install)
# sceUtilityOsk back end unit test (pure: guest struct layout, UTF-8 ->
# UTF-16 conversion, completion writes against a fake guest RAM, bad-pointer
# safety) -- no SDL, no game files.
add_executable(test_osk
    tests/test_osk.cpp
    src/hle/psp_osk.cpp
)

target_include_directories(test_osk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hle  # psp_osk.h
)

target_compile_options(test_osk PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME osk_tests COMMAND test_osk)

# PSMF header parser unit test (pure: parser + synthetic/malformed headers; the
# real-data case reads the user's DATA_CMN.BND and is skipped when absent).
add_executable(test_psmf tests/test_psmf.cpp src/hle/psp_psmf.cpp)

target_include_directories(test_psmf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_psmf PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME psmf_tests COMMAND test_psmf)

# scePsmf HLE glue: real handlers through a stub registry, the PSMF parser,
# and a fake guest RAM. The real-data case reads DATA_CMN.BND.
add_executable(test_hle_psmf
    tests/test_hle_psmf.cpp
    src/hle/psp_hle_psmf.cpp
    src/hle/psp_psmf.cpp
)

target_include_directories(test_hle_psmf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include
)

target_compile_options(test_hle_psmf PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME hle_psmf_tests COMMAND test_hle_psmf)

add_executable(test_hle_misc
    tests/test_hle_misc.cpp
    src/hle/psp_hle_misc.cpp
    src/hle/psp_hle_power.cpp
)

target_include_directories(test_hle_misc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include
)

target_compile_options(test_hle_misc PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME hle_misc COMMAND test_hle_misc)

# MPEG-PS demuxer unit test (pure: byte queue, H.264 AU splitter, PES/pack
# parsing, ATRAC3plus frames; the real-data case streams the user's
# DATA_CMN.BND movie and is skipped when absent).
add_executable(test_mpeg_demux
    tests/test_mpeg_demux.cpp
    src/hle/psp_mpeg_demux.cpp
    src/hle/psp_psmf.cpp
)

target_include_directories(test_mpeg_demux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_mpeg_demux PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME mpeg_demux_tests COMMAND test_mpeg_demux)

add_executable(test_media_engine
    tests/test_media_engine.cpp
    src/hle/psp_media_engine.cpp
    src/hle/psp_mpeg_demux.cpp
    src/hle/psp_psmf.cpp
)
target_include_directories(test_media_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_media_engine PRIVATE at3_standalone)
if(TARGET psp_ffmpeg)
    target_link_libraries(test_media_engine PRIVATE psp_ffmpeg)
    if(WIN32)
        psp_ffmpeg_copy_dlls(test_media_engine)
    endif()
endif()
target_compile_options(test_media_engine PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)
add_test(NAME media_engine_tests COMMAND test_media_engine)

# sceMpeg HLE test: real handlers through a stubbed registry on a fake guest
# RAM; plays the user's opening movie (skipped when DATA_CMN.BND is absent).
add_executable(test_hle_mpeg
    tests/test_hle_mpeg.cpp
    src/hle/psp_hle_mpeg.cpp
    src/psp_cpu.cpp
    src/hle/psp_media_engine.cpp
    src/hle/psp_mpeg_demux.cpp
    src/hle/psp_psmf.cpp
)
target_include_directories(test_hle_mpeg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h (recomp_context, RECOMP_LOOKUP)
)
target_link_libraries(test_hle_mpeg PRIVATE at3_standalone)
if(TARGET psp_ffmpeg)
    target_link_libraries(test_hle_mpeg PRIVATE psp_ffmpeg)
    if(WIN32)
        psp_ffmpeg_copy_dlls(test_hle_mpeg)
    endif()
endif()
target_compile_options(test_hle_mpeg PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)
add_test(NAME hle_mpeg_tests COMMAND test_hle_mpeg)

# GE vertex transform unit test target (dothack-L6c fix). Links the real
# psp_ge_vertex.cpp and calls ge_transform_vertices directly to verify
# through-mode UV texel->[0,1] normalization. No SDL2/GL/scheduler deps
# (the function is pure float math); only needs recomp.h for the TU.
add_executable(test_ge_vertex
    tests/test_ge_vertex.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_lighting.cpp
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

# GE per-vertex lighting unit test (pure: links psp_ge_lighting.cpp only --
# no SDL2/GL/scheduler deps).
add_executable(test_ge_lighting
    tests/test_ge_lighting.cpp
    src/psp_ge_lighting.cpp
)

target_include_directories(test_ge_lighting PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_ge_lighting PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME ge_lighting_tests COMMAND test_ge_lighting)

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
