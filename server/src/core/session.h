// MirRoar server - one connected headset: video (one stream per monitor) and audio out, input in.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../../common/options.h"
#include "../../../common/protocol.h"
#include "../platform/platform.h"
#include "config.h"
#include "net.h"
#include "stats.h"

class Session {
public:
    Session(socket_t s, const ServerConfig& cfg, const std::string& peer, StatsCollector* stats);
    ~Session();
    void start();
    void stop();
    bool alive() const { return alive_; }
    bool handshakeDone() const { return gotHello_; }
    const std::string& peer() const { return peer_; }
    std::string clientName();

    void updateConfig(const ServerConfig& cfg);  // applied live (streams restart if needed)
    void selectMonitors(uint32_t mask) { monitorMask_ = mask; }
    uint32_t activeMask() const { return activeMask_; }

    // Headset options: the headset reports them (C_OPTIONS); edits from the GUI are pushed back (S_OPTIONS).
    bool headsetOptions(float* out);  // false until the headset has reported them
    void setHeadsetOptions(const float* v);

private:
    struct OutMsg {
        std::vector<uint8_t> bytes;
        int stream = -1;  // >= 0 for video frames
        int64_t enqueuedUs = 0;
    };
    struct StreamWorker {
        int monitor = 0;
        std::thread thread;
        std::atomic<bool> run{true};
    };

    void enqueue(uint16_t type, const void* head, size_t headLen, const void* data, size_t dataLen, int videoStream = -1,
                 bool key = false);
    void senderLoop();
    void receiverLoop();
    void controlLoop();
    void streamLoop(StreamWorker* w);
    bool recvAll(void* buf, size_t len);
    void sendMonitorList();
    ServerConfig config();

    socket_t sock_;
    std::mutex cfgM_;
    ServerConfig cfg_;
    std::atomic<uint32_t> cfgVersion_{0};
    std::string peer_;
    StatsCollector* stats_;
    std::atomic<bool> alive_{true};
    std::thread sender_, receiver_, control_;

    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<OutMsg> queue_;
    size_t queuedVideoBytes_ = 0;
    bool waitingForKey_[mirroar::kMaxStreams] = {};
    std::atomic<bool> idrRequested_[mirroar::kMaxStreams];

    std::mutex helloM_;
    std::condition_variable helloCv_;
    std::atomic<bool> gotHello_{false};
    mirroar::CHello hello_{};
    std::atomic<uint32_t> monitorMask_{0};
    std::atomic<uint32_t> activeMask_{0};

    std::mutex optM_;
    bool hasOptions_ = false;
    float options_[mirroar::OPT_COUNT] = {};

    std::unique_ptr<InputInjector> input_;
    std::mutex inputM_;
    size_t monitorCount_ = 0;
};
