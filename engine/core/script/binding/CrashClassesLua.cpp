// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Crash table (spec S28 R32, docs/crash-diagnostics.md, docs/mcp/guide/lua-api.md): the
// reports earlier sessions left (crashes, hangs, abnormal exits), the hook a game gives them to
// at the next start, and tags written into crash.txt. Bound with the Lua C API, so its stub is
// hand-written (stubs/Crash.lua). The engine never sends a report: the hook decides what to do.

#include "LuaBinding.h"
#include "lua.hpp"

#include "Log.h"
#include "diag/CrashHandler.h"

#include <string>

using namespace jasperengine;

namespace {

    void pushReport(lua_State* L, const diag::crash::ReportInfo& report) {
        lua_createtable(L, 0, 8);
        lua_pushstring(L, report.id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, report.kind.c_str());
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, report.reason.c_str());
        lua_setfield(L, -2, "reason");
        lua_pushstring(L, report.time.c_str());
        lua_setfield(L, -2, "time");
        lua_pushstring(L, report.version.c_str());
        lua_setfield(L, -2, "version");
        lua_pushstring(L, report.product.c_str());
        lua_setfield(L, -2, "product");
        lua_pushboolean(L, report.handled);
        lua_setfield(L, -2, "handled");
        lua_createtable(L, static_cast<int>(report.files.size()), 0);
        for (size_t i = 0; i < report.files.size(); i++) {
            lua_pushstring(L, report.files[i].string().c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(L, -2, "files");
    }

    // Crash.setReportHook(fn): fn(report) for each pending report, now; true marks it handled
    int luaSetReportHook(lua_State* L) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        diag::crash::setReportHook([L, ref](const diag::crash::ReportInfo& report) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            pushReport(L, report);
            if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK) {
                Log::error("Crash.setReportHook: the hook failed: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
                return false;
            }
            const bool handled = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return handled;
        });
        // every pending report was offered: the hook keeps no Lua reference past this call
        diag::crash::setReportHook(nullptr);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return 0;
    }

    int luaGetPendingReports(lua_State* L) {
        const std::vector<diag::crash::ReportInfo> reports = diag::crash::pendingReports();
        lua_createtable(L, static_cast<int>(reports.size()), 0);
        for (size_t i = 0; i < reports.size(); i++) {
            pushReport(L, reports[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int luaMarkHandled(lua_State* L) {
        lua_pushboolean(L, diag::crash::markHandled(luaL_checkstring(L, 1)));
        return 1;
    }

    int luaLastSessionEndedAbnormally(lua_State* L) {
        lua_pushboolean(L, diag::crash::lastSessionEndedAbnormally());
        return 1;
    }

    int luaSetTag(lua_State* L) {
        const char* key = luaL_checkstring(L, 1);
        const char* value = luaL_optstring(L, 2, "");
        diag::crash::setTag(key, value);
        return 0;
    }

}

void LuaBinding::registerCrashClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    static const luaL_Reg functions[] = {
        {"setReportHook", luaSetReportHook},
        {"getPendingReports", luaGetPendingReports},
        {"markHandled", luaMarkHandled},
        {"lastSessionEndedAbnormally", luaLastSessionEndedAbnormally},
        {"setTag", luaSetTag},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    lua_setglobal(L, "Crash");
#endif //DISABLE_LUA_BINDINGS
}
