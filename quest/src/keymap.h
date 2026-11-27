// MirRoar Quest client - Android keycode -> Windows virtual-key mapping.
#pragma once
#include <android/keycodes.h>
#include <stdint.h>

// Returns 0 if the key has no Windows equivalent.
inline uint16_t androidKeyToVk(int k) {
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return (uint16_t)('A' + (k - AKEYCODE_A));
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return (uint16_t)('0' + (k - AKEYCODE_0));
    if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) return (uint16_t)(0x70 + (k - AKEYCODE_F1));
    if (k >= AKEYCODE_NUMPAD_0 && k <= AKEYCODE_NUMPAD_9) return (uint16_t)(0x60 + (k - AKEYCODE_NUMPAD_0));
    switch (k) {
        case AKEYCODE_DEL: return 0x08;          // VK_BACK
        case AKEYCODE_FORWARD_DEL: return 0x2E;  // VK_DELETE
        case AKEYCODE_TAB: return 0x09;
        case AKEYCODE_ENTER: return 0x0D;
        case AKEYCODE_NUMPAD_ENTER: return 0x0D;
        case AKEYCODE_SPACE: return 0x20;
        case AKEYCODE_ESCAPE: return 0x1B;
        case AKEYCODE_DPAD_LEFT: return 0x25;
        case AKEYCODE_DPAD_UP: return 0x26;
        case AKEYCODE_DPAD_RIGHT: return 0x27;
        case AKEYCODE_DPAD_DOWN: return 0x28;
        case AKEYCODE_PAGE_UP: return 0x21;
        case AKEYCODE_PAGE_DOWN: return 0x22;
        case AKEYCODE_MOVE_HOME: return 0x24;
        case AKEYCODE_MOVE_END: return 0x23;
        case AKEYCODE_INSERT: return 0x2D;
        case AKEYCODE_SHIFT_LEFT: return 0xA0;
        case AKEYCODE_SHIFT_RIGHT: return 0xA1;
        case AKEYCODE_CTRL_LEFT: return 0xA2;
        case AKEYCODE_CTRL_RIGHT: return 0xA3;
        case AKEYCODE_ALT_LEFT: return 0xA4;
        case AKEYCODE_ALT_RIGHT: return 0xA5;
        case AKEYCODE_META_LEFT: return 0x5B;
        case AKEYCODE_META_RIGHT: return 0x5C;
        case AKEYCODE_CAPS_LOCK: return 0x14;
        case AKEYCODE_SCROLL_LOCK: return 0x91;
        case AKEYCODE_NUM_LOCK: return 0x90;
        case AKEYCODE_SYSRQ: return 0x2C;
        case AKEYCODE_BREAK: return 0x13;
        case AKEYCODE_MENU: return 0x5D;
        case AKEYCODE_SEMICOLON: return 0xBA;
        case AKEYCODE_EQUALS: return 0xBB;
        case AKEYCODE_COMMA: return 0xBC;
        case AKEYCODE_MINUS: return 0xBD;
        case AKEYCODE_PERIOD: return 0xBE;
        case AKEYCODE_SLASH: return 0xBF;
        case AKEYCODE_GRAVE: return 0xC0;
        case AKEYCODE_LEFT_BRACKET: return 0xDB;
        case AKEYCODE_BACKSLASH: return 0xDC;
        case AKEYCODE_RIGHT_BRACKET: return 0xDD;
        case AKEYCODE_APOSTROPHE: return 0xDE;
        case AKEYCODE_NUMPAD_DIVIDE: return 0x6F;
        case AKEYCODE_NUMPAD_MULTIPLY: return 0x6A;
        case AKEYCODE_NUMPAD_SUBTRACT: return 0x6D;
        case AKEYCODE_NUMPAD_ADD: return 0x6B;
        case AKEYCODE_NUMPAD_DOT: return 0x6E;
        case AKEYCODE_VOLUME_MUTE: return 0xAD;
        case AKEYCODE_MEDIA_PLAY_PAUSE: return 0xB3;
        case AKEYCODE_MEDIA_NEXT: return 0xB0;
        case AKEYCODE_MEDIA_PREVIOUS: return 0xB1;
        default: return 0;
    }
}
