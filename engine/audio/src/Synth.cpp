#include "audio/Synth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hyoshi::audio
{

namespace
{

constexpr float INT16_MAX_F = 32767.0f;

constexpr float ACCENT_HZ = 1760.0f;
constexpr float BEAT_HZ = 1100.0f;
constexpr float METRONOME_CLICK_MS = 40.0f;
constexpr float METRONOME_GAIN = 0.6f;

void AddInto(PcmBuffer& target, const PcmBuffer& source, DspFrame atFrame)
{
    const DspFrame count = std::min(source.GetFrameCount(), target.GetFrameCount() - atFrame);
    for (DspFrame frame = 0; frame < count; ++frame)
    {
        for (uint32_t channel = 0; channel < PcmBuffer::CHANNELS; ++channel)
        {
            const size_t targetIndex = static_cast<size_t>((atFrame + frame) * PcmBuffer::CHANNELS) + channel;
            const size_t sourceIndex = static_cast<size_t>(frame * PcmBuffer::CHANNELS) + channel;
            const int32_t sum = int32_t{target.Samples[targetIndex]} + int32_t{source.Samples[sourceIndex]};
            target.Samples[targetIndex] = static_cast<int16_t>(std::clamp(sum, -32768, 32767));
        }
    }
}

} // namespace

PcmBuffer MakeClick(uint32_t sampleRate, float frequencyHz, float durationMs, float gain)
{
    const auto frameCount = static_cast<size_t>(static_cast<float>(sampleRate) * durationMs / 1000.0f);
    const float decayFrames = static_cast<float>(frameCount) / 6.0f;
    const float phaseStep = 2.0f * std::numbers::pi_v<float> * frequencyHz / static_cast<float>(sampleRate);

    PcmBuffer buffer;
    buffer.SampleRate = sampleRate;
    buffer.Samples.resize(frameCount * PcmBuffer::CHANNELS);
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto t = static_cast<float>(frame);
        const float value = gain * std::cos(phaseStep * t) * std::exp(-t / decayFrames);
        const auto sample = static_cast<int16_t>(std::lround(value * INT16_MAX_F));
        buffer.Samples[frame * PcmBuffer::CHANNELS] = sample;
        buffer.Samples[(frame * PcmBuffer::CHANNELS) + 1] = sample;
    }
    return buffer;
}

SongTimeUs GetBeatTimeUs(double bpm, SongTimeUs firstBeatUs, int64_t index)
{
    return firstBeatUs + std::llround(static_cast<double>(index) * 60.0e6 / bpm);
}

PcmBuffer MakeMetronomeTrack(uint32_t sampleRate, double bpm, uint32_t beatCount, uint32_t beatsPerBar,
                             SongTimeUs firstBeatUs)
{
    const PcmBuffer accent = MakeClick(sampleRate, ACCENT_HZ, METRONOME_CLICK_MS, METRONOME_GAIN);
    const PcmBuffer beat = MakeClick(sampleRate, BEAT_HZ, METRONOME_CLICK_MS, METRONOME_GAIN);

    const SongTimeUs endUs = GetBeatTimeUs(bpm, firstBeatUs, beatCount) + US_PER_SECOND;
    PcmBuffer track;
    track.SampleRate = sampleRate;
    track.Samples.resize(static_cast<size_t>(UsToFrames(endUs, sampleRate)) * PcmBuffer::CHANNELS);

    for (uint32_t i = 0; i < beatCount; ++i)
    {
        const DspFrame frame = UsToFrames(GetBeatTimeUs(bpm, firstBeatUs, i), sampleRate);
        AddInto(track, i % beatsPerBar == 0 ? accent : beat, frame);
    }
    return track;
}

} // namespace hyoshi::audio
