#pragma once

namespace hyoshi::detail
{

// Logs the failure, flushes the log, and aborts.
[[noreturn]] void AssertFailed(const char* expression, const char* file, int line, const char* message = nullptr);

} // namespace hyoshi::detail

// Checked in every build, including release. Takes an optional message string.
#define HYOSHI_VERIFY(condition, ...)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition)) [[unlikely]]                                                                                 \
        {                                                                                                              \
            ::hyoshi::detail::AssertFailed(#condition, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__);                 \
        }                                                                                                              \
    } while (false)

// Checked in debug builds only, for programmer errors. The condition is not evaluated in release.
#if defined(NDEBUG)
#define HYOSHI_ASSERT(condition, ...)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        (void)sizeof(condition);                                                                                       \
    } while (false)
#else
#define HYOSHI_ASSERT(condition, ...) HYOSHI_VERIFY(condition __VA_OPT__(, ) __VA_ARGS__)
#endif
