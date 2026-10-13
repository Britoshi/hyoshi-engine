#include "songs/SongLibrary.h"

#include "songs/Charts.h"
#include "songs/Osz.h"

#include "core/Log.h"
#include "mania/StepManiaImport.h"
#include "osu/OsuInfo.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>

namespace hyoshi::songs
{

namespace
{

namespace fs = std::filesystem;

// Without a preview time, previews start this far into the song.
constexpr double DEFAULT_PREVIEW_FRACTION = 0.4;
// .osu files' Mode.
constexpr int STANDARD_OSU_MODE = 0;
constexpr int MANIA_OSU_MODE = 3;

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsChartFile(const fs::path& path, const LibraryOptions& options)
{
    const std::string name = ToLower(path.filename().string());
    return name.ends_with(".osu") || name.ends_with(".rchart.json") || name.ends_with(".sm") ||
           name.ends_with(".ssc") || name.ends_with(".medley.json") || name == "info.yml" || name == "info.txt" ||
           std::any_of(options.MoreChartFiles.begin(), options.MoreChartFiles.end(),
                       [&name](const std::string& ending) { return name.ends_with(ending); });
}

// Map sets and StepMania packs: a song folder's charts can be this many folders down.
constexpr int MAX_FOLDER_DEPTH = 3;

struct Found
{
    LibraryChart Chart;
    // The folder the chart is in: charts of one map set share it.
    std::string Folder;
    bool IsOsu = false;
};

void AddChart(const fs::path& path, const fs::path& songFolder, ChartReader reader, std::vector<Found>& found)
{
    const std::string pathText = path.string();
    // The first folder under the song folder, if the chart is in one.
    const fs::path relative = path.parent_path().lexically_relative(songFolder);
    const std::string pack = relative.empty() || *relative.begin() == "." ? "" : relative.begin()->string();
    for (LibraryChart& chart : reader(pathText))
    {
        Found entry;
        entry.Chart = std::move(chart);
        entry.Chart.Pack = pack;
        entry.Folder = path.parent_path().string();
        entry.IsOsu = ToLower(pathText).ends_with(".osu");
        found.push_back(std::move(entry));
    }
}

// The folder's charts, then its folders' (down to `depth` more levels).
void ScanFolder(const fs::path& folder, const fs::path& songFolder, const LibraryOptions& options, int depth,
                std::vector<Found>& found)
{
    std::error_code error;
    std::vector<fs::path> subfolders;
    // A folder with a medley lists only its medleys: its other charts are their sources.
    bool hasMedley = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
    {
        std::error_code entryError;
        hasMedley = hasMedley || (entry.is_regular_file(entryError) && IsMedleyFile(entry.path().string()));
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
    {
        std::error_code entryError;
        if (entry.is_directory(entryError))
        {
            subfolders.push_back(entry.path());
            continue;
        }
        if (!entry.is_regular_file(entryError) || !IsChartFile(entry.path(), options) ||
            (hasMedley && !IsMedleyFile(entry.path().string())))
        {
            continue;
        }
        const std::string name = ToLower(entry.path().filename().string());
        // Phigros packages often have both info files; info.yml (Phira's) says more. StepMania
        // writes a song as both .ssc and .sm; the .ssc says more.
        fs::path ssc = entry.path();
        ssc.replace_extension(".ssc");
        if ((name == "info.txt" && fs::exists(entry.path().parent_path() / "info.yml", entryError)) ||
            (name.ends_with(".sm") && fs::exists(ssc, entryError)))
        {
            continue;
        }
        AddChart(entry.path(), songFolder, options.Reader, found);
    }
    if (depth > 0)
    {
        std::sort(subfolders.begin(), subfolders.end());
        for (const fs::path& subfolder : subfolders)
        {
            ScanFolder(subfolder, songFolder, options, depth - 1, found);
        }
    }
}

// What every reader fills in the same way.
LibraryChart MakeLibraryChart(const std::string& path, const hyoshi::rhythm::Chart& info, const std::string& directory,
                              uint32_t noteCount, hyoshi::SongTimeUs lengthUs, double bpm)
{
    LibraryChart chart;
    chart.Source.Path = path;
    chart.Metadata = info.Metadata;
    chart.Key = GetChartKey(info.Metadata);
    chart.NoteCount = noteCount;
    chart.LengthUs = lengthUs;
    chart.Bpm = bpm;
    chart.AudioPath = directory + info.AudioFile;
    chart.PreviewUs = info.Metadata.PreviewStartUs > 0
                          ? info.Metadata.PreviewStartUs
                          : static_cast<hyoshi::SongTimeUs>(static_cast<double>(lengthUs) * DEFAULT_PREVIEW_FRACTION);
    return chart;
}

LibrarySong MakeDemoSong()
{
    LibrarySong song;
    hyoshi::Result<hyoshi::mania::ManiaChart> demo = MakeDemoChart(DEFAULT_DEMO_BEATS);
    if (!demo)
    {
        return song;
    }
    LibraryChart chart;
    chart.Source.DemoBeats = DEFAULT_DEMO_BEATS;
    chart.Metadata = demo.Value().Info.Metadata;
    chart.Key = GetChartKey(chart.Metadata);
    chart.LaneCount = demo.Value().LaneCount;
    chart.Modes = {"mania"};
    chart.NoteCount = static_cast<uint32_t>(demo.Value().Notes.size());
    chart.LengthUs = GetChartEnd(demo.Value());
    chart.Bpm = DEMO_BPM;
    chart.PreviewUs = chart.Metadata.PreviewStartUs;
    song.Title = chart.Metadata.Title;
    song.Artist = chart.Metadata.Artist;
    song.Charts.push_back(std::move(chart));
    return song;
}

} // namespace

namespace
{

std::string GetDirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// The file's text, or empty (with a note in the log) if it can't be read.
std::string ReadText(const std::string& path)
{
    hyoshi::Result<std::vector<std::byte>> bytes = hyoshi::platform::LoadFile(path);
    if (!bytes)
    {
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, bytes.GetError().Message);
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size()};
}

// An .osu file as osu!mania lanes or osu!standard circles, whichever it is and is wanted.
std::vector<LibraryChart> ReadOsuChart(const std::string& path, bool isManiaWanted, bool isCircleWanted)
{
    hyoshi::Result<hyoshi::osu::OsuInfo> read = hyoshi::osu::ReadOsuInfo(ReadText(path));
    if (!read)
    {
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, read.GetError().Message);
        return {};
    }
    const hyoshi::osu::OsuInfo& info = read.Value();
    const bool isMania = info.Mode == MANIA_OSU_MODE && isManiaWanted && info.KeyCount >= 1 &&
                         info.KeyCount <= static_cast<int>(hyoshi::mania::MAX_LANES);
    const bool isCircle = info.Mode == STANDARD_OSU_MODE && isCircleWanted;
    if (!isMania && !isCircle)
    {
        // Other osu! modes share the file extension; they're simply not charts here.
        HYOSHI_LOG_DEBUG("Library: skipping {}: osu! mode {}, {} keys", path, info.Mode, info.KeyCount);
        return {};
    }
    hyoshi::rhythm::Chart details;
    details.Metadata = info.Metadata;
    details.AudioFile = info.AudioFile;
    const std::string directory = GetDirectoryOf(path);
    LibraryChart entry = MakeLibraryChart(path, details, directory, info.HitObjectCount, info.EndUs, info.Bpm);
    entry.LaneCount = isMania ? static_cast<uint32_t>(info.KeyCount) : 0;
    entry.Modes = {isMania ? "mania" : "circle"};
    if (!info.BackgroundFile.empty())
    {
        entry.BackgroundPath = directory + info.BackgroundFile;
    }
    entry.Tags = info.Tags + (info.Source.empty() ? "" : " " + info.Source);
    return {std::move(entry)};
}

// A StepMania file's charts, each named by its own chart path.
std::vector<LibraryChart> ReadStepManiaCharts(const std::string& path)
{
    hyoshi::Result<hyoshi::mania::StepManiaInfo> read = hyoshi::mania::ReadStepManiaInfo(ReadText(path));
    if (!read)
    {
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, read.GetError().Message);
        return {};
    }
    const std::string directory = GetDirectoryOf(path);
    std::vector<LibraryChart> entries;
    for (size_t i = 0; i < read.Value().Charts.size(); ++i)
    {
        const hyoshi::mania::StepManiaChartInfo& chart = read.Value().Charts[i];
        SongTimeUs end = 0;
        for (const hyoshi::rhythm::ChartNote& note : chart.Info.Notes)
        {
            end = std::max(end, note.EndTime);
        }
        LibraryChart entry =
            MakeLibraryChart(MakeStepManiaChartPath(path, i), chart.Info, directory,
                             static_cast<uint32_t>(chart.Info.Notes.size()), end, GetMainBpm(chart.Info, end));
        entry.LaneCount = chart.LaneCount;
        entry.Modes = {"mania"};
        if (!read.Value().BackgroundFile.empty())
        {
            entry.BackgroundPath = directory + read.Value().BackgroundFile;
        }
        entry.Tags = read.Value().Genre;
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Charts LoadMixedChartFile reads, loaded in full: the game's own, Phigros packages, and medleys.
std::vector<LibraryChart> LoadMixedChart(const std::string& path)
{
    hyoshi::Result<LoadedMixedChart> loaded = LoadMixedChartFile(path);
    if (!loaded)
    {
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, loaded.GetError().Message);
        return {};
    }
    const LoadedMixedChart& chart = loaded.Value();
    LibraryChart entry =
        MakeLibraryChart(path, chart.Info, chart.Directory, chart.GetNoteCount(), chart.GetEnd(), GetMainBpm(chart));
    entry.LaneCount = chart.Mania ? chart.Mania->LaneCount : (chart.Arcaea ? chart.Arcaea->LaneCount : 0);
    entry.Modes = chart.Info.Modes;
    if (!chart.BackgroundFile.empty())
    {
        entry.BackgroundPath = chart.Directory + chart.BackgroundFile;
    }
    return {std::move(entry)};
}

} // namespace

std::vector<LibraryChart> ReadManiaChart(const std::string& path)
{
    if (ToLower(path).ends_with(".osu"))
    {
        return ReadOsuChart(path, true, false);
    }
    return IsStepManiaFile(path) ? ReadStepManiaCharts(path) : LoadMixedChart(path);
}

std::vector<LibraryChart> ReadCircleChart(const std::string& path)
{
    return ToLower(path).ends_with(".osu") ? ReadOsuChart(path, false, true) : std::vector<LibraryChart>{};
}

std::vector<LibraryChart> ReadAnyChart(const std::string& path)
{
    return ToLower(path).ends_with(".osu") ? ReadOsuChart(path, true, true) : ReadManiaChart(path);
}

std::vector<LibrarySong> ScanSongFolders(const std::vector<std::string>& folders, const LibraryOptions& options)
{
    std::vector<Found> found;
    for (const std::string& folderText : folders)
    {
        const fs::path folder(folderText);
        std::error_code error;
        if (!fs::is_directory(folder, error))
        {
            continue;
        }
        ScanFolder(folder, folder, options, MAX_FOLDER_DEPTH, found);
    }

    // The same chart can be here twice, as an .osu and as its conversion: keep the .osu.
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.IsOsu > b.IsOsu; });
    std::vector<Found> unique;
    for (Found& entry : found)
    {
        const bool isDuplicate = std::any_of(unique.begin(), unique.end(),
                                             [&entry](const Found& kept) { return kept.Chart.Key == entry.Chart.Key; });
        if (!isDuplicate)
        {
            unique.push_back(std::move(entry));
        }
    }

    // A song is a map set's charts with the same artist and title, like osu! groups them. Rate-
    // changed copies (their own audio files) stay with the original.
    std::map<std::string, LibrarySong> bySong;
    for (Found& entry : unique)
    {
        const std::string key = ToLower(entry.Folder) + "|" + ToLower(entry.Chart.Metadata.Artist) + "|" +
                                ToLower(entry.Chart.Metadata.Title);
        LibrarySong& song = bySong[key];
        if (song.Charts.empty())
        {
            song.Title = entry.Chart.Metadata.Title;
            song.Artist = entry.Chart.Metadata.Artist;
        }
        song.Charts.push_back(std::move(entry.Chart));
    }

    std::vector<LibrarySong> songs;
    for (auto& [key, song] : bySong)
    {
        std::sort(song.Charts.begin(), song.Charts.end(),
                  [](const LibraryChart& a, const LibraryChart& b)
                  {
                      if (a.NoteCount != b.NoteCount)
                      {
                          return a.NoteCount < b.NoteCount;
                      }
                      if (a.LaneCount != b.LaneCount)
                      {
                          return a.LaneCount < b.LaneCount;
                      }
                      return a.Metadata.DifficultyName < b.Metadata.DifficultyName;
                  });
        songs.push_back(std::move(song));
    }
    std::sort(songs.begin(), songs.end(),
              [](const LibrarySong& a, const LibrarySong& b)
              {
                  const std::string artistA = ToLower(a.Artist);
                  const std::string artistB = ToLower(b.Artist);
                  return artistA != artistB ? artistA < artistB : ToLower(a.Title) < ToLower(b.Title);
              });

    if (options.HasBuiltInChart)
    {
        LibrarySong demo = MakeDemoSong();
        if (!demo.Charts.empty())
        {
            songs.push_back(std::move(demo));
        }
    }
    return songs;
}

bool UnpackBundledSongs(const std::string& assetFolder, const std::string& destination)
{
    const hyoshi::Result<std::vector<std::byte>> index = hyoshi::platform::LoadFile(assetFolder + "index.txt");
    if (!index)
    {
        return false;
    }

    // One file per line: its size, a tab, and its path under the asset folder.
    const std::string_view text(reinterpret_cast<const char*>(index.Value().data()), index.Value().size());
    uint32_t copied = 0;
    uint32_t kept = 0;
    uint32_t failed = 0;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const size_t tab = line.find('\t');
        uintmax_t size = 0;
        if (tab == std::string_view::npos || std::from_chars(line.data(), line.data() + tab, size).ec != std::errc{})
        {
            continue;
        }
        const std::string relative(line.substr(tab + 1));
        const fs::path target(destination + "/" + relative);

        std::error_code error;
        if (fs::file_size(target, error) == size && !error)
        {
            ++kept;
            continue;
        }
        const hyoshi::Result<std::vector<std::byte>> contents = hyoshi::platform::LoadFile(assetFolder + relative);
        hyoshi::Result<void> result = contents ? hyoshi::platform::MakeDirectory(target.parent_path().string())
                                               : hyoshi::Result<void>(contents.GetError());
        if (result)
        {
            result = hyoshi::platform::SaveFile(target.string(), contents.Value());
        }
        if (!result)
        {
            HYOSHI_LOG_WARN("Bundled songs: {}", result.GetError().Message);
            ++failed;
            continue;
        }
        HYOSHI_LOG_INFO("Bundled songs: copied '{}' ({} bytes)", relative, contents.Value().size());
        ++copied;
    }
    HYOSHI_LOG_INFO("Bundled songs: {} files copied, {} already there, {} failed, in {}", copied, kept, failed,
                    destination);
    return true;
}

SongLibrary::SongLibrary(hyoshi::JobSystem& jobSystem, LibraryOptions libraryOptions)
    : jobs(jobSystem), options(libraryOptions)
{
}

void SongLibrary::SetFolders(std::vector<std::string> songFolders)
{
    folders = std::move(songFolders);
}

void SongLibrary::Rescan()
{
    pendingScan = jobs.Submit([scanFolders = folders, scanOptions = options]
                              { return ScanSongFolders(scanFolders, scanOptions); });
}

void SongLibrary::Import(std::vector<std::string> archives, std::string destination)
{
    queuedArchives.insert(queuedArchives.end(), archives.begin(), archives.end());
    queuedDestination = std::move(destination);
    if (pendingImport.valid() || queuedArchives.empty())
    {
        return;
    }
    pendingImport = jobs.Submit(
        [batch = std::exchange(queuedArchives, {}), folder = queuedDestination]
        {
            std::vector<ImportResult> results;
            for (const std::string& archive : batch)
            {
                ImportResult& result = results.emplace_back();
                result.Archive = archive;
                if (Result<UnpackedOsz> unpacked = UnpackOsz(archive, folder))
                {
                    result.Folder = std::move(unpacked.Value().Folder);
                    result.WasAlreadyThere = unpacked.Value().WasAlreadyThere;
                }
                else
                {
                    result.Error = unpacked.GetError().Message;
                }
            }
            return results;
        });
}

std::vector<ImportResult> SongLibrary::TakeImportResults()
{
    return std::exchange(importResults, {});
}

void SongLibrary::Update()
{
    if (hyoshi::IsReady(pendingImport))
    {
        for (ImportResult& result : pendingImport.get())
        {
            if (!result.Error.empty())
            {
                HYOSHI_LOG_WARN("Import failed: {}", result.Error);
            }
            else if (result.WasAlreadyThere)
            {
                HYOSHI_LOG_INFO("Not importing '{}': '{}' is already there", result.Archive, result.Folder);
            }
            else
            {
                HYOSHI_LOG_INFO("Imported '{}' into '{}'", result.Archive, result.Folder);
            }
            importResults.push_back(std::move(result));
        }
        Rescan();
        // Archives dropped meanwhile.
        Import({}, queuedDestination);
    }

    if (!hyoshi::IsReady(pendingScan))
    {
        return;
    }
    songs = pendingScan.get();
    ++version;
    size_t chartCount = 0;
    for (const LibrarySong& song : songs)
    {
        chartCount += song.Charts.size();
    }
    HYOSHI_LOG_INFO("Library: {} songs, {} charts", songs.size(), chartCount);
}

bool SongLibrary::Find(const std::string& path, size_t& songIndex, size_t& chartIndex) const
{
    for (size_t s = 0; s < songs.size(); ++s)
    {
        for (size_t c = 0; c < songs[s].Charts.size(); ++c)
        {
            if (songs[s].Charts[c].Source.Path == path)
            {
                songIndex = s;
                chartIndex = c;
                return true;
            }
        }
    }
    return false;
}

} // namespace hyoshi::songs
