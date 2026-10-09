#pragma once
// Decoders behind the sceMpeg HLE: H.264 video through FFmpeg (libavcodec +
// libswscale) and ATRAC3plus movie audio through the vendored at3_standalone
// decoder. Pure host-side code: callers pass host pointers to guest memory,
// already bounds-checked to `dstSize`. The demuxer (psp_mpeg_demux.h)
// supplies the access units and audio frames.
//
// Ported from PPSSPP (GPL-2.0-or-later): Core/HW/MediaEngine.cpp
// (stepVideo, writeVideoImageWithRange, writeVideoLine*, getAudioSamples).
//
// FFmpeg is optional: without PSPRECOMP_HAVE_FFMPEG the video decoder
// compiles to a stub whose available() is false and decode() fails. The audio
// decoder does not depend on FFmpeg.

#include <cstddef>
#include <cstdint>
#include <memory>

/// sceMpeg videoPixelMode values (GE colour modes).
enum PspVideoPixelMode : int {
    PSP_VIDEO_BGR5650 = 0,
    PSP_VIDEO_ABGR5551 = 1,
    PSP_VIDEO_ABGR4444 = 2,
    PSP_VIDEO_ABGR8888 = 3,
};

/// Bytes per pixel of a videoPixelMode (4 for 8888, else 2; 0 if unknown).
int psp_video_bytes_per_pixel(int pixelMode);

/// Converts `width` pixels of tightly packed RGBA8888 (bytes R,G,B,A) into
/// `dst` in the PSP layout of `pixelMode`, little-endian, red in the low
/// bits, matching PPSSPP's swscale formats + writeVideoLine* masks: 8888 =
/// bytes R,G,B,0 (alpha cleared); 5650 = AV_PIX_FMT_BGR565LE; 5551 =
/// AV_PIX_FMT_BGR555LE with the alpha bit 0; 4444 = AV_PIX_FMT_BGR444LE with
/// alpha 0. Colour channels are truncated (no rounding/dither). Unknown modes
/// write nothing.
void psp_video_convert_line(uint8_t* dst, const uint8_t* rgba, int width, int pixelMode);

class PspVideoDecoder {
public:
    PspVideoDecoder();
    ~PspVideoDecoder();
    PspVideoDecoder(const PspVideoDecoder&) = delete;
    PspVideoDecoder& operator=(const PspVideoDecoder&) = delete;

    /// True when built with FFmpeg.
    static bool available();

    /// Decodes one Annex-B H.264 access unit (as produced by
    /// PspH264AuSplitter). Returns true if a new picture came out; decoder
    /// delay may hold back the first ones. The newest picture is kept (as
    /// RGBA8888 via libswscale, picture-sized) until the next one replaces it.
    bool decode(const uint8_t* au, size_t size);
    /// End of stream: pulls one delayed picture out of the decoder. Returns
    /// true if one came out (call until false).
    bool drain();

    bool hasPicture() const;
    int width() const;   // picture size, 0 when none yet
    int height() const;

    /// Writes the region (xpos, ypos, w, h) of the current picture, clipped to
    /// the picture, to `dst` as rows `frameWidth` pixels apart in `pixelMode`
    /// (PPSSPP writeVideoImageWithRange). Pixels past the region in each row
    /// are left untouched. Never writes at or beyond dst + dstSize. Returns the
    /// byte size of the written area (frameWidth * bpp * clipped rows), 0 when
    /// there is no picture or the arguments are invalid (frameWidth <= 0 or
    /// > 2048, negative region, unknown pixel mode).
    int writeImage(uint8_t* dst, size_t dstSize, int frameWidth, int pixelMode, int xpos,
                   int ypos, int w, int h) const;

    /// Drops decoder state and the current picture (stream restart / delete).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PspMpegAudioDecoder {
public:
    PspMpegAudioDecoder();
    ~PspMpegAudioDecoder();
    PspMpegAudioDecoder(const PspMpegAudioDecoder&) = delete;
    PspMpegAudioDecoder& operator=(const PspMpegAudioDecoder&) = delete;

    /// Decodes one ATRAC3plus frame payload as returned by
    /// PspMpegDemux::getNextAudioFrame (8-byte header stripped; headerCode1 /
    /// headerCode2 are that header's codes; headerCode1 == 0x24 means a mono
    /// source). Writes interleaved s16 stereo to `out` (room for
    /// `outCapacityFrames` stereo frames). Returns the number of stereo frames
    /// written (2048 for a normal frame), or -1 on a decode error (out is then
    /// zero-filled for 2048 frames when room allows).
    int decode(const uint8_t* frame, int size, int headerCode1, int headerCode2, int16_t* out,
               int outCapacityFrames);

    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
