// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project test runner's case semantics (docs/specs/project-test-runner.md AC2, AC15, AC18's
// bench) against a fake Host that steps game frames, with a Lua state of its own.

#include <gtest/gtest.h>

#include "test/TestResults.h"
#include "test/TestRunner.h"

#include "Input.h"
#include "Log.h"
#include "input/InputRecorder.h"
#include "input/InputScriptRunner.h"
#include "io/UserDataOverride.h"
#include "player/PlayerTests.h"
#include "script/LuaBinding.h"
#include "support/AllocationCounter.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace TestRunnerTest_cpp {

using namespace jasperengine;
using namespace jasperengine::gametest;

namespace {

    int luaLogError(lua_State* L) {
        Log::error("%s", luaL_checkstring(L, 1));
        return 0;
    }

    class FakeHost : public Host {
    public:
        FakeHost() {
            L = luaL_newstate();
            luaL_openlibs(L);
            lua_register(L, "logError", luaLogError);
            lua_newtable(L);
            lua_setglobal(L, "events");
        }
        ~FakeHost() override { lua_close(L); }

        bool beginSession(const CaseSpec& spec, std::string& error) override {
            if (spec.scene == "Missing") {
                error = "no scene \"Missing\"";
                return false;
            }
            sessions++;
            inSession = true;
            return true;
        }
        SessionState sessionState(std::string&) override { return SessionState::Ready; }
        void endSession() override { inSession = false; }
        bool sessionEnded() override { return !inSession; }
        lua_State* luaState() override { return L; }
        bool readTestFile(const std::string& file, std::string& code, std::string& error) override {
            auto it = files.find(file);
            if (it == files.end()) {
                error = "no file " + file;
                return false;
            }
            code = it->second;
            return true;
        }
        uint64_t gameFrames() override { return frames; }
        double gameTime() override { return time; }
        double wallSeconds() override { return wall; }
        bool beginCapture(const CaseSpec&, const std::string& name, const Json&, std::string&) override {
            pendingCapture = name;
            return true;
        }
        bool pollCapture(Json& capture) override {
            capture = {{"name", pendingCapture}, {"status", captureStatus}};
            return true;
        }
        std::string dataFolder() const override { return "/tmp/case"; }

        // the run, one game frame between the runner's frames; at most `limit` frames
        RunResult run(const std::vector<CaseSpec>& plan, Settings settings = Settings(), int limit = 100000, double wallPerFrame = 1.0 / 60) {
            TestRunner runner;
            RunOptions options;
            options.settings = settings;
            options.target.platform = "linux";
            options.target.editor = false;
            runner.start(plan, *this, options);
            for (int i = 0; i < limit && runner.frame(); i++) {
                frames++;
                time += 1.0 / 60.0;
                wall += wallPerFrame;
            }
            EXPECT_FALSE(runner.isRunning()) << "the run did not end within " << limit << " frames";
            return runner.result();
        }

        // every case of a file, as the static listing would plan them
        std::vector<CaseSpec> planOf(const std::string& file, const std::vector<std::string>& names, Settings settings = Settings()) {
            std::vector<CaseSpec> plan;
            for (const std::string& name : names) {
                CaseSpec spec;
                spec.file = file;
                spec.name = name;
                spec.id = file + "#" + name;
                spec.fixedFps = settings.fixedFps;
                spec.timeoutFrames = settings.timeoutFrames;
                spec.waitFrames = settings.waitFrames;
                plan.push_back(spec);
            }
            return plan;
        }

        std::vector<std::string> events() {
            std::vector<std::string> out;
            lua_getglobal(L, "events");
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); i++) {
                lua_rawgeti(L, -1, i);
                out.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return out;
        }

        lua_State* L = nullptr;
        std::map<std::string, std::string> files;
        uint64_t frames = 0;
        double time = 0.0;
        double wall = 0.0;
        int sessions = 0;
        bool inSession = false;
        std::string pendingCapture;
        std::string captureStatus = "passed";
    };

    const CaseResult& byName(const RunResult& result, const std::string& name) {
        for (const CaseResult& r : result.cases)
            if (r.spec.name == name) return r;
        static CaseResult none;
        ADD_FAILURE() << "no result for " << name;
        return none;
    }

}

TEST(TestRunnerCore, CheckContinuesAndAssertEnds) {
    FakeHost host;
    host.files["a.test.lua"] = R"(
Test.case("checks", function(t)
    Test.check(false, "first")
    Test.check(1 == 1, "holds")
    Test.check(false, "second")
    Test.assert(false, "ends here")
    Test.check(false, "never reached")
end)
Test.case("passes", function(t)
    Test.check(true, "fine")
    Test.equal({a = {1, 2}}, {a = {1, 2}}, "deep")
end)
)";
    const RunResult result = host.run(host.planOf("a.test.lua", {"checks", "passes"}));
    ASSERT_EQ(result.cases.size(), 2u);
    const CaseResult& checks = byName(result, "checks");
    EXPECT_EQ(checks.status, Status::Failed);
    ASSERT_EQ(checks.failures.size(), 3u);
    EXPECT_EQ(checks.failures[0].kind, "check");
    EXPECT_EQ(checks.failures[0].message, "first");
    EXPECT_EQ(checks.failures[0].file, "a.test.lua");
    EXPECT_EQ(checks.failures[0].line, 3);
    EXPECT_EQ(checks.failures[1].message, "second");
    EXPECT_EQ(checks.failures[2].kind, "assert");
    EXPECT_EQ(checks.failures[2].message, "ends here");
    EXPECT_EQ(checks.failures[2].line, 6);
    EXPECT_EQ(byName(result, "passes").status, Status::Passed);
    EXPECT_EQ(result.counts.passed, 1);
    EXPECT_EQ(result.counts.failed, 1);
}

TEST(TestRunnerCore, EqualPrintsBothValuesAndThePath) {
    FakeHost host;
    host.files["e.test.lua"] = R"(
Test.case("lives", function(t)
    Test.equal(2, 3, "lives at start")
    Test.equal({inv = {coins = 1}}, {inv = {coins = 2}}, "inventory")
    Test.near(1.0, 1.5, 0.1, "speed")
end)
)";
    const RunResult result = host.run(host.planOf("e.test.lua", {"lives"}));
    const CaseResult& r = byName(result, "lives");
    ASSERT_EQ(r.failures.size(), 3u);
    EXPECT_EQ(r.failures[0].kind, "equal");
    EXPECT_EQ(r.failures[0].message, "lives at start: expected 3, got 2");
    EXPECT_EQ(r.failures[1].message, "inventory: at inv.coins: expected 2, got 1");
    EXPECT_NE(r.failures[2].message.find("speed: expected 1.5"), std::string::npos) << r.failures[2].message;
}

TEST(TestRunnerCore, WaitUntilFailsAtExactlyItsCapFrame) {
    FakeHost host;
    host.files["w.test.lua"] = R"(
Test.case("never", function(t)
    Test.waitFrames(3)
    Test.waitUntil(function() return false end, {frames = 10, message = "the door never opened"})
    Test.check(false, "not reached")
end)
Test.case("soon", function(t)
    local start = t.frame
    Test.waitUntil(function() return t.frame >= start + 4 end, {frames = 10})
    Test.equal(t.frame - start, 4, "the frame the predicate held")
end)
)";
    const RunResult result = host.run(host.planOf("w.test.lua", {"never", "soon"}));
    const CaseResult& never = byName(result, "never");
    ASSERT_EQ(never.failures.size(), 1u);
    EXPECT_EQ(never.failures[0].kind, "timeout");
    EXPECT_EQ(never.failures[0].frame, 13);
    EXPECT_EQ(never.failures[0].message, "waitUntil timed out after 10 frames (0.2 game s): the door never opened");
    EXPECT_EQ(never.failures[0].line, 4);
    EXPECT_EQ(byName(result, "soon").status, Status::Passed) << (byName(result, "soon").failures.empty() ? "" : byName(result, "soon").failures[0].message);
}

TEST(TestRunnerCore, WaitsCountGameFramesAndGameTime) {
    FakeHost host;
    host.files["g.test.lua"] = R"(
Test.case("time", function(t)
    local f = Test.waitFrames(30)
    Test.equal(f, 30, "waitFrames returns the case frame")
    Test.waitGameSeconds(1.0)
    Test.equal(t.frame, 90, "one game second at 60 fps")
    Test.near(t.gameTime, 1.5, 1e-6, "game time")
end)
)";
    const RunResult result = host.run(host.planOf("g.test.lua", {"time"}));
    const CaseResult& r = byName(result, "time");
    EXPECT_EQ(r.status, Status::Passed) << (r.failures.empty() ? "" : r.failures[0].message);
    EXPECT_EQ(r.frames, 90);
}

TEST(TestRunnerCore, LoggedErrorFailsAndExpectErrorAbsorbsExactlyOne) {
    FakeHost host;
    host.files["c.test.lua"] = R"(
Test.case("one expected", function(t)
    Test.expectError("boom")
    logError("boom: the first")
    Test.waitFrames(1)
end)
Test.case("two logged", function(t)
    Test.expectError("boom")
    logError("boom: the first")
    logError("boom: the second")
    Test.waitFrames(1)
end)
Test.case("unmatched expectation", {expectErrors = {"never logged"}}, function(t)
    Test.waitFrames(1)
end)
Test.case("unexpected", function(t)
    logError("player.lua:40: attempt to index a nil value")
    Test.waitFrames(1)
end)
)";
    const RunResult result = host.run(host.planOf("c.test.lua", {"one expected", "two logged", "unmatched expectation", "unexpected"}));
    EXPECT_EQ(byName(result, "one expected").status, Status::Passed);
    const CaseResult& two = byName(result, "two logged");
    ASSERT_EQ(two.failures.size(), 1u);
    EXPECT_EQ(two.failures[0].kind, "console");
    EXPECT_EQ(two.failures[0].message, "boom: the second");
    const CaseResult& unmatched = byName(result, "unmatched expectation");
    ASSERT_EQ(unmatched.failures.size(), 1u);
    EXPECT_EQ(unmatched.failures[0].kind, "expected_error_missing");
    const CaseResult& unexpected = byName(result, "unexpected");
    ASSERT_EQ(unexpected.failures.size(), 1u);
    EXPECT_EQ(unexpected.failures[0].file, "player.lua");
    EXPECT_EQ(unexpected.failures[0].line, 40);

    // console_errors: warn attaches the line without failing
    Settings warn;
    warn.consoleErrorsFail = false;
    const RunResult warned = host.run(host.planOf("c.test.lua", {"unexpected"}, warn), warn);
    EXPECT_EQ(byName(warned, "unexpected").status, Status::Passed);
    EXPECT_EQ(byName(warned, "unexpected").console.size(), 1u);
}

TEST(TestRunnerCore, SkipAndPlatformsGiveSkipped) {
    FakeHost host;
    host.files["s.test.lua"] = R"(
Test.case("option", {skip = "waiting for the shop"}, function(t) Test.fail("ran") end)
Test.case("call", function(t) Test.skip("not today") Test.fail("ran") end)
Test.case("android only", {platforms = {"android"}}, function(t) Test.fail("ran") end)
Test.case("linux too", {platforms = {"android", "linux"}}, function(t) end)
)";
    std::vector<CaseSpec> plan = host.planOf("s.test.lua", {"option", "call", "android only", "linux too"});
    plan[0].skip = "waiting for the shop";
    plan[2].platforms = {"android"};
    plan[3].platforms = {"android", "linux"};
    const RunResult result = host.run(plan);
    EXPECT_EQ(byName(result, "option").status, Status::Skipped);
    EXPECT_EQ(byName(result, "option").skipReason, "waiting for the shop");
    EXPECT_EQ(byName(result, "call").status, Status::Skipped);
    EXPECT_EQ(byName(result, "call").skipReason, "not today");
    EXPECT_EQ(byName(result, "android only").status, Status::Skipped);
    EXPECT_EQ(byName(result, "linux too").status, Status::Passed);
    EXPECT_EQ(result.counts.skipped, 3);
    EXPECT_EQ(result.counts.passed, 1);
}

TEST(TestRunnerCore, SetupAndTeardownRunAroundEachCaseAndAfterAFailure) {
    FakeHost host;
    host.files["o.test.lua"] = R"(
Test.setup(function(t) table.insert(events, "setup " .. t.name) end)
Test.teardown(function(t) table.insert(events, "teardown " .. t.name) end)
Test.case("a", function(t) table.insert(events, "body a") Test.waitFrames(2) end)
Test.case("b", function(t) table.insert(events, "body b") Test.assert(false, "b fails") end)
Test.case("c", function(t) table.insert(events, "body c") error("c raises") end)
)";
    const RunResult result = host.run(host.planOf("o.test.lua", {"a", "b", "c"}));
    const std::vector<std::string> expected = {"setup a", "body a", "teardown a", "setup b", "body b", "teardown b",
                                               "setup c", "body c", "teardown c"};
    EXPECT_EQ(host.events(), expected);
    EXPECT_EQ(host.sessions, 3) << "a fresh session per case";
    const CaseResult& c = byName(result, "c");
    ASSERT_EQ(c.failures.size(), 1u);
    EXPECT_EQ(c.failures[0].kind, "error");
    EXPECT_EQ(c.failures[0].line, 6);
    EXPECT_NE(c.failures[0].message.find("c raises"), std::string::npos);
}

TEST(TestRunnerCore, DuplicateNamesGiveError) {
    FakeHost host;
    host.files["d.test.lua"] = R"(
Test.case("same", function(t) end)
Test.case("same", function(t) end)
Test.case("other", function(t) end)
)";
    std::vector<CaseSpec> plan = host.planOf("d.test.lua", {"same", "other"});
    const RunResult result = host.run(plan);
    EXPECT_EQ(byName(result, "same").status, Status::Error);
    EXPECT_NE(byName(result, "same").failures[0].message.find("duplicate case name"), std::string::npos);
    EXPECT_EQ(byName(result, "other").status, Status::Passed);
}

TEST(TestRunnerCore, ErrorsDoNotStopTheRun) {
    FakeHost host;
    host.files["x.test.lua"] = "Test.case('fine', function(t) end)\n";
    host.files["bad.test.lua"] = "Test.case('broken' function(t) end)\n";
    std::vector<CaseSpec> plan = host.planOf("bad.test.lua", {"broken"});
    std::vector<CaseSpec> more = host.planOf("x.test.lua", {"fine"});
    CaseSpec missing = more[0];
    missing.name = "on a missing scene";
    missing.id = "x.test.lua#on a missing scene";
    missing.scene = "Missing";
    plan.push_back(missing);
    plan.insert(plan.end(), more.begin(), more.end());
    const RunResult result = host.run(plan);
    ASSERT_EQ(result.cases.size(), 3u);
    EXPECT_EQ(result.cases[0].status, Status::Error);
    EXPECT_EQ(result.cases[1].status, Status::Error);
    EXPECT_EQ(result.cases[1].failures[0].message, "no scene \"Missing\"");
    EXPECT_EQ(result.cases[2].status, Status::Passed);
    EXPECT_EQ(result.counts.error, 2);
}

// Test.config's table is the file's defaults for each case: an unknown key errors every case of the
// file, naming it. (The table was iterated after its copy went: the Android player crashed in it,
// and an ASan build reports the read)
TEST(TestRunnerCore, ConfigAppliesToEveryCaseOfTheFile) {
    FakeHost host;
    host.files["f.test.lua"] = R"(
Test.config{ tags = {"smoke", "config"}, colour = "red" }
Test.case("one", function(t) end)
Test.case("two", function(t) end)
)";
    const RunResult result = host.run(host.planOf("f.test.lua", {"one", "two"}));
    for (const char* name : {"one", "two"}) {
        const CaseResult& r = byName(result, name);
        EXPECT_EQ(r.status, Status::Error) << name;
        ASSERT_FALSE(r.failures.empty()) << name;
        EXPECT_NE(r.failures[0].message.find("Test.config: unknown option \"colour\""), std::string::npos) << r.failures[0].message;
    }
}

// R26: the log's category puts "[test] " before each protocol line, so the line itself starts with
// its kind ("[test] [test] summary" missed the Android expectation of AC13)
TEST(TestRunnerCore, ProtocolLinesLeaveTheCategoryToTheLog) {
    Counts counts;
    counts.total = 4;
    counts.passed = 3;
    counts.skipped = 1;
    EXPECT_EQ(summaryLine(counts), "summary {\"error\":0,\"excluded_edit\":0,\"failed\":0,\"hung\":0,\"passed\":3,\"skipped\":1,\"total\":4}");
    EXPECT_EQ(beginLine(4, "r1"), "begin {\"cases\":4,\"run\":\"r1\"}");
    CaseResult result;
    result.spec.name = "one";
    EXPECT_EQ(caseLine(result).rfind("case {", 0), 0u) << caseLine(result);
}

TEST(TestRunnerCore, HangAndTimeoutAreDifferent) {
    FakeHost host;
    host.files["h.test.lua"] = R"(
Test.case("never true", function(t)
    Test.waitUntil(function() return false end)
end)
)";
    // no cap on the wait: the case's timeout_frames ends it, as a failure
    Settings settings;
    settings.timeoutFrames = 50;
    settings.waitFrames = 100000;
    const RunResult timedOut = host.run(host.planOf("h.test.lua", {"never true"}, settings), settings);
    const CaseResult& t = byName(timedOut, "never true");
    EXPECT_EQ(t.status, Status::Failed);
    ASSERT_EQ(t.failures.size(), 1u);
    EXPECT_EQ(t.failures[0].kind, "timeout");
    EXPECT_EQ(t.failures[0].frame, 50);

    // the wall clock past hang_seconds: hung, whatever the frames
    Settings hang;
    hang.timeoutFrames = 1000000;
    hang.waitFrames = 1000000;
    hang.hangSeconds = 5.0;
    const RunResult hung = host.run(host.planOf("h.test.lua", {"never true"}, hang), hang, 100000, 1.0);
    const CaseResult& h = byName(hung, "never true");
    EXPECT_EQ(h.status, Status::Hung);
    ASSERT_FALSE(h.failures.empty());
    EXPECT_EQ(h.failures[0].kind, "hung");
    EXPECT_EQ(hung.counts.hung, 1);
}

TEST(TestRunnerCore, ARunawayCallIsInterrupted) {
    FakeHost host;
    host.files["r.test.lua"] = R"(
Test.case("spins", function(t) while true do end end)
Test.case("next", function(t) end)
)";
    TestRunner runner;
    RunOptions options;
    options.interruptSeconds = 0.2;
    runner.start(host.planOf("r.test.lua", {"spins", "next"}), host, options);
    for (int i = 0; i < 1000 && runner.frame(); i++) host.frames++;
    const RunResult& result = runner.result();
    EXPECT_EQ(byName(result, "spins").status, Status::Error);
    EXPECT_NE(byName(result, "spins").failures[0].message.find("script interrupted"), std::string::npos);
    EXPECT_EQ(byName(result, "next").status, Status::Passed);
}

TEST(TestRunnerCore, CapturesNeverEndTheCaseAndANewOneFails) {
    FakeHost host;
    host.captureStatus = "new";
    host.files["p.test.lua"] = R"(
Test.case("shot", function(t)
    Test.capture("start")
    table.insert(events, "after capture")
end)
)";
    const RunResult result = host.run(host.planOf("p.test.lua", {"shot"}));
    const CaseResult& r = byName(result, "shot");
    EXPECT_EQ(r.status, Status::Failed) << "a capture without a golden is never a pass";
    ASSERT_EQ(r.captures.size(), 1u);
    EXPECT_EQ(r.captures[0].value("status", ""), "new");
    EXPECT_EQ(host.events(), std::vector<std::string>{"after capture"});
}

TEST(TestRunnerCore, TheTestTableStaysOutOfGlobals) {
    FakeHost host;
    luaL_dostring(host.L, "Test = 5");
    host.files["g.test.lua"] = R"(
Test.case("bound", function(t)
    Test.check(type(Test) == "table", "Test in the chunk")
    Test.check(rawget(_G, "Test") == 5, "the game's global untouched")
end)
)";
    const RunResult result = host.run(host.planOf("g.test.lua", {"bound"}));
    EXPECT_EQ(byName(result, "bound").status, Status::Passed);
    lua_getglobal(host.L, "Test");
    EXPECT_EQ(lua_tointeger(host.L, -1), 5);
    lua_pop(host.L, 1);
}

TEST(TestRunnerCore, ComputedNamesJoinThePlanOnceTheirFileRan) {
    FakeHost host;
    host.files["n.test.lua"] = R"(
Test.case("literal", function(t) end)
for i = 1, 2 do Test.case("level " .. i, function(t) end) end
)";
    const RunResult result = host.run(host.planOf("n.test.lua", {"literal"}));
    ASSERT_EQ(result.cases.size(), 3u);
    EXPECT_EQ(result.cases[1].spec.id, "n.test.lua#level 1");
    EXPECT_EQ(result.cases[2].status, Status::Passed);
}

TEST(TestRunnerCore, ResultsDocumentAndJUnit) {
    FakeHost host;
    host.files["j.test.lua"] = R"(
Test.case("ok", {tags = {"smoke"}}, function(t) Test.log("hello") end)
Test.case("bad", function(t) Test.assert(false, "nope") end)
Test.case("skip", {skip = "later"}, function(t) end)
)";
    std::vector<CaseSpec> plan = host.planOf("j.test.lua", {"ok", "bad", "skip"});
    plan[2].skip = "later";
    const RunResult result = host.run(plan);
    const Json json = resultsJson(result);
    EXPECT_EQ(json["format"], RESULTS_FORMAT);
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["counts"]["total"], 3);
    RunResult back;
    std::string error;
    ASSERT_TRUE(resultsFromJson(json, back, error)) << error;
    EXPECT_EQ(back.cases.size(), 3u);
    EXPECT_EQ(back.cases[1].failures[0].message, "nope");
    Json newer = json;
    newer["version"] = 2;
    EXPECT_FALSE(resultsFromJson(newer, back, error));
    EXPECT_NE(error.find("made by a newer editor"), std::string::npos);

    const std::string xml = junitXml(result);
    EXPECT_NE(xml.find("<testsuite name=\"j.test.lua\" tests=\"3\" failures=\"1\" errors=\"0\" skipped=\"1\""), std::string::npos) << xml;
    EXPECT_NE(xml.find("<failure message=\"nope\" type=\"assert\">"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<skipped message=\"later\"/>"), std::string::npos);
    EXPECT_NE(xml.find("<system-out>hello"), std::string::npos);
}

TEST(TestRunnerCore, FiltersAndCaseIdPaths) {
    CaseSpec spec;
    spec.id = "ui/menu.test.lua#Start opens the level";
    spec.tags = {"smoke"};
    EXPECT_TRUE(matchesFilter(spec, ""));
    EXPECT_TRUE(matchesFilter(spec, "START OPENS"));
    EXPECT_TRUE(matchesFilter(spec, "tag:red, tag:Smoke"));
    EXPECT_FALSE(matchesFilter(spec, "tag:red,shop"));
    EXPECT_EQ(caseIdPath("ui/menu.test.lua#Start opens the level"), "ui__menu.test.lua__Start_opens_the_level");
}

// R8: Test.input runs the events through the shared interpreter and returns when they are done;
// a recording is refused, a malformed event is the case's error
TEST(TestRunnerCore, InputRunsOnTheSharedInterpreter) {
    FakeHost host;
    host.files["i.test.lua"] = R"(
Test.case("press", function(t)
    local start = t.frame
    Test.input({ {type = "key", key = "b", action = "down"}, {type = "wait", frames = 3} })
    Test.check(t.frame - start >= 3, "the events' frames passed")
end)
Test.case("recording refused", function(t)
    Test.input("Input/rec.input.json")
end)
Test.case("bad event", function(t)
    Test.input({ {type = "key", key = "nokey"} })
end)
)";
    host.files["Input/rec.input.json"] = R"({"format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play", "fixed_fps": 60,
        "events": [{"frame": 1, "type": "key", "key": "b", "action": "down"}]})";
    TestRunner runner;
    runner.start(host.planOf("i.test.lua", {"press", "recording refused", "bad event"}), host, RunOptions());
    bool sawB = false;
    for (int i = 0; i < 1000 && runner.frame(); i++) {
        // the engine's frame loop: the interpreter before the input latch, then the update
        input::ScriptRunner::beforeFrame(true);
        Input::beginFrame();
        sawB = sawB || Input::isKeyPressed(D_KEY_B);
        input::ScriptRunner::afterUpdate();
        host.frames++;
        host.time += 1.0 / 60.0;
    }
    const RunResult& result = runner.result();
    const CaseResult& press = byName(result, "press");
    EXPECT_EQ(press.status, Status::Passed) << (press.failures.empty() ? "" : press.failures[0].message);
    EXPECT_TRUE(sawB) << "the key reached the game through Engine::system*";
    EXPECT_FALSE(Input::isKeyPressed(D_KEY_B)) << "what the events held is released at their end";
    const CaseResult& refused = byName(result, "recording refused");
    ASSERT_FALSE(refused.failures.empty());
    EXPECT_NE(refused.failures[0].message.find("a recording replays from Play's start: use Test.case{replay = ...}"), std::string::npos)
        << refused.failures[0].message;
    const CaseResult& bad = byName(result, "bad event");
    ASSERT_FALSE(bad.failures.empty());
    EXPECT_NE(bad.failures[0].message.find("unknown key"), std::string::npos) << bad.failures[0].message;
    input::ScriptRunner::stopAll();
}

// R9: a replay case's session takes the recording's frame rate, seed and scene, the recording
// feeds it from the first frame beside the body, and a checkpoint that differs fails it as a
// divergence naming the frame and what differs (a warning with divergence: warn)
TEST(TestRunnerCore, ReplayCasesFeedTheRecordingAndReportDivergence) {
    struct SpecHost : FakeHost {
        bool beginSession(const CaseSpec& spec, std::string& error) override {
            began.push_back(spec);
            readyAt = -1;
            return FakeHost::beginSession(spec, error);
        }
        SessionState sessionState(std::string&) override {
            if (readyAt < 0) readyAt = static_cast<int64_t>(frames);
            return SessionState::Ready;
        }
        std::vector<CaseSpec> began;
        int64_t readyAt = -1;
    };
    const std::string recording = R"({"format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play", "fixed_fps": 30,
        "seed": 7, "scene": "Level", "platform": ")" + input::platformTag() + R"(", "frames": 8,
        "checkpoints": [{"frame": 3, "hash": "1", "values": {"lives": 3}}],
        "events": [{"frame": 0, "type": "key", "key": "b", "action": "down"}, {"frame": 5, "type": "key", "key": "b", "action": "up"}]})";
    const std::string tests = R"(
Test.case("replays", {replay = "Input/run.input.json"}, function(t)
    Test.waitFrames(12)
end)
Test.case("a script is refused", {replay = "Input/script.input.json"}, function(t) end)
)";
    for (const bool warn : {false, true}) {
        SpecHost host;
        host.files["r.test.lua"] = tests;
        host.files["Input/run.input.json"] = recording;
        host.files["Input/script.input.json"] = R"({"format": "jasperengine-input", "version": 1, "kind": "script", "events": [{"type": "wait", "frames": 1}]})";
        Settings settings;
        settings.divergenceFail = !warn;
        std::vector<CaseSpec> plan = host.planOf("r.test.lua", {"replays", "a script is refused"}, settings);
        plan[0].replay = "Input/run.input.json";
        plan[1].replay = "Input/script.input.json";
        TestRunner runner;
        RunOptions options;
        options.settings = settings;
        runner.start(plan, host, options);
        bool sawB = false;
        int64_t firstB = -1;
        for (int i = 0; i < 1000 && runner.frame(); i++) {
            input::ScriptRunner::beforeFrame(true);
            Input::beginFrame();
            if (Input::isKeyPressed(D_KEY_B) && firstB < 0) firstB = static_cast<int64_t>(host.frames);
            sawB = sawB || Input::isKeyPressed(D_KEY_B);
            input::ScriptRunner::afterUpdate();
            host.frames++;
            host.time += 1.0 / 30.0;
        }
        const RunResult& result = runner.result();
        ASSERT_EQ(host.began.size(), 1u) << "the script's case never starts a session";
        EXPECT_EQ(host.began[0].fixedFps, 30);
        EXPECT_EQ(host.began[0].seed, 7u);
        EXPECT_EQ(host.began[0].scene, "Level");
        EXPECT_TRUE(sawB) << "the recording's key reached the game";
        EXPECT_EQ(firstB, host.readyAt) << "the recording plays from the session's first frame";
        const CaseResult& replays = byName(result, "replays");
        if (!warn) {
            EXPECT_EQ(replays.status, Status::Failed);
            ASSERT_EQ(replays.failures.size(), 1u);
            EXPECT_EQ(replays.failures[0].kind, "divergence");
            EXPECT_NE(replays.failures[0].message.find("diverged at frame 3"), std::string::npos) << replays.failures[0].message;
            EXPECT_NE(replays.failures[0].message.find("lives 3 -> null"), std::string::npos) << replays.failures[0].message;
        } else {
            EXPECT_EQ(replays.status, Status::Passed) << (replays.failures.empty() ? "" : replays.failures[0].message);
            const bool warned = std::any_of(replays.console.begin(), replays.console.end(), [](const ConsoleLine& line) {
                return line.level == "warning" && line.message.find("diverged at frame 3") != std::string::npos;
            });
            EXPECT_TRUE(warned) << "divergence: warn keeps the report as a warning";
        }
        const CaseResult& refused = byName(result, "a script is refused");
        EXPECT_EQ(refused.status, Status::Error);
        ASSERT_FALSE(refused.failures.empty());
        EXPECT_NE(refused.failures[0].message.find("is a script, not a recording"), std::string::npos) << refused.failures[0].message;
        EXPECT_FALSE(Input::isKeyPressed(D_KEY_B)) << "the replay's keys are released when it ends";
        input::ScriptRunner::stopAll();
    }
}

// R24, R25: a player's case starts from a new Lua state in a data:// sandbox of its own; nothing the
// earlier case left in Lua reaches it, and the sandbox goes with the case
TEST(TestRunnerCore, PlayerSessionsStartFromAFreshLuaStateInASandbox) {
    struct PlayerLikeHost : FakeHost {
        std::vector<std::string> folders;
        std::string folder;
        int cases = 0;
        bool beginSession(const CaseSpec& spec, std::string& error) override {
            if (!player::PlayerSession::begin(spec, "run-test", ++cases, folder, error)) return false;
            folders.push_back(getUserDataOverride());
            inSession = true;
            return true;
        }
        void endSession() override {
            player::PlayerSession::end(folder, false);
            inSession = false;
        }
        lua_State* luaState() override { return LuaBinding::getLuaState(); }
    } host;
    host.files["leak.test.lua"] = R"(
Test.case("leaves state behind", function(t)
    _G.LeakedByAnEarlierCase = true
    string.leakedByAnEarlierCase = true
    package.loaded.leakedModule = {}
end)
Test.case("starts fresh", function(t)
    Test.check(rawget(_G, "LeakedByAnEarlierCase") == nil, "a global of the earlier case leaked")
    Test.check(string.leakedByAnEarlierCase == nil, "a library change of the earlier case leaked")
    Test.check(package.loaded.leakedModule == nil, "a module of the earlier case leaked")
end)
)";
    const uint64_t generation = LuaBinding::getLuaStateGeneration();
    TestRunner runner;
    runner.start(host.planOf("leak.test.lua", {"leaves state behind", "starts fresh"}), host, RunOptions());
    for (int i = 0; i < 1000 && runner.frame(); i++) host.frames++;
    const RunResult& result = runner.result();
    const CaseResult& fresh = byName(result, "starts fresh");
    EXPECT_EQ(fresh.status, Status::Passed) << (fresh.failures.empty() ? "" : fresh.failures[0].message);
    EXPECT_GE(LuaBinding::getLuaStateGeneration(), generation + 2) << "a new Lua state per case";
    ASSERT_EQ(host.folders.size(), 2u);
    // (the folders are native paths: backslashes on Windows)
    EXPECT_NE(std::filesystem::path(host.folders[0]).generic_string().find(".jasper-test-runs/run-test/1"), std::string::npos) << host.folders[0];
    EXPECT_NE(std::filesystem::path(host.folders[1]).generic_string().find(".jasper-test-runs/run-test/2"), std::string::npos) << host.folders[1];
    EXPECT_FALSE(std::filesystem::exists(host.folders[0])) << "the sandbox goes with its case";
    EXPECT_TRUE(getUserDataOverride().empty()) << "data:// is the game's again after the run";
}

TEST(TestRunnerBench, FrameOverhead) {
    FakeHost host;
    host.files["b.test.lua"] = R"(
local state = {done = false}
Test.case("bench", function(t)
    Test.waitUntil(function() return state.done end, {frames = 1000000})
end)
)";
    Settings settings;
    settings.timeoutFrames = 600;
    settings.waitFrames = 1000000;
    TestRunner runner;
    RunOptions options;
    options.settings = settings;
    runner.start(host.planOf("b.test.lua", {"bench"}, settings), host, options);
    // into the running case
    runner.frame();
    runner.frame();
    double worst = 0.0;
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    for (; frames < 590; frames++) {
        host.frames++;
        host.time += 1.0 / 60.0;
        runner.frame();
    }
    const double perFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
    worst = perFrameMs;
    std::printf("[bench] TestRunnerBench.FrameOverhead %.4f ms/frame (budget 0.05)\n", worst);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(perFrameMs, 0.05 * slack);
    }
}

}  // namespace TestRunnerTest_cpp
