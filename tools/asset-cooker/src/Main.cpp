// hyoshi-asset-cooker: builds files the game ships from source assets. Run it on a desktop host
// when a source changes, and commit what it writes.
//
//     hyoshi-asset-cooker icons <logo.png> <game folder>
//
// From the logo artwork (dark shapes on transparency), writes the app icons: the logo in white on a
// dark rounded square for the window (content/textures/app-icon.png) and the Windows executable
// (windows/app.ico); Android adaptive icon foregrounds (white logo on transparency,
// android/app/src/main/res/mipmap-*/ic_launcher_foreground.png); and the Play Store
// icon (android/play-store-icon.png). The paths are relative to the game folder: the layout a game
// on the engine uses (see the engine README).

#include "renderer/Image.h"

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using hyoshi::renderer::Image;

struct Rgb
{
    float R;
    float G;
    float B;
};

// The game's dark theme (game/ui/Ui.h), a little lighter at the top.
constexpr Rgb BACKGROUND_TOP{46.0f, 46.0f, 84.0f};
constexpr Rgb BACKGROUND_BOTTOM{16.0f, 16.0f, 30.0f};
// Share of the icon's side the logo spans, on a rounded square and on Android's adaptive icon
// (whose visible circle is 66 of its 108 units).
constexpr float ICON_LOGO_FILL = 0.6f;
constexpr float ADAPTIVE_LOGO_FILL = 0.46f;
constexpr float CORNER_RADIUS = 0.22f;
// Samples per axis per output pixel when shrinking the artwork.
constexpr int SUPERSAMPLE = 8;

int Fail(const std::string& message)
{
    std::fprintf(stderr, "hyoshi-asset-cooker: %s\n", message.c_str());
    return EXIT_FAILURE;
}

// The artwork's alpha, trimmed of transparent margins, fitted and centered in a size x size square
// with its longer side at `fill` of the square. One channel.
Image FitArtwork(const Image& art, uint32_t size, float fill)
{
    uint32_t minX = art.Width;
    uint32_t minY = art.Height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    for (uint32_t y = 0; y < art.Height; ++y)
    {
        for (uint32_t x = 0; x < art.Width; ++x)
        {
            if (art.Pixels[((size_t{y} * art.Width) + x) * 4 + 3] >= 8)
            {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    const auto contentWidth = static_cast<float>(maxX - minX + 1);
    const auto contentHeight = static_cast<float>(maxY - minY + 1);
    const float scale = fill * static_cast<float>(size) / std::max(contentWidth, contentHeight);
    const float left = (static_cast<float>(size) - (contentWidth * scale)) * 0.5f;
    const float top = (static_cast<float>(size) - (contentHeight * scale)) * 0.5f;

    Image fitted;
    fitted.Width = size;
    fitted.Height = size;
    fitted.Channels = 1;
    fitted.Pixels.assign(size_t{size} * size, 0);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            float sum = 0.0f;
            for (int sy = 0; sy < SUPERSAMPLE; ++sy)
            {
                for (int sx = 0; sx < SUPERSAMPLE; ++sx)
                {
                    const float px = static_cast<float>(x) + ((static_cast<float>(sx) + 0.5f) / SUPERSAMPLE);
                    const float py = static_cast<float>(y) + ((static_cast<float>(sy) + 0.5f) / SUPERSAMPLE);
                    const float artX = ((px - left) / scale) + static_cast<float>(minX);
                    const float artY = ((py - top) / scale) + static_cast<float>(minY);
                    if (artX >= 0.0f && artY >= 0.0f && artX < static_cast<float>(art.Width) &&
                        artY < static_cast<float>(art.Height))
                    {
                        const size_t pixel = (static_cast<size_t>(artY) * art.Width) + static_cast<size_t>(artX);
                        sum += static_cast<float>(art.Pixels[(pixel * 4) + 3]);
                    }
                }
            }
            fitted.Pixels[(size_t{y} * size) + x] =
                static_cast<uint8_t>(std::lround(sum / static_cast<float>(SUPERSAMPLE * SUPERSAMPLE)));
        }
    }
    return fitted;
}

// How much of the pixel a rounded square covering the whole image covers (anti-aliased).
float RoundedSquareCoverage(uint32_t x, uint32_t y, uint32_t size, float radius)
{
    const float half = static_cast<float>(size) * 0.5f;
    const float px = std::abs(static_cast<float>(x) + 0.5f - half);
    const float py = std::abs(static_cast<float>(y) + 0.5f - half);
    const float inner = half - radius;
    const float dx = std::max(px - inner, 0.0f);
    const float dy = std::max(py - inner, 0.0f);
    const float distance = std::sqrt((dx * dx) + (dy * dy)) - radius;
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

// The white logo over the dark gradient; rounded corners, or a full square for the Play Store
// (which rounds them itself).
Image MakeIcon(const Image& art, uint32_t size, bool isRounded)
{
    const Image logo = FitArtwork(art, size, ICON_LOGO_FILL);
    Image icon;
    icon.Width = size;
    icon.Height = size;
    icon.Pixels.resize(size_t{size} * size * 4);
    const float radius = isRounded ? CORNER_RADIUS * static_cast<float>(size) : 0.0f;
    for (uint32_t y = 0; y < size; ++y)
    {
        const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        const Rgb background{BACKGROUND_TOP.R + ((BACKGROUND_BOTTOM.R - BACKGROUND_TOP.R) * t),
                             BACKGROUND_TOP.G + ((BACKGROUND_BOTTOM.G - BACKGROUND_TOP.G) * t),
                             BACKGROUND_TOP.B + ((BACKGROUND_BOTTOM.B - BACKGROUND_TOP.B) * t)};
        for (uint32_t x = 0; x < size; ++x)
        {
            const size_t pixel = (size_t{y} * size) + x;
            const float coverage = static_cast<float>(logo.Pixels[pixel]) / 255.0f;
            const float alpha = isRounded ? RoundedSquareCoverage(x, y, size, radius) : 1.0f;
            auto channel = [coverage](float base)
            { return static_cast<uint8_t>(std::lround(base + ((255.0f - base) * coverage))); };
            icon.Pixels[(pixel * 4) + 0] = channel(background.R);
            icon.Pixels[(pixel * 4) + 1] = channel(background.G);
            icon.Pixels[(pixel * 4) + 2] = channel(background.B);
            icon.Pixels[(pixel * 4) + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
        }
    }
    return icon;
}

// The white logo on transparency, for an adaptive icon's foreground layer.
Image MakeAdaptiveForeground(const Image& art, uint32_t size)
{
    const Image logo = FitArtwork(art, size, ADAPTIVE_LOGO_FILL);
    Image foreground;
    foreground.Width = size;
    foreground.Height = size;
    foreground.Pixels.resize(size_t{size} * size * 4);
    for (size_t pixel = 0; pixel < logo.Pixels.size(); ++pixel)
    {
        foreground.Pixels[(pixel * 4) + 0] = 255;
        foreground.Pixels[(pixel * 4) + 1] = 255;
        foreground.Pixels[(pixel * 4) + 2] = 255;
        foreground.Pixels[(pixel * 4) + 3] = logo.Pixels[pixel];
    }
    return foreground;
}

std::vector<uint8_t> EncodePng(const Image& image)
{
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size)
        {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            const auto* bytes = static_cast<const uint8_t*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &png, static_cast<int>(image.Width), static_cast<int>(image.Height), static_cast<int>(image.Channels),
        image.Pixels.data(), static_cast<int>(image.Width * image.Channels));
    return png;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("wrote %s\n", path.generic_string().c_str());
    return static_cast<bool>(file);
}

void AppendLittleEndian(std::vector<uint8_t>& out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

// An .ico holding a PNG per size, which Windows reads since Vista.
std::vector<uint8_t> MakeIco(const Image& art, const std::vector<uint32_t>& sizes)
{
    std::vector<std::vector<uint8_t>> pngs;
    for (const uint32_t size : sizes)
    {
        pngs.push_back(EncodePng(MakeIcon(art, size, true)));
    }
    std::vector<uint8_t> ico;
    AppendLittleEndian(ico, 0, 2);
    AppendLittleEndian(ico, 1, 2);
    AppendLittleEndian(ico, static_cast<uint32_t>(sizes.size()), 2);
    uint32_t offset = 6 + (16 * static_cast<uint32_t>(sizes.size()));
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        const uint32_t side = sizes[i] >= 256 ? 0 : sizes[i];
        ico.push_back(static_cast<uint8_t>(side));
        ico.push_back(static_cast<uint8_t>(side));
        ico.push_back(0);
        ico.push_back(0);
        AppendLittleEndian(ico, 1, 2);
        AppendLittleEndian(ico, 32, 2);
        AppendLittleEndian(ico, static_cast<uint32_t>(pngs[i].size()), 4);
        AppendLittleEndian(ico, offset, 4);
        offset += static_cast<uint32_t>(pngs[i].size());
    }
    for (const std::vector<uint8_t>& png : pngs)
    {
        ico.insert(ico.end(), png.begin(), png.end());
    }
    return ico;
}

int CookIcons(const fs::path& artPath, const fs::path& root)
{
    std::ifstream file(artPath, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.empty())
    {
        return Fail("cannot read " + artPath.generic_string());
    }
    hyoshi::Result<Image> art = hyoshi::renderer::DecodeImage(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (!art)
    {
        return Fail(artPath.generic_string() + ": " + art.GetError().Message);
    }

    bool isOk = WriteFile(root / "content/textures/app-icon.png", EncodePng(MakeIcon(art.Value(), 256, true)));
    isOk =
        WriteFile(root / "windows/app.ico", MakeIco(art.Value(), {16, 20, 24, 32, 40, 48, 64, 96, 128, 256})) && isOk;
    isOk = WriteFile(root / "android/play-store-icon.png", EncodePng(MakeIcon(art.Value(), 512, false))) && isOk;
    // Adaptive icons are 108 dp square.
    constexpr std::array<std::pair<const char*, uint32_t>, 5> DENSITIES{
        {{"mdpi", 108}, {"hdpi", 162}, {"xhdpi", 216}, {"xxhdpi", 324}, {"xxxhdpi", 432}}};
    for (const auto& [density, size] : DENSITIES)
    {
        const fs::path path =
            root / "android/app/src/main/res" / (std::string("mipmap-") + density) / "ic_launcher_foreground.png";
        isOk = WriteFile(path, EncodePng(MakeAdaptiveForeground(art.Value(), size))) && isOk;
    }
    return isOk ? EXIT_SUCCESS : Fail("could not write every file");
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 4 && std::string_view(argv[1]) == "icons")
    {
        return CookIcons(argv[2], argv[3]);
    }
    std::fprintf(stderr, "usage: hyoshi-asset-cooker icons <logo.png> <game folder>\n");
    return EXIT_FAILURE;
}
