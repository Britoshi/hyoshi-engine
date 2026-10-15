#pragma once

#include "line/LineChart.h"
#include "line/LineJudge.h"

#include "renderer/SpriteBatch.h"
#include "rhi/IRenderDevice.h"
#include "rhythm/Judgment.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
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

    // A field of `fieldSize` (the chart's FieldSize) fit to the canvas, in its middle.
    static LineLayout Fit(glm::vec2 canvas, glm::vec2 fieldSize = {FIELD_WIDTH, FIELD_HEIGHT});
};

// The field reshaped to a canvas (for LineChart::FieldSize): the canvas's shape, at the scale
// FIELD_WIDTH by FIELD_HEIGHT fits it at, so one side is as long as ever and the other longer, and
// the field fills the canvas.
glm::vec2 GetScreenFieldSize(glm::vec2 canvas);

// Draws a Line chart with SpriteBatch (DESIGN.md section 16.5): the judgment lines as they move,
// turn, and fade; notes coming at them from either side in Phigros's colors (taps blue, drags
// yellow, flicks red, holds a body behind a head); and a burst where a note is hit.
class LinePlayfield
{
public:
    static constexpr int32_t LAYER_COUNT = 3;

    // A picture's texture, and the picture's size in pixels as the chart has it (the texture can be
    // smaller, loaded at the size it's drawn).
    struct Picture
    {
        rhi::TextureHandle Texture;
        glm::vec2 Size{0.0f};
    };
    // Lines with a picture draw it once it's given (JudgeLine::Picture); until then, nothing. The
    // caller owns the textures.
    void SetPicture(const std::string& name, const Picture& picture);
    void ClearPictures();

    void Reset(const LineChart& chart);
    // Call for every judgment event, for the bursts.
    void OnJudgment(const LineChart& chart, const rhythm::JudgmentEvent& event);

    // `time` is the visual song time. Draws in layers firstLayer to firstLayer + LAYER_COUNT - 1:
    // the lines (DrawLines), then the notes on them (DrawNotes). noteSpeed scales how far notes are
    // from their lines (the player's speed, as Phigros has one): they come slower or faster, but
    // just as on time.
    void Draw(renderer::SpriteBatch& sprites, const LineChart& chart, const LineJudge& judge, SongTimeUs time,
              const LineLayout& layout, int32_t firstLayer, float noteSpeed = 1.0f) const;
    // Just the lines, in one layer: a chart's moving picture, which a game can keep up under
    // something else while hiding the notes.
    void DrawLines(renderer::SpriteBatch& sprites, const LineChart& chart, SongTimeUs time, const LineLayout& layout,
                   int32_t layer) const;
    // Just the notes and the bursts, in layers firstLayer and firstLayer + 1. `flash` is how much
    // the notes flash, 0 to 1 (renderer::FLASH_WHITENING).
    void DrawNotes(renderer::SpriteBatch& sprites, const LineChart& chart, const LineJudge& judge, SongTimeUs time,
                   const LineLayout& layout, int32_t firstLayer, float noteSpeed = 1.0f, float flash = 0.0f) const;

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
    std::unordered_map<std::string, Picture> pictures;
};

} // namespace hyoshi::line
