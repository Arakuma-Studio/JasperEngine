// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The bar over a Scene view whose scene (or prefab, or project.yaml) another program changed
// on disk (S05 R11, D5): "<name> changed on disk. [Reload] [Keep Mine]". Open documents are
// never reloaded on their own: overwriting unsaved work, or dropping the external edit
// silently, are both worse than asking.

#include "SceneWindow.h"

#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "reload/HotReloadService.h"

using namespace jasperengine;

void editor::SceneWindow::drawExternalChangeBar(SceneProject& sceneProject) {
    HotReloadService& reloads = HotReloadService::get();
    const bool sceneChanged = reloads.hasExternalChange(sceneProject.id);
    const bool projectChanged = reloads.hasProjectFileChange();
    if (!sceneChanged && !projectChanged) {
        return;
    }

    ImGui::PushID("ExternalChangeBar");
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y;
    ImGui::GetWindowDrawList()->AddRectFilled(barMin, ImVec2(barMin.x + ImGui::GetContentRegionAvail().x, barMin.y + barHeight),
                                              ImGui::GetColorU32(ImVec4(0.45f, 0.33f, 0.08f, 0.85f)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::dpi(6.0f));
    ImGui::AlignTextToFramePadding();
    const std::string name = sceneChanged ? sceneProject.name : std::string("project.yaml");
    ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION "  %s changed on disk.", name.c_str());
    ImGui::SetItemTooltip(sceneChanged ? "Another program wrote the file. Reload reads it (your unsaved changes go; not an undo step);\n"
                                         "Keep Mine keeps the editor's copy, written over the file on the next save."
                                       : "Another program wrote the project settings. Reload opens the project again;\n"
                                         "Keep Mine keeps the editor's settings, written over the file on the next save.");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (sceneChanged) reloads.reloadFromDisk(sceneProject.id);
        else reloads.reloadProjectFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep Mine")) {
        if (sceneChanged) reloads.keepMine(sceneProject.id);
        else reloads.keepMineProjectFile();
    }
    ImGui::PopID();
}
