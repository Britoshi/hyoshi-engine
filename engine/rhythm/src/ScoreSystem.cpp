#include "rhythm/ScoreSystem.h"

#include <algorithm>

namespace hyoshi::rhythm
{

void ScoreSystem::Reset(uint32_t judgmentCount, const ScoreRules& scoreRules)
{
    *this = ScoreSystem{};
    rules = scoreRules;
    totalJudgments = judgmentCount;
}

void ScoreSystem::Add(const JudgmentEvent& event)
{
    const auto index = static_cast<size_t>(event.Result);
    ++counts[index];
    ++judged;
    points += rules.Points[index];

    if (rules.BreaksCombo[index])
    {
        combo = 0;
    }
    else
    {
        ++combo;
        maxCombo = std::max(maxCombo, combo);
    }

    if (event.Result != Judgment::Miss)
    {
        errorSumUs += event.ErrorUs;
        ++errorCount;
    }
}

int64_t ScoreSystem::GetScore() const
{
    const int64_t best = *std::max_element(rules.Points.begin(), rules.Points.end());
    if (totalJudgments == 0 || best <= 0)
    {
        return 0;
    }
    return rules.MaxScore * points / (best * totalJudgments);
}

int64_t ScoreSystem::GetAccuracyBasisPoints() const
{
    const int64_t best = *std::max_element(rules.Points.begin(), rules.Points.end());
    if (judged == 0 || best <= 0)
    {
        return 10'000;
    }
    return 10'000 * points / (best * judged);
}

int64_t ScoreSystem::GetMeanErrorUs() const
{
    return errorCount == 0 ? 0 : errorSumUs / errorCount;
}

} // namespace hyoshi::rhythm
