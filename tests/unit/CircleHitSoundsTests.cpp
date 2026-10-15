#include "songs/CircleHitSounds.h"

#include "audio/IAudioBackend.h"
#include "audio/Synth.h"
#include "circle/CircleChart.h"
#include "circle/CircleJudge.h"
#include "core/JobSystem.h"
#include "songs/SongPlayer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using hyoshi::DspFrame;
using hyoshi::SongTimeUs;
using hyoshi::audio::SoundHandle;
using hyoshi::audio::SoundId;
using hyoshi::circle::CircleAction;
using hyoshi::circle::CircleChart;
using hyoshi::circle::CircleJudge;
using hyoshi::circle::CircleNote;
using hyoshi::circle::MakeAction;
using hyoshi::rhythm::JudgmentEvent;

namespace
{

constexpr uint32_t RATE = 48'000;
constexpr SoundId FIRST_SOUND = 100;

// Records what's loaded and played; its clock snapshot is whatever the test sets.
class FakeAudio final : public hyoshi::audio::IAudioBackend
{
public:
    enum class CallType : uint8_t
    {
        Play,
        PlayAt,
        Stop
    };
    struct Call
    {
        CallType Type = CallType::Play;
        SoundId Sound = 0;
        DspFrame Frame = 0;
        float Volume = 0.0f;
        SoundHandle Handle = 0;
    };

    hyoshi::Result<void> Initialize(const hyoshi::audio::AudioConfig& /*config*/) override
    {
        return {};
    }
    void Shutdown() override
    {
    }
    void Update() override
    {
    }
    void Suspend() override
    {
    }
    void Resume() override
    {
    }
    uint32_t GetSampleRate() const override
    {
        return RATE;
    }
    hyoshi::Result<void> LoadSound(SoundId sound, hyoshi::audio::PcmBuffer pcm) override
    {
        Loaded[sound] = pcm.GetFrameCount();
        return {};
    }
    SoundHandle Play(SoundId sound, hyoshi::audio::Bus /*bus*/, float volume) override
    {
        Calls.push_back({CallType::Play, sound, 0, volume, ++lastHandle});
        return lastHandle;
    }
    SoundHandle PlayAtMusicFrame(SoundId sound, hyoshi::audio::Bus /*bus*/, DspFrame musicFrame, float volume) override
    {
        Calls.push_back({CallType::PlayAt, sound, musicFrame, volume, ++lastHandle});
        return lastHandle;
    }
    void Stop(SoundHandle handle) override
    {
        Calls.push_back({CallType::Stop, 0, 0, 0.0f, handle});
    }
    void SetBusVolume(hyoshi::audio::Bus /*bus*/, float /*volume*/) override
    {
    }
    hyoshi::Result<int64_t> SetMusic(hyoshi::audio::PcmBuffer /*pcm*/) override
    {
        return int64_t{0};
    }
    hyoshi::Result<int64_t> SetMusicStream(std::unique_ptr<hyoshi::audio::MusicStream> /*stream*/) override
    {
        return int64_t{0};
    }
    void PlayMusic() override
    {
    }
    void PauseMusic() override
    {
    }
    void SetMusicRate(double /*rate*/) override
    {
    }
    void SetMusicLowPass(float /*cutoffHz*/) override
    {
    }

    int64_t SeekMusic(DspFrame /*frame*/) override
    {
        return 0;
    }
    DspFrame GetMusicFrameCount() const override
    {
        return 0;
    }
    hyoshi::audio::AudioClockSnapshot GetClockSnapshot() const override
    {
        return Snapshot;
    }
    int64_t GetEstimatedOutputLatencyUs() const override
    {
        return 0;
    }
    bool ConsumeRouteChangedFlag() override
    {
        return false;
    }
    hyoshi::audio::AudioDeviceInfo GetDeviceInfo() const override
    {
        return {};
    }
    bool ShowOutputSettings() override
    {
        return false;
    }
    hyoshi::audio::AudioStats GetStats() const override
    {
        return {};
    }
    void ResetStats() override
    {
    }

    std::vector<Call> Calls;
    // Each loaded sound's length in frames.
    std::map<SoundId, DspFrame> Loaded;
    hyoshi::audio::AudioClockSnapshot Snapshot;

private:
    SoundHandle lastHandle = 0;
};

CircleNote Circle(SongTimeUs time, glm::vec2 position, uint32_t hitSound)
{
    CircleNote note;
    note.Time = time;
    note.EndTime = time;
    note.Position = position;
    note.FirstHitSound = hitSound;
    note.HitSoundCount = 1;
    return note;
}

CircleChart MakeChart(std::vector<CircleNote> notes, std::vector<hyoshi::circle::HitSound> hitSounds)
{
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.StackLeniency = 0.0f;
    chart.Notes = std::move(notes);
    chart.Notes.front().IsNewCombo = true;
    chart.HitSounds = std::move(hitSounds);
    return chart;
}

// A test's audio, music clock, judge, and hit sounds, with the clock moved by hand.
struct Rig
{
    explicit Rig(const CircleChart& chart, const std::string& chartDirectory = {}, bool isUnified = false)
    {
        Audio.Snapshot.SampleRate = RATE;
        Audio.Snapshot.CallbackFrames = 480;
        if (isUnified)
        {
            Sounds.LoadUnified(Audio, chart, hyoshi::audio::MakeClick(RATE, 880.0f, 25.0f, 0.5f), FIRST_SOUND);
        }
        else
        {
            Sounds.Load(Audio, chart, chartDirectory, {}, FIRST_SOUND);
        }
        Sounds.Reset();
        Judge.Reset(chart);
    }

    // The audio thread has got as far as `time`.
    void At(SongTimeUs time)
    {
        Audio.Snapshot.MusicFrame = hyoshi::UsToFrames(time, RATE);
        Player.Update(0);
        Sounds.Update(Player, Judge, false, 1.0f);
    }

    void Act(CircleAction type, SongTimeUs time, glm::vec2 position)
    {
        Judge.ProcessAction(MakeAction(type, 0, position, time), Events);
        Deliver();
    }

    void JudgeTo(SongTimeUs time)
    {
        Judge.Advance(time, Events);
        Deliver();
    }

    void Deliver()
    {
        for (const JudgmentEvent& event : Events)
        {
            Sounds.OnJudgment(event, Player, 1.0f);
        }
        Events.clear();
    }

    FakeAudio Audio;
    hyoshi::JobSystem Jobs{1};
    hyoshi::songs::SongPlayer Player{Audio, Jobs};
    hyoshi::songs::CircleHitSounds Sounds;
    CircleJudge Judge;
    std::vector<JudgmentEvent> Events;
};

// 16-bit mono PCM at RATE.
void WriteWav(const std::filesystem::path& path, uint32_t frames)
{
    const auto put32 = [](std::ofstream& out, uint32_t value)
    {
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            out.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    };
    const auto put16 = [](std::ofstream& out, uint16_t value)
    {
        out.put(static_cast<char>(value & 0xFFU));
        out.put(static_cast<char>(value >> 8U));
    };
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    put32(out, 36 + (frames * 2));
    out.write("WAVEfmt ", 8);
    put32(out, 16);
    put16(out, 1);
    put16(out, 1);
    put32(out, RATE);
    put32(out, RATE * 2);
    put16(out, 2);
    put16(out, 16);
    out.write("data", 4);
    put32(out, frames * 2);
    for (uint32_t i = 0; i < frames; ++i)
    {
        put16(out, static_cast<uint16_t>(i % 2 == 0 ? 8000 : -8000));
    }
}

} // namespace

TEST_CASE("Circle hit sounds: only what's hit sounds, and followed slider parts on their frame")
{
    // A circle with a whistle, then a slider with a tick halfway (2.5 s) and its tail at 3 s.
    hyoshi::circle::HitSound whistle;
    whistle.Additions = hyoshi::circle::ADDITION_WHISTLE;
    CircleChart chart =
        MakeChart({Circle(1'000'000, {100.0f, 100.0f}, 0), Circle(1'500'000, {400.0f, 100.0f}, 0)}, {whistle});
    CircleNote slider;
    slider.Time = 2'000'000;
    slider.EndTime = 3'000'000;
    slider.Position = {200.0f, 200.0f};
    slider.Type = hyoshi::circle::NoteType::Slider;
    slider.Curve = hyoshi::circle::CurveType::Linear;
    slider.ControlPointCount = 2;
    slider.Length = 100.0f;
    slider.TickDistance = 50.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {300.0f, 200.0f}};
    chart.Notes.push_back(slider);
    hyoshi::circle::FinishCircleChart(chart);
    REQUIRE(chart.Notes[2].PartCount == 3);

    Rig rig(chart);
    std::vector<FakeAudio::Call>& calls = rig.Audio.Calls;
    // In play, nothing is scheduled for circles: they sound when struck.
    rig.At(950'000);
    CHECK(calls.empty());
    rig.Act(CircleAction::Press, 1'000'000, {100.0f, 100.0f});
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].Type == FakeAudio::CallType::Play);
    CHECK(calls[1].Type == FakeAudio::CallType::Play);
    CHECK(calls[0].Volume == doctest::Approx(1.0f));
    rig.Act(CircleAction::Release, 1'030'000, {100.0f, 100.0f});

    // The second circle is missed: no sound.
    rig.At(1'450'000);
    rig.JudgeTo(1'800'000);
    CHECK(calls.size() == 2);

    // The slider's head, struck: at once.
    rig.Act(CircleAction::Press, 2'000'000, {200.0f, 200.0f});
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].Type == FakeAudio::CallType::Play);

    // Followed as the audio nears the tick: scheduled on its frame.
    rig.Act(CircleAction::Move, 2'470'000, {247.0f, 200.0f});
    rig.At(2'470'000);
    REQUIRE(calls.size() == 4);
    CHECK(calls[3].Type == FakeAudio::CallType::PlayAt);
    CHECK(calls[3].Frame == hyoshi::UsToFrames(2'500'000, RATE));

    // Let go before it: the tick is missed, and its sound stopped.
    rig.Act(CircleAction::Release, 2'480'000, {248.0f, 200.0f});
    rig.JudgeTo(2'600'000);
    REQUIRE(calls.size() == 5);
    CHECK(calls[4].Type == FakeAudio::CallType::Stop);
    CHECK(calls[4].Handle == calls[3].Handle);

    // Back on the ball: the tail is judged 36 ms early, and still sounds on its frame.
    rig.Act(CircleAction::Press, 2'900'000, {290.0f, 200.0f});
    rig.At(2'900'000);
    CHECK(calls.size() == 5);
    rig.JudgeTo(2'970'000);
    REQUIRE(calls.size() == 6);
    CHECK(calls[5].Type == FakeAudio::CallType::PlayAt);
    CHECK(calls[5].Frame == hyoshi::UsToFrames(3'000'000, RATE));
    CHECK(rig.Judge.IsFinished());
}

TEST_CASE("Circle hit sounds: followed parts sound where struck ones would, past the latency and offset")
{
    // A circle long before, then a slider from 2 s to 3 s with a tick halfway.
    CircleChart chart = MakeChart({Circle(500'000, {100.0f, 100.0f}, 0)}, {hyoshi::circle::HitSound{}});
    CircleNote slider;
    slider.Time = 2'000'000;
    slider.EndTime = 3'000'000;
    slider.Position = {200.0f, 200.0f};
    slider.Type = hyoshi::circle::NoteType::Slider;
    slider.Curve = hyoshi::circle::CurveType::Linear;
    slider.ControlPointCount = 2;
    slider.Length = 100.0f;
    slider.TickDistance = 50.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {300.0f, 200.0f}};
    chart.Notes.push_back(slider);
    hyoshi::circle::FinishCircleChart(chart);

    // In play, with 30 ms of output latency and a 10 ms offset: a perfect hit on the tick lands when
    // the music is 40 ms past it, so the tick sounds there, as a struck circle would.
    Rig rig(chart);
    rig.Player.GetClock().OutputLatencyUs = 30'000;
    rig.Player.GetClock().UserAudioOffsetUs = 10'000;
    rig.Act(CircleAction::Press, 2'000'000, {200.0f, 200.0f});
    rig.Act(CircleAction::Move, 2'470'000, {247.0f, 200.0f});
    rig.At(2'510'000);
    std::vector<FakeAudio::Call>& calls = rig.Audio.Calls;
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].Type == FakeAudio::CallType::PlayAt);
    CHECK(calls[1].Frame == hyoshi::UsToFrames(2'540'000, RATE));

    // Autoplay keeps to the music's own frames.
    Rig autoplay(chart);
    autoplay.Player.GetClock().OutputLatencyUs = 30'000;
    autoplay.Audio.Snapshot.MusicFrame = hyoshi::UsToFrames(1'950'000, RATE);
    autoplay.Player.Update(0);
    autoplay.Sounds.Update(autoplay.Player, autoplay.Judge, true, 1.0f);
    REQUIRE(!autoplay.Audio.Calls.empty());
    CHECK(autoplay.Audio.Calls[0].Type == FakeAudio::CallType::PlayAt);
    CHECK(autoplay.Audio.Calls[0].Frame == hyoshi::UsToFrames(2'000'000, RATE));
}

TEST_CASE("Circle hit sounds: the map's files first, any case, and an empty file is silence")
{
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "hyoshi-hit-sounds-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    WriteWav(folder / "Soft-HitNormal.WAV", 100);
    std::ofstream(folder / "soft-hitclap.wav").close();

    // Custom sample index 1 takes the map's files; 0 takes the defaults.
    hyoshi::circle::HitSound custom;
    custom.Bank = hyoshi::circle::SampleBank::Soft;
    custom.Additions = hyoshi::circle::ADDITION_CLAP;
    custom.CustomIndex = 1;
    hyoshi::circle::HitSound standard = custom;
    standard.CustomIndex = 0;
    CircleChart chart =
        MakeChart({Circle(1'000'000, {100.0f, 100.0f}, 0), Circle(2'000'000, {100.0f, 100.0f}, 1)}, {custom, standard});
    hyoshi::circle::FinishCircleChart(chart);

    Rig rig(chart, folder.string() + "/");
    // The map's normal sound (its clap is silent), then the generated normal sound and clap.
    REQUIRE(rig.Audio.Loaded.size() == 3);
    CHECK(rig.Audio.Loaded[FIRST_SOUND] == 100);
    CHECK(rig.Audio.Loaded[FIRST_SOUND + 1] > 100);

    rig.Act(CircleAction::Press, 1'000'000, {100.0f, 100.0f});
    REQUIRE(rig.Audio.Calls.size() == 1);
    CHECK(rig.Audio.Calls[0].Sound == FIRST_SOUND);
    rig.Act(CircleAction::Release, 1'030'000, {100.0f, 100.0f});
    rig.Act(CircleAction::Press, 2'000'000, {100.0f, 100.0f});
    CHECK(rig.Audio.Calls.size() == 3);
    std::filesystem::remove_all(folder);
}

TEST_CASE("Circle hit sounds: a unified sound for every hit but ticks, at full volume")
{
    hyoshi::circle::HitSound quiet;
    quiet.Volume = 20;
    quiet.Additions = hyoshi::circle::ADDITION_CLAP;
    CircleChart chart = MakeChart({Circle(1'000'000, {100.0f, 100.0f}, 0)}, {quiet});
    // From {200, 200} to {300, 200} over 2 to 3 s, with a tick halfway.
    CircleNote slider;
    slider.Time = 2'000'000;
    slider.EndTime = 3'000'000;
    slider.Position = {200.0f, 200.0f};
    slider.Type = hyoshi::circle::NoteType::Slider;
    slider.Curve = hyoshi::circle::CurveType::Linear;
    slider.ControlPointCount = 2;
    slider.Length = 100.0f;
    slider.TickDistance = 50.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {300.0f, 200.0f}};
    chart.Notes.push_back(slider);
    hyoshi::circle::FinishCircleChart(chart);
    REQUIRE(chart.Notes[1].PartCount == 3);

    Rig rig(chart, {}, true);
    std::vector<FakeAudio::Call>& calls = rig.Audio.Calls;
    REQUIRE(rig.Audio.Loaded.size() == 1);
    rig.Act(CircleAction::Press, 1'000'000, {100.0f, 100.0f});
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].Sound == FIRST_SOUND);
    CHECK(calls[0].Volume == doctest::Approx(1.0f));
    rig.Act(CircleAction::Release, 1'030'000, {100.0f, 100.0f});

    // The head sounds, the tick doesn't, and the tail does on its frame.
    rig.Act(CircleAction::Press, 2'000'000, {200.0f, 200.0f});
    rig.Act(CircleAction::Move, 2'470'000, {247.0f, 200.0f});
    rig.At(2'470'000);
    rig.JudgeTo(2'600'000);
    CHECK(calls.size() == 2);
    rig.Act(CircleAction::Move, 2'900'000, {290.0f, 200.0f});
    rig.At(2'900'000);
    rig.JudgeTo(2'970'000);
    REQUIRE(calls.size() == 3);
    CHECK(calls[2].Type == FakeAudio::CallType::PlayAt);
    CHECK(calls[2].Frame == hyoshi::UsToFrames(3'000'000, RATE));
}
