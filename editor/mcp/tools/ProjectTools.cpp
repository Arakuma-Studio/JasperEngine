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

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorVersion.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "registry/TagManager.h"
#include "util/CxxStandards.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/ShellEnv.h"
#include "window/CodeEditor.h"
#include "window/ConsoleWindow.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <cstdlib>
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
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(path));
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
    data["path"] = FileUtils::pathToGenericUtf8(project->getProjectPath());
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
    data["assets_dir"] = FileUtils::pathToGenericUtf8(project->getAssetsDir());
    data["lua_dir"] = FileUtils::pathToGenericUtf8(project->getLuaDir());
    data["script_dirs"] = Json::array();
    for (const fs::path& dir : project->getScriptDirs()) data["script_dirs"].push_back(FileUtils::pathToGenericUtf8(dir));
    data["cxx_standard"] = project->getCxxStandard();
    data["standalone_prefabs"] = Json::array();
    for (const fs::path& prefab : project->getStandalonePrefabs()) data["standalone_prefabs"].push_back(FileUtils::pathToGenericUtf8(prefab));
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
bool saveEverything(McpContext& ctx, const fs::path& untitledDir, bool saveUntitled, Json& saved, std::string& error) {
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
        if (!project->saveSceneNow(stage->sceneProject.id, {}, &message)) {
            error = "Could not save the prefab open in Prefab Mode: " + message;
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
        if (!project->saveSceneNow(id, target, &message)) {
            error = "Could not save scene " + std::to_string(id) + " \"" + scene->name + "\": " + message;
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

// An asset or Lua root value (assetsDir / luaDir in project.yaml, editor.assets_dir /
// scripting.lua_dir): "" when it is a plain project-relative folder, else why it isn't (absolute,
// a drive, or a ".." step: a root out of the project folder is one only the user sets)
std::string rootValueRefusal(const std::string& text) {
    const fs::path value = FileUtils::pathFromUtf8(text);
    if (value.is_absolute() || value.has_root_name() || value.has_root_directory()) return "\"" + text + "\" is absolute";
    for (const fs::path& part : value) {
        if (part == "..") return "\"" + text + "\" has a '..' step";
        // an editor-owned or hidden folder (.jasperengine, .trash, .git ...) is never an asset root
        const std::string name = safewrite::comparableName(part);
        if (!name.empty() && name[0] == '.' && name != ".") return "\"" + text + "\" goes through the hidden folder " + FileUtils::pathToUtf8(part);
        if (safewrite::isProjectFileName(part) || name == "cmakelists.txt") return "\"" + text + "\" names a file the editor owns";
    }
    return std::string();
}

// A project opened or created over MCP whose project.yaml points its assets or Lua folder out of
// the project folder (absolute, or with ".."): the editor GUI opens it (the user chose it), an MCP
// call doesn't (the folder would become a trusted root of the agent's choosing). "" when fine.
std::string outsideRootsRefusal(const fs::path& projectDir) {
    const fs::path file = projectDir / "project.yaml";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::string();
    YAML::Node node;
    try {
        std::ifstream in(file, std::ios::binary);
        node = YAML::Load(in);
    } catch (const std::exception& e) {
        return "its project.yaml can't be read (" + std::string(e.what()) + ")";
    }
    if (!node.IsMap()) return std::string();
    for (const char* key : {"assetsDir", "luaDir"}) {
        const YAML::Node value = node[key];
        if (!value) continue;
        std::string text;
        try {
            text = value.as<std::string>();
        } catch (const std::exception&) {
            return std::string("its ") + key + " is not a folder name";
        }
        if (const std::string why = rootValueRefusal(text); !why.empty()) {
            return std::string("this project's ") + (std::string(key) == "assetsDir" ? "assets_dir" : "lua_dir") + " (" + why +
                   ") points outside the project folder; open it from the editor";
        }
    }
    return std::string();
}

// The user's home folder, resolved ("" when unknown): $HOME / %USERPROFILE%, else the account's
// own (getpwuid, SHGetKnownFolderPath) so an unset HOME doesn't switch the home rules off
fs::path homeFolder() {
    const fs::path home = ShellEnv::homeDirectory();
    if (home.empty()) return fs::path();
    return safewrite::resolvedPath(home);
}

bool isFilesystemRoot(const fs::path& path) {
    return !path.empty() && (path == path.root_path() || !path.has_relative_path());
}

bool samePlace(const fs::path& a, const fs::path& b) {
    return !a.empty() && !b.empty() && safewrite::isAtOrBelow(a, b) && safewrite::isAtOrBelow(b, a);
}

// Where an MCP create_project / Save As may put a new project. Allowed: a new or empty folder (not a
// link) whose parent IS the current project's parent folder (spelled and resolved), or one at or
// below the user's default export folder (resolved). Below its anchor no path component may be
// hidden ('.'-named); the anchor may not be the user's home folder itself (a project in the home
// root gets siblings only through the GUI). "" when allowed.
std::string switchTargetRefusal(Project* project, const fs::path& target) {
    const fs::path normal = target.lexically_normal();
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(normal, ec))) {
        return FileUtils::pathToGenericUtf8(normal) + " is a symbolic link: a new project goes in a real, new or empty folder";
    }
    const fs::path parent = normal.parent_path();
    const fs::path parentReal = safewrite::resolvedPath(parent);
    const fs::path targetReal = safewrite::writeLocation(normal);
    const fs::path home = homeFolder();
    auto hiddenBelow = [](const fs::path& rel) {
        for (const fs::path& part : rel) {
            const std::string name = safewrite::comparableName(part);
            if (!name.empty() && name[0] == '.' && name != "." && name != "..") return true;
        }
        return false;
    };
    // never the home folder itself, nor a folder directly in it (a project there gets siblings only
    // through the GUI): checked where the target and its parent resolve
    if (!home.empty() && (samePlace(targetReal, home) || samePlace(parentReal, home))) {
        return "it is your home folder (or a folder directly inside it): create the project from the editor (File > New Project)";
    }
    // ... and the same for EVERY folder the call would create on the way (<home>/bin/z would make
    // <home>/bin): none may be the home folder, directly inside it, or directly in a filesystem root
    {
        fs::path existing = normal;
        std::vector<fs::path> missing;
        std::error_code ex;
        while (!fs::exists(fs::symlink_status(existing, ex)) && existing.has_relative_path() && existing.has_parent_path() &&
               existing != existing.parent_path()) {
            missing.insert(missing.begin(), existing.filename());
            existing = existing.parent_path();
        }
        fs::path made = safewrite::resolvedPath(existing);
        for (const fs::path& name : missing) {
            const fs::path above = made;
            made /= name;
            if (!home.empty() && (samePlace(made, home) || samePlace(above, home))) {
                return "it would create " + FileUtils::pathToGenericUtf8(made) +
                       " directly in your home folder: create the project from the editor (File > New Project)";
            }
            if (isFilesystemRoot(above)) {
                return "it would create " + FileUtils::pathToGenericUtf8(made) + " directly in a filesystem root";
            }
        }
    }
    const fs::path cur = project && !project->getProjectPath().empty() ? project->getProjectPath().lexically_normal() : fs::path();
    // (a) a sibling of the current project: same parent, spelled and resolved
    if (!cur.empty() && cur.has_parent_path()) {
        const fs::path curParent = cur.parent_path();
        // not when the project's parent is a filesystem / drive root, spelled or once resolved (a
        // link to "/"): "beside it" would be the root
        const bool parentIsRoot = isFilesystemRoot(curParent) || isFilesystemRoot(safewrite::resolvedPath(curParent));
        if (!parentIsRoot && parent == curParent && samePlace(parentReal, safewrite::resolvedPath(curParent))) {
            if (hiddenBelow(normal.filename())) {
                return "a hidden ('.'-named) folder is not a project folder an MCP call makes";
            }
            return std::string();
        }
    }
    // (b) at or below the user's default export folder (resolved)
    if (const fs::path def = AppSettings::getDefaultExportDirectory(); !def.empty()) {
        const fs::path defReal = safewrite::resolvedPath(def);
        if (!defReal.empty() && !targetReal.empty() && safewrite::isAtOrBelow(targetReal, defReal) && !samePlace(targetReal, defReal)) {
            if (hiddenBelow(targetReal.lexically_relative(defReal))) {
                return "a hidden ('.'-named) folder is not a project folder an MCP call makes";
            }
            if (!cur.empty() && safewrite::isAtOrBelow(targetReal, safewrite::resolvedPath(cur))) {
                return "it is inside the currently open project: a project nested in another is not made over MCP";
            }
            return std::string();
        }
    }
    return "it is neither beside the current project (same parent folder) nor under the default export folder "
           "(Edit > Preferences): create the project from the editor (File > New Project, or Save As), then open it with open_project";
}

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
    fs::path path = FileUtils::pathFromUtf8(pathText);
    if (!path.is_absolute()) {
        call->fail("path must be absolute: \"" + pathText + "\".");
        return;
    }
    path = path.lexically_normal();
    if (path.filename().empty()) path = path.parent_path();   // trailing slash
    std::error_code ec;
    if (create) {
        if (fs::exists(path / "project.yaml", ec)) {
            call->fail("A project already exists in " + FileUtils::pathToGenericUtf8(path) + ": use open_project.");
            return;
        }
        if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
            call->fail(FileUtils::pathToGenericUtf8(path) + " is a file, not a directory.");
            return;
        }
        // a new or empty folder only: the project becomes a trusted root, so an agent can't turn
        // a folder that holds other things (a home directory) into one
        if (fs::is_directory(path, ec) && !fs::is_empty(path, ec)) {
            call->fail(FileUtils::pathToGenericUtf8(path) + " is not empty: create_project makes a project in a new or empty folder. "
                       "Pick a new folder (it is created), or create the project in the editor's New Project dialog.");
            return;
        }
        if (const std::string refusal = switchTargetRefusal(project, path); !refusal.empty()) {
            call->fail("create_project here is refused: " + refusal);
            return;
        }
    } else {
        if (path.filename() == "project.yaml") path = path.parent_path();
        if (!fs::exists(path / "project.yaml", ec)) {
            call->fail("No project.yaml in " + FileUtils::pathToGenericUtf8(path) + ". Use create_project to make a new project there.");
            return;
        }
        std::error_code eq;
        if (fs::equivalent(path, project->getProjectPath(), eq)) {
            Json data = projectSummary(ctx);
            call->ok("That project is already open. " + summaryText(data), data);
            return;
        }
        if (const std::string refusal = outsideRootsRefusal(path); !refusal.empty()) {
            call->fail("Not opened: " + refusal + ".");
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
        // checked again right before the load (the file may have changed since the call came in)
        if (const std::string refusal = outsideRootsRefusal(path); !refusal.empty()) {
            switchState->ok = false;
            switchState->error = refusal;
            return;
        }
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
        if (!McpRefs::resolveProjectWritePath(project, sceneDirText, absolute, error)) {
            call->fail("untitled_scene_dir: " + error);
            return;
        }
        sceneDir = FileUtils::pathFromUtf8(sceneDirText).lexically_normal();
    }

    fs::path target;
    if (!pathText.empty()) {
        target = FileUtils::pathFromUtf8(pathText);
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
                           FileUtils::pathToGenericUtf8(project->getProjectPath()) + "; omit path.");
                return;
            }
            target.clear();
        } else {
            std::error_code ec;
            if (fs::exists(target / "project.yaml", ec)) {
                call->fail("A project already exists in " + FileUtils::pathToGenericUtf8(target) + ".");
                return;
            }
            if (fs::exists(target, ec) && (!fs::is_directory(target, ec) || !fs::is_empty(target, ec))) {
                call->fail(FileUtils::pathToGenericUtf8(target) + " must be a new or empty directory.");
                return;
            }
            if (const std::string refusal = switchTargetRefusal(project, target); !refusal.empty()) {
                call->fail("Save As here is refused: " + refusal);
                return;
            }
        }
    }

    Json saved;
    std::string error;
    if (!saveEverything(ctx, sceneDir, true, saved, error)) {
        call->fail(error, Json{{"unsaved", unsavedState(ctx)}});
        return;
    }

    const std::string name = args.value("name", "");
    if (!name.empty()) project->setName(name);

    bool ok;
    if (!target.empty()) {
        std::error_code ec;
        // Save As: the explicit target outside the project is the user's choice (a temporary
        // project's new home): the call's strict write scope is suspended for it
        safewrite::Scope saveAs{fs::path()};
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

struct Setting {
    std::string key;
    std::string type;          // bool, int, number, string, enum, path, list, object
    std::string description;
    std::vector<std::string> values;   // enum values
    std::function<Json(McpContext&)> get;
    // Validates `value`; applies it when apply is true. Returns false with error.
    std::function<bool(McpContext&, const Json& value, bool apply, std::string& error)> set;
    bool preference = false;   // editor/machine preference, not project.yaml
    // A machine-level preference naming a program to run or a folder outside the project: only the
    // user changes it (Edit > Preferences); over MCP it is read-only
    bool userOnly = false;
};

template <typename T>
struct EnumMap {
    const char* name;
    T value;
};

const EnumMap<Scaling> scalingModes[] = {
    {"fit_width", Scaling::FITWIDTH}, {"fit_height", Scaling::FITHEIGHT}, {"letterbox", Scaling::LETTERBOX},
    {"crop", Scaling::CROP}, {"stretch", Scaling::STRETCH}, {"native", Scaling::NATIVE}};
const EnumMap<TextureStrategy> textureStrategies[] = {
    {"fit", TextureStrategy::FIT}, {"resize", TextureStrategy::RESIZE}, {"none", TextureStrategy::NONE}};
const EnumMap<WindowMode> windowModes[] = {
    {"windowed", WindowMode::WINDOWED}, {"maximized", WindowMode::MAXIMIZED}, {"fullscreen", WindowMode::FULLSCREEN}};
const EnumMap<AndroidOrientation> androidOrientations[] = {
    {"unspecified", AndroidOrientation::Unspecified}, {"portrait", AndroidOrientation::Portrait},
    {"landscape", AndroidOrientation::Landscape}, {"sensor_portrait", AndroidOrientation::SensorPortrait},
    {"sensor_landscape", AndroidOrientation::SensorLandscape}, {"full_sensor", AndroidOrientation::FullSensor}};

template <typename T, size_t N>
std::vector<std::string> enumNames(const EnumMap<T> (&map)[N]) {
    std::vector<std::string> names;
    for (const auto& item : map) names.push_back(item.name);
    return names;
}

template <typename T, size_t N>
std::string enumName(const EnumMap<T> (&map)[N], T value) {
    for (const auto& item : map) {
        if (item.value == value) return item.name;
    }
    return map[0].name;
}

template <typename T, size_t N>
bool parseEnum(const EnumMap<T> (&map)[N], const Json& value, T& out, std::string& error) {
    if (value.is_string()) {
        const std::string text = lowerText(value.get<std::string>());
        for (const auto& item : map) {
            if (text == item.name) {
                out = item.value;
                return true;
            }
        }
    }
    error = "must be one of:";
    for (const auto& item : map) error += std::string(" ") + item.name;
    return false;
}

bool readBool(const Json& value, bool& out, std::string& error) {
    if (!value.is_boolean()) {
        error = "must be true or false";
        return false;
    }
    out = value.get<bool>();
    return true;
}

bool readInt(const Json& value, long long minimum, long long maximum, long long& out, std::string& error) {
    if (!value.is_number_integer() && !(value.is_number_float() && value.get<double>() == static_cast<long long>(value.get<double>()))) {
        error = "must be an integer";
        return false;
    }
    out = value.is_number_integer() ? value.get<long long>() : static_cast<long long>(value.get<double>());
    if (out < minimum || out > maximum) {
        error = "must be between " + std::to_string(minimum) + " and " + std::to_string(maximum);
        return false;
    }
    return true;
}

bool readNumber(const Json& value, double minimum, double maximum, double& out, std::string& error) {
    if (!value.is_number()) {
        error = "must be a number";
        return false;
    }
    out = value.get<double>();
    if (out < minimum || out > maximum) {
        std::ostringstream text;
        text << "must be between " << minimum << " and " << maximum;
        error = text.str();
        return false;
    }
    return true;
}

bool readString(const Json& value, std::string& out, std::string& error, size_t maxLength = 4096) {
    if (!value.is_string()) {
        error = "must be a string";
        return false;
    }
    out = value.get<std::string>();
    if (out.size() > maxLength) {
        error = "is too long (max " + std::to_string(maxLength) + " characters)";
        return false;
    }
    return true;
}

// "" clears; otherwise an existing project-relative file
bool readProjectFile(McpContext& ctx, const Json& value, fs::path& out, std::string& error) {
    std::string text;
    if (!readString(value, text, error)) return false;
    if (text.empty()) {
        out.clear();
        return true;
    }
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(ctx.project(), text, absolute, error, true)) return false;
    out = FileUtils::pathFromUtf8(text).lexically_normal();
    return true;
}

// Validation shared with Project Settings (window/dialog/ProjectSettingsWindow.cpp)
bool isNumericVersion(const std::string& value, int maxParts, unsigned long long maxPart) {
    if (value.empty()) return false;
    int parts = 0;
    size_t start = 0;
    while (true) {
        const size_t dot = value.find('.', start);
        const size_t end = dot == std::string::npos ? value.size() : dot;
        if (start >= end) return false;
        unsigned long long part = 0;
        for (size_t i = start; i < end; i++) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
            part = part * 10 + static_cast<unsigned>(value[i] - '0');
            if (part > maxPart) return false;
        }
        if (++parts > maxParts) return false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

bool isDottedIdentifier(const std::string& value, bool (*partValid)(const std::string&, size_t, size_t)) {
    if (value.empty() || value.back() == '.') return false;
    size_t start = 0;
    int parts = 0;
    while (start < value.size()) {
        const size_t dot = value.find('.', start);
        const size_t end = dot == std::string::npos ? value.size() : dot;
        if (!partValid(value, start, end)) return false;
        parts++;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts >= 2;
}

bool sharedIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || !std::isalpha(static_cast<unsigned char>(v[s]))) return false;
    for (size_t i = s; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i]))) return false;
    return true;
}
bool javaIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || (!std::isalpha(static_cast<unsigned char>(v[s])) && v[s] != '_')) return false;
    for (size_t i = s + 1; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i])) && v[i] != '_') return false;
    return true;
}
bool appleIdPart(const std::string& v, size_t s, size_t e) {
    if (s >= e || v[s] == '-' || v[e - 1] == '-') return false;
    for (size_t i = s; i < e; i++) if (!std::isalnum(static_cast<unsigned char>(v[i])) && v[i] != '-') return false;
    return true;
}

using Validator = std::function<bool(const std::string&)>;

Setting stringSetting(const std::string& key, const std::string& description,
                      std::function<std::string&(Project&)> field, Validator valid = nullptr,
                      const std::string& validHint = {}) {
    Setting s;
    s.key = key;
    s.type = "string";
    s.description = description;
    s.get = [field](McpContext& ctx) { return Json(field(*ctx.project())); };
    s.set = [field, valid, validHint](McpContext& ctx, const Json& value, bool apply, std::string& error) {
        std::string text;
        if (!readString(value, text, error)) return false;
        if (!text.empty() && valid && !valid(text)) {
            error = "\"" + text + "\" is not valid: " + validHint;
            return false;
        }
        if (apply) field(*ctx.project()) = text;
        return true;
    };
    return s;
}

Setting boolSetting(const std::string& key, const std::string& description,
                    std::function<bool(Project&)> get, std::function<void(Project&, bool)> set) {
    Setting s;
    s.key = key;
    s.type = "bool";
    s.description = description;
    s.get = [get](McpContext& ctx) { return Json(get(*ctx.project())); };
    s.set = [set](McpContext& ctx, const Json& value, bool apply, std::string& error) {
        bool b;
        if (!readBool(value, b, error)) return false;
        if (apply) set(*ctx.project(), b);
        return true;
    };
    return s;
}

Setting boolField(const std::string& key, const std::string& description, std::function<bool&(Project&)> field) {
    return boolSetting(key, description, [field](Project& p) { return field(p); },
                       [field](Project& p, bool v) { field(p) = v; });
}

Setting pathSetting(const std::string& key, const std::string& description, std::function<fs::path&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "path";
    s.description = description + " Project-relative file path; \"\" clears it.";
    s.get = [field](McpContext& ctx) { return Json(FileUtils::pathToGenericUtf8(field(*ctx.project()))); };
    s.set = [field](McpContext& ctx, const Json& value, bool apply, std::string& error) {
        fs::path path;
        if (!readProjectFile(ctx, value, path, error)) return false;
        if (apply) field(*ctx.project()) = path;
        return true;
    };
    return s;
}

Setting intField(const std::string& key, const std::string& description, long long minimum, long long maximum,
                 std::function<unsigned int&(Project&)> field) {
    Setting s;
    s.key = key;
    s.type = "int";
    s.description = description;
    s.get = [field](McpContext& ctx) { return Json(field(*ctx.project())); };
    s.set = [field, minimum, maximum](McpContext& ctx, const Json& value, bool apply, std::string& error) {
        long long v;
        if (!readInt(value, minimum, maximum, v, error)) return false;
        if (apply) field(*ctx.project()) = static_cast<unsigned int>(v);
        return true;
    };
    return s;
}

Json layerCollisionJson(const LayerCollisionRows& rows) {
    // Only what differs from "everything collides": the pairs that don't
    Json pairs = Json::array();
    for (int a = 0; a < MAX_LAYERS; a++) {
        for (int b = a; b < MAX_LAYERS; b++) {
            const bool collide = (rows[a] & (1u << b)) && (rows[b] & (1u << a));
            if (!collide) {
                const std::string nameA = TagManager::layerToName(a);
                const std::string nameB = TagManager::layerToName(b);
                pairs.push_back(Json::array({nameA.empty() ? Json(a) : Json(nameA), nameB.empty() ? Json(b) : Json(nameB)}));
            }
        }
    }
    return pairs;
}

bool parseLayer(const Json& value, int& layer, std::string& error) {
    if (value.is_number_integer()) {
        layer = value.get<int>();
        if (layer < 0 || layer >= MAX_LAYERS) {
            error = "layer index " + std::to_string(layer) + " is outside 0-31";
            return false;
        }
        return true;
    }
    if (value.is_string()) {
        layer = TagManager::nameToLayer(value.get<std::string>());
        if (layer < 0) {
            error = "no layer is named \"" + value.get<std::string>() + "\" (name it in tags_and_layers.layers first, or use the index)";
            return false;
        }
        return true;
    }
    error = "a layer is an index (0-31) or a layer name";
    return false;
}

Setting layerCollisionSetting(const std::string& key, bool is3D) {
    Setting s;
    s.key = key;
    s.type = "list";
    s.description = std::string("Layer Collision Matrix of ") + (is3D ? "Physics (3D)" : "Physics 2D") +
                    ": the layer pairs that do NOT collide, as [[a, b], ...] with layer names or indexes "
                    "(everything else collides). Setting it replaces the whole list; [] makes every layer collide. Layer names must "
                    "exist before the call (name them with tags_and_layers.layers in an earlier call) or use indexes.";
    s.get = [is3D](McpContext& ctx) {
        Project* p = ctx.project();
        return layerCollisionJson(is3D ? p->getLayerCollisionMatrix3D() : p->getLayerCollisionMatrix2D());
    };
    s.set = [is3D](McpContext& ctx, const Json& value, bool apply, std::string& error) {
        if (!value.is_array()) {
            error = "must be a list of [layerA, layerB] pairs";
            return false;
        }
        LayerCollisionRows rows = allLayersCollide();
        for (const Json& pair : value) {
            if (!pair.is_array() || pair.size() != 2) {
                error = "each entry must be a [layerA, layerB] pair";
                return false;
            }
            int a, b;
            if (!parseLayer(pair[0], a, error) || !parseLayer(pair[1], b, error)) return false;
            rows[a] &= ~(1u << b);
            rows[b] &= ~(1u << a);
        }
        if (apply) {
            Project* p = ctx.project();
            if (is3D) p->setLayerCollisionMatrices(rows, p->getLayerCollisionMatrix2D());
            else p->setLayerCollisionMatrices(p->getLayerCollisionMatrix3D(), rows);
        }
        return true;
    };
    return s;
}

const std::vector<Setting>& settingsTable() {
    static const std::vector<Setting> table = [] {
        std::vector<Setting> t;
        auto add = [&t](Setting s) { t.push_back(std::move(s)); };

        // ---- player
        {
            Setting s;
            s.key = "player.product_name";
            s.type = "string";
            s.description = "Project name (Product Name): the editor title, the default app/window name and the executable name.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getName()); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 255)) return false;
                if (text.empty()) {
                    error = "must not be empty";
                    return false;
                }
                if (apply && text != ctx.project()->getName()) ctx.project()->setName(text);
                return true;
            };
            add(s);
        }
        add(stringSetting("player.application_name", "Application display name shared by every platform; \"\" uses the project name.",
                          [](Project& p) -> std::string& { return p.getApplicationSettings().name; }));
        {
            Setting s = stringSetting("player.identifier", "Application identifier (reverse domain, letters and digits, e.g. com.mycompany.game) shared by every platform.",
                                      [](Project& p) -> std::string& { return p.getApplicationSettings().identifier; },
                                      [](const std::string& v) { return isDottedIdentifier(v, sharedIdPart); },
                                      "two or more dot-separated parts of letters and digits, each starting with a letter");
            add(s);
        }
        add(stringSetting("player.version", "Application version (1 to 3 numeric parts, e.g. 1.2.0).",
                          [](Project& p) -> std::string& { return p.getApplicationSettings().version; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts, each 0-65535"));
        add(intField("player.build", "Build number (Apple build number, Android version code).", 1, 2100000000,
                     [](Project& p) -> unsigned int& { return p.getApplicationSettings().build; }));
        {
            Setting s;
            s.key = "player.window_mode";
            s.type = "enum";
            s.values = enumNames(windowModes);
            s.description = "Desktop window mode of the built player.";
            s.get = [](McpContext& ctx) { return Json(enumName(windowModes, ctx.project()->getWindowMode())); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                WindowMode mode;
                if (!parseEnum(windowModes, value, mode, error)) return false;
                if (apply) ctx.project()->setWindowMode(mode);
                return true;
            };
            add(s);
        }
        for (const bool width : {true, false}) {
            Setting s;
            s.key = width ? "player.window_width" : "player.window_height";
            s.type = "int";
            s.description = std::string("Desktop window ") + (width ? "width" : "height") + " in pixels.";
            s.get = [width](McpContext& ctx) {
                return Json(width ? ctx.project()->getWindowWidth() : ctx.project()->getWindowHeight());
            };
            s.set = [width](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 16384, v, error)) return false;
                if (apply) {
                    Project* p = ctx.project();
                    if (width) p->setWindowSize(static_cast<unsigned>(v), p->getWindowHeight());
                    else p->setWindowSize(p->getWindowWidth(), static_cast<unsigned>(v));
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("player.window_resizable", "The desktop window can be resized.",
                        [](Project& p) { return p.isWindowResizable(); }, [](Project& p, bool v) { p.setWindowResizable(v); }));
        {
            Setting s;
            s.key = "player.window_title";
            s.type = "string";
            s.description = "Desktop window title; \"\" uses the project name.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getWindowTitle()); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error, 255)) return false;
                if (apply) ctx.project()->setWindowTitle(text);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.window_icon";
            s.type = "path";
            s.description = "Default icon (desktop window and platforms without their own), a project-relative image; \"\" clears it.";
            s.get = [](McpContext& ctx) { return Json(FileUtils::pathToGenericUtf8(ctx.project()->getWindowIcon())); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                fs::path path;
                if (!readProjectFile(ctx, value, path, error)) return false;
                if (apply) ctx.project()->setWindowIcon(path);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_scene";
            s.type = "scene";
            s.description = "Splash/loading scene shown while the start scene loads: a saved scene (id, name or .scene path), or null for none.";
            s.get = [](McpContext& ctx) {
                const SceneProject* scene = ctx.project()->getScene(ctx.project()->getLoadingSettings().sceneId);
                return scene ? Json{{"id", scene->id}, {"name", scene->name}} : Json(nullptr);
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                uint32_t id = 0;
                if (!value.is_null() && !(value.is_number_integer() && value.get<long long>() == 0)) {
                    SceneProject* scene = McpRefs::resolveScene(ctx.project(), value, error);
                    if (!scene) return false;
                    if (scene->filepath.empty()) {
                        error = "the scene is not saved, so it is not in the build";
                        return false;
                    }
                    id = scene->id;
                }
                if (apply) ctx.project()->getLoadingSettings().sceneId = id;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_delay";
            s.type = "number";
            s.description = "Seconds the loading scene stays at least.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getLoadingSettings().delay); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 600.0, v, error)) return false;
                if (apply) ctx.project()->getLoadingSettings().delay = static_cast<float>(v);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "player.loading_timeout";
            s.type = "number";
            s.description = "Seconds before the loading scene gives up waiting.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getLoadingSettings().timeout); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                double v;
                if (!readNumber(value, 0.0, 3600.0, v, error)) return false;
                if (apply) ctx.project()->getLoadingSettings().timeout = static_cast<float>(v);
                return true;
            };
            add(s);
        }
        add(boolField("player.async_loading", "Load the start scene asynchronously behind the loading scene.",
                      [](Project& p) -> bool& { return p.getLoadingSettings().asyncLoading; }));
        add(boolSetting("player.pack_native_resources", "Experimental: pack assets and Lua into resources.pak in native builds instead of copying folders.",
                        [](Project& p) { return p.shouldPackNativeResources(); }, [](Project& p, bool v) { p.setPackNativeResources(v); }));

        // ---- per platform (overrides: "" inherits the shared player value)
        add(stringSetting("web.application_name", "Web page title override.", [](Project& p) -> std::string& { return p.getWebProjectSettings().applicationName; }));
        add(pathSetting("web.favicon", "Web favicon.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().favicon; }));
        add(pathSetting("web.custom_html_shell", "Custom Emscripten HTML shell.", [](Project& p) -> fs::path& { return p.getWebProjectSettings().customHtmlShell; }));
        add(stringSetting("web.head_include", "Extra HTML put in the page <head>.", [](Project& p) -> std::string& { return p.getWebProjectSettings().headInclude; }));
        add(boolField("web.resize_canvas_to_window", "The canvas follows the browser window size.", [](Project& p) -> bool& { return p.getWebProjectSettings().resizeCanvasToWindow; }));
        add(boolField("web.hide_emscripten_ui", "Hide the Emscripten status/console UI.", [](Project& p) -> bool& { return p.getWebProjectSettings().hideEmscriptenUI; }));
        add(stringSetting("linux.application_name", "Linux .desktop name override.", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().applicationName; }));
        add(stringSetting("linux.comment", "Linux .desktop comment.", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().comment; }));
        add(stringSetting("linux.categories", "Linux .desktop categories (e.g. Game;).", [](Project& p) -> std::string& { return p.getLinuxProjectSettings().categories; }));
        add(stringSetting("windows.product_name", "Windows product name override.", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().productName; }));
        add(stringSetting("windows.company_name", "Windows company name.", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().companyName; }));
        add(stringSetting("windows.file_version", "Windows file version override (1-4 numeric parts).", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().fileVersion; },
                          [](const std::string& v) { return isNumericVersion(v, 4, 65535); }, "1 to 4 numeric parts"));
        add(stringSetting("windows.product_version", "Windows product version override (1-4 numeric parts).", [](Project& p) -> std::string& { return p.getWindowsProjectSettings().productVersion; },
                          [](const std::string& v) { return isNumericVersion(v, 4, 65535); }, "1 to 4 numeric parts"));
        add(stringSetting("macos.application_name", "macOS name override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().applicationName; }));
        add(stringSetting("macos.bundle_identifier", "macOS bundle identifier override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().bundleIdentifier; },
                          [](const std::string& v) { return isDottedIdentifier(v, appleIdPart); }, "dot-separated parts of letters, digits and hyphens"));
        add(stringSetting("macos.version_name", "macOS version override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().versionName; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts"));
        add(stringSetting("macos.build_number", "macOS build number override.", [](Project& p) -> std::string& { return p.getMacOSProjectSettings().buildNumber; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 0xFFFFFFFFull); }, "1 to 3 numeric parts"));
        add(pathSetting("macos.icon", "macOS icon.", [](Project& p) -> fs::path& { return p.getMacOSProjectSettings().icon; }));
        add(boolField("macos.high_dpi", "macOS high DPI (Retina) rendering.", [](Project& p) -> bool& { return p.getMacOSProjectSettings().highDpi; }));
        add(stringSetting("ios.application_name", "iOS name override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().applicationName; }));
        add(stringSetting("ios.bundle_identifier", "iOS bundle identifier override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().bundleIdentifier; },
                          [](const std::string& v) { return isDottedIdentifier(v, appleIdPart); }, "dot-separated parts of letters, digits and hyphens"));
        add(stringSetting("ios.version_name", "iOS version override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().versionName; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 65535); }, "1 to 3 numeric parts"));
        add(stringSetting("ios.build_number", "iOS build number override.", [](Project& p) -> std::string& { return p.getIOSProjectSettings().buildNumber; },
                          [](const std::string& v) { return isNumericVersion(v, 3, 0xFFFFFFFFull); }, "1 to 3 numeric parts"));
        add(pathSetting("ios.icon", "iOS icon.", [](Project& p) -> fs::path& { return p.getIOSProjectSettings().icon; }));
        add(boolField("ios.hide_status_bar", "Hide the iOS status bar.", [](Project& p) -> bool& { return p.getIOSProjectSettings().hideStatusBar; }));
        add(boolField("ios.hide_home_indicator", "Hide the iOS home indicator.", [](Project& p) -> bool& { return p.getIOSProjectSettings().hideHomeIndicator; }));
        add(boolField("ios.supports_high_refresh_rate", "Allow 120 Hz on ProMotion devices.", [](Project& p) -> bool& { return p.getIOSProjectSettings().supportsHighRefreshRate; }));
        add(boolField("ios.admob_enabled", "Link Google Mobile Ads into the iOS export (System.initializeAdMob and the interstitial calls); needs ios.admob_app_id.",
                      [](Project& p) -> bool& { return p.getIOSProjectSettings().admobEnabled; }));
        add(stringSetting("ios.admob_app_id", "iOS AdMob App ID (GADApplicationIdentifier) from the AdMob console; without it the export leaves AdMob out.",
                          [](Project& p) -> std::string& { return p.getIOSProjectSettings().admobAppId; },
                          isAdMobAppIdValid, "an AdMob app ID like ca-app-pub-0000000000000000~0000000000 (not an ad unit ID)"));
        add(stringSetting("android.application_name", "Android label override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().applicationName; }));
        add(stringSetting("android.package_name", "Android package name override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().packageName; },
                          [](const std::string& v) { return isDottedIdentifier(v, javaIdPart); }, "a Java package name (dot-separated identifiers)"));
        add(intField("android.version_code", "Android version code override (0 inherits player.build).", 0, 2100000000,
                     [](Project& p) -> unsigned int& { return p.getAndroidProjectSettings().versionCode; }));
        add(stringSetting("android.version_name", "Android version name override.", [](Project& p) -> std::string& { return p.getAndroidProjectSettings().versionName; }));
        add(pathSetting("android.launcher_icon", "Android launcher icon.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().launcherIcon; }));
        add(pathSetting("android.adaptive_icon_foreground", "Android adaptive icon foreground.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().adaptiveIconForeground; }));
        add(pathSetting("android.adaptive_icon_background", "Android adaptive icon background.", [](Project& p) -> fs::path& { return p.getAndroidProjectSettings().adaptiveIconBackground; }));
        {
            Setting s;
            s.key = "android.min_sdk";
            s.type = "int";
            s.description = "Android minimum SDK level.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getAndroidProjectSettings().minSdk); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 100, v, error)) return false;
                if (apply) {
                    auto& a = ctx.project()->getAndroidProjectSettings();
                    a.minSdk = static_cast<unsigned>(v);
                    a.targetSdk = std::max(a.targetSdk, a.minSdk);
                }
                return true;
            };
            add(s);
        }
        add(intField("android.target_sdk", "Android target SDK level.", 1, 100, [](Project& p) -> unsigned int& { return p.getAndroidProjectSettings().targetSdk; }));
        {
            Setting s;
            s.key = "android.orientation";
            s.type = "enum";
            s.values = enumNames(androidOrientations);
            s.description = "Android screen orientation.";
            s.get = [](McpContext& ctx) { return Json(enumName(androidOrientations, ctx.project()->getAndroidProjectSettings().orientation)); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                AndroidOrientation o;
                if (!parseEnum(androidOrientations, value, o, error)) return false;
                if (apply) ctx.project()->getAndroidProjectSettings().orientation = o;
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "android.abis";
            s.type = "list";
            s.values = {"armeabi-v7a", "arm64-v8a", "x86", "x86_64"};
            s.description = "Android ABIs to build (at least one of armeabi-v7a, arm64-v8a, x86, x86_64).";
            s.get = [](McpContext& ctx) {
                const auto& a = ctx.project()->getAndroidProjectSettings();
                Json list = Json::array();
                if (a.abiArmeabiV7a) list.push_back("armeabi-v7a");
                if (a.abiArm64V8a) list.push_back("arm64-v8a");
                if (a.abiX86) list.push_back("x86");
                if (a.abiX86_64) list.push_back("x86_64");
                return list;
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array() || value.empty()) {
                    error = "must be a non-empty list of: armeabi-v7a, arm64-v8a, x86, x86_64";
                    return false;
                }
                bool v7 = false, v8 = false, x86 = false, x64 = false;
                for (const Json& item : value) {
                    const std::string abi = item.is_string() ? item.get<std::string>() : "";
                    if (abi == "armeabi-v7a") v7 = true;
                    else if (abi == "arm64-v8a") v8 = true;
                    else if (abi == "x86") x86 = true;
                    else if (abi == "x86_64") x64 = true;
                    else {
                        error = "unknown ABI " + item.dump() + "; use armeabi-v7a, arm64-v8a, x86, x86_64";
                        return false;
                    }
                }
                if (apply) {
                    auto& a = ctx.project()->getAndroidProjectSettings();
                    a.abiArmeabiV7a = v7;
                    a.abiArm64V8a = v8;
                    a.abiX86 = x86;
                    a.abiX86_64 = x64;
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "android.permissions";
            s.type = "list";
            s.description = "Android permissions, lower-case manifest names without the prefix (internet, vibrate, camera, record_audio, access_fine_location, ...).";
            s.get = [](McpContext& ctx) {
                Json list = Json::array();
                for (const std::string& p : ctx.project()->getAndroidProjectSettings().permissions) list.push_back(p);
                return list;
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of permission names";
                    return false;
                }
                std::set<std::string> permissions;
                for (const Json& item : value) {
                    if (!item.is_string() || item.get<std::string>().empty()) {
                        error = "permissions must be non-empty strings";
                        return false;
                    }
                    permissions.insert(lowerText(item.get<std::string>()));
                }
                if (apply) ctx.project()->getAndroidProjectSettings().permissions = permissions;
                return true;
            };
            add(s);
        }
        add(boolField("android.allow_backup", "Android allowBackup.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().allowBackup; }));
        add(boolField("android.fullscreen", "Android fullscreen theme.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().fullscreen; }));
        add(boolField("android.keep_screen_on", "Keep the Android screen on.", [](Project& p) -> bool& { return p.getAndroidProjectSettings().keepScreenOn; }));
        add(boolField("android.admob_enabled", "Compile Google Mobile Ads into the Android export (System.initializeAdMob and the interstitial calls); needs android.admob_app_id.",
                      [](Project& p) -> bool& { return p.getAndroidProjectSettings().admobEnabled; }));
        add(stringSetting("android.admob_app_id", "Android AdMob App ID (com.google.android.gms.ads.APPLICATION_ID) from the AdMob console; without it the export leaves AdMob out.",
                          [](Project& p) -> std::string& { return p.getAndroidProjectSettings().admobAppId; },
                          isAdMobAppIdValid, "an AdMob app ID like ca-app-pub-0000000000000000~0000000000 (not an ad unit ID)"));

        // ---- graphics / quality
        for (const bool width : {true, false}) {
            Setting s;
            s.key = width ? "graphics.canvas_width" : "graphics.canvas_height";
            s.type = "int";
            s.description = std::string("Reference resolution ") + (width ? "width" : "height") +
                            " the game is designed for (UI and 2D coordinates), scaled to the screen by graphics.scaling_mode.";
            s.get = [width](McpContext& ctx) {
                return Json(width ? ctx.project()->getCanvasWidth() : ctx.project()->getCanvasHeight());
            };
            s.set = [width](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 16384, v, error)) return false;
                if (apply) {
                    Project* p = ctx.project();
                    if (width) p->setCanvasSize(static_cast<unsigned>(v), p->getCanvasHeight());
                    else p->setCanvasSize(p->getCanvasWidth(), static_cast<unsigned>(v));
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.scaling_mode";
            s.type = "enum";
            s.values = enumNames(scalingModes);
            s.description = "How the canvas maps to the screen.";
            s.get = [](McpContext& ctx) { return Json(enumName(scalingModes, ctx.project()->getScalingMode())); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                Scaling mode;
                if (!parseEnum(scalingModes, value, mode, error)) return false;
                if (apply) ctx.project()->setScalingMode(mode);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.texture_strategy";
            s.type = "enum";
            s.values = enumNames(textureStrategies);
            s.description = "How textures larger than the GPU limit are handled.";
            s.get = [](McpContext& ctx) { return Json(enumName(textureStrategies, ctx.project()->getTextureStrategy())); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                TextureStrategy v;
                if (!parseEnum(textureStrategies, value, v, error)) return false;
                if (apply) ctx.project()->setTextureStrategy(v);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "graphics.html_ui_msaa_samples";
            s.type = "int";
            s.description = "MSAA samples of the HTML/CSS UI documents' targets (1, 2, 4 or 8; default 4). Smoother "
                            "rounded corners and rotated edges at more GPU memory; devices without MSAA use 1.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getHtmlUiMsaaSamples()); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 1, 8, v, error)) return false;
                if (!Project::isValidHtmlUiMsaaSamples(static_cast<int>(v))) {
                    error = "must be 1, 2, 4 or 8";
                    return false;
                }
                if (apply) ctx.project()->setHtmlUiMsaaSamples(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        add(boolSetting("graphics.runtime_shaders", "Builds carry the runtime shader compiler for variants not shipped precompiled (scripted meshes/materials). Off makes builds smaller and faster to compile.",
                        [](Project& p) { return p.isRuntimeShadersEnabled(); }, [](Project& p, bool v) { p.setRuntimeShadersEnabled(v); }));
        add(boolSetting("quality.vsync", "VSync in the built player.",
                        [](Project& p) { return p.isVSyncEnabled(); }, [](Project& p, bool v) { p.setVSyncEnabled(v); }));

        // ---- physics
        add(boolSetting("physics.enabled", "3D physics (Jolt). Off leaves Rigidbodies and joints out of builds.",
                        [](Project& p) { return p.isPhysics3DEnabled(); }, [](Project& p, bool v) { p.setPhysics3DEnabled(v); }));
        add(layerCollisionSetting("physics.layer_collision_matrix", true));
        add(boolSetting("physics2d.enabled", "2D physics (Box2D). Off leaves Rigidbody2Ds and joints out of builds.",
                        [](Project& p) { return p.isPhysics2DEnabled(); }, [](Project& p, bool v) { p.setPhysics2DEnabled(v); }));
        add(layerCollisionSetting("physics2d.layer_collision_matrix", false));

        // ---- scripting
        {
            Setting s;
            s.key = "scripting.cxx_standard";
            s.type = "enum";
            for (int v : cxxStandards) s.values.push_back(std::to_string(v));
            s.description = "C++ standard for scripts and builds (" + cxxStandardList() + "). The next Play/build uses it; never edit CMAKE_CXX_STANDARD in the generated CMakeLists.txt.";
            s.get = [](McpContext& ctx) { return Json(ctx.project()->getCxxStandard()); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v = 0;
                if (value.is_string()) {
                    std::string text = value.get<std::string>();
                    if (text.rfind("c++", 0) == 0 || text.rfind("C++", 0) == 0) text = text.substr(3);
                    try { v = std::stoll(text); } catch (...) { v = 0; }
                } else if (!readInt(value, 0, 100, v, error)) {
                    return false;
                }
                if (!isSupportedCxxStandard(static_cast<int>(v))) {
                    error = "must be " + cxxStandardList();
                    return false;
                }
                if (apply) ctx.project()->setCxxStandard(static_cast<int>(v));
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "scripting.script_dirs";
            s.type = "list";
            s.description = "C++ script roots: include directories whose sources all compile. Pass the complete list; entries read "
                            "here can be passed back verbatim; a NEW entry must be an existing project-relative directory.";
            s.get = [](McpContext& ctx) {
                Json list = Json::array();
                for (const fs::path& dir : ctx.project()->getScriptDirs()) list.push_back(FileUtils::pathToGenericUtf8(dir));
                return list;
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                Project* p = ctx.project();
                if (!value.is_array()) {
                    error = "must be a list of project-relative directories";
                    return false;
                }
                const std::vector<fs::path>& configured = p->getScriptDirs();
                std::vector<fs::path> dirs;
                for (const Json& item : value) {
                    if (!item.is_string()) {
                        error = "entries must be strings";
                        return false;
                    }
                    const fs::path dir = FileUtils::pathFromUtf8(item.get<std::string>()).lexically_normal();
                    const bool known = std::any_of(configured.begin(), configured.end(),
                                                   [&dir](const fs::path& c) { return c.lexically_normal() == dir; });
                    if (!known) {
                        fs::path absolute;
                        if (!McpRefs::resolveProjectPath(p, item.get<std::string>(), absolute, error, true)) return false;
                        std::error_code ec;
                        if (!fs::is_directory(absolute, ec)) {
                            error = item.get<std::string>() + " is not a directory";
                            return false;
                        }
                    }
                    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
                }
                if (apply) p->setScriptDirs(dirs);
                return true;
            };
            add(s);
        }
        for (const bool lua : {true, false}) {
            Setting s;
            s.key = lua ? "scripting.lua_dir" : "editor.assets_dir";
            s.type = "string";
            s.description = lua
                ? "Root of Lua script references (lua://), project-relative directory (\".\" = project root). Changing it MOVES referenced files and rewrites references."
                : "Root of asset references (textures, models, sounds, fonts), project-relative directory (\".\" = project root). Changing it MOVES referenced files and rewrites references.";
            s.get = [lua](McpContext& ctx) {
                return Json(FileUtils::pathToGenericUtf8(lua ? ctx.project()->getLuaDir() : ctx.project()->getAssetsDir()));
            };
            s.set = [lua](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                Project* p = ctx.project();
                std::string text;
                if (!readString(value, text, error)) return false;
                // a plain project-relative folder: no absolute path, no ".." (even one that comes back in)
                if (const std::string why = rootValueRefusal(text); !why.empty()) {
                    error = why + ": an asset root is a folder of the project, given project-relative without '..'";
                    return false;
                }
                fs::path dir = text.empty() ? fs::path(".") : FileUtils::pathFromUtf8(text).lexically_normal();
                if (dir != ".") {
                    fs::path absolute;
                    // the referenced files are moved there, and it becomes a trusted root: inside the
                    // project directory itself once links are resolved (never via the current
                    // assets directory's link)
                    if (!McpRefs::resolveProjectPath(p, text, absolute, error)) return false;
                    if (const std::string escape = McpRefs::projectOnlyEscape(p, absolute); !escape.empty()) {
                        error = escape;
                        return false;
                    }
                }
                if (ctx.isPlaying()) {
                    error = "cannot change asset roots during Play";
                    return false;
                }
                if (apply) {
                    std::error_code ec;
                    FileUtils::createDirectories(p->getProjectPath() / dir, ec);
                    if (lua) p->changeAssetRoots(p->getAssetsDir(), dir);
                    else p->changeAssetRoots(dir, p->getLuaDir());
                }
                return true;
            };
            add(s);
        }
        add(boolSetting("editor.version_control_files", "Write version-control metadata files (.gitignore etc.) with the project.",
                        [](Project& p) { return p.hasVersionControlMetadata(); }, [](Project& p, bool v) { p.setVersionControlMetadata(v); }));

        // ---- tags and layers
        {
            Setting s;
            s.key = "tags_and_layers.tags";
            s.type = "list";
            s.description = "User tags (after the built-in Untagged, Respawn, Finish, EditorOnly, MainCamera, Player, GameController). Pass the complete list.";
            s.get = [](McpContext& ctx) {
                Json list = Json::array();
                for (const std::string& tag : ctx.project()->getTags()) list.push_back(tag);
                return list;
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_array()) {
                    error = "must be a list of tag names";
                    return false;
                }
                std::vector<std::string> tags;
                for (const Json& item : value) {
                    if (!item.is_string()) {
                        error = "tags must be strings";
                        return false;
                    }
                    const std::string tag = TagManager::trimName(item.get<std::string>());
                    if (tag.empty()) {
                        error = "tags must not be empty";
                        return false;
                    }
                    if (TagManager::isBuiltinTag(tag)) {
                        error = "\"" + tag + "\" is a built-in tag";
                        return false;
                    }
                    if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
                        error = "\"" + tag + "\" is listed twice";
                        return false;
                    }
                    tags.push_back(tag);
                }
                if (apply) ctx.project()->setTagsAndLayers(tags, ctx.project()->getLayerNames());
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "tags_and_layers.layers";
            s.type = "object";
            s.description = "User layer names by index, {\"3\": \"Player\", \"8\": \"Enemies\"} (user layers are 3 and 6-31; "
                            "0 Default, 1 TransparentFX, 2 Ignore Raycast, 4 Water, 5 UI are built-in). Setting merges: a "
                            "listed index gets that name, \"\" unnames it, unlisted ones keep theirs. Reading returns every named layer, built-ins included.";
            s.get = [](McpContext& ctx) {
                Json map = Json::object();
                const std::vector<std::string>& names = ctx.project()->getLayerNames();
                for (int i = 0; i < MAX_LAYERS; i++) {
                    std::string name = TagManager::isBuiltinLayer(i) ? TagManager::getBuiltinLayerName(i)
                                                                     : (i < static_cast<int>(names.size()) ? names[i] : std::string());
                    if (!name.empty()) map[std::to_string(i)] = name;
                }
                return map;
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                if (!value.is_object()) {
                    error = "must be an object {\"index\": \"name\"}";
                    return false;
                }
                std::vector<std::string> names = ctx.project()->getLayerNames();
                names.resize(MAX_LAYERS);
                for (auto it = value.begin(); it != value.end(); ++it) {
                    int index = -1;
                    try { index = std::stoi(it.key()); } catch (...) { index = -1; }
                    if (index < 0 || index >= MAX_LAYERS || std::to_string(index) != it.key()) {
                        error = "layer key \"" + it.key() + "\" must be an index 0-31";
                        return false;
                    }
                    if (TagManager::isBuiltinLayer(index)) {
                        error = "layer " + it.key() + " is built-in (" + TagManager::getBuiltinLayerName(index) + ")";
                        return false;
                    }
                    if (!it.value().is_string()) {
                        error = "layer names must be strings";
                        return false;
                    }
                    names[index] = TagManager::trimName(it.value().get<std::string>());
                }
                std::set<std::string> seen;
                for (int i = 0; i < MAX_LAYERS; i++) {
                    const std::string name = TagManager::isBuiltinLayer(i) ? TagManager::getBuiltinLayerName(i) : names[i];
                    if (name.empty()) continue;
                    if (!seen.insert(name).second) {
                        error = "two layers are named \"" + name + "\"";
                        return false;
                    }
                }
                if (apply) ctx.project()->setTagsAndLayers(ctx.project()->getTags(), names);
                return true;
            };
            add(s);
        }

        // ---- editor preferences (this machine / this editor session)
        {
            Setting s;
            s.key = "preferences.clear_on_play";
            s.type = "bool";
            s.preference = true;
            s.description = "Console: clear the log when Play starts (editor session).";
            s.get = [](McpContext& ctx) { return ctx.consoleWindow() ? Json(ctx.consoleWindow()->isClearOnPlay()) : Json(nullptr); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                if (!ctx.consoleWindow()) {
                    error = "no Console window";
                    return false;
                }
                if (apply) ctx.consoleWindow()->setClearOnPlay(b);
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.error_pause";
            s.type = "bool";
            s.preference = true;
            s.description = "Console: pause Play when an error is logged (editor session). Usually off for agents.";
            s.get = [](McpContext& ctx) { return ctx.consoleWindow() ? Json(ctx.consoleWindow()->isErrorPause()) : Json(nullptr); };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                bool b;
                if (!readBool(value, b, error)) return false;
                if (!ctx.consoleWindow()) {
                    error = "no Console window";
                    return false;
                }
                if (apply) ctx.consoleWindow()->setErrorPause(b);
                return true;
            };
            add(s);
        }
        {
            Setting s = boolSetting("preferences.simulate_touch", "Play acts as a touch device: the mouse is one finger and isTouchDevice() is true.",
                                    [](Project& p) { return p.isSimulateTouch(); }, [](Project& p, bool v) { p.setSimulateTouch(v); });
            s.preference = true;
            add(s);
        }
        auto stringPreference = [&add](const std::string& key, const std::string& description,
                                       std::function<std::string()> get, std::function<void(const std::string&)> set) {
            Setting s;
            s.key = key;
            s.type = "string";
            s.preference = true;
            s.userOnly = true;
            s.description = description;
            s.get = [get](McpContext&) { return Json(get()); };
            s.set = [set](McpContext&, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                if (apply) set(text);
                return true;
            };
            add(s);
        };
        stringPreference("preferences.cmake_path", "External Tools: cmake executable (\"\" = from PATH).",
                         [] { return AppSettings::getCMakePath(); }, [](const std::string& v) { AppSettings::setCMakePath(v); });
        stringPreference("preferences.emsdk_path", "External Tools: Emscripten SDK root for Web builds (\"\" = EMSDK / PATH).",
                         [] { return AppSettings::getEmsdkPath(); }, [](const std::string& v) { AppSettings::setEmsdkPath(v); });
        stringPreference("preferences.ffmpeg_path", "External Tools: ffmpeg for video imports (\"\" = JASPER_FFMPEG / PATH).",
                         [] { return AppSettings::getFfmpegPath(); }, [](const std::string& v) { AppSettings::setFfmpegPath(v); });
        stringPreference("preferences.default_export_dir", "Default output directory for builds of projects that have none yet.",
                         [] { return FileUtils::pathToGenericUtf8(AppSettings::getDefaultExportDirectory()); },
                         [](const std::string& v) { AppSettings::setDefaultExportDirectory(FileUtils::pathFromUtf8(v)); });
        // Compiler kit and jobs, per project (.jasperengine/user/build.yaml)
        struct KitField {
            const char* key;
            const char* description;
            std::string LocalBuildSettings::* member;
        };
        static const KitField kitFields[] = {
            {"preferences.build_c_compiler", "C compiler for Desktop builds of this project (\"\" = CMake default).", &LocalBuildSettings::cCompiler},
            {"preferences.build_cxx_compiler", "C++ compiler for Desktop builds of this project (\"\" = CMake default).", &LocalBuildSettings::cxxCompiler},
            {"preferences.build_generator", "CMake generator for Desktop builds of this project, e.g. \"Ninja\" or \"Unix Makefiles\" (\"\" = CMake default).", &LocalBuildSettings::generator},
        };
        for (const KitField& field : kitFields) {
            Setting s;
            s.key = field.key;
            s.type = "string";
            s.preference = true;
            // the compilers are programs the build runs: user-only (the generator is just a name)
            s.userOnly = field.member != &LocalBuildSettings::generator;
            s.description = field.description;
            auto member = field.member;
            s.get = [member](McpContext& ctx) {
                return Json(AppSettings::getBuildSettings(ctx.project()->getProjectPath() / "project.yaml").*member);
            };
            s.set = [member](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                std::string text;
                if (!readString(value, text, error)) return false;
                // the generator is a name CMake must recognise (it drives the build command): an
                // arbitrary string is refused, never stored
                if (member == &LocalBuildSettings::generator && !text.empty() && !Generator::isKnownGenerator(text)) {
                    error = "not a CMake generator name (e.g. \"Ninja\", \"Unix Makefiles\", \"Visual Studio 17 2022\")";
                    return false;
                }
                if (apply) {
                    const fs::path file = ctx.project()->getProjectPath() / "project.yaml";
                    LocalBuildSettings build = AppSettings::getBuildSettings(file);
                    build.*member = text;
                    if (!AppSettings::setBuildSettings(file, build)) {
                        error = "could not write the local build settings";
                        return false;
                    }
                }
                return true;
            };
            add(s);
        }
        {
            Setting s;
            s.key = "preferences.build_jobs";
            s.type = "int";
            s.preference = true;
            s.description = "Parallel compile jobs for Desktop builds of this project (0 = automatic).";
            s.get = [](McpContext& ctx) {
                return Json(AppSettings::getBuildSettings(ctx.project()->getProjectPath() / "project.yaml").buildJobs);
            };
            s.set = [](McpContext& ctx, const Json& value, bool apply, std::string& error) {
                long long v;
                if (!readInt(value, 0, 1024, v, error)) return false;
                if (apply) {
                    const fs::path file = ctx.project()->getProjectPath() / "project.yaml";
                    LocalBuildSettings build = AppSettings::getBuildSettings(file);
                    build.buildJobs = static_cast<unsigned>(v);
                    if (!AppSettings::setBuildSettings(file, build)) {
                        error = "could not write the local build settings";
                        return false;
                    }
                }
                return true;
            };
            add(s);
        }
        return t;
    }();
    return table;
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
    const Json& args = call->args();
    const bool describe = args.value("describe", false);

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
        values[s->key] = s->get(ctx);
        if (describe) {
            Json info = {{"type", s->type}, {"description", s->description}};
            if (!s->values.empty()) info["values"] = s->values;
            if (s->preference) info["preference"] = true;
            if (s->userOnly) info["read_only"] = true;
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
        if (s->userOnly) {
            call->fail(s->key + " is a machine-level preference only the user changes: change it in Edit > Preferences. "
                       "Nothing was changed.");
            return;
        }
        std::string error;
        if (!s->set(ctx, it.value(), false, error)) {
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
        if (!setting->set(ctx, value, true, error)) {
            call->fail(setting->key + ": " + error + ". Applied before it: " + applied.dump() + ".");
            return;
        }
        applied[setting->key] = setting->get(ctx);
        if (!setting->preference) projectChanged = true;
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
        if (!pathText.empty() && FileUtils::pathFromUtf8(pathText).extension() == ".scene") {
            fs::path absolute;
            if (!McpRefs::resolveProjectPath(project, pathText, absolute, error, true)) {
                std::string files;
                for (const fs::path& file : project->findSceneFiles()) {
                    if (project->findSceneByPath(file) == NULL_PROJECT_SCENE) files += " " + relText(project, file);
                }
                call->fail(error + (files.empty() ? " Every .scene file is listed already." : " Unlisted .scene files:" + files + "."));
                return;
            }
            const fs::path relative = FileUtils::pathFromUtf8(pathText).lexically_normal();
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
    // written (or removed) beneath the project: a link there leading out of it is refused, and no
    // link planted on the way since the check is followed
    std::string why;
    if (const std::string escape = McpRefs::writeEscape(project, path, content.empty() ? McpRefs::WriteUse::Entry : McpRefs::WriteUse::Write);
        !escape.empty()) {
        call->fail(escape);
        return;
    }
    if (content.empty()) {
        if (!safewrite::removeFileResolvedBeneath(McpRefs::rootFor(project, path, McpRefs::WriteUse::Entry), path, why)) {
            call->fail("Could not remove ProjectBuild.cmake: " + why);
            return;
        }
    } else if (!safewrite::writeFileResolvedBeneath(McpRefs::rootFor(project, path), path, content, why)) {
        call->fail("Could not write ProjectBuild.cmake: " + why);
        return;
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
        "cancel: fail listing them.",
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
            {"untitled_scene_dir", withDefault(string(""), "")}
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
            {"describe", withDefault(boolean(""), false)}
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
            "Set get_project_settings keys. Every value is validated first (one bad value changes nothing); "
            "saved to project.yaml at once, NOT undoable. Lists are replaced; tags_and_layers.layers merges "
            "by index. Example: {\"values\": {\"player.window_width\": 800}}.";
        tool.inputSchema = object({
            {"values", anyObject("")}
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
            "libraries and definitions; content \"\" deletes it. Use ${JASPERENGINE_TARGET} and "
            "${JASPERENGINE_SCRIPTS_DIR}. Not undoable.";
        tool.inputSchema = object({
            {"content", string("Whole new file.")}
        });
        tool.annotations.destructive = true;
        tool.handler = editProjectBuildFileTool;
        registry.addTool(std::move(tool));
    }
}

}
