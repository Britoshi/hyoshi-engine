#include "songs/Charts.h"

#include "core/Log.h"
#include "platform/Platform.h"
#include "rhythm/ChartFile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace hyoshi::songs
{

namespace
{

using hyoshi::SongTimeUs;

constexpr double US_PER_MS = 1000.0;
constexpr SongTimeUs END_OF_SONG = std::numeric_limits<SongTimeUs>::max();

SongTimeUs MsToUs(double ms)
{
    return static_cast<SongTimeUs>(std::llround(ms * US_PER_MS));
}

bool EndsWithLower(const std::string& text, std::string_view suffix)
{
    if (text.size() < suffix.size())
    {
        return false;
    }
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const char c = text[text.size() - suffix.size() + i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c) != suffix[i])
        {
            return false;
        }
    }
    return true;
}

std::string GetDirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// One source chart of a medley, loaded and moved onto the medley's clock.
struct Source
{
    std::string Name;
    // Lanes and lines (any chart LoadMixedChartFile reads), or circles (an osu!standard .osu).
    std::optional<LoadedMixedChart> Mixed;
    std::optional<hyoshi::circle::CircleChart> Circles;
    // Added to the source's chart times to land on the medley's.
    SongTimeUs Delta = 0;
    // The medley's spans where this source's notes play.
    std::vector<std::pair<SongTimeUs, SongTimeUs>> Spans;

    bool Keeps(SongTimeUs time) const
    {
        return std::any_of(Spans.begin(), Spans.end(),
                           [time](const auto& span) { return time >= span.first && time < span.second; });
    }

    std::vector<std::string> GetModes() const
    {
        if (Circles)
        {
            return {"circle"};
        }
        return Mixed ? Mixed->Info.Modes : std::vector<std::string>{};
    }
};

void ShiftTiming(hyoshi::rhythm::Chart& info, SongTimeUs delta)
{
    for (hyoshi::rhythm::TimingPoint& point : info.Timing)
    {
        point.Time += delta;
    }
    for (hyoshi::rhythm::ScrollVelocityPoint& point : info.ScrollVelocity)
    {
        point.Time += delta;
    }
}

// The lanes' notes the source keeps, moved by its delta.
hyoshi::Result<hyoshi::mania::ManiaChart> RebuildLanes(const hyoshi::mania::ManiaChart& chart, const Source& source)
{
    hyoshi::rhythm::Chart info = chart.Info;
    ShiftTiming(info, source.Delta);
    info.Notes.clear();
    std::vector<uint32_t> lanes;
    for (const hyoshi::mania::ManiaNote& note : chart.Notes)
    {
        const SongTimeUs time = note.Time + source.Delta;
        if (!source.Keeps(time))
        {
            continue;
        }
        hyoshi::rhythm::ChartNote kept{time, note.EndTime + source.Delta};
        kept.Sample = note.Sample;
        kept.SampleVolume = note.SampleVolume;
        info.Notes.push_back(kept);
        lanes.push_back(note.Lane);
    }
    return hyoshi::mania::BuildManiaChart(std::move(info), chart.LaneCount, lanes);
}

// The lines, every key moved by the source's delta, with the notes it keeps; their pictures named
// from the medley's folder (`folder` is the source's, relative to it).
hyoshi::Result<hyoshi::line::LineChart> RebuildLines(const hyoshi::line::LineChart& chart, const Source& source,
                                                     const std::string& folder)
{
    hyoshi::rhythm::Chart info = chart.Info;
    ShiftTiming(info, source.Delta);
    info.Notes.clear();
    std::vector<hyoshi::line::JudgeLine> lines = chart.Lines;
    for (hyoshi::line::JudgeLine& line : lines)
    {
        if (!line.Picture.empty())
        {
            line.Picture = folder + line.Picture;
        }
        for (std::vector<hyoshi::line::LineKey>* keys :
             {&line.X, &line.Y, &line.Rotation, &line.Opacity, &line.Speed, &line.Length, &line.Thickness, &line.Red,
              &line.Green, &line.Blue})
        {
            for (hyoshi::line::LineKey& key : *keys)
            {
                key.Time += source.Delta;
            }
        }
    }
    std::vector<hyoshi::line::LineNote> notes;
    for (const hyoshi::line::LineNote& note : chart.Notes)
    {
        const SongTimeUs time = note.Time + source.Delta;
        if (source.Keeps(time))
        {
            info.Notes.push_back({time, note.EndTime + source.Delta});
            notes.push_back(note);
        }
    }
    return hyoshi::line::BuildLineChart(std::move(info), std::move(lines), std::move(notes));
}

// The circles the source keeps, moved by its delta; a note after a cut starts a new combo. Its
// sample points are kept to its parts (the one in effect at each part's start moved to it), so they
// sound its notes alone once merged with another source's. Finished once merged (MergeCircles).
void RebuildCircles(hyoshi::circle::CircleChart& chart, const Source& source)
{
    ShiftTiming(chart.Info, source.Delta);
    for (hyoshi::circle::SamplePoint& point : chart.SamplePoints)
    {
        point.Time += source.Delta;
    }
    std::vector<hyoshi::circle::SamplePoint> points;
    for (const auto& [start, end] : source.Spans)
    {
        std::optional<hyoshi::circle::SamplePoint> inEffect;
        for (const hyoshi::circle::SamplePoint& point : chart.SamplePoints)
        {
            if (point.Time <= start)
            {
                inEffect = point;
            }
            else if (point.Time < end)
            {
                points.push_back(point);
            }
        }
        if (inEffect)
        {
            inEffect->Time = start;
            points.push_back(*inEffect);
        }
    }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
    chart.SamplePoints = std::move(points);
    std::vector<hyoshi::circle::CircleNote> kept;
    bool wasCut = true;
    for (hyoshi::circle::CircleNote note : chart.Notes)
    {
        note.Time += source.Delta;
        note.EndTime += source.Delta;
        note.GlideStartTime += source.Delta;
        note.GlideAppearTime += source.Delta;
        if (!source.Keeps(note.Time))
        {
            wasCut = true;
            continue;
        }
        note.IsNewCombo = note.IsNewCombo || wasCut;
        wasCut = false;
        kept.push_back(note);
    }
    chart.Notes = std::move(kept);
}

// Adds a source's circles (rebuilt) to the medley's: notes in time order, their control points,
// glide paths, and sounds renumbered into the medley's lists, and sample points kept. The first
// source's difficulty and colors stand (another's colors if the first has none).
void MergeCircles(hyoshi::circle::CircleChart& into, hyoshi::circle::CircleChart from)
{
    const auto pointOffset = static_cast<uint32_t>(into.ControlPoints.size());
    const auto glideOffset = static_cast<uint32_t>(into.GlidePath.size());
    const auto soundOffset = static_cast<uint32_t>(into.HitSounds.size());
    into.ControlPoints.insert(into.ControlPoints.end(), from.ControlPoints.begin(), from.ControlPoints.end());
    into.GlidePath.insert(into.GlidePath.end(), from.GlidePath.begin(), from.GlidePath.end());
    into.HitSounds.insert(into.HitSounds.end(), std::make_move_iterator(from.HitSounds.begin()),
                          std::make_move_iterator(from.HitSounds.end()));
    for (hyoshi::circle::CircleNote note : from.Notes)
    {
        note.FirstControlPoint += pointOffset;
        note.FirstGlidePoint += glideOffset;
        note.FirstHitSound += soundOffset;
        into.Notes.push_back(note);
    }
    std::stable_sort(into.Notes.begin(), into.Notes.end(),
                     [](const auto& a, const auto& b) { return a.Time < b.Time; });
    into.SamplePoints.insert(into.SamplePoints.end(), from.SamplePoints.begin(), from.SamplePoints.end());
    std::stable_sort(into.SamplePoints.begin(), into.SamplePoints.end(),
                     [](const auto& a, const auto& b) { return a.Time < b.Time; });
    if (into.ComboColors.empty())
    {
        into.ComboColors = std::move(from.ComboColors);
    }
}

// Plays a chart's lanes as `mode`'s, "mania" or "arcaea": the same notes, judged and drawn as that
// mode's.
hyoshi::Result<void> PlayLanesAs(LoadedMixedChart& mixed, const std::string& mode)
{
    if (mode != "mania" && mode != "arcaea")
    {
        return hyoshi::Error{"'lanesAs' is \"mania\" or \"arcaea\", not '" + mode + "'"};
    }
    const bool isArcaea = mode == "arcaea";
    std::optional<hyoshi::mania::ManiaChart>& from = isArcaea ? mixed.Mania : mixed.Arcaea;
    std::optional<hyoshi::mania::ManiaChart>& to = isArcaea ? mixed.Arcaea : mixed.Mania;
    if (!from)
    {
        return to ? hyoshi::Result<void>{} : hyoshi::Error{"'lanesAs' is for a chart with lanes"};
    }
    if (to)
    {
        return hyoshi::Error{"'lanesAs' is for a chart with one kind of lanes"};
    }
    to = std::move(from);
    from.reset();
    to->Info.Mode = mode;
    to->Info.Modes = {mode};
    const std::string other = isArcaea ? "mania" : "arcaea";
    std::vector<std::string>& modes = mixed.Info.Modes;
    std::replace(modes.begin(), modes.end(), other, mode);
    return {};
}

hyoshi::Result<Source> LoadSource(const hyoshi::rhythm::ChartFields& fields, const std::string& directory,
                                  SongTimeUs medleyOffsetUs)
{
    Source source;
    hyoshi::Result<std::string> name = fields.GetString("name");
    hyoshi::Result<std::string> chartFile = fields.GetString("chart");
    hyoshi::Result<double> lagMs = fields.GetNumber("audioLagMs", 0.0);
    if (!name || !chartFile || !lagMs)
    {
        return !name ? name.GetError() : (!chartFile ? chartFile.GetError() : lagMs.GetError());
    }
    source.Name = name.Value();
    const std::string path = directory + chartFile.Value();
    if (IsMedleyFile(path))
    {
        return hyoshi::Error{"A medley's source can't be a medley"};
    }
    SongTimeUs sourceOffsetUs = 0;
    hyoshi::Result<LoadedMixedChart> mixed = LoadMixedChartFile(path);
    if (mixed)
    {
        sourceOffsetUs = mixed.Value().Info.AudioOffsetUs;
        source.Mixed = std::move(mixed).Value();
    }
    else if (EndsWithLower(path, ".osu"))
    {
        // An osu!standard map: circles.
        hyoshi::Result<LoadedCircleChart> circles = LoadCircleChartFile(path);
        if (!circles)
        {
            return hyoshi::Error{"'" + chartFile.Value() + "': " + circles.GetError().Message};
        }
        sourceOffsetUs = circles.Value().Chart.Info.AudioOffsetUs;
        source.Circles = std::move(circles.Value().Chart);
    }
    else
    {
        return hyoshi::Error{"'" + chartFile.Value() + "': " + mixed.GetError().Message};
    }
    if (fields.Has("lanesAs"))
    {
        hyoshi::Result<std::string> lanesAs = fields.GetString("lanesAs");
        if (!lanesAs)
        {
            return lanesAs.GetError();
        }
        hyoshi::Result<void> played = source.Mixed
                                          ? PlayLanesAs(*source.Mixed, lanesAs.Value())
                                          : hyoshi::Result<void>{hyoshi::Error{"'lanesAs' is for a chart with lanes"}};
        if (!played)
        {
            return hyoshi::Error{"'" + chartFile.Value() + "': " + played.GetError().Message};
        }
    }
    // The source's own offset, unless the medley syncs it with another.
    if (fields.Has("offsetMs"))
    {
        hyoshi::Result<double> offsetMs = fields.GetNumber("offsetMs");
        if (!offsetMs)
        {
            return offsetMs.GetError();
        }
        sourceOffsetUs = MsToUs(offsetMs.Value());
    }
    // A note at source time t sounds at source audio time t + its offset, which is the medley's
    // audio time that plus the lag, and the medley's song time that less the medley's offset.
    source.Delta = MsToUs(lagMs.Value()) + sourceOffsetUs - medleyOffsetUs;
    return source;
}

} // namespace

bool IsMedleyFile(const std::string& path)
{
    return EndsWithLower(path, ".medley.json");
}

hyoshi::Result<LoadedMixedChart> LoadMedleyFile(const std::string& path)
{
    hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
    if (!bytes)
    {
        return bytes.GetError();
    }
    const std::string directory = GetDirectoryOf(path);
    LoadedMixedChart loaded;
    loaded.Directory = directory;
    std::vector<Source> sources;
    // Each part's sources, and its end.
    std::vector<std::pair<std::vector<std::string>, SongTimeUs>> parts;

    hyoshi::Result<void> read = hyoshi::rhythm::ReadJson(
        {reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size()},
        [&](const hyoshi::rhythm::ChartFields& root) -> hyoshi::Result<void>
        {
            hyoshi::rhythm::ChartMetadata& metadata = loaded.Info.Metadata;
            const auto text = [&root](std::string_view key, std::string& target)
            {
                hyoshi::Result<std::string> value = root.GetString(key, "");
                if (value)
                {
                    target = std::move(value).Value();
                }
            };
            text("title", metadata.Title);
            text("artist", metadata.Artist);
            text("charter", metadata.Charter);
            text("difficultyName", metadata.DifficultyName);
            text("pack", metadata.Pack);
            text("audio", loaded.Info.AudioFile);
            text("background", loaded.BackgroundFile);
            hyoshi::Result<double> offsetMs = root.GetNumber("audioOffsetMs", 0.0);
            hyoshi::Result<double> previewMs = root.GetNumber("previewMs", 0.0);
            if (!offsetMs || !previewMs)
            {
                return offsetMs ? previewMs.GetError() : offsetMs.GetError();
            }
            loaded.Info.AudioOffsetUs = MsToUs(offsetMs.Value());
            metadata.PreviewStartUs = MsToUs(previewMs.Value());
            if (loaded.Info.AudioFile.empty())
            {
                return hyoshi::Error{"A medley needs 'audio'"};
            }

            hyoshi::Result<void> sourcesRead =
                root.ForEachObject("sources",
                                   [&](const hyoshi::rhythm::ChartFields& fields, size_t) -> hyoshi::Result<void>
                                   {
                                       hyoshi::Result<Source> source =
                                           LoadSource(fields, directory, loaded.Info.AudioOffsetUs);
                                       if (!source)
                                       {
                                           return source.GetError();
                                       }
                                       sources.push_back(std::move(source).Value());
                                       return {};
                                   });
            if (!sourcesRead)
            {
                return sourcesRead;
            }
            // Each part plays one source from where the one before it ends until its own end.
            return root.ForEachObject("parts",
                                      [&](const hyoshi::rhythm::ChartFields& fields, size_t) -> hyoshi::Result<void>
                                      {
                                          // A source's name, or a list of them that play together.
                                          hyoshi::Result<std::vector<std::string>> names =
                                              fields.IsArray("source")
                                                  ? fields.GetStrings("source")
                                                  : [&fields]() -> hyoshi::Result<std::vector<std::string>>
                                          {
                                              hyoshi::Result<std::string> name = fields.GetString("source");
                                              if (!name)
                                              {
                                                  return name.GetError();
                                              }
                                              return std::vector<std::string>{std::move(name).Value()};
                                          }();
                                          if (!names)
                                          {
                                              return names.GetError();
                                          }
                                          SongTimeUs until = END_OF_SONG;
                                          if (fields.Has("untilMs"))
                                          {
                                              hyoshi::Result<double> ms = fields.GetNumber("untilMs");
                                              if (!ms)
                                              {
                                                  return ms.GetError();
                                              }
                                              until = MsToUs(ms.Value());
                                          }
                                          parts.emplace_back(std::move(names).Value(), until);
                                          return {};
                                      });
        });
    if (!read)
    {
        return hyoshi::Error{path + ": " + read.GetError().Message};
    }
    if (sources.empty() || parts.empty())
    {
        return hyoshi::Error{path + ": a medley needs sources and parts"};
    }

    // Parts to spans, given to their sources.
    SongTimeUs start = std::numeric_limits<SongTimeUs>::min();
    for (const auto& [names, until] : parts)
    {
        if (until <= start)
        {
            return hyoshi::Error{path + ": the parts must go forward in time"};
        }
        LoadedMixedChart::Part part{{}, start, until};
        for (const std::string& name : names)
        {
            const auto source =
                std::find_if(sources.begin(), sources.end(), [&name](const Source& s) { return s.Name == name; });
            if (source == sources.end())
            {
                return hyoshi::Error{path + ": a part plays '" + name + "', which isn't a source"};
            }
            source->Spans.emplace_back(start, until);
            for (const std::string& mode : source->GetModes())
            {
                if (std::find(part.Modes.begin(), part.Modes.end(), mode) == part.Modes.end())
                {
                    part.Modes.push_back(mode);
                }
            }
        }
        loaded.Parts.push_back(std::move(part));
        start = until;
    }

    // Each kind of note from one source: its kept notes, on the medley's clock.
    for (Source& source : sources)
    {
        HYOSHI_LOG_DEBUG("Medley: source '{}', {}, in {} parts", source.Name,
                        source.Circles ? "circles" : (source.Mixed ? "a chart" : "nothing"), source.Spans.size());
        if (source.Spans.empty())
        {
            continue;
        }
        if (source.Circles)
        {
            RebuildCircles(*source.Circles, source);
            HYOSHI_LOG_DEBUG("Medley: '{}' keeps {} circles in {} parts", source.Name, source.Circles->Notes.size(),
                            source.Spans.size());
            if (loaded.Circles)
            {
                MergeCircles(*loaded.Circles, std::move(*source.Circles));
            }
            else
            {
                loaded.Circles = std::move(source.Circles);
            }
            continue;
        }
        LoadedMixedChart& mixed = *source.Mixed;
        for (auto [from, to] : {std::pair{&mixed.Mania, &loaded.Mania}, std::pair{&mixed.Arcaea, &loaded.Arcaea}})
        {
            if (!*from)
            {
                continue;
            }
            if (*to)
            {
                return hyoshi::Error{path + ": only one source can have lanes"};
            }
            hyoshi::Result<hyoshi::mania::ManiaChart> lanes = RebuildLanes(**from, source);
            if (!lanes)
            {
                return hyoshi::Error{path + ": '" + source.Name + "': " + lanes.GetError().Message};
            }
            *to = std::move(lanes).Value();
        }
        if (mixed.Lines)
        {
            if (loaded.Lines)
            {
                return hyoshi::Error{path + ": only one source can have Phigros lines"};
            }
            // A source in a folder of the medley's has its pictures there.
            const std::string folder =
                mixed.Directory.starts_with(directory) ? mixed.Directory.substr(directory.size()) : std::string();
            hyoshi::Result<hyoshi::line::LineChart> lines = RebuildLines(*mixed.Lines, source, folder);
            if (!lines)
            {
                return hyoshi::Error{path + ": '" + source.Name + "': " + lines.GetError().Message};
            }
            loaded.Lines = std::move(lines).Value();
        }
        if (loaded.Info.Timing.empty())
        {
            loaded.Info.Timing = mixed.Info.Timing;
            ShiftTiming(loaded.Info, source.Delta);
        }
        // A source's background, if the medley names none: relative to the medley's folder.
        if (loaded.BackgroundFile.empty() && !mixed.BackgroundFile.empty() && mixed.Directory.starts_with(directory))
        {
            loaded.BackgroundFile = mixed.Directory.substr(directory.size()) + mixed.BackgroundFile;
        }
    }
    if (loaded.Circles)
    {
        hyoshi::circle::FinishCircleChart(*loaded.Circles);
    }
    if (loaded.Info.Timing.empty() && loaded.Circles)
    {
        loaded.Info.Timing = loaded.Circles->Info.Timing;
    }
    MarkSimultaneousAcrossModes(loaded);

    // The modes, in the order the parts first play them.
    for (const LoadedMixedChart::Part& part : loaded.Parts)
    {
        for (const std::string& mode : part.Modes)
        {
            if (std::find(loaded.Info.Modes.begin(), loaded.Info.Modes.end(), mode) == loaded.Info.Modes.end())
            {
                loaded.Info.Modes.push_back(mode);
            }
        }
    }
    loaded.Info.Mode = loaded.Info.Modes.empty() ? std::string() : loaded.Info.Modes.front();
    return loaded;
}

} // namespace hyoshi::songs
