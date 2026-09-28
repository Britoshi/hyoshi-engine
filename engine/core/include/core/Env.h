#pragma once

#include <optional>

namespace hyoshi
{

// Development aids read from the environment, so scripts can drive an app. A value that doesn't
// parse is logged and ignored.
std::optional<double> ReadEnvNumber(const char* name);
std::optional<long long> ReadEnvPositiveInt(const char* name);
bool IsEnvSet(const char* name);

} // namespace hyoshi
