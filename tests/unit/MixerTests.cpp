#include "audio/Mixer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using hyoshi::DspFrame;
using hyoshi::audio::AudioCommand;
using hyoshi::audio::AudioCommandType;
using hyoshi::audio::Bus;
using hyoshi::audio::Mixer;
using hyoshi::audio::PcmBuffer;

namespace
{

constexpr uint32_t RATE = 48'000;
constexpr uint32_t BLOCK = 256;

PcmBuffer MakeConstant(DspFrame frames, int16_t value)
{
    PcmBuffer buffer;
    buffer.SampleRate = RATE;
    buffer.Samples.assign(static_cast<size_t>(frames) * PcmBuffer::CHANNELS, value);
    return buffer;
}

// Renders `frames` frames in fixed blocks, returning the left channel.
std::vector<float> RenderLeft(Mixer& mixer, DspFrame frames, hyoshi::HostTimeNs& time)
{
    std::vector<float> left;
    std::vector<float> block(size_t{BLOCK} * PcmBuffer::CHANNELS);
    for (DspFrame done = 0; done < frames; done += BLOCK)
    {
        time += 5'333'333;
        mixer.Render(block.data(), BLOCK, time);
        for (uint32_t i = 0; i < BLOCK; ++i)
        {
            left.push_back(block[size_t{i} * PcmBuffer::CHANNELS]);
        }
    }
    return left;
}

} // namespace

TEST_CASE("Mixer starts a scheduled voice on its exact music frame")
{
    const PcmBuffer silence = MakeConstant(RATE, 0);
    const PcmBuffer click = MakeConstant(10, 16384);

    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &silence});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 1, .Frame = 1000, .Buffer = &click});

    hyoshi::HostTimeNs time = 0;
    const std::vector<float> left = RenderLeft(mixer, 2048, time);

    CHECK(left[999] == 0.0f);
    CHECK(left[1000] == doctest::Approx(0.5f));
    CHECK(left[1009] == doctest::Approx(0.5f));
    CHECK(left[1010] == 0.0f);
    CHECK(mixer.GetStats().LateVoices == 0);
}

TEST_CASE("Mixer snapshots describe the start of each callback")
{
    const PcmBuffer silence = MakeConstant(RATE, 0);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &silence});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    hyoshi::HostTimeNs time = 0;
    RenderLeft(mixer, BLOCK * 3, time);

    const hyoshi::audio::AudioClockSnapshot snapshot = mixer.GetSnapshot();
    CHECK(snapshot.SampleRate == RATE);
    CHECK(snapshot.DeviceFrames == BLOCK * 2);
    CHECK(snapshot.MusicFrame == BLOCK * 2);
    CHECK(snapshot.HostTime == time);
    CHECK(snapshot.IsMusicPlaying == 1);
    CHECK(snapshot.SeekGeneration == 1);
    CHECK(snapshot.CommandsApplied == 2);
    CHECK(snapshot.CallbackFrames == BLOCK);
}

TEST_CASE("Pausing holds the music and scheduled voices; seeking cancels them")
{
    const PcmBuffer silence = MakeConstant(RATE, 0);
    const PcmBuffer click = MakeConstant(10, 16384);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &silence});
    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 1, .Frame = 100, .Buffer = &click});

    hyoshi::HostTimeNs time = 0;
    std::vector<float> left = RenderLeft(mixer, 1024, time);
    CHECK(mixer.GetSnapshot().MusicFrame == 0);
    CHECK(mixer.GetStats().ActiveVoices == 1);
    CHECK(left[100] == 0.0f);

    mixer.Submit({.Type = AudioCommandType::SeekMusic, .Frame = 5000});
    left = RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetSnapshot().MusicFrame == 5000);
    CHECK(mixer.GetSnapshot().SeekGeneration == 2);
    CHECK(mixer.GetStats().ActiveVoices == 0);
}

TEST_CASE("Music stops at its end and bus volumes scale it")
{
    const PcmBuffer music = MakeConstant(300, 16384);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &music});
    mixer.Submit({.Type = AudioCommandType::SetBusVolume, .TargetBus = Bus::Music, .Volume = 0.5f});
    mixer.Submit({.Type = AudioCommandType::SetBusVolume, .TargetBus = Bus::Master, .Volume = 0.5f});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    hyoshi::HostTimeNs time = 0;
    const std::vector<float> left = RenderLeft(mixer, 512, time);
    CHECK(left[0] == doctest::Approx(0.125f));
    CHECK(left[299] == doctest::Approx(0.125f));
    CHECK(left[300] == 0.0f);

    RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
    CHECK(mixer.GetSnapshot().MusicFrame == 300);
}

TEST_CASE("Releasing a buffer stops everything that reads it")
{
    const PcmBuffer sound = MakeConstant(RATE, 1000);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &sound});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 7, .Frame = Mixer::NOT_SCHEDULED, .Buffer = &sound});

    hyoshi::HostTimeNs time = 0;
    RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetStats().ActiveVoices == 1);

    mixer.Submit({.Type = AudioCommandType::ReleaseBuffer, .Buffer = &sound});
    const std::vector<float> left = RenderLeft(mixer, BLOCK, time);
    CHECK(left[0] == 0.0f);
    CHECK(mixer.GetStats().ActiveVoices == 0);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
}

TEST_CASE("A full voice pool drops new voices")
{
    const PcmBuffer sound = MakeConstant(RATE, 1000);
    Mixer mixer(RATE, 2);
    for (uint32_t i = 1; i <= 3; ++i)
    {
        mixer.Submit(
            {.Type = AudioCommandType::PlayVoice, .Handle = i, .Frame = Mixer::NOT_SCHEDULED, .Buffer = &sound});
    }
    hyoshi::HostTimeNs time = 0;
    RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetStats().ActiveVoices == 2);
    CHECK(mixer.GetStats().DroppedVoices == 1);
}

TEST_CASE("A scheduled voice whose frame has passed is dropped, not played late")
{
    const PcmBuffer silence = MakeConstant(RATE, 0);
    const PcmBuffer click = MakeConstant(10, 16384);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &silence});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    hyoshi::HostTimeNs time = 0;
    RenderLeft(mixer, 1024, time);

    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 1, .Frame = 500, .Buffer = &click});
    const std::vector<float> left = RenderLeft(mixer, BLOCK, time);
    CHECK(left[0] == 0.0f);
    CHECK(mixer.GetStats().LateVoices == 1);
    CHECK(mixer.GetStats().ActiveVoices == 0);
}
