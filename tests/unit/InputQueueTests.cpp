#include "input/InputQueue.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using hyoshi::input::InputEvent;
using hyoshi::input::InputEventType;
using hyoshi::input::InputQueue;

namespace
{

InputEvent MakeKey(InputEventType type, hyoshi::HostTimeNs time, uint32_t code)
{
    InputEvent event;
    event.Type = type;
    event.HostTime = time;
    event.Code = code;
    return event;
}

} // namespace

TEST_CASE("InputQueue hands events over in host time order")
{
    InputQueue queue;
    // Touch and keyboard events can arrive out of timestamp order.
    queue.Push(MakeKey(InputEventType::KeyDown, 300, 1));
    queue.Push(MakeKey(InputEventType::KeyDown, 100, 2));
    queue.Push(MakeKey(InputEventType::KeyUp, 300, 3));
    queue.Push(MakeKey(InputEventType::KeyDown, 200, 4));

    std::vector<InputEvent> events;
    queue.Take(events);
    REQUIRE(events.size() == 4);
    CHECK(events[0].Code == 2);
    CHECK(events[1].Code == 4);
    // Equal timestamps keep their arrival order: a press and release in one poll stay in order.
    CHECK(events[2].Code == 1);
    CHECK(events[3].Code == 3);

    queue.Take(events);
    CHECK(events.empty());
}

TEST_CASE("InputQueue drops the oldest events when full")
{
    InputQueue queue;
    for (uint32_t i = 0; i < InputQueue::CAPACITY + 3; ++i)
    {
        queue.Push(MakeKey(InputEventType::KeyDown, i, i));
    }
    CHECK(queue.GetDroppedCount() == 3);

    std::vector<InputEvent> events;
    queue.Take(events);
    REQUIRE(events.size() == InputQueue::CAPACITY);
    CHECK(events.front().Code == 3);
    CHECK(events.back().Code == InputQueue::CAPACITY + 2);
}
