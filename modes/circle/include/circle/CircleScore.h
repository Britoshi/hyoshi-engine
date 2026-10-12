#pragma once

#include "circle/CircleChart.h"

#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>

namespace hyoshi::circle
{

// Score, combo, and accuracy as osu!lazer counts them (its standardised score), from every
// judgment: circles, and each slider's head, ticks, repeats, and tail.
//
// - Each judgment has a base value: a circle or a slider's head 300, 100, or 50 (Great, Good,
//   Bad; a game can set others with CircleHeadValues), a tick or a repeat 30, a tail 150, and a
//   miss 0.
// - Accuracy is the base values earned over the most there were to earn, for the judgments so
//   far, so it reads right mid-chart. Ticks and tails count.
// - Each hit adds one to the combo. A miss breaks it, except a missed tail, which only doesn't add.
// - The score, out of 1,000,000: 700,000 times the combo portion (each judgment's base value times
//   the square root of the combo after it, over the same for a perfect run), plus 300,000 times
//   accuracy to the 10th power times the fraction of the chart's judgments made so far.
//
// The counts of Greats, Goods, Bads, and misses are the circles' and slider heads'. The arithmetic
// is in doubles in a fixed order with only exactly rounded operations, so replays reproduce scores.
// What a circle's or a slider head's results are worth, a Great the most.
struct CircleHeadValues
{
    int64_t Great = 300;
    int64_t Good = 100;
    int64_t Bad = 50;
};

class CircleScore
{
public:
    // The chart the events come from; it must outlive the score's use.
    void Reset(const CircleChart& chart, const CircleHeadValues& values = {});
    void Add(const rhythm::JudgmentEvent& event);

    int64_t GetScore() const;
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
    // Circles and slider heads judged.
    uint32_t GetJudgedCount() const
    {
        return judged;
    }
    // Hundredths of a percent (10000 = 100%); 100% before the first judgment.
    int64_t GetAccuracyBasisPoints() const;
    // SS at 100%, S at 95% without a miss, then A, B, C at 90, 80, 70%, else D; a dash before the
    // first judgment.
    const char* GetRank() const;

    // A circle's or a head's base value for its result: the head values', or 0 for a miss.
    int64_t GetBaseValue(rhythm::Judgment judgment) const;
    // A slider part's base value when it's hit: 300 for the head (the most), 30 for a tick or a
    // repeat, 150 for the tail.
    static int64_t GetPartValue(SliderPartType type);

private:
    void AddCombo();

    const CircleChart* chart = nullptr;
    CircleHeadValues headValues;
    uint32_t combo = 0;
    uint32_t maxCombo = 0;
    uint32_t judged = 0;
    std::array<uint32_t, rhythm::JUDGMENT_COUNT> counts{};
    // Base values earned, and the most there were to earn, so far.
    int64_t earned = 0;
    int64_t possible = 0;
    uint32_t judgments = 0;
    double comboPortion = 0.0;
    // A perfect run of the chart's.
    double maxComboPortion = 0.0;
    uint32_t maxJudgments = 0;
};

} // namespace hyoshi::circle
