#pragma once

#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>

namespace hyoshi::rhythm
{

// Scoring is per game, not built into the engine (DESIGN.md section 14.4).
struct ScoreRules
{
    // Points per judgment, in Judgment order.
    std::array<int64_t, JUDGMENT_COUNT> Points{300, 200, 100, 50, 0};
    std::array<bool, JUDGMENT_COUNT> BreaksCombo{false, false, false, true, true};
    int64_t MaxScore = 1'000'000;
};

// Turns judgment events into score, combo, accuracy, and a histogram. Integer only, so replays
// reproduce scores exactly.
class ScoreSystem
{
public:
    // judgmentCount: how many events the whole chart produces, which a full perfect run needs.
    void Reset(uint32_t judgmentCount, const ScoreRules& rules = {});
    void Add(const JudgmentEvent& event);

    // Scaled so a perfect run of the whole chart reaches MaxScore.
    int64_t GetScore() const;
    // Points earned over points possible so far, in hundredths of a percent (10000 = 100%).
    int64_t GetAccuracyBasisPoints() const;

    uint32_t GetCombo() const
    {
        return combo;
    }

    uint32_t GetMaxCombo() const
    {
        return maxCombo;
    }

    uint32_t GetCount(Judgment judgment) const
    {
        return counts[static_cast<size_t>(judgment)];
    }

    uint32_t GetJudgedCount() const
    {
        return judged;
    }

    // Mean timing error of hits (not misses), for offset hints. 0 without hits.
    int64_t GetMeanErrorUs() const;

private:
    ScoreRules rules;
    uint32_t totalJudgments = 0;
    uint32_t judged = 0;
    int64_t points = 0;
    uint32_t combo = 0;
    uint32_t maxCombo = 0;
    std::array<uint32_t, JUDGMENT_COUNT> counts{};
    int64_t errorSumUs = 0;
    uint32_t errorCount = 0;
};

} // namespace hyoshi::rhythm
