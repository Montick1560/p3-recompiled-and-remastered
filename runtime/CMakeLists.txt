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
    set(PSP_SOCKET_LIBS ws2_32 advapi32)  # advapi32: MachineGuid for the ad hoc MAC file
    set(PSP_RANDOM_LIBS bcrypt)  # BCryptGenRandom (net/secure_random)
    set(PSP_WINHTTP_LIBS winhttp)  # infrastructure-mode WebSocket transport
else()
    # Find SDL2 via pkg-config (Homebrew installs pkg-config file)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(PSP_SDL2_LIBS PkgConfig::SDL2)

    # Find zlib (system zlib on macOS; pkg-config-discoverable on Linux)
    find_package(ZLIB REQUIRED)
    set(PSP_SOCKET_LIBS)
    set(PSP_WINHTTP_LIBS)
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
# [image] source = "disc" outputs include psp_eboot_loader.h from data_sections.cpp.
get_target_property(_recomp_target psp::recomp ALIASED_TARGET)
target_include_directories(${_recomp_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Runtime sources
set(RUNTIME_SOURCES
    src/main.cpp
    src/psp_memory.cpp
    src/psp_dispatch.cpp
    src/psp_overlay.cpp
    src/psp_audio_mixer.cpp
    src/psp_audio_out.cpp
    src/psp_time.cpp
    src/psp_rtc.cpp
    src/psp_beta.cpp
    src/psp_hw_info.cpp
    src/psp_subintr.cpp
    src/psp_backtrace.cpp
    src/psp_cpu.cpp
    src/psp_host_io.cpp
    src/psp_machine_id.cpp
    src/psp_perf.cpp
    src/psp_scheduler.cpp
    src/psp_render_queue.cpp
    src/psp_event_loop.cpp
    src/psp_ge.cpp
    src/psp_ge_transfer.cpp
    src/psp_ge_rtt.cpp
    src/psp_ini.cpp
    src/psp_settings.cpp
    src/psp_controls.cpp
    src/psp_input_device.cpp
    src/psp_prompts.cpp
    src/psp_prompt_raster.cpp
    src/psp_ge_texpatch.cpp
    src/psp_prompt_atlas.cpp
    src/psp_prompt_patcher.cpp
    src/psp_bar_text.cpp
    src/stb_truetype_impl.cpp
    src/psp_eboot_loader.cpp
    src/psp_menu_prompt.cpp
    src/psp_achievements.cpp
    src/psp_ach_store.cpp
    src/psp_toast.cpp
    src/psp_discord.cpp
    src/psp_presence_run.cpp
    src/psp_vblank_hook.cpp
    src/psp_achievements_run.cpp
    src/psp_present.cpp
    src/psp_fpu.cpp
    src/psp_ge_sampler.cpp
    src/psp_ge_vertex.cpp
    src/psp_ge_patch.cpp
    src/psp_ge_lighting.cpp
    src/psp_ge_texture.cpp
    src/psp_ge_texdecode.cpp
    src/psp_ge_shader.cpp
    src/psp_ge_draw.cpp
    src/psp_gl_state.cpp
    src/psp_ge_viewport.cpp
    src/psp_texrep_hash.cpp
    src/psp_texrep.cpp
    src/psp_texrep_pack.cpp
    src/psp_texrep_png.cpp
    src/psp_texrep_queue.cpp
    src/psp_pad.cpp
    src/psp_wheel.cpp
    src/psp_patch_sets.cpp
    src/psp_text_edit.cpp
    src/psp_osk_session.cpp
    src/psp_osk_keyboard.cpp
    src/psp_osk_skin.cpp
    src/psp_osk_overlay.cpp
    src/net/adhoc_config.cpp
    src/net/pro_protocol.cpp
    src/net/relay_protocol.cpp
    src/net/relay_conn.cpp
    src/net/infra_protocol.cpp
    src/net/infra_client.cpp
    src/net/link_stats.cpp
    src/net/infra_stats.cpp
    src/net/secure_random.cpp
    src/net/direct_crypto.cpp
    src/net/stun.cpp
    src/net/direct_path.cpp
    src/net/direct_link.cpp
    src/net/np2_structs.cpp
    src/net/np2_rooms.cpp
    src/net/p2p_stream.cpp
    src/net/pro_client.cpp
    src/net/adhocctl.cpp
    src/net/guest_callbacks.cpp
    src/net/netconf_adhoc.cpp
    src/net/matching_packet.cpp
    src/net/pdp.cpp
    src/net/matching.cpp
    src/net/ptp.cpp
    src/psp_ge_texmemo.cpp
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
    src/hle/psp_hle_net.cpp
    src/hle/psp_hle_net_matching.cpp
    src/hle/psp_hle_np.cpp
    src/hle/psp_hle_np_matching2.cpp
    src/hle/psp_hle_inet.cpp
    src/hle/psp_hle_ctrl.cpp
    src/hle/psp_hle_utility.cpp
    src/hle/psp_system_param.cpp
    src/hle/psp_hle_sas.cpp
    src/hle/psp_sas_core.cpp
    src/hle/psp_atrac_ctx.cpp
    src/hle/psp_hle_atrac.cpp
    src/hle/psp_psmf.cpp
    src/hle/psp_hle_psmf.cpp
    src/hle/psp_savedata.cpp
    src/hle/psp_save_crypt.cpp
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

# Vendored Monocypher (BSD-2/CC0; third_party/monocypher/README.md): the direct P2P path's AEAD and BLAKE2b.
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
target_compile_options(monocypher PRIVATE -w -O2)

# Vendored PPSSPP PRX decrypter (GPL-2.0; third_party/kirk/README.md): decrypts a "~PSP" boot
# file in memory for [image] source = "disc" builds (psp_eboot_loader.cpp).
# Also PPSSPP's savedata cipher (Core/HLE/sceChnnlsv.cpp), used by src/hle/psp_save_crypt.cpp.
file(GLOB KIRK_C_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/ext/libkirk/*.c)
add_library(kirk STATIC ${KIRK_C_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/Core/ELF/PrxDecrypter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/Core/HLE/sceChnnlsv.cpp)
target_include_directories(kirk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kirk/ext)
target_compile_options(kirk PRIVATE -w -O2)

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

# Game option catalog for settings.ini [game] (docs/contracts/settings-v1.md).
# game_options.json and the official achievement catalogue (docs/contracts/achievements-v1.md) next to the exe,
# read by the launcher. Copied whenever the source file changes, not only when the exe relinks.
set(_game_data_out "")
foreach(_f game_options.json achievements.json)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/${_f}")
    if(EXISTS "${_src}")
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_f}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${CMAKE_CURRENT_BINARY_DIR}/${_f}"
            DEPENDS "${_src}" VERBATIM)
        list(APPEND _game_data_out "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
    endif()
endforeach()
if(_game_data_out)
    add_custom_target(psp_game_data ALL DEPENDS ${_game_data_out})
endif()

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

# ── Beta builds of the online mode ─────────────────────────────────────────
# PSPRECOMP_BETA_UNTIL (ISO UTC, e.g. 2026-10-26T05:59:59Z): after it the exe shows "the beta ended" and exits
# with code 12 (src/psp_beta.cpp). Empty (default) = not a beta build, never expires.
set(PSPRECOMP_BETA_UNTIL "" CACHE STRING "Beta builds: ISO UTC end date (YYYY-MM-DDTHH:MM:SSZ); empty = no limit")
if(PSPRECOMP_BETA_UNTIL)
    if(NOT PSPRECOMP_BETA_UNTIL MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$")
        message(FATAL_ERROR "PSPRECOMP_BETA_UNTIL: '${PSPRECOMP_BETA_UNTIL}' is not YYYY-MM-DDTHH:MM:SSZ")
    endif()
    set_source_files_properties(src/psp_beta.cpp PROPERTIES
        COMPILE_DEFINITIONS "PSPRECOMP_BETA_UNTIL=\"${PSPRECOMP_BETA_UNTIL}\"")
    message(STATUS "psprecomp: beta build, plays until ${PSPRECOMP_BETA_UNTIL}")
endif()

# PSPRECOMP_WINDOW_TITLE names the game window (include/psp_window_title.h); empty = "PSPrecomp Runtime".
set(PSPRECOMP_WINDOW_TITLE "" CACHE STRING "Game window title; empty = PSPrecomp Runtime")
if(PSPRECOMP_WINDOW_TITLE)
    set_property(SOURCE src/psp_event_loop.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "PSPRECOMP_WINDOW_TITLE=\"${PSPRECOMP_WINDOW_TITLE}\"")
    message(STATUS "psprecomp: window title ${PSPRECOMP_WINDOW_TITLE}")
endif()

target_include_directories(psprecomp_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb  # stb_image (texture replacement PNGs)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann  # json.hpp (settings.ini game catalog)
    ${PSPRECOMP_OUTPUT_DIR}/include  # recomp.h
    ${PSPRECOMP_OUTPUT_DIR}          # funcs.h
    ${SDL2_INCLUDE_DIRS}
    ${PSP_GAME_INCLUDE_DIR}          # game-private headers (empty when generic)
)

target_link_libraries(psprecomp_runtime PRIVATE
    psp::recomp
    at3_standalone
    xxhash
    monocypher
    kirk
    ${PSP_SDL2_LIBS}
    OpenGL::GL
    ZLIB::ZLIB
    pthread
    ${PSP_SOCKET_LIBS}
    ${PSP_RANDOM_LIBS}
    ${PSP_WINHTTP_LIBS}
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

# HLE pacing helper unit test (psp_time.cpp): pace math and sleep landing.
add_executable(test_audio_pace tests/test_audio_pace.cpp src/psp_time.cpp)

target_include_directories(test_audio_pace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(test_audio_pace PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
)

add_test(NAME audio_pace_tests COMMAND test_audio_pace)

# Deterministic wall-clock override (psp_rtc.cpp): ISO parse, civil <-> unix
# round trip, the fixed clock, and the PSPRECOMP_RTC_FIXED env read.
add_executable(test_rtc_fixed tests/test_rtc_fixed.cpp src/psp_rtc.cpp)
target_include_directories(test_rtc_fixed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_rtc_fixed PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME rtc_fixed_tests COMMAND test_rtc_fixed)

# Beta builds: ISO UTC end date parse and the expiry rule (PSPRECOMP_BETA_UNTIL).
add_executable(test_beta tests/test_beta.cpp src/psp_beta.cpp src/psp_rtc.cpp)
target_include_directories(test_beta PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${SDL2_INCLUDE_DIRS})
target_compile_options(test_beta PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_beta PRIVATE ${PSP_SDL2_LIBS})
add_test(NAME beta_tests COMMAND test_beta)

# The hardware line logged at start (CPU, RAM, OS, GPU, GL, display): pure formatting.
add_executable(test_hw_info tests/test_hw_info.cpp src/psp_hw_info.cpp)
target_include_directories(test_hw_info PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_hw_info PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME hw_info_tests COMMAND test_hw_info)

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

# Render-to-texture helpers (matcher, overlap, texture span, blit rect). Pure, no GL.
add_executable(test_ge_rtt tests/test_ge_rtt.cpp src/psp_ge_rtt.cpp)
target_include_directories(test_ge_rtt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_rtt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_rtt_tests COMMAND test_ge_rtt)

# settings.ini v1 (docs/contracts/settings-v1.md): INI parser + key resolution. Pure.
add_executable(test_settings tests/test_settings.cpp src/psp_ini.cpp src/psp_settings.cpp src/psp_controls.cpp)
target_include_directories(test_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_definitions(test_settings PRIVATE
    PSP_CONTRACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs/contracts"
    PSP_GAME_OPTIONS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/game_options.json")
target_compile_options(test_settings PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME settings_tests COMMAND test_settings)

# The game window's title (PSPRECOMP_WINDOW_TITLE): the generic one, and a named build's.
add_executable(test_window_title tests/test_window_title.cpp)
target_include_directories(test_window_title PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME window_title_tests COMMAND test_window_title)
add_executable(test_window_title_named tests/test_window_title.cpp)
target_include_directories(test_window_title_named PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(test_window_title_named PRIVATE
    "PSPRECOMP_WINDOW_TITLE=\"Patapon 3 DxD Remastered\"" "EXPECT_TITLE=\"Patapon 3 DxD Remastered\"")
add_test(NAME window_title_named_tests COMMAND test_window_title_named)

# Display mode rules (display.window_mode): windowed / borderless / fullscreen, F11 toggle.
add_executable(test_window_mode tests/test_window_mode.cpp)
target_include_directories(test_window_mode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_window_mode PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME window_mode_tests COMMAND test_window_mode)

# EBOOT image loader ([image] source = "disc"): synthetic ELFs, plus the real EBOOTs of the
# three Patapon 3 releases when they are on this PC (skipped otherwise; never committed).
add_executable(test_eboot_loader tests/test_eboot_loader.cpp src/psp_eboot_loader.cpp)
target_include_directories(test_eboot_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_eboot_loader PRIVATE kirk)
target_compile_definitions(test_eboot_loader PRIVATE PSP_DECO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(test_eboot_loader PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME eboot_loader_tests COMMAND test_eboot_loader)

# Savedata cipher (PPSSPP sceChnnlsv, vendored in kirk) behind psp_save_crypt: round trips, hash
# check, and the user's PPSSPP save when it is on this PC (skipped otherwise).
add_executable(test_save_crypt tests/test_save_crypt.cpp src/hle/psp_save_crypt.cpp)
target_include_directories(test_save_crypt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_save_crypt PRIVATE kirk)
target_compile_definitions(test_save_crypt PRIVATE PSP_DECO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(test_save_crypt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME save_crypt_tests COMMAND test_save_crypt)

# Back-to-menu combo + prompt (pure).
add_executable(test_menu_prompt tests/test_menu_prompt.cpp src/psp_menu_prompt.cpp)
target_include_directories(test_menu_prompt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_menu_prompt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME menu_prompt_tests COMMAND test_menu_prompt)

# Achievement engine (pure): facts, catalogue compile, two-check rule, budget.
add_executable(test_achievements tests/test_achievements.cpp src/psp_achievements.cpp)
target_include_directories(test_achievements PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_achievements PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME achievements_tests COMMAND test_achievements)

# unlocked.json store: append-only, atomic, corrupt kept aside, newer never touched.
add_executable(test_ach_store tests/test_ach_store.cpp src/psp_ach_store.cpp)
target_include_directories(test_ach_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_ach_store PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ach_store_tests COMMAND test_ach_store)

# Achievement toast texts and timing (pure).
add_executable(test_toast tests/test_toast.cpp src/psp_toast.cpp)
target_include_directories(test_toast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_toast PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME toast_tests COMMAND test_toast)

# Discord IPC client: pure framing (the pipe itself is checked live).
add_executable(test_discord_frame tests/test_discord_frame.cpp src/psp_discord.cpp)
target_include_directories(test_discord_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_discord_frame PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME discord_frame_tests COMMAND test_discord_frame)

# Discord presence runner: send rule and off switches.
add_executable(test_presence_run tests/test_presence_run.cpp src/psp_presence_run.cpp src/psp_discord.cpp)
target_include_directories(test_presence_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_presence_run PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME presence_run_tests COMMAND test_presence_run)

# Achievements in a running game: hook, worker, unlocked.json, toasts (fake source, no GL).
add_executable(test_achievements_run tests/test_achievements_run.cpp src/psp_achievements_run.cpp
               src/psp_achievements.cpp src/psp_ach_store.cpp src/psp_toast.cpp src/psp_vblank_hook.cpp)
target_include_directories(test_achievements_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_achievements_run PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME achievements_run_tests COMMAND test_achievements_run)

# Patapon 3 field map + official catalogue over the user's decrypted saves (skipped when absent).
if(PSPRECOMP_GAME STREQUAL "patapon3")
    add_executable(test_p3_achievements tests/test_p3_achievements.cpp src/psp_achievements.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_achievements.cpp)
    target_include_directories(test_p3_achievements PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_definitions(test_p3_achievements PRIVATE
        PSP_P3_CATALOGUE="${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/achievements.json"
        PSP_P3_SAVES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../build/save_main")
    target_compile_options(test_p3_achievements PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_achievements_tests COMMAND test_p3_achievements)

    # Patapon 3 presence facts (place, class, level, mission name) read from guest RAM.
    add_executable(test_p3_presence tests/test_p3_presence.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_presence.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_achievements.cpp src/psp_achievements.cpp)
    target_include_directories(test_p3_presence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_options(test_p3_presence PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_presence_tests COMMAND test_p3_presence)

    # Patapon 3 Discord activity text (Spanish / English).
    add_executable(test_p3_presence_text tests/test_p3_presence_text.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_presence_text.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_presence.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_achievements.cpp src/psp_achievements.cpp)
    target_include_directories(test_p3_presence_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_options(test_p3_presence_text PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_presence_text_tests COMMAND test_p3_presence_text)

    # Patapon 3 help-bar word tables: drawable, inside their rows, fitting at >= 75 %.
    add_executable(test_p3_prompt_labels tests/test_p3_prompt_labels.cpp src/psp_bar_text.cpp
                   src/stb_truetype_impl.cpp ${CMAKE_CURRENT_BINARY_DIR}/bar_fonts_data.cpp  # embedded below
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_prompt_labels.cpp)
    target_include_directories(test_p3_prompt_labels PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_options(test_p3_prompt_labels PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_prompt_labels_tests COMMAND test_p3_prompt_labels)

    # Patapon 3 command-list word tables: the same checks for the 128x64 mission command-list atlas.
    add_executable(test_p3_command_labels tests/test_p3_command_labels.cpp src/psp_bar_text.cpp
                   src/stb_truetype_impl.cpp ${CMAKE_CURRENT_BINARY_DIR}/bar_fonts_data.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_command_labels.cpp)
    target_include_directories(test_p3_command_labels PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_options(test_p3_command_labels PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_command_labels_tests COMMAND test_p3_command_labels)
endif()

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

add_executable(test_texrep_queue tests/test_texrep_queue.cpp src/psp_texrep_queue.cpp)
target_include_directories(test_texrep_queue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_texrep_queue PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_texrep_queue PRIVATE pthread)
add_test(NAME texrep_queue_tests COMMAND test_texrep_queue)

# Header-only pure class (test_refer_info convention: no extra sources).
add_executable(test_texrep_lru tests/test_texrep_lru.cpp)
target_include_directories(test_texrep_lru PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_texrep_lru PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_lru_tests COMMAND test_texrep_lru)

# Header-only pure class (test_texrep_lru convention: no extra sources).
add_executable(test_texrep_share tests/test_texrep_share.cpp)
target_include_directories(test_texrep_share PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_texrep_share PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_share_tests COMMAND test_texrep_share)

add_executable(test_pad tests/test_pad.cpp src/psp_pad.cpp)
target_include_directories(test_pad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${SDL2_INCLUDE_DIRS})
target_compile_options(test_pad PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME pad_tests COMMAND test_pad)

add_executable(test_controls tests/test_controls.cpp src/psp_controls.cpp src/psp_pad.cpp)
target_include_directories(test_controls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_controls PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME controls_tests COMMAND test_controls)

# Last input device (keyboard / Xbox / DualShock 4 / DualSense) for button prompts. Pure.
add_executable(test_input_device tests/test_input_device.cpp src/psp_input_device.cpp)
target_include_directories(test_input_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_input_device PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME input_device_tests COMMAND test_input_device)

# Button prompt icon choice (glyph x device x style x control map). Pure.
add_executable(test_prompts tests/test_prompts.cpp src/psp_prompts.cpp src/psp_controls.cpp src/psp_input_device.cpp)
target_include_directories(test_prompts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_prompts PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME prompts_tests COMMAND test_prompts)

# Button-prompt icons (Kenney Input Prompts 1.5, CC0) embedded in the exe: no data folder to ship.
file(GLOB PSP_PROMPT_ICON_PNGS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/button_prompts/*.png)
set(PSP_PROMPT_ICONS_CPP ${CMAKE_CURRENT_BINARY_DIR}/prompt_icons_data.cpp)
add_custom_command(OUTPUT ${PSP_PROMPT_ICONS_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${PSP_PROMPT_ICONS_CPP} -DSYMBOL=g_prompt_icons
            "-DFILES=${PSP_PROMPT_ICON_PNGS}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    DEPENDS ${PSP_PROMPT_ICON_PNGS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    COMMENT "Embedding button-prompt icons" VERBATIM)
target_sources(psprecomp_runtime PRIVATE ${PSP_PROMPT_ICONS_CPP})

# Button-prompt raster helpers + the embedded icon set. Pure (no GL).
add_executable(test_prompt_raster tests/test_prompt_raster.cpp src/psp_prompt_raster.cpp src/psp_prompts.cpp
    src/psp_controls.cpp src/psp_input_device.cpp src/psp_texrep_png.cpp ${PSP_PROMPT_ICONS_CPP})
target_include_directories(test_prompt_raster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_options(test_prompt_raster PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME prompt_raster_tests COMMAND test_prompt_raster)

# Texture patch seam (recognition memo, per-list variant latch). Pure (no GL).
add_executable(test_ge_texpatch tests/test_ge_texpatch.cpp src/psp_ge_texpatch.cpp src/psp_ge_texdecode.cpp)
target_include_directories(test_ge_texpatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_texpatch PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_texpatch_tests COMMAND test_ge_texpatch)

# Bar font (Zen Maru Gothic Bold, SIL OFL 1.1) embedded in the exe for the sharp help-bar text.
file(GLOB PSP_BAR_FONT_TTFS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf)
set(PSP_BAR_FONTS_CPP ${CMAKE_CURRENT_BINARY_DIR}/bar_fonts_data.cpp)
add_custom_command(OUTPUT ${PSP_BAR_FONTS_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${PSP_BAR_FONTS_CPP} -DSYMBOL=g_bar_fonts
            "-DFILES=${PSP_BAR_FONT_TTFS}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    DEPENDS ${PSP_BAR_FONT_TTFS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    COMMENT "Embedding the bar font" VERBATIM)
target_sources(psprecomp_runtime PRIVATE ${PSP_BAR_FONTS_CPP})

# Bar text: embedded font + word rasteriser. Pure (no GL).
add_executable(test_bar_text tests/test_bar_text.cpp src/psp_bar_text.cpp src/stb_truetype_impl.cpp
    ${PSP_BAR_FONTS_CPP})
target_include_directories(test_bar_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_options(test_bar_text PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME bar_text_tests COMMAND test_bar_text)

# Button-prompt atlas patcher: recognition signature + cell composition. Pure (no GL).
add_executable(test_prompt_atlas tests/test_prompt_atlas.cpp src/psp_prompt_atlas.cpp src/psp_prompt_raster.cpp
    src/psp_prompts.cpp src/psp_controls.cpp src/psp_input_device.cpp src/psp_texrep_png.cpp
    src/psp_ge_texpatch.cpp src/psp_ge_texdecode.cpp ${PSP_PROMPT_ICONS_CPP}
    src/psp_bar_text.cpp src/stb_truetype_impl.cpp ${PSP_BAR_FONTS_CPP})
target_include_directories(test_prompt_atlas PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_options(test_prompt_atlas PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME prompt_atlas_tests COMMAND test_prompt_atlas)

if(PSPRECOMP_GAME STREQUAL "patapon3")
    # Patapon 3 drum-hit words: the drum specs on the recon dumps (build/prompts_ref/p3drums).
    add_executable(test_p3_drum_prompts tests/test_p3_drum_prompts.cpp src/psp_prompt_atlas.cpp
        src/psp_prompt_raster.cpp src/psp_prompts.cpp src/psp_controls.cpp src/psp_input_device.cpp
        src/psp_texrep_png.cpp ${PSP_PROMPT_ICONS_CPP} src/psp_bar_text.cpp src/stb_truetype_impl.cpp
        ${PSP_BAR_FONTS_CPP} ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_prompts.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_prompt_labels.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_command_labels.cpp)
    target_include_directories(test_p3_drum_prompts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_definitions(test_p3_drum_prompts PRIVATE
        PSP_P3_DRUMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../build/prompts_ref/p3drums")
    target_compile_options(test_p3_drum_prompts PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_drum_prompts_tests COMMAND test_p3_drum_prompts)
endif()

add_executable(test_perf tests/test_perf.cpp src/psp_perf.cpp)
target_include_directories(test_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_perf PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME perf_tests COMMAND test_perf)

add_executable(test_tex_memo tests/test_tex_memo.cpp src/psp_ge_texmemo.cpp)
target_include_directories(test_tex_memo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_tex_memo PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME tex_memo_tests COMMAND test_tex_memo)

# Header-only pure policy (test_texrep_lru convention: no extra sources).
add_executable(test_ge_texcache tests/test_ge_texcache.cpp)
target_include_directories(test_ge_texcache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_texcache PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_texcache_tests COMMAND test_ge_texcache)

# Header-only pure policy: shadow state of the render thread's GL calls
# (psp_gl_state.cpp). No GL / SDL dependency.
add_executable(test_gl_shadow tests/test_gl_shadow.cpp)
target_include_directories(test_gl_shadow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_gl_shadow PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME gl_shadow_tests COMMAND test_gl_shadow)

# GE draw batching: flush rule (PPSSPP command flags) and strip/fan -> list indices. Header-only.
add_executable(test_ge_batch tests/test_ge_batch.cpp)
target_include_directories(test_ge_batch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_batch PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_batch_tests COMMAND test_ge_batch)

# GE stencil registers -> GL stencil state (PPSSPP ConvertStencilFuncState port). Header-only.
add_executable(test_ge_stencil tests/test_ge_stencil.cpp)
target_include_directories(test_ge_stencil PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_stencil PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_stencil_tests COMMAND test_ge_stencil)

# GE fog helpers (float24 coefs, factor, colour; PPSSPP semantics). Header-only.
add_executable(test_ge_fog tests/test_ge_fog.cpp)
target_include_directories(test_ge_fog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_fog PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_fog_tests COMMAND test_ge_fog)

# PSPRECOMP_PATCHES parsing (opt-in patch sets). Header-only.
add_executable(test_patch_sets tests/test_patch_sets.cpp)
target_include_directories(test_patch_sets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_patch_sets PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME patch_sets_tests COMMAND test_patch_sets)

# Pose blending for smooth animation (two whole-frame samples). Header-only.
add_executable(test_anim_blend tests/test_anim_blend.cpp)
target_include_directories(test_anim_blend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_anim_blend PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME anim_blend_tests COMMAND test_anim_blend)

add_executable(test_ge_list_blend tests/test_ge_list_blend.cpp)
target_include_directories(test_ge_list_blend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_list_blend PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_list_blend_tests COMMAND test_ge_list_blend)

add_executable(test_paced_step tests/test_paced_step.cpp)
target_include_directories(test_paced_step PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_paced_step PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME paced_step_tests COMMAND test_paced_step)

# Relay-link telemetry of an infrastructure session (link_stats).
add_executable(test_link_stats tests/test_link_stats.cpp src/net/link_stats.cpp)
target_include_directories(test_link_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_link_stats PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME link_stats_tests COMMAND test_link_stats)

add_executable(test_once_per_count tests/test_once_per_count.cpp)
target_include_directories(test_once_per_count PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_once_per_count PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME once_per_count_tests COMMAND test_once_per_count)

add_executable(test_particle_nudge tests/test_particle_nudge.cpp)
target_include_directories(test_particle_nudge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_particle_nudge PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME particle_nudge_tests COMMAND test_particle_nudge)

add_executable(test_half_ahead tests/test_half_ahead.cpp)
target_include_directories(test_half_ahead PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_half_ahead PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME half_ahead_tests COMMAND test_half_ahead)

add_executable(test_code_match tests/test_code_match.cpp)
target_include_directories(test_code_match PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_code_match PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME code_match_tests COMMAND test_code_match)

add_executable(test_frame_wait tests/test_frame_wait.cpp)
target_include_directories(test_frame_wait PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_frame_wait PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME frame_wait_tests COMMAND test_frame_wait)

add_executable(test_frame_latch tests/test_frame_latch.cpp)
target_include_directories(test_frame_latch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_frame_latch PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME frame_latch_tests COMMAND test_frame_latch)

add_executable(test_cpu_close tests/test_cpu_close.cpp src/psp_cpu.cpp src/psp_perf.cpp)
target_include_directories(test_cpu_close PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_cpu_close PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME cpu_close_tests COMMAND test_cpu_close)
add_test(NAME cpu_close_nolock_tests COMMAND test_cpu_close --nolock)
set_tests_properties(cpu_close_nolock_tests PROPERTIES ENVIRONMENT "PSPRECOMP_NO_CPU_LOCK=1")
add_test(NAME cpu_close_closer_tests COMMAND test_cpu_close --closer)

# Guest CPU lock under contention: exclusion, progress, priority, reservations, hand-off time.
add_executable(test_cpu_handoff tests/test_cpu_handoff.cpp src/psp_cpu.cpp src/psp_perf.cpp)
target_include_directories(test_cpu_handoff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_cpu_handoff PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME cpu_handoff_tests COMMAND test_cpu_handoff)
set_tests_properties(cpu_close_closer_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "the closing thread asked for the CPU after close: exiting now")

add_executable(test_host_io tests/test_host_io.cpp src/psp_host_io.cpp)
target_include_directories(test_host_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_host_io PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME host_io_tests COMMAND test_host_io)

add_executable(test_log_verbose tests/test_log_verbose.cpp)
target_include_directories(test_log_verbose PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME log_verbose_tests COMMAND test_log_verbose)
set_tests_properties(log_verbose_tests PROPERTIES ENVIRONMENT "PSPRECOMP_LOG_VERBOSE=")

add_executable(test_on_exit tests/test_on_exit.cpp)
target_include_directories(test_on_exit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME on_exit_quick_tests COMMAND test_on_exit --quick)
set_tests_properties(on_exit_quick_tests PROPERTIES PASS_REGULAR_EXPRESSION "on_exit handler ran")
add_test(NAME on_exit_normal_tests COMMAND test_on_exit)
set_tests_properties(on_exit_normal_tests PROPERTIES PASS_REGULAR_EXPRESSION "on_exit handler ran")

add_executable(test_wheel tests/test_wheel.cpp src/psp_wheel.cpp)
target_include_directories(test_wheel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_wheel PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME wheel_tests COMMAND test_wheel)

add_executable(test_system_param tests/test_system_param.cpp src/hle/psp_system_param.cpp)
target_include_directories(test_system_param PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_system_param PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME system_param_tests COMMAND test_system_param)

add_executable(test_text_edit tests/test_text_edit.cpp src/psp_text_edit.cpp)
target_include_directories(test_text_edit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_text_edit PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME text_edit_tests COMMAND test_text_edit)

add_executable(test_osk_session tests/test_osk_session.cpp src/psp_osk_session.cpp src/psp_text_edit.cpp)
target_include_directories(test_osk_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_osk_session PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_osk_session PRIVATE pthread)
add_test(NAME osk_session_tests COMMAND test_osk_session)

add_executable(test_osk_keyboard tests/test_osk_keyboard.cpp src/psp_osk_keyboard.cpp src/psp_osk_skin.cpp)
target_include_directories(test_osk_keyboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_osk_keyboard PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME osk_keyboard_tests COMMAND test_osk_keyboard)

add_executable(test_stack_layout tests/test_stack_layout.cpp)
target_include_directories(test_stack_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_stack_layout PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME stack_layout_tests COMMAND test_stack_layout)

add_executable(test_adhoc_config tests/test_adhoc_config.cpp src/net/adhoc_config.cpp src/psp_host_io.cpp
    src/psp_machine_id.cpp)
target_link_libraries(test_adhoc_config PRIVATE ${PSP_SOCKET_LIBS})
target_include_directories(test_adhoc_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_adhoc_config PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME adhoc_config_tests COMMAND test_adhoc_config)

add_executable(test_relay_protocol tests/test_relay_protocol.cpp src/net/relay_protocol.cpp)
target_include_directories(test_relay_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_relay_protocol PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME relay_protocol_tests COMMAND test_relay_protocol)

# Direct P2P: vendored Monocypher against published vectors, and the OS CSPRNG.
add_executable(test_direct_crypto_primitives tests/test_direct_crypto_primitives.cpp src/net/secure_random.cpp)
target_include_directories(test_direct_crypto_primitives PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_direct_crypto_primitives PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_direct_crypto_primitives PRIVATE monocypher ${PSP_RANDOM_LIBS})
add_test(NAME direct_crypto_primitives_tests COMMAND test_direct_crypto_primitives)

add_executable(test_direct_crypto tests/test_direct_crypto.cpp src/net/direct_crypto.cpp)
target_include_directories(test_direct_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_direct_crypto PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_direct_crypto PRIVATE monocypher)
add_test(NAME direct_crypto_tests COMMAND test_direct_crypto)

add_executable(test_stun tests/test_stun.cpp src/net/stun.cpp src/net/direct_crypto.cpp)
target_include_directories(test_stun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_stun PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_stun PRIVATE monocypher)
add_test(NAME stun_tests COMMAND test_stun)

add_executable(test_direct_path tests/test_direct_path.cpp src/net/direct_path.cpp src/net/direct_crypto.cpp)
target_include_directories(test_direct_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_direct_path PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_direct_path PRIVATE monocypher)
add_test(NAME direct_path_tests COMMAND test_direct_path)

# Direct P2P I/O shell on real loopback UDP sockets.
add_executable(test_direct_link tests/test_direct_link.cpp src/net/direct_link.cpp src/net/direct_path.cpp
               src/net/direct_crypto.cpp src/net/stun.cpp src/net/secure_random.cpp)
target_include_directories(test_direct_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_direct_link PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_direct_link PRIVATE monocypher pthread ${PSP_SOCKET_LIBS} ${PSP_RANDOM_LIBS})
add_test(NAME direct_link_tests COMMAND test_direct_link)

# Infrastructure-mode server frames (tools/infra_server.py wire format).
add_executable(test_infra_protocol tests/test_infra_protocol.cpp src/net/infra_protocol.cpp)
target_include_directories(test_infra_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_infra_protocol PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME infra_protocol_tests COMMAND test_infra_protocol)

# Infrastructure-mode client against an in-process fake server.
add_executable(test_infra_client tests/test_infra_client.cpp src/net/infra_client.cpp src/net/infra_stats.cpp
               src/net/link_stats.cpp
               src/net/direct_link.cpp src/net/direct_path.cpp src/net/direct_crypto.cpp src/net/stun.cpp
               src/net/secure_random.cpp
               src/net/infra_protocol.cpp src/net/relay_conn.cpp src/net/relay_protocol.cpp)
target_include_directories(test_infra_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_infra_client PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_infra_client PRIVATE monocypher pthread ${PSP_SOCKET_LIBS} ${PSP_RANDOM_LIBS} ${PSP_WINHTTP_LIBS})
add_test(NAME infra_client_tests COMMAND test_infra_client)

# PSPRECOMP_INFRA_STATS network event CSV, and the queue age a pop reports.
add_executable(test_infra_stats tests/test_infra_stats.cpp src/net/infra_stats.cpp src/net/infra_client.cpp
               src/net/link_stats.cpp
               src/net/direct_link.cpp src/net/direct_path.cpp src/net/direct_crypto.cpp src/net/stun.cpp
               src/net/secure_random.cpp
               src/net/infra_protocol.cpp src/net/relay_conn.cpp src/net/relay_protocol.cpp)
target_include_directories(test_infra_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_infra_stats PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_infra_stats PRIVATE monocypher pthread ${PSP_SOCKET_LIBS} ${PSP_RANDOM_LIBS} ${PSP_WINHTTP_LIBS})
add_test(NAME infra_stats_tests COMMAND test_infra_stats)

# sceNpMatching2 response structs written into guest memory.
add_executable(test_np2_structs tests/test_np2_structs.cpp src/net/np2_structs.cpp src/net/infra_protocol.cpp)
target_include_directories(test_np2_structs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_np2_structs PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME np2_structs_tests COMMAND test_np2_structs)

# Matching2 state of every room the game is in (hideout + VS), by room id (pure).
add_executable(test_np2_rooms tests/test_np2_rooms.cpp src/net/np2_rooms.cpp)
target_include_directories(test_np2_rooms PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_np2_rooms PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME np2_rooms_tests COMMAND test_np2_rooms)

# sceNetInet errno -> sceNetInetGetPspError (pure).
add_executable(test_inet_errno tests/test_inet_errno.cpp)
target_include_directories(test_inet_errno PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_inet_errno PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME inet_errno_tests COMMAND test_inet_errno)

# The game's TCP P2P sockets as relayed segments (pure).
add_executable(test_p2p_stream tests/test_p2p_stream.cpp src/net/p2p_stream.cpp)
target_include_directories(test_p2p_stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_p2p_stream PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME p2p_stream_tests COMMAND test_p2p_stream)

# Signal strength reported to the game from the server RTT (pure).
add_executable(test_infra_signal tests/test_infra_signal.cpp)
target_include_directories(test_infra_signal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_infra_signal PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME infra_signal_tests COMMAND test_infra_signal)

add_executable(test_pro_protocol tests/test_pro_protocol.cpp src/net/pro_protocol.cpp)
target_include_directories(test_pro_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_pro_protocol PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME pro_protocol_tests COMMAND test_pro_protocol)

add_executable(test_pro_client tests/test_pro_client.cpp src/net/pro_client.cpp src/net/pro_protocol.cpp)
target_include_directories(test_pro_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_pro_client PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_pro_client PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME pro_client_tests COMMAND test_pro_client)

add_executable(test_adhocctl tests/test_adhocctl.cpp src/net/adhocctl.cpp src/net/pro_client.cpp src/net/pro_protocol.cpp)
target_include_directories(test_adhocctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_adhocctl PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_adhocctl PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME adhocctl_tests COMMAND test_adhocctl)

add_executable(test_guest_callback_queue tests/test_guest_callback_queue.cpp)
target_include_directories(test_guest_callback_queue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_guest_callback_queue PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_guest_callback_queue PRIVATE pthread)
add_test(NAME guest_callback_queue_tests COMMAND test_guest_callback_queue)

add_executable(test_netconf_adhoc tests/test_netconf_adhoc.cpp src/net/netconf_adhoc.cpp)
target_include_directories(test_netconf_adhoc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_netconf_adhoc PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME netconf_adhoc_tests COMMAND test_netconf_adhoc)

add_executable(test_matching_packet tests/test_matching_packet.cpp src/net/matching_packet.cpp)
target_include_directories(test_matching_packet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_matching_packet PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME matching_packet_tests COMMAND test_matching_packet)

add_executable(test_pdp tests/test_pdp.cpp src/net/pdp.cpp src/net/relay_protocol.cpp src/net/relay_conn.cpp)
target_include_directories(test_pdp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_pdp PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_pdp PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME pdp_tests COMMAND test_pdp)

add_executable(test_matching tests/test_matching.cpp src/net/matching.cpp src/net/matching_packet.cpp)
target_include_directories(test_matching PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_matching PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME matching_tests COMMAND test_matching)

add_executable(test_ptp tests/test_ptp.cpp src/net/ptp.cpp src/net/relay_protocol.cpp src/net/relay_conn.cpp)
target_include_directories(test_ptp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ptp PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_ptp PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME ptp_tests COMMAND test_ptp)

add_executable(test_relay_sockets tests/test_relay_sockets.cpp src/net/pdp.cpp src/net/ptp.cpp src/net/relay_protocol.cpp src/net/relay_conn.cpp)
target_include_directories(test_relay_sockets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_relay_sockets PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_relay_sockets PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME relay_sockets_tests COMMAND test_relay_sockets)

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
add_executable(test_savedata tests/test_savedata.cpp src/hle/psp_savedata.cpp src/hle/psp_save_crypt.cpp src/psp_host_io.cpp)
target_link_libraries(test_savedata PRIVATE kirk)

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
    src/hle/psp_save_crypt.cpp
    src/psp_host_io.cpp
)

target_link_libraries(test_gamedata_install PRIVATE kirk)

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

add_executable(test_hle_net
    tests/test_hle_net.cpp
    src/hle/psp_hle_misc.cpp
    src/hle/psp_hle_net.cpp
    src/hle/psp_hle_net_matching.cpp
    src/net/adhoc_config.cpp
    src/psp_host_io.cpp
    src/psp_machine_id.cpp
    src/net/adhocctl.cpp
    src/net/pdp.cpp
    src/net/ptp.cpp
    src/net/matching.cpp
    src/net/matching_packet.cpp
    src/net/pro_client.cpp
    src/net/pro_protocol.cpp
    src/net/relay_protocol.cpp
    src/net/relay_conn.cpp
)
target_include_directories(test_hle_net PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include
)
target_compile_options(test_hle_net PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_hle_net PRIVATE pthread ${PSP_SOCKET_LIBS})
add_test(NAME hle_net COMMAND test_hle_net)

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
    src/psp_perf.cpp
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

# GE BEZIER patch tessellation (Patapon 3 weather wind gusts).
add_executable(test_ge_patch
    tests/test_ge_patch.cpp
    src/psp_ge_patch.cpp
)
target_include_directories(test_ge_patch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PSPRECOMP_OUTPUT_DIR}/include
)
target_compile_options(test_ge_patch PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_patch_tests COMMAND test_ge_patch)

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
