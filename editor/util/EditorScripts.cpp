// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorScripts.h"

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "mcp/McpCall.h"
#include "mcp/McpServer.h"
#include "mcp/McpUndo.h"
#include "script/EditorMenu.h"
#include "script/LuaBinding.h"
#include "script/LuaReload.h"
#include "util/EditorFingerprint.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ScriptStatus.h"
#include "util/Util.h"
#include "reload/HotReloadService.h"
#include "util/Scheduler.h"

#include "lua.hpp"
#include "imgui.h"
#include "font_awesome_names.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <regex>
#include <set>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor {

using mcp::Json;
using mcp::McpCall;
using mcp::McpServer;
using mcp::ToolResult;

namespace {

    std::string trim(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // The trailing "key=value" options of an annotation; `rest` is what comes before them.
    // A key in `restKeys` takes the rest of the line (tooltip=Some words).
    std::map<std::string, std::string> takeOptions(std::string& text, const std::set<std::string>& keys, const std::set<std::string>& restKeys){
        // [key=value] or key=value (the brackets are the documentation's "optional")
        auto unbracket = [](std::string value) {
            value = trim(value);
            if (!value.empty() && value.front() == '[') value.erase(0, 1);
            if (!value.empty() && value.back() == ']') value.pop_back();
            return trim(value);
        };
        std::map<std::string, std::string> options;
        for (const std::string& key : restKeys) {
            size_t at = text.find(" [" + key + "=");
            size_t skip = key.size() + 3;
            if (at == std::string::npos) {
                at = text.find(" " + key + "=");
                skip = key.size() + 2;
            }
            if (at != std::string::npos) {
                options[key] = unbracket(text.substr(at + skip));
                text = text.substr(0, at);
            }
        }
        while (true) {
            const size_t space = text.find_last_of(' ');
            if (space == std::string::npos) break;
            const std::string token = unbracket(text.substr(space + 1));
            const size_t equals = token.find('=');
            if (equals == std::string::npos || !keys.count(token.substr(0, equals))) break;
            options[token.substr(0, equals)] = token.substr(equals + 1);
            text = trim(text.substr(0, space));
        }
        text = trim(text);
        return options;
    }

    // The function a line defines: "function M.snap(ctx)", "function M:snap()", "M.snap = function(...)"
    std::string definedFunction(const std::string& line){
        static const std::regex named(R"(^\s*function\s+[A-Za-z_][\w]*\s*[.:]\s*([A-Za-z_]\w*)\s*\()");
        static const std::regex assigned(R"(^\s*[A-Za-z_][\w]*\s*\.\s*([A-Za-z_]\w*)\s*=\s*function\s*\()");
        std::smatch match;
        if (std::regex_search(line, match, named) || std::regex_search(line, match, assigned)) return match[1].str();
        return {};
    }

    std::vector<std::string> splitLines(const std::string& code){
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= code.size()) {
            size_t end = code.find('\n', start);
            if (end == std::string::npos) end = code.size();
            std::string line = code.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (end == code.size()) break;
            start = end + 1;
        }
        return lines;
    }

    bool hasAnnotation(const std::string& code){
        for (const std::string& line : splitLines(code)) {
            const std::string t = trim(line);
            if (t.rfind("---@menu ", 0) == 0 || t.rfind("---@toolbar ", 0) == 0 || t.rfind("---@validate ", 0) == 0) return true;
        }
        return false;
    }

    // ---- Lua <-> JSON (Editor.call's arguments and results)

    Json luaValue(lua_State* L, int index, int depth);

    Json luaTable(lua_State* L, int index, int depth){
        index = lua_absindex(L, index);
        // an array: keys 1..n
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
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING) key = lua_tostring(L, -2);
            else if (lua_type(L, -2) == LUA_TNUMBER) key = std::to_string(static_cast<long long>(lua_tointeger(L, -2)));
            if (!key.empty()) object[key] = luaValue(L, -1, depth + 1);
            lua_pop(L, 1);
        }
        return object;
    }

    Json luaValue(lua_State* L, int index, int depth){
        if (depth > 16) return nullptr;
        switch (lua_type(L, index)) {
            case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)) return static_cast<long long>(lua_tointeger(L, index));
                return lua_tonumber(L, index);
            case LUA_TSTRING: return std::string(lua_tostring(L, index));
            case LUA_TTABLE: return luaTable(L, index, depth);
            case LUA_TUSERDATA: {
                // Vector2/3/4 (the math classes): their components
                Json out = Json::array();
                for (const char* field : {"x", "y", "z", "w"}) {
                    lua_getfield(L, index, field);
                    if (lua_type(L, -1) == LUA_TNUMBER) out.push_back(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                return out.empty() ? Json(nullptr) : out;
            }
            default: return nullptr;
        }
    }

    void pushJson(lua_State* L, const Json& value){
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

    // a tool's result as Editor.call returns it: {ok, text, data}
    Json resultJson(const ToolResult& result){
        return {{"ok", !result.isError}, {"text", result.text}, {"data", result.structured}};
    }

    Json contextJson(const EditorMenuRunContext& context){
        return {{"menuPath", context.menuPath}, {"selection", context.selection}, {"scene", context.sceneId},
                {"assets", context.assets}, {"component", context.component}};
    }

    // ---- one run of a Lua item

    struct LuaRun : std::enable_shared_from_this<LuaRun> {
        lua_State* L = nullptr;
        EditorMenuItem item;
        std::shared_ptr<McpCall> parent;
        Project* project = nullptr;
        EditorMenuRunResult result;
        mcp::McpUndo::Group undo;
        int pending = 0;            // Editor.call callbacks still to come
        bool returned = false;
        bool finished = false;
        std::function<void(EditorMenuRunResult)> done;

        ~LuaRun() {
            if (L) ProjectChecks::closeIsolatedLuaState(L);
        }

        void fail(const std::string& error) {
            if (result.error.empty()) result.error = error;
            Out::error("%s", error.c_str());
        }

        void maybeFinish() {
            if (finished || !returned || pending > 0) return;
            finished = true;
            result.ok = result.error.empty();
            result.undo = mcp::McpUndo::endGroup(undo, project, item.name());
            if (result.undo.contains("undo")) result.undoName = result.undo["undo"].value("name", std::string());
            if (L) {
                ProjectChecks::closeIsolatedLuaState(L);
                L = nullptr;
            }
            auto callback = std::move(done);
            if (callback) callback(result);
        }

        // a callback of Editor.call(tool, args, callback), on the main thread
        void callbackDone(int callbackRef, const ToolResult& toolResult) {
            pending--;
            if (L && callbackRef != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
                pushJson(L, resultJson(toolResult));
                if (LuaBinding::pcallWithTraceback(L, 1, 0) != LUA_OK) {
                    fail(LuaBinding::getLuaStackErrorString(L, -1));
                    lua_pop(L, 1);
                }
                luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
            }
            maybeFinish();
        }
    };

    LuaRun* runOf(lua_State* L){
        return static_cast<LuaRun*>(lua_touserdata(L, lua_upvalueindex(1)));
    }

    // Editor.call(tool, args[, callback]): no C++ object lives across a Lua error
    int editorCall(lua_State* L){
        LuaRun* run = runOf(L);
        const char* tool = luaL_checkstring(L, 1);
        const bool hasCallback = lua_isfunction(L, 3);
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2)) return luaL_error(L, "Editor.call: the arguments are a table");
        char error[512] = {0};
        int results = 0;
        {
            Json args = lua_istable(L, 2) ? luaValue(L, 2, 0) : Json::object();
            if (!args.is_object()) args = Json::object();
            int callbackRef = LUA_NOREF;
            if (hasCallback) {
                lua_pushvalue(L, 3);
                callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            // 0 running, 1 completed inside runNested, 2 runNested returned first
            auto phase = std::make_shared<std::atomic<int>>(0);
            auto immediate = std::make_shared<ToolResult>();
            std::shared_ptr<LuaRun> self = run->shared_from_this();
            run->pending++;
            const std::string toolName = tool;
            McpServer::get().runNested(run->parent, toolName, args, [phase, immediate, self, callbackRef](const ToolResult& toolResult) {
                int expected = 0;
                if (phase->compare_exchange_strong(expected, 1)) {
                    *immediate = toolResult;
                    return;
                }
                App& app = Backend::getApp();
                if (app.isMainThread()) self->callbackDone(callbackRef, toolResult);
                else app.enqueueMainThreadTask([self, callbackRef, toolResult]() { self->callbackDone(callbackRef, toolResult); });
            });
            int expected = 0;
            if (phase->compare_exchange_strong(expected, 2)) {
                // it completes later: only a callback can see the result
                if (!hasCallback) {
                    std::snprintf(error, sizeof(error), "Editor.call('%s') completes later: pass a function as the third argument "
                                                       "to get its result", toolName.c_str());
                }
            } else {
                run->pending--;
                if (hasCallback) {
                    // the callback runs now, below, once no C++ object is alive (its errors are the caller's)
                    lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
                    luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
                    pushJson(L, resultJson(*immediate));
                    results = -1;
                } else {
                    pushJson(L, resultJson(*immediate));
                    results = 1;
                }
            }
        }
        if (error[0]) return luaL_error(L, "%s", error);
        if (results < 0) {
            lua_call(L, 1, 0);
            return 0;
        }
        return results;
    }

    int editorSelection(lua_State* L){
        LuaRun* run = runOf(L);
        std::vector<Entity> selected;
        if (run->project) selected = run->project->getSelectedEntities(run->project->getSelectedSceneId());
        lua_createtable(L, static_cast<int>(selected.size()), 0);
        for (size_t i = 0; i < selected.size(); i++) {
            lua_pushinteger(L, static_cast<lua_Integer>(selected[i]));
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    // Editor.select(ids): select_entities (an undoable selection step)
    int editorSelect(lua_State* L){
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushcclosure(L, editorCall, 1);
        lua_pushstring(L, "select_entities");
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, "entities");
        lua_call(L, 2, 1);
        return 1;
    }

    std::string joinArgs(lua_State* L){
        std::string line;
        const int n = lua_gettop(L);
        for (int i = 1; i <= n; i++) {
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > 1) line += '\t';
            line.append(text, length);
            lua_pop(L, 1);
        }
        return line;
    }

    int editorLog(lua_State* L){
        LuaRun* run = runOf(L);
        const std::string line = joinArgs(L);
        run->result.log.push_back(line);
        Out::info("[%s] %s", run->item.name().c_str(), line.c_str());
        return 0;
    }

    // A sandboxed editor state with the math classes and the Editor table, the module loaded:
    // the module table on top (false and the error otherwise)
    bool openModule(lua_State*& L, LuaRun* run, Project* project, const EditorMenuItem& item, std::string& error){
        L = ProjectChecks::newIsolatedLuaState(project->getLuaPath(), ProjectChecks::engineLuaGlobals());
        if (!L) {
            error = "could not make the editor script state";
            return false;
        }
        LuaBinding::registerMathClassesIn(L);

        lua_newtable(L);
        const std::pair<const char*, lua_CFunction> functions[] = {
            {"call", editorCall}, {"selection", editorSelection}, {"select", editorSelect}, {"log", editorLog}};
        for (const auto& [name, function] : functions) {
            lua_pushlightuserdata(L, run);
            lua_pushcclosure(L, function, 1);
            lua_setfield(L, -2, name);
        }
        lua_setglobal(L, "Editor");
        lua_pushlightuserdata(L, run);
        lua_pushcclosure(L, editorLog, 1);
        lua_setglobal(L, "print");

        std::string code;
        const fs::path file = project->getProjectPath() / FileUtils::pathFromUtf8(item.file);
        if (!ProjectChecks::readFile(file, code)) {
            error = item.file + ": could not read the file";
            return false;
        }
        const std::string chunk = lua_reload::chunkName(item.file);
        if (luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()) != LUA_OK ||
            LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK) {
            error = LuaBinding::getLuaStackErrorString(L, -1);
            lua_pop(L, 1);
            return false;
        }
        if (!lua_istable(L, -1)) {
            error = item.file + ": an editor script returns its module table";
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    // ---- the C++ editor-scripts library

    struct CppEditorLibrary {
        void* handle = nullptr;
        fs::path copy;
        uint64_t sourcesStamp = 0;        // what it was built from (sources' times and sizes)
        std::shared_ptr<Generator> generator;
        bool building = false;
        uint64_t buildingStamp = 0;
        std::vector<std::function<void(bool, std::string)>> waiting;
    };

    CppEditorLibrary& cppLibrary(){
        static auto* library = new CppEditorLibrary();
        return *library;
    }

    void* librarySymbol(void* handle, const char* name){
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }

    void unloadLibrary(CppEditorLibrary& library){
        if (!library.handle) return;
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(library.handle));
#else
        dlclose(library.handle);
#endif
        library.handle = nullptr;
        std::error_code ec;
        fs::remove(library.copy, ec);
    }

    std::vector<fs::path> cppEditorSources(Project* project){
        std::vector<fs::path> out;
        for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath(), {".cpp", ".cc", ".cxx"})) {
            const fs::path rel = file.lexically_relative(project->getProjectPath());
            if (!EditorScripts::isEditorFolderPath(rel)) continue;
            std::string code;
            if (ProjectChecks::readFile(file, code) && code.find("JE_MENU_ITEM(") != std::string::npos) out.push_back(file);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    uint64_t sourcesStamp(const std::vector<fs::path>& sources){
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        for (const fs::path& source : sources) {
            std::error_code ec;
            for (char c : source.generic_string()) mix(static_cast<unsigned char>(c));
            mix(static_cast<uint64_t>(fs::last_write_time(source, ec).time_since_epoch().count()));
            mix(static_cast<uint64_t>(fs::file_size(source, ec)));
        }
        return hash;
    }

    std::string cmakePath(const fs::path& path){
        std::string text = FileUtils::pathToUtf8(path);
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    }


    bool loadCppLibrary(CppEditorLibrary& library, const fs::path& buildDir, const std::string& libName, std::string& error){
#ifdef _WIN32
        const std::string ext = ".dll";
#elif defined(__APPLE__)
        const std::string ext = ".dylib";
#else
        const std::string ext = ".so";
#endif
        const fs::path built = buildDir / (libName + ext);
        const ScriptLibraryCheck check = checkScriptLibrary(built, editorToolchainFingerprint());
        if (!check.ok) {
            error = check.message;
            return false;
        }
        if (!check.message.empty()) Out::warning("C++ editor scripts: %s", check.message.c_str());
        unloadLibrary(library);
        static int generation = 0;
        const fs::path copy = buildDir / "loaded" / (libName + "_" + std::to_string(generation++) + ext);
        std::error_code ec;
        fs::create_directories(copy.parent_path(), ec);
        fs::copy_file(built, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "could not stage " + FileUtils::pathToUtf8(copy) + ": " + ec.message();
            return false;
        }
#ifdef _WIN32
        library.handle = LoadLibraryW(copy.wstring().c_str());
#else
        library.handle = dlopen(copy.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library.handle) {
            error = "could not load " + FileUtils::pathToUtf8(copy);
            return false;
        }
        library.copy = copy;
        using FingerprintFn = const ToolchainFingerprint* (*)();
        auto exported = reinterpret_cast<FingerprintFn>(librarySymbol(library.handle, "jasper_scriptToolchainFingerprint"));
        const ScriptLibraryCheck after = exported ? checkFingerprint(*exported(), editorToolchainFingerprint()) : ScriptLibraryCheck();
        if (!exported || !after.ok) {
            error = exported ? after.message : "Refused script library: it exports no toolchain fingerprint, rebuild it";
            unloadLibrary(library);
            return false;
        }
        return true;
    }

    // The host table the library calls back through: Editor.call for C++
    struct CppRunHost {
        std::shared_ptr<McpCall> parent;
        EditorMenuRunResult* result = nullptr;
        std::string itemName;
    };

    int hostCall(void* host, const char* tool, const char* jsonArgs, char** outJson){
        CppRunHost* run = static_cast<CppRunHost*>(host);
        Json args = Json::parse(jsonArgs ? jsonArgs : "{}", nullptr, false);
        if (args.is_discarded() || !args.is_object()) args = Json::object();
        auto phase = std::make_shared<std::atomic<int>>(0);
        auto immediate = std::make_shared<ToolResult>();
        McpServer::get().runNested(run->parent, tool ? tool : "", args, [phase, immediate](const ToolResult& result) {
            int expected = 0;
            if (phase->compare_exchange_strong(expected, 1)) *immediate = result;
        });
        int expected = 0;
        Json out;
        if (phase->compare_exchange_strong(expected, 2)) {
            out = {{"ok", false}, {"text", std::string("the tool completes later: C++ editor scripts call tools that answer at once")}};
        } else {
            out = resultJson(*immediate);
        }
        const std::string text = out.dump();
        char* copy = static_cast<char*>(std::malloc(text.size() + 1));
        if (copy) std::memcpy(copy, text.c_str(), text.size() + 1);
        if (outJson) *outJson = copy;
        return 0;
    }

    void hostFree(char* text){
        std::free(text);
    }

    void hostLog(void* host, int level, const char* text){
        CppRunHost* run = static_cast<CppRunHost*>(host);
        const std::string line = text ? text : "";
        run->result->log.push_back(line);
        if (level >= 2) Out::error("[%s] %s", run->itemName.c_str(), line.c_str());
        else if (level == 1) Out::warning("[%s] %s", run->itemName.c_str(), line.c_str());
        else Out::info("[%s] %s", run->itemName.c_str(), line.c_str());
    }

    void runCppItem(Project* project, const EditorMenuItem& item, const EditorMenuRunContext& context,
                    const std::shared_ptr<McpCall>& parent, std::function<void(EditorMenuRunResult)> done){
        CppEditorLibrary& library = cppLibrary();
        auto invoke = [project, item, context, parent, done](bool ready, std::string error) {
            EditorMenuRunResult result;
            if (!ready) {
                result.error = "C++ editor scripts: " + error;
                Out::error("%s", result.error.c_str());
                done(result);
                return;
            }
            using RunFn = int (*)(const char*, const JE_EditorHostApi*, const char*);
            auto runFn = reinterpret_cast<RunFn>(librarySymbol(cppLibrary().handle, "jasper_editorMenuRun"));
            if (!runFn) {
                result.error = "C++ editor scripts: the library has no jasper_editorMenuRun";
                done(result);
                return;
            }
            mcp::McpUndo::Group undo;
            mcp::McpUndo::beginGroup(undo, project);
            CppRunHost host{parent, &result, item.name()};
            const JE_EditorHostApi api{JE_EDITOR_HOST_ABI_VERSION, &host, &hostCall, &hostFree, &hostLog};
            const std::string contextText = contextJson(context).dump();
            int status = 1;
            try {
                status = runFn(item.path.c_str(), &api, contextText.c_str());
            } catch (const std::exception& e) {
                result.error = item.file + ": " + item.name() + " threw: " + e.what();
            } catch (...) {
                result.error = item.file + ": " + item.name() + " threw";
            }
            if (status != 0 && result.error.empty()) result.error = "C++ editor scripts: the library has no item " + item.path + " (rebuild?)";
            if (!result.error.empty()) Out::error("%s", result.error.c_str());
            result.undo = mcp::McpUndo::endGroup(undo, project, item.name());
            if (result.undo.contains("undo")) result.undoName = result.undo["undo"].value("name", std::string());
            result.ok = result.error.empty();
            done(result);
        };

        const std::vector<fs::path> sources = cppEditorSources(project);
        const uint64_t stamp = sourcesStamp(sources);
        if (library.handle && library.sourcesStamp == stamp) {
            invoke(true, {});
            return;
        }
        library.waiting.push_back(invoke);
        if (library.building) return;

        // build it (status row "C++ editor scripts"), then load and run
        const fs::path dir = project->getProjectInternalPath() / "editor-scripts";
        std::string libName;
        EditorScripts::writeCppProject(project, sources, dir, libName);
        const LocalBuildSettings settings = AppSettings::getBuildSettings(project->getProjectPath() / "project.yaml");
        CMakeKit kit;
        const std::string missing = ProjectChecks::checkCppToolchain(settings, kit);
        if (!missing.empty()) {
            std::vector<std::function<void(bool, std::string)>> waiting;
            waiting.swap(library.waiting);
            for (auto& callback : waiting) callback(false, "C++ build tools are missing:\n" + missing);
            return;
        }
        library.generator = std::make_shared<Generator>();
        library.generator->setStatusRow("editor");
        library.building = true;
        library.buildingStamp = stamp;
        const fs::path buildDir = dir / "build";
        library.generator->build(dir, buildDir, kit.cCompiler, kit.cxxCompiler, kit.generator, settings.buildJobs);
        auto generator = library.generator;
        auto poll = std::make_shared<std::function<void()>>();
        // checked once per editor frame until the build ends
        *poll = [generator, buildDir, libName, poll]() {
            CppEditorLibrary& lib = cppLibrary();
            if (generator->isBuildInProgress()) {
                scheduler().afterFrames(1, [poll]() { (*poll)(); }, TimerOpts{});
                return;
            }
            lib.building = false;
            std::string error;
            bool ready = generator->didLastBuildSucceed();
            if (!ready) error = "the build failed (see the Console)";
            else if (!loadCppLibrary(lib, buildDir, libName, error)) {
                ready = false;
                ScriptStatus::get().setEditorCpp(CppScriptState::Refused, {}, error);
            } else {
                lib.sourcesStamp = lib.buildingStamp;
            }
            std::vector<std::function<void(bool, std::string)>> waiting;
            waiting.swap(lib.waiting);
            for (auto& callback : waiting) callback(ready, error);
            // (the next frame: this lambda is running)
            scheduler().afterFrames(1, [poll]() { *poll = nullptr; }, TimerOpts{});
        };
        scheduler().afterFrames(1, [poll]() { (*poll)(); }, TimerOpts{});
    }

}

bool EditorScripts::writeCppProject(Project* project, const std::vector<fs::path>& sources, const fs::path& dir, std::string& libName){
    libName = project->getLibName() + "_editor";
    // the editor's own internal folder (util/SafeWrite.h: a tool's call is refused there)
    safewrite::EditorInternal ownDir(dir);
    const fs::path genDir = dir / "gen";
    std::error_code ec;
    fs::create_directories(genDir, ec);

    std::string registry;
    registry += "// Generated by JasperEngine Editor: the C++ editor scripts' registry. Do not edit.\n";
    registry += "#define JASPERENGINE_ABI_VERSION \"" + std::string(editorAbiVersion()) + "\"\n";
    registry += "#include \"script/ToolchainFingerprint.h\"\n";
    registry += "#include \"script/EditorMenu.h\"\n";
    registry += "#include <string>\n#include <vector>\n\n";
    registry += "#if defined(_WIN32)\n#define JE_EDITOR_EXPORT __declspec(dllexport)\n#else\n#define JE_EDITOR_EXPORT __attribute__((visibility(\"default\")))\n#endif\n\n";
    registry += "struct JeEditorMenuEntry { const char* path; jasperengine::EditorMenuFunction function; };\n";
    registry += "std::vector<JeEditorMenuEntry>& jeEditorMenus() { static std::vector<JeEditorMenuEntry> entries; return entries; }\n\n";
    registry += "JASPERENGINE_SCRIPT_TOOLCHAIN_FINGERPRINT(jasperengine::currentToolchainFingerprint())\n\n";
    registry += "extern \"C\" JE_EDITOR_EXPORT int jasper_editorMenuRun(const char* path, const JE_EditorHostApi* host, const char* contextJson) {\n";
    registry += "    for (const JeEditorMenuEntry& entry : jeEditorMenus()) {\n";
    registry += "        if (std::string(entry.path) != path) continue;\n";
    registry += "        jasperengine::EditorMenuContext context;\n";
    registry += "        context.host = host;\n";
    registry += "        context.contextJson = contextJson ? contextJson : \"{}\";\n";
    registry += "        entry.function(context);\n";
    registry += "        return 0;\n";
    registry += "    }\n";
    registry += "    return 1;\n";
    registry += "}\n";

    std::string cmakeSources;
    for (size_t i = 0; i < sources.size(); i++) {
        std::string code;
        ProjectChecks::readFile(sources[i], code);
        const fs::path rel = sources[i].lexically_relative(project->getProjectPath());
        std::string unit;
        unit += "// Generated by JasperEngine Editor from " + rel.generic_string() + ". Do not edit.\n";
        unit += "#include \"script/EditorMenu.h\"\n";
        unit += "#include <vector>\n";
        unit += "struct JeEditorMenuEntry { const char* path; jasperengine::EditorMenuFunction function; };\n";
        unit += "std::vector<JeEditorMenuEntry>& jeEditorMenus();\n";
        unit += "#include \"" + cmakePath(sources[i]) + "\"\n\n";
        unit += "namespace {\n    struct JeEditorMenuRegistrar" + std::to_string(i) + " {\n        JeEditorMenuRegistrar" + std::to_string(i) + "() {\n";
        for (const EditorMenuItem& item : EditorScripts::parseCpp(code, rel.generic_string())) {
            std::string escaped;
            for (char c : item.path) {
                if (c == '"' || c == '\\') escaped += '\\';
                escaped += c;
            }
            unit += "            jeEditorMenus().push_back({\"" + escaped + "\", &" + item.function + "});\n";
        }
        unit += "        }\n    } jeEditorMenuRegistrar" + std::to_string(i) + ";\n}\n";
        const std::string name = "unit_" + std::to_string(i) + ".cpp";
        FileUtils::writeIfChanged(genDir / name, unit);
        cmakeSources += "    ${CMAKE_CURRENT_SOURCE_DIR}/gen/" + name + "\n";
    }
    FileUtils::writeIfChanged(genDir / "registry.cpp", registry);

    const fs::path engineApi = project->getProjectInternalPath() / "engine-api";
    std::string cmake;
    cmake += "# Generated by JasperEngine Editor: the C++ editor scripts (Editor/*.cpp with JE_MENU_ITEM).\n";
    cmake += "# Built and loaded by the editor only; never part of a game. Do not edit.\n";
    cmake += "cmake_minimum_required(VERSION 3.20)\n";
    cmake += "project(" + libName + ")\n";
    cmake += "set(JASPERENGINE_PARALLEL_BUILD_JOBS \"\" CACHE STRING \"\")\n";
    cmake += "set(CMAKE_CXX_STANDARD " + std::to_string(project->getCxxStandard()) + ")\n";
    cmake += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    cmake += "option(JASPERENGINE_EDITOR_PLUGIN \"\" ON)\n";
    cmake += Generator::getToolchainFloorCheck();
    // the engine library's layouts and linkage, Jolt's and Box2D's included (MSVC links Jolt's
    // data such as JPH::Allocate only through dllimport), as the project's scripts compile
    cmake += "add_compile_definitions(JASPERENGINE_PHYSICS_2D JASPERENGINE_PHYSICS_3D JASPERENGINE_SHARED JASPERENGINE_EDITOR "
             "JASPERENGINE_EDITOR_PLUGIN " + Generator::physicsCompileDefinitions() + ")\n";
#if defined(_DEBUG)
    cmake += "if(MSVC)\n    set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreadedDebugDLL\")\nendif()\n";
#else
    cmake += "if(MSVC)\n    set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreadedDLL\")\nendif()\n";
#endif
    cmake += "add_library(" + libName + " SHARED\n    ${CMAKE_CURRENT_SOURCE_DIR}/gen/registry.cpp\n" + cmakeSources + ")\n";
    cmake += "target_include_directories(" + libName + " PRIVATE\n    \"" + cmakePath(project->getProjectPath()) + "\"\n";
    for (const std::string& sub : Generator::engineApiIncludeDirs()) {
        cmake += "    \"" + cmakePath(sub.empty() ? engineApi : engineApi / sub) + "\"\n";
    }
    cmake += ")\n";
    cmake += "unset(JASPERENGINE_LIB CACHE)\n";
    cmake += "find_library(JASPERENGINE_LIB NAMES jasperengine PATHS \"${JASPERENGINE_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    cmake += "if(NOT JASPERENGINE_LIB)\n    message(FATAL_ERROR \"JasperEngine library not found in ${JASPERENGINE_LIB_DIR}\")\nendif()\n";
    cmake += "target_link_libraries(" + libName + " PRIVATE ${JASPERENGINE_LIB})\n";
    cmake += "if(MSVC)\n    target_compile_options(" + libName + " PRIVATE /EHsc /utf-8 /Zc:__cplusplus /wd4251 /wd4275)\n";
    cmake += "else()\n    target_compile_options(" + libName + " PRIVATE -fPIC)\nendif()\n";
    cmake += "set_target_properties(" + libName + " PROPERTIES PREFIX \"\" OUTPUT_NAME \"" + libName + "\"\n";
    for (const char* config : {"", "_DEBUG", "_RELEASE", "_RELWITHDEBINFO", "_MINSIZEREL"}) {
        cmake += "    RUNTIME_OUTPUT_DIRECTORY" + std::string(config) + " ${CMAKE_BINARY_DIR}\n";
        cmake += "    LIBRARY_OUTPUT_DIRECTORY" + std::string(config) + " ${CMAKE_BINARY_DIR}\n";
    }
    cmake += ")\n";
    FileUtils::writeIfChanged(dir / "CMakeLists.txt", cmake);
    return true;
}

// ------------------------------------------------------------------ items

const char* EditorScripts::fontAwesomeGlyph(const std::string& nameIn){
    const std::string name = nameIn.rfind("fa-", 0) == 0 ? nameIn.substr(3) : nameIn;
    for (const FontAwesomeName& icon : fontAwesomeNames) {
        if (name == icon.name) return icon.glyph;
    }
    return nullptr;
}

std::string EditorMenuItem::name() const {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> EditorMenuItem::targets() const {
    if (kind == "toolbar") return {"toolbar"};
    const std::string first = path.substr(0, path.find('/'));
    if (first == "CONTEXT") return {"component"};
    if (first == "SceneView") return {"sceneview"};
    if (first == "GameObject") return {"main", "gameobject"};
    if (first == "Assets") return {"main", "assets"};
    return {"main"};
}

std::string EditorMenuItem::contextComponent() const {
    if (path.rfind("CONTEXT/", 0) != 0) return {};
    const size_t end = path.find('/', 8);
    return end == std::string::npos ? std::string() : path.substr(8, end - 8);
}

EditorScripts& EditorScripts::get(){
    static EditorScripts* scripts = new EditorScripts();
    return *scripts;
}

bool EditorScripts::isEditorFolderPath(const fs::path& projectRelative){
    fs::path parent = projectRelative.parent_path();
    for (const fs::path& part : parent) {
        if (lower(part.string()) == "editor") return true;
    }
    return false;
}

bool EditorScripts::isLuaEditorScript(const fs::path& projectRelative, const std::string& code){
    return isEditorFolderPath(projectRelative) && hasAnnotation(code);
}

bool EditorScripts::isEditorOnlyScript(const fs::path& absolute, const fs::path& projectRoot){
    const fs::path rel = absolute.lexically_normal().lexically_relative(projectRoot.lexically_normal());
    if (rel.empty() || *rel.begin() == ".." || !isEditorFolderPath(rel)) return false;
    const std::string text = FileUtils::pathToUtf8(absolute);
    const bool lua = Util::isLuaFile(text);
    if (!lua && !Util::isSourceFile(text)) return false;
    std::string code;
    if (!ProjectChecks::readFile(absolute, code)) return false;
    return lua ? hasAnnotation(code) : code.find("JE_MENU_ITEM(") != std::string::npos;
}

std::vector<EditorMenuItem> EditorScripts::parseLua(const std::string& code, const std::string& file){
    std::vector<EditorMenuItem> items;
    std::vector<EditorMenuItem> waiting;     // annotations above the next function
    std::vector<std::pair<std::string, int>> validators;   // (menu function, line) above the next function
    std::map<std::string, std::string> validatorOf;        // menu function -> validate function
    const std::vector<std::string> lines = splitLines(code);
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string line = trim(lines[i]);
        const int lineNumber = static_cast<int>(i + 1);
        if (line.rfind("---@menu ", 0) == 0) {
            EditorMenuItem item;
            std::string rest = trim(line.substr(9));
            const auto options = takeOptions(rest, {"priority", "shortcut"}, {});
            item.kind = "menu";
            item.path = rest;
            if (auto it = options.find("priority"); it != options.end()) item.priority = std::atoi(it->second.c_str());
            if (auto it = options.find("shortcut"); it != options.end()) item.shortcut = it->second;
            item.line = lineNumber;
            if (!item.path.empty()) waiting.push_back(item);
            continue;
        }
        if (line.rfind("---@toolbar ", 0) == 0) {
            EditorMenuItem item;
            std::string rest = trim(line.substr(12));
            const auto options = takeOptions(rest, {"icon", "priority"}, {"tooltip"});
            item.kind = "toolbar";
            item.path = rest;
            if (auto it = options.find("icon"); it != options.end()) {
                item.icon = it->second.rfind("fa-", 0) == 0 ? it->second.substr(3) : it->second;
                item.iconGlyph = fontAwesomeGlyph(item.icon);
            }
            if (auto it = options.find("tooltip"); it != options.end()) item.tooltip = it->second;
            if (auto it = options.find("priority"); it != options.end()) item.priority = std::atoi(it->second.c_str());
            item.line = lineNumber;
            if (!item.path.empty()) waiting.push_back(item);
            continue;
        }
        if (line.rfind("---@validate ", 0) == 0) {
            validators.push_back({trim(line.substr(13)), lineNumber});
            continue;
        }
        if (line.empty() || line.rfind("--", 0) == 0) continue;
        const std::string function = definedFunction(line);
        if (!function.empty()) {
            for (EditorMenuItem& item : waiting) {
                item.function = function;
                item.language = "lua";
                item.file = file;
                items.push_back(item);
            }
            for (const auto& [menuFunction, validatorLine] : validators) validatorOf[menuFunction] = function;
        }
        waiting.clear();
        validators.clear();
    }
    std::map<std::string, int> seen;
    for (EditorMenuItem& item : items) {
        if (auto it = validatorOf.find(item.function); it != validatorOf.end() && item.kind == "menu") item.validateFunction = it->second;
        std::string id = "lua:" + file + "#" + item.function + (item.kind == "toolbar" ? "@toolbar" : "");
        const int count = ++seen[id];
        if (count > 1) id += "#" + std::to_string(count);
        item.id = id;
    }
    return items;
}

std::vector<EditorMenuItem> EditorScripts::parseCpp(const std::string& code, const std::string& file){
    std::vector<EditorMenuItem> items;
    static const std::regex macro(R"re(JE_MENU_ITEM\s*\(\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*(-?\d+)\s*)?\))re");
    static const std::regex function(R"(^\s*(?:static\s+)?void\s+([A-Za-z_]\w*)\s*\()");
    const std::vector<std::string> lines = splitLines(code);
    for (size_t i = 0; i < lines.size(); i++) {
        std::smatch match;
        if (lines[i].find("#define") != std::string::npos || !std::regex_search(lines[i], match, macro)) continue;
        EditorMenuItem item;
        item.kind = "menu";
        item.language = "cpp";
        item.path = match[1].str();
        item.priority = match[2].matched ? std::atoi(match[2].str().c_str()) : 1000;
        item.file = file;
        item.line = static_cast<int>(i + 1);
        // the function: on the same line after the macro, or on the next lines
        std::string after = match.suffix().str();
        for (size_t j = i; j < lines.size() && j < i + 4; j++) {
            const std::string text = j == i ? after : lines[j];
            std::smatch fn;
            if (std::regex_search(text, fn, function)) {
                item.function = fn[1].str();
                break;
            }
        }
        if (item.function.empty() || item.path.empty()) continue;
        item.id = "cpp:" + file + "#" + item.function;
        items.push_back(item);
    }
    return items;
}

void EditorScripts::scanFile(const fs::path& absolute, const std::string& relative, std::vector<EditorMenuItem>& out) const {
    std::string code;
    if (!ProjectChecks::readFile(absolute, code)) return;
    const std::string text = FileUtils::pathToUtf8(absolute);
    std::vector<EditorMenuItem> found;
    if (Util::isLuaFile(text)) {
        if (!hasAnnotation(code)) return;
        found = parseLua(code, relative);
    } else if (Util::isSourceFile(text)) {
        found = parseCpp(code, relative);
    }
    out.insert(out.end(), found.begin(), found.end());
}

void EditorScripts::sortItems(){
    std::stable_sort(menuItems.begin(), menuItems.end(), [](const EditorMenuItem& a, const EditorMenuItem& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.path < b.path;
    });
    revisionCounter++;
}

void EditorScripts::scan(Project* projectIn){
    project = projectIn;
    // a script file changed (or went): its items are read again
    if (invalidator == 0) {
        invalidator = HotReloadService::get().addInvalidator(
            static_cast<uint8_t>(HotReloadService::Event::Reloaded) | static_cast<uint8_t>(HotReloadService::Event::Dropped),
            [](const fs::path& path, const std::string& kind, HotReloadService::Event) {
                if (kind == "script") EditorScripts::get().rescanFile(path);
            });
    }
    menuItems.clear();
    validated.clear();
    if (!project || project->getProjectPath().empty()) {
        revisionCounter++;
        return;
    }
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath(), {".lua", ".cpp", ".cc", ".cxx"})) {
        const fs::path rel = file.lexically_relative(project->getProjectPath());
        if (!isEditorFolderPath(rel)) continue;
        scanFile(file, FileUtils::pathToGenericUtf8(rel), menuItems);
    }
    sortItems();
}

void EditorScripts::rescanFile(const fs::path& absolute){
    if (!project) return;
    const fs::path rel = absolute.lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
    if (rel.empty() || *rel.begin() == ".." || !isEditorFolderPath(rel)) return;
    const std::string relative = FileUtils::pathToGenericUtf8(rel);
    const size_t before = menuItems.size();
    menuItems.erase(std::remove_if(menuItems.begin(), menuItems.end(), [&](const EditorMenuItem& item) { return item.file == relative; }),
                    menuItems.end());
    std::error_code ec;
    if (fs::is_regular_file(absolute, ec)) scanFile(absolute, relative, menuItems);
    if (menuItems.size() != before || !menuItems.empty()) sortItems();
}

const EditorMenuItem* EditorScripts::find(const std::string& idOrPath) const {
    for (const EditorMenuItem& item : menuItems) {
        if (item.id == idOrPath) return &item;
    }
    for (const EditorMenuItem& item : menuItems) {
        if (item.path == idOrPath && item.kind == "menu") return &item;
    }
    for (const EditorMenuItem& item : menuItems) {
        if (item.path == idOrPath) return &item;
    }
    return nullptr;
}

std::vector<const EditorMenuItem*> EditorScripts::itemsFor(const std::string& target, const std::string& component) const {
    std::vector<const EditorMenuItem*> out;
    for (const EditorMenuItem& item : menuItems) {
        const std::vector<std::string> targets = item.targets();
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) continue;
        if (target == "component" && !component.empty() && item.contextComponent() != component) continue;
        out.push_back(&item);
    }
    return out;
}

bool EditorScripts::isEnabled(const EditorMenuItem& item) const {
    auto it = validated.find(item.id);
    return it == validated.end() || it->second;
}

void EditorScripts::menuOpened(const std::string& prefix, const EditorMenuRunContext& context){
    if (!project) return;
    for (const EditorMenuItem& item : menuItems) {
        if (item.validateFunction.empty() || item.language != "lua" || item.path.rfind(prefix, 0) != 0) continue;
        auto run = std::make_shared<LuaRun>();
        run->item = item;
        run->project = project;
        std::string error;
        bool enabled = true;
        const auto started = std::chrono::steady_clock::now();
        if (openModule(run->L, run.get(), project, item, error)) {
            lua_getfield(run->L, -1, item.validateFunction.c_str());
            if (lua_isfunction(run->L, -1)) {
                Json ctx = contextJson(context);
                ctx["menuPath"] = item.path;
                pushJson(run->L, ctx);
                if (LuaBinding::pcallWithTraceback(run->L, 1, 1) == LUA_OK) {
                    enabled = lua_toboolean(run->L, -1) != 0;
                } else {
                    error = LuaBinding::getLuaStackErrorString(run->L, -1);
                }
            }
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
        if (!error.empty()) {
            Out::warning("%s: validate %s failed (the item stays enabled): %s", item.file.c_str(), item.validateFunction.c_str(), error.c_str());
            enabled = true;
        } else if (us > VALIDATE_BUDGET_US) {
            Out::warning("%s: validate %s took %.1f ms (budget %d ms): the item stays enabled", item.file.c_str(),
                         item.validateFunction.c_str(), us / 1000.0, VALIDATE_BUDGET_US / 1000);
            enabled = true;
        }
        validated[item.id] = enabled;
    }
}

void EditorScripts::run(const EditorMenuItem& item, const EditorMenuRunContext& context, const std::shared_ptr<McpCall>& parent,
                        std::function<void(EditorMenuRunResult)> done){
    if (!project) {
        EditorMenuRunResult result;
        result.error = "no project";
        done(result);
        return;
    }
    if (item.language == "cpp") {
        runCppItem(project, item, context, parent, std::move(done));
        return;
    }

    auto run = std::make_shared<LuaRun>();
    run->item = item;
    run->parent = parent;
    run->project = project;
    run->done = std::move(done);
    mcp::McpUndo::beginGroup(run->undo, project);
    std::string error;
    if (!openModule(run->L, run.get(), project, item, error)) {
        run->fail(error);
        run->returned = true;
        run->maybeFinish();
        return;
    }
    lua_getfield(run->L, -1, item.function.c_str());
    if (!lua_isfunction(run->L, -1)) {
        run->fail(item.file + ":" + std::to_string(item.line) + ": the module has no function '" + item.function + "'");
    } else {
        Json ctx = contextJson(context);
        ctx["menuPath"] = item.path;
        pushJson(run->L, ctx);
        if (LuaBinding::pcallWithTraceback(run->L, 1, 0) != LUA_OK) {
            run->fail(LuaBinding::getLuaStackErrorString(run->L, -1));
            lua_pop(run->L, 1);
        }
    }
    run->returned = true;
    run->maybeFinish();
}

bool EditorScripts::drawContextItems(const std::string& target, const std::string& component, const EditorMenuRunContext& context){
    const std::vector<const EditorMenuItem*> items = itemsFor(target, component);
    if (items.empty()) return false;
    if (ImGui::IsWindowAppearing()) {
        for (const EditorMenuItem* item : items) {
            if (!item->validateFunction.empty()) menuOpened(item->path, context);
        }
    }
    ImGui::Separator();
    for (const EditorMenuItem* item : items) {
        // the path below the target's own parts: GameObject/Test/Raise -> Test > Raise
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= item->path.size()) {
            const size_t slash = item->path.find('/', start);
            parts.push_back(item->path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        const size_t skip = target == "component" ? 2 : 1;
        if (parts.size() <= skip) continue;
        int opened = 0;
        bool visible = true;
        for (size_t i = skip; i + 1 < parts.size(); i++) {
            if (!ImGui::BeginMenu(parts[i].c_str())) {
                visible = false;
                break;
            }
            opened++;
        }
        if (visible) {
            ImGui::PushID(item->id.c_str());
            if (ImGui::MenuItem(parts.back().c_str(), item->shortcut.empty() ? nullptr : item->shortcut.c_str(), false, isEnabled(*item))) {
                EditorMenuRunContext run = context;
                run.component = component;
                runFromMenu(item->id, run);
            }
            ImGui::PopID();
        }
        for (int i = 0; i < opened; i++) ImGui::EndMenu();
    }
    return true;
}

void EditorScripts::runFromMenu(const std::string& id, const EditorMenuRunContext& context){
    Json args = {{"id", id}};
    if (context.sceneId != 0) args["scene"] = context.sceneId;
    if (!context.selection.empty()) args["selection"] = context.selection;
    if (!context.assets.empty()) args["assets"] = context.assets;
    if (!context.component.empty()) args["component"] = context.component;
    McpServer::get().runLocal("run_editor_menu", args, "Editor", [](const ToolResult& result) {
        if (result.isError) Out::error("%s", result.text.c_str());
    });
}

}
