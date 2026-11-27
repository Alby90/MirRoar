// MirRoar server - live statistics (rates measured here, latency measured by the headset) with history for the GUI.
#pragma once
#include <stdint.h>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "../../../common/protocol.h"

struct LatencyPoint {
    double t = 0;  // seconds since the collector started
    float convert = 0, encode = 0, queue = 0, network = 0, decode = 0, render = 0;
    float total = 0, maxTotal = 0;
};

struct RatePoint {
    double t = 0;
    float fps = 0;   // average per stream
    float mbps = 0;  // total
};

struct StreamLive {
    int monitor = 0;
    int width = 0, height = 0;
    std::string encoder;
    bool hardware = false;
    float fps = 0, mbps = 0;
    uint32_t serverDrops = 0, clientDrops = 0;
    float totalMs = 0;
};

class StatsCollector {
public:
    StatsCollector();
    void reset();
    void streamStarted(int stream, int w, int h, const std::string& encoder, bool hw);
    void streamStopped(int stream);
    void frameSent(int stream, size_t bytes);
    void frameDropped(int stream);
    void clientStats(const mirroar::CStats& s);
    void tick();  // ~every 500 ms

    struct Snapshot {
        std::vector<StreamLive> streams;
        std::vector<LatencyPoint> latency;
        std::vector<RatePoint> rates;
        LatencyPoint current;
        bool hasLatency = false;
        float rttMs = 0;
        double now = 0;
    };
    Snapshot snapshot();

private:
    struct PerStream {
        bool active = false;
        StreamLive live;
        uint32_t frames = 0;
        uint64_t bytes = 0;
        bool hasPending = false;
        mirroar::CStats pending{};
    };
    double seconds() const;
    std::mutex m_;
    PerStream s_[mirroar::kMaxStreams];
    std::deque<LatencyPoint> latency_;
    std::deque<RatePoint> rates_;
    LatencyPoint current_;
    bool hasLatency_ = false;
    float rttMs_ = 0;
    int64_t t0_ = 0, lastTick_ = 0;
};
