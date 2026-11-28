// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestRunner.h"

#include "test/TestOptions.h"
#include "test/TestResults.h"

#include "Log.h"
#include "input/InputReplay.h"
#include "input/InputScript.h"
#include "input/InputScriptRunner.h"
#include "log/LogSink.h"
#include "log/Logger.h"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace jasperengine;

namespace jasperengine::gametest {

namespace {

    JE_LOG_CATEGORY(logCategoryTest, "test", ::jasperengine::LogLevel::Info)

    // the error object of Test.assert, Test.fail and Test.skip: the case ends, nothing more is recorded
    int endCaseSentinel = 0;

    constexpr size_t MAX_CONSOLE_LINES = 1000;
    constexpr size_t MAX_WARNINGS = 20;
    constexpr int MAX_EQUAL_DEPTH = 8;

    double steadySeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- the console watch (R11): a log sink active only while a case runs

    struct ConsoleCollector {
        std::mutex mutex;
        std::vector<ConsoleLine> lines;
        size_t dropped = 0;
    };

    ConsoleCollector& collector() {
        static auto* c = new ConsoleCollector();
        return *c;
    }

    // "player.lua:40: attempt to index a nil value" -> player.lua, 40
    void parseLocation(const std::string& message, std::string& file, int& line) {
        const size_t colon = message.find(':');
        if (colon == std::string::npos || colon == 0) return;
        size_t end = colon + 1;
        while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end]))) end++;
        if (end == colon + 1 || end >= message.size() || message[end] != ':') return;
        const std::string candidate = message.substr(0, colon);
        if (candidate.find(' ') != std::string::npos || candidate.find('\n') != std::string::npos) return;
        file = candidate;
        line = std::atoi(message.substr(colon + 1, end - colon - 1).c_str());
    }

    class CaseConsoleSink : public LogSink {
    public:
        CaseConsoleSink() { minLevel.store(LogLevel::Warn); }
        void write(const LogRecord& record) override {
            if (record.level < LogLevel::Warn) return;
            const char* category = record.category ? record.category->name : "";
            // the editor's own tooling and the runner's lines are not the game's
            if (std::strcmp(category, "mcp") == 0 || std::strcmp(category, "build") == 0 || std::strcmp(category, "test") == 0) return;
            ConsoleLine line;
            line.level = record.level >= LogLevel::Error ? "error" : "warning";
            line.message = std::string(record.text);
            // the text before a traceback is the line; Lua's "file:line:" names its source
            const size_t traceback = line.message.find("\nstack traceback:");
            if (traceback != std::string::npos) line.message.erase(traceback);
            parseLocation(line.message, line.file, line.line);
            ConsoleCollector& c = collector();
            std::lock_guard<std::mutex> lock(c.mutex);
            if (c.lines.size() < MAX_CONSOLE_LINES) c.lines.push_back(std::move(line));
            else c.dropped++;
        }
        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "test-runner"; }
    };

    std::vector<ConsoleLine> takeConsole() {
        ConsoleCollector& c = collector();
        std::lock_guard<std::mutex> lock(c.mutex);
        std::vector<ConsoleLine> out;
        out.swap(c.lines);
        return out;
    }

    // ---- Lua helpers

    void callerLocation(lua_State* L, std::string& file, int& line) {
        lua_Debug ar;
        for (int level = 1; lua_getstack(L, level, &ar); level++) {
            if (!lua_getinfo(L, "Sl", &ar)) continue;
            if (ar.currentline > 0 && ar.source && ar.source[0] == '@') {
                file = ar.source + 1;
                line = ar.currentline;
                return;
            }
        }
    }

    std::string luaToText(lua_State* L, int index) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, index, &length);
        std::string out(text ? text : "", length);
        lua_pop(L, 1);
        return out;
    }

    // a value as a failure message shows it: strings quoted, tables summarised
    std::string describe(lua_State* L, int index, int depth = 0) {
        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
            case LUA_TNIL: return "nil";
            case LUA_TBOOLEAN: return lua_toboolean(L, index) ? "true" : "false";
            case LUA_TNUMBER: {
                if (lua_isinteger(L, index)) return std::to_string(static_cast<long long>(lua_tointeger(L, index)));
                char text[64];
                std::snprintf(text, sizeof(text), "%.14g", lua_tonumber(L, index));
                return text;
            }
            case LUA_TSTRING: return "\"" + std::string(lua_tostring(L, index)) + "\"";
            case LUA_TTABLE: {
                if (depth >= 2) return "{...}";
                std::string out = "{";
                int shown = 0;
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (shown == 8) {
                        out += ", ...";
                        lua_pop(L, 2);
                        break;
                    }
                    if (shown > 0) out += ", ";
                    if (lua_type(L, -2) == LUA_TSTRING) out += std::string(lua_tostring(L, -2)) + " = ";
                    else out += "[" + luaToText(L, -2) + "] = ";
                    out += describe(L, -1, depth + 1);
                    shown++;
                    lua_pop(L, 1);
                }
                return out + "}";
            }
            default: return luaToText(L, index);
        }
    }

    std::string keyPath(lua_State* L, int keyIndex) {
        if (lua_type(L, keyIndex) == LUA_TSTRING) return "." + std::string(lua_tostring(L, keyIndex));
        return "[" + luaToText(L, keyIndex) + "]";
    }

    // deep equality, at most MAX_EQUAL_DEPTH levels; path gets the first differing key path and
    // the two values are left described in expectedText / actualText
    bool deepEqual(lua_State* L, int a, int b, int depth, std::string& path, std::string& actualText, std::string& expectedText) {
        a = lua_absindex(L, a);
        b = lua_absindex(L, b);
        if (lua_type(L, a) == LUA_TTABLE && lua_type(L, b) == LUA_TTABLE && depth < MAX_EQUAL_DEPTH && !lua_rawequal(L, a, b)) {
            lua_pushnil(L);
            while (lua_next(L, a) != 0) {
                lua_pushvalue(L, -2);
                lua_gettable(L, b);
                std::string sub;
                if (!deepEqual(L, -2, -1, depth + 1, sub, actualText, expectedText)) {
                    path = keyPath(L, -3) + sub;
                    lua_pop(L, 3);
                    return false;
                }
                lua_pop(L, 2);
            }
            lua_pushnil(L);
            while (lua_next(L, b) != 0) {
                lua_pushvalue(L, -2);
                lua_gettable(L, a);
                if (lua_isnil(L, -1)) {
                    path = keyPath(L, -3);
                    actualText = "nil";
                    expectedText = describe(L, -2);
                    lua_pop(L, 3);
                    return false;
                }
                lua_pop(L, 2);
            }
            return true;
        }
        if (lua_compare(L, a, b, LUA_OPEQ)) return true;
        actualText = describe(L, a);
        expectedText = describe(L, b);
        return false;
    }

    // a Lua value as JSON (Test.input's events): tables with keys 1..n are arrays
    Json luaToJson(lua_State* L, int index, int depth = 0) {
        index = lua_absindex(L, index);
        if (depth > 16) return nullptr;
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)) return static_cast<int64_t>(lua_tointeger(L, index));
                return lua_tonumber(L, index);
            case LUA_TSTRING: return std::string(lua_tostring(L, index));
            case LUA_TTABLE: {
                const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(L, index));
                size_t keys = 0;
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    keys++;
                    lua_pop(L, 1);
                }
                if (length > 0 && static_cast<size_t>(length) == keys) {
                    Json array = Json::array();
                    for (lua_Integer i = 1; i <= length; i++) {
                        lua_rawgeti(L, index, i);
                        array.push_back(luaToJson(L, -1, depth + 1));
                        lua_pop(L, 1);
                    }
                    return array;
                }
                Json object = Json::object();
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) object[lua_tostring(L, -2)] = luaToJson(L, -1, depth + 1);
                    lua_pop(L, 1);
                }
                return object;
            }
            default: return nullptr;
        }
    }

    // ---- the runner's interrupt of a Lua call that never yields (R16)

    struct InterruptState {
        std::chrono::steady_clock::time_point deadline;
        double seconds = 10.0;
        bool fired = false;
    };
    InterruptState interrupt;

    void interruptHook(lua_State* L, lua_Debug*) {
        if (std::chrono::steady_clock::now() < interrupt.deadline) return;
        lua_sethook(L, nullptr, 0, 0);
        interrupt.fired = true;
        char message[256];
        {
            // (gone before the raise: Lua is compiled as C, and luaL_error longjmps over this frame)
            std::string file;
            int line = 0;
            callerLocation(L, file, line);
            std::snprintf(message, sizeof(message), "script interrupted: %s:%d ran for %.1f s without yielding (test runner)",
                          file.empty() ? "a function" : file.c_str(), line, interrupt.seconds);
        }
        luaL_error(L, "%s", message);
    }

    void armInterrupt(lua_State* L, double seconds) {
        interrupt.seconds = seconds;
        interrupt.fired = false;
        interrupt.deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6));
        lua_sethook(L, interruptHook, LUA_MASKCOUNT, 10000);
    }

    void disarmInterrupt(lua_State* L) {
        if (lua_gethook(L) == interruptHook) lua_sethook(L, nullptr, 0, 0);
    }

}

}  // namespace jasperengine::gametest

namespace jasperengine::gametest {

    Host::~Host() = default;

    double Host::wallSeconds() {
        return steadySeconds();
    }

    // What a running case waits for
    struct Wait {
        enum Kind { None, Frames, GameSeconds, Until, Capture, Input } kind = None;
        // Test.input: the shared interpreter's run (input-recorder-and-input-scripts R7)
        std::shared_ptr<input::ScriptPlayer> inputPlayer;
        int64_t untilFrame = 0;
        double untilTime = 0.0;
        int predicateRef = LUA_NOREF;
        int64_t startFrame = 0;
        int64_t capFrames = 0;
        std::string message;
        std::string captureName;
        Json captureOptions;
        bool captureBegun = false;
        std::string file;
        int line = 0;
    };

    struct CaseRunState {
        enum class Step { Setup, Body, Teardown, Done };

        std::unique_ptr<TestChunk> chunk;
        CaseResult result;
        Host* host = nullptr;
        Step step = Step::Setup;
        lua_State* co = nullptr;
        int coRef = LUA_NOREF;
        int tRef = LUA_NOREF;
        Wait wait;
        uint64_t frameBase = 0;
        double timeBase = 0.0;
        double wallStart = 0.0;
        int64_t frame = 0;
        int64_t lastFrame = -1;
        double gameTime = 0.0;
        bool skipped = false;
        bool runnerError = false;        // R17: the runner could not run it
        bool bodyFailed = false;         // setup ended the case: the body does not run
        bool hung = false;
        double interruptSeconds = 10.0;
        Logger::SinkId sink = 0;
        // R9: the recording a replay case feeds its session from frame 0
        std::shared_ptr<input::ParsedScript> replayScript;
        std::shared_ptr<input::Replay> replay;

        void fail(const std::string& kind, const std::string& message, const std::string& file, int line) {
            result.failures.push_back({kind, message, file.empty() ? result.spec.file : file, line, frame});
        }
    };

    struct ChunkAccess {
        static TestChunk* chunk(lua_State* L) {
            return static_cast<TestChunk*>(lua_touserdata(L, lua_upvalueindex(1)));
        }

        static CaseRunState* activeCase(lua_State* L, const char* function) {
            TestChunk* c = chunk(L);
            if (!c->active) luaL_error(L, "%s runs inside a case (Test.case(name, function(t) ... end))", function);
            return c->active;
        }

        [[noreturn]] static void endCase(lua_State* L) {
            lua_pushlightuserdata(L, &endCaseSentinel);
            lua_error(L);
            std::abort();
        }

        static void record(lua_State* L, CaseRunState* s, const std::string& kind, const std::string& message) {
            std::string file;
            int line = 0;
            callerLocation(L, file, line);
            s->fail(kind, message, file, line);
        }

        static std::string messageArg(lua_State* L, int index, const char* fallback) {
            if (lua_isnoneornil(L, index)) return fallback;
            return luaToText(L, index);
        }

        // ---- registration

        static Json optionsJson(lua_State* L, int index);

        // (no C++ object is alive where Lua may raise: Lua is built as C, its errors longjmp)
        static int testCase(lua_State* L) {
            TestChunk* c = chunk(L);
            const char* name = luaL_checkstring(L, 1);
            const int fnIndex = lua_istable(L, 2) ? 3 : 2;
            luaL_checktype(L, fnIndex, LUA_TFUNCTION);
            if (!c->loading) return luaL_error(L, "Test.case registers a case at the file's top level, not inside a case");
            TestChunk::Registered reg;
            const Json options = fnIndex == 3 ? optionsJson(L, 2) : Json::object();
            int line = 0;
            callerLocation(L, reg.spec.file, line);
            applyOptions(reg.spec, c->settings, c->configOptions, options, reg.optionError);
            reg.spec.file = c->file;
            reg.spec.name = name;
            reg.spec.id = c->file + "#" + name;
            reg.spec.line = line;
            for (TestChunk::Registered& other : c->registered) {
                if (other.spec.name == reg.spec.name) {
                    other.spec.duplicate = true;
                    reg.spec.duplicate = true;
                }
            }
            lua_pushvalue(L, fnIndex);
            reg.fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
            c->registered.push_back(std::move(reg));
            return 0;
        }

        static int testConfig(lua_State* L) {
            TestChunk* c = chunk(L);
            luaL_checktype(L, 1, LUA_TTABLE);
            if (!c->loading) return luaL_error(L, "Test.config sets the file's defaults at its top level");
            // named: a range-for over items() of the returned temporary would iterate a destroyed
            // object (the Android player crashed in it)
            const Json options = optionsJson(L, 1);
            for (const auto& [key, value] : options.items()) c->configOptions[key] = value;
            return 0;
        }

        static int setHook(lua_State* L, int& ref, const char* name) {
            TestChunk* c = chunk(L);
            luaL_checktype(L, 1, LUA_TFUNCTION);
            if (!c->loading) return luaL_error(L, "%s is set at the file's top level", name);
            if (ref != LUA_NOREF && ref != -1) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            lua_pushvalue(L, 1);
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
            return 0;
        }
        static int testSetup(lua_State* L) { return setHook(L, chunk(L)->setupRef, "Test.setup"); }
        static int testTeardown(lua_State* L) { return setHook(L, chunk(L)->teardownRef, "Test.teardown"); }

        // ---- checks (R6)

        static int testCheck(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.check");
            if (!lua_toboolean(L, 1)) record(L, s, "check", messageArg(L, 2, "check failed"));
            lua_pushboolean(L, lua_toboolean(L, 1));
            return 1;
        }

        static int testAssert(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.assert");
            if (lua_toboolean(L, 1)) {
                lua_settop(L, 1);
                return 1;
            }
            record(L, s, "assert", messageArg(L, 2, "assertion failed"));
            endCase(L);
        }

        static int testFail(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.fail");
            record(L, s, "assert", messageArg(L, 1, "failed"));
            endCase(L);
        }

        static int testEqual(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.equal");
            luaL_checkany(L, 1);
            luaL_checkany(L, 2);
            std::string path, actual, expected;
            if (deepEqual(L, 1, 2, 0, path, actual, expected)) {
                lua_pushboolean(L, 1);
                return 1;
            }
            const std::string message = messageArg(L, 3, "values differ");
            record(L, s, "equal", message + ": " + (path.empty() ? std::string() : "at " + path.substr(path[0] == '.' ? 1 : 0) + ": ") +
                                      "expected " + expected + ", got " + actual);
            lua_pushboolean(L, 0);
            return 1;
        }

        static int testNear(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.near");
            const double a = luaL_checknumber(L, 1);
            const double b = luaL_checknumber(L, 2);
            const double epsilon = luaL_optnumber(L, 3, 1e-6);
            if (std::fabs(a - b) <= epsilon) {
                lua_pushboolean(L, 1);
                return 1;
            }
            char text[160];
            std::snprintf(text, sizeof(text), "expected %.9g (+/- %.9g), got %.9g", b, epsilon, a);
            record(L, s, "equal", messageArg(L, 4, "values differ") + ": " + text);
            lua_pushboolean(L, 0);
            return 1;
        }

        static int testSkip(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.skip");
            s->skipped = true;
            s->result.skipReason = messageArg(L, 1, "skipped");
            endCase(L);
        }

        static int testExpectError(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.expectError");
            s->result.spec.expectErrors.push_back(luaL_checkstring(L, 1));
            return 0;
        }

        static int testLog(lua_State* L) {
            CaseRunState* s = activeCase(L, "Test.log");
            std::string line;
            for (int i = 1; i <= lua_gettop(L); i++) line += (i > 1 ? "\t" : "") + luaToText(L, i);
            s->result.log.push_back(line);
            return 0;
        }

        // ---- waits (R7) and captures (R10)

        static CaseRunState* waitingCase(lua_State* L, const char* function) {
            TestChunk* c = chunk(L);
            CaseRunState* s = activeCase(L, function);
            if (c->mode == Mode::Edit) luaL_error(L, "Edit Mode tests cannot wait: make it a Play Mode case");
            if (!lua_isyieldable(L) || L != s->co)
                luaL_error(L, "%s waits in the case's own function (not in a waitUntil predicate or a callback)", function);
            if (s->wait.kind != Wait::None) luaL_error(L, "%s: the case is already waiting", function);
            callerLocation(L, s->wait.file, s->wait.line);
            return s;
        }

        static int testWaitFrames(lua_State* L) {
            const lua_Integer n = luaL_checkinteger(L, 1);
            CaseRunState* s = waitingCase(L, "Test.waitFrames");
            s->wait.kind = Wait::Frames;
            s->wait.untilFrame = s->frame + std::max<lua_Integer>(n, 0);
            return lua_yield(L, 0);
        }

        static int testWaitGameSeconds(lua_State* L) {
            const double seconds = luaL_checknumber(L, 1);
            CaseRunState* s = waitingCase(L, "Test.waitGameSeconds");
            s->wait.kind = Wait::GameSeconds;
            s->wait.untilTime = s->gameTime + std::max(seconds, 0.0);
            return lua_yield(L, 0);
        }

        static int testWaitUntil(lua_State* L) {
            luaL_checktype(L, 1, LUA_TFUNCTION);
            CaseRunState* s = waitingCase(L, "Test.waitUntil");
            int64_t cap = s->result.spec.waitFrames;
            const char* message = "the condition never held";
            if (lua_istable(L, 2)) {
                lua_getfield(L, 2, "frames");
                if (lua_isinteger(L, -1)) cap = std::max<int64_t>(1, lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, 2, "gameSeconds");
                if (lua_isnumber(L, -1))
                    cap = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(lua_tonumber(L, -1) * s->result.spec.fixedFps - 1e-6)));
                lua_pop(L, 1);
                lua_getfield(L, 2, "message");
                if (lua_type(L, -1) == LUA_TSTRING) message = lua_tostring(L, -1);
            }
            s->wait.message = message;
            lua_pushvalue(L, 1);
            s->wait.predicateRef = luaL_ref(L, LUA_REGISTRYINDEX);
            s->wait.kind = Wait::Until;
            s->wait.startFrame = s->frame;
            s->wait.capFrames = cap;
            return lua_yield(L, 0);
        }

        static int testCapture(lua_State* L) {
            const char* name = luaL_checkstring(L, 1);
            TestChunk* c = chunk(L);
            if (c->mode == Mode::Edit) {
                activeCase(L, "Test.capture");
                return luaL_error(L, "Edit Mode tests cannot capture the game: make it a Play Mode case");
            }
            if (name[0] == '\0' || std::strpbrk(name, "/\\:*?\"<>|"))
                return luaL_error(L, "Test.capture: \"%s\" is not a file name (letters, digits, _ - .)", name);
            CaseRunState* s = waitingCase(L, "Test.capture");
            s->wait.kind = Wait::Capture;
            s->wait.captureName = name;
            s->wait.captureOptions = lua_istable(L, 2) ? optionsJson(L, 2) : Json::object();
            return lua_yield(L, 0);
        }

        // Test.input(events | path, [opts]) (R8): the events through the shared input interpreter,
        // resumed once they are done. No C++ object is alive where Lua raises.
        static int testInput(lua_State* L) {
            TestChunk* c = chunk(L);
            if (c->mode == Mode::Edit) {
                activeCase(L, "Test.input");
                return luaL_error(L, "Edit Mode tests cannot wait: make it a Play Mode case");
            }
            if (!lua_istable(L, 1) && !lua_isstring(L, 1)) return luaL_error(L, "Test.input takes a list of send_input events or an Input/*.input.json path");
            CaseRunState* s = waitingCase(L, "Test.input");
            char error[1024] = {0};
            {
                auto script = std::make_shared<input::ParsedScript>();
                std::string message;
                bool parsed = false;
                if (lua_type(L, 1) == LUA_TSTRING) {
                    const std::string path = lua_tostring(L, 1);
                    std::string code;
                    if (!c->readFile || !c->readFile(path, code)) message = "Test.input: no file " + path + " in the test root";
                    else if (!input::parseScript(code, *script, message)) message = "Test.input " + path + ": " + message;
                    else if (script->header.kind == input::ScriptKind::Recording)
                        message = "a recording replays from Play's start: use Test.case{replay = ...}";
                    else parsed = true;
                } else {
                    int gapFrames = 1;
                    input::Space space = input::Space::Canvas;
                    if (lua_istable(L, 2)) {
                        lua_getfield(L, 2, "gapFrames");
                        if (lua_isinteger(L, -1)) gapFrames = static_cast<int>(std::max<lua_Integer>(0, lua_tointeger(L, -1)));
                        lua_pop(L, 1);
                        lua_getfield(L, 2, "space");
                        if (lua_type(L, -1) == LUA_TSTRING) input::parseSpace(lua_tostring(L, -1), space);
                        lua_pop(L, 1);
                    }
                    const Json events = luaToJson(L, 1);
                    if (!events.is_array()) message = "Test.input: the events are a list ({ {type = \"key\", ...}, ... })";
                    else if (!input::parseEvents(events, space, gapFrames, s->result.spec.fixedFps, *script, message)) message = "Test.input " + message;
                    else parsed = true;
                }
                if (parsed) {
                    input::PlayOptions options;
                    options.actionSource = "test";
                    auto player = std::make_shared<input::ScriptPlayer>(script, input::runtimePlayerHost(), options);
                    input::ScriptRunner::start(player);
                    s->wait.kind = Wait::Input;
                    s->wait.inputPlayer = player;
                } else {
                    std::snprintf(error, sizeof(error), "%s", message.c_str());
                }
            }
            if (error[0]) return luaL_error(L, "%s", error);
            return lua_yield(L, 0);
        }

        // the test root's file for the require of loadEnvironment's Lua: its code, or nil
        static int testReadModule(lua_State* L) {
            TestChunk* c = chunk(L);
            const char* path = luaL_checkstring(L, 1);
            bool found = false;
            {
                std::string code;
                if (c->readFile && c->readFile(path, code)) {
                    lua_pushlstring(L, code.data(), code.size());
                    found = true;
                }
            }
            if (!found) lua_pushnil(L);
            return 1;
        }
    };

    Json ChunkAccess::optionsJson(lua_State* L, int index) {
        index = lua_absindex(L, index);
        Json out = Json::object();
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string key = lua_tostring(L, -2);
                switch (lua_type(L, -1)) {
                    case LUA_TSTRING: out[key] = lua_tostring(L, -1); break;
                    case LUA_TBOOLEAN: out[key] = lua_toboolean(L, -1) != 0; break;
                    case LUA_TNUMBER:
                        if (lua_isinteger(L, -1)) out[key] = static_cast<int64_t>(lua_tointeger(L, -1));
                        else out[key] = lua_tonumber(L, -1);
                        break;
                    case LUA_TTABLE: {
                        Json list = Json::array();
                        Json object = Json::object();
                        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
                        for (lua_Integer i = 1; i <= n; i++) {
                            lua_rawgeti(L, -1, i);
                            if (lua_type(L, -1) == LUA_TSTRING) list.push_back(lua_tostring(L, -1));
                            else if (lua_type(L, -1) == LUA_TNUMBER) list.push_back(lua_tonumber(L, -1));
                            lua_pop(L, 1);
                        }
                        if (n == 0) {
                            // a region {x, y, w, h} given by name
                            lua_pushnil(L);
                            while (lua_next(L, -2) != 0) {
                                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER)
                                    object[lua_tostring(L, -2)] = lua_tonumber(L, -1);
                                lua_pop(L, 1);
                            }
                        }
                        out[key] = object.empty() ? list : object;
                        break;
                    }
                    default: break;
                }
            }
            lua_pop(L, 1);
        }
        return out;
    }

}

// ================================================================== TestChunk

namespace jasperengine::gametest {

TestChunk::TestChunk(lua_State* state, Mode m, const Settings& s, const Target& t,
                     std::function<bool(const std::string&, std::string&)> reader)
    : L(state), mode(m), settings(s), target(t), readFile(std::move(reader)) {}

TestChunk::~TestChunk() {
    if (!L) return;
    for (Registered& r : registered)
        if (r.fnRef >= 0) luaL_unref(L, LUA_REGISTRYINDEX, r.fnRef);
    for (int ref : {envRef, setupRef, teardownRef})
        if (ref >= 0) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

bool TestChunk::load(const std::string& code, const std::string& path, std::string& error) {
    file = path;
    const int top = lua_gettop(L);
    // the environment: Test, a require that knows the test root, then _G
    lua_newtable(L);
    lua_newtable(L);
    const std::pair<const char*, lua_CFunction> functions[] = {
        {"case", ChunkAccess::testCase},           {"config", ChunkAccess::testConfig},
        {"setup", ChunkAccess::testSetup},         {"teardown", ChunkAccess::testTeardown},
        {"check", ChunkAccess::testCheck},         {"assert", ChunkAccess::testAssert},
        {"equal", ChunkAccess::testEqual},         {"near", ChunkAccess::testNear},
        {"fail", ChunkAccess::testFail},           {"skip", ChunkAccess::testSkip},
        {"waitFrames", ChunkAccess::testWaitFrames}, {"waitGameSeconds", ChunkAccess::testWaitGameSeconds},
        {"waitUntil", ChunkAccess::testWaitUntil}, {"capture", ChunkAccess::testCapture},
        {"expectError", ChunkAccess::testExpectError}, {"log", ChunkAccess::testLog},
        {"input", ChunkAccess::testInput}};
    for (const auto& [name, function] : functions) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, function, 1);
        lua_setfield(L, -2, name);
    }
    lua_newtable(L);
    lua_pushstring(L, target.editor ? "editor" : target.platform.c_str());
    lua_setfield(L, -2, "platform");
    lua_pushstring(L, target.backend.c_str());
    lua_setfield(L, -2, "backend");
    lua_pushboolean(L, target.editor);
    lua_setfield(L, -2, "editor");
    lua_pushstring(L, (std::to_string(target.width) + "x" + std::to_string(target.height)).c_str());
    lua_setfield(L, -2, "resolution");
    lua_setfield(L, -2, "target");
    lua_setfield(L, -2, "Test");
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    // require: the game's modules (lua://), then the test root's, loaded in this environment
    static const char* const REQUIRE =
        "local gameRequire, readModule, env = ...\n"
        "local modules = {}\n"
        "return function(name)\n"
        "  local ok, result = pcall(gameRequire, name)\n"
        "  if ok then return result end\n"
        "  if modules[name] ~= nil then return modules[name] end\n"
        "  local path = string.gsub(name, '%.', '/') .. '.lua'\n"
        "  local code = readModule(path)\n"
        "  if not code then error(tostring(result) .. '\\n\\tno file ' .. path .. ' in the test root', 2) end\n"
        "  local chunk = assert(load(code, '@' .. path, 't', env))\n"
        "  local value = chunk(name)\n"
        "  if value == nil then value = true end\n"
        "  modules[name] = value\n"
        "  return value\n"
        "end\n";
    if (luaL_loadbuffer(L, REQUIRE, std::strlen(REQUIRE), "=test-require") == LUA_OK) {
        lua_getglobal(L, "require");
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, ChunkAccess::testReadModule, 1);
        lua_pushvalue(L, -4);
        if (lua_pcall(L, 3, 1, 0) == LUA_OK) lua_setfield(L, -2, "require");
        else lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    envRef = luaL_ref(L, LUA_REGISTRYINDEX);

    if (luaL_loadbuffer(L, code.data(), code.size(), ("@" + path).c_str()) != LUA_OK) {
        error = luaToText(L, -1);
        lua_settop(L, top);
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, envRef);
    lua_setupvalue(L, -2, 1);
    loading = true;
    armInterrupt(L, 10.0);
    const int status = lua_pcall(L, 0, 0, 0);
    disarmInterrupt(L);
    loading = false;
    if (status != LUA_OK) {
        error = luaToText(L, -1);
        lua_settop(L, top);
        return false;
    }
    lua_settop(L, top);
    return true;
}

const TestChunk::Registered* TestChunk::find(const std::string& name, bool& duplicate) const {
    const Registered* found = nullptr;
    duplicate = false;
    for (const Registered& r : registered) {
        if (r.spec.name != name) continue;
        if (found) duplicate = true;
        found = &r;
    }
    return found;
}

namespace {

    // the traceback handler of runSync's calls: the end-case sentinel passes through
    int syncMessageHandler(lua_State* L) {
        if (lua_touserdata(L, 1) == &endCaseSentinel) return 1;
        const std::string message = luaToText(L, 1);
        luaL_traceback(L, L, message.c_str(), 1);
        return 1;
    }

    // the result's failure for a Lua error message (its traceback cut off; the location parsed)
    void recordLuaError(CaseRunState& s, const std::string& message) {
        std::string text = message;
        std::string file;
        int line = 0;
        parseLocation(text, file, line);
        if (interrupt.fired) s.runnerError = true;
        s.fail("error", text, file, line);
    }

}

void TestChunk::runSync(const std::string& name, CaseResult& out) {
    CaseRunState s;
    s.result = out;
    bool duplicate = false;
    const Registered* reg = find(name, duplicate);
    if (!reg || duplicate || !reg->optionError.empty()) {
        s.runnerError = true;
        s.fail("error", !reg ? "no case \"" + name + "\" in " + file : duplicate ? "duplicate case name \"" + name + "\" in " + file : reg->optionError,
               file, reg ? reg->spec.line : 0);
    } else if (!reg->spec.skip.empty()) {
        s.skipped = true;
        s.result.skipReason = reg->spec.skip;
    } else {
        s.result.spec.expectErrors = reg->spec.expectErrors;
        active = &s;
        lua_newtable(L);
        lua_pushstring(L, name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, reg->spec.id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "frame");
        lua_pushnumber(L, 0.0);
        lua_setfield(L, -2, "gameTime");
        const int t = lua_gettop(L);
        auto call = [&](int ref) {
            if (ref < 0) return true;
            lua_pushcfunction(L, syncMessageHandler);
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            lua_pushvalue(L, t);
            const bool hooked = lua_gethook(L) == nullptr;
            if (hooked) armInterrupt(L, 10.0);
            const int status = lua_pcall(L, 1, 0, -3);
            if (hooked) disarmInterrupt(L);
            if (status == LUA_OK) {
                lua_pop(L, 1);
                return true;
            }
            if (lua_touserdata(L, -1) != &endCaseSentinel) recordLuaError(s, luaToText(L, -1));
            lua_pop(L, 2);
            return false;
        };
        if (call(setupRef)) call(reg->fnRef);
        call(teardownRef);
        lua_settop(L, t - 1);
        active = nullptr;
    }
    out = s.result;
    if (s.runnerError) out.status = Status::Error;
    else if (!out.failures.empty()) out.status = Status::Failed;
    else if (s.skipped) out.status = Status::Skipped;
    else out.status = Status::Passed;
}

// ================================================================== TestRunner

TestRunner::TestRunner() = default;

TestRunner::~TestRunner() {
    if (current && current->sink) Logger::removeSink(current->sink);
}

std::string TestRunner::currentCase() const {
    return (phase == Phase::Starting || phase == Phase::Running || phase == Phase::Ending) && index < plan.size() ? plan[index].id
                                                                                                                 : std::string();
}

void TestRunner::start(std::vector<CaseSpec> cases, Host& h, RunOptions o) {
    host = &h;
    options = std::move(o);
    plan = std::move(cases);
    index = 0;
    phase = Phase::Next;
    running = true;
    cancelled = false;
    discovered.clear();
    excludedEdit = 0;
    current.reset();
    results = RunResult();
    results.run.id = options.runId.empty() ? newRunId() : options.runId;
    results.run.project = options.project;
    results.run.target = options.target;
    results.run.started = isoTimeUtc();
    results.run.filter = options.filter;
    results.run.mode = options.mode;
    runStarted = std::chrono::steady_clock::now();
    if (options.logProtocol) {
        int playCases = 0;
        for (const CaseSpec& spec : plan) playCases += spec.mode == Mode::Play ? 1 : 0;
        JE_LOG_INFO(logCategoryTest(), "%s", beginLine(playCases, results.run.id).c_str());
    }
}

void TestRunner::cancel() {
    if (!running) return;
    cancelled = true;
    if (phase == Phase::Starting || phase == Phase::Running) {
        if (current && current->replay) current->replay->stop();
        if (current && current->sink) {
            Logger::removeSink(current->sink);
            current->sink = 0;
        }
        host->endSession();
        phase = Phase::Ending;
    }
}

bool TestRunner::frame() {
    if (!running) return false;
    switch (phase) {
        case Phase::Next:
            if (cancelled || index >= plan.size()) {
                endRun();
                return false;
            }
            beginCase();
            break;
        case Phase::Starting: {
            CaseRunState& s = *current;
            if (host->wallSeconds() - s.wallStart > options.settings.hangSeconds) {
                s.hung = true;
                s.fail("hung", "the session did not start within hang_seconds (" + std::to_string(options.settings.hangSeconds) + " s)", "", 0);
                finishCase();
                break;
            }
            std::string error;
            const Host::SessionState state = host->sessionState(error);
            if (state == Host::SessionState::Starting) break;
            if (state == Host::SessionState::Failed) {
                s.runnerError = true;
                s.fail("error", error.empty() ? "the session failed to start" : error, "", 0);
                finishCase();
                break;
            }
            // Ready: the file in the session's Lua state
            lua_State* L = host->luaState();
            std::string code;
            const CaseSpec spec = s.result.spec;
            if (!L || !host->readTestFile(spec.file, code, error)) {
                s.runnerError = true;
                s.fail("error", error.empty() ? "the session has no Lua state" : error, "", 0);
                finishCase();
                break;
            }
            Host* h = host;
            s.chunk = std::make_unique<TestChunk>(L, Mode::Play, options.settings, options.target,
                                                  [h](const std::string& file, std::string& text) {
                                                      std::string ignored;
                                                      return h->readTestFile(file, text, ignored);
                                                  });
            if (!s.chunk->load(code, spec.file, error)) {
                s.runnerError = true;
                s.fail("error", error, "", 0);
                finishCase();
                break;
            }
            // cases whose names are computed join the plan once their file ran (R3)
            if (!discovered[spec.file]) {
                discovered[spec.file] = true;
                for (const TestChunk::Registered& reg : s.chunk->cases()) {
                    const bool planned = std::any_of(plan.begin(), plan.end(), [&](const CaseSpec& p) { return p.id == reg.spec.id; });
                    if (planned || reg.spec.mode != Mode::Play || options.mode == "edit" || !matchesFilter(reg.spec, options.filter)) continue;
                    plan.push_back(reg.spec);
                }
            }
            bool duplicate = false;
            const TestChunk::Registered* reg = s.chunk->find(spec.name, duplicate);
            if (!reg || duplicate || !reg->optionError.empty()) {
                s.runnerError = true;
                s.fail("error", !reg ? "the case \"" + spec.name + "\" was not registered when its file ran (a name computed differently?)"
                               : duplicate ? "duplicate case name \"" + spec.name + "\" in " + spec.file
                                           : reg->optionError,
                       "", reg ? reg->spec.line : spec.line);
                finishCase();
                break;
            }
            s.result.spec.tags = reg->spec.tags;
            s.result.spec.expectErrors = reg->spec.expectErrors;
            s.result.spec.line = reg->spec.line;
            if (!reg->spec.skip.empty()) {
                s.skipped = true;
                s.result.skipReason = reg->spec.skip;
                finishCase();
                break;
            }
            s.frameBase = host->gameFrames();
            s.timeBase = host->gameTime();
            if (s.replayScript) {
                input::ReplayOptions replayOptions;
                replayOptions.host = input::runtimePlayerHost();
                replayOptions.play.simulateTouch = s.replayScript->header.simulateTouch;
                s.replay = input::Replay::start(s.replayScript, std::move(replayOptions));
            }
            s.chunk->active = &s;
            lua_newtable(L);
            lua_pushstring(L, spec.name.c_str());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, spec.id.c_str());
            lua_setfield(L, -2, "id");
            s.tRef = luaL_ref(L, LUA_REGISTRYINDEX);
            {
                ConsoleCollector& c = collector();
                std::lock_guard<std::mutex> lock(c.mutex);
                c.lines.clear();
                c.dropped = 0;
            }
            if (Logger::isInitialized()) s.sink = Logger::addSink(std::make_unique<CaseConsoleSink>());
            s.step = CaseRunState::Step::Setup;
            phase = Phase::Running;
            runCaseFrame();
            break;
        }
        case Phase::Running:
            runCaseFrame();
            break;
        case Phase::Ending:
            if (!host->sessionEnded()) break;
            if (!cancelled && current) {
                results.cases.push_back(current->result);
                if (options.logProtocol) JE_LOG_INFO(logCategoryTest(), "%s", caseLine(current->result).c_str());
                if (onCaseDone) onCaseDone(current->result);
            }
            current.reset();
            index++;
            phase = Phase::Next;
            break;
        case Phase::Done:
            return false;
    }
    return running;
}

void TestRunner::beginCase() {
    const CaseSpec& spec = plan[index];
    if (spec.mode == Mode::Edit) {
        excludedEdit++;
        index++;
        return;
    }
    current = std::make_unique<CaseRunState>();
    CaseRunState& s = *current;
    s.host = host;
    s.result.spec = spec;
    s.interruptSeconds = options.interruptSeconds;
    s.wallStart = host->wallSeconds();
    const std::string targetName = options.target.editor ? "editor" : options.target.platform;
    auto finishedWithoutSession = [&]() {
        CaseResult r = s.result;
        if (s.runnerError) r.status = Status::Error;
        else if (s.skipped) r.status = Status::Skipped;
        results.cases.push_back(r);
        if (options.logProtocol) JE_LOG_INFO(logCategoryTest(), "%s", caseLine(r).c_str());
        if (onCaseDone) onCaseDone(r);
        current.reset();
        index++;
    };
    if (!spec.skip.empty()) {
        s.skipped = true;
        s.result.skipReason = spec.skip;
        finishedWithoutSession();
        return;
    }
    if (!spec.platforms.empty() && std::find(spec.platforms.begin(), spec.platforms.end(), targetName) == spec.platforms.end()) {
        s.skipped = true;
        std::string list;
        for (const std::string& p : spec.platforms) list += (list.empty() ? "" : ", ") + p;
        s.result.skipReason = "not on " + targetName + " (platforms: " + list + ")";
        finishedWithoutSession();
        return;
    }
    std::string error;
    if (spec.duplicate) error = "duplicate case name \"" + spec.name + "\" in " + spec.file;
    else if (spec.data == "project") error = PROJECT_DATA_REFUSED;
    if (!error.empty()) {
        s.runnerError = true;
        s.fail("error", error, "", spec.line);
        finishedWithoutSession();
        return;
    }
    if (!spec.replay.empty() && !loadReplay(s, error)) {
        s.runnerError = true;
        s.fail("error", error, "", spec.line);
        finishedWithoutSession();
        return;
    }
    if (!host->beginSession(s.result.spec, error)) {
        s.runnerError = true;
        s.fail("error", error, "", spec.line);
        finishedWithoutSession();
        return;
    }
    phase = Phase::Starting;
}

bool TestRunner::loadReplay(CaseRunState& s, std::string& error) {
    CaseSpec& spec = s.result.spec;
    std::string code;
    if (!host->readTestFile(spec.replay, code, error)) {
        error = "replay: no file " + spec.replay + " in the test root";
        return false;
    }
    auto script = std::make_shared<input::ParsedScript>();
    if (!input::parseScript(code, *script, error)) {
        error = "replay " + spec.replay + ": " + error;
        return false;
    }
    if (script->header.kind != input::ScriptKind::Recording) {
        error = "replay " + spec.replay + " is a script, not a recording: run it with Test.input";
        return false;
    }
    const input::ScriptHeader& h = script->header;
    spec.fixedFps = std::max(1, h.fixedFps);
    if (h.hasSeed) spec.seed = h.seed;
    if (!h.scene.empty()) spec.scene = h.scene;
    s.replayScript = std::move(script);
    return true;
}

namespace {

    // resumes (or starts) the step's coroutine; true when the step is over
    bool resumeStep(CaseRunState& s, lua_State* L, int nargs) {
        armInterrupt(s.co, s.interruptSeconds);
        int results = 0;
        const int status = lua_resume(s.co, L, nargs, &results);
        disarmInterrupt(s.co);
        if (status == LUA_YIELD) {
            lua_pop(s.co, results);
            return false;
        }
        if (status != LUA_OK) {
            if (lua_touserdata(s.co, -1) != &endCaseSentinel) {
                const std::string message = luaToText(s.co, -1);
                luaL_traceback(L, s.co, message.c_str(), 0);
                const std::string traced = luaToText(L, -1);
                lua_pop(L, 1);
                recordLuaError(s, traced.size() > 4000 ? traced.substr(0, 4000) : traced);
            }
            if (s.step != CaseRunState::Step::Teardown) s.bodyFailed = true;
        }
        return true;
    }

    void dropCoroutine(CaseRunState& s, lua_State* L) {
        if (s.coRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.coRef);
        s.coRef = LUA_NOREF;
        s.co = nullptr;
        if (s.wait.predicateRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.wait.predicateRef);
        if (s.wait.inputPlayer) input::ScriptRunner::stop(s.wait.inputPlayer.get());
        s.wait = Wait();
    }

    // the next step's function in a new coroutine, run to its first wait; true when it is over
    bool startStep(CaseRunState& s, lua_State* L, int fnRef) {
        dropCoroutine(s, L);
        if (fnRef < 0) return true;
        s.co = lua_newthread(L);
        s.coRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(s.co, LUA_REGISTRYINDEX, fnRef);
        lua_rawgeti(s.co, LUA_REGISTRYINDEX, s.tRef);
        return resumeStep(s, L, 1);
    }

    // the lines logged since the last look: every error, the first warnings
    void appendConsole(CaseRunState& s) {
        for (ConsoleLine& line : takeConsole()) {
            if (line.level == "error") {
                s.result.console.push_back(std::move(line));
                continue;
            }
            const auto warnings = std::count_if(s.result.console.begin(), s.result.console.end(),
                                                [](const ConsoleLine& l) { return l.level == "warning"; });
            if (warnings < static_cast<long>(MAX_WARNINGS)) s.result.console.push_back(std::move(line));
        }
    }

    void updateCaseTable(CaseRunState& s, lua_State* L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.tRef);
        lua_pushinteger(L, static_cast<lua_Integer>(s.frame));
        lua_setfield(L, -2, "frame");
        lua_pushnumber(L, s.gameTime);
        lua_setfield(L, -2, "gameTime");
        lua_pop(L, 1);
    }

}

void TestRunner::runCaseFrame() {
    CaseRunState& s = *current;
    lua_State* L = host->luaState();
    if (host->wallSeconds() - s.wallStart > options.settings.hangSeconds) {
        s.hung = true;
        s.fail("hung", "still running after hang_seconds (" + std::to_string(static_cast<int>(options.settings.hangSeconds)) +
                           " s of wall time): a hang, or a case that never reaches its end", "", 0);
        dropCoroutine(s, L);
        finishCase();
        return;
    }
    std::string sessionError;
    if (host->sessionState(sessionError) == Host::SessionState::Failed) {
        s.runnerError = true;
        s.fail("error", sessionError.empty() ? "the session ended while the case ran" : sessionError, "", 0);
        dropCoroutine(s, L);
        finishCase();
        return;
    }
    const uint64_t frames = host->gameFrames();
    s.frame = static_cast<int64_t>(frames >= s.frameBase ? frames - s.frameBase : 0);
    s.gameTime = std::max(0.0, host->gameTime() - s.timeBase);
    const bool newFrame = s.frame != s.lastFrame;
    s.lastFrame = s.frame;
    updateCaseTable(s, L);

    // the console of the frames since the last look
    appendConsole(s);

    bool stepOver = s.step == CaseRunState::Step::Done;
    if (s.co == nullptr && s.step == CaseRunState::Step::Setup) {
        stepOver = startStep(s, L, s.chunk->setupRef);
    } else if (s.co) {
        // the case's frame cap (R16): its function ends, teardown runs
        if (newFrame && s.step != CaseRunState::Step::Teardown && s.frame >= s.result.spec.timeoutFrames) {
            char text[160];
            std::snprintf(text, sizeof(text), "case timed out after %lld frames (%.1f game s)", static_cast<long long>(s.frame), s.gameTime);
            s.fail("timeout", text, s.wait.file, s.wait.line);
            s.bodyFailed = true;
            dropCoroutine(s, L);
            stepOver = true;
        } else {
            Wait& w = s.wait;
            bool resume = false;
            switch (w.kind) {
                case Wait::None: resume = newFrame; break;
                case Wait::Frames: resume = s.frame >= w.untilFrame; break;
                case Wait::GameSeconds: resume = s.gameTime + 1e-9 >= w.untilTime; break;
                case Wait::Until: {
                    if (!newFrame) break;
                    lua_State* p = lua_newthread(L);
                    const int pRef = luaL_ref(L, LUA_REGISTRYINDEX);
                    lua_rawgeti(p, LUA_REGISTRYINDEX, w.predicateRef);
                    armInterrupt(p, s.interruptSeconds);
                    const int status = lua_pcall(p, 0, 1, 0);
                    disarmInterrupt(p);
                    const bool held = status == LUA_OK && lua_toboolean(p, -1);
                    const std::string error = status == LUA_OK ? std::string() : luaToText(p, -1);
                    luaL_unref(L, LUA_REGISTRYINDEX, pRef);
                    if (status != LUA_OK) {
                        s.chunk->active = &s;
                        recordLuaError(s, "waitUntil predicate: " + error);
                        s.bodyFailed = true;
                        dropCoroutine(s, L);
                        stepOver = true;
                    } else if (held) {
                        resume = true;
                    } else if (s.frame - w.startFrame >= w.capFrames) {
                        char text[96];
                        std::snprintf(text, sizeof(text), "waitUntil timed out after %lld frames (%.1f game s): ",
                                      static_cast<long long>(w.capFrames), static_cast<double>(w.capFrames) / std::max(1, s.result.spec.fixedFps));
                        s.fail("timeout", text + w.message, w.file, w.line);
                        s.bodyFailed = true;
                        dropCoroutine(s, L);
                        stepOver = true;
                    }
                    break;
                }
                case Wait::Input: {
                    if (!w.inputPlayer || !w.inputPlayer->finished()) break;
                    if (w.inputPlayer->report().failed) {
                        s.fail("error", "Test.input: " + w.inputPlayer->errorText(), w.file, w.line);
                        s.bodyFailed = true;
                        dropCoroutine(s, L);
                        stepOver = true;
                    } else {
                        resume = true;
                    }
                    break;
                }
                case Wait::Capture: {
                    if (w.captureName.empty()) break;
                    if (!w.captureBegun) {
                        std::string error;
                        if (!host->beginCapture(s.result.spec, w.captureName, w.captureOptions, error)) {
                            s.result.captures.push_back({{"name", w.captureName}, {"status", "failed"}, {"error", error}});
                            s.fail("capture", "capture " + w.captureName + ": " + error, w.file, w.line);
                            resume = true;
                            break;
                        }
                        w.captureBegun = true;
                    }
                    Json capture;
                    if (!host->pollCapture(capture)) break;
                    const std::string status = capture.value("status", std::string("failed"));
                    if (status == "failed" || status == "new") {
                        std::string message = "capture " + w.captureName + ": " +
                                              (status == "new" ? std::string("no golden yet (accept it to make one)") : capture.value("error", std::string("differs from the golden")));
                        s.fail("capture", message, w.file, w.line);
                    }
                    s.result.captures.push_back(std::move(capture));
                    resume = true;
                    break;
                }
            }
            if (resume && !stepOver) {
                if (w.predicateRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w.predicateRef);
                s.wait = Wait();
                lua_pushinteger(s.co, static_cast<lua_Integer>(s.frame));
                stepOver = resumeStep(s, L, 1);
            }
        }
    }

    // the steps that end this frame hand over at once: setup -> body -> teardown -> done
    while (stepOver && s.step != CaseRunState::Step::Done) {
        dropCoroutine(s, L);
        if (s.step == CaseRunState::Step::Setup) {
            s.step = CaseRunState::Step::Body;
            if (s.bodyFailed || s.skipped) continue;
            bool duplicate = false;
            const TestChunk::Registered* reg = s.chunk->find(s.result.spec.name, duplicate);
            stepOver = startStep(s, L, reg ? reg->fnRef : -1);
        } else if (s.step == CaseRunState::Step::Body) {
            s.step = CaseRunState::Step::Teardown;
            stepOver = startStep(s, L, s.chunk->teardownRef);
        } else {
            s.step = CaseRunState::Step::Done;
        }
    }
    if (s.step == CaseRunState::Step::Done) finishCase();
}

void TestRunner::finishCase() {
    CaseRunState& s = *current;
    lua_State* L = host->luaState();
    if (s.sink) {
        Logger::removeSink(s.sink);
        s.sink = 0;
    }
    appendConsole(s);
    if (s.chunk) s.chunk->active = nullptr;
    if (L) {
        dropCoroutine(s, L);
        if (s.tRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.tRef);
        s.tRef = LUA_NOREF;
    }

    // R11: each expectation absorbs one error line matching its Lua pattern
    std::vector<bool> used(s.result.spec.expectErrors.size(), false);
    auto matches = [L](const std::string& line, const std::string& pattern) {
        if (!L) return line.find(pattern) != std::string::npos;
        lua_getglobal(L, "string");
        lua_getfield(L, -1, "find");
        lua_pushlstring(L, line.data(), line.size());
        lua_pushlstring(L, pattern.data(), pattern.size());
        const bool found = lua_pcall(L, 2, 1, 0) == LUA_OK && !lua_isnil(L, -1);
        lua_pop(L, 2);
        return found;
    };
    for (const ConsoleLine& line : s.result.console) {
        if (line.level != "error") continue;
        bool absorbed = false;
        for (size_t i = 0; i < used.size() && !absorbed; i++) {
            if (used[i] || !matches(line.message, s.result.spec.expectErrors[i])) continue;
            used[i] = true;
            absorbed = true;
        }
        if (!absorbed && options.settings.consoleErrorsFail) s.fail("console", line.message, line.file, line.line);
    }
    for (size_t i = 0; i < used.size(); i++)
        if (!used[i]) s.fail("expected_error_missing", "no error line matched the expected pattern \"" + s.result.spec.expectErrors[i] + "\"", "", s.result.spec.line);
    if (s.replay) reportReplay(s);

    s.result.frames = s.frame;
    s.result.gameSeconds = s.gameTime;
    s.result.wallMs = (host->wallSeconds() - s.wallStart) * 1000.0;
    if (s.hung) s.result.status = Status::Hung;
    else if (s.runnerError) s.result.status = Status::Error;
    else if (!s.result.failures.empty()) s.result.status = Status::Failed;
    else if (s.skipped) s.result.status = Status::Skipped;
    else s.result.status = Status::Passed;
    s.chunk.reset();
    host->endSession();
    phase = Phase::Ending;
}

void TestRunner::reportReplay(CaseRunState& s) {
    input::Replay& replay = *s.replay;
    const input::ReplayReport& report = replay.report();
    const std::string& path = s.result.spec.replay;
    if (report.divergedAtFrame >= 0) {
        // the frame and what differs: each probe's recorded and replayed position, each value
        std::string what;
        for (const Json& d : report.differences) {
            if (what.size() > 600) break;
            what += what.empty() ? "" : "; ";
            what += d.contains("probe") ? d["probe"].get<std::string>() + " " + d["recorded"].dump() + " -> " + d["replayed"].dump()
                                        : d.value("value", std::string("?")) + " " + d["recorded"].dump() + " -> " + d["replayed"].dump();
        }
        if (what.empty()) what = "the state hash";
        const std::string message = "replay " + path + " diverged at frame " + std::to_string(report.divergedAtFrame) + ": " + what;
        if (options.settings.divergenceFail) s.fail("divergence", message, path, 0);
        else s.result.console.push_back({"warning", message, path, 0});
    } else if (report.failed) {
        s.fail("error", "replay " + path + ": " + replay.errorText(), path, 0);
    } else if (!report.comparable && !replay.script().header.checkpoints.empty()) {
        s.result.console.push_back({"warning", "replay " + path + ": checkpoints not compared (recorded on " + replay.script().header.platform +
                                                   (replay.script().header.fromPlayStart ? "" : ", not from Play's start") + ")",
                                    path, 0});
    }
    if (!replay.isDone()) replay.stop();
    s.replay.reset();
}

void TestRunner::endRun() {
    results.counts = countResults(results.cases, excludedEdit);
    results.run.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStarted).count();
    if (options.logProtocol) JE_LOG_INFO(logCategoryTest(), "%s", summaryLine(results.counts).c_str());
    running = false;
    phase = Phase::Done;
}

}  // namespace jasperengine::gametest
