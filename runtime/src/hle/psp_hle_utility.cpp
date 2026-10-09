#include "hle/psp_hle.h"
#include "hle/psp_hle_intr.h"
#include "hle/psp_hle_kernel.h"
#include "psp_audio_out.h"
#include "psp_memory.h"
#include "psp_runtime.h"
#include "psp_scheduler.h"
#include "recomp.h"
#include "recomp_module.h"  // generated module facts (issue #47 Phase 2)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

// ================================================================
// Catch-all HLE stubs for remaining Patapon NIDs.
// Groups: cache, RTC, audio, ATRAC, module mgmt, UMD, network,
//         MPEG, SAS, HTTP, SSL, impose, PSMF, stdio, interrupt,
//         kernel_library, loadexec, sceUtility.
// ================================================================

// ---- Kernel Memory Bump Allocator ----
// Starts after BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE, grows upward.
// Kernel memory range: 0x08000000 - 0x083FFFFF (4MB)
static uint32_t g_kernel_heap_pos =
    BOOT_MODULE_ADDR + NATIVE_MODULE_SIZE;
static constexpr uint32_t KERNEL_MEM_END = 0x08400000U;

uint32_t psp_alloc_kernel_memory(uint32_t size) {
    uint32_t aligned = (size + 0xFU) & ~0xFU;  // 16-byte align
    if (g_kernel_heap_pos + aligned > KERNEL_MEM_END) {
        return 0;  // OOM
    }
    uint32_t addr = g_kernel_heap_pos;
    g_kernel_heap_pos += aligned;
    return addr;
}

uint32_t psp_get_boot_module_gp() {
    return RECOMP_MODULE_GP;  // SceModuleInfo gp (generated fact, #47 P2)
}

// ---- Cache Operations (all no-ops) ----

static void hle_sceKernelDcacheWritebackAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackInvalidateAll(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackRange(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- RTC ----

static void hle_sceRtcGetAccumulativeTime(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return current microsecond timestamp as 64-bit in v0:v1
    auto now = std::chrono::steady_clock::now();
    uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());
    ctx->r[2] = static_cast<int32_t>(us & 0xFFFFFFFF);
    ctx->r[3] = static_cast<int32_t>((us >> 32) & 0xFFFFFFFF);
    (void)rdram;
}

static void hle_sceRtcGetCurrentClockLocalTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t time_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (time_ptr != 0) {
        // ScePspDateTime: year(u16), month(u16), day(u16),
        //   hour(u16), minute(u16), second(u16), microsecond(u32)
        time_t now = std::time(nullptr);
        struct tm* local = std::localtime(&now);
        if (local) {
            psp_mem_write<uint16_t>(rdram, time_ptr,
                static_cast<uint16_t>(local->tm_year + 1900));
            psp_mem_write<uint16_t>(rdram, time_ptr + 2,
                static_cast<uint16_t>(local->tm_mon + 1));
            psp_mem_write<uint16_t>(rdram, time_ptr + 4,
                static_cast<uint16_t>(local->tm_mday));
            psp_mem_write<uint16_t>(rdram, time_ptr + 6,
                static_cast<uint16_t>(local->tm_hour));
            psp_mem_write<uint16_t>(rdram, time_ptr + 8,
                static_cast<uint16_t>(local->tm_min));
            psp_mem_write<uint16_t>(rdram, time_ptr + 10,
                static_cast<uint16_t>(local->tm_sec));
            psp_mem_write<uint32_t>(rdram, time_ptr + 12, 0);
        }
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelLibcTime(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t out_ptr = static_cast<uint32_t>(ctx->r[4]);
    int32_t t = static_cast<int32_t>(std::time(nullptr));

    if (out_ptr != 0) {
        psp_mem_write<int32_t>(rdram, out_ptr, t);
    }

    ctx->r[2] = t;
}

static void hle_sceKernelLibcClock(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(std::clock());
    (void)rdram;
}

static void hle_sceKernelLibcGettimeofday(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t tv_ptr = static_cast<uint32_t>(ctx->r[4]);

    if (tv_ptr != 0) {
        auto now = std::chrono::system_clock::now();
        auto secs = std::chrono::duration_cast<
            std::chrono::seconds>(
                now.time_since_epoch()).count();
        auto usecs = std::chrono::duration_cast<
            std::chrono::microseconds>(
                now.time_since_epoch()).count() % 1000000;
        // struct timeval: tv_sec(u32), tv_usec(u32)
        psp_mem_write<uint32_t>(rdram, tv_ptr,
            static_cast<uint32_t>(secs));
        psp_mem_write<uint32_t>(rdram, tv_ptr + 4,
            static_cast<uint32_t>(usecs));
    }

    ctx->r[2] = SCE_OK;
}

// ---- Audio output ----
// Issue #29: the *Blocking output calls must pace the caller at the
// hardware playback rate (44.1kHz). Returning instantly made the PCM
// loop spin at kHz, starving everything else.
// Samples are queued on the PspAudioMixer (psp_audio_out.cpp) and the
// caller blocks until the device has drained the channel down to about
// one grain, which paces it at the real playback rate. Without an audio
// device (PSPRECOMP_NO_AUDIO, headless) the calls fall back to sleeping
// for the grain's playback time.

static constexpr int AUDIO_OUTPUT_SAMPLE_RATE = 44100;
static constexpr int AUDIO_CHANNEL_COUNT = PspAudioMixer::kNormalChannels;
// Valid PSP sample counts: 17..4111 (sceAudioOutput2Reserve contract).
static constexpr int AUDIO_MIN_SAMPLES = 17;
static constexpr int AUDIO_MAX_SAMPLES = 4111;
static constexpr uint32_t AUDIO_FORMAT_MONO = 0x10;
// Extra frames a blocking channel may keep queued beyond one grain, so the
// cooperative scheduler has slack to produce the next grain.
static constexpr int AUDIO_QUEUE_SLACK = 512;
static constexpr uint32_t SCE_AUDIO_ERROR_NOT_RESERVED = 0x80260002u;

struct AudioChannelState {
    bool reserved = false;
    int samples = 1024;
    bool mono = false;
    int vol_left = 0x8000;
    int vol_right = 0x8000;
};

// Sample count from sceAudioOutput2Reserve (default one 1024 grain
// if Reserve was never seen).
static int g_output2_samples = 1024;
static AudioChannelState g_channels[AUDIO_CHANNEL_COUNT];

/// Block the calling thread for the playback duration of `samples`
/// samples at 44.1kHz. Mirrors the hle_sceKernelDelayThread blocking
/// idiom (psp_hle_kernel_thread.cpp): dispatch pending IO callbacks,
/// yield, then sleep.
static void audio_block_for_samples(
    uint8_t* rdram, recomp_context* ctx, int64_t samples
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    std::this_thread::sleep_for(std::chrono::microseconds(
        samples * 1000000LL / AUDIO_OUTPUT_SAMPLE_RATE));
}

/// Queue one grain from guest memory on mixer channel `mix_ch`. Returns
/// false when there is nothing to play (null/out-of-range buffer or no
/// audio device).
static bool audio_push_guest(
    uint8_t* rdram, int mix_ch, uint32_t buf, int frames, bool mono,
    int vol_left, int vol_right
) {
    if (!psp_audio_out_active() || buf == 0 || frames <= 0) return false;
    const uint32_t off = buf & PSP_ADDR_MASK;
    const size_t bytes = static_cast<size_t>(frames) * (mono ? 2 : 4);
    if (off + bytes > PSP_MEM_SIZE) return false;
    // Guest memory is little-endian s16 and so is every supported host.
    psp_audio_mixer().push(mix_ch,
        reinterpret_cast<const int16_t*>(rdram + off), frames, !mono,
        vol_left & 0xFFFF, vol_right & 0xFFFF);
    return true;
}

/// Block until the device has drained `mix_ch` down to about one grain.
/// Bounded so a stalled device can never hang the game thread.
static void audio_wait_drain(
    uint8_t* rdram, recomp_context* ctx, int mix_ch, int frames
) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(
            4LL * frames * 1000000LL / AUDIO_OUTPUT_SAMPLE_RATE + 50000);
    while (psp_audio_out_active() &&
           psp_audio_mixer().queued(mix_ch) > frames + AUDIO_QUEUE_SLACK &&
           std::chrono::steady_clock::now() < deadline) {
        psp_kernel_check_callbacks(rdram, ctx);
        sched_yield_point();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
}

/// Play one grain on a normal channel: queue it and pace the caller.
static void audio_output_channel(
    uint8_t* rdram, recomp_context* ctx, uint32_t channel, uint32_t buf,
    int vol_left, int vol_right, bool blocking
) {
    if (channel >= static_cast<uint32_t>(AUDIO_CHANNEL_COUNT)) {
        if (blocking) audio_block_for_samples(rdram, ctx, 1024);
        return;
    }
    AudioChannelState& ch = g_channels[channel];
    const bool queued = audio_push_guest(rdram, static_cast<int>(channel),
        buf, ch.samples, ch.mono, vol_left, vol_right);
    if (!blocking) return;
    if (queued) {
        audio_wait_drain(rdram, ctx, static_cast<int>(channel), ch.samples);
    } else {
        audio_block_for_samples(rdram, ctx, ch.samples);
    }
}

static void hle_sceAudioOutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    int vol = static_cast<int>(ctx->r[5]);
    uint32_t buf = static_cast<uint32_t>(ctx->r[6]);
    // Preserve existing return convention (a1) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[5];
    audio_output_channel(rdram, ctx, channel, buf, vol, vol, true);
    ctx->r[2] = ret;
}

static void hle_sceAudioOutputPannedBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    int vol_left = static_cast<int>(ctx->r[5]);
    int vol_right = static_cast<int>(ctx->r[6]);
    uint32_t buf = static_cast<uint32_t>(ctx->r[7]);
    // Preserve existing return convention (a3) before callbacks can
    // clobber argument registers.
    int32_t ret = ctx->r[7];
    audio_output_channel(rdram, ctx, channel, buf, vol_left, vol_right, true);
    ctx->r[2] = ret;
}

static void hle_sceAudioChReserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t channel = ctx->r[4];
    int32_t samples = ctx->r[5];
    uint32_t format = static_cast<uint32_t>(ctx->r[6]);
    if (channel < 0 || channel >= AUDIO_CHANNEL_COUNT) {
        // Auto-allocate: the highest free channel (PSP firmware order),
        // falling back to channel 0 when all are taken.
        channel = 0;
        for (int i = AUDIO_CHANNEL_COUNT - 1; i >= 0; i--) {
            if (!g_channels[i].reserved) { channel = i; break; }
        }
    }
    AudioChannelState& ch = g_channels[channel];
    ch.reserved = true;
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        ch.samples = samples;
    }
    ch.mono = (format == AUDIO_FORMAT_MONO);
    psp_audio_mixer().clear(channel);
    ctx->r[2] = channel;
    (void)rdram;
}

static void hle_sceAudioChRelease(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    if (channel < static_cast<uint32_t>(AUDIO_CHANNEL_COUNT)) {
        g_channels[channel].reserved = false;
        psp_audio_mixer().clear(static_cast<int>(channel));
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2Reserve(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t samples = ctx->r[4] & 0x7FFFFFFF;
    if (samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_output2_samples = samples;
    }
    psp_audio_mixer().clear(PspAudioMixer::kOutput2);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutput2OutputBlocking(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0=vol, a1=buf (stereo). buf==0 is the pre-Release drain: sleep one
    // grain and report success, same as a normal grain.
    int vol = static_cast<int>(ctx->r[4]);
    uint32_t buf = static_cast<uint32_t>(ctx->r[5]);
    const int frames = g_output2_samples;
    if (audio_push_guest(rdram, PspAudioMixer::kOutput2, buf, frames, false,
                         vol, vol)) {
        audio_wait_drain(rdram, ctx, PspAudioMixer::kOutput2, frames);
    } else {
        audio_block_for_samples(rdram, ctx, frames);
    }
    ctx->r[2] = SCE_OK;
}

static void hle_sceAudioOutput2Release(
    uint8_t* rdram, recomp_context* ctx
) {
    psp_audio_mixer().clear(PspAudioMixer::kOutput2);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioOutputPanned(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    int vol_left = static_cast<int>(ctx->r[5]);
    int vol_right = static_cast<int>(ctx->r[6]);
    uint32_t buf = static_cast<uint32_t>(ctx->r[7]);
    audio_output_channel(rdram, ctx, channel, buf, vol_left, vol_right, false);
    ctx->r[2] = SCE_OK;
}

static void hle_sceAudioChangeChannelConfig(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    uint32_t format = static_cast<uint32_t>(ctx->r[5]);
    if (channel < static_cast<uint32_t>(AUDIO_CHANNEL_COUNT)) {
        g_channels[channel].mono = (format == AUDIO_FORMAT_MONO);
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioGetChannelRestLength(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    if (channel >= static_cast<uint32_t>(AUDIO_CHANNEL_COUNT)) {
        ctx->r[2] = static_cast<int32_t>(SCE_AUDIO_ERROR_NOT_RESERVED);
        return;
    }
    ctx->r[2] = psp_audio_mixer().queued(static_cast<int>(channel));
    (void)rdram;
}

static void hle_sceAudioChangeChannelVolume(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    if (channel < static_cast<uint32_t>(AUDIO_CHANNEL_COUNT)) {
        g_channels[channel].vol_left = static_cast<int>(ctx->r[5]);
        g_channels[channel].vol_right = static_cast<int>(ctx->r[6]);
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceAudioSetChannelDataLen(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t channel = static_cast<uint32_t>(ctx->r[4]);
    int32_t samples = ctx->r[5];
    if (channel < static_cast<uint32_t>(AUDIO_CHANNEL_COUNT) &&
        samples >= AUDIO_MIN_SAMPLES && samples <= AUDIO_MAX_SAMPLES) {
        g_channels[channel].samples = samples;
    }
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Module Management ----

struct ModuleInfo {
    int uid;
    uint32_t native_module_addr;  // Address in rdram of NativeModule
    uint32_t entry_addr;          // 0xFFFFFFFF for fake/PRX modules
    char name[28];
};

static std::vector<ModuleInfo> g_modules;
static int g_module_next_uid = 0x1000;

/// Initialize boot module in tracking list.
/// Called from psp_hle_register_utility().
static void init_boot_module_tracking() {
    ModuleInfo boot_mod{};
    boot_mod.uid = BOOT_MODULE_UID;
    boot_mod.native_module_addr = BOOT_MODULE_ADDR;
    boot_mod.entry_addr = RECOMP_MODULE_ENTRY;
    std::strncpy(boot_mod.name, RECOMP_MODULE_NAME, sizeof(boot_mod.name) - 1);
    g_modules.push_back(boot_mod);
}

/// Check if a path refers to BOOT.BIN.
/// Primary: case-insensitive last-component match.
/// Fallback: scan entire path for "BOOT.BIN" substring.
/// Handles "0sdisc0:0s/PSP_GAME/SYSDIR/BOOT.BIN" corruption
/// from LWL/LWR string-copy artifacts.
static bool path_is_boot_bin(const char* path) {
    // Primary: check last path component
    const char* slash = std::strrchr(path, '/');
    const char* filename = slash ? slash + 1 : path;
    const char* target = "BOOT.BIN";
    bool primary_match = true;
    for (int i = 0; target[i]; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != target[i]) { primary_match = false; break; }
    }
    if (primary_match && filename[8] == '\0') return true;

    // Fallback: check if "BOOT.BIN" appears anywhere in path
    for (const char* p = path; *p; p++) {
        if ((*p == 'B' || *p == 'b') &&
            strncasecmp(p, "BOOT.BIN", 8) == 0) {
            return true;
        }
    }
    return false;
}

static void hle_sceKernelLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t path_ptr = static_cast<uint32_t>(ctx->r[4]);
    const char* path = reinterpret_cast<const char*>(
        rdram + (path_ptr & PSP_ADDR_MASK));

    // Special case: BOOT.BIN self-load.
    // PPSSPP handles this via __KernelLoadExec (full restart).
    // Static recompiler: return existing boot module UID since
    // the game binary is already loaded and running.
    if (path_is_boot_bin(path)) {
        std::fprintf(stderr,
            "[HLE] sceKernelLoadModule(\"%s\") "
            "-> returning boot module uid=%d (self-load)\n",
            path, BOOT_MODULE_UID);
        ctx->r[2] = BOOT_MODULE_UID;
        return;
    }

    int uid = g_module_next_uid++;

    // Allocate a fake NativeModule in kernel memory
    uint32_t mod_addr =
        psp_alloc_kernel_memory(NATIVE_MODULE_SIZE);
    if (mod_addr != 0) {
        std::memset(
            rdram + (mod_addr & PSP_ADDR_MASK),
            0, NATIVE_MODULE_SIZE);
        // entry_addr = 0xFFFFFFFF (fake module, no entry)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x64, 0xFFFFFFFFU);
        // status = 4 (MODULE_STATUS_STARTING)
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x24, 4);
        // modid
        psp_mem_write<uint32_t>(
            rdram, mod_addr + 0x2C,
            static_cast<uint32_t>(uid));
        // Extract filename from path (last '/' component)
        const char* filename = path;
        const char* slash = std::strrchr(path, '/');
        if (slash) {
            filename = slash + 1;
        }
        std::strncpy(
            reinterpret_cast<char*>(
                rdram + ((mod_addr + 0x08) & PSP_ADDR_MASK)),
            filename, 27);
    }

    // Track module
    ModuleInfo info{};
    info.uid = uid;
    info.native_module_addr = mod_addr;
    info.entry_addr = 0xFFFFFFFFU;
    const char* fname = path;
    const char* sl = std::strrchr(path, '/');
    if (sl) { fname = sl + 1; }
    std::strncpy(info.name, fname, sizeof(info.name) - 1);
    g_modules.push_back(info);

    std::fprintf(stderr,
        "[HLE] sceKernelLoadModule(\"%s\") -> uid=%d (fake)\n",
        path, uid);
    ctx->r[2] = uid;
}

static void hle_sceKernelStartModule(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    const char* action = "ok";

    // Look up module in tracking list
    for (auto& mod : g_modules) {
        if (mod.uid == uid) {
            // Boot module special case: it is already loaded and
            // running. The game calls LoadModule("BOOT.BIN") which
            // returns BOOT_MODULE_UID, then StartModule on it.
            // PPSSPP returns 0 for already-started modules.
            if (uid == BOOT_MODULE_UID) {
                std::fprintf(stderr,
                    "[HLE] sceKernelStartModule(uid=%d) "
                    "-> already started (boot module), "
                    "returning 0\n", uid);
                ctx->r[2] = SCE_OK;
                return;
            }

            if (mod.entry_addr == 0xFFFFFFFFU) {
                // Fake module: set status to STARTED, skip thread
                if (mod.native_module_addr != 0) {
                    psp_mem_write<uint32_t>(
                        rdram,
                        mod.native_module_addr + 0x24, 5);
                }
                action = "skipped (fake)";
            }
            break;
        }
    }

    std::fprintf(stderr,
        "[HLE] sceKernelStartModule(uid=%d) -> %s\n",
        uid, action);
    ctx->r[2] = SCE_OK;
}

static void hle_sceKernelStopModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelStopUnloadSelfModuleWithStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelGetModuleIdByAddress(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    // If this ever gains a real UNKNOWN_MODULE branch: PPSSPP matches by memory
    // block (RECOMP_SEG0_VADDR..+MEMSZ), NOT the text range — data/.bss addresses
    // must still resolve to the module UID (sceKernelModule.cpp:2388).
    constexpr uint32_t TEXT_START = RECOMP_MODULE_TEXT_START;
    constexpr uint32_t TEXT_END = TEXT_START + RECOMP_MODULE_TEXT_SIZE;
    if (addr >= TEXT_START && addr < TEXT_END) {
        ctx->r[2] = BOOT_MODULE_UID;
    } else {
        // Fallback: only one real module exists
        ctx->r[2] = BOOT_MODULE_UID;
    }
    (void)rdram;
}

static void hle_sceKernelGetModuleId(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = BOOT_MODULE_UID;
    (void)rdram;
}

static void hle_sceKernelQueryModuleInfo(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = ctx->r[4];
    uint32_t info_addr = static_cast<uint32_t>(ctx->r[5]);

    if (info_addr == 0) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> EINVAL (null infoAddr)\n", uid);
        ctx->r[2] = SCE_ERROR_ERRNO_EINVAL;
        return;
    }

    // Find the module in tracking list
    uint32_t native_addr = 0;
    bool found = false;
    for (const auto& mod : g_modules) {
        if (mod.uid == uid) {
            native_addr = mod.native_module_addr;
            found = true;
            break;
        }
    }

    if (!found) {
        std::fprintf(stderr,
            "[HLE] sceKernelQueryModuleInfo(uid=%d) "
            "-> NOT_FOUND_MODULE\n", uid);
        ctx->r[2] = SCE_KERNEL_ERROR_NOT_FOUND_MODULE;
        return;
    }

    // Read fields from NativeModule in rdram and write
    // to SceKernelModuleInfo output struct at info_addr.
    //
    // NativeModule (source)        SceKernelModuleInfo (dest)
    // +0x7C nsegment            -> +0x0004 nsegment (u32)
    // +0x80 segaddr[0..3]       -> +0x0008 segmentaddr[0..3]
    // +0x90 segsize[0..3]       -> +0x0018 segmentsize[0..3]
    // +0x50 entry_addr          -> +0x0028 entry_addr (u32)
    // +0x68 gp_value            -> +0x002C gp_value (u32)
    // +0x6C text_addr           -> +0x0030 text_addr (u32)
    // +0x70 text_size           -> +0x0034 text_size (u32)
    //                              +0x0038 data_size = 0
    //                              +0x003C bss_size = 0
    // +0x04 attribute           -> +0x0040 attribute (u16)
    // +0x06 version             -> +0x0042 version (u8[2])
    // +0x08 name                -> +0x0044 name (28 bytes)

    // nsegment
    uint32_t nseg = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x7C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x04, nseg);

    // segmentaddr[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t sa = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x80 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x08 + i * 4, sa);
    }

    // segmentsize[0..3]
    for (int i = 0; i < 4; i++) {
        uint32_t ss = psp_mem_read<uint32_t>(
            rdram, native_addr + 0x90 + i * 4);
        psp_mem_write<uint32_t>(
            rdram, info_addr + 0x18 + i * 4, ss);
    }

    // entry_addr (from module_start_func at +0x50)
    uint32_t entry = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x50);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x28, entry);

    // gp_value
    uint32_t gp = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x68);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x2C, gp);

    // text_addr
    uint32_t ta = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x6C);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x30, ta);

    // text_size
    uint32_t ts = psp_mem_read<uint32_t>(
        rdram, native_addr + 0x70);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x34, ts);

    // data_size = 0, bss_size = 0 (not tracked in NativeModule)
    psp_mem_write<uint32_t>(rdram, info_addr + 0x38, 0);
    psp_mem_write<uint32_t>(rdram, info_addr + 0x3C, 0);

    // attribute
    uint16_t attr = psp_mem_read<uint16_t>(
        rdram, native_addr + 0x04);
    psp_mem_write<uint16_t>(rdram, info_addr + 0x40, attr);

    // version[2]
    uint8_t v0 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x06);
    uint8_t v1 = psp_mem_read<uint8_t>(
        rdram, native_addr + 0x07);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x42, v0);
    psp_mem_write<uint8_t>(rdram, info_addr + 0x43, v1);

    // name (28 bytes)
    std::memcpy(
        rdram + ((info_addr + 0x44) & PSP_ADDR_MASK),
        rdram + ((native_addr + 0x08) & PSP_ADDR_MASK),
        28);

    std::fprintf(stderr,
        "[HLE] sceKernelQueryModuleInfo(uid=%d) -> OK\n",
        uid);
    ctx->r[2] = SCE_OK;
}

// ---- UMD (Universal Media Disc) ----

static void hle_sceUmdCheckMedium(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // Disc is present
    (void)rdram;
}

static void hle_sceUmdGetDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    // PSP_UMD_PRESENT | PSP_UMD_READY | PSP_UMD_READABLE = 0x02 | 0x10 | 0x20 = 0x32
    // Bit values from PPSSPP sceUmd.h:
    //   0x01 = NOT_PRESENT, 0x02 = PRESENT, 0x04 = CHANGED,
    //   0x08 = NOT_READY,   0x10 = READY,   0x20 = READABLE
    ctx->r[2] = 0x02 | 0x10 | 0x20;
    (void)rdram;
}

static void hle_sceUmdActivate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStat(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdWaitDriveStatCB(
    uint8_t* rdram, recomp_context* ctx
) {
    psp_kernel_check_callbacks(rdram, ctx);
    sched_yield_point();
    ctx->r[2] = SCE_OK;
}

static void hle_sceUmdGetErrorStat(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // No error
    (void)rdram;
}

static void hle_sceUmdRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUmdUnRegisterUMDCallBack(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Networking (all init/term no-ops) ----

static void hle_sceNetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetInetTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetApctlTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceNetResolverTerm(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- HTTP/SSL ----

static void hle_sceHttpInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpsLoadDefaultCert(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpSaveSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceHttpLoadSystemCookie(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslInit(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceSslEnd(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MPEG: registered by psp_hle_register_mpeg() (psp_hle_mpeg.cpp).

// ---- PSMF: registered by psp_hle_register_psmf() (psp_hle_psmf.cpp).

// ---- SAS (Software Audio Synthesis) ----
// Moved to psp_hle_sas.cpp (issue #29: minimal voice state machine).

// ---- Stdio ----

static void hle_sceKernelStdin(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 0;  // fd 0
    (void)rdram;
}

static void hle_sceKernelStdout(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 1;  // fd 1
    (void)rdram;
}

static void hle_sceKernelStderr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = 2;  // fd 2
    (void)rdram;
}

// ---- Interrupt Manager ----
// Sub-interrupt handlers live in psp_subintr_table(); the vblank ones are
// run once per frame by psp_intr_dispatch_vblank (psp_hle_display.cpp).

static void hle_sceKernelRegisterSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    int intr = ctx->r[4];
    int sub = ctx->r[5];
    uint32_t handler = static_cast<uint32_t>(ctx->r[6]);
    uint32_t arg = static_cast<uint32_t>(ctx->r[7]);
    int rc = psp_subintr_table().register_handler(intr, sub, handler, arg);
    std::fprintf(stderr,
        "[HLE] sceKernelRegisterSubIntrHandler(intr=%d sub=%d handler=0x%08X "
        "arg=0x%08X) -> 0x%08X\n",
        intr, sub, handler, arg, static_cast<uint32_t>(rc));
    ctx->r[2] = rc;
    (void)rdram;
}

static void hle_sceKernelReleaseSubIntrHandler(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = psp_subintr_table().release(ctx->r[4], ctx->r[5]);
    (void)rdram;
}

static void hle_sceKernelEnableSubIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = psp_subintr_table().enable(ctx->r[4], ctx->r[5]);
    (void)rdram;
}

// ---- Kernel_Library ----

static void hle_sceKernelCpuSuspendIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    // Return previous interrupt state (0 = enabled)
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceKernelCpuResumeIntr(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- LoadExec ----

static void hle_sceKernelExitGame(
    uint8_t* rdram, recomp_context* ctx
) {
    std::fprintf(stderr, "[HLE] sceKernelExitGame called\n");
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceKernelRegisterExitCallback(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Impose ----

static void hle_sceImposeGetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0 = lang_ptr, a1 = button_ptr
    uint32_t lang_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t button_ptr = static_cast<uint32_t>(ctx->r[5]);

    if (lang_ptr != 0) {
        psp_mem_write<int32_t>(rdram, lang_ptr, 1);  // English
    }
    if (button_ptr != 0) {
        psp_mem_write<int32_t>(rdram, button_ptr, 1);  // Cross=confirm
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceImposeSetLanguageMode(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceImposeSetUMDPopup(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Suspend ----

static void hle_sceKernelPowerTick(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- sceUtility ----

static void hle_sceUtilityLoadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityUnloadModule(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityGetSystemParamInt(
    uint8_t* rdram, recomp_context* ctx
) {
    int32_t param_id = ctx->r[4];
    uint32_t value_ptr = static_cast<uint32_t>(ctx->r[5]);

    int32_t value = 0;
    switch (param_id) {
        case 1: value = 1; break;  // Language = English
        case 5: value = 0; break;  // Date format = YYYYMMDD
        case 6: value = 0; break;  // Time format = 24h
        case 7: value = 0; break;  // Timezone offset
        case 8: value = 1; break;  // Daylight saving
        case 9: value = 1; break;  // Nickname (not impl'd via int)
        default: break;
    }

    if (value_ptr != 0) {
        psp_mem_write<int32_t>(rdram, value_ptr, value);
    }

    ctx->r[2] = SCE_OK;
}

// ---- Utility dialog status protocol (savedata, msg dialog) ----
// PSP utility dialogs share a status machine (PPSSPP
// Core/Dialog/PSPDialog.h DialogStatus):
//   NONE(0) -> InitStart -> INITIALIZE(1) -> RUNNING(2) -> FINISHED(3)
//   -> ShutdownStart -> SHUTDOWN(4) -> NONE(0)
// GetStatus auto-advances INITIALIZE->RUNNING and SHUTDOWN->NONE
// (PPSSPP PSPDialog::GetStatus with UseAutoStatus). We have no dialog
// UI, so the "dialog" completes instantly: the RUNNING poll performs
// the work, writes pspUtilityDialogCommon.result (offset +28: u32
// size, s32 language, s32 buttonSwap, s32 graphics/access/font/sound
// Thread, s32 result, s32 reserved[4] = 48 bytes), and advances to
// FINISHED for the next poll.
static constexpr int32_t PSP_UTILITY_STATUS_NONE = 0;
static constexpr int32_t PSP_UTILITY_STATUS_INITIALIZE = 1;
static constexpr int32_t PSP_UTILITY_STATUS_RUNNING = 2;
static constexpr int32_t PSP_UTILITY_STATUS_FINISHED = 3;
static constexpr int32_t PSP_UTILITY_STATUS_SHUTDOWN = 4;

// PPSSPP Core/HLE/ErrorCodes.h
static constexpr uint32_t SCE_ERROR_UTILITY_INVALID_STATUS = 0x80110001U;
static constexpr uint32_t SCE_ERROR_UTILITY_INVALID_ADDRESS = 0x80110002U;
static constexpr uint32_t SCE_ERROR_UTILITY_INVALID_PARAM_SIZE = 0x80110004U;
static constexpr uint32_t SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA =
    0x80110347U;

#include "hle/psp_savedata.h"
#include "hle/psp_hle_io.h"
#include "psp_gamedata_install.h"

// pspUtilityDialogCommon.result offset within the param struct.
static constexpr uint32_t UTILITY_COMMON_RESULT_OFFSET = 28;
// SceUtilitySavedataParam.mode immediately follows the 48-byte common
// header (PPSSPP Core/Dialog/SavedataParam.h).
static constexpr uint32_t SAVEDATA_MODE_OFFSET = 48;

struct UtilityDialogState {
    int32_t status = PSP_UTILITY_STATUS_NONE;
    uint32_t param_addr = 0;
};

static UtilityDialogState g_savedata_dialog;
static UtilityDialogState g_msg_dialog;

// Modes handled by the real savedata back end (psp_savedata.cpp: AUTOLOAD,
// AUTOSAVE, LOAD, SAVE, LISTLOAD, LISTSAVE, SIZES, LIST, FILES, MAKEDATA*,
// READDATA*, WRITEDATA*, GETSIZE) never reach this. The rest (delete and
// erase modes) keep the old behaviour: deletes report "no data", anything
// else pretends success. Mode values: PPSSPP SceUtilitySavedataType.
static int32_t savedata_completion_result(uint32_t mode) {
    switch (mode) {
        case 6:   // LISTDELETE
        case 7:   // LISTALLDELETE
        case 9:   // AUTODELETE
        case 10:  // DELETE
        case 21:  // DELETEDATA
            return static_cast<int32_t>(
                SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA);
        default:  // ERASE/ERASESECURE/unknown
            return 0;  // pretend success
    }
}

static void utility_dialog_init_start(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_NONE) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    dlg.param_addr = static_cast<uint32_t>(ctx->r[4]);
    dlg.status = PSP_UTILITY_STATUS_INITIALIZE;
    ctx->r[2] = SCE_OK;
}

// Returns the pre-advance status; performs `complete` (writes the
// result into the param struct) when leaving RUNNING.
template <typename CompleteFn>
static void utility_dialog_get_status(
    UtilityDialogState& dlg, uint8_t* rdram, recomp_context* ctx,
    CompleteFn complete
) {
    int32_t ret = dlg.status;
    switch (dlg.status) {
        case PSP_UTILITY_STATUS_INITIALIZE:
            dlg.status = PSP_UTILITY_STATUS_RUNNING;
            break;
        case PSP_UTILITY_STATUS_RUNNING:
            complete(dlg, rdram);
            dlg.status = PSP_UTILITY_STATUS_FINISHED;
            break;
        case PSP_UTILITY_STATUS_SHUTDOWN:
            dlg.status = PSP_UTILITY_STATUS_NONE;
            dlg.param_addr = 0;
            break;
        default:
            break;
    }
    ctx->r[2] = ret;
}

static void utility_dialog_shutdown_start(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_FINISHED) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    dlg.status = PSP_UTILITY_STATUS_SHUTDOWN;
    ctx->r[2] = SCE_OK;
}

static void utility_dialog_update(
    UtilityDialogState& dlg, recomp_context* ctx
) {
    if (dlg.status != PSP_UTILITY_STATUS_RUNNING) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    ctx->r[2] = SCE_OK;
}

static void savedata_complete(UtilityDialogState& dlg, uint8_t* rdram) {
    if (dlg.param_addr == 0) {
        return;
    }
    uint32_t mode = psp_mem_read<uint32_t>(
        rdram, dlg.param_addr + SAVEDATA_MODE_OFFSET);
    if (psp_savedata::handles_mode(mode)) {
        // Real host-backed savedata (root: $PSPRECOMP_SAVEDATA or ./SAVEDATA).
        // execute() writes pspUtilityDialogCommon.result (+28) itself.
        psp_savedata::Options opts;
        opts.sdk_version = psp_kernel_compiled_sdk_version();
        psp_savedata::Outcome out = psp_savedata::execute(
            rdram, PSP_MEM_SIZE, dlg.param_addr, opts);
        fprintf(stderr, "[HLE] sceUtilitySavedata %s\n",
                out.summary.c_str());
        return;
    }
    int32_t result = savedata_completion_result(mode);
    psp_mem_write<int32_t>(
        rdram, dlg.param_addr + UTILITY_COMMON_RESULT_OFFSET, result);
    fprintf(stderr,
            "[HLE] sceUtilitySavedata complete: mode=%u result=0x%08X\n",
            mode, static_cast<uint32_t>(result));
}

static void msgdialog_complete(UtilityDialogState& dlg, uint8_t* rdram) {
    if (dlg.param_addr == 0) {
        return;
    }
    psp_mem_write<int32_t>(
        rdram, dlg.param_addr + UTILITY_COMMON_RESULT_OFFSET, 0);
}

static void hle_sceUtilitySavedataInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_init_start(g_savedata_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilitySavedataGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_get_status(g_savedata_dialog, rdram, ctx,
                              savedata_complete);
}

static void hle_sceUtilitySavedataShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_savedata_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilitySavedataUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_update(g_savedata_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_init_start(g_msg_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_get_status(g_msg_dialog, rdram, ctx,
                              msgdialog_complete);
}

static void hle_sceUtilityMsgDialogShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_msg_dialog, ctx);
    (void)rdram;
}

static void hle_sceUtilityMsgDialogUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_update(g_msg_dialog, ctx);
    (void)rdram;
}

// ---- Gamedata install (sceUtilityGamedataInstall) ----
// Same status machine as the savedata/msg dialogs above
// (PPSSPP PSPDialog::GetStatus with UseAutoStatus:
// INITIALIZE->RUNNING and SHUTDOWN->NONE advance automatically, the
// RUNNING poll performs the work), except the work is chunked: each
// Update copies the next slice of <disc0>/PSP_GAME/INSDIR/ into
// <savedata root>/<gameName><dataName>/ (PPSSPP GetGameDataInstallFileName
// destination naming) and GetStatus only leaves RUNNING once the copy —
// including the PARAM.SFO write — is done. The chunk engine is the pure
// psp_gamedata_install::Installer; this glue only snapshots the guest
// param at Init and writes progress/result back.
namespace gdi = psp_gamedata_install;

struct GamedataDialogState {
    UtilityDialogState base;  // status machine shared with the other dialogs
    std::optional<gdi::Installer> installer;
    bool done = false;    // copy finished (or mode rejected): may FINISH
    bool failed = false;  // result already holds an error, keep it
};

static GamedataDialogState g_gamedata_dialog;

// Guest-range check against the 128 MB rdram (address 0 is never valid,
// like PPSSPP's PSPPointer::IsValid).
static bool gamedata_range(uint32_t addr, uint32_t len) {
    if (addr == 0) {
        return false;
    }
    uint64_t off = addr & PSP_ADDR_MASK;
    return off + len <= PSP_MEM_SIZE;
}

static uint32_t gamedata_rd32(uint8_t* rdram, uint32_t addr) {
    return psp_mem_read<uint32_t>(rdram, addr);
}

static std::string gamedata_str(uint8_t* rdram, uint32_t addr, size_t cap) {
    uint64_t off = addr & PSP_ADDR_MASK;
    if (addr == 0 || off + cap > PSP_MEM_SIZE) {
        return "";
    }
    const char* p = reinterpret_cast<const char*>(rdram + off);
    return std::string(p, strnlen(p, cap));
}

static bool gamedata_name_ok(const std::string& s) {
    if (s.empty() || s == "." || s == "..") {
        return false;
    }
    return s.find_first_of("/\\:") == std::string::npos;
}

static void hle_sceUtilityGamedataInstallInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    GamedataDialogState& dlg = g_gamedata_dialog;
    if (dlg.base.status != PSP_UTILITY_STATUS_NONE) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    // PPSSPP PSPDialog::CheckRequest: common range, then size, then all.
    if (!gamedata_range(p, 48)) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_ADDRESS);
        return;
    }
    uint32_t size = gamedata_rd32(rdram, p + gdi::off::kSize);
    if (size != gdi::off::kSizeV1 && size != gdi::off::kSizeV2) {
        ctx->r[2] = static_cast<int32_t>(
            SCE_ERROR_UTILITY_INVALID_PARAM_SIZE);
        return;
    }
    if (!gamedata_range(p, size)) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_ADDRESS);
        return;
    }

    std::string game = gamedata_str(rdram, p + gdi::off::kGameName, 13);
    std::string data = gamedata_str(rdram, p + gdi::off::kDataName, 20);
    if (!gamedata_name_ok(game) || !gamedata_name_ok(data)) {
        ctx->r[2] = static_cast<int32_t>(
            SCE_ERROR_UTILITY_INVALID_PARAM_SIZE);
        return;
    }
    gdi::SfoParams sfo;
    sfo.title = gamedata_str(rdram, p + gdi::off::kSfoTitle, 128);
    sfo.savedata_title =
        gamedata_str(rdram, p + gdi::off::kSfoSavedataTitle, 128);
    sfo.detail = gamedata_str(rdram, p + gdi::off::kSfoDetail, 1024);
    uint64_t poff = (p + gdi::off::kSfoParentalLevel) & PSP_ADDR_MASK;
    sfo.parental_level = static_cast<int32_t>(rdram[poff]);

    std::string src = psp_path_to_host("disc0:/PSP_GAME/INSDIR");
    std::string dst =
        gdi::install_dir(psp_savedata::default_root(), game, data);
    dlg.installer.emplace(src, dst, sfo);
    if (!dlg.installer->ok()) {
        // PPSSPP Init: "Game install with no files / data" -> -1, and the
        // dialog never starts (status stays NONE).
        dlg.installer.reset();
        std::fprintf(stderr,
            "[HLE] sceUtilityGamedataInstallInitStart: no files in %s\n",
            src.c_str());
        ctx->r[2] = -1;
        return;
    }
    dlg.base.param_addr = p;
    dlg.done = false;
    dlg.failed = false;
    dlg.base.status = PSP_UTILITY_STATUS_INITIALIZE;
    std::fprintf(stderr,
        "[HLE] sceUtilityGamedataInstallInitStart(game=%s data=%s "
        "files=%d bytes=%llu)\n",
        game.c_str(), data.c_str(), dlg.installer->file_count(),
        static_cast<unsigned long long>(dlg.installer->total_bytes()));
    ctx->r[2] = SCE_OK;
}

static void hle_sceUtilityGamedataInstallUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    GamedataDialogState& dlg = g_gamedata_dialog;
    if (dlg.base.status != PSP_UTILITY_STATUS_RUNNING || !dlg.installer) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    uint32_t p = dlg.base.param_addr;
    // PPSSPP Update: mode >= 2 fails the install with INVALID_MODE.
    int32_t mode = 0;
    if (gamedata_range(p, gdi::off::kMode + 4)) {
        mode = static_cast<int32_t>(
            gamedata_rd32(rdram, p + gdi::off::kMode));
    }
    if (mode >= 2) {
        if (gamedata_range(p, gdi::off::kResult + 4)) {
            psp_mem_write<int32_t>(rdram, p + gdi::off::kResult,
                static_cast<int32_t>(gdi::ERR_UTILITY_GAMEDATA_INVALID_MODE));
        }
        dlg.done = true;
        dlg.failed = true;
        std::fprintf(stderr,
            "[HLE] sceUtilityGamedataInstallUpdate: invalid mode %d\n", mode);
        ctx->r[2] = SCE_OK;
        return;
    }
    dlg.installer->step();
    if (gamedata_range(p, gdi::off::kProgress + 4)) {
        psp_mem_write<int32_t>(rdram, p + gdi::off::kProgress,
                               dlg.installer->progress());
    }
    if (dlg.installer->finished()) {
        if (gamedata_range(p, gdi::off::kUnknownResult2 + 4)) {
            psp_mem_write<uint32_t>(rdram, p + gdi::off::kUnknownResult1,
                static_cast<uint32_t>(dlg.installer->files_done()));
            psp_mem_write<uint32_t>(rdram, p + gdi::off::kUnknownResult2,
                static_cast<uint32_t>(dlg.installer->files_done()));
        }
        dlg.done = true;
        std::fprintf(stderr,
            "[HLE] sceUtilityGamedataInstallUpdate: finished (%d files)\n",
            dlg.installer->files_done());
    }
    ctx->r[2] = SCE_OK;
}

static void hle_sceUtilityGamedataInstallGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    GamedataDialogState& dlg = g_gamedata_dialog;
    int32_t ret = dlg.base.status;
    switch (dlg.base.status) {
        case PSP_UTILITY_STATUS_INITIALIZE:
            dlg.base.status = PSP_UTILITY_STATUS_RUNNING;
            break;
        case PSP_UTILITY_STATUS_RUNNING:
            // Unlike the savedata dialog (one poll = done), the copy spans
            // many Updates: only leave RUNNING once the engine reports the
            // install finished (or the mode was rejected).
            if (dlg.done) {
                if (!dlg.failed && dlg.installer &&
                    dlg.installer->finished()) {
                    uint32_t p = dlg.base.param_addr;
                    if (gamedata_range(p, gdi::off::kResult + 4)) {
                        psp_mem_write<int32_t>(
                            rdram, p + gdi::off::kResult, 0);
                    }
                }
                if (dlg.failed ||
                    (dlg.installer && dlg.installer->finished())) {
                    dlg.base.status = PSP_UTILITY_STATUS_FINISHED;
                }
            }
            break;
        case PSP_UTILITY_STATUS_SHUTDOWN:
            dlg.base.status = PSP_UTILITY_STATUS_NONE;
            dlg.base.param_addr = 0;
            dlg.installer.reset();
            dlg.done = false;
            break;
        default:
            break;
    }
    ctx->r[2] = ret;
}

static void hle_sceUtilityGamedataInstallShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    utility_dialog_shutdown_start(g_gamedata_dialog.base, ctx);
    (void)rdram;
}

static void hle_sceUtilityGamedataInstallAbort(
    uint8_t* rdram, recomp_context* ctx
) {
    // PPSSPP Abort: NONE/SHUTDOWN -> INVALID_STATUS, else result = 1 and
    // the dialog shuts down (partial files stay on disk).
    GamedataDialogState& dlg = g_gamedata_dialog;
    if (dlg.base.status == PSP_UTILITY_STATUS_NONE ||
        dlg.base.status == PSP_UTILITY_STATUS_SHUTDOWN) {
        ctx->r[2] = static_cast<int32_t>(SCE_ERROR_UTILITY_INVALID_STATUS);
        return;
    }
    uint32_t p = dlg.base.param_addr;
    if (gamedata_range(p, gdi::off::kResult + 4)) {
        psp_mem_write<int32_t>(rdram, p + gdi::off::kResult, 1);
    }
    dlg.installer.reset();
    dlg.done = false;
    dlg.failed = false;
    dlg.base.status = PSP_UTILITY_STATUS_SHUTDOWN;
    ctx->r[2] = SCE_OK;
}

static void hle_sceUtilityOskInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityOskGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityOskShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityOskUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerInitStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerGetStatus(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = PSP_UTILITY_STATUS_NONE;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerShutdownStart(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceUtilityHtmlViewerUpdate(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- MsgPipe (Kernel message pipes) ----
// PSP MsgPipes are byte streams with no message boundaries
// (PPSSPP Core/HLE/sceKernelMsgPipe.cpp semantics). Patapon's
// sound-stream manager runs a command bus over one: the main
// thread Sends 84-byte commands, the sound thread TryReceives
// 8-byte headers. Producer and consumer are different OS threads,
// so the map and FIFOs are mutex-guarded.

struct PspMsgPipe {
    std::deque<uint8_t> fifo;
    uint32_t buf_size = 0;
};

static std::mutex g_msgpipe_mtx;
static std::unordered_map<int, PspMsgPipe> g_msgpipes;

// waitMode values (PPSSPP SCE_KERNEL_MPW_*):
// FULL(0) = all-or-nothing, ASAP(1) = partial transfer ok
static constexpr uint32_t PSP_MPW_ASAP = 1;

static void hle_sceKernelCreateMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    // a0=name, a1=part, a2=attr, a3=bufSize, t0=opt
    uint32_t buf_size = static_cast<uint32_t>(ctx->r[7]);
    int uid = psp_next_uid();
    {
        std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
        g_msgpipes[uid].buf_size = buf_size;
    }
    ctx->r[2] = uid;
    (void)rdram;
}

static void hle_sceKernelDeleteMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
    g_msgpipes.erase(uid);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// Blocking send (NID 0x876DBFAD). Copies the message into the
// FIFO; if the pipe is full, bounded-polls for space (the game
// sends 84 bytes into a 1024-byte pipe, so this never blocks in
// practice).
static void hle_sceKernelSendMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    uint32_t msg_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    uint32_t result_ptr = static_cast<uint32_t>(ctx->r[8]);
    // r[7]=waitMode, r[9]=timeout*: send completes immediately
    // unless the pipe is full.

    sched_yield_point();

    for (int spins = 0; ; ++spins) {
        {
            std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
            auto it = g_msgpipes.find(uid);
            if (it == g_msgpipes.end()) {
                ctx->r[2] = SCE_KERNEL_ERROR_UNKNOWN_MPPID;
                return;
            }
            PspMsgPipe& mp = it->second;
            if (size > mp.buf_size) {
                ctx->r[2] = SCE_KERNEL_ERROR_ILLEGAL_SIZE;
                return;
            }
            if (mp.fifo.size() + size <= mp.buf_size) {
                for (uint32_t i = 0; i < size; ++i) {
                    mp.fifo.push_back(psp_mem_read<uint8_t>(
                        rdram, msg_ptr + i));
                }
                if (result_ptr != 0) {
                    psp_mem_write<uint32_t>(rdram, result_ptr,
                                            size);
                }
                ctx->r[2] = SCE_OK;
                return;
            }
        }
        // Pipe full: bounded poll (~5s) for the consumer to drain.
        if (g_should_exit.load() || spins >= 5000) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_FULL;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Non-blocking receive (NID 0xDF52098F).
// waitMode FULL(0): all-or-nothing — if fewer than `size` bytes
// are buffered, return MPP_EMPTY without consuming anything.
// waitMode ASAP(1): pop min(available, size) bytes.
static void hle_sceKernelTryReceiveMsgPipe(
    uint8_t* rdram, recomp_context* ctx
) {
    int uid = static_cast<int>(ctx->r[4]);
    uint32_t buf_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t size = static_cast<uint32_t>(ctx->r[6]);
    uint32_t wait_mode = static_cast<uint32_t>(ctx->r[7]);
    uint32_t result_ptr = static_cast<uint32_t>(ctx->r[8]);

    std::lock_guard<std::mutex> lock(g_msgpipe_mtx);
    auto it = g_msgpipes.find(uid);
    if (it == g_msgpipes.end()) {
        ctx->r[2] = SCE_KERNEL_ERROR_UNKNOWN_MPPID;
        return;
    }
    PspMsgPipe& mp = it->second;
    uint32_t avail = static_cast<uint32_t>(mp.fifo.size());

    uint32_t count;
    if (wait_mode == PSP_MPW_ASAP) {
        count = std::min(avail, size);
        if (count == 0 && size != 0) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_EMPTY;
            return;
        }
    } else {  // FULL(0)
        if (avail < size) {
            ctx->r[2] = SCE_KERNEL_ERROR_MPP_EMPTY;
            return;
        }
        count = size;
    }

    for (uint32_t i = 0; i < count; ++i) {
        psp_mem_write<uint8_t>(rdram, buf_ptr + i, mp.fifo.front());
        mp.fifo.pop_front();
    }
    if (result_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, result_ptr, count);
    }
    ctx->r[2] = SCE_OK;
}

// ---- Registration ----

void psp_hle_register_utility() {
    // Initialize boot module tracking
    init_boot_module_tracking();

    // Cache operations
    psp_hle_register("sceKernelDcacheWritebackAll",
                      hle_sceKernelDcacheWritebackAll);
    psp_hle_register("sceKernelDcacheWritebackInvalidateAll",
                      hle_sceKernelDcacheWritebackInvalidateAll);
    psp_hle_register("sceKernelDcacheWritebackRange",
                      hle_sceKernelDcacheWritebackRange);

    // RTC
    psp_hle_register("sceRtcGetAccumulativeTime",
                      hle_sceRtcGetAccumulativeTime);
    psp_hle_register("sceRtcGetCurrentClockLocalTime",
                      hle_sceRtcGetCurrentClockLocalTime);
    psp_hle_register("sceKernelLibcTime",
                      hle_sceKernelLibcTime);
    psp_hle_register("sceKernelLibcClock",
                      hle_sceKernelLibcClock);
    psp_hle_register("sceKernelLibcGettimeofday",
                      hle_sceKernelLibcGettimeofday);

    // Audio
    psp_hle_register("sceAudioOutputBlocking",
                      hle_sceAudioOutputBlocking);
    psp_hle_register("sceAudioOutputPannedBlocking",
                      hle_sceAudioOutputPannedBlocking);
    psp_hle_register("sceAudioChReserve",
                      hle_sceAudioChReserve);
    psp_hle_register("sceAudioChRelease",
                      hle_sceAudioChRelease);
    psp_hle_register("sceAudioOutput2Reserve",
                      hle_sceAudioOutput2Reserve);
    psp_hle_register("sceAudioOutput2OutputBlocking",
                      hle_sceAudioOutput2OutputBlocking);
    psp_hle_register("sceAudioOutput2Release",
                      hle_sceAudioOutput2Release);
    psp_hle_register("sceAudioOutputPanned",
                      hle_sceAudioOutputPanned);
    psp_hle_register("sceAudioChangeChannelConfig",
                      hle_sceAudioChangeChannelConfig);
    psp_hle_register("sceAudioGetChannelRestLength",
                      hle_sceAudioGetChannelRestLength);
    psp_hle_register("sceAudioChangeChannelVolume",
                      hle_sceAudioChangeChannelVolume);
    psp_hle_register("sceAudioSetChannelDataLen",
                      hle_sceAudioSetChannelDataLen);

    // ATRAC3plus: registered by psp_hle_register_atrac() (psp_hle_atrac.cpp).

    // Module management
    psp_hle_register("sceKernelLoadModule",
                      hle_sceKernelLoadModule);
    psp_hle_register("sceKernelStartModule",
                      hle_sceKernelStartModule);
    psp_hle_register("sceKernelStopModule",
                      hle_sceKernelStopModule);
    psp_hle_register("sceKernelUnloadModule",
                      hle_sceKernelUnloadModule);
    psp_hle_register(
        "sceKernelStopUnloadSelfModuleWithStatus",
        hle_sceKernelStopUnloadSelfModuleWithStatus);
    psp_hle_register("sceKernelGetModuleIdByAddress",
                      hle_sceKernelGetModuleIdByAddress);
    psp_hle_register("sceKernelGetModuleId",
                      hle_sceKernelGetModuleId);
    psp_hle_register("sceKernelQueryModuleInfo",
                      hle_sceKernelQueryModuleInfo);

    // UMD
    psp_hle_register("sceUmdCheckMedium",
                      hle_sceUmdCheckMedium);
    psp_hle_register("sceUmdGetDriveStat",
                      hle_sceUmdGetDriveStat);
    psp_hle_register("sceUmdActivate",
                      hle_sceUmdActivate);
    psp_hle_register("sceUmdWaitDriveStat",
                      hle_sceUmdWaitDriveStat);
    psp_hle_register("sceUmdWaitDriveStatCB",
                      hle_sceUmdWaitDriveStatCB);
    psp_hle_register("sceUmdGetErrorStat",
                      hle_sceUmdGetErrorStat);
    psp_hle_register("sceUmdRegisterUMDCallBack",
                      hle_sceUmdRegisterUMDCallBack);
    psp_hle_register("sceUmdUnRegisterUMDCallBack",
                      hle_sceUmdUnRegisterUMDCallBack);

    // Networking
    psp_hle_register("sceNetInit", hle_sceNetInit);
    psp_hle_register("sceNetTerm", hle_sceNetTerm);
    psp_hle_register("sceNetInetInit", hle_sceNetInetInit);
    psp_hle_register("sceNetInetTerm", hle_sceNetInetTerm);
    psp_hle_register("sceNetApctlInit", hle_sceNetApctlInit);
    psp_hle_register("sceNetApctlTerm", hle_sceNetApctlTerm);
    psp_hle_register("sceNetResolverInit",
                      hle_sceNetResolverInit);
    psp_hle_register("sceNetResolverTerm",
                      hle_sceNetResolverTerm);

    // HTTP/SSL
    psp_hle_register("sceHttpInit", hle_sceHttpInit);
    psp_hle_register("sceHttpEnd", hle_sceHttpEnd);
    psp_hle_register("sceHttpsInit", hle_sceHttpsInit);
    psp_hle_register("sceHttpsEnd", hle_sceHttpsEnd);
    psp_hle_register("sceHttpsLoadDefaultCert",
                      hle_sceHttpsLoadDefaultCert);
    psp_hle_register("sceHttpSaveSystemCookie",
                      hle_sceHttpSaveSystemCookie);
    psp_hle_register("sceHttpLoadSystemCookie",
                      hle_sceHttpLoadSystemCookie);
    psp_hle_register("sceSslInit", hle_sceSslInit);
    psp_hle_register("sceSslEnd", hle_sceSslEnd);

    // MPEG

    // SAS (Software Audio Synthesis) -- see psp_hle_sas.cpp
    // (registered by psp_hle_register_sas()).

    // Stdio
    psp_hle_register("sceKernelStdin", hle_sceKernelStdin);
    psp_hle_register("sceKernelStdout", hle_sceKernelStdout);
    psp_hle_register("sceKernelStderr", hle_sceKernelStderr);

    // Interrupt manager
    psp_hle_register("sceKernelRegisterSubIntrHandler",
                      hle_sceKernelRegisterSubIntrHandler);
    psp_hle_register("sceKernelReleaseSubIntrHandler",
                      hle_sceKernelReleaseSubIntrHandler);
    psp_hle_register("sceKernelEnableSubIntr",
                      hle_sceKernelEnableSubIntr);

    // Kernel_Library
    psp_hle_register("sceKernelCpuSuspendIntr",
                      hle_sceKernelCpuSuspendIntr);
    psp_hle_register("sceKernelCpuResumeIntr",
                      hle_sceKernelCpuResumeIntr);

    // LoadExec
    psp_hle_register("sceKernelExitGame",
                      hle_sceKernelExitGame);
    psp_hle_register("sceKernelRegisterExitCallback",
                      hle_sceKernelRegisterExitCallback);

    // Impose
    psp_hle_register("sceImposeGetLanguageMode",
                      hle_sceImposeGetLanguageMode);
    psp_hle_register("sceImposeSetLanguageMode",
                      hle_sceImposeSetLanguageMode);
    psp_hle_register("sceImposeSetUMDPopup",
                      hle_sceImposeSetUMDPopup);

    // Suspend
    psp_hle_register("sceKernelPowerTick",
                      hle_sceKernelPowerTick);

    // sceUtility
    psp_hle_register("sceUtilityLoadModule",
                      hle_sceUtilityLoadModule);
    psp_hle_register("sceUtilityUnloadModule",
                      hle_sceUtilityUnloadModule);
    psp_hle_register("sceUtilityGetSystemParamInt",
                      hle_sceUtilityGetSystemParamInt);
    psp_hle_register("sceUtilitySavedataInitStart",
                      hle_sceUtilitySavedataInitStart);
    psp_hle_register("sceUtilitySavedataGetStatus",
                      hle_sceUtilitySavedataGetStatus);
    psp_hle_register("sceUtilitySavedataShutdownStart",
                      hle_sceUtilitySavedataShutdownStart);
    psp_hle_register("sceUtilitySavedataUpdate",
                      hle_sceUtilitySavedataUpdate);
    psp_hle_register("sceUtilityGamedataInstallInitStart",
                      hle_sceUtilityGamedataInstallInitStart);
    psp_hle_register("sceUtilityGamedataInstallGetStatus",
                      hle_sceUtilityGamedataInstallGetStatus);
    psp_hle_register("sceUtilityGamedataInstallShutdownStart",
                      hle_sceUtilityGamedataInstallShutdownStart);
    psp_hle_register("sceUtilityGamedataInstallUpdate",
                      hle_sceUtilityGamedataInstallUpdate);
    psp_hle_register("sceUtilityGamedataInstallAbort",
                      hle_sceUtilityGamedataInstallAbort);
    psp_hle_register("sceUtilityMsgDialogInitStart",
                      hle_sceUtilityMsgDialogInitStart);
    psp_hle_register("sceUtilityMsgDialogGetStatus",
                      hle_sceUtilityMsgDialogGetStatus);
    psp_hle_register("sceUtilityMsgDialogShutdownStart",
                      hle_sceUtilityMsgDialogShutdownStart);
    psp_hle_register("sceUtilityMsgDialogUpdate",
                      hle_sceUtilityMsgDialogUpdate);
    psp_hle_register("sceUtilityOskInitStart",
                      hle_sceUtilityOskInitStart);
    psp_hle_register("sceUtilityOskGetStatus",
                      hle_sceUtilityOskGetStatus);
    psp_hle_register("sceUtilityOskShutdownStart",
                      hle_sceUtilityOskShutdownStart);
    psp_hle_register("sceUtilityOskUpdate",
                      hle_sceUtilityOskUpdate);
    psp_hle_register("sceUtilityHtmlViewerInitStart",
                      hle_sceUtilityHtmlViewerInitStart);
    psp_hle_register("sceUtilityHtmlViewerGetStatus",
                      hle_sceUtilityHtmlViewerGetStatus);
    psp_hle_register("sceUtilityHtmlViewerShutdownStart",
                      hle_sceUtilityHtmlViewerShutdownStart);
    psp_hle_register("sceUtilityHtmlViewerUpdate",
                      hle_sceUtilityHtmlViewerUpdate);

    // MsgPipe
    psp_hle_register("sceKernelCreateMsgPipe",
                      hle_sceKernelCreateMsgPipe);
    psp_hle_register("sceKernelDeleteMsgPipe",
                      hle_sceKernelDeleteMsgPipe);
    psp_hle_register("sceKernelSendMsgPipe",
                      hle_sceKernelSendMsgPipe);
    psp_hle_register("sceKernelTryReceiveMsgPipe",
                      hle_sceKernelTryReceiveMsgPipe);
}
