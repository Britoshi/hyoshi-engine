#include "osu/OsuManiaImporter.h"

#include <doctest/doctest.h>

#include <string>

namespace
{

// A minimal 4K osu!mania beatmap: 120 BPM, a 0.5x SV section, a BPM change to 240, taps in every
// lane, a hold, and one note overlapping a hold in its lane.
const char* const OSU_4K = "osu file format v14\n"
                           "\n"
                           "[General]\n"
                           "AudioFilename: audio.mp3\n"
                           "PreviewTime: 5000\n"
                           "Mode: 3\n"
                           "\n"
                           "[Metadata]\n"
                           "Title:Test Song\n"
                           "Artist:Test Artist\n"
                           "Creator:Someone\n"
                           "Version:4K Normal\n"
                           "\n"
                           "[Difficulty]\n"
                           "CircleSize:4\n"
                           "OverallDifficulty:8\n"
                           "\n"
                           "[TimingPoints]\n"
                           "1000,500,4,1,0,100,1,0\n"
                           "3000,-200,4,1,0,100,0,0\n"
                           "20000,250,3,1,0,100,1,0\n"
                           "\n"
                           "[HitObjects]\n"
                           "64,192,1000,1,0,0:0:0:0:\n"
                           "192,192,1500,1,0,0:0:0:0:\n"
                           "320,192,2000,128,0,2750:0:0:0:0:\n"
                           "448,192,2500,1,0,0:0:0:0:\n"
                           "320,192,2500,1,0,0:0:0:0:\n"
                           "511,192,3000.4,5,0,0:0:0:0:\n";

std::string WithLine(std::string text, const std::string& from, const std::string& to)
{
    const size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    return text.replace(position, from.size(), to);
}

} // namespace

TEST_CASE("osu!mania import: lanes, holds, metadata, timing, and scroll velocity")
{
    hyoshi::Result<hyoshi::osu::OsuImportResult> imported = hyoshi::osu::ImportOsuMania(OSU_4K);
    REQUIRE(imported);
    const hyoshi::mania::ManiaChart& chart = imported.Value().Chart;

    CHECK(chart.LaneCount == 4);
    CHECK(chart.Info.AudioFile == "audio.mp3");
    CHECK(chart.Info.Metadata.Title == "Test Song");
    CHECK(chart.Info.Metadata.Charter == "Someone");
    CHECK(chart.Info.Metadata.DifficultyName == "4K Normal");
    CHECK(chart.Info.Metadata.PreviewStartUs == 5'000'000);

    // The note at 2500 in lane 2 starts inside the hold, so it is dropped with a warning.
    REQUIRE(chart.Notes.size() == 5);
    REQUIRE(imported.Value().Warnings.size() == 1);
    CHECK(imported.Value().Warnings[0].find("lane 2") != std::string::npos);

    CHECK(chart.Notes[0].Lane == 0);
    CHECK(chart.Notes[1].Lane == 1);
    CHECK(chart.Notes[2].Lane == 2);
    CHECK(chart.Notes[2].IsHold());
    CHECK(chart.Notes[2].Time == 2'000'000);
    CHECK(chart.Notes[2].EndTime == 2'750'000);
    CHECK(chart.Notes[3].Lane == 3);
    // x = 511 is still the last lane; fractional milliseconds round to microseconds.
    CHECK(chart.Notes[4].Lane == 3);
    CHECK(chart.Notes[4].Time == 3'000'400);

    REQUIRE(chart.Info.Timing.size() == 2);
    CHECK(chart.Info.Timing[0].Bpm == doctest::Approx(120.0));
    CHECK(chart.Info.Timing[1].Bpm == doctest::Approx(240.0));
    CHECK(chart.Info.Timing[1].BeatsPerBar == 3);

    // 120 BPM covers the most time, so it scrolls at 1x; -200 is 0.5x; 240 BPM doubles speed.
    REQUIRE(chart.Info.ScrollVelocity.size() == 3);
    CHECK(chart.Info.ScrollVelocity[0].Multiplier == doctest::Approx(1.0));
    CHECK(chart.Info.ScrollVelocity[1].Time == 3'000'000);
    CHECK(chart.Info.ScrollVelocity[1].Multiplier == doctest::Approx(0.5));
    CHECK(chart.Info.ScrollVelocity[2].Time == 20'000'000);
    CHECK(chart.Info.ScrollVelocity[2].Multiplier == doctest::Approx(2.0));
}

TEST_CASE("osu!mania import: the result survives a write and read")
{
    hyoshi::Result<hyoshi::osu::OsuImportResult> imported = hyoshi::osu::ImportOsuMania(OSU_4K);
    REQUIRE(imported);
    hyoshi::Result<std::string> json = hyoshi::mania::WriteManiaChart(imported.Value().Chart);
    REQUIRE(json);
    hyoshi::Result<hyoshi::mania::ManiaChart> reread = hyoshi::mania::ParseManiaChart(json.Value());
    REQUIRE(reread);
    CHECK(reread.Value().Notes.size() == 5);
    CHECK(reread.Value().Notes[4].ScrollPosition == doctest::Approx(imported.Value().Chart.Notes[4].ScrollPosition));
}

TEST_CASE("osu!mania import: old formats get osu!'s 24 ms offset; other modes are refused")
{
    hyoshi::Result<hyoshi::osu::OsuImportResult> old =
        hyoshi::osu::ImportOsuMania(WithLine(OSU_4K, "osu file format v14", "osu file format v4"));
    REQUIRE(old);
    CHECK(old.Value().Chart.Notes[0].Time == 1'024'000);

    hyoshi::Result<hyoshi::osu::OsuImportResult> standard =
        hyoshi::osu::ImportOsuMania(WithLine(OSU_4K, "Mode: 3", "Mode: 0"));
    REQUIRE_FALSE(standard);
    CHECK(standard.GetError().Message.find("osu!mania") != std::string::npos);

    CHECK_FALSE(hyoshi::osu::ImportOsuMania(WithLine(OSU_4K, "CircleSize:4", "CircleSize:18")));
}

TEST_CASE("osu!mania import: notes keep their custom hit sounds")
{
    std::string osu = WithLine(OSU_4K, "3000,-200,4,1,0,100,0,0", "3000,-200,4,1,0,60,0,0");
    osu = WithLine(osu, "64,192,1000,1,0,0:0:0:0:", "64,192,1000,1,0,0:0:0:0:Kick.wav");
    osu = WithLine(osu, "192,192,1500,1,0,0:0:0:0:", "192,192,1500,1,8,1:2:0:80:Kick.wav");
    osu = WithLine(osu, "320,192,2000,128,0,2750:0:0:0:0:", "320,192,2000,128,0,2750:0:0:0:0:Pad 1.wav");
    osu = WithLine(osu, "511,192,3000.4,5,0,0:0:0:0:", "511,192,3000.4,5,0,0:0:0:0:Kick.wav");

    hyoshi::Result<hyoshi::osu::OsuImportResult> imported = hyoshi::osu::ImportOsuMania(osu);
    REQUIRE(imported);
    const hyoshi::mania::ManiaChart& chart = imported.Value().Chart;
    REQUIRE(chart.Info.Samples.size() == 2);
    CHECK(chart.Info.Samples[0] == "Kick.wav");
    CHECK(chart.Info.Samples[1] == "Pad 1.wav");

    REQUIRE(chart.Notes.size() == 5);
    // No volume of its own: the timing point's, 100 here.
    CHECK(chart.Notes[0].Sample == 0);
    CHECK(chart.Notes[0].SampleVolume == 100);
    CHECK(chart.Notes[1].Sample == 0);
    CHECK(chart.Notes[1].SampleVolume == 80);
    // A hold's hitSample follows its end time.
    CHECK(chart.Notes[2].Sample == 1);
    // No file, no sound of its own, whatever the sample set says.
    CHECK(chart.Notes[3].Sample == hyoshi::rhythm::ChartNote::NO_SAMPLE);
    CHECK(chart.Notes[4].Sample == 0);
    CHECK(chart.Notes[4].SampleVolume == 60);
}
