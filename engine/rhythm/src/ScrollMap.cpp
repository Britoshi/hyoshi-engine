#include "rhythm/ScrollMap.h"

#include <algorithm>

namespace hyoshi::rhythm
{

namespace
{

constexpr double US_TO_SECONDS = 1.0e-6;

} // namespace

ScrollMap::ScrollMap(std::span<const ScrollVelocityPoint> points)
{
    for (const ScrollVelocityPoint& point : points)
    {
        // Points at or before time 0 set the starting multiplier.
        if (point.Time <= 0)
        {
            segments.front().Multiplier = point.Multiplier;
            continue;
        }

        const Segment& previous = segments.back();
        if (point.Time == previous.Start)
        {
            segments.back().Multiplier = point.Multiplier;
            continue;
        }

        Segment segment;
        segment.Start = point.Time;
        segment.Multiplier = point.Multiplier;
        segment.StartPosition = previous.StartPosition + (static_cast<double>(point.Time - previous.Start) *
                                                          previous.Multiplier * US_TO_SECONDS);
        segments.push_back(segment);
    }
}

double ScrollMap::GetPosition(SongTimeUs time) const
{
    // The last segment starting at or before `time`, or the first for negative times.
    auto next = std::upper_bound(segments.begin(), segments.end(), time,
                                 [](SongTimeUs value, const Segment& segment) { return value < segment.Start; });
    const Segment& segment = next == segments.begin() ? segments.front() : *(next - 1);
    return segment.StartPosition + (static_cast<double>(time - segment.Start) * segment.Multiplier * US_TO_SECONDS);
}

} // namespace hyoshi::rhythm
