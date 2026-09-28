#include "mania/ManiaChart.h"
#include "mania/ManiaJudge.h"
#include "rhythm/ScoreSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using hyoshi::SongTimeUs;
using hyoshi::mania::MakeAction;
using hyoshi::mania::ManiaAction;
using hyoshi::mania::ManiaChart;
using hyoshi::mania::ManiaJudge;
using hyoshi::rhythm::GameplayAction;
using hyoshi::rhythm::Judgment;
using hyoshi::rhythm::JudgmentEvent;

namespace
{

struct NoteSpec
{
    SongTimeUs Time;
    SongTimeUs EndTime;
    uint32_t Lane;
};

ManiaChart MakeChart(const std::vector<NoteSpec>& notes, uint32_t laneCount = 4)
{
    hyoshi::rhythm::Chart chart;
    chart.Mode = "mania";
    std::vector<uint32_t> lanes;
    for (const NoteSpec& note : notes)
    {
        chart.Notes.push_back({note.Time, note.EndTime});
        lanes.push_back(note.Lane);
    }
    hyoshi::Result<ManiaChart> result = hyoshi::mania::BuildManiaChart(chart, laneCount, lanes);
    REQUIRE(result);
    return std::move(result).Value();
}

NoteSpec Tap(SongTimeUs time, uint32_t lane)
{
    return {time, time, lane};
}

// Feeds actions like a replay: each action, then a final advance.
std::vector<JudgmentEvent> RunReplay(const ManiaChart& chart, const std::vector<GameplayAction>& actions,
                                     SongTimeUs end)
{
    ManiaJudge judge;
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
            a[i].Time != b[i].Time || a[i].IsTail != b[i].IsTail)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("A press is judged by its distance from the note, inclusive at each window's edge")
{
    const std::vector<std::tuple<int64_t, Judgment>> cases{
        {0, Judgment::Perfect},    {16'000, Judgment::Perfect}, {-16'000, Judgment::Perfect},
        {16'001, Judgment::Great}, {-40'000, Judgment::Great},  {40'001, Judgment::Good},
        {-73'000, Judgment::Good}, {73'001, Judgment::Bad},     {103'000, Judgment::Bad},
        {-103'000, Judgment::Bad},
    };
    for (const auto& [error, expected] : cases)
    {
        CAPTURE(error);
        const ManiaChart chart = MakeChart({Tap(1'000'000, 1)});
        ManiaJudge judge;
        judge.Reset(chart);
        std::vector<JudgmentEvent> events;
        judge.ProcessAction(MakeAction(ManiaAction::Press, 1, 1'000'000 + error), events);
        REQUIRE(events.size() == 1);
        CHECK(events[0].Result == expected);
        CHECK(events[0].ErrorUs == error);
        CHECK(judge.IsFinished());
    }
}

TEST_CASE("Presses outside the window: too early is ignored, too late finds the note already missed")
{
    const ManiaChart chart = MakeChart({Tap(1'000'000, 0)});

    ManiaJudge early;
    early.Reset(chart);
    std::vector<JudgmentEvent> events;
    early.ProcessAction(MakeAction(ManiaAction::Press, 0, 1'000'000 - 103'001), events);
    CHECK(events.empty());
    early.Advance(2'000'000, events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].Result == Judgment::Miss);
    CHECK(events[0].Time == 1'103'001);

    ManiaJudge late;
    late.Reset(chart);
    events.clear();
    late.ProcessAction(MakeAction(ManiaAction::Press, 0, 1'000'000 + 103'001), events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].Result == Judgment::Miss);

    // A press in another lane never judges this note.
    ManiaJudge otherLane;
    otherLane.Reset(chart);
    events.clear();
    otherLane.ProcessAction(MakeAction(ManiaAction::Press, 1, 1'000'000), events);
    CHECK(events.empty());
}

TEST_CASE("Holds: held through the end, released early, released far too early, never pressed")
{
    const ManiaChart chart = MakeChart({{1'000'000, 2'000'000, 0}});
    auto run = [&chart](std::vector<GameplayAction> actions) { return RunReplay(chart, actions, 5'000'000); };

    const std::vector<JudgmentEvent> held =
        run({MakeAction(ManiaAction::Press, 0, 1'000'000), MakeAction(ManiaAction::Release, 0, 2'500'000)});
    REQUIRE(held.size() == 2);
    CHECK(held[0].Result == Judgment::Perfect);
    CHECK_FALSE(held[0].IsTail);
    CHECK(held[1].Result == Judgment::Perfect);
    CHECK(held[1].IsTail);
    CHECK(held[1].Time == 2'000'000);

    const std::vector<JudgmentEvent> early =
        run({MakeAction(ManiaAction::Press, 0, 1'000'000), MakeAction(ManiaAction::Release, 0, 1'950'000)});
    REQUIRE(early.size() == 2);
    CHECK(early[1].Result == Judgment::Good);
    CHECK(early[1].ErrorUs == -50'000);

    const std::vector<JudgmentEvent> tooEarly =
        run({MakeAction(ManiaAction::Press, 0, 1'000'000), MakeAction(ManiaAction::Release, 0, 1'500'000)});
    REQUIRE(tooEarly.size() == 2);
    CHECK(tooEarly[1].Result == Judgment::Miss);

    const std::vector<JudgmentEvent> missed = run({});
    REQUIRE(missed.size() == 2);
    CHECK(missed[0].Result == Judgment::Miss);
    CHECK(missed[1].Result == Judgment::Miss);
    CHECK(missed[1].IsTail);
}

TEST_CASE("One advance judges several lanes in time order")
{
    // Lanes in reverse order, so looping over lanes would give the wrong order.
    const ManiaChart chart = MakeChart({Tap(1'000'000, 3), Tap(1'020'000, 2), Tap(1'050'000, 0)});
    ManiaJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> events;
    judge.Advance(3'000'000, events);
    REQUIRE(events.size() == 3);
    CHECK(events[0].Time == 1'103'001);
    CHECK(events[1].Time == 1'123'001);
    CHECK(events[2].Time == 1'153'001);
}

namespace
{

// A long, busy chart: taps and holds in 7 lanes, from a fixed seed.
ManiaChart MakeBusyChart()
{
    uint32_t seed = 2024;
    auto next = [&seed](uint32_t range)
    {
        seed = (seed * 1'664'525u) + 1'013'904'223u;
        return (seed >> 8) % range;
    };

    std::vector<NoteSpec> notes;
    std::vector<SongTimeUs> laneFree(7, 0);
    for (SongTimeUs time = 1'000'000; time < 120'000'000; time += 60'000 + next(200'000))
    {
        const uint32_t lane = next(7);
        if (time <= laneFree[lane])
        {
            continue;
        }
        const SongTimeUs end = next(4) == 0 ? time + 200'000 + next(800'000) : time;
        notes.push_back({time, end, lane});
        laneFree[lane] = end;
    }
    return MakeChart(notes, 7);
}

// Autoplay with a fixed pattern of timing errors, as a player would produce.
std::vector<GameplayAction> MakeSloppyActions(const ManiaChart& chart)
{
    std::vector<GameplayAction> actions = hyoshi::mania::MakeAutoplayActions(chart);
    const std::vector<int64_t> errors{-120'000, -60'000, -12'000, 0, 9'000, 35'000, 70'000, 101'000, 150'000};
    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (actions[i].Type == static_cast<uint16_t>(ManiaAction::Press))
        {
            actions[i].Time += errors[(i * 7) % errors.size()];
        }
    }
    std::stable_sort(actions.begin(), actions.end(),
                     [](const GameplayAction& a, const GameplayAction& b) { return a.Time < b.Time; });
    return actions;
}

} // namespace

TEST_CASE("Judging is deterministic and doesn't depend on frame timing")
{
    const ManiaChart chart = MakeBusyChart();
    REQUIRE(chart.Notes.size() > 500);
    const std::vector<GameplayAction> actions = MakeSloppyActions(chart);
    constexpr SongTimeUs END = 125'000'000;

    const std::vector<JudgmentEvent> replay = RunReplay(chart, actions, END);
    CHECK(SameEvents(replay, RunReplay(chart, actions, END)));

    // Live play: 120 Hz frames, each delivering the actions that happened since the last frame,
    // then advancing to 30 ms before the frame for misses.
    ManiaJudge judge;
    judge.Reset(chart);
    std::vector<JudgmentEvent> live;
    std::vector<GameplayAction> recorded;
    size_t nextAction = 0;
    for (SongTimeUs frame = 0; frame < END; frame += 8'333)
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

    CHECK(recorded == actions);
    CHECK(SameEvents(live, replay));

    hyoshi::rhythm::ScoreSystem score;
    score.Reset(chart.GetJudgmentCount());
    for (const JudgmentEvent& event : replay)
    {
        score.Add(event);
    }
    CHECK(score.GetJudgedCount() == chart.GetJudgmentCount());
    CHECK(score.GetCount(Judgment::Miss) > 0);
    CHECK(score.GetCount(Judgment::Perfect) > 0);
}

TEST_CASE("Autoplay is all Perfect and scores the maximum")
{
    const ManiaChart chart = MakeBusyChart();
    const std::vector<JudgmentEvent> events = RunReplay(chart, hyoshi::mania::MakeAutoplayActions(chart), 125'000'000);

    hyoshi::rhythm::ScoreSystem score;
    score.Reset(chart.GetJudgmentCount());
    for (const JudgmentEvent& event : events)
    {
        CHECK(event.Result == Judgment::Perfect);
        score.Add(event);
    }
    CHECK(score.GetScore() == 1'000'000);
    CHECK(score.GetAccuracyBasisPoints() == 10'000);
    CHECK(score.GetMaxCombo() == chart.GetJudgmentCount());
}

TEST_CASE("ScoreSystem: combo breaks on Bad and Miss; score scales to the chart")
{
    hyoshi::rhythm::ScoreSystem score;
    score.Reset(4);
    score.Add({0, Judgment::Perfect, 1'000, 0, false});
    score.Add({1, Judgment::Great, -3'000, 0, false});
    CHECK(score.GetCombo() == 2);
    score.Add({2, Judgment::Bad, 90'000, 0, false});
    CHECK(score.GetCombo() == 0);
    score.Add({3, Judgment::Good, 0, 0, false});
    CHECK(score.GetCombo() == 1);
    CHECK(score.GetMaxCombo() == 2);
    // (300 + 200 + 50 + 100) / 1200
    CHECK(score.GetScore() == 541'666);
    CHECK(score.GetAccuracyBasisPoints() == 5'416);
    CHECK(score.GetMeanErrorUs() == 22'000);
}

namespace
{

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open " << path);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

// Replay regression fixtures (DESIGN.md section 23): recorded actions and the result they must
// always produce.
TEST_CASE("Replay fixture: mania-basic")
{
    const std::string directory = HYOSHI_REPLAY_DIR;
    std::istringstream replay(ReadFile(directory + "/mania-basic.replay.txt"));

    std::string chartFile;
    std::map<std::string, int64_t> expected;
    std::vector<GameplayAction> actions;
    SongTimeUs end = 0;
    std::string line;
    while (std::getline(replay, line))
    {
        std::istringstream words(line);
        std::string first;
        if (!(words >> first) || first[0] == '#')
        {
            continue;
        }
        if (first == "chart")
        {
            words >> chartFile;
        }
        else if (first == "expect")
        {
            std::string key;
            int64_t value = 0;
            while (words >> key >> value)
            {
                expected[key] = value;
            }
        }
        else if (first == "end")
        {
            words >> end;
        }
        else
        {
            std::string type;
            uint32_t lane = 0;
            words >> type >> lane;
            REQUIRE((type == "press" || type == "release"));
            actions.push_back(
                MakeAction(type == "press" ? ManiaAction::Press : ManiaAction::Release, lane, std::stoll(first)));
        }
    }

    hyoshi::Result<ManiaChart> chart = hyoshi::mania::ParseManiaChart(ReadFile(directory + "/" + chartFile));
    REQUIRE(chart);

    hyoshi::rhythm::ScoreSystem score;
    score.Reset(chart.Value().GetJudgmentCount());
    for (const JudgmentEvent& event : RunReplay(chart.Value(), actions, end))
    {
        score.Add(event);
    }

    CHECK(score.GetScore() == expected.at("score"));
    CHECK(score.GetMaxCombo() == expected.at("maxcombo"));
    CHECK(score.GetCount(Judgment::Perfect) == expected.at("perfect"));
    CHECK(score.GetCount(Judgment::Great) == expected.at("great"));
    CHECK(score.GetCount(Judgment::Good) == expected.at("good"));
    CHECK(score.GetCount(Judgment::Bad) == expected.at("bad"));
    CHECK(score.GetCount(Judgment::Miss) == expected.at("miss"));
}
