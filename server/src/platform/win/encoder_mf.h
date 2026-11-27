// MirRoar server - H.264 encoder on top of Media Foundation (hardware MFT preferred, software fallback).
#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#ifdef __MINGW32__
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
#endif
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "winutil.h"

class MfEncoder : public VideoEncoder {
public:
    ~MfEncoder() override { shutdown(); }

    bool init(int width, int height, int fps, int bitrateKbps, const std::string& hint, Callback cb) override;
    void shutdown() override;
    // Submits one packed NV12 frame (width*height*3/2). Returns false if the encoder could not accept it.
    bool encode(const uint8_t* nv12, int64_t timestampUs, bool forceKey) override;


private:
    bool tryCreate(IMFActivate* act, bool hw);
    bool configure();
    void eventLoop();
    void processOutput();
    void deliver(IMFSample* sample);

    int width_ = 0, height_ = 0, fps_ = 60, bitrateKbps_ = 0;
    Callback cb_;
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codecApi_;
    ComPtr<IMFMediaEventGenerator> events_;
    DWORD inId_ = 0, outId_ = 0;
    bool async_ = false;
    std::thread eventThread_;
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::condition_variable cv_;
    int needInput_ = 0;
    std::vector<uint8_t> seqHeader_;  // SPS/PPS from the output media type
};
