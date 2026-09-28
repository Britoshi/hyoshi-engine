#pragma once

#include "rhythm/Chart.h"

#include "core/Time.h"

#include <span>
#include <vector>

namespace hyoshi::rhythm
{

// Scroll position: the integral of scroll velocity over song time (DESIGN.md section 10.5), in
// seconds at 1x speed. Notes store the position at their time; each frame computes the current
// position once, and a note's distance from the judgment line is the difference. Speed changes,
// stops (multiplier 0), and reverse scrolling (negative) all follow from the integral.
//
// Before the first point, and without points, the multiplier is 1. Position 0 is song time 0.
// Floating point is fine here: positions only affect drawing, never judgment.
class ScrollMap
{
public:
    ScrollMap() = default;

    // Points must be sorted by time.
    explicit ScrollMap(std::span<const ScrollVelocityPoint> points);

    double GetPosition(SongTimeUs time) const;

private:
    struct Segment
    {
        SongTimeUs Start = 0;
        double Multiplier = 1.0;
        double StartPosition = 0.0;
    };

    // Segments[0] starts at time 0 and extends backwards to negative times.
    std::vector<Segment> segments{Segment{}};
};

} // namespace hyoshi::rhythm
