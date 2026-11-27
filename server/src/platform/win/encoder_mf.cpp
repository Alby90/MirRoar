#include <initguid.h>  // instantiate CODECAPI_* / MF GUIDs in this translation unit
#include "encoder_mf.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

#include <algorithm>
#include <cctype>

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static void setCodecValue(ICodecAPI* api, const GUID& g, UINT32 v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&g, &var);
}

static std::string activateName(IMFActivate* act) {
    WCHAR* n = nullptr;
    UINT32 len = 0;
    std::string s = "?";
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &n, &len))) {
        s = wideToUtf8(n);
        CoTaskMemFree(n);
    }
    return s;
}

// Returns true if an Annex-B buffer contains a NAL unit of the given type.
static bool hasNal(const uint8_t* d, size_t n, int type) {
    for (size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1f) == type) return true;
            i += 2;
        }
    }
    return false;
}

bool MfEncoder::init(int width, int height, int fps, int bitrateKbps, const std::string& hint, Callback cb) {
    shutdown();
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrateKbps_ = bitrateKbps;
    cb_ = std::move(cb);

    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    std::string h = lower(hint);

    for (int pass = 0; pass < 2; ++pass) {
        bool hw = pass == 0;
        if (hw && h == "software") continue;
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        UINT32 flags = (hw ? MFT_ENUM_FLAG_HARDWARE : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)) |
                       MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, hw ? nullptr : &inInfo, &outInfo, &acts, &count)))
            continue;
        // Order: encoders matching the hint first.
        std::vector<IMFActivate*> list(acts, acts + count);
        if (!h.empty() && h != "software")
            std::stable_partition(list.begin(), list.end(),
                                  [&](IMFActivate* a) { return lower(activateName(a)).find(h) != std::string::npos; });
        bool ok = false;
        for (IMFActivate* a : list) {
            if (!ok && tryCreate(a, hw)) ok = true;
        }
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (ok) return true;
    }
    logf("No usable H.264 encoder found");
    return false;
}

bool MfEncoder::tryCreate(IMFActivate* act, bool hw) {
    name = activateName(act);
    hardware = hw;
    activate_ = ComPtr<IMFActivate>(act);
    act->AddRef();
    if (FAILED(act->ActivateObject(__uuidof(IMFTransform), (void**)mft_.put()))) {
        logf("Encoder '%s': activation failed", name.c_str());
        activate_.reset();
        return false;
    }
    if (configure()) {
        logf("Encoder: %s (%s, %s) %dx%d @%dfps %d kbps", name.c_str(), hw ? "hardware" : "software",
             async_ ? "async" : "sync", width_, height_, fps_, bitrateKbps_);
        return true;
    }
    logf("Encoder '%s': configuration failed, trying the next one", name.c_str());
    events_.reset();
    codecApi_.reset();
    mft_.reset();
    act->ShutdownObject();
    activate_.reset();
    return false;
}

bool MfEncoder::configure() {
    HRESULT hr;
    ComPtr<IMFAttributes> attrs;
    async_ = false;
    if (SUCCEEDED(mft_->GetAttributes(attrs.put())) && attrs) {
        UINT32 isAsync = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            async_ = true;
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    if (mft_.as(codecApi_) == S_OK) {
        setCodecValue(codecApi_.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodecValue(codecApi_.get(), CODECAPI_AVEncCommonMeanBitRate, (UINT32)bitrateKbps_ * 1000);
        setCodecValue(codecApi_.get(), CODECAPI_AVLowLatencyMode, 1);
        setCodecValue(codecApi_.get(), CODECAPI_AVEncMPVGOPSize, (UINT32)fps_ * 60);
        setCodecValue(codecApi_.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setCodecValue(codecApi_.get(), CODECAPI_AVEncCommonQualityVsSpeed, 50);
    }

    if (FAILED(mft_->GetStreamIDs(1, &inId_, 1, &outId_))) {
        inId_ = 0;
        outId_ = 0;
    }

    ComPtr<IMFMediaType> out;
    MFCreateMediaType(out.put());
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrateKbps_ * 1000);
    MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    hr = mft_->SetOutputType(outId_, out.get(), 0);
    if (FAILED(hr)) {
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = mft_->SetOutputType(outId_, out.get(), 0);
    }
    if (FAILED(hr)) {
        logf("  SetOutputType failed (0x%08lx)", hr);
        return false;
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    in->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    hr = mft_->SetInputType(inId_, in.get(), 0);
    if (FAILED(hr)) {
        logf("  SetInputType failed (0x%08lx)", hr);
        return false;
    }

    // Grab SPS/PPS if the encoder exposes them on the output type.
    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(mft_->GetOutputCurrentType(outId_, cur.put()))) {
        UINT32 sz = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 0) {
            seqHeader_.resize(sz);
            cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader_.data(), sz, nullptr);
        }
    }

    if (async_) {
        if (FAILED(mft_.as(events_))) return false;
    }
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return false;

    needInput_ = 0;
    running_ = true;
    if (async_) eventThread_ = std::thread(&MfEncoder::eventLoop, this);
    return true;
}

void MfEncoder::shutdown() {
    running_ = false;
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.as(sd))) sd->Shutdown();
    }
    cv_.notify_all();
    if (eventThread_.joinable()) eventThread_.join();
    events_.reset();
    codecApi_.reset();
    mft_.reset();
    if (activate_) activate_->ShutdownObject();
    activate_.reset();
    seqHeader_.clear();
}

void MfEncoder::eventLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(events_->GetEvent(0, ev.put()))) break;
        MediaEventType t;
        ev->GetType(&t);
        if (t == METransformNeedInput) {
            std::lock_guard<std::mutex> lk(m_);
            ++needInput_;
            cv_.notify_all();
        } else if (t == METransformHaveOutput) {
            processOutput();
        }
    }
    CoUninitialize();
}

bool MfEncoder::encode(const uint8_t* nv12, int64_t timestampUs, bool forceKey) {
    if (!mft_) return false;
    if (async_) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(50), [&] { return needInput_ > 0 || !running_; }))
            return false;
        if (!running_) return false;
        --needInput_;
    }

    const DWORD size = (DWORD)width_ * height_ * 3 / 2;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(size, buf.put()))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, nv12, size);
    buf->Unlock();
    buf->SetCurrentLength(size);
    ComPtr<IMFSample> sample;
    MFCreateSample(sample.put());
    sample->AddBuffer(buf.get());
    sample->SetSampleTime(timestampUs * 10);
    sample->SetSampleDuration(10000000LL / fps_);

    if (forceKey) setCodecValue(codecApi_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

    HRESULT hr = mft_->ProcessInput(inId_, sample.get(), 0);
    if (FAILED(hr)) {
        if (hr != MF_E_NOTACCEPTING) logf("ProcessInput failed (0x%08lx)", hr);
        return false;
    }
    if (!async_) {
        // Synchronous MFT: drain everything available now.
        for (int i = 0; i < 8; ++i) {
            MFT_OUTPUT_STREAM_INFO si{};
            mft_->GetOutputStreamInfo(outId_, &si);
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = outId_;
            ComPtr<IMFSample> outSample;
            if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> ob2;
                MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, size), ob2.put());
                MFCreateSample(outSample.put());
                outSample->AddBuffer(ob2.get());
                ob.pSample = outSample.get();
            }
            DWORD status = 0;
            hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, t.put()))) mft_->SetOutputType(outId_, t.get(), 0);
                continue;
            }
            if (FAILED(hr)) break;
            deliver(ob.pSample);
            if (!outSample && ob.pSample) ob.pSample->Release();
        }
    }
    return true;
}

void MfEncoder::processOutput() {
    MFT_OUTPUT_STREAM_INFO si{};
    mft_->GetOutputStreamInfo(outId_, &si);
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = outId_;
    ComPtr<IMFSample> outSample;
    if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, (DWORD)width_ * height_), b.put());
        MFCreateSample(outSample.put());
        outSample->AddBuffer(b.get());
        ob.pSample = outSample.get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, t.put()))) mft_->SetOutputType(outId_, t.get(), 0);
        return;
    }
    if (FAILED(hr)) return;
    deliver(ob.pSample);
    if (!outSample && ob.pSample) ob.pSample->Release();
}

void MfEncoder::deliver(IMFSample* sample) {
    if (!sample) return;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(buf.put()))) return;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&p, nullptr, &len))) return;
    EncodedPacket pkt;
    pkt.key = hasNal(p, len, 5) || MFGetAttributeUINT32(sample, MFSampleExtension_CleanPoint, 0) != 0;
    if (pkt.key && !hasNal(p, len, 7) && !seqHeader_.empty()) pkt.data = seqHeader_;
    pkt.data.insert(pkt.data.end(), p, p + len);
    buf->Unlock();
    LONGLONG t = 0;
    sample->GetSampleTime(&t);
    pkt.timestampUs = t / 10;
    pkt.outputUs = nowUs();
    if (cb_ && len > 0) cb_(std::move(pkt));
}

std::unique_ptr<VideoEncoder> createVideoEncoder() { return std::make_unique<MfEncoder>(); }

std::vector<std::string> listVideoEncoders() {
    std::vector<std::string> names;
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    for (int pass = 0; pass < 2; ++pass) {
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        UINT32 flags = (pass == 0 ? MFT_ENUM_FLAG_HARDWARE : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)) |
                       MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, pass == 0 ? nullptr : &inInfo, &outInfo, &acts, &count)))
            continue;
        for (UINT32 i = 0; i < count; ++i) {
            names.push_back(activateName(acts[i]));
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return names;
}
