#include "line/LineChart.h"
#include "line/LineJudge.h"
#include "line/LinePlayfield.h"
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

TEST_CASE("Phigros field reshaped to the screen: lines spread over it, notes keep their size, autoplay follows")
{
    // A 4:3 screen: the field is as wide as ever and taller; a 21:9 one, wider.
    const glm::vec2 squarer = hyoshi::line::GetScreenFieldSize({1600.0f, 1200.0f});
    CHECK(squarer.x == doctest::Approx(hyoshi::line::FIELD_WIDTH));
    CHECK(squarer.y == doctest::Approx(1440.0f));
    const glm::vec2 wider = hyoshi::line::GetScreenFieldSize({2520.0f, 1080.0f});
    CHECK(wider.x == doctest::Approx(2520.0f));
    CHECK(wider.y == doctest::Approx(hyoshi::line::FIELD_HEIGHT));
    // Fit to that screen, it fills it.
    const hyoshi::line::LineLayout layout = hyoshi::line::LineLayout::Fit({1600.0f, 1200.0f}, squarer);
    CHECK(layout.ToCanvas({0.0f, 0.0f}).y == doctest::Approx(0.0f));
    CHECK(layout.ToCanvas(squarer).y == doctest::Approx(1200.0f));

    // A line at a fraction of the field is at that fraction of the reshaped one.
    Mixed chart = ParseMixed(MIXED_CHART);
    chart.Lines.FieldSize = squarer;
    const hyoshi::line::LineState state = hyoshi::line::GetLineState(chart.Lines.Lines[0], 0, chart.Lines.FieldSize);
    const hyoshi::line::LineState usual = hyoshi::line::GetLineState(chart.Lines.Lines[0], 0);
    CHECK(state.Position.x == doctest::Approx(usual.Position.x));
    CHECK(state.Position.y == doctest::Approx(usual.Position.y * squarer.y / hyoshi::line::FIELD_HEIGHT));

    // Autoplay on the reshaped field is still all Perfect.
    const std::vector<GameplayAction> actions = hyoshi::line::MakeAutoplayActions(chart.Lines);
    hyoshi::line::LineJudge judge;
    judge.Reset(chart.Lines);
    std::vector<JudgmentEvent> events;
    for (const GameplayAction& action : actions)
    {
        judge.ProcessAction(action, events);
    }
    judge.Advance(10'000'000, events);
    REQUIRE(events.size() == chart.Lines.GetJudgmentCount());
    for (const JudgmentEvent& event : events)
    {
        CHECK(event.Result == Judgment::Perfect);
    }
}

TEST_CASE("Phigros judge: any pointer down holds, however long it's been down and whatever it pressed")
{
    using hyoshi::line::LineAction;
    using hyoshi::line::NoteKind;
    // A level line across the middle that waits at the field's left edge, then slides under the
    // middle by 1 s and stays. A drag at 1 s, a hold from 1.5 to 2 s, a tap at 2.5 s, and a drag at
    // 3 s, all at its middle.
    hyoshi::line::JudgeLine line;
    line.X = {{0, 0.0f}, {900'000, 0.0f}, {1'000'000, 0.5f}};
    line.Y = {{0, 0.5f}};
    line.Speed = {{0, 1.0f}};
    hyoshi::rhythm::Chart info;
    std::vector<hyoshi::line::LineNote> notes;
    const auto add = [&](SongTimeUs time, SongTimeUs end, NoteKind kind)
    {
        hyoshi::line::LineNote note;
        note.Time = time;
        note.EndTime = end;
        note.Kind = kind;
        notes.push_back(note);
        info.Notes.push_back({time, end});
    };
    add(1'000'000, 1'000'000, NoteKind::Drag);
    add(1'500'000, 2'000'000, NoteKind::Hold);
    add(2'500'000, 2'500'000, NoteKind::Tap);
    add(3'000'000, 3'000'000, NoteKind::Drag);
    hyoshi::Result<hyoshi::line::LineChart> chart = hyoshi::line::BuildLineChart(std::move(info), {line}, notes);
    REQUIRE(chart);

    hyoshi::line::LineJudge judge;
    judge.Reset(chart.Value());
    std::vector<JudgmentEvent> events;
    const glm::vec2 middle{hyoshi::line::FIELD_WIDTH * 0.5f, hyoshi::line::FIELD_HEIGHT * 0.5f};
    // Down in the middle long before the drag, and still: the drag's window opens with the line
    // still far off, and the line comes under the finger just before the drag arrives. Caught.
    judge.ProcessAction(LineAct(LineAction::Press, 0, middle, 500'000), events);
    judge.Advance(1'200'000, events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].NoteIndex == 0);
    CHECK(events[0].Result == Judgment::Perfect);
    CHECK(events[0].Time == 1'000'000);

    // The same finger, still down, takes the hold as it arrives, as if struck on time (untimed).
    judge.Advance(1'600'000, events);
    REQUIRE(events.size() == 2);
    CHECK(events[1].NoteIndex == 1);
    CHECK(events[1].Result == Judgment::Perfect);
    CHECK_FALSE(events[1].IsTimed);
    CHECK(judge.GetNoteState(1) == hyoshi::line::NoteState::Held);

    // A second finger comes down without striking (its press went elsewhere), and the first lets
    // go early: the second holds the hold to its end.
    judge.ProcessAction(LineAct(LineAction::Touch, 1, middle, 1'700'000), events);
    judge.ProcessAction(LineAct(LineAction::Release, 0, middle, 1'800'000), events);
    judge.Advance(2'100'000, events);
    REQUIRE(events.size() == 3);
    CHECK(events[2].IsTail);
    CHECK(events[2].Result == Judgment::Perfect);

    // A tap needs a press: a finger down, or touching, misses it. The touching finger catches the
    // drag after it.
    judge.ProcessAction(LineAct(LineAction::Touch, 2, middle, 2'500'000), events);
    judge.Advance(3'500'000, events);
    REQUIRE(events.size() == 5);
    CHECK(events[3].NoteIndex == 2);
    CHECK(events[3].Result == Judgment::Miss);
    CHECK(events[4].NoteIndex == 3);
    CHECK(events[4].Result == Judgment::Perfect);
    CHECK(judge.IsFinished());
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
    // Past its last combo (2.75 s) but before its end (3 s), the hold is still there, held.
    judge.Advance(2'900'000, events);
    CHECK(judge.GetNoteState(1) == hyoshi::mania::NoteState::Held);
    judge.Advance(4'000'000, events);
    CHECK(judge.GetNoteState(1) == hyoshi::mania::NoteState::Hit);
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

    // Lanes touched rather than pressed (fingers whose presses went to another mode) from before
    // the notes: the tap isn't struck, and the hold has every combo.
    hyoshi::mania::ArcaeaJudge touched;
    touched.Reset(chart.Value());
    std::vector<JudgmentEvent> touchEvents;
    touched.ProcessAction(hyoshi::mania::MakeAction(ManiaAction::Touch, 0, 950'000), touchEvents);
    touched.ProcessAction(hyoshi::mania::MakeAction(ManiaAction::Touch, 1, 1'500'000), touchEvents);
    touched.Advance(4'000'000, touchEvents);
    REQUIRE(touchEvents.size() == 5);
    CHECK(touchEvents[0].NoteIndex == 0);
    CHECK(touchEvents[0].Result == Judgment::Miss);
    for (size_t i = 1; i < touchEvents.size(); ++i)
    {
        CHECK(touchEvents[i].Result == Judgment::Perfect);
    }
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
    judgeLines[0].Length = {{0, 0.5f}};
    judgeLines[0].Blue = {{0, 0.0f}, {1'000'000, 1.0f}};
    judgeLines[0].Picture = "pictures/fish.png";
    judgeLines[0].Anchor = {0.25f, 1.0f};
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
    CHECK(hyoshi::line::GetKeyValue(reread.Lines.Lines[0].Length, 0, 1.0f) == doctest::Approx(0.5f));
    CHECK(reread.Lines.Lines[0].Blue.size() == 2);
    CHECK(reread.Lines.Lines[0].Red.empty());
    CHECK(reread.Lines.Lines[0].Picture == "pictures/fish.png");
    CHECK(reread.Lines.Lines[0].Anchor.x == doctest::Approx(0.25f));
    CHECK(reread.Lines.Lines[0].Anchor.y == doctest::Approx(1.0f));
}

namespace
{

// A small Re:PhiEdit chart: two tempos, three layers (one null), eased events, a picture line
// (twice as wide, anchored at its top left), a null line, a child line drawn first, scaled, and
// colored, and a fake note.
const char* const RPE_CHART = R"({
  "BPMList": [{"bpm": 120, "startTime": [0, 0, 1]}, {"bpm": 60, "startTime": [4, 0, 1]}],
  "META": {"RPEVersion": 160, "name": "Song", "composer": "Someone", "charter": "Me", "level": "IN Lv.12",
           "song": "song.ogg", "offset": -50},
  "judgeLineList": [
    {"Texture": "line.png", "father": -1, "bpmfactor": 1.0,
     "eventLayers": [
       {"moveXEvents": [{"startTime": [0, 0, 1], "endTime": [2, 0, 1], "start": 0, "end": 675, "easingType": 1}],
        "moveYEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": -450, "end": -450, "easingType": 1}],
        "rotateEvents": [{"startTime": [0, 0, 1], "endTime": [2, 0, 1], "start": 0, "end": 90, "easingType": 5}],
        "alphaEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 255, "end": 255, "easingType": 1}],
        "speedEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 9, "end": 9}]},
       null,
       {"moveXEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 135, "end": 135, "easingType": 1}]}],
     "notes": [
       {"type": 1, "startTime": [1, 0, 1], "endTime": [1, 0, 1], "positionX": 151.875, "above": 1, "isFake": 0,
        "speed": 1},
       {"type": 2, "startTime": [2, 0, 1], "endTime": [3, 1, 2], "positionX": 0, "above": 2, "isFake": 0, "speed": 1},
       {"type": 3, "startTime": [5, 0, 1], "endTime": [5, 0, 1], "positionX": 0, "above": 1, "isFake": 0, "speed": 1},
       {"type": 4, "startTime": [5, 0, 1], "endTime": [5, 0, 1], "positionX": 0, "above": 1, "isFake": 1, "speed": 1}]},
    {"Texture": "fish.png", "father": -1, "bpmfactor": 1.0, "anchor": [0.0, 1.0],
     "eventLayers": [{"alphaEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 255, "end": 255}]}],
     "extended": {"scaleXEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 2, "end": 2}]},
     "notes": null},
    null,
    {"Texture": "line.png", "father": 0, "bpmfactor": 1.0, "zOrder": -1,
     "eventLayers": [{"moveXEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 100, "end": 100}]}],
     "extended": {
       "scaleXEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 0.5, "end": 0.5}],
       "scaleYEvents": [{"startTime": [0, 0, 1], "endTime": [1, 0, 1], "start": 200, "end": 200}],
       "colorEvents": [{"startTime": [0, 0, 1], "endTime": [4, 0, 1], "start": [255, 0, 0], "end": [0, 0, 255]}]}}]
})";

hyoshi::line::LineChart MakeOneLineChart(std::vector<hyoshi::line::LineNote> notes)
{
    hyoshi::rhythm::Chart info;
    for (const hyoshi::line::LineNote& note : notes)
    {
        info.Notes.push_back({note.Time, note.EndTime});
    }
    hyoshi::line::JudgeLine line;
    line.X = {{0, 0.5f}};
    line.Y = {{0, 0.5f}};
    hyoshi::Result<hyoshi::line::LineChart> chart = hyoshi::line::BuildLineChart(info, {line}, std::move(notes));
    REQUIRE(chart);
    return std::move(chart).Value();
}

hyoshi::line::LineNote MakeLineNote(SongTimeUs time, hyoshi::line::NoteKind kind)
{
    hyoshi::line::LineNote note;
    note.Time = time;
    note.EndTime = time;
    note.Kind = kind;
    return note;
}

} // namespace

TEST_CASE("Re:PhiEdit import: beats along the tempo, layers added, easing baked, lines scaled, colored, and "
          "ordered, pictures kept, and null lines hidden")
{
    hyoshi::Result<hyoshi::line::PhigrosImportResult> imported = hyoshi::line::ImportRpe(RPE_CHART);
    REQUIRE(imported);
    const hyoshi::line::LineChart& chart = imported.Value().Chart;
    CHECK(chart.Info.Metadata.Title == "Song");
    CHECK(chart.Info.AudioFile == "song.ogg");
    CHECK(chart.Info.AudioOffsetUs == -50'000);
    REQUIRE(chart.Info.Timing.size() == 2);
    CHECK(chart.Info.Timing[1].Time == 2'000'000);

    // The fake note is left out; RPE's types are tap, hold, flick, drag.
    REQUIRE(chart.Notes.size() == 3);
    CHECK(chart.Notes[0].Kind == hyoshi::line::NoteKind::Tap);
    CHECK(chart.Notes[0].Time == 500'000);
    CHECK(chart.Notes[0].X == doctest::Approx(2.0f));
    CHECK(chart.Notes[1].Kind == hyoshi::line::NoteKind::Hold);
    CHECK(chart.Notes[1].Time == 1'000'000);
    CHECK(chart.Notes[1].EndTime == 1'750'000);
    CHECK_FALSE(chart.Notes[1].IsAbove);
    // Beat 5 is a beat at 60 BPM after beat 4, at 2 s.
    CHECK(chart.Notes[2].Kind == hyoshi::line::NoteKind::Flick);
    CHECK(chart.Notes[2].Time == 3'000'000);
    // The child line, lowest in zOrder, comes first, so the notes' line is second.
    REQUIRE(chart.Lines.size() == 4);
    CHECK(chart.Notes[0].Line == 1);
    CHECK(chart.Notes[2].Line == 1);

    const hyoshi::line::JudgeLine& line = chart.Lines[1];
    // Half way: 337.5 and the other layer's 135, from the canvas's middle.
    CHECK(hyoshi::line::GetKeyValue(line.X, 500'000, 0.0f) == doctest::Approx(0.5f + (472.5f / 1350.0f)));
    CHECK(hyoshi::line::GetKeyValue(line.Y, 500'000, 0.0f) == doctest::Approx(0.0f));
    // Ease in quad, half way: a quarter of 90 degrees, turned to counterclockwise.
    CHECK(hyoshi::line::GetKeyValue(line.Rotation, 500'000, 0.0f) == doctest::Approx(-22.5f));
    CHECK(line.Speed.front().Value == doctest::Approx(2.0f));
    // Unscaled, Phira's length (3 field widths) and our thickness, in the usual color.
    CHECK(hyoshi::line::GetKeyValue(line.Length, 0, 0.0f) == doctest::Approx(0.75f));
    CHECK(hyoshi::line::GetKeyValue(line.Thickness, 0, 0.0f) == doctest::Approx(1.0f));
    CHECK(line.Red.empty());
    // The picture line draws its picture: a pixel is a canvas unit (1350 by 900 on the field), times
    // its scale, set by its anchor. The null line hides.
    const hyoshi::line::JudgeLine& picture = chart.Lines[2];
    CHECK(picture.Picture == "fish.png");
    CHECK(picture.Anchor.x == doctest::Approx(0.0f));
    CHECK(picture.Anchor.y == doctest::Approx(1.0f));
    CHECK(hyoshi::line::GetKeyValue(picture.Opacity, 0, 0.0f) == doctest::Approx(1.0f));
    CHECK(hyoshi::line::GetKeyValue(picture.Length, 0, 0.0f) ==
          doctest::Approx(2.0f * hyoshi::line::FIELD_WIDTH / 1350.0f));
    CHECK(hyoshi::line::GetKeyValue(picture.Thickness, 0, 0.0f) ==
          doctest::Approx(hyoshi::line::FIELD_HEIGHT / 900.0f));
    CHECK(hyoshi::line::GetKeyValue(chart.Lines[3].Opacity, 0, 1.0f) == doctest::Approx(0.0f));
    const std::vector<hyoshi::line::PictureUse> uses = hyoshi::line::GetPictureUses(chart);
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].Name == "fish.png");
    CHECK(uses[0].MaxUnitsPerPixel.x == doctest::Approx(2.0f * hyoshi::line::FIELD_WIDTH / 1350.0f));
    // The child at 1 s: its parent at (810, -450) turned 90 degrees clockwise, so its own (100, 0)
    // points down.
    const hyoshi::line::JudgeLine& child = chart.Lines[0];
    CHECK(hyoshi::line::GetKeyValue(child.X, 1'000'000, 0.0f) == doctest::Approx(0.5f + (810.0f / 1350.0f)));
    CHECK(hyoshi::line::GetKeyValue(child.Y, 1'000'000, 0.0f) == doctest::Approx(0.5f - (550.0f / 900.0f)));
    // Half as long as Phira's line, and 200 times Phira's scaled thickness (0.0076 of half the
    // field's width); half way from red to blue at 1 s.
    const hyoshi::line::LineState state = hyoshi::line::GetLineState(child, 1'000'000);
    CHECK(state.Length == doctest::Approx(0.375f));
    CHECK(state.Thickness * hyoshi::line::LINE_THICKNESS ==
          doctest::Approx(200.0f * 0.0076f * hyoshi::line::FIELD_WIDTH / 2.0f));
    REQUIRE(state.Color);
    CHECK(state.Color->r == doctest::Approx(0.5f));
    CHECK(state.Color->g == doctest::Approx(0.0f));
    CHECK(state.Color->b == doctest::Approx(0.5f));
    CHECK(imported.Value().Warnings.size() == 2);

    // The Phigros importer hands it over.
    hyoshi::Result<hyoshi::line::PhigrosImportResult> handed = hyoshi::line::ImportPhigros(RPE_CHART);
    REQUIRE(handed);
    CHECK(handed.Value().Chart.Notes.size() == 3);
}

TEST_CASE("Phigros judge: a press for a flick or drag due sooner leaves a later tap, and a swipe takes one flick")
{
    using hyoshi::line::LineAction;
    using hyoshi::line::NoteKind;
    {
        // A flick, then a tap 100 ms later in the same place.
        const hyoshi::line::LineChart chart =
            MakeOneLineChart({MakeLineNote(1'000'000, NoteKind::Flick), MakeLineNote(1'100'000, NoteKind::Tap)});
        hyoshi::line::LineJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(0.0f), 970'000), events);
        CHECK(events.empty());
        judge.ProcessAction(LineAct(LineAction::Move, 0, OnLine(0.0f) + glm::vec2{0.0f, -80.0f}, 1'000'000), events);
        judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(0.0f), 1'030'000), events);
        judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(0.0f), 1'100'000), events);
        REQUIRE(events.size() == 2);
        CHECK(events[0].NoteIndex == 0);
        CHECK(events[1].NoteIndex == 1);
        CHECK(events[1].Result == Judgment::Perfect);
    }
    {
        // Two flicks 100 ms apart: one swipe catches the first only.
        const hyoshi::line::LineChart chart =
            MakeOneLineChart({MakeLineNote(1'000'000, NoteKind::Flick), MakeLineNote(1'100'000, NoteKind::Flick)});
        hyoshi::line::LineJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(LineAct(LineAction::Press, 0, OnLine(0.0f), 970'000), events);
        judge.ProcessAction(LineAct(LineAction::Move, 0, OnLine(0.0f) + glm::vec2{0.0f, -80.0f}, 1'000'000), events);
        judge.ProcessAction(LineAct(LineAction::Release, 0, OnLine(0.0f), 1'030'000), events);
        judge.Advance(1'300'000, events);
        REQUIRE(events.size() == 2);
        CHECK(events[0].Result == Judgment::Perfect);
        CHECK(events[1].NoteIndex == 1);
        CHECK(events[1].Result == Judgment::Miss);
    }
}
