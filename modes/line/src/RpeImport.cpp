#include "line/PhigrosImport.h"

#include "rhythm/ChartFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hyoshi::line
{

namespace
{

// RPE's canvas: 1350 by 900, its origin in the middle, y up.
constexpr double RPE_WIDTH = 1350.0;
constexpr double RPE_HEIGHT = 900.0;
// A unit of RPE speed moves notes 120 canvas units a second, and one of ours DISTANCE_UNIT (0.6 of
// the field's height) a second: one of ours is 4.5 of RPE's.
constexpr double RPE_SPEED_PER_UNIT = 0.6 * RPE_HEIGHT / 120.0;
// What a line without events of a kind has.
constexpr double RPE_DEFAULT_SPEED = 10.0;
constexpr double RPE_OPAQUE = 255.0;
// Phira draws a plain line 6 of its units long (each half the canvas's width) at scale 1, and one
// scaled thicker or thinner 0.0076 units thick per unit of scale, in field units here. Unscaled,
// it keeps our thickness.
constexpr double RPE_LINE_LENGTH = 3.0 * FIELD_WIDTH;
constexpr double RPE_SCALED_THICKNESS = 0.0076 * FIELD_WIDTH / 2.0;
constexpr double RPE_UNSCALED = 1e-4;
// A note's x along its line, from canvas units to X units (0.05625 of the field's width).
constexpr double RPE_X_PER_UNIT = 0.05625 * RPE_WIDTH;
constexpr const char* PLAIN_LINE = "line.png";
// An eased event becomes this many straight pieces, its springy kinds (back, elastic, bounce) more,
// each at least this long.
constexpr int EASED_PIECES = 16;
constexpr int SPRINGY_PIECES = 40;
constexpr double MIN_PIECE_US = 4'000.0;
// A speed ramp becomes this many steps.
constexpr int SPEED_STEPS = 16;
constexpr int NOTE_ABOVE = 1;
// RPE's note types, 1 to 4.
constexpr std::array<NoteKind, 4> RPE_KINDS{NoteKind::Tap, NoteKind::Hold, NoteKind::Flick, NoteKind::Drag};
constexpr double US_PER_MINUTE = 60'000'000.0;

// RPE's easing kinds, numbered as Re:PhiEdit numbers them (0 and 1 linear, then sine, quad, cubic,
// quart, quint, expo, circ, back, elastic, and bounce), on x from 0 to 1.
double OutBounce(double x)
{
    constexpr double N = 7.5625;
    constexpr double D = 2.75;
    if (x < 1.0 / D)
    {
        return N * x * x;
    }
    if (x < 2.0 / D)
    {
        x -= 1.5 / D;
        return (N * x * x) + 0.75;
    }
    if (x < 2.5 / D)
    {
        x -= 2.25 / D;
        return (N * x * x) + 0.9375;
    }
    x -= 2.625 / D;
    return (N * x * x) + 0.984375;
}

double Ease(int type, double x)
{
    constexpr double PI = std::numbers::pi;
    constexpr double BACK = 1.70158;
    constexpr double BACK_IN_OUT = BACK * 1.525;
    constexpr double ELASTIC = 2.0 * PI / 3.0;
    constexpr double ELASTIC_IN_OUT = 2.0 * PI / 4.5;
    switch (type)
    {
    case 2:
        return std::sin(x * PI / 2.0);
    case 3:
        return 1.0 - std::cos(x * PI / 2.0);
    case 4:
        return 1.0 - ((1.0 - x) * (1.0 - x));
    case 5:
        return x * x;
    case 6:
        return -(std::cos(PI * x) - 1.0) / 2.0;
    case 7:
        return x < 0.5 ? 2.0 * x * x : 1.0 - (std::pow((-2.0 * x) + 2.0, 2.0) / 2.0);
    case 8:
        return 1.0 - std::pow(1.0 - x, 3.0);
    case 9:
        return x * x * x;
    case 10:
        return 1.0 - std::pow(1.0 - x, 4.0);
    case 11:
        return std::pow(x, 4.0);
    case 12:
        return x < 0.5 ? 4.0 * x * x * x : 1.0 - (std::pow((-2.0 * x) + 2.0, 3.0) / 2.0);
    case 13:
        return x < 0.5 ? 8.0 * std::pow(x, 4.0) : 1.0 - (std::pow((-2.0 * x) + 2.0, 4.0) / 2.0);
    case 14:
        return 1.0 - std::pow(1.0 - x, 5.0);
    case 15:
        return std::pow(x, 5.0);
    case 16:
        return x >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * x);
    case 17:
        return x <= 0.0 ? 0.0 : std::pow(2.0, (10.0 * x) - 10.0);
    case 18:
        return std::sqrt(std::max(1.0 - std::pow(x - 1.0, 2.0), 0.0));
    case 19:
        return 1.0 - std::sqrt(std::max(1.0 - (x * x), 0.0));
    case 20:
        return 1.0 + ((BACK + 1.0) * std::pow(x - 1.0, 3.0)) + (BACK * std::pow(x - 1.0, 2.0));
    case 21:
        return ((BACK + 1.0) * x * x * x) - (BACK * x * x);
    case 22:
        return x < 0.5 ? (1.0 - std::sqrt(std::max(1.0 - std::pow(2.0 * x, 2.0), 0.0))) / 2.0
                       : (std::sqrt(std::max(1.0 - std::pow((-2.0 * x) + 2.0, 2.0), 0.0)) + 1.0) / 2.0;
    case 23:
        return x < 0.5 ? (std::pow(2.0 * x, 2.0) * (((BACK_IN_OUT + 1.0) * 2.0 * x) - BACK_IN_OUT)) / 2.0
                       : ((std::pow((2.0 * x) - 2.0, 2.0) * (((BACK_IN_OUT + 1.0) * ((x * 2.0) - 2.0)) + BACK_IN_OUT)) +
                          2.0) /
                             2.0;
    case 24:
        return x <= 0.0   ? 0.0
               : x >= 1.0 ? 1.0
                          : (std::pow(2.0, -10.0 * x) * std::sin(((x * 10.0) - 0.75) * ELASTIC)) + 1.0;
    case 25:
        return x <= 0.0   ? 0.0
               : x >= 1.0 ? 1.0
                          : -std::pow(2.0, (10.0 * x) - 10.0) * std::sin(((x * 10.0) - 10.75) * ELASTIC);
    case 26:
        return OutBounce(x);
    case 27:
        return 1.0 - OutBounce(1.0 - x);
    case 28:
        return x < 0.5 ? (1.0 - OutBounce(1.0 - (2.0 * x))) / 2.0 : (1.0 + OutBounce((2.0 * x) - 1.0)) / 2.0;
    case 29:
        if (x <= 0.0 || x >= 1.0)
        {
            return x <= 0.0 ? 0.0 : 1.0;
        }
        return x < 0.5
                   ? -(std::pow(2.0, (20.0 * x) - 10.0) * std::sin(((20.0 * x) - 11.125) * ELASTIC_IN_OUT)) / 2.0
                   : ((std::pow(2.0, (-20.0 * x) + 10.0) * std::sin(((20.0 * x) - 11.125) * ELASTIC_IN_OUT)) / 2.0) +
                         1.0;
    default:
        break;
    }
    return x;
}

bool IsSpringy(int type)
{
    return type >= 20;
}

// An event, its times in microseconds.
struct Event
{
    double Start = 0.0;
    double End = 0.0;
    double From = 0.0;
    double To = 0.0;
    int Easing = 1;
    // The part of the easing curve the event uses, 0 to 1.
    double Left = 0.0;
    double Right = 1.0;
};

double GetEventValue(const Event& event, double time)
{
    if (time >= event.End || event.End <= event.Start)
    {
        return event.To;
    }
    const double progress = std::clamp((time - event.Start) / (event.End - event.Start), 0.0, 1.0);
    double eased = progress;
    if (event.Easing > 1)
    {
        const double low = Ease(event.Easing, event.Left);
        const double high = Ease(event.Easing, event.Right);
        const double x = event.Left + ((event.Right - event.Left) * progress);
        eased = std::abs(high - low) > 1e-12 ? (Ease(event.Easing, x) - low) / (high - low) : progress;
    }
    return event.From + ((event.To - event.From) * eased);
}

// One layer's events of a kind, sorted by start: before the first, its start value; between, the
// last one's end value.
using Layer = std::vector<Event>;

double GetLayerValue(const Layer& events, double time)
{
    const auto after = std::upper_bound(events.begin(), events.end(), time,
                                        [](double t, const Event& event) { return t < event.Start; });
    if (after == events.begin())
    {
        return events.front().From;
    }
    return GetEventValue(*(after - 1), time);
}

// A value's events in every layer; the layers add up.
struct Property
{
    std::vector<Layer> Layers;

    bool HasEvents() const
    {
        return std::any_of(Layers.begin(), Layers.end(), [](const Layer& layer) { return !layer.empty(); });
    }

    double GetValue(double time) const
    {
        double value = 0.0;
        for (const Layer& layer : Layers)
        {
            if (!layer.empty())
            {
                value += GetLayerValue(layer, time);
            }
        }
        return value;
    }
};

std::vector<double> GetSampleTimes(const Property& property, bool isSpeed)
{
    std::vector<double> times;
    for (const Layer& layer : property.Layers)
    {
        for (const Event& event : layer)
        {
            times.push_back(event.Start);
            times.push_back(event.End);
            const bool isCurved = isSpeed ? event.From != event.To : event.Easing > 1;
            if (!isCurved)
            {
                continue;
            }
            const int most = isSpeed ? SPEED_STEPS : (IsSpringy(event.Easing) ? SPRINGY_PIECES : EASED_PIECES);
            const int pieces = std::clamp(static_cast<int>((event.End - event.Start) / MIN_PIECE_US), 1, most);
            for (int k = 1; k < pieces; ++k)
            {
                times.push_back(event.Start + ((event.End - event.Start) * k / pieces));
            }
        }
    }
    for (double& time : times)
    {
        time = std::round(time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

// Keys through the value's samples, with a second key a microsecond before each jump, in RPE's
// units (`convert` makes them ours).
template <typename Convert>
std::vector<LineKey> BakeKeys(const Property& property, double fallback, Convert convert)
{
    if (!property.HasEvents())
    {
        return {{0, static_cast<float>(convert(fallback))}};
    }
    std::vector<LineKey> keys;
    std::vector<double> starts;
    for (const Layer& layer : property.Layers)
    {
        for (const Event& event : layer)
        {
            starts.push_back(std::round(event.Start));
        }
    }
    std::sort(starts.begin(), starts.end());
    for (const double time : GetSampleTimes(property, false))
    {
        const double value = property.GetValue(time);
        if (std::binary_search(starts.begin(), starts.end(), time) && !keys.empty())
        {
            const double before = property.GetValue(time - 1.0);
            if (std::abs(before - value) > 1e-9)
            {
                keys.push_back({std::llround(time) - 1, static_cast<float>(convert(before))});
            }
        }
        keys.push_back({std::llround(time), static_cast<float>(convert(value))});
    }
    return keys;
}

// Speed as steps held from key to key, each the ramp's average over its step.
std::vector<LineKey> BakeSpeed(const Property& property)
{
    if (!property.HasEvents())
    {
        return {{0, static_cast<float>(RPE_DEFAULT_SPEED / RPE_SPEED_PER_UNIT)}};
    }
    const std::vector<double> times = GetSampleTimes(property, true);
    std::vector<LineKey> keys;
    for (size_t i = 0; i < times.size(); ++i)
    {
        const double sample = i + 1 < times.size() ? (times[i] + times[i + 1]) * 0.5 : times[i];
        keys.push_back({std::llround(times[i]), static_cast<float>(property.GetValue(sample) / RPE_SPEED_PER_UNIT)});
    }
    return keys;
}

// Beats ([bar, numerator, denominator]) to microseconds, along the BPM list.
class BeatClock
{
public:
    void Add(double beat, double bpm)
    {
        points.push_back({beat, bpm, 0.0});
    }

    void Finish()
    {
        std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.Beat < b.Beat; });
        double time = 0.0;
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (i > 0)
            {
                time += (points[i].Beat - points[i - 1].Beat) * US_PER_MINUTE / points[i - 1].Bpm;
            }
            points[i].Us = i == 0 ? points[0].Beat * US_PER_MINUTE / points[0].Bpm : time;
            time = points[i].Us;
        }
    }

    double ToUs(double beat) const
    {
        const auto after = std::upper_bound(points.begin(), points.end(), beat,
                                            [](double b, const Point& point) { return b < point.Beat; });
        const Point& point = after == points.begin() ? points.front() : *(after - 1);
        return point.Us + ((beat - point.Beat) * US_PER_MINUTE / point.Bpm);
    }

    bool IsEmpty() const
    {
        return points.empty();
    }

    struct Point
    {
        double Beat = 0.0;
        double Bpm = 120.0;
        double Us = 0.0;
    };

    const std::vector<Point>& GetPoints() const
    {
        return points;
    }

private:
    std::vector<Point> points;
};

Result<double> ReadBeat(const rhythm::ChartFields& fields, std::string_view key)
{
    Result<std::vector<double>> parts = fields.GetNumbers(key);
    if (!parts)
    {
        return parts.GetError();
    }
    const std::vector<double>& beat = parts.Value();
    if (beat.size() != 3 || beat[2] == 0.0)
    {
        return Error{"'" + std::string(key) + "' must be [bar, numerator, denominator]"};
    }
    return beat[0] + (beat[1] / beat[2]);
}

// A line as RPE has it, before parents and units.
struct RawLine
{
    Property X;
    Property Y;
    Property Rotation;
    Property Opacity;
    Property Speed;
    // Its extended events: scale along and across it, and its color's red, green, and blue.
    Property ScaleX;
    Property ScaleY;
    std::array<Property, 3> Color;
    std::optional<size_t> Parent;
    // Lines are drawn from the lowest up.
    int64_t ZOrder = 0;
    // Its texture, if not a plain line, and where on it the line's position is.
    std::string Picture;
    glm::vec2 Anchor{0.5f};
    // It shows something that isn't drawn (text, paint, the UI), or is a null line.
    bool IsHidden = false;
    bool HasNotes = false;
};

// The events of a kind, as many layers as each event has values ("start" and "end" are a number,
// or for colors, [red, green, blue]).
Result<std::vector<Layer>> ReadEvents(const rhythm::ChartFields& owner, std::string_view key, const BeatClock& clock,
                                      size_t channels)
{
    std::vector<Layer> layers(channels);
    const bool isSpeed = key == "speedEvents";
    Result<void> read = owner.ForEachPresentObject(
        key,
        [&](const rhythm::ChartFields& fields, size_t) -> Result<void>
        {
            Result<double> start = ReadBeat(fields, "startTime");
            Result<double> end = ReadBeat(fields, "endTime");
            Result<int64_t> easing = fields.GetInt("easingType", 1);
            Result<double> left = fields.GetNumber("easingLeft", 0.0);
            Result<double> right = fields.GetNumber("easingRight", 1.0);
            if (!start || !end || !easing || !left || !right)
            {
                return !start    ? start.GetError()
                       : !end    ? end.GetError()
                       : !easing ? easing.GetError()
                       : !left   ? left.GetError()
                                 : right.GetError();
            }
            std::vector<double> from;
            std::vector<double> to;
            if (channels == 1)
            {
                Result<double> one = fields.GetNumber("start");
                if (!one)
                {
                    return one.GetError();
                }
                Result<double> other = fields.GetNumber("end", one.Value());
                if (!other)
                {
                    return other.GetError();
                }
                from = {one.Value()};
                to = {other.Value()};
            }
            else
            {
                Result<std::vector<double>> many = fields.GetNumbers("start");
                if (!many)
                {
                    return many.GetError();
                }
                Result<std::vector<double>> others =
                    fields.Has("end") ? fields.GetNumbers("end") : Result<std::vector<double>>(many.Value());
                if (!others)
                {
                    return others.GetError();
                }
                if (many.Value().size() < channels || others.Value().size() < channels)
                {
                    return Error{"'start' and 'end' need " + std::to_string(channels) + " values"};
                }
                from = std::move(many).Value();
                to = std::move(others).Value();
            }
            Event event;
            event.Start = clock.ToUs(start.Value());
            event.End = std::max(clock.ToUs(end.Value()), event.Start);
            // Speed changes evenly, whatever its easing says.
            event.Easing = isSpeed ? 1 : static_cast<int>(easing.Value());
            event.Left = std::clamp(left.Value(), 0.0, 1.0);
            event.Right = std::clamp(right.Value(), event.Left, 1.0);
            for (size_t channel = 0; channel < channels; ++channel)
            {
                event.From = from[channel];
                event.To = to[channel];
                layers[channel].push_back(event);
            }
            return {};
        });
    if (!read)
    {
        return Error{std::string(key) + ": " + read.GetError().Message};
    }
    for (Layer& events : layers)
    {
        std::stable_sort(events.begin(), events.end(),
                         [](const Event& a, const Event& b) { return a.Start < b.Start; });
    }
    return layers;
}

Result<void> ReadLayers(const rhythm::ChartFields& source, const BeatClock& clock, RawLine& line)
{
    Result<void> layers = source.ForEachPresentObject(
        "eventLayers",
        [&](const rhythm::ChartFields& layer, size_t) -> Result<void>
        {
            for (const auto& [key, property] : {std::pair<std::string_view, Property*>{"moveXEvents", &line.X},
                                                {"moveYEvents", &line.Y},
                                                {"rotateEvents", &line.Rotation},
                                                {"alphaEvents", &line.Opacity},
                                                {"speedEvents", &line.Speed}})
            {
                Result<std::vector<Layer>> events = ReadEvents(layer, key, clock, 1);
                if (!events)
                {
                    return events.GetError();
                }
                property->Layers.push_back(std::move(events.Value().front()));
            }
            return {};
        });
    if (!layers)
    {
        return layers;
    }
    // Extended events come in one layer. A line that shows text or paints is a picture, too.
    Result<rhythm::ChartFields> extended = source.GetObject("extended");
    if (!extended)
    {
        return {};
    }
    for (const auto& [key, property] :
         {std::pair<std::string_view, Property*>{"scaleXEvents", &line.ScaleX}, {"scaleYEvents", &line.ScaleY}})
    {
        Result<std::vector<Layer>> events = ReadEvents(extended.Value(), key, clock, 1);
        if (!events)
        {
            return events.GetError();
        }
        property->Layers.push_back(std::move(events.Value().front()));
    }
    Result<std::vector<Layer>> colors = ReadEvents(extended.Value(), "colorEvents", clock, line.Color.size());
    if (!colors)
    {
        return colors.GetError();
    }
    for (size_t channel = 0; channel < line.Color.size(); ++channel)
    {
        line.Color[channel].Layers.push_back(std::move(colors.Value()[channel]));
    }
    for (const std::string_view key : {"textEvents", "paintEvents"})
    {
        size_t count = 0;
        Result<void> counted = extended.Value().ForEachPresentObject(key,
                                                                     [&count](const rhythm::ChartFields&, size_t)
                                                                     {
                                                                         ++count;
                                                                         return Result<void>{};
                                                                     });
        line.IsHidden = line.IsHidden || (counted && count > 0);
    }
    return {};
}

// Times where any of the key lists has a key, for following a parent.
std::vector<SongTimeUs> MergeTimes(std::initializer_list<const std::vector<LineKey>*> lists)
{
    std::vector<SongTimeUs> times;
    for (const std::vector<LineKey>* keys : lists)
    {
        for (const LineKey& key : *keys)
        {
            times.push_back(key.Time);
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

} // namespace

Result<PhigrosImportResult> ImportRpe(std::string_view json)
{
    PhigrosImportResult result;
    rhythm::Chart info;
    info.Mode = "phigros";
    info.Modes = {info.Mode};
    BeatClock clock;
    std::vector<RawLine> rawLines;
    struct Imported
    {
        rhythm::ChartNote Common;
        LineNote Line;
    };
    std::vector<Imported> notes;
    size_t fakeNotes = 0;
    size_t unknownNotes = 0;
    size_t scaledLines = 0;

    Result<void> read = rhythm::ReadJson(
        json,
        [&](const rhythm::ChartFields& root) -> Result<void>
        {
            Result<void> bpms = root.ForEachPresentObject("BPMList",
                                                          [&](const rhythm::ChartFields& point, size_t) -> Result<void>
                                                          {
                                                              Result<double> beat = ReadBeat(point, "startTime");
                                                              Result<double> bpm = point.GetNumber("bpm");
                                                              if (!beat || !bpm)
                                                              {
                                                                  return beat ? bpm.GetError() : beat.GetError();
                                                              }
                                                              if (bpm.Value() <= 0.0)
                                                              {
                                                                  return Error{"'bpm' must be positive"};
                                                              }
                                                              clock.Add(beat.Value(), bpm.Value());
                                                              return {};
                                                          });
            if (!bpms)
            {
                return bpms;
            }
            if (clock.IsEmpty())
            {
                return Error{"The chart has no BPM"};
            }
            clock.Finish();
            for (const BeatClock::Point& point : clock.GetPoints())
            {
                info.Timing.push_back({std::llround(point.Us), point.Bpm, 4, 4});
            }
            // What the chart says about itself; a package's info file says it too, and wins.
            if (Result<rhythm::ChartFields> meta = root.GetObject("META"))
            {
                const rhythm::ChartFields& fields = meta.Value();
                const auto text = [&fields](std::string_view key)
                {
                    Result<std::string> value = fields.GetString(key, "");
                    return value ? std::move(value).Value() : std::string();
                };
                Result<double> offset = fields.GetNumber("offset", 0.0);
                // Milliseconds, with the official format's sense.
                info.AudioOffsetUs = offset ? std::llround(offset.Value() * 1000.0) : 0;
                info.Metadata.Title = text("name");
                info.Metadata.Artist = text("composer");
                info.Metadata.Charter = text("charter");
                info.Metadata.DifficultyName = text("level");
                info.AudioFile = text("song");
            }
            return root.ForEachPresentObject(
                "judgeLineList",
                [&](const rhythm::ChartFields& source, size_t index) -> Result<void>
                {
                    // A null line keeps its place (notes and parents count lines), hidden.
                    while (rawLines.size() < index)
                    {
                        RawLine hidden;
                        hidden.IsHidden = true;
                        rawLines.push_back(std::move(hidden));
                    }
                    RawLine line;
                    Result<std::string> texture = source.GetString("Texture", PLAIN_LINE);
                    Result<int64_t> father = source.GetInt("father", -1);
                    Result<double> bpmFactor = source.GetNumber("bpmfactor", 1.0);
                    Result<int64_t> zOrder = source.GetInt("zOrder", 0);
                    if (!texture || !father || !bpmFactor || !zOrder)
                    {
                        return !texture  ? texture.GetError()
                               : !father ? father.GetError()
                               : !zOrder ? zOrder.GetError()
                                         : bpmFactor.GetError();
                    }
                    if (texture.Value() != PLAIN_LINE)
                    {
                        line.Picture = texture.Value();
                    }
                    line.IsHidden = source.Has("attachUI");
                    if (source.Has("anchor"))
                    {
                        Result<std::vector<double>> anchor = source.GetNumbers("anchor");
                        if (!anchor || anchor.Value().size() < 2)
                        {
                            return Error{"'anchor' must be [x, y]"};
                        }
                        line.Anchor = {static_cast<float>(anchor.Value()[0]), static_cast<float>(anchor.Value()[1])};
                    }
                    line.ZOrder = zOrder.Value();
                    if (father.Value() >= 0)
                    {
                        line.Parent = static_cast<size_t>(father.Value());
                    }
                    scaledLines += bpmFactor.Value() != 1.0 ? 1 : 0;
                    if (Result<void> layers = ReadLayers(source, clock, line); !layers)
                    {
                        return layers;
                    }
                    Result<void> lineNotes = source.ForEachPresentObject(
                        "notes",
                        [&](const rhythm::ChartFields& fields, size_t) -> Result<void>
                        {
                            Result<int64_t> type = fields.GetInt("type");
                            Result<double> start = ReadBeat(fields, "startTime");
                            Result<double> end = fields.Has("endTime") ? ReadBeat(fields, "endTime") : start;
                            Result<double> x = fields.GetNumber("positionX", 0.0);
                            Result<int64_t> above = fields.GetInt("above", NOTE_ABOVE);
                            Result<int64_t> fake = fields.GetInt("isFake", 0);
                            Result<double> speed = fields.GetNumber("speed", 1.0);
                            if (!type || !start || !end || !x || !above || !fake || !speed)
                            {
                                return !type    ? type.GetError()
                                       : !start ? start.GetError()
                                       : !end   ? end.GetError()
                                       : !x     ? x.GetError()
                                       : !above ? above.GetError()
                                       : !fake  ? fake.GetError()
                                                : speed.GetError();
                            }
                            if (fake.Value() != 0)
                            {
                                ++fakeNotes;
                                return {};
                            }
                            if (type.Value() < 1 || type.Value() > 4)
                            {
                                ++unknownNotes;
                                return {};
                            }
                            Imported note;
                            note.Line.Line = static_cast<uint32_t>(index);
                            note.Line.X = static_cast<float>(x.Value() / RPE_X_PER_UNIT);
                            note.Line.IsAbove = above.Value() == NOTE_ABOVE;
                            note.Line.Kind = RPE_KINDS[static_cast<size_t>(type.Value() - 1)];
                            note.Line.Speed = static_cast<float>(speed.Value());
                            note.Common.Time = std::llround(clock.ToUs(start.Value()));
                            note.Common.EndTime =
                                note.Line.Kind == NoteKind::Hold
                                    ? std::max<SongTimeUs>(note.Common.Time + 1, std::llround(clock.ToUs(end.Value())))
                                    : note.Common.Time;
                            notes.push_back(note);
                            line.HasNotes = true;
                            return {};
                        });
                    if (!lineNotes)
                    {
                        return lineNotes;
                    }
                    rawLines.push_back(std::move(line));
                    return {};
                });
        });
    if (!read)
    {
        return read.GetError();
    }
    if (rawLines.empty())
    {
        return Error{"The chart has no judgment lines"};
    }

    // Each line's own movement in RPE's units, then its parents' carried in: a child's position is
    // its parent's plus its own turned by its parent's rotation (clockwise in RPE), its rotation its
    // own.
    struct Placed
    {
        std::vector<LineKey> X;
        std::vector<LineKey> Y;
        std::vector<LineKey> Rotation;
        bool IsDone = false;
        bool IsVisiting = false;
    };
    const auto identity = [](double value) { return value; };
    std::vector<Placed> placed(rawLines.size());
    for (size_t i = 0; i < rawLines.size(); ++i)
    {
        placed[i].X = BakeKeys(rawLines[i].X, 0.0, identity);
        placed[i].Y = BakeKeys(rawLines[i].Y, 0.0, identity);
        placed[i].Rotation = BakeKeys(rawLines[i].Rotation, 0.0, identity);
    }
    size_t brokenParents = 0;
    const std::function<void(size_t)> place = [&](size_t i)
    {
        Placed& line = placed[i];
        if (line.IsDone || line.IsVisiting)
        {
            return;
        }
        const std::optional<size_t> parent = rawLines[i].Parent;
        if (parent && (*parent >= rawLines.size() || *parent == i))
        {
            ++brokenParents;
        }
        if (!parent || *parent >= rawLines.size() || *parent == i)
        {
            line.IsDone = true;
            return;
        }
        line.IsVisiting = true;
        place(*parent);
        line.IsVisiting = false;
        const Placed& above = placed[*parent];
        std::vector<LineKey> x;
        std::vector<LineKey> y;
        for (const SongTimeUs time : MergeTimes({&line.X, &line.Y, &above.X, &above.Y, &above.Rotation}))
        {
            const double angle = -GetKeyValue(above.Rotation, time, 0.0f) * std::numbers::pi / 180.0;
            const double ownX = GetKeyValue(line.X, time, 0.0f);
            const double ownY = GetKeyValue(line.Y, time, 0.0f);
            x.push_back({time, static_cast<float>(GetKeyValue(above.X, time, 0.0f) + (ownX * std::cos(angle)) -
                                                  (ownY * std::sin(angle)))});
            y.push_back({time, static_cast<float>(GetKeyValue(above.Y, time, 0.0f) + (ownX * std::sin(angle)) +
                                                  (ownY * std::cos(angle)))});
        }
        line.X = std::move(x);
        line.Y = std::move(y);
        line.IsDone = true;
    };

    std::vector<JudgeLine> lines;
    size_t hiddenLines = 0;
    for (size_t i = 0; i < rawLines.size(); ++i)
    {
        place(i);
        JudgeLine line;
        for (const LineKey& key : placed[i].X)
        {
            line.X.push_back({key.Time, static_cast<float>(0.5 + (key.Value / RPE_WIDTH))});
        }
        for (const LineKey& key : placed[i].Y)
        {
            line.Y.push_back({key.Time, static_cast<float>(0.5 + (key.Value / RPE_HEIGHT))});
        }
        // RPE turns clockwise, the official format counterclockwise.
        for (const LineKey& key : placed[i].Rotation)
        {
            line.Rotation.push_back({key.Time, -key.Value});
        }
        // Text, paint, and the UI can't be drawn yet, so those lines hide unless they have notes.
        if (rawLines[i].IsHidden && !rawLines[i].HasNotes)
        {
            line.Opacity = {{0, 0.0f}};
            ++hiddenLines;
        }
        else
        {
            line.Opacity = BakeKeys(rawLines[i].Opacity, RPE_OPAQUE, [](double value) { return value / RPE_OPAQUE; });
        }
        line.Speed = BakeSpeed(rawLines[i].Speed);
        if (!rawLines[i].Picture.empty())
        {
            // A pixel of the picture is a canvas unit, times the line's scale.
            line.Picture = rawLines[i].Picture;
            line.Anchor = rawLines[i].Anchor;
            line.Length = BakeKeys(rawLines[i].ScaleX, 1.0,
                                   [](double value) { return value * static_cast<double>(FIELD_WIDTH) / RPE_WIDTH; });
            line.Thickness = BakeKeys(rawLines[i].ScaleY, 1.0, [](double value)
                                      { return value * static_cast<double>(FIELD_HEIGHT) / RPE_HEIGHT; });
        }
        else
        {
            line.Length =
                BakeKeys(rawLines[i].ScaleX, 1.0, [](double value) { return value * RPE_LINE_LENGTH / LINE_LENGTH; });
            line.Thickness = BakeKeys(rawLines[i].ScaleY, 1.0,
                                      [](double value)
                                      {
                                          return std::abs(value - 1.0) <= RPE_UNSCALED
                                                     ? 1.0
                                                     : value * RPE_SCALED_THICKNESS / LINE_THICKNESS;
                                      });
        }
        if (std::any_of(rawLines[i].Color.begin(), rawLines[i].Color.end(),
                        [](const Property& channel) { return channel.HasEvents(); }))
        {
            const auto unit = [](double value) { return value / RPE_OPAQUE; };
            line.Red = BakeKeys(rawLines[i].Color[0], RPE_OPAQUE, unit);
            line.Green = BakeKeys(rawLines[i].Color[1], RPE_OPAQUE, unit);
            line.Blue = BakeKeys(rawLines[i].Color[2], RPE_OPAQUE, unit);
        }
        lines.push_back(std::move(line));
    }

    // Drawn from the lowest zOrder up, as lines are drawn in order: the notes follow their lines.
    std::vector<size_t> order(lines.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&rawLines](size_t a, size_t b) { return rawLines[a].ZOrder < rawLines[b].ZOrder; });
    std::vector<JudgeLine> ordered;
    std::vector<uint32_t> moved(lines.size());
    for (const size_t i : order)
    {
        moved[i] = static_cast<uint32_t>(ordered.size());
        ordered.push_back(std::move(lines[i]));
    }
    lines = std::move(ordered);
    for (Imported& note : notes)
    {
        note.Line.Line = moved[note.Line.Line];
    }

    if (hiddenLines > 0)
    {
        result.Warnings.push_back(std::to_string(hiddenLines) +
                                  " lines showing text, paint, or the UI are hidden: those aren't drawn yet");
    }
    if (fakeNotes > 0)
    {
        result.Warnings.push_back(std::to_string(fakeNotes) + " fake notes were left out");
    }
    if (unknownNotes > 0)
    {
        result.Warnings.push_back(std::to_string(unknownNotes) + " notes of unknown types were skipped");
    }
    if (scaledLines > 0)
    {
        result.Warnings.push_back(std::to_string(scaledLines) + " lines' bpmfactor isn't supported and was ignored");
    }
    if (brokenParents > 0)
    {
        result.Warnings.push_back(std::to_string(brokenParents) + " lines name a parent that doesn't exist");
    }

    std::stable_sort(notes.begin(), notes.end(),
                     [](const Imported& a, const Imported& b) { return a.Common.Time < b.Common.Time; });
    std::vector<LineNote> lineNotes;
    for (const Imported& note : notes)
    {
        info.Notes.push_back(note.Common);
        lineNotes.push_back(note.Line);
    }
    Result<LineChart> chart = BuildLineChart(std::move(info), std::move(lines), std::move(lineNotes));
    if (!chart)
    {
        return chart.GetError();
    }
    result.Chart = std::move(chart).Value();
    return result;
}

} // namespace hyoshi::line
