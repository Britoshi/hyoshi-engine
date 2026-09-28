#pragma once

#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>

namespace hyoshi::circle
{

// Score, combo, and accuracy the osu! way. Integer only, so replays reproduce scores exactly.
//
// - Great, Good, and Bad are worth 300, 100, and 50, times 1 + combo / 25, where the combo is
//   counted before this judgment (the first note is worth its base value). A miss breaks the combo.
// - Accuracy is weighted by those base values over the notes judged so far, so it reads right
//   mid-chart.
class CircleScore
{
public:
    void Reset();
    void Add(const rhythm::JudgmentEvent& event);

    int64_t GetScore() const
    {
        return score;
    }
    uint32_t GetCombo() const
    {
        return combo;
    }
    uint32_t GetMaxCombo() const
    {
        return maxCombo;
    }
    uint32_t GetCount(rhythm::Judgment judgment) const
    {
        return counts[static_cast<size_t>(judgment)];
    }
    uint32_t GetJudgedCount() const
    {
        return judged;
    }
    // Hundredths of a percent (10000 = 100%); 100% before the first judgment.
    int64_t GetAccuracyBasisPoints() const;
    // SS, S, A, B, C, or D; a dash before the first judgment.
    const char* GetRank() const;

    // Base points for a judgment: 300, 100, 50, or 0.
    static int64_t GetBaseValue(rhythm::Judgment judgment);

private:
    int64_t score = 0;
    uint32_t combo = 0;
    uint32_t maxCombo = 0;
    uint32_t judged = 0;
    int64_t earned = 0;
    std::array<uint32_t, rhythm::JUDGMENT_COUNT> counts{};
};

} // namespace hyoshi::circle
