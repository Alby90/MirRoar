#include "stats.h"

#include <algorithm>
#include "../platform/platform.h"

static const size_t kHistory = 240;  // 2 minutes at 2 Hz

StatsCollector::StatsCollector() { t0_ = lastTick_ = nowUs(); }

double StatsCollector::seconds() const { return (nowUs() - t0_) / 1e6; }

void StatsCollector::reset() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& p : s_) p = PerStream();
    hasLatency_ = false;
    rttMs_ = 0;
}

void StatsCollector::streamStarted(int stream, int w, int h, const std::string& encoder, bool hw) {
    std::lock_guard<std::mutex> lk(m_);
    PerStream& p = s_[stream];
    p.active = true;
    p.live.monitor = stream;
    p.live.width = w;
    p.live.height = h;
    p.live.encoder = encoder;
    p.live.hardware = hw;
}

void StatsCollector::streamStopped(int stream) {
    std::lock_guard<std::mutex> lk(m_);
    s_[stream] = PerStream();
}

void StatsCollector::frameSent(int stream, size_t bytes) {
    std::lock_guard<std::mutex> lk(m_);
    s_[stream].frames++;
    s_[stream].bytes += bytes;
}

void StatsCollector::frameDropped(int stream) {
    std::lock_guard<std::mutex> lk(m_);
    s_[stream].live.serverDrops++;
}

void StatsCollector::clientStats(const mirroar::CStats& c) {
    if (c.stream >= (uint32_t)mirroar::kMaxStreams) return;
    std::lock_guard<std::mutex> lk(m_);
    PerStream& p = s_[c.stream];
    p.live.clientDrops += c.dropped;
    p.live.totalMs = c.totalMs;
    rttMs_ = c.rttMs;
    if (c.frames == 0) return;
    p.pending = c;
    p.hasPending = true;
}

void StatsCollector::tick() {
    std::lock_guard<std::mutex> lk(m_);
    int64_t now = nowUs();
    double dt = std::max(0.05, (now - lastTick_) / 1e6);
    lastTick_ = now;
    double t = (now - t0_) / 1e6;

    RatePoint rp;
    rp.t = t;
    int n = 0;
    LatencyPoint lp;
    lp.t = t;
    double w = 0;
    for (auto& p : s_) {
        if (p.active) {
            p.live.fps = (float)(p.frames / dt);
            p.live.mbps = (float)(p.bytes * 8 / dt / 1e6);
            rp.fps += p.live.fps;
            rp.mbps += p.live.mbps;
            ++n;
        }
        p.frames = 0;
        p.bytes = 0;
        if (p.hasPending) {
            const mirroar::CStats& c = p.pending;
            double k = c.frames;
            lp.convert += (float)(c.convertMs * k);
            lp.encode += (float)(c.encodeMs * k);
            lp.queue += (float)(c.queueMs * k);
            lp.network += (float)(c.networkMs * k);
            lp.decode += (float)(c.decodeMs * k);
            lp.render += (float)(c.renderMs * k);
            lp.total += (float)(c.totalMs * k);
            lp.maxTotal = std::max(lp.maxTotal, c.maxTotalMs);
            w += k;
            p.hasPending = false;
        }
    }
    if (n) rp.fps /= n;
    rates_.push_back(rp);
    while (rates_.size() > kHistory) rates_.pop_front();
    if (w > 0) {
        float iw = (float)(1.0 / w);
        lp.convert *= iw;
        lp.encode *= iw;
        lp.queue *= iw;
        lp.network *= iw;
        lp.decode *= iw;
        lp.render *= iw;
        lp.total *= iw;
        latency_.push_back(lp);
        while (latency_.size() > kHistory) latency_.pop_front();
        current_ = lp;
        hasLatency_ = true;
    }
}

StatsCollector::Snapshot StatsCollector::snapshot() {
    std::lock_guard<std::mutex> lk(m_);
    Snapshot s;
    for (auto& p : s_)
        if (p.active) s.streams.push_back(p.live);
    s.latency.assign(latency_.begin(), latency_.end());
    s.rates.assign(rates_.begin(), rates_.end());
    s.current = current_;
    s.hasLatency = hasLatency_;
    s.rttMs = rttMs_;
    s.now = seconds();
    return s;
}
