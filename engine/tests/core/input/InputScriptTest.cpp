// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Input scripts and recordings (docs/specs/input-recorder-and-input-scripts.md AC1, AC2): the
// .input.json format v1 and its interpreter, which applies the actions due before a running
// frame's input latch (Input::beginFrame), as Engine::systemDraw does through ScriptRunner.

#include "Engine.h"
#include "Input.h"
#include "Screen.h"
#include "LogCapture.h"
#include "TestSystem.h"
#include "input/InputScript.h"
#include "input/InputScriptRunner.h"

#include <gtest/gtest.h>

#include <random>

namespace InputScriptTest_cpp {

using namespace jasperengine;
using namespace jasperengine::input;

namespace {

    const char* callbackTag = "core-tests-input-script";

    std::shared_ptr<ParsedScript> parsed(const std::string& json){
        auto script = std::make_shared<ParsedScript>();
        std::string error;
        EXPECT_TRUE(parseScript(json, *script, error)) << error;
        return script;
    }

    std::string scriptFile(const std::string& events, const std::string& header = std::string()){
        return R"({"format": "jasperengine-input", "version": 1, "kind": "script")" + header + R"(, "events": )" + events + "}";
    }

    std::string recordingFile(const std::string& events, const std::string& header = std::string()){
        return R"({"format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play")" + header +
               R"(, "events": )" + events + "}";
    }

    // what the parser must refuse: the error names the fix, and nothing is left to run
    void expectRejected(const std::string& json, const std::string& text){
        ParsedScript script;
        std::string error;
        EXPECT_FALSE(parseScript(json, script, error)) << json.substr(0, 200);
        EXPECT_NE(error.find(text), std::string::npos) << "error: " << error << "\nexpected: " << text;
        EXPECT_TRUE(script.actions.empty());
    }

    class InputScriptPlayerTest : public ::testing::Test {
    protected:
        void SetUp() override{
            // screen and canvas 640 x 360: the system* coordinates are canvas pixels
            test::TestSystem::get().setScreenSize(640, 360);
            Engine::setCanvasSize(640, 360);
            Engine::setScalingMode(Scaling::FITWIDTH);
            Engine::systemViewChanged();
            Input::resetFrameState();
        }

        void TearDown() override{
            ScriptRunner::stopAll();
            Engine::systemTouchCancel();
            Engine::onKeyDown.remove(callbackTag);
            Engine::onBack.remove(callbackTag);
            Screen::clearExtraInsets();
            Input::resetFrameState();
        }

        // a running frame of Engine::systemDraw: the script's actions due, then the latch
        static void runFrame(ScriptPlayer& player, uint64_t frame){
            player.beforeFrame(frame);
            Input::beginFrame();
        }
    };

}

TEST(InputScriptFormat, RoundTripsEveryEventType){
    const std::string events = R"([
        {"frame": 0, "type": "key", "key": "right", "action": "down", "mods": ["shift", "ctrl"]},
        {"frame": 0, "type": "key", "key": "right", "action": "up", "mods": ["alt"]},
        {"frame": 1, "type": "text", "text": "hé"},
        {"frame": 1, "type": "text", "text": "a😀", "per_unit": true},
        {"frame": 2, "type": "ime", "text": "かな", "cursor": 1},
        {"frame": 2, "type": "ime_commit", "text": "仮名"},
        {"frame": 2, "type": "ime_cancel"},
        {"frame": 3, "type": "mouse_enter"},
        {"frame": 3, "type": "mouse", "action": "move", "x": 0.25, "y": 0.5},
        {"frame": 3, "type": "mouse", "action": "down", "button": "right", "x": 0.25, "y": 0.5},
        {"frame": 4, "type": "mouse", "action": "up", "button": "right", "x": 0.3, "y": 0.5},
        {"frame": 4, "type": "mouse", "action": "scroll", "dx": 0.5, "dy": -2, "x": 0.1, "y": 0.2},
        {"frame": 4, "type": "mouse", "action": "down", "target": "#play", "document": {"id": 12}},
        {"frame": 4, "type": "mouse_leave"},
        {"frame": 5, "type": "touch", "action": "start", "pointer": 1, "x": 0.5, "y": 0.8125},
        {"frame": 5, "type": "touch", "action": "move", "pointer": 1, "x": 120, "y": 200, "space": "canvas"},
        {"frame": 6, "type": "touch", "action": "end", "pointer": 1, "x": 0.9, "y": 0.1, "space": "safe_area"},
        {"frame": 6, "type": "touch", "action": "cancel", "pointer": 2},
        {"frame": 7, "type": "gamepad", "action": "connect", "gamepad": 1, "name": "Xbox Controller"},
        {"frame": 7, "type": "gamepad", "gamepad": 1, "button": "a", "action": "down"},
        {"frame": 8, "type": "gamepad", "gamepad": 1, "button": "dpad_up", "action": "up"},
        {"frame": 8, "type": "gamepad", "action": "axis", "gamepad": 1, "axis": "left_x", "value": -0.5},
        {"frame": 8, "type": "gamepad", "action": "disconnect", "gamepad": 1},
        {"frame": 9, "type": "action", "name": "Jump", "phase": "down"},
        {"frame": 9, "type": "action", "name": "Jump", "phase": "up"},
        {"frame": 9, "type": "axis", "name": "Horizontal", "value": 0.75},
        {"frame": 9, "type": "axis_delta", "name": "Mouse X", "delta": 3.5},
        {"frame": 10, "type": "input_action", "name": "Player/Fire", "action": "press", "hold_frames": 3},
        {"frame": 10, "type": "input_action", "name": "Player/Move", "action": "down", "value": [0.5, -1]},
        {"frame": 11, "type": "input_action", "name": "Player/Move", "action": "up"},
        {"frame": 11, "type": "app", "action": "pause"},
        {"frame": 11, "type": "app", "action": "resume"},
        {"frame": 12, "type": "app", "action": "focus_lost"},
        {"frame": 12, "type": "app", "action": "focus_gained"},
        {"frame": 12, "type": "app", "action": "low_memory"},
        {"frame": 12, "type": "app", "action": "hide"},
        {"frame": 12, "type": "app", "action": "show"},
        {"frame": 13, "type": "app", "action": "orientation", "orientation": "landscape_left"},
        {"frame": 13, "type": "shot", "name": "01_menu"},
        {"frame": 13, "type": "wait", "until": "Player ~= nil", "frames": 90},
        {"frame": 14, "type": "release_all"}
    ])";
    const std::string header = R"(, "fixed_fps": 30, "seed": 3141592653, "scene": "Scenes/Level1.scene", "canvas": [1280, 720],
        "simulate_touch": true, "platform": "linux-x64", "probes": ["Player"], "my_tool": {"kept": true},
        "checkpoints": [{"frame": 60, "hash": "6c1f0e9a2b7d4c13", "probes": [[1.0029, 0, 3.5]], "values": {"score": 10}}])";
    std::shared_ptr<ParsedScript> first = parsed(recordingFile(events, header));
    ASSERT_EQ(first->events, 41u);

    std::string written, error;
    ASSERT_TRUE(writeScript(*first, written, error)) << error;
    ParsedScript second;
    ASSERT_TRUE(parseScript(written, second, error)) << error << "\n" << written;

    ASSERT_EQ(second.actions.size(), first->actions.size()) << written;
    for (size_t i = 0; i < first->actions.size(); i++) {
        EXPECT_TRUE(second.actions[i] == first->actions[i]) << "action " << i << " (" << actionTypeName(first->actions[i].type) << ")\n" << written;
    }
    EXPECT_EQ(second.strings, first->strings);
    ASSERT_EQ(second.targets.size(), 1u);
    EXPECT_EQ(second.targets[0].selector, "#play");
    EXPECT_EQ(second.targets[0].document, first->targets[0].document);

    const ScriptHeader& h = second.header;
    EXPECT_EQ(h.kind, ScriptKind::Recording);
    EXPECT_TRUE(h.fromPlayStart);
    EXPECT_EQ(h.fixedFps, 30);
    EXPECT_EQ(h.space, Space::Normalized);
    EXPECT_TRUE(h.hasSeed);
    EXPECT_EQ(h.seed, 3141592653u);
    EXPECT_EQ(h.scene, "Scenes/Level1.scene");
    EXPECT_EQ(h.canvas.x, 1280.0f);
    EXPECT_TRUE(h.simulateTouch);
    EXPECT_EQ(h.platform, "linux-x64");
    EXPECT_EQ(h.probes, std::vector<std::string>{"Player"});
    EXPECT_EQ(h.extra["my_tool"]["kept"], true);
    ASSERT_EQ(h.checkpoints.size(), 1u);
    EXPECT_TRUE(h.checkpoints[0] == first->header.checkpoints[0]);
    EXPECT_EQ(h.checkpoints[0].hash, 0x6c1f0e9a2b7d4c13ull);

    // every space a file stores
    for (const char* space : {"canvas", "normalized", "safe_area"}) {
        std::shared_ptr<ParsedScript> s = parsed(recordingFile(R"([{"frame": 0, "type": "touch", "action": "start", "x": 0.5, "y": 0.25}])",
                                                               std::string(R"(, "space": ")") + space + "\""));
        ASSERT_EQ(s->actions.size(), 1u);
        EXPECT_STREQ(spaceName(s->actions[0].point.space), space);
        ASSERT_TRUE(writeScript(*s, written, error)) << error;
        ASSERT_TRUE(parseScript(written, second, error)) << error;
        EXPECT_TRUE(second.actions[0] == s->actions[0]) << space;
    }

    // one event per line inside events, so a hand edit diffs as one line
    EXPECT_NE(written.find("\n    {\"frame\":0,\"type\":\"touch\""), std::string::npos) << written;

    // a file's ms are frames at its fixed_fps (ceil): 100 ms is 3 frames at 30 fps, 6 at 60
    for (int fps : {30, 60}) {
        std::shared_ptr<ParsedScript> s = parsed(scriptFile(
            R"([{"type": "wait", "ms": 100}, {"type": "key", "key": "a", "hold_frames": 0, "hold_ms": 50}])",
            ", \"fixed_fps\": " + std::to_string(fps)));
        ASSERT_EQ(s->actions.size(), 3u);
        EXPECT_EQ(s->actions[0].framesAfter, fps == 30 ? 3u : 6u);
        EXPECT_EQ(s->actions[1].framesAfter, fps == 30 ? 2u : 3u);
        EXPECT_EQ(s->actions[0].msAfter, 0u);
    }
}

TEST(InputScriptFormat, RejectsBadFilesNamingTheFix){
    expectRejected(R"({"format": "jasperengine-input", "version": 2, "kind": "script", "events": []})", "made by a newer engine");
    expectRejected(R"({"format": "jasperengine-input", "version": 1, "events": []})", "kind is missing");
    expectRejected(recordingFile(R"([{"frame": 4, "type": "key", "key": "a", "action": "down"},
                                     {"frame": 9, "type": "key", "key": "a", "action": "up"},
                                     {"frame": 7, "type": "key", "key": "b", "action": "down"}])"),
                   "events[2]: frame 7 goes back in time");
    expectRejected(recordingFile(R"([{"type": "key", "key": "a", "action": "down"}])"), "events[0]: a recording's event needs frame");
    expectRejected(scriptFile(R"([{"type": "wait", "frames": 2}, {"frame": 3, "type": "key", "key": "a"}])"),
                   "events[1]: a script's events carry no frame");
    expectRejected(scriptFile(R"([{"type": "teleport"}])"), "events[0]: type must be key, text");
    expectRejected(scriptFile(R"([{"type": "key", "key": "banana"}])"), "events[0]: unknown key \"banana\"");
    expectRejected(scriptFile(R"([{"type": "touch", "x": 4, "y": 5, "space": "view"}])"),
                   "space \"view\" is the editor's window and cannot be stored: use normalized, canvas or safe_area");
    expectRejected(scriptFile("[]", R"(, "space": "view")"), "cannot be stored");
    expectRejected(scriptFile(R"([{"type": "wait"}])"), "wait needs frames, ms or until");
    expectRejected(R"({"format": "jasperengine-input", "version": 1, "kind": "script", "events": [{"type": "key",)", "not valid JSON");
    expectRejected(R"({"format": "something-else", "version": 1, "kind": "script", "events": []})", "format must be");

    // the caps: 64 MB, 2,000,000 events
    std::string huge = scriptFile("[]");
    huge.append(SCRIPT_MAX_BYTES, ' ');
    expectRejected(huge, "larger than 64 MB");
    std::string many = R"({"format": "jasperengine-input", "version": 1, "kind": "script", "events": [)";
    many.reserve(many.size() + (SCRIPT_MAX_EVENTS + 1) * 2 + 8);
    for (size_t i = 0; i <= SCRIPT_MAX_EVENTS; i++)
        many += i ? ",0" : "0";
    many += "]}";
    expectRejected(many, "more than 2000000 events");
}

TEST(InputScriptFormat, RandomBytesNeverCrash){
    const std::string sample = recordingFile(R"([
        {"frame": 0, "type": "key", "key": "right", "action": "down", "mods": ["shift"]},
        {"frame": 3, "type": "touch", "action": "start", "pointer": 0, "x": 0.5, "y": 0.8125},
        {"frame": 4, "type": "gamepad", "action": "axis", "gamepad": 0, "axis": "left_x", "value": 0.5},
        {"frame": 5, "type": "wait", "until": "x", "frames": 3},
        {"frame": 6, "type": "input_action", "name": "P/A", "value": [1, 2]}
    ])", R"(, "seed": 7, "canvas": [10, 20], "probes": ["A"], "checkpoints": [{"frame": 1, "hash": "00ff", "probes": [[1, 2, 3]]}])");
    std::mt19937 random(1234);
    const char alphabet[] = "{}[]\",:0123456789-.eE tfnulrasx\\\xff";
    size_t parsedCount = 0;
    for (int i = 0; i < 10000; i++) {
        std::string text = sample;
        const int edits = 1 + static_cast<int>(random() % 8);
        for (int e = 0; e < edits && !text.empty(); e++) {
            const size_t at = random() % text.size();
            switch (random() % 4) {
                case 0: text[at] = alphabet[random() % (sizeof(alphabet) - 1)]; break;
                case 1: text.insert(at, 1, alphabet[random() % (sizeof(alphabet) - 1)]); break;
                case 2: text.erase(at, 1 + random() % 8); break;
                default: text.resize(at); break;
            }
        }
        ParsedScript script;
        std::string error;
        if (parseScript(text, script, error)) {
            parsedCount++;
            std::string written;
            writeScript(script, written, error);
        } else {
            EXPECT_FALSE(error.empty());
            EXPECT_TRUE(script.actions.empty());
        }
    }
    // the mutations reach past the JSON parser: some files still parse
    EXPECT_GT(parsedCount, 0u);
}

TEST_F(InputScriptPlayerTest, ScriptStepsFollowEachOther){
    // a key press: down for hold_frames (2) frames, up, then gap_frames (1)
    auto script = parsed(scriptFile(R"([{"type": "key", "key": "space"}, {"type": "key", "key": "x", "action": "down"}])"));
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    runFrame(player, 0);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_SPACE));
    runFrame(player, 1);
    EXPECT_TRUE(Input::getKey(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKeyUp(D_KEY_SPACE));
    runFrame(player, 2);
    EXPECT_TRUE(Input::getKeyUp(D_KEY_SPACE));
    EXPECT_FALSE(Input::getKey(D_KEY_X));
    runFrame(player, 3);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_X));
    // the last event's gap, then done: what it held is released at its end
    EXPECT_FALSE(player.finished());
    runFrame(player, 4);
    EXPECT_TRUE(player.report().done);
    EXPECT_TRUE(Input::getKeyUp(D_KEY_X));
    EXPECT_EQ(player.report().actionsDone, 3u);
}

TEST_F(InputScriptPlayerTest, RecordingAppliesEachEventOnItsFrame){
    // a press and its release recorded in one window are both seen by that frame; the next event
    // lands on its stamp, whatever frames came between
    auto script = parsed(recordingFile(R"([
        {"frame": 0, "type": "key", "key": "a", "action": "down"},
        {"frame": 0, "type": "key", "key": "a", "action": "up"},
        {"frame": 3, "type": "touch", "action": "start", "pointer": 0, "x": 0.5, "y": 0.5},
        {"frame": 5, "type": "touch", "action": "end", "pointer": 0, "x": 0.25, "y": 0.5}
    ])"));
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    runFrame(player, 0);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_A));
    EXPECT_TRUE(Input::getKeyUp(D_KEY_A));
    for (uint64_t frame = 1; frame < 3; frame++) {
        runFrame(player, frame);
        EXPECT_EQ(Input::getTouchCount(), 0u) << frame;
    }
    runFrame(player, 3);
    EXPECT_EQ(Input::getTouchCount(), 1u);
    runFrame(player, 4);
    EXPECT_FALSE(player.finished());
    runFrame(player, 5);
    EXPECT_TRUE(player.report().done);
    EXPECT_EQ(player.report().actionsDone, 4u);
}

TEST_F(InputScriptPlayerTest, SpacesLandOnTheCanvas){
    // a 40-unit band at the top (a notch): safe_area 0..1 is the area below it
    Screen::setExtraInsets(EdgeInsets(0, 40, 0, 0));
    auto script = parsed(scriptFile(R"([
        {"type": "mouse", "action": "move", "x": 0.5, "y": 0.0, "space": "safe_area"},
        {"type": "mouse", "action": "move", "x": 0.25, "y": 0.5, "space": "normalized"},
        {"type": "mouse", "action": "move", "x": 64, "y": 36}
    ])", R"(, "gap_frames": 1)"));
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    const jasperengine::Rect safe = Screen::getSafeArea();
    runFrame(player, 0);
    EXPECT_NEAR(Input::getMousePosition().x, safe.getX() + 0.5f * safe.getWidth(), 1e-2f);
    EXPECT_NEAR(Input::getMousePosition().y, 40.0f, 1e-2f);
    runFrame(player, 1);
    EXPECT_NEAR(Input::getMousePosition().x, 160.0f, 1e-2f);
    EXPECT_NEAR(Input::getMousePosition().y, 180.0f, 1e-2f);
    runFrame(player, 2);
    EXPECT_NEAR(Input::getMousePosition().x, 64.0f, 1e-2f);
    EXPECT_NEAR(Input::getMousePosition().y, 36.0f, 1e-2f);
}

TEST_F(InputScriptPlayerTest, WaitUntilChecksOncePerFrameAndTimesOut){
    int checks = 0;
    PlayerHost host;
    host.evalCondition = [&](const std::string& condition, bool& truthy, std::string&) {
        EXPECT_EQ(condition, "Menu.ready");
        truthy = ++checks >= 3;
        return true;
    };
    std::vector<std::string> shots;
    host.shot = [&](const std::string& name, uint64_t, std::string&) {
        shots.push_back(name);
        return true;
    };
    auto script = parsed(scriptFile(R"([{"type": "wait", "until": "Menu.ready", "frames": 10}, {"type": "shot", "name": "01_menu"}])"));
    ScriptPlayer player(script, host, PlayOptions());
    runFrame(player, 0);
    runFrame(player, 0);   // the same frame again: no new check
    runFrame(player, 1);
    EXPECT_TRUE(shots.empty());
    runFrame(player, 2);
    EXPECT_EQ(checks, 3);
    EXPECT_EQ(shots, std::vector<std::string>{"01_menu"});
    ASSERT_EQ(player.report().shots.size(), 1u);
    EXPECT_EQ(player.report().shots[0].name, "01_menu");

    // past its cap the run fails naming the expression
    host.evalCondition = [](const std::string&, bool& truthy, std::string&) {
        truthy = false;
        return true;
    };
    auto never = parsed(scriptFile(R"([{"type": "key", "key": "a", "action": "down"}, {"type": "wait", "until": "Player ~= nil", "frames": 5}])"));
    ScriptPlayer failing(never, host, PlayOptions());
    uint64_t frame = 0;
    while (failing.beforeFrame(frame) && frame < 20) {
        Input::beginFrame();
        frame++;
    }
    EXPECT_TRUE(failing.report().failed);
    EXPECT_EQ(failing.errorText(), "Event 1 (wait until): until timed out after 5 frames: Player ~= nil");
    // what it held is released on the error
    EXPECT_TRUE(failing.held().empty());
    Input::beginFrame();
    EXPECT_FALSE(Input::getKey(D_KEY_A));
}

TEST_F(InputScriptPlayerTest, TargetsNeedAResolver){
    auto script = parsed(scriptFile(R"([{"type": "touch", "action": "tap", "target": "#play"}])"));
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    EXPECT_FALSE(player.beforeFrame(0));
    EXPECT_EQ(player.errorText(), "Event 0 (touch tap): target needs the htmlui module in this player");

    PlayerHost host;
    host.resolveTarget = [](const std::string& selector, const ScriptJson&, Vector2& point, std::string&, ScriptJson* found) {
        EXPECT_EQ(selector, "#play");
        point = Vector2(100, 50);
        if (found) *found = {{"html", "menu.html"}};
        return true;
    };
    ScriptPlayer resolved(script, host, PlayOptions());
    runFrame(resolved, 0);
    EXPECT_EQ(Input::getTouchCount(), 1u);
    EXPECT_NEAR(Input::getTouchPosition(0).x, 100.0f, 1e-2f);
    ASSERT_EQ(resolved.report().targets.size(), 1u);
    EXPECT_EQ(resolved.report().targets[0].document["html"], "menu.html");
}

TEST_F(InputScriptPlayerTest, AppBackReportsWhoTookIt){
    // nobody takes it: the Back button's own path presses Escape
    auto script = parsed(scriptFile(R"([{"type": "app", "action": "back"}])"));
    ScriptPlayer escape(script, PlayerHost(), PlayOptions());
    runFrame(escape, 0);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_ESCAPE));
    ASSERT_EQ(escape.report().backs.size(), 1u);
    EXPECT_EQ(escape.report().backs[0].route, "escape");

    Input::resetFrameState();
    int heard = 0;
    Engine::onBack.add(callbackTag, std::function<void(const std::string&)>([&](const std::string&) { heard++; }));
    ScriptPlayer game(script, PlayerHost(), PlayOptions());
    runFrame(game, 0);
    EXPECT_EQ(heard, 1);
    ASSERT_EQ(game.report().backs.size(), 1u);
    EXPECT_EQ(game.report().backs[0].route, "game");
    EXPECT_FALSE(Input::getKeyDown(D_KEY_ESCAPE));
}

TEST_F(InputScriptPlayerTest, GamepadWarpNeedsTheGamepadCursor){
    auto script = parsed(scriptFile(R"([{"type": "gamepad", "button": "a", "x": 0.5, "y": 0.5, "space": "normalized"}])"));
    ASSERT_EQ(script->actions.size(), 3u);
    EXPECT_EQ(script->actions[0].type, ActionType::GamepadCursor);
    EXPECT_EQ(script->actions[0].framesAfter, 0u);   // the warp and the press share the frame
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    EXPECT_FALSE(player.beforeFrame(0));
    EXPECT_EQ(player.errorText(), "Event 0 (gamepad press): no document has Gamepad Cursor on: set_properties UIDocument gamepadCursor true");
}

TEST_F(InputScriptPlayerTest, RecordedOrientationTurnsAreSkippedOutsideTheEditor){
    // the device turned while recording; here nothing simulates one: the turns are skipped, warned
    // once, and the rest of the recording still runs
    auto recording = parsed(recordingFile(R"([
        {"frame": 0, "type": "app", "action": "orientation", "orientation": "portrait"},
        {"frame": 1, "type": "app", "action": "orientation", "orientation": "landscape_left"},
        {"frame": 1, "type": "key", "key": "a", "action": "down"}
    ])"));
    test::LogCapture log;
    ScriptPlayer player(recording, PlayerHost(), PlayOptions());
    runFrame(player, 0);
    runFrame(player, 1);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_A));
    EXPECT_TRUE(player.report().done);
    EXPECT_FALSE(player.report().failed);
    EXPECT_EQ(log.count(D_LOG_WARN, "recorded orientation turns are skipped"), 1u);
    Engine::systemKeyUp(D_KEY_A, false, 0);

    // a script asks for the turn: that cannot be done here, and says where it can
    auto script = parsed(scriptFile(R"([{"type": "app", "action": "orientation", "orientation": "portrait"}])"));
    ScriptPlayer asked(script, PlayerHost(), PlayOptions());
    EXPECT_FALSE(asked.beforeFrame(0));
    EXPECT_NE(asked.errorText().find("runs in the editor's Game view"), std::string::npos) << asked.errorText();
}

TEST_F(InputScriptPlayerTest, InjectingIsKnownToTheEntryPoints){
    bool injected = false;
    Engine::onKeyDown.add(callbackTag, std::function<void(int, bool, int)>([&](int, bool, int) { injected = ScriptPlayer::isInjecting(); }));
    auto script = parsed(scriptFile(R"([{"type": "key", "key": "z", "action": "down"}])"));
    ScriptPlayer player(script, PlayerHost(), PlayOptions());
    runFrame(player, 0);
    EXPECT_TRUE(injected);
    Engine::systemKeyDown(D_KEY_Q, false, 0);
    EXPECT_FALSE(injected);
    Engine::systemKeyUp(D_KEY_Q, false, 0);
}

TEST_F(InputScriptPlayerTest, RunnerPacesPlayersByRunningFrames){
    auto script = parsed(recordingFile(R"([
        {"frame": 1, "type": "key", "key": "b", "action": "down"},
        {"frame": 2, "type": "key", "key": "b", "action": "up"}
    ])"));
    auto player = std::make_shared<ScriptPlayer>(script, PlayerHost(), PlayOptions());
    int ended = 0;
    ScriptRunner::start(player, [&](ScriptPlayer& p) {
        EXPECT_EQ(&p, player.get());
        ended++;
    });
    EXPECT_TRUE(ScriptRunner::isActive());
    auto frame = [](bool running) {
        ScriptRunner::beforeFrame(running);
        if (running) {
            Input::beginFrame();
            ScriptRunner::afterUpdate();
        }
    };
    frame(true);
    EXPECT_FALSE(Input::getKeyDown(D_KEY_B));
    // a paused app's frame does not count for a recording
    frame(false);
    EXPECT_EQ(ScriptRunner::frameOf(player.get()), 1);
    frame(true);
    EXPECT_TRUE(Input::getKeyDown(D_KEY_B));
    frame(true);
    EXPECT_TRUE(Input::getKeyUp(D_KEY_B));
    EXPECT_EQ(ended, 1);
    EXPECT_FALSE(ScriptRunner::isActive());
    EXPECT_EQ(ScriptRunner::frameOf(player.get()), -1);
}

}  // namespace InputScriptTest_cpp
