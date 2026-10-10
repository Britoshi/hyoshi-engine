#pragma once

#include <string>
#include <vector>

namespace hyoshi::audio
{

// The ASIO drivers installed (Windows), by the names AudioConfig::AsioDriver takes. Empty elsewhere.
std::vector<std::string> ListAsioDrivers();

} // namespace hyoshi::audio
