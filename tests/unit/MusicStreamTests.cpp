#include "audio/AudioDecoder.h"
#include "audio/Mixer.h"
#include "audio/MusicStream.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using hyoshi::DspFrame;
using hyoshi::audio::AudioCommandType;
using hyoshi::audio::IStreamDecoder;
using hyoshi::audio::Mixer;
using hyoshi::audio::MusicStream;
using hyoshi::audio::MusicStreamOptions;
using hyoshi::audio::PcmBuffer;

namespace
{

constexpr uint32_t RATE = 48'000;
constexpr DspFrame BLOCK = MusicStream::BLOCK_FRAMES;
constexpr MusicStreamOptions MANUAL{.IsThreaded = false};

// Frame i of the file holds this in both channels, so every frame is recognizable and none is silent.
int16_t RampValue(DspFrame frame)
{
    return static_cast<int16_t>((frame % 20'000) + 1);
}

// A file whose frames are RampValue. It can end before its stated length, as a truncated file does.
// Without exact seeks, a seek anywhere but the start lands 1000 frames late, as MP3 seeks miss.
class RampDecoder final : public IStreamDecoder
{
public:
    explicit RampDecoder(DspFrame length, DspFrame readableLength = -1, bool hasExactSeeks = true)
        : length(length), readableLength(readableLength < 0 ? length : readableLength), hasExactSeeks(hasExactSeeks)
    {
    }

    DspFrame GetFrameCount() const override
    {
        return length;
    }

    uint32_t Read(int16_t* samples, uint32_t frameCount) override
    {
        const auto frames = static_cast<uint32_t>(std::clamp<DspFrame>(readableLength - position, 0, frameCount));
        for (uint32_t i = 0; i < frames; ++i)
        {
            samples[size_t{i} * 2] = RampValue(position + i);
            samples[(size_t{i} * 2) + 1] = RampValue(position + i);
        }
        position += frames;
        return frames;
    }

    DspFrame Skip(DspFrame frameCount) override
    {
        const DspFrame skipped = std::clamp<DspFrame>(readableLength - position, 0, frameCount);
        position += skipped;
        return skipped;
    }

    bool HasExactSeeks() const override
    {
        return hasExactSeeks;
    }

    bool Seek(DspFrame frame) override
    {
        position = hasExactSeeks || frame == 0 ? frame : frame + 1'000;
        return true;
    }

private:
    DspFrame length;
    DspFrame readableLength;
    bool hasExactSeeks;
    DspFrame position = 0;
};

MusicStream MakeStream(DspFrame length, const MusicStreamOptions& options = MANUAL, DspFrame readableLength = -1,
                       bool hasExactSeeks = true)
{
    return {std::make_unique<RampDecoder>(length, readableLength, hasExactSeeks), RATE, options};
}

// Plays up to maxFrames of what's ready, as the mixer does, returning the left channel.
std::vector<int16_t> Play(MusicStream& stream, DspFrame maxFrames = INT64_MAX)
{
    std::vector<int16_t> left;
    stream.Collect();
    while (static_cast<DspFrame>(left.size()) < maxFrames)
    {
        const auto wanted =
            static_cast<uint32_t>(std::min<DspFrame>(maxFrames - static_cast<DspFrame>(left.size()), BLOCK));
        const std::span<const int16_t> ready = stream.Peek(wanted);
        if (ready.empty())
        {
            break;
        }
        for (size_t i = 0; i < ready.size(); i += 2)
        {
            left.push_back(ready[i]);
        }
        stream.Consume(static_cast<uint32_t>(ready.size() / 2));
    }
    return left;
}

// Whether frames holds the ramp from `first` on.
bool IsRampFrom(const std::vector<int16_t>& frames, DspFrame first)
{
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i] != RampValue(first + static_cast<DspFrame>(i)))
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("MusicStream plays the whole file in order, then stops decoding")
{
    const DspFrame length = (3 * BLOCK) + 100;
    MusicStream stream = MakeStream(length);
    CHECK(stream.GetFrameCount() == length);
    CHECK(stream.Collect() == 0);

    CHECK(stream.DecodeBlocks() == 4);
    CHECK(stream.Collect() == length);
    const std::vector<int16_t> played = Play(stream);
    CHECK(static_cast<DspFrame>(played.size()) == length);
    CHECK(IsRampFrom(played, 0));
    CHECK(stream.DecodeBlocks() == 0);
}

TEST_CASE("MusicStream decodes ahead only as far as its blocks allow")
{
    MusicStream stream = MakeStream(100 * BLOCK);
    CHECK(stream.DecodeBlocks() == MusicStream::BLOCK_COUNT);
    CHECK(stream.DecodeBlocks() == 0);
    CHECK(stream.Collect() == MusicStream::BLOCK_COUNT * BLOCK);

    // Playing part of a block frees nothing; finishing it frees it for the next.
    CHECK(Play(stream, BLOCK / 2).size() == BLOCK / 2);
    CHECK(stream.DecodeBlocks() == 0);
    CHECK(Play(stream, BLOCK / 2).size() == BLOCK / 2);
    CHECK(stream.DecodeBlocks() == 1);

    const std::vector<int16_t> rest = Play(stream);
    CHECK(static_cast<DspFrame>(rest.size()) == MusicStream::BLOCK_COUNT * BLOCK);
    CHECK(IsRampFrom(rest, BLOCK));
}

TEST_CASE("MusicStream plays the lead-in as silence before the file")
{
    MusicStream stream = MakeStream(5'000, {.LeadInFrames = 3'000, .IsThreaded = false});
    CHECK(stream.GetFrameCount() == 8'000);
    stream.DecodeBlocks();
    const std::vector<int16_t> played = Play(stream);
    REQUIRE(played.size() == 8'000);
    CHECK(std::all_of(played.begin(), played.begin() + 3'000, [](int16_t sample) { return sample == 0; }));
    CHECK(IsRampFrom({played.begin() + 3'000, played.end()}, 0));
}

TEST_CASE("MusicStream drops blocks from before a seek once the audio thread applies it")
{
    MusicStream stream = MakeStream(100 * BLOCK);
    stream.DecodeBlocks();
    Play(stream, 500);

    stream.RequestSeek(50'000, 1);
    // Every block is still held, so nothing can be decoded for the new position yet, and until the
    // mixer applies the seek, the old position keeps playing.
    CHECK(stream.DecodeBlocks() == 0);
    CHECK(stream.Collect() == (MusicStream::BLOCK_COUNT * BLOCK) - 500);

    stream.SetReadEpoch(1);
    CHECK(stream.Collect() == 0);
    CHECK(Play(stream).empty());

    CHECK(stream.DecodeBlocks() == MusicStream::BLOCK_COUNT);
    const std::vector<int16_t> played = Play(stream, 3 * BLOCK);
    CHECK(played.size() == 3 * BLOCK);
    CHECK(IsRampFrom(played, 50'000));
}

TEST_CASE("MusicStream keeps blocks decoded after a seek until the seek is applied")
{
    MusicStream stream = MakeStream(100 * BLOCK);
    stream.DecodeBlocks();
    Play(stream, 2 * BLOCK);

    stream.RequestSeek(10'000, 1);
    CHECK(stream.DecodeBlocks() == 2);
    // The new blocks sit behind the old ones, which still play.
    CHECK(stream.Collect() == (MusicStream::BLOCK_COUNT - 2) * BLOCK);
    CHECK(IsRampFrom(Play(stream, BLOCK), 2 * BLOCK));

    stream.SetReadEpoch(1);
    CHECK(stream.Collect() == 2 * BLOCK);
    CHECK(IsRampFrom(Play(stream), 10'000));
}

TEST_CASE("MusicStream seeks into the lead-in and past the end")
{
    MusicStream stream = MakeStream(6'000, {.LeadInFrames = 4'000, .IsThreaded = false});

    stream.RequestSeek(1'000, 1);
    stream.SetReadEpoch(1);
    stream.DecodeBlocks();
    const std::vector<int16_t> played = Play(stream);
    REQUIRE(played.size() == 9'000);
    CHECK(std::all_of(played.begin(), played.begin() + 3'000, [](int16_t sample) { return sample == 0; }));
    CHECK(IsRampFrom({played.begin() + 3'000, played.end()}, 0));

    stream.RequestSeek(20'000, 2);
    stream.SetReadEpoch(2);
    CHECK(stream.DecodeBlocks() == 0);
    CHECK(stream.Collect() == 0);
}

TEST_CASE("MusicStream reaches positions by decoding when the decoder's seeks aren't exact")
{
    MusicStream stream = MakeStream(100 * BLOCK, {.LeadInFrames = 1'000, .IsThreaded = false}, -1, false);
    stream.DecodeBlocks();
    Play(stream, 3 * BLOCK);

    // Forward, from where the decoder is.
    stream.RequestSeek(150'000, 1);
    stream.SetReadEpoch(1);
    stream.Collect();
    stream.DecodeBlocks();
    CHECK(IsRampFrom(Play(stream, 3 * BLOCK), 150'000 - 1'000));

    // Backward, from the start.
    stream.RequestSeek(20'000, 2);
    stream.SetReadEpoch(2);
    stream.Collect();
    stream.DecodeBlocks();
    CHECK(IsRampFrom(Play(stream, 3 * BLOCK), 20'000 - 1'000));

    // Into the lead-in.
    stream.RequestSeek(400, 3);
    stream.SetReadEpoch(3);
    stream.Collect();
    stream.DecodeBlocks();
    const std::vector<int16_t> played = Play(stream, 3 * BLOCK);
    CHECK(std::all_of(played.begin(), played.begin() + 600, [](int16_t sample) { return sample == 0; }));
    CHECK(IsRampFrom({played.begin() + 600, played.end()}, 0));
}

TEST_CASE("MusicStream pads a file that ends early with silence")
{
    MusicStream stream = MakeStream(5'000, MANUAL, 3'000);
    stream.DecodeBlocks();
    const std::vector<int16_t> played = Play(stream);
    REQUIRE(played.size() == 5'000);
    CHECK(IsRampFrom({played.begin(), played.begin() + 3'000}, 0));
    CHECK(std::all_of(played.begin() + 3'000, played.end(), [](int16_t sample) { return sample == 0; }));
}

TEST_CASE("MusicStream decodes on its own thread while the audio thread plays and seeks")
{
    const DspFrame length = (80 * BLOCK) + 7;
    MusicStream stream = MakeStream(length, {});

    // Plays until `count` frames have played or the stream ends, like a device that never stops.
    const auto playFor = [&stream](DspFrame count)
    {
        std::vector<int16_t> played;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (static_cast<DspFrame>(played.size()) < count && std::chrono::steady_clock::now() < deadline)
        {
            const std::vector<int16_t> more = Play(stream, count - static_cast<DspFrame>(played.size()));
            played.insert(played.end(), more.begin(), more.end());
            if (more.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return played;
    };

    const std::vector<int16_t> start = playFor(40 * BLOCK);
    CHECK(static_cast<DspFrame>(start.size()) == 40 * BLOCK);
    CHECK(IsRampFrom(start, 0));

    // A seek, as the backend (RequestSeek) and then the mixer (SetReadEpoch) apply it.
    const DspFrame target = 20 * BLOCK;
    stream.RequestSeek(target, 1);
    stream.SetReadEpoch(1);
    const std::vector<int16_t> rest = playFor(length - target);
    CHECK(static_cast<DspFrame>(rest.size()) == length - target);
    CHECK(IsRampFrom(rest, target));
}

TEST_CASE("Mixer holds streamed music until it is decoded")
{
    MusicStream stream = MakeStream(10'000);
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Stream = &stream});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    std::vector<float> output(size_t{256} * PcmBuffer::CHANNELS);
    mixer.Render(output.data(), 256, 1'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
    CHECK(output[0] == 0.0f);

    stream.DecodeBlocks();
    mixer.Render(output.data(), 256, 2'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 1);
    CHECK(mixer.GetSnapshot().MusicFrame == 0);
    CHECK(output[0] == doctest::Approx(RampValue(0) / 32768.0f));
    CHECK(output[255 * 2] == doctest::Approx(RampValue(255) / 32768.0f));

    mixer.Render(output.data(), 256, 3'000);
    CHECK(mixer.GetSnapshot().MusicFrame == 256);
    CHECK(mixer.GetStats().MusicUnderruns == 0);
}

TEST_CASE("Mixer waits for streamed music after a seek, then plays from the target")
{
    MusicStream stream = MakeStream(100'000);
    stream.DecodeBlocks();
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Stream = &stream});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});
    std::vector<float> output(size_t{256} * PcmBuffer::CHANNELS);
    mixer.Render(output.data(), 256, 1'000);

    // As the backend seeks: the mixer's command, then the stream's request, with the same epoch.
    mixer.Submit({.Type = AudioCommandType::SeekMusic, .Frame = 70'000, .StreamEpoch = 1});
    stream.RequestSeek(70'000, 1);
    mixer.Render(output.data(), 256, 2'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
    CHECK(mixer.GetSnapshot().MusicFrame == 70'000);
    CHECK(output[0] == 0.0f);

    stream.DecodeBlocks();
    mixer.Render(output.data(), 256, 3'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 1);
    CHECK(mixer.GetSnapshot().MusicFrame == 70'000);
    CHECK(output[0] == doctest::Approx(RampValue(70'000) / 32768.0f));
    CHECK(mixer.GetStats().MusicUnderruns == 0);
}

TEST_CASE("Mixer holds streamed music that runs dry without skipping any of it")
{
    MusicStream stream = MakeStream(100 * BLOCK);
    stream.DecodeBlocks();
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Stream = &stream});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    // Play every decoded block (callbacks of 1000 frames don't line up with them), decoding none.
    const DspFrame decoded = MusicStream::BLOCK_COUNT * BLOCK;
    std::vector<float> output(size_t{1'000} * PcmBuffer::CHANNELS);
    hyoshi::HostTimeNs time = 0;
    while (mixer.GetSnapshot().MusicFrame < decoded - 1'000)
    {
        mixer.Render(output.data(), 1'000, time += 1'000);
    }
    mixer.Render(output.data(), 1'000, time += 1'000);
    mixer.Render(output.data(), 1'000, time += 1'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
    CHECK(mixer.GetSnapshot().MusicFrame == decoded);
    CHECK(mixer.GetStats().MusicUnderruns == 1);

    stream.DecodeBlocks();
    mixer.Render(output.data(), 1'000, time += 1'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 1);
    CHECK(mixer.GetSnapshot().MusicFrame == decoded);
    CHECK(output[0] == doctest::Approx(RampValue(decoded) / 32768.0f));
}

TEST_CASE("Mixer stops streamed music at its end")
{
    MusicStream stream = MakeStream(1'000);
    stream.DecodeBlocks();
    Mixer mixer(RATE, 4);
    mixer.Submit({.Type = AudioCommandType::SetMusic, .Stream = &stream});
    mixer.Submit({.Type = AudioCommandType::PlayMusic});

    std::vector<float> output(size_t{768} * PcmBuffer::CHANNELS);
    mixer.Render(output.data(), 768, 1'000);
    mixer.Render(output.data(), 768, 2'000);
    CHECK(output[(231 * 2)] == doctest::Approx(RampValue(999) / 32768.0f));
    CHECK(output[(232 * 2)] == 0.0f);
    mixer.Render(output.data(), 768, 3'000);
    CHECK(mixer.GetSnapshot().IsMusicPlaying == 0);
    CHECK(mixer.GetSnapshot().MusicFrame == 1'000);
    CHECK(mixer.GetStats().MusicUnderruns == 0);
}
