#include "DefaultHitSounds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace hyoshi::songs::detail
{

namespace
{

using circle::SampleBank;

constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
constexpr float INT16_MAX_F = 32767.0f;

// White noise, the same every time.
class Noise
{
public:
    float Next()
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return (static_cast<float>(state) / static_cast<float>(UINT32_MAX) * 2.0f) - 1.0f;
    }

private:
    uint32_t state = 0x9E3779B9U;
};

// A biquad from the Audio EQ Cookbook.
class Filter
{
public:
    enum class Type : uint8_t
    {
        LowPass,
        HighPass,
        BandPass
    };

    Filter(Type type, float hz, float q, float sampleRate)
    {
        const float omega = TWO_PI * std::min(hz, sampleRate * 0.45f) / sampleRate;
        const float alpha = std::sin(omega) / (2.0f * q);
        const float cosine = std::cos(omega);
        float b0 = 0.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        switch (type)
        {
        case Type::LowPass:
            b0 = (1.0f - cosine) * 0.5f;
            b1 = 1.0f - cosine;
            b2 = b0;
            break;
        case Type::HighPass:
            b0 = (1.0f + cosine) * 0.5f;
            b1 = -(1.0f + cosine);
            b2 = b0;
            break;
        case Type::BandPass:
            b0 = alpha;
            b2 = -alpha;
            break;
        }
        const float a0 = 1.0f + alpha;
        coefficients = {b0 / a0, b1 / a0, b2 / a0, -2.0f * cosine / a0, (1.0f - alpha) / a0};
    }

    float Process(float input)
    {
        const auto& [b0, b1, b2, a1, a2] = coefficients;
        const float output = (b0 * input) + (b1 * x1) + (b2 * x2) - (a1 * y1) - (a2 * y2);
        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;
        return output;
    }

private:
    std::array<float, 5> coefficients{};
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
};

// A sound built up in mono floats, then scaled to its peak.
class Builder
{
public:
    Builder(uint32_t sampleRate, float durationMs)
        : rate(static_cast<float>(sampleRate)),
          samples(static_cast<size_t>(static_cast<float>(sampleRate) * durationMs / 1000.0f), 0.0f)
    {
    }

    // A sine gliding from startHz toward endHz (closing 63% of the way every glideMs), rising over
    // attackMs and decaying by 63% every decayMs.
    void Tone(float startHz, float endHz, float glideMs, float decayMs, float gain, float attackMs = 0.0f)
    {
        float phase = 0.0f;
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const float ms = ToMs(i);
            const float hz = endHz + ((startHz - endHz) * std::exp(-ms / glideMs));
            phase += TWO_PI * hz / rate;
            const float rise = attackMs > 0.0f ? 1.0f - std::exp(-ms / attackMs) : 1.0f;
            samples[i] += gain * rise * std::exp(-ms / decayMs) * std::sin(phase);
        }
    }

    // Filtered noise from delayMs on, decaying.
    void Burst(Filter filter, float decayMs, float gain, float delayMs = 0.0f)
    {
        Noise noise;
        const auto start = static_cast<size_t>(delayMs * rate / 1000.0f);
        for (size_t i = start; i < samples.size(); ++i)
        {
            const float ms = ToMs(i - start);
            samples[i] += gain * std::exp(-ms / decayMs) * filter.Process(noise.Next());
        }
    }

    // A cymbal: square waves at inharmonic ratios (a classic drum machine's), high-passed.
    void Metal(float pitch, float cutoffHz, float decayMs, float gain)
    {
        constexpr std::array<float, 6> PARTIALS_HZ{205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
        Filter filter(Filter::Type::HighPass, cutoffHz, 0.7f, rate);
        std::array<float, PARTIALS_HZ.size()> phases{};
        for (size_t i = 0; i < samples.size(); ++i)
        {
            float sum = 0.0f;
            for (size_t p = 0; p < PARTIALS_HZ.size(); ++p)
            {
                phases[p] += TWO_PI * PARTIALS_HZ[p] * pitch / rate;
                sum += std::sin(phases[p]) >= 0.0f ? 1.0f : -1.0f;
            }
            samples[i] += gain * std::exp(-ToMs(i) / decayMs) * filter.Process(sum / PARTIALS_HZ.size());
        }
    }

    audio::PcmBuffer Finish(float peak) const
    {
        float loudest = 0.0f;
        for (const float sample : samples)
        {
            loudest = std::max(loudest, std::abs(sample));
        }
        const float scale = loudest > 0.0f ? peak / loudest : 0.0f;
        audio::PcmBuffer buffer;
        buffer.SampleRate = static_cast<uint32_t>(rate);
        buffer.Samples.resize(samples.size() * audio::PcmBuffer::CHANNELS);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto value =
                static_cast<int16_t>(std::lround(std::clamp(samples[i] * scale, -1.0f, 1.0f) * INT16_MAX_F));
            buffer.Samples[i * audio::PcmBuffer::CHANNELS] = value;
            buffer.Samples[(i * audio::PcmBuffer::CHANNELS) + 1] = value;
        }
        return buffer;
    }

    float GetRate() const
    {
        return rate;
    }

private:
    float ToMs(size_t frame) const
    {
        return static_cast<float>(frame) * 1000.0f / rate;
    }

    float rate;
    std::vector<float> samples;
};

audio::PcmBuffer MakeNormalSound(SampleBank bank, uint32_t sampleRate)
{
    switch (bank)
    {
    case SampleBank::Normal:
    {
        // A crisp click with a little body.
        Builder sound(sampleRate, 90.0f);
        sound.Burst(Filter(Filter::Type::BandPass, 3500.0f, 0.9f, sound.GetRate()), 9.0f, 1.0f);
        sound.Tone(1200.0f, 1000.0f, 10.0f, 22.0f, 0.6f);
        sound.Tone(220.0f, 150.0f, 15.0f, 25.0f, 0.5f);
        return sound.Finish(0.55f);
    }
    case SampleBank::Soft:
    {
        // A muted knock.
        Builder sound(sampleRate, 120.0f);
        sound.Tone(650.0f, 480.0f, 12.0f, 35.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::LowPass, 2500.0f, 0.7f, sound.GetRate()), 6.0f, 0.35f);
        return sound.Finish(0.45f);
    }
    case SampleBank::Drum:
    {
        // A kick.
        Builder sound(sampleRate, 260.0f);
        sound.Tone(170.0f, 48.0f, 30.0f, 110.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::BandPass, 2500.0f, 0.8f, sound.GetRate()), 4.0f, 0.4f);
        return sound.Finish(0.65f);
    }
    }
    return {};
}

audio::PcmBuffer MakeWhistle(SampleBank bank, uint32_t sampleRate)
{
    switch (bank)
    {
    case SampleBank::Normal:
    {
        Builder sound(sampleRate, 320.0f);
        sound.Tone(2300.0f, 2550.0f, 60.0f, 120.0f, 1.0f, 6.0f);
        sound.Tone(4600.0f, 5100.0f, 60.0f, 80.0f, 0.15f, 6.0f);
        return sound.Finish(0.4f);
    }
    case SampleBank::Soft:
    {
        Builder sound(sampleRate, 380.0f);
        sound.Tone(1500.0f, 1650.0f, 80.0f, 150.0f, 1.0f, 10.0f);
        return sound.Finish(0.32f);
    }
    case SampleBank::Drum:
    {
        // A tom.
        Builder sound(sampleRate, 320.0f);
        sound.Tone(280.0f, 170.0f, 40.0f, 130.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::BandPass, 1200.0f, 1.0f, sound.GetRate()), 8.0f, 0.3f);
        return sound.Finish(0.55f);
    }
    }
    return {};
}

audio::PcmBuffer MakeFinish(SampleBank bank, uint32_t sampleRate)
{
    switch (bank)
    {
    case SampleBank::Normal:
    {
        Builder sound(sampleRate, 1300.0f);
        sound.Metal(1.0f, 4000.0f, 450.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::HighPass, 5000.0f, 0.7f, sound.GetRate()), 500.0f, 0.6f);
        return sound.Finish(0.45f);
    }
    case SampleBank::Soft:
    {
        Builder sound(sampleRate, 1500.0f);
        sound.Metal(0.8f, 2500.0f, 550.0f, 0.8f);
        sound.Burst(Filter(Filter::Type::BandPass, 4000.0f, 0.5f, sound.GetRate()), 600.0f, 0.5f);
        return sound.Finish(0.35f);
    }
    case SampleBank::Drum:
    {
        // A crash over a kick.
        Builder sound(sampleRate, 1300.0f);
        sound.Metal(1.0f, 3500.0f, 450.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::HighPass, 4000.0f, 0.7f, sound.GetRate()), 450.0f, 0.6f);
        sound.Tone(150.0f, 50.0f, 30.0f, 120.0f, 1.2f);
        return sound.Finish(0.55f);
    }
    }
    return {};
}

audio::PcmBuffer MakeClap(SampleBank bank, uint32_t sampleRate)
{
    if (bank == SampleBank::Drum)
    {
        // A snare.
        Builder sound(sampleRate, 260.0f);
        sound.Burst(Filter(Filter::Type::HighPass, 1500.0f, 0.7f, sound.GetRate()), 90.0f, 1.0f);
        sound.Tone(200.0f, 170.0f, 20.0f, 45.0f, 0.8f);
        return sound.Finish(0.55f);
    }
    // Hands meeting a few times in quick succession, then the room.
    const bool isSoft = bank == SampleBank::Soft;
    const float hz = isSoft ? 900.0f : 1300.0f;
    const float q = isSoft ? 1.2f : 1.8f;
    Builder sound(sampleRate, 220.0f);
    for (const auto& [delayMs, gain] : {std::pair{0.0f, 1.0f}, std::pair{8.0f, 0.9f}, std::pair{16.0f, 0.8f}})
    {
        sound.Burst(Filter(Filter::Type::BandPass, hz, q, sound.GetRate()), 4.0f, gain, delayMs);
    }
    sound.Burst(Filter(Filter::Type::BandPass, hz * 0.92f, q * 0.8f, sound.GetRate()), 45.0f, 0.7f, 22.0f);
    return sound.Finish(isSoft ? 0.38f : 0.5f);
}

audio::PcmBuffer MakeTick(SampleBank bank, uint32_t sampleRate)
{
    switch (bank)
    {
    case SampleBank::Normal:
    {
        Builder sound(sampleRate, 45.0f);
        sound.Tone(2100.0f, 2100.0f, 1.0f, 9.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::HighPass, 5000.0f, 0.7f, sound.GetRate()), 3.0f, 0.3f);
        return sound.Finish(0.28f);
    }
    case SampleBank::Soft:
    {
        Builder sound(sampleRate, 50.0f);
        sound.Tone(1400.0f, 1300.0f, 10.0f, 12.0f, 1.0f);
        return sound.Finish(0.22f);
    }
    case SampleBank::Drum:
    {
        Builder sound(sampleRate, 60.0f);
        sound.Tone(800.0f, 700.0f, 10.0f, 15.0f, 1.0f);
        sound.Burst(Filter(Filter::Type::BandPass, 2000.0f, 1.0f, sound.GetRate()), 4.0f, 0.3f);
        return sound.Finish(0.3f);
    }
    }
    return {};
}

} // namespace

const char* GetBankFileName(SampleBank bank)
{
    switch (bank)
    {
    case SampleBank::Normal:
        break;
    case SampleBank::Soft:
        return "soft";
    case SampleBank::Drum:
        return "drum";
    }
    return "normal";
}

const char* GetHitSoundFileName(HitSoundName name)
{
    switch (name)
    {
    case HitSoundName::HitNormal:
        break;
    case HitSoundName::HitWhistle:
        return "hitwhistle";
    case HitSoundName::HitFinish:
        return "hitfinish";
    case HitSoundName::HitClap:
        return "hitclap";
    case HitSoundName::SliderTick:
        return "slidertick";
    }
    return "hitnormal";
}

audio::PcmBuffer MakeDefaultHitSound(SampleBank bank, HitSoundName name, uint32_t sampleRate)
{
    switch (name)
    {
    case HitSoundName::HitNormal:
        break;
    case HitSoundName::HitWhistle:
        return MakeWhistle(bank, sampleRate);
    case HitSoundName::HitFinish:
        return MakeFinish(bank, sampleRate);
    case HitSoundName::HitClap:
        return MakeClap(bank, sampleRate);
    case HitSoundName::SliderTick:
        return MakeTick(bank, sampleRate);
    }
    return MakeNormalSound(bank, sampleRate);
}

} // namespace hyoshi::songs::detail
