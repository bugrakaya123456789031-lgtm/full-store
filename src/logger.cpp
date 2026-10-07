// logger.cpp
#include "logger.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <mutex>
#include <string>

namespace fs_log {

namespace {
    std::mutex g_mtx;
    FILE*      g_file = nullptr;

    const char* level_tag(Level l) {
        switch (l) {
            case Level::DEBUG: return "DBG";
            case Level::INFO:  return "INF";
            case Level::WARN:  return "WRN";
            case Level::ERROR: return "ERR";
        }
        return "?";
    }

    std::string ts_now() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[48];
        std::snprintf(buf, sizeof(buf),
                      "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      (long long)ms.count());
        return buf;
    }
}

void init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) std::fclose(g_file);
    g_file = std::fopen(path.c_str(), "a");
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

void log(Level lvl, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::string line = "[" + ts_now() + "] " + level_tag(lvl) + "  " + buf + "\n";

    std::lock_guard<std::mutex> lk(g_mtx);
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
    if (g_file) {
        std::fputs(line.c_str(), g_file);
        std::fflush(g_file);
    }
}

} // namespace fs_log
