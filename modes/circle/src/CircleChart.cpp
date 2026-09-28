#include "circle/CircleChart.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hyoshi::circle
{

namespace
{

// Notes this close (playfield units) count as on top of each other for stacking.
constexpr float STACK_DISTANCE = 3.0f;

// Control points closer than this (squared) mark a Bezier anchor: the curve splits there.
constexpr float ANCHOR_DISTANCE_SQUARED = 0.01f;

// Samples for a curve before it's measured and resampled.
constexpr int DENSE_SAMPLES = 256;

// The resampled path: one point per quarter radius, within these bounds.
constexpr int MIN_PATH_SEGMENTS = 8;
constexpr int MAX_PATH_SEGMENTS = 256;

int64_t MsToUs(double milliseconds)
{
    return std::llround(milliseconds * 1000.0);
}

glm::vec2 DeCasteljau(std::vector<glm::vec2> points, float t)
{
    for (size_t degree = points.size() - 1; degree > 0; --degree)
    {
        for (size_t i = 0; i < degree; ++i)
        {
            points[i] += (points[i + 1] - points[i]) * t;
        }
    }
    return points[0];
}

// Piecewise Bezier: a control point repeated marks the end of one segment and the start of the
// next (osu!'s red anchors).
void SampleBezier(const std::vector<glm::vec2>& points, std::vector<glm::vec2>& output, int samples)
{
    std::vector<std::vector<glm::vec2>> segments;
    std::vector<glm::vec2> current{points[0]};
    for (size_t i = 1; i < points.size(); ++i)
    {
        current.push_back(points[i]);
        if (i + 1 < points.size())
        {
            const glm::vec2 step = points[i + 1] - points[i];
            if (glm::dot(step, step) < ANCHOR_DISTANCE_SQUARED)
            {
                segments.push_back(std::move(current));
                current = {points[i]};
                ++i;
            }
        }
    }
    segments.push_back(std::move(current));

    const int perSegment = std::max(4, samples / static_cast<int>(segments.size()));
    for (const std::vector<glm::vec2>& segment : segments)
    {
        for (int s = output.empty() ? 0 : 1; s <= perSegment; ++s)
        {
            output.push_back(DeCasteljau(segment, static_cast<float>(s) / static_cast<float>(perSegment)));
        }
    }
}

void SampleCatmull(const std::vector<glm::vec2>& points, std::vector<glm::vec2>& output, int samples)
{
    const int segments = static_cast<int>(points.size()) - 1;
    const int perSegment = std::max(2, samples / std::max(1, segments));
    const int last = static_cast<int>(points.size()) - 1;
    const auto at = [&](int index) { return points[static_cast<size_t>(std::clamp(index, 0, last))]; };
    for (int i = 0; i < segments; ++i)
    {
        const glm::vec2 p0 = at(i - 1);
        const glm::vec2 p1 = at(i);
        const glm::vec2 p2 = at(i + 1);
        const glm::vec2 p3 = at(i + 2);
        for (int s = i == 0 ? 0 : 1; s <= perSegment; ++s)
        {
            const float t = static_cast<float>(s) / static_cast<float>(perSegment);
            const float t2 = t * t;
            const float t3 = t2 * t;
            output.push_back(0.5f *
                             ((2.0f * p1) + ((-p0 + p2) * t) + (((2.0f * p0) - (5.0f * p1) + (4.0f * p2) - p3) * t2) +
                              ((-p0 + (3.0f * p1) - (3.0f * p2) + p3) * t3)));
        }
    }
}

// The circle through three points, from the first to the last by way of the second. Collinear
// points have no circle: a straight line instead.
void SampleArc(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, std::vector<glm::vec2>& output, int samples)
{
    const float d = 2.0f * ((p0.x * (p1.y - p2.y)) + (p1.x * (p2.y - p0.y)) + (p2.x * (p0.y - p1.y)));
    if (std::abs(d) < 1e-6f)
    {
        output.insert(output.end(), {p0, (p0 + p2) * 0.5f, p2});
        return;
    }

    const float sq0 = glm::dot(p0, p0);
    const float sq1 = glm::dot(p1, p1);
    const float sq2 = glm::dot(p2, p2);
    const glm::vec2 center{((sq0 * (p1.y - p2.y)) + (sq1 * (p2.y - p0.y)) + (sq2 * (p0.y - p1.y))) / d,
                           ((sq0 * (p2.x - p1.x)) + (sq1 * (p0.x - p2.x)) + (sq2 * (p1.x - p0.x))) / d};
    const float radius = glm::distance(center, p0);
    const float start = std::atan2(p0.y - center.y, p0.x - center.x);
    const float end = std::atan2(p2.y - center.y, p2.x - center.x);
    const bool isCounterClockwise = ((p1.x - p0.x) * (p2.y - p0.y)) - ((p1.y - p0.y) * (p2.x - p0.x)) > 0.0f;
    float span = end - start;
    constexpr float TURN = 2.0f * std::numbers::pi_v<float>;
    if (isCounterClockwise && span < 0.0f)
    {
        span += TURN;
    }
    if (!isCounterClockwise && span > 0.0f)
    {
        span -= TURN;
    }
    for (int s = 0; s <= samples; ++s)
    {
        const float angle = start + (span * static_cast<float>(s) / static_cast<float>(samples));
        output.push_back(center + (radius * glm::vec2{std::cos(angle), std::sin(angle)}));
    }
}

std::vector<glm::vec2> SampleCurve(const std::vector<glm::vec2>& points, CurveType curve, int samples)
{
    std::vector<glm::vec2> dense;
    if (points.size() < 2)
    {
        return dense;
    }
    switch (curve)
    {
    case CurveType::PerfectCircle:
        if (points.size() == 3)
        {
            SampleArc(points[0], points[1], points[2], dense, samples);
            break;
        }
        SampleBezier(points, dense, samples);
        break;
    case CurveType::Catmull:
        SampleCatmull(points, dense, samples);
        break;
    case CurveType::Linear:
        dense = points;
        break;
    case CurveType::Bezier:
        SampleBezier(points, dense, samples);
        break;
    }
    return dense;
}

// `count` + 1 points at even spacing along `dense`, up to `length` (or the whole curve, if it's
// shorter or no length is given).
std::vector<glm::vec2> ResampleByLength(const std::vector<glm::vec2>& dense, float length, int count)
{
    std::vector<float> distances(dense.size(), 0.0f);
    for (size_t i = 1; i < dense.size(); ++i)
    {
        distances[i] = distances[i - 1] + glm::distance(dense[i - 1], dense[i]);
    }
    const float total = distances.back();
    const float clip = length > 0.0f ? std::min(length, total) : total;

    std::vector<glm::vec2> result;
    result.reserve(static_cast<size_t>(count) + 1);
    size_t segment = 1;
    for (int s = 0; s <= count; ++s)
    {
        const float target = static_cast<float>(s) / static_cast<float>(count) * clip;
        while (segment + 1 < dense.size() && distances[segment] < target)
        {
            ++segment;
        }
        const float span = distances[segment] - distances[segment - 1];
        const float t = span < 1e-6f ? 0.0f : std::clamp((target - distances[segment - 1]) / span, 0.0f, 1.0f);
        result.push_back(dense[segment - 1] + ((dense[segment] - dense[segment - 1]) * t));
    }
    return result;
}

void ComputePaths(CircleChart& chart)
{
    chart.Path.clear();
    const float radius = chart.Difficulty.GetRadius();
    for (CircleNote& note : chart.Notes)
    {
        note.FirstPathPoint = 0;
        note.PathPointCount = 0;
        if (note.Type != NoteType::Slider || note.ControlPointCount < 2)
        {
            continue;
        }
        const auto first = chart.ControlPoints.begin() + note.FirstControlPoint;
        const std::vector<glm::vec2> points(first, first + note.ControlPointCount);
        const int segments =
            std::clamp(static_cast<int>(note.Length / (radius * 0.25f)) + 2, MIN_PATH_SEGMENTS, MAX_PATH_SEGMENTS);
        const std::vector<glm::vec2> dense = SampleCurve(points, note.Curve, segments * 8);
        if (dense.size() < 2)
        {
            continue;
        }
        const std::vector<glm::vec2> path = ResampleByLength(dense, note.Length, segments);
        note.FirstPathPoint = static_cast<uint32_t>(chart.Path.size());
        note.PathPointCount = static_cast<uint32_t>(path.size());
        chart.Path.insert(chart.Path.end(), path.begin(), path.end());
    }
}

void ComputeComboNumbers(CircleChart& chart)
{
    uint32_t number = 0;
    for (CircleNote& note : chart.Notes)
    {
        number = note.IsNewCombo ? 1 : number + 1;
        note.ComboNumber = number;
    }
}

// osu!lazer's OsuBeatmapProcessor.applyStacking over the whole chart. Stacks measure against a
// slider's tail (its path's end, whatever the passes), which is most of why slider-heavy charts
// stack at all. Spinners take no part.
void ComputeStacking(CircleChart& chart)
{
    std::vector<CircleNote>& notes = chart.Notes;
    for (CircleNote& note : notes)
    {
        note.StackOffset = {0.0f, 0.0f};
    }
    const CircleDifficulty& difficulty = chart.Difficulty;
    if (difficulty.StackLeniency <= 0.0f || notes.size() < 2)
    {
        return;
    }
    const auto threshold = static_cast<SongTimeUs>(
        std::llround(static_cast<double>(difficulty.GetApproachUs()) * difficulty.StackLeniency));

    const auto isExcluded = [&](size_t i) { return notes[i].Type == NoteType::Spinner; };
    const auto endOf = [&](size_t i)
    {
        const CircleNote& note = notes[i];
        return note.Type == NoteType::Slider && note.PathPointCount >= 2
                   ? chart.Path[note.FirstPathPoint + note.PathPointCount - 1]
                   : note.Position;
    };
    const auto endTimeOf = [&](size_t i) { return std::max(notes[i].EndTime, notes[i].Time); };
    const auto isNear = [](glm::vec2 a, glm::vec2 b) { return glm::distance(a, b) < STACK_DISTANCE; };

    std::vector<int> heights(notes.size(), 0);
    for (size_t i = notes.size() - 1; i > 0; --i)
    {
        if (heights[i] != 0 || isExcluded(i))
        {
            continue;
        }
        // lazer's objectI: it walks down the stack, not the outer loop.
        size_t current = i;
        if (notes[i].Type == NoteType::Circle)
        {
            for (size_t n = i; n-- > 0;)
            {
                if (isExcluded(n))
                {
                    continue;
                }
                if (notes[current].Time - endTimeOf(n) > threshold)
                {
                    break;
                }
                // A slider's tail on this circle pulls the run after the slider back down
                // instead of stacking onto it.
                if (notes[n].Type == NoteType::Slider && isNear(endOf(n), notes[current].Position))
                {
                    const int offset = heights[current] - heights[n] + 1;
                    for (size_t j = n + 1; j <= i; ++j)
                    {
                        if (isNear(endOf(n), notes[j].Position))
                        {
                            heights[j] -= offset;
                        }
                    }
                    break;
                }
                if (isNear(notes[n].Position, notes[current].Position))
                {
                    heights[n] = heights[current] + 1;
                    current = n;
                }
            }
        }
        else if (notes[i].Type == NoteType::Slider)
        {
            for (size_t n = i; n-- > 0;)
            {
                if (isExcluded(n))
                {
                    continue;
                }
                if (notes[current].Time - notes[n].Time > threshold)
                {
                    break;
                }
                if (isNear(endOf(n), notes[current].Position))
                {
                    heights[n] = heights[current] + 1;
                    current = n;
                }
            }
        }
    }

    // osu!'s scale (the radius factor, with its / 2) times 6.4 units per stack level, up and left.
    const float scale = (1.0f - (0.7f * (difficulty.CircleSize - 5.0f) / 5.0f)) * 0.5f;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        // Explicit: the slider tail cascade writes heights over a range that can include spinners.
        const float offset = isExcluded(i) ? 0.0f : -6.4f * scale * static_cast<float>(heights[i]);
        notes[i].StackOffset = {offset, offset};
    }
}

} // namespace

rhythm::Judgment CircleWindows::Classify(int64_t errorUs) const
{
    const int64_t distance = errorUs < 0 ? -errorUs : errorUs;
    if (distance <= GreatUs)
    {
        return rhythm::Judgment::Great;
    }
    return distance <= GoodUs ? rhythm::Judgment::Good : rhythm::Judgment::Bad;
}

float CircleDifficulty::GetRadius() const
{
    return 54.4f - (4.48f * CircleSize);
}

SongTimeUs CircleDifficulty::GetApproachUs() const
{
    const double ar = ApproachRate;
    if (ar < 5.0)
    {
        return MsToUs(1800.0 - (120.0 * ar));
    }
    return ar > 5.0 ? MsToUs(1200.0 - (150.0 * (ar - 5.0))) : MsToUs(1200.0);
}

CircleWindows CircleDifficulty::GetWindows() const
{
    const double od = OverallDifficulty;
    return {MsToUs(80.0 - (6.0 * od)), MsToUs(140.0 - (8.0 * od)), MsToUs(200.0 - (10.0 * od))};
}

uint32_t CircleChart::GetJudgmentCount() const
{
    return static_cast<uint32_t>(std::count_if(Notes.begin(), Notes.end(),
                                               [](const CircleNote& note) { return note.Type != NoteType::Spinner; }));
}

SongTimeUs CircleChart::GetEndTime() const
{
    SongTimeUs end = 0;
    for (const CircleNote& note : Notes)
    {
        end = std::max({end, note.Time, note.EndTime});
    }
    return end;
}

void FinishCircleChart(CircleChart& chart)
{
    ComputeComboNumbers(chart);
    ComputePaths(chart);
    ComputeStacking(chart);
}

glm::vec2 GetPathPosition(const CircleChart& chart, const CircleNote& note, float progress)
{
    if (note.PathPointCount < 2)
    {
        return note.GetStackedPosition();
    }
    const float position = std::clamp(progress, 0.0f, 1.0f) * static_cast<float>(note.PathPointCount - 1);
    const auto index = std::min(static_cast<uint32_t>(position), note.PathPointCount - 2);
    const glm::vec2 a = chart.Path[note.FirstPathPoint + index];
    const glm::vec2 b = chart.Path[note.FirstPathPoint + index + 1];
    return a + ((b - a) * (position - static_cast<float>(index))) + note.StackOffset;
}

glm::vec2 GetBallPosition(const CircleChart& chart, const CircleNote& note, SongTimeUs time)
{
    const SongTimeUs span = note.EndTime - note.Time;
    if (span <= 0)
    {
        return GetPathPosition(chart, note, 0.0f);
    }
    const double passes = std::max<uint32_t>(note.Passes, 1);
    const double along =
        std::clamp(static_cast<double>(time - note.Time) / static_cast<double>(span), 0.0, 1.0) * passes;
    const double pass = std::min(std::floor(along), passes - 1.0);
    const double within = along - pass;
    // Even passes run head to tail, odd ones back.
    const bool isForward = static_cast<int64_t>(pass) % 2 == 0;
    return GetPathPosition(chart, note, static_cast<float>(isForward ? within : 1.0 - within));
}

std::vector<glm::vec2> SampleCurve(const std::vector<glm::vec2>& points, CurveType curve)
{
    return SampleCurve(points, curve, DENSE_SAMPLES);
}

} // namespace hyoshi::circle
