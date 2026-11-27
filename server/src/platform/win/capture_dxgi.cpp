#include "capture_dxgi.h"

#include <algorithm>

static bool findOutput(int monitorIndex, ComPtr<IDXGIAdapter1>& outAdapter, ComPtr<IDXGIOutput1>& outOutput) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.put()))) return false;
    int idx = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            if (idx == monitorIndex) {
                outAdapter = adapter;
                return SUCCEEDED(output.as(outOutput));
            }
            ++idx;
        }
    }
    return false;
}

std::vector<MonitorDesc> listMonitors() {
    std::vector<MonitorDesc> res;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.put()))) return res;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od;
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            MonitorDesc m;
            RECT r = od.DesktopCoordinates;
            m.x = r.left;
            m.y = r.top;
            m.width = r.right - r.left;
            m.height = r.bottom - r.top;
            m.primary = r.left == 0 && r.top == 0;
            m.name = wideToUtf8(od.DeviceName);
            m.adapter = wideToUtf8(ad.Description);
            res.push_back(m);
        }
    }
    return res;
}

void DesktopCapture::shutdown() {
    if (frameAcquired_ && dupl_) dupl_->ReleaseFrame();
    frameAcquired_ = false;
    outView_.reset();
    inView_.reset();
    vproc_.reset();
    venum_.reset();
    vctx_.reset();
    vdev_.reset();
    staging_.reset();
    nv12Tex_.reset();
    bgra_.reset();
    dupl_.reset();
    ctx_.reset();
    device_.reset();
}

bool DesktopCapture::init(int monitorIndex, int maxWidth, int maxHeight) {
    shutdown();
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    if (!findOutput(monitorIndex, adapter, output)) {
        logf("Monitor %d not found", monitorIndex);
        return false;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    adapterName = wideToUtf8(ad.Description);

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   3, D3D11_SDK_VERSION, device_.put(), nullptr, ctx_.put());
    if (FAILED(hr)) {
        logf("D3D11CreateDevice failed (0x%08lx)", hr);
        return false;
    }

    hr = output->DuplicateOutput(device_.get(), dupl_.put());
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED)
            logf("Secure desktop (UAC/login): waiting...");
        else
            logf("DuplicateOutput failed (0x%08lx)", hr);
        return false;
    }

    DXGI_OUTPUT_DESC od;
    output->GetDesc(&od);
    monRect = od.DesktopCoordinates;
    DXGI_OUTDUPL_DESC dd;
    dupl_->GetDesc(&dd);
    monWidth = (int)dd.ModeDesc.Width;
    monHeight = (int)dd.ModeDesc.Height;
    if (dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 || dd.Rotation == DXGI_MODE_ROTATION_ROTATE270)
        std::swap(monWidth, monHeight);

    // Output size: fit into max bounds (and the H.264 limit of 4096), keep aspect, even dimensions.
    double scale = 1.0;
    int mw = maxWidth > 0 ? std::min(maxWidth, 4096) : 4096;
    int mh = maxHeight > 0 ? std::min(maxHeight, 4096) : 4096;
    scale = std::min(scale, (double)mw / monWidth);
    scale = std::min(scale, (double)mh / monHeight);
    outWidth = ((int)(monWidth * scale)) & ~1;
    outHeight = ((int)(monHeight * scale)) & ~1;

    if (!createConverter()) return false;
    nv12_.assign((size_t)outWidth * outHeight * 3 / 2, 0);
    return true;
}

bool DesktopCapture::createConverter() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = monWidth;
    td.Height = monHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, bgra_.put()))) return false;

    td.Width = outWidth;
    td.Height = outHeight;
    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, nv12Tex_.put()))) {
        logf("Cannot create the NV12 texture");
        return false;
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, staging_.put()))) {
        logf("Cannot create the NV12 staging texture");
        return false;
    }

    if (FAILED(device_.as(vdev_)) || FAILED(ctx_.as(vctx_))) {
        logf("D3D11 video processor not available");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = monWidth;
    cd.InputHeight = monHeight;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = outWidth;
    cd.OutputHeight = outHeight;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, venum_.put()))) return false;
    if (FAILED(vdev_->CreateVideoProcessor(venum_.get(), 0, vproc_.put()))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev_->CreateVideoProcessorInputView(bgra_.get(), venum_.get(), &ivd, inView_.put()))) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev_->CreateVideoProcessorOutputView(nv12Tex_.get(), venum_.get(), &ovd, outView_.put())))
        return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.RGB_Range = 0;  // full range RGB
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix = 1;  // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(vproc_.get(), 0, &inCs);
    vctx_->VideoProcessorSetOutputColorSpace(vproc_.get(), &outCs);
    vctx_->VideoProcessorSetStreamFrameFormat(vproc_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.get(), 0, FALSE);
    RECT src{0, 0, monWidth, monHeight}, dst{0, 0, outWidth, outHeight};
    vctx_->VideoProcessorSetStreamSourceRect(vproc_.get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vproc_.get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vproc_.get(), TRUE, &dst);
    return true;
}

int DesktopCapture::capture(int timeoutMs, CursorUpdate& cursor) {
    if (!dupl_) return -1;
    if (frameAcquired_) {
        dupl_->ReleaseFrame();
        frameAcquired_ = false;
    }
    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &fi, res.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) return -1;
    frameAcquired_ = true;
    const int64_t grabUs = nowUs();

    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        cursor.posChanged = true;
        cursor.visible = fi.PointerPosition.Visible != 0;
        cursor.x = fi.PointerPosition.Position.x;
        cursor.y = fi.PointerPosition.Position.y;
    }
    if (fi.PointerShapeBufferSize > 0) {
        shapeBuf_.resize(fi.PointerShapeBufferSize);
        UINT req = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
        if (SUCCEEDED(dupl_->GetFramePointerShape((UINT)shapeBuf_.size(), shapeBuf_.data(), &req, &si)))
            convertPointerShape(si, shapeBuf_, cursor);
    }

    int result = 0;
    if (fi.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.as(tex))) {
            ctx_->CopyResource(bgra_.get(), tex.get());
            result = 1;
        }
    }
    dupl_->ReleaseFrame();
    frameAcquired_ = false;

    if (result == 1) {
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inView_.get();
        if (FAILED(vctx_->VideoProcessorBlt(vproc_.get(), outView_.get(), 0, 1, &stream))) return -1;
        ctx_->CopyResource(staging_.get(), nv12Tex_.get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &m))) return -1;
        const uint8_t* src = (const uint8_t*)m.pData;
        uint8_t* dst = nv12_.data();
        for (int y = 0; y < outHeight; ++y) memcpy(dst + (size_t)y * outWidth, src + (size_t)y * m.RowPitch, outWidth);
        const uint8_t* srcUV = src + (size_t)m.RowPitch * outHeight;
        uint8_t* dstUV = dst + (size_t)outWidth * outHeight;
        for (int y = 0; y < outHeight / 2; ++y)
            memcpy(dstUV + (size_t)y * outWidth, srcUV + (size_t)y * m.RowPitch, outWidth);
        ctx_->Unmap(staging_.get(), 0);
        lastGrabUs = grabUs;
        lastConvertUs = nowUs() - grabUs;
    }
    return result;
}

void DesktopCapture::convertPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const std::vector<uint8_t>& buf,
                                         CursorUpdate& out) {
    int w = (int)si.Width;
    int h = (int)si.Height;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;
    out.shapeChanged = true;
    out.width = w;
    out.height = h;
    out.hotX = si.HotSpot.x;
    out.hotY = si.HotSpot.y;
    out.rgba.assign((size_t)w * h * 4, 0);
    std::vector<uint8_t> inverted((size_t)w * h, 0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* d = &out.rgba[((size_t)y * w + x) * 4];
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                int bit = 0x80 >> (x & 7);
                bool andBit = (buf[(size_t)y * si.Pitch + x / 8] & bit) != 0;
                bool xorBit = (buf[(size_t)(y + h) * si.Pitch + x / 8] & bit) != 0;
                if (!andBit) {
                    uint8_t c = xorBit ? 255 : 0;
                    d[0] = d[1] = d[2] = c;
                    d[3] = 255;
                } else if (xorBit) {  // screen inversion: draw white, outlined below
                    d[0] = d[1] = d[2] = 255;
                    d[3] = 255;
                    inverted[(size_t)y * w + x] = 1;
                }
            } else {
                const uint8_t* s = &buf[(size_t)y * si.Pitch + x * 4];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = s[3];
                } else {  // masked color: alpha 0 = opaque color, 0xFF = XOR with screen
                    bool rgbZero = s[0] == 0 && s[1] == 0 && s[2] == 0;
                    if (s[3] == 0) {
                        d[0] = s[2];
                        d[1] = s[1];
                        d[2] = s[0];
                        d[3] = 255;
                    } else if (!rgbZero) {
                        d[0] = d[1] = d[2] = 255;
                        d[3] = 255;
                        inverted[(size_t)y * w + x] = 1;
                    }
                }
            }
        }
    }
    // Outline inverted pixels with black so they stay visible on light backgrounds too.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* d = &out.rgba[((size_t)y * w + x) * 4];
            if (d[3] != 0) continue;
            bool nearInv = (x > 0 && inverted[(size_t)y * w + x - 1]) || (x + 1 < w && inverted[(size_t)y * w + x + 1]) ||
                        (y > 0 && inverted[(size_t)(y - 1) * w + x]) || (y + 1 < h && inverted[(size_t)(y + 1) * w + x]);
            if (nearInv) {
                d[0] = d[1] = d[2] = 0;
                d[3] = 255;
            }
        }
    }
}

std::unique_ptr<ScreenCapture> createScreenCapture() { return std::make_unique<DesktopCapture>(); }
