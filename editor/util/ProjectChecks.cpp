// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectChecks.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "pool/ShaderPool.h"
#include "script/LuaBinding.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/ProjectUtils.h"
#include "util/ScriptParser.h"
#include "util/Util.h"

#include "lua.hpp"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

using namespace jasperengine;
using jasperengine::editor::Util;

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::string lowerExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

int toolOrder(const std::string& tool) {
    if (tool == "lua") return 0;
    if (tool == "shader") return 1;
    if (tool == "cpp") return 2;
    return 3;
}

std::string oneLine(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\r') continue;
        out += (c == '\n' || c == '\t') ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// "<chunk>:<line>: <message>" -> line + message (line 0 and the whole text otherwise). Lua
// shortens a long chunk name to "...<its end>".
void splitLuaMessage(const std::string& text, const std::string& chunkName, int& line, std::string& message) {
    line = 0;
    message = text;
    size_t prefixEnd = std::string::npos;
    if (text.rfind(chunkName + ":", 0) == 0) {
        prefixEnd = chunkName.size() + 1;
    } else if (text.rfind("...", 0) == 0) {
        // the shortened name is the end of the chunk name, before ":<digits>:"
        for (size_t colon = text.find(':', 3); colon != std::string::npos; colon = text.find(':', colon + 1)) {
            const std::string tail = text.substr(3, colon - 3);
            if (chunkName.size() >= tail.size() && chunkName.compare(chunkName.size() - tail.size(), tail.size(), tail) == 0) {
                prefixEnd = colon + 1;
                break;
            }
        }
    }
    if (prefixEnd == std::string::npos) return;
    const size_t colon = text.find(':', prefixEnd);
    if (colon == std::string::npos || colon == prefixEnd) return;
    for (size_t i = prefixEnd; i < colon; i++) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return;
    }
    try {
        line = std::stoi(text.substr(prefixEnd, colon - prefixEnd));
    } catch (...) {
        line = 0;
        return;
    }
    message = text.substr(colon + 1);
    if (!message.empty() && message[0] == ' ') message.erase(0, 1);
}

} // namespace

// ======================================================================== findings

Json editor::ProjectChecks::toJson(const Finding& finding) {
    return Json{{"tool", finding.tool}, {"path", finding.path}, {"line", finding.line}, {"col", finding.col},
                {"severity", finding.severity}, {"message", finding.message}};
}

Json editor::ProjectChecks::toJson(const std::vector<Finding>& findings) {
    Json out = Json::array();
    for (const Finding& finding : findings) out.push_back(toJson(finding));
    return out;
}

std::string editor::ProjectChecks::toText(const Finding& finding) {
    std::string out = finding.path.empty() ? finding.tool : finding.path;
    if (!finding.path.empty() && finding.line > 0) {
        out += ":" + std::to_string(finding.line);
        if (finding.col > 0) out += ":" + std::to_string(finding.col);
    }
    return out + ": " + finding.severity + ": " + oneLine(finding.message);
}

void editor::ProjectChecks::sortFindings(std::vector<Finding>& findings) {
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
        if (toolOrder(a.tool) != toolOrder(b.tool)) return toolOrder(a.tool) < toolOrder(b.tool);
        if (a.path != b.path) return a.path < b.path;
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
}

size_t editor::ProjectChecks::countSeverity(const std::vector<Finding>& findings, const std::string& severity) {
    return static_cast<size_t>(std::count_if(findings.begin(), findings.end(), [&](const Finding& f) { return f.severity == severity; }));
}

// ======================================================================== files

std::string editor::ProjectChecks::projectRelative(const fs::path& projectRoot, const fs::path& absolute) {
    const fs::path rel = absolute.lexically_normal().lexically_relative(projectRoot.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return absolute.generic_string();
    if (rel == ".") return "";
    return rel.generic_string();
}

bool editor::ProjectChecks::isSkippedDir(const fs::path& dir) {
    const std::string name = dir.filename().string();
    return name.empty() || name[0] == '.' || name == "build";
}

bool editor::ProjectChecks::isInSkippedDir(const fs::path& relative) {
    const fs::path parent = relative.lexically_normal().parent_path();
    for (const fs::path& part : parent) {
        if (part == ".") continue;
        if (isSkippedDir(part)) return true;
    }
    return false;
}

bool editor::ProjectChecks::readFile(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::vector<fs::path> editor::ProjectChecks::findProjectFiles(const fs::path& projectRoot, const std::vector<std::string>& extensions) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(projectRoot, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
         it.increment(ec)) {
        const fs::path& path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(ec)) {
            if (isSkippedDir(path)) it.disable_recursion_pending();
            continue;
        }
        if (name.empty() || name[0] == '.' || !it->is_regular_file(ec)) continue;
        if (std::find(extensions.begin(), extensions.end(), lowerExtension(path)) != extensions.end()) files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path editor::ProjectChecks::luaRootFromProjectFile(const fs::path& projectRoot) {
    std::string luaDir;
    try {
        YAML::Node node = YAML::LoadFile((projectRoot / "project.yaml").string());
        if (node["luaDir"]) luaDir = node["luaDir"].as<std::string>();
    } catch (const std::exception&) {
    }
    const fs::path dir(luaDir);
    if (dir.empty() || dir == ".") return projectRoot;
    if (dir.is_absolute()) return dir.lexically_normal();
    return (projectRoot / dir).lexically_normal();
}

// ======================================================================== Lua

editor::ProjectChecks::LuaSyntax editor::ProjectChecks::checkLuaSyntax(const std::string& code, const std::string& chunkName) {
    LuaSyntax result;
    lua_State* L = luaL_newstate();
    if (!L) return result;
    const std::string chunk = "@" + chunkName;
    if (luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()) != LUA_OK) {
        result.ok = false;
        const char* message = lua_tostring(L, -1);
        splitLuaMessage(message ? message : "syntax error", chunkName, result.line, result.message);
    }
    lua_close(L);
    return result;
}

editor::ProjectChecks::LuaScriptCheck editor::ProjectChecks::checkLuaScript(const std::string& code, const std::string& path, lua_State* L) {
    LuaScriptCheck check;
    const LuaSyntax syntax = checkLuaSyntax(code, path);
    if (!syntax.ok) {
        check.syntaxOk = false;
        check.findings.push_back({"lua", path, syntax.line, 0, "error", syntax.message});
        return check;
    }
    if (!L) return check;

    ScriptEntry probe;
    const ProjectUtils::LuaPropertiesResult run = ProjectUtils::runLuaScriptProperties(L, probe, code, path);
    if (run.status == ProjectUtils::LuaPropertiesResult::Ok) {
        check.properties = probe.properties;
        return check;
    }
    Finding finding{"lua", path, 0, 0, "error", ""};
    splitLuaMessage(run.message, path, finding.line, finding.message);
    if (run.status == ProjectUtils::LuaPropertiesResult::RunError) finding.message = "runtime error at load: " + finding.message;
    check.findings.push_back(finding);
    return check;
}

namespace {

const char* const kLuaRootKey = "jasperengine.check.luaRoot";

// Bounds of an isolated state: memory, and VM instructions (a top level that loops forever)
struct IsolatedLimits {
    size_t used = 0;
    size_t limit = 256u * 1024u * 1024u;
    unsigned long long steps = 0;
    unsigned long long maxSteps = 100000000ull;
};
constexpr int kHookEvery = 10000;

void* limitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    IsolatedLimits* limits = static_cast<IsolatedLimits*>(ud);
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        limits->used -= old;
        std::free(ptr);
        return nullptr;
    }
    if (limits->used - old + nsize > limits->limit) return nullptr;
    void* block = std::realloc(ptr, nsize);
    if (!block) return nullptr;
    limits->used = limits->used - old + nsize;
    return block;
}

void stepHook(lua_State* L, lua_Debug* ar) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    IsolatedLimits* limits = static_cast<IsolatedLimits*>(ud);
    limits->steps += kHookEvery;
    if (limits->steps > limits->maxSteps) {
        const char* message = "the script did not finish loading (an endless loop?)";
        // the chunk name in full (short_src is truncated for long paths)
        if (lua_getinfo(L, "Sl", ar) && ar->currentline > 0 && ar->source && ar->source[0] == '@') {
            lua_pushfstring(L, "%s:%d: %s", ar->source + 1, ar->currentline, message);
        } else {
            lua_pushstring(L, message);
        }
        lua_error(L);
    }
}

// package.searchers[1]: the engine's loader (LuaBinding::moduleLoader), reading the project's
// Lua directory instead of "lua://"
int projectModuleSearcher(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    std::replace(name.begin(), name.end(), '.', '/');
    lua_getfield(L, LUA_REGISTRYINDEX, kLuaRootKey);
    const fs::path root = fs::path(lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
    lua_pop(L, 1);
    for (const std::string& rel : {name + ".lua", "lua/" + name + ".lua"}) {
        if (!Util::isSafeRelativePath(fs::path(rel))) continue;
        std::string content;
        if (!editor::ProjectChecks::readFile(root / rel, content)) continue;
        const std::string chunk = "lua://" + rel;
        luaL_loadbuffer(L, content.data(), content.size(), chunk.c_str());  // the chunk, or its error message
        return 1;
    }
    lua_pushstring(L, "\n\tno file in assets directory");
    return 1;
}

// Run in the isolated state before any script: the sandbox (arguments: the engine global
// names as a set, the module searcher)
const char* const kSandboxSetup = R"lua(
local engineNames, searcher = ...
local stub
local function same() return stub end
stub = setmetatable({}, {
    __index = function(_, key) if type(key) == "number" then return nil end return stub end,
    __newindex = function() end,
    __call = same, __add = same, __sub = same, __mul = same, __div = same, __mod = same, __pow = same,
    __unm = same, __idiv = same, __band = same, __bor = same, __bxor = same, __shl = same, __shr = same, __bnot = same,
    __concat = function() return "" end,
    __len = function() return 0 end,
    __lt = function() return false end,
    __le = function() return false end,
    __tostring = function() return "engine value" end,
    __pairs = function(t) return function() return nil end, t, nil end,
    __close = function() end,
})
setmetatable(_G, { __index = function(_, key) if engineNames[key] then return stub end end })
print = function() end
dofile = nil
loadfile = nil
local textLoad = load
load = function(chunk, name, _, ...) return textLoad(chunk, name, "t", ...) end
os = { time = os.time, clock = os.clock, date = os.date, difftime = os.difftime }
package.loadlib = nil
package.searchpath = nil
package.path = ""
package.cpath = ""
package.searchers = { searcher }
)lua";

} // namespace

lua_State* editor::ProjectChecks::newIsolatedLuaState(const fs::path& luaRoot, const std::vector<std::string>& engineGlobals) {
    IsolatedLimits* limits = new IsolatedLimits();
    lua_State* L = lua_newstate(limitedAlloc, limits);
    if (!L) {
        delete limits;
        return nullptr;
    }
    const std::pair<const char*, lua_CFunction> libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_LOADLIBNAME, luaopen_package}, {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_OSLIBNAME, luaopen_os}};
    for (const auto& [name, open] : libs) {
        luaL_requiref(L, name, open, 1);
        lua_pop(L, 1);
    }
    lua_pushstring(L, luaRoot.string().c_str());
    lua_setfield(L, LUA_REGISTRYINDEX, kLuaRootKey);

    if (luaL_loadbuffer(L, kSandboxSetup, std::char_traits<char>::length(kSandboxSetup), "=sandbox") != LUA_OK) {
        closeIsolatedLuaState(L);
        return nullptr;
    }
    lua_createtable(L, 0, static_cast<int>(engineGlobals.size()));
    for (const std::string& name : engineGlobals) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, name.c_str());
    }
    lua_pushcfunction(L, projectModuleSearcher);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        closeIsolatedLuaState(L);
        return nullptr;
    }
    lua_sethook(L, stepHook, LUA_MASKCOUNT, kHookEvery);
    return L;
}

void editor::ProjectChecks::closeIsolatedLuaState(lua_State* L) {
    if (!L) return;
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    lua_close(L);
    delete static_cast<IsolatedLimits*>(ud);
}

std::vector<std::string> editor::ProjectChecks::engineLuaGlobals() {
    std::vector<std::string> names;
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return names;
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) names.push_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::sort(names.begin(), names.end());
    return names;
}

// ======================================================================== C++ scripts

editor::ProjectChecks::CppHeaderInfo editor::ProjectChecks::checkCppHeader(const std::string& code, const std::string& name) {
    CppHeaderInfo info;
    info.className = ScriptParser::findScriptClassNameFromString(code);
    if (info.className) {
        if (auto base = ScriptParser::inheritsScriptBaseFromString(code, *info.className)) info.directScript = *base;
    }
    info.properties = ScriptParser::parseScriptPropertiesFromString(code, name);
    return info;
}

editor::ProjectChecks::CppPrepare editor::ProjectChecks::prepareCppBuild(Project* project, CppBuildPlan& plan, std::string& error) {
    try {
        plan.scripts = project->prepareScriptBuild();
    } catch (const std::exception& e) {
        error = std::string("Generating the build files failed: ") + e.what();
        return CppPrepare::Failed;
    }
    if (plan.scripts.empty() && project->getScriptDirs().empty()) {
        return CppPrepare::NothingToBuild;
    }
    const LocalBuildSettings settings = AppSettings::getBuildSettings(project->getProjectPath() / "project.yaml");
    const bool useDefaultKit = settings.cCompiler.empty() && settings.cxxCompiler.empty() && settings.generator.empty();
    plan.kit = CMakeKit();
    plan.kit.cCompiler = settings.cCompiler;
    plan.kit.cxxCompiler = settings.cxxCompiler;
    plan.kit.generator = settings.generator;
    plan.jobs = settings.buildJobs;
    const std::string missing = Generator::checkBuildTools(useDefaultKit, &plan.kit);
    if (!missing.empty()) {
        error = "C++ build tools are missing:\n" + missing;
        return CppPrepare::MissingTools;
    }
    return CppPrepare::Ready;
}

void editor::ProjectChecks::startCppBuild(Project* project, Generator& generator, const CppBuildPlan& plan) {
    generator.build(project->getProjectPath(), project->getProjectInternalPath() / "build", plan.kit.cCompiler,
                    plan.kit.cxxCompiler, plan.kit.generator, plan.jobs);
}

std::vector<std::string> editor::ProjectChecks::waitCppBuild(Generator& generator, uint64_t firstLogId,
                                                            const std::function<void(const std::string&)>& onLine) {
    uint64_t cursor = firstLogId;
    std::vector<std::string> output;
    auto drain = [&]() {
        for (const LogEntry& entry : Out::getLogsSince(cursor)) {
            cursor = entry.id;
            if (entry.type == LogType::Info || entry.type == LogType::Success) continue;
            std::istringstream lines(entry.message);
            std::string line;
            while (std::getline(lines, line)) {
                output.push_back(line);
                if (onLine) onLine(line);
            }
        }
    };
    while (generator.isBuildInProgress()) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    generator.waitForBuildToComplete();
    drain();
    return output;
}

editor::ProjectChecks::CppBuildReport editor::ProjectChecks::summarizeCppBuild(const fs::path& projectRoot, bool success,
                                                                             const std::vector<std::string>& output) {
    // gcc/clang: file:line:col: error: message      MSVC: file(line,col): error C1234: message
    static const std::regex gnu(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)");
    static const std::regex msvc(R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning)\s*\w*\d*\s*:\s*(.*)$)");
    CppBuildReport report;
    report.success = success;
    std::set<std::string> seen;
    for (const std::string& raw : output) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::smatch m;
        if (!std::regex_match(line, m, gnu) && !std::regex_match(line, m, msvc)) continue;
        CppDiagnostic d;
        d.file = m[1].str();
        d.line = std::atoi(m[2].str().c_str());
        d.column = m[3].matched ? std::atoi(m[3].str().c_str()) : 0;
        d.severity = m[4].str() == "fatal error" ? "error" : m[4].str();
        d.message = m[5].str();
        if (d.severity == "note") continue;
        const fs::path abs(d.file);
        if (abs.is_absolute()) {
            const fs::path rel = abs.lexically_normal().lexically_relative(projectRoot.lexically_normal());
            if (!rel.empty() && *rel.begin() != "..") d.file = rel.generic_string();
        }
        const std::string key = d.file + ":" + std::to_string(d.line) + ":" + d.message;
        if (!seen.insert(key).second) continue;
        if (d.severity == "error") {
            if (report.errors.size() < 60) report.errors.push_back(d);
        } else if (report.warnings.size() < 30) {
            report.warnings.push_back(d);
        }
    }
    for (size_t i = output.size() > 30 ? output.size() - 30 : 0; i < output.size(); i++) report.logTail.push_back(output[i]);

    for (const std::vector<CppDiagnostic>* list : {&report.errors, &report.warnings}) {
        for (const CppDiagnostic& d : *list) report.findings.push_back({"cpp", d.file, d.line, d.column, d.severity, d.message});
    }
    if (!success && report.errors.empty()) {
        std::string last;
        for (auto it = output.rbegin(); it != output.rend() && last.empty(); ++it) last = oneLine(*it);
        report.findings.push_back({"cpp", "", 0, 0, "error",
                                   "the C++ build failed without a file:line diagnostic (CMake configuration or linking?)" +
                                       (last.empty() ? std::string() : "; last output: " + last)});
    }
    return report;
}

// ======================================================================== shaders

bool editor::ProjectChecks::shaderTypeForComponent(ComponentType component, ShaderType& out) {
    switch (component) {
        case ComponentType::MeshComponent: out = ShaderType::MESH; return true;
        case ComponentType::UIComponent: out = ShaderType::UI; return true;
        case ComponentType::PointsComponent: out = ShaderType::POINTS; return true;
        case ComponentType::LineRendererComponent: out = ShaderType::LINES; return true;
        case ComponentType::SkyboxComponent: out = ShaderType::SKYBOX; return true;
        default: return false;
    }
}

namespace {

std::string forkKey(const editor::ProjectChecks::ShaderUse& use) {
    return use.base + "#" + std::to_string(use.shaderType);
}

} // namespace

std::vector<editor::ProjectChecks::ShaderUse> editor::ProjectChecks::shaderForksInUse(Project* project) {
    std::vector<ShaderUse> uses;
    std::set<std::pair<std::string, int>> seen;
    auto add = [&](const std::string& base, ShaderType type) {
        if (base.empty()) return;
        if (seen.insert({base, static_cast<int>(type)}).second) uses.push_back({base, static_cast<int>(type)});
    };
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        for (Entity entity : sceneProject->entities) {
            for (ComponentType component : Catalog::findComponents(scene, entity)) {
                ShaderType type;
                if (!shaderTypeForComponent(component, type)) continue;
                if (auto* value = Catalog::getPropertyRef<std::string>(scene, entity, component, "customShader")) add(*value, type);
                if (component == ComponentType::MeshComponent) {
                    if (auto* depth = Catalog::getPropertyRef<std::string>(scene, entity, component, "customDepthShader")) add(*depth, ShaderType::DEPTH);
                }
            }
        }
        const std::pair<const char*, ShaderType> defaults[] = {
            {"default_mesh_shader", ShaderType::MESH}, {"default_ui_shader", ShaderType::UI},
            {"default_sky_shader", ShaderType::SKYBOX}, {"default_points_shader", ShaderType::POINTS},
            {"default_lines_shader", ShaderType::LINES}};
        for (const auto& [property, type] : defaults) {
            add(Catalog::getSceneProperty<std::string>(scene, property), type);
        }
        for (const PostProcessPass& pass : scene->getPostProcessPasses()) add(pass.shader, ShaderType::POSTPROCESS);
    }
    return uses;
}

std::vector<editor::ProjectChecks::ShaderUse> editor::ProjectChecks::shaderForksUsingFiles(Project* project, const std::vector<std::string>& files) {
    std::vector<ShaderUse> result;
    std::set<std::string> covered;
    const std::vector<ShaderUse> forks = shaderForksInUse(project);
    for (const std::string& file : files) {
        const std::string ext = lowerExtension(file);
        if (ext == ".glsl") {
            // an include can be reached by any fork (private copies, project-root overrides)
            for (const ShaderUse& use : forks) {
                if (covered.insert(forkKey(use)).second) result.push_back(use);
            }
            continue;
        }
        bool found = false;
        for (const ShaderUse& use : forks) {
            Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(use.base);
            if (paths.vert == file || paths.frag == file) {
                found = true;
                if (covered.insert(forkKey(use)).second) result.push_back(use);
            }
        }
        if (!found && (ext == ".vert" || ext == ".frag")) {
            // not used by anything yet: check it if its pair exists
            const std::string base = (fs::path(file).parent_path() / fs::path(file).stem()).generic_string();
            std::error_code ec;
            if (fs::exists(project->getProjectPath() / (base + ".vert"), ec) && fs::exists(project->getProjectPath() / (base + ".frag"), ec) &&
                covered.insert(base + "#-1").second) {
                result.push_back({base, -1});
            }
        }
    }
    return result;
}

std::vector<editor::ProjectChecks::ShaderUse> editor::ProjectChecks::shaderForksInProject(Project* project) {
    std::vector<ShaderUse> result = shaderForksInUse(project);
    std::set<std::string> usedFiles;
    for (const ShaderUse& use : result) {
        Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(use.base);
        usedFiles.insert(paths.vert);
        usedFiles.insert(paths.frag);
    }
    const fs::path root = project->getProjectPath();
    for (const fs::path& vert : findProjectFiles(root, {".vert"})) {
        const std::string rel = projectRelative(root, vert);
        fs::path frag = vert;
        frag.replace_extension(".frag");
        std::error_code ec;
        if (usedFiles.count(rel) || usedFiles.count(projectRelative(root, frag)) || !fs::is_regular_file(frag, ec)) continue;
        result.push_back({(fs::path(rel).parent_path() / fs::path(rel).stem()).generic_string(), -1});
    }
    return result;
}

int editor::ProjectChecks::guessShaderType(Project* project, const std::string& base) {
    std::string frag;
    readFile(project->getProjectPath() / Util::resolveCustomShaderPaths(base).frag, frag);
    if (frag.find("u_fs_postParams") != std::string::npos || frag.find("postprocess") != std::string::npos) return static_cast<int>(ShaderType::POSTPROCESS);
    if (frag.find("includes/ui") != std::string::npos) return static_cast<int>(ShaderType::UI);
    if (frag.find("includes/sky") != std::string::npos) return static_cast<int>(ShaderType::SKYBOX);
    if (frag.find("includes/points") != std::string::npos) return static_cast<int>(ShaderType::POINTS);
    if (frag.find("includes/lines") != std::string::npos) return static_cast<int>(ShaderType::LINES);
    return static_cast<int>(ShaderType::MESH);
}

std::string editor::ProjectChecks::shaderTypeName(int type) {
    switch (static_cast<ShaderType>(type)) {
        case ShaderType::MESH: return "mesh";
        case ShaderType::UI: return "ui";
        case ShaderType::SKYBOX: return "sky";
        case ShaderType::POINTS: return "points";
        case ShaderType::LINES: return "lines";
        case ShaderType::DEPTH: return "depth";
        case ShaderType::POSTPROCESS: return "post_process";
        default: return "unknown";
    }
}

Json editor::ProjectChecks::parseShaderLog(const std::string& log) {
    // glslang: "ERROR: 0:12: 'x' : undeclared identifier" (also "<file>:12:")
    static const std::regex line(R"((ERROR|WARNING):\s*([^:\n]*):(\d+):\s*(.*))");
    Json errors = Json::array();
    std::istringstream in(log);
    std::string text;
    while (std::getline(in, text)) {
        std::smatch m;
        if (std::regex_search(text, m, line)) {
            errors.push_back({{"severity", m[1].str() == "ERROR" ? "error" : "warning"}, {"source", m[2].str()},
                              {"line", std::atoi(m[3].str().c_str())}, {"message", m[4].str()}});
        }
        if (errors.size() >= 30) break;
    }
    return errors;
}

void editor::ProjectChecks::invalidateShaderCaches(Project* project) {
    project->invalidateCustomShaders();
}

uint64_t editor::ProjectChecks::prepareShaderCheck(Project* project, ShaderUse& use) {
    if (use.shaderType < 0) use.shaderType = guessShaderType(project, use.base);
    const uint16_t customId = ShaderPool::registerCustomShader(use.base);
    return ShaderPool::getShaderKey(static_cast<ShaderType>(use.shaderType), 0, customId);
}

std::string editor::ProjectChecks::shaderSourcePath(Project* project, const std::string& base, const std::string& source) {
    const Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(base);
    if (source == paths.vert || source == paths.frag) return source;
    if (!ShaderVariantCompiler::isSafeRelativePath(source)) return source;
    const fs::path root = project->getProjectPath();
    std::error_code ec;
    // the include resolution of ShaderVariantCompiler::resolveCustomShaderSources
    const std::string includeRoot = ShaderVariantCompiler::getCustomShaderIncludeRoot(base);
    if (!includeRoot.empty() && includeRoot != "." && fs::is_regular_file(root / includeRoot / source, ec)) {
        return (fs::path(includeRoot) / source).lexically_normal().generic_string();
    }
    if (fs::is_regular_file(root / source, ec)) return fs::path(source).lexically_normal().generic_string();
    if (ShaderVariantCompiler::getLibrarySources().count(source)) return "engine/shaderlib/" + source;
    return source;
}

editor::ProjectChecks::ShaderCheck editor::ProjectChecks::runShaderCheck(Project* project, const ShaderUse& use, uint64_t shaderKey) {
    ShaderCheck check;
    check.use = use;
    const uint64_t before = Out::lastLogId();
    try {
        ShaderBuilder builder;
        builder.buildShaderForExport(shaderKey, project, ShaderPool::getShaderBackend());
        check.ok = true;
        return check;
    } catch (const std::exception& e) {
        check.ok = false;
        // (the editor may be building the same fork for the scene meanwhile: same message twice)
        std::string log;
        std::set<std::string> logged;
        for (const LogEntry& entry : Out::getLogsSince(before)) {
            if (entry.type == LogType::Error && entry.message.find("Shader") != std::string::npos && logged.insert(entry.message).second) {
                log += entry.message + "\n";
            }
        }
        if (log.empty()) log = e.what();
        if (log.size() > 6000) log = log.substr(0, 6000) + "\n...";
        check.errors = parseShaderLog(log);
        check.log = log;

        std::set<std::string> seen;
        for (const Json& error : check.errors) {
            const std::string message = error.value("message", std::string());
            if (message.find("compilation terminated") != std::string::npos) continue;
            std::string text = message;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            Finding finding{"shader", shaderSourcePath(project, use.base, error.value("source", std::string())),
                            error.value("line", 0), 0, error.value("severity", std::string("error")), text};
            if (seen.insert(finding.path + ":" + std::to_string(finding.line) + ":" + finding.message).second) check.findings.push_back(finding);
        }
        if (check.findings.empty()) {
            // no file:line (a link or cross-compile failure): the first line of the log
            std::string first;
            std::istringstream lines(log);
            std::string line;
            while (std::getline(lines, line) && first.empty()) {
                if (line.rfind("Shader ", 0) == 0 || line.rfind("File: ", 0) == 0) continue;
                if (line.rfind("ERROR: ", 0) == 0) line = line.substr(7);
                first = oneLine(line);
            }
            check.findings.push_back({"shader", Util::resolveCustomShaderPaths(use.base).frag, 0, 0, "error",
                                      first.empty() ? std::string(e.what()) : first});
        }
    }
    return check;
}

Json editor::ProjectChecks::toJson(const ShaderCheck& check) {
    Json item = {{"shader", check.use.base}, {"type", shaderTypeName(check.use.shaderType)}, {"ok", check.ok}};
    if (!check.ok) {
        item["errors"] = check.errors;
        item["log"] = check.log;
    }
    return item;
}
