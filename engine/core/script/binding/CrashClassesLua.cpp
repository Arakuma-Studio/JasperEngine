// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's Crash table (spec S28 R32, docs/crash-diagnostics.md, docs/mcp/guide/lua-api.md): the
// reports earlier sessions left (crashes, hangs, abnormal exits), the hook a game gives them to
// at the next start, tags written into crash.txt, and a player's "Report a Problem" (user
// reports, docs/specs/shipped-game-diagnostics.md P2). And the Journal table: the Gameplay
// Journal's events (P1). Bound with the Lua C API, so their stubs are hand-written
// (stubs/Crash.lua, stubs/Journal.lua). The engine never sends a report: the game decides.

#include "LuaBinding.h"
#include "lua.hpp"

#include "Log.h"
#include "diag/CrashHandler.h"
#include "diag/Journal.h"
#include "diag/UserReport.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
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
        if (!report.category.empty()) {
            lua_pushstring(L, report.category.c_str());
            lua_setfield(L, -2, "category");
        }
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

    // ---- user reports ("Report a Problem")

    // A Lua function kept in the registry until its one call (the state outlives it: Play stop
    // drops the callbacks first, diag::userreport::reset)
    std::shared_ptr<int> keepFunction(lua_State* L, int index) {
        lua_pushvalue(L, index);
        return std::make_shared<int>(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    // Crash.beginReport([fn [, {maxWidth, maxBytes}]]): captures the frame; fn(ok, err) the
    // first frame after
    int luaBeginReport(lua_State* L) {
        diag::userreport::Callback callback;
        if (!lua_isnoneornil(L, 1)) {
            luaL_checktype(L, 1, LUA_TFUNCTION);
            std::shared_ptr<int> ref = keepFunction(L, 1);
            callback = [L, ref](bool ok, const std::string& error) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
                luaL_unref(L, LUA_REGISTRYINDEX, *ref);
                lua_pushboolean(L, ok);
                if (error.empty()) lua_pushnil(L);
                else lua_pushstring(L, error.c_str());
                if (LuaBinding::pcallWithTraceback(L, 2, 0) != LUA_OK) {
                    Log::error("Crash.beginReport: the callback failed: %s", lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            };
        }
        int maxWidth = diag::userreport::kDefaultMaxWidth;
        size_t maxBytes = diag::userreport::kDefaultMaxBytes;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "maxWidth");
            if (lua_isnumber(L, -1)) maxWidth = static_cast<int>(lua_tointeger(L, -1));
            lua_getfield(L, 2, "maxBytes");
            if (lua_isnumber(L, -1)) maxBytes = static_cast<size_t>(lua_tointeger(L, -1));
            lua_pop(L, 2);
        }
        diag::userreport::begin(std::move(callback), maxWidth, maxBytes);
        return 0;
    }

    int luaCancelReport(lua_State*) {
        diag::userreport::cancel();
        return 0;
    }

    // A scalar as user.txt's field text
    std::string fieldText(lua_State* L, int index) {
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return lua_toboolean(L, index) ? "true" : "false";
            case LUA_TNUMBER: {
                if (lua_isinteger(L, index)) return std::to_string(lua_tointeger(L, index));
                char text[40];
                std::snprintf(text, sizeof(text), "%.9g", lua_tonumber(L, index));
                return text;
            }
            case LUA_TSTRING: {
                size_t size = 0;
                const char* text = lua_tolstring(L, index, &size);
                return std::string(text, size);
            }
            default: return std::string("<") + luaL_typename(L, index) + ">";
        }
    }

    // Crash.createReport{note, category, fields, screenshot, logTail, done}: the report table,
    // or nil and the reason; with done, nothing now and done(report, err) after the write
    int luaCreateReport(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        diag::userreport::Options options;
        lua_getfield(L, 1, "note");
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t size = 0;
            const char* note = lua_tolstring(L, -1, &size);
            options.note.assign(note, size);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "category");
        if (lua_type(L, -1) == LUA_TSTRING) options.category = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 1, "fields");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) options.fields.emplace_back(lua_tostring(L, -2), fieldText(L, -1));
                lua_pop(L, 1);
            }
            // user.txt's lines in one order whatever the table's
            std::sort(options.fields.begin(), options.fields.end());
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "screenshot");
        if (lua_isboolean(L, -1)) options.screenshot = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 1, "logTail");
        if (lua_isboolean(L, -1)) options.logTail = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "done");
        if (lua_isfunction(L, -1)) {
            std::shared_ptr<int> ref = keepFunction(L, -1);
            lua_pop(L, 1);
            diag::userreport::createAsync(options, [L, ref](const std::optional<diag::crash::ReportInfo>& report, const std::string& error) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
                luaL_unref(L, LUA_REGISTRYINDEX, *ref);
                if (report) pushReport(L, *report);
                else lua_pushnil(L);
                if (report) lua_pushnil(L);
                else lua_pushstring(L, error.c_str());
                if (LuaBinding::pcallWithTraceback(L, 2, 0) != LUA_OK) {
                    Log::error("Crash.createReport: done failed: %s", lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            });
            return 0;
        }
        lua_pop(L, 1);

        std::string error;
        const std::optional<diag::crash::ReportInfo> report = diag::userreport::create(options, error);
        if (!report) {
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        pushReport(L, *report);
        return 1;
    }

    // Crash.readReportFile(id, name): the file's bytes (an Http body), or nil, "not_found"
    int luaReadReportFile(lua_State* L) {
        size_t idSize = 0, nameSize = 0;
        const char* id = luaL_checklstring(L, 1, &idSize);
        const char* name = luaL_checklstring(L, 2, &nameSize);
        const std::optional<std::string> bytes = diag::crash::readReportFile(std::string_view(id, idSize), std::string_view(name, nameSize));
        if (!bytes) {
            lua_pushnil(L);
            lua_pushliteral(L, "not_found");
            return 2;
        }
        lua_pushlstring(L, bytes->data(), bytes->size());
        return 1;
    }

    // ---- Journal

    const char* typeText(int type) {
        switch (type) {
            case LUA_TNIL: return "<nil>";
            case LUA_TTABLE: return "<table>";
            case LUA_TFUNCTION: return "<function>";
            case LUA_TUSERDATA: return "<userdata>";
            case LUA_TLIGHTUSERDATA: return "<lightuserdata>";
            case LUA_TTHREAD: return "<thread>";
            default: return "<value>";
        }
    }

    // Journal.log(kind [, fields]): views of the table's strings, no copy and no allocation (a
    // number is read as a number, never converted in place)
    int luaJournalLog(lua_State* L) {
        size_t kindSize = 0;
        const char* kind = luaL_checklstring(L, 1, &kindSize);
        if (!diag::journal::enabled()) return 0;
        // a few past the limit: the journal keeps the 12 smallest keys
        constexpr int kSlots = 32;
        diag::journal::Field fields[kSlots];
        int count = 0;
        if (lua_istable(L, 2)) {
            lua_pushnil(L);
            while (lua_next(L, 2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && count < kSlots) {
                    size_t keySize = 0;
                    const char* key = lua_tolstring(L, -2, &keySize);
                    diag::journal::Field& field = fields[count++];
                    field.key = std::string_view(key, keySize);
                    const int type = lua_type(L, -1);
                    if (type == LUA_TSTRING) {
                        size_t size = 0;
                        const char* text = lua_tolstring(L, -1, &size);
                        field.value = std::string_view(text, size);
                    } else if (type == LUA_TNUMBER) {
                        if (lua_isinteger(L, -1)) field.value = static_cast<int64_t>(lua_tointeger(L, -1));
                        else field.value = static_cast<double>(lua_tonumber(L, -1));
                    } else if (type == LUA_TBOOLEAN) {
                        field.value = lua_toboolean(L, -1) != 0;
                    } else {
                        field.value = std::string_view(typeText(type));
                        field.verbatim = true;
                    }
                }
                // the key's and the value's strings stay alive in the table while the views are used
                lua_pop(L, 1);
            }
        }
        diag::journal::log(std::string_view(kind, kindSize), std::span<const diag::journal::Field>(fields, static_cast<size_t>(count)));
        return 0;
    }

    int luaJournalGetText(lua_State* L) {
        const std::string text = diag::journal::text();
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    // Journal.getEntries([n]): the newest n (50), oldest first, as {time, frame, kind, text}
    int luaJournalGetEntries(lua_State* L) {
        const lua_Integer wanted = luaL_optinteger(L, 1, 50);
        const std::vector<std::string> lines = diag::journal::entries(wanted > 0 ? static_cast<size_t>(wanted) : 0);
        lua_createtable(L, static_cast<int>(lines.size()), 0);
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& line = lines[i];
            // "+412.031 f24718 level.start ..."
            const size_t frameAt = line.find(" f");
            const size_t kindAt = frameAt == std::string::npos ? std::string::npos : line.find(' ', frameAt + 2);
            const size_t kindEnd = kindAt == std::string::npos ? std::string::npos : line.find(' ', kindAt + 1);
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, std::strtod(line.c_str() + 1, nullptr));
            lua_setfield(L, -2, "time");
            lua_pushinteger(L, frameAt == std::string::npos ? 0 : static_cast<lua_Integer>(std::strtoull(line.c_str() + frameAt + 2, nullptr, 10)));
            lua_setfield(L, -2, "frame");
            const std::string kind = kindAt == std::string::npos ? std::string() : line.substr(kindAt + 1, kindEnd == std::string::npos ? std::string::npos : kindEnd - kindAt - 1);
            lua_pushlstring(L, kind.data(), kind.size());
            lua_setfield(L, -2, "kind");
            lua_pushlstring(L, line.data(), line.size());
            lua_setfield(L, -2, "text");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int luaJournalClear(lua_State*) {
        diag::journal::clear();
        return 0;
    }

    // Journal.enabled: read-only, false while Gameplay Journal (KiB) is 0
    int luaJournalIndex(lua_State* L) {
        const char* key = lua_tostring(L, 2);
        if (key && std::strcmp(key, "enabled") == 0) {
            lua_pushboolean(L, diag::journal::enabled());
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    int luaJournalNewIndex(lua_State* L) {
        const char* key = lua_tostring(L, 2);
        if (key && std::strcmp(key, "enabled") == 0) return luaL_error(L, "Journal.enabled is read-only");
        lua_rawset(L, 1);
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
        {"beginReport", luaBeginReport},
        {"createReport", luaCreateReport},
        {"cancelReport", luaCancelReport},
        {"readReportFile", luaReadReportFile},
        {nullptr, nullptr},
    };
    luaL_newlib(L, functions);
    lua_setglobal(L, "Crash");

    static const luaL_Reg journal[] = {
        {"log", luaJournalLog},
        {"getText", luaJournalGetText},
        {"getEntries", luaJournalGetEntries},
        {"clear", luaJournalClear},
        {nullptr, nullptr},
    };
    luaL_newlib(L, journal);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, luaJournalIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaJournalNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "Journal");
#endif //DISABLE_LUA_BINDINGS
}
