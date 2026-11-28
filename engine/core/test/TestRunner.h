// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project test runner (docs/specs/project-test-runner.md §6.8): one runner for the editor and
// the players. A Host gives it a fresh game session per Play Mode case (the editor's Play, a
// player's session reset); the runner loads the case's file into that session's Lua state with
// the Test table bound in the chunk's own environment (never in _G), and resumes the case's body
// as a coroutine after each game frame's update. Waits count game frames and game time only; the
// wall clock is a hang guard. Edit Mode cases run synchronously (TestChunk::runSync), in the
// editor's sandbox state, never through the frame loop.

#include "Export.h"
#include "test/TestTypes.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace jasperengine::gametest {

    // what differs between the editor (Play sessions) and a player (session reset)
    class JASPERENGINE_API Host {
    public:
        enum class SessionState { Starting, Ready, Failed };

        virtual ~Host();

        // starts the case's session: its scene, data folder, seed, fixed delta, render size;
        // false + error when it cannot (the case is an error)
        virtual bool beginSession(const CaseSpec& spec, std::string& error) = 0;
        // Ready once the session ran its first game frame; Failed + error (missing scene, crash)
        virtual SessionState sessionState(std::string& error) = 0;
        // stops the session (every play-mode change discarded) and removes the case's data
        // folder unless the run keeps it
        virtual void endSession() = 0;
        virtual bool sessionEnded() = 0;

        // the session's Lua state (the game's)
        virtual lua_State* luaState() = 0;
        // a file of the test root (root-relative, forward slashes)
        virtual bool readTestFile(const std::string& file, std::string& code, std::string& error) = 0;

        // frames and game seconds the session's update ran
        virtual uint64_t gameFrames() = 0;
        virtual double gameTime() = 0;
        // the hang guard's clock (seconds); the steady clock by default
        virtual double wallSeconds();

        // Test.capture: starts reading the case's frame; pollCapture is asked each tick until it
        // returns true with the §6.4 capture object (status passed, failed, new or pending)
        virtual bool beginCapture(const CaseSpec& spec, const std::string& name, const Json& options, std::string& error) = 0;
        virtual bool pollCapture(Json& capture) = 0;

        // the case's data:// folder
        virtual std::string dataFolder() const = 0;
    };

    struct RunOptions {
        Settings settings;
        Target target;
        std::string runId;
        std::string project;
        std::string filter;
        std::string mode = "all";
        // the [test] log lines (R26): players
        bool logProtocol = false;
        // a Lua call running this long without yielding is interrupted (R16)
        double interruptSeconds = 10.0;
    };

    struct CaseRunState;
    class TestChunk;

    class JASPERENGINE_API TestRunner {
    public:
        TestRunner();
        ~TestRunner();
        TestRunner(const TestRunner&) = delete;
        TestRunner& operator=(const TestRunner&) = delete;

        // A plan of Play Mode cases (an Edit Mode one is counted as excluded_edit, R15)
        void start(std::vector<CaseSpec> plan, Host& host, RunOptions options);
        // Each host frame, after the scenes' update: starts and ends sessions, resumes the
        // running case once per game frame. False once the run is done.
        bool frame();
        bool isRunning() const { return running; }
        // ends the run: the running case's session stops, the cases not run are not reported
        void cancel();

        const RunResult& result() const { return results; }
        size_t planned() const { return plan.size(); }
        size_t finished() const { return results.cases.size(); }
        // the case being run ("" between cases)
        std::string currentCase() const;

        // each case's final result (progress, the players' log lines)
        std::function<void(const CaseResult&)> onCaseDone;

    private:
        enum class Phase { Next, Starting, Running, Ending, Done };

        void beginCase();
        void runCaseFrame();
        void finishCase();
        bool loadReplay(CaseRunState& s, std::string& error);
        void reportReplay(CaseRunState& s);
        void endRun();

        Host* host = nullptr;
        RunOptions options;
        std::vector<CaseSpec> plan;
        size_t index = 0;
        Phase phase = Phase::Done;
        bool running = false;
        bool cancelled = false;
        RunResult results;
        std::unique_ptr<CaseRunState> current;
        std::chrono::steady_clock::time_point runStarted;
        // files whose top level ran once in this run: their computed-name cases joined the plan
        std::map<std::string, bool> discovered;
        int excludedEdit = 0;
    };

    // One test file loaded into a Lua state: the Test table in the chunk's environment, the
    // cases it registered, and running one of them
    class JASPERENGINE_API TestChunk {
    public:
        struct Registered {
            CaseSpec spec;
            int fnRef = -1;
            std::string optionError;   // a refused option (data = "project"): the case is an error
        };

        // readFile resolves require() of the test root's modules after the game's
        TestChunk(lua_State* L, Mode mode, const Settings& settings, const Target& target,
                  std::function<bool(const std::string&, std::string&)> readFile);
        ~TestChunk();
        TestChunk(const TestChunk&) = delete;
        TestChunk& operator=(const TestChunk&) = delete;

        // runs the file's top level; false + error (a syntax or runtime error)
        bool load(const std::string& code, const std::string& file, std::string& error);
        const std::vector<Registered>& cases() const { return registered; }
        // the registration of a name ("" when none; duplicate set when twice)
        const Registered* find(const std::string& name, bool& duplicate) const;

        // An Edit Mode case: setup, body and teardown run now (a wait raises)
        void runSync(const std::string& name, CaseResult& result);

        lua_State* state() const { return L; }

    private:
        friend class TestRunner;
        friend struct CaseRunState;
        friend struct ChunkAccess;

        lua_State* L = nullptr;
        Mode mode;
        Settings settings;
        Target target;
        std::function<bool(const std::string&, std::string&)> readFile;
        std::string file;
        int envRef = -1;
        int setupRef = -1;
        int teardownRef = -1;
        Json configOptions = Json::object();
        bool loading = false;
        std::vector<Registered> registered;
        CaseRunState* active = nullptr;
    };

}
