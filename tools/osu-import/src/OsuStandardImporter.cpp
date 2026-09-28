#include "osu/OsuStandardImporter.h"

#include "OsuText.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

namespace hyoshi::osu
{

namespace
{

using circle::CircleNote;
using circle::CurveType;
using circle::NoteType;
using detail::ParseNumber;
using detail::Split;
using detail::Trim;

constexpr int64_t OLD_FORMAT_OFFSET_US = 24'000;
constexpr int OLD_FORMAT_VERSION = 5;
constexpr int STANDARD_MODE = 0;
constexpr int CIRCLE_BIT = 1;
constexpr int SLIDER_BIT = 2;
constexpr int NEW_COMBO_BIT = 4;
constexpr int SPINNER_BIT = 8;
constexpr int HOLD_BIT = 128;
constexpr double MIN_SV = 0.1;
constexpr double MAX_SV = 10.0;
constexpr glm::vec2 SPINNER_POSITION{256.0f, 192.0f};

struct OsuTimingPoint
{
    double TimeMs = 0.0;
    double BeatLength = 0.0;
    int Meter = 4;
    bool IsUninherited = true;
};

struct OsuHitObject
{
    glm::vec2 Position{0.0f};
    double TimeMs = 0.0;
    int Type = 0;
    // Spinners.
    double EndTimeMs = 0.0;
    // Sliders: the points after the head.
    CurveType Curve = CurveType::Bezier;
    std::vector<glm::vec2> ControlPoints;
    int Passes = 1;
    double Length = 0.0;
};

SongTimeUs MsToUs(double milliseconds, int64_t offsetUs)
{
    return std::llround(milliseconds * 1000.0) + offsetUs;
}

CurveType ParseCurveType(std::string_view letter)
{
    switch (letter.empty() ? 'B' : letter.front())
    {
    case 'L':
        return CurveType::Linear;
    case 'C':
        return CurveType::Catmull;
    case 'P':
        return CurveType::PerfectCircle;
    default:
        return CurveType::Bezier;
    }
}

// x:y pairs after the curve letter. A letter among them (a type change mid-path) is skipped.
bool ReadSliderPath(std::string_view path, OsuHitObject& object)
{
    const std::vector<std::string_view> tokens = Split(path, '|');
    object.Curve = ParseCurveType(tokens.front());
    bool hasTypeChange = false;
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        const size_t colon = tokens[i].find(':');
        if (colon == std::string_view::npos)
        {
            hasTypeChange = hasTypeChange || !tokens[i].empty();
            continue;
        }
        const std::optional<double> x = ParseNumber(tokens[i].substr(0, colon));
        const std::optional<double> y = ParseNumber(tokens[i].substr(colon + 1));
        if (x && y)
        {
            object.ControlPoints.emplace_back(static_cast<float>(*x), static_cast<float>(*y));
        }
    }
    return hasTypeChange;
}

float MeasureCurve(const std::vector<glm::vec2>& points, CurveType curve)
{
    const std::vector<glm::vec2> dense = circle::SampleCurve(points, curve);
    float length = 0.0f;
    for (size_t i = 1; i < dense.size(); ++i)
    {
        length += glm::distance(dense[i - 1], dense[i]);
    }
    return length;
}

} // namespace

Result<OsuStandardImportResult> ImportOsuStandard(std::string_view osuText)
{
    OsuStandardImportResult result;
    circle::CircleChart& chart = result.Chart;
    chart.Info.Mode = "circle";

    int formatVersion = 14;
    int mode = 0;
    std::optional<float> approachRate;
    double audioLeadInMs = 0.0;
    std::vector<OsuTimingPoint> timingPoints;
    std::vector<OsuHitObject> hitObjects;
    size_t pathTypeChanges = 0;

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
            const std::optional<double> number = ParseNumber(value);
            const auto setFloat = [&](float& field)
            {
                if (number)
                {
                    field = static_cast<float>(*number);
                }
            };

            if (key == "AudioFilename")
            {
                chart.Info.AudioFile = value;
            }
            else if (key == "AudioLeadIn")
            {
                audioLeadInMs = std::max(0.0, number.value_or(0.0));
            }
            else if (key == "PreviewTime")
            {
                const double preview = number.value_or(-1.0);
                chart.Info.Metadata.PreviewStartUs = preview < 0.0 ? 0 : MsToUs(preview, 0);
            }
            else if (key == "Mode")
            {
                mode = static_cast<int>(number.value_or(0.0));
            }
            else if (key == "StackLeniency")
            {
                setFloat(chart.Difficulty.StackLeniency);
            }
            else if (key == "Title")
            {
                chart.Info.Metadata.Title = value;
            }
            else if (key == "Artist")
            {
                chart.Info.Metadata.Artist = value;
            }
            else if (key == "Creator")
            {
                chart.Info.Metadata.Charter = value;
            }
            else if (key == "Version")
            {
                chart.Info.Metadata.DifficultyName = value;
            }
            else if (key == "HPDrainRate")
            {
                setFloat(chart.Difficulty.HpDrainRate);
            }
            else if (key == "CircleSize")
            {
                setFloat(chart.Difficulty.CircleSize);
            }
            else if (key == "OverallDifficulty")
            {
                setFloat(chart.Difficulty.OverallDifficulty);
            }
            else if (key == "ApproachRate" && number)
            {
                approachRate = static_cast<float>(*number);
            }
            else if (key == "SliderMultiplier" && number && *number > 0.0)
            {
                chart.Difficulty.SliderMultiplier = *number;
            }
            else if (key == "SliderTickRate" && number && *number > 0.0)
            {
                chart.Difficulty.SliderTickRate = *number;
            }
        }
        else if (section == "Events")
        {
            // Background: 0,0,"file",x,y
            const std::vector<std::string_view> fields = Split(line, ',');
            if (fields.size() >= 3 && (fields[0] == "0" || fields[0] == "Background") && chart.BackgroundFile.empty())
            {
                std::string_view file = fields[2];
                if (file.size() >= 2 && file.front() == '"' && file.back() == '"')
                {
                    file = file.substr(1, file.size() - 2);
                }
                chart.BackgroundFile = std::string(file);
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
            const std::optional<double> y = fields.size() > 3 ? ParseNumber(fields[1]) : std::nullopt;
            const std::optional<double> time = fields.size() > 3 ? ParseNumber(fields[2]) : std::nullopt;
            const std::optional<double> type = fields.size() > 3 ? ParseNumber(fields[3]) : std::nullopt;
            if (!x || !y || !time || !type)
            {
                result.Warnings.push_back(where + ": unreadable hit object, skipped");
                continue;
            }

            OsuHitObject object;
            object.Position = {static_cast<float>(*x), static_cast<float>(*y)};
            object.TimeMs = *time;
            object.Type = static_cast<int>(*type);
            if ((object.Type & SLIDER_BIT) != 0)
            {
                // x,y,time,type,hitSound,curve,slides,length,...
                if (fields.size() < 8)
                {
                    result.Warnings.push_back(where + ": slider without a path, skipped");
                    continue;
                }
                pathTypeChanges += ReadSliderPath(fields[5], object) ? 1 : 0;
                object.Passes = std::max(1, static_cast<int>(ParseNumber(fields[6]).value_or(1.0)));
                object.Length = ParseNumber(fields[7]).value_or(0.0);
            }
            else if ((object.Type & SPINNER_BIT) != 0)
            {
                // x,y,time,type,hitSound,endTime,...
                object.EndTimeMs = fields.size() > 5 ? ParseNumber(fields[5]).value_or(*time) : *time;
            }
            else if ((object.Type & HOLD_BIT) != 0 && (object.Type & CIRCLE_BIT) == 0)
            {
                result.Warnings.push_back(where + ": osu!mania hold in a standard beatmap, skipped");
                continue;
            }
            hitObjects.push_back(std::move(object));
        }
    }

    if (mode != STANDARD_MODE)
    {
        return Error{"Not an osu!standard beatmap (Mode " + std::to_string(mode) + ")"};
    }
    if (hitObjects.empty())
    {
        return Error{"The beatmap has no hit objects"};
    }
    if (pathTypeChanges > 0)
    {
        result.Warnings.push_back(std::to_string(pathTypeChanges) +
                                  " sliders change curve type along their path; each is read as its first type");
    }

    chart.Difficulty.ApproachRate = approachRate.value_or(chart.Difficulty.OverallDifficulty);
    chart.Info.Metadata.DifficultyValue = chart.Difficulty.OverallDifficulty;
    chart.AudioLeadInUs = MsToUs(audioLeadInMs, 0);
    const int64_t offsetUs = formatVersion < OLD_FORMAT_VERSION ? OLD_FORMAT_OFFSET_US : 0;

    // Red lines before green lines at the same time, so the reset doesn't undo the new velocity.
    std::stable_sort(timingPoints.begin(), timingPoints.end(),
                     [](const OsuTimingPoint& a, const OsuTimingPoint& b)
                     {
                         if (a.TimeMs != b.TimeMs)
                         {
                             return a.TimeMs < b.TimeMs;
                         }
                         return a.IsUninherited && !b.IsUninherited;
                     });
    for (const OsuTimingPoint& point : timingPoints)
    {
        const SongTimeUs time = MsToUs(point.TimeMs, offsetUs);
        double velocity = 1.0;
        if (point.IsUninherited)
        {
            rhythm::TimingPoint timing;
            timing.Time = time;
            timing.Bpm = 60'000.0 / point.BeatLength;
            timing.BeatsPerBar = static_cast<uint32_t>(std::clamp(point.Meter, 1, 64));
            chart.Info.Timing.push_back(timing);
        }
        else if (point.BeatLength < 0.0)
        {
            velocity = std::clamp(-100.0 / point.BeatLength, MIN_SV, MAX_SV);
        }
        if (!chart.Info.ScrollVelocity.empty() && chart.Info.ScrollVelocity.back().Time == time)
        {
            chart.Info.ScrollVelocity.back().Multiplier = velocity;
        }
        else
        {
            chart.Info.ScrollVelocity.push_back({time, velocity});
        }
    }

    // Tempo and slider velocity at a time: the last red line at or before it (the first one, for
    // anything earlier), and the green line after that red line, if any.
    const auto getSliderTiming = [&](double timeMs)
    {
        double beatLength = 0.0;
        double velocity = 1.0;
        for (const OsuTimingPoint& point : timingPoints)
        {
            if (point.TimeMs > timeMs)
            {
                if (beatLength == 0.0 && point.IsUninherited)
                {
                    beatLength = point.BeatLength;
                }
                continue;
            }
            if (point.IsUninherited)
            {
                beatLength = point.BeatLength;
                velocity = 1.0;
            }
            else if (point.BeatLength < 0.0)
            {
                velocity = std::clamp(-100.0 / point.BeatLength, MIN_SV, MAX_SV);
            }
        }
        // 100 BPM for a file without any tempo.
        return std::pair{beatLength > 0.0 ? beatLength : 600.0, velocity};
    };

    std::stable_sort(hitObjects.begin(), hitObjects.end(),
                     [](const OsuHitObject& a, const OsuHitObject& b) { return a.TimeMs < b.TimeMs; });
    bool isAfterSpinner = false;
    for (const OsuHitObject& object : hitObjects)
    {
        CircleNote note;
        note.Time = MsToUs(object.TimeMs, offsetUs);
        note.EndTime = note.Time;
        note.Position = object.Position;
        note.IsNewCombo = (object.Type & NEW_COMBO_BIT) != 0 || chart.Notes.empty() || isAfterSpinner;
        isAfterSpinner = false;

        if ((object.Type & SLIDER_BIT) != 0 && !object.ControlPoints.empty())
        {
            std::vector<glm::vec2> points{object.Position};
            points.insert(points.end(), object.ControlPoints.begin(), object.ControlPoints.end());
            double length = object.Length;
            if (length <= 0.0)
            {
                length = MeasureCurve(points, object.Curve);
            }
            const auto [beatLength, velocity] = getSliderTiming(object.TimeMs);
            const double beats = length / (chart.Difficulty.SliderMultiplier * 100.0 * velocity);
            note.Type = NoteType::Slider;
            note.Curve = object.Curve;
            note.FirstControlPoint = static_cast<uint32_t>(chart.ControlPoints.size());
            note.ControlPointCount = static_cast<uint32_t>(points.size());
            note.Length = static_cast<float>(length);
            note.Passes = static_cast<uint32_t>(object.Passes);
            note.EndTime = note.Time + std::llround(beats * beatLength * object.Passes * 1000.0);
            chart.ControlPoints.insert(chart.ControlPoints.end(), points.begin(), points.end());
        }
        else if ((object.Type & SPINNER_BIT) != 0)
        {
            note.Type = NoteType::Spinner;
            note.Position = SPINNER_POSITION;
            note.EndTime = std::max(note.Time, MsToUs(object.EndTimeMs, offsetUs));
            isAfterSpinner = true;
        }
        else if ((object.Type & SLIDER_BIT) != 0)
        {
            result.Warnings.push_back("Slider at " + std::to_string(note.Time) +
                                      " us has a single control point; imported as a circle");
        }
        chart.Notes.push_back(note);
    }

    circle::FinishCircleChart(chart);
    return result;
}

} // namespace hyoshi::osu
