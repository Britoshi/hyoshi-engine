#include "osu/OsuInfo.h"

#include "OsuText.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace hyoshi::osu
{

namespace
{

using detail::ParseNumber;
using detail::Trim;

constexpr int SPINNER_BIT = 8;
constexpr int HOLD_BIT = 128;

SongTimeUs MsToUs(double milliseconds)
{
    return static_cast<SongTimeUs>(std::llround(milliseconds * 1000.0));
}

// The field after `count` commas, up to the next comma.
std::string_view GetField(std::string_view line, size_t count)
{
    size_t start = 0;
    for (size_t i = 0; i < count; ++i)
    {
        start = line.find(',', start);
        if (start == std::string_view::npos)
        {
            return {};
        }
        ++start;
    }
    return Trim(line.substr(start, line.find(',', start) - start));
}

} // namespace

Result<OsuInfo> ReadOsuInfo(std::string_view text)
{
    OsuInfo info;
    std::string_view section;
    bool isFirstLine = true;
    while (!text.empty())
    {
        const size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = Trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (isFirstLine && line.starts_with("\xEF\xBB\xBF"))
        {
            line.remove_prefix(3);
        }
        isFirstLine = false;
        if (line.empty() || line.starts_with("//"))
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        if (section == "General" || section == "Metadata" || section == "Difficulty")
        {
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key = Trim(line.substr(0, colon));
            const std::string_view value = Trim(line.substr(colon + 1));
            if (key == "AudioFilename")
            {
                info.AudioFile = value;
            }
            else if (key == "PreviewTime")
            {
                const double preview = ParseNumber(value).value_or(-1.0);
                info.Metadata.PreviewStartUs = preview < 0.0 ? 0 : MsToUs(preview);
            }
            else if (key == "Mode")
            {
                info.Mode = static_cast<int>(ParseNumber(value).value_or(0.0));
            }
            else if (key == "Title")
            {
                info.Metadata.Title = value;
            }
            else if (key == "Artist")
            {
                info.Metadata.Artist = value;
            }
            else if (key == "Creator")
            {
                info.Metadata.Charter = value;
            }
            else if (key == "Version")
            {
                info.Metadata.DifficultyName = value;
            }
            else if (key == "Tags")
            {
                info.Tags = value;
            }
            else if (key == "Source")
            {
                info.Source = value;
            }
            else if (key == "CircleSize")
            {
                info.KeyCount = static_cast<int>(std::lround(ParseNumber(value).value_or(0.0)));
            }
            else if (key == "OverallDifficulty")
            {
                info.Metadata.OverallDifficulty = ParseNumber(value);
            }
        }
        else if (section == "Events" && info.BackgroundFile.empty())
        {
            // Background: 0,0,"file",x,y
            const std::string_view kind = GetField(line, 0);
            std::string_view file = GetField(line, 2);
            if ((kind == "0" || kind == "Background") && !file.empty())
            {
                if (file.size() >= 2 && file.front() == '"' && file.back() == '"')
                {
                    file = file.substr(1, file.size() - 2);
                }
                info.BackgroundFile = file;
            }
        }
        else if (section == "TimingPoints" && info.Bpm == 0.0)
        {
            const std::optional<double> beatLength = ParseNumber(GetField(line, 1));
            if (beatLength && *beatLength > 0.0)
            {
                info.Bpm = 60'000.0 / *beatLength;
            }
        }
        else if (section == "HitObjects")
        {
            // x,y,time,type,hitSound,...: a spinner's end is the next field, a hold's before a colon.
            const std::optional<double> time = ParseNumber(GetField(line, 2));
            const std::optional<double> type = ParseNumber(GetField(line, 3));
            if (!time || !type)
            {
                continue;
            }
            ++info.HitObjectCount;
            double endMs = *time;
            const int bits = static_cast<int>(*type);
            if ((bits & (SPINNER_BIT | HOLD_BIT)) != 0)
            {
                const std::string_view field = GetField(line, 5);
                endMs = std::max(endMs, ParseNumber(field.substr(0, field.find(':'))).value_or(endMs));
            }
            info.EndUs = std::max(info.EndUs, MsToUs(endMs));
        }
    }
    if (info.HitObjectCount == 0)
    {
        return Error{"The beatmap has no hit objects"};
    }
    // As ImportOsuStandard sets it.
    if (info.Mode == 0)
    {
        info.Metadata.DifficultyValue = info.Metadata.OverallDifficulty.value_or(0.0);
    }
    return info;
}

} // namespace hyoshi::osu
