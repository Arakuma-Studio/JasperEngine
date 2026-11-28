// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Without the dialogue module (docs/specs/dialogue-graphs.md R15, AC14) the Lua names Dialogue and
// DialogueRunner exist: any use logs once that the module is not compiled and gives nil. The stubs
// are bound in every build, so this runs in the full one too (on a Lua state of its own).

#include "Log.h"
#include "script/LuaBinding.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace DialogueStubTest_cpp {

using namespace jasperengine;

TEST(DialogueStubTest, LogsOnceWhenNotCompiled) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    luaL_openlibs(L);
    LuaBinding::registerDialogueStubs(L);
    std::vector<std::string> warnings;
    const LogListenerId listener = Log::addListener([&](const LogEntry& entry) {
        if (entry.level == LogLevel::Warn && entry.message.find("not compiled") != std::string::npos) warnings.push_back(entry.message);
    });
    const char* code =
        "assert(Dialogue ~= nil and DialogueRunner ~= nil)\n"
        "assert(Dialogue.onAction('quest', function() end) == nil)\n"
        "assert(Dialogue.variables.get('gold') == nil)\n"
        "assert(DialogueRunner(nil, 1) == nil)\n"
        "Dialogue.variables.onChanged = function() end\n";
    const bool ok = luaL_loadstring(L, code) == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK;
    EXPECT_TRUE(ok) << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
    Log::removeListener(listener);
    lua_close(L);
    EXPECT_EQ(warnings.size(), 1u) << "once, whatever the uses";
    if (!warnings.empty()) {
        EXPECT_NE(warnings[0].find("JASPERENGINE_DIALOGUE"), std::string::npos);
    }
}

}  // namespace DialogueStubTest_cpp
