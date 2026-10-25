// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptReloadService.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "component/ScriptComponent.h"
#include "jobs/JobSystem.h"
#include "reload/HotReloadService.h"
#include "script/LuaBinding.h"
#include "script/LuaReload.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"
#include "util/SourceLocation.h"

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace jasperengine::editor {

namespace {

    constexpr size_t RECORDS_KEPT = 256;

    double msBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to){
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    std::string plural(size_t count, const char* one, const char* many){
        return std::to_string(count) + " " + (count == 1 ? one : many);
    }

    // require() names a project module may have: "enemies.boss" for lua/enemies/boss.lua
    std::vector<std::string> moduleNames(const std::string& luaRelative){
        std::vector<std::string> names;
        std::string base = luaRelative;
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".lua") == 0) base.resize(base.size() - 4);
        std::string dotted = base;
        std::replace(dotted.begin(), dotted.end(), '/', '.');
        names.push_back(dotted);
        // the loader also looks in lua/ under the Lua directory
        if (dotted.rfind("lua.", 0) == 0) names.push_back(dotted.substr(4));
        return names;
    }

}

ScriptReloadService& ScriptReloadService::get(){
    static ScriptReloadService* service = new ScriptReloadService();
    return *service;
}

void ScriptReloadService::attach(Project* project){
    this->project = project;
    HotReloadService::get().setScriptChangedHook([this](const fs::path& path) { return notifyChanged(path); });
}

void ScriptReloadService::detach(){
    HotReloadService::get().setScriptChangedHook(nullptr);
    playEnded();
    project = nullptr;
}

ScriptReloadService::Policy ScriptReloadService::policy(){
    const std::string value = AppSettings::getScriptChangesWhilePlaying();
    if (value == "after_stop") return Policy::AfterStop;
    if (value == "stop_and_reload") return Policy::StopAndReload;
    return Policy::ReloadAndContinue;
}

bool ScriptReloadService::cppReloadEnabled(){
    return AppSettings::getReloadCppDuringPlay();
}

std::string ScriptReloadService::relative(const fs::path& path) const {
    if (!project) return FileUtils::genericPathToUtf8(path);
    const fs::path rel = path.lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return FileUtils::genericPathToUtf8(path);
    return FileUtils::genericPathToUtf8(rel);
}

bool ScriptReloadService::isCppScriptFile(const fs::path& path) const {
    if (!project) return false;
    const std::string text = FileUtils::pathToUtf8(path);
    if (!Util::isSourceFile(text) && !Util::isHeaderFile(text)) return false;
    const fs::path normal = path.lexically_normal();
    for (const fs::path& dir : project->getScriptDirs()) {
        const fs::path root = (dir.is_absolute() ? dir : project->getProjectPath() / dir).lexically_normal();
        const fs::path rel = normal.lexically_relative(root);
        if (!rel.empty() && *rel.begin() != ".." && !rel.is_absolute()) return true;
    }
    // a script attached by its own path (outside the script directories)
    const fs::path rel = normal.lexically_relative(project->getProjectPath().lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

bool ScriptReloadService::notifyChanged(const fs::path& pathIn){
    if (!project || !project->isAnyScenePlaying()) return false;
    const fs::path path = pathIn.lexically_normal();
    const std::string text = FileUtils::pathToUtf8(path);
    const bool lua = Util::isLuaFile(text);
    const bool cpp = !lua && cppReloadEnabled() && isCppScriptFile(path);
    if (!lua && !cpp) {
        // a C++ change waits for the next Play's build; the badge says so
        if (Util::isScriptFile(text) && isCppScriptFile(path)) ScriptStatus::get().setCpp(CppScriptState::Stale, {}, "changed while playing: built at the next Play");
        return false;
    }
    switch (policy()) {
        case Policy::AfterStop:
            return false;
        case Policy::StopAndReload:
            getEditorHost().enqueueMainThreadTask([this]() {
                if (project && project->isAnyScenePlaying()) project->stopActivePlay();
            });
            return false;
        case Policy::ReloadAndContinue:
            break;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& queue = lua ? pendingLua : pendingCpp;
    auto it = queue.find(path);
    // a newer change supersedes the queued one; the time of the first stays (save to effect)
    if (it == queue.end()) {
        queue[path] = {now + std::chrono::milliseconds(lua ? LUA_DEBOUNCE_MS : CPP_DEBOUNCE_MS), now};
    } else {
        it->second.due = now + std::chrono::milliseconds(lua ? LUA_DEBOUNCE_MS : CPP_DEBOUNCE_MS);
    }
    if (cpp) ScriptStatus::get().setCpp(CppScriptState::Stale, {}, "changed: rebuilding during Play");
    return true;
}

void ScriptReloadService::requestReload(const std::vector<fs::path>& paths, bool lua, bool cpp){
    if (!project) return;
    const auto now = std::chrono::steady_clock::now();
    std::vector<fs::path> targets = paths;
    if (targets.empty()) {
        // every Lua script a running scene uses (a changed file is found by its contents anyway)
        if (lua) {
            for (Scene* scene : project->getPlayingScenes()) {
                auto scripts = scene->getComponentArray<ScriptComponent>();
                for (size_t i = 0; i < scripts->size(); i++) {
                    for (const ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
                        if (entry.type == ScriptType::LUA && entry.instance && !entry.path.empty()) {
                            const fs::path file = project->resolveLuaPath(entry.path).lexically_normal();
                            if (std::find(targets.begin(), targets.end(), file) == targets.end()) targets.push_back(file);
                        }
                    }
                }
            }
        }
        if (cpp && project->hasCppPlayLibrary()) pendingCpp[project->getProjectPath()] = {now, now};
    }
    for (const fs::path& raw : targets) {
        const fs::path path = raw.lexically_normal();
        const std::string text = FileUtils::pathToUtf8(path);
        if (lua && Util::isLuaFile(text)) {
            auto it = pendingLua.find(path);
            if (it == pendingLua.end()) pendingLua[path] = {now, now};
            else it->second.due = now;
        } else if (cpp && (Util::isSourceFile(text) || Util::isHeaderFile(text))) {
            pendingCpp[path] = {now, now};
        }
    }
}

bool ScriptReloadService::busy() const {
    return !pendingLua.empty() || !pendingCpp.empty() || luaCheck || cppBuildRunning;
}

bool ScriptReloadService::idleFor(const std::vector<fs::path>& paths) const {
    for (const fs::path& raw : paths) {
        const fs::path path = raw.lexically_normal();
        if (pendingLua.count(path) || pendingCpp.count(path) || (luaCheck && luaCheckPath == path)) return false;
        const std::string text = FileUtils::pathToUtf8(path);
        if (!Util::isLuaFile(text) && cppBuildRunning) return false;
    }
    if (!pendingCpp.empty() && paths.empty()) return false;
    return true;
}

void ScriptReloadService::playEnded(){
    pendingLua.clear();
    pendingCpp.clear();
    luaErrors.clear();
    if (luaCheck) {
        luaCheck->wait();
        luaCheck.reset();
    }
    luaCheckPath.clear();
    cppBuildRunning = false;
    if (ScriptStatus::get().state().lua != LuaScriptState::Ok) ScriptStatus::get().setLua(LuaScriptState::Ok);
}

void ScriptReloadService::startLuaCheck(const fs::path& path, std::chrono::steady_clock::time_point firstSeen){
    const fs::path luaRoot = project->getLuaPath();
    const std::string rel = relative(path);
    luaCheckPath = path;
    ScriptStatus::get().setLua(LuaScriptState::Reloading, ScriptStatus::get().state().luaRow.findings);
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "lua-reload-check";
    luaCheck = std::make_unique<jobs::Task<Checked>>(jobs::submit([path, luaRoot, rel, firstSeen]() {
        Checked checked;
        checked.path = path;
        checked.firstSeen = firstSeen;
        if (!ProjectChecks::readFile(path, checked.code)) {
            checked.readFailed = true;
            checked.findings.push_back({rel, 0, 0, "could not read the file"});
            return checked;
        }
        // syntax, top level and properties in an isolated state (nothing of the game runs)
        lua_State* L = ProjectChecks::newIsolatedLuaState(luaRoot, ProjectChecks::engineLuaGlobals());
        ProjectChecks::LuaScriptCheck check = ProjectChecks::checkLuaScript(checked.code, rel, L);
        ProjectChecks::closeIsolatedLuaState(L);
        for (const ProjectChecks::Finding& finding : check.findings) {
            checked.findings.push_back({rel, finding.line, finding.col, finding.message});
        }
        checked.ok = check.syntaxOk && check.findings.empty();
        checked.properties = std::move(check.properties);
        return checked;
    }, options));
}

void ScriptReloadService::addRecord(Record record){
    record.serial = nextSerial++;
    records.push_back(std::move(record));
    while (records.size() > RECORDS_KEPT) records.pop_front();
}

std::vector<ScriptReloadService::Record> ScriptReloadService::recordsSince(uint64_t serial) const {
    std::vector<Record> out;
    for (const Record& record : records) {
        if (record.serial > serial) out.push_back(record);
    }
    return out;
}

mcp::Json ScriptReloadService::reportJson(const std::vector<Record>& list, const std::vector<std::string>& paths){
    mcp::Json reloaded = mcp::Json::array(), keptOld = mcp::Json::array();
    for (const Record& record : list) {
        if (!paths.empty() && std::find(paths.begin(), paths.end(), record.path) == paths.end() && record.language == "lua") continue;
        if (record.ok) {
            mcp::Json item = {{"path", record.path}, {"language", record.language}, {"instances", record.instances},
                              {"properties", {{"added", record.added}, {"removed", record.removed}, {"converted", record.converted}, {"reset", record.reset}}},
                              {"dropped_callbacks", record.droppedCallbacks}, {"ms", std::round(record.ms * 10.0) / 10.0},
                              {"since_save_ms", std::round(record.sinceSaveMs)}};
            if (!record.classes.empty()) item["classes"] = record.classes;
            reloaded.push_back(std::move(item));
        } else {
            mcp::Json findings = mcp::Json::array();
            for (const ScriptFinding& finding : record.findings) {
                mcp::Json item = {{"file", finding.file}, {"message", finding.message}};
                if (finding.line > 0) item["line"] = finding.line;
                findings.push_back(std::move(item));
            }
            mcp::Json item = {{"path", record.path}, {"language", record.language}, {"findings", findings}};
            if (!record.findings.empty() && record.findings[0].line > 0) item["line"] = record.findings[0].line;
            if (!record.cppState.empty()) item["cpp_state"] = record.cppState;
            keptOld.push_back(std::move(item));
        }
    }
    return {{"reloaded", reloaded}, {"kept_old", keptOld}};
}

void ScriptReloadService::updateLuaStatus(){
    std::vector<ScriptFinding> all;
    for (const auto& [path, findings] : luaErrors) all.insert(all.end(), findings.begin(), findings.end());
    if (all.empty()) ScriptStatus::get().setLua(LuaScriptState::Ok);
    else ScriptStatus::get().setLua(LuaScriptState::Error, std::move(all), "a script kept its old code");
}

void ScriptReloadService::applyLua(Checked& checked){
    Record record;
    record.path = relative(checked.path);
    record.language = "lua";
    const auto started = std::chrono::steady_clock::now();
    lua_State* L = LuaBinding::getLuaState();

    if (!checked.ok) {
        record.ok = false;
        record.findings = checked.findings;
        for (const ScriptFinding& finding : checked.findings) {
            Out::error("%s: %s (kept the old code)", formatSourceLocation(finding.file, finding.line, 0).c_str(), finding.message.c_str());
        }
        luaErrors[checked.path] = checked.findings;
        updateLuaStatus();
        addRecord(std::move(record));
        return;
    }

    // the live instances of this file, in every running scene (instantiated GameObjects too)
    std::vector<ScriptInstanceRef> instances;
    std::string chunkPath;
    for (Scene* scene : project->getPlayingScenes()) {
        auto scripts = scene->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            ScriptComponent& component = scripts->getComponentFromIndex(i);
            for (ScriptEntry& entry : component.scripts) {
                if (entry.type != ScriptType::LUA || !entry.instance || entry.path.empty()) continue;
                if (project->resolveLuaPath(entry.path).lexically_normal() != checked.path) continue;
                instances.push_back({scene, scripts->getEntity(i), &entry});
                if (chunkPath.empty()) chunkPath = entry.path.str();
            }
        }
    }

    bool keptOld = false;
    std::vector<std::string> warnings;
    if (!instances.empty()) {
        lua_reload::Report report = lua_reload::reloadScript(L, chunkPath, checked.code, instances, checked.properties);
        if (report.keptOld) {
            keptOld = true;
            record.findings.push_back({record.path, report.errorLine, 0, report.error});
        } else {
            record.instances += report.instances;
            record.added = report.added;
            record.removed = report.removed;
            record.converted = report.converted;
            record.reset = report.reset;
            record.droppedCallbacks = report.droppedCallbacks;
            warnings = report.warnings;
        }
    }

    // a require()d module of the same file: patched in place
    const fs::path luaRelative = checked.path.lexically_relative(project->getLuaPath().lexically_normal());
    if (!keptOld && !luaRelative.empty() && *luaRelative.begin() != "..") {
        const std::string luaRel = FileUtils::genericPathToUtf8(luaRelative);
        for (const std::string& name : moduleNames(luaRel)) {
            if (!lua_reload::isModuleLoaded(L, name)) continue;
            lua_reload::Report report = lua_reload::reloadModule(L, name, luaRel, checked.code);
            if (report.keptOld) {
                keptOld = true;
                record.findings.push_back({record.path, report.errorLine, 0, report.error});
                break;
            }
            record.instances += report.instances;
            if (instances.empty()) {
                record.added = report.added;
                record.removed = report.removed;
            }
            warnings.insert(warnings.end(), report.warnings.begin(), report.warnings.end());
        }
    }

    record.ms = msBetween(started, std::chrono::steady_clock::now());
    record.sinceSaveMs = msBetween(checked.firstSeen, std::chrono::steady_clock::now());
    for (const std::string& warning : warnings) Out::warning("%s", warning.c_str());

    if (keptOld) {
        record.ok = false;
        for (const ScriptFinding& finding : record.findings) {
            Out::error("%s: %s (kept the old code)", formatSourceLocation(finding.file, finding.line, 0).c_str(), finding.message.c_str());
        }
        luaErrors[checked.path] = record.findings;
    } else {
        luaErrors.erase(checked.path);
        const std::string name = checked.path.filename().string();
        if (record.instances == 0) {
            Out::info("[Script Reload] %s changed: no running instance or loaded module uses it (%.0f ms after the save)",
                      record.path.c_str(), record.sinceSaveMs);
        } else {
            std::string details = plural(static_cast<size_t>(record.instances), "instance", "instances");
            if (!record.added.empty()) details += ", " + plural(record.added.size(), "property added", "properties added");
            if (!record.removed.empty()) details += ", " + plural(record.removed.size(), "property removed", "properties removed");
            if (!record.converted.empty() || !record.reset.empty())
                details += ", " + plural(record.converted.size() + record.reset.size(), "property changed", "properties changed");
            if (record.droppedCallbacks > 0) details += ", " + plural(static_cast<size_t>(record.droppedCallbacks), "callback dropped", "callbacks dropped");
            Out::success("[Script Reload] Reloaded %s (%s; %.1f ms, %.0f ms after the save)", name.c_str(), details.c_str(), record.ms, record.sinceSaveMs);
            if (record.droppedCallbacks > 0) {
                Out::warning("%s: %d anonymous callback%s dropped; re-create them in onReload", name.c_str(), record.droppedCallbacks,
                             record.droppedCallbacks == 1 ? "" : "s");
            }
        }
        ScriptStatus::get().luaReloaded(record.ms / 1000.0);
    }
    updateLuaStatus();
    addRecord(std::move(record));
}

void ScriptReloadService::cppReloadFinished(const Record& record){
    cppBuildRunning = false;
    addRecord(record);
}

void ScriptReloadService::update(){
    if (!project) return;
    if (!project->isAnyScenePlaying()) {
        if (!pendingLua.empty() || !pendingCpp.empty() || luaCheck) playEnded();
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    // a finished check swaps at this frame boundary (no Lua runs now)
    if (luaCheck && luaCheck->isReady()) {
        Checked checked;
        try {
            checked = std::move(luaCheck->get());
        } catch (const std::exception& e) {
            checked.path = luaCheckPath;
            checked.findings.push_back({relative(luaCheckPath), 0, 0, std::string("the check failed: ") + e.what()});
        }
        luaCheck.reset();
        luaCheckPath.clear();
        // changed again while checked: that newer change is the one to apply
        if (!pendingLua.count(checked.path)) applyLua(checked);
        if (ScriptStatus::get().state().lua == LuaScriptState::Reloading) updateLuaStatus();
    }

    // one reload in flight: the earliest due change goes next
    if (!luaCheck) {
        auto next = pendingLua.end();
        for (auto it = pendingLua.begin(); it != pendingLua.end(); ++it) {
            if (it->second.due <= now && (next == pendingLua.end() || it->second.due < next->second.due)) next = it;
        }
        if (next != pendingLua.end()) {
            const fs::path path = next->first;
            const auto firstSeen = next->second.firstSeen;
            pendingLua.erase(next);
            startLuaCheck(path, firstSeen);
        }
    }

    // C++: one rebuild for every change due, while the old library keeps running
    if (!pendingCpp.empty() && !cppBuildRunning) {
        bool due = false;
        auto firstSeen = now;
        for (const auto& [path, pending] : pendingCpp) {
            due = due || pending.due <= now;
            firstSeen = std::min(firstSeen, pending.firstSeen);
        }
        if (due) {
            std::vector<fs::path> changed;
            for (const auto& [path, pending] : pendingCpp) changed.push_back(path);
            pendingCpp.clear();
            std::string error;
            if (project->startCppReload(changed, firstSeen, error)) {
                cppBuildRunning = true;
            } else if (!error.empty()) {
                Record record;
                record.path = changed.empty() ? std::string() : relative(changed.front());
                record.language = "cpp";
                record.ok = false;
                record.findings.push_back({record.path, 0, 0, error});
                record.cppState = ScriptStatus::cppStateName(ScriptStatus::get().state().cpp);
                addRecord(std::move(record));
                Out::error("C++ reload: %s", error.c_str());
            }
        }
    }
    if (cppBuildRunning) project->updateCppReload();
}

}
