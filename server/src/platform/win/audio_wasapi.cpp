#include "audio_wasapi.h"

#include <initguid.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <vector>
#include "winutil.h"

void WasapiCapture::start(ConfigCallback onConfig, DataCallback onData) {
    stop();
    onConfig_ = std::move(onConfig);
    onData_ = std::move(onData);
    running_ = true;
    thread_ = std::thread(&WasapiCapture::run, this);
}

void WasapiCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

static inline int16_t clamp16(float f) {
    int v = (int)(f * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

void WasapiCapture::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<int16_t> out;
    while (running_) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> cap;
        WAVEFORMATEX* fmt = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                             __uuidof(IMMDeviceEnumerator), (void**)en.put())) &&
                  SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())) &&
                  SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.put())) &&
                  SUCCEEDED(client->GetMixFormat(&fmt)) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, fmt,
                                               nullptr)) &&
                  SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), (void**)cap.put())) &&
                  SUCCEEDED(client->Start());
        if (!ok) {
            if (fmt) CoTaskMemFree(fmt);
            logf("Audio: cannot start capture, retrying in 2 s");
            for (int i = 0; i < 20 && running_; ++i) Sleep(100);
            continue;
        }

        bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = (WAVEFORMATEXTENSIBLE*)fmt;
            isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        const int channels = fmt->nChannels;
        const int bits = fmt->wBitsPerSample;
        if (onConfig_) onConfig_(fmt->nSamplesPerSec);

        bool failed = false;
        while (running_ && !failed) {
            Sleep(5);
            UINT32 packet = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    failed = true;
                    break;
                }
                out.resize((size_t)frames * 2);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(out.begin(), out.end(), 0);
                } else {
                    for (UINT32 i = 0; i < frames; ++i) {
                        for (int c = 0; c < 2; ++c) {
                            int sc = c < channels ? c : 0;
                            size_t idx = (size_t)i * channels + sc;
                            int16_t v = 0;
                            if (isFloat && bits == 32)
                                v = clamp16(((const float*)data)[idx]);
                            else if (bits == 16)
                                v = ((const int16_t*)data)[idx];
                            else if (bits == 32)
                                v = (int16_t)(((const int32_t*)data)[idx] >> 16);
                            else if (bits == 24) {
                                const uint8_t* s = data + idx * 3;
                                v = (int16_t)((s[2] << 8) | s[1]);
                            }
                            out[(size_t)i * 2 + c] = v;
                        }
                    }
                }
                cap->ReleaseBuffer(frames);
                if (onData_ && frames) onData_(out.data(), frames);
            }
            if (FAILED(cap->GetNextPacketSize(&packet))) failed = true;  // device changed / lost
        }
        client->Stop();
        CoTaskMemFree(fmt);
    }
    CoUninitialize();
}

std::unique_ptr<AudioCapture> createAudioCapture() { return std::make_unique<WasapiCapture>(); }
