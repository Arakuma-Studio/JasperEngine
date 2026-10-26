// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptStatusBadge.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/ExternalCodeEditor.h"
#include "util/FileUtils.h"
#include "util/ScriptReloadService.h"
#include "util/ScriptStatus.h"
#include "util/SourceLocation.h"
#include "window/ConsoleWindow.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <ctime>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const ImVec4 GREEN = ImVec4(0.36f, 0.78f, 0.42f, 1.0f);
    const ImVec4 AMBER = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
    const ImVec4 RED = ImVec4(0.92f, 0.33f, 0.30f, 1.0f);

    enum class Look { Ok, Busy, Warning, Failed };

    Look lookOf(const ScriptStatusState& state){
        const bool cppFailed = state.cpp == CppScriptState::Failed || state.cpp == CppScriptState::Refused ||
                               state.editorCpp == CppScriptState::Failed || state.editorCpp == CppScriptState::Refused;
        if (state.lua == LuaScriptState::Error || cppFailed) return Look::Failed;
        if (state.lua == LuaScriptState::Reloading || state.cpp == CppScriptState::Building || state.editorCpp == CppScriptState::Building) return Look::Busy;
        if (state.cpp == CppScriptState::Stale || state.cppRow.softWarning) return Look::Warning;
        return Look::Ok;
    }

    const char* spinner(){
        static const char* const frames[] = {ICON_FA_CIRCLE_NOTCH, ICON_FA_SPINNER};
        return frames[static_cast<int>(ImGui::GetTime() * 4.0) % 2];
    }

    std::string timeText(std::chrono::system_clock::time_point time){
        if (time.time_since_epoch().count() == 0) return "never";
        const std::time_t t = std::chrono::system_clock::to_time_t(time);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", std::localtime(&t));
        return buffer;
    }

    void findingLinks(const std::vector<ScriptFinding>& findings, Project* project){
        const size_t shown = std::min<size_t>(findings.size(), 5);
        for (size_t i = 0; i < shown; i++) {
            const ScriptFinding& finding = findings[i];
            const std::string where = finding.file.empty() ? std::string() : formatSourceLocation(finding.file, finding.line, finding.column);
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            if (!where.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Selectable(where.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(where.c_str()))) {
                    SourceRoots roots;
                    roots.project = project->getProjectPath();
                    roots.lua = project->getLuaPath();
                    roots.assets = project->getAssetsPath();
                    const ResolvedSource resolved = resolveSourceLocation(finding.file, roots);
                    if (resolved.exists) ExternalCodeEditor::open(resolved.absolute, finding.line, finding.column, true);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(finding.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
        if (findings.size() > shown) ImGui::TextDisabled("... %zu more in the Console", findings.size() - shown);
    }

    void row(const char* language, const char* state, const ImVec4& color, const ScriptStatusRow& data, Project* project){
        ImGui::TextUnformatted(language);
        ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
        ImGui::TextColored(color, "%s", state);
        if (!data.message.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", data.message.c_str());
        }
        if (data.lastTime.time_since_epoch().count() != 0) {
            ImGui::TextDisabled("    last: %s (%.1f s)", timeText(data.lastTime).c_str(), data.lastSeconds);
        }
        findingLinks(data.findings, project);
    }

}

ScriptStatusBadge::ScriptStatusBadge(Project* project): project(project) {
}

float ScriptStatusBadge::width(float rowHeight) const {
    return rowHeight + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void ScriptStatusBadge::draw(float rowHeight){
    const ScriptStatusState& state = ScriptStatus::get().state();
    const Look look = lookOf(state);

    // a state that newly failed opens the Console (as OpenEngine does)
    if (ScriptStatus::get().takeNewFailure()) {
        if (ConsoleWindow* console = Out::getConsoleWindow()) console->requestFocus();
    }

    const char* icon = ICON_FA_CODE;
    ImVec4 color = GREEN;
    switch (look) {
        case Look::Ok: icon = ICON_FA_CIRCLE_CHECK; color = GREEN; break;
        case Look::Busy: icon = spinner(); color = ImGui::GetStyleColorVec4(ImGuiCol_Text); break;
        case Look::Warning: icon = ICON_FA_TRIANGLE_EXCLAMATION; color = AMBER; break;
        case Look::Failed: icon = ICON_FA_CIRCLE_XMARK; color = RED; break;
    }
    // the build's progress next to the spinner when it is known (no allocation per frame: R38)
    char label[64];
    if (state.cpp == CppScriptState::Building && state.cppRow.fraction >= 0.0f) {
        std::snprintf(label, sizeof(label), "%s %d%%##ScriptStatusBadge", icon, static_cast<int>(std::round(state.cppRow.fraction * 100.0f)));
    } else {
        std::snprintf(label, sizeof(label), "%s##ScriptStatusBadge", icon);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(label, ImVec2(0.0f, rowHeight))) ImGui::OpenPopup("##ScriptStatusPopover");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsPopupOpen("##ScriptStatusPopover")) {
        ImGui::SetTooltip("%s", ScriptStatus::get().summary().c_str());
    }

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Always);
    if (ImGui::BeginPopup("##ScriptStatusPopover")) {
        drawPopover();
        ImGui::EndPopup();
    }
}

void ScriptStatusBadge::drawPopover(){
    const ScriptStatusState& state = ScriptStatus::get().state();
    ImGui::TextUnformatted("Scripts");
    ImGui::Separator();

    const ImVec4 luaColor = state.lua == LuaScriptState::Error ? RED : state.lua == LuaScriptState::Reloading ? AMBER : GREEN;
    const char* luaState = state.lua == LuaScriptState::Error ? "Error" : state.lua == LuaScriptState::Reloading ? "Reloading" : "OK";
    row("Lua", luaState, luaColor, state.luaRow, project);

    auto cppLook = [](CppScriptState cpp, const ScriptStatusRow& data) -> std::pair<const char*, ImVec4> {
        switch (cpp) {
            case CppScriptState::None: return {"No C++ scripts", ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)};
            case CppScriptState::Stale: return {"Changed, not built", AMBER};
            case CppScriptState::Building: return {"Building", AMBER};
            case CppScriptState::UpToDate: return {data.softWarning ? "Up to date (toolchain warning)" : "Up to date", data.softWarning ? AMBER : GREEN};
            case CppScriptState::Failed: return {data.hasLastGood ? "Build failed \xE2\x80\x94 using last good build" : "Build failed", RED};
            case CppScriptState::Refused: return {"Library refused", RED};
        }
        return {"", GREEN};
    };
    const auto [cppText, cppColor] = cppLook(state.cpp, state.cppRow);
    if (state.cpp == CppScriptState::Building && state.cppRow.fraction >= 0.0f) {
        const std::string building = "Building " + std::to_string(static_cast<int>(std::round(state.cppRow.fraction * 100.0f))) + "%";
        row("C++", building.c_str(), cppColor, state.cppRow, project);
        ImGui::ProgressBar(state.cppRow.fraction, ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f));
    } else {
        row("C++", cppText, cppColor, state.cppRow, project);
    }
    if (state.fingerprintChecked && !state.fingerprintMismatches.empty()) {
        for (const FingerprintMismatch& mismatch : state.fingerprintMismatches) {
            ImGui::TextColored(mismatch.hard ? RED : AMBER, "    %s: library %u, editor %u%s%s", mismatch.field, mismatch.library, mismatch.editor,
                               mismatch.hint.empty() ? "" : " \xE2\x80\x94 ", mismatch.hint.c_str());
        }
    }
    if (state.editorCpp != CppScriptState::None) {
        const auto [editorText, editorColor] = cppLook(state.editorCpp, state.editorCppRow);
        row("C++ editor scripts", editorText, editorColor, state.editorCppRow, project);
    }

    ImGui::Separator();
    ScriptReloadService& reloads = ScriptReloadService::get();
    const bool building = reloads.isCppBuilding();
    ImGui::BeginDisabled(building);
    if (ImGui::Button(ICON_FA_HAMMER " Rebuild C++")) {
        std::string message;
        if (!reloads.rebuildCpp(message)) Out::warning("Rebuild C++: %s", message.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!building);
    if (ImGui::Button(ICON_FA_STOP " Cancel Build")) reloads.cancelCppBuild();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TERMINAL " Show in Console")) {
        if (ConsoleWindow* console = Out::getConsoleWindow()) {
            console->setSearchText(state.lua == LuaScriptState::Error ? ".lua" : "error");
            console->requestFocus();
        }
        ImGui::CloseCurrentPopup();
    }
}
