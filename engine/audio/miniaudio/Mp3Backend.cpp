// miniaudio's implementation is compiled here, with stb_vorbis's declarations so it decodes Ogg
// Vorbis (stb_vorbis's implementation is in ThirdPartyImplementations.cpp): the backend below uses
// dr_mp3, which miniaudio declares only inside its implementation.
// NOLINTBEGIN
#if defined(_MSC_VER)
// Code generator warnings escape /external:W0 (see ThirdPartyImplementations.cpp), and miniaudio was
// compiled with this one off there.
#pragma warning(disable : 4701)
#endif
// Optimized in debug builds too (the decoding is what's slow unoptimized, and nobody steps through
// it). Here for MSVC, which needs its runtime checks off for it; other compilers get -O2 for this
// file (CMakeLists.txt).
#if defined(_MSC_VER) && !defined(__clang__) && !defined(NDEBUG)
#pragma runtime_checks("", off)
#pragma optimize("gt", on)
#endif

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
// NOLINTEND

#include "Mp3Backend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace hyoshi::audio
{

namespace
{

// A seek decodes this many frames before the one it lands in, to refill the bit reservoir: a frame's
// data can start up to 511 bytes back, at most 6 MPEG-1 frames at the lowest bitrate, or 11 MPEG-2
// ones. The last few are decoded in full to settle the filters, which takes one MPEG-1 frame (two
// granules) or two MPEG-2 ones.
constexpr size_t LEAD_FRAMES = 16;
constexpr size_t FILTER_LEAD_FRAMES = 2;

// Where an MP3 frame starts in the file, and the first sample frame it decodes to, counted as dr_mp3
// counts (currentPCMFrame): from the stream's start, the encoder delay included.
struct Mp3FrameStart
{
    size_t Offset = 0;
    uint64_t PcmFrame = 0;
};

// miniaudio's MP3 decoder with its seek and cursor replaced. A seek through its seek table starts two
// frames back, cold, and the first of those frames usually lacks its bit reservoir, so it decodes to
// nothing and the seek lands one or two frames (26 ms each) late. Any seek but to the start also takes
// the target to count the encoder delay, which reads leave out, landing that much (often 25 ms) early,
// and the cursor counts it too. Mp3 comes first: its data source is this one's.
struct ExactMp3
{
    ma_mp3 Mp3{};
    // Every frame decoding from the start decodes, in order; listed at the first seek.
    std::vector<Mp3FrameStart> Frames;
    bool IsIndexed = false;
};

// Lists the frames as dr_mp3 finds them decoding from the stream's start. A frame whose reservoir is
// missing decodes to nothing there and is left out here. Only headers are read and the reservoir kept,
// with no samples decoded, so it's quick. It uses dr_mp3's own decoder state (23 KB, too big for a
// small thread stack), which every seek resets after.
void IndexFrames(ExactMp3& mp3)
{
    mp3.IsIndexed = true;
    ma_dr_mp3& dr = mp3.Mp3.dr;
    const auto* data = static_cast<const ma_uint8*>(dr.memory.pData);
    const size_t size = dr.memory.dataSize;
    ma_dr_mp3dec_init(&dr.decoder);
    uint64_t pcmFrame = 0;
    for (auto offset = static_cast<size_t>(dr.streamStartOffset); offset < size;)
    {
        ma_dr_mp3dec_frame_info info{};
        const int frames =
            ma_dr_mp3dec_decode_frame(&dr.decoder, data + offset, static_cast<int>(size - offset), nullptr, &info);
        if (info.frame_bytes <= 0)
        {
            break;
        }
        if (frames > 0)
        {
            mp3.Frames.push_back({.Offset = offset, .PcmFrame = pcmFrame});
            pcmFrame += static_cast<uint64_t>(frames);
        }
        offset += static_cast<size_t>(info.frame_bytes);
    }
}

// dr_mp3 rewinds and decodes its way there: exact, but slow far into a song.
ma_result SeekFromStart(ma_dr_mp3& dr, uint64_t frame)
{
    if (ma_dr_mp3_seek_to_pcm_frame(&dr, 0) != MA_TRUE)
    {
        return MA_ERROR;
    }
    return frame == 0 || ma_dr_mp3_seek_forward_by_pcm_frames__brute_force(&dr, frame) == MA_TRUE ? MA_SUCCESS
                                                                                                  : MA_ERROR;
}

// `frame` leaves out the encoder delay, as reads do. The frame holding it is looked up, the frames
// before it decoded to rebuild the decoder's state, and the rest of the way decoded within it. Near
// the start, this decodes from the start.
ma_result SeekExactly(ma_data_source* source, ma_uint64 frame)
{
    auto& mp3 = *static_cast<ExactMp3*>(source);
    ma_dr_mp3& dr = mp3.Mp3.dr;
    if (!mp3.IsIndexed)
    {
        IndexFrames(mp3);
    }
    const uint64_t target = frame + dr.delayInPCMFrames;
    const auto after =
        std::upper_bound(mp3.Frames.begin(), mp3.Frames.end(), target,
                         [](uint64_t pcmFrame, const Mp3FrameStart& start) { return pcmFrame < start.PcmFrame; });
    const auto found = static_cast<size_t>(after - mp3.Frames.begin());
    if (found <= LEAD_FRAMES || mp3.Frames[found - 1].PcmFrame < dr.delayInPCMFrames)
    {
        return SeekFromStart(dr, frame);
    }
    const Mp3FrameStart& landing = mp3.Frames[found - 1];
    const size_t decodeFrom = mp3.Frames[found - 1 - FILTER_LEAD_FRAMES].Offset;

    // Decoded as from the start: frame by frame, frames that decode to nothing included.
    const auto* data = static_cast<const ma_uint8*>(dr.memory.pData);
    ma_dr_mp3dec_init(&dr.decoder);
    size_t offset = mp3.Frames[found - 1 - LEAD_FRAMES].Offset;
    while (offset < landing.Offset)
    {
        ma_dr_mp3dec_frame_info info{};
        void* samples = offset >= decodeFrom ? dr.pcmFrames : nullptr;
        ma_dr_mp3dec_decode_frame(&dr.decoder, data + offset, static_cast<int>(dr.memory.dataSize - offset), samples,
                                  &info);
        if (info.frame_bytes <= 0)
        {
            return SeekFromStart(dr, frame);
        }
        offset += static_cast<size_t>(info.frame_bytes);
    }
    if (offset != landing.Offset)
    {
        return SeekFromStart(dr, frame);
    }

    // The landing frame decodes at the next read, as dr_mp3's own seeks leave it.
    dr.memory.currentReadPos = offset;
    dr.streamCursor = offset;
    dr.pcmFramesConsumedInMP3Frame = 0;
    dr.pcmFramesRemainingInMP3Frame = 0;
    dr.currentPCMFrame = landing.PcmFrame;
    dr.atEnd = MA_FALSE;
    const uint64_t rest = target - landing.PcmFrame;
    return rest == 0 || ma_dr_mp3_seek_forward_by_pcm_frames__brute_force(&dr, rest) == MA_TRUE ? MA_SUCCESS : MA_ERROR;
}

ma_result Read(ma_data_source* source, void* frames, ma_uint64 frameCount, ma_uint64* framesRead)
{
    return ma_mp3_read_pcm_frames(&static_cast<ExactMp3*>(source)->Mp3, frames, frameCount, framesRead);
}

ma_result GetDataFormat(ma_data_source* source, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate,
                        ma_channel* channelMap, size_t channelMapCap)
{
    return ma_mp3_get_data_format(&static_cast<ExactMp3*>(source)->Mp3, format, channels, sampleRate, channelMap,
                                  channelMapCap);
}

// The next frame a read gives, leaving out the encoder delay as reads do.
ma_result GetCursor(ma_data_source* source, ma_uint64* cursor)
{
    const ma_dr_mp3& dr = static_cast<ExactMp3*>(source)->Mp3.dr;
    *cursor = dr.currentPCMFrame > dr.delayInPCMFrames ? dr.currentPCMFrame - dr.delayInPCMFrames : 0;
    return MA_SUCCESS;
}

ma_result GetLength(ma_data_source* source, ma_uint64* length)
{
    return ma_mp3_get_length_in_pcm_frames(&static_cast<ExactMp3*>(source)->Mp3, length);
}

constexpr ma_data_source_vtable EXACT_MP3_SOURCE{
    .onRead = Read,
    .onSeek = SeekExactly,
    .onGetDataFormat = GetDataFormat,
    .onGetCursor = GetCursor,
    .onGetLength = GetLength,
    .onSetLooping = nullptr,
    .flags = 0,
};

ma_result InitMemory(void* /*userData*/, const void* data, size_t dataSize, const ma_decoding_backend_config* config,
                     const ma_allocation_callbacks* allocation, ma_data_source** backend)
{
    std::unique_ptr<ExactMp3> mp3(new (std::nothrow) ExactMp3());
    if (mp3 == nullptr)
    {
        return MA_OUT_OF_MEMORY;
    }
    if (const ma_result result = ma_mp3_init_memory(data, dataSize, config, allocation, &mp3->Mp3);
        result != MA_SUCCESS)
    {
        return result;
    }
    mp3->Mp3.ds.vtable = &EXACT_MP3_SOURCE;
    *backend = mp3.release();
    return MA_SUCCESS;
}

void Uninit(void* /*userData*/, ma_data_source* backend, const ma_allocation_callbacks* allocation)
{
    const std::unique_ptr<ExactMp3> mp3(static_cast<ExactMp3*>(backend));
    ma_mp3_uninit(&mp3->Mp3, allocation);
}

} // namespace

ma_decoding_backend_vtable* GetExactMp3Backend()
{
    // Memory only: the engine decodes files it has read.
    static ma_decoding_backend_vtable backend{
        .onInit = nullptr,
        .onInitFile = nullptr,
        .onInitFileW = nullptr,
        .onInitMemory = InitMemory,
        .onUninit = Uninit,
    };
    return &backend;
}

} // namespace hyoshi::audio
