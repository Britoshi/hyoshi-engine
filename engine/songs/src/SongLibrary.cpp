#include "songs/SongLibrary.h"

#include "songs/Charts.h"

#include "core/Log.h"
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

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsChartFile(const fs::path& path)
{
    const std::string name = ToLower(path.filename().string());
    return name.ends_with(".osu") || name.ends_with(".rchart.json");
}

struct Found
{
    LibraryChart Chart;
    // The folder the chart is in: charts of one map set share it.
    std::string Folder;
    bool IsOsu = false;
};

void AddChart(const fs::path& path, ChartReader reader, std::vector<Found>& found)
{
    const std::string pathText = path.string();
    std::optional<LibraryChart> chart = reader(pathText);
    if (!chart)
    {
        return;
    }
    Found entry;
    entry.Chart = std::move(*chart);
    entry.Folder = path.parent_path().string();
    entry.IsOsu = ToLower(pathText).ends_with(".osu");
    found.push_back(std::move(entry));
}

void ScanFolder(const fs::path& folder, ChartReader reader, std::vector<Found>& found)
{
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
    {
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && IsChartFile(entry.path()))
        {
            AddChart(entry.path(), reader, found);
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

std::optional<LibraryChart> ReadManiaChart(const std::string& path)
{
    hyoshi::Result<LoadedChart> loaded = LoadChartFile(path);
    if (!loaded)
    {
        // Other osu! modes share the file extension; they're simply not Mania charts.
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, loaded.GetError().Message);
        return std::nullopt;
    }
    const hyoshi::mania::ManiaChart& chart = loaded.Value().Chart;
    LibraryChart entry =
        MakeLibraryChart(path, chart.Info, loaded.Value().Directory, static_cast<uint32_t>(chart.Notes.size()),
                         GetChartEnd(chart), GetMainBpm(chart));
    entry.LaneCount = chart.LaneCount;
    return entry;
}

std::optional<LibraryChart> ReadCircleChart(const std::string& path)
{
    hyoshi::Result<LoadedCircleChart> loaded = LoadCircleChartFile(path);
    if (!loaded)
    {
        HYOSHI_LOG_DEBUG("Library: skipping {}: {}", path, loaded.GetError().Message);
        return std::nullopt;
    }
    const hyoshi::circle::CircleChart& chart = loaded.Value().Chart;
    LibraryChart entry =
        MakeLibraryChart(path, chart.Info, loaded.Value().Directory, static_cast<uint32_t>(chart.Notes.size()),
                         GetChartEnd(chart), GetMainBpm(chart));
    if (!chart.BackgroundFile.empty())
    {
        entry.BackgroundPath = loaded.Value().Directory + chart.BackgroundFile;
    }
    return entry;
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
        // Charts directly in the song folder, then one level of map folders.
        ScanFolder(folder, options.Reader, found);
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
        {
            std::error_code entryError;
            if (entry.is_directory(entryError))
            {
                ScanFolder(entry.path(), options.Reader, found);
            }
        }
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

    // One file per line: its size, a tab, and its path under dev-songs/.
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

void SongLibrary::Update()
{
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
