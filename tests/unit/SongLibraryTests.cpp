#include "songs/Charts.h"
#include "songs/SongLibrary.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

namespace fs = std::filesystem;

std::string MakeOsu(int mode, const std::string& version)
{
    return "osu file format v14\n"
           "\n"
           "[General]\n"
           "AudioFilename: audio.mp3\n"
           "Mode: " +
           std::to_string(mode) +
           "\n"
           "\n"
           "[Metadata]\n"
           "Title:Song\n"
           "Artist:Artist\n"
           "Creator:Someone\n"
           "Version:" +
           version +
           "\n"
           "\n"
           "[Difficulty]\n"
           "CircleSize:4\n"
           "OverallDifficulty:5\n"
           "\n"
           "[Events]\n"
           "0,0,\"bg.png\",0,0\n"
           "\n"
           "[TimingPoints]\n"
           "0,500,4,1,0,100,1,0\n"
           "\n"
           "[HitObjects]\n"
           "64,192,1000,1,0,0:0:0:0:\n"
           "192,192,1500,1,0,0:0:0:0:\n";
}

// A song folder with one map folder holding an osu!mania and an osu!standard difficulty, removed
// afterwards.
class SongFolder
{
public:
    SongFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("hyoshi-library-test-" + std::to_string(stamp));
        const fs::path map = root / "1 Artist - Song";
        fs::create_directories(map);
        std::ofstream(map / "mania.osu") << MakeOsu(3, "4K");
        std::ofstream(map / "standard.osu") << MakeOsu(0, "Normal");
    }
    ~SongFolder()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
    SongFolder(const SongFolder&) = delete;
    SongFolder& operator=(const SongFolder&) = delete;

    std::vector<std::string> GetFolders() const
    {
        return {root.string()};
    }

private:
    fs::path root;
};

} // namespace

TEST_CASE("Song library: the default reader finds Mania charts, and the built-in chart comes last")
{
    const SongFolder folder;
    const std::vector<hyoshi::songs::LibrarySong> songs = hyoshi::songs::ScanSongFolders(folder.GetFolders());
    REQUIRE(songs.size() == 2);
    REQUIRE(songs[0].Charts.size() == 1);
    CHECK(songs[0].Charts[0].Metadata.DifficultyName == "4K");
    CHECK(songs[0].Charts[0].LaneCount == 4);
    CHECK(fs::path(songs[0].Charts[0].BackgroundPath).filename() == "bg.png");
    CHECK(songs[0].Charts[0].Pack == "1 Artist - Song");
    CHECK(songs[0].Charts[0].NoteCount == 2);
    CHECK(songs[1].Charts[0].Source.IsDemo());
}

TEST_CASE("Song library: the circle reader finds osu!standard charts, with their backgrounds")
{
    const SongFolder folder;
    hyoshi::songs::LibraryOptions options;
    options.Reader = hyoshi::songs::ReadCircleChart;
    options.HasBuiltInChart = false;
    const std::vector<hyoshi::songs::LibrarySong> songs = hyoshi::songs::ScanSongFolders(folder.GetFolders(), options);
    REQUIRE(songs.size() == 1);
    REQUIRE(songs[0].Charts.size() == 1);
    const hyoshi::songs::LibraryChart& chart = songs[0].Charts[0];
    CHECK(chart.Metadata.DifficultyName == "Normal");
    CHECK(chart.NoteCount == 2);
    CHECK(chart.LengthUs == 1'500'000);
    CHECK(chart.Bpm == doctest::Approx(120.0));
    CHECK(chart.LaneCount == 0);
    CHECK(fs::path(chart.BackgroundPath).filename() == "bg.png");
    CHECK(fs::path(chart.AudioPath).filename() == "audio.mp3");
}

TEST_CASE("Medleys: a part can play several sources, and circles from several sources merge")
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("hyoshi-medley-test-" + std::to_string(stamp));
    fs::create_directories(folder);
    // Two osu!standard maps, each a circle at 1 s and 1.5 s of its own audio; the second's audio
    // starts half a second into the medley's.
    std::ofstream(folder / "a.osu") << MakeOsu(0, "A");
    std::ofstream(folder / "b.osu") << MakeOsu(0, "B");
    std::ofstream(folder / "song.medley.json") << R"({"medley": 1, "title": "Song", "audio": "audio.mp3",
        "sources": [{"name": "a", "chart": "a.osu"}, {"name": "b", "chart": "b.osu", "audioLagMs": 500}],
        "parts": [{"source": "a", "untilMs": 1200}, {"source": ["a", "b"]}]})";
    hyoshi::Result<hyoshi::songs::LoadedMixedChart> medley =
        hyoshi::songs::LoadMedleyFile((folder / "song.medley.json").string());
    std::error_code error;
    fs::remove_all(folder, error);
    REQUIRE(medley);
    REQUIRE(medley.Value().Parts.size() == 2);
    CHECK(medley.Value().Parts[1].Modes == std::vector<std::string>{"circle"});
    // A's two, and B's two from where it plays (1.5 s and 2 s on the medley's clock).
    REQUIRE(medley.Value().Circles);
    const std::vector<hyoshi::circle::CircleNote>& notes = medley.Value().Circles->Notes;
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].Time == 1'000'000);
    CHECK(notes[1].Time == 1'500'000);
    CHECK(notes[2].Time == 1'500'000);
    CHECK(notes[3].Time == 2'000'000);
    // A's and B's notes due together shine; the others don't.
    CHECK_FALSE(notes[0].IsSimultaneous);
    CHECK(notes[1].IsSimultaneous);
    CHECK(notes[2].IsSimultaneous);
    CHECK_FALSE(notes[3].IsSimultaneous);
}
