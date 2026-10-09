#include "hle/psp_mpeg_demux.h"

// Ported from PPSSPP (GPL-2.0-or-later): Core/HW/MpegDemux.cpp (demux,
// readPesHeader, skipPackHeader, ATRAC frame extraction) and
// Core/HW/BufferQueue.h (timestamped FIFO).

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t PACKET_START_CODE_MASK = 0xFFFFFF00U;
constexpr uint32_t PACKET_START_CODE_PREFIX = 0x00000100U;

// http://dvd.sourceforge.net/dvdinfo/mpeghdrs.html
constexpr uint32_t USER_DATA_START_CODE = 0x000001B2U;
constexpr uint32_t PACK_START_CODE = 0x000001BAU;
constexpr uint32_t SYSTEM_HEADER_START_CODE = 0x000001BBU;
constexpr uint32_t PRIVATE_STREAM_1 = 0x000001BDU;
constexpr uint32_t PADDING_STREAM = 0x000001BEU;
constexpr uint32_t PRIVATE_STREAM_2 = 0x000001BFU;

constexpr size_t MAX_PENDING_MARKS = 4096;
constexpr size_t MAX_AU_BYTES = 16u << 20;  // a single AU larger than this is junk

bool is_vcl(int nal_type) { return nal_type == 1 || nal_type == 5; }

}  // namespace

// ---------------------------------------------------------------------------
// PspByteQueue
// ---------------------------------------------------------------------------

void PspByteQueue::push(const uint8_t* data, size_t n, int64_t pts) {
    if (n == 0) {
        return;
    }
    if (pts != 0) {
        marks_.push_back({base_ + size(), pts});
        if (marks_.size() > MAX_PENDING_MARKS) {
            marks_.pop_front();
        }
    }
    buf_.insert(buf_.end(), data, data + n);
}

size_t PspByteQueue::peek(uint8_t* dst, size_t max) const {
    const size_t n = std::min(max, size());
    if (n != 0) {
        std::memcpy(dst, buf_.data() + head_, n);
    }
    return n;
}

size_t PspByteQueue::pop(size_t n, int64_t* pts) {
    n = std::min(n, size());
    if (pts) {
        *pts = 0;
    }
    while (!marks_.empty() && marks_.front().offset < base_ + n) {
        if (pts && *pts == 0) {
            *pts = marks_.front().pts;
        }
        marks_.pop_front();
    }
    head_ += n;
    base_ += n;
    if (head_ == buf_.size()) {
        buf_.clear();
        head_ = 0;
    } else if (head_ > 65536 && head_ > buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
    return n;
}

void PspByteQueue::clear() {
    base_ += size();
    buf_.clear();
    head_ = 0;
    marks_.clear();
}

// ---------------------------------------------------------------------------
// PspH264AuSplitter
// ---------------------------------------------------------------------------

void PspH264AuSplitter::push(const uint8_t* data, size_t n, int64_t pts, int64_t dts) {
    if (n == 0) {
        return;
    }
    if (pts != PSP_MPEG_NO_TIMESTAMP || dts != PSP_MPEG_NO_TIMESTAMP) {
        marks_.push_back({base_ + buf_.size(), pts, dts});
        if (marks_.size() > MAX_PENDING_MARKS) {
            marks_.pop_front();
        }
    }
    buf_.insert(buf_.end(), data, data + n);
}

void PspH264AuSplitter::clear() {
    base_ += buf_.size();
    buf_.clear();
    head_ = scan_ = auStart_ = auNal_ = 0;
    haveAu_ = false;
    auHasVcl_ = false;
    marks_.clear();
}

void PspH264AuSplitter::emit(size_t end, PspAvcAccessUnit* out) {
    out->data.assign(buf_.begin() + static_cast<std::ptrdiff_t>(auStart_),
                     buf_.begin() + static_cast<std::ptrdiff_t>(end));
    out->pts = PSP_MPEG_NO_TIMESTAMP;
    out->dts = PSP_MPEG_NO_TIMESTAMP;
    // The AU takes the newest timestamp mark at or before its first NAL; older
    // marks belong to AUs that were already returned (or to junk).
    const uint64_t nal_abs = base_ + auNal_;
    while (!marks_.empty() && marks_.front().offset <= nal_abs) {
        out->pts = marks_.front().pts;
        out->dts = marks_.front().dts;
        marks_.pop_front();
    }
    head_ = end;
}

bool PspH264AuSplitter::pop(PspAvcAccessUnit* out, bool flush) {
    const size_t n = buf_.size();
    bool result = false;
    for (;;) {
        // Locate the next "00 00 01" with its NAL header byte present.
        size_t i = std::max(scan_, head_);
        bool found = false;
        while (i + 3 < n) {
            const uint8_t c = buf_[i + 2];
            if (c > 1) {
                i += 3;
            } else if (c == 1) {
                if (buf_[i] == 0 && buf_[i + 1] == 0) {
                    found = true;
                    break;
                }
                i += 3;
            } else {
                i++;
            }
        }
        if (!found) {
            // Resume 3 bytes back so a start code split across pushes is seen.
            scan_ = n >= 3 ? std::max(head_, n - 3) : head_;
            break;
        }

        const size_t nal_pos = i + 3;
        const int type = buf_[nal_pos] & 0x1F;
        const size_t start = (i > head_ && buf_[i - 1] == 0) ? i - 1 : i;

        if (!haveAu_) {
            haveAu_ = true;
            auStart_ = start;
            auNal_ = nal_pos;
            auHasVcl_ = is_vcl(type);
            scan_ = nal_pos;
            continue;
        }

        // first_mb_in_slice is the first ue(v) of a slice header: the value 0
        // is the single bit '1'.
        bool first_mb_zero = false;
        if (is_vcl(type)) {
            if (nal_pos + 1 >= n) {
                scan_ = i;  // need one more byte to classify this slice
                if (!flush) {
                    break;
                }
                scan_ = nal_pos;  // flush: treat as a continuation
                auHasVcl_ = true;
                continue;
            }
            first_mb_zero = (buf_[nal_pos + 1] & 0x80) != 0;
        }
        bool boundary = false;
        switch (type) {
            case 9:  // access unit delimiter
                boundary = true;
                break;
            case 6:   // SEI
            case 7:   // SPS
            case 8:   // PPS
            case 14:  // prefix NAL
            case 15:  // subset SPS
            case 16:
            case 17:
            case 18:
                boundary = auHasVcl_;
                break;
            case 1:
            case 5:
                boundary = auHasVcl_ && first_mb_zero;
                break;
            default:
                break;
        }
        if (!boundary) {
            auHasVcl_ = auHasVcl_ || is_vcl(type);
            scan_ = nal_pos;
            continue;
        }

        if (auHasVcl_) {
            emit(start, out);
            result = true;
        } else {
            head_ = start;  // a delimiter/parameter-set prefix with no picture
        }
        auStart_ = start;
        auNal_ = nal_pos;
        auHasVcl_ = is_vcl(type);
        scan_ = nal_pos;
        if (result) {
            break;
        }
    }

    if (!result && flush && haveAu_) {
        if (auHasVcl_) {
            emit(n, out);
            result = true;
        }
        haveAu_ = false;
        auHasVcl_ = false;
        head_ = n;
        scan_ = n;
    }
    if (!haveAu_) {
        // Junk before the first start code can never become an AU.
        head_ = std::max(head_, std::min(scan_, n));
    } else if (n - auStart_ > MAX_AU_BYTES) {
        haveAu_ = false;
        auHasVcl_ = false;
        head_ = scan_ = n;
    }

    // Compact consumed bytes away.
    if (head_ == n) {
        base_ += n;
        buf_.clear();
        head_ = scan_ = auStart_ = auNal_ = 0;
    } else if (head_ > 65536 && head_ > n / 2) {
        const size_t d = head_;
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(d));
        base_ += d;
        head_ = 0;
        scan_ -= std::min(scan_, d);
        if (haveAu_) {
            auStart_ -= d;
            auNal_ -= d;
        } else {
            auStart_ = auNal_ = 0;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// PspMpegDemux
// ---------------------------------------------------------------------------

PspMpegDemux::PspMpegDemux(size_t capacity) : capacity_(capacity) {
    std::memset(audioFrame_, 0, sizeof(audioFrame_));
}

bool PspMpegDemux::addStreamData(const uint8_t* data, size_t n) {
    if (n > capacity_ - std::min(capacity_, buf_.size())) {
        return false;
    }
    buf_.insert(buf_.end(), data, data + n);
    return true;
}

void PspMpegDemux::setAudioChannel(int channel) {
    if (audioChannel_ != channel) {
        audioChannel_ = channel;
        audio_.clear();
    }
}

int PspMpegDemux::readPesHeader(PesHeader& pes, int length, int startCode) {
    int c = 0;
    while (length > 0) {
        c = read8();
        length--;
        if (c != 0xFF) {
            break;
        }
    }
    if ((c & 0xC0) == 0x40) {
        read8();
        c = read8();
        length -= 2;
    }
    pes.pts = 0;
    pes.dts = 0;
    pes.hasPts = false;
    if ((c & 0xE0) == 0x20) {
        pes.dts = pes.pts = readPts(c);
        pes.hasPts = true;
        length -= 4;
        if ((c & 0x10) != 0) {
            pes.dts = readPts();
            length -= 5;
        }
    } else if ((c & 0xC0) == 0x80) {
        int flags = read8();
        int headerLength = read8();
        length -= 2;
        length -= headerLength;
        if ((flags & 0x80) != 0) {
            pes.dts = pes.pts = readPts();
            pes.hasPts = true;
            headerLength -= 5;
            if ((flags & 0x40) != 0) {
                pes.dts = readPts();
                headerLength -= 5;
            }
        }
        if ((flags & 0x3F) != 0 && headerLength == 0) {
            flags &= 0xC0;
        }
        if ((flags & 0x01) != 0) {
            int pesExt = read8();
            headerLength--;
            int skipBytes = (pesExt >> 4) & 0x0B;
            skipBytes += skipBytes & 0x09;
            if ((pesExt & 0x40) != 0 || skipBytes > headerLength) {
                pesExt = skipBytes = 0;
            }
            skip(skipBytes);
            headerLength -= skipBytes;
            if ((pesExt & 0x01) != 0) {
                int ext2Length = read8();
                headerLength--;
                if ((ext2Length & 0x7F) != 0) {
                    int idExt = read8();
                    headerLength--;
                    if ((idExt & 0x80) == 0) {
                        startCode = ((startCode & 0xFF) << 8) | idExt;
                    }
                }
            }
        }
        skip(headerLength);
    }
    if (startCode == static_cast<int>(PRIVATE_STREAM_1)) {
        int channel = read8();
        pes.channel = channel;
        length--;
        if (channel >= 0x80 && channel <= 0xCF) {
            // Skip audio header. Standard PSP audio sub-streams have a 3 or
            // 4 byte sub-header; ATRAC3+ (0x90) and some others have an
            // additional byte in the sub-header.
            skip(3);
            length -= 3;
            if (channel >= 0xB0 && channel <= 0xBF) {
                skip(1);
                length--;
            }
        } else {
            // PSP audio has additional 3 bytes in header.
            skip(3);
            length -= 3;
        }
    }
    return length;
}

void PspMpegDemux::demuxPes(int startCode, int length) {
    // The packet's declared end is authoritative; PPSSPP trusts the header
    // arithmetic instead, which a crafted header can make inconsistent.
    const size_t pes_end = std::min(buf_.size(), index_ + static_cast<size_t>(std::max(length, 0)));
    PesHeader pes;
    pes.channel = audioChannel_;
    const int payload = readPesHeader(pes, length, startCode);
    const size_t avail = index_ < pes_end ? pes_end - index_ : 0;
    const size_t n = payload > 0 ? std::min(static_cast<size_t>(payload), avail) : 0;

    if (startCode == static_cast<int>(PRIVATE_STREAM_1)) {
        if (pes.channel == audioChannel_ || audioChannel_ < 0) {
            audioChannel_ = pes.channel;
            if (n != 0 && audio_.size() + n <= capacity_) {
                audio_.push(buf_.data() + index_, n, pes.hasPts ? pes.pts : 0);
            }
        }
    } else if ((startCode & 0xFF) == videoId_ && n != 0) {
        video_.push(buf_.data() + index_, n,
                    pes.hasPts ? pes.pts : PSP_MPEG_NO_TIMESTAMP,
                    pes.hasPts ? pes.dts : PSP_MPEG_NO_TIMESTAMP);
    }
    index_ = pes_end;
}

bool PspMpegDemux::skipPackHeader() {
    // MPEG version / SCR
    if ((read8() & 0xC4) != 0x44) {
        return false;
    }
    skip(1);
    if ((read8() & 0x04) != 0x04) {
        return false;
    }
    skip(1);
    if ((read8() & 0x04) != 0x04) {
        return false;
    }
    // SCR_ext
    if ((read8() & 0x01) != 0x01) {
        return false;
    }

    int muxrate = read24();
    if ((muxrate & 3) != 3) {
        return false;
    }
    int stuffing = read8() & 7;
    while (stuffing > 0) {
        if (read8() != 0xFF) {
            return false;
        }
        --stuffing;
    }
    return true;
}

bool PspMpegDemux::demux() {
    bool looksValid = false;
    bool needMore = false;
    while (index_ < buf_.size() && !needMore) {
        // Search for start code
        uint32_t startCode = 0xFF;
        while ((startCode & PACKET_START_CODE_MASK) != PACKET_START_CODE_PREFIX && !isEOF()) {
            startCode = (startCode << 8) | static_cast<uint32_t>(read8());
        }
        // Not enough data available yet: rewind so the start code (or the
        // bytes of a split one) is found again once more data arrives.
        if (buf_.size() - index_ < 16) {
            index_ = index_ >= 4 ? index_ - 4 : 0;
            break;
        }

        switch (startCode) {
            case PACK_START_CODE:
                if (skipPackHeader()) {
                    looksValid = true;
                }
                break;
            case SYSTEM_HEADER_START_CODE:
            case PADDING_STREAM:
            case PRIVATE_STREAM_2: {
                looksValid = true;
                int length = read16();
                if (buf_.size() - index_ < static_cast<size_t>(length)) {
                    index_ = index_ >= 6 ? index_ - 6 : 0;
                    needMore = true;
                    break;
                }
                skip(length);
                break;
            }
            case PRIVATE_STREAM_1:
            case 0x1E0: case 0x1E1: case 0x1E2: case 0x1E3:
            case 0x1E4: case 0x1E5: case 0x1E6: case 0x1E7:
            case 0x1E8: case 0x1E9: case 0x1EA: case 0x1EB:
            case 0x1EC: case 0x1ED: case 0x1EE: case 0x1EF: {
                // Audio (private stream 1) or video PES packet.
                int length = read16();
                // Check for PES header marker.
                looksValid = index_ < buf_.size() && (buf_[index_] & 0xC0) == 0x80;
                if (buf_.size() - index_ < static_cast<size_t>(length)) {
                    index_ = index_ >= 6 ? index_ - 6 : 0;
                    needMore = true;
                    break;
                }
                demuxPes(static_cast<int>(startCode), length);
                if (startCode == PRIVATE_STREAM_1) {
                    looksValid = true;
                }
                break;
            }
            case USER_DATA_START_CODE:
                // User data, probably same as queried by sceMpegGetUserdataAu.
                looksValid = true;
                break;
            default:
                break;
        }
    }
    if (index_ < buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(index_));
    } else {
        buf_.clear();
    }
    index_ = 0;
    return looksValid;
}

bool PspMpegDemux::nextVideoAu(PspAvcAccessUnit* out, bool flush) {
    return video_.pop(out, flush);
}

size_t PspMpegDemux::syncAudio() {
    size_t got = audio_.peek(audioFrame_, sizeof(audioFrame_));
    if (got < 4) {
        return 0;
    }
    size_t h = 0;
    bool found = false;
    for (; h + 1 < got; h++) {
        if (audioFrame_[h] == 0x0F && audioFrame_[h + 1] == 0xD0) {
            found = true;
            break;
        }
    }
    if (!found) {
        // Nothing here can start a frame (keep the last byte: it may be the
        // first half of a header whose second half has not arrived).
        audio_.pop(got - 1);
        return 0;
    }
    if (h > 0) {
        // Garbage before the header, e.g. after a chapter transition.
        audio_.pop(h);
        got = audio_.peek(audioFrame_, sizeof(audioFrame_));
    }
    return got >= 4 ? got : 0;
}

bool PspMpegDemux::hasNextAudioFrame(int* gotSize, int* frameSizeOut, int* headerCode1,
                                     int* headerCode2) {
    const size_t got = syncAudio();
    if (got == 0) {
        return false;
    }
    const int code1 = audioFrame_[2];
    const int code2 = audioFrame_[3];
    // Frame size = (13-bit unit count) * 8 + header; PPSSPP ORs the high bits
    // in after the multiply, which only matters above 2 KiB frames.
    const size_t frameSize = ((static_cast<size_t>(code1 & 0x03) << 8 | code2) * 8) + 0x10;
    if (frameSize > got) {
        return false;
    }
    if (gotSize) {
        *gotSize = static_cast<int>(got);
    }
    if (frameSizeOut) {
        *frameSizeOut = static_cast<int>(frameSize);
    }
    if (headerCode1) {
        *headerCode1 = code1;
    }
    if (headerCode2) {
        *headerCode2 = code2;
    }
    return true;
}

int PspMpegDemux::getNextAudioFrame(const uint8_t** buf, int* headerCode1, int* headerCode2,
                                    int64_t* pts) {
    int frameSize = 0;
    if (!hasNextAudioFrame(nullptr, &frameSize, headerCode1, headerCode2)) {
        return 0;
    }
    if (frameSize <= 8) {
        audio_.pop(2);  // degenerate header: step past it and resync
        return 0;
    }
    audio_.pop(static_cast<size_t>(frameSize), pts);
    if (buf) {
        *buf = audioFrame_ + 8;
    }
    return frameSize - 8;
}
