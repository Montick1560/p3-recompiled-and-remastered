#include "psp_audio_out.h"

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr int kSampleRate = 44100;
constexpr int kDeviceFrames = 512;  // ~11.6 ms: low latency for rhythm timing

PspAudioMixer& mixer_instance() {
    static auto* m = new PspAudioMixer();  // leaked: the device thread may outlive exit
    return *m;
}

SDL_AudioDeviceID g_device = 0;
std::atomic<bool> g_active{false};

std::mutex g_dump_mutex;
FILE* g_dump = nullptr;
uint32_t g_dump_bytes = 0;

void write_wav_header(FILE* f, uint32_t data_bytes) {
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, f); u32(16);
    u16(1); u16(2); u32(kSampleRate); u32(kSampleRate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data_bytes);
}

void SDLCALL audio_callback(void*, Uint8* stream, int len) {
    const int frames = len / 4;
    mixer_instance().mix(reinterpret_cast<int16_t*>(stream), frames);
    std::lock_guard<std::mutex> lock(g_dump_mutex);
    if (g_dump) {
        std::fwrite(stream, 1, static_cast<size_t>(len), g_dump);
        g_dump_bytes += static_cast<uint32_t>(len);
    }
}

}  // namespace

PspAudioMixer& psp_audio_mixer() { return mixer_instance(); }

bool psp_audio_out_active() { return g_active.load(std::memory_order_relaxed); }

bool psp_audio_out_init() {
    if (std::getenv("PSPRECOMP_NO_AUDIO")) {
        std::fprintf(stderr, "[AUDIO] disabled by PSPRECOMP_NO_AUDIO (timing-only output)\n");
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[AUDIO] SDL audio unavailable (%s): timing-only output\n",
                     SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kDeviceFrames;
    want.callback = audio_callback;
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_device == 0) {
        std::fprintf(stderr, "[AUDIO] no audio device (%s): timing-only output\n",
                     SDL_GetError());
        return false;
    }
    if (const char* dump = std::getenv("PSPRECOMP_AUDIO_DUMP")) {
        std::lock_guard<std::mutex> lock(g_dump_mutex);
        g_dump = std::fopen(dump, "wb");
        if (g_dump) write_wav_header(g_dump, 0);  // sizes patched at shutdown
    }
    g_active = true;
    SDL_PauseAudioDevice(g_device, 0);
    std::fprintf(stderr, "[AUDIO] device open: %d Hz, %d ch, %d-frame buffer\n",
                 have.freq, have.channels, have.samples);
    return true;
}

void psp_audio_out_shutdown() {
    if (g_device != 0) {
        g_active = false;
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    std::lock_guard<std::mutex> lock(g_dump_mutex);
    if (g_dump) {
        std::fseek(g_dump, 0, SEEK_SET);
        write_wav_header(g_dump, g_dump_bytes);
        std::fclose(g_dump);
        g_dump = nullptr;
    }
}
