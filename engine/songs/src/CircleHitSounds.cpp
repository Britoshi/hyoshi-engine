#include "songs/CircleHitSounds.h"

#include "DefaultHitSounds.h"

#include "audio/AudioDecoder.h"
#include "core/Log.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <utility>

namespace hyoshi::songs
{

namespace
{

namespace fs = std::filesystem;
using detail::HitSoundName;

// Autoplay schedules its sounds this far past the earliest frame it can.
constexpr SongTimeUs AUTOPLAY_AHEAD_US = 100'000;
// In play, a followed slider's parts are scheduled this far past it: enough to last until the
// next frame's update at 50 frames a second and up.
constexpr SongTimeUs FOLLOW_AHEAD_US = 20'000;
// A slider's parts from here on are judged by following (the head, part 1, by a press).
constexpr uint32_t FIRST_FOLLOWED_PART = 2;
constexpr std::array<const char*, 3> EXTENSIONS{".wav", ".ogg", ".mp3"};

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A folder's files by lowercase name, so a map's Soft-HitClap.WAV counts too.
using FileList = std::unordered_map<std::string, std::string>;

FileList ListFiles(const std::string& directory)
{
    FileList files;
    if (directory.empty())
    {
        return files;
    }
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(directory), error))
    {
        std::error_code entryError;
        if (entry.is_regular_file(entryError))
        {
            files.emplace(ToLower(entry.path().filename().string()), entry.path().string());
        }
    }
    return files;
}

// The first name (without its extension) that's a sound file in the folder.
const std::string* FindSoundFile(const FileList& files, const std::vector<std::string>& stems)
{
    for (const std::string& stem : stems)
    {
        for (const char* extension : EXTENSIONS)
        {
            if (const auto file = files.find(stem + extension); file != files.end())
            {
                return &file->second;
            }
        }
    }
    return nullptr;
}

} // namespace

void CircleHitSounds::Load(audio::IAudioBackend& audioBackend, const circle::CircleChart& chart,
                           const std::string& chartDirectory, const std::string& skinDirectory,
                           audio::SoundId firstSound)
{
    Clear(audioBackend);
    const uint32_t rate = audio->GetSampleRate();
    const FileList chartFiles = ListFiles(chartDirectory);
    const FileList skinFiles = ListFiles(skinDirectory);
    uint32_t fromChart = 0;
    uint32_t fromSkin = 0;
    uint32_t generated = 0;

    const auto addSound = [&](audio::PcmBuffer pcm) -> int32_t
    {
        const audio::SoundId id = firstSound + sounds.size();
        if (Result<void> result = audio->LoadSound(id, std::move(pcm)); !result)
        {
            HYOSHI_LOG_WARN("Hit sounds: {}", result.GetError().Message);
            return SILENT;
        }
        sounds.push_back(id);
        return static_cast<int32_t>(sounds.size() - 1);
    };
    // An empty or unreadable file is silence.
    const auto loadFile = [&](const std::string& path) -> int32_t
    {
        Result<std::vector<std::byte>> bytes = platform::LoadFile(path);
        if (!bytes || bytes.Value().empty())
        {
            return SILENT;
        }
        Result<audio::PcmBuffer> pcm = audio::DecodeAudio(bytes.Value(), rate);
        if (!pcm || pcm.Value().GetFrameCount() == 0)
        {
            HYOSHI_LOG_WARN("Hit sounds: can't read {}, so it's silent", path);
            return SILENT;
        }
        return addSound(std::move(pcm).Value());
    };

    // The skin's sound, else the generated one, by name (soft-hitclap).
    std::map<std::string, int32_t> fallbacks;
    const auto getFallback = [&](circle::SampleBank bank, HitSoundName name, const std::vector<std::string>& stems)
    {
        if (const auto known = fallbacks.find(stems.back()); known != fallbacks.end())
        {
            return known->second;
        }
        int32_t sound = SILENT;
        if (const std::string* path = FindSoundFile(skinFiles, stems))
        {
            sound = loadFile(*path);
            ++fromSkin;
        }
        else
        {
            sound = addSound(detail::MakeDefaultHitSound(bank, name, rate));
            ++generated;
        }
        fallbacks.emplace(stems.back(), sound);
        return sound;
    };
    // A bank's sound for a custom sample index: the chart's own, then the fallback.
    std::map<std::pair<std::string, uint32_t>, int32_t> samples;
    const auto getSample = [&](circle::SampleBank bank, HitSoundName name, uint32_t customIndex)
    {
        const std::string base = std::string(detail::GetBankFileName(bank)) + "-" + detail::GetHitSoundFileName(name);
        const std::pair key{base, customIndex};
        if (const auto known = samples.find(key); known != samples.end())
        {
            return known->second;
        }
        std::vector<std::string> stems;
        if (customIndex >= 2)
        {
            stems.push_back(base + std::to_string(customIndex));
        }
        stems.push_back(base);
        int32_t sound = SILENT;
        const std::string* path = customIndex > 0 ? FindSoundFile(chartFiles, stems) : nullptr;
        if (path != nullptr)
        {
            sound = loadFile(*path);
            ++fromChart;
        }
        else
        {
            sound = getFallback(bank, name, stems);
        }
        samples.emplace(key, sound);
        return sound;
    };
    // A file the chart names, with or without its extension; silence if it isn't there.
    std::map<std::string, int32_t> files;
    const auto getFile = [&](const std::string& file)
    {
        const std::string name = ToLower(file);
        if (const auto known = files.find(name); known != files.end())
        {
            return known->second;
        }
        const auto exact = chartFiles.find(name);
        const std::string* path = exact != chartFiles.end()
                                      ? &exact->second
                                      : FindSoundFile(chartFiles, {fs::path(name).replace_extension().string()});
        const int32_t sound = path != nullptr ? loadFile(*path) : SILENT;
        fromChart += path != nullptr ? 1 : 0;
        files.emplace(name, sound);
        return sound;
    };

    AddCues(chart,
            [&](Cue& cue)
            {
                const circle::ResolvedHitSound sound = circle::GetHitSound(chart, cue.Note, cue.Part);
                const circle::SliderPart* sliderPart = circle::GetSliderPart(chart, cue.Note, cue.Part);
                cue.Volume = sound.Volume;
                size_t layer = 0;
                if (sliderPart != nullptr && sliderPart->Type == circle::SliderPartType::Tick)
                {
                    // A slider that plays a file instead has no tick (osu!'s tick is its normal sound's).
                    if (sound.File.empty())
                    {
                        cue.Layers[layer++] = getSample(sound.Bank, HitSoundName::SliderTick, sound.CustomIndex);
                    }
                }
                else if (!sound.File.empty())
                {
                    cue.Layers[layer++] = getFile(sound.File);
                }
                else
                {
                    // The normal sound always plays, under the additions.
                    cue.Layers[layer++] = getSample(sound.Bank, HitSoundName::HitNormal, sound.CustomIndex);
                    for (const auto& [bit, name] : {std::pair{circle::ADDITION_WHISTLE, HitSoundName::HitWhistle},
                                                    std::pair{circle::ADDITION_FINISH, HitSoundName::HitFinish},
                                                    std::pair{circle::ADDITION_CLAP, HitSoundName::HitClap}})
                    {
                        if ((sound.Additions & bit) != 0)
                        {
                            cue.Layers[layer++] = getSample(sound.AdditionBank, name, sound.CustomIndex);
                        }
                    }
                }
            });
    HYOSHI_LOG_INFO("Hit sounds: {} from the map's folder, {} from the skin, {} generated", fromChart, fromSkin,
                    generated);
}

void CircleHitSounds::LoadUnified(audio::IAudioBackend& audioBackend, const circle::CircleChart& chart,
                                  audio::PcmBuffer sound, audio::SoundId firstSound)
{
    Clear(audioBackend);
    int32_t layer = SILENT;
    if (Result<void> result = audio->LoadSound(firstSound, std::move(sound)); result)
    {
        sounds.push_back(firstSound);
        layer = 0;
    }
    else
    {
        HYOSHI_LOG_WARN("Hit sounds: {}", result.GetError().Message);
    }
    AddCues(chart,
            [&](Cue& cue)
            {
                const circle::SliderPart* sliderPart = circle::GetSliderPart(chart, cue.Note, cue.Part);
                if (sliderPart == nullptr || sliderPart->Type != circle::SliderPartType::Tick)
                {
                    cue.Layers[0] = layer;
                }
            });
}

void CircleHitSounds::Clear(audio::IAudioBackend& audioBackend)
{
    audio = &audioBackend;
    sounds.clear();
    cues.clear();
    order.clear();
    firstCue.clear();
    nextCue = 0;
}

void CircleHitSounds::AddCues(const circle::CircleChart& chart, const std::function<void(Cue&)>& fill)
{
    firstCue.assign(chart.Notes.size(), 0);
    const auto addCue = [&](uint32_t note, uint32_t part, SongTimeUs time)
    {
        Cue cue;
        cue.Time = time;
        cue.Note = note;
        cue.Part = part;
        fill(cue);
        cues.push_back(cue);
    };
    for (uint32_t i = 0; i < chart.Notes.size(); ++i)
    {
        const circle::CircleNote& note = chart.Notes[i];
        firstCue[i] = static_cast<uint32_t>(cues.size());
        if (note.Type == circle::NoteType::Circle)
        {
            addCue(i, 0, note.Time);
        }
        for (uint32_t part = 1; part <= note.PartCount; ++part)
        {
            addCue(i, part, circle::GetSliderPart(chart, i, part)->Time);
        }
    }
    order.resize(cues.size());
    for (uint32_t i = 0; i < cues.size(); ++i)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cues[a].Time < cues[b].Time; });
}

void CircleHitSounds::Reset()
{
    for (Cue& cue : cues)
    {
        cue.State = CueState::Waiting;
        cue.Handles = {};
    }
    nextCue = 0;
}

void CircleHitSounds::Update(const SongPlayer& player, const circle::CircleJudge& judge, bool isAutoplaying,
                             float volume)
{
    if (audio == nullptr || volume <= 0.0f)
    {
        return;
    }
    isAutoplay = isAutoplaying;
    const DspFrame earliest = GetEarliestFrame(player);
    const DspFrame horizon =
        earliest + UsToFrames(isAutoplay ? AUTOPLAY_AHEAD_US : FOLLOW_AHEAD_US, audio->GetSampleRate());
    while (nextCue < order.size())
    {
        Cue& cue = cues[order[nextCue]];
        const DspFrame frame = GetFrame(cue, player);
        if (frame > horizon)
        {
            break;
        }
        ++nextCue;
        // Too late to be on time: if it's hit, OnJudgment plays it at once.
        if (cue.State != CueState::Waiting || frame < earliest)
        {
            continue;
        }
        const bool isFollowed =
            cue.Part >= FIRST_FOLLOWED_PART && judge.IsFollowing(cue.Note) && judge.GetNextPart(cue.Note) <= cue.Part;
        if (isAutoplay || isFollowed)
        {
            Play(cue, volume, frame);
        }
    }
}

void CircleHitSounds::OnJudgment(const rhythm::JudgmentEvent& event, const SongPlayer& player, float volume)
{
    Cue* cue = FindCue(event.NoteIndex, event.Part);
    if (audio == nullptr || cue == nullptr)
    {
        return;
    }
    const CueState state = cue->State;
    cue->State = CueState::Done;
    if (event.Result == rhythm::Judgment::Miss)
    {
        if (state == CueState::Scheduled)
        {
            for (const audio::SoundHandle handle : cue->Handles)
            {
                if (handle != audio::INVALID_SOUND_HANDLE)
                {
                    audio->Stop(handle);
                }
            }
        }
        return;
    }
    if (state != CueState::Waiting || volume <= 0.0f)
    {
        return;
    }
    // Struck: at once. A followed part: on its frame if that's still to come.
    std::optional<DspFrame> frame;
    if (cue->Part >= FIRST_FOLLOWED_PART)
    {
        if (const DspFrame partFrame = GetFrame(*cue, player); partFrame >= GetEarliestFrame(player))
        {
            frame = partFrame;
        }
    }
    Play(*cue, volume, frame);
    cue->State = CueState::Done;
}

CircleHitSounds::Cue* CircleHitSounds::FindCue(uint32_t note, uint32_t part)
{
    if (note >= firstCue.size())
    {
        return nullptr;
    }
    const size_t index = firstCue[note] + (part == 0 ? 0 : part - 1);
    if (index >= cues.size() || cues[index].Note != note || cues[index].Part != part)
    {
        return nullptr;
    }
    return &cues[index];
}

// The audio thread takes commands at the start of each callback, and one may have started since
// the snapshot: two callbacks past the one it reported.
DspFrame CircleHitSounds::GetEarliestFrame(const SongPlayer& player)
{
    return player.GetRenderCursor() + (2 * player.GetSnapshot().CallbackFrames);
}

DspFrame CircleHitSounds::GetFrame(const Cue& cue, const SongPlayer& player) const
{
    if (isAutoplay)
    {
        return UsToFrames(cue.Time + player.GetClock().ChartOffsetUs, audio->GetSampleRate());
    }
    return player.GetClock().MusicFrameForSongTime(cue.Time, audio->GetSampleRate());
}

void CircleHitSounds::Play(Cue& cue, float volume, std::optional<DspFrame> frame)
{
    for (size_t layer = 0; layer < MAX_LAYERS; ++layer)
    {
        const int32_t sound = cue.Layers[layer];
        if (sound == SILENT)
        {
            continue;
        }
        const audio::SoundId id = sounds[static_cast<size_t>(sound)];
        const float gain = cue.Volume * volume;
        cue.Handles[layer] = frame ? audio->PlayAtMusicFrame(id, audio::Bus::HitSounds, *frame, gain)
                                   : audio->Play(id, audio::Bus::HitSounds, gain);
    }
    cue.State = frame ? CueState::Scheduled : CueState::Done;
}

} // namespace hyoshi::songs
