#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace hyoshi
{

// Publishes a small value from one writer thread to any number of readers without locks
// (DESIGN.md section 9). Store never blocks, so the writer can be the audio thread. Load retries
// while a store is in progress, which is rare and brief.
template <typename T>
class SeqLock
{
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock values are copied as raw words");
    static_assert(sizeof(T) % sizeof(uint64_t) == 0, "SeqLock values must be a whole number of 64-bit words");

public:
    SeqLock()
    {
        Store(T{});
    }

    // Single writer only.
    void Store(const T& value)
    {
        std::array<uint64_t, WORD_COUNT> source{};
        std::memcpy(source.data(), &value, sizeof(T));

        const uint64_t sequenceNumber = sequence.load(std::memory_order_relaxed);
        sequence.store(sequenceNumber + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < WORD_COUNT; ++i)
        {
            words[i].store(source[i], std::memory_order_relaxed);
        }
        sequence.store(sequenceNumber + 2, std::memory_order_release);
    }

    T Load() const
    {
        std::array<uint64_t, WORD_COUNT> copy{};
        uint64_t before = 0;
        uint64_t after = 0;
        do
        {
            before = sequence.load(std::memory_order_acquire);
            for (size_t i = 0; i < WORD_COUNT; ++i)
            {
                copy[i] = words[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            after = sequence.load(std::memory_order_relaxed);
        } while ((before & 1) != 0 || before != after);

        T value;
        std::memcpy(&value, copy.data(), sizeof(T));
        return value;
    }

private:
    static constexpr size_t WORD_COUNT = sizeof(T) / sizeof(uint64_t);

    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint64_t>, WORD_COUNT> words{};
};

} // namespace hyoshi
