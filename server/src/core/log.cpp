#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace {
std::mutex g_m;
std::deque<LogLine> g_lines;
std::atomic<uint64_t> g_version{0};
bool g_console = true;
}  // namespace

void logEnableConsole(bool on) { g_console = on; }

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char ts[16];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    std::lock_guard<std::mutex> lk(g_m);
    if (g_console) {
        printf("[%s] %s\n", ts, buf);
        fflush(stdout);
    }
    g_lines.push_back({ts, buf});
    while (g_lines.size() > 400) g_lines.pop_front();
    ++g_version;
}

std::vector<LogLine> logLines() {
    std::lock_guard<std::mutex> lk(g_m);
    return std::vector<LogLine>(g_lines.begin(), g_lines.end());
}

uint64_t logVersion() { return g_version; }
