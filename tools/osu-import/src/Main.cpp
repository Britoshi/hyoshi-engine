// hyoshi-osu-import: converts an osu!mania .osu file to a .rchart.json.
//
//     hyoshi-osu-import <input.osu> <output.rchart.json> [--force]
//
// Writes only the output path it is given, and never replaces an existing file without --force.
// An .osz is a zip archive: extract it first and point this at one of its .osu files.

#include "osu/OsuManiaImporter.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

int Fail(const std::string& message)
{
    std::fprintf(stderr, "hyoshi-osu-import: %s\n", message.c_str());
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[])
{
    namespace fs = std::filesystem;

    std::vector<std::string_view> paths;
    bool isForced = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--force")
        {
            isForced = true;
        }
        else
        {
            paths.push_back(argument);
        }
    }
    if (paths.size() != 2)
    {
        std::fprintf(stderr, "usage: hyoshi-osu-import <input.osu> <output.rchart.json> [--force]\n");
        return EXIT_FAILURE;
    }

    const fs::path input(paths[0]);
    const fs::path output(paths[1]);
    if (fs::exists(output) && !isForced)
    {
        return Fail(output.string() + " already exists; pass --force to replace it");
    }

    std::ifstream file(input, std::ios::binary);
    if (!file)
    {
        return Fail("cannot read " + input.string());
    }
    std::stringstream text;
    text << file.rdbuf();

    hyoshi::Result<hyoshi::osu::OsuImportResult> imported = hyoshi::osu::ImportOsuMania(text.str());
    if (!imported)
    {
        return Fail(input.string() + ": " + imported.GetError().Message);
    }
    hyoshi::osu::OsuImportResult& result = imported.Value();
    for (const std::string& warning : result.Warnings)
    {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }

    // The chart names its audio relative to itself; the .osu names it relative to the .osu.
    const fs::path audio = fs::absolute(input).parent_path() / result.Chart.Info.AudioFile;
    const fs::path outputDirectory = fs::absolute(output).parent_path();
    result.Chart.Info.AudioFile = fs::relative(audio, outputDirectory).generic_string();
    if (!fs::exists(audio))
    {
        std::fprintf(stderr, "warning: audio file %s not found\n", audio.string().c_str());
    }
    // Hit sound samples too.
    for (std::string& sample : result.Chart.Info.Samples)
    {
        const fs::path samplePath = fs::absolute(input).parent_path() / sample;
        sample = fs::relative(samplePath, outputDirectory).generic_string();
        if (!fs::exists(samplePath))
        {
            std::fprintf(stderr, "warning: hit sound %s not found\n", samplePath.string().c_str());
        }
    }

    hyoshi::Result<std::string> json = hyoshi::mania::WriteManiaChart(result.Chart);
    if (!json)
    {
        return Fail(json.GetError().Message);
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    out << json.Value();
    if (!out)
    {
        return Fail("cannot write " + output.string());
    }

    std::printf("%s: %zu notes in %u lanes, %zu hit sounds -> %s\n", input.filename().string().c_str(),
                result.Chart.Notes.size(), result.Chart.LaneCount, result.Chart.Info.Samples.size(),
                output.string().c_str());
    return EXIT_SUCCESS;
}
