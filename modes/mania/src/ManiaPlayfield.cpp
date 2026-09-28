#include "mania/ManiaPlayfield.h"

#include <algorithm>

namespace hyoshi::mania
{

using renderer::Color;

namespace
{

constexpr int32_t BACKGROUND_LAYER = 10;
constexpr int32_t BODY_LAYER = 11;
constexpr int32_t NOTE_LAYER = 12;
constexpr int32_t OVERLAY_LAYER = 13;

constexpr SongTimeUs LANE_FLASH_US = 150'000;
constexpr SongTimeUs BANNER_US = 400'000;
constexpr SongTimeUs ERROR_TICK_US = 3'000'000;
// Notes this far behind are no longer drawn, and this far ahead not yet.
constexpr SongTimeUs DRAW_BEHIND_US = 1'000'000;
constexpr SongTimeUs DRAW_AHEAD_US = 10'000'000;

constexpr float ERROR_METER_HALF_WIDTH = 200.0f;

// Mirrored lane colors, like most Mania skins: outer lanes white, next ones blue, a gold center
// lane when there is one.
Color GetLaneColor(uint32_t lane, uint32_t laneCount)
{
    if (laneCount % 2 == 1 && lane == laneCount / 2)
    {
        return {250, 210, 90, 255};
    }
    const uint32_t fromEdge = std::min(lane, laneCount - 1 - lane);
    return fromEdge % 2 == 0 ? Color{235, 235, 245, 255} : Color{110, 180, 255, 255};
}

// Fades from 1 to 0 over `duration` after `start`. A start of INT64_MIN never happened.
float Fade(SongTimeUs now, SongTimeUs start, SongTimeUs duration)
{
    if (start == INT64_MIN)
    {
        return 0.0f;
    }
    if (now < start)
    {
        return 1.0f;
    }
    const SongTimeUs elapsed = now - start;
    if (elapsed >= duration)
    {
        return 0.0f;
    }
    return 1.0f - (static_cast<float>(elapsed) / static_cast<float>(duration));
}

} // namespace

void ManiaPlayfield::Reset(uint32_t lanes)
{
    *this = ManiaPlayfield{};
    laneCount = lanes;
}

void ManiaPlayfield::OnJudgment(const rhythm::JudgmentEvent& event, uint32_t lane)
{
    const Feedback feedback{event.Time, event.Result, event.ErrorUs};
    lastJudgment = feedback;
    if (lane < laneFeedback.size())
    {
        laneFeedback[lane] = feedback;
    }
    if (event.Result != rhythm::Judgment::Miss)
    {
        recentErrors[errorCursor] = feedback;
        errorCursor = (errorCursor + 1) % ERROR_HISTORY;
    }
}

Color ManiaPlayfield::GetJudgmentColor(rhythm::Judgment judgment)
{
    switch (judgment)
    {
    case rhythm::Judgment::Perfect:
        return {130, 235, 255, 255};
    case rhythm::Judgment::Great:
        return {120, 230, 120, 255};
    case rhythm::Judgment::Good:
        return {240, 220, 90, 255};
    case rhythm::Judgment::Bad:
        return {240, 140, 60, 255};
    case rhythm::Judgment::Miss:
    case rhythm::Judgment::Count:
        break;
    }
    return {230, 70, 70, 255};
}

float ManiaPlayfield::GetLeft(glm::vec2 canvas, const PlayfieldStyle& style) const
{
    return (canvas.x - (style.LaneWidth * static_cast<float>(laneCount))) * 0.5f;
}

std::optional<uint32_t> ManiaPlayfield::GetLaneAt(float x, glm::vec2 canvas, const PlayfieldStyle& style) const
{
    const float relative = (x - GetLeft(canvas, style)) / style.LaneWidth;
    if (relative < 0.0f || relative >= static_cast<float>(laneCount))
    {
        return std::nullopt;
    }
    return static_cast<uint32_t>(relative);
}

void ManiaPlayfield::Draw(renderer::SpriteBatch& sprites, glm::vec2 canvas, const ManiaChart& chart,
                          const ManiaJudge& judge, SongTimeUs time, const PlayfieldStyle& style) const
{
    const float left = GetLeft(canvas, style);
    const float width = style.LaneWidth * static_cast<float>(laneCount);
    const float judgeY = canvas.y - style.JudgeLineFromBottom;

    // Lanes, separators, and the receptor area below the judgment line.
    sprites.DrawRect({left, 0.0f}, {width, canvas.y}, {14, 14, 22, 235}, BACKGROUND_LAYER);
    for (uint32_t lane = 0; lane <= laneCount; ++lane)
    {
        const float x = left + (static_cast<float>(lane) * style.LaneWidth);
        sprites.DrawRect({x - 1.0f, 0.0f}, {2.0f, canvas.y}, {45, 45, 65, 255}, BACKGROUND_LAYER);
    }
    for (uint32_t lane = 0; lane < laneCount; ++lane)
    {
        const float x = left + (static_cast<float>(lane) * style.LaneWidth);
        const Color laneColor = GetLaneColor(lane, laneCount);
        const bool isHeld = judge.IsLaneHeld(lane);
        sprites.DrawRect({x + 4.0f, judgeY + 14.0f}, {style.LaneWidth - 8.0f, style.JudgeLineFromBottom - 28.0f},
                         WithAlpha(laneColor, isHeld ? 0.55f : 0.12f), BACKGROUND_LAYER);
        if (isHeld)
        {
            // A soft column of light above a pressed lane.
            sprites.DrawRect({x + 2.0f, judgeY - 260.0f}, {style.LaneWidth - 4.0f, 260.0f}, WithAlpha(laneColor, 0.08f),
                             BACKGROUND_LAYER);
        }

        const Feedback& feedback = laneFeedback[lane];
        const float flash = Fade(time, feedback.Time, LANE_FLASH_US);
        if (flash > 0.0f && feedback.Result != rhythm::Judgment::Miss)
        {
            sprites.DrawRect({x + 2.0f, judgeY - 120.0f}, {style.LaneWidth - 4.0f, 120.0f},
                             WithAlpha(GetJudgmentColor(feedback.Result), flash * 0.5f), OVERLAY_LAYER);
        }
    }
    sprites.DrawRect({left, judgeY - 3.0f}, {width, 6.0f}, {240, 240, 255, 255}, OVERLAY_LAYER);

    // Notes. Start at whichever comes first: an unfinished note (a hold still in progress) or the
    // first note recent enough to still be on screen.
    const double scroll = chart.Scroll.GetPosition(time);
    auto noteY = [&](double position)
    { return judgeY - static_cast<float>((position - scroll) * static_cast<double>(style.ScrollSpeed)); };

    const auto recent = std::lower_bound(chart.Notes.begin(), chart.Notes.end(), time - DRAW_BEHIND_US,
                                         [](const ManiaNote& note, SongTimeUs value) { return note.Time < value; });
    size_t first = std::min(static_cast<size_t>(recent - chart.Notes.begin()),
                            static_cast<size_t>(judge.GetFirstUnfinishedNote()));
    for (size_t i = first; i < chart.Notes.size() && chart.Notes[i].Time <= time + DRAW_AHEAD_US; ++i)
    {
        const ManiaNote& note = chart.Notes[i];
        const NoteState state = judge.GetNoteState(static_cast<uint32_t>(i));
        if (state == NoteState::Hit)
        {
            continue;
        }

        const bool isMissed = state == NoteState::Missed;
        const Color color = WithAlpha(GetLaneColor(note.Lane, laneCount), isMissed ? 0.3f : 1.0f);
        const float x = left + (static_cast<float>(note.Lane) * style.LaneWidth) + 6.0f;
        const float noteWidth = style.LaneWidth - 12.0f;

        // A held note's head stays on the judgment line while its body drains into it.
        float headY = noteY(note.ScrollPosition);
        if (state == NoteState::Held)
        {
            headY = judgeY;
        }
        if (note.IsHold())
        {
            const float tailY = noteY(note.EndScrollPosition);
            const float top = std::min(tailY, headY);
            const float bottom = std::max(tailY, headY);
            if (bottom >= 0.0f && top <= canvas.y)
            {
                sprites.DrawRect({x + (noteWidth * 0.15f), top}, {noteWidth * 0.7f, bottom - top},
                                 WithAlpha(color, state == NoteState::Held ? 0.75f : 0.45f), BODY_LAYER);
                sprites.DrawRect({x, tailY - 4.0f}, {noteWidth, 8.0f}, color, NOTE_LAYER);
            }
        }
        if (headY + style.NoteHeight >= 0.0f && headY - style.NoteHeight <= canvas.y)
        {
            sprites.DrawRect({x, headY - (style.NoteHeight * 0.5f)}, {noteWidth, style.NoteHeight}, color, NOTE_LAYER);
        }
    }

    // The latest judgment as a colored banner above the lanes, wider for better judgments.
    const float banner = Fade(time, lastJudgment.Time, BANNER_US);
    if (banner > 0.0f)
    {
        const float scale = 1.0f - (0.15f * static_cast<float>(lastJudgment.Result));
        const float bannerWidth = width * scale;
        sprites.DrawRect({left + ((width - bannerWidth) * 0.5f), judgeY - 340.0f}, {bannerWidth, 18.0f},
                         WithAlpha(GetJudgmentColor(lastJudgment.Result), banner), OVERLAY_LAYER);
    }

    // Timing error meter under the lanes: early to the left, late to the right, fading ticks.
    const float meterY = judgeY + style.JudgeLineFromBottom - 40.0f;
    const float centerX = left + (width * 0.5f);
    const int64_t badUs = rhythm::HitWindows{}.BadUs;
    sprites.DrawRect({centerX - ERROR_METER_HALF_WIDTH, meterY}, {ERROR_METER_HALF_WIDTH * 2.0f, 4.0f},
                     {60, 60, 80, 255}, OVERLAY_LAYER);
    sprites.DrawRect({centerX - 1.0f, meterY - 10.0f}, {2.0f, 24.0f}, {240, 240, 255, 255}, OVERLAY_LAYER);
    for (const Feedback& tick : recentErrors)
    {
        const float fade = Fade(time, tick.Time, ERROR_TICK_US);
        if (fade <= 0.0f)
        {
            continue;
        }
        const float offset = std::clamp(static_cast<float>(tick.ErrorUs) / static_cast<float>(badUs), -1.0f, 1.0f) *
                             ERROR_METER_HALF_WIDTH;
        sprites.DrawRect({centerX + offset - 1.5f, meterY - 8.0f}, {3.0f, 20.0f},
                         WithAlpha(GetJudgmentColor(tick.Result), fade), OVERLAY_LAYER);
    }
}

} // namespace hyoshi::mania
