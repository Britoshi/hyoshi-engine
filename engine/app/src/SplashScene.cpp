#include "SplashScene.h"

#include "renderer/Image.h"
#include "ui/Ui.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace hyoshi::app
{

namespace
{

using renderer::Color;
using renderer::WithAlpha;

constexpr float PI = std::numbers::pi_v<float>;

// The beat: a tile lands on each of the first four, and on the fifth (the hit) the line joins and
// the wordmark comes out.
constexpr int32_t TILE_COUNT = 4;
constexpr float FIRST_BEAT_SECONDS = 0.3f;
constexpr float BEAT_SECONDS = 0.25f;
constexpr float LEAVE_AT_SECONDS = 2.45f;
constexpr float FADE_OUT_SECONDS = 0.4f;
// A slow frame (the first few, while the GPU warms up) moves the splash on by at most this much,
// so no beat is skipped.
constexpr float MAX_STEP_SECONDS = 1.0f / 30.0f;

// How long each part moves: a tile's fall onto its beat (from this many mark heights up), a dash
// growing, the line joining, the mark moving aside, the wordmark coming out, and the hit's wave.
constexpr float FALL_SECONDS = 0.16f;
constexpr float DROP = 0.32f;
constexpr float DASH_SECONDS = 0.12f;
constexpr float JOIN_SECONDS = 0.22f;
constexpr float SLIDE_SECONDS = 0.45f;
constexpr float REVEAL_SECONDS = 0.5f;
constexpr float WAVE_SECONDS = 0.6f;
// Sparks thrown off the line at the hit, and how fast the air slows them (per second).
constexpr uint32_t SPARK_COUNT = 40;
constexpr float SPARK_DRAG = 5.0f;

// The mark's tiles, as fractions of its box (logo-icon.png), in the order they land: two
// columns, the right one lower, each a tall and a short tile.
struct Tile
{
    float Left;
    float Top;
    float Right;
    float Bottom;
};
constexpr std::array<Tile, TILE_COUNT> TILES{{
    {0.0f, 0.0f, 0.4443f, 0.4675f},
    {0.5557f, 0.1592f, 1.0f, 0.5439f},
    {0.0f, 0.5096f, 0.4443f, 0.9096f},
    {0.5557f, 0.5822f, 1.0f, 1.0f},
}};
// The mark's width, and its tiles' corners, in mark heights.
constexpr float MARK_ASPECT = 0.777f;
constexpr float TILE_RADIUS = 0.041f;
// The wordmark beside the mark (logo-horizontal.png), in mark heights: its height and the gap
// before it, and its width over height should it not load.
constexpr float WORDMARK_HEIGHT = 0.411f;
constexpr float WORDMARK_GAP = 0.174f;
constexpr float DEFAULT_WORDMARK_ASPECT = 10.2f;

// Neutral, no tint: the night, the logo, and the small words.
constexpr Color NIGHT{9, 9, 10, 255};
constexpr Color INK{240, 240, 240, 255};
constexpr Color DIM{150, 150, 152, 255};

// Layers over ui::layers::PANEL: glows at the back, then shapes, the wordmark, sparks, words, and
// the hit's flash over everything.
constexpr int32_t GLOW_LAYER = 0;
constexpr int32_t SHAPE_LAYER = 2;
constexpr int32_t WORDMARK_LAYER = 3;
constexpr int32_t SPARK_LAYER = 4;
constexpr int32_t TEXT_LAYER = 5;
constexpr int32_t FLASH_LAYER = 6;

// The beats' sounds, made here: a knock for each tile, rising through F major 7 (F5 A5 C6 E6), and
// on the hit a low thump with the chord over it (F3 C4 E4 A4, and E5 on top, softer). Their ids
// are far from any a game picks.
constexpr audio::SoundId FIRST_SOUND = 0x5350'4C41'5348'0000;
constexpr std::array<float, TILE_COUNT> KNOCK_HZ{698.46f, 880.0f, 1046.5f, 1318.51f};
constexpr std::array<float, 5> CHORD_HZ{174.61f, 261.63f, 329.63f, 440.0f, 659.26f};
// Sounds start early by the output's delay, less the display's (about a frame), up to a limit.
constexpr float DISPLAY_DELAY_SECONDS = 0.017f;
constexpr float MAX_LEAD_SECONDS = 0.12f;

float Clamp01(float t)
{
    return std::clamp(t, 0.0f, 1.0f);
}

float EaseOutCubic(float t)
{
    const float left = 1.0f - Clamp01(t);
    return 1.0f - (left * left * left);
}

// A flash at `start`: 1 then, fading by e every `seconds`, and 0 before.
float Flash(float age, float start, float seconds)
{
    return age < start ? 0.0f : std::exp(-(age - start) / seconds);
}

float GetBeatSeconds(int32_t beat)
{
    return FIRST_BEAT_SECONDS + (static_cast<float>(beat) * BEAT_SECONDS);
}

Color Mix(Color from, Color to, float t)
{
    const float amount = Clamp01(t);
    const auto channel = [amount](uint8_t a, uint8_t b)
    {
        const float value = static_cast<float>(a) + ((static_cast<float>(b) - static_cast<float>(a)) * amount);
        return static_cast<uint8_t>(std::lround(value));
    };
    return {channel(from.R, to.R), channel(from.G, to.G), channel(from.B, to.B), channel(from.A, to.A)};
}

// From 0 to 1, the same for the same spark and use.
float Random(uint32_t spark, uint32_t use)
{
    uint32_t value = (spark * 0x9E37'79B9U) ^ (use * 0x85EB'CA6BU) ^ 0x5F35'6495U;
    value ^= value >> 16U;
    value *= 0x7FEB'352DU;
    value ^= value >> 15U;
    value *= 0x846C'A68BU;
    value ^= value >> 16U;
    return static_cast<float>(value >> 8U) / 16777216.0f;
}

// A white image, its alpha `shape` of the distance from the middle (1 at the edge).
template <typename Shape>
renderer::Image MakeRoundImage(uint32_t size, Shape shape)
{
    renderer::Image image;
    image.Width = size;
    image.Height = size;
    image.Pixels.resize(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const glm::vec2 at =
                ((glm::vec2{static_cast<float>(x), static_cast<float>(y)} + 0.5f) / static_cast<float>(size) * 2.0f) -
                1.0f;
            const float alpha = Clamp01(shape(glm::length(at)));
            const size_t i = ((static_cast<size_t>(y) * size) + x) * 4;
            image.Pixels[i] = 255;
            image.Pixels[i + 1] = 255;
            image.Pixels[i + 2] = 255;
            image.Pixels[i + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
        }
    }
    return image;
}

// Noise from -1 to 1, the same each time from the same state.
float Noise(uint32_t& state)
{
    state = (state * 1664525U) + 1013904223U;
    return (static_cast<float>(state >> 8U) / 8388608.0f) - 1.0f;
}

void Advance(float& phase, float hz, uint32_t rate)
{
    phase += 2.0f * PI * hz / static_cast<float>(rate);
    if (phase > 2.0f * PI)
    {
        phase -= 2.0f * PI;
    }
}

audio::PcmBuffer ToPcm(const std::vector<float>& wave, uint32_t rate)
{
    audio::PcmBuffer pcm;
    pcm.SampleRate = rate;
    pcm.Samples.resize(wave.size() * audio::PcmBuffer::CHANNELS);
    for (size_t i = 0; i < wave.size(); ++i)
    {
        const auto sample = static_cast<int16_t>(std::lround(std::clamp(wave[i], -1.0f, 1.0f) * 32767.0f));
        pcm.Samples[i * audio::PcmBuffer::CHANNELS] = sample;
        pcm.Samples[(i * audio::PcmBuffer::CHANNELS) + 1] = sample;
    }
    return pcm;
}

// A struck, tuned knock: its pitch drops a moment on the attack, with a click of noise.
audio::PcmBuffer MakeKnock(uint32_t rate, float hz)
{
    std::vector<float> wave(static_cast<size_t>(static_cast<float>(rate) * 0.25f));
    float phase = 0.0f;
    uint32_t noise = 1;
    for (size_t i = 0; i < wave.size(); ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        Advance(phase, hz * (1.0f + (0.6f * std::exp(-t / 0.004f))), rate);
        const float tone = (std::sin(phase) + (0.3f * std::sin(2.0f * phase)) + (0.12f * std::sin(3.0f * phase))) *
                           std::exp(-t / 0.05f);
        const float click = Noise(noise) * std::exp(-t / 0.0015f);
        wave[i] = (0.3f * tone) + (0.12f * click);
    }
    return ToPcm(wave, rate);
}

// The hit: a thump falling from 160 Hz to 48, a snap of noise, and the chord ringing out over them.
audio::PcmBuffer MakeHit(uint32_t rate)
{
    std::vector<float> wave(static_cast<size_t>(static_cast<float>(rate) * 1.4f));
    float thumpPhase = 0.0f;
    std::array<float, CHORD_HZ.size()> phases{};
    uint32_t noise = 7;
    for (size_t i = 0; i < wave.size(); ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        Advance(thumpPhase, 48.0f + (112.0f * std::exp(-t / 0.035f)), rate);
        const float thump = std::sin(thumpPhase) * std::exp(-t / 0.25f);
        float chord = 0.0f;
        for (size_t note = 0; note < CHORD_HZ.size(); ++note)
        {
            Advance(phases[note], CHORD_HZ[note], rate);
            chord += std::sin(phases[note]) * (note + 1 == CHORD_HZ.size() ? 0.5f : 1.0f);
        }
        chord *= (1.0f - std::exp(-t / 0.003f)) * std::exp(-t / 0.5f);
        const float snap = Noise(noise) * std::exp(-t / 0.01f);
        wave[i] = std::tanh((0.65f * thump) + (0.09f * chord) + (0.22f * snap));
    }
    return ToPcm(wave, rate);
}

// Text spaced out, centered on `center`.
void DrawTracked(ui::Ui& ui, std::string_view text, glm::vec2 center, float size, float tracking, Color tint)
{
    float width = tracking * static_cast<float>(text.size() - 1);
    for (size_t i = 0; i < text.size(); ++i)
    {
        width += ui.MeasureText(text.substr(i, 1), size);
    }
    ui::TextOptions options;
    options.Size = size;
    options.Tint = tint;
    float x = center.x - (width * 0.5f);
    for (size_t i = 0; i < text.size(); ++i)
    {
        ui.DrawText(text.substr(i, 1), {x, center.y}, options, TEXT_LAYER);
        x += ui.MeasureText(text.substr(i, 1), size) + tracking;
    }
}

// Where everything goes on this canvas, in mark heights from MarkHeight: the logo side by side
// when the screen is wide, stacked when it's tall.
struct Layout
{
    float MarkHeight = 0.0f;
    // The mark's top left alone in the middle, before the hit, and beside the wordmark after.
    glm::vec2 MarkStart{0.0f};
    glm::vec2 MarkEnd{0.0f};
    ui::Rect Wordmark;
    // Where the wordmark starts, from where it ends: behind the mark.
    glm::vec2 RevealFrom{0.0f};
    float LineLeft = 0.0f;
    float LineRight = 0.0f;
    float LineY = 0.0f;
    // Over the logo, for "made with".
    glm::vec2 Words{0.0f};
    bool IsStacked = false;
};

Layout MakeLayout(glm::vec2 canvas, bool isPortrait, float wordmarkAspect)
{
    Layout layout;
    layout.IsStacked = isPortrait;
    const glm::vec2 center{canvas.x * 0.5f, canvas.y * 0.48f};
    const float wordmarkWidth = WORDMARK_HEIGHT * wordmarkAspect;
    if (!isPortrait)
    {
        const float width = MARK_ASPECT + WORDMARK_GAP + wordmarkWidth;
        const float h = std::min(canvas.y * 0.26f, std::min(canvas.x * 0.6f, 1150.0f) / width);
        const float left = center.x - (h * width * 0.5f);
        const float top = center.y - (h * 0.5f);
        const float wordmarkLeft = left + (h * (MARK_ASPECT + WORDMARK_GAP));
        layout.MarkHeight = h;
        layout.MarkStart = {center.x - (h * MARK_ASPECT * 0.5f), top};
        layout.MarkEnd = {left, top};
        layout.Wordmark = {{wordmarkLeft, center.y - (h * WORDMARK_HEIGHT * 0.5f)},
                           {wordmarkLeft + (h * wordmarkWidth), center.y + (h * WORDMARK_HEIGHT * 0.5f)}};
        layout.RevealFrom = {-h * (WORDMARK_GAP + (wordmarkWidth * 0.45f)), 0.0f};
        layout.LineLeft = left;
        layout.LineRight = left + (h * width);
        layout.LineY = top + (h * 1.3f);
        layout.Words = {center.x, top - (h * 0.36f)};
        return layout;
    }
    const float height = 1.0f + WORDMARK_GAP + WORDMARK_HEIGHT;
    const float h = std::min({canvas.x * 0.32f, canvas.y * 0.36f / height, canvas.x * 0.8f / wordmarkWidth});
    const float top = center.y - (h * height * 0.5f);
    const float markLeft = center.x - (h * MARK_ASPECT * 0.5f);
    const float wordmarkTop = top + (h * (1.0f + WORDMARK_GAP));
    const float halfWidth = h * wordmarkWidth * 0.5f;
    layout.MarkHeight = h;
    layout.MarkStart = {markLeft, center.y - (h * 0.5f)};
    layout.MarkEnd = {markLeft, top};
    layout.Wordmark = {{center.x - halfWidth, wordmarkTop},
                       {center.x + halfWidth, wordmarkTop + (h * WORDMARK_HEIGHT)}};
    layout.RevealFrom = {0.0f, -h * (WORDMARK_GAP + WORDMARK_HEIGHT)};
    layout.LineLeft = center.x - halfWidth;
    layout.LineRight = center.x + halfWidth;
    layout.LineY = wordmarkTop + (h * (WORDMARK_HEIGHT + 0.3f));
    layout.Words = {center.x, top - (h * 0.36f)};
    return layout;
}

} // namespace

SplashScene::SplashScene(const ui::Logo& wordmarkLogo, rhi::IRenderDevice& renderDevice,
                         audio::IAudioBackend& audioBackend, NextScene nextScene)
    : wordmark(wordmarkLogo), device(renderDevice), audio(audioBackend), next(std::move(nextScene))
{
    const renderer::Image glowImage =
        MakeRoundImage(64, [](float r) { return r >= 1.0f ? 0.0f : std::exp(-4.5f * r * r) * (1.0f - (r * r)); });
    if (Result<rhi::TextureHandle> texture = renderer::CreateTexture(device, glowImage, "Splash glow"); texture)
    {
        glow = texture.Value();
    }
    const renderer::Image ringImage = MakeRoundImage(128,
                                                     [](float r)
                                                     {
                                                         const float off = (r - 0.85f) / 0.06f;
                                                         return r >= 1.0f ? 0.0f : std::exp(-off * off);
                                                     });
    if (Result<rhi::TextureHandle> texture = renderer::CreateTexture(device, ringImage, "Splash ring"); texture)
    {
        ring = texture.Value();
    }

    const uint32_t rate = audio.GetSampleRate();
    areSoundsLoaded = rate > 0;
    for (int32_t beat = 0; beat <= TILE_COUNT && areSoundsLoaded; ++beat)
    {
        audio::PcmBuffer sound =
            beat < TILE_COUNT ? MakeKnock(rate, KNOCK_HZ[static_cast<size_t>(beat)]) : MakeHit(rate);
        areSoundsLoaded =
            static_cast<bool>(audio.LoadSound(FIRST_SOUND + static_cast<audio::SoundId>(beat), std::move(sound)));
    }
}

SplashScene::~SplashScene()
{
    for (rhi::TextureHandle texture : {glow, ring})
    {
        if (texture.IsValid())
        {
            device.Destroy(texture);
        }
    }
}

void SplashScene::PlayBeats()
{
    const float lead =
        std::clamp((static_cast<float>(audio.GetEstimatedOutputLatencyUs()) / 1.0e6f) - DISPLAY_DELAY_SECONDS, 0.0f,
                   MAX_LEAD_SECONDS);
    while (areSoundsLoaded && beatsPlayed <= TILE_COUNT && age + lead >= GetBeatSeconds(beatsPlayed))
    {
        audio.Play(FIRST_SOUND + static_cast<audio::SoundId>(beatsPlayed), audio::Bus::Ui);
        ++beatsPlayed;
    }
}

void SplashScene::RunFrame(const FrameContext& frame, ui::Ui& ui)
{
    age += std::min(frame.DeltaSeconds, MAX_STEP_SECONDS);
    if (leaveAge < 0.0f && age >= LEAVE_AT_SECONDS)
    {
        leaveAge = age;
    }
    const float leaving = leaveAge < 0.0f ? 0.0f : Clamp01((age - leaveAge) / FADE_OUT_SECONDS);
    if (leaving >= 1.0f && next)
    {
        SwitchTo(std::exchange(next, nullptr)());
    }
    if (leaveAge < 0.0f)
    {
        PlayBeats();
    }

    const glm::vec2 canvas = ui.GetCanvas();
    ui.SetLayer(ui::layers::BACKGROUND);
    ui.DrawRect({{0.0f, 0.0f}, canvas}, NIGHT);
    ui.SetLayer(ui::layers::PANEL);
    renderer::SpriteBatch& sprites = ui.GetSprites();
    const float shown = 1.0f - leaving;
    const Layout layout =
        MakeLayout(canvas, ui.IsPortrait(), wordmark.IsLoaded() ? wordmark.GetAspect() : DEFAULT_WORDMARK_ASPECT);
    const float h = layout.MarkHeight;
    const float hit = GetBeatSeconds(TILE_COUNT);
    const float sinceHit = age - hit;

    const auto drawSoft = [&](rhi::TextureHandle texture, glm::vec2 center, glm::vec2 size, float alpha, int32_t layer,
                              float rotation = 0.0f)
    {
        if (alpha * shown <= 0.002f || !texture.IsValid())
        {
            return;
        }
        renderer::Sprite sprite;
        sprite.Center = center;
        sprite.Size = size;
        sprite.Rotation = rotation;
        sprite.Texture = texture;
        sprite.Tint = WithAlpha(renderer::WHITE, alpha * shown);
        sprite.Layer = ui.GetLayer() + layer;
        sprites.Draw(sprite);
    };
    const auto drawLine = [&](float left, float right, float thickness, Color color)
    {
        if (right - left >= 0.5f)
        {
            ui.DrawRoundedRect({{left, layout.LineY - (thickness * 0.5f)}, {right, layout.LineY + (thickness * 0.5f)}},
                               thickness * 0.5f, WithAlpha(color, shown), SHAPE_LAYER);
        }
    };

    // A faint light behind it all, swelling on each beat.
    float swell = 0.0f;
    for (int32_t beat = 0; beat < TILE_COUNT; ++beat)
    {
        swell += 0.025f * Flash(age, GetBeatSeconds(beat), 0.22f);
    }
    swell += 0.07f * Flash(age, hit, 0.5f);
    drawSoft(glow, canvas * glm::vec2{0.5f, 0.48f}, canvas * glm::vec2{1.2f, 1.0f}, 0.03f + swell, GLOW_LAYER);

    // The mark: alone in the middle until the hit, then aside for the wordmark, jolted by the hit.
    const float slide = EaseOutCubic(sinceHit / SLIDE_SECONDS);
    const glm::vec2 markTopLeft = glm::mix(layout.MarkStart, layout.MarkEnd, slide);
    const glm::vec2 markSize = glm::vec2{MARK_ASPECT, 1.0f} * h;
    const glm::vec2 markCenter = markTopLeft + (markSize * 0.5f);
    const float punch = 1.0f + (0.04f * Flash(age, hit, 0.09f));
    for (int32_t i = 0; i < TILE_COUNT; ++i)
    {
        const float impact = GetBeatSeconds(i);
        const float fall = (age - (impact - FALL_SECONDS)) / FALL_SECONDS;
        if (fall <= 0.0f)
        {
            continue;
        }
        // Falling faster and faster onto its beat, then squashed a moment from the landing.
        const Tile& tile = TILES[static_cast<size_t>(i)];
        const float drop = fall < 1.0f ? h * DROP * (1.0f - (fall * fall)) : 0.0f;
        const float squash = 0.08f * Flash(age, impact, 0.05f);
        const glm::vec2 min = markTopLeft + (glm::vec2{tile.Left, tile.Top} * markSize);
        const glm::vec2 max = markTopLeft + (glm::vec2{tile.Right, tile.Bottom} * markSize);
        const glm::vec2 size = (max - min) * glm::vec2{1.0f + squash, 1.0f - squash};
        const glm::vec2 bottom{(min.x + max.x) * 0.5f, max.y};
        ui::Rect rect{bottom - (size * glm::vec2{0.5f, 1.0f}), bottom + (size * glm::vec2{0.5f, 0.0f})};
        rect = {markCenter + ((rect.Min - markCenter) * punch) - glm::vec2{0.0f, drop},
                markCenter + ((rect.Max - markCenter) * punch) - glm::vec2{0.0f, drop}};
        const float flash = Flash(age, impact, 0.12f);
        ui.DrawRoundedRect(rect, TILE_RADIUS * h * punch,
                           WithAlpha(Mix(INK, renderer::WHITE, flash), Clamp01(fall * 4.0f) * shown), SHAPE_LAYER);
        drawSoft(glow, rect.Center(), rect.Size() * 2.4f, 0.45f * flash, GLOW_LAYER);
    }

    // The line under it: a dash lit with each tile, shooting out from its middle and flaring.
    const float lineWidth = layout.LineRight - layout.LineLeft;
    const float thickness = std::max(2.0f, h * 0.018f);
    const float segment = lineWidth / static_cast<float>(TILE_COUNT);
    const float dash = segment - std::min(segment * 0.18f, h * 0.08f);
    for (int32_t i = 0; i < TILE_COUNT; ++i)
    {
        const float impact = GetBeatSeconds(i);
        if (age < impact)
        {
            continue;
        }
        const float middle = layout.LineLeft + (segment * (static_cast<float>(i) + 0.5f));
        const float half = dash * 0.5f * EaseOutCubic((age - impact) / DASH_SECONDS);
        const float flash = Flash(age, impact, 0.16f);
        drawLine(middle - half, middle + half, thickness * (1.0f + (1.5f * flash)), Mix(INK, renderer::WHITE, flash));
        drawSoft(glow, {middle, layout.LineY}, {dash * 1.5f, thickness * 16.0f}, 0.12f + (0.5f * flash), GLOW_LAYER);
    }

    if (sinceHit >= 0.0f)
    {
        // The hit: the dashes join from the middle out, a bright head running to each end, the
        // line flaring, a wave going out from it and sparks thrown off where the heads pass.
        const float middle = (layout.LineLeft + layout.LineRight) * 0.5f;
        const float join = EaseOutCubic(sinceHit / JOIN_SECONDS);
        const float reach = lineWidth * 0.5f * join;
        const float flare = Flash(age, hit, 0.14f);
        drawLine(middle - reach, middle + reach, thickness * (1.0f + (2.5f * flare)), Mix(INK, renderer::WHITE, flare));
        for (const float side : {-1.0f, 1.0f})
        {
            drawSoft(glow, {middle + (side * reach), layout.LineY}, {thickness * 18.0f, thickness * 10.0f},
                     0.9f * (1.0f - join), SPARK_LAYER);
        }
        drawSoft(glow, {middle, layout.LineY}, {lineWidth * 1.3f, h * (0.35f + (0.25f * flare))},
                 0.1f + (0.5f * Flash(age, hit, 0.3f)), GLOW_LAYER);
        const float wave = EaseOutCubic(sinceHit / WAVE_SECONDS);
        drawSoft(ring, {middle, layout.LineY}, glm::vec2{lineWidth * 1.15f, h * 1.6f} * wave, 0.35f * (1.0f - wave),
                 GLOW_LAYER);
        for (uint32_t i = 0; i < SPARK_COUNT; ++i)
        {
            const float along = (Random(i, 0) * 2.0f) - 1.0f;
            // When the head passed it: the join's easing, undone.
            const float born = hit + (JOIN_SECONDS * (1.0f - std::cbrt(1.0f - std::abs(along))));
            const float life = 0.35f + (0.45f * Random(i, 1));
            const float t = age - born;
            if (t < 0.0f || t >= life)
            {
                continue;
            }
            const float speed = h * (1.4f + (2.6f * Random(i, 2)));
            const float side = Random(i, 3) < 0.5f ? -1.0f : 1.0f;
            const glm::vec2 velocity =
                glm::vec2{((Random(i, 4) - 0.5f) * 1.2f) + (along * 0.6f), side * (0.35f + (0.65f * Random(i, 5)))} *
                speed;
            const float slowing = std::exp(-t * SPARK_DRAG);
            const glm::vec2 position = glm::vec2{middle + (along * lineWidth * 0.5f), layout.LineY} +
                                       (velocity * ((1.0f - slowing) / SPARK_DRAG));
            const glm::vec2 moving = velocity * slowing;
            const float fade = 1.0f - (t / life);
            drawSoft(glow, position, {(thickness * 2.0f) + (glm::length(moving) * 0.05f), thickness * 2.2f},
                     fade * fade, SPARK_LAYER, std::atan2(moving.y, moving.x));
        }
        ui.DrawRect({{0.0f, 0.0f}, canvas}, WithAlpha(renderer::WHITE, 0.045f * Flash(age, hit, 0.08f) * shown),
                    FLASH_LAYER);

        // The wordmark slides out from behind the mark: nothing of it shows on the mark's side.
        if (wordmark.IsLoaded())
        {
            const float reveal = EaseOutCubic((sinceHit - 0.03f) / REVEAL_SECONDS);
            const glm::vec2 offset = layout.RevealFrom * (1.0f - reveal);
            const glm::vec2 markMax = markTopLeft + markSize;
            const float edge = h * WORDMARK_GAP * 0.35f;
            sprites.SetClip(layout.IsStacked ? glm::vec2{0.0f, markMax.y + edge} : glm::vec2{markMax.x + edge, 0.0f},
                            canvas);
            ui::DrawLogo(ui, wordmark, {layout.Wordmark.Min + offset, layout.Wordmark.Max + offset},
                         WithAlpha(INK, Clamp01(reveal * 3.0f) * shown), WORDMARK_LAYER);
            sprites.ClearClip();
        }
    }

    // "Made with" over it from the start, its letters closing up as it fades in.
    const float words = EaseOutCubic((age - 0.05f) / 0.6f);
    if (words > 0.0f)
    {
        const float size = std::max(h * 0.12f, 16.0f);
        DrawTracked(ui, "MADE WITH", layout.Words, size, size * (0.65f - (0.3f * words)),
                    WithAlpha(DIM, words * shown));
    }
}

} // namespace hyoshi::app
