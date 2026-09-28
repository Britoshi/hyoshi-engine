#include "mania/ManiaChart.h"

#include "rhythm/ChartFile.h"

#include <array>
#include <optional>
#include <utility>

namespace hyoshi::mania
{

namespace
{

constexpr const char* MODE_NAME = "mania";

} // namespace

uint32_t ManiaChart::GetJudgmentCount() const
{
    uint32_t count = 0;
    for (const ManiaNote& note : Notes)
    {
        count += note.IsHold() ? 2 : 1;
    }
    return count;
}

Result<ManiaChart> BuildManiaChart(rhythm::Chart chart, uint32_t laneCount, std::span<const uint32_t> lanes)
{
    if (laneCount == 0 || laneCount > MAX_LANES)
    {
        return Error{"laneCount must be 1 to " + std::to_string(MAX_LANES)};
    }
    if (lanes.size() != chart.Notes.size())
    {
        return Error{"Every note needs a lane"};
    }

    ManiaChart result;
    result.LaneCount = laneCount;
    result.Scroll = rhythm::ScrollMap(chart.ScrollVelocity);
    result.Notes.reserve(chart.Notes.size());

    std::array<std::optional<SongTimeUs>, MAX_LANES> laneEnds{};
    for (size_t i = 0; i < chart.Notes.size(); ++i)
    {
        const rhythm::ChartNote& source = chart.Notes[i];
        const uint32_t lane = lanes[i];
        const std::string context = "notes[" + std::to_string(i) + "]: ";
        if (lane >= laneCount)
        {
            return Error{context + "lane " + std::to_string(lane) + " is out of range for " +
                         std::to_string(laneCount) + " lanes"};
        }
        if (i > 0 && source.Time < chart.Notes[i - 1].Time)
        {
            return Error{context + "notes must be sorted by time"};
        }
        if (laneEnds[lane] && source.Time <= *laneEnds[lane])
        {
            return Error{context + "starts before the previous note in lane " + std::to_string(lane) + " ends"};
        }
        laneEnds[lane] = source.EndTime;

        ManiaNote note;
        note.Time = source.Time;
        note.EndTime = source.EndTime;
        note.Lane = lane;
        note.Sample = source.Sample;
        note.SampleVolume = source.SampleVolume;
        note.ScrollPosition = result.Scroll.GetPosition(source.Time);
        note.EndScrollPosition = result.Scroll.GetPosition(source.EndTime);
        result.Notes.push_back(note);
    }

    chart.Notes.clear();
    result.Info = std::move(chart);
    return result;
}

Result<ManiaChart> ParseManiaChart(std::string_view json)
{
    uint32_t laneCount = 0;
    std::vector<uint32_t> lanes;

    rhythm::ChartModeFormat format;
    format.Mode = MODE_NAME;
    format.ReadSettings = [&laneCount](const rhythm::ChartFields& settings) -> Result<void>
    {
        Result<int64_t> count = settings.GetInt("laneCount");
        if (!count)
        {
            return count.GetError();
        }
        if (count.Value() < 1 || count.Value() > MAX_LANES)
        {
            return Error{"'laneCount' must be 1 to " + std::to_string(MAX_LANES)};
        }
        laneCount = static_cast<uint32_t>(count.Value());
        return {};
    };
    format.ReadNote = [&lanes](const rhythm::ChartFields& note, size_t) -> Result<void>
    {
        Result<int64_t> lane = note.GetInt("lane");
        if (!lane)
        {
            return lane.GetError();
        }
        if (lane.Value() < 0 || lane.Value() >= MAX_LANES)
        {
            return Error{"'lane' is out of range"};
        }
        lanes.push_back(static_cast<uint32_t>(lane.Value()));
        return {};
    };

    Result<rhythm::Chart> chart = rhythm::ParseChart(json, format);
    if (!chart)
    {
        return chart.GetError();
    }
    return BuildManiaChart(std::move(chart).Value(), laneCount, lanes);
}

Result<std::string> WriteManiaChart(const ManiaChart& chart)
{
    rhythm::Chart common = chart.Info;
    common.Mode = MODE_NAME;
    common.Notes.clear();
    common.Notes.reserve(chart.Notes.size());
    for (const ManiaNote& note : chart.Notes)
    {
        common.Notes.push_back({note.Time, note.EndTime, note.Sample, note.SampleVolume});
    }

    rhythm::ChartModeFormat format;
    format.Mode = MODE_NAME;
    format.WriteSettings = [&chart](rhythm::ChartFieldWriter& settings)
    { settings.AddInt("laneCount", chart.LaneCount); };
    format.WriteNote = [&chart](rhythm::ChartFieldWriter& note, size_t index)
    { note.AddInt("lane", chart.Notes[index].Lane); };
    return rhythm::WriteChart(common, format);
}

} // namespace hyoshi::mania
