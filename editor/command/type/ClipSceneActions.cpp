// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipSceneActions.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipEditorUtil.h"
#include "animation/ClipValidation.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "animation/PlayerModelLayout.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateClipCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "pose/PoseEditSession.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/ModelComponent.h"
#include "util/AssetDatabase.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool fail(std::string& error, const std::string& message){
        error = message;
        return false;
    }

    // the scene, open and not playing
    Scene* editableScene(Project* project, uint32_t sceneId, std::string& error){
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene){
            fail(error, "the scene is not open");
            return nullptr;
        }
        if (sceneProject->playState != ScenePlayState::STOPPED){
            fail(error, "the scene is playing: stop play mode first");
            return nullptr;
        }
        return sceneProject->scene;
    }

    bool isEntity(Scene* scene, Entity entity){
        return entity != NULL_ENTITY && scene->isEntityCreated(entity);
    }

    std::string nameOf(Scene* scene, Entity entity){
        return clipui::quotedName(scene, entity);
    }

    using clipui::readWholeFile;

    // a project-relative .anim path to write: inside the project, .anim
    bool checkClipPath(Project* project, const std::string& clipPath, fs::path& absolute, std::string& error){
        if (clipPath.empty()) return fail(error, "no clip path given (a project-relative .anim path, e.g. assets/anim/walk.anim)");
        fs::path given = editor::FileUtils::pathFromUtf8(clipPath);
        absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
        const fs::path relative = project->normalizeToProjectRelative(absolute);
        const std::string text = FileUtils::pathToGenericUtf8(relative);
        if (relative.empty() || relative.is_absolute() || text.compare(0, 2, "..") == 0){
            return fail(error, "'" + clipPath + "' is outside the project");
        }
        std::string extension = absolute.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (extension != ".anim") return fail(error, "'" + clipPath + "' is not an .anim path");
        if (!project->isInsideAssetsPath(absolute)){
            return fail(error, "'" + clipPath + "' is outside the assets folder (" +
                        FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath())) +
                        "): the runtime can't load it");
        }
        return true;
    }

    std::string projectRelative(Project* project, const fs::path& absolute){
        return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
    }

    // the entry of a file this step writes (its GUID is in the text: the database learns it on execute)
    AnimationPlayerClip fileEntry(Project* project, const fs::path& absolute, const std::string& text){
        AnimationPlayerClip entry;
        entry.clip = ClipConverters::assetRelative(project, absolute);
        entry.guid = anim::readClipGuid(text);
        return entry;
    }

    // the state name an entry plays as (AnimationPlayer's default: the file stem or sub-clip name)
    std::string stateName(const AnimationPlayerClip& entry){
        if (!entry.name.empty()) return entry.name;
        anim::SubClipReference reference;
        if (anim::parseSubClipReference(entry.clip, reference)) return reference.clip;
        return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(entry.clip).stem());
    }

    std::string readText(const fs::path& path){
        std::string text;
        readWholeFile(path, text);
        return text;
    }

    // "n/m tracks bind" for each clip that doesn't bind fully on `target` (a model sub-clip dropped
    // on a bone, a clip of another rig)
    void warnBindings(Project* project, uint32_t sceneId, Entity target, const std::vector<AnimationPlayerClip>& entries,
                      ClipActionResult& result){
        SceneProject* sceneProject = project->getScene(sceneId);
        for (const AnimationPlayerClip& entry : entries){
            ClipData data;
            std::vector<ClipWarning> ignored;
            std::string failure;
            if (anim::isSubClipReference(entry.clip)){
                if (!anim::buildModelSubClip(entry.clip, data, ignored, &failure)) continue;
            }else{
                const anim::ClipParseResult parsed = anim::parseClip(readText(project->resolveAssetPath(editor::FileUtils::pathFromUtf8(entry.clip))));
                if (!parsed.ok()) continue;
                data = *parsed.data;
            }
            const ClipValidationReport report = ClipValidation::validateInScene(project, sceneProject, data, target);
            if (report.resolved < report.total){
                ClipWarning warning;
                warning.path = entry.clip;
                warning.message = std::to_string(report.resolved) + "/" + std::to_string(report.total) + " tracks bind on " +
                                  nameOf(sceneProject->scene, target) +
                                  (report.legacyModelLayout ? " (a model under it is in the legacy layout)" : "");
                result.warnings.push_back(warning);
            }
        }
    }

    // ---- the hooks' UI (native save dialog, alerts) ----

    std::string askClipPath(Project* project, const std::string& directory, const std::string& name){
        return ClipSceneActions::askNewClipPath(project, directory, name);
    }

    void report(const std::string& title, bool ok, const ClipActionResult& result, const std::string& error){
        if (ok){
            editor::Out::info("%s", result.status.c_str());
            for (const ClipWarning& warning : result.warnings){
                editor::Out::warning("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
            }
        }else{
            editor::Out::error("%s failed: %s", title.c_str(), error.c_str());
            Backend::getApp().registerAlert(title, error);
        }
    }

    void installHooks(){
        AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();

        hooks.createNewClip = [](Project* project, uint32_t sceneId, Entity root){
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene || !isEntity(sceneProject->scene, root)) return;
            const std::string path = askClipPath(project, ClipConverters::defaultClipDirectory(project, sceneId, root),
                                                 sceneProject->scene->getEntityName(root));
            if (path.empty()) return;
            ClipActionResult result;
            std::string error;
            const bool ok = ClipSceneActions::createNewClip(project, sceneId, root, path, result, error);
            report("Create New Clip", ok, result, error);
        };

        hooks.convertToClip = [](Project* project, uint32_t sceneId, Entity animation){
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene || !isEntity(sceneProject->scene, animation)) return;
            Scene* scene = sceneProject->scene;
            // what would convert, before asking anything: only the counts are shown (they don't
            // depend on values), so a running preview and Record go on if the dialog is cancelled;
            // convertToClip stops them before it reads the keys
            ClipData data;
            ClipConvertReport preview;
            std::string error;
            if (!ClipConverters::convertEntityAnimation(scene, animation, NULL_ENTITY, data, preview, &error)){
                report("Convert to Clip", false, ClipActionResult(), error);
                return;
            }
            const Entity root = ClipConverters::defaultConvertRoot(scene, animation);
            const std::string path = askClipPath(project, ClipConverters::defaultClipDirectory(project, sceneId, root),
                                                 scene->getEntityName(animation));
            if (path.empty()) return;
            auto run = [project, sceneId, animation, path](bool remove){
                ClipActionResult result;
                std::string failure;
                const bool ok = ClipSceneActions::convertToClip(project, sceneId, animation, NULL_ENTITY, path, remove, result, failure);
                report("Convert to Clip", ok, result, failure);
            };
            std::string message = std::to_string(preview.convertedTracks) + " keyframe tracks convert to the clip, played by an "
                                  "Animation Player on " + nameOf(scene, root) + ". Remove the converted track GameObjects from "
                                  "the Animation " + nameOf(scene, animation) + "?";
            if (!preview.notConverted.empty()){
                message += "\n\nNot converted (they stay in the Animation): ";
                for (size_t i = 0; i < preview.notConverted.size() && i < 6; i++){
                    message += (i ? ", " : "") + preview.notConverted[i];
                }
                if (preview.notConverted.size() > 6) message += ", ...";
            }
            Backend::getApp().registerThreeButtonAlert("Convert to Clip", message, [run](){ run(true); }, [run](){ run(false); }, nullptr);
        };

        hooks.extractSubClip = [](Project* project, uint32_t sceneId, Entity model, const std::string& subClip){
            anim::SubClipReference reference;
            if (!anim::parseSubClipReference(subClip, reference)) return;
            const std::string path = askClipPath(project, ClipSceneActions::subClipDirectory(project, subClip), reference.clip);
            if (path.empty()) return;
            // the player that plays the model (on it or an ancestor, as the Animation window binds)
            Entity playerRoot = NULL_ENTITY;
            SceneProject* sceneProject = project->getScene(sceneId);
            if (model != NULL_ENTITY && sceneProject && sceneProject->scene && isEntity(sceneProject->scene, model)){
                playerRoot = AnimationClipController::findPlayerRoot(sceneProject->scene, model);
            }
            ClipActionResult result;
            std::string error;
            const uint64_t clock = AnimationClipController::clipCommitClock(project);
            const bool ok = ClipSceneActions::extractSubClip(project, sceneId, playerRoot, subClip, path, playerRoot != NULL_ENTITY, result, error);
            // the file alone is a clip commit of the editor UI: Ctrl+Z undoes it (9.R5c)
            if (ok && result.projectStep) AnimationClipController::markClipCommit(clock);
            report("Extract to .anim", ok, result, error);
        };

        hooks.useAnimationPlayer = [](Project* project, uint32_t sceneId, Entity model){
            ClipActionResult result;
            std::string error;
            const bool ok = ClipSceneActions::useAnimationPlayer(project, sceneId, model, result, error);
            report("Use Animation Player", ok, result, error);
        };

        hooks.addToAnimationPlayer = [](Project* project, uint32_t sceneId, Entity model, const std::string& subClip){
            ClipActionResult result;
            std::string error;
            const bool ok = ClipSceneActions::addClipsToPlayer(project, sceneId, model, {subClip}, result, error);
            report("Add to Animation Player", ok, result, error);
        };

        animationClipAppHooks().dropClips = [](Project* project, uint32_t sceneId, Entity target, const std::vector<std::string>& paths){
            ClipActionResult result;
            std::string error;
            const bool ok = ClipSceneActions::addClipsToPlayer(project, sceneId, target, paths, result, error);
            if (ok){
                editor::Out::info("%s", result.status.c_str());
                for (const ClipWarning& warning : result.warnings){
                    editor::Out::warning("%s: %s", warning.path.c_str(), warning.message.c_str());
                }
            }else{
                editor::Out::error("Clip drop: %s", error.c_str());
            }
            return ok;
        };
    }

    struct InstallClipActionHooks{
        InstallClipActionHooks(){ installHooks(); }
    } installClipActionHooks;

}

std::string ClipSceneActions::newClipText(Project* project, ClipData data, const std::string& keepGuidOf){
    std::string guid;
    if (!keepGuidOf.empty()){
        std::string text;
        if (readWholeFile(editor::FileUtils::pathFromUtf8(keepGuidOf), text)) guid = anim::readClipGuid(text);
    }
    if (!anim::isClipGuid(guid)){
        const AssetDatabase* database = project ? &project->getAssetDatabase() : nullptr;
        guid = data.guid;
        while (!anim::isClipGuid(guid) || (database && database->contains(guid))){
            guid = AssetDatabase::generateGuid();
        }
    }
    data.guid = guid;
    anim::normalize(data, nullptr);
    return anim::writeClip(data);
}

void ClipSceneActions::appendPlayerCommands(Project* project, uint32_t sceneId, Entity root, const std::vector<AnimationPlayerClip>& entries,
                                            std::vector<Command*>& commands, ClipActionResult& result){
    Scene* scene = project->getScene(sceneId)->scene;
    result.player = root;
    std::vector<AnimationPlayerClip> list;
    if (const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(root)){
        list = player->clips;
        result.addedPlayer = false;
    }else{
        result.addedPlayer = true;
        commands.push_back(new AddComponentCmd(project, sceneId, root, ComponentType::AnimationPlayerComponent));
    }
    const size_t before = list.size();
    result.entries.clear();
    for (const AnimationPlayerClip& entry : entries){
        size_t found = list.size();
        for (size_t i = 0; i < list.size(); i++){
            const bool sameGuid = !entry.guid.empty() && list[i].guid == entry.guid;
            if (sameGuid || list[i].clip == entry.clip){
                found = i;
                break;
            }
        }
        if (found == list.size()) list.push_back(entry);
        result.entries.push_back(found);
    }
    result.appended = list.size() - before;
    if (result.appended > 0){
        commands.push_back(new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneId, root, ComponentType::AnimationPlayerComponent,
                                                                             "clips", list));
    }
}

bool ClipSceneActions::runStep(Project* project, uint32_t sceneId, const std::vector<std::pair<std::string, std::string>>& files,
                               std::vector<Command*> commands, Entity playerRoot, ClipActionResult& result, std::string& error){
    std::vector<CreateClipCmd::File> written;
    for (const auto& file : files){
        fs::path path = editor::FileUtils::pathFromUtf8(file.first);
        if (!path.is_absolute()) path = project->getProjectPath() / path;
        written.push_back({path.lexically_normal(), file.second});
    }
    auto* command = new CreateClipCmd(project, sceneId, std::move(written), std::move(commands));
    command->setPlayerRoot(playerRoot);
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    // the models on or under the player's root without their node hierarchy now (the player's
    // AddComponentCmd or this command reloads them)
    Scene* scene = project->getScene(sceneId)->scene;
    std::vector<Entity> legacy;
    if (playerRoot != NULL_ENTITY && scene->isEntityCreated(playerRoot)){
        // (MeshSystem::needsNodeHierarchyReload without the player, which may not be there yet)
        legacy = ClipValidation::validate(ClipData(), scene, playerRoot).legacyModels;
    }
    // the history executes it, and deletes it when that fails
    if (!CommandHandle::get(sceneId)->addCommand(command)){
        return fail(error, failure->empty() ? std::string("the step failed") : *failure);
    }
    result.reloadedModels = 0;
    for (Entity entity : legacy){
        const ModelComponent* model = scene->isEntityCreated(entity) ? scene->findComponent<ModelComponent>(entity) : nullptr;
        if (model && !model->nodesIdMapping.empty()) result.reloadedModels++;
    }
    return true;
}

bool ClipSceneActions::createNewClip(Project* project, uint32_t sceneId, Entity root, const std::string& clipPath,
                                     ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    if (!isEntity(scene, root)) return fail(error, "no GameObject to animate");
    fs::path absolute;
    if (!checkClipPath(project, clipPath, absolute, error)) return false;
    std::error_code ec;
    if (fs::exists(absolute, ec)) return fail(error, "'" + projectRelative(project, absolute) + "' already exists: a new clip never replaces a file, choose another name");
    // (after the checks: a refused step leaves a running preview and Record alone)
    stopPreviewsFor(sceneId, {root});

    ClipData data;
    if (const ModelComponent* model = scene->findComponent<ModelComponent>(root)){
        data.previewModel = model->filename;
    }
    const std::string text = newClipText(project, data);
    std::vector<Command*> commands;
    appendPlayerCommands(project, sceneId, root, {fileEntry(project, absolute, text)}, commands, result);
    result.clip = projectRelative(project, absolute);
    if (!runStep(project, sceneId, {{result.clip, text}}, std::move(commands), root, result, error)) return false;
    result.status = "Created '" + result.clip + "' on the Animation Player of " + nameOf(scene, root) + " (one scene undo step)";
    return true;
}

bool ClipSceneActions::convertToClip(Project* project, uint32_t sceneId, Entity animation, Entity root, const std::string& clipPath,
                                     bool removeConvertedTracks, ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    if (!isEntity(scene, animation)) return fail(error, "no Animation to convert");
    fs::path absolute;
    if (!checkClipPath(project, clipPath, absolute, error)) return false;
    std::error_code ec;
    if (fs::exists(absolute, ec)) return fail(error, "'" + projectRelative(project, absolute) + "' already exists: a new clip never replaces a file, choose another name");
    if (root == NULL_ENTITY){
        root = ClipConverters::defaultConvertRoot(scene, animation);
    }
    ClipData data;
    std::string failure;
    {
        // a dry run first: an Animation with nothing to convert leaves the previews alone
        ClipData probe;
        ClipConvertReport report;
        if (!ClipConverters::convertEntityAnimation(scene, animation, root, probe, report, &failure)) return fail(error, failure);
    }
    // the keys come from the scene's values (a hold key at the playhead): never a previewed one
    stopPreviewsFor(sceneId, {animation, root});
    if (!ClipConverters::convertEntityAnimation(scene, animation, root, data, result.convert, &failure)){
        return fail(error, failure);
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject->filepath.empty()){
        data.source.path = projectRelative(project, sceneProject->filepath.is_absolute() ? sceneProject->filepath
                                                                                          : project->getProjectPath() / sceneProject->filepath);
    }
    const std::string text = newClipText(project, data);
    result.warnings = result.convert.warnings;
    if (!removeConvertedTracks){
        for (Entity model : result.convert.legacyRootModels){
            ClipWarning warning;
            warning.message = "the kept tracks of " + nameOf(scene, model) + " still write its legacy local values: after its reload in "
                              "the node hierarchy they apply the file's root transform twice (remove them, or key them again)";
            result.warnings.push_back(warning);
        }
    }

    std::vector<Command*> commands;
    appendPlayerCommands(project, sceneId, root, {fileEntry(project, absolute, text)}, commands, result);
    if (removeConvertedTracks && !result.convert.convertedTrackEntities.empty()){
        // the frames go first, then their entities (undo: the entities, then the frames)
        std::vector<ActionFrame> kept;
        for (const ActionFrame& frame : scene->getComponent<AnimationComponent>(animation).actions){
            const auto& converted = result.convert.convertedTrackEntities;
            if (std::find(converted.begin(), converted.end(), frame.action) == converted.end()) kept.push_back(frame);
        }
        commands.push_back(new PropertyCmd<std::vector<ActionFrame>>(project, sceneId, animation, ComponentType::AnimationComponent,
                                                                     "actions", kept));
        commands.push_back(new DeleteEntityCmd(project, sceneId, result.convert.convertedTrackEntities, true));
        result.removedEntities = result.convert.convertedTrackEntities.size();
    }
    result.clip = projectRelative(project, absolute);
    if (!runStep(project, sceneId, {{result.clip, text}}, std::move(commands), root, result, error)) return false;
    result.status = "Converted " + std::to_string(result.convert.convertedTracks) + " tracks of the Animation " + nameOf(scene, animation) +
                    " to '" + result.clip + "', played by the Animation Player of " + nameOf(scene, root) +
                    (result.removedEntities ? " (converted tracks removed)" : "") +
                    (result.convert.notConverted.empty() ? "" : "; " + std::to_string(result.convert.notConverted.size()) +
                     " actions not converted stay in the Animation") + " (one scene undo step)";
    return true;
}

bool ClipSceneActions::extractSubClip(Project* project, uint32_t sceneId, Entity model, const std::string& subClip,
                                      const std::string& clipPath, bool usePlayer, ClipActionResult& result, std::string& error,
                                      const ClipData* edited){
    result = ClipActionResult();
    if (!project) return fail(error, "no project");
    fs::path absolute;
    if (!checkClipPath(project, clipPath, absolute, error)) return false;
    // the reference as players store it (asset-relative model)
    const std::string reference = ClipConverters::playerEntry(project, subClip).clip;
    ClipData data;
    std::string failure;
    anim::SubClipReference parsed;
    anim::parseSubClipReference(reference, parsed);
    if (edited){
        data = *edited;
    }else{
        if (!ClipConverters::extractModelSubClip(reference, data, result.warnings, &failure)) return fail(error, failure);
        data.previewModel = parsed.model;
    }
    result.clip = projectRelative(project, absolute);
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);

    Scene* scene = nullptr;
    if (usePlayer && model != NULL_ENTITY){
        scene = editableScene(project, sceneId, error);
        if (!scene) return false;
        if (!isEntity(scene, model)) return fail(error, "the model GameObject is gone");
    }
    if (!scene || exists){
        // the file alone: a project step (into an existing file: a commit, its GUID kept)
        if (scene && exists){
            return fail(error, "'" + result.clip + "' already exists: extract into a new file to switch the Animation Player to it");
        }
        result.projectStep = true;
        if (exists){
            std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, result.clip, &failure);
            if (!document) return fail(error, failure);
            if (!document->commit(data, "Extract to .anim", "", &failure, &result.warnings)) return fail(error, failure);
            result.status = "Extracted '" + reference + "' into '" + result.clip + "' (one project undo step)";
            return true;
        }
        if (!AnimationClipDocument::create(project, result.clip, data, "Extract to .anim", &failure)) return fail(error, failure);
        result.status = "Extracted '" + reference + "' to '" + result.clip + "' (one project undo step)";
        return true;
    }

    // the model's entries of that sub-clip switch to the file (their state names kept); the
    // previews of the player's hierarchy restore first (after the checks)
    stopPreviewsFor(sceneId, {model});
    const std::string text = newClipText(project, data);
    const AnimationPlayerClip file = fileEntry(project, absolute, text);
    std::vector<Command*> commands;
    result.player = model;
    if (const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(model)){
        std::vector<AnimationPlayerClip> list = player->clips;
        std::vector<anim::ModelClipInfo> clips;
        std::string listError;
        anim::listModelFileClips(parsed.model, clips, &listError);
        auto sameSubClip = [&](const std::string& clip){
            anim::SubClipReference other;
            if (!anim::parseSubClipReference(clip, other) || other.model != parsed.model) return false;
            if (other.clip == parsed.clip) return true;
            // a name and the index of the same animation
            for (const anim::ModelClipInfo& info : clips){
                const bool a = info.name == other.clip || std::to_string(info.index) == other.clip;
                const bool b = info.name == parsed.clip || std::to_string(info.index) == parsed.clip;
                if (a && b) return true;
            }
            return false;
        };
        for (size_t i = 0; i < list.size(); i++){
            if (!sameSubClip(list[i].clip)) continue;
            const std::string name = stateName(list[i]);
            list[i].clip = file.clip;
            list[i].guid = file.guid;
            if (stateName(list[i]) != name) list[i].name = name;
            result.entries.push_back(i);
        }
        if (result.entries.empty()){
            result.entries.push_back(list.size());
            list.push_back(file);
            result.appended = 1;
        }
        commands.push_back(new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneId, model, ComponentType::AnimationPlayerComponent,
                                                                             "clips", list));
    }else{
        appendPlayerCommands(project, sceneId, model, {file}, commands, result);
    }
    if (!runStep(project, sceneId, {{result.clip, text}}, std::move(commands), model, result, error)) return false;
    result.status = "Extracted '" + reference + "' to '" + result.clip + "'; the Animation Player of " + nameOf(scene, model) +
                    " plays the file now (one scene undo step)";
    return true;
}

bool ClipSceneActions::writeOptimizedClips(Project* project, uint32_t sceneId, Entity player, const std::vector<OptimizedClip>& clips,
                                           ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    const AnimationPlayerComponent* component = isEntity(scene, player) ? scene->findComponent<AnimationPlayerComponent>(player) : nullptr;
    if (!component) return fail(error, "root has no Animation Player to switch: leave root out to write the files alone");
    stopPreviewsFor(sceneId, {player});
    std::vector<AnimationPlayerClip> list = component->clips;
    std::vector<std::pair<std::string, std::string>> files;
    for (const OptimizedClip& clip : clips){
        const AnimationPlayerClip file = fileEntry(project, clip.file, clip.text);
        for (const ClipUser& user : findClipUsers(project, clip.source)){
            if (user.prefabAsset || user.sceneId != sceneId || user.entity != player || user.entry >= list.size()) continue;
            const std::string name = stateName(list[user.entry]);
            list[user.entry].clip = file.clip;
            list[user.entry].guid = file.guid;
            if (stateName(list[user.entry]) != name) list[user.entry].name = name;
            result.entries.push_back(user.entry);
        }
        files.push_back({editor::FileUtils::pathToGenericUtf8(clip.file), clip.text});
    }
    std::vector<Command*> commands;
    if (!result.entries.empty()){
        commands.push_back(new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneId, player, ComponentType::AnimationPlayerComponent,
                                                                             "clips", list));
    }
    result.player = player;
    return runStep(project, sceneId, files, std::move(commands), player, result, error);
}

std::string ClipSceneActions::humanoidClipPath(Project* project, const std::string& source){
    anim::SubClipReference parsed;
    std::string directory;
    std::string name;
    if (anim::parseSubClipReference(source, parsed)){
        directory = subClipDirectory(project, source);
        name = parsed.clip;
    }else{
        const fs::path file = project->resolveAssetPath(fs::u8path(source));
        directory = projectRelative(project, file.parent_path());
        name = editor::FileUtils::pathToUtf8(file.stem());
    }
    const std::string base = (directory.empty() ? std::string() : directory + "/") + name + " (Humanoid)";
    std::string path = base + ".anim";
    std::error_code ec;
    for (int k = 2; fs::exists(project->getProjectPath() / fs::u8path(path), ec) && k < 1000; k++){
        path = base + " " + std::to_string(k) + ".anim";
    }
    return path;
}

bool ClipSceneActions::convertToHumanoid(Project* project, uint32_t sceneId, Entity player, size_t entry, const std::string& source,
                                         const std::string& model, const std::string& clipPath, ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    if (!project) return fail(error, "no project");
    fs::path absolute;
    if (!checkClipPath(project, clipPath, absolute, error)) return false;
    std::error_code ec;
    if (fs::exists(absolute, ec)) return fail(error, "'" + projectRelative(project, absolute) + "' already exists: convert into a new file");
    // the source clip as players store it
    const std::string reference = ClipConverters::playerEntry(project, source).clip;
    ClipData data;
    std::string failure;
    if (anim::isSubClipReference(reference)){
        if (!anim::buildModelSubClip(reference, data, result.warnings, &failure)) return fail(error, failure);
    }else{
        std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, projectRelative(project, project->resolveAssetPath(fs::u8path(reference))), &failure);
        if (!document) return fail(error, failure);
        data = document->data();
    }
    const std::string rig = model.empty() ? ClipConverters::humanoidSourceModel(reference, data) : ClipConverters::playerEntry(project, model).clip;
    if (rig.empty()) return fail(error, "'" + reference + "' names no source model: choose the model it was made on (or set its Preview Model)");
    ClipData converted;
    if (!ClipConverters::convertToHumanoid(data, rig, converted, result.warnings, &failure)) return fail(error, failure);
    result.clip = projectRelative(project, absolute);

    Scene* scene = nullptr;
    if (player != NULL_ENTITY){
        scene = editableScene(project, sceneId, error);
        if (!scene) return false;
        if (!isEntity(scene, player)) return fail(error, "the Animation Player's GameObject is gone");
        const AnimationPlayerComponent* component = scene->findComponent<AnimationPlayerComponent>(player);
        if (!component || entry >= component->clips.size()) return fail(error, "the Animation Player entry is gone");
    }
    if (!scene){
        result.projectStep = true;
        if (!AnimationClipDocument::create(project, result.clip, converted, "Convert to Humanoid Clip", &failure)) return fail(error, failure);
        result.status = "Converted '" + reference + "' to the humanoid clip '" + result.clip + "' on " + rig + " (one project undo step)";
        return true;
    }
    // Use in this player: the entry plays the file (its state name kept), one scene step
    stopPreviewsFor(sceneId, {player});
    const std::string text = newClipText(project, converted);
    const AnimationPlayerClip file = fileEntry(project, absolute, text);
    std::vector<AnimationPlayerClip> list = scene->getComponent<AnimationPlayerComponent>(player).clips;
    const std::string name = stateName(list[entry]);
    list[entry].clip = file.clip;
    list[entry].guid = file.guid;
    list[entry].retarget = false;   // humanoid already
    if (stateName(list[entry]) != name) list[entry].name = name;
    result.entries.push_back(entry);
    result.player = player;
    std::vector<Command*> commands;
    commands.push_back(new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneId, player, ComponentType::AnimationPlayerComponent,
                                                                         "clips", list));
    if (!runStep(project, sceneId, {{result.clip, text}}, std::move(commands), player, result, error)) return false;
    result.status = "Converted '" + reference + "' to the humanoid clip '" + result.clip + "'; the Animation Player of " + nameOf(scene, player) +
                    " plays it now (one scene undo step)";
    return true;
}

bool ClipSceneActions::useAnimationPlayer(Project* project, uint32_t sceneId, Entity model, ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    UseAnimationPlayerPlan plan;
    std::string failure;
    if (!ClipConverters::planUseAnimationPlayer(scene, model, plan, &failure)) return fail(error, failure);
    stopPreviewsFor(sceneId, {model});
    // the stored form (a model loaded from an absolute path: asset-relative)
    for (AnimationPlayerClip& entry : plan.clips) entry = ClipConverters::playerEntry(project, entry.clip);

    const ModelComponent& component = scene->getComponent<ModelComponent>(model);
    const bool animationEntities = component.animationEntities;
    const bool listsAnimations = !component.animations.empty();
    std::vector<Command*> commands;
    if (animationEntities){
        commands.push_back(new PropertyCmd<bool>(project, sceneId, model, ComponentType::ModelComponent, "animationEntities", false));
    }
    if (listsAnimations){
        commands.push_back(new PropertyCmd<std::vector<Entity>>(project, sceneId, model, ComponentType::ModelComponent, "animations",
                                                                std::vector<Entity>()));
    }
    if (!plan.removeEntities.empty()){
        commands.push_back(new DeleteEntityCmd(project, sceneId, plan.removeEntities, true));
        result.removedEntities = plan.removeEntities.size();
    }
    appendPlayerCommands(project, sceneId, model, plan.clips, commands, result);
    if (!runStep(project, sceneId, {}, std::move(commands), model, result, error)) return false;
    result.status = "The Animation Player of " + nameOf(scene, model) + " plays the model's " + std::to_string(plan.clips.size()) +
                    " animations as clips; " + std::to_string(plan.removeEntities.size()) + " entity animation GameObjects removed" +
                    (result.reloadedModels ? ", the model reloaded in its node hierarchy" : "") + " (one scene undo step)";
    return true;
}

bool ClipSceneActions::addClipsToPlayer(Project* project, uint32_t sceneId, Entity target, const std::vector<std::string>& clips,
                                        ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    if (!isEntity(scene, target)) return fail(error, "no GameObject to add the clips to");
    if (clips.empty()) return fail(error, "no clips given");

    std::vector<AnimationPlayerClip> entries;
    for (const std::string& clip : clips){
        if (anim::isSubClipReference(clip)){
            AnimationPlayerClip entry = ClipConverters::playerEntry(project, clip);
            anim::SubClipReference reference;
            anim::parseSubClipReference(entry.clip, reference);
            std::vector<anim::ModelClipInfo> infos;
            std::string listError;
            if (!anim::listModelFileClips(reference.model, infos, &listError)) return fail(error, listError);
            bool found = false;
            for (const anim::ModelClipInfo& info : infos){
                found = found || info.name == reference.clip || std::to_string(info.index) == reference.clip;
            }
            if (!found) return fail(error, "'" + reference.model + "' has no animation '" + reference.clip + "'");
            entries.push_back(entry);
            continue;
        }
        if (anim::isModelFilePath(clip)){
            // a model file: every sub-clip of it
            const std::string model = ClipConverters::playerEntry(project, clip).clip;
            std::vector<anim::ModelClipInfo> infos;
            std::string listError;
            if (!anim::listModelFileClips(model, infos, &listError)) return fail(error, listError);
            if (infos.empty()) return fail(error, "'" + model + "' has no animations");
            for (const anim::ModelClipInfo& info : infos){
                AnimationPlayerClip entry;
                entry.clip = anim::formatSubClipReference(model, info.name);
                entries.push_back(entry);
            }
            continue;
        }
        std::string extension = editor::FileUtils::pathFromUtf8(clip).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (extension != ".anim"){
            return fail(error, "'" + clip + "' is not an animation clip (.anim, a model file, or \"<model>#<clip>\")");
        }
        AnimationPlayerClip entry = ClipConverters::playerEntry(project, clip);
        const fs::path file = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(entry.clip));
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return fail(error, "'" + clip + "' does not exist");
        if (!project->isInsideAssetsPath(file)) return fail(error, "'" + clip + "' is outside the assets folder");
        const anim::ClipParseResult parsed = anim::parseClip(readText(file));
        if (!parsed.ok()) return fail(error, "'" + clip + "' is not a valid animation clip (" + parsed.error.toString() + ")");
        entries.push_back(entry);
    }

    std::vector<Command*> commands;
    appendPlayerCommands(project, sceneId, target, entries, commands, result);
    if (commands.empty()){
        // already listed; a legacy-layout model under the player is still switched (R3)
        if (!PlayerModelLayout::modelsNeedingReload(project, sceneId, target).empty()){
            stopPreviewsFor(sceneId, {target});
            if (!runStep(project, sceneId, {}, {}, target, result, error)) return false;
        }
        result.status = "The Animation Player of " + nameOf(scene, target) + " already lists " +
                        (entries.size() == 1 ? "that clip" : "those clips") +
                        (result.reloadedModels ? std::string(" (the model reloaded in its node hierarchy, one scene undo step)") : std::string());
        warnBindings(project, sceneId, target, entries, result);
        return true;
    }
    // every clip checked and something to do: the previews of the target's hierarchy restore first
    stopPreviewsFor(sceneId, {target});
    if (!runStep(project, sceneId, {}, std::move(commands), target, result, error)) return false;
    result.status = std::string(result.addedPlayer ? "Added an Animation Player to " : "The Animation Player of ") + nameOf(scene, target) +
                    (result.addedPlayer ? " with " : " lists ") + std::to_string(result.appended) + " more clip" +
                    (result.appended == 1 ? "" : "s") + " (one scene undo step)";
    warnBindings(project, sceneId, target, entries, result);
    return true;
}

bool ClipSceneActions::reloadModelsForPlayer(Project* project, uint32_t sceneId, Entity root, ClipActionResult& result, std::string& error){
    result = ClipActionResult();
    Scene* scene = editableScene(project, sceneId, error);
    if (!scene) return false;
    if (!isEntity(scene, root)) return fail(error, "no GameObject");
    if (PlayerModelLayout::modelsNeedingReload(project, sceneId, root).empty()){
        return fail(error, "no model on or under " + nameOf(scene, root) + " needs a reload (none is in the legacy layout under an "
                    "Animation Player)");
    }
    stopPreviewsFor(sceneId, {root});
    result.player = root;
    if (!runStep(project, sceneId, {}, {}, root, result, error)) return false;
    if (result.reloadedModels == 0){
        CommandHandle::get(sceneId)->undo();
        CommandHandle::get(sceneId)->freeRedo();
        return fail(error, "the models could not be reloaded (see the Console)");
    }
    result.status = "Reloaded " + std::to_string(result.reloadedModels) + " model" + (result.reloadedModels == 1 ? "" : "s") +
                    " under " + nameOf(scene, root) + " in the node hierarchy (one scene undo step)";
    return true;
}

void ClipSceneActions::stopPreviewsFor(uint32_t sceneId, std::vector<Entity> roots){
    roots.erase(std::remove(roots.begin(), roots.end(), NULL_ENTITY), roots.end());
    if (roots.empty()) return;
    PoseEditSession::suspendActiveFor(sceneId, roots);
}

std::string ClipSceneActions::askNewClipPath(Project* project, const std::string& directory, const std::string& name){
    fs::path start = project->getProjectPath();
    if (!directory.empty()){
        const fs::path given = editor::FileUtils::pathFromUtf8(directory);
        const fs::path absolute = (given.is_absolute() ? given : start / given).lexically_normal();
        const fs::path relative = project->normalizeToProjectRelative(absolute);
        if (!relative.empty() && !relative.is_absolute() && FileUtils::pathToGenericUtf8(relative).compare(0, 2, "..") != 0){
            start = absolute;
        }
    }
    const std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToGenericUtf8(start), (name.empty() ? "New Animation" : name) + ".anim",
                                                           FILE_DIALOG_ANIMATION_CLIP);
    if (chosen.empty()) return "";
    fs::path path = editor::FileUtils::pathFromUtf8(chosen);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (extension != ".anim") path += ".anim";
    return FileUtils::pathToGenericUtf8(path);
}

std::string ClipSceneActions::subClipDirectory(Project* project, const std::string& subClip){
    anim::SubClipReference reference;
    if (!project || !anim::parseSubClipReference(subClip, reference)) return "";
    const fs::path modelFile = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(reference.model));
    const fs::path directory = project->normalizeToProjectRelative(modelFile.parent_path());
    const std::string text = FileUtils::pathToGenericUtf8(directory);
    if (directory.is_absolute() || text.compare(0, 2, "..") == 0) return "";
    return text;
}
