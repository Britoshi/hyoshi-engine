#include "rhythm/SongClock.h"

#include <algorithm>
#include <cmath>

namespace hyoshi::rhythm
{

namespace
{

HostTimeNs GetStallThreshold(const audio::AudioClockSnapshot& snapshot)
{
    if (snapshot.SampleRate <= 0)
    {
        return SongClock::STALL_THRESHOLD_NS;
    }
    const HostTimeNs fourCallbacks = 4 * snapshot.CallbackFrames * NS_PER_SECOND / snapshot.SampleRate;
    return std::max(SongClock::STALL_THRESHOLD_NS, fourCallbacks);
}

} // namespace

void SongClock::Update(const audio::AudioClockSnapshot& snapshot, HostTimeNs now)
{
    const double rate = snapshot.MusicRate > 0.0 ? snapshot.MusicRate : 1.0;
    const bool isRateChange = rate != musicRate;
    musicRate = rate;
    rawSongTime = ComputeRawSongTime(snapshot, now);

    if (hasPendingSeek)
    {
        if (snapshot.SeekGeneration < pendingSeekGeneration)
        {
            songTime = pendingSeekTarget;
            return;
        }
        hasPendingSeek = false;
    }

    const bool isJump = snapshot.SeekGeneration != lastSeekGeneration || isRateChange;
    lastSeekGeneration = snapshot.SeekGeneration;
    const SongTimeUs nowUs = FloorDiv(now, NS_PER_US);

    if (snapshot.IsMusicPlaying == 0)
    {
        isPlaying = false;
        isStalled = false;
        songTime = rawSongTime;
        return;
    }

    // The device stopped delivering callbacks: hold rather than extrapolate past the music.
    if (now - snapshot.HostTime > GetStallThreshold(snapshot))
    {
        isStalled = true;
        return;
    }

    // Starting, resuming from pause, a seek, or a new rate: take the new position as is.
    if (!isPlaying || isJump)
    {
        mappingOffsetUs = rawSongTime;
        anchorHostUs = nowUs;
        songTime = rawSongTime;
        isPlaying = true;
        isStalled = false;
        return;
    }

    const SongTimeUs error = rawSongTime - MapHostTime(nowUs);
    if (isStalled || error > SNAP_THRESHOLD_US || error < -SNAP_THRESHOLD_US)
    {
        // Re-anchor. If that is behind the time already shown (the clock ran ahead while the
        // device stalled), the max below holds until the music catches up instead of rewinding.
        mappingOffsetUs += error;
        resyncCount += isStalled ? 0 : 1;
        isStalled = false;
    }
    else
    {
        mappingOffsetUs += std::clamp(error / CORRECTION_DIVISOR, -MAX_CORRECTION_US, MAX_CORRECTION_US);
    }
    songTime = std::max(songTime, MapHostTime(nowUs));
}

void SongClock::ExpectSeek(int64_t seekGeneration, SongTimeUs target)
{
    hasPendingSeek = true;
    pendingSeekGeneration = seekGeneration;
    pendingSeekTarget = target;
    songTime = target;
}

SongTimeUs SongClock::HostTimeToSongTime(HostTimeNs hostTime) const
{
    if (!isPlaying || isStalled || hasPendingSeek)
    {
        return songTime;
    }
    return MapHostTime(FloorDiv(hostTime, NS_PER_US));
}

HostTimeNs SongClock::SongTimeToHostTime(SongTimeUs time) const
{
    const SongTimeUs musicUs = time - mappingOffsetUs;
    const SongTimeUs hostUs =
        musicRate == 1.0 ? musicUs : static_cast<SongTimeUs>(std::llround(static_cast<double>(musicUs) / musicRate));
    return (anchorHostUs + hostUs) * NS_PER_US;
}

SongTimeUs SongClock::ToMusicTime(SongTimeUs us) const
{
    return musicRate == 1.0 ? us : static_cast<SongTimeUs>(std::llround(static_cast<double>(us) * musicRate));
}

SongTimeUs SongClock::SongTimeForMusicFrame(DspFrame frame, int64_t sampleRate) const
{
    return FramesToUs(frame, sampleRate) - GetTotalOffsetUs();
}

DspFrame SongClock::MusicFrameForSongTime(SongTimeUs time, int64_t sampleRate) const
{
    return UsToFrames(time + GetTotalOffsetUs(), sampleRate);
}

// The music frame audible at `now`: the snapshot's cursor, advanced by the host time since the
// callback started, minus what is still buffered on its way to the speaker.
SongTimeUs SongClock::ComputeRawSongTime(const audio::AudioClockSnapshot& snapshot, HostTimeNs now) const
{
    if (snapshot.SampleRate <= 0)
    {
        return -GetTotalOffsetUs();
    }

    SongTimeUs musicTime = FramesToUs(snapshot.MusicFrame, snapshot.SampleRate);
    if (snapshot.IsMusicPlaying != 0)
    {
        const HostTimeNs elapsed = std::clamp<HostTimeNs>(now - snapshot.HostTime, 0, GetStallThreshold(snapshot));
        musicTime += ToMusicTime(elapsed / NS_PER_US);
    }
    return musicTime - GetTotalOffsetUs();
}

} // namespace hyoshi::rhythm
