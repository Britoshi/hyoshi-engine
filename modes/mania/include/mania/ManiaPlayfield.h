#pragma once

#include "mania/ManiaChart.h"
#include "mania/ManiaJudge.h"

#include "renderer/SpriteBatch.h"
#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>
#include <optional>

namespace hyoshi::mania
{

struct PlayfieldStyle
{
    float LaneWidth = 120.0f;
    float NoteHeight = 34.0f;
    // Distance from the bottom of the canvas to the judgment line.
    float JudgeLineFromBottom = 190.0f;
    // Virtual units per second of scroll at 1x.
    float ScrollSpeed = 1500.0f;
};

// Draws a Mania playfield with SpriteBatch: lanes, notes, hold bodies, the judgment line, pressed
// lanes, and judgment feedback without text (a color flash per lane and a timing error meter).
// Note positions come from precomputed scroll positions (DESIGN.md section 10.5), computed on
// the CPU for now.
class ManiaPlayfield
{
public:
    void Reset(uint32_t laneCount);

    // Call for every judgment event, for the feedback effects.
    void OnJudgment(const rhythm::JudgmentEvent& event, uint32_t lane);

    void Draw(renderer::SpriteBatch& sprites, glm::vec2 canvas, const ManiaChart& chart, const ManiaJudge& judge,
              SongTimeUs time, const PlayfieldStyle& style) const;

    // The lane under a horizontal position on the canvas, for touch input.
    std::optional<uint32_t> GetLaneAt(float x, glm::vec2 canvas, const PlayfieldStyle& style) const;

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

    uint32_t laneCount = 0;
    std::array<Feedback, MAX_LANES> laneFeedback{};
    std::array<Feedback, ERROR_HISTORY> recentErrors{};
    size_t errorCursor = 0;
    Feedback lastJudgment;
};

} // namespace hyoshi::mania
