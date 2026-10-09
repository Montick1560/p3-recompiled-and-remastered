#pragma once
// MPEG-PS demuxer for the PSMF stream the game feeds through the sceMpeg
// ringbuffer, plus the H.264 access-unit splitter and the ATRAC3plus frame
// extractor built on top of it. Pure byte processing: no guest memory, no
// FFmpeg (the decoder lives in psp_media_engine.cpp).
//
// Ported from PPSSPP (GPL-2.0-or-later): Core/HW/MpegDemux.{h,cpp} (packet
// walk, PES header parsing, ATRAC frame extraction) and Core/HW/BufferQueue.h
// (timestamped FIFO). PPSSPP hands the video elementary stream to libavformat
// for AU splitting; this build has no libavformat, so the video PES payloads
// are collected here and cut into access units by PspH264AuSplitter.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/// "No timestamp" marker for access units (PES carried no PTS/DTS).
constexpr int64_t PSP_MPEG_NO_TIMESTAMP = INT64_MIN;

/// Byte FIFO whose pushes may carry a 90 kHz timestamp (BufferQueue
/// equivalent). Popping a range reports the earliest timestamp pushed inside
/// it, like PPSSPP's ptsMarks.
class PspByteQueue {
public:
    void push(const uint8_t* data, size_t n, int64_t pts = 0);
    size_t size() const { return buf_.size() - head_; }
    /// Copies up to `max` bytes from the front without consuming them.
    size_t peek(uint8_t* dst, size_t max) const;
    /// Pops up to `n` bytes; *pts = earliest timestamp mark in the popped
    /// range (0 when none).
    size_t pop(size_t n, int64_t* pts = nullptr);
    void clear();

private:
    struct Mark {
        uint64_t offset;  // absolute stream offset of the push
        int64_t pts;
    };
    std::vector<uint8_t> buf_;
    size_t head_ = 0;
    uint64_t base_ = 0;  // absolute stream offset of buf_[head_]
    std::deque<Mark> marks_;
};

struct PspAvcAccessUnit {
    std::vector<uint8_t> data;  // Annex-B H.264, starts at a start code
    int64_t pts = PSP_MPEG_NO_TIMESTAMP;  // 90 kHz
    int64_t dts = PSP_MPEG_NO_TIMESTAMP;
};

/// Cuts an Annex-B H.264 elementary stream (fed in arbitrary chunks) into
/// access units. A new AU starts at an access unit delimiter, at SPS/PPS/SEI
/// after a slice, or at a slice with first_mb_in_slice == 0 after a slice.
/// An AU is only returned once the start of the next one is seen (or on
/// flush), so the last AU of a stream needs pop(.., flush = true).
class PspH264AuSplitter {
public:
    /// `pts`/`dts` apply to the first AU starting at or after this chunk.
    void push(const uint8_t* data, size_t n, int64_t pts, int64_t dts);
    bool pop(PspAvcAccessUnit* out, bool flush);
    /// Bytes buffered and not yet returned as AUs.
    size_t pendingBytes() const { return buf_.size() - head_; }
    void clear();

private:
    struct Mark {
        uint64_t offset;
        int64_t pts;
        int64_t dts;
    };
    void emit(size_t end, PspAvcAccessUnit* out);

    std::vector<uint8_t> buf_;
    size_t head_ = 0;      // first live byte in buf_
    uint64_t base_ = 0;    // absolute stream offset of buf_[0]
    size_t scan_ = 0;      // next position to search for a start code
    bool haveAu_ = false;  // an AU start has been located
    size_t auStart_ = 0;   // buf_ index of the current AU's first byte
    size_t auNal_ = 0;     // buf_ index of the current AU's first NAL header
    bool auHasVcl_ = false;
    std::deque<Mark> marks_;
};

class PspMpegDemux {
public:
    /// `capacity` bounds the raw bytes held between demux() calls and the
    /// audio queue, like PPSSPP's ringbuffer-sized buffers.
    explicit PspMpegDemux(size_t capacity);

    /// Appends raw MPEG-PS bytes. Returns false (and adds nothing) when that
    /// would exceed the capacity.
    bool addStreamData(const uint8_t* data, size_t n);
    size_t getRemainSize() const { return capacity_ - buf_.size(); }
    /// Raw bytes not yet walked by demux().
    size_t rawBytes() const { return buf_.size() - index_; }

    /// Selects the audio sub-stream (private stream 1 channel). A change
    /// clears the audio queue; -1 latches onto the first channel seen.
    void setAudioChannel(int channel);
    /// PES stream id of the video stream to collect (0xE0..0xEF).
    void setVideoStreamId(int id) { videoId_ = id; }

    /// Walks every complete packet in the raw buffer, routing audio payloads
    /// to the audio queue and the selected video PES payloads to the AU
    /// splitter. Returns true if the data looked like MPEG-PS.
    bool demux();

    // ---- video ----
    /// Next complete AU; with `flush` also the trailing one. Call demux()
    /// first to move raw bytes into the splitter.
    bool nextVideoAu(PspAvcAccessUnit* out, bool flush);
    /// Raw + buffered-video bytes: what the guest ringbuffer still holds.
    size_t pendingVideoBytes() const { return rawBytes() + video_.pendingBytes(); }

    // ---- audio ----
    /// True when a complete ATRAC3plus frame is queued. Outputs are optional.
    bool hasNextAudioFrame(int* gotSize, int* frameSize, int* headerCode1,
                           int* headerCode2);
    /// Pops the next ATRAC3plus frame: *buf points at the frame payload (the
    /// 8-byte header is stripped, valid until the next call); returns the
    /// payload size, 0 when no complete frame is queued. *pts is the
    /// timestamp mark inside the frame's bytes (0 when none).
    int getNextAudioFrame(const uint8_t** buf, int* headerCode1, int* headerCode2,
                          int64_t* pts);

private:
    struct PesHeader {
        int64_t pts = 0;
        int64_t dts = 0;
        bool hasPts = false;
        int channel = -1;
    };

    int read8() { return index_ < buf_.size() ? buf_[index_++] : 0; }
    int read16() {
        int hi = read8();
        return (hi << 8) | read8();
    }
    int read24() {
        int a = read8();
        int b = read8();
        return (a << 16) | (b << 8) | read8();
    }
    int64_t readPts(int c) {
        int64_t hi = static_cast<int64_t>(c & 0x0E) << 29;
        int64_t mid = static_cast<int64_t>(read16() >> 1) << 15;
        return hi | mid | static_cast<int64_t>(read16() >> 1);
    }
    int64_t readPts() { return readPts(read8()); }
    bool isEOF() const { return index_ >= buf_.size(); }
    void skip(int n) {
        if (n > 0) {
            index_ += static_cast<size_t>(n);
            if (index_ > buf_.size()) {
                index_ = buf_.size();
            }
        }
    }
    int readPesHeader(PesHeader& pes, int length, int startCode);
    void demuxPes(int startCode, int length);
    bool skipPackHeader();
    /// Drops queued audio bytes that precede the first frame header, then
    /// copies the queue head into audioFrame_. Returns the bytes copied (0 when
    /// no frame header is queued).
    size_t syncAudio();

    size_t capacity_;
    std::vector<uint8_t> buf_;  // raw bytes; index_ is the read position
    size_t index_ = 0;
    int audioChannel_ = -1;
    int videoId_ = 0xE0;
    PspByteQueue audio_;
    PspH264AuSplitter video_;
    uint8_t audioFrame_[0x2010];
};
