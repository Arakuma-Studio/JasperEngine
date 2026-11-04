// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectChecks.h"
#include "component/ScriptComponent.h"
#include "io/AtomicFile.h"
#include "component/ActionComponent.h"
#include "component/PointsComponent.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleWorld.h"
#include "util/ParticleEffectEdit.h"
#include "shader/ShaderGlobals.h"
#include "util/FileUtils.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "Out.h"
#include "EditorVersion.h"
#include "Project.h"
#include "htmlui/HtmlUIDependencies.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"
#include "pool/ShaderPool.h"
#include "script/LuaBinding.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderVariantCompiler.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "util/ProjectUtils.h"
#include "util/ScriptParser.h"
#include "util/Util.h"
#include "texsynth/SynthesisRecipe.h"
#include "util/NormalMapTilt.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "subsystem/MeshSystem.h"

#include "lua.hpp"
#include "yaml-cpp/yaml.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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
namespace fx = ::jasperengine::htmlui;
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
    if (tool == "ui") return 3;
    // sprite normal maps after every other tool (texture, anim, vat)
    if (tool == "normals") return 5;
    return 4;
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
    Json out{{"tool", finding.tool}, {"path", finding.path}, {"line", finding.line}, {"col", finding.col},
             {"severity", finding.severity}, {"message", finding.message}};
    if (!finding.code.empty()) out["code"] = finding.code;
    if (!finding.suggestion.empty()) out["suggestion"] = finding.suggestion;
    return out;
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
    out += ": " + finding.severity + ": " + oneLine(finding.message);
    if (!finding.suggestion.empty()) out += " (fix: " + oneLine(finding.suggestion) + ")";
    if (!finding.code.empty()) out += " [" + finding.code + "]";
    return out;
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
    bool top = true;
    for (const fs::path& part : parent) {
        if (part == ".") continue;
        // at the project's top Recordings and EditorSettings hold no assets either (S27 R31)
        if (top ? Project::isNonAssetTopLevelDir(part.string()) : isSkippedDir(part)) return true;
        top = false;
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
    // the names the engine registered, not what scripts added since: readable from any thread
    return LuaBinding::getEngineGlobalNames();
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
    if (plan.scripts.empty() && project->getBuildScriptDirs().empty()) {
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

// ======================================================================== HTML/CSS UI

namespace {

namespace compat = jasperengine::htmlui::compat;

// The asset-relative path of an absolute path inside the assets directory ("" when outside)
std::string assetRelative(const fs::path& assetsRoot, const fs::path& absolute) {
    const fs::path rel = absolute.lexically_normal().lexically_relative(assetsRoot.lexically_normal());
    if (rel.empty() || rel == "." || *rel.begin() == "..") return {};
    return editor::FileUtils::pathToGenericUtf8(rel);
}

// compat's asset-relative path under the assets directory; empty when it is absolute or climbs
// out of it
fs::path assetFile(const fs::path& assetsRoot, const std::string& assetPath) {
    if (assetPath.empty() || assetPath[0] == '/' || assetPath[0] == '\\') return {};
    const fs::path rel = editor::FileUtils::pathFromUtf8(assetPath);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return {};
    const fs::path normal = rel.lexically_normal();
    if (normal.empty() || normal == "." || *normal.begin() == "..") return {};
    return assetsRoot / normal;
}

YAML::Node loadProjectYaml(const fs::path& projectRoot) {
    try {
        return YAML::LoadFile((projectRoot / "project.yaml").string());
    } catch (const std::exception&) {
        return YAML::Node();
    }
}

// The web export's custom HTML shell (project.yaml web.customHtmlShell, relative to the
// project), absolute; empty when there is none
fs::path webShellFile(const fs::path& projectRoot, const YAML::Node& node) {
    try {
        if (node && node["web"] && node["web"]["customHtmlShell"]) {
            const fs::path shell = editor::FileUtils::pathFromUtf8(node["web"]["customHtmlShell"].as<std::string>());
            if (!shell.empty()) return (shell.is_absolute() ? shell : projectRoot / shell).lexically_normal();
        }
    } catch (const std::exception&) {
    }
    return {};
}

// The canvas size documents load with (project.yaml canvasWidth/canvasHeight)
compat::CompatSize canvasSize(const YAML::Node& node) {
    compat::CompatSize size;
    try {
        if (node && node["canvasWidth"]) size.x = node["canvasWidth"].as<int>();
        if (node && node["canvasHeight"]) size.y = node["canvasHeight"].as<int>();
    } catch (const std::exception&) {
    }
    if (size.x <= 0 || size.y <= 0) size = compat::CompatSize();
    return size;
}

// ---- ui-support: the code a construct of a table row is reported with

struct RowCode {
    const compat::DiagCodeInfo* info = nullptr;
    compat::Severity severity = compat::Severity::Info;
};

RowCode rowCode(compat::DiagCode code) {
    RowCode out;
    out.info = compat::findDiagnosticCode(code);
    if (out.info) out.severity = out.info->severity;
    return out;
}

RowCode rowCode(compat::DiagCode code, compat::Severity severity) {
    RowCode out = rowCode(code);
    out.severity = severity;
    return out;
}

// The severity a row's note gives its diagnostic ("... (Warning)", "(Info, phase-3 spec 3.9)"):
// the first marker, which describes the engine as built (a note on a fork-dependent row puts
// the fork-present case first); `fallback` when the note has none
compat::Severity noteSeverity(const char* note, compat::Severity fallback) {
    const std::string_view text(note ? note : "");
    const size_t warning = text.find("(Warning");
    const size_t info = text.find("(Info");
    if (warning == std::string_view::npos && info == std::string_view::npos) return fallback;
    return warning < info ? compat::Severity::Warning : compat::Severity::Info;
}

// An approximated CSS row: approximated-visible (a Warning) or approximated-minor (an Info)
RowCode approximatedCode(const char* note, compat::Severity fallback) {
    return noteSeverity(note, fallback) == compat::Severity::Warning ? rowCode(compat::codes::ApproximatedVisible)
                                                                     : rowCode(compat::codes::ApproximatedMinor);
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool editor::ProjectChecks::isUiFile(const std::string& path) {
    const std::string ext = lowerExtension(editor::FileUtils::pathFromUtf8(path));
    return ext == ".html" || ext == ".htm" || ext == ".css";
}

fs::path editor::ProjectChecks::assetsRootFromProjectFile(const fs::path& projectRoot) {
    std::string assetsDir;
    const YAML::Node node = loadProjectYaml(projectRoot);
    try {
        if (node && node["assetsDir"]) assetsDir = node["assetsDir"].as<std::string>();
    } catch (const std::exception&) {
    }
    const fs::path dir = editor::FileUtils::pathFromUtf8(assetsDir);
    fs::path out;
    if (dir.empty() || dir == ".") out = projectRoot;
    else if (dir.is_absolute()) out = dir;
    else out = projectRoot / dir;
    out = out.lexically_normal();
    if (!out.empty() && out.filename().empty()) out = out.parent_path();
    return out;
}

compat::FileAccess editor::ProjectChecks::projectFiles(const fs::path& projectRoot) {
    const fs::path assets = assetsRootFromProjectFile(projectRoot.lexically_normal());
    compat::FileAccess access;
    access.read = [assets](const std::string& assetPath, std::string& content) {
        const fs::path file = assetFile(assets, assetPath);
        std::error_code ec;
        if (file.empty() || !fs::is_regular_file(file, ec)) return false;
        return readFile(file, content);
    };
    access.exists = [assets](const std::string& assetPath) {
        const fs::path file = assetFile(assets, assetPath);
        std::error_code ec;
        return !file.empty() && fs::is_regular_file(file, ec);
    };
    return access;
}

editor::ProjectChecks::Finding editor::ProjectChecks::uiFinding(const fs::path& projectRoot, const fs::path& assetsRoot,
                                                                const compat::Diagnostic& diagnostic) {
    Finding finding;
    finding.tool = "ui";
    if (!diagnostic.file.empty()) {
        const fs::path file = assetFile(assetsRoot, diagnostic.file);
        finding.path = file.empty() ? diagnostic.file : projectRelative(projectRoot, file);
    }
    finding.line = static_cast<int>(diagnostic.line);
    finding.col = static_cast<int>(diagnostic.column);
    finding.severity = compat::severityName(diagnostic.severity);
    finding.message = diagnostic.message;
    finding.code = diagnostic.code.id();
    finding.suggestion = diagnostic.suggestion;
    return finding;
}

editor::ProjectChecks::UiCheck editor::ProjectChecks::checkUiFiles(const fs::path& projectRootIn, const std::vector<fs::path>& files) {
    UiCheck check;
    const fs::path projectRoot = projectRootIn.lexically_normal();
    const fs::path assets = assetsRootFromProjectFile(projectRoot);
    const YAML::Node project = loadProjectYaml(projectRoot);
    const fs::path shell = webShellFile(projectRoot, project);

    // as the documents load (DocumentLoader's compile), without RmlUi: pure CPU
    compat::CheckOptions options;
    options.verifyWithRmlUi = false;
    options.files = projectFiles(projectRoot);
    options.compat.viewport = canvasSize(project);

    // what to check: documents first, then stylesheets
    std::vector<fs::path> documents, stylesheets;
    auto classify = [&](const fs::path& path) {
        const fs::path normal = path.lexically_normal();
        if (!shell.empty() && normal == shell) return;   // the web export's page, not a game UI
        if (assetRelative(assets, normal).empty()) return;
        const std::string ext = lowerExtension(normal);
        if (ext == ".html" || ext == ".htm") documents.push_back(normal);
        else if (ext == ".css") stylesheets.push_back(normal);
    };
    if (files.empty()) {
        for (const fs::path& file : findProjectFiles(projectRoot, {".html", ".htm", ".css"})) classify(file);
    } else {
        std::set<fs::path> seen;
        for (const fs::path& file : files) {
            if (seen.insert(file.lexically_normal()).second) classify(file);
        }
    }

    std::set<std::string> findingKeys, filesRead, readByDocuments;
    auto add = [&](const std::string& checked, const compat::CheckReport& report) {
        filesRead.insert(checked);
        for (const std::string& file : report.files) {
            const fs::path absolute = assetFile(assets, file);
            if (!absolute.empty()) filesRead.insert(projectRelative(projectRoot, absolute));
        }
        for (const compat::Diagnostic& diagnostic : report.diagnostics) {
            if (diagnostic.severity == compat::Severity::Info) continue;
            Finding finding = uiFinding(projectRoot, assets, diagnostic);
            const std::string key = finding.path + "\n" + std::to_string(finding.line) + "\n" + std::to_string(finding.col) +
                                    "\n" + finding.severity + "\n" + finding.code + "\n" + finding.message;
            if (findingKeys.insert(key).second) check.findings.push_back(std::move(finding));
        }
    };
    std::map<fs::path, compat::CheckReport> documentReports;
    for (const fs::path& document : documents) {
        const compat::CheckReport& report = documentReports[document] = compat::checkDocument(assetRelative(assets, document), options);
        check.documents++;
        for (const std::string& file : report.files) readByDocuments.insert(jasperengine::normalizeHtmlUIPath(file));
        add(projectRelative(projectRoot, document), report);
    }
    // A stylesheet named on its own is checked in the context of the project's documents that load
    // it (p4-gaps): only they know the custom properties it reads (a style attribute, another sheet),
    // so a variable a document defines is no finding and one no document defines is. A sheet no
    // document loads is checked alone, which does not judge the variables it does not declare.
    std::vector<fs::path> projectDocuments;
    bool projectDocumentsListed = false;
    auto documentsLoading = [&](const std::string& assetPath) {
        if (!projectDocumentsListed) {
            projectDocumentsListed = true;
            for (const fs::path& file : findProjectFiles(projectRoot, {".html", ".htm"})) {
                const fs::path normal = file.lexically_normal();
                if ((!shell.empty() && normal == shell) || assetRelative(assets, normal).empty()) continue;
                projectDocuments.push_back(normal);
            }
        }
        std::vector<const compat::CheckReport*> loading;
        const std::string wanted = jasperengine::normalizeHtmlUIPath(assetPath);
        for (const fs::path& document : projectDocuments) {
            auto found = documentReports.find(document);
            if (found == documentReports.end())
                found = documentReports.emplace(document, compat::checkDocument(assetRelative(assets, document), options)).first;
            for (const std::string& file : found->second.files) {
                if (jasperengine::normalizeHtmlUIPath(file) == wanted) {
                    loading.push_back(&found->second);
                    break;
                }
            }
        }
        return loading;
    };
    for (const fs::path& stylesheet : stylesheets) {
        const std::string assetPath = assetRelative(assets, stylesheet);
        // the whole project: a stylesheet a document links was checked as that document loads it
        if (files.empty() && readByDocuments.count(jasperengine::normalizeHtmlUIPath(assetPath))) continue;
        compat::CheckReport report = compat::checkStylesheet(assetPath, options);
        check.stylesheets++;
        const std::vector<const compat::CheckReport*> loading = files.empty() ? std::vector<const compat::CheckReport*>() : documentsLoading(assetPath);
        if (!loading.empty()) {
            // the findings in the sheet and the files it imports, as its documents load them
            std::set<std::string> own;
            for (const std::string& file : report.files) own.insert(jasperengine::normalizeHtmlUIPath(file));
            compat::CheckReport inContext;
            inContext.files = report.files;
            for (const compat::CheckReport* document : loading) {
                for (const compat::Diagnostic& diagnostic : document->diagnostics) {
                    if (own.count(jasperengine::normalizeHtmlUIPath(diagnostic.file))) inContext.diagnostics.push_back(diagnostic);
                }
            }
            report = std::move(inContext);
        }
        add(projectRelative(projectRoot, stylesheet), report);
    }
    check.files.assign(filesRead.begin(), filesRead.end());
    sortFindings(check.findings);
    return check;
}

Json editor::ProjectChecks::uiSupportJson() {
    using compat::Support;
    using compat::Severity;
    namespace codes = compat::codes;
    const compat::ForkFeatures present = compat::ForkFeatures::present();

    Json features = Json::array();
    auto add = [&](const char* kind, const std::string& name, Support support, const char* note, const char* suggestion,
                   const RowCode& code, Json extra = Json::object()) {
        Json item = {{"kind", kind}, {"name", name}, {"support", compat::supportName(support)},
                     {"note", note ? note : ""}, {"suggestion", suggestion ? suggestion : ""}};
        if (code.info) {
            item["code"] = code.info->code.id();
            item["severity"] = compat::severityName(code.severity);
        }
        for (auto it = extra.begin(); it != extra.end(); ++it) item[it.key()] = it.value();
        features.push_back(std::move(item));
    };

    for (const compat::ElementSupport& row : compat::elementTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        if (support == Support::Unsupported || support == Support::Ignored || support == Support::Fallback) {
            code = rowCode(codes::UnsupportedElement);
        } else if (support == Support::Approximated) {
            code = rowCode(codes::ElementMapped, noteSeverity(row.note, Severity::Info));
        }
        add("element", row.name, support, row.note, row.suggestion, code);
    }
    for (const compat::AttributeSupport& row : compat::attributeTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        if (std::string_view(row.name) == "on*") {
            code = rowCode(codes::EventHandlerAttribute);
        } else if (std::string_view(row.element) == "video" && support == Support::Ignored) {
            // the media attributes mapVideo drops (phase 6)
            code = rowCode(codes::MediaAttributeIgnored);
        } else if (support == Support::Unsupported || support == Support::Fallback) {
            code = rowCode(codes::UnsupportedAttribute, Severity::Warning);
        } else if (support == Support::Ignored || support == Support::Approximated) {
            code = rowCode(codes::UnsupportedAttribute, Severity::Info);
        }
        add("attribute", row.name, support, row.note, row.suggestion, code, {{"element", row.element}});
    }
    for (const compat::InputTypeSupport& row : compat::inputTypeTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        if (row.reported) code = rowCode(codes::InputTypeApproximated, row.severity);
        add("input_type", row.type, support, row.note, row.suggestion, code);
    }
    for (const compat::PropertySupport& row : compat::propertyTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        switch (support) {
            case Support::Unsupported: code = rowCode(codes::UnsupportedProperty, Severity::Warning); break;
            case Support::Ignored: code = rowCode(codes::IgnoredProperty); break;
            case Support::Approximated: code = approximatedCode(row.note, Severity::Warning); break;
            case Support::Fallback: code = rowCode(codes::ForkFallback, Severity::Warning); break;
            default: break;
        }
        add("property", row.name, support, row.note, row.suggestion, code);
    }
    for (const compat::ValueSupport& row : compat::valueTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        switch (support) {
            case Support::Unsupported: code = rowCode(codes::UnsupportedValue, Severity::Warning); break;
            case Support::Ignored: code = rowCode(codes::ApproximatedMinor); break;
            case Support::Approximated: code = approximatedCode(row.note, Severity::Warning); break;
            case Support::Fallback: code = rowCode(codes::ForkFallback, Severity::Warning); break;
            default: break;
        }
        add("value", row.value, support, row.note, row.suggestion, code, {{"property", row.property}});
    }
    for (const compat::FunctionSupport& row : compat::functionTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        switch (support) {
            case Support::Unsupported: code = rowCode(codes::UnsupportedFunction); break;
            case Support::Approximated: case Support::Ignored: code = approximatedCode(row.note, Severity::Info); break;
            case Support::Fallback: code = rowCode(codes::ForkFallback, Severity::Warning); break;
            default: break;
        }
        add("function", row.name, support, row.note, row.suggestion, code);
    }
    for (const compat::UnitSupport& row : compat::unitTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        switch (support) {
            case Support::Unsupported: code = rowCode(codes::UnsupportedUnit); break;
            case Support::Approximated: case Support::Ignored: code = approximatedCode(row.note, Severity::Info); break;
            case Support::Fallback: code = rowCode(codes::ForkFallback, Severity::Warning); break;
            default: break;
        }
        add("unit", row.unit, support, row.note, row.suggestion, code);
    }
    for (const compat::SelectorSupport& row : compat::selectorTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        switch (support) {
            case Support::Unsupported: code = rowCode(codes::UnsupportedSelector); break;
            case Support::Ignored: code = rowCode(codes::SelectorIgnored); break;
            case Support::Approximated: code = rowCode(codes::SelectorRewritten, noteSeverity(row.note, Severity::Info)); break;
            case Support::Fallback:
                code = rowCode(row.kind == compat::SelectorKind::PseudoElement ? codes::PseudoElementPending : codes::UnsupportedSelector);
                break;
            default: break;
        }
        add(compat::selectorKindName(row.kind), row.name, support, row.note, row.suggestion, code);
    }
    for (const compat::AtRuleSupport& row : compat::atRuleTable()) {
        const Support support = compat::effectiveSupport(row, present);
        const std::string_view name(row.name);
        RowCode code;
        if (support == Support::Unsupported || support == Support::Fallback) {
            code = rowCode(codes::UnsupportedAtRule, Severity::Warning);
        } else if (support == Support::Ignored) {
            code = rowCode(codes::UnsupportedAtRule, Severity::Info);
        } else if (support == Support::Approximated) {
            if (startsWith(name, "@layer")) code = rowCode(codes::LayerFlattened);
            else if (startsWith(name, "@supports")) code = rowCode(codes::SupportsEvaluated);
            else if (startsWith(name, "@property")) code = rowCode(codes::PropertyApproximated);
            else if (startsWith(name, "@import")) code = rowCode(codes::ImportProblem, Severity::Info);
            else if (startsWith(name, "@keyframes")) code = rowCode(codes::AnimationApproximated, Severity::Info);
            else if (startsWith(name, "@font-face")) code = rowCode(codes::FontFaceDescriptor);
            else if (startsWith(name, "@media")) code = rowCode(codes::MediaNormalized);
            else code = rowCode(codes::UnsupportedAtRule, noteSeverity(row.note, Severity::Info));
        }
        add("at_rule", row.name, support, row.note, row.suggestion, code);
    }
    for (const compat::MediaFeatureSupport& row : compat::mediaFeatureTable()) {
        const Support support = compat::effectiveSupport(row, present);
        RowCode code;
        if (support == Support::Unsupported || support == Support::Fallback) code = rowCode(codes::MediaFeatureUnsupported);
        else if (support == Support::Approximated || support == Support::Ignored) code = rowCode(codes::MediaStatic);
        add("media_feature", row.name, support, row.note, row.suggestion, code);
    }

    Json diagnostics = Json::array();
    for (const compat::DiagCodeInfo& info : compat::diagnosticCodes()) {
        // severity: the default one; by_case: the reporter picks it (Appendix F "by case")
        diagnostics.push_back({{"code", info.code.id()}, {"name", info.name}, {"severity", compat::severityName(info.severity)},
                               {"by_case", info.severityByCase}, {"summary", info.title ? info.title : ""},
                               {"help", info.help ? info.help : ""}});
    }
    return {{"version", 1}, {"editor_version", JASPERENGINE_EDITOR_VERSION}, {"features", features}, {"diagnostics", diagnostics}};
}

// ======================================================================== HTML UI effects

namespace {

    // a parameter's range as written in manifests ("-180deg..180deg", "0%..100%")
    std::string effectRangeText(const fx::EffectParamDef& def) {
        if (def.type == fx::EffectParamType::Enum)
            return "";
        if (!def.hasRange)
            return "";
        auto number = [](double v) {
            char text[64];
            std::snprintf(text, sizeof(text), "%.6g", v);
            return std::string(text);
        };
        auto value = [&](float v) {
            switch (def.type) {
                case fx::EffectParamType::Angle: return number(v * 180.0 / 3.14159265358979323846) + "deg";
                case fx::EffectParamType::Time: return number(v) + "s";
                case fx::EffectParamType::Length: return number(v) + "px";
                case fx::EffectParamType::Percentage: return number(v * 100.0) + "%";
                default: return number(v);
            }
        };
        return value(def.minValue) + ".." + value(def.maxValue);
    }

    // one CSS declaration using the effect (its first simple parameter at its default)
    std::string effectExampleOf(const fx::EffectManifest& m) {
        std::string call = "effect(" + m.name;
        for (const fx::EffectParamDef& def : m.params) {
            if (def.type == fx::EffectParamType::Palette || def.type == fx::EffectParamType::Position ||
                def.type == fx::EffectParamType::Color || def.defaultText.empty())
                continue;
            call += ", " + def.name + " " + def.defaultText;
            break;
        }
        call += ")";
        if (m.kind == fx::EffectKind::Paint)
            return "background-image: " + call + ";";
        return std::string(m.backdrop ? "backdrop-filter: " : "filter: ") + call + ";";
    }

    nlohmann::json effectJsonOf(const fx::EffectManifest& m) {
        nlohmann::json params = nlohmann::json::array();
        for (const fx::EffectParamDef& def : m.params) {
            nlohmann::json p = {{"name", def.name}, {"type", fx::effectParamTypeName(def.type)}, {"default", def.defaultText},
                                {"range", effectRangeText(def)}, {"description", def.description}, {"uniform", def.uniform}};
            if (def.hasRange && def.type != fx::EffectParamType::Enum) {
                p["min"] = def.minValue;
                p["max"] = def.maxValue;
            }
            if (!def.options.empty())
                p["options"] = def.options;
            params.push_back(std::move(p));
        }
        auto scaled = [](const fx::EffectScaledParam& s) {
            if (s.param.empty())
                return nlohmann::json(std::to_string(s.addPx) + "px");
            return nlohmann::json({{"param", s.param}, {"scale", s.scale}, {"addPx", s.addPx}});
        };
        nlohmann::json entry = {
            {"name", m.name}, {"kind", fx::effectKindName(m.kind)}, {"category", m.category}, {"summary", m.summary},
            {"source", m.source}, {"cost", fx::effectCostName(m.cost)}, {"backdrop", m.backdrop},
            {"pointer", fx::effectPointerName(m.pointer)}, {"time", m.timeParams}, {"suggestFor", m.suggestFor},
            {"params", params}, {"builtin", m.builtin}, {"path", m.builtin ? "engine/shaderlib/" + m.sourcePath : m.sourcePath},
            {"example", effectExampleOf(m)},
            {"thumbnail", m.builtin ? "docs/mcp/guide/img/effects/" + m.name + ".png" : std::string()}};
        if (m.hasPrepass) {
            entry["prepass"] = {{"blur", scaled(m.prepassSigma)}, {"threshold", m.prepassThreshold}};
        }
        if (!m.margin.param.empty() || m.margin.addPx != 0.f)
            entry["margin"] = scaled(m.margin);
        if (m.boundsGeometry)
            entry["geometry"] = {{"bounds", scaled(m.boundsExtent)}};
        // phase 6: a stateful effect's images and passes
        if (m.stateful()) {
            nlohmann::json states = nlohmann::json::array();
            for (const fx::EffectStateDef& st : m.states) {
                std::string size;
                if (st.fixedSize) {
                    size = std::to_string(st.width) + "x" + std::to_string(st.height);
                } else if (st.aspectShort > 0) {
                    size = "aspect " + std::to_string(st.aspectShort) + " max " + std::to_string(st.boxMax);
                } else {
                    char scale[32];
                    std::snprintf(scale, sizeof(scale), "%g", st.boxScale);
                    size = std::string("box") + (st.boxScale != 1.f ? std::string("*") + scale : std::string()) + " max " + std::to_string(st.boxMax);
                }
                states.push_back({{"name", st.name}, {"format", fx::effectStateFormatName(st.format)}, {"size", size},
                                  {"filter", st.linear ? "linear" : "nearest"}, {"wrap", st.repeat ? "repeat" : "clamp"},
                                  {"resize", st.resample ? "resample" : "clear"}, {"images", st.single ? 1 : 2}});
            }
            nlohmann::json passes = nlohmann::json::array();
            for (const fx::EffectPassDef& pass : m.passes) {
                nlohmann::json p = {{"name", pass.name}, {"state", m.states[(size_t)pass.state].name}, {"init", pass.init}};
                if (pass.frame) p["frame"] = true;
                if (!pass.ifParam.empty()) p["if"] = pass.ifParam;
                if (!pass.repeatParam.empty()) p["repeat"] = pass.repeatParam;
                else if (pass.repeat > 1) p["repeat"] = pass.repeat;
                if (pass.additive) p["add"] = true;
                if (pass.scatterState >= 0) p["scatter"] = m.states[(size_t)pass.scatterState].name;
                passes.push_back(std::move(p));
            }
            nlohmann::json state = {{"states", states}, {"passes", passes}, {"rate", m.rateHz}, {"max_substeps", m.maxSubsteps},
                                    {"replay", m.replaySeconds}, {"reseed", m.reseedParams}};
            if (m.warmupSteps > 0)
                state["warmup"] = {{"steps", m.warmupSteps}, {"dt", m.warmupDt}};
            entry["state"] = std::move(state);
        }
        // @preset: the options of the `preset` parameter and the values each one sets
        if (!m.presets.empty()) {
            nlohmann::json presets = nlohmann::json::object();
            for (const fx::EffectPresetDef& preset : m.presets) {
                nlohmann::json values = nlohmann::json::object();
                for (const auto& value : preset.values) values[value.first] = value.second;
                presets[preset.option] = std::move(values);
            }
            entry["presets"] = std::move(presets);
        }
        return entry;
    }

}

Json editor::ProjectChecks::uiEffectJson(const fx::EffectManifest& manifest) {
    return effectJsonOf(manifest);
}

std::string editor::ProjectChecks::uiEffectExample(const fx::EffectManifest& manifest) {
    return effectExampleOf(manifest);
}

// ======================================================================== shaders

bool editor::ProjectChecks::shaderTypeForComponent(ComponentType component, ShaderType& out) {
    switch (component) {
        case ComponentType::MeshComponent: out = ShaderType::MESH; return true;
        case ComponentType::UIComponent: out = ShaderType::UI; return true;
        case ComponentType::PointsComponent: out = ShaderType::POINTS; return true;
        case ComponentType::LineRendererComponent: out = ShaderType::LINES; return true;
        case ComponentType::TrailRendererComponent: out = ShaderType::LINES; return true;
        case ComponentType::SkyboxComponent: out = ShaderType::SKYBOX; return true;
        default: return false;
    }
}

namespace {

std::string forkKey(const editor::ProjectChecks::ShaderUse& use) {
    return use.base + "#" + std::to_string(use.shaderType) + "#" + std::to_string(use.properties);
}

bool isUiEffectUse(const editor::ProjectChecks::ShaderUse& use) {
    return use.shaderType == static_cast<int>(ShaderType::UI_EFFECT);
}

} // namespace

std::vector<editor::ProjectChecks::ShaderUse> editor::ProjectChecks::shaderForksInUse(Project* project) {
    std::vector<ShaderUse> uses;
    std::set<std::pair<std::string, int>> seen;
    auto add = [&](std::string base, ShaderType type) {
        if (base.empty()) return;
        // a bare library reference (a customShader, a scene default) is checked as the variant
        // the engine draws from it: its default features and keyword values, and for a colour
        // program the blend define of its default state
        if (ShaderReference::isLibrary(base) && ShaderReference::getFeatures(base).empty()) {
            base = (type == ShaderType::DEPTH) ? ShaderReference::getVariantReference(base, {})
                                               : ShaderReference::getColorVariantReference(base, {}, MaterialBlendMode::AUTO);
        }
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
            // shader library: each submesh's shader and depth program with the features its
            // material enables, and the components' shaders with theirs, as they are compiled
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
                const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
                for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                    add(RenderSystem::getSubmeshShaderReference(*mesh, mesh->submeshes[s], sceneMeshShader), ShaderType::MESH);
                    add(RenderSystem::getSubmeshDepthShaderReference(*mesh, mesh->submeshes[s], sceneMeshShader), ShaderType::DEPTH);
                    // and its material's extra pass
                    if (RenderSystem::hasExtraPass(mesh->submeshes[s], scene->findComponent<TerrainComponent>(entity) != nullptr))
                        add(RenderSystem::getSubmeshExtraPassReference(mesh->submeshes[s]), ShaderType::MESH);
                }
            }
            auto addComponent = [&](const std::string& customShader, const std::vector<std::string>& features, ShaderType componentType,
                                    MaterialBlendMode blendMode) {
                // as the component loads it: its features and blend mode (forks without a manifest: the path)
                if (!customShader.empty()) add(RenderSystem::getComponentShaderReference(customShader, features, blendMode), componentType);
            };
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity)) addComponent(ui->customShader, ui->shaderFeatures, ShaderType::UI, ui->blendMode);
            if (PointsComponent* points = scene->findComponent<PointsComponent>(entity)) addComponent(points->customShader, points->shaderFeatures, ShaderType::POINTS, points->blendMode);
            if (LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)) addComponent(lines->customShader, lines->shaderFeatures, ShaderType::LINES, lines->blendMode);
            if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity)) addComponent(trail->customShader, trail->shaderFeatures, ShaderType::LINES, trail->blendMode);
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity)) addComponent(sky->customShader, sky->shaderFeatures, ShaderType::SKYBOX, MaterialBlendMode::AUTO);
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
    auto addUses = [&](const std::vector<ShaderUse>& uses) {
        for (const ShaderUse& use : uses) {
            if (covered.insert(forkKey(use)).second) result.push_back(use);
        }
    };
    for (const std::string& file : files) {
        const std::string ext = lowerExtension(file);
        if (ext == ".glsl") {
            // an include can be reached by any fork (private copies, project-root overrides)
            // and by any UI effect
            addUses(forks);
            for (const std::string& effect : uiEffectFiles(project)) addUses(uiEffectUses(project, effect));
            continue;
        }
        // a UI effect: its own forms (not a fork's half)
        if (ext == ".frag" && isUiEffectFile(project->getProjectPath() / editor::FileUtils::pathFromUtf8(file))) {
            addUses(uiEffectUses(project, file));
            continue;
        }
        bool found = false;
        for (const ShaderUse& use : forks) {
            // the files it resolves to (an assets-relative fork path: under the assets directory)
            Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(use.base);
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
        Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(use.base);
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
    for (const std::string& effect : uiEffectFiles(project)) {
        for (const ShaderUse& use : uiEffectUses(project, effect)) result.push_back(use);
    }
    return result;
}

bool editor::ProjectChecks::isUiEffectFile(const fs::path& absolute) {
    if (lowerExtension(absolute.generic_string()) != ".frag") return false;
    std::string source;
    return readFile(absolute, source) && fx::EffectManifestParser::hasEffectManifest(source);
}

std::vector<editor::ProjectChecks::ShaderUse> editor::ProjectChecks::uiEffectUses(Project* project, const std::string& relativeFile) {
    const fs::path root = project->getProjectPath();
    const fs::path file = (root / editor::FileUtils::pathFromUtf8(relativeFile)).lexically_normal();
    // the name @effect registers: the asset path
    std::string base = fs::path(relativeFile).generic_string();
    const fs::path inAssets = file.lexically_relative(project->getAssetsPath().lexically_normal());
    if (!inAssets.empty() && *inAssets.begin() != ".." && !inAssets.is_absolute()) base = inAssets.generic_string();
    std::string source;
    readFile(file, source);
    fx::EffectManifest manifest;
    std::vector<fx::EffectDiagnostic> diags;
    const int type = static_cast<int>(ShaderType::UI_EFFECT);
    std::vector<ShaderUse> uses;
    const bool parsed = fx::EffectManifestParser::parse(source, relativeFile, false, manifest, diags);
    for (uint32_t drawForm = (parsed && manifest.kind == fx::EffectKind::Paint) ? 0u : 1u; drawForm <= 1u; drawForm++) {
        uses.push_back({base, type, drawForm});
        // phase 6: a stateful effect's step (and scatter) forms
        if (parsed)
            for (uint32_t form : fx::EffectManifestParser::passForms(manifest, drawForm))
                uses.push_back({base, type, form});
    }
    return uses;
}

std::vector<std::string> editor::ProjectChecks::uiEffectFiles(Project* project) {
    std::vector<std::string> files;
    const fs::path root = project->getProjectPath();
    for (const fs::path& frag : findProjectFiles(root, {".frag"})) {
        if (isUiEffectFile(frag)) files.push_back(projectRelative(root, frag));
    }
    return files;
}

fs::path editor::ProjectChecks::uiEffectFile(Project* project, const std::string& base) {
    std::error_code ec;
    const fs::path inAssets = (project->getAssetsPath() / editor::FileUtils::pathFromUtf8(base)).lexically_normal();
    if (fs::is_regular_file(inAssets, ec)) return inAssets;
    return (project->getProjectPath() / editor::FileUtils::pathFromUtf8(base)).lexically_normal();
}

int editor::ProjectChecks::guessShaderType(Project* project, const std::string& base) {
    if (lowerExtension(base) == ".frag" && isUiEffectFile(uiEffectFile(project, base))) return static_cast<int>(ShaderType::UI_EFFECT);
    // a manifest says what it is
    if (std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base)) {
        if (manifest->hasType) return static_cast<int>(manifest->type);
    }
    std::string frag;
    project->readShaderFile(Util::resolveCustomShaderPaths(base).frag, frag);
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
        case ShaderType::UI_EFFECT: return "ui_effect";
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

ShaderKey editor::ProjectChecks::prepareShaderCheck(Project* project, ShaderUse& use) {
    if (use.shaderType < 0) use.shaderType = guessShaderType(project, use.base);
    const uint16_t customId = ShaderPool::registerCustomShader(use.base);
    // a UI effect: its form (use.properties)
    uint64_t properties = use.properties;
    if (use.shaderType == static_cast<int>(ShaderType::MESH)) {
        // a mesh shader with a manifest: the variant a mesh with UVs, normals and colours
        // draws with in its default lighting mode (lit modes in a scene with lights), so
        // what the lighting enables (textures sampled per pixel, the lights) is checked
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(use.base);
        if (manifest && manifest->present) {
            SubmeshShaderInputs inputs;
            inputs.texCoord1 = true;
            inputs.normal = true;
            inputs.vertexColor4 = true;
            inputs.manifest = manifest;
            // the features of the reference decide its feature= @require inputs
            inputs.features = ShaderReference::getFeatures(use.base);
            MeshShaderEnvironment environment;
            environment.lights = manifest->getDefaultLighting() != ShaderLightingMode::NONE;
            properties = RenderSystem::getSubmeshShaderVariant(environment, inputs).properties;
        }
    }
    return ShaderPool::getShaderKey(static_cast<ShaderType>(use.shaderType), properties, customId);
}

std::vector<editor::ProjectChecks::Finding> editor::ProjectChecks::sceneInputFindings(Project* project, const std::vector<ShaderUse>* forks) {
    std::vector<Finding> findings;
    // the references the forks were collected as (shaderForksInUse: the submeshes' colour
    // references and their extra passes')
    std::set<std::string> checked;
    if (forks) {
        for (const ShaderUse& use : *forks) checked.insert(use.base);
    }
    const fs::path root = project->getProjectPath();
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        const std::string path = sceneProject->filepath.empty() ? std::string() : projectRelative(root, root / sceneProject->filepath);
        const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
        for (Entity entity : sceneProject->entities) {
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || scene->findComponent<TerrainComponent>(entity)) continue;
            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                bool opaqueQueue = false;
                const uint32_t inputs = RenderSystem::getSubmeshSceneInputs(*mesh, mesh->submeshes[s], sceneMeshShader, opaqueQueue);
                if (!inputs || !opaqueQueue) continue;
                if (forks && !checked.count(RenderSystem::getSubmeshShaderReference(*mesh, mesh->submeshes[s], sceneMeshShader)) &&
                        !(RenderSystem::hasExtraPass(mesh->submeshes[s], false) &&
                          checked.count(RenderSystem::getSubmeshExtraPassReference(mesh->submeshes[s])))) continue;
                const std::string what = (inputs == ShaderRequire_SceneInputs) ? "depth and colour" : (inputs & ShaderRequire_SceneDepth) ? "depth" : "colour";
                findings.push_back({"shader", path, 0, 0, "error",
                    "GameObject '" + scene->getEntityName(entity) + "' submesh " + std::to_string(s) + ": its shader reads the scene " + what +
                    " but it draws in the opaque queue (it would read its own depth and a black scene colour); " +
                    Catalog::getSceneInputQueueAdvice(Catalog::getSceneInputQueueFix(scene, entity, s), s)});
            }
        }
    }
    return findings;
}

std::string editor::ProjectChecks::shaderSourcePath(Project* project, const std::string& base, const std::string& source) {
    // a library shader's files are all in the engine library
    if (ShaderReference::isLibrary(base)) {
        return ShaderVariantCompiler::getLibrarySources().count(source) ? "engine/shaderlib/" + source : source;
    }
    // the files as the compiler's reader found them (Project::findShaderFile), project-relative
    const Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(base);
    if (source == paths.vert || source == paths.frag) return project->resolveShaderFile(source);
    if (!ShaderVariantCompiler::isSafeRelativePath(source)) return source;
    const fs::path projectRoot = project->getProjectPath();
    const fs::path assetsRoot = project->getAssetsPath();
    // the include resolution of ShaderVariantCompiler::resolveCustomShaderSources
    const std::string includeRoot = ShaderVariantCompiler::getCustomShaderIncludeRoot(base);
    if (!includeRoot.empty() && includeRoot != ".") {
        const std::string local = (fs::path(includeRoot) / source).lexically_normal().generic_string();
        if (!Project::findShaderFile(projectRoot, assetsRoot, local).empty()) return project->resolveShaderFile(local);
    }
    if (!Project::findShaderFile(projectRoot, assetsRoot, source).empty()) return project->resolveShaderFile(source);
    if (ShaderVariantCompiler::getLibrarySources().count(source)) return "engine/shaderlib/" + source;
    return source;
}

namespace {

// A UI effect's include as a project file (the include resolution of a fragment-only custom
// shader: its directory, the project root, the engine library), project-relative
std::string uiEffectSourcePath(editor::Project* project, const editor::ProjectChecks::ShaderUse& use, const std::string& source) {
    const fs::path root = project->getProjectPath();
    const fs::path frag = editor::ProjectChecks::uiEffectFile(project, use.base);
    // glslang names the entry point by its key, or by a number ("0:12") when it has no name
    const bool numbered = !source.empty() && std::all_of(source.begin(), source.end(), [](unsigned char c) { return std::isdigit(c); });
    if (source == use.base || source.empty() || numbered) return editor::ProjectChecks::projectRelative(root, frag);
    if (!ShaderVariantCompiler::isSafeRelativePath(source)) return source;
    std::error_code ec;
    for (const fs::path& dir : {frag.parent_path(), root, project->getAssetsPath()}) {
        if (fs::is_regular_file(dir / editor::FileUtils::pathFromUtf8(source), ec)) {
            return editor::ProjectChecks::projectRelative(root, (dir / editor::FileUtils::pathFromUtf8(source)).lexically_normal());
        }
    }
    if (ShaderVariantCompiler::getLibrarySources().count(source)) return "engine/shaderlib/" + source;
    return source;
}

// The manifest of a UI effect use: false with the findings (HTMLUI-FX-0014) when it is wrong
bool checkUiEffectManifest(editor::Project* project, const editor::ProjectChecks::ShaderUse& use, fx::EffectManifest& manifest,
                           std::vector<editor::ProjectChecks::Finding>& findings) {
    const fs::path root = project->getProjectPath();
    const fs::path file = editor::ProjectChecks::uiEffectFile(project, use.base);
    const std::string rel = editor::ProjectChecks::projectRelative(root, file);
    std::string source;
    if (!editor::ProjectChecks::readFile(file, source)) {
        findings.push_back({"shader", rel, 0, 0, "error", "the effect file is missing",
                            fx::compat::codes::EffectFileMissing.id(), "restore it or remove its @effect"});
        return false;
    }
    std::vector<fx::EffectDiagnostic> diags;
    bool ok = fx::EffectManifestParser::parse(source, rel, false, manifest, diags);
    if (ok) ok = fx::EffectManifestParser::validate(manifest, source, diags);
    for (const fx::EffectDiagnostic& d : diags) {
        const bool error = d.severity == fx::compat::Severity::Error;
        if (error) ok = false;
        if (d.severity == fx::compat::Severity::Info) continue;
        findings.push_back({"shader", rel, static_cast<int>(d.line), static_cast<int>(d.column), error ? "error" : "warning", d.message,
                            d.code.id(), d.suggestion});
    }
    if (ok) {
        const fx::EffectManifest* builtin = fx::EffectRegistry::get().find(manifest.name);
        if (builtin && builtin->builtin) {
            findings.push_back({"shader", rel, manifest.line, 1, "error", "'" + manifest.name + "' is the name of a built-in effect",
                                fx::compat::codes::EffectDeclareConflict.id(), "rename the effect (@effect and the file)"});
            ok = false;
        }
    }
    return ok;
}

} // namespace

editor::ProjectChecks::ShaderCheck editor::ProjectChecks::runShaderCheck(Project* project, const ShaderUse& use, ShaderKey shaderKey, ShaderData* built) {
    ShaderCheck check;
    check.use = use;
    const bool effect = isUiEffectUse(use);
    fx::EffectManifest manifest;
    if (effect && !checkUiEffectManifest(project, use, manifest, check.findings)) {
        check.ok = false;
        for (const Finding& finding : check.findings) {
            if (finding.severity != "error") continue;
            check.errors.push_back({{"severity", "error"}, {"source", finding.path}, {"line", finding.line}, {"message", finding.message}});
            check.log += toText(finding) + "\n";
        }
        return check;
    }
    const uint64_t before = Out::lastLogId();
    try {
        ShaderBuilder builder;
        ShaderData data = builder.buildShaderForExport(shaderKey, project, ShaderPool::getShaderBackend());
        check.ok = true;
        if (built) *built = data;
        // a fork written before the fragment-stage shadows (docs/rendering/shadows.md): it still
        // works for directional and spot lights, its point lights cast no shadows. Read from its
        // sources: the blocks are only in the variants that receive shadows.
        if (!effect && !ShaderReference::isLibrary(use.base) && use.shaderType == static_cast<int>(ShaderType::MESH)) {
            const Util::CustomShaderPaths files = project->resolveCustomShaderFiles(use.base);
            for (const std::string& file : {files.frag, files.vert}) {
                std::string source;
                if (!file.empty() && readFile(project->getProjectPath() / file, source) &&
                        (source.find("u_fs_shadows") != std::string::npos || source.find("v_lightProjPos") != std::string::npos)) {
                    check.findings.push_back({"shader", file, 0, 0, "warning",
                                              "legacy shadow interface: re-fork to get point shadows, blend and PCSS"});
                    break;
                }
            }
        }
        if (effect) {
            // the parameter block the shader declares is the manifest's
            std::vector<std::string> mismatches;
            if (!fx::EffectManifestParser::checkReflection(manifest, data, mismatches, use.properties)) {
                check.ok = false;
                const std::string rel = projectRelative(project->getProjectPath(), uiEffectFile(project, use.base));
                for (const std::string& mismatch : mismatches) {
                    // at the @param line of the parameter it names, else at the manifest
                    int line = manifest.line;
                    for (const fx::EffectParamDef& param : manifest.params) {
                        if (param.line > 0 && (mismatch.find("'" + param.name + "'") != std::string::npos ||
                                               mismatch.find("'" + param.uniform + "'") != std::string::npos)) {
                            line = param.line;
                            break;
                        }
                    }
                    check.findings.push_back({"shader", rel, line, 1, "error", mismatch, fx::compat::codes::EffectManifest.id(),
                                              "u_fs_effectParams declares one member per @param, of its type"});
                    check.errors.push_back({{"severity", "error"}, {"source", rel}, {"line", line}, {"message", mismatch}});
                    check.log += rel + ":" + std::to_string(line) + ": " + mismatch + "\n";
                }
            }
        }
        if (!effect) {
            // the global shader values it reads, against Project Settings > Graphics > Shader
            // Globals, at its fragment shader (vertex when only the vertex stage reads them)
            const Util::CustomShaderPaths paths = ShaderReference::isLibrary(use.base) ? Util::CustomShaderPaths()
                                                                                        : project->resolveCustomShaderFiles(use.base);
            const std::string file = ShaderReference::isLibrary(use.base)
                ? "engine/shaderlib/library/" + ShaderReference::getLibraryName(use.base) + ".frag"
                : (paths.frag.empty() ? paths.vert : paths.frag);
            for (const ShaderGlobalDiagnostic& d : ShaderGlobals::checkShader(data, project->getShaderGlobals())) {
                check.findings.push_back({"shader", file, 0, 0, d.error ? "error" : "warning", d.code + ": " + d.message, d.code});
                if (!d.error) continue;
                check.ok = false;
                check.errors.push_back({{"severity", "error"}, {"source", file}, {"line", 0}, {"message", d.code + ": " + d.message}});
                check.log += file + ": error: " + d.code + ": " + d.message + "\n";
            }
            // sokol binds at most 8 uniform blocks a stage (a globals block counts)
            for (const ShaderStage& stage : data.stages) {
                if (stage.uniformblocks.size() <= 8) continue;
                const std::string message = std::string("uniform-block-limit: the ") + (stage.type == ShaderStageType::VERTEX ? "vertex" : "fragment") +
                    " stage declares " + std::to_string(stage.uniformblocks.size()) + " uniform blocks, over the 8 a stage binds";
                check.findings.push_back({"shader", file, 0, 0, "error", message, "uniform-block-limit"});
                check.ok = false;
                check.errors.push_back({{"severity", "error"}, {"source", file}, {"line", 0}, {"message", message}});
                check.log += file + ": error: " + message + "\n";
            }
        }
        if (!effect && use.shaderType != static_cast<int>(ShaderType::DEPTH)) {
            // a shader-library manifest (a UI effect's comment header is its own manifest,
            // checked above): its syntax, and its @property/@texture rows against what compiled
            for (const ShaderManifestDiagnostic& d : validateShaderManifest(use.base, {&data}, true)) {
                // a fork's manifest names its files as referenced: report them where they are
                const std::string file = shaderSourcePath(project, use.base, d.file);
                check.findings.push_back({"shader", file, d.line, 0, d.error ? "error" : "warning", "manifest: " + d.message});
                if (!d.error) continue;
                check.ok = false;
                check.errors.push_back({{"severity", "error"}, {"source", file}, {"line", d.line}, {"message", "manifest: " + d.message}});
                check.log += file + ":" + std::to_string(d.line) + ": error: manifest: " + d.message + "\n";
            }
        }
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
        if (effect) {
            // a UI effect's sources by their project paths (the compiler names the asset path)
            for (Json& error : check.errors) error["source"] = uiEffectSourcePath(project, use, error.value("source", std::string()));
        }

        std::set<std::string> seen;
        for (const Json& error : check.errors) {
            const std::string message = error.value("message", std::string());
            if (message.find("compilation terminated") != std::string::npos) continue;
            std::string text = message;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            const std::string source = error.value("source", std::string());
            Finding finding{"shader", effect ? source : shaderSourcePath(project, use.base, source),
                            error.value("line", 0), 0, error.value("severity", std::string("error")), text};
            if (effect) finding.code = fx::compat::codes::EffectShaderCompile.id();
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
            const std::string entry = effect ? uiEffectSourcePath(project, use, use.base)
                : ShaderReference::isLibrary(use.base)
                ? "engine/shaderlib/library/" + ShaderReference::getLibraryName(use.base) + ".frag"
                : project->resolveCustomShaderFiles(use.base).frag;
            // a globals block over its size fails the compile with its own code
            const std::string what = e.what();
            const bool globalsTooLarge = what.rfind("globals-block-too-large", 0) == 0;
            check.findings.push_back({"shader", entry, 0, 0, "error", globalsTooLarge ? what : (first.empty() ? what : first),
                                      effect ? fx::compat::codes::EffectShaderCompile.id() : globalsTooLarge ? std::string("globals-block-too-large") : std::string()});
        }
    }
    return check;
}

std::vector<ShaderManifestDiagnostic> editor::ProjectChecks::validateShaderManifest(const std::string& reference,
                                                                                   const std::vector<const ShaderData*>& variants,
                                                                                   bool onlyEnabledFeatures) {
    std::shared_ptr<const ShaderManifest> cached = ShaderManifests::get(reference);
    if (!cached || !cached->present) return {};
    ShaderManifest manifest = *cached;
    // the file the rows checked against reflection are reported in (the manifest's .frag)
    const std::string file = ShaderReference::isLibrary(reference)
        ? "engine/shaderlib/library/" + ShaderReference::getLibraryName(reference) + ".frag"
        : Util::resolveCustomShaderPaths(reference).frag;
    if (onlyEnabledFeatures) {
        // a row behind a feature the compiled variant does not enable is not in it. The
        // reference is the resolved variant, its defines exactly what was compiled: resolving
        // it again would turn back on a default-on feature the variant has off
        const std::vector<std::string> enabled = ShaderReference::getFeatures(reference);
        auto off = [&enabled](const std::string& feature) {
            return !ShaderManifest::isRowShown(feature, enabled);
        };
        manifest.properties.erase(std::remove_if(manifest.properties.begin(), manifest.properties.end(),
            [&](const ShaderManifestProperty& p) { return off(p.feature); }), manifest.properties.end());
        manifest.textures.erase(std::remove_if(manifest.textures.begin(), manifest.textures.end(),
            [&](const ShaderManifestTexture& t) { return off(t.feature); }), manifest.textures.end());
    }
    manifest.validateReflection(variants, file);
    if (onlyEnabledFeatures) {
        // a member read only by disabled features is fine in this variant
        manifest.diagnostics.erase(std::remove_if(manifest.diagnostics.begin(), manifest.diagnostics.end(),
            [](const ShaderManifestDiagnostic& d) { return !d.error && d.message.find("has no @property") != std::string::npos; }),
            manifest.diagnostics.end());
    }
    return manifest.diagnostics;
}

Json editor::ProjectChecks::toJson(const ShaderCheck& check) {
    Json item = {{"shader", check.use.base}, {"type", shaderTypeName(check.use.shaderType)}, {"ok", check.ok}};
    if (isUiEffectUse(check.use)) {
        item["effect"] = ShaderPool::getUiEffectName(check.use.base);
        item["form"] = std::string((check.use.properties & 1u) ? "filter" : "paint") + ((check.use.properties & 4u) ? "-scatter" : (check.use.properties & 2u) ? "-step" : "");
    }
    if (!check.ok) {
        item["errors"] = check.errors;
        item["log"] = check.log;
        if (isUiEffectUse(check.use)) {
            // the first error's code: the manifest's, else the compiler's
            for (const Finding& finding : check.findings) {
                if (finding.severity == "error" && !finding.code.empty()) {
                    item["code"] = finding.code;
                    break;
                }
            }
        }
    }
    return item;
}

// ======================================================================== texture synthesis recipes

namespace {

int64_t stampTime(const fs::path& path, std::uintmax_t& size, bool& exists) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    exists = !ec;
    size = exists ? fs::file_size(path, ec) : 0;
    if (ec) exists = false;
    return exists ? static_cast<int64_t>(time.time_since_epoch().count()) : 0;
}

// .jasperengine/synthesis-check-cache.json: {"version": 1, "pngs": {rel: [time, size, recipe 0|1]},
// "hashes": {path: [time, size, sha1]}}
struct SynthesisCache {
    fs::path file;
    Json pngs = Json::object();
    Json hashes = Json::object();
    bool dirty = false;
};

void loadSynthesisCache(SynthesisCache& cache, const fs::path& projectRoot) {
    const fs::path dir = projectRoot / ".jasperengine";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    cache.file = dir / "synthesis-check-cache.json";
    std::string text;
    if (!editor::ProjectChecks::readFile(cache.file, text)) return;
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("version") || j["version"] != 1) return;
    if (j.contains("pngs") && j["pngs"].is_object()) cache.pngs = j["pngs"];
    if (j.contains("hashes") && j["hashes"].is_object()) cache.hashes = j["hashes"];
}

void saveSynthesisCache(const SynthesisCache& cache) {
    if (cache.file.empty() || !cache.dirty) return;
    const Json j = {{"version", 1}, {"pngs", cache.pngs}, {"hashes", cache.hashes}};
    // the keys are valid UTF-8 (cacheable() keeps the others out), so nothing is lost; replace
    // only makes sure a dump never throws
    const std::string text = editor::texsynth::jsonTextLossy(j);
    // a cache: atomic, never synced (a failed write leaves the old one, read again next scan)
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    io::writeFileAtomic(cache.file, text, options);
}

// JSON keys are UTF-8 text: a path that is not (a Latin-1 file name) is not cached, it is read
// again on every scan instead (a lossy key could match another file)
bool cacheable(const std::string& key) {
    return editor::texsynth::isValidUtf8(key);
}

bool cachedStamp(const Json& entry, int64_t time, std::uintmax_t size) {
    return entry.is_array() && entry.size() == 3 && entry[0].is_number_integer() && entry[0].get<int64_t>() == time &&
           entry[1].is_number_unsigned() && entry[1].get<std::uintmax_t>() == size;
}

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}

editor::ProjectChecks::SynthesisCheck editor::ProjectChecks::checkSynthesisRecipes(const fs::path& projectRoot, const std::vector<fs::path>& files) {
    SynthesisCheck check;
    SynthesisCache cache;
    loadSynthesisCache(cache, projectRoot);

    // the recipe of a PNG, nullopt without one (cached: PNGs without a recipe are not reopened).
    // A garbled one (CRC mismatch, not JSON, wrong types) comes back as a string: why
    auto recipeOf = [&](const fs::path& png) -> std::optional<Json> {
        std::uintmax_t size = 0;
        bool exists = false;
        const int64_t time = stampTime(png, size, exists);
        if (!exists) return std::nullopt;
        const std::string key = projectRelative(projectRoot, png);
        check.pngs++;
        if (cache.pngs.contains(key) && cachedStamp(cache.pngs[key], time, size) && cache.pngs[key][2] == 0) return std::nullopt;
        std::string readError;
        editor::texsynth::RecipeReadError origin = editor::texsynth::RecipeReadError::None;
        const std::optional<std::string> text = editor::texsynth::readRecipeTextHead(png, &readError, editor::texsynth::kRecipeKeyword, &origin);
        std::optional<Json> recipe;
        if (text) {
            Json parsed = Json::parse(*text, nullptr, false);
            std::string invalid;
            if (parsed.is_discarded()) recipe = Json("the jasper.synthesis chunk is not JSON");
            else if (!editor::texsynth::validateRecipe(parsed, invalid)) recipe = Json(invalid);
            else recipe = parsed;
        } else if (origin == editor::texsynth::RecipeReadError::Chunk) {
            // garbled chunk (CRC mismatch, compressed, malformed); a file that is not a PNG or
            // can't be read has no recipe (decided on the error's origin, not its text)
            recipe = Json(readError);
        }
        if (cacheable(key)) {
            cache.pngs[key] = Json::array({time, size, recipe ? 1 : 0});
            cache.dirty = true;
        }
        return recipe;
    };
    // a source's SHA-1 (cached by mtime and size); "" when it is gone
    auto hashOf = [&](const fs::path& source) -> std::string {
        std::uintmax_t size = 0;
        bool exists = false;
        const int64_t time = stampTime(source, size, exists);
        if (!exists) return std::string();
        const std::string key = source.lexically_normal().generic_string();
        if (cache.hashes.contains(key) && cachedStamp(cache.hashes[key], time, size) && cache.hashes[key][2].is_string()) {
            return cache.hashes[key][2].get<std::string>();
        }
        std::string content;
        if (!readFile(source, content)) return std::string();
        const std::string sha1 = editor::texsynth::sha1Hex(reinterpret_cast<const uint8_t*>(content.data()), content.size());
        if (cacheable(key)) {
            cache.hashes[key] = Json::array({time, size, sha1});
            cache.dirty = true;
        }
        return sha1;
    };

    struct Stale {
        fs::path png;
        Json recipe;
    };
    std::vector<Stale> checked;
    std::set<std::string> givenSources;   // files mode: changed sources
    std::set<fs::path> givenPngs;
    if (files.empty()) {
        for (const fs::path& png : findProjectFiles(projectRoot, {".png"})) {
            if (std::optional<Json> recipe = recipeOf(png)) checked.push_back({png, *recipe});
        }
    } else {
        for (const fs::path& file : files) {
            if (lowerExt(file) == ".png") {
                if (std::optional<Json> recipe = recipeOf(file)) {
                    checked.push_back({file, *recipe});
                    givenPngs.insert(file.lexically_normal());
                }
            }
            // a synthesized PNG may be the source of others too (synthesized from a synthesized one)
            givenSources.insert(file.lexically_normal().generic_string());
        }
        if (!givenSources.empty()) {
            // the textures made from the given files
            for (const fs::path& png : findProjectFiles(projectRoot, {".png"})) {
                if (givenPngs.count(png.lexically_normal())) continue;
                std::optional<Json> recipe = recipeOf(png);
                if (!recipe) continue;
                bool uses = false;
                for (const editor::texsynth::SourceState& s :
                     editor::texsynth::checkRecipeSources(*recipe, png, projectRoot, [](const fs::path&) { return std::string("-"); })) {
                    if (givenSources.count(s.resolved.lexically_normal().generic_string())) uses = true;
                }
                if (uses) checked.push_back({png, *recipe});
            }
        }
    }

    for (const Stale& item : checked) {
        const Json& recipe = item.recipe;
        if (recipe.is_string()) {
            // garbled: can't be checked nor regenerated from it
            check.recipes++;
            Finding finding;
            finding.tool = "texture";
            finding.path = projectRelative(projectRoot, item.png);
            finding.severity = "warning";
            finding.code = "synthesis-recipe-invalid";
            finding.message = "its texture synthesis recipe (jasper.synthesis chunk) is garbled: " + recipe.get<std::string>() +
                              "; its sources can't be checked nor the texture regenerated from it";
            finding.suggestion = "synthesize it again (Synthesize Texture... in the Project window, or synthesize_texture) to write a "
                                 "fresh recipe; a PNG re-saved without the chunk is no longer checked";
            check.findings.push_back(std::move(finding));
            continue;
        }
        const std::string role = editor::texsynth::jsonString(recipe, "role", "primary");
        // a companion / uncertainty output: its primary output reports the set
        if (files.empty() && role != "primary" && recipe.contains("outputs") && recipe["outputs"].is_object() &&
            recipe["outputs"].contains("primary") && recipe["outputs"]["primary"].is_string()) {
            const fs::path recorded = editor::FileUtils::pathFromUtf8(recipe["outputs"]["primary"].get<std::string>());
            const bool fromProject = editor::texsynth::jsonString(recipe, "pathBase", "file") == "project";
            const fs::path primary = recorded.is_absolute() ? recorded : (fromProject ? projectRoot : item.png.parent_path()) / recorded;
            std::error_code ec;
            if (primary.lexically_normal() != item.png.lexically_normal() && fs::is_regular_file(primary, ec)) continue;
        }
        check.recipes++;
        const std::string rel = projectRelative(projectRoot, item.png);
        const std::optional<uint64_t> seedValue = editor::texsynth::recipeSeed(recipe);
        const std::string seed = seedValue ? std::to_string(*seedValue) : "?";
        const std::string suggestion =
            "regenerate it: right-click it in the Project window > Edit Synthesis... > Regenerate > Save, or synthesize_texture with the "
            "`regenerate` arguments get_asset_info gives (mode " + editor::texsynth::jsonString(recipe, "mode", "?") + ", seed " + seed + ", overwrite: true)";
        for (const editor::texsynth::SourceState& s : editor::texsynth::checkRecipeSources(recipe, item.png, projectRoot, hashOf)) {
            if (s.state != "changed" && s.state != "missing") continue;
            Finding finding;
            finding.tool = "texture";
            finding.path = rel;
            finding.severity = "warning";
            const std::string source = s.resolved.empty() ? s.path : projectRelative(projectRoot, s.resolved);
            if (s.state == "changed") {
                finding.code = "synthesis-source-changed";
                finding.message = "synthesized from " + source + " (" + s.role + "), which changed since: the texture no longer matches its source";
                finding.suggestion = suggestion;
            } else {
                finding.code = "synthesis-source-missing";
                finding.message = "synthesized from " + source + " (" + s.role + "), which no longer exists";
                finding.suggestion = "restore " + source + " (or Edit Synthesis... and pick another source) before regenerating; the texture "
                                     "itself still works";
            }
            check.findings.push_back(std::move(finding));
        }
    }
    // forget PNGs that are gone (whole-project scans only)
    if (files.empty()) {
        for (auto it = cache.pngs.begin(); it != cache.pngs.end();) {
            std::error_code ec;
            if (!fs::exists(projectRoot / editor::FileUtils::pathFromUtf8(it.key()), ec)) {
                it = cache.pngs.erase(it);
                cache.dirty = true;
            } else {
                ++it;
            }
        }
    }
    saveSynthesisCache(cache);
    sortFindings(check.findings);
    return check;
}

// ---- Stochastic Tiling

namespace {

    // the tilt of a normal map a material or layer turns, when it is at least the warning's
    bool tiltedNormalMap(editor::Project* project, const Texture& texture, fs::path& file, float& degrees) {
        if (texture.empty() || texture.isFramebuffer()) return false;
        const std::string path = texture.getPath(0);
        if (path.empty()) return false;
        file = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(path));
        const editor::NormalMapTiltResult tilt = editor::NormalMapTilt::measure(file);
        degrees = tilt.degrees;
        return tilt.ready && tilt.valid && tilt.degrees >= editor::NormalMapTilt::WARNING_DEGREES;
    }

    std::string formatValue(float value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value);
        return text;
    }

}

std::vector<editor::ProjectChecks::Finding> editor::ProjectChecks::stochasticTilingFindings(Project* project) {
    std::vector<Finding> findings;
    const fs::path root = project->getProjectPath();
    const char* const tiltCode = "stochastic-tilted-normal-map";
    const char* const noEffectCode = "stochastic-no-effect";
    const std::string tiltFix = "set Rotation (stochasticRotation) to 0, or use a normal map whose average normal is +Z";

    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        const std::string path = sceneProject->filepath.empty() ? std::string() : projectRelative(root, root / sceneProject->filepath);
        const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
        auto warn = [&](const std::string& message, const char* code, const std::string& suggestion) {
            findings.push_back({"shader", path, 0, 0, "warning", message, code, suggestion});
        };

        for (Entity entity : sceneProject->entities) {
            const std::string name = "GameObject '" + scene->getEntityName(entity) + "'";

            if (const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity)) {
                for (size_t i = 0; i < terrain->surfaceLayers.size(); i++) {
                    const TerrainSurfaceLayer& layer = terrain->surfaceLayers[i];
                    fs::path file;
                    float degrees = 0.0f;
                    // a layer reads its normal map with its PBR surface only
                    if (layer.stochasticTiling && layer.pbr && layer.stochasticRotation > 0.0f &&
                            tiltedNormalMap(project, layer.normalTexture, file, degrees)) {
                        warn(name + " terrain layer " + std::to_string(i) + ": " + NormalMapTilt::describe(file, degrees) +
                             ", and Stochastic Tiling turns its cells (Rotation " + formatValue(layer.stochasticRotation) +
                             "): the rotated cells look tilted", tiltCode, tiltFix);
                    }
                }
            }

            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh) continue;
            const bool terrain = scene->findComponent<TerrainComponent>(entity) != nullptr;
            const bool atlas = scene->findComponent<SpriteComponent>(entity) != nullptr || scene->findComponent<TilemapComponent>(entity) != nullptr;
            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
                const Submesh& submesh = mesh->submeshes[s];
                const Material& material = submesh.material;
                const std::string where = name + " submesh " + std::to_string(s);
                const std::string& shader = RenderSystem::getSubmeshShader(*mesh, submesh, sceneMeshShader);
                const std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
                const bool hasManifest = manifest && manifest->present;

                // a library shader's STOCHASTIC feature (its own parameters and normal map)
                if (hasManifest) {
                    const std::vector<std::string> features = ShaderReference::getEnabledFeatures(shader, material.shaderFeatures);
                    if (std::find(features.begin(), features.end(), "STOCHASTIC") != features.end()) {
                        const float rotation = ShaderUniforms::has(material.shaderParams, "stochasticRotation")
                            ? ShaderUniforms::get(material.shaderParams, "stochasticRotation").x : 0.0f;
                        const auto normal = std::find_if(material.shaderTextures.begin(), material.shaderTextures.end(),
                            [](const std::pair<std::string, Texture>& entry) { return entry.first == "u_normalTexture"; });
                        fs::path file;
                        float degrees = 0.0f;
                        if (rotation > 0.0f && normal != material.shaderTextures.end() && tiltedNormalMap(project, normal->second, file, degrees)) {
                            warn(where + " (" + shader + "): " + NormalMapTilt::describe(file, degrees) + ", and Stochastic Tiling turns its cells (Rotation " +
                                 formatValue(rotation) + "): the rotated cells look tilted", tiltCode,
                                 "set the shader's Rotation (stochasticRotation) to 0, or use a normal map whose average normal is +Z");
                        }
                    }
                }

                if (!material.stochasticTiling) continue;

                // where Lit's Stochastic Tiling does not apply (RenderSystem::getSubmeshShaderVariant)
                if (hasManifest) {
                    warn(where + ": Stochastic Tiling is on, but the submesh draws with " + shader +
                         ", whose maps the built-in Lit's Stochastic Tiling does not sample", noEffectCode,
                         "turn the shader's own Stochastic Tiling feature on (a library shader with STOCHASTIC), or turn the material's off");
                    continue;
                }
                // a fork of Lit made before the feature (no LIT_STOCHASTIC_PARAMS): none of its passes take it
                if (!RenderSystem::supportsStochasticTiling(shader)) {
                    warn(where + ": Stochastic Tiling is on, but the material's custom shader " + shader +
                         " predates Stochastic Tiling: update the fork (see docs) or turn the feature off", noEffectCode,
                         "update the fork: add the 'vec4 stochastic;' member to its u_fs_pbrParams under #ifdef USE_STOCHASTIC_TILING "
                         "and '#define LIT_STOCHASTIC_PARAMS pbrParams.stochastic' in its .frag, as the engine's mesh.frag does "
                         "(docs/mcp/guide/shaders.md, Forking); or turn stochasticTiling off");
                    continue;
                }
                if (terrain) {
                    warn(where + ": Stochastic Tiling is on for a terrain's base material, which does not take it", noEffectCode,
                         "turn it on per terrain layer (surfaceLayers[N].stochasticTiling) instead");
                    continue;
                }
                if (atlas || MeshSystem::usesTextureRect(submesh)) {
                    warn(where + ": Stochastic Tiling is on, but the submesh draws through a texture rect (a sprite's atlas frame, "
                         "a tilemap's tiles), whose offset reads would show the neighbouring frames: it is left off", noEffectCode,
                         "turn it off, or draw the texture on a mesh without a texture rect");
                    continue;
                }
                const bool anyTexture = !material.baseColorTexture.empty() || !material.metallicRoughnessTexture.empty() ||
                                        !material.occlusionTexture.empty() || !material.emissiveTexture.empty() || !material.normalTexture.empty();
                if (!anyTexture) {
                    warn(where + ": Stochastic Tiling is on, but the material has no texture to tile", noEffectCode,
                         "give it a tiled texture, or turn Stochastic Tiling off");
                    continue;
                }
                fs::path file;
                float degrees = 0.0f;
                if (material.stochasticRotation > 0.0f && tiltedNormalMap(project, material.normalTexture, file, degrees)) {
                    warn(where + ": " + NormalMapTilt::describe(file, degrees) + ", and Stochastic Tiling turns its cells (Rotation " +
                         formatValue(material.stochasticRotation) + "): the rotated cells look tilted", tiltCode, tiltFix);
                }
            }
        }
    }
    return findings;
}

std::vector<editor::ProjectChecks::Finding> editor::ProjectChecks::lineTrailFindings(Project* project) {
    std::vector<Finding> findings;
    const fs::path root = project->getProjectPath();

    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        const std::string path = sceneProject->filepath.empty() ? std::string() : projectRelative(root, root / sceneProject->filepath);
        for (Entity entity : sceneProject->entities) {
            const std::string name = "GameObject '" + scene->getEntityName(entity) + "'";

            if (const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)) {
                if (lines->widthMultiplier <= 0.0f) {
                    const std::string& shader = lines->customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines->customShader.path;
                    const std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
                    bool samples = RenderSystem::getLinesBaseTexture(lines->shaderTextures) != nullptr;
                    if (manifest && manifest->present) {
                        samples = samples || !manifest->textures.empty() || (manifest->getPossibleRequire() & ShaderRequire_UV1);
                    }
                    if (samples) {
                        findings.push_back({"lines", path, 0, 0, "warning",
                            name + ": its Line Renderer has no width (a 1-pixel hairline, without texture coordinates) but its shader " +
                            (shader.empty() ? std::string("samples a base texture") : "'" + shader + "' samples a texture") +
                            ": the texture reads one texel", "line-texture-no-uv",
                            "give it a width (widthMultiplier > 0, e.g. 0.1) so it draws a textured ribbon, or remove the texture"});
                    }
                }
            }
            if (const TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity)) {
                if (!(trail->time > 0.0f)) {
                    findings.push_back({"lines", path, 0, 0, "warning",
                        name + ": its Trail Renderer's Time is 0, so it keeps no point and draws nothing", "trail-zero-time",
                        "set time to how long a point lives in seconds (Unity's default is 5; a bullet trail ~0.3)"});
                }
            }
        }
    }
    return findings;
}

std::vector<editor::ProjectChecks::Finding> editor::ProjectChecks::particleFindings(Project* project) {
    std::vector<Finding> findings;
    const fs::path root = project->getProjectPath();

    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        Scene* scene = sceneProject->scene;
        const std::string path = sceneProject->filepath.empty() ? std::string() : projectRelative(root, root / sceneProject->filepath);
        for (Entity entity : sceneProject->entities) {
            const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
            if (!ps) continue;
            const std::string name = "GameObject '" + scene->getEntityName(entity) + "'";
            // the effect it plays: its asset, or the inline one
            particles::ParticleEffectDocument document;
            std::vector<particles::Diagnostic> readDiagnostics;
            if (!ParticleEffectEdit::read(*ps, document, &readDiagnostics)) {
                findings.push_back({"particles", path, 0, 0, "error",
                    name + ": its Particle System's effect " + ps->effect.path + " cannot be read (" +
                    particles::ParticleEffectCodec::format(readDiagnostics) + "): it draws nothing", "particles-effect-invalid",
                    "fix or reassign the effect (get_particle_effect lists its diagnostics)"});
                continue;
            }
            std::vector<particles::Diagnostic> diagnostics;
            if (!particles::ParticleEffectCodec::validate(document, diagnostics)) {
                findings.push_back({"particles", path, 0, 0, "error",
                    name + ": its Particle System's effect does not compile (" + particles::ParticleEffectCodec::format(diagnostics) + "): it draws nothing",
                    "particles-effect-invalid", "edit_particle_effect with the fields the diagnostics name"});
                continue;
            }
            if (document.maxParticles > particles::kBudgetMobile) {
                findings.push_back({"particles", path, 0, 0, "warning",
                    name + ": its Particle System allows " + std::to_string(document.maxParticles) + " particles, past a mobile build's " +
                    "world budget of " + std::to_string(particles::kBudgetMobile) + " (its emission is thinned there)",
                    "particles-max-exceeded-budget", "lower maxParticles (and the emission rate), or keep the system off mobile builds"});
            }
        }
    }
    return findings;
}
