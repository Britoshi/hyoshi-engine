#pragma once

#include "audio/IAudioBackend.h"
#include "audio/MusicStream.h"
#include "core/JobSystem.h"
#include "core/Result.h"
#include "rhythm/SongClock.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace hyoshi::songs
{

// Loads a song on a worker and plays it through the audio backend, keeping a SongClock in step
// with it: seeks, route changes, and a check that song time never runs backwards during playback.
class SongPlayer
{
public:
    using Loader = std::function<hyoshi::Result<hyoshi::audio::PcmBuffer>(uint32_t sampleRate)>;

    SongPlayer(hyoshi::audio::IAudioBackend& audio, hyoshi::JobSystem& jobs);

    // Runs the loader on a worker and plays the PCM it returns, for music made in memory (the
    // metronome track). The music plays as soon as it's ready if playWhenLoaded.
    void Load(std::string name, Loader loader, bool playWhenLoaded = true);
    // Streams an audio file: it's ready as soon as the file is read and its headers parsed, and
    // it's decoded while it plays (MusicStream). leadInUs of silence come before it, as part of
    // the music. startUs: where it starts once loaded (none: its beginning), nothing decoded before
    // it, so a preview from a song's middle is heard as soon as one from its start.
    void LoadFile(const std::string& path, bool playWhenLoaded = true, hyoshi::SongTimeUs leadInUs = 0,
                  std::optional<hyoshi::SongTimeUs> startUs = std::nullopt);

    // Once per frame: finishes loading, follows route changes, updates the clock.
    void Update(hyoshi::HostTimeNs now);

    bool IsLoaded() const
    {
        return isLoaded;
    }

    // True during the Update in which the music was handed to the audio thread.
    bool WasJustLoaded() const
    {
        return wasJustLoaded;
    }

    const std::string& GetName() const
    {
        return name;
    }

    const std::string& GetLoadError() const
    {
        return loadError;
    }

    void Play();
    void Pause();
    // Plays faster or slower, pitch and all (IAudioBackend::SetMusicRate), music loaded later too.
    void SetMusicRate(double rate);

    // Whether the music was advancing at the last callback.
    bool IsPlaying() const
    {
        return snapshot.IsMusicPlaying != 0;
    }

    // Past the last frame of the music.
    bool IsAtEnd() const;

    // Seeks, clamped to the music. The clock shows the target from this frame on.
    void SeekTo(hyoshi::SongTimeUs target);

    // Song times of the first and last music frames.
    hyoshi::SongTimeUs GetStartTime() const;
    hyoshi::SongTimeUs GetEndTime() const;

    hyoshi::rhythm::SongClock& GetClock()
    {
        return clock;
    }

    const hyoshi::rhythm::SongClock& GetClock() const
    {
        return clock;
    }

    const hyoshi::audio::AudioClockSnapshot& GetSnapshot() const
    {
        return snapshot;
    }

    hyoshi::HostTimeNs GetLastUpdateTime() const
    {
        return lastUpdateTime;
    }

    // The next music frame the audio thread will render, counting a seek or music change it
    // hasn't applied yet. Sounds scheduled at or after it will play.
    hyoshi::DspFrame GetRenderCursor() const;

    // This frame continues the last one's timeline: playing then and now, no seek (applied or
    // pending), no jump.
    bool IsContinuous() const
    {
        return isContinuous;
    }

    // Call after changing clock offsets: song time jumps, which is not a rewind.
    void MarkDiscontinuity()
    {
        isDiscontinuity = true;
    }

    // Frames where song time went backwards during continuous playback. Must stay 0.
    uint32_t GetBackwardSteps() const
    {
        return backwardSteps;
    }

    // App lifecycle: the music pauses in the background and stays paused on return.
    void OnSuspend();
    void OnResume();

private:
    void OnLoaded(hyoshi::Result<int64_t> generation, hyoshi::DspFrame frameCount, uint32_t sampleRate);

    hyoshi::audio::IAudioBackend& audio;
    hyoshi::JobSystem& jobs;
    hyoshi::rhythm::SongClock clock;
    // At most one is pending: a new load replaces the last.
    std::future<hyoshi::Result<hyoshi::audio::PcmBuffer>> pendingMusic;
    std::future<hyoshi::Result<std::unique_ptr<hyoshi::audio::MusicStream>>> pendingStream;

    std::string name;
    std::string loadError;
    // When the load began, for the log.
    std::chrono::steady_clock::time_point loadStart;
    bool shouldPlayWhenLoaded = true;
    // Where the music loading starts (LoadFile's startUs).
    std::optional<hyoshi::SongTimeUs> pendingStartUs;
    bool isLoaded = false;
    bool wasJustLoaded = false;
    // Loaded (or loading) music not yet played, for the log: its first callback that plays it, after
    // any seek, is when it's heard (but for the output's latency).
    bool isAwaitingFirstPlay = false;

    hyoshi::audio::AudioClockSnapshot snapshot;
    hyoshi::HostTimeNs lastUpdateTime = 0;
    // Until snapshots reach this generation, their cursor predates the latest seek or music
    // change, and pendingCursorFrame is where the audio thread will continue.
    int64_t expectedGeneration = 0;
    hyoshi::DspFrame pendingCursorFrame = 0;

    hyoshi::SongTimeUs lastSongTime = 0;
    int64_t lastSeekGeneration = -1;
    double musicRate = 1.0;
    double lastMusicRate = 1.0;
    bool wasPlaying = false;
    bool isContinuous = false;
    bool isDiscontinuity = false;
    uint32_t backwardSteps = 0;
};

} // namespace hyoshi::songs
