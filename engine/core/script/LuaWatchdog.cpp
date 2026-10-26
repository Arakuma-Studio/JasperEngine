// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaWatchdog.h"

#include "LuaBinding.h"
#include "diag/HangWatchdog.h"
#include "lua.hpp"

#include <atomic>
#include <cstdio>

using namespace jasperengine;

namespace {

    std::atomic<uint64_t> interruptStalledMs{0};
    std::atomic<bool> interruptRaised{false};

    // "player.lua:12 update"
    void describe(lua_State* L, char* out, size_t capacity) {
        out[0] = '\0';
        lua_Debug info{};
        if (!lua_getstack(L, 0, &info) || !lua_getinfo(L, "Sln", &info)) return;
        std::snprintf(out, capacity, "%s:%d %s", info.short_src, info.currentline, info.name ? info.name : "?");
    }

    void locateHook(lua_State* L, lua_Debug*) {
        lua_sethook(L, nullptr, 0, 0);
        char where[128];
        describe(L, where, sizeof(where));
        diag::watchdog::setContext(diag::ContextSlot::Lua, where[0] ? where : "unknown function");
    }

    void interruptHook(lua_State* L, lua_Debug*) {
        lua_sethook(L, nullptr, 0, 0);
        char where[128];
        describe(L, where, sizeof(where));
        interruptRaised.store(true, std::memory_order_release);
        // lua_pushfstring knows no precision: the text is made here
        char message[256];
        std::snprintf(message, sizeof(message), "script interrupted: %s ran for %.1f s without returning (hang watchdog)",
                      where[0] ? where : "a function", static_cast<double>(interruptStalledMs.load()) / 1000.0);
        luaL_error(L, "%s", message);
    }

    // Only when no one else hooks the state; ours replace each other
    bool canHook(lua_State* L) {
        const lua_Hook hook = lua_gethook(L);
        return hook == nullptr || hook == locateHook || hook == interruptHook;
    }

}

void LuaWatchdog::attach() {
    diag::watchdog::LuaHooks hooks;
    hooks.locate = []() {
        lua_State* L = LuaBinding::getLuaState();
        if (L && canHook(L)) lua_sethook(L, locateHook, LUA_MASKCOUNT, 1);
    };
    hooks.interrupt = [](uint64_t stalledMs) {
        lua_State* L = LuaBinding::getLuaState();
        if (!L || !canHook(L)) return;
        interruptStalledMs.store(stalledMs);
        lua_sethook(L, interruptHook, LUA_MASKCOUNT, 1);
    };
    diag::watchdog::setLuaHooks(std::move(hooks));
}

bool LuaWatchdog::takeInterrupt() {
    return interruptRaised.exchange(false, std::memory_order_acq_rel);
}
