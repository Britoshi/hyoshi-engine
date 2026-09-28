#include "core/Env.h"

#include "core/Log.h"

#include <cstdlib>

namespace hyoshi
{

std::optional<double> ReadEnvNumber(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const double number = std::strtod(value, &end);
    if (end == value)
    {
        HYOSHI_LOG_WARN("Ignoring {}='{}'", name, value);
        return std::nullopt;
    }
    return number;
}

std::optional<long long> ReadEnvPositiveInt(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const long long number = std::strtoll(value, &end, 10);
    if (end == value || number <= 0)
    {
        HYOSHI_LOG_WARN("Ignoring {}='{}'", name, value);
        return std::nullopt;
    }
    return number;
}

bool IsEnvSet(const char* name)
{
    return std::getenv(name) != nullptr;
}

} // namespace hyoshi
