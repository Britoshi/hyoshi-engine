#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace hyoshi
{

// A fixed pool of worker threads for asset loading, decoding, and parsing (DESIGN.md section 8).
// Jobs never touch gameplay state; results come back through the returned future, which the main
// thread polls.
class JobSystem
{
public:
    // Zero picks hardware_concurrency - 2, at least 1.
    explicit JobSystem(uint32_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    template <typename Fn>
    auto Submit(Fn&& job) -> std::future<std::invoke_result_t<std::decay_t<Fn>>>
    {
        using ResultType = std::invoke_result_t<std::decay_t<Fn>>;
        auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Fn>(job));
        std::future<ResultType> future = task->get_future();
        Enqueue([task] { (*task)(); });
        return future;
    }

    uint32_t GetWorkerCount() const
    {
        return static_cast<uint32_t>(workers.size());
    }

private:
    void Enqueue(std::function<void()> job);
    void WorkerLoop();

    std::mutex mutex;
    std::condition_variable wakeUp;
    std::deque<std::function<void()>> jobs;
    bool isStopping = false;
    std::vector<std::thread> workers;
};

// Whether a future's result is ready, without blocking.
template <typename T>
bool IsReady(const std::future<T>& future)
{
    return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

} // namespace hyoshi
