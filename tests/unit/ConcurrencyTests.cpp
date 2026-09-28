#include "core/JobSystem.h"
#include "core/SeqLock.h"
#include "core/SpscQueue.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

TEST_CASE("SpscQueue keeps order and reports full and empty")
{
    hyoshi::SpscQueue<int> queue(3);
    CHECK(queue.GetCapacity() == 4);
    CHECK_FALSE(queue.Pop().has_value());

    for (int i = 0; i < 4; ++i)
    {
        CHECK(queue.Push(i));
    }
    CHECK_FALSE(queue.Push(99));

    for (int i = 0; i < 4; ++i)
    {
        CHECK(queue.Pop() == i);
    }
    CHECK_FALSE(queue.Pop().has_value());
}

TEST_CASE("SpscQueue delivers every value across threads")
{
    constexpr int COUNT = 200'000;
    hyoshi::SpscQueue<int> queue(64);

    std::thread producer(
        [&queue]
        {
            for (int i = 0; i < COUNT; ++i)
            {
                while (!queue.Push(i))
                {
                    std::this_thread::yield();
                }
            }
        });

    int expected = 0;
    bool isInOrder = true;
    while (expected < COUNT)
    {
        if (const std::optional<int> value = queue.Pop())
        {
            isInOrder = isInOrder && *value == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(isInOrder);
}

namespace
{

struct Pair
{
    uint64_t A = 0;
    uint64_t B = 0;
};

} // namespace

TEST_CASE("SeqLock readers never see a torn value")
{
    hyoshi::SeqLock<Pair> lock;
    CHECK(lock.Load().A == 0);

    std::atomic<bool> isDone{false};
    std::thread writer(
        [&]
        {
            for (uint64_t i = 1; i <= 200'000; ++i)
            {
                lock.Store({i, i * 3});
            }
            isDone = true;
        });

    bool isConsistent = true;
    uint64_t last = 0;
    bool isMonotonic = true;
    while (!isDone)
    {
        const Pair value = lock.Load();
        isConsistent = isConsistent && value.B == value.A * 3;
        isMonotonic = isMonotonic && value.A >= last;
        last = value.A;
    }
    writer.join();

    CHECK(isConsistent);
    CHECK(isMonotonic);
    CHECK(lock.Load().A == 200'000);
}

TEST_CASE("JobSystem runs jobs and returns results through futures")
{
    hyoshi::JobSystem jobs(2);
    CHECK(jobs.GetWorkerCount() == 2);

    std::vector<std::future<int>> results;
    for (int i = 0; i < 16; ++i)
    {
        results.push_back(jobs.Submit([i] { return i * i; }));
    }
    for (int i = 0; i < 16; ++i)
    {
        CHECK(results[static_cast<size_t>(i)].get() == i * i);
    }

    std::future<void> done = jobs.Submit([] {});
    done.wait();
    CHECK(hyoshi::IsReady(done));
}
