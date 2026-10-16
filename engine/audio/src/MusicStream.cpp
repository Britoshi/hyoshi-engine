#include "audio/MusicStream.h"

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace hyoshi::audio
{

namespace
{

// How often the decoder thread looks for blocks the audio thread has finished with. About 1.4 s is
// buffered, so this is plenty. Right after a seek the music waits for the first block, so it looks
// more often until that block is decoded.
constexpr auto REFILL_INTERVAL = std::chrono::milliseconds(10);
constexpr auto SEEK_REFILL_INTERVAL = std::chrono::milliseconds(1);
// Frames skipped per step while seeking by decoding: a few milliseconds of work.
constexpr DspFrame SKIP_STEP_FRAMES = 32'768;

} // namespace

Result<std::unique_ptr<MusicStream>> MusicStream::Open(std::vector<std::byte> encoded, uint32_t sampleRate,
                                                       const MusicStreamOptions& options)
{
    Result<std::unique_ptr<IStreamDecoder>> opened = OpenStreamDecoder(std::move(encoded), sampleRate);
    if (!opened)
    {
        return opened.GetError();
    }
    return std::make_unique<MusicStream>(std::move(opened).Value(), sampleRate, options);
}

MusicStream::MusicStream(std::unique_ptr<IStreamDecoder> streamDecoder, uint32_t rate,
                         const MusicStreamOptions& options)
    : decoder(std::move(streamDecoder)), sampleRate(rate), leadInFrames(std::max<DspFrame>(options.LeadInFrames, 0)),
      frameCount(leadInFrames + decoder->GetFrameCount()),
      samples(size_t{BLOCK_COUNT} * BLOCK_FRAMES * PcmBuffer::CHANNELS), decodedBlocks(BLOCK_COUNT),
      freeBlocks(BLOCK_COUNT), isWaitingForSeek(options.IsWaitingForSeek)
{
    for (uint32_t block = 0; block < BLOCK_COUNT; ++block)
    {
        freeBlocks.Push(block);
    }
    if (options.IsThreaded)
    {
        thread = std::thread([this] { RunDecoderThread(); });
    }
}

MusicStream::~MusicStream()
{
    if (thread.joinable())
    {
        {
            const std::lock_guard lock(requestMutex);
            isStopping.store(true, std::memory_order_relaxed);
        }
        wakeDecoder.notify_one();
        thread.join();
    }
}

void MusicStream::RequestSeek(DspFrame frame, int64_t epoch)
{
    {
        const std::lock_guard lock(requestMutex);
        requestedFrame = std::clamp<DspFrame>(frame, 0, frameCount);
        requestedEpoch = epoch;
        hasSeekRequest.store(true, std::memory_order_release);
    }
    wakeDecoder.notify_one();
}

uint32_t MusicStream::DecodeBlocks()
{
    uint32_t decoded = 0;
    // Stopping checks between blocks, so destroying a stream never waits for a whole refill.
    while (!isStopping.load(std::memory_order_relaxed))
    {
        ApplySeekRequest();
        if (isWaitingForSeek || decodeFrame >= frameCount)
        {
            break;
        }
        if (framesToSkip > 0)
        {
            const DspFrame skipped = decoder->Skip(std::min(framesToSkip, SKIP_STEP_FRAMES));
            decoderPosition += skipped;
            framesToSkip = skipped > 0 ? framesToSkip - skipped : 0;
            continue;
        }
        const std::optional<uint32_t> block = freeBlocks.Pop();
        if (!block)
        {
            break;
        }
        const auto frames = static_cast<uint32_t>(std::min<DspFrame>(BLOCK_FRAMES, frameCount - decodeFrame));
        DecodeInto(*block, frames);
        blocks[*block] = {.Epoch = decodeEpoch, .FrameCount = frames};
        // Never full: there are only BLOCK_COUNT blocks.
        decodedBlocks.Push(*block);
        decodeFrame += frames;
        hasDecodedForEpoch = true;
        ++decoded;
    }
    return decoded;
}

void MusicStream::ApplySeekRequest()
{
    if (!hasSeekRequest.load(std::memory_order_acquire))
    {
        return;
    }
    {
        const std::lock_guard lock(requestMutex);
        decodeFrame = requestedFrame;
        decodeEpoch = requestedEpoch;
        hasSeekRequest.store(false, std::memory_order_relaxed);
    }
    hasDecodedForEpoch = false;
    isWaitingForSeek = false;
    framesToSkip = 0;
    if (decodeFrame >= frameCount)
    {
        // Nothing left to decode.
        return;
    }

    // Inside the lead-in, the decoder starts from its beginning once the silence is done.
    const DspFrame target = std::max<DspFrame>(decodeFrame - leadInFrames, 0);
    if (decoder->HasExactSeeks())
    {
        isSilent = !decoder->Seek(target);
        decoderPosition = target;
    }
    else
    {
        // Skip forward from here, or from the start (Seek(0) is always exact).
        isSilent = false;
        if (target < decoderPosition)
        {
            isSilent = !decoder->Seek(0);
            decoderPosition = 0;
        }
        framesToSkip = target - decoderPosition;
    }
    if (isSilent)
    {
        HYOSHI_LOG_WARN("Music: could not seek to {:.3f} s; playing silence",
                        static_cast<double>(decodeFrame) / sampleRate);
    }
}

void MusicStream::DecodeInto(uint32_t block, uint32_t frames)
{
    int16_t* out = samples.data() + (size_t{block} * BLOCK_FRAMES * PcmBuffer::CHANNELS);
    const auto silentFrames = static_cast<uint32_t>(std::clamp<DspFrame>(leadInFrames - decodeFrame, 0, frames));
    uint32_t written = silentFrames;
    std::fill_n(out, size_t{silentFrames} * PcmBuffer::CHANNELS, int16_t{0});

    if (written < frames && !isSilent)
    {
        const uint32_t read = decoder->Read(out + (size_t{written} * PcmBuffer::CHANNELS), frames - written);
        decoderPosition += read;
        written += read;
        if (written < frames)
        {
            // The file ended early or is corrupt. The length is fixed, so the rest is silence. A
            // few frames short is only resampling rounding.
            isSilent = true;
            const DspFrame missing = frameCount - decodeFrame - written;
            if (missing > sampleRate / 10)
            {
                HYOSHI_LOG_WARN("Music: decoding stopped {:.1f} s before the end; the rest is silence",
                                static_cast<double>(missing) / sampleRate);
            }
        }
    }
    std::fill(out + (size_t{written} * PcmBuffer::CHANNELS), out + (size_t{frames} * PcmBuffer::CHANNELS), int16_t{0});
}

void MusicStream::RunDecoderThread()
{
    std::unique_lock lock(requestMutex);
    while (!isStopping.load(std::memory_order_relaxed))
    {
        lock.unlock();
        DecodeBlocks();
        lock.lock();
        const auto isWoken = [this]
        { return isStopping.load(std::memory_order_relaxed) || hasSeekRequest.load(std::memory_order_relaxed); };
        if (isWaitingForSeek || decodeFrame >= frameCount)
        {
            // At the end, or not started: nothing to do until a seek.
            wakeDecoder.wait(lock, isWoken);
        }
        else
        {
            // Waiting for the audio thread to finish with blocks.
            wakeDecoder.wait_for(lock, hasDecodedForEpoch ? REFILL_INTERVAL : SEEK_REFILL_INTERVAL, isWoken);
        }
    }
}

void MusicStream::SetReadEpoch(int64_t epoch)
{
    readEpoch = epoch;
}

DspFrame MusicStream::Collect()
{
    while (const std::optional<uint32_t> block = decodedBlocks.Pop())
    {
        heldBlocks[(heldStart + heldCount) % BLOCK_COUNT] = *block;
        ++heldCount;
    }

    // Blocks decoded before the latest seek. Epochs only grow, so they're all at the front.
    while (heldCount > 0 && blocks[heldBlocks[heldStart]].Epoch < readEpoch)
    {
        ReleaseFront();
    }

    DspFrame ready = 0;
    for (uint32_t i = 0; i < heldCount; ++i)
    {
        const BlockInfo& info = blocks[heldBlocks[(heldStart + i) % BLOCK_COUNT]];
        if (info.Epoch != readEpoch)
        {
            break;
        }
        ready += info.FrameCount;
    }
    // Only a block of the read epoch is ever partly played.
    return ready > 0 ? ready - readOffset : 0;
}

std::span<const int16_t> MusicStream::Peek(uint32_t maxFrames) const
{
    if (heldCount == 0)
    {
        return {};
    }
    const uint32_t block = heldBlocks[heldStart];
    const BlockInfo& info = blocks[block];
    if (info.Epoch != readEpoch)
    {
        return {};
    }
    const uint32_t frames = std::min(maxFrames, info.FrameCount - readOffset);
    const int16_t* start = samples.data() + (((size_t{block} * BLOCK_FRAMES) + readOffset) * PcmBuffer::CHANNELS);
    return {start, size_t{frames} * PcmBuffer::CHANNELS};
}

void MusicStream::Consume(uint32_t frames)
{
    readOffset += frames;
    if (heldCount > 0 && readOffset >= blocks[heldBlocks[heldStart]].FrameCount)
    {
        ReleaseFront();
    }
}

void MusicStream::ReleaseFront()
{
    // Never full: there are only BLOCK_COUNT blocks.
    freeBlocks.Push(heldBlocks[heldStart]);
    heldStart = (heldStart + 1) % BLOCK_COUNT;
    --heldCount;
    readOffset = 0;
}

} // namespace hyoshi::audio
