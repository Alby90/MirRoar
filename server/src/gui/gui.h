// MirRoar server - control panel (Dear ImGui + ImPlot on SDL3) with tray icon.
#pragma once
#include <string>
#include "../core/config.h"
#include "../core/server.h"

// Runs the GUI until the user quits. `cfgPath` is where settings are saved on change.
int runGui(Server& server, const std::string& cfgPath, bool startHidden);
