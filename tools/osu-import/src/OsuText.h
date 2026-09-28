#pragma once

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reading .osu text, shared by the importers.
namespace hyoshi::osu::detail
{

inline std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

inline std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true)
    {
        const size_t end = text.find(separator, start);
        parts.push_back(Trim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)));
        if (end == std::string_view::npos)
        {
            return parts;
        }
        start = end + 1;
    }
}

inline std::optional<double> ParseNumber(std::string_view text)
{
    double value = 0.0;
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000
    // libc++ before LLVM 20 (the Android NDK's) has no floating-point from_chars. strtod takes the
    // same numbers once its extras (leading spaces, '+', hex) are refused, and nothing in the game
    // changes the C locale, so the decimal point stays '.'.
    if (text.empty() || text.front() == '+' || std::isspace(static_cast<unsigned char>(text.front())) != 0 ||
        text.find_first_of("xX") != std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string terminated(text);
    char* end = nullptr;
    errno = 0;
    value = std::strtod(terminated.c_str(), &end);
    if (errno == ERANGE || end != terminated.c_str() + terminated.size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
#else
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
#endif
    return value;
}

} // namespace hyoshi::osu::detail
