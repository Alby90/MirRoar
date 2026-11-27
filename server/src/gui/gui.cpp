#include "gui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../core/log.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "implot.h"

namespace {

// ------------------------------------------------------------------ palette
const ImVec4 kAccent(0.25f, 0.61f, 1.00f, 1.0f);
const ImVec4 kGood(0.30f, 0.82f, 0.45f, 1.0f);
const ImVec4 kWarn(0.98f, 0.76f, 0.25f, 1.0f);
const ImVec4 kBad(0.96f, 0.36f, 0.36f, 1.0f);
const ImVec4 kMuted(0.60f, 0.64f, 0.72f, 1.0f);

struct Stage {
    const char* name;
    ImVec4 color;
};
const Stage kStages[6] = {
    {"Capture", ImVec4(0.66f, 0.47f, 0.96f, 1)},  {"Encode", ImVec4(0.29f, 0.56f, 0.98f, 1)},
    {"Queue", ImVec4(0.30f, 0.80f, 0.90f, 1)},     {"Network", ImVec4(0.36f, 0.82f, 0.48f, 1)},
    {"Decode", ImVec4(0.98f, 0.66f, 0.28f, 1)}, {"Display", ImVec4(0.94f, 0.42f, 0.62f, 1)},
};

float stageValue(const LatencyPoint& p, int i) {
    switch (i) {
        case 0: return p.convert;
        case 1: return p.encode;
        case 2: return p.queue;
        case 3: return p.network;
        case 4: return p.decode;
        default: return p.render;
    }
}

ImVec4 latencyColor(float ms) { return ms < 50 ? kGood : (ms < 90 ? kWarn : kBad); }

// Small monitor-shaped icon for the window and the tray, drawn in code (no image files needed).
SDL_Surface* makeIcon(int size) {
    SDL_Surface* s = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    uint8_t* px = (uint8_t*)s->pixels;
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint8_t* p = px + y * s->pitch + x * 4;
        p[0] = r, p[1] = g, p[2] = b, p[3] = a;
    };
    float k = size / 32.0f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float fx = x / k, fy = y / k;
            bool frame = fx >= 2 && fx < 30 && fy >= 5 && fy < 23;
            bool inner = fx >= 4 && fx < 28 && fy >= 7 && fy < 21;
            bool stand = (fx >= 13 && fx < 19 && fy >= 23 && fy < 26) || (fx >= 9 && fx < 23 && fy >= 26 && fy < 28);
            bool line = inner && ((fy >= 10 && fy < 11.5f && fx < 18) || (fy >= 13.5f && fy < 15 && fx < 24));
            if (line) put(x, y, 140, 192, 255, 255);
            else if (inner) put(x, y, 12, 16, 32, 255);
            else if (frame || stand) put(x, y, 64, 156, 255, 255);
            else put(x, y, 0, 0, 0, 0);
        }
    return s;
}

void applyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.PopupRounding = 6;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.WindowPadding = ImVec2(16, 14);
    st.SeparatorTextBorderSize = 1;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.105f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.115f, 0.145f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 1);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.28f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.175f, 0.22f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.28f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.26f, 0.33f, 1);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.26f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.45f, 0.75f, 1);
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.20f, 0.26f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.30f, 0.45f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.40f, 0.65f, 1);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.72f, 1, 1);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.30f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.155f, 0.195f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.96f, 1);
    c[ImGuiCol_TextDisabled] = kMuted;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;

    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.Colors[ImPlotCol_PlotBg] = ImVec4(0.085f, 0.093f, 0.12f, 1);
    ps.Colors[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    ps.Colors[ImPlotCol_LegendBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.85f);
    ps.PlotPadding = ImVec2(6 * scale, 6 * scale);
}

struct Gui {
    Server& server;
    std::string cfgPath;
    ServerConfig cfg;
    Server::Status st;
    int64_t lastStatus = 0;
    std::vector<std::string> encoders;
    bool autostart = false;
    bool visible = true;
    bool quit = false;
    bool showRequested = false;
    float scale = 1;
    ImFont* titleFont = nullptr;
    ImFont* bigFont = nullptr;
    uint64_t lastLogVersion = 0;
    char nameBuf[64] = {};
    float opts[mirroar::OPT_COUNT] = {};  // headset options being shown/edited

    explicit Gui(Server& s) : server(s) {}

    void commit() {
        if (cfg != server.config()) {
            server.applyConfig(cfg);
            saveConfig(cfg, cfgPath);
        }
    }

    // ---------------------------------------------------------- left column
    void drawStatusDot(const ImVec4& col) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetTextLineHeight() * 0.32f;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.55f), r,
                                                    ImGui::ColorConvertFloat4ToU32(col));
        ImGui::Dummy(ImVec2(r * 2, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
    }

    void leftPanel() {
        ImGui::SeparatorText("Headset");
        if (st.connected) {
            drawStatusDot(kGood);
            ImGui::Text("Connected: %s", st.client.c_str());
            ImGui::TextDisabled("Link: %s", st.via.c_str());
            if (st.stats.rttMs > 0) ImGui::TextDisabled("Ping: %.1f ms", st.stats.rttMs);
            if (ImGui::Button("Disconnect")) server.disconnectClient();
        } else {
            drawStatusDot(st.listening ? kWarn : kBad);
            ImGui::TextUnformatted(st.listening ? "Waiting for the headset..." : "Server not listening");
            if (!st.listenError.empty()) ImGui::TextColored(kBad, "%s", st.listenError.c_str());
            ImGui::TextWrapped("Open MirRoar on the Quest: this PC is found automatically (same network or USB cable).");
        }
        if (!st.addresses.empty() && ImGui::TreeNode("Addresses of this PC")) {
            for (auto& a : st.addresses) ImGui::TextDisabled("%s", a.c_str());
            ImGui::TreePop();
        }

        ImGui::SeparatorText("Screens");
        if (st.monitors.empty()) ImGui::TextDisabled("No monitor found");
        if (st.connected) {
            uint32_t mask = st.activeMask;
            for (size_t i = 0; i < st.monitors.size(); ++i) {
                const MonitorDesc& m = st.monitors[i];
                bool on = (mask >> i) & 1;
                char label[96];
                snprintf(label, sizeof(label), "Screen %zu   %dx%d%s##mon%zu", i + 1, m.width, m.height,
                         m.primary ? "  (primary)" : "", i);
                if (ImGui::Checkbox(label, &on)) {
                    uint32_t nm = on ? (mask | (1u << i)) : (mask & ~(1u << i));
                    if (nm) server.selectMonitors(nm);
                }
            }
        } else {
            for (size_t i = 0; i < st.monitors.size(); ++i)
                ImGui::TextDisabled("Screen %zu   %dx%d%s", i + 1, st.monitors[i].width, st.monitors[i].height,
                                    st.monitors[i].primary ? "  (primary)" : "");
        }
        {
            std::vector<std::string> items;
            for (size_t i = 0; i < st.monitors.size(); ++i) items.push_back("Screen " + std::to_string(i + 1));
            items.push_back("All");
            int idx = cfg.monitor < 0 ? (int)items.size() - 1 : std::min(cfg.monitor, (int)items.size() - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##startmon", ("At startup: " + items[idx]).c_str())) {
                for (int i = 0; i < (int)items.size(); ++i)
                    if (ImGui::Selectable(items[i].c_str(), i == idx)) {
                        cfg.monitor = i == (int)items.size() - 1 ? -1 : i;
                        commit();
                    }
                ImGui::EndCombo();
            }
        }

        ImGui::SeparatorText("Video");
        int mbps = cfg.bitrateKbps / 1000;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##bitrate", &mbps, 5, 150, "Bitrate: %d Mbps")) cfg.bitrateKbps = mbps * 1000;
        if (ImGui::IsItemDeactivatedAfterEdit()) commit();
        ImGui::TextDisabled("Wi-Fi 6: 30-60 Mbps  |  USB cable: up to 150");

        static const int kFps[] = {30, 45, 60, 72, 90, 120};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##fps", ("Frames per second: " + std::to_string(cfg.fps)).c_str())) {
            for (int f : kFps)
                if (ImGui::Selectable(std::to_string(f).c_str(), f == cfg.fps)) {
                    cfg.fps = f;
                    commit();
                }
            ImGui::EndCombo();
        }

        static const int kRes[] = {0, 3840, 2560, 1920, 1600, 1280};
        auto resName = [](int w) { return w == 0 ? std::string("native") : std::to_string(w) + " px wide"; };
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##res", ("Max resolution: " + resName(cfg.maxWidth)).c_str())) {
            for (int w : kRes)
                if (ImGui::Selectable(resName(w).c_str(), w == cfg.maxWidth)) {
                    cfg.maxWidth = w;
                    cfg.maxHeight = 0;
                    commit();
                }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-1);
        std::string encLabel = "Encoder: " + (cfg.encoderHint.empty() ? std::string("automatic") : cfg.encoderHint);
        if (ImGui::BeginCombo("##enc", encLabel.c_str())) {
            if (ImGui::Selectable("Automatic (hardware when available)", cfg.encoderHint.empty())) {
                cfg.encoderHint.clear();
                commit();
            }
            for (auto& e : encoders)
                if (ImGui::Selectable(e.c_str(), e == cfg.encoderHint)) {
                    cfg.encoderHint = e;
                    commit();
                }
            ImGui::EndCombo();
        }

        ImGui::SeparatorText("Audio and USB cable");
        if (ImGui::Checkbox("Stream the PC audio", &cfg.audio)) commit();
        if (ImGui::Checkbox("USB cable link (adb)", &cfg.adb)) commit();
        if (cfg.adb) {
            if (st.adbPath.empty())
                ImGui::TextColored(kWarn, "adb.exe not found: put it next to the server or in PATH");
            else if (st.usbDevices.empty())
                ImGui::TextDisabled("No headset connected by cable");
            else
                for (auto& d : st.usbDevices) ImGui::TextColored(kGood, "Headset on cable: %s", d.c_str());
        }

        optionsSection();

        ImGui::SeparatorText("General");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##name", "PC name shown in the headset", nameBuf, sizeof(nameBuf));
        if (ImGui::IsItemDeactivatedAfterEdit() && nameBuf[0]) {
            cfg.name = nameBuf;
            commit();
        }
        int port = cfg.port;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputInt("Port", &port, 0);
        if (ImGui::IsItemDeactivatedAfterEdit() && port > 1024 && port < 65535) {
            cfg.port = (uint16_t)port;
            commit();
        }
        if (ImGui::Checkbox("Start with Windows", &autostart)) setAutostart(autostart);
        if (ImGui::Checkbox("Start hidden in the system tray", &cfg.startMinimized)) commit();
    }

    // Headset options: stored in the headset; shown and edited here while it is connected.
    void optionsSection() {
        using namespace mirroar;
        ImGui::SeparatorText("Headset options");
        if (!st.connected || !st.hasOptions) {
            ImGui::TextDisabled(st.connected ? "Waiting for the headset options..." : "Connect the headset to change its options.");
            return;
        }
        if (!ImGui::IsAnyItemActive()) memcpy(opts, st.options, sizeof(opts));
        const char* group = nullptr;
        for (int i = 0; i < OPT_COUNT; ++i) {
            const OptionDef& d = kOptions[i];
            if (!group || strcmp(group, d.group) != 0) {
                group = d.group;
                ImGui::TextDisabled("%s", group);
            }
            ImGui::PushID(i);
            if (!d.unit) {
                bool on = opts[i] >= 0.5f;
                if (ImGui::Checkbox(d.label, &on)) {
                    opts[i] = on ? 1.0f : 0.0f;
                    server.setHeadsetOptions(opts);
                }
            } else {
                float shown = opts[i] * d.shown;
                char fmt[96];
                snprintf(fmt, sizeof(fmt), "%s: %%.%df %s", d.label, d.decimals, d.unit);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##v", &shown, d.min * d.shown, d.max * d.shown, fmt))
                    opts[i] = clampOption(i, shown / d.shown);
                if (ImGui::IsItemDeactivatedAfterEdit()) server.setHeadsetOptions(opts);
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Reset options to defaults")) {
            defaultOptions(opts);
            server.setHeadsetOptions(opts);
        }
    }

    // ---------------------------------------------------------- right column
    void latencyBar(const LatencyPoint& p) {
        float total = 0;
        for (int i = 0; i < 6; ++i) total += std::max(0.0f, stageValue(p, i));
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 14 * scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), h * 0.5f);
        float x = pos.x;
        for (int i = 0; i < 6 && total > 0; ++i) {
            float seg = w * std::max(0.0f, stageValue(p, i)) / total;
            if (seg < 0.5f) continue;
            ImDrawFlags fl = ImDrawFlags_RoundCornersNone;
            dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + seg, pos.y + h), ImGui::ColorConvertFloat4ToU32(kStages[i].color),
                              0, fl);
            x += seg;
        }
        ImGui::Dummy(ImVec2(w, h));
        // legend with values
        for (int i = 0; i < 6; ++i) {
            if (i % 3 != 0) ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0 + (i % 3) * (w / 3));
            ImGui::ColorButton(kStages[i].name, kStages[i].color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                               ImVec2(10 * scale, 10 * scale));
            ImGui::SameLine();
            ImGui::Text("%s  %.1f ms", kStages[i].name, stageValue(p, i));
        }
    }

    void rightPanel() {
        const auto& s = st.stats;
        // headline
        if (st.connected && s.hasLatency) {
            ImGui::PushFont(bigFont, 40.0f);
            ImGui::TextColored(latencyColor(s.current.total), "%.0f ms", s.current.total);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted("latency from PC to headset");
            ImGui::TextDisabled("peak %.0f ms  |  ping %.1f ms", s.current.maxTotal, s.rttMs);
            ImGui::EndGroup();
            latencyBar(s.current);
        } else {
            ImGui::PushFont(bigFont, 40.0f);
            ImGui::TextDisabled("-- ms");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted("latency from PC to headset");
            ImGui::TextDisabled(st.connected ? "waiting for measurements (move something on screen)" : "no headset connected");
            ImGui::EndGroup();
        }
        ImGui::Spacing();

        float avail = ImGui::GetContentRegionAvail().y;
        float plotH = std::max(140.0f * scale, (avail - 240 * scale) * 0.5f);

        // latency history (stacked)
        const int n = (int)s.latency.size();
        std::vector<double> xs(n), lo(n), hi(n), tot(n);
        for (int i = 0; i < n; ++i) {
            xs[i] = s.latency[i].t - s.now;
            tot[i] = s.latency[i].total;
        }
        if (ImPlot::BeginPlot("Latency (ms)", ImVec2(-1, plotH), ImPlotFlags_NoMouseText)) {
            ImPlot::SetupAxes(nullptr, nullptr, 0, n ? ImPlotAxisFlags_AutoFit : 0);
            if (!n) ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, -60, 0, ImPlotCond_Always);
            ImPlot::SetupAxisFormat(ImAxis_X1, "%.0f s");
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
            std::fill(lo.begin(), lo.end(), 0.0);
            for (int k = 0; k < 6; ++k) {
                for (int i = 0; i < n; ++i) hi[i] = lo[i] + std::max(0.0f, stageValue(s.latency[i], k));
                ImPlotSpec spec;
                spec.FillColor = kStages[k].color;
                spec.FillAlpha = 0.75f;
                if (n) ImPlot::PlotShaded(kStages[k].name, xs.data(), lo.data(), hi.data(), n, spec);
                lo = hi;
            }
            ImPlotSpec ls;
            ls.LineColor = ImVec4(1, 1, 1, 0.9f);
            ls.LineWeight = 1.5f * scale;
            if (n) ImPlot::PlotLine("Total", xs.data(), tot.data(), n, ls);
            ImPlot::EndPlot();
        }

        // fps / bandwidth history
        const int m = (int)s.rates.size();
        std::vector<double> rx(m), fps(m), mb(m);
        for (int i = 0; i < m; ++i) {
            rx[i] = s.rates[i].t - s.now;
            fps[i] = s.rates[i].fps;
            mb[i] = s.rates[i].mbps;
        }
        if (ImPlot::BeginPlot("Frames and bandwidth", ImVec2(-1, plotH), ImPlotFlags_NoMouseText)) {
            ImPlot::SetupAxis(ImAxis_X1, nullptr);
            ImPlot::SetupAxisLimits(ImAxis_X1, -60, 0, ImPlotCond_Always);
            ImPlot::SetupAxisFormat(ImAxis_X1, "%.0f s");
            ImPlot::SetupAxis(ImAxis_Y1, "fps", ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "Mbps", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
            ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, 1000);
            ImPlot::SetupAxisLimitsConstraints(ImAxis_Y2, 0, 10000);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
            ImPlotSpec a;
            a.LineColor = kAccent;
            a.LineWeight = 1.5f * scale;
            if (m) ImPlot::PlotLine("fps (average per screen)", rx.data(), fps.data(), m, a);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlotSpec b;
            b.LineColor = kGood;
            b.LineWeight = 1.5f * scale;
            if (m) ImPlot::PlotLine("Mbps (total)", rx.data(), mb.data(), m, b);
            ImPlot::EndPlot();
        }

        // per-stream table
        if (ImGui::BeginTable("streams", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Screen");
            ImGui::TableSetupColumn("Resolution");
            ImGui::TableSetupColumn("Encoder", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("fps");
            ImGui::TableSetupColumn("Mbps");
            ImGui::TableSetupColumn("Dropped PC / headset", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableHeadersRow();
            for (auto& sl : s.streams) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", sl.monitor + 1);
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d", sl.width, sl.height);
                ImGui::TableNextColumn();
                ImGui::Text("%s%s", sl.encoder.c_str(), sl.hardware ? "" : " (software)");
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", sl.fps);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", sl.mbps);
                ImGui::TableNextColumn();
                ImGui::Text("%u / %u", sl.serverDrops, sl.clientDrops);
            }
            if (s.streams.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }

        // log
        ImGui::SeparatorText("Log");
        ImGui::BeginChild("log", ImVec2(-1, -1), ImGuiChildFlags_Borders);
        uint64_t v = logVersion();
        for (auto& l : logLines()) {
            ImGui::TextDisabled("%s", l.time.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(l.text.c_str());
        }
        if (v != lastLogVersion) {
            ImGui::SetScrollHereY(1.0f);
            lastLogVersion = v;
        }
        ImGui::EndChild();
    }

    void frame() {
        int64_t now = SDL_GetTicks();
        if (now - lastStatus > 250) {
            st = server.status();
            lastStatus = now;
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("MirRoar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // header
        ImGui::PushFont(titleFont, 26.0f);
        ImGui::TextUnformatted("MirRoar");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("your PC desktop on Meta Quest");
        std::string right = "PC: " + cfg.name + "     port " + std::to_string(cfg.port);
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("%s", right.c_str());
        ImGui::Spacing();

        if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed, 380 * scale);
            ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginChild("leftc", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None);
            leftPanel();
            ImGui::EndChild();
            ImGui::TableNextColumn();
            ImGui::BeginChild("rightc", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None);
            rightPanel();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::End();
    }
};

void SDLCALL onTrayOpen(void* user, SDL_TrayEntry*) { ((Gui*)user)->showRequested = true; }
void SDLCALL onTrayQuit(void* user, SDL_TrayEntry*) { ((Gui*)user)->quit = true; }

}  // namespace

int runGui(Server& server, const std::string& cfgPath, bool startHidden) {
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        logf("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (startHidden ? SDL_WINDOW_HIDDEN : 0);
    SDL_Window* win = SDL_CreateWindow("MirRoar Server", (int)(1180 * scale), (int)(780 * scale), flags);
    if (!win) {
        logf("Cannot create the window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, (int)(900 * scale), (int)(600 * scale));
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderVSync(ren, 1);
    SDL_Surface* icon = makeIcon(64);
    if (icon) SDL_SetWindowIcon(win, icon);

    Gui gui(server);
    gui.cfgPath = cfgPath;
    gui.cfg = server.config();
    gui.encoders = listVideoEncoders();
    gui.autostart = autostartEnabled();
    gui.visible = !startHidden;
    gui.scale = scale;
    strncpy(gui.nameBuf, gui.cfg.name.c_str(), sizeof(gui.nameBuf) - 1);

    SDL_Surface* trayIcon = makeIcon(32);
    SDL_Tray* tray = SDL_CreateTray(trayIcon, "MirRoar Server");
    if (tray) {
        SDL_TrayMenu* menu = SDL_CreateTrayMenu(tray);
        SDL_TrayEntry* open = SDL_InsertTrayEntryAt(menu, -1, "Open MirRoar", SDL_TRAYENTRY_BUTTON);
        SDL_SetTrayEntryCallback(open, onTrayOpen, &gui);
        SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);  // separator
        SDL_TrayEntry* quit = SDL_InsertTrayEntryAt(menu, -1, "Quit", SDL_TRAYENTRY_BUTTON);
        SDL_SetTrayEntryCallback(quit, onTrayQuit, &gui);
    } else if (startHidden) {
        SDL_ShowWindow(win);
        gui.visible = true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const char* fontPaths[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    const char* boldPaths[] = {"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
    ImFont* body = nullptr;
    for (auto p : fontPaths)
        if (!body && SDL_GetPathInfo(p, nullptr)) body = io.Fonts->AddFontFromFileTTF(p, 16.0f);
    if (!body) body = io.Fonts->AddFontDefault();
    for (auto p : boldPaths)
        if (!gui.titleFont && SDL_GetPathInfo(p, nullptr)) {
            gui.titleFont = io.Fonts->AddFontFromFileTTF(p, 26.0f);
            gui.bigFont = io.Fonts->AddFontFromFileTTF(p, 40.0f);
        }
    if (!gui.titleFont) gui.titleFont = gui.bigFont = body;
    io.FontDefault = body;
    applyTheme(scale);

    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    while (!gui.quit) {
        SDL_Event e;
        bool got = gui.visible ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 250);
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) gui.quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)) {
                if (tray) {
                    SDL_HideWindow(win);
                    gui.visible = false;
                } else {
                    gui.quit = true;
                }
            }
            got = SDL_PollEvent(&e);
        }
        if (gui.showRequested) {
            gui.showRequested = false;
            SDL_ShowWindow(win);
            SDL_RestoreWindow(win);
            SDL_RaiseWindow(win);
            gui.visible = true;
        }
        if (!gui.visible || (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(gui.visible ? 50 : 0);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        gui.frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 19, 21, 27, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (tray) SDL_DestroyTray(tray);
    if (trayIcon) SDL_DestroySurface(trayIcon);
    if (icon) SDL_DestroySurface(icon);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
