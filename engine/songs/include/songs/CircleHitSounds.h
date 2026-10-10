#pragma once

#include "songs/SongPlayer.h"

#include "audio/IAudioBackend.h"
#include "circle/CircleChart.h"
#include "circle/CircleJudge.h"
#include "rhythm/Judgment.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hyoshi::songs
{

// A circle chart's hit sounds, the osu! way (DESIGN.md section 16.4). Each circle and slider part
// sounds its bank's normal sound with its additions (whistle, finish, clap) at its volume, or the
// file it names; a slider tick sounds its bank's slider tick.
//
// The sounds come from the chart's folder for a custom sample index of 1 or more (soft-hitclap2.wav
// for index 2, then soft-hitclap.wav), then from a skin folder (soft-hitclap.wav), then generated
// stand-ins: the engine can't ship osu!'s. Names match in any case, as .wav, .ogg, or .mp3. A file
// that's there but empty or unreadable is silence, as maps mute sounds with one.
//
// Only what's hit sounds. With autoplay, everything is scheduled on its music frame ahead of time.
// In play, a circle or a slider's head sounds the moment it's struck (OnJudgment). The other
// parts are judged once the ball has passed them, too late to sound on time, so a part is scheduled
// on its frame just before the audio thread gets there if its slider is being followed then; if
// it's missed after all, it's stopped, and a part that's hit without being scheduled plays at once.
class CircleHitSounds
{
public:
    // Loads the sounds the chart uses, as SoundIds from firstSound up. The directories end with a
    // separator; the skin's may be empty for none.
    void Load(audio::IAudioBackend& audio, const circle::CircleChart& chart, const std::string& chartDirectory,
              const std::string& skinDirectory, audio::SoundId firstSound);
    // Instead of the chart's sounds: one sound (as SoundId firstSound) for every circle and slider
    // part, at full volume, except slider ticks, which are silent. The same rules for when.
    void LoadUnified(audio::IAudioBackend& audio, const circle::CircleChart& chart, audio::PcmBuffer sound,
                     audio::SoundId firstSound);
    // Before playing, and whenever play starts over (a seek drops what's scheduled).
    void Reset();
    // Every frame, after the player's update. Volume scales the chart's (0 is silent).
    void Update(const SongPlayer& player, const circle::CircleJudge& judge, bool isAutoplay, float volume);
    // For every judgment event.
    void OnJudgment(const rhythm::JudgmentEvent& event, const SongPlayer& player, float volume);

private:
    static constexpr size_t MAX_LAYERS = 4;
    static constexpr int32_t SILENT = -1;

    enum class CueState : uint8_t
    {
        Waiting,
        Scheduled,
        Done
    };

    // What sounds for a circle or a slider part, and when.
    struct Cue
    {
        SongTimeUs Time = 0;
        uint32_t Note = 0;
        // As JudgmentEvent::Part counts them: 0 for a circle.
        uint32_t Part = 0;
        // Indices into sounds, SILENT for none.
        std::array<int32_t, MAX_LAYERS> Layers{SILENT, SILENT, SILENT, SILENT};
        float Volume = 1.0f;
        CueState State = CueState::Waiting;
        std::array<audio::SoundHandle, MAX_LAYERS> Handles{};
    };

    void Clear(audio::IAudioBackend& audioBackend);
    // A cue for every circle and slider part, which `fill` gives its sounds and volume.
    void AddCues(const circle::CircleChart& chart, const std::function<void(Cue&)>& fill);
    // The cue of a circle (part 0) or a slider part, or nullptr.
    Cue* FindCue(uint32_t note, uint32_t part);
    // The earliest music frame a sound sent now surely reaches the audio thread in time for.
    static DspFrame GetEarliestFrame(const SongPlayer& player);
    DspFrame GetFrame(const Cue& cue, const SongPlayer& player) const;
    void Play(Cue& cue, float volume, std::optional<DspFrame> frame);

    audio::IAudioBackend* audio = nullptr;
    // Loaded sound IDs; SILENT entries never make it here.
    std::vector<audio::SoundId> sounds;
    std::vector<Cue> cues;
    // Cues by time.
    std::vector<uint32_t> order;
    // A note's first cue: a circle's one, or a slider's parts' from the head.
    std::vector<uint32_t> firstCue;
    size_t nextCue = 0;
};

} // namespace hyoshi::songs
