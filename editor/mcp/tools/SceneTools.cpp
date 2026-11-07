// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W1: scenes (list, create, open, close, select, save, rename, delete), child scenes, main
// camera, scene settings
#include "McpTools.h"
#include "shader/ShaderGlobals.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpDataSafety.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "PostProcessingPresets.h"
#include "Project.h"
#include "StreamPreserved.h"
#include "command/CommandNames.h"
#include "command/type/AddChildSceneCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "command/type/SetChildSceneStartActiveCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "util/FileUtils.h"
#include "util/CubeLut.h"
#include "util/Util.h"
#include "shader/ShaderManifest.h"
#include "window/ProjectWindow.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

// ------------------------------------------------------------------ helpers

SceneProject* sceneArg(const CallPtr& call, const char* key = "scene") {
    std::string error;
    const Json& args = call->args();
    SceneProject* scene = McpRefs::resolveScene(call->ctx().project(), args.contains(key) ? args[key] : Json(), error);
    if (!scene) call->fail(error);
    return scene;
}

bool parseUnsaved(const CallPtr& call, UnsavedChoice& out) {
    const std::string value = call->args().value("unsaved", std::string("cancel"));
    if (value == "cancel") out = UnsavedChoice::Cancel;
    else if (value == "save") out = UnsavedChoice::Save;
    else if (value == "discard") out = UnsavedChoice::Discard;
    else {
        call->fail("unsaved must be cancel (fail when there are unsaved changes), save or discard.");
        return false;
    }
    return true;
}

Json unsavedSchema() {
    return withDefault(stringEnum("cancel: fail.", {"cancel", "save", "discard"}),
                       "cancel");
}

// set_main_camera's entity: null = the default camera
Json nullableEntityRef(const std::string& description = {}) {
    Json ref = entityRef(description);
    ref["type"] = Json::array({"string", "null"});
    return ref;
}

// A project-relative .scene path for a new file (".scene" added when missing)
bool scenePathArg(Project* project, const std::string& given, fs::path& relative, std::string& error) {
    fs::path rel = FileUtils::pathFromUtf8(given).lexically_normal();
    if (rel.extension() != ".scene") rel += ".scene";
    fs::path absolute;
    // a file the tool writes: inside the project once links are resolved
    if (!McpRefs::resolveProjectWritePath(project, FileUtils::pathToGenericUtf8(rel), absolute, error)) return false;
    relative = rel;
    return true;
}

Json childScenesJson(Project* project, const SceneProject& scene) {
    Json list = Json::array();
    for (const ChildSceneRef& child : scene.childScenes) {
        const SceneProject* childScene = project->getScene(child.id);
        list.push_back({
            {"id", child.id},
            {"name", childScene ? childScene->name : std::string()},
            {"start_active", child.startActive},
            {"loaded_inline", childScene ? childScene->expandedInline : false}
        });
    }
    return list;
}

Json sceneInfo(Project* project, SceneProject& scene) {
    Json item = McpRefs::sceneJson(project, scene);
    item["selected"] = scene.id == project->getSelectedSceneId();
    item["start_scene"] = scene.id == project->getStartSceneId();
    if (project->isPrefabStage(scene.id)) item["prefab_stage"] = true;
    if (scene.scene) {
        item["entity_count"] = scene.entities.size();
        item["main_camera"] = McpValues::entityToJson(&scene, scene.mainCamera);
    }
    if (!scene.childScenes.empty()) item["child_scenes"] = childScenesJson(project, scene);
    // what its file's load could not read (docs/data-safety.md); a scene with dropped values
    // is saved only with allow_data_loss
    if (scene.scene) item["degraded"] = degradedJson(scene);
    const uint32_t runtime = project->getPlayRuntimeSceneId(scene.id);
    if (runtime != NULL_PROJECT_SCENE && runtime != scene.id) item["runtime_scene_id"] = runtime;
    return item;
}

// ------------------------------------------------------------------ list / create / open / close / select

void listScenesTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    Json scenes = Json::array();
    std::string text;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        Json item = sceneInfo(project, *scene);
        text += "\n" + std::to_string(scene->id) + " \"" + scene->name + "\" " + McpRefs::sceneTypeName(scene->sceneType) +
                (scene->filepath.empty() ? " (untitled)" : " " + item["path"].get<std::string>()) +
                (scene->opened ? " open" : " closed") + (scene->isModified ? " modified" : "") +
                (item["selected"].get<bool>() ? " selected" : "") + (item["start_scene"].get<bool>() ? " start" : "") +
                (scene->playState != ScenePlayState::STOPPED ? " " + McpRefs::playStateName(scene->playState) : "");
        scenes.push_back(item);
    }
    Json unresolved = Json::array();
    for (const auto& entry : project->getUnresolvedScenes()) unresolved.push_back(FileUtils::pathToGenericUtf8(entry.second));
    Json data = {{"scenes", scenes}, {"selected_scene_id", project->getSelectedSceneId()}};
    if (!unresolved.empty()) data["unresolved_scene_files"] = unresolved;
    call->ok(std::to_string(scenes.size()) + " scene(s):" + text, data);
}

void createSceneTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneType type = SceneType::SCENE_3D;
    if (args.contains("type") && !McpRefs::parseSceneType(args.value("type", std::string()), type)) {
        call->fail("type must be 3D, 2D or UI.");
        return;
    }
    UnsavedChoice unsaved;
    if (!parseUnsaved(call, unsaved)) return;
    fs::path path;
    std::string error;
    if (args.contains("path") && !args["path"].get<std::string>().empty()) {
        if (!scenePathArg(project, args["path"].get<std::string>(), path, error)) {
            call->fail(error);
            return;
        }
        if (fs::exists(project->getProjectPath() / path)) {
            call->fail("\"" + FileUtils::pathToGenericUtf8(path) + "\" exists already; open it with open_scene, or pick another path.");
            return;
        }
    }
    const uint32_t id = project->createSceneNow(args.value("name", std::string()), type, path,
                                                args.value("close_previous", false), unsaved, &error);
    SceneProject* scene = id != NULL_PROJECT_SCENE ? project->getScene(id) : nullptr;
    if (!scene) {
        call->fail(error.empty() ? "The scene could not be created." : error);
        return;
    }
    Json data = {{"scene", sceneInfo(project, *scene)}};
    if (!error.empty()) {
        data["warning"] = error;
        call->ok("Created scene " + std::to_string(id) + " \"" + scene->name + "\" but: " + error, data);
        return;
    }
    call->ok("Created scene " + std::to_string(id) + " \"" + scene->name + "\" (" + McpRefs::sceneTypeName(type) + ")" +
             (scene->filepath.empty() ? ", not saved yet (save_scene with a path)." : " at " + data["scene"]["path"].get<std::string>() + "."),
             data);
}

// recovery: {available, autosaved_at, status, applied} of the scene just opened
Json recoveryJson(const RecoveryOutcome& outcome, const char* applied) {
    const bool usable = outcome.checked && outcome.backup.status == BackupStatus::Usable;
    return {{"available", usable},
            {"autosaved_at", outcome.backup.savedAt},
            {"status", outcome.checked ? backupStatusName(outcome.backup.status) : "none"},
            {"applied", usable ? applied : "none"}};
}

const char* recoveryName(RecoveryChoice choice) {
    switch (choice) {
        case RecoveryChoice::Restore: return "restore";
        case RecoveryChoice::Discard: return "discard";
        case RecoveryChoice::Keep: return "keep";
    }
    return "keep";
}

void openSceneTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    UnsavedChoice unsaved;
    if (!parseUnsaved(call, unsaved)) return;
    // a crash backup of the scene: decided here, never by a modal (R36)
    const std::string recoveryArg = args.value("recovery", std::string("keep"));
    RecoveryChoice recovery = RecoveryChoice::Keep;
    if (recoveryArg == "restore") recovery = RecoveryChoice::Restore;
    else if (recoveryArg == "discard") recovery = RecoveryChoice::Discard;
    else if (recoveryArg != "keep") {
        call->fail("recovery is keep, restore or discard.");
        return;
    }
    std::string given;
    if (args.contains("scene")) {
        if (args["scene"].is_string()) given = args["scene"].get<std::string>();
        std::string error;
        if (SceneProject* known = McpRefs::resolveScene(project, args["scene"], error)) {
            if (known->opened && known->scene) {
                // open already: its backup can still be restored or discarded now
                RecoveryOutcome outcome;
                const uint32_t knownId = known->id;
                if (!known->filepath.empty()) {
                    outcome.checked = true;
                    outcome.backup = SceneBackups::scan(project->getProjectPath(), project->normalizeToProjectRelative(known->filepath));
                    outcome.applied = recovery;
                    if (outcome.backup.status == BackupStatus::Usable) {
                        if (recovery == RecoveryChoice::Restore && !project->restoreSceneFromBackup(knownId)) {
                            call->fail("The auto-saved copy of the scene could not be restored (see the console).");
                            return;
                        }
                        if (recovery == RecoveryChoice::Discard) project->discardSceneBackup(knownId);
                    }
                }
                SceneProject* current = project->getScene(project->findSceneByPath(project->getProjectPath() / outcome.backup.source));
                if (!current) current = project->getScene(knownId);
                if (!current) {
                    call->fail("The scene is gone.");
                    return;
                }
                project->setSelectedSceneId(current->id);
                call->ok("Scene " + std::to_string(current->id) + " \"" + current->name + "\" is open; selected it.",
                         Json{{"scene", sceneInfo(project, *current)}, {"recovery", recoveryJson(outcome, recoveryName(outcome.applied))},
                              {"load_report", loadReportSummary(current->loadReport, findPreservedStore(current->scene))}});
                return;
            }
            if (known->filepath.empty()) {
                call->fail("Scene \"" + known->name + "\" has no file.");
                return;
            }
            given = FileUtils::pathToGenericUtf8(known->filepath);
        } else if (given.empty() || !Util::isSceneFile(given)) {
            call->fail(error + " To open a file, pass its project-relative .scene path.");
            return;
        }
    }
    if (given.empty()) {
        call->fail("Pass scene: a project-relative .scene path, or the id/name of a closed project scene.");
        return;
    }
    fs::path absolute;
    std::string error;
    if (!McpRefs::resolveProjectPath(project, given, absolute, error, true)) {
        call->fail(error);
        return;
    }
    if (!Util::isSceneFile(absolute.string())) {
        call->fail("\"" + given + "\" is not a .scene file.");
        return;
    }
    project->setNextRecoveryChoice(recovery);
    const uint32_t id = project->openSceneNow(given, args.value("close_previous", false), unsaved, &error);
    project->setNextRecoveryChoice(RecoveryChoice::Keep);
    SceneProject* scene = id != NULL_PROJECT_SCENE ? project->getScene(id) : nullptr;
    if (!scene) {
        call->fail(error.empty() ? "The scene could not be opened." : error);
        return;
    }
    const RecoveryOutcome& outcome = project->getLastRecovery();
    std::string note;
    if (outcome.checked && outcome.backup.status == BackupStatus::Usable) {
        note = outcome.applied == RecoveryChoice::Restore ? " Restored its auto-saved changes (modified; save to keep them)." :
               outcome.applied == RecoveryChoice::Discard ? " Discarded its auto-saved changes." :
               " It has auto-saved changes from a session that did not close normally (" + outcome.backup.savedAt +
               "): open_scene recovery=restore or discard.";
    }
    const Json report = loadReportSummary(scene->loadReport, findPreservedStore(scene->scene));
    if (report["skipped"].get<size_t>() > 0) {
        note += " It did not load completely: get_load_report lists what was kept and dropped.";
    }
    call->ok("Opened scene " + std::to_string(id) + " \"" + scene->name + "\" and selected it." + note,
             Json{{"scene", sceneInfo(project, *scene)}, {"recovery", recoveryJson(outcome, recoveryName(outcome.applied))},
                  {"load_report", report}});
}

void closeSceneTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    UnsavedChoice unsaved;
    if (!parseUnsaved(call, unsaved)) return;
    const uint32_t id = scene->id;
    const std::string name = scene->name;
    std::string error;
    if (!project->closeSceneNow(id, unsaved, &error)) {
        call->fail(error.empty() ? "The scene could not be closed." : error);
        return;
    }
    call->ok("Closed scene " + std::to_string(id) + " \"" + name + "\".",
             Json{{"closed_scene_id", id}, {"selected_scene_id", project->getSelectedSceneId()}});
}

void selectSceneTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->opened || !scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *scene));
        return;
    }
    project->setSelectedSceneId(scene->id);
    // its tab takes the focus too: a focused Scene view selects its own scene every frame
    if (SceneWindow* window = call->ctx().sceneWindow()) window->requestPlayFocus(scene->id);
    call->ok("Selected scene " + std::to_string(scene->id) + " \"" + scene->name + "\".", Json{{"scene", sceneInfo(project, *scene)}});
}

// ------------------------------------------------------------------ save / rename / delete

void saveSceneTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    // a scene that did not load completely is written only when the agent accepts the loss
    const DegradedSavePolicy policy = args.value("allow_data_loss", false) ? DegradedSavePolicy::SaveAnyway : DegradedSavePolicy::Refuse;

    if (args.value("all", false)) {
        Json saved = Json::array();
        Json skipped = Json::array();
        Json failed = Json::array();
        Json refused = Json::array();
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (!scene->scene || !project->hasSceneUnsavedChanges(scene->id)) continue;
            if (scene->filepath.empty() && !project->isPrefabStage(scene->id)) {
                skipped.push_back(Json{{"id", scene->id}, {"name", scene->name}, {"reason", "never saved: save_scene with a path"}});
                continue;
            }
            const uint32_t id = scene->id;
            const std::string name = scene->name;
            SaveResult result = SaveResult::Failed;
            if (project->saveSceneNow(id, {}, &error, policy, &result)) {
                SceneProject* after = project->getScene(id);
                saved.push_back(Json{{"id", id}, {"name", name}, {"degraded", after ? degradedJson(*after) : Json(nullptr)}});
            } else if (result == SaveResult::RefusedDegraded) {
                SceneProject* after = project->getScene(id);
                Json item = after ? degradedDocumentError(after->filepath.generic_string(), after->loadReport) : Json::object();
                item["id"] = id;
                item["name"] = name;
                refused.push_back(item);
            } else {
                failed.push_back(Json{{"id", id}, {"name", name}, {"error", error}});
            }
        }
        Json data = {{"saved", saved}, {"skipped", skipped}, {"failed", failed}, {"refused", refused}};
        const std::string text = "Saved " + std::to_string(saved.size()) + " scene(s)" +
            (skipped.empty() ? std::string() : ", skipped " + std::to_string(skipped.size()) + " untitled") +
            (failed.empty() ? std::string() : ", " + std::to_string(failed.size()) + " failed") +
            (refused.empty() ? std::string() : ", refused " + std::to_string(refused.size()) +
                " that did not load completely (allow_data_loss=true writes them anyway)") + ".";
        if (!refused.empty()) data["code"] = "degraded_document";
        if (!failed.empty() || !refused.empty()) call->fail(text, data);
        else call->ok(text, data);
        return;
    }

    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    const uint32_t id = scene->id;
    const std::string pathArg = args.value("path", std::string());
    if (!pathArg.empty() && !scene->filepath.empty() && !project->isPrefabStage(id)) {
        fs::path relative;
        if (!scenePathArg(project, pathArg, relative, error)) {
            call->fail(error);
            return;
        }
        const fs::path current = project->normalizeToProjectRelative(scene->filepath).lexically_normal();
        if (relative != current) {
            // Save As: the scene now lives in the new file (the old one stays on disk, and with
            // it whatever this editor could not read: never gated)
            if (scene->playState != ScenePlayState::STOPPED) {
                call->fail("Cannot save a scene while it plays.");
                return;
            }
            const uint32_t owner = project->findSceneByPath(relative);
            if (owner != NULL_PROJECT_SCENE && owner != id) {
                call->fail("Scene " + std::to_string(owner) + " of the project is saved at " + FileUtils::pathToGenericUtf8(relative) + ".");
                return;
            }
            std::error_code ec;
            FileUtils::createDirectories((project->getProjectPath() / relative).parent_path(), ec);
            if (!project->saveSceneToPath(id, relative)) {
                call->fail("Failed to save the scene to " + FileUtils::pathToGenericUtf8(relative) + " (see the console).");
                return;
            }
            if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
            SceneProject* saved = project->getScene(id);
            call->ok("Saved scene " + std::to_string(id) + " as " + FileUtils::pathToGenericUtf8(relative) + ".",
                     Json{{"scene", sceneInfo(project, *saved)}, {"degraded", degradedJson(*saved)}});
            return;
        }
    }
    // saved in place: its file (a link out of the project would be written through) stays inside
    if (!scene->filepath.empty()) {
        const fs::path file = scene->filepath.is_absolute() ? scene->filepath : project->getProjectPath() / scene->filepath;
        if (const std::string escape = McpRefs::writeEscape(project, file); !escape.empty()) {
            call->fail(escape);
            return;
        }
    }
    fs::path untitledPath;
    if (!pathArg.empty() && scene->filepath.empty()) {
        if (!scenePathArg(project, pathArg, untitledPath, error)) {
            call->fail(error);
            return;
        }
    }
    SaveResult result = SaveResult::Failed;
    if (!project->saveSceneNow(id, untitledPath, &error, policy, &result)) {
        SceneProject* refused = project->getScene(id);
        if (result == SaveResult::RefusedDegraded && refused) {
            if (project->isPrefabStage(id)) {
                const PrefabStage* stage = project->getPrefabStage(id);
                const Prefab* prefab = stage ? project->getPrefab(stage->assetPath) : nullptr;
                if (prefab) {
                    failDegraded(*call, stage->assetPath.generic_string(), prefab->loadReport);
                    return;
                }
            }
            failDegraded(*call, refused->filepath.generic_string(), refused->loadReport);
            return;
        }
        call->fail(error.empty() ? "The scene could not be saved." : error);
        return;
    }
    if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
    SceneProject* saved = project->getScene(id);
    if (!saved) {
        call->ok("Saved.", Json{{"scene_id", id}});
        return;
    }
    call->ok("Saved scene " + std::to_string(id) + " \"" + saved->name + "\"" +
             (saved->filepath.empty() ? "." : " to " + FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(saved->filepath)) + "."),
             Json{{"scene", sceneInfo(project, *saved)}, {"degraded", degradedJson(*saved)}});
}

// ------------------------------------------------------------------ load reports / backups

void getLoadReportTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    const PreservedStore* store = findPreservedStore(scene->scene);
    Json report = loadReportJson(scene->loadReport, store);
    report["scene_id"] = scene->id;
    std::string text = "Scene " + std::to_string(scene->id) + " \"" + scene->name + "\": ";
    const size_t preserved = report["preserved"].get<size_t>();
    const size_t dropped = report["dropped"].get<size_t>();
    if (preserved == 0 && dropped == 0) {
        text += "loaded completely.";
    } else {
        text += std::to_string(preserved) + " value(s) this editor does not read are kept in the file, " + std::to_string(dropped) +
                " would be lost by a save" + (dropped > 0 ? " (save_scene refuses without allow_data_loss)." : ".");
        for (const LoadIssue* issue : scene->loadReport.outstanding(store)) {
            text += "\n" + issue->consoleLine();
        }
    }
    (void)project;
    call->ok(text, report);
}

void listSceneBackupsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    Json backups = Json::array();
    std::string text;
    for (const SceneBackupInfo& backup : project->listSceneBackups()) {
        Json item = backupJson(project, backup);
        text += "\n" + item["source"].get<std::string>() + " -> " + item["backup"].get<std::string>() + " (" +
                item["status"].get<std::string>() + ", auto-saved " + backup.savedAt + ")";
        backups.push_back(item);
    }
    call->ok(backups.empty() ? "No auto-saved backups." : std::to_string(backups.size()) + " backup(s):" + text, Json{{"backups", backups}});
}

void discardSceneBackupTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string error;
    if (SceneProject* scene = McpRefs::resolveScene(project, args.value("scene", Json()), error)) {
        if (project->discardSceneBackup(scene->id)) {
            call->ok("Deleted the auto-saved backup of scene " + std::to_string(scene->id) + " \"" + scene->name + "\".",
                     Json{{"scene_id", scene->id}, {"discarded", true}});
        } else {
            call->ok("Scene \"" + scene->name + "\" has no auto-saved backup.", Json{{"scene_id", scene->id}, {"discarded", false}});
        }
        return;
    }
    const std::string given = args.value("scene", Json()).is_string() ? args["scene"].get<std::string>() : std::string();
    if (!given.empty() && Util::isSceneFile(given)) {
        const bool removed = project->discardSceneBackup(fs::path(given));
        call->ok(removed ? "Deleted the auto-saved backup of " + given + "." : given + " has no auto-saved backup.",
                 Json{{"scene", given}, {"discarded", removed}});
        return;
    }
    call->fail(error.empty() ? "Pass scene: a scene id, name or .scene path." : error);
}

void renameSceneTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    const std::string name = call->args().value("name", std::string());
    if (name.empty()) {
        call->fail("name must not be empty.");
        return;
    }
    for (const SceneProject& other : project->getScenes()) {
        if (other.id != scene->id && other.name == name) {
            call->fail("Scene " + std::to_string(other.id) + " is named \"" + name + "\" already: scene names must be unique "
                       "(they name the generated code).");
            return;
        }
    }
    const uint32_t id = scene->id;
    std::string error;
    if (!runSceneCommand(id, new SceneNameCmd(project, id, name), error)) {
        call->fail(error);
        return;
    }
    call->ok("Renamed scene " + std::to_string(id) + " to \"" + name + "\" (the file keeps its name; save_scene to keep it).",
             Json{{"scene", sceneInfo(project, *project->getScene(id))}});
}

// Removes a saved scene from the project (its file stays), undoable from the project history
class RemoveProjectSceneCmd : public Command {
public:
    RemoveProjectSceneCmd(Project* project, uint32_t sceneId): project(project), sceneId(sceneId) {}

    bool execute() override {
        if (!project || project->isAnyScenePlaying() || project->getScenes().size() <= 1) return false;
        uint32_t target = sceneId;
        SceneProject* sceneProject = project->getScene(target);
        if (!sceneProject && !scenePath.empty()) {
            target = project->findSceneByPath(scenePath);
            sceneProject = project->getScene(target);
        }
        if (!sceneProject || sceneProject->filepath.empty() || project->hasSceneUnsavedChanges(sceneProject->id)) return false;
        sceneId = target;
        scenePath = sceneProject->filepath.lexically_normal();
        wasOpened = sceneProject->opened;
        previousSelected = project->getSelectedSceneId();
        project->removeScene(sceneId);
        if (project->getScene(sceneId) || project->findSceneByPath(scenePath) != NULL_PROJECT_SCENE) return false;
        if (!project->saveProjectFile()) {
            restore();
            return false;
        }
        removed = true;
        return true;
    }

    void undo() override {
        if (removed) restore();
    }

    std::string describe() const override { return "Remove Scene " + CommandNames::file(scenePath); }
    const char* typeName() const override { return "RemoveProjectSceneCmd"; }

private:
    void restore() {
        if (scenePath.empty()) return;
        if (project->findSceneByPath(scenePath) == NULL_PROJECT_SCENE) {
            project->loadScene(scenePath, wasOpened, true, wasOpened);
        }
        const uint32_t restored = project->findSceneByPath(scenePath);
        if (previousSelected != NULL_PROJECT_SCENE && project->getScene(previousSelected)) {
            project->setSelectedSceneId(previousSelected);
        } else if (restored != NULL_PROJECT_SCENE) {
            project->setSelectedSceneId(restored);
        }
        project->saveProjectFile();
    }

    Project* project;
    uint32_t sceneId;
    fs::path scenePath;
    bool wasOpened = false;
    bool removed = false;
    uint32_t previousSelected = NULL_PROJECT_SCENE;
};

void deleteSceneTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    const bool deleteFile = call->args().value("delete_file", false);
    if (project->isPrefabStage(scene->id)) {
        call->fail("That is a prefab stage (Prefab Mode), not a project scene.");
        return;
    }
    if (project->getScenes().size() <= 1) {
        call->fail("Cannot remove the project's last scene.");
        return;
    }
    if (scene->filepath.empty()) {
        call->fail("Scene \"" + scene->name + "\" was never saved: close it with close_scene (unsaved: discard) instead.");
        return;
    }
    if (project->hasSceneUnsavedChanges(scene->id)) {
        call->fail("Scene \"" + scene->name + "\" has unsaved changes: save_scene (or close_scene with unsaved: discard) first.");
        return;
    }
    const uint32_t id = scene->id;
    const std::string name = scene->name;
    const fs::path relative = project->normalizeToProjectRelative(scene->filepath);
    const fs::path absolute = project->getProjectPath() / relative;
    if (deleteFile) {
        if (const std::string escape = McpRefs::writeEscape(project, absolute, McpRefs::WriteUse::Entry); !escape.empty()) {
            call->fail(escape);
            return;
        }
    }
    CommandHistory* history = project->getProjectCommandHistory();
    const size_t mark = history->mark();
    std::string error;
    if (!runProjectCommand(project, new RemoveProjectSceneCmd(project, id), error, "The scene could not be removed from the project.")) {
        call->fail(error);
        return;
    }
    if (deleteFile) {
        // the file moved to the trash beneath the project (a link: the link itself)
        auto* deleteCmd = new DeleteFileCmd(project, {absolute}, project->getProjectPath());
        deleteCmd->setWriteRoots(McpRefs::trustedRoots(project));
        if (!runProjectCommand(project, deleteCmd, error, "The scene file could not be deleted.")) {
            history->undo();
            call->fail(error);
            return;
        }
        history->groupSince(mark);
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
    }
    call->ok("Removed scene " + std::to_string(id) + " \"" + name + "\" from the project" +
             (deleteFile ? " and moved " + FileUtils::pathToGenericUtf8(relative) + " to the project's .trash" : " (file kept)") +
             ". Undo with undo scope=project.",
             Json{{"removed_scene_id", id}, {"path", FileUtils::pathToGenericUtf8(relative)}, {"file_deleted", deleteFile},
                  {"selected_scene_id", project->getSelectedSceneId()}});
}

// ------------------------------------------------------------------ child scenes / main camera

void setChildSceneTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* parent = sceneArg(call);
    if (!parent) return;
    std::string error;
    SceneProject* child = McpRefs::resolveScene(project, args.contains("child") ? args["child"] : Json(""), error);
    if (!child) {
        call->fail("child: " + error);
        return;
    }
    const uint32_t parentId = parent->id;
    const uint32_t childId = child->id;
    if (parentId == childId) {
        call->fail("A scene cannot be its own child scene.");
        return;
    }
    if (project->isPrefabStage(parentId) || project->isPrefabStage(childId)) {
        call->fail("Prefab stages have no child scenes.");
        return;
    }
    const bool attachedNow = project->hasChildScene(parentId, childId);
    const bool attach = args.contains("attached") ? args["attached"].get<bool>() : true;
    if (attach && !attachedNow && project->wouldCreateChildSceneCycle(parentId, childId)) {
        call->fail("\"" + child->name + "\" already contains \"" + parent->name + "\" as a child scene (directly or "
                   "deeper), so adding it would make a cycle.");
        return;
    }
    auto* multi = new MultiPropertyCmd();
    bool any = false;
    if (attach && !attachedNow) {
        multi->addCommand(std::make_unique<AddChildSceneCmd>(project, parentId, childId));
        any = true;
        if (args.contains("start_active") && !args["start_active"].get<bool>()) {
            multi->addCommand(std::make_unique<SetChildSceneStartActiveCmd>(project, parentId, childId, false));
        }
    } else if (!attach && attachedNow) {
        multi->addCommand(std::make_unique<RemoveChildSceneCmd>(project, parentId, childId));
        any = true;
    } else if (attach && args.contains("start_active") &&
               project->isChildSceneStartActive(parentId, childId) != args["start_active"].get<bool>()) {
        multi->addCommand(std::make_unique<SetChildSceneStartActiveCmd>(project, parentId, childId, args["start_active"].get<bool>()));
        any = true;
    }
    if (any) {
        if (!runSceneCommand(parentId, multi, error, "The child scene change did not apply (a cycle, or Play mode?).")) {
            call->fail(error);
            return;
        }
    } else {
        delete multi;
    }
    if (args.contains("loaded_inline") && project->hasChildScene(parentId, childId)) {
        if (args["loaded_inline"].get<bool>()) {
            if (!project->loadChildSceneInline(childId)) {
                call->fail("The child scene could not be loaded inline.");
                return;
            }
        } else {
            project->unloadChildSceneInline(childId);
        }
    }
    SceneProject* updated = project->getScene(parentId);
    call->ok(std::string(project->hasChildScene(parentId, childId) ? "Scene " : "Scene ") + std::to_string(childId) +
             (project->hasChildScene(parentId, childId) ? " is a child scene of " : " is not a child scene of ") +
             std::to_string(parentId) + ".",
             Json{{"scene", sceneInfo(project, *updated)}});
}

void setMainCameraTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->scene) {
        call->fail("The scene is not open.");
        return;
    }
    Entity camera = NULL_ENTITY;
    std::string error;
    if (!args.contains("entity")) {
        call->fail("Pass entity (the camera GameObject), or entity: null to use the scene's default camera.");
        return;
    }
    if (!McpValues::entityFromJson(scene, args["entity"], camera, error)) {
        call->fail(error);
        return;
    }
    if (camera != NULL_ENTITY) {
        CameraComponent* component = scene->scene->findComponent<CameraComponent>(camera);
        if (!component) {
            call->fail("\"" + scene->scene->getEntityName(camera) + "\" has no Camera component.");
            return;
        }
        if (component->renderToTexture) {
            call->fail("\"" + scene->scene->getEntityName(camera) + "\" renders to a texture: it cannot be the main camera.");
            return;
        }
    }
    const uint32_t id = scene->id;
    if (!runSceneCommand(id, new SetMainCameraCmd(project, id, camera), error)) {
        call->fail(error);
        return;
    }
    SceneProject* updated = project->getScene(id);
    call->ok(camera == NULL_ENTITY ? "Cleared the main camera." : "Main camera: \"" + updated->scene->getEntityName(camera) + "\".",
             Json{{"scene_id", id}, {"main_camera", McpValues::entityToJson(updated, updated->mainCamera)}});
}

// ------------------------------------------------------------------ scene settings

enum class SettingKind { Bool, Int, Float, Vector2, Vector3, Vector4, String, ShadowQuality, SceneColorScale, LightState, LitQuality, Filter, PostProcess,
                         PostProcessing, PostProcessingField, ShadowDebugView, CanvasScaler };

struct SceneSetting {
    std::string name;
    SettingKind kind;
    std::string help;
    // listed by get_scene_settings without names (the post-processing fields are in
    // post_processing; each is still readable and settable by its own name)
    bool listed = true;
    const PostProcessingSettings::Field* field = nullptr;
};

const std::vector<SceneSetting>& sceneSettings() {
    static const std::vector<SceneSetting> settings = {
        {"background_color", SettingKind::Vector4, "clear color [r,g,b,a] (sRGB)"},
        {"light_state", SettingKind::LightState, "scene lighting: off, on, auto (on when a light exists)"},
        {"global_illumination_color", SettingKind::Vector3, "ambient light color [r,g,b] (sRGB)"},
        {"global_illumination_intensity", SettingKind::Float, "ambient light intensity"},
        {"max_lights_per_object", SettingKind::Int, "lights each object is lit by, 1-16 (the rest reaching it fold into its ambient)"},
        {"lit_quality", SettingKind::LitQuality, "the built-in Lit's quality: standard (as before), high (specular anti-aliasing, direct-light energy compensation; new scenes)"},
        {"physical_light_ev100", SettingKind::Float, "Physical Light Exposure, EV100 -6..24 (Sunny 15, Overcast 12, Interior 7, Night 2): only lights in lux, candela or lumen"},
        {"shadows_quality", SettingKind::ShadowQuality,
         "3D shadow quality tier: auto (the project's tier of the platform), none (that tier, every light hard), low, medium, high, ultra (PCSS)"},
        {"shadow_distance", SettingKind::Float, "metres of directional shadows from the camera (0: the quality tier's; a light's own overrides it)"},
        {"shadow_fade_fraction", SettingKind::Float, "last fraction of the shadow distance over which directional shadows fade out (0..1)"},
        {"shadow_cascade_blend", SettingKind::Bool, "blend (dither on Low) between shadow cascades over a band before each split"},
        {"capsule_shadows_enabled", SettingKind::Bool,
         "soft shadows of the main directional light from the capsules of the 4 Capsule Shadow Casters nearest the camera "
         "(per lit pixel, up to 32 capsule tests; Capsules Only casters keep the static shadow cache valid)"},
        {"capsule_shadow_intensity", SettingKind::Float, "how dark capsule shadows are, 0..1 (1)"},
        {"capsule_shadow_softness", SettingKind::Float, "how fast a capsule shadow's penumbra widens per metre along the light, 0..2 (0.25)"},
        {"shadows_debug_view", SettingKind::ShadowDebugView,
         "Scene view shadow debug view (editor only, never saved, no undo step): none, cascades, shadow_factor, texel_density, cache, atlas"},
        {"ambient_light_2d_color", SettingKind::Vector3, "2D ambient light color [r,g,b] (sRGB)"},
        {"ambient_light_2d_intensity", SettingKind::Float, "2D ambient light intensity"},
        {"shadows_2d_quality", SettingKind::ShadowQuality, "2D shadow edge filtering: none, low, medium, high (auto reads as low, ultra as high)"},
        {"physics_gravity_2d", SettingKind::Vector2, "2D gravity [x,y]"},
        {"physics_gravity_3d", SettingKind::Vector3, "3D gravity [x,y,z]"},
        {"physics_interpolation", SettingKind::Bool, "draw bodies between their last two fixed steps (smooth at any frame rate)"},
        {"physics_2d_sub_steps", SettingKind::Int, "Box2D sub-steps per fixed step, 1-16 (default 4; more = stiffer joints)"},
        {"mesh_lod_enabled", SettingKind::Bool, "build and use mesh detail levels"},
        {"mesh_lod_threshold", SettingKind::Float, "pixels of error a detail level may show"},
        {"mesh_lod_cross_fade", SettingKind::Bool, "dithered cross-fade between detail levels (fading objects draw twice for the duration; off: no fade shaders built or shipped)"},
        {"mesh_lod_cross_fade_duration", SettingKind::Float, "seconds of a detail level cross-fade, 0.05-2 (default 0.5)"},
        {"depth_prepass_enabled", SettingKind::Bool, "depth-only pass before opaque draws"},
        {"scene_color_scale", SettingKind::SceneColorScale,
         "resolution of the scene-colour copy shaders requiring scenecolor sample: full, half (default)"},
        {"ssao_enabled", SettingKind::Bool, "screen-space ambient occlusion"},
        {"ssao_radius", SettingKind::Float, "SSAO radius"},
        {"ssao_intensity", SettingKind::Float, "SSAO intensity"},
        {"ssao_bias", SettingKind::Float, "SSAO bias"},
        {"ssao_debug", SettingKind::Bool, "show the SSAO buffer"},
        {"ssr_enabled", SettingKind::Bool, "screen-space reflections"},
        {"ssr_max_distance", SettingKind::Float, "SSR max ray distance"},
        {"ssr_thickness", SettingKind::Float, "SSR thickness"},
        {"ssr_intensity", SettingKind::Float, "SSR intensity"},
        {"ssr_blur", SettingKind::Float, "SSR blur"},
        {"ssr_max_steps", SettingKind::Int, "SSR ray march steps"},
        {"ssr_debug_mode", SettingKind::Int, "SSR debug view: 0 off, 1 reflection, 2 normal, 3 roughness, 4 metallic, 5 albedo, "
         "6 environment specular, 7 reflection source, 8 occlusion"},
        {"canvas_scaler", SettingKind::CanvasScaler,
         "UI scale (Canvas Scaler) of the scene's UI and screen HTML documents: {use_project_default, mode: follow_game_canvas|"
         "constant_pixel_size|scale_with_screen_size|constant_physical_size, reference: [w,h], match_mode: "
         "match_width_or_height|expand|shrink, match: 0..1, scale_factor, physical_unit, fallback_dpi}; fields given "
         "override the project default (use_project_default false)"},
        {"fixed_resolution_enabled", SettingKind::Bool, "render at a fixed resolution, then scale"},
        {"fixed_resolution_width", SettingKind::Int, "fixed resolution width (>= 1)"},
        {"fixed_resolution_height", SettingKind::Int, "fixed resolution height (>= 1)"},
        {"fixed_resolution_filter", SettingKind::Filter, "fixed resolution upscale filter: nearest, linear"},
        {"default_mesh_shader", SettingKind::String, "library:<name> or forked shader base path for meshes (\"\" built-in)"},
        {"default_ui_shader", SettingKind::String, "library:<name> or forked shader base path for UI (\"\" built-in)"},
        {"default_sky_shader", SettingKind::String, "library:<name> or forked shader base path for the sky (\"\" built-in)"},
        {"default_points_shader", SettingKind::String, "library:<name> or forked shader base path for points (\"\" built-in)"},
        {"default_lines_shader", SettingKind::String, "library:<name> or forked shader base path for lines (\"\" built-in)"},
        {"post_process", SettingKind::PostProcess,
         "post-process passes in order: [{shader: \"library:<name>\" (list_shaders type=\"postprocess\") or a fork base path, "
         "enabled: bool, uniforms: {name: [x,y,z,w]}, textures: {uniform: image path}, scale: 0.1..1 (output resolution)}]"},
        {"post_processing", SettingKind::PostProcessing,
         "the engine's post-processing of the main camera: {field: value} of the fields away from their defaults; "
         "setting an object changes the fields it names, null resets all. Each field is also the setting "
         "post_processing.<field>"}
    };
    static const std::vector<SceneSetting> all = [](){
        std::vector<SceneSetting> list = settings;
        for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
            SceneSetting setting{std::string("post_processing.") + field.snakeName, SettingKind::PostProcessingField, field.help, false, &field};
            list.push_back(setting);
        }
        return list;
    }();
    return all;
}

// one post-processing field as JSON (enums by name; the LUT image project-relative, as every asset
// path of MCP, when project is given: it is stored relative to the assets directory)
Json postProcessingFieldToJson(const PostProcessingSettings& settings, const PostProcessingSettings::Field& field,
                               Project* project = nullptr) {
    using FT = PostProcessingSettings::FieldType;
    const void* ref = settings.getFieldRef(field);
    switch (field.type) {
        case FT::Bool: return *(const bool*)ref;
        case FT::Int: return *(const int*)ref;
        case FT::Float: return McpValues::number(*(const float*)ref);
        case FT::Vector3: return McpValues::toJson(*(const Vector3*)ref);
        case FT::String: return McpValues::assetPathToJson(project, *(const std::string*)ref);
        case FT::Asset: return McpValues::assetPathToJson(project, ((const AssetRef*)ref)->path);
        case FT::Enum: return std::string(field.enumNames[*(const int*)ref]);
    }
    return nullptr;
}

// the fields away from their defaults, by snake name
Json postProcessingToJson(const PostProcessingSettings& settings, Project* project = nullptr) {
    static const PostProcessingSettings defaults;
    Json object = Json::object();
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        Json value = postProcessingFieldToJson(settings, field, project);
        if (value != postProcessingFieldToJson(defaults, field)) object[field.snakeName] = value;
    }
    return object;
}

std::string enumNamesText(const PostProcessingSettings::Field& field) {
    std::string text;
    for (int i = 0; field.enumNames[i]; i++) text += std::string(i ? ", " : "") + field.enumNames[i];
    return text;
}

// parses `value` into the field of `settings`; false with `error`
bool postProcessingFieldFromJson(Project* project, const PostProcessingSettings::Field& field, const Json& value,
                                 PostProcessingSettings& settings, std::string& error) {
    using FT = PostProcessingSettings::FieldType;
    const std::string name = std::string("post_processing.") + field.snakeName;
    void* ref = field.ref(settings);
    auto range = [&]() {
        return " (" + Json(McpValues::number(field.minValue)).dump() + " to " + Json(McpValues::number(field.maxValue)).dump() + ")";
    };
    switch (field.type) {
        case FT::Bool:
            if (!value.is_boolean()) { error = name + " must be true or false."; return false; }
            *(bool*)ref = value.get<bool>();
            return true;
        case FT::Int: {
            if (!value.is_number_integer()) { error = name + " must be an integer."; return false; }
            const int v = value.get<int>();
            if (std::string(field.name) == "lutSize" && v != 16 && v != 32) { error = name + " must be 16 or 32."; return false; }
            if (std::string(field.name) == "msaaSamples" && v != 1 && v != 2 && v != 4) { error = name + " must be 1, 2 or 4."; return false; }
            *(int*)ref = v;
            return true;
        }
        case FT::Float: {
            if (!value.is_number()) { error = name + " must be a number" + range() + "."; return false; }
            const float v = value.get<float>();
            if (!(v >= field.minValue && v <= field.maxValue)) { error = name + " must be a number" + range() + "."; return false; }
            *(float*)ref = v;
            return true;
        }
        case FT::Vector3: {
            Vector3 v = *(Vector3*)ref;
            if (!McpValues::parseVector3(value, v)) { error = name + " must be [x,y,z] (or \"#RRGGBB\")" + range() + "."; return false; }
            // refused outside the range, as the other number fields (the setter would clamp it silently)
            for (float c : {v.x, v.y, v.z}) {
                if (!(c >= field.minValue && c <= field.maxValue)) {
                    error = name + " components must be numbers" + range() + ".";
                    return false;
                }
            }
            *(Vector3*)ref = v;
            return true;
        }
        case FT::String:
        case FT::Asset: {
            if (!value.is_string()) { error = name + " must be a project asset path (\"\" none)."; return false; }
            const std::string path = value.get<std::string>();
            // a GUID names the file too (assetPathFromJson resolves it)
            if (!path.empty() && !Guid::isValidText(path) && !fs::exists(project->getProjectPath() / path)) {
                error = name + ": no file \"" + path + "\" in the project.";
                return false;
            }
            // stored relative to the assets directory, as the engine loads it (and the inspector stores it)
            std::string stored;
            if (!McpValues::assetPathFromJson(project, path, stored, error)) {
                error = name + ": " + error;
                return false;
            }
            if (field.type == FT::Asset) {
                // the file's GUID now, not at the next save: a rename or move of it before the
                // scene is saved is followed. A file written a moment ago that no asset refresh
                // has seen yet (the refresh is a Background walk) is imported first.
                Guid guid;
                AssetDatabase& database = project->getAssetDatabase();
                if (!stored.empty() && database.isOpen()) {
                    const fs::path full = project->resolveAssetPath(FileUtils::pathFromUtf8(stored));
                    guid = database.getAssetGuid(full);
                    std::error_code ec;
                    if (guid.isNull() && fs::is_regular_file(full, ec)) {
                        project->importPrefabFiles(full);
                        guid = database.getAssetGuid(full);
                    }
                }
                *(AssetRef*)ref = AssetRef(stored, guid);
            } else {
                *(std::string*)ref = stored;
            }
            return true;
        }
        case FT::Enum: {
            // by name (any case) or by its int (the index in the names), as every MCP enum
            int count = 0;
            while (field.enumNames[count]) count++;
            if (value.is_number_integer()) {
                const long long index = value.get<long long>();
                if (index >= 0 && index < count) { *(int*)ref = (int)index; return true; }
            } else if (value.is_string()) {
                std::string text = value.get<std::string>();
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                for (int i = 0; i < count; i++) {
                    if (text == field.enumNames[i]) { *(int*)ref = i; return true; }
                }
            }
            error = name + " must be one of: " + enumNamesText(field) + " (or its index 0 to " + std::to_string(count - 1) + ").";
            return false;
        }
    }
    return false;
}

const SceneSetting* findSetting(const std::string& name) {
    for (const SceneSetting& setting : sceneSettings()) {
        if (name == setting.name) return &setting;
    }
    return nullptr;
}

std::string settingNamesText() {
    std::string text;
    for (const SceneSetting& setting : sceneSettings()) {
        if (!setting.listed) continue;
        if (!text.empty()) text += ", ";
        text += setting.name;
    }
    return text + ", post_processing.<field> (the fields of post_processing)";
}

const char* kindName(SettingKind kind) {
    switch (kind) {
        case SettingKind::Bool: return "bool";
        case SettingKind::Int: return "int";
        case SettingKind::Float: return "float";
        case SettingKind::Vector2: return "vector2";
        case SettingKind::Vector3: return "vector3";
        case SettingKind::Vector4: return "vector4";
        case SettingKind::String: return "string";
        case SettingKind::ShadowQuality:
        case SettingKind::SceneColorScale:
        case SettingKind::LightState:
        case SettingKind::LitQuality:
        case SettingKind::ShadowDebugView:
        case SettingKind::Filter: return "enum";
        case SettingKind::PostProcess: return "post_process";
        case SettingKind::PostProcessing: return "post_processing";
        case SettingKind::PostProcessingField: return "post_processing_field";
        case SettingKind::CanvasScaler: return "canvas_scaler";
    }
    return "unknown";
}

const McpValues::EnumOptions* settingOptions(SettingKind kind) {
    switch (kind) {
        case SettingKind::ShadowQuality: return McpValues::enumTable("shadow_quality");
        case SettingKind::ShadowDebugView: return McpValues::enumTable("shadow_debug_view");
        case SettingKind::SceneColorScale: return McpValues::enumTable("scene_color_scale");
        case SettingKind::LightState: return McpValues::enumTable("light_state");
        case SettingKind::LitQuality: return McpValues::enumTable("lit_quality");
        case SettingKind::Filter: return McpValues::enumTable("framebuffer_filter");
        default: return nullptr;
    }
}

Json postProcessToJson(const std::vector<PostProcessPass>& passes, Project* project = nullptr) {
    Json list = Json::array();
    for (const PostProcessPass& pass : passes) {
        Json item = {{"shader", pass.shader}, {"enabled", pass.enabled}, {"uniforms", McpValues::shaderUniformsToJson(pass.uniforms)}};
        // (phase 4) only when set: the pass's textures by uniform and its scale. A texture is
        // written as the shader textures of a material are (a project-relative path, or
        // {path, filters, wraps} / {cube} when it has them), so a read-modify-write of the
        // chain keeps its sampler settings and cube maps
        Json textures = McpValues::shaderTexturesToJson(project, nullptr, pass.textures);
        if (!textures.empty()) item["textures"] = textures;
        if (pass.scale != 1.0f) item["scale"] = pass.scale;
        // members following global shader values {member: global}, only when set
        if (!pass.shaderGlobalBindings.empty()) {
            Json bindings = Json::object();
            for (const auto& binding : pass.shaderGlobalBindings) bindings[binding.first] = binding.second;
            item["shaderGlobalBindings"] = bindings;
        }
        list.push_back(item);
    }
    return list;
}

template<typename T>
Json readSetting(Scene* scene, const SceneSetting& setting, bool defaults);

Json settingValue(Scene* scene, const SceneSetting& setting, bool defaults, Project* project);

template<typename T>
Json readSetting(Scene* scene, const SceneSetting& setting, bool defaults) {
    return defaults ? Json(Catalog::getScenePropertyDefault<T>(setting.name)) : Json(Catalog::getSceneProperty<T>(scene, setting.name));
}

Json settingValue(Scene* scene, const SceneSetting& setting, bool defaults = false, Project* project = nullptr) {
    switch (setting.kind) {
        case SettingKind::PostProcessing:
            return postProcessingToJson(defaults || !scene ? PostProcessingSettings() : scene->getPostProcessing(), project);
        case SettingKind::PostProcessingField:
            return postProcessingFieldToJson(defaults || !scene ? PostProcessingSettings() : scene->getPostProcessing(), *setting.field,
                                             project);
        case SettingKind::Bool: return readSetting<bool>(scene, setting, defaults);
        case SettingKind::Int: return readSetting<int>(scene, setting, defaults);
        case SettingKind::Float:
            return McpValues::number(defaults ? Catalog::getScenePropertyDefault<float>(setting.name)
                                              : Catalog::getSceneProperty<float>(scene, setting.name));
        case SettingKind::String: return readSetting<std::string>(scene, setting, defaults);
        case SettingKind::Vector2:
            return McpValues::toJson(defaults ? Catalog::getScenePropertyDefault<Vector2>(setting.name)
                                              : Catalog::getSceneProperty<Vector2>(scene, setting.name));
        case SettingKind::Vector3:
            return McpValues::toJson(defaults ? Catalog::getScenePropertyDefault<Vector3>(setting.name)
                                              : Catalog::getSceneProperty<Vector3>(scene, setting.name));
        case SettingKind::Vector4:
            return McpValues::toJson(defaults ? Catalog::getScenePropertyDefault<Vector4>(setting.name)
                                              : Catalog::getSceneProperty<Vector4>(scene, setting.name));
        case SettingKind::ShadowQuality:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<ShadowQuality>(setting.name)
                                                        : Catalog::getSceneProperty<ShadowQuality>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::ShadowDebugView:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<ShadowDebugView>(setting.name)
                                                        : Catalog::getSceneProperty<ShadowDebugView>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::SceneColorScale:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<SceneColorScale>(setting.name)
                                                        : Catalog::getSceneProperty<SceneColorScale>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::LightState:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<LightState>(setting.name)
                                                        : Catalog::getSceneProperty<LightState>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::LitQuality:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<LitQuality>(setting.name)
                                                        : Catalog::getSceneProperty<LitQuality>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::Filter:
            return McpValues::enumToJson((int)(defaults ? Catalog::getScenePropertyDefault<TextureFilter>(setting.name)
                                                        : Catalog::getSceneProperty<TextureFilter>(scene, setting.name)),
                                         *settingOptions(setting.kind));
        case SettingKind::PostProcess:
            return postProcessToJson(defaults ? Catalog::getScenePropertyDefault<std::vector<PostProcessPass>>(setting.name)
                                              : Catalog::getSceneProperty<std::vector<PostProcessPass>>(scene, setting.name), project);
        case SettingKind::CanvasScaler: {
            const SceneCanvasScaler value = defaults ? Catalog::getScenePropertyDefault<SceneCanvasScaler>(setting.name)
                                                     : Catalog::getSceneProperty<SceneCanvasScaler>(scene, setting.name);
            Json json = McpValues::canvasScalerToJson(value.settings);
            json["use_project_default"] = value.useProjectDefault;
            return json;
        }
    }
    return nullptr;
}

bool shaderBaseExists(Project* project, const std::string& base) {
    if (base.empty()) return true;
    Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(base);
    return fs::exists(project->getProjectPath() / paths.vert) && fs::exists(project->getProjectPath() / paths.frag);
}

// A post_processing or post_processing.<field> value applied to `settings` (every one of a call
// edits the same struct, so they compose in one command); false with `error`
bool applyPostProcessingSetting(Project* project, const SceneSetting& setting, const Json& value,
                                PostProcessingSettings& settings, std::string& error) {
    if (setting.kind == SettingKind::PostProcessingField)
        return postProcessingFieldFromJson(project, *setting.field, value, settings, error);
    if (value.is_null()) {
        settings = PostProcessingSettings();
        return true;
    }
    if (!value.is_object()) {
        error = setting.name + " must be an object {field: value} (null resets every field).";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const PostProcessingSettings::Field* field = PostProcessingSettings::findField(it.key());
        if (!field) {
            std::string names;
            for (const PostProcessingSettings::Field& f : PostProcessingSettings::getFields())
                names += std::string(names.empty() ? "" : ", ") + f.snakeName;
            error = setting.name + ": unknown field \"" + it.key() + "\". Fields: " + names + ".";
            return false;
        }
        if (!postProcessingFieldFromJson(project, *field, it.value(), settings, error)) return false;
    }
    return true;
}

// The ScenePropertyCmd setting `value`, null with `error`
Command* buildSettingCommand(Project* project, SceneProject* scene, const SceneSetting& setting, const Json& value, std::string& error) {
    const uint32_t id = scene->id;
    const std::string name = setting.name;
    auto expect = [&](const std::string& what) -> Command* {
        error = name + " must be " + what + ".";
        return nullptr;
    };
    switch (setting.kind) {
        case SettingKind::Bool:
            if (!value.is_boolean()) return expect("true or false");
            return new ScenePropertyCmd<bool>(project, id, name, value.get<bool>());
        case SettingKind::Int:
            if (!value.is_number_integer()) return expect("an integer");
            if (name.rfind("fixed_resolution_", 0) == 0 && value.get<int>() < 1) return expect("at least 1");
            if (name == "max_lights_per_object" && (value.get<int>() < 1 || value.get<int>() > 16)) return expect("1 to 16");
            if (name == "physics_2d_sub_steps" && (value.get<int>() < 1 || value.get<int>() > 16)) return expect("1 to 16");
            if (name == "ssr_debug_mode" && (value.get<int>() < 0 || value.get<int>() > 8)) return expect("0 to 8");
            return new ScenePropertyCmd<int>(project, id, name, value.get<int>());
        case SettingKind::Float:
            if (!value.is_number()) return expect("a number");
            if (name == "physical_light_ev100" && (value.get<float>() < -6.0f || value.get<float>() > 24.0f)) return expect("-6 to 24");
            if (name == "capsule_shadow_intensity" && (value.get<float>() < 0.0f || value.get<float>() > 1.0f)) return expect("0 to 1");
            if (name == "capsule_shadow_softness" && (value.get<float>() < 0.0f || value.get<float>() > 2.0f)) return expect("0 to 2");
            return new ScenePropertyCmd<float>(project, id, name, value.get<float>());
        case SettingKind::String: {
            if (!value.is_string()) return expect("a string (\"\" for the built-in shader)");
            const std::string base = value.get<std::string>();
            if (ShaderReference::isLibrary(base)) {
                // a library shader of the type this default is for
                const ShaderType type = name == "default_ui_shader" ? ShaderType::UI : name == "default_sky_shader" ? ShaderType::SKYBOX
                                      : name == "default_points_shader" ? ShaderType::POINTS
                                      : name == "default_lines_shader" ? ShaderType::LINES : ShaderType::MESH;
                std::string why;
                if (!McpValues::checkShaderReference(project, base, type, why)) {
                    error = name + ": " + why;
                    return nullptr;
                }
            } else if (!shaderBaseExists(project, base)) {
                return expect("\"library:<name>\" (list_shaders), the base path of a forked shader whose .vert/.frag exist "
                              "(fork the shader first), or \"\"");
            }
            return new ScenePropertyCmd<std::string>(project, id, name, base);
        }
        case SettingKind::Vector2: {
            Vector2 v = Catalog::getSceneProperty<Vector2>(scene->scene, name);
            if (!McpValues::parseVector2(value, v)) return expect("[x,y]");
            return new ScenePropertyCmd<Vector2>(project, id, name, v);
        }
        case SettingKind::Vector3: {
            Vector3 v = Catalog::getSceneProperty<Vector3>(scene->scene, name);
            if (!McpValues::parseVector3(value, v)) return expect("[x,y,z] (or \"#RRGGBB\")");
            return new ScenePropertyCmd<Vector3>(project, id, name, v);
        }
        case SettingKind::Vector4: {
            Vector4 v = Catalog::getSceneProperty<Vector4>(scene->scene, name);
            if (!McpValues::parseVector4(value, v)) return expect("[r,g,b,a] (or [r,g,b], \"#RRGGBB[AA]\")");
            return new ScenePropertyCmd<Vector4>(project, id, name, v);
        }
        case SettingKind::ShadowQuality:
        case SettingKind::SceneColorScale:
        case SettingKind::LightState:
        case SettingKind::LitQuality:
        case SettingKind::Filter: {
            int parsed = 0;
            std::string why;
            if (!McpValues::enumFromJson(value, *settingOptions(setting.kind), parsed, why)) {
                error = name + ": " + why;
                return nullptr;
            }
            if (setting.kind == SettingKind::ShadowQuality) return new ScenePropertyCmd<ShadowQuality>(project, id, name, (ShadowQuality)parsed);
            if (setting.kind == SettingKind::SceneColorScale) return new ScenePropertyCmd<SceneColorScale>(project, id, name, (SceneColorScale)parsed);
            if (setting.kind == SettingKind::LightState) return new ScenePropertyCmd<LightState>(project, id, name, (LightState)parsed);
            if (setting.kind == SettingKind::LitQuality) return new ScenePropertyCmd<LitQuality>(project, id, name, (LitQuality)parsed);
            return new ScenePropertyCmd<TextureFilter>(project, id, name, (TextureFilter)parsed);
        }
        case SettingKind::PostProcessing:
        case SettingKind::PostProcessingField:
            break; // setSceneSettingsTool gathers them (applyPostProcessingSetting)
        case SettingKind::ShadowDebugView:
            break; // editor state, applied by setSceneSettingsTool without an undo step
        case SettingKind::CanvasScaler: {
            if (!value.is_object()) return expect("an object {use_project_default, mode, reference, match_mode, match, ...}");
            SceneCanvasScaler scaler = Catalog::getSceneProperty<SceneCanvasScaler>(scene->scene, name);
            Json fields = value;
            bool useDefaultGiven = false;
            if (value.contains("use_project_default")) {
                if (!value["use_project_default"].is_boolean()) return expect("an object whose use_project_default is true or false");
                useDefaultGiven = true;
                fields.erase("use_project_default");
            }
            // fields on a scene that used the project default start from that default
            if (!fields.empty() && scaler.useProjectDefault && !useDefaultGiven)
                scaler.settings = project->getUIScale();
            std::string why;
            if (!McpValues::canvasScalerFromJson(fields, scaler.settings, why)) {
                error = name + ": " + why + ".";
                return nullptr;
            }
            scaler.useProjectDefault = useDefaultGiven ? value["use_project_default"].get<bool>() : (fields.empty() && scaler.useProjectDefault);
            return new ScenePropertyCmd<SceneCanvasScaler>(project, id, name, scaler);
        }
        case SettingKind::PostProcess: {
            if (!value.is_array()) return expect("an array of {shader, enabled, uniforms, textures, scale}");
            std::vector<PostProcessPass> passes;
            for (size_t i = 0; i < value.size(); i++) {
                const Json& item = value[i];
                const std::string where = name + "[" + std::to_string(i) + "]";
                if (!item.is_object()) {
                    error = where + " must be {shader, enabled, uniforms, textures, scale}.";
                    return nullptr;
                }
                // a misspelt key would otherwise be dropped without a word
                for (auto key = item.begin(); key != item.end(); ++key) {
                    static const std::set<std::string> known = {"shader", "enabled", "uniforms", "textures", "scale", "shaderGlobalBindings"};
                    if (!known.count(key.key())) {
                        error = where + ": unknown key \"" + key.key() + "\". A pass is {shader, enabled, uniforms, textures, scale, "
                                "shaderGlobalBindings}.";
                        return nullptr;
                    }
                }
                PostProcessPass pass;
                pass.shader = item.value("shader", std::string());
                const bool library = ShaderReference::isLibrary(pass.shader);
                std::shared_ptr<const ShaderManifest> manifest = library ? ShaderManifests::get(pass.shader) : nullptr;
                if (library) {
                    // a library post-process shader (@shader postprocess, list_shaders)
                    std::string why;
                    if (!McpValues::checkShaderReference(project, pass.shader, ShaderType::POSTPROCESS, why)) {
                        error = where + ".shader: " + why;
                        return nullptr;
                    }
                } else if (!shaderBaseExists(project, pass.shader)) {
                    error = where + ".shader \"" + pass.shader + "\": no forked shader there (fork the post-process shader first).";
                    return nullptr;
                }
                if (item.contains("enabled") && !item["enabled"].is_boolean()) {
                    error = where + ".enabled must be true or false.";
                    return nullptr;
                }
                pass.enabled = item.value("enabled", true);
                std::string why;
                // a library shader's uniforms are its manifest's parameters (@property): unknown
                // members are refused and colours may be "#RRGGBB" (sRGB, stored linear), as a
                // material's shaderParams; a fork's are any member of its u_fs_postParams
                const bool uniformsOk = !item.contains("uniforms") ||
                    (library ? McpValues::shaderParamsFromJson(manifest.get(), true, item["uniforms"], pass.uniforms, why)
                             : McpValues::shaderUniformsFromJson(item["uniforms"], pass.uniforms, why));
                if (!uniformsOk) {
                    error = where + ".uniforms: " + why;
                    return nullptr;
                }
                if (item.contains("textures") &&
                        !McpValues::shaderTexturesFromJson(project, scene, manifest.get(), library, item["textures"], pass.textures, why)) {
                    error = where + ".textures: " + why;
                    return nullptr;
                }
                if (item.contains("scale")) {
                    if (!item["scale"].is_number() || item["scale"].get<double>() < 0.1 || item["scale"].get<double>() > 1.0) {
                        error = where + ".scale must be a number from 0.1 to 1 (the pass's resolution, a fraction of the chain's).";
                        return nullptr;
                    }
                    pass.scale = item["scale"].get<float>();
                }
                if (item.contains("shaderGlobalBindings")) {
                    // {member: global}: u_fs_postParams members that follow global shader values
                    const Json& bindings = item["shaderGlobalBindings"];
                    if (!bindings.is_object()) {
                        error = where + ".shaderGlobalBindings is an object {member: global name}.";
                        return nullptr;
                    }
                    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
                        std::string invalid;
                        if (it.value().is_null()) continue;
                        if (!it.value().is_string() || !ShaderGlobals::isValidName(it.value().get<std::string>(), &invalid)) {
                            error = where + ".shaderGlobalBindings." + it.key() + ": " + (invalid.empty() ? "a global's name" : invalid) + ".";
                            return nullptr;
                        }
                        ShaderGlobals::setBinding(pass.shaderGlobalBindings, it.key(), it.value().get<std::string>());
                    }
                }
                passes.push_back(pass);
            }
            return new ScenePropertyCmd<std::vector<PostProcessPass>>(project, id, name, passes);
        }
    }
    error = "unsupported setting";
    return nullptr;
}

void getSceneSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *scene));
        return;
    }
    std::vector<const SceneSetting*> wanted;
    if (args.contains("names") && args["names"].is_array() && !args["names"].empty()) {
        for (const Json& item : args["names"]) {
            const SceneSetting* setting = item.is_string() ? findSetting(item.get<std::string>()) : nullptr;
            if (!setting) {
                call->fail("Unknown scene setting " + item.dump() + ". Settings: " + settingNamesText() + ".");
                return;
            }
            wanted.push_back(setting);
        }
    } else {
        for (const SceneSetting& setting : sceneSettings()) {
            if (setting.listed) wanted.push_back(&setting);
        }
    }
    const bool detail = args.value("detail", false);
    Json values = Json::object();
    Json schemaOut = Json::object();
    std::string text = "Scene " + std::to_string(scene->id) + " \"" + scene->name + "\" settings:";
    for (const SceneSetting* setting : wanted) {
        Json value = settingValue(scene->scene, *setting, false, call->ctx().project());
        text += "\n" + std::string(setting->name) + " = " + value.dump();
        values[setting->name] = value;
        if (detail) {
            Json entry = {{"type", kindName(setting->kind)}, {"default", settingValue(nullptr, *setting, true)}, {"help", setting->help}};
            if (const McpValues::EnumOptions* options = settingOptions(setting->kind)) entry["options"] = McpValues::enumNames(*options);
            if (setting->field) {
                using FT = PostProcessingSettings::FieldType;
                entry["field_type"] = setting->field->type == FT::Bool ? "bool" : setting->field->type == FT::Int ? "int"
                                    : setting->field->type == FT::Float ? "float" : setting->field->type == FT::Vector3 ? "vector3"
                                    : setting->field->type == FT::Enum ? "enum" : "string";
                if (setting->field->type == FT::Enum) {
                    Json names = Json::array();
                    for (int i = 0; setting->field->enumNames[i]; i++) names.push_back(setting->field->enumNames[i]);
                    entry["options"] = names;
                } else if (setting->field->type == FT::Float || setting->field->type == FT::Int || setting->field->type == FT::Vector3) {
                    entry["min"] = McpValues::number(setting->field->minValue);
                    entry["max"] = McpValues::number(setting->field->maxValue);
                }
            }
            schemaOut[setting->name] = entry;
        }
    }
    Json data = {{"scene_id", scene->id}, {"settings", values}};
    if (detail) data["schema"] = schemaOut;
    if (!args.contains("names") || !args["names"].is_array() || args["names"].empty()) {
        // not a setting: what the last frame made for the draws reading the scene (phase 3), the
        // inspector's Scene Color > In Use row
        const uint32_t inUse = Catalog::getSceneInputsInUse(scene->scene);
        data["scene_inputs_in_use"] = McpValues::sceneInputsToJson(inUse);
        if (inUse) text += "\n(last frame: scene " + Catalog::getSceneInputsLabel(inUse) + " made for the shaders reading it)";
    }
    call->ok(text, data);
}

void setSceneSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *scene));
        return;
    }
    if (!args.contains("settings") || !args["settings"].is_object() || args["settings"].empty()) {
        call->fail("settings must be an object {name: value}. Settings: " + settingNamesText() + ".");
        return;
    }
    auto* multi = new MultiPropertyCmd();
    PostProcessingSettings postProcessing = scene->scene->getPostProcessing();
    bool postProcessingSet = false;
    for (auto it = args["settings"].begin(); it != args["settings"].end(); ++it) {
        const SceneSetting* setting = findSetting(it.key());
        if (!setting) {
            delete multi;
            call->fail("Unknown scene setting \"" + it.key() + "\". Settings: " + settingNamesText() +
                       ". (Fog and the skybox are GameObjects: create_entity type fog / skybox.)");
            return;
        }
        std::string error;
        if (setting->kind == SettingKind::PostProcessing || setting->kind == SettingKind::PostProcessingField) {
            // as apply_post_processing_preset and the inspector (which has no Post-processing
            // section there): a UI scene's camera is never post-processed
            if (scene->sceneType == SceneType::SCENE_UI) {
                delete multi;
                call->fail("\"" + scene->name + "\" is a UI scene: UI scenes are never post-processed (set " + it.key() +
                           " on the 2D or 3D scene whose main camera draws the game).");
                return;
            }
            if (!applyPostProcessingSetting(project, *setting, it.value(), postProcessing, error)) {
                delete multi;
                call->fail(error);
                return;
            }
            postProcessingSet = true;
            continue;
        }
        if (setting->kind == SettingKind::ShadowDebugView) {
            // editor state, not a setting: applied at once, no undo step, never saved
            int parsed = 0;
            if (!McpValues::enumFromJson(it.value(), *settingOptions(setting->kind), parsed, error)) {
                delete multi;
                call->fail(it.key() + ": " + error);
                return;
            }
            scene->scene->setShadowDebugView((ShadowDebugView)parsed);
            continue;
        }
        Command* cmd = buildSettingCommand(project, scene, *setting, it.value(), error);
        if (!cmd) {
            delete multi;
            call->fail(error);
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(cmd));
    }
    if (postProcessingSet) {
        multi->addCommand(std::unique_ptr<Command>(new ScenePropertyCmd<PostProcessingSettings>(project, scene->id, "post_processing", postProcessing)));
    }
    const uint32_t id = scene->id;
    std::string error;
    if (multi->empty()) {
        delete multi;
    }else if (!runSceneCommand(id, multi, error)) {
        call->fail(error);
        return;
    }
    SceneProject* updated = project->getScene(id);
    Json values = Json::object();
    for (auto it = args["settings"].begin(); it != args["settings"].end(); ++it) {
        values[it.key()] = settingValue(updated->scene, *findSetting(it.key()), false, project);
    }
    const bool debugView = args["settings"].contains("shadows_debug_view");
    call->ok("Set " + std::to_string(values.size()) + " scene setting(s) (one undo step" +
             (debugView ? std::string("; shadows_debug_view takes none).") : std::string(").")),
             Json{{"scene_id", id}, {"settings", values}});
}

// ------------------------------------------------------------------ post-processing presets

// the cost estimate of PostProcessingPresets (the inspector's Estimated cost readout)
Json postProcessingCostToJson(const PostProcessingCost& cost) {
    auto side = [](const PostProcessingCostSide& side) {
        Json list = Json::array();
        for (const PostProcessingCostPass& pass : side.passes) {
            list.push_back({{"label", pass.label}, {"count", pass.count}, {"screens", McpValues::number(pass.area)},
                            {"full_resolution", pass.fullResolution}});
        }
        return Json{{"passes", side.passCount}, {"full_resolution_passes", side.fullResolutionPasses},
                    {"screens", McpValues::number(side.fullscreen)}, {"pass_list", list}};
    };
    return Json{{"estimate", true},
                {"engine", side(cost.engine)},
                {"unity_urp", side(cost.urp)},
                {"tiers", {{"in_shader", cost.inShader}, {"uber_pass", cost.uber}, {"bloom_chain", cost.chain}}},
                {"scene_shading", McpValues::number(cost.sceneShading)},
                {"notes", cost.notes}};
}

PostProcessingCost sceneCost(Scene* scene, const PostProcessingSettings& settings) {
    return estimatePostProcessingCost(settings, scene->getPostProcessPasses(), Catalog::getSceneProperty<bool>(scene, "fixed_resolution_enabled"));
}

std::string costText(const PostProcessingCost& cost) {
    char text[200];
    std::snprintf(text, sizeof(text), "%d passes, %d at full resolution, %.2f screens shaded (Unity URP: %d, %d, %.2f)",
                  cost.engine.passCount, cost.engine.fullResolutionPasses, cost.engine.fullscreen,
                  cost.urp.passCount, cost.urp.fullResolutionPasses, cost.urp.fullscreen);
    return text;
}

std::string postPresetIdsText() {
    std::string ids;
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) ids += (ids.empty() ? "" : ", ") + preset.id;
    return ids;
}

// what the running game's renderer applies (play mode only)
Json appliedPostProcessingJson(SceneProject* scene) {
    if (scene->playState != ScenePlayState::PLAYING && scene->playState != ScenePlayState::PAUSED) return nullptr;
    std::shared_ptr<RenderSystem> render = scene->scene->getSystem<RenderSystem>();
    if (!render) return nullptr;
    const PostProcessingTiers& tiers = render->getActivePostProcessingTiers();
    Json features = Json::array();
    const int count = ShaderPool::getShaderPropertyCount(ShaderType::BLIT);
    for (int bit = 0; bit < count && bit < 32; bit++) {
        if (tiers.uberFeatures & (1u << bit)) features.push_back(ShaderPool::getShaderPropertyName(ShaderType::BLIT, bit, false));
    }
    return Json{{"in_shader", tiers.tailColor}, {"uber_pass", tiers.uber}, {"uber_features", features},
                {"render_scale", McpValues::number(tiers.renderScale)}};
}

void listPostProcessingPresetsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    Json presets = Json::array();
    std::string text = std::to_string(getPostProcessingPresets().size()) + " post-processing preset(s); "
                       "apply_post_processing_preset applies one (estimated cost after the scene, alone):";
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
        const PostProcessingCost cost = estimatePostProcessingCost(preset.settings, {}, false);
        presets.push_back({{"id", preset.id}, {"name", preset.name}, {"description", preset.description},
                           {"settings", postProcessingToJson(preset.settings, project)},
                           {"cost", {{"passes", cost.engine.passCount}, {"screens", McpValues::number(cost.engine.fullscreen)},
                                     {"unity_urp_passes", cost.urp.passCount},
                                     {"unity_urp_screens", McpValues::number(cost.urp.fullscreen)}}}});
        text += "\n" + preset.id + " (" + preset.name + "): " + preset.description + " [" + costText(cost) + "]";
    }
    const PostProcessingSettings settings = scene->scene->getPostProcessing();
    const PostProcessingPreset* matched = matchPostProcessingPreset(settings);
    const PostProcessingCost cost = sceneCost(scene->scene, settings);
    Json current = {{"scene_id", scene->id}, {"preset", matched ? Json(matched->id) : Json(nullptr)},
                    {"settings", postProcessingToJson(settings, project)}, {"cost", postProcessingCostToJson(cost)}};
    text += "\nScene \"" + scene->name + "\": " + (matched ? "preset " + matched->id : std::string("custom settings")) +
            ", estimated " + costText(cost) + ".";
    Json applied = appliedPostProcessingJson(scene);
    if (!applied.is_null()) current["applied"] = applied;
    call->ok(text, Json{{"presets", presets}, {"current", current}});
}

void applyPostProcessingPresetTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const std::string id = call->args().value("preset", std::string());
    const PostProcessingPreset* preset = findPostProcessingPreset(id);
    if (!preset) {
        call->fail("No post-processing preset \"" + id + "\". The presets: " + postPresetIdsText() + " (list_post_processing_presets).");
        return;
    }
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    if (scene->sceneType == SceneType::SCENE_UI) {
        call->fail("\"" + scene->name + "\" is a UI scene: UI scenes are never post-processed (set it on the 2D or 3D scene "
                   "whose main camera draws the game).");
        return;
    }
    const uint32_t sceneId = scene->id;
    std::string error;
    if (!runSceneCommand(sceneId, makePostProcessingCommand(project, sceneId, preset->settings, "Apply " + preset->name + " Preset"), error)) {
        call->fail(error);
        return;
    }
    SceneProject* updated = project->getScene(sceneId);
    const PostProcessingCost cost = sceneCost(updated->scene, updated->scene->getPostProcessing());
    call->ok("Applied the " + preset->name + " post-processing preset to \"" + updated->name + "\" (one undo step): estimated " +
             costText(cost) + ".",
             Json{{"scene_id", sceneId}, {"preset", preset->id},
                  {"settings", postProcessingToJson(updated->scene->getPostProcessing(), project)},
                  {"cost", postProcessingCostToJson(cost)}});
}

// ------------------------------------------------------------------ S13: LUTs, status, debug

const char* lutEncodingId(LutInputEncoding encoding) {
    const PostProcessingSettings::Field* field = PostProcessingSettings::findField("lutInputEncoding");
    const int index = (int)encoding;
    return field && field->enumNames && index >= 0 && index <= (int)LutInputEncoding::DAVINCI_INTERMEDIATE ? field->enumNames[index] : "auto";
}

void inspectColorLutTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const std::string requested = call->args().value("path", std::string());
    std::string stored;
    std::string error;
    if (requested.empty()) {
        call->fail("path is required: a project-relative .cube file or LUT strip image (or its GUID).");
        return;
    }
    if (!McpValues::assetPathFromJson(project, requested, stored, error)) {
        call->fail(error);
        return;
    }
    const fs::path absolute = project->resolveAssetPath(stored);
    const std::string relative = McpValues::assetPathToJson(project, stored);
    if (!fs::exists(absolute)) {
        call->fail("No file \"" + relative + "\" in the project.");
        return;
    }
    // read fresh (not through the renderer's cache): the file as it is on disk now; a .cube is
    // parsed here so a bad one reports without logging an engine error
    CubeLut lut;
    bool ok = false;
    if (Util::isColorLutFile(absolute.string())) {
        std::ifstream in(absolute, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        ok = parseCubeLut(text.str(), lut, error);
    } else {
        ok = loadColorLut(absolute.string(), lut, &error);
    }
    Json out = {{"path", relative}, {"ok", ok}, {"error", ok ? Json(nullptr) : Json(error)}};
    if (!ok) {
        call->ok("\"" + relative + "\" is not a usable colour LUT: " + error, out);
        return;
    }
    std::string reason;
    const LutInputEncoding proposed = proposeEncoding(lut, reason);
    const LutStage stage = stageOf(proposed, lut);
    out["title"] = lut.title;
    out["kind"] = lut.fromStrip ? "strip" : "cube";
    out["size_1d"] = lut.size1D;
    out["size_3d"] = lut.size3D;
    out["input_range"] = {McpValues::number(lut.size3D > 0 ? lut.in3D[0] : lut.in1D[0]), McpValues::number(lut.size3D > 0 ? lut.in3D[1] : lut.in1D[1])};
    if (lut.size1D > 0 && lut.size3D > 0) out["input_range_1d"] = {McpValues::number(lut.in1D[0]), McpValues::number(lut.in1D[1])};
    out["proposed_encoding"] = lutEncodingId(proposed);
    out["reason"] = reason;
    out["stage"] = stage == LutStage::SCENE ? "scene" : "display";
    std::string text = "\"" + relative + "\"" + (lut.title.empty() ? std::string() : " (\"" + lut.title + "\")") + ": ";
    if (lut.size1D > 0) text += "1D " + std::to_string(lut.size1D) + (lut.size3D > 0 ? " + " : "");
    if (lut.size3D > 0) text += "3D " + std::to_string(lut.size3D) + "^3";
    text += std::string(", proposed encoding ") + lutEncodingId(proposed) + " (" + reason + "), " +
            (stage == LutStage::SCENE ? "scene stage (before the tone mapping)." : "display stage.");
    call->ok(text, out);
}

Json dynamicResolutionToJson(const DynamicResolutionStatus& status) {
    return Json{{"enabled", status.enabled}, {"running", status.running}, {"idle", status.idle}, {"overridden", status.overridden},
                {"scale", McpValues::number(status.scale)}, {"min_scale", McpValues::number(status.minScale)},
                {"max_scale", McpValues::number(status.maxScale)}, {"source", status.source},
                {"smoothed_ms", McpValues::number(status.smoothedMs)}, {"target_ms", McpValues::number(status.targetMs)},
                {"saturated_low", status.saturatedLow}, {"ineffective", status.ineffective},
                {"accepted_changes", status.acceptedChanges}, {"probe_seconds", McpValues::number(status.probeSeconds)}};
}

// set_post_processing_debug's pins (null: released)
Json postDebugJson(const RenderSystem& render) {
    auto pin = [](bool set, double value) { return set ? Json(value) : Json(nullptr); };
    return Json{{"time_seconds", pin(render.getPostProcessingDebugTime() >= 0.0, render.getPostProcessingDebugTime())},
                {"force_render_scale", pin(render.getDynamicResolutionOverride() > 0.0f, McpValues::number(render.getDynamicResolutionOverride()))},
                {"fake_gpu_ms", pin(render.getDynamicResolutionFakeGpuMs() > 0.0f, McpValues::number(render.getDynamicResolutionFakeGpuMs()))}};
}

void getPostProcessingStatusTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    // while playing, the scene's running copy (its renderer is the game's)
    SceneProject* scene = McpRefs::runtimeScene(project, sceneArg(call));
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    std::shared_ptr<RenderSystem> render = scene->scene->getSystem<RenderSystem>();
    const PostProcessingSettings& settings = scene->scene->getPostProcessing();
    // what applies: the running game's renderer, else what the settings draw on the next frame
    // (an idle editor renders on demand, so its last tiers can be older than the settings)
    const bool playing = scene->playState == ScenePlayState::PLAYING || scene->playState == ScenePlayState::PAUSED;
    const PostProcessingTiers tiers = (render && playing) ? render->getActivePostProcessingTiers() : RenderSystem::getPostProcessingTiers(settings);
    Json features = Json::array();
    const int count = ShaderPool::getShaderPropertyCount(ShaderType::BLIT);
    for (int bit = 0; bit < count && bit < 32; bit++) {
        if (tiers.uberFeatures & (1u << bit)) features.push_back(ShaderPool::getShaderPropertyName(ShaderType::BLIT, bit, false));
    }
    const PostProcessingCost cost = sceneCost(scene->scene, settings);
    Json passes = Json::array();
    for (const PostProcessingCostPass& pass : cost.engine.passes) {
        passes.push_back({{"label", pass.label}, {"count", pass.count}, {"screens", McpValues::number(pass.area)},
                          {"full_resolution", pass.fullResolution}});
    }
    const DynamicResolutionStatus status = render ? render->getDynamicResolutionStatus() : DynamicResolutionStatus();
    const float applied = (render && playing) ? render->getAppliedRenderScale() : tiers.renderScale;
    Json out = {{"scene_id", scene->id}, {"playing", playing}, {"tail_color", tiers.tailColor}, {"uber", tiers.uber},
                {"uber_features", features}, {"render_scale", McpValues::number(tiers.renderScale)},
                {"render_scale_applied", McpValues::number(applied)}, {"passes", passes}, {"passes_estimate", true},
                {"dynamic_resolution", dynamicResolutionToJson(status)}};
    if (render) out["debug"] = postDebugJson(*render);
    char text[256];
    std::snprintf(text, sizeof(text), "\"%s\": %s, uber pass %s, render scale %.2f applied", scene->name.c_str(),
                  tiers.tailColor ? "in-shader colour" : "no in-shader colour", tiers.uber ? "on" : "off", applied);
    std::string message = text;
    if (!features.empty()) message += ", features " + describeUberFeatures(tiers.uberFeatures);
    if (status.enabled) {
        std::snprintf(text, sizeof(text), "; dynamic resolution %s (%s, %.2f, %.1f / %.1f ms)", status.running ? "running" : "idle",
                      status.source, status.scale, status.smoothedMs, status.targetMs);
        message += text;
    }
    call->ok(message + ".", out);
}

void setPostProcessingDebugTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    // while playing, the scene's running copy (its renderer is the game's)
    SceneProject* scene = McpRefs::runtimeScene(project, sceneArg(call));
    if (!scene) return;
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    std::shared_ptr<RenderSystem> render = scene->scene->getSystem<RenderSystem>();
    if (!render) {
        call->fail("\"" + scene->name + "\" has no renderer.");
        return;
    }
    const Json& args = call->args();
    // each value: a number pins it, null releases it, absent leaves it
    auto read = [&](const char* key, double minimum, double maximum, double& out, bool& release) -> bool {
        release = false;
        if (!args.contains(key)) return false;
        if (args[key].is_null()) {
            release = true;
            return true;
        }
        if (!args[key].is_number()) {
            call->fail(std::string(key) + " must be a number or null.");
            return false;
        }
        out = args[key].get<double>();
        if (!(out >= minimum && out <= maximum)) {
            char text[96];
            std::snprintf(text, sizeof(text), " must be a number from %g to %g, or null.", minimum, maximum);
            call->fail(key + std::string(text));
            return false;
        }
        return true;
    };
    double time = 0.0, scale = 0.0, gpuMs = 0.0;
    bool releaseTime = false, releaseScale = false, releaseGpu = false;
    const bool hasTime = read("time_seconds", 0.0, 1e6, time, releaseTime);
    if (call->completed()) return;
    const bool hasScale = read("force_render_scale", 0.5, 1.0, scale, releaseScale);
    if (call->completed()) return;
    const bool hasGpu = read("fake_gpu_ms", 0.01, 1000.0, gpuMs, releaseGpu);
    if (call->completed()) return;
    if (hasTime) render->setPostProcessingDebugTime(releaseTime ? -1.0 : time);
    if (hasScale) render->setDynamicResolutionOverride(releaseScale ? 0.0f : (float)scale);
    if (hasGpu) render->setDynamicResolutionFakeGpuMs(releaseGpu ? 0.0f : (float)gpuMs);
    scene->needUpdateRender = true;
    const Json debug = postDebugJson(*render);
    call->ok("Post-processing debug of \"" + scene->name + "\" (editor only, not saved): " + debug.dump() + ".",
             Json{{"scene_id", scene->id}, {"debug", debug}});
}

} // namespace

void registerSceneTools(McpRegistry& registry) {

    {
        ToolDef tool;
        tool.name = "list_scenes";
        tool.title = "List Scenes";
        tool.description =
            "Project scenes and open Prefab Mode stages: id, name, path (\"\" = unsaved), type, opened, modified, "
            "selected, start_scene, entity_count, main_camera, child_scenes; selected_scene_id.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listScenesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "create_scene";
        tool.title = "Create Scene";
        tool.description =
            "File > New Scene (3D: with sun and sky); adds it to the project and selects it. With path it is saved "
            "at once, else untitled until save_scene with path.";
        tool.inputSchema = object({
            {"name", string("Default: the file name or \"New Scene\".")},
            {"type", withDefault(stringEnum("", {"3D", "2D", "UI"}), "3D")},
            {"path", assetPath(".scene path (extension optional).")},
            {"close_previous", withDefault(boolean(""), false)},
            {"unsaved", unsavedSchema()}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = createSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "open_scene";
        tool.title = "Open Scene";
        tool.description =
            "Opens a .scene file (adding it to the project if unlisted) and selects it; an open scene is just selected.";
        tool.inputSchema = object({
            {"scene", sceneRef(".scene path, or id/name of a project scene.")},
            {"close_previous", withDefault(boolean("Close the previously selected scene."), false)},
            {"unsaved", unsavedSchema()},
            {"recovery", withDefault(stringEnum("Its crash backup: keep (offered again), restore (modified) or discard.",
                {"keep", "restore", "discard"}), "keep")}
        }, {"scene"});
        tool.allowedWhilePlaying = false;
        tool.handler = openSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "close_scene";
        tool.title = "Close Scene";
        tool.description =
            "Closes an open scene's tab (it stays in the project; a never-saved one leaves it). The last open scene "
            "cannot close.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"unsaved", unsavedSchema()}});
        tool.allowedWhilePlaying = false;
        tool.annotations.destructive = true;
        tool.handler = closeSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "select_scene";
        tool.title = "Select Scene";
        tool.description = "Selects an open scene (its tab): the default scene of other tools.";
        tool.inputSchema = object({{"scene", sceneRef("")}}, {"scene"});
        tool.annotations.idempotent = true;
        tool.handler = selectSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "save_scene";
        tool.title = "Save Scene";
        tool.description =
            "Save a scene and its modified child scenes. Untitled scenes need path; path on a saved scene = Save As. "
            "all=true: every modified scene (untitled skipped). A prefab stage id saves its prefab. Fails with "
            "degraded_document if data would be lost (get_load_report).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"path", assetPath(".scene path.")},
            {"all", withDefault(boolean(""), false)},
            {"allow_data_loss", boolean("")}
        });
        tool.allowedWhilePlaying = false;
        tool.annotations.idempotent = true;
        tool.handler = saveSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_load_report";
        tool.group = "data_safety";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Get Load Report";
        tool.description =
            "What the scene's load could not read: issues with kind, file:line, did-you-mean and fate (preserved: "
            "kept on save; dropped: lost by a save).";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.handler = getLoadReportTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_scene_backups";
        tool.group = "data_safety";  // S31 tool groups: listed once enabled, callable always
        tool.title = "List Scene Backups";
        tool.description =
            "Auto Save backups with status (usable: open_scene recovery=restore; stale: the file changed).";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.handler = listSceneBackupsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "discard_scene_backup";
        tool.group = "data_safety";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Discard Scene Backup";
        tool.description = "Deletes a scene's Auto Save backup.";
        tool.inputSchema = object({{"scene", sceneRef()}}, {"scene"});
        tool.annotations.destructive = true;
        tool.handler = discardSceneBackupTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "rename_scene";
        tool.title = "Rename Scene";
        tool.description =
            "Renames a scene (in the project and generated code; the .scene file keeps its name). Names are unique.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"name", string("")}}, {"name"});
        tool.allowedWhilePlaying = false;
        tool.annotations.idempotent = true;
        tool.handler = renameSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "delete_scene";
        tool.title = "Delete Scene";
        tool.description =
            "Remove a saved, unmodified scene from the project (undo scope=project); delete_file=true also moves the "
            "file to .trash. Not the last scene.";
        tool.inputSchema = object({
            {"scene", sceneRef("")},
            {"delete_file", withDefault(boolean(""), false)}
        }, {"scene"});
        tool.allowedWhilePlaying = false;
        tool.annotations.destructive = true;
        tool.handler = deleteSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_child_scene";
        tool.title = "Set Child Scene";
        tool.description =
            "Attach (attached=false: detach) `child` as an additive child scene of `scene`. start_active: active "
            "when the parent starts; loaded_inline: shown inside the parent in the editor. Not undoable.";
        tool.inputSchema = object({
            {"scene", sceneRef("Parent (default: selected).")},
            {"child", sceneRef()},
            {"attached", withDefault(boolean(""), true)},
            {"start_active", boolean("")},
            {"loaded_inline", boolean("")}
        }, {"child"});
        tool.allowedWhilePlaying = false;
        tool.annotations.idempotent = true;
        tool.handler = setChildSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_main_camera";
        tool.title = "Set Main Camera";
        tool.description =
            "Set the scene's main camera (the game renders through it) to a GameObject with a Camera; entity null = "
            "the default camera.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"entity", nullableEntityRef()}}, {"entity"});
        tool.annotations.idempotent = true;
        tool.handler = setMainCameraTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_scene_settings";
        tool.title = "Get Scene Settings";
        tool.description =
            "Scene settings (Inspector > Scene: lighting, shadows, gravity, LOD, SSAO, SSR, default shaders, "
            "post_process, post_processing...): the fields away from their defaults; detail adds type, default, "
            "options, help. Without names also scene_inputs_in_use (made by the last frame for scene-reading "
            "shaders).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"names", array(string(""), "")},
            {"detail", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getSceneSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_scene_settings";
        tool.title = "Set Scene Settings";
        tool.description =
            "Set scene settings {name: value} (names and shapes: get_scene_settings). post_process replaces "
            "the pass list: [{\"shader\": \"shaders/MyPost\", \"enabled\": true, \"uniforms\": {\"strength\": "
            "[0.5,0,0,0]}}]. post_processing {field: value} sets the fields named (null resets all); one undo "
            "step per call.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"settings", anyObject("")}}, {"settings"});
        tool.annotations.idempotent = true;
        tool.handler = setSceneSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_post_processing_presets";
        tool.title = "List Post-processing Presets";
        tool.description =
            "Post-processing presets (none, mobile, mobile_adaptive, console, stylised, crt_retro, film, vhs): id, name, "
            "description, the post_processing fields each sets, estimated cost (passes, full-resolution passes, screens, against "
            "Unity URP). current: the scene's preset (null = custom), settings and cost; in play also applied.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listPostProcessingPresetsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "apply_post_processing_preset";
        tool.title = "Apply Post-processing Preset";
        tool.description =
            "Replace the scene's post_processing with a preset (list_post_processing_presets) in one undo step; "
            "fine-tune with set_scene_settings post_processing.<field>.";
        tool.inputSchema = object({
            {"preset", string("Preset id or name, e.g. \"mobile\".")},
            {"scene", sceneRef()}
        }, {"preset"});
        tool.annotations.idempotent = true;
        tool.handler = applyPostProcessingPresetTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "inspect_color_lut";
        tool.group = "rendering_fx";  // S13: listed once tool_groups enables it, callable always
        tool.title = "Inspect Color LUT";
        tool.description =
            "Read a colour LUT file (.cube, or an image strip) as post_processing.lut_texture would: ok, error (one line "
            "with the line number), title, size_1d, size_3d, input_range, proposed_encoding (what lut_input_encoding auto "
            "picks), reason, stage (display, or scene: before the tone mapping).";
        tool.inputSchema = object({{"path", assetPath("Project-relative .cube or strip image, or its GUID.")}}, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = inspectColorLutTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_post_processing_status";
        tool.group = "rendering_fx";  // S13: listed once tool_groups enables it, callable always
        tool.title = "Get Post-processing Status";
        tool.description =
            "What the scene's renderer applies now: tail_color, uber, uber_features, render_scale (settings) and "
            "render_scale_applied, passes (estimate), dynamic_resolution {enabled, running, idle, scale, source gpu|frame, "
            "smoothed_ms, target_ms, saturated_low, ineffective, accepted_changes, probe_seconds}, debug pins.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getPostProcessingStatusTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_post_processing_debug";
        tool.group = "rendering_fx";  // S13: listed once tool_groups enables it, callable always
        tool.title = "Set Post-processing Debug";
        tool.description =
            "Editor-only, unsaved pins for deterministic captures: time_seconds (the uber effects' time: grain, VHS, "
            "film damage), force_render_scale (0.5..1, in Play), fake_gpu_ms (GPU time fed to dynamic resolution). "
            "A number pins, null releases, absent keeps. Stop or Pause releases the scale and GPU pins.";
        auto nullableNumber = [](const std::string& description) {
            Json value = number(description);
            value["type"] = Json::array({"number", "null"});
            return value;
        };
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"time_seconds", nullableNumber("Seconds, or null: live.")},
            {"force_render_scale", nullableNumber("0.5..1, or null: the controller's / the settings'.")},
            {"fake_gpu_ms", nullableNumber("Milliseconds, or null: measured.")}
        });
        tool.annotations.idempotent = true;
        tool.handler = setPostProcessingDebugTool;
        registry.addTool(std::move(tool));
    }
}

}
