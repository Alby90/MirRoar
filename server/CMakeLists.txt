cmake_minimum_required(VERSION 3.16)
project(MirRoarServer CXX C RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
set(IMGUI ${TP}/imgui)
set(IMPLOT ${TP}/implot)
list(APPEND CMAKE_PREFIX_PATH ${TP}/sdl-install)
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)

set(CORE_SOURCES
  src/core/log.cpp
  src/core/config.cpp
  src/core/stats.cpp
  src/core/session.cpp
  src/core/server.cpp
)
set(GUI_SOURCES
  src/gui/gui.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp
  ${IMPLOT}/implot.cpp ${IMPLOT}/implot_items.cpp
)

if(WIN32)
  set(PLATFORM_SOURCES
    src/platform/win/system_win.cpp
    src/platform/win/capture_dxgi.cpp
    src/platform/win/encoder_mf.cpp
    src/platform/win/input_win.cpp
    src/platform/win/audio_wasapi.cpp
    res/server.rc
  )
  set(PLATFORM_LIBS d3d11 dxgi mfplat mfuuid mf strmiids ole32 oleaut32 uuid ws2_32 iphlpapi winmm ksuser shell32 advapi32)
else()
  message(FATAL_ERROR "Linux platform layer not implemented yet (src/platform/linux)")
endif()

# third-party UI code: optimize for size
set_source_files_properties(${GUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-Os")

add_executable(MirRoarServer WIN32 src/main.cpp ${CORE_SOURCES} ${GUI_SOURCES} ${PLATFORM_SOURCES})
target_include_directories(MirRoarServer PRIVATE ${IMGUI} ${IMGUI}/backends ${IMPLOT})
target_compile_definitions(MirRoarServer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  "IMPLOT_CUSTOM_NUMERIC_TYPES=(double)")
target_link_libraries(MirRoarServer PRIVATE SDL3::SDL3-static ${PLATFORM_LIBS})

if(MINGW)
  # Single self-contained executable: no MinGW runtime DLLs needed.
  target_link_options(MirRoarServer PRIVATE -static -static-libgcc -static-libstdc++ -s -Wl,--gc-sections)
  target_compile_options(MirRoarServer PRIVATE -O2 -ffunction-sections -fdata-sections)
endif()
