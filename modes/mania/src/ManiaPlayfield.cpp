#include "mania/ManiaPlayfield.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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
// The judgment banner's height above the judgment line.
constexpr float BANNER_HEIGHT_ABOVE = 340.0f;

// The track (PlayfieldView::Track) vanishes this far down the canvas, from the top. The eye is
// this many judgment-line-to-horizon heights behind the judgment line: nearer is steeper. The far
// end is drawn at this scale of the judgment line, and notes fade in over the last part of the way.
constexpr float TRACK_HORIZON = 0.08f;
constexpr float TRACK_EYE_DISTANCE = 1.15f;
constexpr float TRACK_FAR_SCALE = 0.2f;
constexpr float TRACK_FADE = 0.15f;

// Notes appearing at the track's far end flare up and settle over this part of the way: a glow
// that shrinks onto the note, a beam of light rising from it, and its lane's end of the track
// lighting up. The far end glows faintly all the time.
constexpr float SPAWN_PART = 0.35f;
constexpr float SPAWN_GLOW_ACROSS = 0.22f;
constexpr float SPAWN_GLOW_ALONG = 1.4f;
constexpr float SPAWN_GLOW_ALPHA = 0.16f;
constexpr int SPAWN_GLOW_RINGS = 3;
constexpr float SPAWN_BEAM_HEIGHT = 120.0f;
constexpr float SPAWN_BEAM_ALPHA = 0.45f;
constexpr int SPAWN_BEAM_STEPS = 6;
constexpr float GATE_ALPHA = 0.3f;
constexpr float GATE_DEPTH = 10.0f;

constexpr Color TRACK_FLOOR{14, 14, 22, 235};
constexpr Color TRACK_LINE{45, 45, 65, 255};
constexpr Color TRACK_EDGE{140, 140, 200, 255};

// The track as seen in perspective. A point on it is `across` the lanes (canvas units at the
// judgment line, from the middle) and `along` them (scroll units past the judgment line, negative
// before it): it's drawn at Eye / (Eye + along) of its size at the judgment line.
struct TrackProjection
{
    TrackProjection(glm::vec2 canvas, float judgeY, float centerX)
        : CenterX(centerX), HorizonY(canvas.y * TRACK_HORIZON), Height(std::max(judgeY - HorizonY, 1.0f)),
          Eye(Height * TRACK_EYE_DISTANCE), NearAlong((Eye * Height / std::max(canvas.y - HorizonY, 1.0f)) - Eye),
          FarAlong((Eye / TRACK_FAR_SCALE) - Eye)
    {
    }

    // How large the track is drawn `along` it, against the judgment line.
    float GetScale(float along) const
    {
        return Eye / (Eye + along);
    }

    glm::vec2 Project(float across, float along) const
    {
        const float scale = GetScale(along);
        return {CenterX + (across * scale), HorizonY + (Height * scale)};
    }

    // A quad on the track between two lines across it, clipped to the part on screen.
    void DrawQuad(renderer::SpriteBatch& sprites, float left, float right, float near, float far, Color color,
                  int32_t layer) const
    {
        near = std::max(near, NearAlong);
        far = std::min(far, FarAlong);
        if (far <= near || right <= left)
        {
            return;
        }
        sprites.DrawQuad(Project(left, far), Project(right, far), Project(left, near), Project(right, near), color,
                         layer);
    }

    float CenterX;
    float HorizonY;
    // From the judgment line up to the horizon.
    float Height;
    float Eye;
    // The bottom of the canvas, and the track's far end.
    float NearAlong;
    float FarAlong;
};

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

// Halfway to white, for light.
Color Brighten(Color color)
{
    const auto mix = [](uint8_t channel) { return static_cast<uint8_t>((channel + 255) / 2); };
    return {mix(color.R), mix(color.G), mix(color.B), color.A};
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
    return GetCenterX(canvas, style) - (style.LaneWidth * static_cast<float>(laneCount) * 0.5f);
}

float ManiaPlayfield::GetCenterX(glm::vec2 canvas, const PlayfieldStyle& style)
{
    return style.CenterX < 0.0f ? canvas.x * 0.5f : style.CenterX;
}

float ManiaPlayfield::GetJudgeLineX(glm::vec2 point, glm::vec2 canvas, const PlayfieldStyle& style) const
{
    if (style.View != PlayfieldView::Track)
    {
        return point.x;
    }
    const TrackProjection track(canvas, canvas.y - style.JudgeLineFromBottom, GetCenterX(canvas, style));
    // Above the far end, as if at the far end.
    const float scale = std::max((point.y - track.HorizonY) / track.Height, TRACK_FAR_SCALE);
    return track.CenterX + ((point.x - track.CenterX) / scale);
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
                          const ILaneJudge& judge, SongTimeUs time, const PlayfieldStyle& style) const
{
    if (style.View == PlayfieldView::Track)
    {
        DrawTrack(sprites, canvas, chart, judge, time, style);
        DrawMeters(sprites, canvas, time, style);
        return;
    }
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
    DrawMeters(sprites, canvas, time, style);
}

// The same as the flat playfield, laid on the track: everything is a quad between two lines
// across it.
void ManiaPlayfield::DrawTrack(renderer::SpriteBatch& sprites, glm::vec2 canvas, const ManiaChart& chart,
                               const ILaneJudge& judge, SongTimeUs time, const PlayfieldStyle& style) const
{
    const TrackProjection track(canvas, canvas.y - style.JudgeLineFromBottom, GetCenterX(canvas, style));
    const float half = style.LaneWidth * static_cast<float>(laneCount) * 0.5f;
    const auto laneLeft = [&](uint32_t lane) { return -half + (static_cast<float>(lane) * style.LaneWidth); };

    // The floor, the lines between lanes, and brighter edges.
    track.DrawQuad(sprites, -half, half, track.NearAlong, track.FarAlong, TRACK_FLOOR, BACKGROUND_LAYER);
    for (uint32_t lane = 0; lane <= laneCount; ++lane)
    {
        const bool isEdge = lane == 0 || lane == laneCount;
        const float lineHalf = isEdge ? 3.0f : 1.0f;
        const float x = laneLeft(lane);
        track.DrawQuad(sprites, x - lineHalf, x + lineHalf, track.NearAlong, track.FarAlong,
                       isEdge ? TRACK_EDGE : TRACK_LINE, BACKGROUND_LAYER);
    }
    for (uint32_t lane = 0; lane < laneCount; ++lane)
    {
        const float left = laneLeft(lane);
        const float right = left + style.LaneWidth;
        const Color laneColor = GetLaneColor(lane, laneCount);
        const bool isHeld = judge.IsLaneHeld(lane);
        track.DrawQuad(sprites, left + 4.0f, right - 4.0f, track.NearAlong, -10.0f,
                       WithAlpha(laneColor, isHeld ? 0.55f : 0.12f), BACKGROUND_LAYER);
        if (isHeld)
        {
            track.DrawQuad(sprites, left + 2.0f, right - 2.0f, 0.0f, 260.0f, WithAlpha(laneColor, 0.08f),
                           BACKGROUND_LAYER);
        }
        const Feedback& feedback = laneFeedback[lane];
        const float flash = Fade(time, feedback.Time, LANE_FLASH_US);
        if (flash > 0.0f && feedback.Result != rhythm::Judgment::Miss)
        {
            track.DrawQuad(sprites, left + 2.0f, right - 2.0f, 0.0f, 120.0f,
                           WithAlpha(GetJudgmentColor(feedback.Result), flash * 0.5f), OVERLAY_LAYER);
        }
    }
    track.DrawQuad(sprites, -half, half, -3.0f, 3.0f, {240, 240, 255, 255}, OVERLAY_LAYER);

    // Notes, from the far end of the track, fading in there.
    const double scroll = chart.Scroll.GetPosition(time);
    const auto along = [&](double position)
    { return static_cast<float>((position - scroll) * static_cast<double>(style.ScrollSpeed)); };
    const auto fadeIn = [&](float distance)
    { return std::clamp((track.FarAlong - distance) / (track.FarAlong * TRACK_FADE), 0.0f, 1.0f); };
    // 1 as a note appears at the far end, down to 0 once it has settled.
    const auto flare = [&](float distance)
    {
        const float settled = std::clamp((track.FarAlong - distance) / (track.FarAlong * SPAWN_PART), 0.0f, 1.0f);
        return (1.0f - settled) * (1.0f - settled);
    };
    std::array<float, MAX_LANES> laneFlare{};

    const auto recent = std::lower_bound(chart.Notes.begin(), chart.Notes.end(), time - DRAW_BEHIND_US,
                                         [](const ManiaNote& note, SongTimeUs value) { return note.Time < value; });
    const size_t first = std::min(static_cast<size_t>(recent - chart.Notes.begin()),
                                  static_cast<size_t>(judge.GetFirstUnfinishedNote()));
    for (size_t i = first; i < chart.Notes.size() && chart.Notes[i].Time <= time + DRAW_AHEAD_US; ++i)
    {
        const ManiaNote& note = chart.Notes[i];
        const NoteState state = judge.GetNoteState(static_cast<uint32_t>(i));
        if (state == NoteState::Hit)
        {
            continue;
        }
        // A held note's head stays on the judgment line while its body drains into it.
        const float head = state == NoteState::Held ? 0.0f : along(note.ScrollPosition);
        if (head - style.NoteHeight > track.FarAlong)
        {
            continue;
        }
        const float alpha = state == NoteState::Missed ? 0.3f : 1.0f;
        const Color color = GetLaneColor(note.Lane, laneCount);
        const float left = laneLeft(note.Lane) + 6.0f;
        const float right = laneLeft(note.Lane) + style.LaneWidth - 6.0f;
        if (note.IsHold())
        {
            const float tail = along(note.EndScrollPosition);
            const float inset = (right - left) * 0.15f;
            track.DrawQuad(sprites, left + inset, right - inset, std::min(head, tail), std::max(head, tail),
                           WithAlpha(color, alpha * (state == NoteState::Held ? 0.75f : 0.45f)), BODY_LAYER);
            track.DrawQuad(sprites, left, right, tail - 4.0f, tail + 4.0f, WithAlpha(color, alpha * fadeIn(tail)),
                           NOTE_LAYER);
        }
        const float halfHeight = style.NoteHeight * 0.5f;
        track.DrawQuad(sprites, left, right, head - halfHeight, head + halfHeight,
                       WithAlpha(color, alpha * fadeIn(head)), NOTE_LAYER);

        // Appearing: a glow around the note, wider while it's new, and a beam of light rising from it.
        const float spawn = state == NoteState::Pending ? flare(head) : 0.0f;
        if (spawn <= 0.0f)
        {
            continue;
        }
        laneFlare[note.Lane] = std::max(laneFlare[note.Lane], spawn);
        const Color glow = Brighten(color);
        for (int ring = 1; ring <= SPAWN_GLOW_RINGS; ++ring)
        {
            const float grow = spawn * static_cast<float>(ring) / static_cast<float>(SPAWN_GLOW_RINGS);
            const float across = style.LaneWidth * SPAWN_GLOW_ACROSS * grow;
            const float extent = halfHeight * (1.0f + (SPAWN_GLOW_ALONG * grow));
            track.DrawQuad(sprites, left - across, right + across, head - extent, head + extent,
                           WithAlpha(glow, SPAWN_GLOW_ALPHA * spawn), BODY_LAYER);
        }
        const float scale = track.GetScale(head);
        const glm::vec2 base = track.Project((left + right) * 0.5f, head + halfHeight);
        // A wide soft beam and a bright core, each fading toward the top in steps.
        const float beamHeight = SPAWN_BEAM_HEIGHT * spawn;
        const float stepHeight = beamHeight / static_cast<float>(SPAWN_BEAM_STEPS);
        for (const auto& [widthPart, alphaPart] : {std::pair{0.8f, 0.45f}, std::pair{0.25f, 1.0f}})
        {
            const float beamHalf = (right - left) * scale * widthPart * 0.5f;
            for (int step = 0; step < SPAWN_BEAM_STEPS; ++step)
            {
                const float fade = 1.0f - (static_cast<float>(step) / static_cast<float>(SPAWN_BEAM_STEPS));
                const float bottom = base.y - (static_cast<float>(step) * stepHeight);
                sprites.DrawRect({base.x - beamHalf, bottom - stepHeight}, {beamHalf * 2.0f, stepHeight},
                                 WithAlpha(glow, SPAWN_BEAM_ALPHA * alphaPart * fade * spawn), BODY_LAYER);
            }
        }
    }

    // The far end: a faint line across the track, each lane brighter while a note appears in it.
    for (uint32_t lane = 0; lane < laneCount; ++lane)
    {
        const float gate = GATE_ALPHA + ((1.0f - GATE_ALPHA) * laneFlare[lane]);
        track.DrawQuad(sprites, laneLeft(lane), laneLeft(lane) + style.LaneWidth, track.FarAlong - GATE_DEPTH,
                       track.FarAlong, WithAlpha(Brighten(GetLaneColor(lane, laneCount)), gate), OVERLAY_LAYER);
    }
}

void ManiaPlayfield::DrawMeters(renderer::SpriteBatch& sprites, glm::vec2 canvas, SongTimeUs time,
                                const PlayfieldStyle& style) const
{
    const float left = GetLeft(canvas, style);
    const float width = style.LaneWidth * static_cast<float>(laneCount);
    const float judgeY = canvas.y - style.JudgeLineFromBottom;

    // The latest judgment as a colored banner above the lanes, wider for better judgments.
    const float banner = Fade(time, lastJudgment.Time, BANNER_US);
    if (banner > 0.0f)
    {
        const float scale = 1.0f - (0.15f * static_cast<float>(lastJudgment.Result));
        float bannerWidth = width * scale;
        // On a track, as wide as the track is that far up.
        if (style.View == PlayfieldView::Track)
        {
            const TrackProjection track(canvas, judgeY, GetCenterX(canvas, style));
            bannerWidth *= std::max((judgeY - BANNER_HEIGHT_ABOVE - track.HorizonY) / track.Height, TRACK_FAR_SCALE);
        }
        sprites.DrawRect({left + ((width - bannerWidth) * 0.5f), judgeY - BANNER_HEIGHT_ABOVE}, {bannerWidth, 18.0f},
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
