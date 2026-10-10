#include "audio/AudioDecoder.h"
#include "audio/Synth.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

void Append(std::vector<std::byte>& bytes, const void* data, size_t size)
{
    const auto* begin = static_cast<const std::byte*>(data);
    bytes.insert(bytes.end(), begin, begin + size);
}

template <typename T>
void AppendValue(std::vector<std::byte>& bytes, T value)
{
    Append(bytes, &value, sizeof(value));
}

// A minimal 16-bit PCM WAV file (little-endian host assumed, as on every target platform).
std::vector<std::byte> MakeWav(const std::vector<int16_t>& samples, uint16_t channels, uint32_t sampleRate)
{
    const auto dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::vector<std::byte> bytes;
    Append(bytes, "RIFF", 4);
    AppendValue<uint32_t>(bytes, 36 + dataSize);
    Append(bytes, "WAVEfmt ", 8);
    AppendValue<uint32_t>(bytes, 16);
    AppendValue<uint16_t>(bytes, 1);
    AppendValue<uint16_t>(bytes, channels);
    AppendValue<uint32_t>(bytes, sampleRate);
    AppendValue<uint32_t>(bytes, sampleRate * channels * 2);
    AppendValue<uint16_t>(bytes, static_cast<uint16_t>(channels * 2));
    AppendValue<uint16_t>(bytes, 16);
    Append(bytes, "data", 4);
    AppendValue<uint32_t>(bytes, dataSize);
    Append(bytes, samples.data(), dataSize);
    return bytes;
}

} // namespace

TEST_CASE("DecodeAudio reads WAV and duplicates mono to stereo")
{
    const std::vector<int16_t> samples{0, 1000, -1000, 32767, -32768};
    const std::vector<std::byte> wav = MakeWav(samples, 1, 48'000);

    hyoshi::Result<hyoshi::audio::PcmBuffer> pcm = hyoshi::audio::DecodeAudio(wav, 48'000);
    REQUIRE(pcm);
    CHECK(pcm.Value().SampleRate == 48'000);
    REQUIRE(pcm.Value().GetFrameCount() == 5);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        CHECK(pcm.Value().Samples[i * 2] == samples[i]);
        CHECK(pcm.Value().Samples[(i * 2) + 1] == samples[i]);
    }
}

TEST_CASE("DecodeAudio resamples to the requested rate")
{
    const std::vector<int16_t> samples(44'100, 0);
    const std::vector<std::byte> wav = MakeWav(samples, 1, 44'100);

    hyoshi::Result<hyoshi::audio::PcmBuffer> pcm = hyoshi::audio::DecodeAudio(wav, 48'000);
    REQUIRE(pcm);
    CHECK(pcm.Value().GetFrameCount() == doctest::Approx(48'000).epsilon(0.001));
}

TEST_CASE("DecodeAudio rejects data that isn't audio")
{
    const std::vector<std::byte> garbage(1000, std::byte{0x5A});
    CHECK_FALSE(hyoshi::audio::DecodeAudio(garbage, 48'000));
}

TEST_CASE("OpenStreamDecoder decodes a WAV file as DecodeAudio does, from any position")
{
    std::vector<int16_t> samples(10'007 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<int16_t>((i * 37) % 30'000);
    }
    const std::vector<std::byte> wav = MakeWav(samples, 2, 48'000);
    hyoshi::Result<hyoshi::audio::PcmBuffer> whole = hyoshi::audio::DecodeAudio(wav, 48'000);
    REQUIRE(whole);

    hyoshi::Result<std::unique_ptr<hyoshi::audio::IStreamDecoder>> opened =
        hyoshi::audio::OpenStreamDecoder(wav, 48'000);
    REQUIRE(opened);
    hyoshi::audio::IStreamDecoder& decoder = *opened.Value();
    CHECK(decoder.GetFrameCount() == whole.Value().GetFrameCount());
    CHECK(decoder.HasExactSeeks());

    std::vector<int16_t> streamed(samples.size());
    CHECK(decoder.Read(streamed.data(), 4'000) == 4'000);
    CHECK(decoder.Read(streamed.data() + (4'000 * 2), 20'000) == 6'007);
    CHECK(streamed == whole.Value().Samples);
    CHECK(decoder.Read(streamed.data(), 100) == 0);

    REQUIRE(decoder.Seek(9'000));
    std::vector<int16_t> tail(1'007 * 2);
    CHECK(decoder.Read(tail.data(), 1'007) == 1'007);
    CHECK(std::equal(tail.begin(), tail.end(), whole.Value().Samples.begin() + (9'000 * 2)));

    REQUIRE(decoder.Seek(0));
    CHECK(decoder.Skip(5'000) == 5'000);
    CHECK(decoder.Read(tail.data(), 1'007) == 1'007);
    CHECK(std::equal(tail.begin(), tail.end(), whole.Value().Samples.begin() + (5'000 * 2)));
}

TEST_CASE("OpenStreamDecoder knows a resampled file's length")
{
    const std::vector<std::byte> wav = MakeWav(std::vector<int16_t>(44'100, 0), 1, 44'100);
    hyoshi::Result<std::unique_ptr<hyoshi::audio::IStreamDecoder>> opened =
        hyoshi::audio::OpenStreamDecoder(wav, 48'000);
    REQUIRE(opened);
    CHECK(opened.Value()->GetFrameCount() == doctest::Approx(48'000).epsilon(0.001));
}

TEST_CASE("OpenStreamDecoder rejects data that isn't audio")
{
    CHECK_FALSE(hyoshi::audio::OpenStreamDecoder(std::vector<std::byte>(1000, std::byte{0x5A}), 48'000));
}

TEST_CASE("The metronome track has each click on its beat frame")
{
    constexpr uint32_t RATE = 48'000;
    const hyoshi::audio::PcmBuffer track = hyoshi::audio::MakeMetronomeTrack(RATE, 128.0, 8, 4, 250'000);

    for (int64_t beat = 0; beat < 8; ++beat)
    {
        const hyoshi::DspFrame frame = hyoshi::UsToFrames(hyoshi::audio::GetBeatTimeUs(128.0, 250'000, beat), RATE);
        CHECK(track.Samples[static_cast<size_t>(frame - 1) * 2] == 0);
        CHECK(track.Samples[static_cast<size_t>(frame) * 2] > 10'000);
    }
}
