#include "app/ClockDemo.h"

#include "core/Env.h"
#include "ui/Ui.h"

#include "audio/Synth.h"
#include "core/Log.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace hyoshi::app
{

using hyoshi::DspFrame;
using hyoshi::HostTimeNs;
using hyoshi::SongTimeUs;
using hyoshi::audio::Bus;
using hyoshi::audio::PcmBuffer;
using hyoshi::renderer::Color;

namespace
{

constexpr hyoshi::audio::SoundId CLICK_SOUND = 1;

// The generated track: two minutes at 128 BPM.
constexpr double DEFAULT_BPM = 128.0;
constexpr SongTimeUs DEFAULT_FIRST_BEAT_US = 500'000;
constexpr uint32_t DEFAULT_BEAT_COUNT = 256;

// The scheduled click is pitched below the track's own clicks, so both stay audible and any
// misalignment between them is heard as a flam.
constexpr float CLICK_HZ = 660.0f;
constexpr float CLICK_MS = 30.0f;
constexpr float CLICK_GAIN = 0.7f;

// How far past the audio thread's cursor clicks are queued. One callback is enough; this leaves
// room for slow frames.
constexpr SongTimeUs SCHEDULE_AHEAD_US = 100'000;

constexpr SongTimeUs FLASH_US = 150'000;
constexpr int OFFSET_LIMIT_MS = 300;
constexpr float WINDOW_WIDTH = 400.0f;

} // namespace

ClockDemo::ClockDemo(hyoshi::audio::IAudioBackend& audioBackend, hyoshi::JobSystem& jobs)
    : audio(audioBackend), player(audioBackend, jobs)
{
}

void ClockDemo::Start()
{
    const uint32_t rate = audio.GetSampleRate();
    if (hyoshi::Result<void> result =
            audio.LoadSound(CLICK_SOUND, hyoshi::audio::MakeClick(rate, CLICK_HZ, CLICK_MS, CLICK_GAIN));
        !result)
    {
        HYOSHI_LOG_ERROR("Could not load the metronome click: {}", result.GetError().Message);
    }
    audio.SetBusVolume(Bus::Music, musicVolume);
    audio.SetBusVolume(Bus::HitSounds, clickVolume);

    if (const char* path = std::getenv("HYOSHI_MUSIC"))
    {
        bpm = ReadEnvNumber("HYOSHI_BPM").value_or(0.0);
        firstBeatUs = std::llround(ReadEnvNumber("HYOSHI_FIRST_BEAT_MS").value_or(0.0) * 1000.0);
        if (bpm <= 0.0)
        {
            HYOSHI_LOG_WARN("HYOSHI_BPM not set: no metronome or beat flash for {}", path);
        }
        player.LoadFile(path);
    }
    else
    {
        bpm = DEFAULT_BPM;
        firstBeatUs = DEFAULT_FIRST_BEAT_US;
        player.Load(
            "Generated metronome track",
            [tempo = bpm, first = firstBeatUs, perBar = beatsPerBar](uint32_t sampleRate) -> hyoshi::Result<PcmBuffer>
            { return hyoshi::audio::MakeMetronomeTrack(sampleRate, tempo, DEFAULT_BEAT_COUNT, perBar, first); });
    }
}

void ClockDemo::RunFrame(const FrameContext& frame, ui::Ui& ui)
{
    Update(frame.Now);
    Draw(ui.GetSprites(), frame.Canvas);
}

void ClockDemo::Update(HostTimeNs now)
{
    player.Update(now);
    if (player.WasJustLoaded())
    {
        nextScheduledBeat = 0;
    }

    const hyoshi::rhythm::SongClock& clock = player.GetClock();
    if (player.IsContinuous() && !clock.IsStalled())
    {
        const auto errorMs = static_cast<float>(ToMilliseconds(clock.GetRawSongTime() - clock.GetSongTime()));
        rawErrorMs[historyCursor] = errorMs;
        historyCursor = (historyCursor + 1) % HISTORY;
        worstRawErrorMs = std::max(worstRawErrorMs, std::abs(errorMs));
    }

    ScheduleMetronome();
}

void ClockDemo::SeekTo(SongTimeUs target)
{
    player.SeekTo(target);
    nextScheduledBeat = FirstBeatAtOrAfter(player.GetRenderCursor());

    // The audio thread applies commands in order, so clicks queued now land after the seek.
    ScheduleMetronome();
}

// Clicks are placed by music frame, so they land sample-exact in the audio stream whatever the
// latency; only the visuals depend on the song clock.
void ClockDemo::ScheduleMetronome()
{
    if (!player.IsLoaded() || bpm <= 0.0 || !isMetronomeOn)
    {
        return;
    }

    const int64_t rate = audio.GetSampleRate();
    const SongTimeUs horizon = hyoshi::FramesToUs(player.GetRenderCursor(), rate) + SCHEDULE_AHEAD_US;
    while (true)
    {
        const SongTimeUs beatTime = hyoshi::audio::GetBeatTimeUs(bpm, firstBeatUs, nextScheduledBeat);
        const DspFrame beatFrame = hyoshi::UsToFrames(beatTime, rate);
        if (beatTime > horizon || beatFrame >= audio.GetMusicFrameCount())
        {
            break;
        }
        audio.PlayAtMusicFrame(CLICK_SOUND, Bus::HitSounds, beatFrame);
        ++nextScheduledBeat;
    }
}

int64_t ClockDemo::FirstBeatAtOrAfter(DspFrame frame) const
{
    const SongTimeUs time = hyoshi::FramesToUs(frame, audio.GetSampleRate());
    int64_t beat = BeatAt(time);
    while (hyoshi::audio::GetBeatTimeUs(bpm, firstBeatUs, beat) < time)
    {
        ++beat;
    }
    return std::max<int64_t>(beat, 0);
}

int64_t ClockDemo::BeatAt(SongTimeUs time) const
{
    if (bpm <= 0.0 || time < firstBeatUs)
    {
        return -1;
    }
    auto beat = static_cast<int64_t>(static_cast<double>(time - firstBeatUs) * bpm / 60.0e6);
    while (hyoshi::audio::GetBeatTimeUs(bpm, firstBeatUs, beat) > time)
    {
        --beat;
    }
    while (hyoshi::audio::GetBeatTimeUs(bpm, firstBeatUs, beat + 1) <= time)
    {
        ++beat;
    }
    return beat;
}

void ClockDemo::Draw(hyoshi::renderer::SpriteBatch& sprites, glm::vec2 canvas) const
{
    if (!player.IsLoaded())
    {
        return;
    }

    constexpr int32_t LAYER = 2;
    const glm::vec2 center = canvas * 0.5f;
    const SongTimeUs time = player.GetClock().GetVisualSongTime();

    // Progress through the song along the bottom.
    const SongTimeUs startTime = player.GetStartTime();
    const SongTimeUs endTime = player.GetEndTime();
    const float progress =
        endTime > startTime
            ? std::clamp(static_cast<float>(time - startTime) / static_cast<float>(endTime - startTime), 0.0f, 1.0f)
            : 0.0f;
    sprites.DrawRect({0.0f, canvas.y - 12.0f}, {canvas.x, 12.0f}, {40, 40, 60, 255}, LAYER);
    sprites.DrawRect({0.0f, canvas.y - 12.0f}, {canvas.x * progress, 12.0f}, {120, 160, 255, 255}, LAYER + 1);

    if (bpm <= 0.0)
    {
        return;
    }

    // The flash: bright on each beat, fading over FLASH_US. Accented on the first beat of a bar.
    const int64_t beat = BeatAt(time);
    const Color accent{255, 170, 60, 255};
    const Color normal{200, 220, 255, 255};
    const glm::vec2 flashSize{360.0f};
    sprites.DrawRect(center - (flashSize * 0.5f), flashSize, {30, 30, 45, 255}, LAYER);
    if (beat >= 0)
    {
        const SongTimeUs sinceBeat = time - hyoshi::audio::GetBeatTimeUs(bpm, firstBeatUs, beat);
        const float intensity = 1.0f - (static_cast<float>(sinceBeat) / static_cast<float>(FLASH_US));
        const bool isAccent = beat % static_cast<int64_t>(beatsPerBar) == 0;
        sprites.DrawRect(center - (flashSize * 0.5f), flashSize, WithAlpha(isAccent ? accent : normal, intensity),
                         LAYER + 1);
    }

    // One box per beat of the bar, and a cursor sweeping across them: continuous motion shows
    // stutter that the flash alone would hide.
    const float barWidth = 720.0f;
    const float boxWidth = barWidth / static_cast<float>(beatsPerBar);
    const glm::vec2 barOrigin{center.x - (barWidth * 0.5f), center.y + (flashSize.y * 0.5f) + 60.0f};
    const int64_t beatInBar = beat >= 0 ? beat % static_cast<int64_t>(beatsPerBar) : -1;
    for (uint32_t i = 0; i < beatsPerBar; ++i)
    {
        const Color color = static_cast<int64_t>(i) == beatInBar ? (i == 0 ? accent : normal) : Color{50, 50, 70, 255};
        sprites.DrawRect({barOrigin.x + (static_cast<float>(i) * boxWidth) + 6.0f, barOrigin.y},
                         {boxWidth - 12.0f, 40.0f}, color, LAYER);
    }

    const double beatsElapsed = static_cast<double>(time - firstBeatUs) * bpm / 60.0e6;
    if (beatsElapsed >= 0.0)
    {
        const double barPhase = std::fmod(beatsElapsed, static_cast<double>(beatsPerBar)) / beatsPerBar;
        const float cursorX = barOrigin.x + (static_cast<float>(barPhase) * barWidth);
        sprites.DrawRect({cursorX - 2.0f, barOrigin.y - 12.0f}, {4.0f, 64.0f}, {255, 255, 255, 255}, LAYER + 1);
    }
}

void ClockDemo::BuildWindow()
{
    // Top right, fixed width, height fitted to the contents, which grow once the music loads.
    const ImVec2 viewport = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowPos({viewport.x - WINDOW_WIDTH - 12.0f, 12.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({WINDOW_WIDTH, 0.0f}, {WINDOW_WIDTH, FLT_MAX});
    if (!ImGui::Begin("Audio clock", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    if (!player.IsLoaded())
    {
        if (player.GetLoadError().empty())
        {
            ImGui::Text("Loading %s...", player.GetName().c_str());
        }
        else
        {
            ImGui::TextWrapped("Could not load %s: %s", player.GetName().c_str(), player.GetLoadError().c_str());
        }
        ImGui::End();
        return;
    }

    hyoshi::rhythm::SongClock& clock = player.GetClock();
    const hyoshi::audio::AudioClockSnapshot& snapshot = player.GetSnapshot();
    const SongTimeUs songTime = clock.GetSongTime();
    ImGui::TextWrapped("%s", player.GetName().c_str());
    if (bpm > 0.0)
    {
        ImGui::Text("%.2f BPM, first beat at %.3f s, beat %lld", bpm, ToSeconds(firstBeatUs),
                    static_cast<long long>(BeatAt(clock.GetVisualSongTime())));
    }

    // Transport.
    if (ImGui::Button(player.IsPlaying() ? "Pause" : "Play"))
    {
        player.IsPlaying() ? player.Pause() : player.Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart"))
    {
        SeekTo(player.GetStartTime());
    }
    ImGui::SameLine();
    if (ImGui::Button("-5 s"))
    {
        SeekTo(songTime - (5 * hyoshi::US_PER_SECOND));
    }
    ImGui::SameLine();
    if (ImGui::Button("+5 s"))
    {
        SeekTo(songTime + (5 * hyoshi::US_PER_SECOND));
    }

    auto position = static_cast<float>(ToSeconds(songTime));
    const auto duration = static_cast<float>(ToSeconds(player.GetEndTime()));
    if (ImGui::SliderFloat("Position", &position, 0.0f, duration, "%.2f s"))
    {
        SeekTo(std::llround(static_cast<double>(position) * 1.0e6));
    }

    // Clock.
    if (ImGui::CollapsingHeader("Clock", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Song %.3f s   visual %.3f s", ToSeconds(songTime), ToSeconds(clock.GetVisualSongTime()));
        ImGui::Text("Raw - filtered %+.2f ms   worst %.2f ms", ToMilliseconds(clock.GetRawSongTime() - songTime),
                    static_cast<double>(worstRawErrorMs));
        ImGui::PlotLines("##raw error", rawErrorMs.data(), static_cast<int>(HISTORY), static_cast<int>(historyCursor),
                         "raw - filtered (ms)", -5.0f, 5.0f, {-1.0f, 60.0f});
        ImGui::Text("Snapshot age %.2f ms, callback %lld frames",
                    static_cast<double>(player.GetLastUpdateTime() - snapshot.HostTime) / 1.0e6,
                    static_cast<long long>(snapshot.CallbackFrames));
        ImGui::Text("Resyncs %u   backward steps %u%s", clock.GetResyncCount(), player.GetBackwardSteps(),
                    clock.IsStalled() ? "   STALLED" : "");
    }

    // Offsets. Changing the audio offset shifts song time, which isn't a rewind.
    if (ImGui::CollapsingHeader("Offsets", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Output latency estimate %.1f ms", ToMilliseconds(clock.OutputLatencyUs));
        auto audioOffsetMs = static_cast<int>(clock.UserAudioOffsetUs / hyoshi::US_PER_MS);
        if (ImGui::SliderInt("Audio offset (ms)", &audioOffsetMs, -OFFSET_LIMIT_MS, OFFSET_LIMIT_MS))
        {
            clock.UserAudioOffsetUs = audioOffsetMs * hyoshi::US_PER_MS;
            player.MarkDiscontinuity();
        }
        auto visualOffsetMs = static_cast<int>(clock.UserVisualOffsetUs / hyoshi::US_PER_MS);
        if (ImGui::SliderInt("Visual offset (ms)", &visualOffsetMs, -OFFSET_LIMIT_MS, OFFSET_LIMIT_MS))
        {
            clock.UserVisualOffsetUs = visualOffsetMs * hyoshi::US_PER_MS;
        }
    }

    // Mix.
    if (ImGui::CollapsingHeader("Mix"))
    {
        if (ImGui::Checkbox("Metronome click", &isMetronomeOn) && isMetronomeOn)
        {
            nextScheduledBeat = FirstBeatAtOrAfter(player.GetRenderCursor() + snapshot.CallbackFrames);
        }
        if (ImGui::SliderFloat("Music volume", &musicVolume, 0.0f, 1.0f))
        {
            audio.SetBusVolume(Bus::Music, musicVolume);
        }
        if (ImGui::SliderFloat("Click volume", &clickVolume, 0.0f, 1.0f))
        {
            audio.SetBusVolume(Bus::HitSounds, clickVolume);
        }
    }

    // Device.
    if (ImGui::CollapsingHeader("Device"))
    {
        const hyoshi::audio::AudioDeviceInfo info = audio.GetDeviceInfo();
        const hyoshi::audio::AudioStats stats = audio.GetStats();
        ImGui::TextWrapped("%s: %s", info.BackendName.c_str(), info.DeviceName.c_str());
        ImGui::Text("%u Hz (device %u Hz), period %u frames x %u", info.SampleRate, info.DeviceSampleRate,
                    info.PeriodFrames, info.PeriodCount);
        ImGui::Text("Callbacks %llu, longest gap %.2f ms", static_cast<unsigned long long>(stats.Callbacks),
                    static_cast<double>(stats.MaxCallbackGapNs) / 1.0e6);
        ImGui::Text("Voices %u active, %llu dropped, %llu late", stats.ActiveVoices,
                    static_cast<unsigned long long>(stats.DroppedVoices),
                    static_cast<unsigned long long>(stats.LateVoices));
        ImGui::Text("Route changes %u", stats.RouteChanges);
        if (ImGui::Button("Reset stats"))
        {
            audio.ResetStats();
            worstRawErrorMs = 0.0f;
        }
    }

    ImGui::End();
}

void ClockDemo::LogSummary() const
{
    const hyoshi::rhythm::SongClock& clock = player.GetClock();
    const hyoshi::audio::AudioStats stats = audio.GetStats();
    HYOSHI_LOG_INFO("Clock: song {:.3f} s, {} resyncs, {} backward steps, worst raw-filtered {:.2f} ms",
                    ToSeconds(clock.GetSongTime()), clock.GetResyncCount(), player.GetBackwardSteps(),
                    static_cast<double>(worstRawErrorMs));
    HYOSHI_LOG_INFO("Audio: {} callbacks, longest gap {:.2f} ms, {} late and {} dropped voices, {} route changes",
                    stats.Callbacks, static_cast<double>(stats.MaxCallbackGapNs) / 1.0e6, stats.LateVoices,
                    stats.DroppedVoices, stats.RouteChanges);
}

} // namespace hyoshi::app
