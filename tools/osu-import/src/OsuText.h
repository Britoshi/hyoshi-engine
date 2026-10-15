#pragma once

#include "core/Time.h"
#include "rhythm/Chart.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

// Whether a timing point's effects (its eighth field) turn kiai time on: bit 0.
inline bool IsKiaiOn(const std::vector<std::string_view>& fields)
{
    return fields.size() > 7 && (static_cast<int64_t>(ParseNumber(fields[7]).value_or(0.0)) & 1) != 0;
}

// osu!'s kiai time, from every timing point (red and green lines alike) in the order the import
// sorted them, each a time and whether it turns kiai on: on from a point that turns it on to the
// next that doesn't. Of points at the same time, the last counts (as osu!lazer's replace each
// other). A section still on after the last point runs to `endUs`.
inline std::vector<rhythm::KiaiSection> MakeKiaiSections(const std::vector<std::pair<SongTimeUs, bool>>& points,
                                                         SongTimeUs endUs)
{
    std::vector<rhythm::KiaiSection> sections;
    std::optional<SongTimeUs> start;
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (i + 1 < points.size() && points[i + 1].first == points[i].first)
        {
            continue;
        }
        const auto [time, isOn] = points[i];
        if (isOn && !start)
        {
            start = time;
        }
        else if (!isOn && start)
        {
            if (time > *start)
            {
                sections.push_back({*start, time});
            }
            start.reset();
        }
    }
    if (start && endUs > *start)
    {
        sections.push_back({*start, endUs});
    }
    return sections;
}

} // namespace hyoshi::osu::detail
