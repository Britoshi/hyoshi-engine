#include "mania/StepManiaImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hyoshi::mania
{

namespace
{

constexpr double US_PER_SECOND = 1'000'000.0;
constexpr double SECONDS_PER_MINUTE = 60.0;
constexpr double BEATS_PER_MEASURE = 4.0;
constexpr std::string_view DANCE_SINGLE = "dance-single";
// .sm's #NOTES fields: type, description, difficulty, meter, radar values, notes.
constexpr size_t SM_NOTES_FIELDS = 6;

using Tags = std::map<std::string, std::string, std::less<>>;
// A beat and a value: BPMs, stops and delays (seconds), warps (beats).
using BeatValues = std::vector<std::pair<double, double>>;

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string ToUpper(std::string_view text)
{
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return upper;
}

std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == separator)
        {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// strtod reads '.' as the decimal point, as nothing in the engine sets a locale.
std::optional<double> ParseNumber(std::string_view text)
{
    const std::string copy(Trim(text));
    if (copy.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

// The text without comments: // to the end of the line.
std::string StripComments(std::string_view text)
{
    std::string kept;
    kept.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
            {
                ++i;
            }
        }
        if (i < text.size())
        {
            kept += text[i];
        }
    }
    return kept;
}

struct Tag
{
    std::string Key;
    std::string Value;
};

// #KEY:value; tags, in order. A value ends at its semicolon, or before a line that starts with the
// next tag, since StepMania forgives a missing semicolon.
std::vector<Tag> ReadTags(std::string_view text)
{
    std::vector<Tag> tags;
    size_t at = text.find('#');
    while (at != std::string_view::npos)
    {
        const size_t colon = text.find_first_of(":\n;", at);
        if (colon == std::string_view::npos)
        {
            break;
        }
        if (text[colon] != ':')
        {
            at = text.find('#', colon);
            continue;
        }
        size_t end = colon + 1;
        while (end < text.size() && text[end] != ';')
        {
            if (text[end] == '\n')
            {
                size_t next = end + 1;
                while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r'))
                {
                    ++next;
                }
                if (next < text.size() && text[next] == '#')
                {
                    break;
                }
            }
            ++end;
        }
        tags.push_back({ToUpper(Trim(text.substr(at + 1, colon - at - 1))),
                        std::string(Trim(text.substr(colon + 1, end - colon - 1)))});
        at = text.find('#', end);
    }
    return tags;
}

BeatValues ReadBeatValues(std::string_view value)
{
    BeatValues pairs;
    for (const std::string_view entry : Split(value, ','))
    {
        const std::vector<std::string_view> parts = Split(entry, '=');
        if (parts.size() < 2)
        {
            continue;
        }
        const std::optional<double> beat = ParseNumber(parts[0]);
        const std::optional<double> amount = ParseNumber(parts[1]);
        if (beat && amount)
        {
            pairs.emplace_back(*beat, *amount);
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return pairs;
}

// Beats to seconds from beat 0, through BPM changes, stops, delays, and warps.
class BeatClock
{
public:
    // False without a positive BPM.
    bool Set(BeatValues bpmValues, BeatValues stopValues, BeatValues delayValues, BeatValues warpValues,
             std::vector<std::string>& warnings)
    {
        bpms = std::move(bpmValues);
        stops = std::move(stopValues);
        delays = std::move(delayValues);
        if (!bpms.empty())
        {
            // The first BPM holds from beat 0.
            bpms.front().first = std::min(bpms.front().first, 0.0);
        }
        // Old charts warp with a negative BPM (or stop): skip over the beats it covers.
        bool hasNegative = false;
        for (size_t i = 0; i < bpms.size(); ++i)
        {
            if (bpms[i].second <= 0.0)
            {
                const double end = i + 1 < bpms.size() ? bpms[i + 1].first : std::numeric_limits<double>::infinity();
                warpValues.emplace_back(bpms[i].first, end - bpms[i].first);
                hasNegative = true;
            }
        }
        const auto negativeStop =
            std::remove_if(stops.begin(), stops.end(), [](const auto& s) { return s.second < 0.0; });
        hasNegative = hasNegative || negativeStop != stops.end();
        stops.erase(negativeStop, stops.end());
        if (hasNegative)
        {
            warnings.emplace_back("negative BPMs are skipped over as warps, and negative stops left out");
        }
        // Warps, merged where they overlap.
        std::sort(warpValues.begin(), warpValues.end());
        warps.clear();
        for (const auto& [beat, length] : warpValues)
        {
            if (length <= 0.0)
            {
                continue;
            }
            if (!warps.empty() && beat <= warps.back().second)
            {
                warps.back().second = std::max(warps.back().second, beat + length);
            }
            else
            {
                warps.emplace_back(beat, beat + length);
            }
        }
        return std::any_of(bpms.begin(), bpms.end(), [](const auto& bpm) { return bpm.second > 0.0; });
    }

    bool IsWarped(double beat) const
    {
        return std::any_of(warps.begin(), warps.end(),
                           [beat](const auto& warp) { return beat >= warp.first && beat < warp.second; });
    }

    double GetSeconds(double beat) const
    {
        double seconds = 0.0;
        for (size_t i = 0; i < bpms.size(); ++i)
        {
            const double bpm = bpms[i].second;
            const double from = bpms[i].first;
            const double to = std::min(i + 1 < bpms.size() ? bpms[i + 1].first : beat, beat);
            if (bpm <= 0.0 || to <= from)
            {
                continue;
            }
            seconds += (to - from - GetWarpedBeats(from, to)) * SECONDS_PER_MINUTE / bpm;
        }
        // A stop comes after its beat's notes, a delay before them.
        for (const auto& [stopBeat, length] : stops)
        {
            seconds += stopBeat < beat && !IsWarped(stopBeat) ? length : 0.0;
        }
        for (const auto& [delayBeat, length] : delays)
        {
            seconds += delayBeat <= beat && !IsWarped(delayBeat) ? length : 0.0;
        }
        return seconds;
    }

    const BeatValues& GetBpms() const
    {
        return bpms;
    }

private:
    double GetWarpedBeats(double from, double to) const
    {
        double warped = 0.0;
        for (const auto& [start, end] : warps)
        {
            warped += std::max(0.0, std::min(end, to) - std::max(start, from));
        }
        return warped;
    }

    BeatValues bpms;
    BeatValues stops;
    BeatValues delays;
    // Start and end beats.
    std::vector<std::pair<double, double>> warps;
};

struct RawNote
{
    double Beat = 0.0;
    double EndBeat = 0.0;
    uint32_t Lane = 0;
};

struct NoteCounts
{
    uint32_t Mines = 0;
    uint32_t Fakes = 0;
    uint32_t Warped = 0;
    uint32_t Unended = 0;
    uint32_t Overlapping = 0;
};

// The notes of a chart's note data, and its lane count (0 if it has none).
std::vector<RawNote> ReadNotes(std::string_view data, uint32_t& laneCount, NoteCounts& counts)
{
    std::vector<RawNote> notes;
    laneCount = 0;
    std::vector<std::optional<double>> heads;
    const std::vector<std::string_view> measures = Split(data, ',');
    for (size_t measure = 0; measure < measures.size(); ++measure)
    {
        std::vector<std::string_view> rows;
        for (const std::string_view line : Split(measures[measure], '\n'))
        {
            const std::string_view row = Trim(line);
            if (!row.empty())
            {
                rows.push_back(row);
            }
        }
        for (size_t r = 0; r < rows.size(); ++r)
        {
            if (laneCount == 0)
            {
                laneCount = static_cast<uint32_t>(rows[r].size());
                heads.assign(laneCount, std::nullopt);
            }
            const double beat =
                (static_cast<double>(measure) + (static_cast<double>(r) / static_cast<double>(rows.size()))) *
                BEATS_PER_MEASURE;
            for (uint32_t lane = 0; lane < laneCount && lane < rows[r].size(); ++lane)
            {
                switch (rows[r][lane])
                {
                case '1':
                case 'L':
                    notes.push_back({beat, beat, lane});
                    break;
                case '2':
                case '4':
                    heads[lane] = beat;
                    break;
                case '3':
                    if (heads[lane])
                    {
                        notes.push_back({*heads[lane], beat, lane});
                        heads[lane].reset();
                    }
                    break;
                case 'M':
                    ++counts.Mines;
                    break;
                case 'F':
                    ++counts.Fakes;
                    break;
                default:
                    break;
                }
            }
        }
    }
    for (uint32_t lane = 0; lane < heads.size(); ++lane)
    {
        if (heads[lane])
        {
            notes.push_back({*heads[lane], *heads[lane], lane});
            ++counts.Unended;
        }
    }
    return notes;
}

std::string Find(const Tags& tags, std::string_view key)
{
    const auto found = tags.find(key);
    return found == tags.end() ? std::string() : found->second;
}

// A chart as read, before it's built into a Mania chart.
struct ReadChart
{
    rhythm::Chart Chart;
    uint32_t LaneCount = 0;
    std::vector<uint32_t> Lanes;
};

struct ReadSong
{
    std::vector<ReadChart> Charts;
    std::string BackgroundFile;
    std::string Genre;
    std::vector<std::string> Warnings;
};

Result<ReadSong> ReadStepManiaSong(std::string_view fileText)
{
    const std::string text = StripComments(fileText);
    Tags song;
    // Each chart's own tags: .ssc's after its #NOTEDATA, .sm's from its #NOTES.
    std::vector<Tags> charts;
    bool isSsc = false;
    for (Tag& tag : ReadTags(text))
    {
        if (tag.Key == "NOTEDATA")
        {
            isSsc = true;
            charts.emplace_back();
        }
        else if (isSsc)
        {
            charts.back()[tag.Key] = std::move(tag.Value);
        }
        else if (tag.Key == "NOTES")
        {
            const std::vector<std::string_view> fields = Split(tag.Value, ':');
            if (fields.size() < SM_NOTES_FIELDS)
            {
                continue;
            }
            Tags chart;
            chart["STEPSTYPE"] = std::string(Trim(fields[0]));
            chart["DESCRIPTION"] = std::string(Trim(fields[1]));
            chart["DIFFICULTY"] = std::string(Trim(fields[2]));
            chart["METER"] = std::string(Trim(fields[3]));
            chart["NOTES"] = std::string(fields[SM_NOTES_FIELDS - 1]);
            charts.push_back(std::move(chart));
        }
        else
        {
            song[tag.Key] = std::move(tag.Value);
        }
    }
    if (charts.empty())
    {
        return Error{"No charts in the StepMania file"};
    }

    ReadSong result;
    result.BackgroundFile = Find(song, "BACKGROUND");
    result.Genre = Find(song, "GENRE");
    std::string title = Find(song, "TITLE");
    if (const std::string subtitle = Find(song, "SUBTITLE"); !subtitle.empty())
    {
        title += " " + subtitle;
    }
    std::set<std::string> names;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        const Tags& chartTags = charts[i];
        // A chart with its own BPMs has its own timing (.ssc's split timing).
        const Tags& timing = Find(chartTags, "BPMS").empty() ? song : chartTags;
        const auto get = [&](std::string_view key)
        {
            const std::string own = Find(chartTags, key);
            return own.empty() ? Find(song, key) : own;
        };
        const std::string type = Find(chartTags, "STEPSTYPE");
        const std::string difficulty = Find(chartTags, "DIFFICULTY");
        const std::string meter = Find(chartTags, "METER");
        std::string name = (type.empty() || type == DANCE_SINGLE ? "" : type + " ") +
                           (difficulty.empty() ? "Chart" : difficulty) + (meter.empty() ? "" : " " + meter);
        if (ToUpper(difficulty) == "EDIT" && !Find(chartTags, "DESCRIPTION").empty())
        {
            name += " (" + Find(chartTags, "DESCRIPTION") + ")";
        }
        for (int copy = 2; !names.insert(name).second; ++copy)
        {
            name += " (" + std::to_string(copy) + ")";
        }
        const std::string context = title + " [" + name + "]: ";

        std::vector<std::string> warnings;
        BeatClock clock;
        if (!clock.Set(ReadBeatValues(Find(timing, "BPMS")), ReadBeatValues(Find(timing, "STOPS")),
                       ReadBeatValues(Find(timing, "DELAYS")), ReadBeatValues(Find(timing, "WARPS")), warnings))
        {
            result.Warnings.push_back(context + "no BPM, skipped");
            continue;
        }
        const double offset = ParseNumber(Find(timing, "OFFSET")).value_or(0.0);
        const auto toUs = [&clock, offset](double beat)
        { return static_cast<SongTimeUs>(std::llround((clock.GetSeconds(beat) - offset) * US_PER_SECOND)); };

        uint32_t laneCount = 0;
        NoteCounts counts;
        const std::vector<RawNote> raw = ReadNotes(Find(chartTags, "NOTES"), laneCount, counts);
        if (laneCount == 0 || laneCount > MAX_LANES || raw.empty())
        {
            result.Warnings.push_back(context + (raw.empty() ? "no notes" : std::to_string(laneCount) + " columns") +
                                      ", skipped");
            continue;
        }

        struct Timed
        {
            SongTimeUs Time = 0;
            SongTimeUs EndTime = 0;
            uint32_t Lane = 0;
        };
        std::vector<Timed> timed;
        for (const RawNote& note : raw)
        {
            if (clock.IsWarped(note.Beat))
            {
                ++counts.Warped;
                continue;
            }
            const SongTimeUs time = toUs(note.Beat);
            timed.push_back({time, std::max(time, toUs(note.EndBeat)), note.Lane});
        }
        std::stable_sort(timed.begin(), timed.end(), [](const Timed& a, const Timed& b)
                         { return a.Time != b.Time ? a.Time < b.Time : a.Lane < b.Lane; });

        rhythm::Chart chart;
        chart.Mode = "mania";
        chart.Modes = {chart.Mode};
        chart.Metadata.Title = title;
        chart.Metadata.Artist = Find(song, "ARTIST");
        chart.Metadata.Charter = get("CREDIT");
        chart.Metadata.DifficultyName = name;
        chart.Metadata.DifficultyValue = ParseNumber(meter).value_or(0.0);
        chart.Metadata.PreviewStartUs =
            static_cast<SongTimeUs>(std::llround(ParseNumber(get("SAMPLESTART")).value_or(0.0) * US_PER_SECOND));
        chart.AudioFile = get("MUSIC");
        for (const auto& [beat, bpm] : clock.GetBpms())
        {
            if (bpm <= 0.0)
            {
                continue;
            }
            rhythm::TimingPoint point;
            point.Time = toUs(std::max(beat, 0.0));
            point.Bpm = bpm;
            if (!chart.Timing.empty() && chart.Timing.back().Time == point.Time)
            {
                chart.Timing.back() = point;
            }
            else
            {
                chart.Timing.push_back(point);
            }
        }

        std::vector<uint32_t> lanes;
        std::vector<std::optional<SongTimeUs>> laneEnds(laneCount);
        for (const Timed& note : timed)
        {
            if (laneEnds[note.Lane] && note.Time <= *laneEnds[note.Lane])
            {
                ++counts.Overlapping;
                continue;
            }
            laneEnds[note.Lane] = note.EndTime;
            chart.Notes.push_back({note.Time, note.EndTime});
            lanes.push_back(note.Lane);
        }

        for (const auto& [count, what] : {std::pair{counts.Mines, "mines"},
                                          {counts.Fakes, "fake notes"},
                                          {counts.Warped, "notes in warps"},
                                          {counts.Overlapping, "notes overlapping the one before in their lane"}})
        {
            if (count > 0)
            {
                warnings.push_back(std::to_string(count) + " " + what + " left out");
            }
        }
        if (counts.Unended > 0)
        {
            warnings.push_back(std::to_string(counts.Unended) + " holds without an end made taps");
        }
        for (const std::string& warning : warnings)
        {
            result.Warnings.push_back(context + warning);
        }

        result.Charts.push_back({std::move(chart), laneCount, std::move(lanes)});
    }
    if (result.Charts.empty())
    {
        return Error{"No playable charts in the StepMania file" +
                     (result.Warnings.empty() ? std::string() : ": " + result.Warnings.front())};
    }
    return result;
}

} // namespace

Result<StepManiaImportResult> ImportStepMania(std::string_view fileText)
{
    Result<ReadSong> song = ReadStepManiaSong(fileText);
    if (!song)
    {
        return song.GetError();
    }
    StepManiaImportResult result;
    result.BackgroundFile = std::move(song.Value().BackgroundFile);
    result.Warnings = std::move(song.Value().Warnings);
    for (ReadChart& read : song.Value().Charts)
    {
        const std::string name = read.Chart.Metadata.Title + " [" + read.Chart.Metadata.DifficultyName + "]: ";
        Result<ManiaChart> built = BuildManiaChart(std::move(read.Chart), read.LaneCount, read.Lanes);
        if (!built)
        {
            // Never so far: the lanes were checked as they were read.
            result.Warnings.push_back(name + built.GetError().Message);
            continue;
        }
        result.Charts.push_back(std::move(built).Value());
    }
    if (result.Charts.empty())
    {
        return Error{"No playable charts in the StepMania file"};
    }
    return result;
}

Result<StepManiaInfo> ReadStepManiaInfo(std::string_view fileText)
{
    Result<ReadSong> song = ReadStepManiaSong(fileText);
    if (!song)
    {
        return song.GetError();
    }
    StepManiaInfo info;
    info.BackgroundFile = std::move(song.Value().BackgroundFile);
    info.Genre = std::move(song.Value().Genre);
    for (ReadChart& read : song.Value().Charts)
    {
        info.Charts.push_back({std::move(read.Chart), read.LaneCount});
    }
    return info;
}

} // namespace hyoshi::mania
