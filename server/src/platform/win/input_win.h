// MirRoar server - mouse/keyboard injection via SendInput.
#pragma once
#include <string>
#include <vector>
#include "winutil.h"

class WinInput : public InputInjector {
public:
    void setMonitors(const std::vector<MonitorDesc>& m) override {
        mons_.clear();
        for (auto& d : m) mons_.push_back(RECT{d.x, d.y, d.x + d.width, d.y + d.height});
    }
    void mouseMove(int monitor, float nx, float ny) override;
    void mouseButton(int button, bool down) override;
    void mouseWheel(int dx, int dy) override;
    void key(unsigned vk, bool down, bool extended) override;
    void text(const std::string& utf8) override;
    void releaseAll() override;  // releases buttons/keys still held when the client disconnects

private:
    bool modifiersHeld() const;
    std::vector<RECT> mons_;
    bool buttons_[3] = {};
    bool keys_[256] = {};
};
