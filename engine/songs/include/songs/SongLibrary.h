#pragma once

#include "songs/Charts.h"

#include "core/JobSystem.h"
#include "core/Time.h"
#include "rhythm/Chart.h"

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace hyoshi::songs
{

// One playable difficulty.
struct LibraryChart
{
    ChartSource Source;
    hyoshi::rhythm::ChartMetadata Metadata;
    std::string Key;
    uint32_t LaneCount = 0;
    uint32_t NoteCount = 0;
    // The chart's modes ("mania", "arcaea", "phigros"), as its file lists them.
    std::vector<std::string> Modes;
    hyoshi::SongTimeUs LengthUs = 0;
    double Bpm = 0.0;
    // Empty for the built-in chart, whose music is generated. Difficulties of one song can have
    // their own audio (map sets with rate-changed copies).
    std::string AudioPath;
    // Where the song select's preview starts.
    hyoshi::SongTimeUs PreviewUs = 0;
    // The chart's background image, or empty.
    std::string BackgroundPath;
};

// One song: the difficulties of one map set with the same artist and title.
struct LibrarySong
{
    std::string Title;
    std::string Artist;
    // Easiest first, by note count.
    std::vector<LibraryChart> Charts;
};

// Reads one chart file for the library: its details, or nothing if it isn't a chart the game
// plays. Runs on a worker, so it only reads the file.
using ChartReader = std::optional<LibraryChart> (*)(const std::string& path);

// The engine's readers: Mania charts (.rchart.json of the lane and line modes, osu!mania .osu, and
// Phigros packages' info files: LoadMixedChartFile), and circle charts (osu!standard .osu).
std::optional<LibraryChart> ReadManiaChart(const std::string& path);
std::optional<LibraryChart> ReadCircleChart(const std::string& path);

// What became of one archive given to SongLibrary::Import.
struct ImportResult
{
    std::string Archive;
    // The map folder it unpacked into; empty if it failed.
    std::string Folder;
    // The folder was already there, and was left alone.
    bool WasAlreadyThere = false;
    std::string Error;
};

struct LibraryOptions
{
    ChartReader Reader = ReadManiaChart;
    // The built-in Mania chart, listed last.
    bool HasBuiltInChart = true;
};

// The songs in the song folders: map folders holding chart files (.osu or .rchart.json), and
// charts directly in a song folder, read with the options' reader. Scans run on a worker.
class SongLibrary
{
public:
    explicit SongLibrary(hyoshi::JobSystem& jobs, LibraryOptions options = {});

    void SetFolders(std::vector<std::string> folders);

    const std::vector<std::string>& GetFolders() const
    {
        return folders;
    }

    // Starts scanning the folders again.
    void Rescan();

    // Unpacks osu! map sets (.osz) into their own folders in destination on a worker, then
    // rescans; see UnpackOsz. Archives given while an import runs wait for it.
    void Import(std::vector<std::string> archives, std::string destination);

    bool IsImporting() const
    {
        return pendingImport.valid() || !queuedArchives.empty();
    }

    // The results of finished imports, once each.
    std::vector<ImportResult> TakeImportResults();

    // Once per frame: takes a finished scan or import.
    void Update();

    bool IsScanning() const
    {
        return pendingScan.valid();
    }

    // Sorted by artist and title, with the built-in chart (if the options have it) last.
    const std::vector<LibrarySong>& GetSongs() const
    {
        return songs;
    }

    // Counts finished scans, so the song select knows when to refresh.
    uint32_t GetVersion() const
    {
        return version;
    }

    // Finds the difficulty with this chart path ("" is the built-in chart).
    bool Find(const std::string& path, size_t& songIndex, size_t& chartIndex) const;

private:
    hyoshi::JobSystem& jobs;
    LibraryOptions options;
    std::vector<std::string> folders;
    std::vector<LibrarySong> songs;
    std::future<std::vector<LibrarySong>> pendingScan;
    std::future<std::vector<ImportResult>> pendingImport;
    std::vector<std::string> queuedArchives;
    std::string queuedDestination;
    std::vector<ImportResult> importResults;
    uint32_t version = 0;
};

// Scans the folders; exposed for tests.
std::vector<LibrarySong> ScanSongFolders(const std::vector<std::string>& folders, const LibraryOptions& options = {});

// Debug APKs carry the maps from content/charts as assets in dev-songs/, listed with their sizes
// in dev-songs/index.txt (platforms/android/app/build.gradle). Assets aren't files the library
// can scan, so this copies them into a folder, skipping files already there at the same size.
// Returns false if there's no index: release builds, and desktops, which read content/charts.
bool UnpackBundledSongs(const std::string& assetFolder, const std::string& destination);

} // namespace hyoshi::songs
