#pragma once

#include "circle/CircleChart.h"
#include "circle/CircleJudge.h"

#include "core/Result.h"
#include "renderer/SpriteBatch.h"
#include "renderer/TextRenderer.h"
#include "rhi/IRenderDevice.h"
#include "rhythm/Judgment.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace hyoshi::circle
{

// Where the playfield sits on the canvas: playfield units times Scale, from Origin.
struct CircleLayout
{
    glm::vec2 Origin{0.0f};
    float Scale = 1.0f;

    glm::vec2 ToCanvas(glm::vec2 position) const
    {
        return Origin + (position * Scale);
    }

    glm::vec2 ToPlayfield(glm::vec2 canvasPosition) const
    {
        return (canvasPosition - Origin) / Scale;
    }

    // osu!'s placement: the playfield takes 80% of the canvas height (or width, on a canvas
    // narrower than 4:3), centered, and 8 units low.
    static CircleLayout Fit(glm::vec2 canvas);
};

struct CirclePlayfieldStyle
{
    // The playfield draws in layers FirstLayer to FirstLayer + CirclePlayfield::LAYER_COUNT - 1.
    int32_t FirstLayer = 1;
    // Combo numbers.
    renderer::FontId NumberFont = 0;
    bool AreComboNumbersShown = true;
    // The dashes between consecutive notes of a combo.
    bool AreFollowPointsShown = true;
    // Follow point thickness, as a fraction of a circle's diameter.
    float FollowPointThickness = 0.035f;
};

// Draws a circle chart with SpriteBatch (DESIGN.md section 16.4):
// circles with combo numbers and shrinking approach circles; sliders that snake in, carry a ball,
// and shrink behind it on their last pass, with reverse arrows; follow points; a pop where a note
// is struck and a mark with the result; and spinners as a ring that empties over their length.
//
// Notes are drawn latest first, so the next note is always on top. Slider bodies go below every
// circle. The playfield's shapes are generated at load, not skinned.
class CirclePlayfield
{
public:
    static constexpr int32_t LAYER_COUNT = 5;

    Result<void> Initialize(rhi::IRenderDevice& device);
    void Shutdown();

    // Call before playing a chart, and again to restart it.
    void Reset(const CircleChart& chart);

    // Call for every judgment event, for the pops and result marks.
    void OnJudgment(const rhythm::JudgmentEvent& event);

    // `time` is the visual song time.
    void Draw(renderer::SpriteBatch& sprites, renderer::TextRenderer& text, const CircleChart& chart,
              const CircleJudge& judge, SongTimeUs time, const CircleLayout& layout, const CirclePlayfieldStyle& style);

private:
    struct FollowPoint
    {
        glm::vec2 From{0.0f};
        glm::vec2 To{0.0f};
        // Radians, clockwise on screen.
        float Rotation = 0.0f;
        // The dash starts appearing at FadeInTime, and fades out once the moment it marks
        // (FadeOutTime) has passed.
        SongTimeUs FadeInTime = 0;
        SongTimeUs FadeOutTime = 0;
    };

    struct Feedback
    {
        SongTimeUs Time = INT64_MIN;
        rhythm::Judgment Result = rhythm::Judgment::Miss;
    };

    struct Frame;

    void BuildFollowPoints(const CircleChart& chart);
    void ConnectFollowPoints(const CircleChart& chart, uint32_t sourceStart, uint32_t sourceEnd, uint32_t targetStart,
                             uint32_t targetEnd);
    void EmitFollowPoints(glm::vec2 from, glm::vec2 to, SongTimeUs startTime, SongTimeUs endTime, float leadIn,
                          float tailGap);

    void DrawFollowPoints(const Frame& frame);
    void DrawSliderBody(const Frame& frame, uint32_t note) const;
    void DrawHead(const Frame& frame, uint32_t note) const;
    void DrawSliderBall(const Frame& frame, uint32_t note) const;
    void DrawApproach(const Frame& frame, uint32_t note) const;
    void DrawSpinner(const Frame& frame, uint32_t note) const;
    void DrawFeedback(const Frame& frame, uint32_t note) const;

    rhi::IRenderDevice* device = nullptr;
    rhi::TextureHandle shapes;

    std::vector<Feedback> feedback;
    std::vector<FollowPoint> followPoints;
    // Everything before these is finished drawing.
    uint32_t firstNote = 0;
    size_t firstFollowPoint = 0;
    SongTimeUs lastTime = INT64_MIN;
};

} // namespace hyoshi::circle
