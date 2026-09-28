#pragma once

#include "input/InputEvent.h"

#include <cstdint>
#include <vector>

namespace hyoshi::input
{

// Collects the frame's input events (DESIGN.md section 12.2). The platform pushes while pumping
// OS events; the game takes them once per frame, ordered by host time.
class InputQueue
{
public:
    // Past this many untaken events, the oldest are dropped, so a paused consumer can't grow the
    // queue without bound.
    static constexpr size_t CAPACITY = 4096;

    void Push(const InputEvent& event);

    // Replaces `out` with the queued events sorted by host time (stable for equal times), and
    // empties the queue.
    void Take(std::vector<InputEvent>& out);

    uint64_t GetDroppedCount() const
    {
        return droppedCount;
    }

private:
    std::vector<InputEvent> events;
    uint64_t droppedCount = 0;
};

} // namespace hyoshi::input
