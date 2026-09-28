#include "circle/CirclePlayfield.h"

#include "renderer/Image.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace hyoshi::circle
{

namespace
{

using renderer::Color;
using renderer::Sprite;
using renderer::WithAlpha;
using rhythm::Judgment;

// The playfield's shapes, generated side by side in one texture, so a note's parts share it and
// draw together.
enum class Shape : uint32_t
{
    Disc,
    // Gray with a light rim.
    HitCircle,
    Ring,
    // A chevron pointing right: a slider's reverse arrow.
    Arrow,
    Count
};

constexpr uint32_t SHAPE_TEXELS = 256;
constexpr auto SHAPE_COUNT = static_cast<uint32_t>(Shape::Count);
// The shapes' radius in texels: a margin keeps the linear filter from reaching the next shape.
constexpr float SHAPE_RADIUS = 124.0f;
// Anti-aliased edges, this many texels wide. Wider than one so that shrunk shapes stay smooth.
constexpr float EDGE_TEXELS = 2.5f;

// Layers, above CirclePlayfieldStyle::FirstLayer.
constexpr int32_t FOLLOW_POINT_LAYER = 0;
constexpr int32_t BODY_LAYER = 1;
constexpr int32_t NOTE_LAYER = 2;
constexpr int32_t APPROACH_LAYER = 3;
constexpr int32_t FEEDBACK_LAYER = 4;

// A note fades in over the first 40% of its approach, and an unstruck one fades out over 0.2 s
// after its time. A struck one vanishes into a pop that grows by half as it fades, over 0.1 s.
constexpr float FADE_IN_FRACTION = 0.4f;
constexpr SongTimeUs FADE_OUT_US = 200'000;
constexpr SongTimeUs POP_US = 100'000;
constexpr float POP_GROWTH = 0.5f;
// The approach circle starts at three times the note's size.
constexpr float APPROACH_START_SCALE = 3.0f;
// The result mark: 0.5 s, 70% of a note's size.
constexpr SongTimeUs MARK_US = 500'000;
constexpr float MARK_SIZE = 0.7f;

// A slider snakes in from its head between 1/3 and 2/3 of the way through its approach.
constexpr float SNAKE_START = 2.0f / 3.0f;
constexpr float SNAKE_SPEED = 3.0f;

constexpr Color NOTE_TINT{255, 255, 255, 255};
constexpr Color NUMBER_TINT{255, 255, 255, 235};
constexpr float NUMBER_SIZE = 0.5f;
constexpr Color BODY_BORDER{99, 99, 99, 255};
constexpr Color BODY_FILL{55, 55, 55, 255};
constexpr float BODY_FILL_SCALE = 0.84f;
constexpr float ARROW_SIZE = 0.5f;
// The follow circle around a held slider's ball.
constexpr Color FOLLOW_CIRCLE_TINT{255, 255, 255, 140};
constexpr float FOLLOW_CIRCLE_SCALE = 2.0f;
constexpr Color SPINNER_TINT{255, 255, 255, 170};
constexpr float SPINNER_SIZE = 0.8f * PLAYFIELD_HEIGHT;

constexpr Color GREAT_TINT{255, 230, 51, 255};
constexpr Color GOOD_TINT{51, 230, 77, 255};
constexpr Color BAD_TINT{77, 128, 255, 255};
constexpr Color MISS_TINT{255, 38, 38, 255};

// Follow points: a dash every 32 units, starting 48 units out of the note it leaves and ending 32
// before the next. Each appears 0.8 s before the moment it marks, gliding a tenth of the way in
// over 0.35 s, and fades over 0.18 s after that moment.
constexpr float FOLLOW_SPACING = 32.0f;
constexpr float FOLLOW_LEAD_IN = FOLLOW_SPACING * 1.5f;
constexpr float FOLLOW_TAIL_GAP = FOLLOW_SPACING;
constexpr float FOLLOW_GLIDE = 0.1f;
constexpr SongTimeUs FOLLOW_PREEMPT_US = 800'000;
constexpr SongTimeUs FOLLOW_FADE_IN_US = 350'000;
constexpr SongTimeUs FOLLOW_FADE_OUT_US = 180'000;
constexpr float FOLLOW_LENGTH = 0.3f;
constexpr Color FOLLOW_TINT{255, 255, 255, 140};
// A chord's follow points split toward its notes at this angle between them, and before that
// share one line.
constexpr float FOLLOW_SPLIT_ANGLE = std::numbers::pi_v<float> / 3.0f;
constexpr float EPSILON = 1.0e-4f;

float Coverage(float insideTexels)
{
    return std::clamp((insideTexels / EDGE_TEXELS) + 0.5f, 0.0f, 1.0f);
}

float DistanceToSegment(glm::vec2 point, glm::vec2 a, glm::vec2 b)
{
    const glm::vec2 ab = b - a;
    const float t = std::clamp(glm::dot(point - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
    return glm::distance(point, a + (ab * t));
}

// A shape's color and coverage at a point, in texels from its center.
glm::vec4 ShapeTexel(Shape shape, glm::vec2 point)
{
    const float distance = glm::length(point);
    switch (shape)
    {
    case Shape::Disc:
        return {1.0f, 1.0f, 1.0f, Coverage(SHAPE_RADIUS - distance)};
    case Shape::HitCircle:
    {
        const float inner = SHAPE_RADIUS * 0.86f;
        const float rim = 0.8f;
        const float fill = 0.65f;
        const float line = 0.55f;
        float gray = glm::mix(rim, fill, Coverage(inner - distance));
        gray = glm::mix(gray, line, 0.8f * Coverage(1.5f - std::abs(distance - inner)));
        return {gray, gray, gray, Coverage(SHAPE_RADIUS - distance)};
    }
    case Shape::Ring:
    {
        const float inner = SHAPE_RADIUS * 0.94f;
        return {1.0f, 1.0f, 1.0f, Coverage(std::min(SHAPE_RADIUS - distance, distance - inner))};
    }
    case Shape::Arrow:
    {
        const glm::vec2 tip{SHAPE_RADIUS * 0.3f, 0.0f};
        const glm::vec2 top{-SHAPE_RADIUS * 0.35f, -SHAPE_RADIUS * 0.55f};
        const glm::vec2 bottom{-SHAPE_RADIUS * 0.35f, SHAPE_RADIUS * 0.55f};
        const float halfWidth = SHAPE_RADIUS * 0.14f;
        const float fromLines = std::min(DistanceToSegment(point, top, tip), DistanceToSegment(point, tip, bottom));
        return {1.0f, 1.0f, 1.0f, Coverage(halfWidth - fromLines)};
    }
    case Shape::Count:
        break;
    }
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

renderer::Image MakeShapes()
{
    renderer::Image image;
    image.Width = SHAPE_TEXELS * SHAPE_COUNT;
    image.Height = SHAPE_TEXELS;
    image.Channels = 4;
    image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height * 4);
    const float center = static_cast<float>(SHAPE_TEXELS) * 0.5f;
    for (uint32_t s = 0; s < SHAPE_COUNT; ++s)
    {
        for (uint32_t y = 0; y < SHAPE_TEXELS; ++y)
        {
            for (uint32_t x = 0; x < SHAPE_TEXELS; ++x)
            {
                const glm::vec2 point{static_cast<float>(x) + 0.5f - center, static_cast<float>(y) + 0.5f - center};
                const glm::vec4 texel = ShapeTexel(static_cast<Shape>(s), point);
                const size_t offset = ((static_cast<size_t>(y) * image.Width) + (s * SHAPE_TEXELS) + x) * 4;
                for (glm::length_t c = 0; c < 4; ++c)
                {
                    image.Pixels[offset + static_cast<size_t>(c)] =
                        static_cast<uint8_t>(std::lround(std::clamp(texel[c], 0.0f, 1.0f) * 255.0f));
                }
            }
        }
    }
    return image;
}

float Saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float Ratio(SongTimeUs part, SongTimeUs whole)
{
    return whole > 0 ? static_cast<float>(part) / static_cast<float>(whole) : 1.0f;
}

// Fades in over the approach and out after the note's time.
float GetNoteAlpha(const CircleNote& note, SongTimeUs time, SongTimeUs approachUs)
{
    const SongTimeUs untilHit = note.Time - time;
    if (untilHit > approachUs)
    {
        return 0.0f;
    }
    if (untilHit < 0)
    {
        return Saturate(1.0f - Ratio(-untilHit, FADE_OUT_US));
    }
    return Saturate((1.0f - Ratio(untilHit, approachUs)) / FADE_IN_FRACTION);
}

bool IsStruck(const CircleJudge& judge, uint32_t note)
{
    const NoteState state = judge.GetNoteState(note);
    return state == NoteState::Hit || state == NoteState::Held;
}

glm::vec2 GetHeadPosition(const CircleChart& chart, const CircleNote& note)
{
    return note.Type == NoteType::Slider && note.PathPointCount >= 2 ? GetPathPosition(chart, note, 0.0f)
                                                                     : note.GetStackedPosition();
}

// Where a note is left from: a slider's ball at its end, or the note itself.
glm::vec2 GetLeavePosition(const CircleChart& chart, const CircleNote& note)
{
    return note.Type == NoteType::Slider && note.PathPointCount >= 2 ? GetBallPosition(chart, note, note.EndTime)
                                                                     : note.GetStackedPosition();
}

// How far along its path a slider's ball is, and which pass it's on.
struct BallProgress
{
    float Progress = 0.0f;
    uint32_t Pass = 0;
    bool IsForward = true;
};

BallProgress GetBallProgress(const CircleNote& note, SongTimeUs time)
{
    const uint32_t passes = std::max<uint32_t>(note.Passes, 1);
    const SongTimeUs span = note.EndTime - note.Time;
    const float along = Saturate(Ratio(time - note.Time, span)) * static_cast<float>(passes);
    BallProgress ball;
    ball.Pass = std::min(static_cast<uint32_t>(along), passes - 1);
    const float within = along - static_cast<float>(ball.Pass);
    ball.IsForward = ball.Pass % 2 == 0;
    ball.Progress = ball.IsForward ? within : 1.0f - within;
    return ball;
}

Color GetMarkTint(Judgment result)
{
    switch (result)
    {
    case Judgment::Perfect:
    case Judgment::Great:
        return GREAT_TINT;
    case Judgment::Good:
        return GOOD_TINT;
    case Judgment::Bad:
        return BAD_TINT;
    case Judgment::Miss:
    case Judgment::Count:
        break;
    }
    return MISS_TINT;
}

const char* GetMarkText(Judgment result)
{
    switch (result)
    {
    case Judgment::Perfect:
    case Judgment::Great:
        return "300";
    case Judgment::Good:
        return "100";
    case Judgment::Bad:
        return "50";
    case Judgment::Miss:
    case Judgment::Count:
        break;
    }
    return "";
}

} // namespace

// What one Draw call works with.
struct CirclePlayfield::Frame
{
    renderer::SpriteBatch& Sprites;
    renderer::TextRenderer& Text;
    const CircleChart& Chart;
    const CircleJudge& Judge;
    const CircleLayout& Layout;
    const CirclePlayfieldStyle& Style;
    rhi::TextureHandle Shapes;
    SongTimeUs Time = 0;
    SongTimeUs ApproachUs = 0;
    // A note's diameter on the canvas.
    float Diameter = 0.0f;

    int32_t GetLayer(int32_t offset) const
    {
        return Style.FirstLayer + offset;
    }

    void DrawShape(Shape shape, glm::vec2 position, float size, Color tint, int32_t layer, float rotation = 0.0f) const
    {
        const float u0 = static_cast<float>(shape) / static_cast<float>(SHAPE_COUNT);
        const float u1 = (static_cast<float>(shape) + 1.0f) / static_cast<float>(SHAPE_COUNT);
        Sprite sprite;
        sprite.Center = Layout.ToCanvas(position);
        sprite.Size = {size, size};
        sprite.Rotation = rotation;
        sprite.UvRect = {u0, 0.0f, u1, 1.0f};
        sprite.Tint = tint;
        sprite.Texture = Shapes;
        sprite.Layer = GetLayer(layer);
        Sprites.Draw(sprite);
    }

    void DrawText(const std::string& text, glm::vec2 position, float size, Color tint, int32_t layer) const
    {
        renderer::TextStyle style;
        style.Font = Style.NumberFont;
        style.Size = size;
        style.Tint = tint;
        style.Align = renderer::TextAlign::Center;
        style.Baseline = renderer::TextBaseline::Middle;
        style.Layer = GetLayer(layer);
        Text.Draw(Sprites, text, Layout.ToCanvas(position), style);
    }
};

CircleLayout CircleLayout::Fit(glm::vec2 canvas)
{
    // osu!'s 640 by 480 screen, whose middle 512 by 384 is the playfield.
    constexpr float SCREEN_WIDTH = 640.0f;
    constexpr float SCREEN_HEIGHT = 480.0f;
    constexpr float SHIFT_DOWN = 8.0f;
    CircleLayout layout;
    layout.Scale = std::min(canvas.x / SCREEN_WIDTH, canvas.y / SCREEN_HEIGHT);
    layout.Origin = (canvas * 0.5f) - (glm::vec2{PLAYFIELD_WIDTH, PLAYFIELD_HEIGHT} * (0.5f * layout.Scale)) +
                    glm::vec2{0.0f, SHIFT_DOWN * layout.Scale};
    return layout;
}

Result<void> CirclePlayfield::Initialize(rhi::IRenderDevice& renderDevice)
{
    device = &renderDevice;
    Result<rhi::TextureHandle> texture = renderer::CreateTexture(renderDevice, MakeShapes(), "Circle shapes");
    if (!texture)
    {
        return texture.GetError();
    }
    shapes = texture.Value();
    return {};
}

void CirclePlayfield::Shutdown()
{
    if (device != nullptr && shapes.IsValid())
    {
        device->Destroy(shapes);
    }
    shapes = {};
    device = nullptr;
}

void CirclePlayfield::Reset(const CircleChart& chart)
{
    feedback.assign(chart.Notes.size(), Feedback{});
    firstNote = 0;
    firstFollowPoint = 0;
    lastTime = INT64_MIN;
    BuildFollowPoints(chart);
}

void CirclePlayfield::OnJudgment(const rhythm::JudgmentEvent& event)
{
    if (event.NoteIndex < feedback.size())
    {
        feedback[event.NoteIndex] = {event.Time, event.Result};
    }
}

void CirclePlayfield::Draw(renderer::SpriteBatch& sprites, renderer::TextRenderer& text, const CircleChart& chart,
                           const CircleJudge& judge, SongTimeUs time, const CircleLayout& layout,
                           const CirclePlayfieldStyle& style)
{
    const std::vector<CircleNote>& notes = chart.Notes;
    if (!shapes.IsValid() || notes.empty() || feedback.size() != notes.size())
    {
        return;
    }
    // Played back (a restart): start looking from the beginning again.
    if (time < lastTime)
    {
        firstNote = 0;
        firstFollowPoint = 0;
    }
    lastTime = time;

    const Frame frame{sprites,
                      text,
                      chart,
                      judge,
                      layout,
                      style,
                      shapes,
                      time,
                      chart.Difficulty.GetApproachUs(),
                      2.0f * chart.Difficulty.GetRadius() * layout.Scale};
    // Notes overlap back to front, each with its number on it.
    sprites.KeepOrder(frame.GetLayer(BODY_LAYER));
    sprites.KeepOrder(frame.GetLayer(NOTE_LAYER));

    // A note is done drawing once its body has ended and its result mark has faded. It's judged
    // by the end of its Bad window at the latest.
    const SongTimeUs lingerUs = judge.GetWindows().MehUs + 1 + MARK_US;
    while (firstNote < notes.size() &&
           time > std::max(notes[firstNote].EndTime, notes[firstNote].Time + std::max(lingerUs, FADE_OUT_US)))
    {
        ++firstNote;
    }
    auto end = static_cast<uint32_t>(firstNote);
    while (end < notes.size() && notes[end].Time - frame.ApproachUs <= time)
    {
        ++end;
    }

    if (style.AreFollowPointsShown)
    {
        DrawFollowPoints(frame);
    }
    // Latest first, so earlier notes land on top.
    for (uint32_t i = end; i-- > firstNote;)
    {
        if (notes[i].Type == NoteType::Slider)
        {
            DrawSliderBody(frame, i);
        }
        else if (notes[i].Type == NoteType::Spinner)
        {
            DrawSpinner(frame, i);
        }
    }
    for (uint32_t i = end; i-- > firstNote;)
    {
        if (notes[i].Type != NoteType::Spinner)
        {
            DrawHead(frame, i);
        }
        if (notes[i].Type == NoteType::Slider)
        {
            DrawSliderBall(frame, i);
        }
    }
    for (uint32_t i = end; i-- > firstNote;)
    {
        DrawApproach(frame, i);
    }
    for (uint32_t i = firstNote; i < end; ++i)
    {
        DrawFeedback(frame, i);
    }
}

void CirclePlayfield::DrawFollowPoints(const Frame& frame)
{
    // Sorted by FadeOutTime, and so by FadeInTime too.
    while (firstFollowPoint < followPoints.size() &&
           frame.Time > followPoints[firstFollowPoint].FadeOutTime + FOLLOW_FADE_OUT_US)
    {
        ++firstFollowPoint;
    }

    const glm::vec2 size{FOLLOW_LENGTH * frame.Diameter, frame.Style.FollowPointThickness * frame.Diameter};
    for (size_t i = firstFollowPoint; i < followPoints.size(); ++i)
    {
        const FollowPoint& point = followPoints[i];
        if (point.FadeInTime > frame.Time)
        {
            break;
        }
        const SongTimeUs past = frame.Time - point.FadeOutTime;
        if (past > FOLLOW_FADE_OUT_US)
        {
            continue;
        }
        const float appear = Saturate(Ratio(frame.Time - point.FadeInTime, FOLLOW_FADE_IN_US));
        // Easing out, so the dash settles into place.
        const float eased = 1.0f - ((1.0f - appear) * (1.0f - appear));
        float alpha = appear;
        if (past > 0)
        {
            alpha *= 1.0f - Ratio(past, FOLLOW_FADE_OUT_US);
        }
        Sprite sprite;
        sprite.Center = frame.Layout.ToCanvas(glm::mix(point.From, point.To, eased));
        sprite.Size = size;
        sprite.Rotation = point.Rotation;
        sprite.Tint = WithAlpha(FOLLOW_TINT, alpha);
        sprite.Layer = frame.GetLayer(FOLLOW_POINT_LAYER);
        frame.Sprites.Draw(sprite);
    }
}

// The body is a run of overlapping discs along the path: the border color, then the fill color
// smaller on top. The discs are opaque, so the body can't fade: it snakes in at full strength,
// under its fading head.
void CirclePlayfield::DrawSliderBody(const Frame& frame, uint32_t index) const
{
    const CircleNote& note = frame.Chart.Notes[index];
    const SongTimeUs untilHit = note.Time - frame.Time;
    if (note.PathPointCount < 2 || untilHit > frame.ApproachUs || frame.Time > note.EndTime)
    {
        return;
    }

    float start = 0.0f;
    float end = 1.0f;
    BallProgress ball;
    const bool isActive = untilHit <= 0;
    if (isActive)
    {
        // On the last pass, the body shrinks behind the ball.
        ball = GetBallProgress(note, frame.Time);
        const bool isLastPass = ball.Pass + 1 >= std::max<uint32_t>(note.Passes, 1);
        if (isLastPass)
        {
            (ball.IsForward ? start : end) = ball.Progress;
        }
    }
    else
    {
        end = Saturate((SNAKE_START - Ratio(untilHit, frame.ApproachUs)) * SNAKE_SPEED);
    }
    if (end <= start)
    {
        return;
    }

    const glm::vec2* path = &frame.Chart.Path[note.FirstPathPoint];
    const uint32_t last = note.PathPointCount - 1;
    const auto first = static_cast<uint32_t>(std::ceil(start * static_cast<float>(last)));
    const auto lastInRange = static_cast<uint32_t>(std::floor(end * static_cast<float>(last)));
    const glm::vec2 startPosition = GetPathPosition(frame.Chart, note, start);
    const glm::vec2 endPosition = GetPathPosition(frame.Chart, note, end);
    for (const auto& [size, tint] :
         {std::pair{frame.Diameter, BODY_BORDER}, std::pair{frame.Diameter * BODY_FILL_SCALE, BODY_FILL}})
    {
        frame.DrawShape(Shape::Disc, startPosition, size, tint, BODY_LAYER);
        for (uint32_t p = first; p <= lastInRange && p <= last; ++p)
        {
            frame.DrawShape(Shape::Disc, path[p] + note.StackOffset, size, tint, BODY_LAYER);
        }
        frame.DrawShape(Shape::Disc, endPosition, size, tint, BODY_LAYER);
    }

    // Reverse arrows at the ends the ball has yet to turn back from.
    const uint32_t passes = std::max<uint32_t>(note.Passes, 1);
    bool isArrowAtEnd = false;
    bool isArrowAtStart = false;
    for (uint32_t pass = isActive ? ball.Pass : 0; pass + 1 < passes; ++pass)
    {
        (pass % 2 == 0 ? isArrowAtEnd : isArrowAtStart) = true;
    }
    const float arrowSize = frame.Diameter * ARROW_SIZE;
    if (isArrowAtEnd && end >= 1.0f)
    {
        const glm::vec2 along = path[last] - path[last - 1];
        frame.DrawShape(Shape::Arrow, path[last] + note.StackOffset, arrowSize, NOTE_TINT, BODY_LAYER,
                        std::atan2(-along.y, -along.x));
    }
    if (isArrowAtStart && start <= 0.0f)
    {
        const glm::vec2 along = path[1] - path[0];
        frame.DrawShape(Shape::Arrow, path[0] + note.StackOffset, arrowSize, NOTE_TINT, BODY_LAYER,
                        std::atan2(along.y, along.x));
    }
}

void CirclePlayfield::DrawHead(const Frame& frame, uint32_t index) const
{
    const CircleNote& note = frame.Chart.Notes[index];
    if (IsStruck(frame.Judge, index))
    {
        return;
    }
    const float alpha = GetNoteAlpha(note, frame.Time, frame.ApproachUs);
    if (alpha <= 0.0f)
    {
        return;
    }
    const glm::vec2 head = GetHeadPosition(frame.Chart, note);
    frame.DrawShape(Shape::HitCircle, head, frame.Diameter, WithAlpha(NOTE_TINT, alpha), NOTE_LAYER);
    if (frame.Style.AreComboNumbersShown)
    {
        frame.DrawText(std::to_string(note.ComboNumber), head, frame.Diameter * NUMBER_SIZE,
                       WithAlpha(NUMBER_TINT, alpha), NOTE_LAYER);
    }
}

void CirclePlayfield::DrawSliderBall(const Frame& frame, uint32_t index) const
{
    const CircleNote& note = frame.Chart.Notes[index];
    if (note.PathPointCount < 2 || frame.Time < note.Time || frame.Time > note.EndTime)
    {
        return;
    }
    const glm::vec2 ball = GetBallPosition(frame.Chart, note, frame.Time);
    frame.DrawShape(Shape::Disc, ball, frame.Diameter, BODY_BORDER, NOTE_LAYER);
    frame.DrawShape(Shape::Ring, ball, frame.Diameter, NOTE_TINT, NOTE_LAYER);
    frame.DrawShape(Shape::Ring, ball, frame.Diameter * 0.8f, NOTE_TINT, NOTE_LAYER);
    if (frame.Judge.GetNoteState(index) == NoteState::Held)
    {
        frame.DrawShape(Shape::Ring, ball, frame.Diameter * FOLLOW_CIRCLE_SCALE, FOLLOW_CIRCLE_TINT, NOTE_LAYER);
    }
}

void CirclePlayfield::DrawApproach(const Frame& frame, uint32_t index) const
{
    const CircleNote& note = frame.Chart.Notes[index];
    const SongTimeUs untilHit = note.Time - frame.Time;
    if (note.Type == NoteType::Spinner || IsStruck(frame.Judge, index) || untilHit < 0 || untilHit > frame.ApproachUs)
    {
        return;
    }
    const float scale = 1.0f + (Ratio(untilHit, frame.ApproachUs) * (APPROACH_START_SCALE - 1.0f));
    frame.DrawShape(Shape::Ring, GetHeadPosition(frame.Chart, note), frame.Diameter * scale,
                    WithAlpha(NOTE_TINT, GetNoteAlpha(note, frame.Time, frame.ApproachUs)), APPROACH_LAYER);
}

// A ring that fades in like a note, with a second ring inside it that empties over the spin.
void CirclePlayfield::DrawSpinner(const Frame& frame, uint32_t index) const
{
    const CircleNote& note = frame.Chart.Notes[index];
    if (frame.Time > note.EndTime)
    {
        return;
    }
    const float alpha = frame.Time < note.Time ? GetNoteAlpha(note, frame.Time, frame.ApproachUs) : 1.0f;
    const float size = SPINNER_SIZE * frame.Layout.Scale;
    frame.DrawShape(Shape::Ring, note.Position, size, WithAlpha(SPINNER_TINT, alpha), BODY_LAYER);
    const float left = 1.0f - Saturate(Ratio(frame.Time - note.Time, note.EndTime - note.Time));
    frame.DrawShape(Shape::Ring, note.Position, size * left, WithAlpha(SPINNER_TINT, alpha), BODY_LAYER);
    frame.DrawShape(Shape::Disc, note.Position, frame.Diameter * 0.25f, WithAlpha(SPINNER_TINT, alpha), BODY_LAYER);
}

void CirclePlayfield::DrawFeedback(const Frame& frame, uint32_t index) const
{
    const Feedback& result = feedback[index];
    const SongTimeUs age = frame.Time - result.Time;
    if (result.Time == INT64_MIN || age < 0 || age >= MARK_US)
    {
        return;
    }
    const glm::vec2 head = GetHeadPosition(frame.Chart, frame.Chart.Notes[index]);
    if (result.Result != Judgment::Miss && age < POP_US)
    {
        const float grown = Ratio(age, POP_US);
        frame.DrawShape(Shape::HitCircle, head, frame.Diameter * (1.0f + (POP_GROWTH * grown)),
                        WithAlpha(NOTE_TINT, 1.0f - grown), FEEDBACK_LAYER);
    }

    const float alpha = 1.0f - Ratio(age, MARK_US);
    const float size = frame.Diameter * MARK_SIZE;
    const Color tint = WithAlpha(GetMarkTint(result.Result), alpha);
    if (result.Result == Judgment::Miss)
    {
        // An X.
        for (const float angle : {0.25f, -0.25f})
        {
            Sprite sprite;
            sprite.Center = frame.Layout.ToCanvas(head);
            sprite.Size = {size * 0.18f, size};
            sprite.Rotation = angle * std::numbers::pi_v<float>;
            sprite.Tint = tint;
            sprite.Layer = frame.GetLayer(FEEDBACK_LAYER);
            frame.Sprites.Draw(sprite);
        }
        return;
    }
    frame.DrawText(GetMarkText(result.Result), head, size * 0.6f, tint, FEEDBACK_LAYER);
}

// Follow points join each step of a combo to the next, where a step is a note or a chord. Into a
// chord, one line runs from the last step and forks toward the chord's notes; out of one, lines
// from its notes merge first.
void CirclePlayfield::BuildFollowPoints(const CircleChart& chart)
{
    followPoints.clear();
    const std::vector<CircleNote>& notes = chart.Notes;
    const auto stepEnd = [&notes](uint32_t start)
    {
        uint32_t end = start + 1;
        while (end < notes.size() && notes[end].Time - notes[end - 1].Time <= CHORD_TOLERANCE_US)
        {
            ++end;
        }
        return end;
    };
    uint32_t source = 0;
    while (source < notes.size())
    {
        const uint32_t sourceEnd = stepEnd(source);
        if (sourceEnd >= notes.size())
        {
            break;
        }
        ConnectFollowPoints(chart, source, sourceEnd, sourceEnd, stepEnd(sourceEnd));
        source = sourceEnd;
    }
    std::stable_sort(followPoints.begin(), followPoints.end(),
                     [](const FollowPoint& a, const FollowPoint& b) { return a.FadeOutTime < b.FadeOutTime; });
}

void CirclePlayfield::ConnectFollowPoints(const CircleChart& chart, uint32_t sourceStart, uint32_t sourceEnd,
                                          uint32_t targetStart, uint32_t targetEnd)
{
    const std::vector<CircleNote>& notes = chart.Notes;
    for (uint32_t i = sourceStart; i < targetEnd; ++i)
    {
        if (notes[i].Type == NoteType::Spinner)
        {
            return;
        }
    }
    if (notes[targetStart].IsNewCombo)
    {
        return;
    }

    const auto sourceCount = static_cast<float>(sourceEnd - sourceStart);
    const auto targetCount = static_cast<float>(targetEnd - targetStart);
    glm::vec2 exit{0.0f};
    SongTimeUs startTime = INT64_MIN;
    for (uint32_t i = sourceStart; i < sourceEnd; ++i)
    {
        exit += GetLeavePosition(chart, notes[i]);
        startTime = std::max(startTime, std::max(notes[i].Time, notes[i].EndTime));
    }
    exit /= sourceCount;
    const SongTimeUs endTime = notes[targetStart].Time;
    if (endTime <= startTime)
    {
        return;
    }

    glm::vec2 centroid{0.0f};
    for (uint32_t i = targetStart; i < targetEnd; ++i)
    {
        centroid += GetHeadPosition(chart, notes[i]);
    }
    centroid /= targetCount;

    // The widest angle between the chord's notes, seen from the exit, sets where the line forks:
    // right away at FOLLOW_SPLIT_ANGLE or more, at the chord itself for one note.
    float widest = 0.0f;
    for (uint32_t a = targetStart; a < targetEnd; ++a)
    {
        for (uint32_t b = a + 1; b < targetEnd; ++b)
        {
            const glm::vec2 toA = GetHeadPosition(chart, notes[a]) - exit;
            const glm::vec2 toB = GetHeadPosition(chart, notes[b]) - exit;
            if (glm::length(toA) < EPSILON || glm::length(toB) < EPSILON)
            {
                continue;
            }
            const float cosine = std::clamp(glm::dot(glm::normalize(toA), glm::normalize(toB)), -1.0f, 1.0f);
            widest = std::max(widest, std::acos(cosine));
        }
    }
    const float forkAt = targetCount <= 1.0f ? 1.0f : Saturate(1.0f - (widest / FOLLOW_SPLIT_ANGLE));
    const glm::vec2 fork = exit + (forkAt * (centroid - exit));

    float mergeLength = 0.0f;
    for (uint32_t i = sourceStart; i < sourceEnd; ++i)
    {
        mergeLength += glm::distance(exit, GetLeavePosition(chart, notes[i]));
    }
    mergeLength /= sourceCount;
    const float trunkLength = glm::distance(fork, exit);
    float branchLength = 0.0f;
    for (uint32_t i = targetStart; i < targetEnd; ++i)
    {
        branchLength += glm::distance(GetHeadPosition(chart, notes[i]), fork);
    }
    branchLength /= targetCount;
    const float total = mergeLength + trunkLength + branchLength;
    if (total < EPSILON)
    {
        return;
    }

    // Each part of the way gets its share of the time between the steps.
    const auto duration = static_cast<double>(endTime - startTime);
    const SongTimeUs mergeTime = startTime + std::llround(duration * mergeLength / total);
    const SongTimeUs forkTime = mergeTime + std::llround(duration * trunkLength / total);
    const bool hasMerge = mergeLength > EPSILON;
    const bool hasBranches = branchLength > EPSILON;
    if (hasMerge)
    {
        for (uint32_t i = sourceStart; i < sourceEnd; ++i)
        {
            EmitFollowPoints(GetLeavePosition(chart, notes[i]), exit, startTime, mergeTime, FOLLOW_LEAD_IN, 0.0f);
        }
    }
    EmitFollowPoints(exit, fork, mergeTime, forkTime, hasMerge ? 0.0f : FOLLOW_LEAD_IN,
                     hasBranches ? 0.0f : FOLLOW_TAIL_GAP);
    if (!hasBranches)
    {
        return;
    }
    for (uint32_t i = targetStart; i < targetEnd; ++i)
    {
        EmitFollowPoints(fork, GetHeadPosition(chart, notes[i]), forkTime, endTime,
                         trunkLength > EPSILON ? 0.0f : FOLLOW_LEAD_IN, FOLLOW_TAIL_GAP);
    }
}

void CirclePlayfield::EmitFollowPoints(glm::vec2 from, glm::vec2 to, SongTimeUs startTime, SongTimeUs endTime,
                                       float leadIn, float tailGap)
{
    const glm::vec2 delta = to - from;
    const float distance = glm::length(delta);
    if (distance < leadIn + tailGap)
    {
        return;
    }
    const float rotation = std::atan2(delta.y, delta.x);
    const auto span = static_cast<double>(endTime - startTime);
    for (float along = leadIn; along < distance - tailGap; along += FOLLOW_SPACING)
    {
        const float fraction = along / distance;
        FollowPoint point;
        point.From = from + ((fraction - FOLLOW_GLIDE) * delta);
        point.To = from + (fraction * delta);
        point.Rotation = rotation;
        point.FadeOutTime = startTime + std::llround(span * fraction);
        point.FadeInTime = point.FadeOutTime - FOLLOW_PREEMPT_US;
        followPoints.push_back(point);
    }
}

} // namespace hyoshi::circle
