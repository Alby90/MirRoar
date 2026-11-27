// MirRoar server - system audio capture (WASAPI loopback) -> 16-bit stereo PCM.
#pragma once
#include <atomic>
#include <functional>
#include <thread>
#include <stdint.h>
#include "../platform.h"

class WasapiCapture : public AudioCapture {
public:
    ~WasapiCapture() override { stop(); }
    void start(ConfigCallback onConfig, DataCallback onData) override;
    void stop() override;

private:
    void run();
    std::thread thread_;
    std::atomic<bool> running_{false};
    ConfigCallback onConfig_;
    DataCallback onData_;
};
