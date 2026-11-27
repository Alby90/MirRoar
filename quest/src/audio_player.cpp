#include "audio_player.h"

#include <string.h>
#include "common.h"

void AudioPlayer::open(int sampleRate) {
    close();
    std::lock_guard<std::mutex> lk(m_);
    rate_ = sampleRate;
    ring_.assign((size_t)rate_ * 2 /*1s*/ * 2, 0);
    readPos_ = writePos_ = count_ = 0;
    primed_ = false;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rate_);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, &AudioPlayer::dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(b, &AudioPlayer::errorCallback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return;
    }
    AAudioStream_requestStart(stream_);
    LOGI("audio started %d Hz", rate_);
}

void AudioPlayer::close() {
    AAudioStream* s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s = stream_;
        stream_ = nullptr;
    }
    if (s) {
        AAudioStream_requestStop(s);
        AAudioStream_close(s);
    }
}

void AudioPlayer::push(const int16_t* stereo, size_t frames) {
    std::lock_guard<std::mutex> lk(m_);
    if (!stream_) return;
    const size_t cap = ring_.size() / 2;
    const size_t maxBuffered = (size_t)rate_ * 120 / 1000;  // keep latency bounded
    const size_t target = (size_t)rate_ * 50 / 1000;
    for (size_t i = 0; i < frames; ++i) {
        if (count_ >= cap) break;
        ring_[writePos_ * 2] = stereo[i * 2];
        ring_[writePos_ * 2 + 1] = stereo[i * 2 + 1];
        writePos_ = (writePos_ + 1) % cap;
        ++count_;
    }
    if (count_ > maxBuffered) {  // drift / burst: skip ahead
        size_t drop = count_ - target;
        readPos_ = (readPos_ + drop) % cap;
        count_ -= drop;
    }
}

aaudio_data_callback_result_t AudioPlayer::dataCallback(AAudioStream*, void* user, void* audio, int32_t frames) {
    auto* self = (AudioPlayer*)user;
    int16_t* out = (int16_t*)audio;
    std::lock_guard<std::mutex> lk(self->m_);
    const size_t cap = self->ring_.size() / 2;
    const size_t prime = (size_t)self->rate_ * 40 / 1000;
    if (!self->primed_ && self->count_ >= prime) self->primed_ = true;
    for (int32_t i = 0; i < frames; ++i) {
        if (self->primed_ && self->count_ > 0) {
            out[i * 2] = self->ring_[self->readPos_ * 2];
            out[i * 2 + 1] = self->ring_[self->readPos_ * 2 + 1];
            self->readPos_ = (self->readPos_ + 1) % cap;
            --self->count_;
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
            self->primed_ = false;  // underrun: rebuffer
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::errorCallback(AAudioStream*, void*, aaudio_result_t err) {
    LOGW("AAudio error: %s", AAudio_convertResultToText(err));
}
