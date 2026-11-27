// MirRoar server - the whole service: listener, LAN discovery, USB (adb reverse), sessions, statistics.
// Used both by the GUI and by the headless (console) mode.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "config.h"
#include "net.h"
#include "session.h"
#include "stats.h"

class Server {
public:
    ~Server() { stop(); }
    bool start(const ServerConfig& cfg);
    void stop();

    void applyConfig(const ServerConfig& cfg);  // live: restarts what needs restarting
    ServerConfig config();
    void selectMonitors(uint32_t mask);         // live selection on the connected headset
    void disconnectClient();
    void setHeadsetOptions(const float* v);     // pushed to the connected headset, which stores them

    struct Status {
        bool listening = false;
        std::string listenError;
        std::vector<std::string> addresses;  // "192.168.1.10 (Ethernet)"
        bool connected = false;
        std::string client, via;
        std::string adbPath;
        std::vector<std::string> usbDevices;
        std::vector<MonitorDesc> monitors;
        uint32_t activeMask = 0;
        StatsCollector::Snapshot stats;
        bool hasOptions = false;  // the connected headset reported its options
        float options[mirroar::OPT_COUNT] = {};
    };
    Status status();

private:
    bool openListener();
    void closeListener();
    void acceptLoop();
    void discoveryLoop();
    void adbLoop();
    void statsLoop();

    std::mutex m_;
    ServerConfig cfg_;
    std::atomic<bool> running_{false};
    socket_t listen_ = kInvalidSocket;
    std::string listenError_;
    std::thread accept_, discovery_, adb_, statsT_;
    std::unique_ptr<Session> session_;
    StatsCollector stats_;

    std::mutex usbM_;
    std::string adbPath_;
    std::vector<std::string> usbDevices_;
    std::atomic<bool> adbReset_{false};

    std::mutex monM_;
    std::vector<MonitorDesc> monitors_;
};
