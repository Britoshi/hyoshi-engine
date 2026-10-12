#include "line/LineChart.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>

namespace hyoshi::line
{

namespace
{

constexpr std::string_view MODE_NAME = "phigros";
constexpr double US_PER_SECOND_D = 1'000'000.0;

double ToSeconds(SongTimeUs time)
{
    return static_cast<double>(time) / US_PER_SECOND_D;
}

// The key at or before `time`, if any, and the one after it.
template <typename Keys>
size_t FindKeyAfter(const Keys& keys, SongTimeUs time)
{
    return static_cast<size_t>(std::upper_bound(keys.begin(), keys.end(), time,
                                                [](SongTimeUs t, const LineKey& key) { return t < key.Time; }) -
                               keys.begin());
}

// A field that's one number for the whole chart, or a list of {"timeUs", "value"} keys.
Result<void> ReadKeys(const rhythm::ChartFields& fields, std::string_view key, std::vector<LineKey>& keys)
{
    if (!fields.Has(key))
    {
        return {};
    }
    if (!fields.IsArray(key))
    {
        Result<double> value = fields.GetNumber(key);
        if (!value)
        {
            return value.GetError();
        }
        keys.push_back({0, static_cast<float>(value.Value())});
        return {};
    }
    return fields.ForEachObject(key,
                                [&keys](const rhythm::ChartFields& element, size_t) -> Result<void>
                                {
                                    Result<int64_t> time = element.GetInt("timeUs");
                                    Result<double> value = element.GetNumber("value");
                                    if (!time || !value)
                                    {
                                        return time ? value.GetError() : time.GetError();
                                    }
                                    if (!keys.empty() && time.Value() < keys.back().Time)
                                    {
                                        return Error{"keys must be sorted by time"};
                                    }
                                    keys.push_back({time.Value(), static_cast<float>(value.Value())});
                                    return {};
                                });
}

// A float as the shortest decimal that reads back as it, so 0.6f is written 0.6 and not
// 0.6000000238418579.
double ToShortest(float value)
{
    return std::strtod(std::format("{}", value).c_str(), nullptr);
}

// A value with one key at the start is written as a number, and one without keys not at all
// (its default).
void WriteKeys(rhythm::ChartFieldWriter& fields, std::string_view key, const std::vector<LineKey>& keys)
{
    if (keys.empty())
    {
        return;
    }
    if (keys.size() == 1 && keys.front().Time == 0)
    {
        fields.AddNumber(key, ToShortest(keys.front().Value));
        return;
    }
    fields.AddObjects(key, keys.size(),
                      [&keys](rhythm::ChartFieldWriter& element, size_t index)
                      {
                          element.AddInt("timeUs", keys[index].Time);
                          element.AddNumber("value", ToShortest(keys[index].Value));
                      });
}

std::string_view GetKindName(NoteKind kind)
{
    switch (kind)
    {
    case NoteKind::Hold:
        return "hold";
    case NoteKind::Drag:
        return "drag";
    case NoteKind::Flick:
        return "flick";
    case NoteKind::Tap:
        break;
    }
    return "tap";
}

Result<NoteKind> ParseKind(std::string_view name)
{
    if (name == "tap")
    {
        return NoteKind::Tap;
    }
    if (name == "hold")
    {
        return NoteKind::Hold;
    }
    if (name == "drag")
    {
        return NoteKind::Drag;
    }
    if (name == "flick")
    {
        return NoteKind::Flick;
    }
    return Error{"'kind' must be tap, hold, drag, or flick, not '" + std::string(name) + "'"};
}

} // namespace

uint32_t LineChart::GetJudgmentCount() const
{
    uint32_t count = 0;
    for (const LineNote& note : Notes)
    {
        count += note.IsHold() ? 2 : 1;
    }
    return count;
}

float GetKeyValue(const std::vector<LineKey>& keys, SongTimeUs time, float fallback)
{
    if (keys.empty())
    {
        return fallback;
    }
    const size_t after = FindKeyAfter(keys, time);
    if (after == 0)
    {
        return keys.front().Value;
    }
    if (after == keys.size())
    {
        return keys.back().Value;
    }
    const LineKey& from = keys[after - 1];
    const LineKey& to = keys[after];
    const SongTimeUs span = to.Time - from.Time;
    const float t = span > 0 ? static_cast<float>(time - from.Time) / static_cast<float>(span) : 1.0f;
    return from.Value + ((to.Value - from.Value) * t);
}

LineState GetLineState(const JudgeLine& line, SongTimeUs time, glm::vec2 fieldSize)
{
    LineState state;
    state.Position = {GetKeyValue(line.X, time, DEFAULT_LINE_X) * fieldSize.x,
                      (1.0f - GetKeyValue(line.Y, time, DEFAULT_LINE_Y)) * fieldSize.y};
    // Counterclockwise degrees in the chart; clockwise radians on screen, y down.
    state.Angle = -GetKeyValue(line.Rotation, time, 0.0f) * std::numbers::pi_v<float> / 180.0f;
    state.Along = {std::cos(state.Angle), std::sin(state.Angle)};
    state.Up = {std::sin(state.Angle), -std::cos(state.Angle)};
    state.Opacity = std::clamp(GetKeyValue(line.Opacity, time, 1.0f), 0.0f, 1.0f);
    state.Length = std::max(GetKeyValue(line.Length, time, 1.0f), 0.0f);
    state.Thickness = std::max(GetKeyValue(line.Thickness, time, 1.0f), 0.0f);
    if (!line.Red.empty() || !line.Green.empty() || !line.Blue.empty())
    {
        state.Color = glm::clamp(glm::vec3{GetKeyValue(line.Red, time, 1.0f), GetKeyValue(line.Green, time, 1.0f),
                                           GetKeyValue(line.Blue, time, 1.0f)},
                                 0.0f, 1.0f);
    }
    return state;
}

double GetFloorPosition(const JudgeLine& line, SongTimeUs time)
{
    if (line.Speed.empty())
    {
        return ToSeconds(time) * DEFAULT_LINE_SPEED;
    }
    // Before the first key, at its speed; from each key on, at its speed until the next.
    const size_t after = FindKeyAfter(line.Speed, time);
    const size_t key = after == 0 ? 0 : after - 1;
    const double keyFloor = line.SpeedKeyFloors.empty() ? 0.0 : line.SpeedKeyFloors[key];
    return keyFloor + (ToSeconds(time - line.Speed[key].Time) * line.Speed[key].Value);
}

float GetNoteDistance(const LineChart& chart, const LineNote& note, SongTimeUs time)
{
    const double floor = GetFloorPosition(chart.Lines[note.Line], time);
    return static_cast<float>((note.FloorPosition - floor) * note.Speed) * DISTANCE_UNIT;
}

glm::vec2 GetNotePosition(const LineChart& chart, const LineNote& note, SongTimeUs time, bool isEnd)
{
    const JudgeLine& line = chart.Lines[note.Line];
    const LineState state = GetLineState(line, time, chart.FieldSize);
    const double floor = GetFloorPosition(line, time);
    const double target = isEnd ? note.EndFloorPosition : note.FloorPosition;
    const float distance = static_cast<float>((target - floor) * note.Speed) * DISTANCE_UNIT;
    return state.Position + (state.Along * (note.X * X_UNIT)) + (state.Up * (note.IsAbove ? distance : -distance));
}

float GetLineX(const LineState& state, glm::vec2 point)
{
    return glm::dot(point - state.Position, state.Along) / X_UNIT;
}

Result<LineChart> BuildLineChart(rhythm::Chart chart, std::vector<JudgeLine> lines, std::vector<LineNote> notes)
{
    if (notes.size() != chart.Notes.size())
    {
        return Error{"Every note needs its line fields"};
    }
    if (lines.empty())
    {
        return Error{"A line chart needs at least one line"};
    }
    for (JudgeLine& line : lines)
    {
        // Integrate speed key to key.
        line.SpeedKeyFloors.assign(line.Speed.size(), 0.0);
        for (size_t k = 0; k < line.Speed.size(); ++k)
        {
            line.SpeedKeyFloors[k] =
                k == 0 ? ToSeconds(line.Speed[0].Time) * line.Speed[0].Value
                       : line.SpeedKeyFloors[k - 1] +
                             (ToSeconds(line.Speed[k].Time - line.Speed[k - 1].Time) * line.Speed[k - 1].Value);
        }
    }

    LineChart result;
    result.Lines = std::move(lines);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const rhythm::ChartNote& source = chart.Notes[i];
        LineNote note = notes[i];
        const std::string context = "notes[" + std::to_string(i) + "]: ";
        if (note.Line >= result.Lines.size())
        {
            return Error{context + "line " + std::to_string(note.Line) + " doesn't exist"};
        }
        note.Time = source.Time;
        note.EndTime = source.EndTime;
        if (note.Kind == NoteKind::Hold && note.EndTime <= note.Time)
        {
            return Error{context + "a hold needs 'endTimeUs' after 'timeUs'"};
        }
        if (note.Kind != NoteKind::Hold)
        {
            note.EndTime = note.Time;
        }
        const JudgeLine& line = result.Lines[note.Line];
        note.FloorPosition = GetFloorPosition(line, note.Time);
        note.EndFloorPosition = GetFloorPosition(line, note.EndTime);
        result.Notes.push_back(note);
    }
    rhythm::MarkSimultaneous(result.Notes);
    chart.Notes.clear();
    result.Info = std::move(chart);
    return result;
}

std::vector<PictureUse> GetPictureUses(const LineChart& chart)
{
    std::vector<PictureUse> uses;
    for (const JudgeLine& line : chart.Lines)
    {
        if (line.Picture.empty())
        {
            continue;
        }
        auto use =
            std::find_if(uses.begin(), uses.end(), [&line](const PictureUse& u) { return u.Name == line.Picture; });
        if (use == uses.end())
        {
            uses.push_back({line.Picture, glm::vec2{0.0f}});
            use = std::prev(uses.end());
        }
        // Without keys, a pixel is a field unit.
        const auto largest = [](const std::vector<LineKey>& keys)
        {
            float most = keys.empty() ? 1.0f : 0.0f;
            for (const LineKey& key : keys)
            {
                most = std::max(most, std::abs(key.Value));
            }
            return most;
        };
        use->MaxUnitsPerPixel =
            glm::max(use->MaxUnitsPerPixel, glm::vec2{largest(line.Length), largest(line.Thickness)});
    }
    return uses;
}

rhythm::ChartModeFormat MakeLineFormat(std::vector<JudgeLine>& lines, std::vector<LineNote>& notes)
{
    rhythm::ChartModeFormat format;
    format.Mode = std::string(MODE_NAME);
    format.ReadSettings = [&lines](const rhythm::ChartFields& settings) -> Result<void>
    {
        return settings.ForEachObject(
            "lines",
            [&lines](const rhythm::ChartFields& fields, size_t) -> Result<void>
            {
                JudgeLine line;
                for (const auto& [key, target] : {std::pair<std::string_view, std::vector<LineKey>*>{"x", &line.X},
                                                  {"y", &line.Y},
                                                  {"rotation", &line.Rotation},
                                                  {"opacity", &line.Opacity},
                                                  {"speed", &line.Speed},
                                                  {"length", &line.Length},
                                                  {"thickness", &line.Thickness},
                                                  {"red", &line.Red},
                                                  {"green", &line.Green},
                                                  {"blue", &line.Blue}})
                {
                    if (Result<void> result = ReadKeys(fields, key, *target); !result)
                    {
                        return Error{"'" + std::string(key) + "': " + result.GetError().Message};
                    }
                }
                Result<std::string> picture = fields.GetString("picture", "");
                Result<double> anchorX = fields.GetNumber("anchorX", 0.5);
                Result<double> anchorY = fields.GetNumber("anchorY", 0.5);
                if (!picture || !anchorX || !anchorY)
                {
                    return !picture ? picture.GetError() : (!anchorX ? anchorX.GetError() : anchorY.GetError());
                }
                line.Picture = std::move(picture).Value();
                line.Anchor = {static_cast<float>(anchorX.Value()), static_cast<float>(anchorY.Value())};
                lines.push_back(std::move(line));
                return {};
            });
    };
    format.ReadNote = [&notes](const rhythm::ChartFields& fields, size_t) -> Result<void>
    {
        LineNote note;
        Result<int64_t> line = fields.GetInt("line", 0);
        Result<double> x = fields.GetNumber("x", 0.0);
        Result<std::string> side = fields.GetString("side", "above");
        Result<std::string> kind = fields.GetString("kind", "tap");
        Result<double> speed = fields.GetNumber("speed", 1.0);
        if (!line || !x || !side || !kind || !speed)
        {
            return !line
                       ? line.GetError()
                       : (!x ? x.GetError() : (!side ? side.GetError() : (!kind ? kind.GetError() : speed.GetError())));
        }
        if (line.Value() < 0)
        {
            return Error{"'line' must be 0 or more"};
        }
        if (side.Value() != "above" && side.Value() != "below")
        {
            return Error{"'side' must be above or below"};
        }
        Result<NoteKind> parsedKind = ParseKind(kind.Value());
        if (!parsedKind)
        {
            return parsedKind.GetError();
        }
        note.Line = static_cast<uint32_t>(line.Value());
        note.X = static_cast<float>(x.Value());
        note.IsAbove = side.Value() == "above";
        note.Kind = parsedKind.Value();
        note.Speed = static_cast<float>(speed.Value());
        notes.push_back(note);
        return {};
    };
    format.WriteSettings = [&lines](rhythm::ChartFieldWriter& settings)
    {
        settings.AddObjects("lines", lines.size(),
                            [&lines](rhythm::ChartFieldWriter& fields, size_t index)
                            {
                                const JudgeLine& line = lines[index];
                                for (const auto& [key, source] :
                                     {std::pair<std::string_view, const std::vector<LineKey>*>{"x", &line.X},
                                      {"y", &line.Y},
                                      {"rotation", &line.Rotation},
                                      {"opacity", &line.Opacity},
                                      {"speed", &line.Speed},
                                      {"length", &line.Length},
                                      {"thickness", &line.Thickness},
                                      {"red", &line.Red},
                                      {"green", &line.Green},
                                      {"blue", &line.Blue}})
                                {
                                    WriteKeys(fields, key, *source);
                                }
                                if (!line.Picture.empty())
                                {
                                    fields.AddString("picture", line.Picture);
                                    fields.AddNumber("anchorX", ToShortest(line.Anchor.x));
                                    fields.AddNumber("anchorY", ToShortest(line.Anchor.y));
                                }
                            });
    };
    format.WriteNote = [&notes](rhythm::ChartFieldWriter& fields, size_t index)
    {
        if (index >= notes.size())
        {
            return;
        }
        const LineNote& note = notes[index];
        fields.AddInt("line", note.Line);
        fields.AddNumber("x", ToShortest(note.X));
        fields.AddString("side", note.IsAbove ? "above" : "below");
        fields.AddString("kind", GetKindName(note.Kind));
        if (note.Speed != 1.0f)
        {
            fields.AddNumber("speed", ToShortest(note.Speed));
        }
    };
    return format;
}

Result<LineChart> ParseLineChart(std::string_view json)
{
    std::vector<JudgeLine> lines;
    std::vector<LineNote> notes;
    Result<rhythm::Chart> chart = rhythm::ParseChart(json, MakeLineFormat(lines, notes));
    if (!chart)
    {
        return chart.GetError();
    }
    return BuildLineChart(std::move(chart).Value(), std::move(lines), std::move(notes));
}

} // namespace hyoshi::line
