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

// Frame i holds i, up to 20000, in both channels.
PcmBuffer MakeRamp(DspFrame frames)
{
    PcmBuffer buffer;
    buffer.SampleRate = RATE;
    for (DspFrame i = 0; i < frames; ++i)
    {
        const auto value = static_cast<int16_t>(i % 20'000);
        buffer.Samples.push_back(value);
        buffer.Samples.push_back(value);
    }
    return buffer;
}

float RampAt(double position)
{
    return static_cast<float>(position / 32768.0);
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

TEST_CASE("Mixer plays music at a rate, between its frames")
{
    const PcmBuffer ramp = MakeRamp(RATE);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &ramp});
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 2.0});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    hyoshi::HostTimeNs time = 0;
    std::vector<float> left = RenderLeft(mixer, BLOCK * 2, time);
    CHECK(left[10] == doctest::Approx(RampAt(20.0)));
    CHECK(left[300] == doctest::Approx(RampAt(600.0)));
    CHECK(mixer.GetSnapshot().MusicFrame == BLOCK * 2);
    CHECK(mixer.GetSnapshot().MusicRate == 2.0);

    // Half speed: every other frame is halfway between two.
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 0.5});
    left = RenderLeft(mixer, BLOCK, time);
    CHECK(left[0] == doctest::Approx(RampAt(1024.0)));
    CHECK(left[1] == doctest::Approx(RampAt(1024.5)));
    CHECK(left[2] == doctest::Approx(RampAt(1025.0)));

    // Back at 1 from part of the way between frames, it plays whole frames again.
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 0.3});
    RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetSnapshot().MusicFrame == 1152);
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 1.0});
    left = RenderLeft(mixer, BLOCK, time);
    CHECK(mixer.GetSnapshot().MusicFrame == 1152 + 76);
    CHECK(mixer.GetSnapshot().MusicRate == 1.0);
    CHECK(left[0] == doctest::Approx(RampAt(1228.0)));
    CHECK(left[1] == doctest::Approx(RampAt(1229.0)));
}

TEST_CASE("Mixer starts a scheduled voice where the music reaches its frame, at any rate")
{
    const PcmBuffer silence = MakeConstant(RATE, 0);
    const PcmBuffer click = MakeConstant(10, 16384);

    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &silence});
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 2.0});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 1, .Frame = 1000, .Buffer = &click});

    hyoshi::HostTimeNs time = 0;
    const std::vector<float> left = RenderLeft(mixer, 1024, time);

    // The music reaches frame 1000 at frame 500; the click itself plays as it is.
    CHECK(left[499] == 0.0f);
    CHECK(left[500] == doctest::Approx(0.5f));
    CHECK(left[509] == doctest::Approx(0.5f));
    CHECK(left[510] == 0.0f);
    CHECK(mixer.GetStats().LateVoices == 0);
}

TEST_CASE("New music plays at a rate of 1")
{
    const PcmBuffer first = MakeConstant(RATE, 0);
    const PcmBuffer second = MakeConstant(RATE, 0);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &first});
    mixer.Submit({.Type = AudioCommandType::SetMusicRate, .Rate = 1.5});
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &second});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    hyoshi::HostTimeNs time = 0;
    RenderLeft(mixer, BLOCK * 2, time);
    CHECK(mixer.GetSnapshot().MusicRate == 1.0);
    CHECK(mixer.GetSnapshot().MusicFrame == BLOCK);
}

TEST_CASE("A low-pass muffles the music alone, until new music")
{
    // The highest pitch there is: each frame the other way from the last, at half of full scale.
    PcmBuffer buzz;
    buzz.SampleRate = RATE;
    for (DspFrame i = 0; i < RATE; ++i)
    {
        const int16_t value = i % 2 == 0 ? 16384 : -16384;
        buzz.Samples.push_back(value);
        buzz.Samples.push_back(value);
    }
    const PcmBuffer hold = MakeConstant(BLOCK * 4, 8192);
    const auto loudest = [](const std::vector<float>& samples, size_t from)
    {
        float most = 0.0f;
        for (size_t i = from; i < samples.size(); ++i)
        {
            most = std::max(most, std::abs(samples[i]));
        }
        return most;
    };

    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &buzz});
    mixer.Submit({.Type = AudioCommandType::SetMusicLowPass, .CutoffHz = 400.0f});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    hyoshi::HostTimeNs time = 0;
    CHECK(loudest(RenderLeft(mixer, BLOCK * 2, time), BLOCK) < 0.01f);

    // A voice isn't muffled: it adds its own level on the muffled music.
    mixer.Submit({.Type = AudioCommandType::PlayVoice, .Handle = 1, .Frame = Mixer::NOT_SCHEDULED, .Buffer = &hold});
    const std::vector<float> withVoice = RenderLeft(mixer, BLOCK, time);
    CHECK(withVoice[BLOCK / 2] == doctest::Approx(0.25f).epsilon(0.04));

    // New music plays without it.
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Buffer = &buzz});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    RenderLeft(mixer, BLOCK * 4, time);
    CHECK(loudest(RenderLeft(mixer, BLOCK, time), 0) == doctest::Approx(0.5f));
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
