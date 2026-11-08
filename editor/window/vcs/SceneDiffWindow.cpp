// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/SceneDiffWindow.h"

#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/RevertToBaselineCmd.h"
#include "external/IconsFontAwesome6.h"
#include "vcs/SceneDiffTracker.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsOverlay.h"
#include "window/vcs/VcsUi.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

namespace {

    ImVec4 stateColor(scene::ChangeState state) {
        switch (state) {
            case scene::ChangeState::Added: return ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Added));
            case scene::ChangeState::Removed: return ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Deleted));
            case scene::ChangeState::Modified: return ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Modified));
            case scene::ChangeState::Unchanged: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    const char* stateWord(scene::ChangeState state) {
        switch (state) {
            case scene::ChangeState::Added: return "added";
            case scene::ChangeState::Removed: return "removed";
            case scene::ChangeState::Modified: return "modified";
            case scene::ChangeState::Unchanged: return "";
        }
        return "";
    }

}

SceneDiffWindow::SceneDiffWindow(VcsUi* ui): ui(ui) {
}

void SceneDiffWindow::open(uint32_t id, const std::string& revision) {
    windowOpen = true;
    ImGui::SetWindowFocus(VcsUi::SCENE_DIFF_WINDOW);
    sceneId = id;
    setRev(revision);
    file.clear();
    fileDiff.reset();
    status.clear();
}

void SceneDiffWindow::openFile(const fs::path& path) {
    windowOpen = true;
    ImGui::SetWindowFocus(VcsUi::SCENE_DIFF_WINDOW);
    sceneId = 0;
    setRev("HEAD");
    file = path;
    fileDiff.reset();
    fileError.clear();
    fileDiffPending = true;
    SceneDiffTracker::Request request;
    request.file = path;
    request.fromEditor = false;
    request.changedOnly = true;
    Project* project = ui->getProject();
    project->getSceneDiffs().diffNow(*project, request,
        [this, path](const std::string& error, std::shared_ptr<const scene::SceneYamlModel>, std::shared_ptr<const scene::SceneDiffResult> diff) {
            if (file != path) return;
            fileDiffPending = false;
            fileError = error;
            fileDiff = diff;
        });
}

void SceneDiffWindow::setRev(const std::string& revision) {
    const VcsService& service = ui->getProject()->getVcs();
    const std::string baseline = service.baselineRevision();
    // the baseline is watched as "HEAD", as the Hierarchy and Inspector watch it (one diff), and
    // shown by its provider's name (svn: BASE)
    rev = !baseline.empty() && service.effectiveRevision(revision) == baseline ? std::string("HEAD") : revision;
    std::snprintf(revInput, sizeof(revInput), "%s", service.effectiveRevision(rev).c_str());
}

void SceneDiffWindow::watch() {
    if (!windowOpen || sceneId == 0) return;
    ui->getProject()->getSceneDiffs().watch(sceneId, rev);
}

void SceneDiffWindow::revert(const scene::ObjectDiff& object, const scene::ComponentDiff* component, const scene::PropertyDiff* property) {
    Project* project = ui->getProject();
    const SceneDiffView* view = project->getSceneDiffs().view(sceneId, rev);
    if (!view || !view->baseline) return;
    std::string why;
    RevertToBaselineCmd* command = RevertToBaselineCmd::build(project, sceneId, *view->baseline, object, component, property, &why);
    if (!command) {
        status = "Cannot revert: " + why;
        return;
    }
    status.clear();
    CommandHandle::get(sceneId)->addCommandNoMerge(command);
}

void SceneDiffWindow::drawObject(const scene::ObjectDiff& object, const scene::SceneYamlModel*, bool canRevert) {
    Project* project = ui->getProject();
    ImGui::PushID(static_cast<int>(object.id & 0x7fffffff));
    ImGui::PushID(static_cast<int>(object.id >> 31));
    const float revertWidth = ImGui::CalcTextSize(ICON_FA_ROTATE_LEFT).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    auto revertButton = [&](const scene::ComponentDiff* component, const scene::PropertyDiff* property) {
        if (!canRevert) return;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - revertWidth);
        if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT)) revert(object, component, property);
        ImGui::SetItemTooltip("Revert to Baseline");
    };

    std::string label = object.name.empty() ? "GameObject " + std::to_string(object.id) : object.name;
    if (object.prefabMember) label += "  (prefab instance member)";
    std::string notes;
    if (object.state != scene::ChangeState::Unchanged) notes += stateWord(object.state);
    if (object.reparented) notes += (notes.empty() ? "" : ", ") + std::string("reparented");
    if (object.reordered) notes += (notes.empty() ? "" : ", ") + std::string("reordered");
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor(object.state == scene::ChangeState::Unchanged && (object.reparented || object.reordered) ?
                                                    scene::ChangeState::Modified : object.state));
    const bool open = ImGui::TreeNodeEx("##object", ImGuiTreeNodeFlags_SpanAvailWidth | (object.components.empty() ? ImGuiTreeNodeFlags_Leaf : 0), "%s%s%s",
                                        label.c_str(), notes.empty() ? "" : "  - ", notes.c_str());
    ImGui::PopStyleColor();
    // a click selects it in the Hierarchy
    if (ImGui::IsItemClicked() && !object.settings && object.state != scene::ChangeState::Removed) {
        const SceneProject* sceneProject = project->getScene(sceneId);
        if (sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(static_cast<Entity>(object.id))) {
            project->setSelectedEntity(sceneId, static_cast<Entity>(object.id));
        }
    }
    revertButton(nullptr, nullptr);
    if (open) {
        for (const scene::ComponentDiff& component : object.components) {
            ImGui::PushID(component.key.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, stateColor(component.state));
            const bool componentOpen = ImGui::TreeNodeEx("##component", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen, "%s  %s",
                                                         component.key.c_str(), stateWord(component.state));
            ImGui::PopStyleColor();
            revertButton(&component, nullptr);
            if (componentOpen) {
                for (const scene::PropertyDiff& property : component.properties) {
                    ImGui::PushID(property.path.c_str());
                    const std::string path = property.path.empty() ? component.key : property.path;
                    ImGui::TreeNodeEx("##property", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "%s:  %s  " ICON_FA_ARROW_RIGHT "  %s", path.c_str(), property.inBase ? property.baseValue.c_str() : "(none)",
                                      property.inCurrent ? property.currentValue.c_str() : "(none)");
                    ImGui::SetItemTooltip("%s\nBaseline: %s\nCurrent: %s", scene::propertyPath(component, property).c_str(),
                                          property.inBase ? property.baseValue.c_str() : "(none)", property.inCurrent ? property.currentValue.c_str() : "(none)");
                    if (ImGui::BeginPopupContextItem("##property_menu")) {
                        if (ImGui::MenuItem("Copy Baseline Value", nullptr, false, property.inBase)) ImGui::SetClipboardText(property.baseValue.c_str());
                        ImGui::EndPopup();
                    }
                    revertButton(&component, &property);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::PopID();
}

void SceneDiffWindow::show() {
    if (!windowOpen) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(640.0f, 520.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(VcsUi::SCENE_DIFF_WINDOW, &windowOpen)) {
        ImGui::End();
        return;
    }
    Project* project = ui->getProject();
    VcsService& service = project->getVcs();
    if (!service.active()) {
        ImGui::TextWrapped("%s", service.stateMessage().empty() ? "Version control: not available" : service.stateMessage().c_str());
        ImGui::End();
        return;
    }
    // the document: an open scene or prefab stage, or a file
    std::string current = file.empty() ? std::string("(choose a scene)") : file.filename().string() + " (saved)";
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->id == sceneId) current = sceneProject->name;
    }
    if (sceneId == 0 && file.empty() && project->getSelectedScene()) sceneId = project->getSelectedSceneId();
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    if (ImGui::BeginCombo("##scene", current.c_str())) {
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene || sceneProject->filepath.empty()) continue;
            if (ImGui::Selectable(sceneProject->name.c_str(), sceneProject->id == sceneId)) open(sceneProject->id, rev);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("against");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    if (ImGui::InputText("##rev", revInput, sizeof(revInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::shared_ptr<IVcsProvider> provider = service.providerForReads();
        if (provider && provider->validRevision(revInput)) {
            setRev(revInput);
            status.clear();
        } else {
            status = std::string("Not a revision: ") + revInput;
        }
    }
    const std::string baselineName = service.baselineRevision();
    // (a window opened before version control was active shows the provider's name once it is)
    if (rev == "HEAD" && baselineName != "HEAD" && std::strcmp(revInput, "HEAD") == 0 && !ImGui::IsItemActive()) setRev(rev);
    ImGui::SetItemTooltip("The baseline (%s), or a revision (History's Diff with Working Copy sets it). Enter applies it.", baselineName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(baselineName.c_str())) setRev("HEAD");
    ImGui::SameLine();
    ImGui::Checkbox("Changed only", &changedOnly);

    std::shared_ptr<const scene::SceneDiffResult> diff;
    const scene::SceneYamlModel* baseline = nullptr;
    bool canRevert = false;
    if (!file.empty() && sceneId == 0) {
        if (fileDiffPending) ImGui::TextDisabled("Comparing...");
        else if (!fileError.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Deleted)), "%s", fileError.c_str());
        else ImGui::TextDisabled("The saved file: open it to revert its rows");
        diff = fileDiff;
    } else if (const SceneDiffView* view = project->getSceneDiffs().view(sceneId, rev)) {
        if (!view->error.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Deleted)), "%s", view->error.c_str());
        else if (view->pending && !view->diff) ImGui::TextDisabled("Comparing...");
        if (view->fromDisk) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Modified)), "Showing saved changes (the scene is too big to compare its unsaved edits as you type)");
        diff = view->diff;
        baseline = view->baseline.get();
        canRevert = baseline != nullptr && project->getScene(sceneId) != nullptr;
    } else {
        ImGui::TextDisabled("Comparing...");
    }
    if (!status.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colorOf(Badge::Modified)), "%s", status.c_str());
    ImGui::Separator();
    if (diff) {
        ImGui::TextDisabled("%zu added, %zu removed, %zu modified", diff->added, diff->removed, diff->modified);
        ImGui::BeginChild("##scene_diff_tree");
        for (const scene::ObjectDiff& object : diff->objects) drawObject(object, baseline, canRevert);
        // "Changed only" off: the GameObjects without changes too
        if (!changedOnly && sceneId != 0) {
            if (const SceneProject* sceneProject = project->getScene(sceneId); sceneProject && sceneProject->scene) {
                for (Entity entity : sceneProject->entities) {
                    bool listed = false;
                    for (const scene::ObjectDiff& object : diff->objects) listed = listed || object.id == entity;
                    if (listed) continue;
                    ImGui::PushID(static_cast<int>(entity));
                    ImGui::TreeNodeEx("##unchanged", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                                      sceneProject->scene->getEntityName(entity).c_str());
                    if (ImGui::IsItemClicked()) project->setSelectedEntity(sceneId, entity);
                    ImGui::PopID();
                }
            }
        }
        if (diff->objects.empty()) ImGui::TextDisabled("No changes against %s.", service.effectiveRevision(rev).c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}

}
