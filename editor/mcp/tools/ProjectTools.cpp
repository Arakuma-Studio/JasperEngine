// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W5: project lifecycle and settings, scenes in build
//
//   get_project_summary    project overview (replaces the legacy one)
//   create_project         New Project at a path, waits for it to load
//   open_project           Open Project, waits for it to load
//   save_project           save scenes, scripts and project.yaml (Save As for a temporary project)
//   get_project_settings   Project Settings (+ editor preferences) as key/value pairs
//   set_project_settings   change them (replaces set_project_script_dirs / set_project_cxx_standard)
//   edit_build_scenes      Scenes in Build: list, add, remove, move, set start scene
//                          (replaces add_project_scene / set_start_scene)
//   edit_project_build_file  read / write ProjectBuild.cmake (replaces update_project_build_file)
#include "McpTools.h"
#include "log/Logger.h"
#include "DocumentFile.h"

#include "mcp/McpDataSafety.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"
#include "settings/SettingsRegistry.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorVersion.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "registry/TagManager.h"
#include "util/CxxStandards.h"
#include "window/CodeEditor.h"
#include "window/ConsoleWindow.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

const char* hostPlatformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string relText(Project* project, const fs::path& path) {
    if (path.empty()) return {};
    return project->normalizeToProjectRelative(path).generic_string();
}

// ------------------------------------------------------------------ scenes in build

// The start scene the build uses: the project's, or the first saved scene (Build Settings rule)
uint32_t effectiveStartScene(Project* project) {
    const SceneProject* start = project->getScene(project->getStartSceneId());
    if (start && !start->filepath.empty()) return start->id;
    for (const SceneProject& scene : project->getScenes()) {
        if (!scene.filepath.empty()) return scene.id;
    }
    return NULL_PROJECT_SCENE;
}

// Unity order: the start scene is index 0, then the other saved scenes in project order
std::vector<uint32_t> buildSceneOrder(Project* project) {
    std::vector<uint32_t> order;
    const uint32_t start = effectiveStartScene(project);
    if (start != NULL_PROJECT_SCENE) order.push_back(start);
    for (const SceneProject& scene : project->getScenes()) {
        if (!scene.filepath.empty() && scene.id != start) order.push_back(scene.id);
    }
    return order;
}

Json buildScenesJson(Project* project) {
    Json list = Json::array();
    const std::vector<uint32_t> order = buildSceneOrder(project);
    for (size_t i = 0; i < order.size(); i++) {
        const SceneProject* scene = project->getScene(order[i]);
        if (!scene) continue;
        Json item = McpRefs::sceneJson(project, *scene);
        item.erase("play_state");
        item["index"] = i;
        item["start"] = i == 0;
        list.push_back(item);
    }
    return list;
}

Json untitledScenesJson(Project* project) {
    Json list = Json::array();
    for (const SceneProject& scene : project->getScenes()) {
        if (scene.filepath.empty()) list.push_back({{"id", scene.id}, {"name", scene.name}});
    }
    return list;
}

// A build job reads the project from its own thread: project edits wait for it.
// The server's busy policy already refuses edits (and project switches) then; this
// re-checks for handlers that run later (deferred), through the same policy.
bool refuseDuringBuild(const CallPtr& call) {
    return call->server().refuseIfBusy(call, ToolActivity::Edit);
}

// ------------------------------------------------------------------ summary

Json projectSummary(McpContext& ctx) {
    Project* project = ctx.project();
    Json data;
    data["name"] = project->getName();
    data["path"] = project->getProjectPath().generic_string();
    data["temporary"] = project->isTempProject();
    data["unsaved"] = {
        {"scenes", Json::array()},
        {"code_editor", ctx.codeEditor() && ctx.codeEditor()->hasUnsavedChanges()},
        {"temporary_project", project->isTempUnsavedProject()}
    };
    for (const SceneProject& scene : project->getScenes()) {
        if (project->hasSceneUnsavedChanges(scene.id)) data["unsaved"]["scenes"].push_back(scene.id);
    }
    data["scenes_in_build"] = buildScenesJson(project);
    data["untitled_scenes"] = untitledScenesJson(project);
    const uint32_t start = effectiveStartScene(project);
    data["start_scene_id"] = start == NULL_PROJECT_SCENE ? Json(nullptr) : Json(start);
    data["selected_scene_id"] = project->getSelectedSceneId();
    data["assets_dir"] = project->getAssetsDir().generic_string();
    data["lua_dir"] = project->getLuaDir().generic_string();
    data["script_dirs"] = Json::array();
    for (const fs::path& dir : project->getScriptDirs()) data["script_dirs"].push_back(dir.generic_string());
    data["cxx_standard"] = project->getCxxStandard();
    data["standalone_prefabs"] = Json::array();
    for (const fs::path& prefab : project->getStandalonePrefabs()) data["standalone_prefabs"].push_back(prefab.generic_string());
    std::error_code ec;
    data["has_project_build_file"] = fs::exists(project->getProjectPath() / "ProjectBuild.cmake", ec);
    data["physics"] = {{"enabled_2d", project->isPhysics2DEnabled()}, {"enabled_3d", project->isPhysics3DEnabled()}};
    data["engine_version"] = JASPERENGINE_EDITOR_VERSION;
    data["platform"] = hostPlatformName();
    data["playing"] = ctx.isPlaying();
    return data;
}

std::string summaryText(const Json& data) {
    std::string text = "Project \"" + data["name"].get<std::string>() + "\" at " + data["path"].get<std::string>();
    if (data["temporary"].get<bool>()) text += " (in the system temp directory: the editor treats it as a New Project; save_project with path moves it elsewhere)";
    text += ". Scenes in build:";
    if (data["scenes_in_build"].empty()) text += " none (save a scene first)";
    for (const Json& scene : data["scenes_in_build"]) {
        text += " " + std::to_string(scene["index"].get<size_t>()) + "=" + scene["name"].get<std::string>() +
                (scene["start"].get<bool>() ? " (start)" : "");
    }
    text += ". C++" + std::to_string(data["cxx_standard"].get<int>()) + ", engine " + data["engine_version"].get<std::string>() +
            ", " + data["platform"].get<std::string>() + ".";
    if (!data["unsaved"]["scenes"].empty()) text += " Unsaved scenes: " + data["unsaved"]["scenes"].dump() + ".";
    return text;
}

void getProjectSummaryTool(const CallPtr& call) {
    Json data = projectSummary(call->ctx());
    call->ok(summaryText(data), data);
}

// ------------------------------------------------------------------ saving

// Where an untitled scene is saved: "<dir>/<name>.scene", made unique
fs::path untitledScenePath(Project* project, const SceneProject& scene, const fs::path& dir) {
    std::string base;
    for (char c : scene.name) {
        base += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') ? c : '_';
    }
    if (base.empty()) base = "Scene";
    fs::path candidate = dir / (base + ".scene");
    std::error_code ec;
    for (int i = 2; fs::exists(project->getProjectPath() / candidate, ec) && i < 1000; i++) {
        candidate = dir / (base + " " + std::to_string(i) + ".scene");
    }
    return candidate;
}

// Saves prefab stages, scenes with unsaved changes (untitled ones into untitledDir) and the
// code editor's buffers. Fills `saved` with what it saved.
// saveUntitled: also untitled scenes without changes (a new project's scene), so they reach the build.
bool saveEverything(McpContext& ctx, const fs::path& untitledDir, bool saveUntitled, Json& saved, std::string& error,
        DegradedSavePolicy policy = DegradedSavePolicy::Refuse, Json* refused = nullptr) {
    Project* project = ctx.project();
    saved = {{"scenes", Json::array()}, {"prefab_stages", Json::array()}, {"scripts", false}};

    if (CodeEditor* code = ctx.codeEditor(); code && code->hasUnsavedChanges()) {
        code->saveAll();
        if (code->hasUnsavedChanges()) {
            error = "Some scripts open in the code editor could not be saved (file permissions?).";
            return false;
        }
        saved["scripts"] = true;
    }

    for (size_t i = 0; i < project->getPrefabStageCount(); i++) {
        PrefabStage* stage = project->getPrefabStageAt(i);
        if (!stage || !project->hasUnsavedPrefabStage(stage->sceneProject.id)) continue;
        std::string message;
        SaveResult result = SaveResult::Failed;
        if (!project->saveSceneNow(stage->sceneProject.id, {}, &message, policy, &result)) {
            error = "Could not save the prefab open in Prefab Mode: " + message;
            if (result == SaveResult::RefusedDegraded && refused) {
                const Prefab* prefab = project->getPrefab(stage->assetPath);
                if (prefab) *refused = degradedDocumentError(stage->assetPath.generic_string(), prefab->loadReport);
            }
            return false;
        }
        saved["prefab_stages"].push_back(stage->sceneProject.id);
    }

    std::vector<uint32_t> ids;
    for (const SceneProject& scene : project->getScenes()) {
        if (scene.isModified || project->hasSceneUnsavedChanges(scene.id) || (saveUntitled && scene.filepath.empty())) {
            ids.push_back(scene.id);
        }
    }
    for (uint32_t id : ids) {
        SceneProject* scene = project->getScene(id);
        if (!scene || !scene->scene) continue;   // closed scenes have nothing in memory to save
        fs::path target;
        if (scene->filepath.empty()) target = untitledScenePath(project, *scene, untitledDir);
        std::string message;
        SaveResult result = SaveResult::Failed;
        if (!project->saveSceneNow(id, target, &message, policy, &result)) {
            error = "Could not save scene " + std::to_string(id) + " \"" + scene->name + "\": " + message;
            if (result == SaveResult::RefusedDegraded && refused) {
                SceneProject* after = project->getScene(id);
                if (after) *refused = degradedDocumentError(after->filepath.generic_string(), after->loadReport);
            }
            return false;
        }
        SceneProject* after = project->getScene(id);
        saved["scenes"].push_back({{"id", id}, {"path", after ? relText(project, after->filepath) : std::string()}});
    }
    return true;
}

// What would be lost by switching projects
Json unsavedState(McpContext& ctx) {
    Project* project = ctx.project();
    Json scenes = Json::array();
    for (const SceneProject& scene : project->getScenes()) {
        if (scene.isModified || project->hasSceneUnsavedChanges(scene.id)) {
            scenes.push_back({{"id", scene.id}, {"name", scene.name}, {"untitled", scene.filepath.empty()}});
        }
    }
    Json stages = Json::array();
    for (size_t i = 0; i < project->getPrefabStageCount(); i++) {
        PrefabStage* stage = project->getPrefabStageAt(i);
        if (stage && project->hasUnsavedPrefabStage(stage->sceneProject.id)) stages.push_back(stage->sceneProject.id);
    }
    return {
        {"scenes", scenes},
        {"prefab_stages", stages},
        {"code_editor", ctx.codeEditor() && ctx.codeEditor()->hasUnsavedChanges()},
        {"temporary_project", project->isTempUnsavedProject()}
    };
}

bool hasUnsaved(const Json& state) {
    return !state["scenes"].empty() || !state["prefab_stages"].empty() || state["code_editor"].get<bool>();
}

// ------------------------------------------------------------------ create / open

struct SwitchState {
    bool ran = false;
    bool ok = false;
    std::string error;
    std::chrono::steady_clock::time_point requested = std::chrono::steady_clock::now();
};

void switchProjectTool(const CallPtr& call, bool create) {
    McpContext& ctx = call->ctx();
    App& app = ctx.app();
    Project* project = ctx.project();
    const Json& args = call->args();

    const std::string pathText = args.value("path", "");
    if (pathText.empty()) {
        call->fail(create ? "path is required: the directory for the new project (absolute; created when missing)."
                          : "path is required: the project directory or its project.yaml (absolute).");
        return;
    }
    fs::path path(pathText);
    if (!path.is_absolute()) {
        call->fail("path must be absolute: \"" + pathText + "\".");
        return;
    }
    path = path.lexically_normal();
    if (path.filename().empty()) path = path.parent_path();   // trailing slash
    std::error_code ec;
    if (create) {
        if (fs::exists(path / "project.yaml", ec)) {
            call->fail("A project already exists in " + path.generic_string() + ": use open_project.");
            return;
        }
        if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
            call->fail(path.generic_string() + " is a file, not a directory.");
            return;
        }
    } else {
        if (path.filename() == "project.yaml") path = path.parent_path();
        if (!fs::exists(path / "project.yaml", ec)) {
            call->fail("No project.yaml in " + path.generic_string() + ". Use create_project to make a new project there.");
            return;
        }
        std::error_code eq;
        if (fs::equivalent(path, project->getProjectPath(), eq)) {
            Json data = projectSummary(ctx);
            call->ok("That project is already open. " + summaryText(data), data);
            return;
        }
    }
    if (app.isProjectLoading()) {
        call->fail("A project is loading; try again in a moment.");
        return;
    }
    if (refuseDuringBuild(call)) return;

    const std::string unsaved = args.value("unsaved", "cancel");
    if (unsaved != "cancel" && unsaved != "save" && unsaved != "discard") {
        call->fail("unsaved must be cancel, save or discard.");
        return;
    }
    Json state = unsavedState(ctx);
    Json saved = nullptr;
    if (hasUnsaved(state)) {
        if (unsaved == "cancel") {
            call->fail("The open project has unsaved changes. Call again with unsaved: \"save\" (saves them; untitled "
                       "scenes go to the project root as <name>.scene) or \"discard\" (drops them), or save first.",
                       Json{{"unsaved", state}});
            return;
        }
        if (unsaved == "save") {
            std::string error;
            if (!saveEverything(ctx, fs::path(), false, saved, error)) {
                call->fail(error + " Nothing was switched.", Json{{"unsaved", unsavedState(ctx)}});
                return;
            }
            project->saveProjectFile();
        } else {
            // discard: the code editor closes its buffers on the switch without asking
        }
    } else if (unsaved == "save" && !project->isTempProject()) {
        project->saveProjectFile();
    }

    const std::string name = args.value("name", "");
    auto switchState = std::make_shared<SwitchState>();
    app.requestProjectChange([switchState, project, path, name, create]() {
        switchState->ran = true;
        switchState->ok = create ? project->createProjectAt(path, name, &switchState->error)
                                 : project->openProjectNow(path, &switchState->error);
    });

    const double timeoutSeconds = std::max(5.0, std::min(600.0, args.value("timeout_seconds", 180.0)));
    App* appPtr = &app;
    call->waitUntil([switchState, appPtr]() {
        if (switchState->ran) return !appPtr->isProjectLoading();
        // The App drops a request made while another one is pending
        return !appPtr->isProjectLoading() &&
               std::chrono::steady_clock::now() - switchState->requested > std::chrono::seconds(10);
    }, static_cast<int>(timeoutSeconds * 1000.0), [call, switchState, create, path, saved](bool timedOut) {
        McpContext& ctx = call->ctx();
        if (!switchState->ran) {
            call->fail(timedOut ? "The project switch did not start in time (a project is loading?)."
                                : "The editor did not run the project switch (another project change was pending). Try again.");
            return;
        }
        if (timedOut) {
            call->fail("The project is still loading; check editor_status in a moment.");
            return;
        }
        if (!switchState->ok) {
            Json data = projectSummary(ctx);
            call->fail(std::string(create ? "Could not create the project: " : "Could not open the project: ") +
                       switchState->error + ". Open now: " + data["path"].get<std::string>() + ".", data);
            return;
        }
        Json data = projectSummary(ctx);
        if (!saved.is_null()) data["saved_before_switch"] = saved;
        std::string text = std::string(create ? "Created and opened " : "Opened ") + summaryText(data);
        if (create) {
            text += " The new project has one untitled 3D scene: save it with a path (save_project, or the scene tools) "
                    "to put it in the build.";
        }
        call->ok(text, data);
    });
}

// ------------------------------------------------------------------ save_project

void saveProjectTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    if (ctx.isPlaying()) {
        call->fail("Stop Play mode before saving the project.");
        return;
    }
    if (refuseDuringBuild(call)) return;

    const std::string pathText = args.value("path", "");
    const std::string sceneDirText = args.value("untitled_scene_dir", "");
    fs::path sceneDir;
    if (!sceneDirText.empty()) {
        fs::path absolute;
        std::string error;
        if (!McpRefs::resolveProjectPath(project, sceneDirText, absolute, error)) {
            call->fail("untitled_scene_dir: " + error);
            return;
        }
        sceneDir = fs::path(sceneDirText).lexically_normal();
    }

    fs::path target;
    if (!pathText.empty()) {
        target = fs::path(pathText);
        if (!target.is_absolute()) {
            call->fail("path must be absolute.");
            return;
        }
        target = target.lexically_normal();
        if (target.filename().empty()) target = target.parent_path();
        if (!project->isTempProject()) {
            std::error_code eq;
            if (!fs::equivalent(target, project->getProjectPath(), eq)) {
                call->fail("path (Save As) only applies to a temporary project. This project lives in " +
                           project->getProjectPath().generic_string() + "; omit path.");
                return;
            }
            target.clear();
        } else {
            std::error_code ec;
            if (fs::exists(target / "project.yaml", ec)) {
                call->fail("A project already exists in " + target.generic_string() + ".");
                return;
            }
            if (fs::exists(target, ec) && (!fs::is_directory(target, ec) || !fs::is_empty(target, ec))) {
                call->fail(target.generic_string() + " must be a new or empty directory.");
                return;
            }
        }
    }

    // documents that did not load completely are written only when the agent accepts the loss
    const bool allowDataLoss = args.value("allow_data_loss", false);
    const DegradedSavePolicy policy = allowDataLoss ? DegradedSavePolicy::SaveAnyway : DegradedSavePolicy::Refuse;
    Json saved;
    Json refused;
    std::string error;
    if (!saveEverything(ctx, sceneDir, true, saved, error, policy, &refused)) {
        Json data = refused.is_object() ? refused : Json::object();
        data["unsaved"] = unsavedState(ctx);
        call->fail(refused.is_object() ? error + " Pass allow_data_loss=true to write anyway." : error, data);
        return;
    }
    const LoadReport& projectReport = project->getProjectLoadReport();
    if (projectReport.dropped() > 0) {
        if (!allowDataLoss) {
            Json data = degradedDocumentError("project.yaml", projectReport);
            data["saved"] = saved;
            call->fail("project.yaml did not load completely (" + std::to_string(projectReport.dropped()) +
                       " value(s) this editor could not read); the scenes were saved, project.yaml was not. Pass "
                       "allow_data_loss=true to write it anyway.", data);
            return;
        }
        project->saveProjectFile(DegradedSavePolicy::SaveAnyway);
    }

    const std::string name = args.value("name", "");
    if (!name.empty()) project->setName(name);

    bool ok;
    if (!target.empty()) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);   // saveProjectToPath makes only the last level
        ok = project->saveProjectToPath(target);
        if (ok) ctx.app().updateWindowTitle(project->getName());
    } else {
        ok = project->saveProject(false);
    }
    if (!ok) {
        call->fail("Could not write the project file (see the console).", Json{{"saved", saved}});
        return;
    }
    if (ctx.projectWindow()) ctx.projectWindow()->refreshCurrentDirectory();

    Json data = projectSummary(ctx);
    data["saved"] = saved;
    call->ok("Saved " + std::to_string(saved["scenes"].size()) + " scene(s)" +
             (saved["scripts"].get<bool>() ? ", the open scripts" : "") + " and project.yaml. " + summaryText(data), data);
}

// ------------------------------------------------------------------ settings

// The settings registry's MCP rows (editor/settings: the table these tools had before S25), in
// registration order
using Setting = SettingRow;

const std::vector<std::reference_wrapper<const SettingRow>>& settingsTable() {
    static const std::vector<std::reference_wrapper<const SettingRow>> table = [] {
        std::vector<std::reference_wrapper<const SettingRow>> rows;
        for (const SettingRow& row : SettingsRegistry::get().rows()) {
            if (row.mcp) rows.push_back(std::cref(row));
        }
        return rows;
    }();
    return table;
}

SettingsContext settingsContext(McpContext& ctx) {
    return SettingsContext{ctx.project(), ctx.consoleWindow(), ctx.isPlaying()};
}

bool isPreference(const SettingRow& row) {
    return row.scope == SettingsScope::Preference;
}

std::string categoryOf(const std::string& key) {
    const size_t dot = key.find('.');
    return dot == std::string::npos ? key : key.substr(0, dot);
}

std::vector<std::string> categories() {
    std::vector<std::string> out;
    for (const Setting& s : settingsTable()) {
        const std::string c = categoryOf(s.key);
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

const Setting* findSetting(const std::string& key) {
    for (const Setting& s : settingsTable()) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

std::string unknownKeyMessage(const std::string& key) {
    std::string text = "Unknown setting \"" + key + "\".";
    const std::string category = categoryOf(key);
    std::string sameCategory;
    for (const Setting& s : settingsTable()) {
        if (categoryOf(s.key) == category) sameCategory += " " + s.key;
    }
    if (!sameCategory.empty()) {
        text += " In " + category + ":" + sameCategory + ".";
    } else {
        text += " Categories:";
        for (const std::string& c : categories()) text += " " + c;
        text += ". get_project_settings with describe: true lists every key.";
    }
    return text;
}

const std::set<std::string>& platformCategories() {
    static const std::set<std::string> set = {"web", "linux", "windows", "macos", "ios", "android"};
    return set;
}

void getProjectSettingsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    SettingsContext settings = settingsContext(ctx);
    const Json& args = call->args();
    const bool describe = args.value("describe", false);

    // search: where a setting is (every row of Preferences and Project Settings, S25 R39)
    if (args.contains("search") && args["search"].is_string()) {
        Json matches = Json::array();
        for (const SettingRow* row : SettingsRegistry::get().search(args["search"].get<std::string>())) {
            Json match = {{"key", row->key}, {"page", row->page}, {"label", row->label}, {"scope", SettingsRegistry::scopeName(row->scope)}};
            if (!row->section.empty()) match["section"] = row->section;
            if (row->mcp) match["value"] = row->get(settings);
            matches.push_back(match);
        }
        call->ok(std::to_string(matches.size()) + " setting(s) match. The ones with a value are keys for set_project_settings.",
                 Json{{"matches", matches}});
        return;
    }

    std::vector<const Setting*> selected;
    if (args.contains("keys") && args["keys"].is_array() && !args["keys"].empty()) {
        for (const Json& item : args["keys"]) {
            const std::string key = item.is_string() ? item.get<std::string>() : std::string();
            if (const Setting* s = findSetting(key)) {
                selected.push_back(s);
                continue;
            }
            // a category name selects the whole category
            bool any = false;
            for (const Setting& s : settingsTable()) {
                if (categoryOf(s.key) == key) {
                    selected.push_back(&s);
                    any = true;
                }
            }
            if (!any) {
                call->fail(unknownKeyMessage(key));
                return;
            }
        }
    } else {
        const std::string category = args.value("category", "");
        if (!category.empty() && category != "all") {
            const auto cats = categories();
            if (std::find(cats.begin(), cats.end(), category) == cats.end()) {
                std::string text = "Unknown category \"" + category + "\". Categories: all";
                for (const std::string& c : cats) text += ", " + c;
                call->fail(text + ".");
                return;
            }
        }
        for (const Setting& s : settingsTable()) {
            const std::string c = categoryOf(s.key);
            if (category.empty()) {
                if (platformCategories().count(c)) continue;
            } else if (category != "all" && c != category) {
                continue;
            }
            selected.push_back(&s);
        }
    }

    Json values = Json::object();
    Json schema = Json::object();
    for (const Setting* s : selected) {
        values[s->key] = s->get(settings);
        if (describe) {
            Json info = {{"type", s->type}, {"description", s->description}};
            if (!s->values.empty()) info["values"] = s->values;
            if (isPreference(*s)) info["preference"] = true;
            schema[s->key] = info;
        }
    }
    Json data = {{"settings", values}};
    if (describe) data["describe"] = schema;
    if (!args.contains("keys") && args.value("category", "").empty()) {
        data["note"] = "Platform overrides (web, linux, windows, macos, ios, android) are left out: pass category or keys.";
    }
    call->ok(std::to_string(values.size()) + " setting(s).", data);
}

void setProjectSettingsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    SettingsContext settings = settingsContext(ctx);
    Project* project = ctx.project();
    const Json& args = call->args();
    if (!args.contains("values") || !args["values"].is_object() || args["values"].empty()) {
        call->fail("values must be a non-empty object {\"key\": value}, e.g. {\"player.window_width\": 1280, "
                   "\"physics2d.enabled\": true}. get_project_settings lists the keys (describe: true for types).");
        return;
    }
    const Json& values = args["values"];
    if (refuseDuringBuild(call)) return;

    // Validate everything first so a bad value changes nothing
    std::vector<std::pair<const Setting*, Json>> changes;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const Setting* s = findSetting(it.key());
        if (!s) {
            call->fail(unknownKeyMessage(it.key()) + " Nothing was changed.");
            return;
        }
        std::string error;
        if (!s->set(settings, it.value(), false, error)) {
            std::string hint;
            if (!s->values.empty()) {
                hint = " Values:";
                for (const std::string& v : s->values) hint += " " + v;
                hint += ".";
            }
            call->fail(s->key + ": " + error + "." + hint + " Nothing was changed.");
            return;
        }
        changes.push_back({s, it.value()});
    }

    // Apply settings whose application depends on others (asset roots move files) last
    std::stable_partition(changes.begin(), changes.end(), [](const auto& change) {
        return change.first->key != "editor.assets_dir" && change.first->key != "scripting.lua_dir";
    });

    Json applied = Json::object();
    bool projectChanged = false;
    for (const auto& [setting, value] : changes) {
        std::string error;
        if (!setting->set(settings, value, true, error)) {
            call->fail(setting->key + ": " + error + ". Applied before it: " + applied.dump() + ".");
            return;
        }
        applied[setting->key] = setting->get(settings);
        if (!isPreference(*setting)) projectChanged = true;
    }

    std::string text = "Set " + std::to_string(applied.size()) + " setting(s).";
    if (projectChanged) {
        // the standalone build's main.cpp bakes these settings in
        project->markGeneratedSourcesDirty();
        if (!project->isTempProject() || !project->getProjectPath().empty()) {
            if (!project->saveProjectFile()) {
                call->fail("The settings changed in memory but project.yaml could not be written (see the console).",
                           Json{{"settings", applied}});
                return;
            }
            text += " project.yaml saved.";
        }
    }
    call->ok(text, Json{{"settings", applied}});
}

// ------------------------------------------------------------------ edit_build_scenes

// Puts sceneId at `index` of the build order: saved scenes take the new order in the project
// list (untitled ones keep their place after them) and the first one becomes the start scene
void moveBuildScene(Project* project, uint32_t sceneId, size_t index) {
    std::vector<uint32_t> order = buildSceneOrder(project);
    order.erase(std::remove(order.begin(), order.end(), sceneId), order.end());
    order.insert(order.begin() + static_cast<long>(std::min(index, order.size())), sceneId);
    std::vector<SceneProject>& scenes = project->getScenes();
    auto rank = [&order](const SceneProject& s) {
        auto it = std::find(order.begin(), order.end(), s.id);
        return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
    };
    std::stable_sort(scenes.begin(), scenes.end(), [&rank](const SceneProject& a, const SceneProject& b) {
        return rank(a) < rank(b);
    });
    project->setStartSceneId(order.front());
}

void editBuildScenesTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const std::string action = args.value("action", "list");

    auto reply = [&](const std::string& prefix) {
        Json data = {{"scenes_in_build", buildScenesJson(project)}, {"untitled_scenes", untitledScenesJson(project)}};
        std::string text = prefix + "Scenes in build:";
        if (data["scenes_in_build"].empty()) text += " none";
        for (const Json& scene : data["scenes_in_build"]) {
            text += " " + std::to_string(scene["index"].get<size_t>()) + "=" + scene["name"].get<std::string>() +
                    " (" + scene["path"].get<std::string>() + ")";
        }
        text += ".";
        if (!data["untitled_scenes"].empty()) text += " Untitled scenes (not in the build until saved): " + data["untitled_scenes"].dump() + ".";
        call->ok(text, data);
    };

    if (action == "list") {
        reply("");
        return;
    }
    if (action != "add" && action != "remove" && action != "move" && action != "set_start") {
        call->fail("action must be list, add, remove, move or set_start.");
        return;
    }
    if (ctx.isPlaying()) {
        call->fail("Stop Play mode before changing the scenes in build.");
        return;
    }
    if (refuseDuringBuild(call)) return;
    if (!args.contains("scene") || args["scene"].is_null()) {
        call->fail("scene is required for " + action + ": a scene id, name or project-relative .scene path.");
        return;
    }

    SceneProject* scene = nullptr;
    std::string error;
    if (action == "add") {
        // A .scene file in the project folder that is not listed yet
        const std::string pathText = args["scene"].is_string() ? args["scene"].get<std::string>() : std::string();
        if (!pathText.empty() && fs::path(pathText).extension() == ".scene") {
            fs::path absolute;
            if (!McpRefs::resolveProjectPath(project, pathText, absolute, error, true)) {
                std::string files;
                for (const fs::path& file : project->findSceneFiles()) {
                    if (project->findSceneByPath(file) == NULL_PROJECT_SCENE) files += " " + relText(project, file);
                }
                call->fail(error + (files.empty() ? " Every .scene file is listed already." : " Unlisted .scene files:" + files + "."));
                return;
            }
            const fs::path relative = fs::path(pathText).lexically_normal();
            if (project->findSceneByPath(relative) == NULL_PROJECT_SCENE) {
                project->loadScene(relative, false, true, false);
                if (project->findSceneByPath(relative) == NULL_PROJECT_SCENE) {
                    call->fail("Could not load " + pathText + " (see the console).");
                    return;
                }
            }
            scene = project->getScene(project->findSceneByPath(relative));
        } else {
            scene = McpRefs::resolveScene(project, args["scene"], error);
            if (!scene) {
                call->fail(error + " To add a .scene file that is not in the project yet, pass its project-relative path.");
                return;
            }
        }
        if (scene->filepath.empty()) {
            call->fail("Scene " + std::to_string(scene->id) + " \"" + scene->name + "\" is untitled: save it to a .scene path "
                       "first (save_project saves untitled scenes to the project root), then it is in the build.");
            return;
        }
    } else {
        scene = McpRefs::resolveScene(project, args["scene"], error);
        if (!scene) {
            call->fail(error);
            return;
        }
        if (project->isPrefabStage(scene->id)) {
            call->fail("That is a Prefab Mode stage, not a scene.");
            return;
        }
    }
    const uint32_t sceneId = scene->id;
    const std::string sceneName = scene->name;

    if (action == "remove") {
        if (project->getScenes().size() <= 1) {
            call->fail("The project's last scene can't be removed.");
            return;
        }
        if (project->hasSceneUnsavedChanges(sceneId)) {
            call->fail("Scene \"" + sceneName + "\" has unsaved changes: save or discard them first (it would lose them).");
            return;
        }
        project->removeScene(sceneId);
        if (project->getScene(sceneId)) {
            call->fail("The scene could not be removed (see the console).");
            return;
        }
        project->saveProjectFile();
        reply("Removed \"" + sceneName + "\" from the project (its .scene file is kept). ");
        return;
    }

    if (scene->filepath.empty()) {
        call->fail("Scene \"" + sceneName + "\" is untitled, so it is not in the build: save it first.");
        return;
    }

    if (action == "set_start") {
        project->setStartSceneId(sceneId);
    } else if (action == "move") {
        if (!args.contains("index") || !args["index"].is_number_integer()) {
            call->fail("index is required for move: the new build index (0 = start scene).");
            return;
        }
        const size_t count = buildSceneOrder(project).size();
        const long long index = args["index"].get<long long>();
        if (index < 0 || index >= static_cast<long long>(count)) {
            call->fail("index must be 0-" + std::to_string(count - 1) + ".");
            return;
        }
        moveBuildScene(project, sceneId, static_cast<size_t>(index));
    }
    // add: loading it listed it

    if (action == "add" && args.contains("index") && args["index"].is_number_integer()) {
        const long long last = static_cast<long long>(buildSceneOrder(project).size()) - 1;
        moveBuildScene(project, sceneId, static_cast<size_t>(std::max(0LL, std::min(args["index"].get<long long>(), last))));
    }

    if (!project->saveProjectFile()) {
        call->fail("The scene list changed but project.yaml could not be written (see the console).");
        return;
    }
    const char* verb = action == "add" ? "Added" : action == "move" ? "Moved" : "Start scene set to";
    reply(std::string(verb) + " \"" + sceneName + "\". ");
}

// ------------------------------------------------------------------ ProjectBuild.cmake

void editProjectBuildFileTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const fs::path path = project->getProjectPath() / "ProjectBuild.cmake";

    if (!args.contains("content") || args["content"].is_null()) {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            call->ok("There is no ProjectBuild.cmake yet. Write one with content, e.g.:\n"
                     "target_include_directories(${JASPERENGINE_TARGET} PRIVATE ${JASPERENGINE_SCRIPTS_DIR}/thirdparty)\n"
                     "target_compile_definitions(${JASPERENGINE_TARGET} PRIVATE MY_FLAG=1)",
                     Json{{"exists", false}, {"path", "ProjectBuild.cmake"}});
            return;
        }
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        call->ok(buffer.str(), Json{{"exists", true}, {"path", "ProjectBuild.cmake"}, {"content", buffer.str()}});
        return;
    }
    if (!args["content"].is_string()) {
        call->fail("content must be a string (the complete file), or omitted to read the file.");
        return;
    }
    if (refuseDuringBuild(call)) return;
    const std::string content = args["content"].get<std::string>();
    if (content.size() > 256 * 1024) {
        call->fail("content is too large (max 256 KB).");
        return;
    }
    if (content.find("target_") != std::string::npos && content.find("${JASPERENGINE_TARGET}") == std::string::npos) {
        call->fail("Attach target settings to ${JASPERENGINE_TARGET} (the target name differs between the editor's Play "
                   "build and exported players), and use ${JASPERENGINE_SCRIPTS_DIR} as the root of script paths.");
        return;
    }
    std::error_code ec;
    if (content.empty()) {
        fs::remove(path, ec);
    } else {
        const io::WriteResult written = DocumentFiles::write(path, content);
        if (!written.ok()) {
            call->fail(DocumentFiles::saveFailureMessage("ProjectBuild.cmake", written));
            return;
        }
    }
    if (ctx.projectWindow()) ctx.projectWindow()->refreshCurrentDirectory();
    Out::info("MCP: %s ProjectBuild.cmake", content.empty() ? "removed" : "updated");
    call->ok(std::string(content.empty() ? "Removed" : "Wrote") + " ProjectBuild.cmake (" + std::to_string(content.size()) +
             " bytes). It is read by the next Play with C++ scripts and by Desktop/Web builds (build_project); check their "
             "log for CMake errors.", Json{{"path", "ProjectBuild.cmake"}, {"bytes", content.size()}});
}

} // namespace

void registerProjectTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "get_project_summary";
        tool.title = "Get Project Summary";
        tool.description =
            "Open project overview: name, path, temporary, unsaved, Scenes in Build (0 = start), untitled scenes, "
            "dirs, cxx_standard, prefabs, physics, versions. All settings: get_project_settings.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getProjectSummaryTool;
        registry.addTool(std::move(tool));
    }

    // Shared by create_project / open_project
    const Json unsavedSchema = withDefault(stringEnum(
        "The open project's unsaved changes (cancel: fail listing them).",
        {"cancel", "save", "discard"}), "cancel");
    const Json loadTimeout = withDefault(number("", 5, 600), 180);

    {
        ToolDef tool;
        tool.name = "create_project";
        tool.activity = ToolActivity::ProjectSwitch;  // busy policy (McpServer::busyRefusal)
        tool.title = "Create Project";
        tool.description =
            "File > New Project: creates a project in `path` (no project.yaml there yet) with one untitled 3D scene, "
            "opens it and waits for load. Save the scene (save_project) before building.";
        tool.inputSchema = object({
            {"path", string("Absolute directory.")},
            {"name", string("Default: directory name.")},
            {"unsaved", unsavedSchema},
            {"timeout_seconds", loadTimeout}
        }, {"path"});
        tool.annotations.destructive = true;
        tool.annotations.openWorld = true;
        tool.allowedWhilePlaying = false;
        tool.handler = [](CallPtr call) { switchProjectTool(call, true); };
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "open_project";
        tool.activity = ToolActivity::ProjectSwitch;  // busy policy (McpServer::busyRefusal)
        tool.title = "Open Project";
        tool.description = "File > Open Project: opens `path` and waits for load. Returns the project summary.";
        tool.inputSchema = object({
            {"path", string("Absolute project dir or project.yaml.")},
            {"unsaved", unsavedSchema},
            {"timeout_seconds", loadTimeout}
        }, {"path"});
        tool.annotations.destructive = true;
        tool.annotations.openWorld = true;
        tool.allowedWhilePlaying = false;
        tool.handler = [](CallPtr call) { switchProjectTool(call, false); };
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "save_project";
        tool.title = "Save Project";
        tool.description =
            "File > Save All: modified scenes (untitled ones to <untitled_scene_dir>/<name>.scene), Prefab Mode, "
            "open scripts, project.yaml; returns what was saved.";
        tool.inputSchema = object({
            {"path", string("Temporary project: Save As to this empty absolute dir.")},
            {"name", string("Rename the project.")},
            {"untitled_scene_dir", withDefault(string(""), "")},
            {"allow_data_loss", boolean("")}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = saveProjectTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_project_settings";
        tool.title = "Get Project Settings";
        tool.description =
            "Project Settings and editor preferences as flat dotted keys (player.product_name, "
            "physics.layer_collision_matrix, tags_and_layers.layers, preferences.build_jobs...). No args: all but "
            "platform overrides. describe adds each key's type, allowed values, meaning.";
        tool.inputSchema = object({
            {"category", string("player, web, linux, windows, macos, ios, android, graphics, quality, physics, "
                                "physics2d, scripting, editor, tags_and_layers, preferences or all.")},
            {"keys", array(Json{{"type", "string"}}, "Keys or categories.")},
            {"describe", withDefault(boolean(""), false)},
            {"search", string("Words: matches {key, page, label, scope} of every Preferences/Project Settings row.")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getProjectSettingsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_project_settings";
        tool.title = "Set Project Settings";
        tool.description =
            "Set get_project_settings keys. Every value is validated first (one bad value changes nothing); saved to "
            "project.yaml at once, NOT undoable. Lists are replaced; tags_and_layers.layers merges by index. "
            "Example: {\"values\": {\"player.window_width\": 800, \"physics.layer_collision_matrix\": [[\"Player\", "
            "\"Water\"]]}}.";
        tool.inputSchema = object({
            {"values", anyObject("{key: value}")}
        }, {"values"});
        tool.annotations.destructive = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = false;
        tool.handler = setProjectSettingsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_build_scenes";
        tool.title = "Edit Scenes in Build";
        tool.description =
            "Edit Scenes in Build (index 0 = start scene). add: also a .scene path not in the project yet. remove: "
            "off the list, file kept (refused if unsaved or the last scene). Not undoable.";
        tool.inputSchema = object({
            {"action", withDefault(stringEnum("", {"list", "add", "remove", "move", "set_start"}), "list")},
            {"scene", sceneRef("Required unless list.")},
            {"index", integer("move (or add): build index.")}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = editBuildScenesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_project_build_file";
        tool.title = "Edit ProjectBuild.cmake";
        tool.description =
            "Read (omit content) or replace ProjectBuild.cmake, the CMake hook for C++ scripts' include dirs, "
            "libraries and definitions (Play and player builds); content \"\" deletes it. Use ${JASPERENGINE_TARGET} "
            "and ${JASPERENGINE_SCRIPTS_DIR}. Not undoable.";
        tool.inputSchema = object({
            {"content", string("Whole new file.")}
        });
        tool.annotations.destructive = true;
        tool.handler = editProjectBuildFileTool;
        registry.addTool(std::move(tool));
    }
}

}
