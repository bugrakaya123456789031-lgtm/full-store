// logger.h — simple thread-safe log sink.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace fs_log {

enum class Level { DEBUG, INFO, WARN, ERROR };

void init(const std::string& path);
void shutdown();
void log(Level lvl, const char* fmt, ...);

#define LOGD(...) fs_log::log(fs_log::Level::DEBUG, __VA_ARGS__)
#define LOGI(...) fs_log::log(fs_log::Level::INFO,  __VA_ARGS__)
#define LOGW(...) fs_log::log(fs_log::Level::WARN,  __VA_ARGS__)
#define LOGE(...) fs_log::log(fs_log::Level::ERROR, __VA_ARGS__)

} // namespace fs_log
