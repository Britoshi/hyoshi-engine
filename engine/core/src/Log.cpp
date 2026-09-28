#include "core/Log.h"

#include <memory>
#include <utility>

#if defined(__ANDROID__)
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace hyoshi::log
{

void Initialize()
{
#if defined(__ANDROID__)
    // Logcat adds its own timestamp and level.
    auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("Hyoshi");
    sink->set_pattern("%v");
#else
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
#endif

    auto logger = std::make_shared<spdlog::logger>("hyoshi", std::move(sink));

    // Filtering happens at compile time through SPDLOG_ACTIVE_LEVEL.
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(std::move(logger));
}

void Shutdown()
{
    spdlog::shutdown();
}

} // namespace hyoshi::log
