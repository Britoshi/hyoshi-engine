#pragma once

#include "audio/AudioDecoder.h"
#include "audio/AudioTypes.h"

#include "core/Result.h"
#include "core/SpscQueue.h"
#include "core/Time.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace hyoshi::audio
{

struct MusicStreamOptions
{
    // Silence before the decoded audio, as part of the music (a lead-in before the first note).
    DspFrame LeadInFrames = 0;
    // Decode on the stream's own thread. Tests turn this off and call DecodeBlocks themselves.
    bool IsThreaded = true;
    // Decode nothing until the first seek (RequestSeek), for music started elsewhere than its
    // beginning (a preview): its first blocks are then where it starts, every block free for them,
    // instead of waiting for the audio thread to drop blocks from the beginning first.
    bool IsWaitingForSeek = false;
};

// Music decoded a little ahead of playback instead of all at once, so it starts in milliseconds and
// never holds the whole song in memory. Games stream music and preload short sounds; this is the
// streaming half.
//
// A decoder thread keeps about 1.4 s decoded ahead in fixed blocks, which move to the audio thread
// and back through lock-free queues, so the audio thread never waits. Each seek starts a new epoch:
// blocks are tagged with the epoch they were decoded for, and the audio thread drops blocks from
// before the latest seek it has applied. Until blocks for the current epoch arrive there is nothing
// to play, and the mixer holds the music where it is rather than skip ahead.
//
// A decoder without exact seeks (MP3) reaches a position by decoding up to it from where it is, or
// from the start, which takes up to a few hundred milliseconds; it's done in steps, so a newer seek
// or destroying the stream never waits for it.
//
// Threads: the main thread (through the audio backend) calls RequestSeek; the decoder thread calls
// DecodeBlocks; the audio thread (the Mixer) calls SetReadEpoch, Collect, Peek, and Consume.
class MusicStream
{
public:
    static constexpr uint32_t BLOCK_FRAMES = 2048;
    static constexpr uint32_t BLOCK_COUNT = 32;

    // Opens WAV, MP3, FLAC, or Ogg Vorbis bytes and starts decoding from the beginning.
    static Result<std::unique_ptr<MusicStream>> Open(std::vector<std::byte> encoded, uint32_t sampleRate,
                                                     const MusicStreamOptions& options = {});

    MusicStream(std::unique_ptr<IStreamDecoder> decoder, uint32_t sampleRate, const MusicStreamOptions& options = {});
    ~MusicStream();

    MusicStream(const MusicStream&) = delete;
    MusicStream& operator=(const MusicStream&) = delete;

    // Any thread. The length, lead-in included.
    DspFrame GetFrameCount() const
    {
        return frameCount;
    }

    uint32_t GetSampleRate() const
    {
        return sampleRate;
    }

    // Main thread: decode from frame onward, as blocks of this epoch. Epochs start at 0 and must
    // increase with each seek.
    void RequestSeek(DspFrame frame, int64_t epoch);

    // Decoder thread, or the test calling it. Decodes blocks while there are free ones and returns
    // how many it decoded.
    uint32_t DecodeBlocks();

    // Audio thread: play blocks of this epoch from now on (the mixer applied a seek).
    void SetReadEpoch(int64_t epoch);
    // Audio thread: takes newly decoded blocks, drops those from before the latest seek, and returns
    // the frames ready to play.
    DspFrame Collect();
    // Audio thread: the next ready samples (interleaved stereo), at most maxFrames frames; empty if
    // nothing is ready.
    std::span<const int16_t> Peek(uint32_t maxFrames) const;
    // Audio thread: marks frames returned by Peek as played.
    void Consume(uint32_t frames);

private:
    struct BlockInfo
    {
        int64_t Epoch = 0;
        uint32_t FrameCount = 0;
    };

    void RunDecoderThread();
    // Decoder thread.
    void ApplySeekRequest();
    void DecodeInto(uint32_t block, uint32_t frames);
    // Audio thread.
    void ReleaseFront();

    const std::unique_ptr<IStreamDecoder> decoder;
    const uint32_t sampleRate;
    const DspFrame leadInFrames;
    const DspFrame frameCount;

    std::vector<int16_t> samples;
    std::array<BlockInfo, BLOCK_COUNT> blocks{};
    // Block indices: decoded, from the decoder to the audio thread, and free, back again.
    SpscQueue<uint32_t> decodedBlocks;
    SpscQueue<uint32_t> freeBlocks;

    // Seek requests, main thread to decoder thread.
    std::mutex requestMutex;
    std::condition_variable wakeDecoder;
    DspFrame requestedFrame = 0;
    int64_t requestedEpoch = 0;
    std::atomic<bool> hasSeekRequest{false};
    std::atomic<bool> isStopping{false};

    // Decoder thread state. The decoder's position doesn't count the lead-in; after a seek without
    // exact seeks, it's behind decodeFrame by the frames still to skip.
    DspFrame decodeFrame = 0;
    int64_t decodeEpoch = 0;
    DspFrame decoderPosition = 0;
    DspFrame framesToSkip = 0;
    bool hasDecodedForEpoch = false;
    // Nothing is decoded until a seek (MusicStreamOptions::IsWaitingForSeek).
    bool isWaitingForSeek = false;
    // After a failed seek or read, until the next seek: blocks are silence.
    bool isSilent = false;

    // Audio thread state: the blocks it holds, oldest first, and how far into the first it has
    // played.
    std::array<uint32_t, BLOCK_COUNT> heldBlocks{};
    uint32_t heldStart = 0;
    uint32_t heldCount = 0;
    uint32_t readOffset = 0;
    int64_t readEpoch = 0;

    std::thread thread;
};

} // namespace hyoshi::audio
