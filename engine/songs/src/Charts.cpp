#include "songs/Charts.h"

#include "osu/OsuManiaImporter.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string_view>
#include <utility>

namespace hyoshi::songs
{

using hyoshi::SongTimeUs;
using hyoshi::mania::ManiaChart;

namespace
{

constexpr uint32_t LEAD_IN_BARS = 2;

SongTimeUs GetDemoBeatTime(double beat)
{
    return DEMO_FIRST_BEAT_US + std::llround(beat * 60.0e6 / DEMO_BPM);
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size())
    {
        return false;
    }
    return std::equal(suffix.begin(), suffix.end(), text.end() - static_cast<std::ptrdiff_t>(suffix.size()),
                      [](char a, char b)
                      {
                          const auto lower = [](char c)
                          { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
                          return lower(a) == lower(b);
                      });
}

} // namespace

std::string GetChartKey(const hyoshi::rhythm::ChartMetadata& metadata)
{
    return metadata.Artist + " | " + metadata.Title + " | " + metadata.Charter + " | " + metadata.DifficultyName;
}

hyoshi::Result<ManiaChart> MakeDemoChart(uint32_t beatCount)
{
    struct Hit
    {
        double Beat;
        double EndBeat;
        uint32_t Lane;
    };

    std::vector<Hit> hits;
    for (uint32_t bar = LEAD_IN_BARS; ((bar + 1) * DEMO_BEATS_PER_BAR) + 2 <= beatCount; ++bar)
    {
        const double b = bar * static_cast<double>(DEMO_BEATS_PER_BAR);
        switch (bar % 4)
        {
        case 0:
            for (uint32_t i = 0; i < 4; ++i)
            {
                hits.push_back({b + i, b + i, i});
            }
            break;
        case 1:
        {
            constexpr std::array<uint32_t, 8> TRILL{0, 2, 1, 3, 0, 2, 1, 3};
            for (uint32_t i = 0; i < TRILL.size(); ++i)
            {
                const double beat = b + (i * 0.5);
                hits.push_back({beat, beat, TRILL[i]});
            }
            break;
        }
        case 2:
            hits.insert(hits.end(), {{b, b + 2.0, 0},
                                     {b, b, 3},
                                     {b + 1.0, b + 1.0, 2},
                                     {b + 2.0, b + 3.5, 1},
                                     {b + 2.0, b + 2.0, 3},
                                     {b + 3.0, b + 3.0, 0},
                                     {b + 3.0, b + 3.0, 3}});
            break;
        default:
            for (uint32_t i = 0; i < 4; ++i)
            {
                const uint32_t firstLane = i % 2;
                hits.push_back({b + i, b + i, firstLane});
                hits.push_back({b + i, b + i, firstLane + 2});
            }
            break;
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.Beat < b.Beat; });

    hyoshi::rhythm::Chart chart;
    chart.Mode = "mania";
    chart.Metadata.Title = "Metronome drill";
    chart.Metadata.Artist = "Hyoshi";
    chart.Metadata.Charter = "Generated";
    chart.Metadata.DifficultyName = "4K demo";
    chart.Metadata.PreviewStartUs = GetDemoBeatTime(16.0);
    chart.Timing.push_back({DEMO_FIRST_BEAT_US, DEMO_BPM, DEMO_BEATS_PER_BAR, 4});
    chart.ScrollVelocity = {{GetDemoBeatTime(48.0), 0.75}, {GetDemoBeatTime(64.0), 1.25}, {GetDemoBeatTime(80.0), 1.0}};

    std::vector<uint32_t> lanes;
    for (const Hit& hit : hits)
    {
        chart.Notes.push_back({GetDemoBeatTime(hit.Beat), GetDemoBeatTime(hit.EndBeat)});
        lanes.push_back(hit.Lane);
    }
    return hyoshi::mania::BuildManiaChart(std::move(chart), 4, lanes);
}

hyoshi::Result<LoadedChart> LoadChartFile(const std::string& path)
{
    hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
    if (!bytes)
    {
        return bytes.GetError();
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());

    LoadedChart loaded;
    const size_t slash = path.find_last_of("/\\");
    loaded.Directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    if (EndsWith(path, ".osu"))
    {
        hyoshi::Result<hyoshi::osu::OsuImportResult> imported = hyoshi::osu::ImportOsuMania(text);
        if (!imported)
        {
            return imported.GetError();
        }
        loaded.Chart = std::move(imported.Value().Chart);
        loaded.Warnings = std::move(imported.Value().Warnings);
        return loaded;
    }

    hyoshi::Result<ManiaChart> parsed = hyoshi::mania::ParseManiaChart(text);
    if (!parsed)
    {
        return parsed.GetError();
    }
    loaded.Chart = std::move(parsed).Value();
    return loaded;
}

double GetMainBpm(const ManiaChart& chart)
{
    const std::vector<hyoshi::rhythm::TimingPoint>& timing = chart.Info.Timing;
    if (timing.empty())
    {
        return 0.0;
    }
    const SongTimeUs end = std::max(GetChartEnd(chart), timing.back().Time);
    std::map<long long, SongTimeUs> durations;
    for (size_t i = 0; i < timing.size(); ++i)
    {
        const SongTimeUs until = i + 1 < timing.size() ? timing[i + 1].Time : end;
        durations[std::llround(timing[i].Bpm)] += std::max<SongTimeUs>(until - timing[i].Time, 0);
    }
    const auto longest = std::max_element(durations.begin(), durations.end(),
                                          [](const auto& a, const auto& b) { return a.second < b.second; });
    return static_cast<double>(longest->first);
}

SongTimeUs GetChartEnd(const ManiaChart& chart)
{
    SongTimeUs end = 0;
    for (const hyoshi::mania::ManiaNote& note : chart.Notes)
    {
        end = std::max(end, note.EndTime);
    }
    return end;
}

} // namespace hyoshi::songs
