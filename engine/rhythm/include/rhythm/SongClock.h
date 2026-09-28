#pragma once

#include "audio/AudioTypes.h"

#include "core/Time.h"

#include <cstdint>

namespace hyoshi::rhythm
{

// Song time from the audio clock (DESIGN.md section 9). Song time follows the music cursor in the
// snapshots, not frames since the song started, so pauses, seeks, and device restarts can't
// desynchronize it.
//
// Snapshots arrive once per audio callback, so extrapolating from the newest one jitters by the
// callback timing. The clock keeps a filtered mapping, song time = host time + offset, that
// corrects toward the raw estimate a little per update and re-anchors on large errors. Visuals and
// input judgment use the same mapping. Only a seek or a pause moves song time backwards: when a
// re-anchor lands behind the time already shown, song time holds until the music catches up.
class SongClock
{
public:
    // A raw error beyond this is a real jump (hitch, glitch) rather than jitter: resync at once.
    static constexpr SongTimeUs SNAP_THRESHOLD_US = 30'000;
    // Each update corrects this fraction of the error, at most MAX_CORRECTION_US.
    static constexpr int64_t CORRECTION_DIVISOR = 10;
    static constexpr SongTimeUs MAX_CORRECTION_US = 1'000;
    // Snapshots older than this (or four callbacks, if longer) mean the audio device stopped;
    // the clock holds instead of running ahead of the music.
    static constexpr HostTimeNs STALL_THRESHOLD_NS = 60 * NS_PER_MS;

    // Once per frame, with the newest snapshot.
    void Update(const audio::AudioClockSnapshot& snapshot, HostTimeNs now);

    // Call right after IAudioBackend::SeekMusic with the generation it returned. Until snapshots
    // report that generation, the clock holds at the target so no frame shows the old position.
    void ExpectSeek(int64_t seekGeneration, SongTimeUs target);

    // Filtered song time for gameplay. Never moves backwards while playing, except across a seek.
    SongTimeUs GetSongTime() const
    {
        return songTime;
    }

    // Song time for drawing, shifted by the display latency calibration.
    SongTimeUs GetVisualSongTime() const
    {
        return songTime + UserVisualOffsetUs;
    }

    // The unfiltered estimate from the last Update, for the debug panel.
    SongTimeUs GetRawSongTime() const
    {
        return rawSongTime;
    }

    // Converts an input timestamp to song time through the filtered mapping. While GetSongTime is
    // holding (after a stall or a backward re-anchor) it can be ahead of this; judgment uses this.
    SongTimeUs HostTimeToSongTime(HostTimeNs hostTime) const;

    // The inverse of HostTimeToSongTime while playing, for synthesizing input at a song time
    // (autoplay, tests). Exact: HostTimeToSongTime(SongTimeToHostTime(t)) == t.
    HostTimeNs SongTimeToHostTime(SongTimeUs time) const
    {
        return (time - mappingOffsetUs) * NS_PER_US;
    }

    // Where music frame `frame` lands on the song timeline, and back. Includes the latency and
    // offsets, so seeking to MusicFrameFor(t) shows song time t.
    SongTimeUs SongTimeForMusicFrame(DspFrame frame, int64_t sampleRate) const;
    DspFrame MusicFrameForSongTime(SongTimeUs time, int64_t sampleRate) const;

    bool IsPlaying() const
    {
        return isPlaying;
    }

    bool IsStalled() const
    {
        return isStalled;
    }

    // Times the clock re-anchored to the raw estimate because the error exceeded the threshold.
    uint32_t GetResyncCount() const
    {
        return resyncCount;
    }

    // Offsets in microseconds (DESIGN.md section 9).
    int64_t OutputLatencyUs = 0;
    int64_t UserAudioOffsetUs = 0;
    int64_t UserVisualOffsetUs = 0;
    int64_t ChartOffsetUs = 0;

private:
    int64_t GetTotalOffsetUs() const
    {
        return OutputLatencyUs + UserAudioOffsetUs + ChartOffsetUs;
    }

    SongTimeUs ComputeRawSongTime(const audio::AudioClockSnapshot& snapshot, HostTimeNs now) const;

    // Filtered song time = host time in microseconds + mappingOffsetUs, while playing.
    SongTimeUs mappingOffsetUs = 0;
    SongTimeUs songTime = 0;
    SongTimeUs rawSongTime = 0;
    int64_t lastSeekGeneration = -1;
    int64_t pendingSeekGeneration = 0;
    SongTimeUs pendingSeekTarget = 0;
    bool hasPendingSeek = false;
    bool isPlaying = false;
    bool isStalled = false;
    uint32_t resyncCount = 0;
};

} // namespace hyoshi::rhythm
