#include "rhythm/SongClock.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>

using hyoshi::DspFrame;
using hyoshi::HostTimeNs;
using hyoshi::NS_PER_MS;
using hyoshi::SongTimeUs;
using hyoshi::audio::AudioClockSnapshot;
using hyoshi::rhythm::SongClock;

namespace
{

constexpr int64_t RATE = 48'000;
constexpr DspFrame PERIOD_FRAMES = 480; // 10 ms callbacks
constexpr HostTimeNs PERIOD_NS = 10 * NS_PER_MS;
constexpr HostTimeNs FRAME_NS = 8'333'333; // 120 Hz display
constexpr int64_t LATENCY_US = 20'000;

// A simulated device: music started at host time 0, and callback k renders music frames from
// k * PERIOD_FRAMES. Callback start times jitter by up to +-2 ms, like a real OS scheduler.
struct FakeDevice
{
    uint32_t Seed = 12345;
    int64_t Generation = 1;
    DspFrame MusicStartFrame = 0;
    HostTimeNs MusicStartHost = 0;

    HostTimeNs Jitter(int64_t callback) const
    {
        uint32_t x = Seed ^ static_cast<uint32_t>(callback * 2654435761u);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return static_cast<HostTimeNs>(x % 4'000'001) - 2'000'000;
    }

    // The newest snapshot published at or before `now`.
    AudioClockSnapshot Latest(HostTimeNs now) const
    {
        int64_t callback = (now - MusicStartHost) / PERIOD_NS + 1;
        HostTimeNs start = 0;
        do
        {
            --callback;
            start = MusicStartHost + (callback * PERIOD_NS) + Jitter(callback);
        } while (callback > 0 && start > now);

        AudioClockSnapshot snapshot;
        snapshot.SampleRate = RATE;
        snapshot.HostTime = callback == 0 ? MusicStartHost : start;
        snapshot.MusicFrame = MusicStartFrame + (callback * PERIOD_FRAMES);
        snapshot.DeviceFrames = snapshot.MusicFrame;
        snapshot.IsMusicPlaying = 1;
        snapshot.SeekGeneration = Generation;
        snapshot.CallbackFrames = PERIOD_FRAMES;
        return snapshot;
    }

    // What is audible at `now`, in song time.
    SongTimeUs Truth(HostTimeNs now) const
    {
        return hyoshi::FramesToUs(MusicStartFrame, RATE) + ((now - MusicStartHost) / hyoshi::NS_PER_US) - LATENCY_US;
    }
};

// Updates a clock once per display frame and watches that it never goes backwards.
struct Simulation
{
    SongClock Clock;
    // Time of the last update.
    HostTimeNs Now = 0;
    SongTimeUs Previous = INT64_MIN;
    bool IsMonotonic = true;

    Simulation()
    {
        Clock.OutputLatencyUs = LATENCY_US;
    }

    template <typename SnapshotFn>
    void RunWith(HostTimeNs duration, SnapshotFn&& snapshotAt)
    {
        const HostTimeNs end = Now + duration;
        while (Now < end)
        {
            Now += FRAME_NS;
            Clock.Update(snapshotAt(Now), Now);
            IsMonotonic = IsMonotonic && Clock.GetSongTime() >= Previous;
            Previous = Clock.GetSongTime();
        }
    }

    void RunFor(HostTimeNs duration, const FakeDevice& device)
    {
        RunWith(duration, [&device](HostTimeNs now) { return device.Latest(now); });
    }

    SongTimeUs ErrorAgainst(const FakeDevice& device) const
    {
        return std::abs(Clock.GetSongTime() - device.Truth(Now));
    }
};

} // namespace

TEST_CASE("SongClock tracks jittery snapshots smoothly and never goes backwards")
{
    const FakeDevice device;
    Simulation simulation;
    simulation.RunFor(1'000 * NS_PER_MS, device);

    SongTimeUs worstError = 0;
    SongTimeUs worstInputError = 0;
    for (int i = 0; i < 2'000; ++i)
    {
        simulation.RunFor(FRAME_NS, device);
        worstError = std::max(worstError, simulation.ErrorAgainst(device));
        const HostTimeNs inputTime = simulation.Now - (3 * NS_PER_MS);
        worstInputError = std::max(worstInputError,
                                   std::abs(simulation.Clock.HostTimeToSongTime(inputTime) - device.Truth(inputTime)));
    }

    CHECK(simulation.IsMonotonic);
    CHECK(simulation.Clock.IsPlaying());
    CHECK(simulation.Clock.GetResyncCount() == 0);
    // Raw estimates are off by up to 2 ms; the filtered clock should do clearly better.
    CHECK(worstError <= 1'000);
    CHECK(worstInputError <= 1'000);
}

TEST_CASE("SongClock freezes while paused")
{
    SongClock clock;
    clock.OutputLatencyUs = LATENCY_US;
    AudioClockSnapshot paused;
    paused.SampleRate = RATE;
    paused.MusicFrame = 96'000;
    paused.IsMusicPlaying = 0;
    paused.SeekGeneration = 1;
    paused.CallbackFrames = PERIOD_FRAMES;

    clock.Update(paused, 5'000 * NS_PER_MS);
    CHECK(clock.GetSongTime() == 2'000'000 - LATENCY_US);
    clock.Update(paused, 6'000 * NS_PER_MS);
    CHECK(clock.GetSongTime() == 2'000'000 - LATENCY_US);
    CHECK_FALSE(clock.IsPlaying());
    CHECK(clock.HostTimeToSongTime(7'000 * NS_PER_MS) == 2'000'000 - LATENCY_US);
}

TEST_CASE("SongClock shows a seek target immediately and resyncs when the audio thread applies it")
{
    FakeDevice device;
    Simulation simulation;
    simulation.RunFor(2'000 * NS_PER_MS, device);

    const DspFrame seekFrame = 30 * RATE;
    const SongTimeUs target = simulation.Clock.SongTimeForMusicFrame(seekFrame, RATE);
    CHECK(simulation.Clock.MusicFrameForSongTime(target, RATE) == seekFrame);
    simulation.Clock.ExpectSeek(2, target);
    CHECK(simulation.Clock.GetSongTime() == target);

    // Snapshots from before the seek are ignored.
    simulation.RunFor(FRAME_NS, device);
    CHECK(simulation.Clock.GetSongTime() == target);

    // The audio thread applies it at its next callback.
    device.Generation = 2;
    device.MusicStartFrame = seekFrame;
    device.MusicStartHost = simulation.Now + (2 * NS_PER_MS);
    simulation.RunFor(FRAME_NS, device);
    CHECK(simulation.ErrorAgainst(device) <= 2'000);

    simulation.RunFor(2'000 * NS_PER_MS, device);
    CHECK(simulation.IsMonotonic);
    CHECK(simulation.ErrorAgainst(device) <= 1'000);
    CHECK(simulation.Clock.GetResyncCount() == 0);
}

TEST_CASE("SongClock holds through a device stall without rewinding")
{
    const FakeDevice device;
    Simulation simulation;
    simulation.RunFor(1'000 * NS_PER_MS, device);

    // The device stops: the newest snapshot stays the same for 300 ms.
    const AudioClockSnapshot frozen = device.Latest(simulation.Now);
    simulation.RunWith(300 * NS_PER_MS, [&frozen](HostTimeNs) { return frozen; });
    CHECK(simulation.Clock.IsStalled());
    CHECK(simulation.Clock.GetSongTime() - hyoshi::FramesToUs(frozen.MusicFrame, RATE) <= 60'000);

    // It restarts where the music left off.
    FakeDevice resumed = device;
    resumed.MusicStartFrame = frozen.MusicFrame + PERIOD_FRAMES;
    resumed.MusicStartHost = simulation.Now;
    simulation.RunFor(1'000 * NS_PER_MS, resumed);

    CHECK(simulation.IsMonotonic);
    CHECK_FALSE(simulation.Clock.IsStalled());
    CHECK(simulation.ErrorAgainst(resumed) <= 1'000);
}

TEST_CASE("SongClock re-anchors on a large jump without a seek")
{
    FakeDevice device;
    Simulation simulation;
    simulation.RunFor(1'000 * NS_PER_MS, device);

    // The music skips 100 ms ahead, as after a dropped buffer on some devices.
    device.MusicStartFrame += RATE / 10;
    simulation.RunFor(500 * NS_PER_MS, device);

    CHECK(simulation.Clock.GetResyncCount() == 1);
    CHECK(simulation.ErrorAgainst(device) <= 1'000);
    CHECK(simulation.IsMonotonic);
}
