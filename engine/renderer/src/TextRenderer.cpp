#include "renderer/TextRenderer.h"

#include "core/Log.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace hyoshi::renderer
{

namespace
{

// Atlas pages are this many texels square, single channel.
constexpr uint32_t ATLAS_PAGE_SIZE = 2048;
constexpr size_t MAX_PAGES = 8;
// Glyphs are rasterized at this many pixels per em. The distance field reaches PADDING pixels
// beyond the outline, which bounds how far the edge can be smoothed when drawn large.
constexpr float BAKE_PIXELS_PER_EM = 48.0f;
constexpr int PADDING = 6;
constexpr unsigned char ON_EDGE = 128;
constexpr float PIXEL_DISTANCE_SCALE = static_cast<float>(ON_EDGE) / static_cast<float>(PADDING);
// Empty texels between glyphs, so filtering never picks up a neighbor.
constexpr uint32_t GLYPH_GAP = 1;

constexpr char32_t REPLACEMENT_CHARACTER = 0xFFFD;
constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";

uint64_t GetGlyphKey(FontId font, char32_t codepoint)
{
    return (uint64_t{font} << 32) | codepoint;
}

bool IsControl(char32_t codepoint)
{
    return codepoint < 0x20 || codepoint == 0x7F;
}

struct Segment
{
    glm::vec2 A;
    glm::vec2 B;
    // Bounding box, for skipping segments that can't be the nearest.
    glm::vec2 Min;
    glm::vec2 Max;
};

// The glyph's outline as line segments in pixels (y down): curves, quadratic (TrueType) and cubic
// (OpenType CFF), are flattened into about two-pixel pieces.
std::vector<Segment> GetOutline(const stbtt_fontinfo& info, int glyph, float scale)
{
    stbtt_vertex* vertices = nullptr;
    const int count = stbtt_GetGlyphShape(&info, glyph, &vertices);
    std::vector<Segment> segments;
    auto add = [&segments](glm::vec2 a, glm::vec2 b)
    {
        if (a != b)
        {
            segments.push_back({a, b, glm::min(a, b), glm::max(a, b)});
        }
    };
    auto toPixels = [scale](stbtt_vertex_type x, stbtt_vertex_type y)
    { return glm::vec2(static_cast<float>(x) * scale, -static_cast<float>(y) * scale); };
    auto addCurve = [&add](glm::vec2 from, glm::vec2 control0, glm::vec2 control1, glm::vec2 to, bool isCubic)
    {
        const float length =
            glm::distance(from, control0) + glm::distance(control0, control1) + glm::distance(control1, to);
        const int pieces = std::clamp(static_cast<int>(length * 0.5f), 2, 24);
        glm::vec2 previous = from;
        for (int i = 1; i <= pieces; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(pieces);
            const float u = 1.0f - t;
            const glm::vec2 point = isCubic ? (u * u * u * from) + (3.0f * u * u * t * control0) +
                                                  (3.0f * u * t * t * control1) + (t * t * t * to)
                                            : (u * u * from) + (2.0f * u * t * control0) + (t * t * to);
            add(previous, point);
            previous = point;
        }
    };

    glm::vec2 pen{0.0f};
    glm::vec2 contourStart{0.0f};
    for (int i = 0; i < count; ++i)
    {
        const stbtt_vertex& vertex = vertices[i];
        const glm::vec2 point = toPixels(vertex.x, vertex.y);
        switch (vertex.type)
        {
        case STBTT_vmove:
            add(pen, contourStart);
            pen = point;
            contourStart = point;
            break;
        case STBTT_vline:
            add(pen, point);
            pen = point;
            break;
        case STBTT_vcurve:
        {
            const glm::vec2 control = toPixels(vertex.cx, vertex.cy);
            addCurve(pen, control, control, point, false);
            pen = point;
            break;
        }
        case STBTT_vcubic:
            addCurve(pen, toPixels(vertex.cx, vertex.cy), toPixels(vertex.cx1, vertex.cy1), point, true);
            pen = point;
            break;
        default:
            break;
        }
    }
    add(pen, contourStart);
    stbtt_FreeShape(&info, vertices);
    return segments;
}

float GetSquaredDistance(glm::vec2 point, const Segment& segment)
{
    const glm::vec2 direction = segment.B - segment.A;
    const float t = std::clamp(glm::dot(point - segment.A, direction) / glm::dot(direction, direction), 0.0f, 1.0f);
    const glm::vec2 offset = point - (segment.A + (direction * t));
    return glm::dot(offset, offset);
}

// A signed distance field of the glyph, like stbtt_GetGlyphSDF's (which only handles quadratic
// curves): ON_EDGE on the outline, PIXEL_DISTANCE_SCALE per pixel inward, and as much less
// outward. Inside is by the nonzero winding rule. Returns an empty vector for a blank glyph.
std::vector<uint8_t> MakeDistanceField(const stbtt_fontinfo& info, int glyph, float scale, int& width, int& height,
                                       int& xOffset, int& yOffset)
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0)
    {
        return {};
    }
    const std::vector<Segment> segments = GetOutline(info, glyph, scale);
    if (segments.empty())
    {
        return {};
    }

    xOffset = x0 - PADDING;
    yOffset = y0 - PADDING;
    width = (x1 - x0) + (2 * PADDING);
    height = (y1 - y0) + (2 * PADDING);
    std::vector<uint8_t> field(static_cast<size_t>(width) * static_cast<size_t>(height));
    constexpr float REACH = static_cast<float>(PADDING) + 1.0f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const glm::vec2 point{static_cast<float>(xOffset + x) + 0.5f, static_cast<float>(yOffset + y) + 0.5f};
            float nearest = REACH * REACH;
            int winding = 0;
            for (const Segment& segment : segments)
            {
                // A ray to the right: count the segments it crosses, by direction.
                if ((segment.A.y <= point.y) != (segment.B.y <= point.y))
                {
                    const float crossing = segment.A.x + ((point.y - segment.A.y) / (segment.B.y - segment.A.y) *
                                                          (segment.B.x - segment.A.x));
                    if (crossing > point.x)
                    {
                        winding += segment.B.y > segment.A.y ? 1 : -1;
                    }
                }
                const glm::vec2 outside = glm::max(glm::max(segment.Min - point, point - segment.Max), glm::vec2(0.0f));
                if (glm::dot(outside, outside) < nearest)
                {
                    nearest = std::min(nearest, GetSquaredDistance(point, segment));
                }
            }
            const float distance = std::sqrt(nearest) * (winding != 0 ? 1.0f : -1.0f);
            const float value = static_cast<float>(ON_EDGE) + (distance * PIXEL_DISTANCE_SCALE);
            field[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
        }
    }
    return field;
}

} // namespace

char32_t DecodeUtf8(std::string_view text, size_t& offset)
{
    const auto byteAt = [&text](size_t index) { return static_cast<uint8_t>(text[index]); };
    const uint8_t lead = byteAt(offset);
    if (lead < 0x80)
    {
        ++offset;
        return lead;
    }

    size_t length = 0;
    char32_t codepoint = 0;
    char32_t smallest = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        codepoint = lead & 0x1Fu;
        smallest = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        codepoint = lead & 0x0Fu;
        smallest = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        codepoint = lead & 0x07u;
        smallest = 0x10000;
    }
    else
    {
        ++offset;
        return REPLACEMENT_CHARACTER;
    }

    if (offset + length > text.size())
    {
        ++offset;
        return REPLACEMENT_CHARACTER;
    }
    for (size_t i = 1; i < length; ++i)
    {
        const uint8_t next = byteAt(offset + i);
        if ((next & 0xC0) != 0x80)
        {
            ++offset;
            return REPLACEMENT_CHARACTER;
        }
        codepoint = (codepoint << 6) | (next & 0x3Fu);
    }
    // Overlong encodings, surrogates, and values past Unicode's range are malformed.
    if (codepoint < smallest || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    {
        ++offset;
        return REPLACEMENT_CHARACTER;
    }
    offset += length;
    return codepoint;
}

struct TextRenderer::Face
{
    std::vector<std::byte> Data;
    stbtt_fontinfo Info{};
    std::optional<FontId> Fallback;
    // Font units to ems.
    float EmScale = 0.0f;
    float AscentEm = 0.0f;
    // Negative: below the baseline.
    float DescentEm = 0.0f;
    float LineGapEm = 0.0f;
    float CapHeightEm = 0.0f;
};

// Glyphs are packed in shelves: rows filled left to right, each as tall as its tallest glyph.
// A CPU copy of the texels is uploaded by Flush, one band of rows per page.
struct TextRenderer::Page
{
    rhi::TextureHandle Texture;
    std::vector<uint8_t> Texels;
    uint32_t ShelfTop = 0;
    uint32_t ShelfHeight = 0;
    uint32_t CursorX = 0;
    uint32_t DirtyTop = 0;
    uint32_t DirtyBottom = 0;
};

TextRenderer::TextRenderer() = default;

TextRenderer::~TextRenderer()
{
    Shutdown();
}

void TextRenderer::Initialize(rhi::IRenderDevice& renderDevice)
{
    device = &renderDevice;
}

void TextRenderer::Shutdown()
{
    if (device != nullptr)
    {
        for (const std::unique_ptr<Page>& page : pages)
        {
            device->Destroy(page->Texture);
        }
    }
    pages.clear();
    glyphs.clear();
    faces.clear();
    isAtlasFull = false;
    device = nullptr;
}

Result<FontId> TextRenderer::AddFont(std::vector<std::byte> data, std::optional<FontId> fallback)
{
    if (fallback && *fallback >= faces.size())
    {
        return Error{"AddFont: the fallback font doesn't exist"};
    }

    auto face = std::make_unique<Face>();
    face->Data = std::move(data);
    const auto* bytes = reinterpret_cast<const unsigned char*>(face->Data.data());
    const int offset = face->Data.empty() ? -1 : stbtt_GetFontOffsetForIndex(bytes, 0);
    if (offset < 0 || stbtt_InitFont(&face->Info, bytes, offset) == 0)
    {
        return Error{"Not a TrueType or OpenType font"};
    }
    face->Fallback = fallback;
    face->EmScale = stbtt_ScaleForMappingEmToPixels(&face->Info, 1.0f);

    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&face->Info, &ascent, &descent, &lineGap);
    face->AscentEm = static_cast<float>(ascent) * face->EmScale;
    face->DescentEm = static_cast<float>(descent) * face->EmScale;
    face->LineGapEm = static_cast<float>(lineGap) * face->EmScale;

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    face->CapHeightEm = stbtt_GetCodepointBox(&face->Info, 'H', &x0, &y0, &x1, &y1) != 0
                            ? static_cast<float>(y1) * face->EmScale
                            : 0.7f;

    faces.push_back(std::move(face));
    return static_cast<FontId>(faces.size() - 1);
}

TextRenderer::Glyph& TextRenderer::GetGlyph(FontId font, char32_t codepoint)
{
    const uint64_t key = GetGlyphKey(font, codepoint);
    if (const auto found = glyphs.find(key); found != glyphs.end())
    {
        return found->second;
    }

    // The first font along the fallback chain that has it, or else the requested font's "missing"
    // glyph (index 0, usually a box).
    Glyph glyph;
    glyph.Face = font;
    for (std::optional<FontId> current = font; current; current = faces[*current]->Fallback)
    {
        const int index = stbtt_FindGlyphIndex(&faces[*current]->Info, static_cast<int>(codepoint));
        if (index != 0)
        {
            glyph.Face = *current;
            glyph.Index = index;
            break;
        }
    }

    const Face& face = *faces[glyph.Face];
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&face.Info, glyph.Index, &advance, &bearing);
    glyph.AdvanceEm = static_cast<float>(advance) * face.EmScale;
    return glyphs.emplace(key, glyph).first->second;
}

bool TextRenderer::HasGlyph(FontId font, char32_t codepoint)
{
    return font < faces.size() && GetGlyph(font, codepoint).Index != 0;
}

bool TextRenderer::Rasterize(Glyph& glyph)
{
    if (glyph.IsRasterized)
    {
        return true;
    }
    if (device == nullptr || isAtlasFull)
    {
        return false;
    }

    const Face& face = *faces[glyph.Face];
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    const std::vector<uint8_t> field =
        MakeDistanceField(face.Info, glyph.Index, face.EmScale * BAKE_PIXELS_PER_EM, width, height, xOffset, yOffset);
    if (field.empty())
    {
        glyph.IsRasterized = true;
        glyph.IsBlank = true;
        return true;
    }
    const uint8_t* bitmap = field.data();

    const auto glyphWidth = static_cast<uint32_t>(width);
    const auto glyphHeight = static_cast<uint32_t>(height);
    auto place = [glyphWidth, glyphHeight](Page& page, uint32_t& x, uint32_t& y)
    {
        if (page.CursorX + glyphWidth > ATLAS_PAGE_SIZE)
        {
            page.ShelfTop += page.ShelfHeight + GLYPH_GAP;
            page.ShelfHeight = 0;
            page.CursorX = 0;
        }
        if (page.ShelfTop + glyphHeight > ATLAS_PAGE_SIZE)
        {
            return false;
        }
        x = page.CursorX;
        y = page.ShelfTop;
        page.CursorX += glyphWidth + GLYPH_GAP;
        page.ShelfHeight = std::max(page.ShelfHeight, glyphHeight);
        return true;
    };

    uint32_t x = 0;
    uint32_t y = 0;
    if (pages.empty() || !place(*pages.back(), x, y))
    {
        if (pages.size() == MAX_PAGES)
        {
            HYOSHI_LOG_ERROR("Text: the glyph atlas is full; new glyphs won't draw");
            isAtlasFull = true;
            return false;
        }
        rhi::TextureDesc desc;
        desc.Width = ATLAS_PAGE_SIZE;
        desc.Height = ATLAS_PAGE_SIZE;
        desc.Format = rhi::TextureFormat::R8Unorm;
        desc.DebugName = "Glyph atlas";
        Result<rhi::TextureHandle> texture = device->CreateTexture(desc);
        if (!texture)
        {
            HYOSHI_LOG_ERROR("Text: {}", texture.GetError().Message);
            isAtlasFull = true;
            return false;
        }
        auto page = std::make_unique<Page>();
        page->Texture = texture.Value();
        page->Texels.assign(size_t{ATLAS_PAGE_SIZE} * ATLAS_PAGE_SIZE, 0);
        // The whole page goes up once, so the texture never holds undefined texels.
        page->DirtyBottom = ATLAS_PAGE_SIZE;
        pages.push_back(std::move(page));
        place(*pages.back(), x, y);
    }

    Page& page = *pages.back();
    for (uint32_t row = 0; row < glyphHeight; ++row)
    {
        std::memcpy(&page.Texels[(size_t{y + row} * ATLAS_PAGE_SIZE) + x], bitmap + (size_t{row} * glyphWidth),
                    glyphWidth);
    }
    if (page.DirtyTop >= page.DirtyBottom)
    {
        page.DirtyTop = y;
        page.DirtyBottom = y + glyphHeight;
    }
    else
    {
        page.DirtyTop = std::min(page.DirtyTop, y);
        page.DirtyBottom = std::max(page.DirtyBottom, y + glyphHeight);
    }

    constexpr auto PAGE = static_cast<float>(ATLAS_PAGE_SIZE);
    glyph.IsRasterized = true;
    glyph.Page = static_cast<uint32_t>(pages.size() - 1);
    glyph.UvRect = {static_cast<float>(x) / PAGE, static_cast<float>(y) / PAGE,
                    static_cast<float>(x + glyphWidth) / PAGE, static_cast<float>(y + glyphHeight) / PAGE};
    glyph.OffsetEm = glm::vec2(static_cast<float>(xOffset), static_cast<float>(yOffset)) / BAKE_PIXELS_PER_EM;
    glyph.SizeEm = glm::vec2(static_cast<float>(glyphWidth), static_cast<float>(glyphHeight)) / BAKE_PIXELS_PER_EM;
    return true;
}

float TextRenderer::MeasureWidth(std::string_view text, FontId font, float size)
{
    if (font >= faces.size())
    {
        return 0.0f;
    }
    float widthEm = 0.0f;
    size_t offset = 0;
    while (offset < text.size())
    {
        const char32_t codepoint = DecodeUtf8(text, offset);
        if (!IsControl(codepoint))
        {
            widthEm += GetGlyph(font, codepoint).AdvanceEm;
        }
    }
    return widthEm * size;
}

float TextRenderer::GetLineHeight(FontId font, float size) const
{
    if (font >= faces.size())
    {
        return 0.0f;
    }
    const Face& face = *faces[font];
    return (face.AscentEm - face.DescentEm + face.LineGapEm) * size;
}

float TextRenderer::GetAscent(FontId font, float size) const
{
    return font < faces.size() ? faces[font]->AscentEm * size : 0.0f;
}

float TextRenderer::GetCapHeight(FontId font, float size) const
{
    return font < faces.size() ? faces[font]->CapHeightEm * size : 0.0f;
}

float TextRenderer::GetBaselineY(FontId font, float size, TextBaseline baseline, float y) const
{
    const Face& face = *faces[font];
    switch (baseline)
    {
    case TextBaseline::Top:
        return y + (face.AscentEm * size);
    case TextBaseline::Middle:
        return y + (face.CapHeightEm * size * 0.5f);
    case TextBaseline::Bottom:
        return y + (face.DescentEm * size);
    case TextBaseline::Alphabetic:
        break;
    }
    return y;
}

float TextRenderer::Draw(SpriteBatch& sprites, std::string_view text, glm::vec2 anchor, const TextStyle& style)
{
    if (style.Font >= faces.size() || text.empty())
    {
        return 0.0f;
    }

    const float width = MeasureWidth(text, style.Font, style.Size);
    float x = anchor.x;
    if (style.Align == TextAlign::Center)
    {
        x -= width * 0.5f;
    }
    else if (style.Align == TextAlign::Right)
    {
        x -= width;
    }
    const float baseline = GetBaselineY(style.Font, style.Size, style.Baseline, anchor.y);

    size_t offset = 0;
    while (offset < text.size())
    {
        const char32_t codepoint = DecodeUtf8(text, offset);
        if (IsControl(codepoint))
        {
            continue;
        }
        Glyph& glyph = GetGlyph(style.Font, codepoint);
        if (Rasterize(glyph) && !glyph.IsBlank)
        {
            Sprite sprite;
            sprite.Size = glyph.SizeEm * style.Size;
            sprite.Center = glm::vec2(x, baseline) + (glyph.OffsetEm * style.Size) + (sprite.Size * 0.5f);
            sprite.UvRect = glyph.UvRect;
            sprite.Texture = pages[glyph.Page]->Texture;
            sprite.Tint = style.Tint;
            sprite.Layer = style.Layer;
            sprite.IsDistanceField = true;
            sprites.Draw(sprite);
        }
        x += glyph.AdvanceEm * style.Size;
    }
    return width;
}

std::string TextRenderer::Elide(std::string_view text, FontId font, float size, float maxWidth)
{
    if (font >= faces.size() || MeasureWidth(text, font, size) <= maxWidth)
    {
        return std::string(text);
    }

    const float ellipsisWidth = MeasureWidth(ELLIPSIS, font, size);
    float width = 0.0f;
    size_t fits = 0;
    size_t offset = 0;
    while (offset < text.size())
    {
        size_t next = offset;
        const char32_t codepoint = DecodeUtf8(text, next);
        if (!IsControl(codepoint))
        {
            width += GetGlyph(font, codepoint).AdvanceEm * size;
        }
        if (width + ellipsisWidth > maxWidth)
        {
            break;
        }
        offset = next;
        fits = offset;
    }

    std::string result(text.substr(0, fits));
    while (!result.empty() && result.back() == ' ')
    {
        result.pop_back();
    }
    return result + std::string(ELLIPSIS);
}

Result<void> TextRenderer::Flush()
{
    for (const std::unique_ptr<Page>& page : pages)
    {
        if (page->DirtyTop >= page->DirtyBottom)
        {
            continue;
        }
        const uint32_t top = page->DirtyTop;
        const uint32_t rows = page->DirtyBottom - top;
        page->DirtyTop = 0;
        page->DirtyBottom = 0;
        if (Result<void> result = device->UpdateTexture(page->Texture, &page->Texels[size_t{top} * ATLAS_PAGE_SIZE], 0,
                                                        top, ATLAS_PAGE_SIZE, rows);
            !result)
        {
            return result;
        }
    }
    return {};
}

} // namespace hyoshi::renderer
