#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Plain stdio on purpose: <fstream> would pull ~1 MB of locale machinery into the static executable.

bool ServerConfig::operator==(const ServerConfig& o) const {
    return monitor == o.monitor && fps == o.fps && bitrateKbps == o.bitrateKbps && maxWidth == o.maxWidth &&
           maxHeight == o.maxHeight && audio == o.audio && adb == o.adb && port == o.port &&
           encoderHint == o.encoderHint && name == o.name && adbPath == o.adbPath &&
           startMinimized == o.startMinimized;
}

bool loadConfig(ServerConfig& c, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
        int i = atoi(v.c_str());
        if (k == "monitor") c.monitor = i;
        else if (k == "fps") c.fps = i;
        else if (k == "bitrate_kbps") c.bitrateKbps = i;
        else if (k == "max_width") c.maxWidth = i;
        else if (k == "max_height") c.maxHeight = i;
        else if (k == "audio") c.audio = i != 0;
        else if (k == "adb") c.adb = i != 0;
        else if (k == "port") c.port = (uint16_t)i;
        else if (k == "encoder") c.encoderHint = v;
        else if (k == "name") c.name = v;
        else if (k == "adb_path") c.adbPath = v;
        else if (k == "start_minimized") c.startMinimized = i != 0;
    }
    fclose(f);
    if (c.fps < 10 || c.fps > 240) c.fps = 60;
    if (c.bitrateKbps < 1000) c.bitrateKbps = 1000;
    if (c.port == 0) c.port = 47800;
    return true;
}

bool saveConfig(const ServerConfig& c, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f,
            "monitor=%d\nfps=%d\nbitrate_kbps=%d\nmax_width=%d\nmax_height=%d\naudio=%d\nadb=%d\nport=%u\n"
            "encoder=%s\nname=%s\nadb_path=%s\nstart_minimized=%d\n",
            c.monitor, c.fps, c.bitrateKbps, c.maxWidth, c.maxHeight, c.audio ? 1 : 0, c.adb ? 1 : 0, (unsigned)c.port,
            c.encoderHint.c_str(), c.name.c_str(), c.adbPath.c_str(), c.startMinimized ? 1 : 0);
    fclose(f);
    return true;
}
