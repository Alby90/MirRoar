# Third-party components

The repository contains only MirRoar's own code. The components below are downloaded by the build scripts into `third_party/` (not committed) and end up in the built binaries. Anyone redistributing the binaries must also keep these notices.

| Component | Used in | License |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | server GUI (window, renderer, tray icon) | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | server GUI | MIT |
| [ImPlot](https://github.com/epezent/implot) | server GUI charts | MIT |
| [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (loader and headers, `openxr_loader_for_android`) | Quest app | Apache-2.0 OR MIT |
| Android NDK `native_app_glue` | Quest app | Apache-2.0 |

System libraries of Windows (Direct3D 11, DXGI, Media Foundation, WASAPI) and Android (MediaCodec, AAudio, EGL/GLES) are used through their public APIs and are not redistributed.

"Meta Quest" is a trademark of Meta Platforms, Inc. MirRoar is an independent project and is not affiliated with or endorsed by Meta.
