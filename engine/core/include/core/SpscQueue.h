#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>

namespace hyoshi
{

// Lock-free single-producer, single-consumer ring buffer (DESIGN.md section 8). Push and Pop never
// allocate or block, so either end may be the audio thread. Capacity is fixed at construction and
// rounded up to a power of two.
template <typename T>
class SpscQueue
{
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue elements are copied without constructors");

public:
    explicit SpscQueue(size_t capacity)
        : mask(std::bit_ceil(capacity < 2 ? size_t{2} : capacity) - 1), slots(std::make_unique<T[]>(mask + 1))
    {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer only. Returns false, dropping the value, when the queue is full.
    bool Push(const T& value)
    {
        const size_t tail = tailIndex.load(std::memory_order_relaxed);
        if (tail - cachedHead > mask)
        {
            cachedHead = headIndex.load(std::memory_order_acquire);
            if (tail - cachedHead > mask)
            {
                return false;
            }
        }
        slots[tail & mask] = value;
        tailIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    std::optional<T> Pop()
    {
        const size_t head = headIndex.load(std::memory_order_relaxed);
        if (head == cachedTail)
        {
            cachedTail = tailIndex.load(std::memory_order_acquire);
            if (head == cachedTail)
            {
                return std::nullopt;
            }
        }
        T value = slots[head & mask];
        headIndex.store(head + 1, std::memory_order_release);
        return value;
    }

    size_t GetCapacity() const
    {
        return mask + 1;
    }

private:
    // Separate cache lines, so the producer and consumer don't slow each other down.
    static constexpr size_t CACHE_LINE = 64;

    const size_t mask;
    std::unique_ptr<T[]> slots;

    alignas(CACHE_LINE) std::atomic<size_t> headIndex{0};
    size_t cachedTail = 0; // consumer's copy of tailIndex

    alignas(CACHE_LINE) std::atomic<size_t> tailIndex{0};
    size_t cachedHead = 0; // producer's copy of headIndex
};

} // namespace hyoshi
