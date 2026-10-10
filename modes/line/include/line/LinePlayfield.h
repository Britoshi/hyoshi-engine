#pragma once

#include "line/LineChart.h"
#include "line/LineJudge.h"

#include "renderer/SpriteBatch.h"
#include "rhythm/Judgment.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace hyoshi::line
{

// Where the field sits on the canvas: field units times Scale, from Origin. It's fit whole and
// centered, so a Phigros-shaped (16:9) chart fills a landscape screen.
struct LineLayout
{
    glm::vec2 Origin{0.0f};
    float Scale = 1.0f;

    glm::vec2 ToCanvas(glm::vec2 position) const
    {
        return Origin + (position * Scale);
    }

    glm::vec2 ToField(glm::vec2 canvasPosition) const
    {
        return (canvasPosition - Origin) / Scale;
    }

    static LineLayout Fit(glm::vec2 canvas);
};

// Draws a Line chart with SpriteBatch (DESIGN.md section 16.5): the judgment lines as they move,
// turn, and fade; notes coming at them from either side in Phigros's colors (taps blue, drags
// yellow, flicks red, holds a body behind a head); and a burst where a note is hit.
class LinePlayfield
{
public:
    static constexpr int32_t LAYER_COUNT = 3;

    void Reset(const LineChart& chart);
    // Call for every judgment event, for the bursts.
    void OnJudgment(const LineChart& chart, const rhythm::JudgmentEvent& event);

    // `time` is the visual song time. Draws in layers firstLayer to firstLayer + LAYER_COUNT - 1.
    void Draw(renderer::SpriteBatch& sprites, const LineChart& chart, const LineJudge& judge, SongTimeUs time,
              const LineLayout& layout, int32_t firstLayer) const;

private:
    struct Burst
    {
        SongTimeUs Time = 0;
        glm::vec2 Position{0.0f};
        float Angle = 0.0f;
        rhythm::Judgment Result = rhythm::Judgment::Perfect;
    };
    static constexpr size_t BURST_COUNT = 32;

    std::vector<Burst> bursts;
    size_t nextBurst = 0;
};

} // namespace hyoshi::line
