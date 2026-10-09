#include "hle/psp_media_engine.h"

#include "at3_decoders.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef PSPRECOMP_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

int psp_video_bytes_per_pixel(int pixelMode) {
    switch (pixelMode) {
    case PSP_VIDEO_ABGR8888:
        return 4;
    case PSP_VIDEO_BGR5650:
    case PSP_VIDEO_ABGR5551:
    case PSP_VIDEO_ABGR4444:
        return 2;
    default:
        return 0;
    }
}

void psp_video_convert_line(uint8_t* dst, const uint8_t* rgba, int width, int pixelMode) {
    if (!dst || !rgba || width <= 0) {
        return;
    }
    switch (pixelMode) {
    case PSP_VIDEO_ABGR8888:
        for (int i = 0; i < width; i++) {
            const uint8_t* s = rgba + static_cast<size_t>(i) * 4;
            uint8_t* d = dst + static_cast<size_t>(i) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 0;
        }
        break;
    case PSP_VIDEO_BGR5650:
        for (int i = 0; i < width; i++) {
            const uint8_t* s = rgba + static_cast<size_t>(i) * 4;
            const unsigned px = (static_cast<unsigned>(s[2] >> 3) << 11) |
                                (static_cast<unsigned>(s[1] >> 2) << 5) |
                                static_cast<unsigned>(s[0] >> 3);
            uint8_t* d = dst + static_cast<size_t>(i) * 2;
            d[0] = static_cast<uint8_t>(px & 0xFF);
            d[1] = static_cast<uint8_t>(px >> 8);
        }
        break;
    case PSP_VIDEO_ABGR5551:
        for (int i = 0; i < width; i++) {
            const uint8_t* s = rgba + static_cast<size_t>(i) * 4;
            const unsigned px = (static_cast<unsigned>(s[2] >> 3) << 10) |
                                (static_cast<unsigned>(s[1] >> 3) << 5) |
                                static_cast<unsigned>(s[0] >> 3);
            uint8_t* d = dst + static_cast<size_t>(i) * 2;
            d[0] = static_cast<uint8_t>(px & 0xFF);
            d[1] = static_cast<uint8_t>(px >> 8);
        }
        break;
    case PSP_VIDEO_ABGR4444:
        for (int i = 0; i < width; i++) {
            const uint8_t* s = rgba + static_cast<size_t>(i) * 4;
            const unsigned px = (static_cast<unsigned>(s[2] >> 4) << 8) |
                                (static_cast<unsigned>(s[1] >> 4) << 4) |
                                static_cast<unsigned>(s[0] >> 4);
            uint8_t* d = dst + static_cast<size_t>(i) * 2;
            d[0] = static_cast<uint8_t>(px & 0xFF);
            d[1] = static_cast<uint8_t>(px >> 8);
        }
        break;
    default:
        break;
    }
}

struct PspVideoDecoder::Impl {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool has = false;
#ifdef PSPRECOMP_HAVE_FFMPEG
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    int swsW = 0;
    int swsH = 0;
    AVPixelFormat swsFmt = AV_PIX_FMT_NONE;
    bool draining = false;
    bool storeFrame();
    bool receiveOne();
    int sendAu(const uint8_t* data, int size);
#endif
};

#ifdef PSPRECOMP_HAVE_FFMPEG
bool PspVideoDecoder::Impl::storeFrame() {
    AVFrame* f = frame;
    if (!f || !f->data[0] || f->width <= 0 || f->height <= 0 || f->width > 2048 || f->height > 2048) {
        return false;
    }
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(f->format);
    if (!sws || swsW != f->width || swsH != f->height || swsFmt != fmt) {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        sws = sws_getContext(f->width, f->height, fmt, f->width, f->height, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            return false;
        }
        swsW = f->width;
        swsH = f->height;
        swsFmt = fmt;
    }
    const size_t bytes = static_cast<size_t>(f->width) * static_cast<size_t>(f->height) * 4;
    rgba.assign(bytes, 0);
    const uint8_t* srcSlice[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    const int srcStride[4] = {f->linesize[0], f->linesize[1], f->linesize[2], f->linesize[3]};
    uint8_t* dstSlice[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {f->width * 4, 0, 0, 0};
    if (sws_scale(sws, srcSlice, srcStride, 0, f->height, dstSlice, dstStride) != f->height) {
        return false;
    }
    width = f->width;
    height = f->height;
    has = true;
    return true;
}

bool PspVideoDecoder::Impl::receiveOne() {
    if (!ctx || !frame) {
        return false;
    }
    if (avcodec_receive_frame(ctx, frame) < 0) {
        return false;
    }
    return storeFrame();
}

int PspVideoDecoder::Impl::sendAu(const uint8_t* data, int size) {
    if (av_new_packet(pkt, size) < 0) {
        return AVERROR(ENOMEM);
    }
    std::memcpy(pkt->data, data, static_cast<size_t>(size));
    const int err = avcodec_send_packet(ctx, pkt);
    av_packet_unref(pkt);
    return err;
}
#endif

PspVideoDecoder::PspVideoDecoder() : impl_(std::make_unique<Impl>()) {
#ifdef PSPRECOMP_HAVE_FFMPEG
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        return;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return;
    }
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->max_pixels = 2048LL * 2048LL;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return;
    }
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!frame || !pkt) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return;
    }
    impl_->ctx = ctx;
    impl_->frame = frame;
    impl_->pkt = pkt;
#endif
}

PspVideoDecoder::~PspVideoDecoder() {
#ifdef PSPRECOMP_HAVE_FFMPEG
    if (impl_) {
        if (impl_->sws) {
            sws_freeContext(impl_->sws);
            impl_->sws = nullptr;
        }
        av_frame_free(&impl_->frame);
        av_packet_free(&impl_->pkt);
        avcodec_free_context(&impl_->ctx);
    }
#endif
}

bool PspVideoDecoder::available() {
#ifdef PSPRECOMP_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

bool PspVideoDecoder::decode(const uint8_t* au, size_t size) {
#ifdef PSPRECOMP_HAVE_FFMPEG
    if (!impl_ || !impl_->ctx || !impl_->pkt || !au || size == 0 || size > static_cast<size_t>(INT32_MAX)) {
        return false;
    }
    int err = impl_->sendAu(au, static_cast<int>(size));
    if (err == AVERROR(EAGAIN)) {
        if (!impl_->receiveOne()) {
            return false;
        }
        err = impl_->sendAu(au, static_cast<int>(size));
        if (err < 0) {
            return true;
        }
        if (impl_->receiveOne()) {
            return true;
        }
        return impl_->has;
    }
    if (err < 0) {
        return false;
    }
    return impl_->receiveOne();
#else
    (void)au;
    (void)size;
    return false;
#endif
}

bool PspVideoDecoder::drain() {
#ifdef PSPRECOMP_HAVE_FFMPEG
    if (!impl_ || !impl_->ctx) {
        return false;
    }
    if (!impl_->draining) {
        const int err = avcodec_send_packet(impl_->ctx, nullptr);
        if (err == AVERROR(EAGAIN)) {
            return impl_->receiveOne();
        }
        impl_->draining = true;
    }
    return impl_->receiveOne();
#else
    return false;
#endif
}

bool PspVideoDecoder::hasPicture() const {
    return impl_ && impl_->has;
}

int PspVideoDecoder::width() const {
    return impl_ && impl_->has ? impl_->width : 0;
}

int PspVideoDecoder::height() const {
    return impl_ && impl_->has ? impl_->height : 0;
}

int PspVideoDecoder::writeImage(uint8_t* dst, size_t dstSize, int frameWidth, int pixelMode, int xpos,
                                int ypos, int w, int h) const {
    if (!dst || frameWidth <= 0 || frameWidth > 2048 || xpos < 0 || ypos < 0 || w < 0 || h < 0) {
        return 0;
    }
    const int bpp = psp_video_bytes_per_pixel(pixelMode);
    if (bpp <= 0 || !impl_ || !impl_->has || impl_->width <= 0 || impl_->height <= 0) {
        return 0;
    }
    if (xpos >= impl_->width || ypos >= impl_->height || w == 0 || h == 0) {
        return 0;
    }
    int clipW = w;
    if (clipW > impl_->width - xpos) {
        clipW = impl_->width - xpos;
    }
    int clipH = h;
    if (clipH > impl_->height - ypos) {
        clipH = impl_->height - ypos;
    }
    if (clipW <= 0 || clipH <= 0) {
        return 0;
    }
    int writeW = clipW;
    if (writeW > frameWidth) {
        writeW = frameWidth;
    }
    const size_t stride = static_cast<size_t>(frameWidth) * static_cast<size_t>(bpp);
    const size_t rowBytes = static_cast<size_t>(writeW) * static_cast<size_t>(bpp);
    if (stride == 0 || stride > dstSize || static_cast<size_t>(clipH) > dstSize / stride) {
        return 0;
    }
    const size_t need = stride * static_cast<size_t>(clipH);
    if (need > static_cast<size_t>(INT32_MAX)) {
        return 0;
    }
    const size_t picStride = static_cast<size_t>(impl_->width) * 4;
    const size_t picBytes = picStride * static_cast<size_t>(impl_->height);
    if (impl_->rgba.size() < picBytes) {
        return 0;
    }
    for (int y = 0; y < clipH; y++) {
        const size_t rowOff = static_cast<size_t>(y) * stride;
        if (rowOff > dstSize || dstSize - rowOff < rowBytes) {
            return 0;
        }
        const size_t srcOff = (static_cast<size_t>(ypos + y) * static_cast<size_t>(impl_->width) +
                               static_cast<size_t>(xpos)) *
                              4;
        if (srcOff > impl_->rgba.size() || impl_->rgba.size() - srcOff < static_cast<size_t>(writeW) * 4) {
            return 0;
        }
        psp_video_convert_line(dst + rowOff, impl_->rgba.data() + srcOff, writeW, pixelMode);
    }
    return static_cast<int>(need);
}

void PspVideoDecoder::reset() {
    if (!impl_) {
        return;
    }
    impl_->rgba.clear();
    impl_->width = 0;
    impl_->height = 0;
    impl_->has = false;
#ifdef PSPRECOMP_HAVE_FFMPEG
    impl_->draining = false;
    if (impl_->ctx) {
        avcodec_flush_buffers(impl_->ctx);
    }
    if (impl_->frame) {
        av_frame_unref(impl_->frame);
    }
    if (impl_->sws) {
        sws_freeContext(impl_->sws);
        impl_->sws = nullptr;
        impl_->swsW = 0;
        impl_->swsH = 0;
        impl_->swsFmt = AV_PIX_FMT_NONE;
    }
#endif
}

namespace {

constexpr int kAtracFrames = 2048;
constexpr int kAtracPad = 64;

int16_t clamp16(float f) {
    if (f >= 1.0f) {
        return 32767;
    }
    if (f <= -1.0f) {
        return -32767;
    }
    return static_cast<int16_t>(static_cast<int>(f * 32767.0f));
}

int audio_fail(int16_t* out, int cap) {
    if (out && cap > 0) {
        const int n = cap < kAtracFrames ? cap : kAtracFrames;
        std::memset(out, 0, static_cast<size_t>(n) * 2 * sizeof(int16_t));
    }
    return -1;
}

}  // namespace

struct PspMpegAudioDecoder::Impl {
    ATRAC3PContext* ctx = nullptr;
    int channels = 0;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<uint8_t> padded;
    Impl() : left(kAtracFrames), right(kAtracFrames) {}
    ~Impl() {
        if (ctx) {
            atrac3p_free(ctx);
            ctx = nullptr;
        }
    }
};

PspMpegAudioDecoder::PspMpegAudioDecoder() : impl_(std::make_unique<Impl>()) {}

PspMpegAudioDecoder::~PspMpegAudioDecoder() = default;

int PspMpegAudioDecoder::decode(const uint8_t* frame, int size, int headerCode1, int headerCode2,
                                int16_t* out, int outCapacityFrames) {
    (void)headerCode2;
    if (!out || outCapacityFrames <= 0) {
        return -1;
    }
    if (!impl_ || !frame || size <= 0 || size > 65536) {
        return audio_fail(out, outCapacityFrames);
    }
    const int channels = headerCode1 == 0x24 ? 1 : 2;
    if (!impl_->ctx || impl_->channels != channels) {
        if (impl_->ctx) {
            atrac3p_free(impl_->ctx);
            impl_->ctx = nullptr;
        }
        int blockAlign = size;
        impl_->ctx = atrac3p_alloc(channels, &blockAlign);
        impl_->channels = impl_->ctx ? channels : 0;
        if (!impl_->ctx) {
            return audio_fail(out, outCapacityFrames);
        }
    }
    impl_->padded.assign(static_cast<size_t>(size) + kAtracPad, 0);
    std::memcpy(impl_->padded.data(), frame, static_cast<size_t>(size));
    float* bufs[2] = {impl_->left.data(), impl_->right.data()};
    int nb = 0;
    const int result = atrac3p_decode_frame(impl_->ctx, bufs, &nb, impl_->padded.data(), size);
    if (result < 0 || nb <= 0) {
        return audio_fail(out, outCapacityFrames);
    }
    int frames = nb < kAtracFrames ? nb : kAtracFrames;
    if (frames > outCapacityFrames) {
        frames = outCapacityFrames;
    }
    const float* left = impl_->left.data();
    const float* right = channels == 2 ? impl_->right.data() : impl_->left.data();
    for (int i = 0; i < frames; i++) {
        out[i * 2] = clamp16(left[i]);
        out[i * 2 + 1] = clamp16(right[i]);
    }
    return frames;
}

void PspMpegAudioDecoder::reset() {
    if (!impl_) {
        return;
    }
    if (impl_->ctx) {
        atrac3p_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    impl_->channels = 0;
}
