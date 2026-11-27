#include "video_decoder.h"

#include <media/NdkMediaFormat.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include "common.h"

// Finds the NAL unit of a given type (including its start code) inside an Annex-B buffer.
static bool findNal(const uint8_t* d, size_t n, int type, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i + 3 < n) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) sc = 3;
        else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) sc = 4;
        if (!sc) {
            ++i;
            continue;
        }
        size_t start = i;
        size_t payload = i + sc;
        size_t j = payload;
        while (j + 3 < n && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (d[j + 2] == 0 && d[j + 3] == 1)))) ++j;
        if (j + 3 >= n) j = n;
        if ((d[payload] & 0x1f) == type) {
            out.assign(d + start, d + j);
            return true;
        }
        i = j;
    }
    return false;
}

bool VideoDecoder::create(const uint8_t* data, size_t len, int width, int height) {
    std::vector<uint8_t> sps, pps;
    if (!findNal(data, len, 7, sps) || !findNal(data, len, 8, pps)) {
        LOGW("keyframe without SPS/PPS");
        return false;
    }
    media_status_t st = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
                                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &reader_);
    if (st != AMEDIA_OK) {
        LOGE("AImageReader_newWithUsage failed %d", st);
        return false;
    }
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(reader_, &window);

    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
        LOGE("no avc decoder");
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setBuffer(fmt, "csd-0", sps.data(), sps.size());
    AMediaFormat_setBuffer(fmt, "csd-1", pps.data(), pps.size());
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(fmt, "priority", 0);
    AMediaFormat_setInt32(fmt, "operating-rate", 120);
    st = AMediaCodec_configure(codec_, fmt, window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed %d", st);
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed");
        return false;
    }
    width_ = width;
    height_ = height;
    running_ = true;
    outThread_ = std::thread(&VideoDecoder::outputLoop, this);
    LOGI("decoder started %dx%d", width, height);
    return true;
}

void VideoDecoder::reset() {
    running_ = false;
    if (outThread_.joinable()) outThread_.join();
    std::lock_guard<std::mutex> lk(m_);
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    for (AImage* img : held_) AImage_delete(img);
    held_.clear();
    ++generation_;
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    width_ = height_ = 0;
    waitKey_ = true;
}

bool VideoDecoder::submit(const uint8_t* data, size_t len, uint64_t tsUs, bool key, int width, int height) {
    if (!codec_ || width != width_ || height != height_) {
        if (!key) return false;
        reset();
        bool ok;
        {
            std::lock_guard<std::mutex> lk(m_);  // the render thread reads reader_/codec_ under m_
            ok = create(data, len, width, height);
        }
        if (!ok) {
            reset();
            return false;
        }
    }
    if (waitKey_ && !key) return false;
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 30000);
    if (idx < 0) {
        LOGW("decoder input full, dropping frame");
        waitKey_ = true;
        return false;
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &cap);
    if (!buf || cap < len) {
        AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, tsUs, 0);
        waitKey_ = true;
        return false;
    }
    memcpy(buf, data, len);
    AMediaCodec_queueInputBuffer(codec_, idx, 0, len, tsUs, 0);
    waitKey_ = false;
    return true;
}

void VideoDecoder::outputLoop() {
    while (running_) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
        if (idx >= 0) {
            {
                std::lock_guard<std::mutex> lk(timesM_);
                times_[timesPos_] = {info.presentationTimeUs, monoUs()};
                timesPos_ = (timesPos_ + 1) % 64;
            }
            AMediaCodec_releaseOutputBuffer(codec_, idx, info.size != 0);
            ++decodedFrames;
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            LOGI("decoder output format: %s", AMediaFormat_toString(f));
            AMediaFormat_delete(f);
        }
    }
}

int64_t VideoDecoder::decodedAt(int64_t ptsUs) {
    std::lock_guard<std::mutex> lk(timesM_);
    for (auto& t : times_)
        if (t.pts == ptsUs) return t.at;
    return 0;
}

bool VideoDecoder::acquireLatest(DecodedFrame& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!reader_ || !codec_) return false;
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader_, &img) != AMEDIA_OK || !img) return false;
    AHardwareBuffer* hb = nullptr;
    if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) {
        AImage_delete(img);
        return false;
    }
    AHardwareBuffer_acquire(hb);
    out = DecodedFrame();
    out.image = img;
    out.generation = generation_;
    out.buffer = hb;
    int64_t tsNs = 0;
    if (AImage_getTimestamp(img, &tsNs) == AMEDIA_OK) out.ptsUs = tsNs / 1000;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hb, &desc);
    AImageCropRect crop;
    if (AImage_getCropRect(img, &crop) == AMEDIA_OK && desc.width > 0 && desc.height > 0) {
        out.uMax = (float)(crop.right - crop.left) / (float)desc.width;
        out.vMax = (float)(crop.bottom - crop.top) / (float)desc.height;
        if (out.uMax <= 0 || out.uMax > 1) out.uMax = 1;
        if (out.vMax <= 0 || out.vMax > 1) out.vMax = 1;
    }
    held_.push_back(img);
    return true;
}

void VideoDecoder::release(AImage* img, uint64_t generation) {
    std::lock_guard<std::mutex> lk(m_);
    if (!img || generation != generation_) return;  // freed by an earlier reset()
    auto it = std::find(held_.begin(), held_.end(), img);
    if (it == held_.end()) return;  // already freed by reset()
    AImage_delete(img);
    held_.erase(it);
}
