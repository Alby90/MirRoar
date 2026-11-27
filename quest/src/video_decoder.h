// MirRoar Quest client - hardware H.264 decoding (AMediaCodec) into an AImageReader (GPU-sampleable buffers).
#pragma once
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// A decoded frame handed to the render thread. `buffer` carries its own reference
// (AHardwareBuffer_release it once the EGLImage exists); `image` must go back via release(image, generation).
struct DecodedFrame {
    AImage* image = nullptr;
    uint64_t generation = 0;
    AHardwareBuffer* buffer = nullptr;
    int64_t ptsUs = 0;
    float uMax = 1, vMax = 1;  // crop of the visible area inside the buffer
};

class VideoDecoder {
public:
    ~VideoDecoder() { reset(); }
    // Network thread. Returns false when the decoder needs a keyframe to (re)start.
    bool submit(const uint8_t* data, size_t len, uint64_t tsUs, bool key, int width, int height);
    void reset();

    // Render thread: newest decoded frame, false if nothing new since the last call.
    // Everything is read under the decoder lock, so a concurrent reset() can't free it underneath.
    bool acquireLatest(DecodedFrame& out);
    // Gives an image back. Images from an older decoder generation (before a reset) are ignored:
    // they were already freed, and a new image may even live at the same address.
    void release(AImage* img, uint64_t generation);

    std::atomic<uint32_t> decodedFrames{0};
    // When the frame with this pts came out of the decoder (monoUs), or 0 if unknown.
    int64_t decodedAt(int64_t ptsUs);

private:
    bool create(const uint8_t* data, size_t len, int width, int height);
    void outputLoop();

    std::mutex m_;
    AMediaCodec* codec_ = nullptr;
    AImageReader* reader_ = nullptr;
    std::thread outThread_;
    std::atomic<bool> running_{false};
    int width_ = 0, height_ = 0;
    bool waitKey_ = true;
    std::mutex timesM_;
    struct DecTime {
        int64_t pts, at;
    };
    DecTime times_[64] = {};
    int timesPos_ = 0;
    std::vector<AImage*> held_;
    uint64_t generation_ = 1;  // bumped by every reset(), guarded by m_
};
