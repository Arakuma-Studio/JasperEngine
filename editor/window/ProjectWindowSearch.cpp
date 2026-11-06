// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project window's search and organisation (S25 R20-R26, R28): the search over the asset
// index with Unity's grammar, its scope bar, Search by Type / Search by Label / Save Search,
// Favorites (saved searches and bookmarked folders and assets) at the top of the folder tree,
// asset labels (the Labels context menu, the dots and chips of the listing), type-ahead and
// Ctrl+wheel item sizes. Type-ahead follows OpenEngine's AssetsPanel (750 ms repeat window,
// A-Z/0-9; 16 px size steps), Apps/Editor/Source/Panels/AssetsPanel.cpp, MIT, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors (NOTICE.md).

#include "ProjectWindow.h"

#include "AppSettings.h"
#include "Backend.h"
#include "App.h"
#include "Configs.h"
#include "Out.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "organisation/Bookmarks.h"
#include "organisation/OrganisationCommands.h"
#include "organisation/SavedSearchRows.h"
#include "search/AssetQuery.h"
#include "window/BookmarksWindow.h"

#include "imgui_internal.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    ImU32 labelColor(const std::string& hex, float alpha = 1.0f) {
        unsigned int value = 0x888888;
        if (AssetLabels::isColor(hex)) value = static_cast<unsigned int>(std::strtoul(hex.c_str() + 1, nullptr, 16));
        return IM_COL32((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, static_cast<int>(255 * alpha));
    }

    // Unity's Search by Type entries: the name shown, the t: value written
    struct TypeChoice {
        const char* label;
        const char* token;
    };
    const TypeChoice typeChoices[] = {
        {"AudioClip", "AudioClip"}, {"Folder", "Folder"}, {"Font", "Font"}, {"Material", "Material"}, {"Model", "Model"},
        {"Prefab", "Prefab"}, {"Scene", "Scene"}, {"Script", "Script"}, {"Shader", "Shader"}, {"TextAsset", "TextAsset"},
        {"Texture2D", "Texture2D"},
    };

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

}

// ---- the search

bool editor::ProjectWindow::hasToken(const std::string& key, const std::string& value) const {
    const AssetQuery query = AssetQuery::parse(searchText);
    for (const AssetQuery::Group& group : query.groups()) {
        for (const AssetQuery::Term& term : group.terms) {
            if (term.key == key && lower(term.value) == lower(value)) return true;
        }
    }
    return false;
}

void editor::ProjectWindow::toggleToken(const std::string& key, const std::string& value) {
    // the search box's text gains or loses "key:value" (Unity's drop-downs edit the same text)
    AssetQuery query = AssetQuery::parse(searchText);
    std::string text;
    bool removed = false;
    for (const AssetQuery::Group& group : query.groups()) {
        std::string groupText;
        for (const AssetQuery::Term& term : group.terms) {
            if (term.key == key && lower(term.value) == lower(value)) {
                removed = true;
                continue;
            }
            std::string part = term.key.empty() ? term.value
                             : term.key == "\"" ? "\"" + term.value + "\""
                             : term.key + ":" + (term.value.find(' ') != std::string::npos ? "\"" + term.value + "\"" : term.value);
            groupText += (groupText.empty() ? "" : " ") + part;
        }
        if (groupText.empty()) continue;
        text += (text.empty() ? "" : " OR ") + groupText;
    }
    if (!removed) text += (text.empty() ? "" : " ") + key + ":" + (value.find(' ') != std::string::npos ? "\"" + value + "\"" : value);
    setSearch(text);
}

void editor::ProjectWindow::setSearch(const std::string& text) {
    std::snprintf(searchBuffer, sizeof(searchBuffer), "%s", text.c_str());
    std::string trimmed = searchBuffer;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    searchText = trimmed;
    selectedFiles.clear();
    if (searchText.empty() && !searchReturnPath.empty()) {
        // leaving an All search: back to the folder it started in
        const fs::path back = searchReturnPath;
        searchReturnPath.clear();
        scanDirectory(back);
        return;
    }
    scanDirectory(currentPath);
}

void editor::ProjectWindow::setSearchScope(bool all) {
    if (all == searchAll) return;
    searchAll = all;
    if (all) {
        searchReturnPath = currentPath;
        scanDirectory(project->getProjectPath());
    } else if (!searchReturnPath.empty()) {
        const fs::path back = searchReturnPath;
        searchReturnPath.clear();
        scanDirectory(back);
    } else {
        scanDirectory(currentPath);
    }
}

void editor::ProjectWindow::collectSearchResults() {
    // The query over the asset index (S25 R21): never a walk of the disk. Entry names are paths
    // relative to currentPath, so every currentPath / name use (open, drag, delete, rename) works.
    AssetSearchIndex& index = project->getAssetSearchIndex();
    searchIndexVersion = index.version();
    const AssetQuery query = AssetQuery::parse(searchText);
    searchError = query.error();
    const std::string folder = project->normalizeToProjectRelative(currentPath).generic_string();
    const std::vector<AssetQuery::Match> matches = query.evaluate(index, folder == "." ? std::string() : folder);
    constexpr size_t MAX_SEARCH_RESULTS = 2000;
    searchMoreResults = matches.size() > MAX_SEARCH_RESULTS ? matches.size() - MAX_SEARCH_RESULTS : 0;
    for (size_t i = 0; i < matches.size() && i < MAX_SEARCH_RESULTS; i++) {
        const AssetRecord& record = index.at(matches[i].index);
        const fs::path absolute = project->getProjectPath() / record.path;
        FileEntry entry = makeFileEntry(absolute, absolute.lexically_relative(currentPath).generic_string(), record.folder);
        entry.labels = record.labels;
        files.push_back(std::move(entry));
    }
    // the score order is the listing's (no folders-first sort while searching)
    requestSort = false;
}

void editor::ProjectWindow::refreshSearchIfStale() {
    if (searchText.empty()) return;
    if (project->getAssetSearchIndex().version() != searchIndexVersion) scanDirectory(currentPath);
}

void editor::ProjectWindow::refreshEntryLabels() {
    const uint64_t version = project->getAssetLabels().version();
    if (version == entryLabelsVersion) return;
    entryLabelsVersion = version;
    const AssetDatabase& database = project->getAssetDatabase();
    for (FileEntry& entry : files) {
        const std::string relative = project->normalizeToProjectRelative(currentPath / entry.name).generic_string();
        const Guid guid = database.getAssetGuid(relative);
        entry.labels = guid.isNull() ? std::vector<std::string>() : project->getAssetLabels().labelsOf(guid);
    }
}

void editor::ProjectWindow::renderSearchTools() {
    // Search by Type, Search by Label (checkbox lists over the t: and l: tokens), Save Search
    if (ImGui::Button(ICON_FA_SHAPES "##SearchByType")) ImGui::OpenPopup("##SearchByTypePopup");
    ImGui::SetItemTooltip("Search by Type");
    if (ImGui::BeginPopup("##SearchByTypePopup")) {
        for (const TypeChoice& choice : typeChoices) {
            bool checked = hasToken("t", choice.token);
            if (ImGui::Checkbox(choice.label, &checked)) toggleToken("t", choice.token);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, Theme::dpi(2.0f));
    if (ImGui::Button(ICON_FA_TAG "##SearchByLabel")) ImGui::OpenPopup("##SearchByLabelPopup");
    ImGui::SetItemTooltip("Search by Label");
    if (ImGui::BeginPopup("##SearchByLabelPopup")) {
        const std::vector<AssetLabels::LabelInfo> labels = project->getAssetLabels().labels();
        if (labels.empty()) ImGui::TextDisabled("No labels yet: right-click an asset > Labels");
        for (const AssetLabels::LabelInfo& label : labels) {
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(labelColor(label.color)));
            bool checked = hasToken("l", label.name);
            if (ImGui::Checkbox(label.name.c_str(), &checked)) toggleToken("l", label.name);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, Theme::dpi(2.0f));
    const bool canSave = !searchText.empty() && searchError.empty() && project->getOrganisation().isOpen();
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button(ICON_FA_STAR "##SaveSearch")) {
        saveSearchRequested = true;
        std::snprintf(searchNameBuffer, sizeof(searchNameBuffer), "%s", searchText.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(canSave ? "Save Search: keep this search under Favorites"
                                  : "Save Search: type a search first (a saved project is needed)");
}

void editor::ProjectWindow::renderSearchScopeBar() {
    if (searchText.empty()) return;
    const std::string folderName = (searchAll ? searchReturnPath : currentPath).filename().string();
    const std::string inFolder = "In '" + (folderName.empty() ? std::string("Project") : folderName) + "'";
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(6.0f), Theme::dpi(1.0f)));
    ImGui::TextDisabled("Search:");
    ImGui::SameLine();
    if (ImGui::RadioButton("All", searchAll)) setSearchScope(true);
    ImGui::SetItemTooltip("Search the whole project");
    ImGui::SameLine();
    if (ImGui::RadioButton(inFolder.c_str(), !searchAll)) setSearchScope(false);
    ImGui::SetItemTooltip("Search this folder and its subfolders");
    if (!searchError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", searchError.c_str());
    }
    ImGui::PopStyleVar();
}

// ---- labels

std::vector<std::pair<Guid, std::string>> editor::ProjectWindow::selectedAssets() const {
    std::vector<std::pair<Guid, std::string>> out;
    const AssetDatabase& database = project->getAssetDatabase();
    for (const std::string& name : selectedFiles) {
        const std::string relative = project->normalizeToProjectRelative(currentPath / name).generic_string();
        const Guid guid = database.getAssetGuid(relative);
        if (!guid.isNull()) out.push_back({guid, relative});
    }
    return out;
}

void editor::ProjectWindow::setLabelOnSelection(const std::string& label, bool on) {
    AssetLabels& store = project->getAssetLabels();
    SetAssetLabelsCmd::Changes before, after;
    for (const auto& [guid, path] : selectedAssets()) {
        std::vector<std::string> current = store.labelsOf(guid);
        std::vector<std::string> next;
        for (const std::string& existing : current) {
            if (!AssetLabels::sameLabel(existing, label)) next.push_back(existing);
        }
        if (on) next.push_back(label);
        if (next.size() > AssetLabels::MAX_LABELS) {
            Out::warning("Labels: %s already has 32 labels.", path.c_str());
            return;
        }
        before.push_back({guid, current});
        after.push_back({guid, next});
    }
    if (after.empty()) return;
    auto* command = new SetAssetLabelsCmd(project, "Set Asset Labels", before, after);
    command->setNoMerge();
    if (!project->getProjectCommandHistory()->addCommand(command)) Out::warning("Labels: they could not be written (see the Console).");
}

void editor::ProjectWindow::renderLabelsMenu() {
    if (!ImGui::BeginMenu(ICON_FA_TAG " Labels", !selectedAssets().empty())) {
        if (selectedAssets().empty()) ImGui::SetItemTooltip("Labels belong to assets with an identity (not folders)");
        return;
    }
    renderLabelItems();
    ImGui::EndMenu();
}

void editor::ProjectWindow::renderLabelItems() {
    // tri-state over the selection (checked: every selected asset has it; partly: some)
    const std::vector<std::pair<Guid, std::string>> assets = selectedAssets();
    AssetLabels& store = project->getAssetLabels();
    for (const AssetLabels::LabelInfo& label : store.labels()) {
        size_t having = 0;
        for (const auto& [guid, path] : assets) {
            const std::vector<std::string> labels = store.labelsOf(guid);
            if (std::any_of(labels.begin(), labels.end(), [&](const std::string& l) { return AssetLabels::sameLabel(l, label.name); })) having++;
        }
        const bool all = !assets.empty() && having == assets.size();
        const bool some = having > 0 && !all;
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(labelColor(label.color)));
        if (some) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        bool checked = all;
        if (ImGui::Checkbox(label.name.c_str(), &checked)) setLabelOnSelection(label.name, some ? true : checked);
        if (some) ImGui::PopItemFlag();
        ImGui::PopStyleColor();
    }
    if (!store.labels().empty()) ImGui::Separator();
    if (ImGui::MenuItem("New Label...")) {
        newLabelRequested = true;
        newLabelBuffer[0] = '\0';
    }
}

void editor::ProjectWindow::drawItemLabels(const FileEntry& entry, ImVec2 min, ImVec2 max, bool listView) {
    if (entry.labels.empty()) return;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const AssetLabels& store = project->getAssetLabels();
    if (listView) {
        // chips at the right end of the row
        float x = max.x - Theme::dpi(4.0f);
        const float height = std::max(1.0f, max.y - min.y - Theme::dpi(4.0f));
        for (auto it = entry.labels.rbegin(); it != entry.labels.rend(); ++it) {
            const float width = ImGui::CalcTextSize(it->c_str()).x * 0.8f + Theme::dpi(8.0f);
            if (x - width < min.x + (max.x - min.x) * 0.5f) break;
            const ImVec2 chipMin(x - width, min.y + Theme::dpi(2.0f));
            drawList->AddRectFilled(chipMin, ImVec2(x, chipMin.y + height), labelColor(store.colorOf(*it), 0.85f), height * 0.5f);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, ImVec2(chipMin.x + Theme::dpi(4.0f), chipMin.y + Theme::dpi(1.0f)),
                              IM_COL32(20, 20, 20, 255), it->c_str());
            x -= width + Theme::dpi(3.0f);
        }
        return;
    }
    // dots along the tile's top edge: up to 3, then +N
    const float radius = Theme::dpi(3.5f);
    float x = min.x + Theme::dpi(4.0f) + radius;
    const float y = min.y + Theme::dpi(4.0f) + radius;
    for (size_t i = 0; i < entry.labels.size() && i < 3; i++) {
        drawList->AddCircleFilled(ImVec2(x, y), radius, labelColor(store.colorOf(entry.labels[i])));
        drawList->AddCircle(ImVec2(x, y), radius, IM_COL32(0, 0, 0, 120));
        x += radius * 2.0f + Theme::dpi(2.0f);
    }
    if (entry.labels.size() > 3) {
        const std::string more = "+" + std::to_string(entry.labels.size() - 3);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, ImVec2(x - radius, y - radius * 1.4f),
                          ImGui::GetColorU32(ImGuiCol_Text), more.c_str());
    }
}

// ---- type-ahead and Ctrl+wheel

void editor::ProjectWindow::handleTypeAhead() {
    // A-Z, 0-9 with no modifier and no text field active: the first item starting with the
    // typed prefix (750 ms window); the same key again inside it cycles through its matches
    const ImGuiIO& io = ImGui::GetIO();
    if (!windowFocused || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift || isRenaming) return;
    char typed = 0;
    for (int k = ImGuiKey_0; k <= ImGuiKey_Z; k++) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) continue;   // shortcut-lint: allow type-ahead in the focused listing: bare letters and digits, never a binding of the window's commands
        typed = k <= ImGuiKey_9 ? static_cast<char>('0' + (k - ImGuiKey_0)) : static_cast<char>('a' + (k - ImGuiKey_A));
        break;
    }
    if (!typed) return;
    const double now = ImGui::GetTime();
    const bool fresh = now - typeAheadTime > 0.75;
    typeAheadTime = now;
    const bool cycling = !fresh && typeAheadPrefix.size() == 1 && typeAheadPrefix[0] == typed;
    if (fresh) typeAheadPrefix.clear();
    if (!cycling) typeAheadPrefix.push_back(typed);

    std::vector<size_t> matchesIndex;
    for (size_t i = 0; i < files.size(); i++) {
        const std::string name = lower(fs::path(files[i].name).filename().string());
        if (name.compare(0, typeAheadPrefix.size(), typeAheadPrefix) == 0) matchesIndex.push_back(i);
    }
    if (matchesIndex.empty()) return;
    size_t pick = matchesIndex.front();
    if (cycling) {
        // the match after the selected one
        for (size_t i = 0; i < matchesIndex.size(); i++) {
            if (files[matchesIndex[i]].name == lastSelectedFile) {
                pick = matchesIndex[(i + 1) % matchesIndex.size()];
                break;
            }
        }
    }
    selectedFiles.clear();
    selectedFiles.insert(files[pick].name);
    lastSelectedFile = files[pick].name;
    scrollToFile = files[pick].name;
}

void editor::ProjectWindow::handleWheelResize() {
    // Ctrl/Cmd+wheel over the listing: the icon size in 16 px steps, saved once the wheel rests
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && io.MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const int step = io.MouseWheel > 0.0f ? 16 : -16;
        iconSize = std::clamp(((iconSize + step) / 16) * 16, MIN_GRID_ICON_SIZE, THUMBNAIL_SIZE);
        iconSizeChangedAt = ImGui::GetTime();
    }
    if (iconSizeChangedAt >= 0.0 && ImGui::GetTime() - iconSizeChangedAt > 0.5) {
        iconSizeChangedAt = -1.0;
        AppSettings::setResourcesIconSize(iconSize);
        AppSettings::saveSettings();
    }
}

// ---- Favorites: saved searches, bookmarked folders and assets

void editor::ProjectWindow::runSavedSearch(const SavedSearch& search) {
    fs::path folder = project->getProjectPath();
    if (!search.folder.empty()) folder /= search.folder;
    searchAll = false;
    searchReturnPath.clear();
    currentPath = folder;
    setSearch(search.query);
    activeSavedSearch = search.name;
}

void editor::ProjectWindow::commitSearches(std::vector<SavedSearch> user, std::vector<SavedSearch> shared, const char* name) {
    auto* command = new SetSavedSearchesCmd(project, name, std::move(user), std::move(shared));
    command->setNoMerge();
    if (!project->getProjectCommandHistory()->addCommand(command)) Out::warning("Saved searches: they could not be written (see the Console).");
}

void editor::ProjectWindow::renderFavorites() {
    Organisation& organisation = project->getOrganisation();
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const bool open = ImGui::TreeNodeEx("##Favorites", ImGuiTreeNodeFlags_SpanFullWidth, ICON_FA_STAR "  Favorites");
    ImGui::SetItemTooltip("Saved searches and bookmarks (Window > General > Bookmarks)");
    if (!open) return;

    auto searchRow = [&](const SavedSearch& search, int index) {
        ImGui::PushID(index);
        const bool active = activeSavedSearch == search.name && searchText == search.query;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
        if (active) flags |= ImGuiTreeNodeFlags_Selected;
        const char* icon = search.project ? ICON_FA_USERS : ICON_FA_MAGNIFYING_GLASS;
        ImGui::TreeNodeEx("##search", flags, "%s  %s", icon, search.name.c_str());
        if (ImGui::IsItemClicked()) runSavedSearch(search);
        ImGui::SetItemTooltip("%s%s%s", search.query.c_str(), search.project ? "\nShared with the project (project.yaml)" : "",
                              search.builtin ? "\nBuilt in" : "");
        if (!search.builtin && ImGui::BeginPopupContextItem("##SavedSearchMenu")) {
            if (ImGui::MenuItem("Rename")) {
                editSearchName = search.name;
                std::snprintf(searchNameBuffer, sizeof(searchNameBuffer), "%s", search.name.c_str());
                renameSearchRequested = true;
            }
            if (ImGui::MenuItem("Edit...")) openSearchEditor(search);
            if (ImGui::MenuItem("Delete")) {
                std::vector<SavedSearch> user = organisation.userSearches();
                std::vector<SavedSearch> shared = organisation.projectSearches();
                auto drop = [&](std::vector<SavedSearch>& list) {
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedSearch& s) { return s.name == search.name; }), list.end());
                };
                drop(user);
                drop(shared);
                commitSearches(user, shared, "Delete Saved Search");
            }
            if (!search.project && ImGui::MenuItem("Share with Project")) {
                std::vector<SavedSearch> user = organisation.userSearches();
                std::vector<SavedSearch> shared = organisation.projectSearches();
                user.erase(std::remove_if(user.begin(), user.end(), [&](const SavedSearch& s) { return s.name == search.name; }), user.end());
                SavedSearch moved = search;
                moved.project = true;
                shared.push_back(moved);
                commitSearches(user, shared, "Share Saved Search");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    };

    int index = 0;
    for (const SavedSearch& search : Organisation::builtinSearches()) searchRow(search, index++);
    for (const SavedSearch& search : organisation.savedSearches()) searchRow(search, index++);

    // bookmarked folders and assets
    const BookmarkLookups lookups = Bookmarks::projectLookups(project);
    for (const Bookmark& bookmark : organisation.bookmarks()) {
        if (bookmark.kind == Bookmark::Kind::GameObject) continue;
        const BookmarkTarget target = Bookmarks::resolve(bookmark, lookups);
        ImGui::PushID(index++);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
        if (!target.found) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TreeNodeEx("##bookmark", flags, "%s  %s%s", bookmark.kind == Bookmark::Kind::Folder ? ICON_FA_FOLDER : ICON_FA_BOOKMARK,
                          bookmark.name.c_str(), target.found ? "" : " (Missing)");
        if (!target.found) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", (target.path.empty() ? bookmark.path : target.path).c_str());
        if (ImGui::IsItemClicked() && target.found) {
            const fs::path absolute = project->getProjectPath() / target.path;
            if (bookmark.kind == Bookmark::Kind::Folder) {
                searchText.clear();
                searchBuffer[0] = '\0';
                scanDirectory(absolute);
            } else {
                searchText.clear();
                searchBuffer[0] = '\0';
                revealAsset(absolute);
                pingAsset(target.path);
            }
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void editor::ProjectWindow::openSearchEditor(const SavedSearch& search) {
    editSearchName = search.name;
    editSearchProject = search.project;
    std::snprintf(searchNameBuffer, sizeof(searchNameBuffer), "%s", search.name.c_str());
    std::snprintf(searchQueryBuffer, sizeof(searchQueryBuffer), "%s", search.query.c_str());
    std::snprintf(searchFolderBuffer, sizeof(searchFolderBuffer), "%s", search.folder.c_str());
    editSearchRaw = !SavedSearchRows::fromQuery(search.query, editSearchRows);
    editSearchRequested = true;
}

void editor::ProjectWindow::renderSearchPopups() {
    Organisation& organisation = project->getOrganisation();
    const char* savePopup = "Save Search##ProjectSaveSearch";
    const char* renamePopup = "Rename Saved Search##ProjectRenameSearch";
    const char* editPopup = "Edit Saved Search##ProjectEditSearch";
    const char* labelPopup = "New Label##ProjectNewLabel";
    if (saveSearchRequested) {
        saveSearchRequested = false;
        ImGui::OpenPopup(savePopup);
    }
    if (renameSearchRequested) {
        renameSearchRequested = false;
        ImGui::OpenPopup(renamePopup);
    }
    if (editSearchRequested) {
        editSearchRequested = false;
        ImGui::OpenPopup(editPopup);
    }
    if (newLabelRequested) {
        newLabelRequested = false;
        ImGui::OpenPopup(labelPopup);
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    const float buttonWidth = ImGui::GetFontSize() * 6.0f;

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin(savePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Save the search \"%s\" as:", searchText.c_str());
        EditorModal::message("Save the search \"" + searchText + "\" as:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        const bool enter = EditorModal::inputText("##SearchName", "name", searchNameBuffer, sizeof(searchNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        EditorModal::checkbox("Share with Project", "share_with_project", &saveSearchShared);
        ImGui::SetItemTooltip("Kept in project.yaml for everyone, instead of only for you");
        const bool valid = searchNameBuffer[0] != '\0';
        ImGui::BeginDisabled(!valid);
        const bool save = EditorModal::button("Save", "ok", ImVec2(buttonWidth, 0.0f)) || (enter && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (save) {
            std::vector<SavedSearch> user = organisation.userSearches();
            std::vector<SavedSearch> shared = organisation.projectSearches();
            const std::string name = searchNameBuffer;
            auto drop = [&](std::vector<SavedSearch>& list) {
                list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedSearch& s) { return s.name == name; }), list.end());
            };
            drop(user);
            drop(shared);
            const std::string folder = searchAll ? std::string() : project->normalizeToProjectRelative(currentPath).generic_string();
            SavedSearch search{name, searchText, folder == "." ? std::string() : folder, saveSearchShared, false};
            (saveSearchShared ? shared : user).push_back(search);
            commitSearches(user, shared, "Save Search");
            activeSavedSearch = name;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin(renamePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        EditorModal::message("Rename the saved search \"" + editSearchName + "\".");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        const bool enter = EditorModal::inputText("##SearchRename", "name", searchNameBuffer, sizeof(searchNameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = searchNameBuffer[0] != '\0';
        ImGui::BeginDisabled(!valid);
        const bool rename = EditorModal::button("Rename", "ok", ImVec2(buttonWidth, 0.0f)) || (enter && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (rename) {
            std::vector<SavedSearch> user = organisation.userSearches();
            std::vector<SavedSearch> shared = organisation.projectSearches();
            for (auto* list : {&user, &shared}) {
                for (SavedSearch& search : *list) {
                    if (search.name == editSearchName) search.name = searchNameBuffer;
                }
            }
            commitSearches(user, shared, "Rename Saved Search");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin(editPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        EditorModal::message("Edit the saved search \"" + editSearchName + "\".");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        EditorModal::inputText("Name", "name", searchNameBuffer, sizeof(searchNameBuffer));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        EditorModal::inputTextWithHint("Folder", "folder", "the whole project", searchFolderBuffer, sizeof(searchFolderBuffer));
        ImGui::Separator();
        if (editSearchRaw) {
            ImGui::TextDisabled("This search uses terms the rows cannot show: edit its text.");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
            EditorModal::inputText("Query", "query", searchQueryBuffer, sizeof(searchQueryBuffer));
        } else {
            int mode = editSearchRows.any ? 1 : 0;
            EditorModal::enumField("match", mode, {"All of", "Any of"});
            ImGui::RadioButton("All of", &mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Any of", &mode, 1);
            editSearchRows.any = mode == 1;
            int removeRow = -1;
            for (size_t i = 0; i < editSearchRows.rows.size(); i++) {
                SavedSearchRows::Row& row = editSearchRows.rows[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
                if (ImGui::BeginCombo("##field", SavedSearchRows::fieldName(row.field))) {
                    for (int f = 0; f <= static_cast<int>(SavedSearchRows::Field::Path); f++) {
                        const auto field = static_cast<SavedSearchRows::Field>(f);
                        if (ImGui::Selectable(SavedSearchRows::fieldName(field), row.field == field)) row.field = field;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                char value[256];
                std::snprintf(value, sizeof(value), "%s", row.value.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
                if (ImGui::InputText("##value", value, sizeof(value))) row.value = value;
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK)) removeRow = static_cast<int>(i);
                ImGui::SetItemTooltip("Remove this row");
                ImGui::PopID();
            }
            if (removeRow >= 0) editSearchRows.rows.erase(editSearchRows.rows.begin() + removeRow);
            if (ImGui::SmallButton(ICON_FA_PLUS " Add Row")) editSearchRows.rows.push_back({SavedSearchRows::Field::Name, ""});
            ImGui::TextDisabled("%s", editSearchRows.toQuery().c_str());
            // the rows edit a query an agent writes whole
            std::string query = editSearchRows.toQuery();
            if (EditorModal::field("query", query)) {
                std::snprintf(searchQueryBuffer, sizeof(searchQueryBuffer), "%s", query.c_str());
                editSearchRaw = true;
            }
        }
        ImGui::Separator();
        const bool valid = searchNameBuffer[0] != '\0';
        ImGui::BeginDisabled(!valid);
        const bool save = EditorModal::button("Save", "ok", ImVec2(buttonWidth, 0.0f));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (save) {
            std::vector<SavedSearch> user = organisation.userSearches();
            std::vector<SavedSearch> shared = organisation.projectSearches();
            const std::string query = editSearchRaw ? std::string(searchQueryBuffer) : editSearchRows.toQuery();
            for (auto* list : {&user, &shared}) {
                for (SavedSearch& search : *list) {
                    if (search.name != editSearchName) continue;
                    search.name = searchNameBuffer;
                    search.query = query;
                    search.folder = searchFolderBuffer;
                }
            }
            commitSearches(user, shared, "Edit Saved Search");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (EditorModal::begin(labelPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("A label for the selected assets:");
        EditorModal::message("A label for the selected assets:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        const bool enter = EditorModal::inputText("##NewLabel", "label", newLabelBuffer, sizeof(newLabelBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string label, why;
        const bool valid = AssetLabels::normalizeLabel(newLabelBuffer, label, &why);
        if (!valid && newLabelBuffer[0]) ImGui::TextDisabled("%s", why.c_str());
        EditorModal::error(valid ? std::string() : newLabelBuffer[0] ? why : std::string("The label is empty."));
        ImGui::BeginDisabled(!valid);
        const bool add = EditorModal::button("Add", "ok", ImVec2(buttonWidth, 0.0f)) || (enter && valid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        if (add) {
            setLabelOnSelection(project->getAssetLabels().canonical(label), true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void editor::ProjectWindow::addSelectionToBookmarks() {
    BookmarksWindow* bookmarks = Backend::getApp().getBookmarksWindow();
    for (const std::string& name : selectedFiles) {
        const std::string relative = project->normalizeToProjectRelative(currentPath / name).generic_string();
        std::string error;
        if (!bookmarks->add(Bookmarks::forPath(project, relative), &error)) Out::warning("Bookmarks: %s", error.c_str());
    }
}

// The Asset Labels strip of the selection (Unity shows it under the Inspector's asset preview;
// this editor's Inspector shows GameObjects only, so it sits in the Project window's bottom bar):
// the labels of the selected assets as chips, and the Labels menu behind its tag button.
// Returns the width it took, drawn from `right` leftwards.
float editor::ProjectWindow::renderLabelStrip(ImVec2 barMin, float barHeight, float right) {
    const std::vector<std::pair<Guid, std::string>> assets = selectedAssets();
    if (assets.empty()) return 0.0f;
    const AssetLabels& store = project->getAssetLabels();
    std::vector<std::string> shown = store.labelsOf(assets.front().first);
    for (size_t i = 1; i < assets.size(); i++) {
        // what every selected asset has
        const std::vector<std::string> labels = store.labelsOf(assets[i].first);
        shown.erase(std::remove_if(shown.begin(), shown.end(), [&](const std::string& label) {
            return std::none_of(labels.begin(), labels.end(), [&](const std::string& l) { return AssetLabels::sameLabel(l, label); });
        }), shown.end());
    }
    const float buttonSize = ImGui::GetFrameHeight();
    float x = right - buttonSize;
    ImGui::SetCursorScreenPos(ImVec2(x, barMin.y + (barHeight - buttonSize) * 0.5f));
    if (ImGui::Button(ICON_FA_TAG "##AssetLabelsStrip", ImVec2(buttonSize, buttonSize))) ImGui::OpenPopup("##AssetLabelsStripMenu");
    ImGui::SetItemTooltip("Asset Labels of the selection");
    if (ImGui::BeginPopup("##AssetLabelsStripMenu")) {
        renderLabelItems();
        ImGui::EndPopup();
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float chipHeight = ImGui::GetTextLineHeight();
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        const float width = ImGui::CalcTextSize(it->c_str()).x * 0.85f + Theme::dpi(8.0f);
        if (x - width - Theme::dpi(3.0f) < barMin.x + Theme::dpi(160.0f)) break;
        x -= width + Theme::dpi(3.0f);
        const ImVec2 chipMin(x, barMin.y + (barHeight - chipHeight) * 0.5f);
        drawList->AddRectFilled(chipMin, ImVec2(x + width, chipMin.y + chipHeight), labelColor(store.colorOf(*it), 0.85f), chipHeight * 0.5f);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, ImVec2(x + Theme::dpi(4.0f), chipMin.y + Theme::dpi(1.0f)),
                          IM_COL32(20, 20, 20, 255), it->c_str());
    }
    return right - x;
}
