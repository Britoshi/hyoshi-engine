#include "core/Assert.h"

#include "core/Log.h"

#include <cstdlib>

namespace hyoshi::detail
{

void AssertFailed(const char* expression, const char* file, int line, const char* message)
{
    if (message != nullptr)
    {
        HYOSHI_LOG_CRITICAL("Assertion failed: {} ({}) at {}:{}", expression, message, file, line);
    }
    else
    {
        HYOSHI_LOG_CRITICAL("Assertion failed: {} at {}:{}", expression, file, line);
    }

    spdlog::default_logger_raw()->flush();
    std::abort();
}

} // namespace hyoshi::detail
