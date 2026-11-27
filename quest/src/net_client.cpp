#include "net_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

using namespace mirroar;

void NetClient::start(const CHello& hello) {
    hello_ = hello;
    running_ = true;
    thread_ = std::thread(&NetClient::run, this);
}

void NetClient::stop() {
    running_ = false;
    int fd = fd_;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
}

void NetClient::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(statusM_);
    status_ = s;
}

std::string NetClient::status() {
    std::lock_guard<std::mutex> lk(statusM_);
    return status_;
}

bool NetClient::send(uint16_t type, const void* data, size_t len) {
    int fd = fd_;
    if (fd < 0 || !connected_) return false;
    MsgHeader h{(uint32_t)len, type, 0};
    std::lock_guard<std::mutex> lk(sendM_);
    if (::send(fd, &h, sizeof(h), MSG_NOSIGNAL) != (ssize_t)sizeof(h)) return false;
    const char* p = (const char*)data;
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static int connectWithTimeout(const sockaddr_in& addr, int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (const sockaddr*)&addr, sizeof(addr));
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r != 0) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeoutMs) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rcv = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    return fd;
}

static bool recvAll(int fd, void* buf, size_t len) {
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static void setRecvTimeout(int fd, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void NetClient::run() {
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    sockaddr_in ua{};
    ua.sin_family = AF_INET;
    ua.sin_port = htons(kDiscoveryPort);
    ua.sin_addr.s_addr = INADDR_ANY;
    if (bind(udp, (sockaddr*)&ua, sizeof(ua)) != 0) LOGE("discovery bind failed: %d", errno);

    while (running_) {
        setStatus("Looking for the PC...");
        // 1) USB: the server sets up `adb reverse tcp:47800`, so the PC is reachable on localhost.
        sockaddr_in lo{};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(kTcpPort);
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int fd = connectWithTimeout(lo, 300);
        if (fd >= 0) {
            if (session(fd, "USB")) continue;
        }
        if (!running_) break;

        // 2) LAN: wait up to ~1.5s for a beacon.
        pollfd pfd{udp, POLLIN, 0};
        int64_t until = monoUs() + 1500000;
        while (running_ && monoUs() < until) {
            if (poll(&pfd, 1, 200) <= 0) continue;
            DiscoveryBeacon b{};
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(udp, &b, sizeof(b), 0, (sockaddr*)&from, &fl);
            if (n < (ssize_t)sizeof(b) || memcmp(b.magic, kDiscoveryMagic, 8) != 0) continue;
            b.name[63] = 0;
            char ip[32];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            setStatus(std::string("Connecting to ") + b.name + " (" + ip + ")...");
            sockaddr_in sa = from;
            sa.sin_port = htons(b.tcpPort);
            fd = connectWithTimeout(sa, 1500);
            if (fd >= 0) {
                session(fd, std::string("Wi-Fi/LAN ") + ip);
            } else {
                setStatus(std::string("Cannot connect to ") + ip + "\n#Check the Windows firewall");
                sleep(2);
            }
            break;
        }
    }
    close(udp);
}

bool NetClient::session(int fd, const std::string& via) {
    fd_ = fd;
    // Wait for the server greeting (an `adb reverse` with no server behind it accepts and then closes).
    setRecvTimeout(fd, 2000);
    MsgHeader h;
    std::vector<uint8_t> buf;
    if (!recvAll(fd, &h, sizeof(h)) || h.type != S_HELLO || h.size < sizeof(SHello) || h.size > 4096) {
        fd_ = -1;
        close(fd);
        return false;
    }
    buf.resize(h.size);
    if (!recvAll(fd, buf.data(), h.size)) {
        fd_ = -1;
        close(fd);
        return false;
    }
    SHello sh;
    memcpy(&sh, buf.data(), sizeof(sh));
    sh.name[63] = 0;
    if (sh.version != kProtocolVersion) {
        setStatus(std::string("The server on ") + sh.name + " is a different version\n#Update MirRoarServer.exe and the app together");
        fd_ = -1;
        close(fd);
        sleep(3);
        return true;
    }
    connected_ = true;
    send(C_HELLO, &hello_, sizeof(hello_));
    setStatus(std::string("Connected to ") + sh.name + " via " + via);
    LOGI("connected to %s via %s", sh.name, via.c_str());
    handler_->onConnected(sh.name, via);

    // Keepalive / RTT measurement
    std::atomic<bool> pingRun{true};
    std::thread pinger([&] {
        while (pingRun && running_) {
            SPing p{(uint64_t)monoUs()};
            send(C_PING, &p, sizeof(p));
            for (int i = 0; i < 5 && pingRun; ++i) usleep(100000);
        }
    });

    setRecvTimeout(fd, 5000);
    while (running_) {
        if (!recvAll(fd, &h, sizeof(h))) break;
        if (h.size > 64u * 1024 * 1024) break;
        buf.resize(h.size);
        if (h.size && !recvAll(fd, buf.data(), h.size)) break;
        const uint8_t* p = buf.data();
        switch (h.type) {
            case S_VIDEO_CONFIG:
                if (h.size >= sizeof(SVideoConfig)) {
                    SVideoConfig c;
                    memcpy(&c, p, sizeof(c));
                    if (c.stream < (uint32_t)kMaxStreams) handler_->onVideoConfig(c);
                }
                break;
            case S_VIDEO_FRAME:
                if (h.size > sizeof(SVideoFrame)) {
                    SVideoFrame f;
                    memcpy(&f, p, sizeof(f));
                    if (f.stream < (uint32_t)kMaxStreams)
                        handler_->onVideoFrame(f, p + sizeof(f), h.size - sizeof(f), monoUs());
                }
                break;
            case S_VIDEO_STOP:
                if (h.size >= sizeof(SStreamId)) {
                    SStreamId sid;
                    memcpy(&sid, p, sizeof(sid));
                    if (sid.stream < (uint32_t)kMaxStreams) handler_->onVideoStop((int)sid.stream);
                }
                break;
            case S_MONITOR_LIST:
                if (h.size >= sizeof(SMonitorList)) {
                    SMonitorList ml;
                    memcpy(&ml, p, sizeof(ml));
                    std::vector<SMonitorInfo> mons;
                    for (uint32_t i = 0; i < ml.count && sizeof(ml) + (i + 1) * sizeof(SMonitorInfo) <= h.size; ++i) {
                        SMonitorInfo mi;
                        memcpy(&mi, p + sizeof(ml) + i * sizeof(SMonitorInfo), sizeof(mi));
                        mons.push_back(mi);
                    }
                    handler_->onMonitorList(mons);
                }
                break;
            case S_CURSOR_POS:
                if (h.size >= sizeof(SCursorPos)) {
                    SCursorPos c;
                    memcpy(&c, p, sizeof(c));
                    handler_->onCursorPos(c);
                }
                break;
            case S_CURSOR_SHAPE:
                if (h.size >= sizeof(SCursorShape)) {
                    SCursorShape s;
                    memcpy(&s, p, sizeof(s));
                    if (h.size >= sizeof(s) + (size_t)s.width * s.height * 4) handler_->onCursorShape(s, p + sizeof(s));
                }
                break;
            case S_AUDIO_CONFIG:
                if (h.size >= sizeof(SAudioConfig)) {
                    SAudioConfig c;
                    memcpy(&c, p, sizeof(c));
                    handler_->onAudioConfig(c);
                }
                break;
            case S_AUDIO:
                handler_->onAudio((const int16_t*)p, h.size / 4);
                break;
            case S_PONG:
                if (h.size >= sizeof(SPong)) {
                    SPong pp;
                    memcpy(&pp, p, sizeof(pp));
                    int64_t now = monoUs();
                    int64_t rtt = now - (int64_t)pp.clientTimeUs;
                    rttUs_ = (int)rtt;
                    // NTP-style estimate; the sample with the smallest round trip is the most accurate
                    clockSamples_.push_back({rtt, (int64_t)pp.serverTimeUs + rtt / 2 - now});
                    if (clockSamples_.size() > 16) clockSamples_.erase(clockSamples_.begin());
                    auto best = clockSamples_.front();
                    for (auto& cs : clockSamples_)
                        if (cs.rtt < best.rtt) best = cs;
                    clockOffsetUs_ = best.offset;
                    clockSynced_ = true;
                }
                break;
            default:
                break;
        }
    }
    pingRun = false;
    connected_ = false;
    clockSynced_ = false;
    clockSamples_.clear();
    {
        std::lock_guard<std::mutex> lk(sendM_);
        int f = fd_.exchange(-1);
        if (f >= 0) close(f);
    }
    pinger.join();
    LOGI("disconnected");
    handler_->onDisconnected();
    setStatus("Connection lost, looking for the PC...");
    sleep(1);
    return true;
}
