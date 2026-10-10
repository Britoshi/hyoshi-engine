#include "line/LineChart.h"
#include "line/LineJudge.h"
#include "line/PhigrosImport.h"
#include "mania/ArcaeaJudge.h"
#include "mania/ManiaChart.h"
#include "rhythm/ChartFile.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

using hyoshi::SongTimeUs;
using hyoshi::rhythm::GameplayAction;
using hyoshi::rhythm::Judgment;
using hyoshi::rhythm::JudgmentEvent;

namespace
{

// Two lanes of Mania, and a level Phigros line across the middle of the field.
const char* const MIXED_CHART = R"({
  "formatVersion": 2,
  "modes": ["mania", "phigros"],
  "metadata": {"title": "Mixed", "artist": "Test", "charter": "Test", "difficultyName": "Test",
               "difficultyValue": 1, "previewStartUs": 0},
  "audio": {"file": "audio.ogg"},
  "timing": [{"timeUs": 0, "bpm": 120, "meter": [4, 4]}],
  "modeSettings": {"mania": {"laneCount": 2}, "phigros": {"lines": [{"x": 0.5, "y": 0.5, "speed": 1}]}},
  "notes": [
    {"timeUs": 1000000, "mode": "mania", "lane": 0},
    {"timeUs": 1000000, "mode": "phigros", "line": 0, "x": 0, "kind": "tap"},
    {"timeUs": 1500000, "mode": "phigros", "line": 0, "x": 3, "kind": "drag"},
    {"timeUs": 2000000, "mode": "phigros", "line": 0, "x": -3, "side": "below", "kind": "flick"},
    {"timeUs": 2500000, "endTimeUs": 3000000, "mode": "phigros", "line": 0, "x": 0, "kind": "hold"},
    {"timeUs": 3000000, "endTimeUs": 3500000, "mode": "mania", "lane": 1}
  ]
})";

struct Mixed
{
    hyoshi::mania::ManiaChart Mania;
    hyoshi::line::LineChart Lines;
};

Mixed ParseMixed(const std::string& json)
{
    uint32_t laneCount = 0;
    std::vector<uint32_t> lanes;
    std::vector<hyoshi::line::JudgeLine> judgeLines;
    std::vector<hyoshi::line::LineNote> lineNotes;
    const std::array<hyoshi::rhythm::ChartModeFormat, 2> formats{
        hyoshi::mania::MakeLaneFormat("mania", laneCount, lanes), hyoshi::line::MakeLineFormat(judgeLines, lineNotes)};
    hyoshi::Result<hyoshi::rhythm::Chart> chart = hyoshi::rhythm::ParseChart(json, formats);
    REQUIRE(chart);
    REQUIRE(chart.Value().Modes.size() == 2);
    hyoshi::Result<hyoshi::mania::ManiaChart> mania =
        hyoshi::mania::BuildManiaChart(hyoshi::rhythm::SelectMode(chart.Value(), 0), laneCount, lanes);
    hyoshi::Result<hyoshi::line::LineChart> lines = hyoshi::line::BuildLineChart(
        hyoshi::rhythm::SelectMode(chart.Value(), 1), std::move(judgeLines), std::move(lineNotes));
    REQUIRE(mania);
    REQUIRE(lines);
    return {std::move(mania).Value(), std::move(lines).Value()};
}

// Where a note on the level line meets it: the field's middle, x along it.
glm::vec2 OnLine(float x)
{
    return {(hyoshi::line::FIELD_WIDTH * 0.5f) + (x * hyoshi::line::X_UNIT), hyoshi::line::FIELD_HEIGHT * 0.5f};
}

GameplayAction LineAct(hyoshi::line::LineAction type, uint32_t pointer, glm::vec2 position, SongTimeUs time)
{
    return hyoshi::line::MakeAction(type, pointer, position, time);
}

} // namespace

TEST_CASE("Chart format 2: modes listed, settings per mode, and each note in its mode")
{
    const Mixed chart = ParseMixed(MIXED_CHART);
    CHECK(chart.Mania.LaneCount == 2);
    REQUIRE(chart.Mania.Notes.size() == 2);
    CHECK(chart.Mania.Notes[1].Lane == 1);
    CHECK(chart.Mania.Notes[1].IsHold());
    REQUIRE(chart.Lines.Notes.size() == 4);
    CHECK(chart.Lines.Notes[1].Kind == hyoshi::line::NoteKind::Drag);
    CHECK(chart.Lines.Notes[1].X == doctest::Approx(3.0f));
    CHECK_FALSE(chart.Lines.Notes[2].IsAbove);
    CHECK(chart.Lines.Notes[3].IsHold());
    // A hold is two judgments.
    CHECK(chart.Lines.GetJudgmentCount() == 5);

    // A note's mode has to be one the chart lists, and with several, named.
    std::string json = MIXED_CHART;
    json.replace(json.find("\"mode\": \"mania\", \"lane\": 0"), 15, "\"mode\": \"circle\"");
    uint32_t laneCount = 0;
    std::vector<uint32_t> lanes;
    std::vector<hyoshi::line::JudgeLine> judgeLines;
    std::vector<hyoshi::line::LineNote> lineNotes;
    const std::array<hyoshi::rhythm::ChartModeFormat, 2> formats{
        hyoshi::mania::MakeLaneFormat("mania", laneCount, lanes), hyoshi::line::MakeLineFormat(judgeLines, lineNotes)};
    CHECK_FALSE(hyoshi::rhythm::ParseChart(json, formats));
}

TEST_CASE("Phigros judge: taps by timing, drags by presence, flicks by a swipe, holds to their end")
{
    const Mixed chart = ParseMixed(MIXED_CHART);
    hyoshi::line::LineJudge judge;
    judge.Reset(chart.Lines);
    std::vector<JudgmentEvent> events;
    using hyoshi::line::LineAction;

    // 30 ms late, on the note: Perfect. A press elsewhere does nothing.
    judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(6.0f), 1'000'000), events);
    CHECK(events.empty());
    judge.ProcessAction(LineAct(LineAction::Press, 1, OnLine(0.2f), 1'030'000), events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].Result == Judgment::Perfect);
    CHECK(events[0].ErrorUs == 30'000);
    CHECK(events[0].IsTimed);
    judge.ProcessAction(LineAct(LineAction::Release, 1, OnLine(0.2f), 1'060'000), events);
    judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(6.0f), 1'060'000), events);

    // Down on the drag before it arrives: caught, Perfect when it does.
    judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(3.0f), 1'400'000), events);
    CHECK(events.size() == 1);
    judge.Advance(1'500'000, events);
    REQUIRE(events.size() == 2);
    CHECK(events[1].Result == Judgment::Perfect);
    CHECK_FALSE(events[1].IsTimed);
    judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(3.0f), 1'550'000), events);

    // A swipe across the flick (from below the line, where it comes from).
    judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(-3.0f) + glm::vec2{0.0f, 60.0f}, 1'970'000), events);
    judge.ProcessAction(LineAct(LineAction::Move, 0, OnLine(-3.0f), 2'000'000), events);
    REQUIRE(events.size() == 3);
    CHECK(events[2].Result == Judgment::Perfect);
    judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(-3.0f), 2'020'000), events);

    // The hold, 100 ms early (Good), let go in its last 100 ms: the end takes the head's result.
    judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(0.0f), 2'400'000), events);
    REQUIRE(events.size() == 4);
    CHECK(events[3].Result == Judgment::Good);
    judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(0.0f), 2'950'000), events);
    REQUIRE(events.size() == 5);
    CHECK(events[4].Result == Judgment::Good);
    CHECK(events[4].IsTail);
    CHECK(judge.IsFinished());
}

TEST_CASE("Phigros judge: misses, an early release, and keys that reach every note")
{
    const Mixed chart = ParseMixed(MIXED_CHART);
    using hyoshi::line::LineAction;
    {
        hyoshi::line::LineJudge judge;
        judge.Reset(chart.Lines);
        std::vector<JudgmentEvent> events;
        // The hold let go halfway: its end is missed.
        judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(0.0f), 2'500'000), events);
        judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(0.0f), 2'700'000), events);
        judge.Advance(5'000'000, events);
        // Tap, drag, and flick missed; the hold's head Perfect and its end missed.
        REQUIRE(events.size() == 5);
        size_t misses = 0;
        for (const JudgmentEvent& event : events)
        {
            misses += event.Result == Judgment::Miss ? 1 : 0;
        }
        CHECK(misses == 4);
    }
    {
        // A key strikes wherever the note is, and flicks. Nothing was down for the drag.
        hyoshi::line::LineJudge judge;
        judge.Reset(chart.Lines);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(LineAct(LineAction::KeyPress, 5, {}, 1'000'000), events);
        judge.ProcessAction(LineAct(LineAction::KeyRelease, 5, {}, 1'050'000), events);
        judge.ProcessAction(LineAct(LineAction::KeyPress, 5, {}, 2'000'000), events);
        REQUIRE(events.size() == 3);
        CHECK(events[0].Result == Judgment::Perfect);
        CHECK(events[1].Result == Judgment::Miss);
        CHECK(events[1].NoteIndex == 1);
        CHECK(events[2].Result == Judgment::Perfect);
        CHECK(events[2].NoteIndex == 2);
    }
}

TEST_CASE("Phigros autoplay: every judgment Perfect, the same live and replayed")
{
    const Mixed chart = ParseMixed(MIXED_CHART);
    const std::vector<GameplayAction> actions = hyoshi::line::MakeAutoplayActions(chart.Lines);
    hyoshi::line::LineJudge judge;
    judge.Reset(chart.Lines);
    std::vector<JudgmentEvent> events;
    for (const GameplayAction& action : actions)
    {
        judge.ProcessAction(action, events);
    }
    judge.Advance(10'000'000, events);
    CHECK(judge.IsFinished());
    REQUIRE(events.size() == chart.Lines.GetJudgmentCount());
    for (const JudgmentEvent& event : events)
    {
        CHECK(event.Result == Judgment::Perfect);
    }

    // Frame by frame, with the judge 20 ms behind, the events are the same.
    hyoshi::line::LineJudge live;
    live.Reset(chart.Lines);
    std::vector<JudgmentEvent> liveEvents;
    size_t next = 0;
    for (SongTimeUs frame = 0; frame < 10'000'000; frame += 16'667)
    {
        while (next < actions.size() && actions[next].Time <= frame)
        {
            GameplayAction action = actions[next++];
            action.Time = std::max(action.Time, live.GetTime());
            live.ProcessAction(action, liveEvents);
        }
        live.Advance(frame - 20'000, liveEvents);
    }
    live.Advance(10'000'000, liveEvents);
    REQUIRE(liveEvents.size() == events.size());
    for (size_t i = 0; i < events.size(); ++i)
    {
        CHECK(liveEvents[i].NoteIndex == events[i].NoteIndex);
        CHECK(liveEvents[i].Result == events[i].Result);
    }
}

TEST_CASE("Arcaea judge: Pure and Far windows, and holds held for their combos")
{
    hyoshi::rhythm::Chart common;
    common.Timing.push_back({0, 120.0, 4, 4});
    // A tap, then a two-beat hold at 120 BPM: four combos, one every half beat.
    common.Notes = {{1'000'000, 1'000'000}, {2'000'000, 3'000'000}};
    const std::vector<uint32_t> lanes{0, 1};
    hyoshi::Result<hyoshi::mania::ManiaChart> chart = hyoshi::mania::BuildManiaChart(common, 2, lanes);
    REQUIRE(chart);
    CHECK(hyoshi::mania::ArcaeaJudge::GetJudgmentCount(chart.Value()) == 5);

    for (const auto& [offset, expected] : {std::pair<SongTimeUs, Judgment>{20'000, Judgment::Perfect},
                                           {-45'000, Judgment::Great},
                                           {90'000, Judgment::Good}})
    {
        CAPTURE(offset);
        hyoshi::mania::ArcaeaJudge judge;
        judge.Reset(chart.Value());
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(hyoshi::mania::MakeAction(hyoshi::mania::ManiaAction::Press, 0, 1'000'000 + offset),
                            events);
        REQUIRE(events.size() == 1);
        CHECK(events[0].Result == expected);
    }

    // The hold's combos are at 2, 2.25, 2.5 and 2.75 s. Pressed past Far, so the first is lost;
    // let go over the third and pressed again for the last.
    hyoshi::mania::ArcaeaJudge judge;
    judge.Reset(chart.Value());
    std::vector<JudgmentEvent> events;
    using hyoshi::mania::ManiaAction;
    judge.ProcessAction(hyoshi::mania::MakeAction(ManiaAction::Press, 1, 2'200'000), events);
    judge.ProcessAction(hyoshi::mania::MakeAction(ManiaAction::Release, 1, 2'400'000), events);
    judge.ProcessAction(hyoshi::mania::MakeAction(ManiaAction::Press, 1, 2'600'000), events);
    judge.Advance(4'000'000, events);
    std::vector<Judgment> holdResults;
    for (const JudgmentEvent& event : events)
    {
        if (event.NoteIndex == 1)
        {
            holdResults.push_back(event.Result);
        }
    }
    const std::vector<Judgment> expected{Judgment::Miss, Judgment::Perfect, Judgment::Miss, Judgment::Perfect};
    CHECK(holdResults == expected);
    CHECK(judge.IsFinished());
}

TEST_CASE("Phigros import: format 3 and the packed moves of format 1, times in 1/32 beats")
{
    const char* const phigros = R"({"formatVersion": 3, "offset": 0.1, "judgeLineList": [{
        "bpm": 120,
        "notesAbove": [{"type": 1, "time": 32, "positionX": 2.5, "holdTime": 0, "speed": 1, "floorPosition": 0}],
        "notesBelow": [{"type": 3, "time": 64, "positionX": -1, "holdTime": 32, "speed": 1.5, "floorPosition": 0}],
        "speedEvents": [{"startTime": -999999, "endTime": 1000000000, "value": 2}],
        "judgeLineMoveEvents": [{"startTime": -999999, "endTime": 64, "start": 0.5, "end": 0.25,
                                 "start2": 0.5, "end2": 0.75}],
        "judgeLineRotateEvents": [], "judgeLineDisappearEvents": []}]})";
    hyoshi::Result<hyoshi::line::PhigrosImportResult> imported = hyoshi::line::ImportPhigros(phigros);
    REQUIRE(imported);
    const hyoshi::line::LineChart& chart = imported.Value().Chart;
    CHECK(chart.Info.AudioOffsetUs == 100'000);
    REQUIRE(chart.Notes.size() == 2);
    // 32 units at 120 BPM is a beat: half a second.
    CHECK(chart.Notes[0].Time == 500'000);
    CHECK(chart.Notes[0].X == doctest::Approx(2.5f));
    CHECK(chart.Notes[1].Kind == hyoshi::line::NoteKind::Hold);
    CHECK(chart.Notes[1].EndTime == 1'500'000);
    CHECK_FALSE(chart.Notes[1].IsAbove);
    // Speed 2: two units of floor a second.
    CHECK(chart.Notes[1].FloorPosition == doctest::Approx(2.0));
    // Halfway through the move.
    const hyoshi::line::LineState state = hyoshi::line::GetLineState(chart.Lines[0], 500'000);
    CHECK(state.Position.x == doctest::Approx(0.375f * hyoshi::line::FIELD_WIDTH));
    CHECK(state.Position.y == doctest::Approx((1.0f - 0.625f) * hyoshi::line::FIELD_HEIGHT));

    // Format 1: x * 1000 + y on an 880 by 520 screen.
    const char* const packed = R"({"formatVersion": 1, "judgeLineList": [{"bpm": 120, "notesAbove": [],
        "notesBelow": [], "speedEvents": [],
        "judgeLineMoveEvents": [{"startTime": 0, "endTime": 100, "start": 440260, "end": 440260}]}]})";
    hyoshi::Result<hyoshi::line::PhigrosImportResult> old = hyoshi::line::ImportPhigros(packed);
    REQUIRE(old);
    const hyoshi::line::LineState oldState = hyoshi::line::GetLineState(old.Value().Chart.Lines[0], 0);
    CHECK(oldState.Position.x == doctest::Approx(0.5f * hyoshi::line::FIELD_WIDTH));
    CHECK(oldState.Position.y == doctest::Approx(0.5f * hyoshi::line::FIELD_HEIGHT));

    const hyoshi::line::PhigrosInfo info =
        hyoshi::line::ReadPhigrosInfo("name: Song\ncomposer: Someone\nmusic: \"song.ogg\"\nchart: chart.json\n");
    CHECK(info.Title == "Song");
    CHECK(info.MusicFile == "song.ogg");
    CHECK(info.ChartFile == "chart.json");
}

TEST_CASE("Chart format 2 writes what it reads")
{
    uint32_t laneCount = 0;
    std::vector<uint32_t> lanes;
    std::vector<hyoshi::line::JudgeLine> judgeLines;
    std::vector<hyoshi::line::LineNote> lineNotes;
    const std::array<hyoshi::rhythm::ChartModeFormat, 2> formats{
        hyoshi::mania::MakeLaneFormat("mania", laneCount, lanes), hyoshi::line::MakeLineFormat(judgeLines, lineNotes)};
    hyoshi::Result<hyoshi::rhythm::Chart> chart = hyoshi::rhythm::ParseChart(MIXED_CHART, formats);
    REQUIRE(chart);
    // A line moving over time, so its keys are written as a list.
    judgeLines[0].Y = {{0, 0.25f}, {2'000'000, 0.75f}};
    lineNotes[2].Speed = 1.5f;
    hyoshi::Result<std::string> written = hyoshi::rhythm::WriteChart(chart.Value(), formats);
    REQUIRE(written);
    CHECK(written.Value().find("\"formatVersion\": 2") != std::string::npos);

    const Mixed reread = ParseMixed(written.Value());
    CHECK(reread.Mania.LaneCount == 2);
    REQUIRE(reread.Mania.Notes.size() == 2);
    CHECK(reread.Mania.Notes[1].Lane == 1);
    CHECK(reread.Mania.Notes[1].EndTime == 3'500'000);
    REQUIRE(reread.Lines.Notes.size() == 4);
    CHECK(reread.Lines.Notes[1].Kind == hyoshi::line::NoteKind::Drag);
    CHECK(reread.Lines.Notes[1].X == doctest::Approx(3.0f));
    CHECK_FALSE(reread.Lines.Notes[2].IsAbove);
    CHECK(reread.Lines.Notes[2].Speed == doctest::Approx(1.5f));
    CHECK(reread.Lines.Notes[3].EndTime == 3'000'000);
    REQUIRE(reread.Lines.Lines[0].Y.size() == 2);
    CHECK(reread.Lines.Lines[0].Y[1].Time == 2'000'000);
    CHECK(reread.Lines.Lines[0].X.size() == 1);
}
