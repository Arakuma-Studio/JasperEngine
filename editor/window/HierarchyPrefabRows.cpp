// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Hierarchy's prefab rows, Unity's way: added GameObjects get a "+" badge, Missing Prefab
// roots are red with "(Missing Prefab)" and their own Prefab menu, instance roots a ">"
// button opening their prefab, and Replace Prefab (Prefab > Replace..., or a prefab asset
// dropped on an instance root with Alt held). See editor/project/PREFABS.md, sections 13-14.

#include "HierarchyWindow.h"
#include "EditorModal.h"
#include "pose/PoseEditSession.h"

#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "Out.h"
#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ReplacePrefabCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/CreatePrefabVariantAssetCmd.h"
#include "Stream.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "util/UIUtils.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <functional>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {

    const ImVec4 ADDED_BADGE_COLOR = ImVec4(0.55f, 0.85f, 0.45f, 1.0f);       // Unity's green "+"
    const ImVec4 MISSING_BADGE_COLOR = ImVec4(0.96f, 0.36f, 0.36f, 1.0f);
    const ImVec4 ARROW_COLOR = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    const ImVec4 VARIANT_BADGE_COLOR = ImVec4(0.55f, 0.80f, 1.0f, 1.0f);

    // The GameObjects of a scene's prefab instances
    struct SceneInstanceSets {
        std::unordered_set<Entity> members;   // members of an instance (nested roots included)
        std::unordered_set<Entity> roots;     // roots of a live instance
    };

    std::string lowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

}

void editor::HierarchyWindow::markPrefabRows(TreeNode& root) {
    std::unordered_map<uint32_t, SceneInstanceSets> sets;
    auto setsFor = [&](uint32_t sceneId) -> const SceneInstanceSets& {
        auto it = sets.find(sceneId);
        if (it != sets.end()) {
            return it->second;
        }
        SceneInstanceSets& result = sets[sceneId];
        for (const auto& [path, prefab] : project->getPrefabs(sceneId)) {
            auto sceneIt = prefab->instances.find(sceneId);
            if (sceneIt == prefab->instances.end()) {
                continue;
            }
            for (const Prefab::Instance& instance : sceneIt->second) {
                result.roots.insert(instance.rootEntity);
                for (const Prefab::EntityMember& member : instance.members) {
                    result.members.insert(member.localEntity);
                }
            }
        }
        return result;
    };

    // parentKind: 0 outside any instance, 1 in a live instance, 2 below a Missing Prefab
    std::function<void(TreeNode&, int)> visit = [&](TreeNode& node, int parentKind) {
        int kind = 0;
        if (!node.isScene && !node.isChildScene) {
            const uint32_t sceneId = getNodeSceneId(node);
            const SceneInstanceSets& instances = setsFor(sceneId);
            const bool member = instances.members.count(node.id) != 0;
            const bool liveRoot = instances.roots.count(node.id) != 0;

            // Below an instance without being one of its GameObjects: an added GameObject
            // (Unity's "+"); below a Missing Prefab root everything is
            if (node.hasTransform && !node.isPlayCreated && parentKind != 0 && !member) {
                node.isAddedGameObject = true;
                node.isInMissingPrefab = parentKind == 2;
            }
            if (liveRoot) {
                const PrefabComponent* component = nullptr;
                if (SceneProject* nodeScene = project->getScene(sceneId)) {
                    component = nodeScene->scene ? nodeScene->scene->findComponent<PrefabComponent>(node.id) : nullptr;
                }
                node.canOpenPrefab = component && !component->path.empty();
                // Unity's variant icon: the prefab cube with an arrow (drawPrefabRowOverlay)
                node.isPrefabVariant = component && isPrefabVariantAsset(component->path);
            }
            if (node.isMissingPrefab) {
                kind = 2;
            } else if (member || liveRoot) {
                kind = 1;
            } else if (parentKind == 2) {
                kind = 2;   // what the scene added below a Missing Prefab stays its own
            }
        }
        for (TreeNode& child : node.children) {
            visit(child, kind);
        }
    };
    visit(root, 0);
}

void editor::HierarchyWindow::drawPrefabRowOverlay(const TreeNode& node, const ImVec2& rowMin, const ImVec2& rowMax) {
    if (node.isScene || node.isChildScene) {
        return;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing();
    const float iconWidth = ImGui::CalcTextSize(node.icon.c_str()).x;
    // An instance root of a Prefab Variant: Unity's variant icon, the cube with an arrow
    if (node.isPrefabVariant) {
        const float fontSize = ImGui::GetFontSize();
        const float top = rowMin.y + (rowMax.y - rowMin.y - fontSize) * 0.5f;
        UIUtils::drawPrefabVariantBadge(drawList, ImVec2(iconX, top), ImVec2(iconX + iconWidth, top + fontSize),
            ImGui::GetColorU32(VARIANT_BADGE_COLOR));
        if (ImGui::IsMouseHoveringRect(ImVec2(iconX - fontSize * 0.2f, rowMin.y), ImVec2(iconX + iconWidth, rowMax.y)) && !ImGui::GetDragDropPayload()) {
            const SceneProject* sceneProject = project->getScene(getNodeSceneId(node));
            const PrefabComponent* component = sceneProject && sceneProject->scene ?
                sceneProject->scene->findComponent<PrefabComponent>(node.id) : nullptr;
            const std::filesystem::path base = component ? project->getPrefabFileBase(component->path) : std::filesystem::path();
            ImGui::SetTooltip("Prefab Variant instance%s%s", base.empty() ? "" : " (base: ", base.empty() ? "" : (base.stem().string() + ")").c_str());
        }
    }
    if (!node.isAddedGameObject) {
        return;
    }
    // A small "+" on the lower right of the row's icon (the label starts with it)
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 0.85f;
    const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "+");
    const ImVec2 pos(iconX + iconWidth - textSize.x * 0.45f, rowMax.y - textSize.y * 0.95f);
    // a dark outline keeps it readable over the icon and the selection highlight
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx || dy) {
                drawList->AddText(font, size, ImVec2(pos.x + dx, pos.y + dy), shadow, "+");
            }
        }
    }
    drawList->AddText(font, size, pos, ImGui::GetColorU32(node.isInMissingPrefab ? MISSING_BADGE_COLOR : ADDED_BADGE_COLOR), "+");
    if (ImGui::IsMouseHoveringRect(ImVec2(iconX, rowMin.y), ImVec2(iconX + iconWidth, rowMax.y)) && !ImGui::GetDragDropPayload()) {
        ImGui::SetTooltip(node.isInMissingPrefab ? "Added to a Missing Prefab instance" : "Added GameObject (Prefab override)");
    }
}

void editor::HierarchyWindow::showPrefabRowArrow(const TreeNode& node) {
    if (!node.canOpenPrefab || node.isScene || node.isChildScene || node.isPlayCreated) {
        return;
    }
    const char* icon = ICON_FA_CHEVRON_RIGHT;
    const float buttonWidth = ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float buttonPosX = ImGui::GetWindowContentRegionMax().x - buttonWidth;
    if (buttonPosX > ImGui::GetCursorPosX() + 10.0f) {
        ImGui::SameLine(buttonPosX);
    } else {
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    ImGui::PushStyleColor(ImGuiCol_Text, ARROW_COLOR);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushID("OpenPrefabArrow");
    if (ImGui::SmallButton(icon)) {
        const SceneProject* sceneProject = project->getScene(getNodeSceneId(node));
        const PrefabComponent* component = sceneProject && sceneProject->scene ?
            sceneProject->scene->findComponent<PrefabComponent>(node.id) : nullptr;
        if (component) {
            // a GameObject of a child scene shown inline opens its prefab alone
            const bool shownScene = getNodeSceneId(node) == project->getSelectedSceneId();
            openPrefabFromHierarchy(component->path, shownScene ? static_cast<Entity>(node.id) : NULL_ENTITY);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Prefab Asset");
    }
    ImGui::PopID();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

bool editor::HierarchyWindow::isPrefabVariantAsset(const std::filesystem::path& path) const {
    // A Prefab Variant: its root is an instance of its base (PREFABS.md 19), read from the
    // loaded asset or, cached, from its file
    return !path.empty() && !project->getPrefabFileBase(path).empty();
}

void editor::HierarchyWindow::requestCreateVariant(uint32_t sceneId, Entity root) {
    // Unity's Create Prefab Variant from an instance: the variant goes next to the base,
    // named after it; the instance becomes an instance of it
    SceneProject* sceneProject = project->getScene(sceneId);
    std::filesystem::path base;
    if (!sceneProject || !sceneProject->scene || !project->findInstanceRootedAt(sceneProject, root, &base)) {
        return;
    }
    const std::filesystem::path path = CreatePrefabVariantAssetCmd::defaultPath(project, base);
    // a posed character goes back to its rest bones first, so they are what gets copied, and a
    // clip preview of the instance restores (9.R7)
    PoseEditSession::suspendActive();
    PoseEditSession::suspendActiveFor(sceneId, {root});
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project, sceneId,
        {{path, Stream::encodeEntity(root, sceneProject->scene, project, sceneProject)}}, true));
    revealInProject(path);
}

void editor::HierarchyWindow::showMissingPrefabMenu(const TreeNode& node) {
    const uint32_t sceneId = getNodeSceneId(node);
    const MissingPrefabInstance* missing = project->getMissingPrefab(sceneId, node.id);
    const std::string path = missing ? missing->path : node.nestedPrefabFilepath.generic_string();
    const std::string guid = missing ? missing->guid : std::string();

    ImGui::TextColored(MISSING_BADGE_COLOR, ICON_FA_TRIANGLE_EXCLAMATION "  Missing Prefab");
    ImGui::TextDisabled("Last path: %s", path.empty() ? "unknown" : path.c_str());
    ImGui::TextDisabled("GUID: %s", guid.empty() ? "none" : guid.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_COPY "  Copy GUID", nullptr, false, !guid.empty())) {
        ImGui::SetClipboardText(guid.c_str());
    }
    // Where the asset was: its folder in the Project window, when the folder is still there
    std::filesystem::path folder = std::filesystem::path(path).parent_path();
    std::error_code ec;
    const bool folderExists = !folder.empty() && std::filesystem::is_directory(project->getProjectPath() / folder, ec);
    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Select Missing Asset Folder", nullptr, false, folderExists)) {
        revealInProject(folder);
    }
    ImGui::SetItemTooltip("Shows the folder the prefab was in; a .prefab with this GUID put back anywhere relinks the instance");

    ImGui::Separator();
    const bool canReplace = project->canReplacePrefab(sceneId, node.id);
    if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Replace...", nullptr, false, canReplace)) {
        requestReplacePrefab(sceneId, {node.id});
    }
    ImGui::SetItemTooltip("%s", canReplace ? "Makes it an instance of another prefab (relinks it)"
        : "A Missing Prefab inside another instance belongs to that prefab");
    std::vector<Entity> targets = project->getSelectedEntities(sceneId);
    if (targets.empty() || !project->isSelectedEntity(sceneId, node.id)) {
        targets = {node.id};
    }
    std::vector<Entity> missingRoots;
    for (Entity entity : targets) {
        if (project->canReplacePrefab(sceneId, entity) && project->isMissingPrefab(sceneId, entity)) {
            missingRoots.push_back(entity);
        }
    }
    if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Unpack", nullptr, false, !missingRoots.empty())) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new UnpackMissingPrefabCmd(project, sceneId, missingRoots));
    }
    ImGui::SetItemTooltip("%s", project->isAnyScenePlaying() ? "Stop the scene to unpack"
        : "Makes it a plain GameObject; the overrides kept for the missing asset are discarded");
    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove", nullptr, false, !node.isLocked && !project->isAnyScenePlaying())) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project, sceneId, targets));
    }
    ImGui::SetItemTooltip("Deletes the Missing Prefab instance from the scene");
}

void editor::HierarchyWindow::requestReplacePrefab(uint32_t sceneId, const std::vector<Entity>& roots) {
    replaceRequest.sceneId = sceneId;
    replaceRequest.roots = roots;
}

void editor::HierarchyWindow::showReplacePrefabPopup() {
    static constexpr const char* POPUP = "Replace Prefab##ReplacePrefabPopup";
    if (replaceRequest.sceneId != NULL_PROJECT_SCENE) {
        replaceTargets = replaceRequest;
        replaceRequest = ReplacePrefabRequest();
        replaceFilter[0] = '\0';
        replaceChoice.clear();
        replaceCandidates = project->findPrefabFiles();
        std::sort(replaceCandidates.begin(), replaceCandidates.end());
        ImGui::OpenPopup(POPUP);
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(460.0f), Theme::dpi(440.0f)), ImGuiCond_Appearing);
    bool open = true;
    if (!EditorModal::begin(POPUP, &open, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    SceneProject* sceneProject = project->getScene(replaceTargets.sceneId);
    std::vector<Entity> roots = sceneProject ?
        ReplacePrefabCmd::replaceableRoots(project, replaceTargets.sceneId, replaceTargets.roots) : std::vector<Entity>();
    if (roots.empty() || !open) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // What is replaced
    std::string current;
    if (roots.size() == 1) {
        const PrefabComponent* component = sceneProject->scene->findComponent<PrefabComponent>(roots.front());
        current = component ? component->path : std::string();
        ImGui::Text("Replace the prefab of '%s'", sceneProject->scene->getEntityName(roots.front()).c_str());
        EditorModal::message("Replace the prefab of '" + sceneProject->scene->getEntityName(roots.front()) + "' (now " + current +
                             "): pick the new prefab in the list");
        ImGui::TextDisabled("Now: %s%s", current.c_str(), project->isMissingPrefab(sceneProject->id, roots.front()) ? " (missing)" : "");
    } else {
        ImGui::Text("Replace the prefab of %zu instances", roots.size());
        EditorModal::message("Replace the prefab of " + std::to_string(roots.size()) + " instances: pick the new prefab in the list");
    }
    ImGui::Spacing();

    // an agent names the row a human clicks
    std::vector<std::string> prefabOptions;
    for (const std::filesystem::path& candidate : replaceCandidates) {
        if (roots.size() != 1 || candidate.generic_string() != current) prefabOptions.push_back(candidate.generic_string());
    }
    EditorModal::enumField("prefab", replaceChoice, prefabOptions);
    UIUtils::searchInput("##ReplacePrefabSearch", "Search prefabs", replaceFilter, sizeof(replaceFilter), ImGui::IsWindowAppearing(), nullptr, -1.0f);
    const std::string filter = lowered(replaceFilter);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 4.0f + style.ItemSpacing.y * 3.0f;
    ProjectWindow* projectWindow = Backend::getApp().getProjectWindow();
    if (ImGui::BeginChild("##ReplacePrefabList", ImVec2(0, std::max(Theme::dpi(80.0f), ImGui::GetContentRegionAvail().y - footer)), ImGuiChildFlags_Borders)) {
        const float rowHeight = Theme::dpi(28.0f);
        size_t shown = 0;
        for (const std::filesystem::path& candidate : replaceCandidates) {
            const std::string text = candidate.generic_string();
            if (roots.size() == 1 && text == current) {
                continue;
            }
            if (!filter.empty() && lowered(text).find(filter) == std::string::npos) {
                continue;
            }
            shown++;
            ImGui::PushID(text.c_str());
            const bool selected = replaceChoice == text;
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##candidate", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight))) {
                replaceChoice = text;
            }
            const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            ImGui::SetCursorScreenPos(rowStart);
            int width = 0;
            int height = 0;
            ImTextureID thumbnail = projectWindow ? projectWindow->getAssetThumbnail(project->getProjectPath() / candidate, width, height) : ImTextureID{};
            if (thumbnail && width > 0 && height > 0) {
                ImGui::Image(thumbnail, ImVec2(rowHeight, rowHeight));
            } else {
                ImGui::Dummy(ImVec2(rowHeight, rowHeight));
                ImGui::GetWindowDrawList()->AddText(ImVec2(rowStart.x + rowHeight * 0.25f, rowStart.y + (rowHeight - ImGui::GetFontSize()) * 0.5f),
                    ImGui::GetColorU32(ImVec4(0.48f, 0.68f, 0.98f, 1.0f)), ICON_FA_CUBE);
            }
            ImGui::SameLine();
            const float textY = ImGui::GetCursorPosY() + (rowHeight - ImGui::GetFontSize()) * 0.5f;
            ImGui::SetCursorPosY(textY);
            ImGui::TextUnformatted(candidate.stem().string().c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosY(textY);
            ImGui::TextDisabled("%s", candidate.parent_path().generic_string().c_str());
            ImGui::PopID();
            if (doubleClicked) {
                replaceChoice = text;
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("No other prefab in the project");
        }
    }
    ImGui::EndChild();

    // Unity's PrefabReplacingSettings
    const char* matchNames[] = {"By Name", "By Hierarchy", "Root only"};
    int match = static_cast<int>(replaceSettings.match);
    if (EditorModal::enumField("match_overrides", match, {"By Name", "By Hierarchy", "Root only"})) {
        replaceSettings.match = static_cast<PrefabReplaceSettings::Match>(match);
    }
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (ImGui::Combo("Match Overrides", &match, matchNames, IM_ARRAYSIZE(matchNames))) {
        replaceSettings.match = static_cast<PrefabReplaceSettings::Match>(match);
    }
    ImGui::SetItemTooltip("How the overrides find their GameObject in the new prefab: the same name path from the root, the same child index path, or only the root");
    EditorModal::checkbox("Keep Overrides", "keep_overrides", &replaceSettings.keepOverrides);
    ImGui::SetItemTooltip("Off: only the default overrides (name, position, rotation) stay");
    ImGui::SameLine();
    EditorModal::checkbox("Rename Root", "rename_root", &replaceSettings.renameRoot);
    ImGui::SetItemTooltip("A root still named after its old prefab takes the new prefab's name");

    const float buttonWidth = Theme::dpi(100.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonWidth * 2.0f - style.ItemSpacing.x);
    ImGui::BeginDisabled(replaceChoice.empty());
    const bool replace = EditorModal::button("Replace", "yes", ImVec2(buttonWidth, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    } else if (replace) {
        CommandHandle::get(replaceTargets.sceneId)->addCommandNoMerge(
            new ReplacePrefabCmd(project, replaceTargets.sceneId, roots, replaceChoice, replaceSettings));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool editor::HierarchyWindow::handlePrefabReplaceDrop(const TreeNode& node) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyAlt || node.isScene || node.isChildScene) {
        return false;
    }
    const uint32_t sceneId = project->getSelectedSceneId();
    if (getNodeSceneId(node) != sceneId || !project->canReplacePrefab(sceneId, node.id)) {
        return false;
    }
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType("resource_files")) {
        return false;
    }
    std::string prefab;
    for (const std::string& path : Util::getStringsFromPayload(payload)) {
        if (Util::isPrefabFile(path)) {
            prefab = path;
            break;
        }
    }
    if (prefab.empty()) {
        return false;
    }
    std::filesystem::path relative = std::filesystem::path(prefab).lexically_relative(project->getProjectPath());
    if (relative.empty() || relative.begin()->string() == "..") {
        return false;
    }

    PrefabReplaceSettings settings;
    settings.keepOverrides = !io.KeyShift;
    ImGui::SetTooltip(ICON_FA_RIGHT_LEFT "  Replace Prefab of '%s' with '%s'%s", node.name.c_str(), relative.stem().string().c_str(),
        settings.keepOverrides ? "\n(keeps the overrides that match; Alt+Shift clears them)" : "\n(clears the overrides)");
    if (const ImGuiPayload* accepted = ImGui::AcceptDragDropPayload("resource_files")) {
        if (accepted->IsDelivery()) {
            std::vector<Entity> roots = {static_cast<Entity>(node.id)};
            // the whole selection when the row is part of it, like the menu
            if (project->isSelectedEntity(sceneId, node.id)) {
                std::vector<Entity> selected = ReplacePrefabCmd::replaceableRoots(project, sceneId, project->getSelectedEntities(sceneId));
                if (!selected.empty()) {
                    roots = selected;
                }
            }
            CommandHandle::get(sceneId)->addCommandNoMerge(new ReplacePrefabCmd(project, sceneId, roots, relative, settings));
        }
    }
    return true;
}

// ---- Restructuring a Prefab instance (Unity refuses it) ----

bool editor::HierarchyWindow::refusePrefabRestructure(uint32_t sceneId, const std::vector<Entity>& entities) {
    for (Entity entity : entities) {
        std::filesystem::path prefab;
        Entity instanceRoot = NULL_ENTITY;
        if (!MoveEntityOrderCmd::getPrefabRestructureBlock(project, sceneId, entity, &prefab, &instanceRoot).empty()) {
            SceneProject* sceneProject = project->getScene(sceneId);
            restructureRefusal.pending = true;
            restructureRefusal.delay = 1;
            restructureRefusal.prefab = prefab;
            restructureRefusal.instanceRoot = instanceRoot;
            restructureRefusal.name = sceneProject && sceneProject->scene ? sceneProject->scene->getEntityName(entity) : std::string();
            Out::warning("Cannot move '%s': %s", restructureRefusal.name.c_str(), MoveEntityOrderCmd::PREFAB_RESTRUCTURE_MESSAGE);
            return true;
        }
    }
    return false;
}

void editor::HierarchyWindow::showRestructurePopup() {
    static constexpr const char* POPUP = "Cannot restructure Prefab instance##PrefabRestructure";
    static RestructureRefusal shown;
    // a frame after the drop: the drop's own frame (its release, the drag ending) closes a
    // popup opened in it
    if (restructureRefusal.pending && restructureRefusal.delay-- <= 0) {
        shown = restructureRefusal;
        restructureRefusal = RestructureRefusal();
        ImGui::OpenPopup(POPUP);
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin(POPUP, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    EditorModal::message(std::string(MoveEntityOrderCmd::PREFAB_RESTRUCTURE_MESSAGE) +
                         (shown.name.empty() ? std::string() : " ('" + shown.name + "' belongs to " + shown.prefab.generic_string() + ")"));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Theme::dpi(380.0f));
    ImGui::TextUnformatted(MoveEntityOrderCmd::PREFAB_RESTRUCTURE_MESSAGE);
    if (!shown.name.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("'%s' belongs to %s", shown.name.c_str(), shown.prefab.generic_string().c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const float buttonWidth = Theme::dpi(110.0f);
    ImGui::Dummy(ImVec2(Theme::dpi(380.0f) - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, 0.0f));
    ImGui::SameLine();
    ImGui::BeginDisabled(shown.prefab.empty());
    const bool open = EditorModal::button("Open Prefab", "yes", ImVec2(buttonWidth, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    } else if (open) {
        ImGui::CloseCurrentPopup();
        // Prefab Mode in context of the instance holding it
        openPrefabFromHierarchy(shown.prefab, shown.instanceRoot);
    }
    ImGui::EndPopup();
}
