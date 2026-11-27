#include "input_win.h"

void WinInput::mouseMove(int monitor, float nx, float ny) {
    if (monitor < 0 || monitor >= (int)mons_.size()) return;
    const RECT& mon_ = mons_[monitor];
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    int w = mon_.right - mon_.left, h = mon_.bottom - mon_.top;
    double px = mon_.left + nx * (w - 1);
    double py = mon_.top + ny * (h - 1);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((px - vx) * 65535.0 / (vw - 1) + 0.5);
    in.mi.dy = (LONG)((py - vy) * 65535.0 / (vh - 1) + 0.5);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

void WinInput::mouseButton(int button, bool down) {
    static const DWORD downFlags[3] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
    static const DWORD upFlags[3] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
    if (button < 0 || button > 2) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? downFlags[button] : upFlags[button];
    SendInput(1, &in, sizeof(in));
    buttons_[button] = down;
}

void WinInput::mouseWheel(int dx, int dy) {
    INPUT in[2]{};
    int n = 0;
    if (dy) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[n].mi.mouseData = (DWORD)dy;
        ++n;
    }
    if (dx) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in[n].mi.mouseData = (DWORD)dx;
        ++n;
    }
    if (n) SendInput(n, in, sizeof(INPUT));
}

static bool isExtendedVk(unsigned vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_NUMLOCK: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

void WinInput::key(unsigned vk, bool down, bool extended) {
    if (vk == 0 || vk > 255) return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | ((extended || isExtendedVk(vk)) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
    keys_[vk] = down;
}

bool WinInput::modifiersHeld() const {
    return keys_[VK_LCONTROL] || keys_[VK_RCONTROL] || keys_[VK_CONTROL] || keys_[VK_LMENU] || keys_[VK_RMENU] ||
           keys_[VK_MENU] || keys_[VK_LWIN] || keys_[VK_RWIN];
}

void WinInput::text(const std::string& utf8) {
    std::wstring w = utf8ToWide(utf8);
    for (wchar_t c : w) {
        // With Ctrl/Alt/Win held, unicode injection would ignore them: type the real key instead (Ctrl+C...).
        if (modifiersHeld()) {
            SHORT r = VkKeyScanW(c);
            if (r != -1) {
                unsigned vk = LOBYTE(r);
                bool needShift = (HIBYTE(r) & 1) && !keys_[VK_LSHIFT] && !keys_[VK_SHIFT];
                if (needShift) key(VK_LSHIFT, true, false);
                key(vk, true, false);
                key(vk, false, false);
                if (needShift) key(VK_LSHIFT, false, false);
                continue;
            }
        }
        if (c == L'\n' || c == L'\r') {
            key(VK_RETURN, true, false);
            key(VK_RETURN, false, false);
            continue;
        }
        if (c == L'\t') {
            key(VK_TAB, true, false);
            key(VK_TAB, false, false);
            continue;
        }
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = (WORD)c;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

void WinInput::releaseAll() {
    for (int b = 0; b < 3; ++b)
        if (buttons_[b]) mouseButton(b, false);
    for (int k = 0; k < 256; ++k)
        if (keys_[k]) key(k, false, false);
}

std::unique_ptr<InputInjector> createInputInjector() { return std::make_unique<WinInput>(); }
