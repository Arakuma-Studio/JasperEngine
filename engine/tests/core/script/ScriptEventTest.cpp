// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Event and CancellableEvent (engine/core/script/ScriptEvent.cpp,
// docs/specs/cancellable-prioritised-events.md AC1-AC10): listener priorities, the veto the
// emitter reads, the monitor pass, the errors that name the fix, nesting, re-entrancy without a
// snapshot copy, and the inspection members.

#include "script/LuaBinding.h"
#include "LogCapture.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>

namespace ScriptEventTest_cpp {

using namespace jasperengine;
using jasperengine::test::LogCapture;

namespace {

    const char* const PRIORITY_ERROR = "priority must be an integer from -1000000 to 1000000 or Event.MONITOR";

    class ScriptEventTest : public ::testing::Test {
    protected:
        lua_State* L = nullptr;

        void SetUp() override {
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            ASSERT_EQ(lua("log = {}"), "nil");
        }

        void TearDown() override {
            lua("log = nil; ev = nil; stored = nil; collectgarbage()");
        }

        // the first result as tostring gives it, or "error: <message>" without the position Lua
        // puts first ("ScriptEventTest:<line>: "); chunk name ScriptEventTest
        std::string lua(const std::string& code) {
            if (luaL_loadbuffer(L, code.data(), code.size(), "=ScriptEventTest") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
                std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
                lua_pop(L, 1);
                const std::string where = "ScriptEventTest:";
                const size_t colon = message.rfind(where, 0) == 0 ? message.find(": ", where.size()) : std::string::npos;
                return "error: " + (colon == std::string::npos ? message : message.substr(colon + 2));
            }
            std::string result = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 2);
            return result;
        }

        // the `log` entries, space separated, then empties it
        std::string takeLog() {
            return lua("local s = table.concat(log, ' '); log = {}; return s");
        }
    };

}

// AC1 (R1, R2, R5): highest first, ties in add order, MONITOR last
TEST_F(ScriptEventTest, PriorityOrder) {
    lua("ev = Event()\n"
        "local function add(name, priority) ev:addListener(function() log[#log + 1] = name end, priority) end\n"
        "add('a0', 0); add('b10', 10); add('c-5', -5); add('d10', 10); add('eMonitor', Event.MONITOR); add('f0', nil)\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "b10 d10 a0 f0 c-5 eMonitor");
    EXPECT_EQ(lua("return Event.MONITOR"), "-2147483648");

    // methods and RegisterEvent take the same priorities; every priority 0 is add order
    lua("ev = Event()\n"
        "local owner = {}\n"
        "function owner:m1() log[#log + 1] = 'm1' end\n"
        "function owner:m2() log[#log + 1] = 'm2' end\n"
        "function owner:m3() log[#log + 1] = 'm3' end\n"
        "ev:addListener(owner, 'm1')\n"
        "RegisterEvent(owner, ev, 'm2', nil, 20)\n"
        "ev:addListener(owner, 'm3', 5)\n"
        "ev:add('t', function() log[#log + 1] = 't' end, 7)\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "m2 t m3 m1");
    lua("ev = Event()\n"
        "for i = 1, 4 do ev:addListener(function() log[#log + 1] = i end) end\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "1 2 3 4");
}

// AC2 (R1, R4)
TEST_F(ScriptEventTest, PriorityArgumentErrors) {
    const std::string addListener = std::string("error: Event:addListener: ") + PRIORITY_ERROR;
    EXPECT_EQ(lua("Event():addListener(function() end, 1.5)"), addListener);
    EXPECT_EQ(lua("Event():addListener(function() end, 'high')"), addListener);
    EXPECT_EQ(lua("Event():addListener(function() end, 2^40)"), addListener);
    EXPECT_EQ(lua("Event():addListener(function() end, 1000001)"), addListener);
    EXPECT_EQ(lua("Event():addListener({m = function() end}, 'm', 1.5)"), addListener);
    EXPECT_EQ(lua("CancellableEvent():addListener(function() end, -1000001)"),
              std::string("error: CancellableEvent:addListener: ") + PRIORITY_ERROR);
    EXPECT_EQ(lua("Event():add('t', function() end, '1')"), std::string("error: Event:add: ") + PRIORITY_ERROR);
    EXPECT_EQ(lua("local s = {m = function() end}; RegisterEvent(s, Event(), 'm', nil, 1.5)"),
              std::string("error: RegisterEvent: ") + PRIORITY_ERROR);
    EXPECT_EQ(lua("local s = {m = function() end}; RegisterEvent(s, Engine.onUpdate, 'm', nil, 3)"),
              "error: RegisterEvent: priority is for Event and CancellableEvent; engine events run in subscription order");
    // the limits themselves are priorities
    EXPECT_EQ(lua("local e = Event(); e:addListener(function() end, 1000000); e:addListener(function() end, -1000000); "
                  "e:addListener(function() end, 2.0); return e.listenerCount"), "3");
}

// AC3 (R3, R35)
TEST_F(ScriptEventTest, ReAddKeepsTodaysRules) {
    lua("ev = Event()\n"
        "local f = function() log[#log + 1] = 'f' end\n"
        "ev:addListener(function() log[#log + 1] = 'h' end)\n"
        "ev:addListener(f, 0)\n"
        "ev:addListener(f, 9)\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "h f") << "a function added again keeps its first priority";
    EXPECT_EQ(lua("return ev.listenerCount"), "2");

    lua("ev = Event()\n"
        "ev:add('t', function() log[#log + 1] = 'old' end)\n"
        "ev:addListener(function() log[#log + 1] = 'zero' end)\n"
        "ev:add('t', function() log[#log + 1] = 'new' end, 9)\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "new zero") << "a tag added again replaces its listener with the new priority";

    // a number in position 4 is still a tag
    lua("ev = Event()\n"
        "local s = {}\n"
        "function s:m() log[#log + 1] = 'm' end\n"
        "RegisterEvent(s, ev, 'm', 7)\n"
        "ev:invoke()");
    EXPECT_EQ(takeLog(), "m");
    EXPECT_EQ(lua("return ev:has('7')"), "true");
    EXPECT_EQ(lua("return Event():invoke()"), "nil") << "Event:invoke returns nothing";
}

// AC4 (R6-R9)
TEST_F(ScriptEventTest, CancellableVeto) {
    lua("ev = CancellableEvent()\n"
        "ev:addListener(function(e, v) log[#log + 1] = 'p10:' .. v; if v == 1 then e:cancel() end end, 10)\n"
        "ev:addListener(function(e, v) log[#log + 1] = 'p5:' .. v end, 5)\n"
        "ev:addListener(function(e, v) log[#log + 1] = 'monitor:' .. v .. ':' .. tostring(e.cancelled) end, Event.MONITOR)");
    EXPECT_EQ(lua("return ev:invoke(1)"), "true");
    EXPECT_EQ(takeLog(), "p10:1 monitor:1:true") << "the decider after the veto is skipped";
    EXPECT_EQ(lua("return ev:invoke(2)"), "false");
    EXPECT_EQ(takeLog(), "p10:2 p5:2 monitor:2:false");
    EXPECT_EQ(lua("return CancellableEvent():invoke()"), "false");
    EXPECT_EQ(lua("local c = CancellableEvent(); local s = {}; "
                  "function s:check(e, v) if v then e:cancel() end end; "
                  "RegisterEvent(s, c, 'check', nil, 1); return tostring(c:invoke(true)) .. tostring(c:invoke(false))"),
              "truefalse") << "RegisterEvent's method gets the CancelEventArgs first";
}

// AC5 (R8)
TEST_F(ScriptEventTest, VetoIsSticky) {
    lua("ev = CancellableEvent()\n"
        "ev:addListener(function(e) e:cancel(); e:cancel() end, 10)\n"
        "ev:addListener(function(e) log[#log + 1] = 'first:' .. tostring(e.cancelled) end, Event.MONITOR)\n"
        "ev:addListener(function(e)\n"
        "    local ok, err = pcall(function() e.cancelled = false end)\n"
        "    log[#log + 1] = tostring(ok) .. ':' .. tostring(string.find(err, 'read-only', 1, true) ~= nil)\n"
        "    log[#log + 1] = 'second:' .. tostring(e.cancelled)\n"
        "end, Event.MONITOR)");
    EXPECT_EQ(lua("return ev:invoke()"), "true");
    EXPECT_EQ(takeLog(), "first:true false:true second:true");
}

// AC6 (R10, R11)
TEST_F(ScriptEventTest, CancelErrorsNameTheFix) {
    const char* const monitorError = "CancelEventArgs:cancel: a monitor listener runs after the decision and cannot cancel; "
                                     "give it a priority instead";
    const char* const endedError = "CancelEventArgs:cancel: the invoke it belongs to has ended; cancel inside the listener";
    const char* const plainError = "Event:cancel: this Event cannot be cancelled; make it with CancellableEvent() and cancel "
                                   "through the listener's first argument";
    LogCapture log;
    EXPECT_EQ(lua("local c = CancellableEvent(); c:addListener(function(e) e:cancel() end, Event.MONITOR); return c:invoke()"),
              "false");
    EXPECT_EQ(lua("ev = CancellableEvent(); ev:addListener(function(e) stored = e end); return ev:invoke()"), "false");
    EXPECT_EQ(lua("local other = Event(); other:addListener(function() stored:cancel() end); other:invoke(); "
                  "return tostring(ev.cancelledBy)"), "nil");
    lua("local other = Event(); other:addListener(function() Event():cancel() end); other:invoke()");
    EXPECT_EQ(log.count(D_LOG_ERROR, monitorError), 1u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, endedError), 1u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, plainError), 1u) << log.describeErrors();
    EXPECT_EQ(log.errors().size(), 3u) << log.describeErrors();
}

// AC7 (R11)
TEST_F(ScriptEventTest, ListenerErrorKeepsOthersAndCancel) {
    LogCapture log;
    lua("ev = CancellableEvent()\n"
        "ev:addListener(function(e) e:cancel(); error('boom7') end, 10)\n"
        "ev:addListener(function(e) log[#log + 1] = 'monitor:' .. tostring(e.cancelled) end, Event.MONITOR)");
    EXPECT_EQ(lua("return ev:invoke()"), "true");
    EXPECT_EQ(takeLog(), "monitor:true");
    ASSERT_EQ(log.count(D_LOG_ERROR, "boom7"), 1u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, "stack traceback"), 1u) << log.describeErrors();
}

// AC8 (R12)
TEST_F(ScriptEventTest, NestedInvokeHasItsOwnArgs) {
    lua("ev = CancellableEvent()\n"
        "ev:addListener(function(e, v)\n"
        "    if v == 'outer' then\n"
        "        log[#log + 1] = 'inner:' .. tostring(ev:invoke('inner'))\n"
        "        log[#log + 1] = 'outerCancelled:' .. tostring(e.cancelled)\n"
        "    end\n"
        "end, 10)\n"
        "ev:addListener(function(e, v) if v == 'inner' then e:cancel() end end, 5)");
    EXPECT_EQ(lua("return ev:invoke('outer')"), "false");
    EXPECT_EQ(takeLog(), "inner:true outerCancelled:false");
    EXPECT_EQ(lua("return tostring(ev.cancelledBy)"), "nil") << "the outer invoke completed last and went ahead";
}

// AC9 (R13): both kinds, without a snapshot copy
TEST_F(ScriptEventTest, ReentrancyWithoutSnapshot) {
    for (const char* constructor : {"Event", "CancellableEvent"}) {
        lua(std::string("make = ") + constructor);
        lua("ev = make()\n"
            "local late = function() log[#log + 1] = 'late' end\n"
            "local second = function() log[#log + 1] = 'second' end\n"
            "ev:addListener(function() log[#log + 1] = 'first'; ev:addListener(late); ev:removeListener(second) end)\n"
            "ev:addListener(second)\n"
            "local _ = ev:invoke(); _ = ev:invoke()");
        EXPECT_EQ(takeLog(), "first first late") << constructor;
        lua("ev = make()\n"
            "ev:addListener(function() log[#log + 1] = 'a'; ev:removeAllListeners() end)\n"
            "ev:addListener(function() log[#log + 1] = 'b' end)\n"
            "local _ = ev:invoke(); _ = ev:invoke()\n"
            "log[#log + 1] = ev.listenerCount");
        EXPECT_EQ(takeLog(), "a 0") << constructor;
        // one added meanwhile with a priority runs at its place from the next invoke
        lua("ev = make()\n"
            "local high = function() log[#log + 1] = 'high' end\n"
            "ev:addListener(function() log[#log + 1] = 'low'; ev:addListener(high, 10); log[#log + 1] = ev.listenerCount end)\n"
            "local _ = ev:invoke(); _ = ev:invoke()");
        EXPECT_EQ(takeLog(), "low 2 high low 2") << constructor;
        // the running listener removing itself stays callable until it returns
        lua("ev = make()\n"
            "local selfRemoving\n"
            "selfRemoving = function() ev:removeListener(selfRemoving); collectgarbage(); log[#log + 1] = 'still' end\n"
            "ev:addListener(selfRemoving)\n"
            "local _ = ev:invoke(); _ = ev:invoke()");
        EXPECT_EQ(takeLog(), "still") << constructor;
    }
    lua("make = nil");
}

// AC10 (R15-R17)
TEST_F(ScriptEventTest, InspectionNames) {
    EXPECT_EQ(lua("return tostring(Event())"), "Event(0 listeners)");
    lua("ev = CancellableEvent()\n"
        "lock = {__name = 'Lock', locked = true}\n"
        "function lock:check(e) if self.locked then e:cancel() end end\n"
        "door = {__name = 'Door'}\n"
        "function door:report(e) end\n"
        "ev:addListener(lock, 'check', 10)\n"
        "RegisterEvent(door, ev, 'report', nil, Event.MONITOR)\n"
        "ev:add('mytag', function() end, 5)\n"
        "ev:addListener(function() end)\n");
    EXPECT_EQ(lua("return tostring(ev)"), "CancellableEvent(4 listeners, not invoked yet)");
    EXPECT_EQ(lua("local out = {}\n"
                  "for _, l in ipairs(ev:getListeners()) do out[#out + 1] = l.name .. '|' .. l.priority .. '|' .. tostring(l.monitor) end\n"
                  "return table.concat(out, ' ')"),
              "Lock:check|10|false mytag|5|false function ScriptEventTest:9|0|false Door:report|-2147483648|true");
    EXPECT_EQ(lua("return tostring(ev.cancelledBy)"), "nil") << "before the first invoke";
    EXPECT_EQ(lua("return ev:invoke()"), "true");
    EXPECT_EQ(lua("return ev.cancelledBy"), "Lock:check");
    EXPECT_EQ(lua("return tostring(ev)"), "CancellableEvent(4 listeners, last invoke cancelled by Lock:check)");
    EXPECT_EQ(lua("lock.locked = false; local r = ev:invoke(); return tostring(r) .. ' ' .. tostring(ev.cancelledBy)"), "false nil");
    EXPECT_EQ(lua("return tostring(ev)"), "CancellableEvent(4 listeners, last invoke went ahead)");
    EXPECT_EQ(lua("local e = Event(); e:addListener(function() end); return tostring(e)"), "Event(1 listeners)");
    lua("lock = nil; door = nil");
}

}  // namespace ScriptEventTest_cpp
