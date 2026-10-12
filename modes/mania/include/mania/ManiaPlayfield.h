#pragma once

#include "mania/ManiaChart.h"
#include "mania/ManiaJudge.h"

#include "renderer/SpriteBatch.h"
#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::mania
{

// How the lanes are laid out.
enum class PlayfieldView : uint8_t
{
    // Straight down the screen.
    Flat,
    // On a floor that recedes toward the top of the screen, as in Arcaea: notes come down the
    // track from its far end. The lanes are LaneWidth wide at the judgment line.
    Track
};

struct PlayfieldStyle
{
    PlayfieldView View = PlayfieldView::Flat;
    float LaneWidth = 120.0f;
    // Canvas units on flat lanes; on the track, as long in time as that (the track stretches with
    // its length, like the scroll).
    float NoteHeight = 34.0f;
    // Distance from the bottom of the canvas to the judgment line.
    float JudgeLineFromBottom = 190.0f;
    // Canvas units per second of scroll at 1x, on flat lanes. The track covers the same time from its
    // far end as flat lanes do from the canvas's top.
    float ScrollSpeed = 1500.0f;
    // Where the lanes' middle is across the canvas (and a track's vanishing point); negative for
    // the canvas's middle.
    float CenterX = -1.0f;
    // How early or late the timing meter's ends are: the judge's widest window.
    int64_t ErrorMeterRangeUs = rhythm::HitWindows{}.BadUs;
    // The lanes' colors, mirrored from the outside in (the outer lanes take the first), with the
    // track's edges in the first: a map's own colors. Empty for the usual white and blue, with a
    // gold middle lane.
    std::vector<renderer::Color> LaneColors;
};

// Draws a Mania playfield with SpriteBatch, flat or as a track in perspective (PlayfieldView): lanes,
// notes, hold bodies, the judgment line, pressed lanes, and judgment feedback without text (a color
// flash per lane and a timing error meter).
// Note positions come from precomputed scroll positions (DESIGN.md section 10.5), computed on
// the CPU for now.
class ManiaPlayfield
{
public:
    void Reset(uint32_t laneCount);

    // How many times its width at the judgment line the track is at canvas height `y` (Track
    // view): more below the line, less toward the far end.
    static float GetTrackScaleAt(glm::vec2 canvas, const PlayfieldStyle& style, float y);

    // Call for every judgment event, for the feedback effects.
    void OnJudgment(const rhythm::JudgmentEvent& event, uint32_t lane);

    void Draw(renderer::SpriteBatch& sprites, glm::vec2 canvas, const ManiaChart& chart, const ILaneJudge& judge,
              SongTimeUs time, const PlayfieldStyle& style) const;

    // The lane under a horizontal position on the canvas, for touch input.
    std::optional<uint32_t> GetLaneAt(float x, glm::vec2 canvas, const PlayfieldStyle& style) const;

    // Where a canvas point lines up with the judgment line along the lanes: the point's own x on a
    // flat playfield; on a track, where the lane under the point meets the judgment line. For touch
    // input, with GetLaneAt.
    float GetJudgeLineX(glm::vec2 point, glm::vec2 canvas, const PlayfieldStyle& style) const;

    static renderer::Color GetJudgmentColor(rhythm::Judgment judgment);

private:
    struct Feedback
    {
        SongTimeUs Time = INT64_MIN;
        rhythm::Judgment Result = rhythm::Judgment::Miss;
        int64_t ErrorUs = 0;
    };

    static constexpr size_t ERROR_HISTORY = 32;

    float GetLeft(glm::vec2 canvas, const PlayfieldStyle& style) const;
    static float GetCenterX(glm::vec2 canvas, const PlayfieldStyle& style);
    void DrawTrack(renderer::SpriteBatch& sprites, glm::vec2 canvas, const ManiaChart& chart, const ILaneJudge& judge,
                   SongTimeUs time, const PlayfieldStyle& style) const;
    // The latest judgment's banner and the timing error meter, flat over either view.
    void DrawMeters(renderer::SpriteBatch& sprites, glm::vec2 canvas, SongTimeUs time,
                    const PlayfieldStyle& style) const;

    uint32_t laneCount = 0;
    std::array<Feedback, MAX_LANES> laneFeedback{};
    std::array<Feedback, ERROR_HISTORY> recentErrors{};
    size_t errorCursor = 0;
    Feedback lastJudgment;
};

} // namespace hyoshi::mania
