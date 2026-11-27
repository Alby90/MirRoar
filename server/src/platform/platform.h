// MirRoar server - platform abstraction. The core (sessions, network, GUI) only talks to these
// interfaces; each OS provides an implementation (platform/win today, platform/linux later).
#pragma once
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ------------------------------------------------------------------ system
void platformInit();        // once, at startup
void platformShutdown();
void platformThreadInit();  // every worker thread that uses capture/encoder/audio
void platformThreadExit();
int64_t nowUs();            // monotonic clock, microseconds

std::string hostName();
std::string configDir();   // per-user settings directory (created if missing)
std::string exePath();

struct NetIface {
    std::string name;
    uint32_t addr = 0;  // IPv4, network byte order
    uint32_t mask = 0;
};
std::vector<NetIface> listIPv4Interfaces();

// Runs a command line without showing a window and returns its stdout+stderr.
std::string runProcess(const std::string& cmdline);
std::string findAdb(const std::string& configured);

// False if another server instance is already running for this user.
bool acquireSingleInstance();
void showMessage(const std::string& title, const std::string& text);

bool autostartEnabled();
void setAutostart(bool on);

// ------------------------------------------------------------------ screen capture
struct MonitorDesc {
    int x = 0, y = 0, width = 0, height = 0;  // position on the virtual desktop
    bool primary = false;
    std::string name, adapter;
};
std::vector<MonitorDesc> listMonitors();

struct CursorUpdate {
    bool posChanged = false;
    bool visible = false;
    int x = 0, y = 0;  // top-left of the cursor shape, monitor pixels
    bool shapeChanged = false;
    int width = 0, height = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> rgba;
};

class ScreenCapture {
public:
    virtual ~ScreenCapture() = default;
    virtual bool init(int monitorIndex, int maxWidth, int maxHeight) = 0;
    virtual void shutdown() = 0;
    // 1 = new image converted to NV12 (see nv12()), 0 = nothing new, -1 = lost (call init again)
    virtual int capture(int timeoutMs, CursorUpdate& cursor) = 0;
    virtual const uint8_t* nv12() const = 0;

    int outWidth = 0, outHeight = 0;  // encoded size
    int monWidth = 0, monHeight = 0;  // monitor size
    std::string adapterName;
    int64_t lastGrabUs = 0;     // when the last image was grabbed
    int64_t lastConvertUs = 0;  // how long it took to get it in NV12
};
std::unique_ptr<ScreenCapture> createScreenCapture();

// ------------------------------------------------------------------ video encoder
struct EncodedPacket {
    std::vector<uint8_t> data;  // Annex-B H.264
    bool key = false;
    int64_t timestampUs = 0;    // the timestamp given to encode()
    int64_t outputUs = 0;       // nowUs() when the bitstream came out
};

class VideoEncoder {
public:
    using Callback = std::function<void(EncodedPacket&&)>;
    virtual ~VideoEncoder() = default;
    virtual bool init(int width, int height, int fps, int bitrateKbps, const std::string& hint, Callback cb) = 0;
    virtual void shutdown() = 0;
    // Submits one packed NV12 frame. Returns false if the encoder could not take it now.
    virtual bool encode(const uint8_t* nv12, int64_t timestampUs, bool forceKey) = 0;

    std::string name;
    bool hardware = false;
};
std::unique_ptr<VideoEncoder> createVideoEncoder();
std::vector<std::string> listVideoEncoders();

// ------------------------------------------------------------------ input injection
class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual void setMonitors(const std::vector<MonitorDesc>& monitors) = 0;
    virtual void mouseMove(int monitor, float nx, float ny) = 0;
    virtual void mouseButton(int button, bool down) = 0;
    virtual void mouseWheel(int dx, int dy) = 0;
    virtual void key(unsigned vk, bool down, bool extended) = 0;  // Windows virtual-key codes
    virtual void text(const std::string& utf8) = 0;
    virtual void releaseAll() = 0;
};
std::unique_ptr<InputInjector> createInputInjector();

// ------------------------------------------------------------------ system audio
class AudioCapture {
public:
    using ConfigCallback = std::function<void(uint32_t sampleRate)>;
    using DataCallback = std::function<void(const int16_t* stereo, uint32_t frames)>;
    virtual ~AudioCapture() = default;
    virtual void start(ConfigCallback onConfig, DataCallback onData) = 0;
    virtual void stop() = 0;
};
std::unique_ptr<AudioCapture> createAudioCapture();
