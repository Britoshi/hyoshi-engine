#include "circle/CircleChart.h"
#include "circle/CircleJudge.h"
#include "circle/CircleScore.h"
#include "osu/OsuStandardImporter.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using hyoshi::SongTimeUs;
using hyoshi::circle::CircleAction;
using hyoshi::circle::CircleChart;
using hyoshi::circle::CircleJudge;
using hyoshi::circle::CircleNote;
using hyoshi::circle::CircleScore;
using hyoshi::circle::CurveType;
using hyoshi::circle::MakeAction;
using hyoshi::circle::NoteState;
using hyoshi::circle::NoteType;
using hyoshi::circle::SampleBank;
using hyoshi::circle::SliderPart;
using hyoshi::circle::SliderPartType;
using hyoshi::rhythm::GameplayAction;
using hyoshi::rhythm::Judgment;
using hyoshi::rhythm::JudgmentEvent;

namespace
{

// 120 BPM from 1 s, 2x slider velocity from 3 s. Two circles, a one-pass slider, a two-pass
// slider at 2x, a spinner, and a circle after it. No ApproachRate, so it follows OD 5.
const char* const OSU_STANDARD = "osu file format v14\n"
                                 "\n"
                                 "[General]\n"
                                 "AudioFilename: audio.mp3\n"
                                 "AudioLeadIn: 500\n"
                                 "PreviewTime: 2000\n"
                                 "StackLeniency: 0.7\n"
                                 "Mode: 0\n"
                                 "\n"
                                 "[Metadata]\n"
                                 "Title:Test Song\n"
                                 "Artist:Test Artist\n"
                                 "Creator:Someone\n"
                                 "Version:Normal\n"
                                 "\n"
                                 "[Difficulty]\n"
                                 "HPDrainRate:5\n"
                                 "CircleSize:4\n"
                                 "OverallDifficulty:5\n"
                                 "SliderMultiplier:1\n"
                                 "SliderTickRate:1\n"
                                 "\n"
                                 "[Events]\n"
                                 "0,0,\"bg.jpg\",0,0\n"
                                 "\n"
                                 "[TimingPoints]\n"
                                 "1000,500,4,1,0,100,1,0\n"
                                 "3000,-50,4,1,0,100,0,0\n"
                                 "\n"
                                 "[HitObjects]\n"
                                 "100,100,1000,5,0,0:0:0:0:\n"
                                 "200,100,1500,1,0,0:0:0:0:\n"
                                 "100,200,2000,2,0,L|200:200,1,100\n"
                                 "300,200,3000,6,0,L|400:200,2,100\n"
                                 "256,192,4000,12,0,5000\n"
                                 "50,50,6000,1,0,0:0:0:0:\n";

std::string WithLine(std::string text, const std::string& from, const std::string& to)
{
    const size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    return text.replace(position, from.size(), to);
}

CircleChart ImportChart(const std::string& text)
{
    hyoshi::Result<hyoshi::osu::OsuStandardImportResult> result = hyoshi::osu::ImportOsuStandard(text);
    REQUIRE(result);
    return std::move(result).Value().Chart;
}

// OD 5 (windows 50, 100, and 150 ms) and CS 4, with no stacking unless asked for.
CircleChart MakeChart(std::vector<CircleNote> notes, float stackLeniency = 0.0f)
{
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.ApproachRate = 5.0f;
    chart.Difficulty.StackLeniency = stackLeniency;
    chart.Notes = std::move(notes);
    chart.Notes.front().IsNewCombo = true;
    FinishCircleChart(chart);
    return chart;
}

CircleNote Circle(SongTimeUs time, glm::vec2 position)
{
    CircleNote note;
    note.Time = time;
    note.EndTime = time;
    note.Position = position;
    return note;
}

// A straight slider. The caller adds its control points to the chart.
CircleNote Slider(SongTimeUs time, SongTimeUs endTime, glm::vec2 position, uint32_t firstControlPoint, float length)
{
    CircleNote note = Circle(time, position);
    note.Type = NoteType::Slider;
    note.EndTime = endTime;
    note.Curve = CurveType::Linear;
    note.FirstControlPoint = firstControlPoint;
    note.ControlPointCount = 2;
    note.Length = length;
    return note;
}

std::vector<JudgmentEvent> RunReplay(const CircleChart& chart, const std::vector<GameplayAction>& actions,
                                     SongTimeUs end)
{
    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    for (const GameplayAction& action : actions)
    {
        judge.ProcessAction(action, events);
    }
    judge.Advance(end, events);
    CHECK(judge.IsFinished());
    return events;
}

bool SameEvents(const std::vector<JudgmentEvent>& a, const std::vector<JudgmentEvent>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].NoteIndex != b[i].NoteIndex || a[i].Result != b[i].Result || a[i].ErrorUs != b[i].ErrorUs ||
            a[i].Time != b[i].Time || a[i].IsTail != b[i].IsTail || a[i].Part != b[i].Part ||
            a[i].IsTimed != b[i].IsTimed)
        {
            return false;
        }
    }
    return true;
}

GameplayAction Press(SongTimeUs time, glm::vec2 position, uint32_t pointer = 0)
{
    return MakeAction(CircleAction::Press, pointer, position, time);
}

GameplayAction Release(SongTimeUs time, glm::vec2 position, uint32_t pointer = 0)
{
    return MakeAction(CircleAction::Release, pointer, position, time);
}

GameplayAction Move(SongTimeUs time, glm::vec2 position, uint32_t pointer = 0)
{
    return MakeAction(CircleAction::Move, pointer, position, time);
}

// Graded results: one per circle and slider head, the timed events.
size_t CountResults(const std::vector<JudgmentEvent>& events)
{
    return static_cast<size_t>(
        std::count_if(events.begin(), events.end(), [](const JudgmentEvent& event) { return event.IsTimed; }));
}

// lazer's standardised score from the combo portions and accuracy.
int64_t StandardisedScore(double comboPortion, double maxComboPortion, double accuracy, double progress)
{
    return std::llround((700'000.0 * comboPortion / maxComboPortion) + (300'000.0 * std::pow(accuracy, 10) * progress));
}

void CheckPart(const JudgmentEvent& event, uint32_t note, uint32_t part, Judgment result)
{
    CHECK(event.NoteIndex == note);
    CHECK(event.Part == part);
    CHECK(event.Result == result);
}

void CheckNear(glm::vec2 actual, glm::vec2 expected)
{
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(0.001));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(0.001));
}

} // namespace

TEST_CASE("Circle difficulty: radius, approach time, and hit windows follow osu!")
{
    hyoshi::circle::CircleDifficulty difficulty;
    difficulty.CircleSize = 4.0f;
    CHECK(difficulty.GetRadius() == doctest::Approx(36.48f));

    difficulty.ApproachRate = 3.0f;
    CHECK(difficulty.GetApproachUs() == 1'440'000);
    difficulty.ApproachRate = 5.0f;
    CHECK(difficulty.GetApproachUs() == 1'200'000);
    difficulty.ApproachRate = 9.0f;
    CHECK(difficulty.GetApproachUs() == 600'000);
    difficulty.ApproachRate = 10.0f;
    CHECK(difficulty.GetApproachUs() == 450'000);

    difficulty.OverallDifficulty = 5.0f;
    const hyoshi::circle::CircleWindows windows = difficulty.GetWindows();
    CHECK(windows.GreatUs == 50'000);
    CHECK(windows.GoodUs == 100'000);
    CHECK(windows.MehUs == 150'000);
    CHECK(windows.Classify(50'000) == Judgment::Great);
    CHECK(windows.Classify(-50'000) == Judgment::Great);
    CHECK(windows.Classify(50'001) == Judgment::Good);
    CHECK(windows.Classify(-100'000) == Judgment::Good);
    CHECK(windows.Classify(100'001) == Judgment::Bad);
}

TEST_CASE("osu!standard import: notes, combos, sliders, timing, and metadata")
{
    const CircleChart chart = ImportChart(OSU_STANDARD);

    CHECK(chart.Info.Metadata.Title == "Test Song");
    CHECK(chart.Info.Metadata.Artist == "Test Artist");
    CHECK(chart.Info.Metadata.Charter == "Someone");
    CHECK(chart.Info.Metadata.DifficultyName == "Normal");
    CHECK(chart.Info.Metadata.PreviewStartUs == 2'000'000);
    CHECK(chart.Info.AudioFile == "audio.mp3");
    CHECK(chart.BackgroundFile == "bg.jpg");
    CHECK(chart.AudioLeadInUs == 500'000);

    CHECK(chart.Difficulty.CircleSize == doctest::Approx(4.0f));
    CHECK(chart.Difficulty.ApproachRate == doctest::Approx(5.0f));
    CHECK(chart.Difficulty.GetApproachUs() == 1'200'000);

    REQUIRE(chart.Info.Timing.size() == 1);
    CHECK(chart.Info.Timing[0].Time == 1'000'000);
    CHECK(chart.Info.Timing[0].Bpm == doctest::Approx(120.0));
    REQUIRE(chart.Info.ScrollVelocity.size() == 2);
    CHECK(chart.Info.ScrollVelocity[0].Time == 1'000'000);
    CHECK(chart.Info.ScrollVelocity[0].Multiplier == doctest::Approx(1.0));
    CHECK(chart.Info.ScrollVelocity[1].Time == 3'000'000);
    CHECK(chart.Info.ScrollVelocity[1].Multiplier == doctest::Approx(2.0));

    REQUIRE(chart.Notes.size() == 6);
    const std::vector<NoteType> types{NoteType::Circle, NoteType::Circle,  NoteType::Slider,
                                      NoteType::Slider, NoteType::Spinner, NoteType::Circle};
    const std::vector<uint32_t> comboNumbers{1, 2, 3, 1, 1, 1};
    for (size_t i = 0; i < chart.Notes.size(); ++i)
    {
        CAPTURE(i);
        CHECK(chart.Notes[i].Type == types[i]);
        CHECK(chart.Notes[i].ComboNumber == comboNumbers[i]);
    }
    CHECK(chart.Notes[0].Time == 1'000'000);
    CHECK(chart.Notes[0].EndTime == 1'000'000);
    // The note after a spinner starts a combo even without the bit.
    CHECK(chart.Notes[5].IsNewCombo);
    CHECK(chart.GetJudgmentCount() == 5);
    // Three circles, a slider's head and tail, and the other's head, repeat, and tail. At 100
    // units a tick, neither slider is long enough for one.
    CHECK(chart.GetMaxCombo() == 8);

    // 100 units at 100 units a beat: one beat at 120 BPM.
    const CircleNote& slider = chart.Notes[2];
    CHECK(slider.EndTime == 2'500'000);
    CHECK(slider.Length == doctest::Approx(100.0f));
    REQUIRE(slider.PathPointCount >= 2);
    CheckNear(hyoshi::circle::GetPathPosition(chart, slider, 0.0f), {100.0f, 200.0f});
    CheckNear(hyoshi::circle::GetPathPosition(chart, slider, 1.0f), {200.0f, 200.0f});
    CheckNear(hyoshi::circle::GetBallPosition(chart, slider, 2'250'000), {150.0f, 200.0f});
    CheckNear(hyoshi::circle::GetBallPosition(chart, slider, 9'000'000), {200.0f, 200.0f});

    // Twice the velocity: half a beat each way, there and back.
    const CircleNote& repeat = chart.Notes[3];
    CHECK(repeat.Passes == 2);
    CHECK(repeat.EndTime == 3'500'000);
    CheckNear(hyoshi::circle::GetBallPosition(chart, repeat, 3'125'000), {350.0f, 200.0f});
    CheckNear(hyoshi::circle::GetBallPosition(chart, repeat, 3'250'000), {400.0f, 200.0f});
    CheckNear(hyoshi::circle::GetBallPosition(chart, repeat, 3'375'000), {350.0f, 200.0f});
    CheckNear(hyoshi::circle::GetBallPosition(chart, repeat, 3'500'000), {300.0f, 200.0f});

    CHECK(chart.GetEndTime() == 6'000'000);

    const CircleNote& spinner = chart.Notes[4];
    CHECK(spinner.Time == 4'000'000);
    CHECK(spinner.EndTime == 5'000'000);
}

TEST_CASE("osu!standard import: a red line resets slider velocity")
{
    // 2x from 2 s, so the first slider takes half a beat. A red line at 2.9 s puts the second back
    // to 1x: one beat a pass.
    const CircleChart chart = ImportChart(
        WithLine(OSU_STANDARD, "3000,-50,4,1,0,100,0,0\n", "2000,-50,4,1,0,100,0,0\n2900,500,4,1,0,100,1,0\n"));
    CHECK(chart.Notes[2].EndTime == 2'250'000);
    CHECK(chart.Notes[3].EndTime == 4'000'000);
}

TEST_CASE("osu!standard import: old formats, other modes, empty maps, and one-point sliders")
{
    const CircleChart old = ImportChart(WithLine(OSU_STANDARD, "osu file format v14", "osu file format v4"));
    CHECK(old.Notes[0].Time == 1'024'000);

    CHECK_FALSE(hyoshi::osu::ImportOsuStandard(WithLine(OSU_STANDARD, "Mode: 0", "Mode: 3")));

    const std::string header = std::string(OSU_STANDARD).substr(0, std::string(OSU_STANDARD).find("100,100,1000"));
    CHECK_FALSE(hyoshi::osu::ImportOsuStandard(header));

    hyoshi::Result<hyoshi::osu::OsuStandardImportResult> onePoint =
        hyoshi::osu::ImportOsuStandard(WithLine(OSU_STANDARD, "L|200:200,1,100", "L,1,100"));
    REQUIRE(onePoint);
    CHECK(onePoint.Value().Chart.Notes[2].Type == NoteType::Circle);
    CHECK_FALSE(onePoint.Value().Warnings.empty());

    // Old files have no ApproachRate and use OverallDifficulty; a present one wins.
    const CircleChart withRate =
        ImportChart(WithLine(OSU_STANDARD, "OverallDifficulty:5\n", "OverallDifficulty:5\nApproachRate:9\n"));
    CHECK(withRate.Difficulty.ApproachRate == doctest::Approx(9.0f));
}

TEST_CASE("Stacking pulls notes on the same spot apart, earlier ones up and left")
{
    // AR 9: 600 ms approach, so notes within 420 ms stack at leniency 0.7.
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.ApproachRate = 9.0f;
    chart.Difficulty.StackLeniency = 0.7f;
    chart.Notes = {Circle(1'000'000, {200.0f, 200.0f}), Circle(1'100'000, {200.0f, 200.0f}),
                   Circle(1'200'000, {201.0f, 201.0f}), Circle(3'000'000, {200.0f, 200.0f})};
    chart.Notes[0].IsNewCombo = true;
    FinishCircleChart(chart);

    // Scale 0.57 at CS 4, so 3.648 units per level.
    CheckNear(chart.Notes[0].StackOffset, {-7.296f, -7.296f});
    CheckNear(chart.Notes[1].StackOffset, {-3.648f, -3.648f});
    CheckNear(chart.Notes[2].StackOffset, {0.0f, 0.0f});
    // Too late to stack on the others.
    CheckNear(chart.Notes[3].StackOffset, {0.0f, 0.0f});

    // Leniency 0 turns stacking off.
    chart.Difficulty.StackLeniency = 0.0f;
    FinishCircleChart(chart);
    CheckNear(chart.Notes[0].StackOffset, {0.0f, 0.0f});
}

TEST_CASE("Circle judge: presses are graded by timing, and unstruck notes miss")
{
    const CircleChart chart = MakeChart({Circle(1'000'000, {100.0f, 100.0f}), Circle(2'000'000, {100.0f, 100.0f}),
                                         Circle(3'000'000, {100.0f, 100.0f}), Circle(4'000'000, {100.0f, 100.0f})});
    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;

    // Too early to reach the first note: the press does nothing.
    judge.ProcessAction(Press(800'000, {100.0f, 100.0f}), events);
    CHECK(events.empty());
    judge.ProcessAction(Release(810'000, {100.0f, 100.0f}), events);
    judge.ProcessAction(Press(1'040'000, {100.0f, 100.0f}), events);
    judge.ProcessAction(Release(1'050'000, {100.0f, 100.0f}), events);
    // Off the circle (radius 36.48).
    judge.ProcessAction(Press(1'990'000, {140.0f, 100.0f}), events);
    judge.ProcessAction(Release(1'995'000, {140.0f, 100.0f}), events);
    judge.ProcessAction(Press(2'090'000, {130.0f, 100.0f}), events);
    judge.ProcessAction(Release(2'100'000, {130.0f, 100.0f}), events);
    judge.ProcessAction(Press(2'860'000, {100.0f, 100.0f}), events);
    judge.ProcessAction(Release(2'870'000, {100.0f, 100.0f}), events);

    REQUIRE(events.size() == 3);
    CHECK(events[0].NoteIndex == 0);
    CHECK(events[0].Result == Judgment::Great);
    CHECK(events[0].ErrorUs == 40'000);
    CHECK(events[1].NoteIndex == 1);
    CHECK(events[1].Result == Judgment::Good);
    CHECK(events[2].NoteIndex == 2);
    CHECK(events[2].Result == Judgment::Bad);
    CHECK(events[2].ErrorUs == -140'000);
    CHECK_FALSE(judge.IsFinished());

    // The last note misses once its Bad window has passed, stamped with that moment.
    judge.Advance(4'150'000, events);
    CHECK(events.size() == 3);
    judge.Advance(4'500'000, events);
    REQUIRE(events.size() == 4);
    CHECK(events[3].NoteIndex == 3);
    CHECK(events[3].Result == Judgment::Miss);
    CHECK(events[3].Time == 4'150'001);
    CHECK(judge.GetNoteState(3) == NoteState::Missed);
    CHECK(judge.IsFinished());
}

TEST_CASE("Circle judge: note lock blocks early presses, then force-misses skipped circles")
{
    const CircleChart chart = MakeChart({Circle(1'000'000, {100.0f, 100.0f}), Circle(1'100'000, {300.0f, 300.0f})});

    // Before the first circle's time, striking the second is blocked and spent.
    {
        CircleJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(Press(950'000, {300.0f, 300.0f}), events);
        CHECK(events.empty());
        CHECK(judge.GetNoteState(0) == NoteState::Pending);
        CHECK(judge.GetNoteState(1) == NoteState::Pending);
    }

    // After it, the second is struck and the first is missed.
    {
        CircleJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(Press(1'050'000, {300.0f, 300.0f}), events);
        REQUIRE(events.size() == 2);
        CHECK(events[0].NoteIndex == 0);
        CHECK(events[0].Result == Judgment::Miss);
        CHECK(events[0].Time == 1'050'000);
        CHECK(events[1].NoteIndex == 1);
        CHECK(events[1].Result == Judgment::Great);
        CHECK(events[1].ErrorUs == -50'000);
        CHECK(judge.IsFinished());
    }
}

TEST_CASE("Circle judge: chords don't block or miss each other")
{
    const CircleChart chart = MakeChart({Circle(1'000'000, {100.0f, 100.0f}), Circle(1'000'500, {300.0f, 300.0f})});
    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    judge.ProcessAction(Press(990'000, {300.0f, 300.0f}, 0), events);
    judge.ProcessAction(Press(1'010'000, {100.0f, 100.0f}, 1), events);
    REQUIRE(events.size() == 2);
    CHECK(events[0].NoteIndex == 1);
    CHECK(events[0].Result == Judgment::Great);
    CHECK(events[1].NoteIndex == 0);
    CHECK(events[1].Result == Judgment::Great);
}

TEST_CASE("Circle judge: a press takes the earliest note under it, even while another pointer follows")
{
    // A slider and a circle on its head a moment later (stacking off), then a second slider during
    // the first.
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.StackLeniency = 0.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {300.0f, 200.0f}, {200.0f, 300.0f}, {300.0f, 300.0f}};
    chart.Notes = {Slider(1'000'000, 1'500'000, {200.0f, 200.0f}, 0, 100.0f), Circle(1'100'000, {200.0f, 200.0f}),
                   Slider(1'200'000, 1'400'000, {200.0f, 300.0f}, 2, 100.0f)};
    chart.Notes[0].IsNewCombo = true;
    FinishCircleChart(chart);
    // The first slider ends last.
    CHECK(chart.GetEndTime() == 1'500'000);
    // Without ticks, a head and a tail each.
    CHECK(chart.GetMaxCombo() == 5);

    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    judge.ProcessAction(Press(1'000'000, {200.0f, 200.0f}), events);
    REQUIRE(events.size() == 1);
    CheckPart(events[0], 0, 1, Judgment::Great);
    CHECK(events[0].IsTimed);
    CHECK(judge.GetNoteState(0) == NoteState::Hit);
    CHECK(judge.IsFollowing(0));

    // A second pointer takes the circle, then the other slider's head.
    judge.ProcessAction(Press(1'100'000, {200.0f, 200.0f}, 1), events);
    judge.ProcessAction(Release(1'105'000, {200.0f, 200.0f}, 1), events);
    judge.ProcessAction(Move(1'200'000, {240.0f, 200.0f}), events);
    judge.ProcessAction(Press(1'210'000, {200.0f, 300.0f}, 1), events);
    REQUIRE(events.size() == 3);
    CHECK(events[1].NoteIndex == 1);
    CHECK(events[1].Part == 0);
    CHECK(events[1].Result == Judgment::Great);
    CheckPart(events[2], 2, 1, Judgment::Great);
    CHECK(events[2].ErrorUs == 10'000);

    // Letting go misses the second slider's tail (judged 36 ms early, at 1.364 s). The first is
    // followed to its end. Sliders have no result of their own.
    judge.ProcessAction(Release(1'300'000, {250.0f, 300.0f}, 1), events);
    CHECK_FALSE(judge.IsFollowing(2));
    judge.ProcessAction(Move(1'460'000, {292.0f, 200.0f}), events);
    REQUIRE(events.size() == 4);
    CheckPart(events[3], 2, 2, Judgment::Miss);
    CHECK_FALSE(events[3].IsTimed);
    CHECK(events[3].Time == 1'400'000);
    CHECK(judge.IsFollowing(0));

    judge.Advance(2'000'000, events);
    REQUIRE(events.size() == 5);
    CheckPart(events[4], 0, 2, Judgment::Great);
    CHECK(events[4].Time == 1'500'000);
    CHECK(judge.IsFinished());
}

TEST_CASE("Circle sliders: ticks along each pass, repeats, and a tail judged early")
{
    // 200 units there and back over a second, a tick every 50: three a pass, met in reverse on the
    // way back, 4 units (10 ms of travel) clear of the ends.
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.StackLeniency = 0.0f;
    chart.ControlPoints = {{100.0f, 100.0f}, {300.0f, 100.0f}};
    chart.Notes = {Slider(1'000'000, 2'000'000, {100.0f, 100.0f}, 0, 200.0f)};
    chart.Notes[0].IsNewCombo = true;
    chart.Notes[0].Passes = 2;
    chart.Notes[0].TickDistance = 50.0f;
    FinishCircleChart(chart);

    const CircleNote& slider = chart.Notes[0];
    REQUIRE(slider.PartCount == 9);
    CHECK(chart.GetMaxCombo() == 9);
    struct Expected
    {
        SliderPartType Type;
        SongTimeUs Time;
        float Progress;
    };
    const std::vector<Expected> expected{
        {SliderPartType::Head, 1'000'000, 0.0f},   {SliderPartType::Tick, 1'125'000, 0.25f},
        {SliderPartType::Tick, 1'250'000, 0.5f},   {SliderPartType::Tick, 1'375'000, 0.75f},
        {SliderPartType::Repeat, 1'500'000, 1.0f}, {SliderPartType::Tick, 1'625'000, 0.75f},
        {SliderPartType::Tick, 1'750'000, 0.5f},   {SliderPartType::Tick, 1'875'000, 0.25f},
        {SliderPartType::Tail, 2'000'000, 0.0f}};
    for (uint32_t part = 1; part <= slider.PartCount; ++part)
    {
        CAPTURE(part);
        const SliderPart* sliderPart = hyoshi::circle::GetSliderPart(chart, 0, part);
        REQUIRE(sliderPart != nullptr);
        CHECK(sliderPart->Type == expected[part - 1].Type);
        CHECK(sliderPart->Time == expected[part - 1].Time);
        CHECK(sliderPart->Progress == doctest::Approx(expected[part - 1].Progress));
    }
    CHECK(hyoshi::circle::GetSliderPart(chart, 0, 9)->JudgeTime == 1'964'000);
    CHECK(hyoshi::circle::GetSliderPart(chart, 0, 0) == nullptr);
    CHECK(hyoshi::circle::GetSliderPart(chart, 0, 10) == nullptr);

    // Struck, then followed, but lost for the second tick (more than 2.4 radii off) and found
    // again within a radius of the ball, and let go before the tail: 7 parts of 9, a Good.
    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    const std::vector<GameplayAction> actions{Press(1'000'000, {100.0f, 100.0f}), Move(1'120'000, {148.0f, 100.0f}),
                                              Move(1'200'000, {180.0f, 300.0f}),  Move(1'300'000, {220.0f, 100.0f}),
                                              Move(1'370'000, {248.0f, 100.0f}),  Move(1'495'000, {298.0f, 100.0f}),
                                              Move(1'620'000, {252.0f, 100.0f}),  Move(1'745'000, {202.0f, 100.0f}),
                                              Move(1'870'000, {152.0f, 100.0f}),  Release(1'900'000, {140.0f, 100.0f})};
    for (const GameplayAction& action : actions)
    {
        judge.ProcessAction(action, events);
    }
    judge.Advance(3'000'000, events);
    CHECK(judge.IsFinished());
    REQUIRE(events.size() == 9);
    const std::vector<Judgment> parts{Judgment::Great, Judgment::Great, Judgment::Miss,
                                      Judgment::Great, Judgment::Great, Judgment::Great,
                                      Judgment::Great, Judgment::Great, Judgment::Miss};
    for (uint32_t part = 1; part <= 9; ++part)
    {
        CAPTURE(part);
        CheckPart(events[part - 1], 0, part, parts[part - 1]);
        CHECK(events[part - 1].IsTimed == (part == 1));
    }

    // The head is worth 300, each tick and repeat 30, and the tail 150, all counting for accuracy,
    // with a combo each; a missed tick breaks it, a missed tail doesn't.
    CircleScore score;
    score.Reset(chart);
    for (const JudgmentEvent& event : events)
    {
        score.Add(event);
    }
    CHECK(score.GetCombo() == 5);
    CHECK(score.GetMaxCombo() == 5);
    CHECK(score.GetJudgedCount() == 1);
    CHECK(score.GetCount(Judgment::Great) == 1);
    // 480 of 660.
    CHECK(score.GetAccuracyBasisPoints() == 7'272);
    const auto root = [](double combo) { return std::sqrt(combo); };
    const double comboPortion =
        300.0 + (30.0 * root(2)) + (30.0 * root(1)) + (30.0 * (root(2) + root(3) + root(4) + root(5)));
    const double maxComboPortion =
        300.0 + (30.0 * (root(2) + root(3) + root(4) + root(5) + root(6) + root(7) + root(8))) + (150.0 * root(9));
    CHECK(score.GetScore() == StandardisedScore(comboPortion, maxComboPortion, 480.0 / 660.0, 1.0));
    CHECK(std::string(score.GetRank()) == "C");
}

TEST_CASE("Circle sliders: a head struck late judges the parts already passed by where it was struck")
{
    // Two sliders there and back, 0.2 s each: the first 50 units long, so its turn is within
    // reach of the head (2.4 radii, 87.6 units), the second 100, so it isn't.
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.StackLeniency = 0.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {250.0f, 200.0f}, {200.0f, 300.0f}, {300.0f, 300.0f}};
    chart.Notes = {Slider(1'000'000, 1'200'000, {200.0f, 200.0f}, 0, 50.0f),
                   Slider(2'000'000, 2'200'000, {200.0f, 300.0f}, 2, 100.0f)};
    chart.Notes[0].IsNewCombo = true;
    chart.Notes[0].Passes = 2;
    chart.Notes[1].Passes = 2;
    FinishCircleChart(chart);

    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    // 120 ms late: a 50. The turn (at 1.1 s) waited for the head, and is hit from there.
    judge.ProcessAction(Press(1'120'000, {200.0f, 200.0f}), events);
    REQUIRE(events.size() == 2);
    CheckPart(events[0], 0, 1, Judgment::Bad);
    CHECK(events[0].ErrorUs == 120'000);
    CheckPart(events[1], 0, 2, Judgment::Great);
    CHECK_FALSE(events[1].IsTimed);
    // Onto the ball for the tail.
    judge.ProcessAction(Move(1'130'000, {235.0f, 200.0f}), events);
    judge.ProcessAction(Release(1'190'000, {205.0f, 200.0f}), events);
    REQUIRE(events.size() == 3);
    CheckPart(events[2], 0, 3, Judgment::Great);

    // The second's turn is out of reach of its head: missed.
    judge.ProcessAction(Press(2'120'000, {200.0f, 300.0f}), events);
    judge.ProcessAction(Release(2'130'000, {200.0f, 300.0f}), events);
    judge.Advance(3'000'000, events);
    REQUIRE(events.size() == 6);
    CheckPart(events[3], 1, 1, Judgment::Bad);
    CheckPart(events[4], 1, 2, Judgment::Miss);
    CheckPart(events[5], 1, 3, Judgment::Miss);
    CHECK(judge.IsFinished());
}

TEST_CASE("Circle sliders: held through without striking the head, the rest is still followed")
{
    CircleChart chart;
    chart.Difficulty.CircleSize = 4.0f;
    chart.Difficulty.OverallDifficulty = 5.0f;
    chart.Difficulty.StackLeniency = 0.0f;
    chart.ControlPoints = {{200.0f, 200.0f}, {300.0f, 200.0f}};
    chart.Notes = {Slider(1'000'000, 1'500'000, {200.0f, 200.0f}, 0, 100.0f)};
    chart.Notes[0].IsNewCombo = true;
    FinishCircleChart(chart);

    // Down on the head long before it can be struck, so the press does nothing; following starts
    // with the slider, and the head misses at the end of its window.
    CircleJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    judge.ProcessAction(Press(500'000, {200.0f, 200.0f}), events);
    judge.ProcessAction(Move(1'460'000, {292.0f, 200.0f}), events);
    judge.Advance(2'000'000, events);
    REQUIRE(events.size() == 2);
    CheckPart(events[0], 0, 1, Judgment::Miss);
    CHECK(events[0].Time == 1'150'001);
    CHECK(judge.GetNoteState(0) == NoteState::Missed);
    CheckPart(events[1], 0, 2, Judgment::Great);

    // The missed head broke the combo; the tail added to it.
    CircleScore score;
    score.Reset(chart);
    for (const JudgmentEvent& event : events)
    {
        score.Add(event);
    }
    CHECK(score.GetMaxCombo() == 1);
}

TEST_CASE("osu!standard import: hit sounds, from the notes and the sample points")
{
    const std::string text =
        WithLine(WithLine(WithLine(OSU_STANDARD, "Mode: 0\n", "Mode: 0\nSampleSet: Soft\n"), "[TimingPoints]\n",
                          "[TimingPoints]\n1000,500,4,0,0,60,1,0\n2000,-100,4,3,2,40,0,0\n"),
                 "1000,500,4,1,0,100,1,0\n3000,-50,4,1,0,100,0,0\n", "");
    const std::string objects = "[HitObjects]\n"
                                "100,100,1000,5,2,0:0:0:0:\n"
                                "100,100,1500,1,8,1:2:0:80:\n"
                                "100,200,2000,2,4,L|200:200,1,100,2|8,1:0|2:3,0:0:0:0:\n"
                                "100,100,3000,1,0,0:0:5:0:hit.wav\n";
    const CircleChart chart = ImportChart(text.substr(0, text.find("[HitObjects]")) + objects);
    REQUIRE(chart.Notes.size() == 4);
    REQUIRE(chart.SamplePoints.size() == 2);
    // A sample set of 0 is the map's.
    CHECK(chart.SamplePoints[0].Bank == SampleBank::Soft);
    CHECK(chart.SamplePoints[1].Bank == SampleBank::Drum);
    CHECK(chart.SamplePoints[1].CustomIndex == 2);
    CHECK(chart.SamplePoints[1].Volume == 40);

    // A whistle, all else from the sample point.
    hyoshi::circle::ResolvedHitSound sound = hyoshi::circle::GetHitSound(chart, 0, 0);
    CHECK(sound.Bank == SampleBank::Soft);
    CHECK(sound.AdditionBank == SampleBank::Soft);
    CHECK(sound.Additions == hyoshi::circle::ADDITION_WHISTLE);
    CHECK(sound.CustomIndex == 0);
    CHECK(sound.Volume == doctest::Approx(0.6f));

    // Its own banks and volume.
    sound = hyoshi::circle::GetHitSound(chart, 1, 0);
    CHECK(sound.Bank == SampleBank::Normal);
    CHECK(sound.AdditionBank == SampleBank::Soft);
    CHECK(sound.Additions == hyoshi::circle::ADDITION_CLAP);
    CHECK(sound.Volume == doctest::Approx(0.8f));

    // A slider's edges have their own sounds and banks; its body (part 0) has the object's.
    sound = hyoshi::circle::GetHitSound(chart, 2, 1);
    CHECK(sound.Bank == SampleBank::Normal);
    CHECK(sound.AdditionBank == SampleBank::Normal);
    CHECK(sound.Additions == hyoshi::circle::ADDITION_WHISTLE);
    CHECK(sound.CustomIndex == 2);
    CHECK(sound.Volume == doctest::Approx(0.4f));
    sound = hyoshi::circle::GetHitSound(chart, 2, 2);
    CHECK(sound.Bank == SampleBank::Soft);
    CHECK(sound.AdditionBank == SampleBank::Drum);
    CHECK(sound.Additions == hyoshi::circle::ADDITION_CLAP);
    sound = hyoshi::circle::GetHitSound(chart, 2, 0);
    CHECK(sound.Bank == SampleBank::Drum);
    CHECK(sound.Additions == hyoshi::circle::ADDITION_FINISH);

    // A file instead, with its custom index.
    sound = hyoshi::circle::GetHitSound(chart, 3, 0);
    CHECK(sound.File == "hit.wav");
    CHECK(sound.CustomIndex == 5);

    // Tick distance: SliderMultiplier * 100 * velocity / SliderTickRate.
    CHECK(chart.Notes[2].TickDistance == doctest::Approx(100.0f));
}

TEST_CASE("Circle autoplay: every note Great, on imported and stacked charts")
{
    std::vector<CircleChart> charts;
    charts.push_back(ImportChart(OSU_STANDARD));
    // A stack, a chord, and back-to-back sliders.
    std::string dense = OSU_STANDARD;
    dense = WithLine(dense, "OverallDifficulty:5\n", "OverallDifficulty:10\nApproachRate:9\n");
    // Ticks on every slider.
    dense = WithLine(dense, "SliderTickRate:1\n", "SliderTickRate:4\n");
    dense += "256,192,7000,5,0,0:0:0:0:\n"
             "256,192,7100,1,0,0:0:0:0:\n"
             "256,192,7200,1,0,0:0:0:0:\n"
             "100,300,7500,1,0,0:0:0:0:\n"
             "400,300,7500,1,0,0:0:0:0:\n"
             "100,100,8000,2,0,B|200:50|300:100,1,200\n"
             "300,100,8300,2,0,P|350:150|300:200,2,100\n"
             "300,200,8400,1,0,0:0:0:0:\n";
    charts.push_back(ImportChart(dense));

    for (const CircleChart& chart : charts)
    {
        const std::vector<JudgmentEvent> events =
            RunReplay(chart, hyoshi::circle::MakeAutoplayActions(chart), chart.GetEndTime() + 1'000'000);
        CHECK(CountResults(events) == chart.GetJudgmentCount());
        CHECK(chart.GetMaxCombo() > chart.GetJudgmentCount());
        CircleScore score;
        score.Reset(chart);
        for (const JudgmentEvent& event : events)
        {
            CAPTURE(event.NoteIndex);
            CAPTURE(event.Part);
            CHECK(event.Result == Judgment::Great);
            CHECK(event.ErrorUs == 0);
            score.Add(event);
        }
        CHECK(score.GetAccuracyBasisPoints() == 10'000);
        CHECK(score.GetMaxCombo() == chart.GetMaxCombo());
        CHECK(std::string(score.GetRank()) == "SS");
        // Every judgment in the order a perfect run makes them.
        CHECK(score.GetScore() == 1'000'000);
    }
}

TEST_CASE("Circle judging is deterministic and doesn't depend on frame timing")
{
    const CircleChart chart = ImportChart(OSU_STANDARD);
    constexpr SongTimeUs END = 8'000'000;

    // Autoplay made sloppy: presses and releases shifted by up to 60 ms, some aim off the note, and
    // every fourth press dropped.
    std::vector<GameplayAction> actions;
    uint32_t pressCount = 0;
    int32_t index = 0;
    for (GameplayAction action : hyoshi::circle::MakeAutoplayActions(chart))
    {
        ++index;
        action.Time += static_cast<SongTimeUs>((index * 37) % 121 - 60) * 1'000;
        if (action.Type == static_cast<uint16_t>(CircleAction::Press))
        {
            if (++pressCount % 4 == 0)
            {
                continue;
            }
            if (pressCount % 3 == 0)
            {
                action.X += 50 * static_cast<int32_t>(hyoshi::circle::ACTION_POSITION_SCALE);
            }
        }
        actions.push_back(action);
    }
    std::stable_sort(actions.begin(), actions.end(),
                     [](const GameplayAction& a, const GameplayAction& b) { return a.Time < b.Time; });

    for (const SongTimeUs frameUs : {4'167, 8'333, 16'667})
    {
        CAPTURE(frameUs);
        // Live play: each frame delivers the actions since the last one, then advances to 30 ms
        // before the frame for misses. Actions are clamped to the judge's time and recorded.
        CircleJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> live;
        std::vector<GameplayAction> recorded;
        size_t nextAction = 0;
        for (SongTimeUs frame = 0; frame < END; frame += frameUs)
        {
            while (nextAction < actions.size() && actions[nextAction].Time <= frame)
            {
                GameplayAction action = actions[nextAction++];
                action.Time = std::max(action.Time, judge.GetTime());
                recorded.push_back(action);
                judge.ProcessAction(action, live);
            }
            judge.Advance(frame - 30'000, live);
        }
        judge.Advance(END, live);
        CHECK(judge.IsFinished());

        const std::vector<JudgmentEvent> replayed = RunReplay(chart, recorded, END);
        CHECK(SameEvents(live, replayed));
        CHECK(CountResults(live) == chart.GetJudgmentCount());
        CHECK(std::any_of(live.begin(), live.end(), [](const JudgmentEvent& e) { return e.Result == Judgment::Miss; }));
        CHECK(std::any_of(live.begin(), live.end(), [](const JudgmentEvent& e) { return e.Result != Judgment::Miss; }));
    }
}

TEST_CASE("Circle score: lazer's standardised score, weighted accuracy, and ranks")
{
    std::vector<CircleNote> notes;
    for (int i = 0; i < 5; ++i)
    {
        notes.push_back(Circle(1'000'000 + (i * 500'000), {100.0f, 100.0f}));
    }
    const CircleChart chart = MakeChart(notes);
    const auto event = [](uint32_t note, Judgment result)
    {
        JudgmentEvent e;
        e.NoteIndex = note;
        e.Result = result;
        return e;
    };

    CircleScore score;
    score.Reset(chart);
    CHECK(score.GetScore() == 0);
    CHECK(score.GetAccuracyBasisPoints() == 10'000);
    CHECK(std::string(score.GetRank()) == "-");

    // A perfect run is 1,000,000.
    for (uint32_t i = 0; i < 5; ++i)
    {
        score.Add(event(i, Judgment::Great));
    }
    CHECK(score.GetScore() == 1'000'000);
    CHECK(std::string(score.GetRank()) == "SS");

    score.Reset(chart);
    const std::vector<Judgment> results{Judgment::Great, Judgment::Great, Judgment::Good, Judgment::Miss,
                                        Judgment::Bad};
    for (uint32_t i = 0; i < 5; ++i)
    {
        score.Add(event(i, results[i]));
    }
    const double comboPortion = 300.0 + (300.0 * std::sqrt(2.0)) + (100.0 * std::sqrt(3.0)) + 50.0;
    double maxComboPortion = 0.0;
    for (int i = 1; i <= 5; ++i)
    {
        maxComboPortion += 300.0 * std::sqrt(static_cast<double>(i));
    }
    CHECK(score.GetScore() == StandardisedScore(comboPortion, maxComboPortion, 0.5, 1.0));
    CHECK(score.GetCombo() == 1);
    CHECK(score.GetMaxCombo() == 3);
    CHECK(score.GetCount(Judgment::Great) == 2);
    CHECK(score.GetCount(Judgment::Miss) == 1);
    CHECK(score.GetJudgedCount() == 5);
    // 750 of 1500.
    CHECK(score.GetAccuracyBasisPoints() == 5'000);
    CHECK(std::string(score.GetRank()) == "D");

    // Partway: accuracy's share grows with the judgments made.
    score.Reset(chart);
    score.Add(event(0, Judgment::Great));
    CHECK(score.GetScore() == StandardisedScore(300.0, maxComboPortion, 1.0, 0.2));

    // 19 Greats and a Good: 96.66%.
    score.Reset(chart);
    for (int i = 0; i < 19; ++i)
    {
        score.Add(event(0, Judgment::Great));
    }
    score.Add(event(0, Judgment::Good));
    CHECK(score.GetAccuracyBasisPoints() == 9'666);
    CHECK(std::string(score.GetRank()) == "S");

    // Over 95% with a miss is an A.
    score.Reset(chart);
    for (int i = 0; i < 39; ++i)
    {
        score.Add(event(0, Judgment::Great));
    }
    score.Add(event(0, Judgment::Miss));
    CHECK(score.GetAccuracyBasisPoints() == 9'750);
    CHECK(std::string(score.GetRank()) == "A");
}
