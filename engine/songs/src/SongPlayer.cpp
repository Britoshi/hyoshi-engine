#include "songs/SongPlayer.h"

#include "audio/AudioDecoder.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace hyoshi::songs
{

using hyoshi::DspFrame;
using hyoshi::SongTimeUs;

SongPlayer::SongPlayer(hyoshi::audio::IAudioBackend& audioBackend, hyoshi::JobSystem& jobSystem)
    : audio(audioBackend), jobs(jobSystem)
{
    clock.OutputLatencyUs = audio.GetEstimatedOutputLatencyUs();
}

void SongPlayer::Load(std::string songName, Loader loader, bool playWhenLoaded)
{
    name = std::move(songName);
    loadError.clear();
    shouldPlayWhenLoaded = playWhenLoaded;
    isLoaded = false;
    const uint32_t rate = audio.GetSampleRate();
    pendingMusic = jobs.Submit([loader = std::move(loader), rate] { return loader(rate); });
}

void SongPlayer::LoadFile(const std::string& path, bool playWhenLoaded)
{
    Load(
        path,
        [path](uint32_t rate) -> hyoshi::Result<hyoshi::audio::PcmBuffer>
        {
            hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
            if (!bytes)
            {
                return bytes.GetError();
            }
            return hyoshi::audio::DecodeAudio(bytes.Value(), rate);
        },
        playWhenLoaded);
}

void SongPlayer::Update(hyoshi::HostTimeNs now)
{
    wasJustLoaded = false;
    if (hyoshi::IsReady(pendingMusic))
    {
        hyoshi::Result<hyoshi::audio::PcmBuffer> music = pendingMusic.get();
        if (music)
        {
            OnLoaded(std::move(music).Value());
        }
        else
        {
            loadError = music.GetError().Message;
            HYOSHI_LOG_ERROR("Could not load {}: {}", name, loadError);
        }
    }

    if (audio.ConsumeRouteChangedFlag())
    {
        clock.OutputLatencyUs = audio.GetEstimatedOutputLatencyUs();
        isDiscontinuity = true;
        HYOSHI_LOG_WARN("Audio output changed; latency estimate now {:.1f} ms. Offsets may need recalibrating",
                        static_cast<double>(clock.OutputLatencyUs) / 1000.0);
    }

    snapshot = audio.GetClockSnapshot();
    clock.Update(snapshot, now);
    lastUpdateTime = now;

    const SongTimeUs songTime = clock.GetSongTime();
    // Until the audio thread applies a seek, the snapshot still describes the old position.
    const bool isSeekPending = snapshot.SeekGeneration < expectedGeneration;
    isContinuous = clock.IsPlaying() && wasPlaying && !isSeekPending && snapshot.SeekGeneration == lastSeekGeneration &&
                   !isDiscontinuity;
    if (isContinuous && songTime < lastSongTime)
    {
        ++backwardSteps;
        HYOSHI_LOG_WARN("Song time went backwards by {} us", lastSongTime - songTime);
    }
    wasPlaying = clock.IsPlaying();
    lastSeekGeneration = snapshot.SeekGeneration;
    lastSongTime = songTime;
    isDiscontinuity = false;
}

void SongPlayer::OnLoaded(hyoshi::audio::PcmBuffer music)
{
    const double seconds = static_cast<double>(music.GetFrameCount()) / music.SampleRate;
    hyoshi::Result<int64_t> generation = audio.SetMusic(std::move(music));
    if (!generation)
    {
        loadError = generation.GetError().Message;
        HYOSHI_LOG_ERROR("Could not start {}: {}", name, loadError);
        return;
    }

    isLoaded = true;
    wasJustLoaded = true;
    expectedGeneration = generation.Value();
    pendingCursorFrame = 0;
    isDiscontinuity = true;
    if (shouldPlayWhenLoaded)
    {
        audio.PlayMusic();
    }
    HYOSHI_LOG_INFO("Loaded {} ({:.1f} s)", name, seconds);
}

void SongPlayer::Play()
{
    audio.PlayMusic();
}

void SongPlayer::Pause()
{
    audio.PauseMusic();
}

bool SongPlayer::IsAtEnd() const
{
    return isLoaded && GetRenderCursor() >= audio.GetMusicFrameCount();
}

void SongPlayer::SeekTo(SongTimeUs target)
{
    const int64_t rate = audio.GetSampleRate();
    const DspFrame frame =
        std::clamp<DspFrame>(clock.MusicFrameForSongTime(target, rate), 0, audio.GetMusicFrameCount());
    const int64_t generation = audio.SeekMusic(frame);
    clock.ExpectSeek(generation, clock.SongTimeForMusicFrame(frame, rate));
    expectedGeneration = generation;
    pendingCursorFrame = frame;
    isDiscontinuity = true;
}

SongTimeUs SongPlayer::GetStartTime() const
{
    return clock.SongTimeForMusicFrame(0, audio.GetSampleRate());
}

SongTimeUs SongPlayer::GetEndTime() const
{
    return clock.SongTimeForMusicFrame(audio.GetMusicFrameCount(), audio.GetSampleRate());
}

DspFrame SongPlayer::GetRenderCursor() const
{
    return snapshot.SeekGeneration < expectedGeneration ? pendingCursorFrame : snapshot.MusicFrame;
}

void SongPlayer::OnSuspend()
{
    audio.PauseMusic();
    audio.Suspend();
}

void SongPlayer::OnResume()
{
    audio.Resume();
}

} // namespace hyoshi::songs
