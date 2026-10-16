#include "songs/SongPlayer.h"

#include "audio/MusicStream.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
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
    loadStart = std::chrono::steady_clock::now();
    isAwaitingFirstPlay = true;
    pendingStartUs.reset();
    pendingStream = {};
    const uint32_t rate = audio.GetSampleRate();
    pendingMusic = jobs.Submit([loader = std::move(loader), rate] { return loader(rate); });
}

void SongPlayer::LoadFile(const std::string& path, bool playWhenLoaded, SongTimeUs leadInUs,
                          std::optional<SongTimeUs> startUs)
{
    name = path;
    loadError.clear();
    shouldPlayWhenLoaded = playWhenLoaded;
    isLoaded = false;
    loadStart = std::chrono::steady_clock::now();
    isAwaitingFirstPlay = true;
    pendingMusic = {};
    const uint32_t rate = audio.GetSampleRate();
    pendingStartUs = startUs;
    const hyoshi::audio::MusicStreamOptions options{.LeadInFrames = hyoshi::UsToFrames(leadInUs, rate),
                                                    .IsWaitingForSeek = startUs.has_value()};
    pendingStream = jobs.Submit(
        [path, rate, options]() -> hyoshi::Result<std::unique_ptr<hyoshi::audio::MusicStream>>
        {
            hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
            if (!bytes)
            {
                return bytes.GetError();
            }
            return hyoshi::audio::MusicStream::Open(std::move(bytes).Value(), rate, options);
        });
}

void SongPlayer::Update(hyoshi::HostTimeNs now)
{
    wasJustLoaded = false;
    if (hyoshi::IsReady(pendingMusic))
    {
        hyoshi::Result<hyoshi::audio::PcmBuffer> music = pendingMusic.get();
        if (music)
        {
            const DspFrame frames = music.Value().GetFrameCount();
            const uint32_t rate = music.Value().SampleRate;
            OnLoaded(audio.SetMusic(std::move(music).Value()), frames, rate);
        }
        else
        {
            loadError = music.GetError().Message;
            HYOSHI_LOG_ERROR("Could not load {}: {}", name, loadError);
        }
    }
    if (hyoshi::IsReady(pendingStream))
    {
        hyoshi::Result<std::unique_ptr<hyoshi::audio::MusicStream>> stream = pendingStream.get();
        if (stream)
        {
            const DspFrame frames = stream.Value()->GetFrameCount();
            const uint32_t rate = stream.Value()->GetSampleRate();
            OnLoaded(audio.SetMusicStream(std::move(stream).Value()), frames, rate);
        }
        else
        {
            loadError = stream.GetError().Message;
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
    // What "Loaded" leaves out: a seek to where it's to start, its first blocks decoded, and the
    // output's callbacks till one plays it.
    if (isAwaitingFirstPlay && isLoaded && snapshot.SeekGeneration >= expectedGeneration &&
        snapshot.IsMusicPlaying != 0)
    {
        isAwaitingFirstPlay = false;
        const std::chrono::duration<double, std::milli> waited = std::chrono::steady_clock::now() - loadStart;
        HYOSHI_LOG_INFO("Playing {} {:.0f} ms after it was asked for (heard ~{:.0f} ms after that)", name,
                        waited.count(), static_cast<double>(audio.GetEstimatedOutputLatencyUs()) / 1000.0);
    }

    const SongTimeUs songTime = clock.GetSongTime();
    // Until the audio thread applies a seek, the snapshot still describes the old position.
    const bool isSeekPending = snapshot.SeekGeneration < expectedGeneration;
    isContinuous = clock.IsPlaying() && wasPlaying && !isSeekPending && snapshot.SeekGeneration == lastSeekGeneration &&
                   snapshot.MusicRate == lastMusicRate && !isDiscontinuity;
    if (isContinuous && songTime < lastSongTime)
    {
        ++backwardSteps;
        HYOSHI_LOG_WARN("Song time went backwards by {} us", lastSongTime - songTime);
    }
    wasPlaying = clock.IsPlaying();
    lastSeekGeneration = snapshot.SeekGeneration;
    lastMusicRate = snapshot.MusicRate;
    lastSongTime = songTime;
    isDiscontinuity = false;
}

void SongPlayer::OnLoaded(hyoshi::Result<int64_t> generation, DspFrame frameCount, uint32_t sampleRate)
{
    const double seconds = static_cast<double>(frameCount) / sampleRate;
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
    // New music starts at its first frame once the audio thread has it. Until then the snapshots
    // describe the music before it, which may still be playing (a menu's preview, behind a song
    // loading), so the clock holds at the start rather than follow it.
    clock.ExpectSeek(expectedGeneration, clock.SongTimeForMusicFrame(0, audio.GetSampleRate()));
    // New music plays at 1.
    if (musicRate != 1.0)
    {
        audio.SetMusicRate(musicRate);
    }
    // Music loaded to start elsewhere decodes nothing till it's told where.
    if (pendingStartUs)
    {
        SeekTo(*pendingStartUs);
        pendingStartUs.reset();
    }
    if (shouldPlayWhenLoaded)
    {
        audio.PlayMusic();
    }
    const std::chrono::duration<double, std::milli> loadTime = std::chrono::steady_clock::now() - loadStart;
    HYOSHI_LOG_INFO("Loaded {} ({:.1f} s) in {:.0f} ms", name, seconds, loadTime.count());
}

void SongPlayer::Play()
{
    audio.PlayMusic();
}

void SongPlayer::Pause()
{
    audio.PauseMusic();
}

void SongPlayer::SetMusicRate(double rate)
{
    musicRate = rate;
    audio.SetMusicRate(rate);
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
