// MirRoar server - settings (command line, GUI, saved to <configDir>/server.ini).
#pragma once
#include <stdint.h>
#include <string>

struct ServerConfig {
    int monitor = 0;  // initial screen: index, or -1 = all
    int fps = 60;
    int bitrateKbps = 30000;
    int maxWidth = 0;  // 0 = native
    int maxHeight = 0;
    bool audio = true;
    bool adb = true;
    uint16_t port = 47800;
    std::string encoderHint;  // "" = automatic, otherwise (part of) the encoder name
    std::string name;         // PC name shown in the headset
    std::string adbPath;
    bool startMinimized = false;

    bool operator==(const ServerConfig& o) const;
    bool operator!=(const ServerConfig& o) const { return !(*this == o); }
    // true if a change requires restarting the video streams
    bool videoDiffers(const ServerConfig& o) const {
        return fps != o.fps || bitrateKbps != o.bitrateKbps || maxWidth != o.maxWidth || maxHeight != o.maxHeight ||
               encoderHint != o.encoderHint;
    }
};

bool loadConfig(ServerConfig& cfg, const std::string& path);
bool saveConfig(const ServerConfig& cfg, const std::string& path);
