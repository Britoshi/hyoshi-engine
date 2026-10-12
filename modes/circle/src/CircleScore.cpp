#include "circle/CircleScore.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace hyoshi::circle
{

using rhythm::Judgment;

namespace
{

constexpr double COMBO_SCORE = 700'000.0;
constexpr double ACCURACY_SCORE = 300'000.0;
constexpr int ACCURACY_EXPONENT = 10;

const SliderPart* FindPart(const CircleChart* chart, const rhythm::JudgmentEvent& event)
{
    if (chart == nullptr || event.NoteIndex >= chart->Notes.size())
    {
        return nullptr;
    }
    return GetSliderPart(*chart, event.NoteIndex, event.Part);
}

} // namespace

void CircleScore::Reset(const CircleChart& circleChart, const CircleHeadValues& values)
{
    *this = CircleScore{};
    chart = &circleChart;
    headValues = values;

    // A perfect run: every judgment at its best, in the order they come.
    struct Item
    {
        SongTimeUs Time = 0;
        uint32_t Note = 0;
        uint32_t Part = 0;
        int64_t Value = 0;
    };
    std::vector<Item> items;
    for (uint32_t i = 0; i < chart->Notes.size(); ++i)
    {
        const CircleNote& note = chart->Notes[i];
        if (note.Type == NoteType::Circle)
        {
            items.push_back({note.Time, i, 0, GetBaseValue(Judgment::Great)});
        }
        for (uint32_t part = 1; part <= note.PartCount; ++part)
        {
            const SliderPart& sliderPart = *GetSliderPart(*chart, i, part);
            items.push_back({sliderPart.JudgeTime, i, part, GetPartValue(sliderPart.Type)});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b)
              { return std::tie(a.Time, a.Note, a.Part) < std::tie(b.Time, b.Note, b.Part); });
    for (size_t i = 0; i < items.size(); ++i)
    {
        maxComboPortion += static_cast<double>(items[i].Value) * std::sqrt(static_cast<double>(i + 1));
    }
    maxJudgments = static_cast<uint32_t>(items.size());
}

void CircleScore::Add(const rhythm::JudgmentEvent& event)
{
    const SliderPart* part = FindPart(chart, event);
    const bool isSlider = chart != nullptr && event.NoteIndex < chart->Notes.size() &&
                          chart->Notes[event.NoteIndex].Type == NoteType::Slider;
    // Circles' events are part 0, sliders' are their parts.
    if (isSlider ? part == nullptr : event.Part != 0)
    {
        return;
    }
    const bool isHit = event.Result != Judgment::Miss;
    const SliderPartType type = part != nullptr ? part->Type : SliderPartType::Head;
    int64_t value = 0;
    if (type == SliderPartType::Head)
    {
        // A circle, or a head: graded.
        ++counts[static_cast<size_t>(event.Result)];
        ++judged;
        value = GetBaseValue(event.Result);
        possible += GetBaseValue(Judgment::Great);
    }
    else
    {
        value = isHit ? GetPartValue(type) : 0;
        possible += GetPartValue(type);
    }
    earned += value;
    ++judgments;

    if (isHit)
    {
        AddCombo();
    }
    else if (type != SliderPartType::Tail)
    {
        combo = 0;
    }
    comboPortion += static_cast<double>(value) * std::sqrt(static_cast<double>(combo));
}

void CircleScore::AddCombo()
{
    ++combo;
    maxCombo = std::max(maxCombo, combo);
}

int64_t CircleScore::GetScore() const
{
    if (maxComboPortion <= 0.0 || maxJudgments == 0)
    {
        return 0;
    }
    const double accuracy = possible > 0 ? static_cast<double>(earned) / static_cast<double>(possible) : 1.0;
    double accuracyPower = 1.0;
    for (int i = 0; i < ACCURACY_EXPONENT; ++i)
    {
        accuracyPower *= accuracy;
    }
    const double progress = static_cast<double>(judgments) / static_cast<double>(maxJudgments);
    return std::llround((COMBO_SCORE * comboPortion / maxComboPortion) + (ACCURACY_SCORE * accuracyPower * progress));
}

int64_t CircleScore::GetAccuracyBasisPoints() const
{
    if (possible == 0)
    {
        return 10'000;
    }
    return earned * 10'000 / possible;
}

const char* CircleScore::GetRank() const
{
    if (judgments == 0)
    {
        return "-";
    }
    if (earned == possible)
    {
        return "SS";
    }
    const int64_t accuracy = GetAccuracyBasisPoints();
    if (accuracy >= 9'500 && GetCount(Judgment::Miss) == 0)
    {
        return "S";
    }
    if (accuracy >= 9'000)
    {
        return "A";
    }
    if (accuracy >= 8'000)
    {
        return "B";
    }
    return accuracy >= 7'000 ? "C" : "D";
}

int64_t CircleScore::GetBaseValue(Judgment judgment) const
{
    switch (judgment)
    {
    case Judgment::Perfect:
    case Judgment::Great:
        return headValues.Great;
    case Judgment::Good:
        return headValues.Good;
    case Judgment::Bad:
        return headValues.Bad;
    case Judgment::Miss:
    case Judgment::Count:
        break;
    }
    return 0;
}

int64_t CircleScore::GetPartValue(SliderPartType type)
{
    switch (type)
    {
    case SliderPartType::Head:
        return 300;
    case SliderPartType::Tick:
    case SliderPartType::Repeat:
        break;
    case SliderPartType::Tail:
        return 150;
    }
    return 30;
}

} // namespace hyoshi::circle
