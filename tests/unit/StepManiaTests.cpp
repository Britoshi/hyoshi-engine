#include "mania/StepManiaImport.h"
#include "songs/Charts.h"

#include <doctest/doctest.h>

#include <string>

namespace
{

// Two charts: a 4-key one through a BPM change, a stop, a hold, a mine, and a warp, and an 8-key
// one. Beat 0 is 0.1 s in.
const char* const SSC_SONG = R"(#VERSION:0.83;
#TITLE:Song;
#SUBTITLE:Sub;
#ARTIST:Someone;
#MUSIC:song.ogg;
#BACKGROUND:bg.png;
#SAMPLESTART:12.5;
#OFFSET:-0.1;
#GENRE:Pop;
#BPMS:0=120,
8=240;
#STOPS:4=0.5;
#DELAYS:;
#WARPS:12=4;
//--------------- dance-single ----------------
#NOTEDATA:;
#STEPSTYPE:dance-single;
#DIFFICULTY:Challenge;
#METER:12;
#CREDIT:Me;
#NOTES:
1000
0100
0010
0001
,
2000
0000
3M00
0000
,
0000
,
1000
0000
0000
0000
,
0100
0000
0000
0000
;
#NOTEDATA:;
#STEPSTYPE:dance-double;
#DIFFICULTY:Hard;
#METER:9;
#NOTES:
10000001
;
)";

const char* const SM_SONG = R"(#TITLE:Old;
#ARTIST:Band;
#MUSIC:old.mp3;
#OFFSET:0.000;
#BPMS:0.000=60.000;
// A comment.
#NOTES:
     dance-single:
     Someone:
     Easy:
     3:
     0,0,0,0,0:
1000
0000
0000
0001
;
)";

} // namespace

TEST_CASE("StepMania import: .ssc charts through BPM changes, stops, holds, mines, and warps")
{
    hyoshi::Result<hyoshi::mania::StepManiaImportResult> imported = hyoshi::mania::ImportStepMania(SSC_SONG);
    REQUIRE(imported);
    REQUIRE(imported.Value().Charts.size() == 2);
    CHECK(imported.Value().BackgroundFile == "bg.png");

    const hyoshi::mania::ManiaChart& single = imported.Value().Charts[0];
    CHECK(single.LaneCount == 4);
    CHECK(single.Info.Metadata.Title == "Song Sub");
    CHECK(single.Info.Metadata.Artist == "Someone");
    CHECK(single.Info.Metadata.Charter == "Me");
    CHECK(single.Info.Metadata.DifficultyName == "Challenge 12");
    CHECK(single.Info.Metadata.PreviewStartUs == 12'500'000);
    CHECK(single.Info.AudioFile == "song.ogg");

    // Four taps a beat apart at 120 BPM, a hold from beat 4 (before its stop) to beat 6 (after
    // it), the warped tap at beat 12 left out, and beat 16: 4 s, then 4 beats at 240 BPM (the
    // other 4 are warped), the stop, and the offset.
    REQUIRE(single.Notes.size() == 6);
    CHECK(single.Notes[0].Time == 100'000);
    CHECK(single.Notes[3].Time == 1'600'000);
    CHECK(single.Notes[3].Lane == 3);
    CHECK(single.Notes[4].Lane == 0);
    CHECK(single.Notes[4].Time == 2'100'000);
    CHECK(single.Notes[4].EndTime == 3'600'000);
    CHECK(single.Notes[5].Lane == 1);
    CHECK(single.Notes[5].Time == 5'600'000);
    REQUIRE(single.Info.Timing.size() == 2);
    CHECK(single.Info.Timing[1].Time == 4'600'000);
    CHECK(single.Info.Timing[1].Bpm == doctest::Approx(240.0));

    const hyoshi::mania::ManiaChart& doubles = imported.Value().Charts[1];
    CHECK(doubles.LaneCount == 8);
    CHECK(doubles.Info.Metadata.DifficultyName == "dance-double Hard 9");
    REQUIRE(doubles.Notes.size() == 2);
    CHECK(doubles.Notes[1].Lane == 7);
    // A chord's notes shine; notes apart don't.
    CHECK(doubles.Notes[0].IsSimultaneous);
    CHECK(doubles.Notes[1].IsSimultaneous);
    CHECK_FALSE(single.Notes[0].IsSimultaneous);

    // The mine and the warped tap are reported.
    CHECK(imported.Value().Warnings.size() == 2);
}

TEST_CASE("StepMania info: the same charts, unbuilt, for a song list")
{
    hyoshi::Result<hyoshi::mania::StepManiaInfo> info = hyoshi::mania::ReadStepManiaInfo(SSC_SONG);
    REQUIRE(info);
    hyoshi::Result<hyoshi::mania::StepManiaImportResult> imported = hyoshi::mania::ImportStepMania(SSC_SONG);
    REQUIRE(imported);
    REQUIRE(info.Value().Charts.size() == imported.Value().Charts.size());
    CHECK(info.Value().BackgroundFile == "bg.png");
    CHECK(info.Value().Genre == "Pop");
    for (size_t i = 0; i < info.Value().Charts.size(); ++i)
    {
        const hyoshi::mania::StepManiaChartInfo& chart = info.Value().Charts[i];
        const hyoshi::mania::ManiaChart& built = imported.Value().Charts[i];
        CHECK(chart.Info.Metadata.DifficultyName == built.Info.Metadata.DifficultyName);
        CHECK(chart.Info.Metadata.Charter == built.Info.Metadata.Charter);
        CHECK(chart.LaneCount == built.LaneCount);
        REQUIRE(chart.Info.Notes.size() == built.Notes.size());
        CHECK(chart.Info.Notes.back().EndTime == built.Notes.back().EndTime);
    }
}

TEST_CASE("StepMania import: .sm's #NOTES fields, and chart paths")
{
    hyoshi::Result<hyoshi::mania::StepManiaImportResult> imported = hyoshi::mania::ImportStepMania(SM_SONG);
    REQUIRE(imported);
    REQUIRE(imported.Value().Charts.size() == 1);
    const hyoshi::mania::ManiaChart& chart = imported.Value().Charts[0];
    CHECK(chart.Info.Metadata.DifficultyName == "Easy 3");
    CHECK(chart.Info.AudioFile == "old.mp3");
    REQUIRE(chart.Notes.size() == 2);
    CHECK(chart.Notes[1].Time == 3'000'000);

    CHECK_FALSE(hyoshi::mania::ImportStepMania("#TITLE:Nothing;"));

    CHECK(hyoshi::songs::MakeStepManiaChartPath("a/b.ssc", 0) == "a/b.ssc");
    CHECK(hyoshi::songs::MakeStepManiaChartPath("a/b.ssc", 2) == "a/b.ssc#2");
    CHECK(hyoshi::songs::IsStepManiaFile("a/b.ssc#2"));
    CHECK(hyoshi::songs::IsStepManiaFile("a/B.SM"));
    CHECK_FALSE(hyoshi::songs::IsStepManiaFile("a/b.osu"));
    CHECK_FALSE(hyoshi::songs::IsStepManiaFile("a/#1 hits.ogg"));
}
