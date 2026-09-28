#include "renderer/TextRenderer.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using hyoshi::renderer::DecodeUtf8;
using hyoshi::renderer::FontId;
using hyoshi::renderer::TextRenderer;

namespace
{

constexpr char32_t REPLACEMENT = 0xFFFD;

std::vector<char32_t> DecodeAll(std::string_view text)
{
    std::vector<char32_t> codepoints;
    size_t offset = 0;
    while (offset < text.size())
    {
        codepoints.push_back(DecodeUtf8(text, offset));
    }
    return codepoints;
}

std::vector<std::byte> ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> data(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        data[i] = static_cast<std::byte>(bytes[i]);
    }
    return data;
}

} // namespace

TEST_CASE("DecodeUtf8 decodes one to four byte sequences")
{
    // A, é, テ, and an emoji outside the basic plane.
    const std::vector<char32_t> expected{U'A', 0xE9, 0x30C6, 0x1F3B5};
    CHECK(DecodeAll("A\xC3\xA9\xE3\x83\x86\xF0\x9F\x8E\xB5") == expected);
    CHECK(DecodeAll("").empty());
}

TEST_CASE("DecodeUtf8 replaces malformed bytes one at a time")
{
    // A lone continuation byte, then text that must survive it.
    CHECK(DecodeAll("\x80"
                    "ab") == std::vector<char32_t>{REPLACEMENT, U'a', U'b'});
    // A sequence cut short by the end of the text.
    CHECK(DecodeAll("a\xE3\x83") == std::vector<char32_t>{U'a', REPLACEMENT, REPLACEMENT});
    // A lead byte followed by something that isn't a continuation.
    CHECK(DecodeAll("\xE3"
                    "a") == std::vector<char32_t>{REPLACEMENT, U'a'});
    // An overlong encoding of '/', and an encoded surrogate.
    CHECK(DecodeAll("\xC0\xAF").front() == REPLACEMENT);
    CHECK(DecodeAll("\xED\xA0\x80").front() == REPLACEMENT);
}

TEST_CASE("TextRenderer measures and elides without a device")
{
    TextRenderer text;
    CHECK_FALSE(text.AddFont({}));
    hyoshi::Result<FontId> added = text.AddFont(ReadFile(HYOSHI_TEST_FONT));
    REQUIRE(added);
    const FontId font = added.Value();
    CHECK_FALSE(text.AddFont(ReadFile(HYOSHI_TEST_FONT), font + 1));

    SUBCASE("widths add up and scale with the size")
    {
        const float a = text.MeasureWidth("A", font, 40.0f);
        const float b = text.MeasureWidth("B", font, 40.0f);
        CHECK(a > 0.0f);
        CHECK(text.MeasureWidth("AB", font, 40.0f) == doctest::Approx(a + b));
        CHECK(text.MeasureWidth("AB", font, 80.0f) == doctest::Approx(2.0f * (a + b)));
        CHECK(text.MeasureWidth("", font, 40.0f) == 0.0f);
    }

    SUBCASE("Japanese is covered")
    {
        CHECK(text.HasGlyph(font, 0x30C6));
        CHECK(text.MeasureWidth("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88", font, 40.0f) > 0.0f);
        CHECK(text.GetCapHeight(font, 100.0f) > 50.0f);
        CHECK(text.GetLineHeight(font, 100.0f) > text.GetAscent(font, 100.0f));
    }

    SUBCASE("eliding")
    {
        const std::string title = "Camellia - Light It Up (Extended Mix)";
        const float full = text.MeasureWidth(title, font, 40.0f);
        CHECK(text.Elide(title, font, 40.0f, full) == title);

        const std::string elided = text.Elide(title, font, 40.0f, full * 0.5f);
        CHECK(elided.size() < title.size());
        CHECK(elided.ends_with("\xE2\x80\xA6"));
        CHECK(text.MeasureWidth(elided, font, 40.0f) <= full * 0.5f);
        // Never cut inside a multi-byte character.
        const std::string japanese = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
        const std::string elidedJapanese =
            text.Elide(japanese, font, 40.0f, text.MeasureWidth(japanese, font, 40.0f) * 0.6f);
        CHECK((elidedJapanese.size() - 3) % 3 == 0);
    }
}
