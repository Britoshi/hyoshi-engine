#include "osu/OsuManiaImporter.h"

#include "OsuText.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace hyoshi::osu
{

namespace
{

using detail::ParseNumber;
using detail::Split;
using detail::Trim;

constexpr int64_t OLD_FORMAT_OFFSET_US = 24'000;
constexpr int OLD_FORMAT_VERSION = 5;
constexpr int MANIA_MODE = 3;
constexpr int HOLD_TYPE_BIT = 128;
constexpr double PLAYFIELD_WIDTH = 512.0;
constexpr double MIN_SV = 0.1;
constexpr double MAX_SV = 10.0;

// osu! looks up a note's timing point this far after the note, so a point placed a hair late
// still applies (osu!lazer's control point leniency).
constexpr double TIMING_POINT_LENIENCY_MS = 1.0;

struct OsuTimingPoint
{
    double TimeMs = 0.0;
    double BeatLength = 0.0;
    int Meter = 4;
    // Hit sound volume, percent.
    int Volume = 100;
    bool IsUninherited = true;
};

struct OsuHitObject
{
    double X = 0.0;
    double TimeMs = 0.0;
    int Type = 0;
    std::optional<double> EndTimeMs;
    // From hitSample: a custom sound file, and its volume (0 means the timing point's).
    std::string SampleFile;
    int SampleVolume = 0;
};

// hitSample is normalSet:additionSet:index:volume:filename. Only the file matters here: notes
// without one have no sound of their own.
void ReadHitSample(std::string_view hitSample, OsuHitObject& object)
{
    size_t start = 0;
    for (int field = 0; field < 4; ++field)
    {
        const size_t colon = hitSample.find(':', start);
        if (colon == std::string_view::npos)
        {
            return;
        }
        if (field == 3)
        {
            object.SampleVolume =
                static_cast<int>(std::lround(ParseNumber(Trim(hitSample.substr(start, colon - start))).value_or(0.0)));
        }
        start = colon + 1;
    }
    object.SampleFile = std::string(Trim(hitSample.substr(start)));
}

SongTimeUs MsToUs(double milliseconds, int64_t offsetUs)
{
    return std::llround(milliseconds * 1000.0) + offsetUs;
}

// The beat length that covers the most time, which osu!mania scrolls at 1x.
double GetDominantBeatLength(const std::vector<OsuTimingPoint>& points, double endMs)
{
    std::map<double, double> durations;
    const OsuTimingPoint* previous = nullptr;
    for (const OsuTimingPoint& point : points)
    {
        if (!point.IsUninherited)
        {
            continue;
        }
        if (previous != nullptr)
        {
            durations[previous->BeatLength] += std::max(0.0, point.TimeMs - previous->TimeMs);
        }
        previous = &point;
    }
    if (previous == nullptr)
    {
        return 0.0;
    }
    durations[previous->BeatLength] += std::max(0.0, endMs - previous->TimeMs);

    return std::max_element(durations.begin(), durations.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; })
        ->first;
}

} // namespace

Result<OsuImportResult> ImportOsuMania(std::string_view osuText)
{
    OsuImportResult result;
    rhythm::Chart chart;
    chart.Mode = "mania";

    int formatVersion = 14;
    int mode = 0;
    std::optional<int> keyCount;
    std::vector<OsuTimingPoint> timingPoints;
    std::vector<OsuHitObject> hitObjects;

    std::string section;
    size_t lineNumber = 0;
    std::istringstream lines{std::string(osuText)};
    std::string rawLine;
    while (std::getline(lines, rawLine))
    {
        ++lineNumber;
        std::string_view line = Trim(rawLine);
        if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF"))
        {
            line.remove_prefix(3);
        }
        if (line.empty() || line.starts_with("//"))
        {
            continue;
        }

        if (line.starts_with("osu file format v"))
        {
            formatVersion = static_cast<int>(ParseNumber(line.substr(17)).value_or(14));
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            section = std::string(line.substr(1, line.size() - 2));
            continue;
        }

        const std::string where = "line " + std::to_string(lineNumber);
        if (section == "General" || section == "Metadata" || section == "Difficulty")
        {
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key = Trim(line.substr(0, colon));
            const std::string value(Trim(line.substr(colon + 1)));

            if (key == "AudioFilename")
            {
                chart.AudioFile = value;
            }
            else if (key == "PreviewTime")
            {
                const double preview = ParseNumber(value).value_or(-1.0);
                chart.Metadata.PreviewStartUs = preview < 0.0 ? 0 : MsToUs(preview, 0);
            }
            else if (key == "Mode")
            {
                mode = static_cast<int>(ParseNumber(value).value_or(0.0));
            }
            else if (key == "Title")
            {
                chart.Metadata.Title = value;
            }
            else if (key == "Artist")
            {
                chart.Metadata.Artist = value;
            }
            else if (key == "Creator")
            {
                chart.Metadata.Charter = value;
            }
            else if (key == "Version")
            {
                chart.Metadata.DifficultyName = value;
            }
            else if (key == "CircleSize")
            {
                const std::optional<double> keys = ParseNumber(value);
                if (keys)
                {
                    keyCount = static_cast<int>(std::lround(*keys));
                }
            }
        }
        else if (section == "TimingPoints")
        {
            const std::vector<std::string_view> fields = Split(line, ',');
            const std::optional<double> time = fields.size() > 1 ? ParseNumber(fields[0]) : std::nullopt;
            const std::optional<double> beatLength = fields.size() > 1 ? ParseNumber(fields[1]) : std::nullopt;
            if (!time || !beatLength)
            {
                result.Warnings.push_back(where + ": unreadable timing point, skipped");
                continue;
            }

            OsuTimingPoint point;
            point.TimeMs = *time;
            point.BeatLength = *beatLength;
            point.Meter = fields.size() > 2 ? static_cast<int>(ParseNumber(fields[2]).value_or(4.0)) : 4;
            point.Volume = fields.size() > 5 ? static_cast<int>(ParseNumber(fields[5]).value_or(100.0)) : 100;
            point.IsUninherited = fields.size() > 6 ? ParseNumber(fields[6]).value_or(1.0) != 0.0 : true;
            if (point.IsUninherited && point.BeatLength <= 0.0)
            {
                result.Warnings.push_back(where + ": timing point with no tempo, skipped");
                continue;
            }
            timingPoints.push_back(point);
        }
        else if (section == "HitObjects")
        {
            const std::vector<std::string_view> fields = Split(line, ',');
            const std::optional<double> x = fields.size() > 3 ? ParseNumber(fields[0]) : std::nullopt;
            const std::optional<double> time = fields.size() > 3 ? ParseNumber(fields[2]) : std::nullopt;
            const std::optional<double> type = fields.size() > 3 ? ParseNumber(fields[3]) : std::nullopt;
            if (!x || !time || !type)
            {
                result.Warnings.push_back(where + ": unreadable hit object, skipped");
                continue;
            }

            OsuHitObject object;
            object.X = *x;
            object.TimeMs = *time;
            object.Type = static_cast<int>(*type);
            std::string_view hitSample = fields.size() > 5 ? fields[5] : std::string_view();
            if ((object.Type & HOLD_TYPE_BIT) != 0)
            {
                // Holds: x,y,time,type,hitSound,endTime:hitSample
                const std::optional<double> end =
                    fields.size() > 5 ? ParseNumber(Split(fields[5], ':').front()) : std::nullopt;
                if (!end)
                {
                    result.Warnings.push_back(where + ": hold without an end time, imported as a tap");
                }
                object.EndTimeMs = end;
                const size_t colon = hitSample.find(':');
                hitSample = colon == std::string_view::npos ? std::string_view() : hitSample.substr(colon + 1);
            }
            ReadHitSample(hitSample, object);
            hitObjects.push_back(std::move(object));
        }
    }

    if (mode != MANIA_MODE)
    {
        return Error{"Not an osu!mania beatmap (Mode " + std::to_string(mode) + ")"};
    }
    if (!keyCount || *keyCount < 1 || *keyCount > static_cast<int>(mania::MAX_LANES))
    {
        return Error{"Unsupported key count (CircleSize); Hyoshi supports 1 to " + std::to_string(mania::MAX_LANES)};
    }
    if (hitObjects.empty())
    {
        return Error{"The beatmap has no hit objects"};
    }

    const int64_t offsetUs = formatVersion < OLD_FORMAT_VERSION ? OLD_FORMAT_OFFSET_US : 0;

    // Uninherited before inherited at the same time, so the SV reset doesn't override the SV.
    std::stable_sort(timingPoints.begin(), timingPoints.end(),
                     [](const OsuTimingPoint& a, const OsuTimingPoint& b)
                     {
                         if (a.TimeMs != b.TimeMs)
                         {
                             return a.TimeMs < b.TimeMs;
                         }
                         return a.IsUninherited && !b.IsUninherited;
                     });

    double lastObjectMs = 0.0;
    for (const OsuHitObject& object : hitObjects)
    {
        lastObjectMs = std::max(lastObjectMs, object.EndTimeMs.value_or(object.TimeMs));
    }
    const double dominantBeatLength = GetDominantBeatLength(timingPoints, lastObjectMs);

    double beatLength = dominantBeatLength;
    double sv = 1.0;
    for (const OsuTimingPoint& point : timingPoints)
    {
        const SongTimeUs time = MsToUs(point.TimeMs, offsetUs);
        if (point.IsUninherited)
        {
            beatLength = point.BeatLength;
            sv = 1.0;
            rhythm::TimingPoint timing;
            timing.Time = time;
            timing.Bpm = 60'000.0 / point.BeatLength;
            timing.BeatsPerBar = static_cast<uint32_t>(std::clamp(point.Meter, 1, 64));
            chart.Timing.push_back(timing);
        }
        else if (point.BeatLength < 0.0)
        {
            sv = std::clamp(-100.0 / point.BeatLength, MIN_SV, MAX_SV);
        }

        const double multiplier = dominantBeatLength > 0.0 ? sv * dominantBeatLength / beatLength : sv;
        if (!chart.ScrollVelocity.empty() && chart.ScrollVelocity.back().Time == time)
        {
            chart.ScrollVelocity.back().Multiplier = multiplier;
        }
        else if (chart.ScrollVelocity.empty() || chart.ScrollVelocity.back().Multiplier != multiplier)
        {
            chart.ScrollVelocity.push_back({time, multiplier});
        }
    }

    std::stable_sort(hitObjects.begin(), hitObjects.end(),
                     [](const OsuHitObject& a, const OsuHitObject& b) { return a.TimeMs < b.TimeMs; });

    // Volume of the timing point in effect at a time, inherited or not.
    std::vector<OsuTimingPoint> byTime = timingPoints;
    std::stable_sort(byTime.begin(), byTime.end(),
                     [](const OsuTimingPoint& a, const OsuTimingPoint& b) { return a.TimeMs < b.TimeMs; });
    const auto getVolumeAt = [&byTime](double timeMs)
    {
        const auto after =
            std::upper_bound(byTime.begin(), byTime.end(), timeMs + TIMING_POINT_LENIENCY_MS,
                             [](double time, const OsuTimingPoint& point) { return time < point.TimeMs; });
        return after == byTime.begin() ? 100 : std::prev(after)->Volume;
    };
    std::map<std::string, int32_t, std::less<>> sampleIndices;

    const auto keys = static_cast<uint32_t>(*keyCount);
    std::vector<uint32_t> lanes;
    std::vector<std::optional<SongTimeUs>> laneEnds(keys);
    for (const OsuHitObject& object : hitObjects)
    {
        const auto lane =
            static_cast<uint32_t>(std::clamp(static_cast<int64_t>(std::floor(object.X * keys / PLAYFIELD_WIDTH)),
                                             int64_t{0}, static_cast<int64_t>(keys) - 1));
        const SongTimeUs time = MsToUs(object.TimeMs, offsetUs);
        SongTimeUs endTime = object.EndTimeMs ? MsToUs(*object.EndTimeMs, offsetUs) : time;
        if (endTime < time)
        {
            result.Warnings.push_back("Hold at " + std::to_string(time) +
                                      " us ends before it starts; imported as a tap");
            endTime = time;
        }
        if (laneEnds[lane] && time <= *laneEnds[lane])
        {
            result.Warnings.push_back("Note at " + std::to_string(time) + " us in lane " + std::to_string(lane) +
                                      " overlaps the previous note; dropped");
            continue;
        }
        laneEnds[lane] = endTime;

        rhythm::ChartNote note{time, endTime};
        if (!object.SampleFile.empty())
        {
            const auto [found, isNew] =
                sampleIndices.try_emplace(object.SampleFile, static_cast<int32_t>(chart.Samples.size()));
            if (isNew)
            {
                chart.Samples.push_back(object.SampleFile);
            }
            note.Sample = found->second;
            const int volume = object.SampleVolume > 0 ? object.SampleVolume : getVolumeAt(object.TimeMs);
            note.SampleVolume = static_cast<uint32_t>(std::clamp(volume, 0, 100));
        }
        chart.Notes.push_back(note);
        lanes.push_back(lane);
    }

    Result<mania::ManiaChart> maniaChart = mania::BuildManiaChart(std::move(chart), keys, lanes);
    if (!maniaChart)
    {
        return maniaChart.GetError();
    }
    result.Chart = std::move(maniaChart).Value();
    return result;
}

} // namespace hyoshi::osu
