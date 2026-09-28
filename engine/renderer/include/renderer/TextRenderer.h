#pragma once

#include "renderer/SpriteBatch.h"

#include "core/Result.h"
#include "rhi/IRenderDevice.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hyoshi::renderer
{

// Decodes the UTF-8 code point at `offset` and advances past it. Malformed input decodes as
// U+FFFD, one byte at a time, so a bad byte never swallows the text after it.
char32_t DecodeUtf8(std::string_view text, size_t& offset);

using FontId = uint32_t;

enum class TextAlign : uint8_t
{
    Left,
    Center,
    Right
};

// What the anchor's y is: the top of the line, the middle of the capital letters (for centering
// text in a box), the baseline, or the bottom of the line.
enum class TextBaseline : uint8_t
{
    Top,
    Middle,
    Alphabetic,
    Bottom
};

struct TextStyle
{
    FontId Font = 0;
    // The em size, in virtual units: roughly the height of the tallest letters plus descenders.
    float Size = 32.0f;
    Color Tint = WHITE;
    TextAlign Align = TextAlign::Left;
    TextBaseline Baseline = TextBaseline::Top;
    int32_t Layer = 0;
};

// Single-line text from TrueType and OpenType fonts (DESIGN.md section 10.3, "As built (text)").
// Glyphs are rasterized on first use as signed distance fields (outlines from stb_truetype) into
// atlas pages, and drawn through SpriteBatch as distance field sprites, so text sorts with other
// sprites by layer and stays sharp at any size. Glyphs a font lacks come from its fallback font.
// No kerning or complex shaping.
class TextRenderer
{
public:
    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Without a device, text can be measured but not drawn (tests).
    void Initialize(rhi::IRenderDevice& device);
    void Shutdown();

    // Adds a TTF or OTF font (the first face of a collection).
    Result<FontId> AddFont(std::vector<std::byte> data, std::optional<FontId> fallback = std::nullopt);

    float MeasureWidth(std::string_view text, FontId font, float size);
    // Distance between baselines.
    float GetLineHeight(FontId font, float size) const;
    // From the top of the line to the baseline, and the height of capital letters.
    float GetAscent(FontId font, float size) const;
    float GetCapHeight(FontId font, float size) const;

    // Draws one line of text; the anchor is placed by the style's Align and Baseline. Returns the
    // width.
    float Draw(SpriteBatch& sprites, std::string_view text, glm::vec2 anchor, const TextStyle& style);

    // The text shortened with an ellipsis to fit in maxWidth, or unchanged if it fits.
    std::string Elide(std::string_view text, FontId font, float size, float maxWidth);

    // Whether the font or its fallbacks have a glyph for the code point.
    bool HasGlyph(FontId font, char32_t codepoint);

    // Uploads the glyphs rasterized since the last call. Call once per frame, before the sprite
    // batch records its draws.
    Result<void> Flush();

private:
    struct Face;
    struct Page;

    struct Glyph
    {
        uint32_t Face = 0;
        int Index = 0;
        float AdvanceEm = 0.0f;
        bool IsRasterized = false;
        // Nothing to draw (spaces).
        bool IsBlank = false;
        uint32_t Page = 0;
        glm::vec4 UvRect{0.0f};
        // The bitmap's top-left corner relative to the pen on the baseline, and its size, in ems.
        glm::vec2 OffsetEm{0.0f};
        glm::vec2 SizeEm{0.0f};
    };

    Glyph& GetGlyph(FontId font, char32_t codepoint);
    bool Rasterize(Glyph& glyph);
    float GetBaselineY(FontId font, float size, TextBaseline baseline, float y) const;

    rhi::IRenderDevice* device = nullptr;
    std::vector<std::unique_ptr<Face>> faces;
    std::vector<std::unique_ptr<Page>> pages;
    // Keyed by font and code point.
    std::unordered_map<uint64_t, Glyph> glyphs;
    bool isAtlasFull = false;
};

} // namespace hyoshi::renderer
