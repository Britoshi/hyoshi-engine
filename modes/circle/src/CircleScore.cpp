#include "circle/CircleScore.h"

#include <algorithm>

namespace hyoshi::circle
{

using rhythm::Judgment;

void CircleScore::Reset()
{
    *this = CircleScore{};
}

void CircleScore::Add(const rhythm::JudgmentEvent& event)
{
    ++counts[static_cast<size_t>(event.Result)];
    ++judged;
    const int64_t base = GetBaseValue(event.Result);
    earned += base;
    if (event.Result == Judgment::Miss)
    {
        combo = 0;
        return;
    }
    // base * (1 + combo / 25), exact in integers for 300, 100, and 50.
    score += base * (25 + static_cast<int64_t>(combo)) / 25;
    ++combo;
    maxCombo = std::max(maxCombo, combo);
}

int64_t CircleScore::GetAccuracyBasisPoints() const
{
    if (judged == 0)
    {
        return 10'000;
    }
    return earned * 10'000 / (static_cast<int64_t>(judged) * 300);
}

const char* CircleScore::GetRank() const
{
    if (judged == 0)
    {
        return "-";
    }
    const int64_t accuracy = GetAccuracyBasisPoints();
    if (accuracy >= 9'999)
    {
        return "SS";
    }
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

int64_t CircleScore::GetBaseValue(Judgment judgment)
{
    switch (judgment)
    {
    case Judgment::Perfect:
    case Judgment::Great:
        return 300;
    case Judgment::Good:
        return 100;
    case Judgment::Bad:
        return 50;
    case Judgment::Miss:
    case Judgment::Count:
        break;
    }
    return 0;
}

} // namespace hyoshi::circle
