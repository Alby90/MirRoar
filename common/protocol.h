// MirRoar - shared wire protocol between the PC server and the Quest client.
// All integers are little-endian. Every TCP message is: MsgHeader + payload.
// A "stream" is one captured monitor; its id is the monitor index reported in S_MONITOR_LIST.
#pragma once
#include <stdint.h>
#include <string.h>

namespace mirroar {

constexpr uint32_t kProtocolVersion = 4;
constexpr uint16_t kTcpPort = 47800;        // server listens here (also used by adb reverse)
constexpr uint16_t kDiscoveryPort = 47801;  // server broadcasts beacons here (UDP)
constexpr char kDiscoveryMagic[8] = {'M', 'I', 'R', 'R', 'O', 'A', 'R', '1'};
constexpr int kMaxStreams = 8;

enum MsgType : uint16_t {
    // server -> client
    S_HELLO = 1,          // SHello
    S_VIDEO_CONFIG = 2,   // SVideoConfig (before the first frame of a stream and on every resolution change)
    S_VIDEO_FRAME = 3,    // SVideoFrame + Annex-B bitstream
    S_CURSOR_POS = 4,     // SCursorPos
    S_CURSOR_SHAPE = 5,   // SCursorShape + width*height*4 bytes RGBA (straight alpha)
    S_AUDIO_CONFIG = 6,   // SAudioConfig
    S_AUDIO = 7,          // interleaved int16 PCM
    S_PONG = 8,           // SPong
    S_MONITOR_LIST = 9,   // SMonitorList + count * SMonitorInfo
    S_VIDEO_STOP = 10,    // SStreamId: the stream is no longer sent
    S_OPTIONS = 11,       // headset options edited on the PC: text "key=value" lines (see options.h)

    // client -> server
    C_HELLO = 100,            // CHello
    C_MOUSE_MOVE = 101,       // CMouseMove
    C_MOUSE_BUTTON = 102,     // CMouseButton
    C_MOUSE_WHEEL = 103,      // CMouseWheel
    C_KEY = 104,              // CKey (Windows virtual-key code)
    C_TEXT = 105,             // UTF-8 text (typed as unicode, or as key presses while modifiers are held)
    C_REQUEST_IDR = 106,      // SStreamId
    C_PING = 107,             // SPing
    C_SELECT_MONITORS = 108,  // CSelectMonitors
    C_STATS = 109,            // CStats (latency measured by the headset, ~2 per second per stream)
    C_OPTIONS = 110,          // current headset options, after C_HELLO and on every change (see options.h)
};

enum VideoCodec : uint32_t { CODEC_H264 = 0, CODEC_HEVC = 1 };
enum VideoFrameFlags : uint32_t { FRAME_KEY = 1 };
enum MouseButtonId : uint8_t { MOUSE_LEFT = 0, MOUSE_RIGHT = 1, MOUSE_MIDDLE = 2 };

#pragma pack(push, 1)
struct MsgHeader {
    uint32_t size;  // payload size in bytes (excluding header)
    uint16_t type;
    uint16_t reserved;
};

struct DiscoveryBeacon {
    char magic[8];
    uint32_t version;
    uint16_t tcpPort;
    uint16_t reserved;
    char name[64];  // PC name, NUL terminated
};

struct SHello {
    uint32_t version;
    char name[64];
};

struct SMonitorList {
    uint32_t count;
};

struct SMonitorInfo {
    int32_t x, y;  // position on the Windows virtual desktop
    uint32_t width, height;
    uint8_t primary;
    uint8_t reserved[3];
};

struct SStreamId {
    uint32_t stream;
};

struct SVideoConfig {
    uint32_t stream;
    uint32_t codec;
    uint32_t width, height;        // encoded stream size
    uint32_t fps;
    uint32_t srcWidth, srcHeight;  // captured monitor size (cursor shapes are in these pixels)
};

struct SVideoFrame {
    uint32_t stream;
    uint32_t flags;
    uint64_t captureUs;  // server clock: when the desktop image was grabbed (also the decoder pts)
    uint32_t convertUs;  // grab -> NV12 ready
    uint32_t encodeUs;   // NV12 submitted -> bitstream out of the encoder
    uint32_t queueUs;    // waiting in the send queue (filled in right before sending)
};

struct SCursorPos {
    uint32_t stream;
    float x, y;  // normalized (0..1) position of the hotspot on that monitor
    uint8_t visible;
    uint8_t reserved[3];
};

struct SCursorShape {
    uint16_t width, height;
    uint16_t hotX, hotY;
};

struct SAudioConfig {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;  // always 16
};

struct SPing {
    uint64_t clientTimeUs;
};

struct SPong {
    uint64_t clientTimeUs;  // echoed from SPing
    uint64_t serverTimeUs;  // server clock when answering (for clock offset estimation)
};

// Averages over the last window, in milliseconds. All stages of a frame's journey:
// convert -> encode -> queue -> network -> decode -> render (until shown in the headset).
struct CStats {
    uint32_t stream;
    uint32_t frames;    // frames shown in this window
    uint32_t windowMs;
    uint32_t dropped;   // frames the headset could not decode
    float convertMs, encodeMs, queueMs, networkMs, decodeMs, renderMs;
    float totalMs, maxTotalMs;
    float rttMs;
};

struct CHello {
    uint32_t version;
    uint32_t maxWidth, maxHeight;  // max decodable size (0 = no limit)
    uint32_t bitrateKbps;          // 0 = server default
    uint32_t fps;                  // 0 = server default
    char deviceName[64];
    uint32_t monitorMask;          // monitors to stream (bit = monitor index), 0 = server default
};

struct CSelectMonitors {
    uint32_t mask;
};

struct CMouseMove {
    uint32_t stream;
    float x, y;  // normalized 0..1 on that monitor
};

struct CMouseButton {
    uint8_t button;
    uint8_t down;
};

struct CMouseWheel {
    int16_t dx, dy;  // in WHEEL_DELTA units (120 = one notch)
};

struct CKey {
    uint16_t vk;  // Windows virtual-key code
    uint8_t down;
    uint8_t extended;
};
#pragma pack(pop)

}  // namespace mirroar
