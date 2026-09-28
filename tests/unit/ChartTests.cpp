#include "mania/ManiaChart.h"
#include "rhythm/ScrollMap.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using hyoshi::mania::ManiaChart;
using hyoshi::rhythm::ScrollMap;
using hyoshi::rhythm::ScrollVelocityPoint;

TEST_CASE("ScrollMap integrates scroll velocity")
{
    const ScrollMap plain;
    CHECK(plain.GetPosition(0) == 0.0);
    CHECK(plain.GetPosition(2'500'000) == doctest::Approx(2.5));
    CHECK(plain.GetPosition(-1'000'000) == doctest::Approx(-1.0));

    // Half speed from 2 s, a stop from 4 s, reverse from 5 s.
    const std::vector<ScrollVelocityPoint> points{{2'000'000, 0.5}, {4'000'000, 0.0}, {5'000'000, -1.0}};
    const ScrollMap map(points);
    CHECK(map.GetPosition(1'000'000) == doctest::Approx(1.0));
    CHECK(map.GetPosition(3'000'000) == doctest::Approx(2.5));
    CHECK(map.GetPosition(4'000'000) == doctest::Approx(3.0));
    CHECK(map.GetPosition(4'900'000) == doctest::Approx(3.0));
    CHECK(map.GetPosition(6'000'000) == doctest::Approx(2.0));

    // A point at time 0 or before sets the starting speed.
    const std::vector<ScrollVelocityPoint> fast{{0, 2.0}};
    CHECK(ScrollMap(fast).GetPosition(1'000'000) == doctest::Approx(2.0));
    CHECK(ScrollMap(fast).GetPosition(-1'000'000) == doctest::Approx(-2.0));
}

namespace
{

const char* const EXAMPLE_CHART = R"({
  "formatVersion": 1,
  "mode": "mania",
  "metadata": {"title": "Example Song", "artist": "Example Artist", "charter": "Britoshi",
               "difficultyName": "Hard", "difficultyValue": 7.5, "previewStartUs": 45000000},
  "audio": {"file": "audio/example.ogg", "offsetUs": 1500},
  "timing": [{"timeUs": 0, "bpm": 180.0, "meter": [4, 4]},
             {"timeUs": 64000000, "bpm": 90.0, "meter": [3, 4]}],
  "scrollVelocity": [{"timeUs": 0, "multiplier": 1.0}, {"timeUs": 32000000, "multiplier": 0.5}],
  "modeSettings": {"laneCount": 4},
  "notes": [{"timeUs": 1000000, "lane": 0},
            {"timeUs": 1333333, "lane": 2, "endTimeUs": 2000000},
            {"timeUs": 40000000, "lane": 3}],
  "timeline": []
})";

std::string Replace(std::string text, const std::string& from, const std::string& to)
{
    const size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    return text.replace(position, from.size(), to);
}

std::string ErrorOf(const std::string& json)
{
    hyoshi::Result<ManiaChart> chart = hyoshi::mania::ParseManiaChart(json);
    REQUIRE_FALSE(chart);
    return chart.GetError().Message;
}

} // namespace

TEST_CASE("The design document's example chart loads")
{
    hyoshi::Result<ManiaChart> result = hyoshi::mania::ParseManiaChart(EXAMPLE_CHART);
    REQUIRE(result);
    const ManiaChart& chart = result.Value();

    CHECK(chart.Info.Metadata.Title == "Example Song");
    CHECK(chart.Info.Metadata.DifficultyValue == doctest::Approx(7.5));
    CHECK(chart.Info.Metadata.PreviewStartUs == 45'000'000);
    CHECK(chart.Info.AudioFile == "audio/example.ogg");
    CHECK(chart.Info.AudioOffsetUs == 1500);
    REQUIRE(chart.Info.Timing.size() == 2);
    CHECK(chart.Info.Timing[1].Bpm == doctest::Approx(90.0));
    CHECK(chart.Info.Timing[1].BeatsPerBar == 3);
    CHECK(chart.LaneCount == 4);

    REQUIRE(chart.Notes.size() == 3);
    CHECK(chart.Notes[1].Lane == 2);
    CHECK(chart.Notes[1].IsHold());
    CHECK(chart.Notes[1].EndTime == 2'000'000);
    CHECK_FALSE(chart.Notes[0].IsHold());
    CHECK(chart.Notes[1].EndScrollPosition == doctest::Approx(2.0));
    // Half speed from 32 s: 32 + 8 * 0.5.
    CHECK(chart.Notes[2].ScrollPosition == doctest::Approx(36.0));
    CHECK(chart.GetJudgmentCount() == 4);
}

TEST_CASE("Invalid charts are rejected with a reason")
{
    CHECK(ErrorOf("{not json").find("Invalid JSON") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"("formatVersion": 1)", R"("formatVersion": 2)"))
              .find("Unsupported chart format version 2") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"("mode": "mania")", R"("mode": "drum")")).find("'drum'") !=
          std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"({"timeUs": 40000000, "lane": 3})", R"({"timeUs": 500000, "lane": 3})"))
              .find("sorted") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"("lane": 3)", R"("lane": 4)")).find("out of range") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"("endTimeUs": 2000000)", R"("endTimeUs": 1000)")).find("before") !=
          std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"({"timeUs": 1000000, "lane": 0})", R"({"timeUs": 1000000.5, "lane": 0})"))
              .find("notes[0]: 'timeUs' must be an integer") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"({"timeUs": 40000000, "lane": 3})", R"({"timeUs": 1500000, "lane": 2})"))
              .find("previous note in lane 2") != std::string::npos);
    CHECK(ErrorOf(Replace(EXAMPLE_CHART, R"("bpm": 90.0)", R"("bpm": 0)")).find("positive") != std::string::npos);
}

TEST_CASE("Writing a chart and reading it back gives the same chart")
{
    hyoshi::Result<ManiaChart> original = hyoshi::mania::ParseManiaChart(EXAMPLE_CHART);
    REQUIRE(original);
    hyoshi::Result<std::string> json = hyoshi::mania::WriteManiaChart(original.Value());
    REQUIRE(json);
    // One note per line.
    CHECK(json.Value().find(R"({"timeUs":1333333,"endTimeUs":2000000,"lane":2})") != std::string::npos);

    hyoshi::Result<ManiaChart> reread = hyoshi::mania::ParseManiaChart(json.Value());
    REQUIRE(reread);
    const ManiaChart& a = original.Value();
    const ManiaChart& b = reread.Value();
    CHECK(b.Info.Metadata.Title == a.Info.Metadata.Title);
    CHECK(b.Info.AudioOffsetUs == a.Info.AudioOffsetUs);
    CHECK(b.Info.Timing.size() == a.Info.Timing.size());
    CHECK(b.Info.Timing[1].BeatsPerBar == a.Info.Timing[1].BeatsPerBar);
    CHECK(b.Info.ScrollVelocity.size() == a.Info.ScrollVelocity.size());
    CHECK(b.LaneCount == a.LaneCount);
    REQUIRE(b.Notes.size() == a.Notes.size());
    for (size_t i = 0; i < a.Notes.size(); ++i)
    {
        CHECK(b.Notes[i].Time == a.Notes[i].Time);
        CHECK(b.Notes[i].EndTime == a.Notes[i].EndTime);
        CHECK(b.Notes[i].Lane == a.Notes[i].Lane);
    }
}

TEST_CASE("Notes can carry their own sound")
{
    std::string json = Replace(EXAMPLE_CHART, R"("offsetUs": 1500},)",
                               R"("offsetUs": 1500}, "samples": ["kick.wav", "sounds/snare 2.wav"],)");
    json = Replace(json, R"({"timeUs": 1000000, "lane": 0})",
                   R"({"timeUs": 1000000, "lane": 0, "sample": 1, "sampleVolume": 70})");
    json = Replace(json, R"({"timeUs": 40000000, "lane": 3})", R"({"timeUs": 40000000, "lane": 3, "sample": 0})");

    hyoshi::Result<ManiaChart> chart = hyoshi::mania::ParseManiaChart(json);
    REQUIRE(chart);
    const std::vector<std::string>& samples = chart.Value().Info.Samples;
    const std::vector<hyoshi::mania::ManiaNote>& notes = chart.Value().Notes;
    REQUIRE(samples.size() == 2);
    CHECK(samples[1] == "sounds/snare 2.wav");
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].Sample == 1);
    CHECK(notes[0].SampleVolume == 70);
    CHECK(notes[1].Sample == hyoshi::rhythm::ChartNote::NO_SAMPLE);
    CHECK(notes[2].Sample == 0);
    CHECK(notes[2].SampleVolume == 100);

    hyoshi::Result<std::string> written = hyoshi::mania::WriteManiaChart(chart.Value());
    REQUIRE(written);
    CHECK(written.Value().find(R"({"timeUs":1000000,"lane":0,"sample":1,"sampleVolume":70})") != std::string::npos);
    CHECK(written.Value().find(R"({"timeUs":1333333,"endTimeUs":2000000,"lane":2})") != std::string::npos);
    hyoshi::Result<ManiaChart> reread = hyoshi::mania::ParseManiaChart(written.Value());
    REQUIRE(reread);
    CHECK(reread.Value().Info.Samples == samples);
    REQUIRE(reread.Value().Notes.size() == 3);
    CHECK(reread.Value().Notes[0].Sample == 1);
    CHECK(reread.Value().Notes[0].SampleVolume == 70);
    CHECK(reread.Value().Notes[2].Sample == 0);

    // A chart without samples writes none.
    hyoshi::Result<ManiaChart> plain = hyoshi::mania::ParseManiaChart(EXAMPLE_CHART);
    REQUIRE(plain);
    hyoshi::Result<std::string> plainJson = hyoshi::mania::WriteManiaChart(plain.Value());
    REQUIRE(plainJson);
    CHECK(plainJson.Value().find("sample") == std::string::npos);

    CHECK(ErrorOf(Replace(json, R"("sample": 0})", R"("sample": 2})")).find("'sample' 2 is not in 'samples'") !=
          std::string::npos);
    CHECK(ErrorOf(Replace(json, R"("sampleVolume": 70)", R"("sampleVolume": 101)")).find("0 to 100") !=
          std::string::npos);
    CHECK(ErrorOf(Replace(json, R"("kick.wav")", "7")).find("samples[0] must be a string") != std::string::npos);
}
