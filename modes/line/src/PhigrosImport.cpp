#include "line/PhigrosImport.h"

#include "rhythm/ChartFile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <utility>

namespace hyoshi::line
{

namespace
{

// A Phigros time unit is 1/32 of a beat: 1.875 / bpm seconds.
constexpr double US_PER_UNIT_AT_ONE_BPM = 1'875'000.0;
// Format 1 packs a move event's x and y into one number, x * 1000 + y, on an 880 by 520 screen.
constexpr int64_t PACKED_FORMAT = 1;
constexpr double PACKED_X = 880.0;
constexpr double PACKED_Y = 520.0;
// Charts open their first events long before the song and close their last long after it.
constexpr SongTimeUs LATEST_EVENT_US = 24LL * 3600 * 1'000'000;

SongTimeUs ToUs(double units, double bpm)
{
    return std::llround(units * US_PER_UNIT_AT_ONE_BPM / bpm);
}

SongTimeUs ClampEventTime(SongTimeUs time)
{
    return std::clamp<SongTimeUs>(time, 0, LATEST_EVENT_US);
}

// An event's span as two keys.
void AddSpan(std::vector<LineKey>& keys, SongTimeUs start, double startValue, SongTimeUs end, double endValue)
{
    keys.push_back({start, static_cast<float>(startValue)});
    keys.push_back({std::max(end, start), static_cast<float>(endValue)});
}

void SortKeys(std::vector<LineKey>& keys)
{
    std::stable_sort(keys.begin(), keys.end(), [](const LineKey& a, const LineKey& b) { return a.Time < b.Time; });
}

// Reads every event of `key`, with its times in microseconds.
Result<void> ReadEvents(const rhythm::ChartFields& line, std::string_view key, double bpm,
                        const std::function<Result<void>(const rhythm::ChartFields&, SongTimeUs, SongTimeUs)>& read)
{
    return line.ForEachObject(key,
                              [&](const rhythm::ChartFields& event, size_t) -> Result<void>
                              {
                                  Result<double> start = event.GetNumber("startTime");
                                  Result<double> end = event.GetNumber("endTime");
                                  if (!start || !end)
                                  {
                                      return start ? end.GetError() : start.GetError();
                                  }
                                  return read(event, ClampEventTime(ToUs(start.Value(), bpm)),
                                              ClampEventTime(ToUs(end.Value(), bpm)));
                              });
}

Result<double> GetPair(const rhythm::ChartFields& event, std::string_view from, std::string_view to, double& end)
{
    Result<double> start = event.GetNumber(from);
    Result<double> finish = event.GetNumber(to, start ? start.Value() : 0.0);
    if (!start || !finish)
    {
        return start ? finish.GetError() : start.GetError();
    }
    end = finish.Value();
    return start.Value();
}

std::string Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }
    // YAML's quotes.
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
    {
        text = text.substr(1, text.size() - 2);
    }
    return std::string(text);
}

} // namespace

Result<PhigrosImportResult> ImportPhigros(std::string_view json)
{
    PhigrosImportResult result;
    rhythm::Chart info;
    info.Mode = "phigros";
    info.Modes = {info.Mode};
    std::vector<JudgeLine> lines;
    struct Imported
    {
        rhythm::ChartNote Common;
        LineNote Line;
    };
    std::vector<Imported> notes;
    size_t unknownNotes = 0;
    bool isRpe = false;

    Result<void> read = rhythm::ReadJson(
        json,
        [&](const rhythm::ChartFields& root) -> Result<void>
        {
            // Re:PhiEdit's format has a global BPM list and its META.
            if (root.Has("BPMList") || root.Has("META"))
            {
                isRpe = true;
                return {};
            }
            Result<int64_t> version = root.GetInt("formatVersion", 3);
            Result<double> offset = root.GetNumber("offset", 0.0);
            if (!version || !offset)
            {
                return version ? offset.GetError() : version.GetError();
            }
            info.AudioOffsetUs = std::llround(offset.Value() * 1'000'000.0);
            if (!root.IsArray("judgeLineList"))
            {
                return Error{"Not a Phigros chart: no 'judgeLineList'"};
            }
            const bool isPacked = version.Value() == PACKED_FORMAT;
            return root.ForEachObject(
                "judgeLineList",
                [&](const rhythm::ChartFields& source, size_t index) -> Result<void>
                {
                    Result<double> bpm = source.GetNumber("bpm");
                    if (!bpm)
                    {
                        return bpm.GetError();
                    }
                    if (bpm.Value() <= 0.0)
                    {
                        return Error{"'bpm' must be positive"};
                    }
                    if (info.Timing.empty())
                    {
                        info.Timing.push_back({0, bpm.Value(), 4, 4});
                    }
                    JudgeLine line;
                    Result<void> events = ReadEvents(
                        source, "judgeLineMoveEvents", bpm.Value(),
                        [&](const rhythm::ChartFields& event, SongTimeUs start, SongTimeUs end) -> Result<void>
                        {
                            double xEnd = 0.0;
                            Result<double> x = GetPair(event, "start", "end", xEnd);
                            if (!x)
                            {
                                return x.GetError();
                            }
                            double yStart = 0.0;
                            double yEnd = 0.0;
                            double xStart = x.Value();
                            if (isPacked)
                            {
                                yStart = std::fmod(xStart, 1000.0) / PACKED_Y;
                                yEnd = std::fmod(xEnd, 1000.0) / PACKED_Y;
                                xStart = std::floor(xStart / 1000.0) / PACKED_X;
                                xEnd = std::floor(xEnd / 1000.0) / PACKED_X;
                            }
                            else
                            {
                                Result<double> y = GetPair(event, "start2", "end2", yEnd);
                                if (!y)
                                {
                                    return y.GetError();
                                }
                                yStart = y.Value();
                            }
                            AddSpan(line.X, start, xStart, end, xEnd);
                            AddSpan(line.Y, start, yStart, end, yEnd);
                            return {};
                        });
                    for (const auto& [key, target] :
                         {std::pair<std::string_view, std::vector<LineKey>*>{"judgeLineRotateEvents", &line.Rotation},
                          {"judgeLineDisappearEvents", &line.Opacity}})
                    {
                        if (!events)
                        {
                            break;
                        }
                        events = ReadEvents(
                            source, key, bpm.Value(),
                            [target](const rhythm::ChartFields& event, SongTimeUs start, SongTimeUs end) -> Result<void>
                            {
                                double endValue = 0.0;
                                Result<double> value = GetPair(event, "start", "end", endValue);
                                if (!value)
                                {
                                    return value.GetError();
                                }
                                AddSpan(*target, start, value.Value(), end, endValue);
                                return {};
                            });
                    }
                    if (events)
                    {
                        events = ReadEvents(
                            source, "speedEvents", bpm.Value(),
                            [&line](const rhythm::ChartFields& event, SongTimeUs start, SongTimeUs) -> Result<void>
                            {
                                Result<double> value = event.GetNumber("value");
                                if (!value)
                                {
                                    return value.GetError();
                                }
                                line.Speed.push_back({start, static_cast<float>(value.Value())});
                                return {};
                            });
                    }
                    if (!events)
                    {
                        return events;
                    }
                    for (std::vector<LineKey>* keys : {&line.X, &line.Y, &line.Rotation, &line.Opacity, &line.Speed})
                    {
                        SortKeys(*keys);
                    }

                    for (const auto& [key, isAbove] : {std::pair<std::string_view, bool>{"notesAbove", true},
                                                       std::pair<std::string_view, bool>{"notesBelow", false}})
                    {
                        Result<void> notesRead = source.ForEachObject(
                            key,
                            [&, isAbove = isAbove](const rhythm::ChartFields& fields, size_t) -> Result<void>
                            {
                                Result<int64_t> type = fields.GetInt("type");
                                Result<double> time = fields.GetNumber("time");
                                Result<double> x = fields.GetNumber("positionX", 0.0);
                                Result<double> hold = fields.GetNumber("holdTime", 0.0);
                                Result<double> speed = fields.GetNumber("speed", 1.0);
                                if (!type || !time || !x || !hold || !speed)
                                {
                                    return !type ? type.GetError()
                                                 : (!time ? time.GetError()
                                                          : (!x ? x.GetError()
                                                                : (!hold ? hold.GetError() : speed.GetError())));
                                }
                                constexpr std::array<NoteKind, 4> KINDS{NoteKind::Tap, NoteKind::Drag, NoteKind::Hold,
                                                                        NoteKind::Flick};
                                if (type.Value() < 1 || type.Value() > 4)
                                {
                                    ++unknownNotes;
                                    return {};
                                }
                                Imported note;
                                note.Line.Line = static_cast<uint32_t>(index);
                                note.Line.X = static_cast<float>(x.Value());
                                note.Line.IsAbove = isAbove;
                                note.Line.Kind = KINDS[static_cast<size_t>(type.Value() - 1)];
                                note.Line.Speed = static_cast<float>(speed.Value());
                                note.Common.Time = ToUs(time.Value(), bpm.Value());
                                note.Common.EndTime =
                                    note.Line.Kind == NoteKind::Hold
                                        ? std::max(note.Common.Time + 1, ToUs(time.Value() + hold.Value(), bpm.Value()))
                                        : note.Common.Time;
                                notes.push_back(note);
                                return {};
                            });
                        if (!notesRead)
                        {
                            return notesRead;
                        }
                    }
                    lines.push_back(std::move(line));
                    return {};
                });
        });
    if (!read)
    {
        return read.GetError();
    }
    if (isRpe)
    {
        return ImportRpe(json);
    }
    if (lines.empty())
    {
        return Error{"The chart has no judgment lines"};
    }
    if (unknownNotes > 0)
    {
        result.Warnings.push_back(std::to_string(unknownNotes) + " notes of unknown types were skipped");
    }

    std::stable_sort(notes.begin(), notes.end(),
                     [](const Imported& a, const Imported& b) { return a.Common.Time < b.Common.Time; });
    std::vector<LineNote> lineNotes;
    for (const Imported& note : notes)
    {
        info.Notes.push_back(note.Common);
        lineNotes.push_back(note.Line);
    }
    Result<LineChart> chart = BuildLineChart(std::move(info), std::move(lines), std::move(lineNotes));
    if (!chart)
    {
        return chart.GetError();
    }
    result.Chart = std::move(chart).Value();
    return result;
}

PhigrosInfo ReadPhigrosInfo(std::string_view text)
{
    PhigrosInfo info;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            continue;
        }
        std::string key = Trim(line.substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string value = Trim(line.substr(colon + 1));
        if (key == "name")
        {
            info.Title = std::move(value);
        }
        else if (key == "composer" || key == "artist")
        {
            info.Artist = std::move(value);
        }
        else if (key == "charter")
        {
            info.Charter = std::move(value);
        }
        else if (key == "level")
        {
            info.Level = std::move(value);
        }
        else if (key == "music" || key == "song")
        {
            info.MusicFile = std::move(value);
        }
        else if (key == "chart")
        {
            info.ChartFile = std::move(value);
        }
        else if (key == "illustration" || key == "picture")
        {
            info.Illustration = std::move(value);
        }
    }
    return info;
}

} // namespace hyoshi::line
