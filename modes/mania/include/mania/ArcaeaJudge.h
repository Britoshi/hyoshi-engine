#pragma once

#include "mania/ManiaChart.h"
#include "mania/ManiaJudge.h"

#include "rhythm/Judgment.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace hyoshi::mania
{

// Arcaea's windows: Pure within 50 ms (a max Pure within 25), Far within 100.
struct ArcaeaWindows
{
    int64_t MaxPureUs = 25'000;
    int64_t PureUs = 50'000;
    int64_t FarUs = 100'000;
};

// Arcaea's rules for its floor lanes (DESIGN.md section 16.6), for charts' "arcaea" notes, which
// are lane notes like Mania's. Deterministic like ManiaJudge, and driven by the same actions.
//
// - A press strikes the earliest tap in its lane within Far: Perfect (a max Pure) within 25 ms,
//   Great (Pure) within 50, Good (Far) within 100. A tap not struck by then is missed (Lost).
// - A hold isn't struck or released: it's a combo for every half beat of it (every beat from
//   255 BPM), each Perfect (a max Pure, as Arcaea counts them) while its lane is held at that
//   moment and missed if not. It can be pressed (or touched: ManiaAction::Touch) any time, and
//   letting go only loses the combos until it's held again. Its first combo also takes a press
//   within Far either side of its start.
class ArcaeaJudge : public ILaneJudge
{
public:
    void Reset(const ManiaChart& chart, const ArcaeaWindows& windows = {});

    void Advance(SongTimeUs time, std::vector<rhythm::JudgmentEvent>& events) override;
    void ProcessAction(const rhythm::GameplayAction& action, std::vector<rhythm::JudgmentEvent>& events) override;
    std::optional<SongTimeUs> GetPressTarget(uint32_t lane, SongTimeUs time) const override;

    SongTimeUs GetTime() const override
    {
        return time;
    }
    bool IsFinished() const override
    {
        return unfinishedNotes == 0;
    }
    // A hold under way is Held while its lane is held.
    NoteState GetNoteState(uint32_t note) const override;
    bool IsLaneHeld(uint32_t lane) const override
    {
        return lane < lanes.size() && lanes[lane].IsHeld;
    }
    uint32_t GetFirstUnfinishedNote() const override
    {
        return firstUnfinished;
    }

    // Judgment events a full run produces: one per tap, one per combo of each hold.
    static uint32_t GetJudgmentCount(const ManiaChart& chart);

private:
    struct Lane
    {
        bool IsHeld = false;
        SongTimeUs LastPress = INT64_MIN;
    };
    // A hold's combo, judged at CheckTime.
    struct Tick
    {
        SongTimeUs Time = 0;
        SongTimeUs CheckTime = 0;
        uint32_t Note = 0;
        bool IsFirst = false;
        // Not a combo: the hold's end, which finishes it. Its last combo comes up to a combo's
        // length before, and the hold is drawn until it ends.
        bool IsEnd = false;
    };

    std::optional<uint32_t> FindTap(uint32_t lane, SongTimeUs at) const;
    void Finish(uint32_t note, NoteState state);

    const ManiaChart* chart = nullptr;
    ArcaeaWindows windows;
    std::vector<Lane> lanes;
    std::vector<NoteState> states;
    std::vector<bool> finished;
    // Holds: whether any combo was held.
    std::vector<bool> holdHit;
    std::vector<Tick> ticks;
    size_t nextTick = 0;
    SongTimeUs time = INT64_MIN;
    uint32_t unfinishedNotes = 0;
    uint32_t firstUnfinished = 0;
};

} // namespace hyoshi::mania
