// MirRoar server - logging to console and to an in-memory ring buffer shown by the GUI.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

void logf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void logEnableConsole(bool on);

struct LogLine {
    std::string time;
    std::string text;
};
std::vector<LogLine> logLines();
uint64_t logVersion();  // increments on every new line
