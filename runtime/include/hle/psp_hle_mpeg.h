#pragma once
// sceMpeg HLE (psp_hle_mpeg.cpp): movie playback through the game's
// ringbuffer, on top of psp_mpeg_demux.h and psp_media_engine.h.

#include <cstdint>

struct recomp_context;

/// Registers every sceMpeg* handler.
void psp_hle_register_mpeg();

/// Calls guest function `fn` with a0..a2 and returns its v0. The default
/// (nullptr) runs it through RECOMP_LOOKUP on the calling thread; tests
/// substitute a host function to play the game's ringbuffer callback.
using PspMpegGuestCall = int32_t (*)(uint8_t* rdram, recomp_context* ctx, uint32_t fn,
                                     uint32_t a0, uint32_t a1, uint32_t a2);
void psp_hle_mpeg_set_guest_call(PspMpegGuestCall fn);

/// Drops every MPEG context and restarts stream ids (tests only).
void psp_hle_mpeg_reset_for_tests();
