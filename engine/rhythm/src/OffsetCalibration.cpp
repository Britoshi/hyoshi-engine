#include "rhythm/OffsetCalibration.h"

#include <algorithm>
#include <vector>

namespace hyoshi::rhythm
{

void OffsetCalibration::Add(int64_t errorUs, int64_t offsetUs)
{
    if (hits.size() == MAX_HITS)
    {
        hits.pop_front();
    }
    hits.push_back(errorUs + offsetUs);
}

// The median, so a few wild presses don't move it.
std::optional<int64_t> OffsetCalibration::SuggestOffset() const
{
    if (hits.size() < MIN_HITS)
    {
        return std::nullopt;
    }

    std::vector<int64_t> sorted(hits.begin(), hits.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    if (sorted.size() % 2 == 1)
    {
        return sorted[middle];
    }
    return sorted[middle - 1] + ((sorted[middle] - sorted[middle - 1]) / 2);
}

} // namespace hyoshi::rhythm
