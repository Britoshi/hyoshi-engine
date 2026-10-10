#include "songs/Osz.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using hyoshi::songs::IsOszPath;
using hyoshi::songs::ToMapRelativePath;
using hyoshi::songs::UnpackedOsz;
using hyoshi::songs::UnpackOsz;

namespace
{

// A temporary folder, removed afterwards.
class TemporaryFolder
{
public:
    TemporaryFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("hyoshi-osz-test-" + std::to_string(stamp));
        fs::create_directories(root);
    }
    ~TemporaryFolder()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    const fs::path& GetPath() const
    {
        return root;
    }

private:
    fs::path root;
};

using Entries = std::vector<std::pair<std::string, std::string>>;

// Writes a zip archive holding the entries (name, contents).
void WriteZip(const fs::path& path, const Entries& entries)
{
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, contents] : entries)
    {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }
    void* data = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    mz_zip_writer_end(&zip);
    mz_free(data);
}

std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Osz: map sets are recognized by their extension, in any case")
{
    CHECK(IsOszPath("set.osz"));
    CHECK(IsOszPath("/tmp/filedrop/0/123 Artist - Title.OSZ"));
    CHECK(IsOszPath("C:\\Downloads\\set.Osz"));
    CHECK_FALSE(IsOszPath("chart.osu"));
    CHECK_FALSE(IsOszPath("osz"));
    CHECK_FALSE(IsOszPath(".osz"));
}

TEST_CASE("Osz: entry paths stay inside the map folder")
{
    CHECK(ToMapRelativePath("chart.osu") == "chart.osu");
    CHECK(ToMapRelativePath("sb\\bg.png") == "sb/bg.png");
    CHECK(ToMapRelativePath("./sb//bg.png") == "sb/bg.png");
    CHECK_FALSE(ToMapRelativePath("../evil.txt").has_value());
    CHECK_FALSE(ToMapRelativePath("sb/../../evil.txt").has_value());
    CHECK_FALSE(ToMapRelativePath("sb\\..\\..\\evil.txt").has_value());
    CHECK_FALSE(ToMapRelativePath("/etc/evil.txt").has_value());
    CHECK_FALSE(ToMapRelativePath("C:\\evil.txt").has_value());
    CHECK_FALSE(ToMapRelativePath("").has_value());
    CHECK_FALSE(ToMapRelativePath("./").has_value());
}

TEST_CASE("Osz: a map set unpacks into a folder named after the archive")
{
    TemporaryFolder folder;
    const fs::path archive = folder.GetPath() / "123 Artist - Song.osz";
    const fs::path songs = folder.GetPath() / "songs";
    WriteZip(archive, {{"Artist - Song (Mapper) [Normal].osu", "chart"}, {"audio.mp3", "music"}, {"sb/bg.png", "art"}});

    hyoshi::Result<UnpackedOsz> unpacked = UnpackOsz(archive.string(), songs.string());
    REQUIRE(unpacked);
    const fs::path map = songs / "123 Artist - Song";
    CHECK(fs::path(unpacked.Value().Folder) == map);
    CHECK_FALSE(unpacked.Value().WasAlreadyThere);
    CHECK(ReadText(map / "Artist - Song (Mapper) [Normal].osu") == "chart");
    CHECK(ReadText(map / "audio.mp3") == "music");
    CHECK(ReadText(map / "sb" / "bg.png") == "art");
}

TEST_CASE("Osz: a folder already there is left alone")
{
    TemporaryFolder folder;
    const fs::path archive = folder.GetPath() / "Song.osz";
    const fs::path songs = folder.GetPath() / "songs";
    const fs::path map = songs / "Song";
    fs::create_directories(map);
    std::ofstream(map / "chart.osu") << "edited by hand";
    WriteZip(archive, {{"chart.osu", "original"}, {"audio.mp3", "music"}});

    hyoshi::Result<UnpackedOsz> unpacked = UnpackOsz(archive.string(), songs.string());
    REQUIRE(unpacked);
    CHECK(fs::path(unpacked.Value().Folder) == map);
    CHECK(unpacked.Value().WasAlreadyThere);
    CHECK(ReadText(map / "chart.osu") == "edited by hand");
    CHECK_FALSE(fs::exists(map / "audio.mp3"));
}

TEST_CASE("Osz: an archive with a file outside its folder is refused, and writes nothing")
{
    TemporaryFolder folder;
    const fs::path archive = folder.GetPath() / "evil.osz";
    const fs::path songs = folder.GetPath() / "songs";
    WriteZip(archive, {{"chart.osu", "chart"}, {"../escaped.txt", "evil"}});

    hyoshi::Result<UnpackedOsz> unpacked = UnpackOsz(archive.string(), songs.string());
    CHECK_FALSE(unpacked);
    CHECK_FALSE(fs::exists(songs / "evil"));
    CHECK_FALSE(fs::exists(folder.GetPath() / "escaped.txt"));
}

TEST_CASE("Osz: a file that isn't a zip archive fails")
{
    TemporaryFolder folder;
    const fs::path archive = folder.GetPath() / "broken.osz";
    std::ofstream(archive) << "not a zip";
    CHECK_FALSE(UnpackOsz(archive.string(), (folder.GetPath() / "songs").string()));
    CHECK_FALSE(UnpackOsz((folder.GetPath() / "missing.osz").string(), (folder.GetPath() / "songs").string()));
}
