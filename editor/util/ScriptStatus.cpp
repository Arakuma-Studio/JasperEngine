// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptStatus.h"

#include "Generator.h"
#include "util/ProjectChecks.h"

#include <cmath>

using namespace jasperengine::editor;

ScriptStatus& ScriptStatus::get(){
    static ScriptStatus* status = new ScriptStatus();
    return *status;
}

void ScriptStatus::bump(bool failedNow){
    if (failedNow) newFailure = true;
    current.revision++;
    changed.invoke();
}

void ScriptStatus::setLua(LuaScriptState state, std::vector<ScriptFinding> findings, std::string message){
    const bool failedNow = state == LuaScriptState::Error && current.lua != LuaScriptState::Error;
    current.lua = state;
    if (state != LuaScriptState::Reloading) current.luaRow.findings = std::move(findings);
    current.luaRow.message = std::move(message);
    bump(failedNow);
}

void ScriptStatus::luaReloaded(double seconds){
    current.luaRow.lastTime = std::chrono::system_clock::now();
    current.luaRow.lastSeconds = seconds;
    bump(false);
}

void ScriptStatus::setCpp(CppScriptState state, std::vector<ScriptFinding> findings, std::string message){
    const bool failed = state == CppScriptState::Failed || state == CppScriptState::Refused;
    const bool wasFailed = current.cpp == CppScriptState::Failed || current.cpp == CppScriptState::Refused;
    current.cpp = state;
    if (state != CppScriptState::Building) current.cppRow.findings = std::move(findings);
    current.cppRow.message = std::move(message);
    current.cppRow.fraction = -1.0f;
    if (state != CppScriptState::None) cppScriptsPresent = true;
    bump(failed && !wasFailed);
}

void ScriptStatus::setCppProgress(float fraction){
    if (current.cpp != CppScriptState::Building) return;
    if (std::abs(current.cppRow.fraction - fraction) < 0.005f) return;
    current.cppRow.fraction = fraction;
    bump(false);
}

void ScriptStatus::cppBuilt(bool ok, double seconds, std::vector<ScriptFinding> findings, bool keptLastGood){
    current.cppRow.lastTime = std::chrono::system_clock::now();
    current.cppRow.lastSeconds = seconds;
    current.cppRow.hasLastGood = ok || keptLastGood;
    if (ok) {
        setCpp(CppScriptState::UpToDate, std::move(findings));
    } else {
        setCpp(CppScriptState::Failed, std::move(findings),
               keptLastGood ? "C++ build failed \xE2\x80\x94 using last good build" : "C++ build failed");
    }
}

void ScriptStatus::setEditorCpp(CppScriptState state, std::vector<ScriptFinding> findings, std::string message){
    const bool failed = state == CppScriptState::Failed || state == CppScriptState::Refused;
    const bool wasFailed = current.editorCpp == CppScriptState::Failed || current.editorCpp == CppScriptState::Refused;
    current.editorCpp = state;
    current.editorCppRow.findings = std::move(findings);
    current.editorCppRow.message = std::move(message);
    if (state == CppScriptState::UpToDate || state == CppScriptState::Failed) current.editorCppRow.lastTime = std::chrono::system_clock::now();
    bump(failed && !wasFailed);
}

void ScriptStatus::setFingerprint(bool ok, std::vector<FingerprintMismatch> mismatches){
    current.fingerprintChecked = true;
    current.fingerprintOk = ok;
    bool soft = false;
    for (const FingerprintMismatch& mismatch : mismatches) soft = soft || !mismatch.hard;
    current.fingerprintMismatches = std::move(mismatches);
    current.cppRow.softWarning = ok && soft;
    bump(false);
}

void ScriptStatus::setHasCppScripts(bool has){
    if (cppScriptsPresent == has) return;
    cppScriptsPresent = has;
    if (!has && current.cpp != CppScriptState::Building) current.cpp = CppScriptState::None;
    else if (has && current.cpp == CppScriptState::None) current.cpp = CppScriptState::Stale;
    bump(false);
}

void ScriptStatus::postCppProgress(float fraction){
    std::scoped_lock lock(postMutex);
    postedFraction = fraction;
}

void ScriptStatus::followBuilds(){
    buildSubscription = Generator::buildEvents().subscribe([this](const ScriptBuildEvent& event) {
        std::scoped_lock lock(postMutex);
        // a burst of progress lines keeps only the newest one
        if (event.kind == ScriptBuildEvent::Kind::Progress && !postedBuilds.empty() &&
            postedBuilds.back().kind == ScriptBuildEvent::Kind::Progress && postedBuilds.back().row == event.row) {
            postedBuilds.back() = event;
            return;
        }
        postedBuilds.push_back(event);
    });
}

void ScriptStatus::applyBuild(const ScriptBuildEvent& event){
    const bool editorRow = event.row == "editor";
    switch (event.kind) {
        case ScriptBuildEvent::Kind::Started:
            if (editorRow) setEditorCpp(CppScriptState::Building);
            else setCpp(CppScriptState::Building);
            break;
        case ScriptBuildEvent::Kind::Progress:
            if (!editorRow) setCppProgress(event.fraction);
            break;
        case ScriptBuildEvent::Kind::Finished: {
            const ProjectChecks::CppBuildReport report = ProjectChecks::summarizeCppBuild(event.projectPath, event.success, event.output);
            std::vector<ScriptFinding> findings;
            for (const ProjectChecks::CppDiagnostic& d : report.errors) findings.push_back({d.file, d.line, d.column, d.message});
            if (findings.empty() && !event.success && !event.cancelled) {
                findings.push_back({"", 0, 0, report.logTail.empty() ? "the build failed (see the Console)" : report.logTail.back()});
            }
            if (editorRow) {
                setEditorCpp(event.success ? CppScriptState::UpToDate : CppScriptState::Failed, std::move(findings),
                             event.success ? std::string() : "C++ editor scripts: build failed");
            } else if (event.cancelled) {
                setCpp(current.cppRow.hasLastGood ? CppScriptState::UpToDate : CppScriptState::Stale, {}, "build cancelled");
            } else {
                cppBuilt(event.success, event.seconds, std::move(findings), current.cppRow.hasLastGood);
            }
            break;
        }
    }
}

bool ScriptStatus::update(){
    std::optional<float> fraction;
    std::vector<ScriptBuildEvent> builds;
    {
        std::scoped_lock lock(postMutex);
        fraction.swap(postedFraction);
        builds.swap(postedBuilds);
    }
    const uint64_t before = current.revision;
    for (const ScriptBuildEvent& event : builds) applyBuild(event);
    if (fraction) setCppProgress(*fraction);
    return current.revision != before;
}

bool ScriptStatus::takeNewFailure(){
    const bool failure = newFailure;
    newFailure = false;
    return failure;
}

const char* ScriptStatus::luaStateName(LuaScriptState state){
    switch (state) {
        case LuaScriptState::Reloading: return "reloading";
        case LuaScriptState::Error: return "error";
        default: return "ok";
    }
}

const char* ScriptStatus::cppStateName(CppScriptState state){
    switch (state) {
        case CppScriptState::Stale: return "stale";
        case CppScriptState::Building: return "building";
        case CppScriptState::UpToDate: return "up_to_date";
        case CppScriptState::Failed: return "failed";
        case CppScriptState::Refused: return "refused";
        default: return "none";
    }
}

std::string ScriptStatus::summary() const {
    std::string lua = "Lua: ";
    switch (current.lua) {
        case LuaScriptState::Ok: lua += "ok"; break;
        case LuaScriptState::Reloading: lua += "reloading"; break;
        case LuaScriptState::Error: {
            const size_t n = current.luaRow.findings.size();
            lua += std::to_string(n) + (n == 1 ? " error" : " errors");
            break;
        }
    }
    std::string cpp;
    switch (current.cpp) {
        case CppScriptState::None: break;
        case CppScriptState::Stale: cpp = "C++: changed, not built"; break;
        case CppScriptState::Building:
            cpp = current.cppRow.fraction >= 0.0f ? "C++: building " + std::to_string(static_cast<int>(current.cppRow.fraction * 100.0f + 0.5f)) + "%"
                                                  : "C++: building";
            break;
        case CppScriptState::UpToDate: cpp = current.cppRow.softWarning ? "C++: up to date (toolchain warning)" : "C++: up to date"; break;
        case CppScriptState::Failed:
            cpp = current.cppRow.hasLastGood ? "C++: build failed \xE2\x80\x94 using last good build" : "C++: build failed";
            break;
        case CppScriptState::Refused: cpp = "C++: library refused (toolchain fingerprint)"; break;
    }
    std::string text = lua;
    if (!cpp.empty()) text += " \xC2\xB7 " + cpp;
    if (current.editorCpp == CppScriptState::Failed) text += " \xC2\xB7 C++ editor scripts: build failed";
    return text;
}

namespace {

    jasperengine::editor::mcp::Json rowJson(const char* state, const ScriptStatusRow& row){
        using jasperengine::editor::mcp::Json;
        Json findings = Json::array();
        for (const ScriptFinding& finding : row.findings) {
            Json item = {{"file", finding.file}, {"message", finding.message}};
            if (finding.line > 0) item["line"] = finding.line;
            if (finding.column > 0) item["column"] = finding.column;
            findings.push_back(std::move(item));
        }
        Json out = {{"state", state}, {"findings", findings}};
        if (!row.message.empty()) out["message"] = row.message;
        if (row.fraction >= 0.0f) out["fraction"] = std::round(row.fraction * 1000.0f) / 1000.0f;
        else if (std::string(state) == "building") out["fraction"] = nullptr;
        if (row.lastTime.time_since_epoch().count() != 0) {
            out["last_seconds_ago"] = std::round(std::chrono::duration<double>(std::chrono::system_clock::now() - row.lastTime).count());
            out["last_duration_seconds"] = std::round(row.lastSeconds * 100.0) / 100.0;
        }
        if (row.softWarning) out["warning"] = true;
        return out;
    }

}

jasperengine::editor::mcp::Json ScriptStatus::toJson() {
    using jasperengine::editor::mcp::Json;
    // reload_scripts' wait ends when the build's task does, which may be after this frame's
    // update() (the build posts its Finished event, then its task completes)
    update();
    Json mismatches = Json::array();
    for (const FingerprintMismatch& mismatch : current.fingerprintMismatches) {
        mismatches.push_back({{"field", mismatch.field}, {"editor", mismatch.editor}, {"library", mismatch.library},
                              {"severity", mismatch.hard ? "hard" : "soft"}, {"hint", mismatch.hint}});
    }
    Json fingerprint = {{"checked", current.fingerprintChecked}, {"ok", current.fingerprintOk}, {"mismatches", mismatches}};
    Json cpp = rowJson(cppStateName(current.cpp), current.cppRow);
    cpp["using_last_good_build"] = current.cpp == CppScriptState::Failed && current.cppRow.hasLastGood;
    return {{"lua", rowJson(luaStateName(current.lua), current.luaRow)}, {"cpp", cpp},
            {"editor_cpp", rowJson(cppStateName(current.editorCpp), current.editorCppRow)},
            {"fingerprint", fingerprint}, {"summary", summary()}};
}
