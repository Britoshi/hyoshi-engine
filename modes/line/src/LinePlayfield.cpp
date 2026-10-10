#include "line/LinePlayfield.h"

#include <algorithm>
#include <numbers>

namespace hyoshi::line
{

namespace
{

using renderer::Color;
using renderer::Sprite;
using renderer::WithAlpha;

constexpr int32_t LINE_LAYER = 0;
constexpr int32_t NOTE_LAYER = 1;
constexpr int32_t BURST_LAYER = 2;

// Notes are NOTE_WIDTH X_UNITs wide, as Phigros's are (about a tenth of the field), and
// NOTE_HEIGHT field units thick; drags are thinner. Lines run far past the field either way.
constexpr float NOTE_WIDTH = 1.9f;
constexpr float NOTE_HEIGHT = 22.0f;
constexpr float DRAG_HEIGHT = 0.6f;
constexpr float HOLD_BODY_WIDTH = 0.85f;
constexpr float FLICK_MARK_WIDTH = 0.5f;
constexpr float FLICK_MARK_HEIGHT = 0.35f;
constexpr float LINE_THICKNESS = 6.0f;
constexpr float LINE_LENGTH = FIELD_WIDTH * 4.0f;
// Notes farther than this from their line, or due further ahead, aren't drawn yet.
constexpr float DRAW_DISTANCE = FIELD_HEIGHT * 2.0f;
constexpr SongTimeUs DRAW_AHEAD_US = 10'000'000;

constexpr Color LINE_TINT{255, 236, 160, 255};
constexpr Color TAP_TINT{10, 195, 255, 255};
constexpr Color DRAG_TINT{240, 237, 105, 255};
constexpr Color FLICK_TINT{254, 67, 101, 255};
constexpr Color HOLD_TINT{10, 195, 255, 255};
constexpr Color PERFECT_TINT{255, 236, 160, 255};
constexpr Color GOOD_TINT{180, 230, 255, 255};

// A hit bursts into a square that grows and fades.
constexpr SongTimeUs BURST_US = 280'000;
constexpr float BURST_SIZE = 150.0f;

Color GetNoteTint(NoteKind kind)
{
    switch (kind)
    {
    case NoteKind::Tap:
        break;
    case NoteKind::Hold:
        return HOLD_TINT;
    case NoteKind::Drag:
        return DRAG_TINT;
    case NoteKind::Flick:
        return FLICK_TINT;
    }
    return TAP_TINT;
}

} // namespace

LineLayout LineLayout::Fit(glm::vec2 canvas)
{
    LineLayout layout;
    layout.Scale = std::min(canvas.x / FIELD_WIDTH, canvas.y / FIELD_HEIGHT);
    layout.Origin = (canvas - (glm::vec2{FIELD_WIDTH, FIELD_HEIGHT} * layout.Scale)) * 0.5f;
    return layout;
}

void LinePlayfield::Reset(const LineChart& /*chart*/)
{
    Burst none;
    none.Time = INT64_MIN;
    bursts.assign(BURST_COUNT, none);
    nextBurst = 0;
}

void LinePlayfield::OnJudgment(const LineChart& chart, const rhythm::JudgmentEvent& event)
{
    if (event.Result == rhythm::Judgment::Miss || event.NoteIndex >= chart.Notes.size() || bursts.empty())
    {
        return;
    }
    const LineNote& note = chart.Notes[event.NoteIndex];
    const LineState state = GetLineState(chart.Lines[note.Line], event.Time);
    Burst& burst = bursts[nextBurst];
    nextBurst = (nextBurst + 1) % bursts.size();
    burst.Time = event.Time;
    burst.Position = state.Position + (state.Along * (note.X * X_UNIT));
    burst.Angle = state.Angle;
    burst.Result = event.Result;
}

void LinePlayfield::Draw(renderer::SpriteBatch& sprites, const LineChart& chart, const LineJudge& judge,
                         SongTimeUs time, const LineLayout& layout, int32_t firstLayer) const
{
    const auto draw = [&](glm::vec2 center, glm::vec2 size, float angle, Color tint, int32_t layer)
    {
        Sprite sprite;
        sprite.Center = layout.ToCanvas(center);
        sprite.Size = size * layout.Scale;
        sprite.Rotation = angle;
        sprite.Tint = tint;
        sprite.Layer = firstLayer + layer;
        sprites.Draw(sprite);
    };

    for (const JudgeLine& line : chart.Lines)
    {
        const LineState state = GetLineState(line, time);
        if (state.Opacity > 0.0f)
        {
            draw(state.Position, {LINE_LENGTH, LINE_THICKNESS}, state.Angle, WithAlpha(LINE_TINT, state.Opacity),
                 LINE_LAYER);
        }
    }

    // Hold bodies, then heads, so heads sit on bodies.
    const float noteWidth = NOTE_WIDTH * X_UNIT;
    for (uint32_t i = judge.GetFirstActiveNote(); i < chart.Notes.size() && chart.Notes[i].Time <= time + DRAW_AHEAD_US;
         ++i)
    {
        const LineNote& note = chart.Notes[i];
        const NoteState state = judge.GetNoteState(i);
        if (state == NoteState::Hit || state == NoteState::Missed)
        {
            continue;
        }
        const LineState line = GetLineState(chart.Lines[note.Line], time);
        const float side = note.IsAbove ? 1.0f : -1.0f;
        const glm::vec2 onLine = line.Position + (line.Along * (note.X * X_UNIT));
        // A held note's head stays on the line while its body drains into it.
        const float head = state == NoteState::Held ? 0.0f : GetNoteDistance(chart, note, time);
        const Color tint = GetNoteTint(note.Kind);
        if (note.IsHold())
        {
            const float end =
                static_cast<float>((note.EndFloorPosition - GetFloorPosition(chart.Lines[note.Line], time)) *
                                   note.Speed) *
                DISTANCE_UNIT;
            const float near = std::max(head, 0.0f);
            const float far = std::min(end, DRAW_DISTANCE);
            if (far > near)
            {
                const glm::vec2 middle = onLine + (line.Up * (side * (near + far) * 0.5f));
                draw(middle, {noteWidth * HOLD_BODY_WIDTH, far - near}, line.Angle,
                     WithAlpha(tint, state == NoteState::Held ? 0.75f : 0.5f), NOTE_LAYER);
            }
        }
        // Past its line, it's gone (a late strike still counts); too far ahead, not yet drawn.
        if (head < 0.0f || head > DRAW_DISTANCE)
        {
            continue;
        }
        const glm::vec2 center = onLine + (line.Up * (side * head));
        const float height = NOTE_HEIGHT * (note.Kind == NoteKind::Drag ? DRAG_HEIGHT : 1.0f);
        draw(center, {noteWidth, height}, line.Angle, tint, NOTE_LAYER);
        if (note.Kind == NoteKind::Flick)
        {
            draw(center, {noteWidth * FLICK_MARK_WIDTH, height * FLICK_MARK_HEIGHT}, line.Angle, renderer::WHITE,
                 NOTE_LAYER);
        }
    }

    for (const Burst& burst : bursts)
    {
        if (burst.Time == INT64_MIN || time < burst.Time || time - burst.Time >= BURST_US)
        {
            continue;
        }
        const float progress = static_cast<float>(time - burst.Time) / static_cast<float>(BURST_US);
        const float size = BURST_SIZE * (0.6f + (0.8f * progress));
        const Color tint = burst.Result == rhythm::Judgment::Perfect ? PERFECT_TINT : GOOD_TINT;
        draw(burst.Position, {size, size}, burst.Angle + (std::numbers::pi_v<float> * 0.25f),
             WithAlpha(tint, 0.7f * (1.0f - progress)), BURST_LAYER);
    }
}

} // namespace hyoshi::line
