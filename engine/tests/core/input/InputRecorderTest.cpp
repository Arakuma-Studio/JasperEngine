// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The input recorder (docs/specs/input-recorder-and-input-scripts.md AC3-AC6, AC9, AC16, AC18):
// the sites at every Engine::system* input entry point, frame stamps, caps, checkpoints and the
// state hash. Frames are driven as Engine::systemDraw runs them: the scripts due, the latch, the
// frame counted, the checkpoints after the update.

#include "Engine.h"
#include "Input.h"
#include "LogCapture.h"
#include "Scene.h"
#include "TestSystem.h"
#include "component/Transform.h"
#include "input/InputRecorder.h"
#include "input/InputScriptRunner.h"
#include "input/StateHash.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace jasperengine {
    // the frame counter Engine::systemDraw advances after the latch
    struct EngineTestAccess {
        static void countGameFrame() { Engine::gameFrames++; }
    };
}

using namespace jasperengine;
using namespace jasperengine::input;

namespace {

    class InputRecorderTest : public ::testing::Test {
    protected:
        void SetUp() override{
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::resetFrameState();
        }

        void TearDown() override{
            InputRecorder::end();
            ScriptRunner::stopAll();
            Engine::systemTouchCancel();
            Engine::systemGamepadDisconnect(0);
            Input::resetFrameState();
        }

        // a running frame: the scripts due, the latch, the count, the checkpoints
        static void runFrame(){
            if (ScriptRunner::isActive()) ScriptRunner::beforeFrame(true);
            Input::beginFrame();
            EngineTestAccess::countGameFrame();
            if (ScriptRunner::isActive()) ScriptRunner::afterUpdate();
            if (InputRecorder::isActive()) InputRecorder::afterUpdate();
        }

        static RecordOptions options(int checkpointFrames = 0){
            RecordOptions o;
            o.header.fromPlayStart = true;
            o.header.platform = platformTag();
            o.checkpointFrames = checkpointFrames;
            return o;
        }

        static std::shared_ptr<ParsedScript> stop(){
            std::shared_ptr<RecordedInput> recorded = InputRecorder::end();
            return recorded ? recorded->toScript() : std::make_shared<ParsedScript>();
        }
    };

    std::string trace(const ParsedScript& script){
        std::string out;
        for (const ScriptAction& a : script.actions) {
            char line[200];
            std::snprintf(line, sizeof(line), "%u %s code=%d dev=%d mods=%d p=%.4f,%.4f v=%.3f,%.3f s=%s\n", a.frame,
                          actionTypeName(a.type), a.code, a.device, a.mods, a.point.x, a.point.y, a.value, a.value2,
                          script.stringAt(a.string).c_str());
            out += line;
        }
        return out;
    }

}

TEST_F(InputRecorderTest, RecordsEveryEntryPointOnce){
    Engine::systemFocus(true);
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    Engine::systemKeyDown(D_KEY_A, false, D_MODIFIER_SHIFT);
    Engine::systemKeyDown(D_KEY_A, true, D_MODIFIER_SHIFT);     // a repeat: dropped
    Engine::systemKeyUp(D_KEY_A, false, 0);
    runFrame();
    Engine::systemCharInput(U'h');
    Engine::systemTextInput("hi");
    Engine::systemImeComposition("ka", 1);
    Engine::systemImeCancel();
    Engine::systemMouseEnter();
    Engine::systemMouseMove(10, 10, 0);
    Engine::systemMouseMove(20, 10, 0);    // the same frame: only the last move stays
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 320, 180, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 320, 180, 0);
    Engine::systemMouseScroll(0, 2, 0);
    Engine::systemMouseLeave();
    runFrame();
    Engine::systemTouchStart(1, 160, 90);
    Engine::systemTouchMove(1, 170, 90);
    Engine::systemTouchEnd(1, 170, 90);
    Engine::systemTouchStart(2, 10, 10);
    Engine::systemTouchCancel(2);
    Engine::systemGamepadConnect(0, "Pad");
    Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
    Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.5f);
    Engine::systemGamepadDisconnect(0);
    Engine::systemLowMemory();
    Engine::systemFocus(false);
    Engine::systemFocus(true);
    std::shared_ptr<ParsedScript> script = stop();

    const std::string expected =
        "0 key down code=65 dev=0 mods=1 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "0 key up code=65 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "1 text per unit code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=h\n"
        "1 text code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=hi\n"
        "1 ime code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=ka\n"
        "1 ime cancel code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "1 mouse_enter code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "1 mouse move code=0 dev=0 mods=0 p=0.0313,0.0278 v=0.000,0.000 s=\n"
        "1 mouse down code=0 dev=0 mods=0 p=0.5000,0.5000 v=0.000,0.000 s=\n"
        "1 mouse up code=0 dev=0 mods=0 p=0.5000,0.5000 v=0.000,0.000 s=\n"
        "1 scroll code=0 dev=0 mods=0 p=0.0000,0.0000 v=2.000,0.000 s=\n"
        "1 mouse_leave code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 touch start code=0 dev=1 mods=0 p=0.2500,0.2500 v=0.000,0.000 s=\n"
        "2 touch move code=0 dev=1 mods=0 p=0.2656,0.2500 v=0.000,0.000 s=\n"
        "2 touch end code=0 dev=1 mods=0 p=0.2656,0.2500 v=0.000,0.000 s=\n"
        "2 touch start code=0 dev=2 mods=0 p=0.0156,0.0278 v=0.000,0.000 s=\n"
        "2 touch cancel code=0 dev=2 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 gamepad connect code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=Pad\n"
        "2 gamepad down code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 gamepad up code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 gamepad axis code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.500,0.000 s=\n"
        "2 gamepad disconnect code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 app low_memory code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 app focus_lost code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n"
        "2 app focus_gained code=0 dev=0 mods=0 p=0.0000,0.0000 v=0.000,0.000 s=\n";
    EXPECT_EQ(trace(*script), expected);

    // what the interpreter forces counts only through recordForced
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    ParsedScript forced;
    forced.strings = {"Player/Fire"};
    ScriptAction press;
    press.type = ActionType::InputActionPress;
    press.string = 0;
    press.count = 3;
    InputRecorder::recordForced(press, forced);
    std::shared_ptr<ParsedScript> forcedScript = stop();
    ASSERT_EQ(forcedScript->actions.size(), 1u);
    EXPECT_EQ(forcedScript->actions[0].type, ActionType::InputActionPress);
    EXPECT_EQ(forcedScript->actions[0].count, 3);
    EXPECT_EQ(forcedScript->stringAt(forcedScript->actions[0].string), "Player/Fire");
}

TEST_F(InputRecorderTest, ReplayHitsTheSameFrameEdges){
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    runFrame();
    // a press and its release in one window: the next frame sees both
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    runFrame();
    const bool recordedDown = Input::getKeyDown(D_KEY_SPACE), recordedUp = Input::getKeyUp(D_KEY_SPACE);
    // a press during a paused frame (no latch, no count) lands on the next running frame
    Engine::systemKeyDown(D_KEY_P, false, 0);
    runFrame();
    const bool recordedP = Input::getKeyDown(D_KEY_P);
    Engine::systemKeyUp(D_KEY_P, false, 0);
    runFrame();
    std::shared_ptr<ParsedScript> script = stop();
    ASSERT_TRUE(recordedDown && recordedUp && recordedP);

    Input::resetFrameState();
    auto player = std::make_shared<ScriptPlayer>(script, PlayerHost(), PlayOptions());
    ScriptRunner::start(player);
    runFrame();
    EXPECT_FALSE(Input::getKeyDown(D_KEY_SPACE));
    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_SPACE));
    ScriptRunner::beforeFrame(false);    // a paused app's frame: no latch, not counted
    runFrame();
    EXPECT_TRUE(Input::getKeyDown(D_KEY_P));
    runFrame();
    EXPECT_TRUE(Input::getKeyUp(D_KEY_P));
}

TEST_F(InputRecorderTest, ReplayLastsToTheStopFrame){
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    runFrame();
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    for (int i = 1; i < 10; i++)
        runFrame();
    std::shared_ptr<ParsedScript> recorded = stop();
    EXPECT_EQ(recorded->header.frames, 10u);
    std::string text;
    ASSERT_TRUE(writeScript(*recorded, text, error)) << error;
    auto script = std::make_shared<ParsedScript>();
    ASSERT_TRUE(parseScript(text, *script, error)) << error;
    EXPECT_EQ(script->header.frames, 10u);
    EXPECT_EQ(script->lastFrame(), 10u);
    // a decoded Lua table (InputRecording.play(t)) gives its integers signed
    ScriptJson root = ScriptJson::parse(text);
    root["frames"] = int64_t(10);
    for (ScriptJson& event : root["events"])
        event["frame"] = event["frame"].get<int64_t>();
    ParsedScript fromTable;
    ASSERT_TRUE(parseScriptJson(root, fromTable, error)) << error;
    EXPECT_EQ(fromTable.header.frames, 10u);
    EXPECT_EQ(trace(fromTable), trace(*script));
    root["frames"] = int64_t(-1);
    EXPECT_FALSE(parseScriptJson(root, fromTable, error));

    // its press is on frame 1, yet it runs the ten frames recorded, as long as the recording ran
    Input::resetFrameState();
    auto player = std::make_shared<ScriptPlayer>(script, PlayerHost(), PlayOptions());
    ScriptRunner::start(player);
    for (int i = 0; i < 10; i++)
        runFrame();
    EXPECT_FALSE(player->finished());
    runFrame();
    EXPECT_TRUE(player->finished());
    EXPECT_TRUE(player->report().done);
}

TEST_F(InputRecorderTest, RecordsAnOrientationTurnOnly){
    Engine::systemOrientationChanged();
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    // asked every frame (the editor's Game view does) without a turn: nothing to replay
    for (int i = 0; i < 3; i++) {
        Engine::systemOrientationChanged();
        runFrame();
    }
    test::TestSystem::get().setScreenSize(360, 640);
    Engine::systemOrientationChanged();
    runFrame();
    std::shared_ptr<ParsedScript> script = stop();
    test::TestSystem::get().setScreenSize(640, 360);
    Engine::systemOrientationChanged();
    std::vector<std::string> turns;
    for (const ScriptAction& a : script->actions)
        if (a.type == ActionType::AppOrientation) turns.push_back(std::to_string(a.frame) + " " + script->stringAt(a.string));
    EXPECT_EQ(turns, std::vector<std::string>{"3 portrait"});
}

TEST_F(InputRecorderTest, IdleCostsNothing){
    ASSERT_FALSE(InputRecorder::isActive());
    // the entry points' own first-call setup, before counting
    Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, 0.0f);
    Engine::systemKeyUp(D_KEY_Q, false, 0);
    testsupport::AllocationCountScope allocations;
    for (int i = 0; i < 100000; i++) {
        Engine::systemGamepadAxisMove(0, D_GAMEPAD_AXIS_LEFT_X, (i & 1) ? 0.25f : 0.5f);
        Engine::systemKeyUp(D_KEY_Q, false, 0);
    }
    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations.count(), 0u);
    }
}

TEST(InputRecorderBench, IdleSite){
    // the site alone: one relaxed load and a branch
    const int calls = 10000000;
    volatile int sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; i++) {
        if (InputRecorder::isActive() && !InputRecorder::record(RecordedEvent::of(RecordKind::KeyUp, i))) sink = sink + 1;
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
    std::printf("[bench] InputRecorder idle site: %.3f ns a call (budget 2 ns)\n", ns);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ns, 2.0 * testsupport::perfSlack());
    }
}

TEST_F(InputRecorderTest, StopsAtTheBufferCap){
    RecordOptions o = options();
    o.maxBytes = 64 * 1024;
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(o, error)) << error;
    test::LogCapture log;
    for (int frame = 0; frame < 100 && InputRecorder::isRecording(); frame++) {
        for (int i = 0; i < 50; i++) Engine::systemKeyUp(D_KEY_Z, false, 0);
        runFrame();
    }
    EXPECT_FALSE(InputRecorder::isRecording());
    EXPECT_TRUE(InputRecorder::truncated());
    EXPECT_EQ(log.count(D_LOG_WARN, "Input recording stopped at frame 40"), 1u);
    std::shared_ptr<RecordedInput> recorded = InputRecorder::end();
    ASSERT_TRUE(recorded);
    EXPECT_TRUE(recorded->header.truncated);
    EXPECT_EQ(recorded->header.frames, 40u);   // its replay ends where the cap stopped it
    EXPECT_EQ(recorded->events(), 64u * 1024u / sizeof(RecordedEvent));
}

TEST_F(InputRecorderTest, CheckpointsCompareOnReplay){
    Scene scene;
    const Entity ball = scene.createEntity();
    scene.setEntityName(ball, "Ball");
    Transform t;
    scene.addComponent<Transform>(ball, t);
    Engine::setScene(&scene);
    RecordOptions o = options(2);
    o.probes = {"Ball"};
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(o, error)) << error;
    for (int i = 0; i < 4; i++) {
        scene.findComponent<Transform>(ball)->worldPosition.x = float(i);
        runFrame();
    }
    std::shared_ptr<ParsedScript> script = stop();
    ASSERT_EQ(script->header.checkpoints.size(), 2u);
    EXPECT_EQ(script->header.checkpoints[0].frame, 2u);
    EXPECT_EQ(script->header.probes, std::vector<std::string>{"Ball"});

    // the same moves compare equal; a different one diverges at its checkpoint, naming the probe
    for (const float moved : {0.0f, 5.0f}) {
        auto player = std::make_shared<ScriptPlayer>(script, PlayerHost(), PlayOptions());
        ScriptRunner::start(player);
        for (int i = 0; i < 4; i++) {
            scene.findComponent<Transform>(ball)->worldPosition.x = float(i) + (i >= 2 ? moved : 0.0f);
            runFrame();
        }
        const ReplayReport& report = player->report();
        EXPECT_TRUE(report.comparable);
        if (moved == 0.0f) {
            EXPECT_EQ(report.compared, 2);
            EXPECT_EQ(report.divergedAtFrame, -1);
        } else {
            EXPECT_EQ(report.divergedAtFrame, 4);
            ASSERT_EQ(report.differences.size(), 1u);
            EXPECT_EQ(report.differences[0]["probe"], "Ball");
            EXPECT_NEAR(report.differences[0]["distance"].get<double>(), 5.0, 1e-3);
        }
        ScriptRunner::stopAll();
    }
    Engine::removeAllScenes();
}

TEST(InputReplay, OtherPlatformIsNotComparable){
    ParsedScript script;
    std::string error;
    ASSERT_TRUE(parseScript(R"({"format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play",
        "platform": "android-arm64", "events": [{"frame": 0, "type": "wait", "frames": 1}],
        "checkpoints": [{"frame": 1, "hash": "0123456789abcdef"}]})", script, error)) << error;
    ScriptPlayer player(std::make_shared<ParsedScript>(script), PlayerHost(), PlayOptions());
    player.beforeFrame(0);
    player.afterUpdate(0);
    EXPECT_FALSE(player.report().comparable);
    EXPECT_EQ(player.report().compared, 0);
    EXPECT_EQ(player.report().divergedAtFrame, -1);
}

TEST(StateHash, QuantizesBelowAThousandth){
    auto hashWith = [](float x) {
        Scene scene;
        const Entity e = scene.createEntity();
        Transform t;
        t.worldPosition = Vector3(x, 2.0f, 3.0f);
        scene.addComponent<Transform>(e, t);
        return hashState({&scene}, ScriptJson::object());
    };
    EXPECT_EQ(hashWith(1.0f), hashWith(1.0001f));
    EXPECT_NE(hashWith(1.0f), hashWith(1.002f));
    EXPECT_NE(hashState({}, ScriptJson{{"score", 10}}), hashState({}, ScriptJson{{"score", 11}}));
}

TEST(StateHashBench, Hash2000Transforms){
    Scene scene;
    for (int i = 0; i < 2000; i++) {
        const Entity e = scene.createEntity();
        Transform t;
        t.worldPosition = Vector3(float(i), 0.5f, -float(i));
        scene.addComponent<Transform>(e, t);
    }
    const int runs = 50;
    uint64_t hash = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; i++) hash ^= hashState({&scene}, ScriptJson::object());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
    std::printf("[bench] StateHash over 2000 transforms: %.4f ms (budget 0.15 ms) %llx\n", ms, (unsigned long long)hash);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ms, 0.15 * testsupport::perfSlack());
    }
}

// send_input's live events (the editor's pacing: game frames counted between polls) and the same
// events as a script file run by the engine's frame loop make the same Engine::system* calls on
// the same frames (R9). The recorder at the entry points is the trace.
TEST_F(InputRecorderTest, SendInputParityEventsGiveTheSameSystemCalls){
    const std::string events = R"([
        {"type": "key", "key": "a", "mods": ["shift"]},
        {"type": "key", "key": "b", "action": "down"}, {"type": "key", "key": "b", "action": "up"},
        {"type": "text", "text": "hé"}, {"type": "text", "text": "x", "per_unit": true},
        {"type": "ime", "text": "ka"}, {"type": "ime_commit", "text": "か"}, {"type": "ime_cancel"},
        {"type": "mouse_enter"},
        {"type": "mouse", "action": "click", "x": 100, "y": 50},
        {"type": "mouse", "action": "double_click", "x": 0.5, "y": 0.5, "space": "normalized"},
        {"type": "mouse", "action": "drag", "x": 10, "y": 10, "to": [60, 40], "steps": 3},
        {"type": "mouse", "action": "move", "x": 30, "y": 30}, {"type": "mouse", "action": "move", "dx": 5, "dy": 0},
        {"type": "mouse", "action": "scroll", "dy": -1},
        {"type": "mouse_leave"},
        {"type": "touch", "action": "tap", "x": 200, "y": 100, "hold_frames": 3},
        {"type": "touch", "action": "swipe", "pointer": 1, "x": 0.1, "y": 0.1, "to": [0.3, 0.2], "steps": 2, "space": "safe_area"},
        {"type": "touch", "action": "start", "pointer": 2, "x": 5, "y": 5}, {"type": "touch", "action": "cancel", "pointer": 2},
        {"type": "gamepad", "button": "a"}, {"type": "gamepad", "axis": "left_y", "value": -1},
        {"type": "gamepad", "action": "disconnect"},
        {"type": "wait", "frames": 3},
        {"type": "app", "action": "low_memory"}, {"type": "app", "action": "focus_lost"}, {"type": "app", "action": "focus_gained"},
        {"type": "release_all"}
    ])";
    auto runLive = [&]() {
        auto script = std::make_shared<ParsedScript>();
        EventParseOptions live;
        live.live = true;
        std::string error;
        size_t index = 0;
        EXPECT_TRUE(parseEventList(ScriptJson::parse(events), live, *script, error, index)) << error;
        PlayOptions play;
        play.releaseAtEnd = false;
        ScriptPlayer player(script, PlayerHost(), play);
        // the editor polls between frames: the steps due, then the game frame
        uint64_t frame = 0;
        while (player.beforeFrame(frame) && frame < 500) {
            runFrame();
            frame++;
        }
    };
    auto runFile = [&]() {
        auto script = std::make_shared<ParsedScript>();
        std::string error;
        EXPECT_TRUE(parseScript(R"({"format": "jasperengine-input", "version": 1, "kind": "script", "events": )" + events + "}",
                                *script, error)) << error;
        PlayOptions play;
        play.releaseAtEnd = false;
        auto player = std::make_shared<ScriptPlayer>(script, PlayerHost(), play);
        ScriptRunner::start(player);
        for (int frame = 0; frame < 500 && ScriptRunner::isActive(); frame++) runFrame();
    };
    std::string error;
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    runLive();
    const std::string live = trace(*stop());
    Input::resetFrameState();
    ASSERT_TRUE(InputRecorder::begin(options(), error)) << error;
    runFile();
    const std::string file = trace(*stop());
    EXPECT_GT(live.size(), 1000u);
    EXPECT_EQ(live, file);
}
