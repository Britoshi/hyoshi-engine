#include "input/InputQueue.h"

#include <algorithm>

namespace hyoshi::input
{

void InputQueue::Push(const InputEvent& event)
{
    if (events.size() >= CAPACITY)
    {
        events.erase(events.begin());
        ++droppedCount;
    }
    events.push_back(event);
}

void InputQueue::Take(std::vector<InputEvent>& out)
{
    out.clear();
    out.swap(events);
    std::stable_sort(out.begin(), out.end(),
                     [](const InputEvent& a, const InputEvent& b) { return a.HostTime < b.HostTime; });
}

} // namespace hyoshi::input
