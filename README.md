# MirRoar

Your PC desktop inside **Meta Quest 3**: giant virtual screens in a virtual environment or in passthrough, controlled with the Touch controllers or your hands, with the Quest system keyboard typing on the PC.

- **PC server**: one small self-contained Windows executable (~3 MB, no installer, no runtime) with a control panel.
- **Quest app**: native OpenXR app (~0.8 MB).
- **Connection**: Wi-Fi, Ethernet (same LAN) or USB cable. The headset finds the PC automatically.

## Features

- Hardware H.264 streaming of one or **all your monitors** (side by side, in Windows' layout), switchable from the headset or from the PC.
- Screens are native compositor layers (flat or **curved**) with super-sampling and sharpening for crisp text.
- **Controllers or hand tracking** as mouse: click, right click, drag, scroll; grab handles to move/resize screens.
- **Quest system keyboard** typing on the PC (accents and emoji included), a toolbar with Esc, Tab and sticky **Ctrl / Alt / Shift / Win** (Ctrl then `c` = Ctrl+C), and support for Bluetooth keyboards paired to the Quest.
- Environments: Studio, Space, Dark, or **passthrough**. In virtual environments a **passthrough desk window** lets you see your real keyboard and hands.
- PC audio streamed to the headset (WASAPI loopback, low-latency playback).
- **Latency measured end to end** (from the desktop grab on the PC to the moment the image is shown in the headset), split by stage and plotted live in the server control panel.

## Download

Ready-to-use builds are on the [Releases page](https://github.com/Alby90/MirRoar/releases): `MirRoarServer.exe` (Windows 10/11, 64-bit) and `MirRoar-Quest.apk` (Meta Quest 3 / 3S / Pro / 2).

## Usage

1. **PC**: run `MirRoarServer.exe`. The first time, Windows asks for firewall access: choose **Allow**, otherwise Wi-Fi will not work.
2. **Quest**: enable developer mode, install the APK once (`adb install -r MirRoar-Quest.apk`) and open **MirRoar** from *Library → Unknown sources*.
3. The headset finds the PC by itself:
   - **Wi-Fi / Ethernet**: PC and headset must be on the same local network. The PC announces itself with a UDP broadcast.
   - **USB cable**: if `adb.exe` is in the PATH or next to the server, the server sets up `adb reverse` automatically. The cable has priority over the network.

### Controls

| Controller | Hands | Action |
|---|---|---|
| Trigger | thumb + index pinch | left click (hold to drag) |
| A / X | thumb + middle pinch | right click |
| Thumbstick | thumb + ring pinch, move the hand up/down | scroll |
| Grip (hold) | pinch the **handle** under a screen | grab and move the screen (thumbstick: distance / size) |
| B / Y | *Keyboard* button | Quest keyboard (text is typed on the PC) |
| Right thumbstick click | *Flat/Curved* button | flat or curved screens |
| Left thumbstick click | *Recenter* button | bring the screens back in front of you |
| Menu (left) | *Environment* button | Studio → Space → Dark → Passthrough |

The trigger (or pinch) of the other hand makes it the active pointer.

**Toolbar** (under the main screen):

- **Esc**, **Tab**.
- **Ctrl / Alt / Shift / Win**: stay pressed until the next key or click. Pressing Win twice opens the Start menu.
- **Screen**: cycles Screen 1 → Screen 2 → … → all screens. The choice is remembered.
- **Environment**, **Flat/Curved**, **Recenter**.
- **Desk**: a passthrough window over your real keyboard, visible in virtual environments. Press *Desk*, hold the controller (or your hand) over the center of the keyboard and pull the trigger (or pinch) to drop it. While placing it, the thumbstick changes its width and depth. Press *Desk* again to remove it. It stays fixed in the room even when you recenter.

Position, size, environment and desk window are saved between sessions.

### Server control panel

Running `MirRoarServer.exe` opens the control panel. Closing the window keeps the server running in the system tray (right-click the icon: *Open MirRoar* / *Quit*).

- **Headset**: connection state (USB or Wi-Fi/LAN), ping, *Disconnect*, IP addresses of the PC.
- **Screens**: which monitors are shown in the headset (live), and which one at startup.
- **Video**: bitrate, frames per second, maximum resolution, encoder. Changes apply immediately.
- **Audio and USB cable**, PC name, port, *Start with Windows*, *Start hidden*.
- **Latency** split into capture, encode, queue, network, decode and display, with a 60-second chart, plus frames per second, bandwidth and dropped frames per screen.
- **Log** of events.

Settings are stored in `%APPDATA%\MirRoar\server.ini`.

### Command line

```
MirRoarServer --headless             console only, no window
MirRoarServer --minimized            start hidden in the system tray
MirRoarServer --list-monitors        list the monitors
MirRoarServer --monitor 2            start with the second monitor (--monitor all: every monitor)
MirRoarServer --bitrate 50 --fps 72  quality and smoothness
MirRoarServer --max-width 2560       limit the stream resolution
MirRoarServer --encoder intel        choose the encoder (nvidia, intel, amd, software)
MirRoarServer --no-audio / --no-adb
```

## Architecture

```
PC (MirRoarServer.exe, C++ / MinGW, static)             Quest 3 (C++ OpenXR + a small Java layer)
  DXGI Desktop Duplication (GPU), one per monitor          AMediaCodec H.264 (hardware), one per screen
  -> D3D11 Video Processor BGRA -> NV12                     -> AImageReader -> EGLImage (zero copy)
  -> Media Foundation H.264 (NVENC / QSV / AMF)             -> quad / cylinder compositor layers
  WASAPI loopback (PCM 48 kHz)             ---TCP--->       AAudio (low latency)
  SendInput (mouse, keys, Unicode text)    <--TCP----       controllers, hands, system keyboard (IME)
  UDP beacon 47801, TCP 47800, adb reverse                  XR_FB_passthrough (+ projected desk window)
```

- Shared wire protocol: [common/protocol.h](common/protocol.h)
- Server: [server/src/core](server/src/core) (portable: sessions, network, statistics), [server/src/platform](server/src/platform) (OS interfaces; Windows implementation in `platform/win`, Linux planned), [server/src/gui](server/src/gui) (Dear ImGui + ImPlot on SDL3)
- Quest app: [quest/src](quest/src), [MainActivity.java](quest/java/com/mirroar/app/MainActivity.java)

## Building

Requirements: MSYS2 MinGW-w64 (g++, cmake), JDK 17, Android SDK (platform 34, build-tools 34.0.0, NDK 27.2; set `ANDROID_SDK_ROOT`, default `D:/Android/sdk`), Python 3, curl.

```bash
bash server/build.sh          # -> dist/MirRoarServer.exe (first run downloads and builds SDL3, Dear ImGui, ImPlot)
bash quest/build.sh install   # -> dist/MirRoar-Quest.apk (first run downloads the OpenXR loader; "install" pushes it to the headset)
python tools/test_client.py   # desktop test client: handshake and H.264 stream check
```

The Quest build signs the APK with a local debug key (`quest/keystore/debug.keystore`, generated on first build and not part of the repository).

App log: `adb logcat -s MirRoar`.

## Known limitations

- Meta's *surface keyboard* (keyboard and touchpad on the desk) currently exists only in the Quest home: there is no API for apps.
- Automatic tracking of physical keyboards is not available in native OpenXR on the current runtime (`XR_FB_keyboard_tracking` is deprecated), so the desk window is placed by hand.
- The secure desktop (UAC prompts, login screen) cannot be captured, and input does not reach "run as administrator" windows unless the server runs as administrator.
- Video is H.264 over TCP: great on LAN/USB; on a congested Wi-Fi, frames are dropped and the stream restarts from a keyframe.

## License

MirRoar is released under the [MIT License](LICENSE): you can use, modify and redistribute it, also in other and commercial projects, as long as you keep the copyright notice crediting this project. Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
