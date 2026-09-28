#pragma once

#include <cstdint>

namespace hyoshi
{

// Time types from DESIGN.md section 9. Gameplay timing is integer only.

// Nanoseconds on the host clock (SDL_GetTicksNS), shared by input timestamps and audio snapshots.
// Monotonic.
using HostTimeNs = int64_t;

// Song time in microseconds; 0 is the start of the audio file.
using SongTimeUs = int64_t;

// Audio sample frames.
using DspFrame = int64_t;

// A function returning the current host time. Safe to call from any thread, including the audio
// thread, so modules that must not depend on the platform layer can still read the host clock.
using HostClockFn = HostTimeNs (*)();

constexpr int64_t NS_PER_US = 1'000;
constexpr int64_t NS_PER_MS = 1'000'000;
constexpr int64_t US_PER_MS = 1'000;
constexpr int64_t US_PER_SECOND = 1'000'000;
constexpr int64_t NS_PER_SECOND = 1'000'000'000;

// Frame-to-time conversions round toward negative infinity, so a frame count maps to the start of
// the microsecond it falls in, and they stay exact for any song length.
constexpr int64_t FloorDiv(int64_t numerator, int64_t denominator)
{
    const int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) ? quotient - 1 : quotient;
}

constexpr SongTimeUs FramesToUs(DspFrame frames, int64_t sampleRate)
{
    return FloorDiv(frames * US_PER_SECOND, sampleRate);
}

constexpr DspFrame UsToFrames(int64_t microseconds, int64_t sampleRate)
{
    return FloorDiv(microseconds * sampleRate, US_PER_SECOND);
}

// For display and logs only; gameplay math stays in integer microseconds.
constexpr double ToSeconds(SongTimeUs time)
{
    return static_cast<double>(time) / static_cast<double>(US_PER_SECOND);
}

constexpr double ToMilliseconds(int64_t microseconds)
{
    return static_cast<double>(microseconds) / static_cast<double>(US_PER_MS);
}

} // namespace hyoshi
