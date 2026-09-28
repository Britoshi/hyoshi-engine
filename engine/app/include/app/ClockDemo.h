#pragma once

#include "app/Scene.h"
#include "songs/SongPlayer.h"

#include "audio/IAudioBackend.h"
#include "core/JobSystem.h"
#include "renderer/SpriteBatch.h"

#include <array>
#include <cstdint>

namespace hyoshi::app
{

// M3 check (DESIGN.md section 25): music with a scheduled metronome click and a visual beat flash
// that must stay in sync, plus the clock debug panel. Plays a generated metronome track, or the
// file in HYOSHI_MUSIC with its tempo from HYOSHI_BPM and HYOSHI_FIRST_BEAT_MS. Runs instead of
// the game with HYOSHI_SCENE=clock.
class ClockDemo : public Scene
{
public:
    ClockDemo(hyoshi::audio::IAudioBackend& audio, hyoshi::JobSystem& jobs);

    // Starts decoding or generating the music on a worker.
    void Start();

    void RunFrame(const FrameContext& frame, ui::Ui& ui) override;

    // Once per frame, before drawing.
    void Update(hyoshi::HostTimeNs now);

    void Draw(hyoshi::renderer::SpriteBatch& sprites, glm::vec2 canvas) const;
    void BuildWindow();

    void BuildDebugWindow() override
    {
        BuildWindow();
    }

    songs::SongPlayer* GetPlayer() override
    {
        return &player;
    }

    // Seeks the music and requeues the metronome from the new position.
    void SeekTo(hyoshi::SongTimeUs target);

    void LogSummary() const override;

private:
    void ScheduleMetronome();
    // The first beat at or after a music frame.
    int64_t FirstBeatAtOrAfter(hyoshi::DspFrame frame) const;
    // The index of the last beat at or before a song time, or -1 before the first beat.
    int64_t BeatAt(hyoshi::SongTimeUs time) const;

    hyoshi::audio::IAudioBackend& audio;
    songs::SongPlayer player;

    double bpm = 0.0;
    hyoshi::SongTimeUs firstBeatUs = 0;
    uint32_t beatsPerBar = 4;

    bool isMetronomeOn = true;
    int64_t nextScheduledBeat = 0;
    float musicVolume = 0.8f;
    float clickVolume = 0.8f;

    static constexpr size_t HISTORY = 240;
    // Raw minus filtered song time per frame, in milliseconds.
    std::array<float, HISTORY> rawErrorMs{};
    size_t historyCursor = 0;
    float worstRawErrorMs = 0.0f;
};

} // namespace hyoshi::app
