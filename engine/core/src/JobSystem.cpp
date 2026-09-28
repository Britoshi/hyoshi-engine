#include "core/JobSystem.h"

#include <algorithm>

namespace hyoshi
{

JobSystem::JobSystem(uint32_t workerCount)
{
    if (workerCount == 0)
    {
        const uint32_t hardwareThreads = std::thread::hardware_concurrency();
        workerCount = hardwareThreads > 2 ? hardwareThreads - 2 : 1;
    }

    workers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i)
    {
        workers.emplace_back([this] { WorkerLoop(); });
    }
}

JobSystem::~JobSystem()
{
    {
        const std::scoped_lock lock(mutex);
        isStopping = true;
    }
    wakeUp.notify_all();
    for (std::thread& worker : workers)
    {
        worker.join();
    }
}

void JobSystem::Enqueue(std::function<void()> job)
{
    {
        const std::scoped_lock lock(mutex);
        jobs.push_back(std::move(job));
    }
    wakeUp.notify_one();
}

// Finishes every queued job before exiting, so no future is left without a result.
void JobSystem::WorkerLoop()
{
    while (true)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex);
            wakeUp.wait(lock, [this] { return isStopping || !jobs.empty(); });
            if (jobs.empty())
            {
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
    }
}

} // namespace hyoshi
