// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/EditModeTests.h"

#include "Project.h"
#include "mcp/McpCall.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpServer.h"
#include "script/LuaBinding.h"
#include "test/TestRunner.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::gametests {

using gametest::Json;
namespace fs = std::filesystem;

namespace {

    struct EditContext {
        std::shared_ptr<mcp::McpCall> parent;
        std::vector<std::string> log;
    };

    EditContext* contextOf(lua_State* L) {
        return static_cast<EditContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    }

    Json luaValue(lua_State* L, int index, int depth) {
        if (depth > 16) return nullptr;
        index = lua_absindex(L, index);
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)) return static_cast<long long>(lua_tointeger(L, index));
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
                        array.push_back(luaValue(L, -1, depth + 1));
                        lua_pop(L, 1);
                    }
                    return array;
                }
                Json object = Json::object();
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING) object[lua_tostring(L, -2)] = luaValue(L, -1, depth + 1);
                    else if (lua_type(L, -2) == LUA_TNUMBER) object[std::to_string(static_cast<long long>(lua_tointeger(L, -2)))] = luaValue(L, -1, depth + 1);
                    lua_pop(L, 1);
                }
                return object;
            }
            default: return nullptr;
        }
    }

    void pushJson(lua_State* L, const Json& value) {
        if (value.is_null()) lua_pushnil(L);
        else if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
        else if (value.is_number_integer()) lua_pushinteger(L, static_cast<lua_Integer>(value.get<long long>()));
        else if (value.is_number()) lua_pushnumber(L, value.get<double>());
        else if (value.is_string()) lua_pushstring(L, value.get<std::string>().c_str());
        else if (value.is_array()) {
            lua_createtable(L, static_cast<int>(value.size()), 0);
            lua_Integer i = 1;
            for (const Json& item : value) {
                pushJson(L, item);
                lua_rawseti(L, -2, i++);
            }
        } else {
            lua_createtable(L, 0, static_cast<int>(value.size()));
            for (auto it = value.begin(); it != value.end(); ++it) {
                pushJson(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
        }
    }

    // Editor.call(tool, args): a read-only tool's {ok, text, data}, at once. No C++ object is
    // alive where it raises (Lua is built as C: its errors longjmp)
    int editorCall(lua_State* L) {
        EditContext* context = contextOf(L);
        const char* tool = luaL_checkstring(L, 1);
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2)) return luaL_error(L, "Editor.call: the arguments are a table");
        char error[512] = {0};
        {
            const mcp::ToolDef* def = mcp::McpServer::get().registry().findTool(tool);
            if (!def) {
                std::snprintf(error, sizeof(error), "Editor.call: no tool \"%s\"", tool);
            } else if (!def->annotations.readOnly) {
                std::snprintf(error, sizeof(error), EDIT_MODE_READ_ONLY, tool);
            } else {
                Json args = lua_istable(L, 2) ? luaValue(L, 2, 0) : Json::object();
                if (!args.is_object()) args = Json::object();
                auto done = std::make_shared<std::atomic<bool>>(false);
                auto result = std::make_shared<mcp::ToolResult>();
                mcp::McpServer::get().runNested(context->parent, tool, args, [done, result](const mcp::ToolResult& r) {
                    *result = r;
                    done->store(true);
                });
                if (!done->load()) {
                    std::snprintf(error, sizeof(error), "Editor.call('%s') answers later (a job): Edit Mode tests call tools that answer at once", tool);
                } else {
                    pushJson(L, Json{{"ok", !result->isError}, {"text", result->text}, {"data", result->structured}});
                }
            }
        }
        if (error[0]) return luaL_error(L, "%s", error);
        return 1;
    }

    int editorLog(lua_State* L) {
        EditContext* context = contextOf(L);
        std::string line;
        for (int i = 1; i <= lua_gettop(L); i++) {
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > 1) line += '\t';
            line.append(text, length);
            lua_pop(L, 1);
        }
        context->log.push_back(line);
        return 0;
    }

    bool editModeReadText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    }

    gametest::CaseResult errorResult(const gametest::CaseSpec& spec, const std::string& message) {
        gametest::CaseResult r;
        r.spec = spec;
        r.status = gametest::Status::Error;
        r.failures.push_back({"error", message, spec.file, spec.line, 0});
        return r;
    }

}

std::vector<gametest::CaseResult> runEditModeCases(const std::shared_ptr<mcp::McpCall>& parent, Project* project, const fs::path& root,
                                                              const gametest::Settings& settings, const gametest::Target& target,
                                                              const std::vector<gametest::CaseSpec>& cases) {
    std::vector<gametest::CaseResult> results;
    // the files in the order their cases come
    std::vector<std::string> files;
    std::map<std::string, std::vector<const gametest::CaseSpec*>> byFile;
    for (const gametest::CaseSpec& spec : cases) {
        if (!byFile.count(spec.file)) files.push_back(spec.file);
        byFile[spec.file].push_back(&spec);
    }
    const std::string targetName = target.editor ? "editor" : target.platform;
    for (const std::string& file : files) {
        EditContext context;
        context.parent = parent;
        lua_State* L = ProjectChecks::newIsolatedLuaState(project->getLuaPath(), ProjectChecks::engineLuaGlobals());
        if (!L) {
            for (const gametest::CaseSpec* spec : byFile[file]) results.push_back(errorResult(*spec, "could not make the editor test state"));
            continue;
        }
        LuaBinding::registerMathClassesIn(L);
        lua_newtable(L);
        lua_pushlightuserdata(L, &context);
        lua_pushcclosure(L, editorCall, 1);
        lua_setfield(L, -2, "call");
        lua_pushlightuserdata(L, &context);
        lua_pushcclosure(L, editorLog, 1);
        lua_setfield(L, -2, "log");
        lua_setglobal(L, "Editor");
        lua_pushlightuserdata(L, &context);
        lua_pushcclosure(L, editorLog, 1);
        lua_setglobal(L, "print");
        {
            gametest::TestChunk chunk(L, gametest::Mode::Edit, settings, target, [&root](const std::string& path, std::string& code) {
                return editModeReadText(root / FileUtils::pathFromUtf8(path), code);
            });
            std::string code, error;
            if (!editModeReadText(root / FileUtils::pathFromUtf8(file), code)) error = "cannot read " + file;
            else chunk.load(code, file, error);
            for (const gametest::CaseSpec* spec : byFile[file]) {
                const auto started = std::chrono::steady_clock::now();
                if (!error.empty()) {
                    results.push_back(errorResult(*spec, error));
                    continue;
                }
                gametest::CaseResult r;
                r.spec = *spec;
                if (!spec->skip.empty()) {
                    r.status = gametest::Status::Skipped;
                    r.skipReason = spec->skip;
                } else if (!spec->platforms.empty() && std::find(spec->platforms.begin(), spec->platforms.end(), targetName) == spec->platforms.end()) {
                    r.status = gametest::Status::Skipped;
                    r.skipReason = "not on " + targetName;
                } else if (spec->duplicate) {
                    r = errorResult(*spec, "duplicate case name \"" + spec->name + "\" in " + file);
                } else {
                    context.log.clear();
                    chunk.runSync(spec->name, r);
                    r.log.insert(r.log.begin(), context.log.begin(), context.log.end());
                }
                r.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                results.push_back(std::move(r));
            }
        }
        ProjectChecks::closeIsolatedLuaState(L);
    }
    return results;
}

}  // namespace jasperengine::editor::gametests
