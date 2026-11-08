// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The palette controller's rules (scope chips, a snapshot taken on open, a versioned cancel of
// the scoring) follow OpenEngine's Apps/Editor/Source/Search/UniversalSearchController.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "SearchPalette.h"
#include "window/InspectorWindow.h"
#include "window/HierarchyWindow.h"
#include "window/ProjectWindow.h"

#include "audio/BuiltinSoundImport.h"
#include "audio/SoundEssentials.h"
#include "audio/SoundLibrary.h"
#include "util/ProjectFileWrites.h"

#include "App.h"
#include "AppMenuCommand.h"
#include "Catalog.h"
#include "Out.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "search/SearchScorer.h"
#include "settings/SettingsRegistry.h"
#include "settings/SettingsSearch.h"
#include "shortcuts/CommandCatalog.h"
#include "util/EntityPath.h"
#include "window/dialog/ComponentAddDialog.h"
#include "window/BookmarksWindow.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // The scope chips: All, then one per kind, as their prefixes
    struct Chip {
        const char* label;
        const char* prefix;
        const char* tooltip;
    };
    const Chip chips[] = {
        {"All", "", "Everything"},
        {"Commands", ">", "Commands of the menus and the shortcut catalog (>)"},
        {"Windows", "/", "Open and focus a window (/)"},
        {"Settings", ":", "Preferences and Project Settings rows (:)"},
        {"GameObjects", "@", "GameObjects of the open scenes (@)"},
        {"Components", "+", "Add a component to the selection (+)"},
        {"Assets", "asset:", "Assets and folders, with t: l: ext: path: (asset:)"},
    };
    constexpr int CHIP_COUNT = static_cast<int>(sizeof(chips) / sizeof(chips[0]));

    const char* kindIcon(SearchKind kind) {
        switch (kind) {
            case SearchKind::Command: return ICON_FA_TERMINAL;
            case SearchKind::Panel: return ICON_FA_WINDOW_MAXIMIZE;
            case SearchKind::Setting: return ICON_FA_GEAR;
            case SearchKind::Component: return ICON_FA_PUZZLE_PIECE;
            case SearchKind::GameObject: return ICON_FA_CUBE;
            case SearchKind::Asset: return ICON_FA_FILE;
            case SearchKind::Sound: return ICON_FA_VOLUME_HIGH;
        }
        return ICON_FA_TERMINAL;
    }

    // The dockable windows: their name and the catalog command that opens and focuses them. Kept
    // by hand: test_palette_lists_every_window compares it with the catalog's window.* commands
    struct PanelEntry {
        const char* name;
        const char* command;
    };
    const PanelEntry panels[] = {
        {"Scene", "window.scene"}, {"Game", "window.game"}, {"Inspector", "window.inspector"}, {"Hierarchy", "window.hierarchy"},
        {"Project", "window.project"}, {"Console", "window.console"}, {"Animation", "window.animation"},
        {"Pose Animation", "window.poseAnimation"}, {"Animator", "window.animator"}, {"Vertex Animation Baker", "window.vertexAnimationBaker"},
        {"Audio Mixer", "window.audioMixer"}, {"Particle Effect", "window.particleEffect"}, {"Sound Browser", "window.soundBrowser"}, {"Audio Captures", "window.audioCaptures"},
        {"Color Picker", "window.colorPicker"},
        {"Terrain", "window.terrain"}, {"Input Actions", "window.inputActions"}, {"Navigation", "window.navigation"},
        {"Texture Synthesis", "window.textureSynthesis"}, {"Sprite Normal Map", "window.spriteNormals"}, {"Physics Placement", "window.physicsPlacement"},
        {"MCP Server", "window.mcpActivity"}, {"Bookmarks", "window.bookmarks"},
        {"Undo History", "window.undoHistory"}, {"Play Mode Changes", "window.playModeChanges"},
        {"Missing Assets", "window.missingAssets"}, {"Graph Editor", "window.graphEditor"}, {"References", "window.assetReferences"}, {"Profiler", "window.profiler"},
        {"Frame Debugger", "window.frameDebugger"}, {"Systems", "window.systems"}, {"Jobs & Memory", "window.jobsMemory"},
        {"Mesh Optimization Audit", "window.meshAudit"}, {"Build Report", "window.buildReport"}, {"Animation Statistics", "window.animationStatistics"},
        {"Sprite Preview", "window.spritePreview"},
    };

    // A label drawn with the matched ranges highlighted
    void highlightedText(ImDrawList* drawList, ImVec2 pos, const std::string& text, const std::string& query, ImU32 color,
                         ImU32 highlight) {
        const auto ranges = search::highlightRanges(query, text);
        for (const auto& [begin, end] : ranges) {
            const float x0 = ImGui::CalcTextSize(text.c_str(), text.c_str() + begin).x;
            const float x1 = ImGui::CalcTextSize(text.c_str(), text.c_str() + end).x;
            drawList->AddRectFilled(ImVec2(pos.x + x0, pos.y), ImVec2(pos.x + x1, pos.y + ImGui::GetFontSize()), highlight, 2.0f);
        }
        drawList->AddText(pos, color, text.c_str());
    }

    std::string sceneKey(Project* project, const SceneProject& scene) {
        if (!scene.filepath.empty()) {
            const Guid guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(scene.filepath));
            if (!guid.isNull()) return guid.str();
        }
        return "scene" + std::to_string(scene.id);
    }

}

SearchPalette::SearchPalette(App* app) : app(app) {
}

SearchPalette::~SearchPalette() {
    latest->fetch_add(1);
    if (scoringPending) scoring.wait();
}

void SearchPalette::open(const std::string& query) {
    openRequested = true;
    focusQuery = true;
    std::snprintf(queryBuffer, sizeof(queryBuffer), "%s", query.c_str());
    scope = 0;
    selected = 0;
    lastQuery.clear();
    results.clear();
    snapshot = buildSnapshot();
    startQuery();
}

void SearchPalette::close() {
    openRequested = false;
    visible = false;
    latest->fetch_add(1);
    // the snapshot goes with the palette: nothing is held while it is closed (R32)
    if (scoringPending) {
        scoring.wait();
        scoringPending = false;
    }
    snapshot.reset();
    results.clear();
}

std::shared_ptr<SearchSnapshot> SearchPalette::buildSnapshot(const std::vector<SearchKind>& kinds) {
    auto out = std::make_shared<SearchSnapshot>();
    auto want = [&kinds](SearchKind kind) { return kinds.empty() || std::find(kinds.begin(), kinds.end(), kind) != kinds.end(); };
    Project* project = app->getProject();
    CommandCatalog& catalog = CommandCatalog::editor();

    if (want(SearchKind::Command)) {
        std::vector<std::string> seenCatalog;
        std::function<void(const std::vector<PlatformMenuItem>&, const std::string&)> walk =
            [&](const std::vector<PlatformMenuItem>& items, const std::string& path) {
                for (const PlatformMenuItem& item : items) {
                    if (item.type == PlatformMenuItemType::Separator) continue;
                    if (item.type == PlatformMenuItemType::Submenu) {
                        walk(item.children, path.empty() ? item.label : path + " > " + item.label);
                        continue;
                    }
                    SearchEntry entry;
                    entry.kind = SearchKind::Command;
                    entry.label = item.label;
                    entry.detail = path;
                    entry.shortcut = item.shortcut;
                    entry.enabled = item.enabled;
                    entry.payload = "menu:" + std::to_string(item.command.id) + ":" + item.command.payload;
                    const CommandDef* def = catalog.findByMenuCommand(item.command.id, item.command.payload);
                    if (def) {
                        entry.key = "command:" + def->id + (def->menuPayload == item.command.payload ? std::string() : ":" + item.command.payload);
                        entry.searchText = def->group + " " + def->name + " " + def->description + " " + path;
                        seenCatalog.push_back(def->id);
                    } else {
                        entry.key = "command:" + std::to_string(item.command.id) + "+" + item.command.payload;
                        entry.searchText = path;
                    }
                    out->entries.push_back(std::move(entry));
                }
            };
        walk(app->currentMenuModel().menus, "");
        // catalog commands without a menu item (tools, Scene view keys, the Project window's)
        for (const CommandDef& def : catalog.commands()) {
            if (def.readOnly || !def.run) continue;
            if (std::find(seenCatalog.begin(), seenCatalog.end(), def.id) != seenCatalog.end()) continue;
            if (def.menuCommand && !def.dynamic) continue;   // a menu item not shown now (a context it lacks)
            SearchEntry entry;
            entry.kind = SearchKind::Command;
            entry.key = "command:" + def.id;
            entry.label = def.name;
            entry.detail = def.group;
            entry.searchText = def.group + " " + def.description;
            entry.shortcut = catalog.menuShortcutText(def.id);
            entry.enabled = !def.enabled || def.enabled();
            entry.payload = "catalog:" + def.id;
            out->entries.push_back(std::move(entry));
        }
    }

    if (want(SearchKind::Panel)) {
        for (const PanelEntry& panel : panels) {
            const CommandDef* def = catalog.find(panel.command);
            if (!def) continue;
            SearchEntry entry;
            entry.kind = SearchKind::Panel;
            entry.key = std::string("panel:") + panel.name;
            entry.label = panel.name;
            entry.detail = catalog.menuPath(panel.command);
            entry.searchText = "window panel tab";
            entry.shortcut = catalog.menuShortcutText(panel.command);
            entry.enabled = !def->enabled || def->enabled();
            entry.payload = std::string("catalog:") + panel.command;
            out->entries.push_back(std::move(entry));
        }
    }

    if (want(SearchKind::Setting)) {
        const SettingsRegistry& registry = SettingsRegistry::get();
        for (const SettingRow& row : registry.rows()) {
            SearchEntry entry;
            entry.kind = SearchKind::Setting;
            entry.key = "setting:" + row.key;
            entry.label = row.label;
            entry.detail = registry.location(row);
            entry.searchText = row.keywords + " " + row.key + " " + row.section;
            entry.payload = row.key;
            out->entries.push_back(std::move(entry));
        }
    }

    if (want(SearchKind::GameObject)) {
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (!scene || !scene->scene) continue;
            const std::string key = sceneKey(project, *scene);
            EntityPath::Cache cache;
            for (Entity entity : scene->entities) {
                if (!scene->scene->isEntityCreated(entity)) continue;
                out->entries.push_back(SearchEntries::gameObject(scene->scene->getEntityName(entity), EntityPath::of(*scene, entity, &cache),
                                                                 scene->name, key, scene->id, entity));
            }
        }
    }

    if (want(SearchKind::Component)) {
        InspectorWindow* inspector = app->getInspectorWindow();
        const auto& categories = ComponentAddDialog::listing();
        for (const ComponentCategory& category : categories) {
            for (const ComponentEntry& component : category.entries) {
                SearchEntry entry;
                entry.kind = SearchKind::Component;
                entry.label = Catalog::getComponentDisplayName(component.type);
                entry.detail = std::string("Add Component > ") + category.name;
                entry.searchText = std::string(category.name) + " " + component.description;
                entry.key = "component:" + Catalog::getComponentName(component.type);
                entry.enabled = inspector->canAddComponentToSelection(component.type);
                entry.payload = std::to_string(static_cast<int>(component.type));
                out->entries.push_back(std::move(entry));
            }
        }
    }

    if (want(SearchKind::Asset)) {
        AssetSearchIndex& index = project->getAssetSearchIndex();
        std::shared_ptr<const std::vector<AssetRecord>> records = index.shared();
        out->assets = std::make_shared<AssetRecordsView>(records, index.withMissingReferences());
        for (size_t i = 0; i < records->size(); i++) out->entries.push_back(SearchEntries::asset((*records)[i], i));
    }

    if (want(SearchKind::Sound)) {
        // the project's clips by name and labels, and the built-in clips, which Enter imports
        if (const std::shared_ptr<const AssetIndexView>& assets = out->assets) {
            for (size_t i = 0; i < assets->size(); i++) {
                const AssetRecord& record = assets->at(i);
                if (record.type != "audio") continue;
                SearchEntry entry;
                entry.kind = SearchKind::Sound;
                entry.key = "sound:" + record.path;
                entry.label = record.name;
                entry.detail = record.path;
                for (const std::string& label : record.labels) entry.searchText += label + " ";
                entry.payload = record.path;
                out->entries.push_back(std::move(entry));
            }
        }
        auto addBuiltin = [&out](const SoundManifestClip& clip, const std::string& reference, const char* set) {
            SearchEntry entry;
            entry.kind = SearchKind::Sound;
            entry.key = "sound:" + reference;
            entry.label = clip.id;
            entry.detail = clip.type + " \xC2\xB7 " + set + " \xC2\xB7 " + clip.licence;
            entry.searchText = clip.type + " " + clip.description;
            for (const std::string& tag : clip.tags) entry.searchText += " " + tag;
            entry.payload = reference;
            out->entries.push_back(std::move(entry));
        };
        for (const SoundManifestClip& clip : SoundEssentials::all()) addBuiltin(clip, SoundEssentials::PREFIX + clip.id, "Essentials");
        for (const SoundManifestClip& clip : SoundLibrary::get().all()) addBuiltin(clip, SoundLibrary::PREFIX + clip.id, "Starter Library");
    }

    out->recents = project->getOrganisation().recents();
    out->prepare();
    return out;
}

bool SearchPalette::run(const SearchEntry& entry, bool alternate, CommandSource source, std::string* error) {
    Project* project = app->getProject();
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!entry.enabled) return fail(entry.label + " is disabled now.");
    switch (entry.kind) {
        case SearchKind::Command:
        case SearchKind::Panel: {
            if (entry.payload.rfind("catalog:", 0) == 0) {
                const CommandDef* def = CommandCatalog::editor().find(entry.payload.substr(8));
                if (!def || !def->run) return fail("No command " + entry.payload.substr(8) + ".");
                const auto run = def->run;
                run(CommandArgs{source, def->menuPayload});
            } else {
                // menu:<id>:<payload>
                const size_t colon = entry.payload.find(':', 5);
                const uint32_t id = static_cast<uint32_t>(std::strtoul(entry.payload.c_str() + 5, nullptr, 10));
                app->runMenuCommand({id, colon == std::string::npos ? std::string() : entry.payload.substr(colon + 1)});
            }
            break;
        }
        case SearchKind::Setting: {
            const SettingRow* row = SettingsRegistry::get().find(entry.payload);
            if (!row) return fail("No setting " + entry.payload + ".");
            // the window at the row's page, scrolled to it, the row flashing
            settingsui::requestReveal(row->key);
            if (row->scope == SettingsScope::Project) {
                app->getProjectSettingsWindow().open(project, row->page.c_str());
            } else {
                app->getEditorSettingsWindow().open(project);
                const SettingsPage* page = SettingsRegistry::get().findPage(row->page);
                app->getEditorSettingsWindow().showPage(page ? page->displayName() : row->page);
            }
            break;
        }
        case SearchKind::GameObject: {
            const size_t colon = entry.payload.find(':');
            const uint32_t sceneId = static_cast<uint32_t>(std::strtoul(entry.payload.c_str(), nullptr, 10));
            const Entity entity = static_cast<Entity>(std::strtoul(entry.payload.c_str() + colon + 1, nullptr, 10));
            SceneProject* scene = project->getScene(sceneId);
            if (!scene || !scene->scene || !scene->scene->isEntityCreated(entity)) return fail("The GameObject is gone.");
            project->setSelectedSceneId(sceneId);
            project->replaceSelectedEntities(sceneId, {entity});
            app->getHierarchyWindow()->revealEntity(sceneId, entity);
            if (alternate) app->getSceneWindow()->frameSelection(sceneId);
            break;
        }
        case SearchKind::Component: {
            const auto type = static_cast<ComponentType>(std::strtol(entry.payload.c_str(), nullptr, 10));
            std::string why;
            if (!app->getInspectorWindow()->addComponentToSelection(type, &why)) return fail(why);
            break;
        }
        case SearchKind::Sound: {
            if (BuiltinSoundImport::isReference(entry.payload)) {
                std::vector<BuiltinSoundImport::Clip> clips;
                std::string why;
                if (!BuiltinSoundImport::resolve({entry.payload}, clips, why)) return fail(why);
                const fs::path folder = project->getAssetsPath() / "sounds" / (clips[0].library ? "library" : "essentials");
                BuiltinSoundImport::Plan plan;
                if (!BuiltinSoundImport::plan(clips, folder, BuiltinSoundImport::Existing::RefuseDifferent, plan, why)) return fail(why);
                std::vector<WriteFilesCmd::Item> items;
                for (BuiltinSoundImport::File& file : plan.writes) items.push_back({file.path, std::move(file.content)});
                if (!items.empty() && !ProjectFileWrites::writeUndoable(project, std::move(items), why)) return fail(why);
                const fs::path written = folder / fs::path(clips[0].entry->id + ".ogg");
                app->getProjectWindow()->revealAsset(written);
                app->getProjectWindow()->pingAsset(project->normalizeToProjectRelative(written).generic_string());
            } else {
                const fs::path absolute = project->getProjectPath() / entry.payload;
                std::error_code ec;
                if (!fs::exists(absolute, ec)) return fail(entry.payload + " is gone.");
                app->getProjectWindow()->revealAsset(absolute);
                app->getProjectWindow()->pingAsset(entry.payload);
                project->setInspectedAsset(absolute.lexically_normal());
            }
            break;
        }
        case SearchKind::Asset: {
            const fs::path absolute = project->getProjectPath() / entry.payload;
            std::error_code ec;
            if (!fs::exists(absolute, ec)) return fail(entry.payload + " is gone.");
            app->getProjectWindow()->revealAsset(absolute);
            app->getProjectWindow()->pingAsset(entry.payload);
            if (alternate) app->getProjectWindow()->assetsOpenSelected();
            break;
        }
    }
    project->getOrganisation().noteRecent(entry.key);
    return true;
}

bool SearchPalette::runKey(const std::string& key, bool alternate, std::string* error) {
    const std::string kindName = key.substr(0, key.find(':'));
    std::vector<SearchKind> kinds;
    if (kindName == "panel") kinds = {SearchKind::Panel};
    else if (kindName == "setting") kinds = {SearchKind::Setting};
    else if (kindName == "gameobject") kinds = {SearchKind::GameObject};
    else if (kindName == "component") kinds = {SearchKind::Component};
    else if (kindName == "asset") kinds = {SearchKind::Asset};
    else if (kindName == "sound") kinds = {SearchKind::Sound, SearchKind::Asset};
    else if (kindName == "command") kinds = {SearchKind::Command};
    else {
        if (error) *error = "A key starts with command:, panel:, setting:, gameobject:, component:, asset: or sound:.";
        return false;
    }
    std::shared_ptr<SearchSnapshot> fresh = buildSnapshot(kinds);
    for (const SearchEntry& entry : fresh->entries) {
        if (entry.key == key) return run(entry, alternate, CommandSource::Agent, error);
    }
    if (error) *error = "Nothing has the key " + key + " now (search_editor lists the keys).";
    return false;
}

std::string SearchPalette::effectiveQuery() const {
    std::string query = queryBuffer;
    if (scope > 0) {
        // the chip's prefix replaces one typed
        const ParsedSearchQuery parsed = parseSearchQuery(query);
        query = std::string(chips[scope].prefix) + (parsed.kind ? parsed.text : query);
    }
    return query;
}

void SearchPalette::startQuery() {
    if (!snapshot) return;
    const std::string query = effectiveQuery();
    lastQuery = query;
    version = latest->fetch_add(1) + 1;
    const uint64_t mine = version;
    std::shared_ptr<SearchSnapshot> held = snapshot;
    std::shared_ptr<std::atomic<uint64_t>> counter = latest;
    const ParsedSearchQuery parsed = parseSearchQuery(query);
    if (parsed.kind == SearchKind::Asset && parsed.text.find("ref:") != std::string::npos) {
        // ref: asks the live dependency index, which is the main thread's
        SearchSnapshot live = *held;
        AssetSearchIndex& index = app->getProject()->getAssetSearchIndex();
        live.assets = std::shared_ptr<const AssetIndexView>(&index, [](const AssetIndexView*) {});
        results = SearchEngine::query(live, query);
        selected = 0;
        scoringPending = false;
        return;
    }
    if (scoringPending) {
        // still running: its result is stale (the counter moved) and is dropped when it ends
        scoringPending = false;
    }
    scoring = jobs::submit([held, counter, query, mine]() {
        return SearchEngine::query(*held, query, counter.get(), mine);
    }, {jobs::Lane::High, jobs::GateClass::Default, "search-palette"});
    scoringPending = true;
    scoringVersion = mine;
}

void SearchPalette::collectResults() {
    if (!scoringPending || !scoring.isReady()) return;
    scoringPending = false;
    if (scoringVersion != latest->load()) return;
    try {
        results = scoring.get();
    } catch (const std::exception&) {
        results.clear();
    }
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(results.size()) - 1));
}

void SearchPalette::runSelected(bool alternate) {
    if (!snapshot || selected < 0 || selected >= static_cast<int>(results.size())) return;
    const SearchEntry entry = snapshot->entries[results[selected].entry];
    if (!entry.enabled) return;
    close();
    std::string error;
    if (!run(entry, alternate, CommandSource::Palette, &error)) Out::warning("Search: %s", error.c_str());
}

void SearchPalette::show() {
    const char* popupId = "Search##SearchPalette";
    if (openRequested) {
        openRequested = false;
        visible = true;
        ImGui::OpenPopup(popupId);
    }
    if (!visible) return;
    collectResults();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(Theme::dpi(640.0f), viewport->WorkSize.x * 0.9f);
    const float height = std::min(Theme::dpi(440.0f), viewport->WorkSize.y * 0.8f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.2f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    if (!ImGui::BeginPopup(popupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        // Esc or a click outside closed it
        close();
        return;
    }

    // keys first: they act on the results the user saw
    bool runNow = false;
    bool alternate = false;
    const int count = static_cast<int>(results.size());
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        close();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { selected = std::min(selected + 1, count - 1); scrollToSelected = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { selected = std::max(selected - 1, 0); scrollToSelected = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) { selected = std::min(selected + 10, count - 1); scrollToSelected = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) { selected = std::max(selected - 10, 0); scrollToSelected = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        scope = (scope + (ImGui::GetIO().KeyShift ? CHIP_COUNT - 1 : 1)) % CHIP_COUNT;
        focusQuery = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        runNow = true;
        alternate = ImGui::GetIO().KeyCtrl;
    }

    // the query
    if (focusQuery) {
        ImGui::SetKeyboardFocusHere();
        focusQuery = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##SearchQuery", "Search everything...  > commands  / windows  : settings  @ GameObjects  + components  asset:",
                             queryBuffer, sizeof(queryBuffer), ImGuiInputTextFlags_AutoSelectAll);
    // Tab moves the chip, not the focus
    if (ImGui::IsItemDeactivated() && !runNow) focusQuery = true;

    // the chips
    for (int i = 0; i < CHIP_COUNT; i++) {
        if (i > 0) ImGui::SameLine(0.0f, Theme::dpi(4.0f));
        const bool active = scope == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(chips[i].label)) {
            scope = i;
            focusQuery = true;
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", chips[i].tooltip);
    }
    if (effectiveQuery() != lastQuery) {
        selected = 0;
        startQuery();
    }
    ImGui::Separator();

    // the results
    const ParsedSearchQuery parsed = parseSearchQuery(effectiveQuery());
    ImGui::BeginChild("##SearchResults", ImVec2(0, 0), ImGuiChildFlags_None);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rowHeight = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 highlight = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.2f, 0.35f));
    int clicked = -1;
    if (snapshot) {
        ImGuiListClipper clipper;
        clipper.Begin(count, rowHeight);
        if (scrollToSelected && selected >= 0) clipper.IncludeItemByIndex(selected);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const SearchResult& result = results[i];
                const SearchEntry& entry = snapshot->entries[result.entry];
                ImGui::PushID(i);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", i == selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight))) {
                    selected = i;
                    clicked = i;
                }
                if (!entry.enabled && ImGui::IsItemHovered()) ImGui::SetTooltip("Disabled now: its menu item is greyed out too");
                if (i == selected && scrollToSelected) {
                    ImGui::SetScrollHereY();
                    scrollToSelected = false;
                }
                const float padding = ImGui::GetStyle().FramePadding.x;
                const ImU32 color = entry.enabled ? textColor : dimColor;
                drawList->AddText(ImVec2(pos.x + padding, pos.y + ImGui::GetTextLineHeight() * 0.5f), dimColor, kindIcon(entry.kind));
                const float textX = pos.x + padding + ImGui::GetFontSize() * 1.6f;
                highlightedText(drawList, ImVec2(textX, pos.y), entry.label, parsed.text, color, highlight);
                std::string detail = std::string(searchKindTitle(entry.kind)) + (entry.detail.empty() ? "" : "  \xC2\xB7  " + entry.detail);
                if (result.recent) detail += "  \xC2\xB7  Recent";
                drawList->AddText(ImVec2(textX, pos.y + ImGui::GetTextLineHeight()), dimColor, detail.c_str());
                if (!entry.shortcut.empty()) {
                    const float shortcutWidth = ImGui::CalcTextSize(entry.shortcut.c_str()).x;
                    drawList->AddText(ImVec2(pos.x + ImGui::GetContentRegionAvail().x - shortcutWidth - padding, pos.y),
                                      dimColor, entry.shortcut.c_str());
                }
                ImGui::PopID();
            }
        }
        if (results.empty()) {
            ImGui::TextDisabled(scoringPending ? "Searching..." : "Nothing matches.");
        }
    }
    ImGui::EndChild();
    if (clicked >= 0) {
        // a click runs the row, as Enter does (Ctrl: the alternate action)
        runNow = true;
        alternate = ImGui::GetIO().KeyCtrl;
    }

    if (runNow) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        runSelected(alternate);
        return;
    }
    ImGui::EndPopup();
}

SearchPalette::UiState SearchPalette::uiState() const {
    UiState state;
    state.open = isOpen();
    state.query = queryBuffer;
    state.selected = selected;
    if (snapshot) {
        for (const SearchResult& result : results) {
            if (result.entry >= snapshot->entries.size()) continue;
            const SearchEntry& entry = snapshot->entries[result.entry];
            state.results.push_back(std::string(searchKindName(entry.kind)) + ": " + entry.label);
        }
    }
    return state;
}
