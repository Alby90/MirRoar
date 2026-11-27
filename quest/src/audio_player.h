// MirRoar Quest client - low-latency PCM playback with AAudio and a small jitter buffer.
#pragma once
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <vector>

class AudioPlayer {
public:
    ~AudioPlayer() { close(); }
    void open(int sampleRate);
    void close();
    void push(const int16_t* stereo, size_t frames);  // network thread

private:
    static aaudio_data_callback_result_t dataCallback(AAudioStream* s, void* user, void* audio, int32_t frames);
    static void errorCallback(AAudioStream* s, void* user, aaudio_result_t err);

    std::mutex m_;
    AAudioStream* stream_ = nullptr;
    int rate_ = 48000;
    std::vector<int16_t> ring_;  // interleaved stereo
    size_t readPos_ = 0, writePos_ = 0, count_ = 0;  // in frames
    bool primed_ = false;
};
