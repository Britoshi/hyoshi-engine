#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace hyoshi::rhythm
{

// Suggests an audio offset from the player's recent hits (DESIGN.md section 11.5): the offset
// that would have put the median timing error at zero. Each error is kept with the offset it was
// made under, so a change of offset mid-play doesn't skew the suggestion.
class OffsetCalibration
{
public:
    static constexpr size_t MAX_HITS = 100;
    static constexpr size_t MIN_HITS = 20;

    // A hit's timing error (positive is late) and the total user audio offset at the time.
    void Add(int64_t errorUs, int64_t offsetUs);

    // Forgets every hit, e.g. once a suggestion has been applied: the hits were measured under
    // the old offset.
    void Clear()
    {
        hits.clear();
    }

    size_t GetCount() const
    {
        return hits.size();
    }

    // The total user audio offset that centers the recent hits, once there are MIN_HITS of them.
    std::optional<int64_t> SuggestOffset() const;

private:
    // Error plus offset: what each error would have been with no offset.
    std::deque<int64_t> hits;
};

} // namespace hyoshi::rhythm
