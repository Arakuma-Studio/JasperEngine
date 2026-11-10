// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S25: the editor's search and organisation (group "editor_ux", S31 §6.4)
//
//   search_editor          the Search palette's entries (commands, windows, settings, GameObjects, components, assets)
//   run_command            a catalog command or palette entry, as its menu item or the palette runs it
//   list_shortcuts         the command catalog with bindings, defaults, conflicts
//   set_shortcut           rebind (or reset) a command; conflicts refused unless force
//   edit_layout_presets    window layouts: list, apply, save, rename, delete, export, import, reset
//   edit_asset_labels      asset labels and their colours (undoable)
//   edit_saved_searches    saved searches of the Project window (undoable)
//   edit_bookmarks         the Bookmarks window (undoable)
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "App.h"
#include "Project.h"
#include "organisation/AssetLabels.h"
#include "organisation/Bookmarks.h"
#include "organisation/LabelOperations.h"
#include "organisation/OrganisationCommands.h"
#include "search/AssetQuery.h"
#include "search/SearchPalette.h"
#include "shortcuts/CommandCatalog.h"
#include "shortcuts/ShortcutsWindow.h"
#include "window/BookmarksWindow.h"
#include "window/ProjectWindow.h"

#include <algorithm>

namespace jasperengine::editor::mcp {

using namespace schema;

namespace {

namespace fs = std::filesystem;

const char* GROUP = "editor_ux";

Json stringList(const Json& value) {
    Json out = Json::array();
    if (value.is_array()) {
        for (const Json& item : value) {
            if (item.is_string()) out.push_back(item);
        }
    } else if (value.is_string()) {
        out.push_back(value);
    }
    return out;
}

// ------------------------------------------------------------------ search_editor

void searchEditorTool(const CallPtr& call) {
    const Json& args = call->args();
    App& app = call->ctx().app();
    std::vector<SearchKind> kinds;
    for (const Json& scope : stringList(args.value("scopes", Json::array()))) {
        const std::optional<SearchKind> kind = parseSearchKind(scope.get<std::string>());
        if (!kind) {
            call->fail("Unknown scope " + scope.get<std::string>() + ": command, panel, setting, gameobject, component or asset.");
            return;
        }
        kinds.push_back(*kind);
    }
    const std::string query = args.value("query", "");
    const size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 20), 1, 100));
    std::shared_ptr<SearchSnapshot> snapshot = app.getSearchPalette()->buildSnapshot(kinds);
    // ref: asks the live dependency index (as the palette does)
    if (query.find("ref:") != std::string::npos) {
        AssetSearchIndex& index = app.getProject()->getAssetSearchIndex();
        snapshot->assets = std::shared_ptr<const AssetIndexView>(&index, [](const AssetIndexView*) {});
    }
    const std::vector<SearchResult> results = SearchEngine::query(*snapshot, query);
    Json list = Json::array();
    std::string text;
    for (const SearchResult& result : results) {
        if (list.size() >= limit) break;
        const SearchEntry& entry = snapshot->entries[result.entry];
        Json item = {{"kind", searchKindName(entry.kind)}, {"key", entry.key}, {"label", entry.label},
                     {"detail", entry.detail}, {"enabled", entry.enabled}, {"score", result.score}};
        if (!entry.shortcut.empty()) item["shortcut"] = entry.shortcut;
        if (entry.kind == SearchKind::Command && entry.key.rfind("command:", 0) == 0) {
            const std::string id = entry.key.substr(8, entry.key.find(':', 8) == std::string::npos ? std::string::npos : entry.key.find(':', 8) - 8);
            if (CommandCatalog::editor().find(id)) item["id"] = id;
        }
        text += "\n" + std::string(searchKindName(entry.kind)) + "  " + entry.label + (entry.detail.empty() ? "" : "  (" + entry.detail + ")") +
                (entry.shortcut.empty() ? "" : "  [" + entry.shortcut + "]") + (entry.enabled ? "" : "  disabled");
        list.push_back(item);
    }
    call->ok(std::to_string(list.size()) + " result(s)" + (results.size() > list.size() ? " of " + std::to_string(results.size()) : "") +
             ":" + text, Json{{"results", list}, {"total", results.size()}});
}

// ------------------------------------------------------------------ run_command

void runCommandTool(const CallPtr& call) {
    const Json& args = call->args();
    App& app = call->ctx().app();
    const std::string id = args.value("id", "");
    const std::string key = args.value("key", "");
    const bool alternate = args.value("alternate", false);
    if (id.empty() == key.empty()) {
        call->fail("Give id (a catalog command: list_shortcuts, search_editor's id) or key (a search_editor result's key).");
        return;
    }
    CommandCatalog& catalog = CommandCatalog::editor();
    if (!id.empty()) {
        const CommandDef* def = catalog.find(id);
        if (!def) {
            call->fail("No command " + id + " (list_shortcuts lists them; search_editor finds them by name).");
            return;
        }
        if (!def->agentSafe) {
            call->fail(def->group + " / " + def->name + " is not run for agents (it quits, opens or creates a project, or deletes "
                       "for good). Use the project tools, or ask the user.");
            return;
        }
        if (def->readOnly || !def->run) {
            call->fail(def->group + " / " + def->name + " is a reference row (" + def->gesture + "): nothing to run.");
            return;
        }
        if (def->enabled && !def->enabled()) {
            call->fail(def->group + " / " + def->name + " is disabled now (its menu item is greyed out): " +
                       "check the selection, the focused window and Play mode.");
            return;
        }
        const auto run = def->run;
        run(CommandArgs{CommandSource::Agent, args.value("payload", def->menuPayload)});
        app.getProject()->getOrganisation().noteRecent("command:" + id);
        call->afterFrames(1, [call, id]() { call->ok("Ran " + id + ".", Json{{"id", id}}); });
        return;
    }
    // a palette entry; a command entry still respects agentSafe
    if (key.rfind("command:", 0) == 0) {
        const std::string commandId = key.substr(8, key.find(':', 8) == std::string::npos ? std::string::npos : key.find(':', 8) - 8);
        if (const CommandDef* def = catalog.find(commandId); def && !def->agentSafe) {
            call->fail(def->group + " / " + def->name + " is not run for agents.");
            return;
        }
        if (key.find('+') != std::string::npos && key.find("command:6+") == 0) {
            call->fail("Opening a recent project is not run for agents: use open_project.");
            return;
        }
    }
    std::string error;
    if (!app.getSearchPalette()->runKey(key, alternate, &error)) {
        call->fail(error);
        return;
    }
    call->afterFrames(1, [call, key]() { call->ok("Ran " + key + ".", Json{{"key", key}}); });
}

// ------------------------------------------------------------------ list_shortcuts / set_shortcut

Json chordsJson(const std::vector<Chord>& chords, bool mac) {
    Json list = Json::array();
    for (Chord chord : chords) list.push_back(chord.format(mac));
    return list;
}

Json shortcutJson(const CommandCatalog& catalog, const CommandDef& def) {
    const bool mac = catalog.isMac();
    const std::span<const Chord> bound = catalog.bindings(def.id);
    std::vector<Chord> chords(bound.begin(), bound.end());
    Json item = {{"id", def.id}, {"group", def.group}, {"name", def.name}, {"context", shortcutContextKey(def.context)},
                 {"chords", chordsJson(chords, mac)}, {"default_chords", chordsJson(catalog.defaults(def.id), mac)},
                 {"overridden", catalog.isOverridden(def.id)}, {"read_only", def.readOnly}};
    if (def.readOnly) item["gesture"] = def.gesture;
    if (!def.alsoIn.empty()) {
        Json also = Json::array();
        for (ShortcutContext context : def.alsoIn) also.push_back(shortcutContextKey(context));
        item["also_in"] = also;
    }
    const std::string path = catalog.menuPath(def.id);
    if (!path.empty()) {
        // what the main menu's item shows now (recorded each time the menu model changes)
        item["menu_path"] = path;
        item["menu_shortcut"] = catalog.menuChord(def.id).format(mac);
    }
    Json conflicts = Json::array();
    Json shadowed = Json::array();
    for (Chord chord : chords) {
        bool shadow = false;
        if (const CommandDef* other = catalog.findConflict(def.id, chord, &shadow)) {
            (shadow ? shadowed : conflicts).push_back(Json{{"chord", chord.format(mac)}, {"id", other->id}});
        }
    }
    if (!conflicts.empty()) item["conflicts"] = conflicts;
    if (!shadowed.empty()) item["shadowed_by"] = shadowed;
    if (!def.agentSafe) item["agent_safe"] = false;
    return item;
}

void listShortcutsTool(const CallPtr& call) {
    const Json& args = call->args();
    const CommandCatalog& catalog = CommandCatalog::editor();
    const std::string group = args.value("group", "");
    const std::string contextName = args.value("context", "");
    const bool conflictsOnly = args.value("conflicts_only", false);
    std::optional<ShortcutContext> context;
    if (!contextName.empty()) {
        context = parseShortcutContext(contextName);
        if (!context) {
            call->fail("context is global, editor, scene_view, hierarchy, project, console, animation, pose_animation, curve, color_picker, "
                       "voxel_brush or terrain_brush.");
            return;
        }
    }
    Json list = Json::array();
    std::string text;
    for (const CommandDef& def : catalog.commands()) {
        if (!group.empty() && def.group != group) continue;
        if (context) {
            const std::vector<ShortcutContext> contexts = commandContexts(def);
            if (std::find(contexts.begin(), contexts.end(), *context) == contexts.end()) continue;
        }
        Json item = shortcutJson(catalog, def);
        if (conflictsOnly && !item.contains("conflicts")) continue;
        const std::string keys = catalog.bindingsText(def.id);
        if (!keys.empty()) text += "\n" + def.id + "  " + keys;
        list.push_back(item);
    }
    call->ok(std::to_string(list.size()) + " command(s); with keys:" + text, Json{{"shortcuts", list}});
}

void setShortcutTool(const CallPtr& call) {
    const Json& args = call->args();
    CommandCatalog& catalog = CommandCatalog::editor();
    const std::string id = args.value("id", "");
    const CommandDef* def = catalog.find(id);
    if (!def) {
        call->fail("No command " + id + " (list_shortcuts lists them).");
        return;
    }
    std::string error;
    if (!args.contains("chords") || args["chords"].is_null()) {
        catalog.resetBindings(id);
        if (!ShortcutsWindow::save(catalog, &error)) {
            call->fail("Reset in memory, but shortcuts.yaml could not be written: " + error);
            return;
        }
        call->ok("Reset " + id + " to " + catalog.bindingsText(id) + ".", shortcutJson(catalog, *def));
        return;
    }
    std::vector<Chord> chords;
    for (const Json& value : args["chords"]) {
        const std::string text = value.is_string() ? value.get<std::string>() : std::string();
        // the names list_shortcuts printed on this platform (Control+Cmd+F on macOS)
        std::optional<Chord> chord = Chord::parse(text, catalog.isMac());
        if (!chord || !Chord::isBindableKey(chord->key())) {
            call->fail("\"" + text + "\" is not a chord: Ctrl, Shift, Alt then one key (\"Ctrl+Shift+S\", \"F2\", \"Numpad7\", \"Shift+[\").");
            return;
        }
        chords.push_back(*chord);
    }
    if (!args.value("force", false)) {
        for (Chord chord : chords) {
            bool shadowed = false;
            if (const CommandDef* other = catalog.findConflict(id, chord, &shadowed); other && !shadowed) {
                call->fail(chord.format(catalog.isMac()) + " is already used by " + other->group + " / " + other->name + " (" + other->id +
                           ") in the same context. force: true reassigns it (that command loses it).",
                           Json{{"conflict", other->id}});
                return;
            }
        }
    }
    if (!ShortcutsWindow::reassign(catalog, id, chords, &error)) {
        call->fail(error);
        return;
    }
    // the menus show the new text from the next frame on
    call->afterFrames(1, [call, id]() {
        const CommandCatalog& catalog = CommandCatalog::editor();
        const CommandDef* def = catalog.find(id);
        call->ok("Bound " + id + " to " + (catalog.bindingsText(id).empty() ? std::string("nothing") : catalog.bindingsText(id)) + ".",
                 def ? shortcutJson(catalog, *def) : Json());
    });
}

// ------------------------------------------------------------------ edit_layout_presets

Json presetsJson(LayoutManager& layouts) {
    Json list = Json::array();
    const bool dirty = layouts.isDirty();
    for (const LayoutPresetInfo& preset : layouts.presets()) {
        list.push_back(Json{{"name", preset.name}, {"builtin", preset.builtin}, {"active", preset.active}, {"dirty", preset.active && dirty}});
    }
    return list;
}

void editLayoutPresetsTool(const CallPtr& call) {
    const Json& args = call->args();
    App& app = call->ctx().app();
    LayoutManager& layouts = app.getLayouts();
    const std::string action = args.value("action", "list");
    const std::string name = args.value("name", "");
    std::vector<fs::path> written;
    std::string error;
    auto finish = [call, &layouts](const std::string& text, const std::vector<fs::path>& files) {
        Json filesJson = Json::array();
        for (const fs::path& file : files) filesJson.push_back(file.generic_string());
        // after the apply (between frames) and its post-load frame
        call->afterFrames(3, [call, &layouts, text, filesJson]() {
            Json data = {{"presets", presetsJson(layouts)}, {"active", layouts.activePreset()}, {"files_written", filesJson},
                         {"working_layout", layouts.workingLayoutFile().generic_string()}, {"dock_nodes", layouts.dockNodeCount()}};
            call->ok(text, data);
        });
    };
    if (action == "list") {
        finish(std::to_string(layouts.presets().size()) + " layout(s).", {});
        return;
    }
    if (action == "apply" || action == "reset") {
        const std::string target = action == "reset" ? std::string(LayoutPresets::DEFAULT_NAME) : name;
        if (target.empty()) {
            call->fail("apply needs name (list shows them).");
            return;
        }
        if (!layouts.apply(target, &error)) {
            call->fail(error);
            return;
        }
        finish("Applied layout '" + target + "'.", {});
        return;
    }
    if (action == "save") {
        if (!layouts.save(name, &error, &written)) {
            call->fail(error);
            return;
        }
        finish("Saved the window layout as '" + name + "'.", written);
        return;
    }
    if (action == "rename") {
        if (!layouts.rename(name, args.value("new_name", ""), &error, &written)) {
            call->fail(error);
            return;
        }
        finish("Renamed layout '" + name + "'.", written);
        return;
    }
    if (action == "delete") {
        if (!layouts.remove(name, &error)) {
            call->fail(error);
            return;
        }
        finish("Deleted layout '" + name + "' (the windows stay as they are).", {});
        return;
    }
    const std::string path = args.value("path", "");
    if (path.empty()) {
        call->fail(action + " needs path (an absolute file path, .layout.yaml).");
        return;
    }
    if (action == "export") {
        if (!layouts.exportTo(path, name, &error)) {
            call->fail(error);
            return;
        }
        finish(name.empty() ? "Exported every saved layout to " + path + "." : "Exported layout '" + name + "' to " + path + ".", {fs::path(path)});
        return;
    }
    if (action == "import") {
        std::vector<std::string> names;
        if (!layouts.importFrom(path, &names, &error, &written)) {
            call->fail(error);
            return;
        }
        std::string list;
        for (const std::string& imported : names) list += (list.empty() ? "" : ", ") + imported;
        finish("Imported: " + list + ".", written);
        return;
    }
    call->fail("action is list, apply, save, rename, delete, export, import or reset.");
}

// ------------------------------------------------------------------ edit_asset_labels

bool resolveAsset(Project* project, const std::string& ref, Guid& guid, std::string& path, std::string& error) {
    AssetDatabase& database = project->getAssetDatabase();
    if (Guid::isValidText(ref)) {
        guid = Guid::parse(ref);
        path = database.getPath(guid).generic_string();
    } else {
        path = fs::path(ref).lexically_normal().generic_string();
        guid = database.getAssetGuid(path);
    }
    if (guid.isNull() || path.empty()) {
        error = ref + " is not an asset with an identity (a project-relative path or a GUID of list_assets).";
        return false;
    }
    return true;
}

Json labelsJson(const std::vector<std::string>& labels) {
    Json list = Json::array();
    for (const std::string& label : labels) list.push_back(label);
    return list;
}

void editAssetLabelsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    AssetLabels& store = project->getAssetLabels();
    store.waitUntilReady();
    const std::string action = args.value("action", "list");

    if (action == "list") {
        Json list = Json::array();
        for (const AssetLabels::LabelInfo& info : store.labels()) {
            list.push_back(Json{{"name", info.name}, {"color", info.color}, {"assets", info.assets}, {"in_project_settings", info.hasEntry}});
        }
        call->ok(std::to_string(list.size()) + " label(s).", Json{{"labels", list}});
        return;
    }

    auto runCommand = [call, project](SetAssetLabelsCmd* command) {
        command->setNoMerge();
        const bool ok = project->getProjectCommandHistory()->addCommand(command);
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        return ok;
    };
    auto writtenJson = [](const SetAssetLabelsCmd& command) {
        Json files = Json::array();
        for (const std::string& file : command.filesWritten()) files.push_back(file);
        return files;
    };

    if (action == "rename_label" || action == "delete_label" || action == "set_color") {
        const std::string label = args.value("label", "");
        const LabelOperations::Result result =
            action == "rename_label" ? LabelOperations::rename(project, label, args.value("new_name", ""))
            : action == "delete_label" ? LabelOperations::remove(project, label)
            : LabelOperations::setColor(project, label, args.value("color", ""));
        if (!result.ok) {
            call->fail(result.error);
            return;
        }
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        Json files = Json::array();
        for (const std::string& file : result.filesWritten) files.push_back(file);
        call->ok(action + ": " + std::to_string(result.assets) + " asset(s). Undoable (undo scope project).",
                 Json{{"assets", result.assets}, {"files_written", files}});
        return;
    }

    // get, set, add, remove: per asset
    const Json assets = stringList(args.value("assets", Json::array()));
    if (assets.empty()) {
        call->fail(action + " needs assets: project-relative paths or GUIDs.");
        return;
    }
    std::vector<std::pair<Guid, std::string>> targets;
    for (const Json& ref : assets) {
        Guid guid;
        std::string path, error;
        if (!resolveAsset(project, ref.get<std::string>(), guid, path, error)) {
            call->fail(error);
            return;
        }
        targets.push_back({guid, path});
    }
    if (action == "get") {
        Json list = Json::array();
        for (const auto& [guid, path] : targets) list.push_back(Json{{"path", path}, {"guid", guid.str()}, {"labels", labelsJson(store.labelsOf(guid))}});
        call->ok(list.dump(), Json{{"assets", list}});
        return;
    }
    if (action != "set" && action != "add" && action != "remove") {
        call->fail("action is list, get, set, add, remove, rename_label, delete_label or set_color.");
        return;
    }
    std::vector<std::string> given;
    for (const Json& label : stringList(args.value("labels", Json::array()))) {
        std::string normalized, why;
        if (!AssetLabels::normalizeLabel(label.get<std::string>(), normalized, &why)) {
            call->fail("\"" + label.get<std::string>() + "\": " + why);
            return;
        }
        given.push_back(normalized);
    }
    if (given.empty() && action != "set") {
        call->fail(action + " needs labels.");
        return;
    }
    SetAssetLabelsCmd::Changes before, after;
    for (const auto& [guid, path] : targets) {
        std::vector<std::string> current = store.labelsOf(guid);
        std::vector<std::string> next;
        if (action == "set") {
            next = given;
        } else if (action == "add") {
            next = current;
            next.insert(next.end(), given.begin(), given.end());
        } else {
            for (const std::string& label : current) {
                if (std::none_of(given.begin(), given.end(), [&](const std::string& g) { return AssetLabels::sameLabel(g, label); })) next.push_back(label);
            }
        }
        if (next.size() > AssetLabels::MAX_LABELS) {
            call->fail(path + ": an asset has at most 32 labels.");
            return;
        }
        before.push_back({guid, current});
        after.push_back({guid, next});
    }
    auto* command = new SetAssetLabelsCmd(project, "Set Asset Labels", before, after);
    if (!runCommand(command)) {
        call->fail("The labels could not be written (see the Console).");
        return;
    }
    Json list = Json::array();
    for (const auto& [guid, path] : targets) list.push_back(Json{{"path", path}, {"labels", labelsJson(store.labelsOf(guid))}});
    const UndoEntry* top = project->getProjectCommandHistory()->top();
    const SetAssetLabelsCmd* applied = top ? dynamic_cast<const SetAssetLabelsCmd*>(top->cmd.get()) : nullptr;
    call->ok("Labels of " + std::to_string(targets.size()) + " asset(s) set. Undoable (undo scope project).",
             Json{{"assets", list}, {"files_written", applied ? writtenJson(*applied) : Json::array()}});
}

// ------------------------------------------------------------------ edit_saved_searches

Json searchJson(const SavedSearch& search) {
    Json item = {{"name", search.name}, {"query", search.query}, {"scope", search.builtin ? "builtin" : (search.project ? "project" : "user")}};
    if (!search.folder.empty()) item["folder"] = search.folder;
    return item;
}

void editSavedSearchesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    Organisation& organisation = project->getOrganisation();
    const std::string action = args.value("action", "list");
    const std::string name = args.value("name", "");
    if (action == "list") {
        Json list = Json::array();
        for (const SavedSearch& search : Organisation::builtinSearches()) list.push_back(searchJson(search));
        for (const SavedSearch& search : organisation.savedSearches()) list.push_back(searchJson(search));
        call->ok(std::to_string(list.size()) + " saved search(es).", Json{{"searches", list}});
        return;
    }
    if (action == "run") {
        const SavedSearch* search = organisation.findSearch(name);
        if (!search) {
            call->fail("No saved search " + name + " (list shows them).");
            return;
        }
        std::string error;
        Json data = searchAssetsJson(project, search->query, search->folder, static_cast<size_t>(std::clamp(args.value("limit", 200), 1, 2000)), error);
        if (!error.empty()) {
            call->fail(error);
            return;
        }
        data["search"] = searchJson(*search);
        call->ok(name + ": " + std::to_string(data["total"].get<size_t>()) + " match(es).", data);
        return;
    }
    if (!organisation.isOpen()) {
        call->fail("Saved searches belong to a saved project.");
        return;
    }
    std::vector<SavedSearch> user = organisation.userSearches();
    std::vector<SavedSearch> shared = organisation.projectSearches();
    auto erase = [&name](std::vector<SavedSearch>& list) {
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedSearch& s) { return s.name == name; }), list.end());
        return before != list.size();
    };
    const char* undoName = nullptr;
    if (action == "save") {
        if (name.empty() || args.value("query", "").empty()) {
            call->fail("save needs name and query (the Project window's grammar: t:texture l:Hero ...).");
            return;
        }
        if (std::any_of(Organisation::builtinSearches().begin(), Organisation::builtinSearches().end(), [&](const SavedSearch& s) { return s.name == name; })) {
            call->fail(name + " is a built-in search.");
            return;
        }
        const std::string scope = args.value("scope", "user");
        if (scope != "user" && scope != "project") {
            call->fail("scope is user (organisation.yaml) or project (project.yaml, shared).");
            return;
        }
        erase(user);
        erase(shared);
        SavedSearch search{name, args.value("query", ""), args.value("folder", ""), scope == "project", false};
        (scope == "project" ? shared : user).push_back(search);
        undoName = "Save Search";
    } else if (action == "delete") {
        if (!erase(user) && !erase(shared)) {
            call->fail("No saved search " + name + " of the user or the project (built-ins cannot be deleted).");
            return;
        }
        undoName = "Delete Saved Search";
    } else {
        call->fail("action is list, save, delete or run.");
        return;
    }
    auto* command = new SetSavedSearchesCmd(project, undoName, user, shared);
    command->setNoMerge();
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        call->fail("The saved searches could not be written (see the Console).");
        return;
    }
    const UndoEntry* top = project->getProjectCommandHistory()->top();
    const auto* applied = top ? dynamic_cast<const SetSavedSearchesCmd*>(top->cmd.get()) : nullptr;
    Json files = Json::array();
    if (applied) for (const std::string& file : applied->filesWritten()) files.push_back(file);
    Json list = Json::array();
    for (const SavedSearch& search : organisation.savedSearches()) list.push_back(searchJson(search));
    call->ok(std::string(undoName) + " " + name + ". Undoable (undo scope project).", Json{{"searches", list}, {"files_written", files}});
}

// ------------------------------------------------------------------ edit_bookmarks

Json bookmarksJson(Project* project) {
    Json list = Json::array();
    const BookmarkLookups lookups = Bookmarks::projectLookups(project);
    const std::vector<Bookmark>& bookmarks = project->getOrganisation().bookmarks();
    for (size_t i = 0; i < bookmarks.size(); i++) {
        const Bookmark& bookmark = bookmarks[i];
        const BookmarkTarget target = Bookmarks::resolve(bookmark, lookups);
        Json item = {{"index", i}, {"kind", bookmarkKindName(bookmark.kind)}, {"name", bookmark.name}, {"path", target.path.empty() ? bookmark.path : target.path}};
        if (!bookmark.guid.empty()) item[bookmark.kind == Bookmark::Kind::GameObject ? "scene_guid" : "guid"] = bookmark.guid;
        if (bookmark.kind == Bookmark::Kind::GameObject) {
            item["entity"] = target.found ? target.entity : bookmark.entity;
            item["entity_path"] = bookmark.entityPath;
            if (target.sceneClosed) item["scene_open"] = false;
        }
        item["missing"] = !target.found && !target.sceneClosed;
        list.push_back(item);
    }
    return list;
}

void editBookmarksTool(const CallPtr& call) {
    const Json& args = call->args();
    App& app = call->ctx().app();
    Project* project = app.getProject();
    BookmarksWindow* window = app.getBookmarksWindow();
    const std::string action = args.value("action", "list");
    std::string error;
    auto done = [call, project](const std::string& text, bool written) {
        Json data = {{"bookmarks", bookmarksJson(project)}};
        data["files_written"] = written ? Json::array({".jasperengine/user/organisation.yaml"}) : Json::array();
        call->ok(text, data);
    };
    if (action == "list" || action == "resolve") {
        done(std::to_string(project->getOrganisation().bookmarks().size()) + " bookmark(s).", false);
        return;
    }
    if (action == "add") {
        Bookmark bookmark;
        if (args.contains("entity")) {
            SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
            if (!scene) {
                call->fail(error);
                return;
            }
            const Entity entity = McpRefs::resolveEntity(scene, args["entity"], error);
            if (entity == NULL_ENTITY) {
                call->fail(error);
                return;
            }
            bookmark = Bookmarks::forGameObject(project, scene->id, entity);
        } else {
            const std::string ref = args.value("ref", "");
            std::string path = ref;
            if (Guid::isValidText(ref)) path = project->getAssetDatabase().getPath(Guid::parse(ref)).generic_string();
            fs::path absolute;
            if (path.empty() || !McpRefs::resolveProjectPath(project, path, absolute, error, true)) {
                call->fail(error.empty() ? "add needs ref (an asset/folder path or GUID) or entity (with scene)." : error);
                return;
            }
            bookmark = Bookmarks::forPath(project, project->normalizeToProjectRelative(absolute).generic_string());
        }
        if (!window->add(bookmark, &error)) {
            call->fail(error);
            return;
        }
        done("Bookmarked " + bookmark.name + ". Undoable (undo scope project).", true);
        return;
    }
    const std::vector<Bookmark>& list = project->getOrganisation().bookmarks();
    size_t index = static_cast<size_t>(std::max(0, args.value("index", -1)));
    if (!args.contains("index") && args.contains("ref")) {
        const std::string ref = args.value("ref", "");
        index = list.size();
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].path == ref || list[i].guid == ref || list[i].name == ref) index = i;
        }
    }
    if (action == "remove") {
        if (!window->removeAt(index, &error)) {
            call->fail(error + " (index, or ref: a bookmark's path, GUID or name)");
            return;
        }
        done("Removed the bookmark. Undoable (undo scope project).", true);
        return;
    }
    if (action == "move") {
        if (!window->move(index, static_cast<size_t>(std::max(0, args.value("to", 0))), &error)) {
            call->fail(error);
            return;
        }
        done("Moved the bookmark. Undoable (undo scope project).", true);
        return;
    }
    if (action == "navigate") {
        if (!window->navigate(index, &error)) {
            call->fail(error);
            return;
        }
        done("Went to the bookmark.", false);
        return;
    }
    call->fail("action is list, add, remove, move, navigate or resolve.");
}

} // namespace

// The list_assets shape of an R20 search over the asset index (list_assets search, saved searches)
Json searchAssetsJson(Project* project, const std::string& query, const std::string& folder, size_t limit, std::string& error) {
    project->getAssetLabels().waitUntilReady();
    AssetSearchIndex& index = project->getAssetSearchIndex();
    const AssetQuery parsed = AssetQuery::parse(query);
    if (!parsed.error().empty()) {
        error = parsed.error();
        return Json();
    }
    const std::string scope = folder == "." ? std::string() : fs::path(folder).lexically_normal().generic_string();
    const std::vector<AssetQuery::Match> matches = parsed.evaluate(index, scope);
    Json entries = Json::array();
    for (const AssetQuery::Match& match : matches) {
        if (entries.size() >= limit) break;
        const AssetRecord& record = index.at(match.index);
        Json item = {{"path", record.path}, {"type", record.type}};
        if (!record.guid.empty()) item["guid"] = record.guid;
        if (!record.labels.empty()) item["labels"] = labelsJson(record.labels);
        entries.push_back(item);
    }
    return Json{{"entries", entries}, {"total", matches.size()}, {"truncated", matches.size() > entries.size()}, {"query", parsed.toString()}};
}

void registerOrganisationTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "search_editor";
        tool.title = "Search Editor";
        tool.group = GROUP;
        tool.description =
            "The Search palette (Ctrl+K): commands, windows, settings, GameObjects, components, assets ranked by one fuzzy "
            "scorer. Prefixes > / : @ + asset: pick one kind (asset: takes t: l: ext: path: re: ref: missing:). Results "
            "{kind, key, id?, label, detail, shortcut, enabled, score}; run one with run_command key.";
        tool.inputSchema = object({
            {"query", string("")},
            {"scopes", array(stringEnum("", {"command", "panel", "setting", "gameobject", "component", "asset"}), "")},
            {"limit", withDefault(integer("", 1, 100), 20)}
        }, {"query"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = searchEditorTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "run_command";
        tool.title = "Run Command";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.attributesUndo = true;
        tool.description =
            "Runs a command as its menu item or the palette does: id (list_shortcuts, e.g. window.console, edit.duplicate) "
            "or key (search_editor). Refuses disabled commands and ones not for agents (exit, new/open project, empty trash).";
        tool.inputSchema = object({
            {"id", string("")},
            {"key", string("")},
            {"payload", string("A menu payload (a layout's name for window.layouts.apply).")},
            {"alternate", withDefault(boolean("Ctrl+Enter: frame a GameObject, open an asset."), false)}
        });
        tool.handler = runCommandTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_shortcuts";
        tool.title = "List Shortcuts";
        tool.group = GROUP;
        tool.description =
            "The command catalog: {id, group, name, context, chords, default_chords, overridden, read_only, menu_path, "
            "menu_shortcut, conflicts, shadowed_by}. Ctrl means Cmd on macOS.";
        tool.inputSchema = object({
            {"group", string("File, Edit, Tools, Scene View...")},
            {"context", stringEnum("", {"global", "editor", "scene_view", "hierarchy", "project", "console", "animation", "pose_animation", "curve",
                                        "color_picker", "sprite_preview", "voxel_brush", "terrain_brush"})},
            {"conflicts_only", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listShortcutsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_shortcut";
        tool.title = "Set Shortcut";
        tool.group = GROUP;
        tool.activity = ToolActivity::Control;
        tool.description =
            "Rebinds a command (Edit > Shortcuts...): chords like \"Ctrl+Shift+S\", \"F2\", \"Numpad7\"; [] unbinds; null "
            "resets to the default. A chord another command has in the same context fails unless force (it loses it). "
            "Saved to shortcuts.yaml (every project). Not undoable.";
        tool.inputSchema = object({
            {"id", string("")},
            {"chords", Json{{"type", Json::array({"array", "null"})}, {"items", Json{{"type", "string"}}}}},
            {"force", withDefault(boolean(""), false)}
        }, {"id"});
        tool.annotations.idempotent = true;
        tool.handler = setShortcutTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_layout_presets";
        tool.title = "Edit Layout Presets";
        tool.group = GROUP;
        tool.activity = ToolActivity::Control;
        tool.description =
            "Window layouts (Window > Layouts): list; apply/save/rename/delete name (Default is built in); export (name, or "
            "all) / import path; reset = Default. Presets are user-wide; the project keeps its working layout. Returns "
            "{presets:[{name, builtin, active, dirty}], files_written}. Not undoable.";
        tool.inputSchema = object({
            {"action", stringEnum("", {"list", "apply", "save", "rename", "delete", "export", "import", "reset"})},
            {"name", string("")},
            {"new_name", string("")},
            {"path", string("Absolute .layout.yaml path.")}
        }, {"action"});
        tool.handler = editLayoutPresetsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_asset_labels";
        tool.title = "Edit Asset Labels";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description =
            "Asset labels (Unity's): list; get/set/add/remove labels on assets (paths or GUIDs); rename_label, delete_label "
            "and set_color (\"#RRGGBB\") over the project. Stored in .meta files or in-file keys; undoable (scope project); "
            "files_written. Search them with list_assets search \"l:Name\".";
        tool.inputSchema = object({
            {"action", stringEnum("", {"list", "get", "set", "add", "remove", "rename_label", "delete_label", "set_color"})},
            {"assets", array(Json{{"type", "string"}}, "")},
            {"labels", array(Json{{"type", "string"}}, "")},
            {"label", string("")},
            {"new_name", string("")},
            {"color", string("")}
        }, {"action"});
        tool.handler = editAssetLabelsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_saved_searches";
        tool.title = "Edit Saved Searches";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description =
            "The Project window's saved searches (Favorites): list; save name + query (+ folder, scope user|project); "
            "delete; run name (list_assets' shape). Built-ins: All Materials, All Models, All Prefabs, All Scenes, All "
            "Scripts. Save/delete are undoable (scope project).";
        tool.inputSchema = object({
            {"action", stringEnum("", {"list", "save", "delete", "run"})},
            {"name", string("")},
            {"query", string("")},
            {"folder", string("")},
            {"scope", withDefault(stringEnum("", {"user", "project"}), "user")},
            {"limit", withDefault(integer("", 1, 2000), 200)}
        }, {"action"});
        tool.handler = editSavedSearchesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_bookmarks";
        tool.title = "Edit Bookmarks";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description =
            "The Bookmarks window: list/resolve; add ref (asset or folder path/GUID) or entity (+ scene); remove index (or "
            "ref); move index to; navigate index. Each row says where it resolves now or missing. Add/remove/move are "
            "undoable (scope project).";
        tool.inputSchema = object({
            {"action", stringEnum("", {"list", "add", "remove", "move", "navigate", "resolve"})},
            {"ref", string("")},
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"index", integer("", 0, 10000)},
            {"to", integer("", 0, 10000)}
        }, {"action"});
        tool.handler = editBookmarksTool;
        registry.addTool(std::move(tool));
    }
}

}
