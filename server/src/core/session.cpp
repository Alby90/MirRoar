#include "session.h"

#include <string.h>

#include <algorithm>
#include <map>

#include "log.h"

using namespace mirroar;

Session::Session(socket_t s, const ServerConfig& cfg, const std::string& peer, StatsCollector* stats)
    : sock_(s), cfg_(cfg), peer_(peer), stats_(stats) {
    int nd = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
    int sndbuf = 512 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    for (auto& f : idrRequested_) f = true;
    input_ = createInputInjector();
}

Session::~Session() { stop(); }

void Session::start() {
    sender_ = std::thread(&Session::senderLoop, this);
    receiver_ = std::thread(&Session::receiverLoop, this);
    control_ = std::thread(&Session::controlLoop, this);
}

void Session::stop() {
    alive_ = false;
    if (sock_ != kInvalidSocket) shutdownSocket(sock_);
    qcv_.notify_all();
    helloCv_.notify_all();
    if (control_.joinable()) control_.join();
    if (receiver_.joinable()) receiver_.join();
    if (sender_.joinable()) sender_.join();
    if (sock_ != kInvalidSocket) closeSocket(sock_);
    sock_ = kInvalidSocket;
    std::lock_guard<std::mutex> lk(inputM_);
    input_->releaseAll();
}

std::string Session::clientName() {
    std::lock_guard<std::mutex> lk(helloM_);
    return gotHello_ ? std::string(hello_.deviceName) : std::string();
}

ServerConfig Session::config() {
    std::lock_guard<std::mutex> lk(cfgM_);
    return cfg_;
}

void Session::updateConfig(const ServerConfig& cfg) {
    bool restart;
    {
        std::lock_guard<std::mutex> lk(cfgM_);
        restart = cfg.videoDiffers(cfg_);
        cfg_ = cfg;
    }
    if (restart) ++cfgVersion_;
}

void Session::enqueue(uint16_t type, const void* head, size_t headLen, const void* data, size_t dataLen,
                      int videoStream, bool key) {
    OutMsg m;
    m.stream = videoStream;
    m.enqueuedUs = nowUs();
    MsgHeader h{(uint32_t)(headLen + dataLen), type, 0};
    m.bytes.resize(sizeof(h) + headLen + dataLen);
    memcpy(m.bytes.data(), &h, sizeof(h));
    if (headLen) memcpy(m.bytes.data() + sizeof(h), head, headLen);
    if (dataLen) memcpy(m.bytes.data() + sizeof(h) + headLen, data, dataLen);

    std::lock_guard<std::mutex> lk(qm_);
    if (videoStream >= 0) {
        if (waitingForKey_[videoStream] && !key) {
            stats_->frameDropped(videoStream);
            return;
        }
        if (key) waitingForKey_[videoStream] = false;
        // Network can't keep up: drop all pending video and restart every stream from a keyframe.
        int bitrate;
        {
            std::lock_guard<std::mutex> cl(cfgM_);
            bitrate = cfg_.bitrateKbps;
        }
        size_t maxBacklog = std::max<size_t>(1u << 20, (size_t)bitrate * 1000 / 8 / 4);
        if (queuedVideoBytes_ + m.bytes.size() > maxBacklog && !key) {
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (it->stream >= 0) {
                    stats_->frameDropped(it->stream);
                    queuedVideoBytes_ -= it->bytes.size();
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }
            for (int i = 0; i < kMaxStreams; ++i) {
                waitingForKey_[i] = true;
                idrRequested_[i] = true;
            }
            stats_->frameDropped(videoStream);
            return;
        }
        queuedVideoBytes_ += m.bytes.size();
    }
    queue_.push_back(std::move(m));
    qcv_.notify_one();
}

void Session::senderLoop() {
    while (alive_) {
        OutMsg m;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait(lk, [&] { return !queue_.empty() || !alive_; });
            if (!alive_) break;
            m = std::move(queue_.front());
            queue_.pop_front();
            if (m.stream >= 0) queuedVideoBytes_ -= m.bytes.size();
        }
        if (m.stream >= 0) {
            uint32_t q = (uint32_t)(nowUs() - m.enqueuedUs);
            memcpy(m.bytes.data() + sizeof(MsgHeader) + offsetof(SVideoFrame, queueUs), &q, sizeof(q));
        }
        size_t off = 0;
        while (off < m.bytes.size()) {
            int n = send(sock_, (const char*)m.bytes.data() + off, (int)(m.bytes.size() - off), 0);
            if (n <= 0) {
                alive_ = false;
                break;
            }
            off += n;
        }
        if (m.stream >= 0) stats_->frameSent(m.stream, m.bytes.size());
    }
    alive_ = false;
    helloCv_.notify_all();
}

bool Session::recvAll(void* buf, size_t len) {
    char* p = (char*)buf;
    while (len > 0) {
        int n = recv(sock_, p, (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

void Session::receiverLoop() {
    std::vector<uint8_t> payload;
    while (alive_) {
        MsgHeader h;
        if (!recvAll(&h, sizeof(h))) break;
        if (h.size > (1u << 20)) break;
        payload.resize(h.size);
        if (h.size && !recvAll(payload.data(), h.size)) break;
        const uint8_t* p = payload.data();
        if (h.type == C_HELLO) {
            if (h.size >= sizeof(CHello)) {
                std::lock_guard<std::mutex> hl(helloM_);
                memcpy(&hello_, p, sizeof(CHello));
                hello_.deviceName[63] = 0;
                gotHello_ = true;
                helloCv_.notify_all();
            }
            continue;
        }
        if (h.type == C_PING) {
            if (h.size >= sizeof(SPing)) {
                SPong pong{((const SPing*)p)->clientTimeUs, (uint64_t)nowUs()};
                enqueue(S_PONG, &pong, sizeof(pong), nullptr, 0);
            }
            continue;
        }
        if (h.type == C_OPTIONS) {
            std::lock_guard<std::mutex> ol(optM_);
            if (!hasOptions_) defaultOptions(options_);
            decodeOptions((const char*)p, h.size, options_);
            hasOptions_ = true;
            continue;
        }
        if (h.type == C_STATS) {
            if (h.size >= sizeof(CStats)) {
                CStats cs;
                memcpy(&cs, p, sizeof(cs));
                stats_->clientStats(cs);
            }
            continue;
        }
        std::lock_guard<std::mutex> lk(inputM_);
        switch (h.type) {
            case C_SELECT_MONITORS:
                if (h.size >= sizeof(CSelectMonitors)) monitorMask_ = ((const CSelectMonitors*)p)->mask;
                break;
            case C_MOUSE_MOVE:
                if (h.size >= sizeof(CMouseMove)) {
                    auto* m = (const CMouseMove*)p;
                    input_->mouseMove((int)m->stream, m->x, m->y);
                }
                break;
            case C_MOUSE_BUTTON:
                if (h.size >= sizeof(CMouseButton)) {
                    auto* m = (const CMouseButton*)p;
                    input_->mouseButton(m->button, m->down != 0);
                }
                break;
            case C_MOUSE_WHEEL:
                if (h.size >= sizeof(CMouseWheel)) {
                    auto* m = (const CMouseWheel*)p;
                    input_->mouseWheel(m->dx, m->dy);
                }
                break;
            case C_KEY:
                if (h.size >= sizeof(CKey)) {
                    auto* m = (const CKey*)p;
                    input_->key(m->vk, m->down != 0, m->extended != 0);
                }
                break;
            case C_TEXT:
                input_->text(std::string((const char*)p, h.size));
                break;
            case C_REQUEST_IDR:
                if (h.size >= sizeof(SStreamId)) {
                    uint32_t s = ((const SStreamId*)p)->stream;
                    if (s < (uint32_t)kMaxStreams) idrRequested_[s] = true;
                } else {
                    for (auto& f : idrRequested_) f = true;
                }
                break;
            default:
                break;
        }
    }
    alive_ = false;
    qcv_.notify_all();
    helloCv_.notify_all();
}

bool Session::headsetOptions(float* out) {
    std::lock_guard<std::mutex> lk(optM_);
    if (hasOptions_) memcpy(out, options_, sizeof(options_));
    return hasOptions_;
}

void Session::setHeadsetOptions(const float* v) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(optM_);
        for (int i = 0; i < OPT_COUNT; ++i) options_[i] = clampOption(i, v[i]);
        hasOptions_ = true;
        text = encodeOptions(options_);
    }
    enqueue(S_OPTIONS, text.data(), text.size(), nullptr, 0);
}

void Session::sendMonitorList() {
    auto mons = listMonitors();
    if (mons.size() > (size_t)kMaxStreams) mons.resize(kMaxStreams);
    std::vector<SMonitorInfo> infos;
    for (auto& m : mons) {
        SMonitorInfo mi{};
        mi.x = m.x;
        mi.y = m.y;
        mi.width = m.width;
        mi.height = m.height;
        mi.primary = m.primary ? 1 : 0;
        infos.push_back(mi);
    }
    {
        std::lock_guard<std::mutex> lk(inputM_);
        input_->setMonitors(mons);
        monitorCount_ = mons.size();
    }
    SMonitorList ml{(uint32_t)infos.size()};
    enqueue(S_MONITOR_LIST, &ml, sizeof(ml), infos.data(), infos.size() * sizeof(SMonitorInfo));
}

// Starts/stops one StreamWorker per selected monitor and runs audio.
void Session::controlLoop() {
    platformThreadInit();
    ServerConfig cfg = config();
    SHello sh{};
    sh.version = kProtocolVersion;
    strncpy(sh.name, cfg.name.c_str(), sizeof(sh.name) - 1);
    enqueue(S_HELLO, &sh, sizeof(sh), nullptr, 0);

    {
        std::unique_lock<std::mutex> lk(helloM_);
        if (!helloCv_.wait_for(lk, std::chrono::seconds(5), [&] { return gotHello_ || !alive_; }) || !gotHello_) {
            logf("%s: no handshake from the headset, closing", peer_.c_str());
            alive_ = false;
            qcv_.notify_all();
            platformThreadExit();
            return;
        }
    }
    if (hello_.version != kProtocolVersion) {
        logf("%s: app version differs from the server (app %u, server %u): update both", peer_.c_str(),
             hello_.version, kProtocolVersion);
        alive_ = false;
        qcv_.notify_all();
        platformThreadExit();
        return;
    }
    logf("Headset connected: %s (%s)", hello_.deviceName, peer_.c_str());

    sendMonitorList();
    {
        uint32_t m = hello_.monitorMask;
        if (!m) m = cfg.monitor < 0 ? 0xFFu : 1u << std::min(cfg.monitor, kMaxStreams - 1);
        monitorMask_ = m;
    }

    std::unique_ptr<AudioCapture> audio;
    std::map<int, std::unique_ptr<StreamWorker>> workers;
    int64_t lastMonCheck = nowUs();
    while (alive_) {
        // audio on/off can change live
        bool wantAudio = config().audio;
        if (wantAudio && !audio) {
            audio = createAudioCapture();
            audio->start(
                [this](uint32_t rate) {
                    SAudioConfig ac{rate, 2, 16};
                    enqueue(S_AUDIO_CONFIG, &ac, sizeof(ac), nullptr, 0);
                },
                [this](const int16_t* s, uint32_t frames) { enqueue(S_AUDIO, nullptr, 0, s, (size_t)frames * 4); });
        } else if (!wantAudio && audio) {
            audio->stop();
            audio.reset();
        }

        uint32_t mask = monitorMask_;
        size_t count;
        {
            std::lock_guard<std::mutex> lk(inputM_);
            count = monitorCount_;
        }
        for (auto it = workers.begin(); it != workers.end();) {
            if (!(mask & (1u << it->first)) || it->first >= (int)count) {
                it->second->run = false;
                it->second->thread.join();
                SStreamId sid{(uint32_t)it->first};
                enqueue(S_VIDEO_STOP, &sid, sizeof(sid), nullptr, 0);
                stats_->streamStopped(it->first);
                logf("Screen %d: stream stopped", it->first + 1);
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
        for (int m = 0; m < (int)count; ++m) {
            if ((mask & (1u << m)) && !workers.count(m)) {
                auto w = std::make_unique<StreamWorker>();
                w->monitor = m;
                StreamWorker* raw = w.get();
                w->thread = std::thread(&Session::streamLoop, this, raw);
                workers[m] = std::move(w);
            }
        }
        uint32_t active = 0;
        for (auto& kv : workers) active |= 1u << kv.first;
        activeMask_ = active;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int64_t now = nowUs();
        if (now - lastMonCheck > 3000000) {  // monitors plugged/unplugged
            lastMonCheck = now;
            if (listMonitors().size() != count) sendMonitorList();
        }
    }
    for (auto& kv : workers) {
        kv.second->run = false;
        kv.second->thread.join();
        stats_->streamStopped(kv.first);
    }
    activeMask_ = 0;
    if (audio) audio->stop();
    alive_ = false;
    qcv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(inputM_);
        input_->releaseAll();
    }
    logf("Headset disconnected: %s", peer_.c_str());
    platformThreadExit();
}

// Capture + encode loop for one monitor.
void Session::streamLoop(StreamWorker* w) {
    platformThreadInit();
    const int sid = w->monitor;
    auto cap = createScreenCapture();
    auto enc = createVideoEncoder();
    int hotX = 0, hotY = 0;

    // capture timing of each submitted frame, looked up when its bitstream comes out of the encoder
    struct Timing {
        int64_t submitUs, captureUs, convertUs;
    };
    std::mutex tm;
    std::deque<Timing> timings;

    while (alive_ && w->run) {
        ServerConfig cfg = config();
        uint32_t version = cfgVersion_;
        int bitrate = hello_.bitrateKbps ? (int)hello_.bitrateKbps : cfg.bitrateKbps;
        int fps = hello_.fps ? (int)hello_.fps : cfg.fps;
        int maxW = cfg.maxWidth, maxH = cfg.maxHeight;
        if (hello_.maxWidth && (!maxW || (int)hello_.maxWidth < maxW)) maxW = hello_.maxWidth;
        if (hello_.maxHeight && (!maxH || (int)hello_.maxHeight < maxH)) maxH = hello_.maxHeight;
        const int64_t frameInterval = 1000000 / fps;

        if (!cap->init(sid, maxW, maxH)) {
            for (int i = 0; i < 10 && alive_ && w->run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        logf("Screen %d: %dx%d on %s -> stream %dx%d", sid + 1, cap->monWidth, cap->monHeight,
             cap->adapterName.c_str(), cap->outWidth, cap->outHeight);
        bool ok = enc->init(cap->outWidth, cap->outHeight, fps, bitrate, cfg.encoderHint, [&, sid](EncodedPacket&& pkt) {
            SVideoFrame vf{};
            vf.stream = (uint32_t)sid;
            vf.flags = pkt.key ? (uint32_t)FRAME_KEY : 0u;
            vf.captureUs = (uint64_t)pkt.timestampUs;
            vf.encodeUs = (uint32_t)std::max<int64_t>(0, pkt.outputUs - pkt.timestampUs);
            {
                std::lock_guard<std::mutex> lk(tm);
                while (!timings.empty() && timings.front().submitUs < pkt.timestampUs) timings.pop_front();
                if (!timings.empty() && timings.front().submitUs == pkt.timestampUs) {
                    vf.captureUs = (uint64_t)timings.front().captureUs;
                    vf.convertUs = (uint32_t)timings.front().convertUs;
                    vf.encodeUs = (uint32_t)std::max<int64_t>(0, pkt.outputUs - pkt.timestampUs);
                }
            }
            enqueue(S_VIDEO_FRAME, &vf, sizeof(vf), pkt.data.data(), pkt.data.size(), sid, pkt.key);
        });
        if (!ok) {
            for (int i = 0; i < 20 && alive_ && w->run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        stats_->streamStarted(sid, cap->outWidth, cap->outHeight, enc->name, enc->hardware);
        SVideoConfig vc{(uint32_t)sid,           CODEC_H264,
                        (uint32_t)cap->outWidth, (uint32_t)cap->outHeight,
                        (uint32_t)fps,           (uint32_t)cap->monWidth,
                        (uint32_t)cap->monHeight};
        enqueue(S_VIDEO_CONFIG, &vc, sizeof(vc), nullptr, 0);
        idrRequested_[sid] = true;

        bool dirty = false, haveImage = false;
        int64_t lastEncode = 0;
        int refreshCount = 0;
        while (alive_ && w->run && cfgVersion_ == version) {
            CursorUpdate cu;
            int r = cap->capture(4, cu);
            if (cu.shapeChanged) {
                hotX = cu.hotX;
                hotY = cu.hotY;
                SCursorShape cs{(uint16_t)cu.width, (uint16_t)cu.height, (uint16_t)cu.hotX, (uint16_t)cu.hotY};
                enqueue(S_CURSOR_SHAPE, &cs, sizeof(cs), cu.rgba.data(), cu.rgba.size());
            }
            if (cu.posChanged) {
                SCursorPos cp{};
                cp.stream = sid;
                cp.x = (float)(cu.x + hotX) / (float)cap->monWidth;
                cp.y = (float)(cu.y + hotY) / (float)cap->monHeight;
                cp.visible = cu.visible ? 1 : 0;
                enqueue(S_CURSOR_POS, &cp, sizeof(cp), nullptr, 0);
            }
            if (r < 0) break;  // reinit capture (resolution change, UAC, ...)
            if (r == 1) dirty = haveImage = true;

            int64_t now = nowUs();
            bool key = idrRequested_[sid].load();
            bool due = now - lastEncode >= frameInterval;
            // Re-send the last image a few times after the screen goes static: lets CBR refine the quality.
            bool refresh = !dirty && haveImage && refreshCount < 4 && now - lastEncode > 150000;
            if (haveImage && due && (dirty || key || refresh)) {
                {
                    std::lock_guard<std::mutex> lk(tm);
                    timings.push_back({now, dirty ? cap->lastGrabUs : now, dirty ? cap->lastConvertUs : 0});
                    while (timings.size() > 32) timings.pop_front();
                }
                if (enc->encode(cap->nv12(), now, key)) {
                    if (key) idrRequested_[sid] = false;
                    refreshCount = dirty ? 0 : refreshCount + 1;
                    dirty = false;
                    lastEncode = now;
                }
            }
        }
        enc->shutdown();
        cap->shutdown();
        stats_->streamStopped(sid);
    }
    platformThreadExit();
}
