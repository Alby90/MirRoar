// MirRoar Quest client - server discovery (LAN broadcast / USB via adb reverse) and TCP session.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../common/protocol.h"

class NetClient {
public:
    struct Handler {
        virtual ~Handler() = default;
        virtual void onConnected(const std::string& serverName, const std::string& via) = 0;
        virtual void onDisconnected() = 0;
        virtual void onMonitorList(const std::vector<mirroar::SMonitorInfo>& monitors) = 0;
        virtual void onVideoConfig(const mirroar::SVideoConfig& cfg) = 0;
        // recvUs: client clock (monoUs) when the whole frame was received
        virtual void onVideoFrame(const mirroar::SVideoFrame& f, const uint8_t* data, size_t len, int64_t recvUs) = 0;
        virtual void onVideoStop(int stream) = 0;
        virtual void onCursorPos(const mirroar::SCursorPos& p) = 0;
        virtual void onCursorShape(const mirroar::SCursorShape& s, const uint8_t* rgba) = 0;
        virtual void onAudioConfig(const mirroar::SAudioConfig& cfg) = 0;
        virtual void onAudio(const int16_t* samples, size_t frames) = 0;
        virtual void onOptions(const char* text, size_t len) = 0;  // headset options edited on the PC
    };

    explicit NetClient(Handler* h) : handler_(h) {}
    ~NetClient() { stop(); }
    void start(const mirroar::CHello& hello);
    void setMonitorMask(uint32_t mask) { hello_.monitorMask = mask; }  // used for the next connection
    void stop();

    bool send(uint16_t type, const void* data, size_t len);
    bool connected() const { return connected_; }
    std::string status();
    float rttMs() const { return rttUs_ / 1000.0f; }
    // Converts a server timestamp (server clock, us) to the local monoUs() clock.
    int64_t serverToLocalUs(int64_t serverUs) const { return serverUs - clockOffsetUs_; }
    bool clockSynced() const { return clockSynced_; }

private:
    void run();
    bool session(int fd, const std::string& via);
    void setStatus(const std::string& s);

    Handler* handler_;
    mirroar::CHello hello_{};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> fd_{-1};
    std::atomic<int> rttUs_{0};
    std::atomic<int64_t> clockOffsetUs_{0};  // server clock - local clock
    std::atomic<bool> clockSynced_{false};
    struct ClockSample {
        int64_t rtt, offset;
    };
    std::vector<ClockSample> clockSamples_;
    std::mutex sendM_;
    std::mutex statusM_;
    std::string status_;
};
