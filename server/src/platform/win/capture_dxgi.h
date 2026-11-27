// MirRoar server - desktop capture (DXGI Desktop Duplication) with GPU BGRA->NV12 conversion.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>
#include "winutil.h"

class DesktopCapture : public ScreenCapture {
public:
    ~DesktopCapture() override { shutdown(); }
    bool init(int monitorIndex, int maxWidth, int maxHeight) override;
    void shutdown() override;

    // 1 = a new image was converted and is available in nv12()
    // 0 = no new image (timeout or pointer-only update)
    // -1 = duplication lost (mode change, secure desktop...) -> call init() again
    int capture(int timeoutMs, CursorUpdate& cursor) override;

    const uint8_t* nv12() const override { return nv12_.data(); }
    size_t nv12Size() const { return nv12_.size(); }

    RECT monRect{};  // monitor rect in virtual-desktop coordinates

private:
    bool createConverter();
    void convertPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const std::vector<uint8_t>& buf,
                             CursorUpdate& out);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D> bgra_;
    ComPtr<ID3D11Texture2D> nv12Tex_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    ComPtr<ID3D11VideoProcessor> vproc_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    ComPtr<ID3D11VideoProcessorOutputView> outView_;
    std::vector<uint8_t> nv12_;
    std::vector<uint8_t> shapeBuf_;
    bool frameAcquired_ = false;
};
