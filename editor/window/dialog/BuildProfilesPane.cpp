// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildProfilesPane.h"

#include "AppSettings.h"
#include "Project.h"
#include "SettingsCategoryList.h"
#include "Theme.h"
#include "command/type/EditBuildProfileCmd.h"
#include "external/IconsFontAwesome6.h"
#include "organisation/AssetLabels.h"
#include "publish/Publishing.h"
#include "publish/StoreLimits.h"
#include "settings/SettingsRegistry.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace jasperengine::editor {

namespace {

    namespace fs = std::filesystem;

    const char* const kProfilePlatforms[] = {"windows", "linux", "macos", "android", "ios", "web"};
    const char* const kProfilePlatformLabels[] = {"Windows", "Linux", "macOS", "Android", "iOS", "Web"};

    void copyText(char* buffer, size_t size, const std::string& text) {
        std::snprintf(buffer, size, "%s", text.c_str());
    }

    std::optional<std::string> paneFileText(const fs::path& file) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return std::nullopt;
        std::ifstream stream(file, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    void beginPaneRow(const char* label) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool beginPaneTable(const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(150.0f));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    std::string sceneGuidOf(Project* project, const SceneProject& scene) {
        return scene.filepath.empty() ? std::string() : project->getAssetDatabase().getAssetGuid(scene.filepath).str();
    }

}

void BuildProfilesPane::reset() {
    notice.clear();
    selected.clear();
    renaming.clear();
    confirmDelete.clear();
    loadedName.clear();
    loadedRevision = 0;
    message.clear();
}

fs::path BuildProfilesPane::outputDir(Project* project) const {
    const buildprofiles::View& view = buildprofiles::view(project->getProjectPath());
    const BuildProfileEntry* entry = buildprofiles::find(view.entries, selected);
    if (!entry || !entry->readable) return fs::path();
    if (!entry->profile.outputDir.empty()) {
        fs::path dir = FileUtils::pathFromUtf8(entry->profile.outputDir);
        if (dir.is_relative()) dir = project->getProjectPath() / dir;
        return dir.lexically_normal();
    }
    // Build Settings' folder for the platform (the key build_project and the window share)
    const BuildPlatform platform = entry->profile.platform;
    const std::string key = platform == hostBuildPlatform() ? "desktop" : platform == BuildPlatform::Web ? "web"
                                                                         : std::string("player-") + buildPlatformName(platform);
    const fs::path remembered = AppSettings::getExportTargetDir(project->getProjectPath() / "project.yaml", key);
    if (!remembered.empty()) return remembered;
    const fs::path base = AppSettings::getDefaultExportDirectory();
    if (!base.empty()) return base / (project->getName() + "-" + entry->name);
    return project->getProjectInternalPath() / "builds" / entry->name;
}

bool BuildProfilesPane::writeFiles(Project* project, const std::string& step, std::vector<std::pair<fs::path, std::string>> writes,
                                   std::vector<fs::path> removes, const std::string& mergeKey,
                                   const std::optional<std::pair<std::string, std::string>>& activeNameChange) {
    std::vector<EditBuildProfileCmd::FileChange> changes;
    for (const fs::path& file : removes) changes.push_back({file, paneFileText(file), std::nullopt});
    for (auto& [file, text] : writes) {
        // a file the step also removes (a rename to another letter case, on a case-insensitive
        // filesystem) did not exist before under its new name
        const bool removed = std::any_of(removes.begin(), removes.end(), [&file](const fs::path& gone) {
            std::error_code ec;
            return fs::equivalent(gone, file, ec);
        });
        changes.push_back({file, removed ? std::nullopt : paneFileText(file), std::move(text)});
    }
    auto* command = new EditBuildProfileCmd(step, std::move(changes));
    if (activeNameChange) command->setActiveNameChange(project->getProjectPath(), activeNameChange->first, activeNameChange->second);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey(mergeKey);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        message = failure->empty() ? "The Build Profile could not be written." : *failure;
        return false;
    }
    message.clear();
    return true;
}

bool BuildProfilesPane::commit(Project* project, const BuildProfile& next, const std::string& mergeKey) {
    const fs::path file = BuildProfileFile::pathOf(project->getProjectPath(), next.name);
    if (!writeFiles(project, "Edit Build Profile '" + next.name + "'", {{file, BuildProfileFile::encode(next)}}, {}, next.name + ":" + mergeKey))
        return false;
    working = next;
    loadedRevision = buildprofiles::revision();
    return true;
}

void BuildProfilesPane::load(const BuildProfile& profile) {
    working = profile;
    loadedName = profile.name;
    copyText(productName, sizeof(productName), profile.productName);
    copyText(identifier, sizeof(identifier), profile.identifier);
    copyText(versionSuffix, sizeof(versionSuffix), profile.versionSuffix);
    copyText(icon, sizeof(icon), profile.icon);
    copyText(storeTitle, sizeof(storeTitle), profile.storeTitle);
    copyText(outputFolder, sizeof(outputFolder), profile.outputDir);
    fileChangedWhileTyping = false;
}

void BuildProfilesPane::drawList(Project* project, float height) {
    const fs::path projectPath = project->getProjectPath();
    const buildprofiles::View& view = buildprofiles::view(projectPath);

    // the agents' way in: the selected profile, by name ("" none)
    std::vector<std::string> names = {""};
    for (const BuildProfileEntry& entry : view.entries) names.push_back(entry.name);
    std::string chosen = selected;
    if (EditorModal::enumField("build_profile", chosen, names)) selected = chosen;

    if (ImGui::BeginChild("##BuildProfileList", ImVec2(0, height), ImGuiChildFlags_Borders)) {
        for (const BuildProfileEntry& entry : view.entries) {
            ImGui::PushID(entry.name.c_str());
            const bool active = !view.activeName.empty() && buildprofiles::find({entry}, view.activeName) != nullptr;
            std::string label = entry.readable ? std::string(entry.profile.platform == BuildPlatform::Web ? ICON_FA_GLOBE : ICON_FA_GAMEPAD) + "  " + entry.name
                                               : std::string(ICON_FA_TRIANGLE_EXCLAMATION) + "  " + entry.name;
            if (active) label += "  (Active)";
            if (renaming == entry.name) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    std::string why;
                    const std::string target = renameBuffer;
                    const BuildProfileEntry* taken = buildprofiles::find(view.entries, target);
                    if (!BuildProfileFile::isValidName(target, &why)) message = why;
                    else if (taken && taken->name != entry.name) message = "Build Profile '" + taken->name + "' exists already";
                    else {
                        std::vector<std::pair<fs::path, std::string>> writes = {{BuildProfileFile::pathOf(projectPath, target), paneFileText(entry.file).value_or("")}};
                        // publishing.json targets naming it follow the rename (Risks)
                        publish::PublishingConfig publishing;
                        std::string error;
                        if (publish::PublishingConfig::load(publish::PublishingConfig::pathOf(projectPath), publishing, error)) {
                            bool renamed = false;
                            for (auto& [targetName, publishTarget] : publishing.targets)
                                if (buildprofiles::find({entry}, publishTarget.profile)) {
                                    publishTarget.profile = target;
                                    renamed = true;
                                }
                            if (renamed) writes.push_back({publish::PublishingConfig::pathOf(projectPath), publishing.toJson()});
                        }
                        // the active profile follows its rename, in the same undo step
                        if (writeFiles(project, "Rename Build Profile '" + entry.name + "'", std::move(writes), {entry.file}, std::string(),
                                       active ? std::optional<std::pair<std::string, std::string>>({entry.name, target}) : std::nullopt)) {
                            if (selected == entry.name) selected = target;
                        }
                    }
                    renaming.clear();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming.clear();
            } else if (ImGui::Selectable(label.c_str(), selected == entry.name)) {
                selected = entry.name;
            }
            if (!entry.readable) ImGui::SetItemTooltip("%s", entry.error.c_str());
            if (ImGui::BeginPopupContextItem("##profile_menu")) {
                if (ImGui::MenuItem("Switch Profile", nullptr, false, entry.readable && !active)) {
                    std::string error;
                    if (!buildprofiles::setActiveName(projectPath, entry.name, error)) message = error;
                }
                if (ImGui::MenuItem("Duplicate")) {
                    std::string copy = entry.name + " Copy";
                    for (int n = 2; buildprofiles::find(view.entries, copy); n++) copy = entry.name + " Copy " + std::to_string(n);
                    if (writeFiles(project, "Duplicate Build Profile '" + entry.name + "'",
                                   {{BuildProfileFile::pathOf(projectPath, copy), paneFileText(entry.file).value_or("")}}, {}))
                        selected = copy;
                }
                if (ImGui::MenuItem("Rename")) {
                    renaming = entry.name;
                    copyText(renameBuffer, sizeof(renameBuffer), entry.name);
                }
                if (ImGui::MenuItem("Delete")) confirmDelete = entry.name;
                ImGui::EndPopup();
            }
            if (confirmDelete == entry.name) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Delete '%s'?", entry.name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    if (writeFiles(project, "Delete Build Profile '" + entry.name + "'", {}, {entry.file}) && selected == entry.name) selected.clear();
                    confirmDelete.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) confirmDelete.clear();
            }
            ImGui::PopID();
        }
        if (view.entries.empty()) ImGui::TextDisabled("No Build Profiles: a platform builds the project as it is.");
    }
    ImGui::EndChild();

    // Add Build Profile: a name and a platform
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    EditorModal::inputTextWithHint("##new_profile_name", "new_profile_name", "Profile name", newName, sizeof(newName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::vector<std::string> platforms(std::begin(kProfilePlatformLabels), std::end(kProfilePlatformLabels));
    EditorModal::combo("##new_profile_platform", "new_profile_platform", &newPlatform, platforms);
    if (EditorModal::button("Add Build Profile", "add_build_profile", ImVec2(-FLT_MIN, 0))) {
        std::string why;
        const std::string name = newName;
        if (!BuildProfileFile::isValidName(name, &why)) {
            message = why;
        } else if (const BuildProfileEntry* taken = buildprofiles::find(view.entries, name)) {
            message = "Build Profile '" + taken->name + "' exists already";
        } else {
            BuildProfile profile;
            profile.name = name;
            parseBuildPlatform(kProfilePlatforms[std::clamp(newPlatform, 0, 5)], profile.platform);
            if (writeFiles(project, "Create Build Profile '" + name + "'", {{BuildProfileFile::pathOf(projectPath, name), BuildProfileFile::encode(profile)}}, {})) {
                selected = name;
                newName[0] = '\0';
            }
        }
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
}

void BuildProfilesPane::drawIdentity(Project* project, BuildProfile& next, bool& changed, std::string& key) {
    const auto resolved = buildprofiles::resolveIdentity(*project, [&] {
        BuildProfile inherited = next;
        inherited.productName.clear();
        inherited.identifier.clear();
        inherited.icon.clear();
        return inherited;
    }());
    auto text = [&](const char* label, const char* name, const char* field, char* buffer, size_t size, std::string& value) {
        beginPaneRow(label);
        const bool inherited = resolved.count(field) && !resolved.at(field).value.empty();
        const std::string hint = inherited ? resolved.at(field).value + "  (" + resolved.at(field).source + ")" : std::string();
        if (EditorModal::inputTextWithHint((std::string("##") + name).c_str(), name, hint.c_str(), buffer, size)) {
            value = buffer;
            changed = true;
            key = name;
        }
    };
    if (!ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!beginPaneTable("##identity")) return;
    text("Product Name", "profile_product_name", "product_name", productName, sizeof(productName), next.productName);
    text("Identifier", "profile_identifier", "identifier", identifier, sizeof(identifier), next.identifier);
    beginPaneRow("Version Suffix");
    if (EditorModal::inputTextWithHint("##profile_version_suffix", "profile_version_suffix", "-demo", versionSuffix, sizeof(versionSuffix))) {
        next.versionSuffix = versionSuffix;
        changed = true;
        key = "version_suffix";
    }
    text("Icon", "profile_icon", "icon", icon, sizeof(icon), next.icon);
    ImGui::EndTable();
}

void BuildProfilesPane::drawStore(BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Store", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!beginPaneTable("##store")) return;
    beginPaneRow("Store");
    std::vector<std::string> stores = buildprofiles::storeNames();
    std::string store = next.store.empty() ? "none" : next.store;
    if (EditorModal::enumField("profile_store", store, stores)) {
        next.store = store == "none" ? std::string() : store;
        changed = true;
        key = "store";
    }
    if (ImGui::BeginCombo("##profile_store", store.c_str())) {
        for (const std::string& option : stores)
            if (ImGui::Selectable(option.c_str(), option == store)) {
                next.store = option == "none" ? std::string() : option;
                changed = true;
                key = "store";
            }
        ImGui::EndCombo();
    }
    beginPaneRow("Store Title");
    // the live counter against the store's limit (R16)
    size_t limit = 0;
    for (const publish::StoreLimitRow& row : publish::storeLimitRows())
        if (next.store == row.store && std::string(row.checked) == "store_title") limit = row.limit;
    const float counterWidth = limit ? ImGui::CalcTextSize("000 / 000").x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - counterWidth);
    if (EditorModal::inputText("##profile_store_title", "profile_store_title", storeTitle, sizeof(storeTitle))) {
        next.storeTitle = storeTitle;
        changed = true;
        key = "store_title";
    }
    if (limit) {
        const size_t length = publish::codePoints(storeTitle);
        ImGui::SameLine();
        const ImVec4 color = length > limit ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(color, "%zu / %zu", length, limit);
        if (length > limit) {
            const std::string warning = "W-STORE-TITLE-LENGTH: " + next.store + " allows " + std::to_string(limit) + " characters";
            // a warning, not a refusal: the dialog's message carries it for agents (ExportWindow)
            notice = "Build Profile '" + next.name + "': " + warning;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextColored(color, ICON_FA_TRIANGLE_EXCLAMATION " %s", warning.c_str());
        }
    }
    ImGui::EndTable();
}

void BuildProfilesPane::drawBuild(Project* project, BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Build", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!beginPaneTable("##build")) return;
    beginPaneRow("Graphics API");
    std::vector<std::string> apis = {""};
    for (const std::string& backend : platformBackends(next.platform)) apis.push_back(backend);
    std::string api = next.backend;
    if (EditorModal::enumField("profile_backend", api, apis)) {
        next.backend = api;
        changed = true;
        key = "backend";
    }
    if (ImGui::BeginCombo("##profile_backend", api.empty() ? ("Default (" + apis[1] + ")").c_str() : api.c_str())) {
        for (const std::string& option : apis)
            if (ImGui::Selectable(option.empty() ? "Default" : option.c_str(), option == next.backend)) {
                next.backend = option;
                changed = true;
                key = "backend";
            }
        ImGui::EndCombo();
    }
    beginPaneRow("Development Build");
    int development = !next.development ? 0 : *next.development ? 1 : 2;
    const std::vector<std::string> developmentOptions = {std::string("Build Settings' (") + (project->isDevelopmentBuild() ? "on" : "off") + ")", "On", "Off"};
    if (EditorModal::combo("##profile_development", "profile_development", &development, developmentOptions)) {
        next.development = development == 0 ? std::nullopt : std::optional<bool>(development == 1);
        changed = true;
        key = "development";
    }
    beginPaneRow("Output Folder");
    if (EditorModal::inputTextWithHint("##profile_output_dir", "profile_output_dir", "Build Settings' folder for the platform", outputFolder, sizeof(outputFolder))) {
        next.outputDir = outputFolder;
        changed = true;
        key = "output_dir";
    }
    ImGui::EndTable();
}

void BuildProfilesPane::drawSceneList(Project* project, BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Scene List", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool overrideScenes = next.scenes.has_value();
    if (EditorModal::checkbox("Override Scenes In Build", "profile_override_scenes", &overrideScenes)) {
        if (overrideScenes) {
            std::vector<BuildProfileScene> scenes;
            for (const SceneProject& scene : project->getScenes())
                if (!scene.filepath.empty()) scenes.push_back({sceneGuidOf(project, scene), FileUtils::pathToGenericUtf8(scene.filepath)});
            next.scenes = std::move(scenes);
        } else {
            next.scenes.reset();
            next.startScene.clear();
        }
        changed = true;
        key = "scenes";
    }
    ImGui::SetItemTooltip("Off: the project's Scenes In Build");
    if (!next.scenes) return;
    // involved scenes ship too, greyed: "(added: involved by Level1)"
    std::vector<BuildProfileScene>& scenes = *next.scenes;
    int moveFrom = -1, moveTo = -1, remove = -1;
    for (size_t i = 0; i < scenes.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        const bool start = next.startScene.empty() ? i == 0 : next.startScene == scenes[i].guid;
        if (ImGui::RadioButton("##start", start)) {
            next.startScene = scenes[i].guid;
            changed = true;
            key = "start_scene";
        }
        ImGui::SetItemTooltip("Start scene");
        ImGui::SameLine();
        ImGui::Selectable(scenes[i].path.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - Theme::dpi(28.0f), 0));
        if (ImGui::BeginDragDropSource()) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload("JE_PROFILE_SCENE", &from, sizeof(from));
            ImGui::TextUnformatted(scenes[i].path.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("JE_PROFILE_SCENE")) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = static_cast<int>(i);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MINUS)) remove = static_cast<int>(i);
        // the scenes it involves, which the build adds
        for (const SceneProject& scene : project->getScenes()) {
            if (sceneGuidOf(project, scene) != scenes[i].guid) continue;
            std::vector<uint32_t> involved;
            project->collectInvolvedScenesForExport(scene.id, involved);
            for (uint32_t id : involved) {
                if (id == scene.id) continue;
                const SceneProject* child = project->getScene(id);
                if (child) ImGui::TextDisabled("      %s (added: involved by %s)", child->name.c_str(), scene.name.c_str());
            }
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        BuildProfileScene moved = scenes[moveFrom];
        scenes.erase(scenes.begin() + moveFrom);
        scenes.insert(scenes.begin() + moveTo, moved);
        changed = true;
        key = "scenes";
    }
    if (remove >= 0) {
        if (scenes[remove].guid == next.startScene) next.startScene.clear();
        scenes.erase(scenes.begin() + remove);
        changed = true;
        key = "scenes";
    }
    if (ImGui::Button("Add Open Scenes")) {
        for (const SceneProject& scene : project->getScenes()) {
            const std::string guid = sceneGuidOf(project, scene);
            if (!scene.opened || guid.empty()) continue;
            if (std::none_of(scenes.begin(), scenes.end(), [&](const BuildProfileScene& listed) { return listed.guid == guid; }))
                scenes.push_back({guid, FileUtils::pathToGenericUtf8(scene.filepath)});
        }
        changed = true;
        key = "scenes";
    }
    ImGui::SameLine();
    if (ImGui::BeginCombo("##add_scene", "Add Scene...")) {
        for (const SceneProject& scene : project->getScenes()) {
            const std::string guid = sceneGuidOf(project, scene);
            if (guid.empty() || std::any_of(scenes.begin(), scenes.end(), [&](const BuildProfileScene& listed) { return listed.guid == guid; }))
                continue;
            if (ImGui::Selectable(scene.name.c_str())) {
                scenes.push_back({guid, FileUtils::pathToGenericUtf8(scene.filepath)});
                changed = true;
                key = "scenes";
            }
        }
        ImGui::EndCombo();
    }
}

void BuildProfilesPane::drawDefines(BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Scripting Define Symbols", ImGuiTreeNodeFlags_DefaultOpen)) return;
    int remove = -1;
    for (size_t i = 0; i < next.defines.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BulletText("%s", next.defines[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MINUS)) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        next.defines.erase(next.defines.begin() + remove);
        changed = true;
        key = "defines";
    }
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    const bool entered = EditorModal::inputTextWithHint("##profile_new_define", "profile_new_define", "DEMO", newDefine, sizeof(newDefine),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::SmallButton(ICON_FA_PLUS) || entered) && newDefine[0]) {
        next.defines.push_back(newDefine);
        newDefine[0] = '\0';
        changed = true;
        key = "defines";
    }
    ImGui::SetItemTooltip("Add a Scripting Define Symbol (System.isDefined in Lua; code behind it still ships)");
}

void BuildProfilesPane::drawContent(Project* project, BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Content", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextUnformatted("Exclude Labels");
    int remove = -1;
    for (size_t i = 0; i < next.excludeLabels.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        ImVec4 color(0.4f, 0.4f, 0.4f, 1.0f);
        const std::string hex = project->getAssetLabels().colorOf(next.excludeLabels[i]);
        unsigned int r = 0, g = 0, b = 0;
        if (hex.size() == 7 && std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) color = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        if (ImGui::SmallButton((next.excludeLabels[i] + "  " ICON_FA_XMARK).c_str())) remove = static_cast<int>(i);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PopID();
    }
    if (remove >= 0) {
        next.excludeLabels.erase(next.excludeLabels.begin() + remove);
        changed = true;
        key = "exclude_labels";
    }
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (ImGui::BeginCombo("##add_label", "Add Label...")) {
        for (const AssetLabels::LabelInfo& label : project->getAssetLabels().labels()) {
            const bool listed = std::any_of(next.excludeLabels.begin(), next.excludeLabels.end(),
                                            [&](const std::string& existing) { return AssetLabels::sameLabel(existing, label.name); });
            if (!listed && ImGui::Selectable(label.name.c_str())) {
                next.excludeLabels.push_back(label.name);
                changed = true;
                key = "exclude_labels";
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Exclude Paths");
    remove = -1;
    for (size_t i = 0; i < next.excludePaths.size(); i++) {
        ImGui::PushID(1000 + static_cast<int>(i));
        ImGui::BulletText("%s", next.excludePaths[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MINUS)) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        next.excludePaths.erase(next.excludePaths.begin() + remove);
        changed = true;
        key = "exclude_paths";
    }
    ImGui::SetNextItemWidth(Theme::dpi(260.0f));
    const bool entered = EditorModal::inputTextWithHint("##profile_new_path", "profile_new_path", "music/full/", newPath, sizeof(newPath),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::SmallButton(ICON_FA_PLUS "##path") || entered) && newPath[0]) {
        next.excludePaths.push_back(newPath);
        newPath[0] = '\0';
        changed = true;
        key = "exclude_paths";
    }
    ImGui::SetItemTooltip("A project-relative folder (ending in /) or file the build leaves out, Lua included");
}

void BuildProfilesPane::drawOverrides(Project* project, BuildProfile& next, bool& changed, std::string& key) {
    if (!ImGui::CollapsingHeader("Player Settings Overrides", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const std::vector<std::string> categories = buildprofiles::allowedCategories(next.platform);
    SettingsContext ctx;
    ctx.projectPtr = project;
    std::string removeKey;
    if (beginPaneTable("##overrides")) {
        for (auto& [settingKey, value] : next.overrides) {
            ImGui::PushID(settingKey.c_str());
            const SettingRow* row = SettingsRegistry::get().find(settingKey);
            beginPaneRow(row ? row->label.c_str() : settingKey.c_str());
            ImGui::SetItemTooltip("%s", settingKey.c_str());
            if (row && row->get) {
                ImGui::TextDisabled("Project: %s", row->get(ctx).dump().c_str());
            }
            const float removeWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - removeWidth);
            const std::string type = row ? row->type : std::string();
            if (type == "bool" && value.is_boolean()) {
                bool on = value.get<bool>();
                if (ImGui::Checkbox("##value", &on)) {
                    value = on;
                    changed = true;
                    key = "override:" + settingKey;
                }
            } else if (type == "enum" && row && value.is_string()) {
                if (ImGui::BeginCombo("##value", value.get<std::string>().c_str())) {
                    for (const std::string& option : row->values)
                        if (ImGui::Selectable(option.c_str(), option == value.get<std::string>())) {
                            value = option;
                            changed = true;
                            key = "override:" + settingKey;
                        }
                    ImGui::EndCombo();
                }
            } else if ((type == "int" || type == "number") && value.is_number()) {
                double number = value.get<double>();
                if (ImGui::InputDouble("##value", &number, 0.0, 0.0, type == "int" ? "%.0f" : "%.3f")) {
                    value = type == "int" ? mcp::Json(static_cast<long long>(number)) : mcp::Json(number);
                    changed = true;
                    key = "override:" + settingKey;
                }
            } else {
                // text values as typed, other values as JSON
                std::string text = value.is_string() ? value.get<std::string>() : value.dump();
                char buffer[1024];
                copyText(buffer, sizeof(buffer), text);
                if (ImGui::InputText("##value", buffer, sizeof(buffer))) {
                    const mcp::Json parsed = value.is_string() ? mcp::Json(std::string(buffer)) : mcp::Json::parse(buffer, nullptr, false);
                    if (!parsed.is_discarded()) {
                        value = parsed;
                        changed = true;
                        key = "override:" + settingKey;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK)) removeKey = settingKey;
            ImGui::SetItemTooltip("Remove");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeKey.empty()) {
        next.overrides.erase(removeKey);
        changed = true;
        key = "overrides";
    }
    // Add Override...: a search over the allowed keys (the settings search)
    ImGui::SetNextItemWidth(Theme::dpi(260.0f));
    if (ImGui::BeginCombo("##add_override", "Add Override...", ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##override_search", "Search settings", overrideSearch, sizeof(overrideSearch));
        for (const SettingRow* row : SettingsRegistry::get().search(overrideSearch, SettingsScope::Project)) {
            const std::string category = row->key.substr(0, row->key.find('.'));
            if (!row->mcp || !row->get || next.overrides.count(row->key) ||
                std::find(categories.begin(), categories.end(), category) == categories.end())
                continue;
            if (ImGui::Selectable((row->label + "  (" + row->key + ")").c_str())) {
                next.overrides[row->key] = row->get(ctx);
                changed = true;
                key = "overrides";
            }
        }
        ImGui::EndCombo();
    }
}

bool BuildProfilesPane::drawPane(Project* project) {
    notice.clear();
    const fs::path projectPath = project->getProjectPath();
    const buildprofiles::View& view = buildprofiles::view(projectPath);
    const BuildProfileEntry* entry = buildprofiles::find(view.entries, selected);
    if (!entry) {
        selected.clear();
        ImGui::TextDisabled("The Build Profile is gone.");
        return false;
    }
    if (!entry->readable) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s: %s", entry->name.c_str(), entry->error.c_str());
        return false;
    }
    // the file changed (an undo, a checkout): read again, unless a field is being typed in (R21)
    if (loadedName != entry->name) load(entry->profile);
    else if (loadedRevision != view.revision) {
        if (ImGui::IsAnyItemActive() && !(working.name == entry->profile.name && BuildProfileFile::encode(working) == BuildProfileFile::encode(entry->profile)))
            fileChangedWhileTyping = true;
        else load(entry->profile);
    }
    loadedRevision = view.revision;

    const bool active = buildprofiles::find({*entry}, view.activeName) != nullptr;
    const int platformIndex = static_cast<int>(entry->profile.platform);
    settingsui::boldText((entry->name + " \xe2\x80\x94 " + kProfilePlatformLabels[std::clamp(platformIndex, 0, 5)]).c_str(), 1.15f);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Theme::dpi(120.0f));
    if (active) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Active");
    } else if (EditorModal::button("Switch Profile", "switch_profile", ImVec2(Theme::dpi(120.0f), 0))) {
        std::string error;
        if (!buildprofiles::setActiveName(projectPath, entry->name, error)) message = error;
    }
    if (fileChangedWhileTyping)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "The file changed on disk; your typing is kept until you finish.");

    BuildProfile next = working;
    bool changed = false;
    std::string key;
    drawIdentity(project, next, changed, key);
    drawStore(next, changed, key);
    drawBuild(project, next, changed, key);
    drawSceneList(project, next, changed, key);
    drawDefines(next, changed, key);
    drawContent(project, next, changed, key);
    drawOverrides(project, next, changed, key);
    if (changed) commit(project, next, key);

    const std::vector<std::string> findings = validateProfile(*project, working);
    for (const std::string& finding : findings) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), ICON_FA_CIRCLE_XMARK " %s", finding.c_str());
        ImGui::PopTextWrapPos();
    }
    for (const BuildWarning& warning : buildprofiles::storeFindings(*project, working)) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s: %s", warning.code.c_str(), warning.message.c_str());
        ImGui::PopTextWrapPos();
    }
    return findings.empty();
}

}
