// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "command/CommandNames.h"
#include "AssetPanels.h"

#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "PrefabStage.h"
#include "Project.h"
#include "Theme.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetTypes.h"
#include "command/type/AssetRefCmds.h"
#include "external/IconsFontAwesome6.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const ImVec4 missingColor(0.95f, 0.35f, 0.35f, 1.0f);

    // A Locate waiting for its scene to finish opening
    struct PendingLocate {
        std::string owner;
        uint64_t entity = 0;
        int framesLeft = 0;
    };
    PendingLocate pendingLocate;

    SceneProject* loadedDocument(Project* project, const std::string& owner) {
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            const PrefabStage* stage = project->getPrefabStage(sceneProject->id);
            const fs::path path = stage ? stage->assetPath : sceneProject->filepath;
            if (!path.empty() && project->normalizeToProjectRelative(path).generic_string() == owner) {
                return sceneProject;
            }
        }
        return nullptr;
    }

    bool selectInDocument(Project* project, const std::string& owner, uint64_t entity) {
        SceneProject* sceneProject = loadedDocument(project, owner);
        if (!sceneProject) {
            return false;
        }
        project->setSelectedSceneId(sceneProject->id);
        if (entity != 0) {
            project->setSelectedEntity(sceneProject->id, static_cast<Entity>(entity));
        }
        return true;
    }

    void pumpPendingLocate(Project* project) {
        if (pendingLocate.framesLeft <= 0) {
            return;
        }
        if (selectInDocument(project, pendingLocate.owner, pendingLocate.entity) || --pendingLocate.framesLeft == 0) {
            pendingLocate.framesLeft = 0;
        }
    }

    std::string referrerLabel(const MissingAssetTracker::Referrer& referrer) {
        std::string label = referrer.ownerPath;
        if (!referrer.entityPath.empty()) label += "  " ICON_FA_ANGLE_RIGHT "  " + referrer.entityPath;
        if (!referrer.component.empty()) label += "  " ICON_FA_ANGLE_RIGHT "  " + Catalog::getComponentDisplayName(
            [&] { ComponentType type; return AssetRefSchema::componentByName(referrer.component, type) ? type : ComponentType::Transform; }());
        label += "  " + referrer.field;
        return label;
    }

    std::string selectionKey(const MissingAssetTracker::Referrer& referrer, uint32_t ordinal = 0) {
        return referrer.ownerPath + "\n" + referrer.locator + "\n" + std::to_string(ordinal);
    }

    AssetRefTarget targetFromKey(const std::string& key) {
        AssetRefTarget target;
        const size_t first = key.find('\n');
        const size_t second = key.find('\n', first + 1);
        target.owner = key.substr(0, first);
        target.locator = key.substr(first + 1, second - first - 1);
        target.ordinal = static_cast<uint32_t>(std::strtoul(key.c_str() + second + 1, nullptr, 10));
        return target;
    }

    std::vector<AssetRefTarget> panelGroupTargets(const MissingAssetTracker::Group& group) {
        std::vector<AssetRefTarget> targets;
        for (const MissingAssetTracker::Referrer& referrer : group.referrers) {
            targets.push_back({referrer.ownerPath, referrer.locator, 0});
        }
        return targets;
    }

    void panelRunProjectCommand(Project* project, Command* command, const char* what) {
        if (!project->getProjectCommandHistory()->addCommand(command)) {
            Out::error("%s failed", what);
        }
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
            projectWindow->refreshCurrentDirectory();
        }
    }

    // An orphan meta to the trash; the undo brings it back
    class DeleteOrphanMetaCmd : public Command {
    public:
        DeleteOrphanMetaCmd(Project* project, std::string metaPath) : project(project), metaPath(std::move(metaPath)) {}
        bool execute() override {
            const fs::path trashDir = project->getProjectPath() / ".trash";
            std::error_code ec;
            fs::create_directories(trashDir, ec);
            trashPath = trashDir / fs::path(metaPath).filename();
            for (int n = 1; fs::exists(trashPath, ec); n++) {
                trashPath = trashDir / (fs::path(metaPath).stem().string() + " (" + std::to_string(n) + ").meta");
            }
            std::string error;
            if (!project->getAssetDatabase().removeOrphanMeta(metaPath, trashDir, &error)) {
                Out::error("Couldn't delete %s: %s", metaPath.c_str(), error.c_str());
                return false;
            }
            return true;
        }
        void undo() override {
            std::string error;
            if (assetio::renameFile(trashPath, project->getProjectPath() / metaPath, &error)) {
                project->refreshAssetDatabase(true);
            }
        }
        bool affectsStructure() const override { return false; }
        std::string describe() const override {
            return CommandNames::clip("Delete Orphan Meta " + CommandNames::file(metaPath));
        }
        const char* typeName() const override { return "DeleteOrphanMetaCmd"; }

    private:
        Project* project;
        std::string metaPath;
        fs::path trashPath;
    };

}

void editor::locateAssetReferrer(Project* project, const std::string& ownerPath, uint64_t entity) {
    if (selectInDocument(project, ownerPath, entity)) {
        return;
    }
    const fs::path full = project->getProjectPath() / ownerPath;
    if (Util::isSceneFile(ownerPath)) {
        project->openScene(full, false);
    } else if (Util::isPrefabFile(ownerPath)) {
        editor::getEditorHost().openPrefabAsset(full);
    } else {
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
            projectWindow->revealAsset(full);
        }
        return;
    }
    pendingLocate = {ownerPath, entity, 120};
    pumpPendingLocate(project);
}

// ==================================================================== Missing Assets

MissingAssetsWindow::MissingAssetsWindow(Project* project) : project(project) {
}

void MissingAssetsWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void MissingAssetsWindow::openPicker(AssetType type, std::function<void(const AssetDatabase::AssetInfo&)> done) {
    pickerOpen = true;
    pickerType = type;
    pickerFilter.clear();
    pickerDone = std::move(done);
}

void MissingAssetsWindow::drawPicker() {
    if (pickerOpen) {
        ImGui::OpenPopup("Choose Asset##missing_assets_picker");
        pickerOpen = false;
    }
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(480.0f, 360.0f)), ImGuiCond_Appearing);
    if (!EditorModal::begin("Choose Asset##missing_assets_picker", nullptr)) {
        return;
    }
    EditorModal::message(std::string("Choose a ") + assetTypeName(pickerType) + " asset: values.path picks it.");
    EditorModal::error(pickerMiss);
    char filter[256];
    snprintf(filter, sizeof(filter), "%s", pickerFilter.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter))) {
        pickerFilter = filter;
    }
    std::string needle = pickerFilter;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<AssetDatabase::AssetInfo> candidates;
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (pickerType != AssetType::Unknown && info.type != pickerType) return;
        std::string lower = info.path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (needle.empty() || lower.find(needle) != std::string::npos) candidates.push_back(info);
    });
    // the list is what a human clicks; an agent names the row, as the values of a field
    std::string typed;
    const bool named = EditorModal::pathField("path", typed);
    if (named) pickerMiss.clear();
    ImGui::BeginChild("##candidates", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    for (const AssetDatabase::AssetInfo& info : candidates) {
        const bool clicked = ImGui::Selectable(info.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if ((clicked || (named && info.path == typed)) && pickerDone) {
            pickerDone(info);
            pickerDone = nullptr;
            ImGui::CloseCurrentPopup();
        }
    }
    if (named && pickerDone) pickerMiss = "No " + std::string(assetTypeName(pickerType)) + " asset is at " + typed + ".";
    if (candidates.empty()) {
        ImGui::TextDisabled("No %s asset matches", assetTypeName(pickerType));
    }
    ImGui::EndChild();
    if (EditorModal::button("Cancel", "cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        pickerDone = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MissingAssetsWindow::drawGroup(const MissingAssetTracker::Group& group, size_t index) {
    ImGui::PushID(static_cast<int>(index));
    const bool prefab = group.expected == AssetType::Prefab;
    std::string header = ICON_FA_TRIANGLE_EXCLAMATION " " + (group.lastPath.empty() ? std::string("(no path)") : group.lastPath);
    header += "  (" + std::to_string(group.referrers.size()) + (group.referrers.size() == 1 ? " reference)" : " references)");
    ImGui::PushStyleColor(ImGuiCol_Text, missingColor);
    const bool open = ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", header.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Missing %s\nGUID %s\nLast path: %s", assetTypeName(group.expected),
                          group.guid.isNull() ? "(none, a reference by path)" : group.guid.str().c_str(), group.lastPath.c_str());
    }
    if (!open) {
        ImGui::PopID();
        return;
    }

    // whole-group actions
    if (!prefab && !group.guid.isNull()) {
        if (ImGui::SmallButton("Retarget All...")) {
            const Guid missing = group.guid;
            openPicker(group.expected, [this, missing](const AssetDatabase::AssetInfo& info) {
                panelRunProjectCommand(project, new AddFormerGuidCmd(project, info.path, missing), "Retarget All");
            });
        }
        ImGui::SetItemTooltip("Every reference to the missing asset resolves to the chosen one: its GUID becomes a former GUID of it. No referrer file is rewritten.");
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(prefab ? "Replace Prefab..." : "Retarget Rewrite...")) {
        const std::vector<AssetRefTarget> targets = panelGroupTargets(group);
        openPicker(group.expected, [this, targets](const AssetDatabase::AssetInfo& info) {
            panelRunProjectCommand(project, new RetargetAssetRefsCmd(project, targets, AssetRef(info.path, info.guid)), "Retarget");
            project->relinkMissingPrefabs();
        });
    }
    ImGui::SetItemTooltip(prefab ? "The instances use the chosen prefab; their overrides stay" : "The referencing fields are rewritten to the chosen asset");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear All")) {
        panelRunProjectCommand(project, new RetargetAssetRefsCmd(project, panelGroupTargets(group), AssetRef()), "Clear All");
    }

    for (const MissingAssetTracker::Referrer& referrer : group.referrers) {
        const std::string key = selectionKey(referrer);
        bool checked = selected.count(key) != 0;
        ImGui::PushID(key.c_str());
        if (ImGui::Checkbox("##pick", &checked)) {
            if (checked) selected.insert(key); else selected.erase(key);
        }
        ImGui::SameLine();
        if (ImGui::Selectable(referrerLabel(referrer).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            locateAssetReferrer(project, referrer.ownerPath, referrer.entity);
        }
        if (ImGui::BeginPopupContextItem("##referrer")) {
            if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Locate")) {
                locateAssetReferrer(project, referrer.ownerPath, referrer.entity);
            }
            ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("%s\nDouble-click to locate", referrer.locator.c_str());
        ImGui::PopID();
    }
    ImGui::TreePop();
    ImGui::PopID();
}

void MissingAssetsWindow::show() {
    pumpPendingLocate(project);
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }

    if (!project->getAssetIndex().isReady()) {
        ImGui::TextDisabled(ICON_FA_SPINNER " Indexing...");
        ImGui::End();
        return;
    }
    // unsaved edits count: the lists follow the loaded documents too
    MissingAssetTracker& tracker = project->getMissingAssets();
    tracker.update(project->getAssetDatabase(), project->getAssetIndex(), ImGui::GetFrameCount() % 30 == 0);

    if (tracker.getGroups().empty() && tracker.getResolvedByPath().empty() && tracker.getOrphanMetas().empty() &&
        tracker.getReadOnlyLinked().empty()) {
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " Every asset reference of the project resolves.");
        ImGui::End();
        drawPicker();
        return;
    }

    // actions on the referrers chosen with the checkboxes
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button("Retarget Selected...")) {
        std::vector<AssetRefTarget> targets;
        for (const std::string& key : selected) targets.push_back(targetFromKey(key));
        openPicker(AssetType::Unknown, [this, targets](const AssetDatabase::AssetInfo& info) {
            panelRunProjectCommand(project, new RetargetAssetRefsCmd(project, targets, AssetRef(info.path, info.guid)), "Retarget Selected");
            selected.clear();
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Selected")) {
        std::vector<AssetRefTarget> targets;
        for (const std::string& key : selected) targets.push_back(targetFromKey(key));
        panelRunProjectCommand(project, new RetargetAssetRefsCmd(project, targets, AssetRef()), "Clear Selected");
        selected.clear();
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::BeginChild("##missing_list");
    if (!tracker.getGroups().empty()) {
        ImGui::SeparatorText("Missing");
        for (size_t i = 0; i < tracker.getGroups().size(); i++) {
            drawGroup(tracker.getGroups()[i], i);
        }
    }
    if (!tracker.getResolvedByPath().empty()) {
        ImGui::SeparatorText("Resolved by path");
        for (size_t i = 0; i < tracker.getResolvedByPath().size(); i++) {
            const MissingAssetTracker::ResolvedByPath& entry = tracker.getResolvedByPath()[i];
            ImGui::PushID(static_cast<int>(1000000 + i));
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_LINK " %s", entry.path.c_str());
            ImGui::SetItemTooltip("%zu references hold the GUID %s, which no asset has; they bound to the file at their path (GUID %s)",
                                  entry.referrers.size(), entry.referencedGuid.str().c_str(), entry.currentGuid.str().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Keep")) {
                panelRunProjectCommand(project, new AddFormerGuidCmd(project, entry.path, entry.referencedGuid), "Keep");
            }
            ImGui::SetItemTooltip("The file keeps these references: the GUID they hold becomes a former GUID of it");
            for (const MissingAssetTracker::Referrer& referrer : entry.referrers) {
                ImGui::Indent();
                if (ImGui::Selectable(referrerLabel(referrer).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    locateAssetReferrer(project, referrer.ownerPath, referrer.entity);
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
    }
    if (!tracker.getOrphanMetas().empty()) {
        ImGui::SeparatorText("Orphan metas");
        for (size_t i = 0; i < tracker.getOrphanMetas().size(); i++) {
            const AssetDatabase::OrphanMeta& orphan = tracker.getOrphanMetas()[i];
            ImGui::PushID(static_cast<int>(2000000 + i));
            ImGui::TextDisabled(ICON_FA_FILE_CIRCLE_QUESTION " %s", orphan.metaPath.c_str());
            ImGui::SetItemTooltip("The meta of %s (GUID %s), which is gone: moved without its meta, or deleted outside the editor",
                                  orphan.assetPath.c_str(), orphan.guid.str().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete Meta")) {
                panelRunProjectCommand(project, new DeleteOrphanMetaCmd(project, orphan.metaPath), "Delete Meta");
            }
            ImGui::PopID();
        }
    }
    if (!tracker.getReadOnlyLinked().empty()) {
        ImGui::SeparatorText("Read-only linked files");
        ImGui::TextDisabled("A linked folder cannot be written: these assets work, and keep their GUIDs in the editor only.");
        for (const std::string& path : tracker.getReadOnlyLinked()) {
            ImGui::TextDisabled(ICON_FA_LOCK " %s", path.c_str());
            ImGui::SetItemTooltip("Its identity (its .meta or GUID line) cannot be written beside the file. Give the folder write access, or copy it into the project, so the GUID is saved with it.");
        }
    }
    ImGui::EndChild();
    ImGui::End();
    drawPicker();
}

// ==================================================================== References

AssetReferencesWindow::AssetReferencesWindow(Project* project) : project(project) {
}

void AssetReferencesWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void AssetReferencesWindow::findReferences(const std::string& path) {
    targetPath = project->normalizeToProjectRelative(path).generic_string();
    targetGuid = project->getAssetDatabase().getAssetGuid(targetPath);
    resultsVersion = UINT64_MAX;
    setOpen(true);
    focusRequested = true;
}

void AssetReferencesWindow::refresh() {
    const uint64_t version = project->getAssetIndex().getVersion() ^ (project->getAssetDatabase().getVersion() << 1);
    if (version == resultsVersion && resultsTransitive == transitive && ImGui::GetFrameCount() % 30 != 0) {
        return;
    }
    resultsVersion = version;
    resultsTransitive = transitive;
    results = project->getAssetIndex().referencesTo(targetGuid, targetPath, transitive, 5000);
}

void AssetReferencesWindow::show() {
    if (!windowOpen) return;
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    if (targetPath.empty()) {
        ImGui::TextDisabled("Right-click an asset in the Project window and choose Find References.");
        ImGui::End();
        return;
    }
    ImGui::Text(ICON_FA_MAGNIFYING_GLASS " %s", targetPath.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Transitive", &transitive);
    ImGui::SetItemTooltip("Also who references the referrers (the scenes using a material that uses this texture)");
    if (!project->getAssetIndex().isReady()) {
        ImGui::TextDisabled(ICON_FA_SPINNER " Indexing...");
        ImGui::End();
        return;
    }
    refresh();
    if (results.empty()) {
        ImGui::TextDisabled("No references.");
        ImGui::End();
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##references", 4, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Owner");
        ImGui::TableSetupColumn("GameObject");
        ImGui::TableSetupColumn("Component");
        ImGui::TableSetupColumn("Field");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < results.size(); i++) {
            const DepEdge& edge = results[i];
            const std::string owner = project->getAssetIndex().referrerPath(edge.referrer);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(owner.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                locateAssetReferrer(project, owner, edge.entity);
            }
            ImGui::SetItemTooltip("%s\n%s\nDouble-click to open it", edge.field.c_str(), depEdgeKindName(edge.kind));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(edge.entityPath.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(edge.component.c_str());
            ImGui::TableNextColumn();
            const size_t slash = edge.field.find('/');
            std::string field = slash == std::string::npos ? edge.field : edge.field.substr(slash + 1);
            if (!edge.component.empty() && field.compare(0, edge.component.size() + 1, edge.component + ".") == 0) {
                field = field.substr(edge.component.size() + 1);
            }
            if (edge.line) field = "line " + std::to_string(edge.line);
            ImGui::TextUnformatted(field.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
