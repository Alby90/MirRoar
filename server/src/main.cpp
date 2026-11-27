// MirRoar server - entry point: GUI by default, console mode with --headless.
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "core/server.h"
#include "gui/gui.h"
#include "platform/platform.h"

static std::atomic<bool> g_stop{false};

static void printUsage() {
    printf(
        "Usage: MirRoarServer [options]\n"
        "  --headless         console only, no window\n"
        "  --minimized        start hidden in the system tray\n"
        "  --monitor N|all    initial screen: 1, 2, ... or all\n"
        "  --list-monitors    list the monitors and exit\n"
        "  --bitrate MBPS     video bitrate in Mbps (default 30)\n"
        "  --fps N            frames per second (default 60)\n"
        "  --max-width W      maximum stream width (default: native)\n"
        "  --encoder NAME     preferred encoder (nvidia, intel, amd, software...)\n"
        "  --port P           TCP port (default 47800)\n"
        "  --name NAME        PC name shown in the headset\n"
        "  --no-audio         do not stream audio\n"
        "  --no-adb           do not set up the USB link through adb\n"
        "  --adb PATH         path of adb.exe\n"
        "Settings from the window are saved in %s\n",
        (configDir() + "/server.ini").c_str());
}

#ifdef _WIN32
// The executable is a GUI app: attach to the parent console for --headless / --help output.
static void attachConsole() {
    // Output already redirected to a file or pipe: keep it.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        printf("\n");
    }
}
#else
static void attachConsole() {}
#endif

int main(int argc, char** argv) {
    platformInit();

    std::string cfgPath = configDir() + "/server.ini";
    ServerConfig cfg;
    cfg.name = hostName();
    loadConfig(cfg, cfgPath);
    if (cfg.name.empty()) cfg.name = hostName();

    bool headless = false, minimized = cfg.startMinimized, cliOverrides = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--headless") headless = true;
        else if (a == "--minimized") minimized = true;
        else if (a == "--monitor") {
            std::string v = next();
            cfg.monitor = (v == "all") ? -1 : std::max(0, atoi(v.c_str()) - 1);
            cliOverrides = true;
        } else if (a == "--bitrate") cfg.bitrateKbps = (int)(atof(next().c_str()) * 1000), cliOverrides = true;
        else if (a == "--fps") cfg.fps = atoi(next().c_str()), cliOverrides = true;
        else if (a == "--max-width") cfg.maxWidth = atoi(next().c_str()), cliOverrides = true;
        else if (a == "--max-height") cfg.maxHeight = atoi(next().c_str()), cliOverrides = true;
        else if (a == "--encoder") cfg.encoderHint = next(), cliOverrides = true;
        else if (a == "--port") cfg.port = (uint16_t)atoi(next().c_str()), cliOverrides = true;
        else if (a == "--name") cfg.name = next(), cliOverrides = true;
        else if (a == "--no-audio") cfg.audio = false, cliOverrides = true;
        else if (a == "--no-adb") cfg.adb = false, cliOverrides = true;
        else if (a == "--adb") cfg.adbPath = next(), cliOverrides = true;
        else if (a == "--list-monitors") {
            attachConsole();
            auto mons = listMonitors();
            for (size_t m = 0; m < mons.size(); ++m)
                printf("%zu: %s %dx%d @(%d,%d)%s [%s]\n", m + 1, mons[m].name.c_str(), mons[m].width, mons[m].height,
                       mons[m].x, mons[m].y, mons[m].primary ? " primary" : "", mons[m].adapter.c_str());
            return 0;
        } else {
            attachConsole();
            printUsage();
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }
    if (cfg.fps < 10) cfg.fps = 60;
    if (cfg.bitrateKbps < 1000) cfg.bitrateKbps = 1000;
    (void)cliOverrides;

    if (!acquireSingleInstance()) {
        if (headless) {
            attachConsole();
            printf("MirRoar Server is already running.\n");
        } else {
            showMessage("MirRoar Server", "MirRoar Server is already running.\nLook for its icon in the system tray.");
        }
        return 1;
    }

    Server server;
    int rc = 0;
    if (headless) {
        attachConsole();
        logEnableConsole(true);
        printf("MirRoar Server  -  PC: %s  port: %u   (Ctrl+C to quit)\n", cfg.name.c_str(), cfg.port);
        server.start(cfg);
        std::signal(SIGINT, [](int) { g_stop = true; });
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        logEnableConsole(false);
        server.start(cfg);
        rc = runGui(server, cfgPath, minimized);
    }
    server.stop();
    platformShutdown();
    return rc;
}
