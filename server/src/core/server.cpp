#include "server.h"

#include <string.h>

#include <algorithm>

#include "log.h"

using namespace mirroar;

static std::string ipToString(uint32_t addrNet) {
    char ip[32];
    in_addr a;
    memcpy(&a, &addrNet, 4);
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    return ip;
}

bool Server::start(const ServerConfig& cfg) {
    cfg_ = cfg;
    running_ = true;
    {
        std::lock_guard<std::mutex> lk(monM_);
        monitors_ = listMonitors();
    }
    openListener();
    accept_ = std::thread(&Server::acceptLoop, this);
    discovery_ = std::thread(&Server::discoveryLoop, this);
    adb_ = std::thread(&Server::adbLoop, this);
    statsT_ = std::thread(&Server::statsLoop, this);
    return listen_ != kInvalidSocket;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    closeListener();
    if (accept_.joinable()) accept_.join();
    if (discovery_.joinable()) discovery_.join();
    if (adb_.joinable()) adb_.join();
    if (statsT_.joinable()) statsT_.join();
    std::unique_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s = std::move(session_);
    }
    if (s) s->stop();
}

bool Server::openListener() {
    uint16_t port;
    {
        std::lock_guard<std::mutex> lk(m_);
        port = cfg_.port;
    }
    socket_t ls = noInherit(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
        closeSocket(ls);
        std::lock_guard<std::mutex> lk(m_);
        listen_ = kInvalidSocket;
        listenError_ = "Port " + std::to_string(port) + " is already in use";
        logf("Cannot listen on port %u (already in use?)", port);
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    listen_ = ls;
    listenError_.clear();
    logf("Listening on port %u", port);
    return true;
}

void Server::closeListener() {
    std::lock_guard<std::mutex> lk(m_);
    if (listen_ != kInvalidSocket) {
        closeSocket(listen_);
        listen_ = kInvalidSocket;
    }
}

void Server::acceptLoop() {
    while (running_) {
        socket_t ls;
        {
            std::lock_guard<std::mutex> lk(m_);
            ls = listen_;
        }
        if (ls == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        timeval tv{0, 200000};
        if (select((int)ls + 1, &rs, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        socket_t cs = noInherit(accept(ls, (sockaddr*)&peer, &plen));
        if (cs == kInvalidSocket) continue;
        char ip[64];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string peerStr = ip;
        if (peerStr == "127.0.0.1") peerStr = "USB";
        else peerStr = "Wi-Fi/LAN " + peerStr;

        std::unique_ptr<Session> old;
        ServerConfig cfg;
        {
            std::lock_guard<std::mutex> lk(m_);
            old = std::move(session_);
            cfg = cfg_;
        }
        if (old) {
            if (old->alive()) logf("New connection from %s: replacing %s", peerStr.c_str(), old->peer().c_str());
            old->stop();
        }
        stats_.reset();
        auto s = std::make_unique<Session>(cs, cfg, peerStr, &stats_);
        s->start();
        std::lock_guard<std::mutex> lk(m_);
        session_ = std::move(s);
    }
}

void Server::discoveryLoop() {
    while (running_) {
        DiscoveryBeacon b{};
        memcpy(b.magic, kDiscoveryMagic, 8);
        b.version = kProtocolVersion;
        {
            std::lock_guard<std::mutex> lk(m_);
            b.tcpPort = cfg_.port;
            strncpy(b.name, cfg_.name.c_str(), sizeof(b.name) - 1);
        }
        for (auto& ni : listIPv4Interfaces()) {
            socket_t s = noInherit(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            if (s == kInvalidSocket) continue;
            int yes = 1;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = ni.addr;
            if (bind(s, (sockaddr*)&local, sizeof(local)) == 0) {
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(kDiscoveryPort);
                dst.sin_addr.s_addr = INADDR_BROADCAST;
                sendto(s, (const char*)&b, sizeof(b), 0, (sockaddr*)&dst, sizeof(dst));
                if (ni.mask && ni.mask != 0xFFFFFFFFu) {  // subnet-directed broadcast too
                    dst.sin_addr.s_addr = ni.addr | ~ni.mask;
                    sendto(s, (const char*)&b, sizeof(b), 0, (sockaddr*)&dst, sizeof(dst));
                }
            }
            closeSocket(s);
        }
        for (int i = 0; i < 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Server::adbLoop() {
    std::set<std::string> reversed;
    std::string lastAdb = "?";
    while (running_) {
        ServerConfig cfg = config();
        if (adbReset_.exchange(false)) reversed.clear();
        std::string adb = cfg.adb ? findAdb(cfg.adbPath) : std::string();
        if (adb != lastAdb) {
            if (cfg.adb && adb.empty())
                logf("USB: adb not found (put it next to the server or in PATH to use the cable)");
            else if (!adb.empty())
                logf("USB: using %s", adb.c_str());
            lastAdb = adb;
            reversed.clear();
        }
        std::vector<std::string> present;
        if (!adb.empty()) {
            std::string out = runProcess("\"" + adb + "\" devices");
            size_t pos = 0;
            while (pos < out.size()) {
                size_t eol = out.find('\n', pos);
                if (eol == std::string::npos) eol = out.size();
                std::string line = out.substr(pos, eol - pos);
                pos = eol + 1;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string serial = line.substr(0, tab);
                std::string state = line.substr(tab + 1);
                while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
                if (state != "device" || serial.find(':') != std::string::npos) continue;
                present.push_back(serial);
                if (!reversed.count(serial)) {
                    std::string p = std::to_string(cfg.port);
                    runProcess("\"" + adb + "\" -s " + serial + " reverse tcp:" + p + " tcp:" + p);
                    logf("USB: cable link active for headset %s", serial.c_str());
                    reversed.insert(serial);
                }
            }
            for (auto it = reversed.begin(); it != reversed.end();) {
                if (std::find(present.begin(), present.end(), *it) == present.end()) {
                    logf("USB: headset %s unplugged", it->c_str());
                    it = reversed.erase(it);
                } else {
                    ++it;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(usbM_);
            adbPath_ = adb;
            usbDevices_ = present;
        }
        for (int i = 0; i < 30 && running_ && !adbReset_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Server::statsLoop() {
    int n = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stats_.tick();
        // drop finished sessions
        std::unique_ptr<Session> dead;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (session_ && !session_->alive()) dead = std::move(session_);
        }
        if (dead) {
            dead->stop();
            stats_.reset();
        }
        if (++n % 6 == 0) {  // monitors plugged/unplugged
            auto mons = listMonitors();
            std::lock_guard<std::mutex> lk(monM_);
            monitors_ = mons;
        }
    }
}

ServerConfig Server::config() {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

void Server::applyConfig(const ServerConfig& cfg) {
    ServerConfig old;
    {
        std::lock_guard<std::mutex> lk(m_);
        old = cfg_;
        cfg_ = cfg;
        if (session_) session_->updateConfig(cfg);
    }
    if (cfg.port != old.port) {
        closeListener();
        openListener();
        adbReset_ = true;
    }
    if (cfg.adb != old.adb || cfg.adbPath != old.adbPath) adbReset_ = true;
}

void Server::selectMonitors(uint32_t mask) {
    std::lock_guard<std::mutex> lk(m_);
    if (session_) session_->selectMonitors(mask);
}

void Server::disconnectClient() {
    std::unique_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s = std::move(session_);
    }
    if (s) {
        logf("Headset disconnected from the PC");
        s->stop();
    }
    stats_.reset();
}

Server::Status Server::status() {
    Status st;
    {
        std::lock_guard<std::mutex> lk(m_);
        st.listening = listen_ != kInvalidSocket;
        st.listenError = listenError_;
        if (session_ && session_->alive() && session_->handshakeDone()) {
            st.connected = true;
            st.client = session_->clientName();
            st.via = session_->peer();
            st.activeMask = session_->activeMask();
        }
    }
    for (auto& ni : listIPv4Interfaces()) st.addresses.push_back(ipToString(ni.addr) + "  (" + ni.name + ")");
    {
        std::lock_guard<std::mutex> lk(usbM_);
        st.adbPath = adbPath_;
        st.usbDevices = usbDevices_;
    }
    {
        std::lock_guard<std::mutex> lk(monM_);
        st.monitors = monitors_;
    }
    st.stats = stats_.snapshot();
    return st;
}
