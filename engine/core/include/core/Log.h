#pragma once

#include <spdlog/spdlog.h>

namespace hyoshi::log
{

// Creates the engine logger and makes it the default: terminal on desktop, logcat on Android.
void Initialize();

// Flushes and releases all loggers.
void Shutdown();

} // namespace hyoshi::log

// Format strings use fmt syntax: HYOSHI_LOG_INFO("Loaded {} notes", count).
#define HYOSHI_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define HYOSHI_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define HYOSHI_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define HYOSHI_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define HYOSHI_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define HYOSHI_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
