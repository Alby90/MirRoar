// MirRoar - headset options, shared by the Quest app (which owns and stores them) and the PC
// server (which can show and edit them while the headset is connected).
// To add an option: add an id to OptionId and a row to kOptions, then read opt[OPT_x] in the app.
// On the wire (C_OPTIONS / S_OPTIONS) options are UTF-8 text lines "key=value", so both sides
// ignore keys they don't know and keep defaults for keys they don't receive.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace mirroar {

enum OptionId {
    OPT_DESK_HOLD,
    OPT_DESK_STILL,
    OPT_DESK_MARGIN,
    OPT_DESK_DEPTH,
    OPT_DESK_LIFT,
    OPT_DESK_FORWARD,
    OPT_KEYS_POINTER_OFF,
    OPT_KEYS_HEIGHT,
    OPT_KEYS_MARGIN,
    OPT_KEYS_RELEASE,
    OPT_COUNT
};

struct OptionDef {
    const char* key;    // stable identifier (settings file and wire)
    const char* group;  // section title
    const char* label;
    float def, min, max, step;  // in internal units (meters, seconds)
    float shown;        // display = value * shown
    const char* unit;   // display unit; nullptr = on/off switch
    int decimals;       // display decimals
};

constexpr OptionDef kOptions[OPT_COUNT] = {
    {"desk_hold", "Desk placement with both hands", "Hold still to confirm", 2.0f, 0.5f, 5.0f, 0.25f, 1, "s", 2},
    {"desk_still", "Desk placement with both hands", "Stillness tolerance", 0.025f, 0.01f, 0.06f, 0.005f, 100, "cm", 1},
    {"desk_margin", "Desk placement with both hands", "Extra width around the hands", 0.12f, 0.0f, 0.4f, 0.01f, 100, "cm", 0},
    {"desk_depth", "Desk placement with both hands", "Window depth", 0.40f, 0.2f, 1.0f, 0.02f, 100, "cm", 0},
    {"desk_lift", "Desk placement with both hands", "Keys below the palms", 0.03f, 0.0f, 0.08f, 0.005f, 100, "cm", 1},
    {"desk_forward", "Desk placement with both hands", "Shift towards the fingers", 0.05f, -0.05f, 0.15f, 0.01f, 100, "cm", 0},
    {"keys_pointer_off", "Typing with tracked hands", "Pointer off while the hands are on the keyboard", 1, 0, 1, 1, 1, nullptr, 0},
    {"keys_height", "Typing with tracked hands", "Pointer-off height above the keys", 0.15f, 0.05f, 0.35f, 0.01f, 100, "cm", 0},
    {"keys_margin", "Typing with tracked hands", "Pointer-off margin around the window", 0.03f, 0.0f, 0.15f, 0.01f, 100, "cm", 0},
    {"keys_release", "Typing with tracked hands", "Pointer back after leaving", 0.25f, 0.0f, 1.0f, 0.05f, 1000, "ms", 0},
};

inline void defaultOptions(float* v) {
    for (int i = 0; i < OPT_COUNT; ++i) v[i] = kOptions[i].def;
}

inline float clampOption(int i, float x) {
    const OptionDef& d = kOptions[i];
    if (!(x == x)) return d.def;  // NaN
    if (d.step > 0) x = d.min + (float)(long)((x - d.min) / d.step + (x >= d.min ? 0.5f : -0.5f)) * d.step;
    return x < d.min ? d.min : (x > d.max ? d.max : x);
}

inline int findOption(const char* key, size_t len) {
    for (int i = 0; i < OPT_COUNT; ++i)
        if (strlen(kOptions[i].key) == len && memcmp(kOptions[i].key, key, len) == 0) return i;
    return -1;
}

inline std::string encodeOptions(const float* v) {
    std::string s;
    char line[96];
    for (int i = 0; i < OPT_COUNT; ++i) {
        snprintf(line, sizeof(line), "%s=%g\n", kOptions[i].key, v[i]);
        s += line;
    }
    return s;
}

// Updates v[] with the known keys found in `text`; returns how many were set.
inline int decodeOptions(const char* text, size_t len, float* v) {
    int n = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') ++eol;
        size_t eq = pos;
        while (eq < eol && text[eq] != '=') ++eq;
        if (eq < eol) {
            int id = findOption(text + pos, eq - pos);
            if (id >= 0) {
                std::string num(text + eq + 1, eol - eq - 1);
                v[id] = clampOption(id, (float)strtod(num.c_str(), nullptr));
                ++n;
            }
        }
        pos = eol + 1;
    }
    return n;
}

// "2.25 s", "3.5 cm", "On"
inline std::string formatOption(int i, float x) {
    const OptionDef& d = kOptions[i];
    if (!d.unit) return x >= 0.5f ? "On" : "Off";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.*f %s", d.decimals, x * d.shown, d.unit);
    return buf;
}

}  // namespace mirroar
