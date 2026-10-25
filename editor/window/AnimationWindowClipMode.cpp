// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's mode switch (animation clips spec 4.4, 9.R31): which mode the
// selection gives, the hooks opening a clip or an entity Animation in the window (Project window
// double-click, Inspector buttons) and the MCP window test hook's window part.
// Precedence, re-evaluated when the scene's selection changes (per scene):
//   1. a selected Animation entity (AnimationComponent): entity mode on it;
//   2. a GameObject with an Animation Player on it or an ancestor: clip mode on that root;
//   3. a GameObject without one that entity Animations target (glTF entity animations): entity
//      mode on the first of them (the dropdown also offers Create New Clip...);
//   4. any other GameObject: clip mode's empty state ("create an Animation Clip");
//   5. nothing selected: the mode stays (a clip opened target-less keeps showing).
// Entity mode keeps its own "first Animation of the scene" fallback; clip mode never uses it.

#include "AnimationWindow.h"
#include "AnimationWindowClip.h"

#include "Out.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "animation/PreviewOwner.h"
#include "util/EntityPath.h"

using namespace jasperengine;
using namespace jasperengine::editor;

editor::AnimationWindow::~AnimationWindow(){
    AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    hooks.openClip = nullptr;
    hooks.openEntityAnimation = nullptr;
    animationClipAppHooks().windowTestHook = nullptr;
    // the entity preview's PreviewOwner stop captures this window
    releasePreviewOwners();
}

bool editor::AnimationWindow::moveFrameToLane(SceneProject* sceneProject, size_t frameIndex, uint32_t lane, std::string* error){
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    AnimationComponent* anim = scene && selectedEntity != NULL_ENTITY && scene->isEntityCreated(selectedEntity)
        ? scene->findComponent<AnimationComponent>(selectedEntity) : nullptr;
    if (!anim || frameIndex >= anim->actions.size()){
        if (error) *error = "no such frame of the shown Animation";
        return false;
    }
    autoAssignTracks(*anim, sceneProject);
    // a frame drag released on `lane` at the same time
    isDraggingFrame = true;
    draggingFrameIndex = (int)frameIndex;
    dragStartTime = anim->actions[frameIndex].startTime;
    dragStartTrack = laneOf(*anim, frameIndex);
    draggingLane = (int)lane;
    finishTimelineDrag(scene, sceneProject, true);
    return true;
}

void editor::AnimationWindow::installClipHooks(){
    AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    hooks.openClip = [this](Project*, uint32_t sceneId, Entity root, const std::string& clip){
        openClip(sceneId, root, clip);
    };
    hooks.openEntityAnimation = [this](Project*, uint32_t sceneId, Entity animation){
        openEntityAnimation(sceneId, animation);
    };
    animationClipAppHooks().windowTestHook = [this](Project*, const nlohmann::json& hook, nlohmann::json& out, std::string& error){
        return handleTestHook(hook, out, error);
    };
}

bool editor::AnimationWindow::isClipMode() const{
    return clipModeActive;
}

void editor::AnimationWindow::openClip(uint32_t sceneId, Entity root, const std::string& clip){
    setOpen(true);
    focusRequested = true;
    stopTransientPreviews();
    clipModeActive = true;
    std::string error;
    if (!clipMode->open(sceneId, root, clip, &error)){
        Out::error("Animation window: can't open '" + clip + "': " + error);
    }
}

void editor::AnimationWindow::openEntityAnimation(uint32_t sceneId, Entity animation){
    setOpen(true);
    focusRequested = true;
    clipMode->stopPreview();
    clipModeActive = false;
    selectEntity(animation, sceneId);
}

void editor::AnimationWindow::applySelection(SceneProject* sceneProject, const std::vector<Entity>& selected){
    // while recording the window keeps its clip and root whatever is selected (Unity's lock):
    // selecting another GameObject to move it never ends Record; the selection applies after
    if (clipModeActive && clipMode->isRecording()) return;
    lastSelectionScene = sceneProject->id;
    lastSelection = selected;
    Scene* scene = sceneProject->scene;

    auto toEntityMode = [&](Entity animation){
        if (clipModeActive) clipMode->stopPreview();
        clipModeActive = false;
        if (selectedEntity != animation || selectedSceneId != sceneProject->id) selectEntity(animation, sceneProject->id);
        if (!isWindowVisible) hasNotification = true;
    };
    for (Entity entity : selected){
        if (scene->isEntityCreated(entity) && scene->findComponent<AnimationComponent>(entity)){
            toEntityMode(entity);
            return;
        }
    }
    if (selected.empty() || !scene->isEntityCreated(selected.front())){
        hasNotification = false;
        return;
    }
    const Entity first = selected.front();
    const Entity playerRoot = AnimationClipController::findPlayerRoot(scene, first);
    if (playerRoot == NULL_ENTITY){
        // entity Animations of this GameObject (a glTF model's) stay in entity mode
        auto animations = scene->getComponentArray<AnimationComponent>();
        for (size_t i = 0; i < animations->size(); i++){
            const Entity animation = animations->getEntity(i);
            const ActionComponent* action = scene->findComponent<ActionComponent>(animation);
            if (action && action->target == first){
                toEntityMode(animation);
                return;
            }
        }
    }
    if (!clipModeActive && isPreviewing){
        // the entity-mode preview restores before clip mode shows
        stopTransientPreviews();
    }
    clipModeActive = true;
    clipMode->bind(sceneProject->id, first);
    hasNotification = !isWindowVisible && playerRoot != NULL_ENTITY;
}

bool editor::AnimationWindow::updateMode(SceneProject* sceneProject){
    uint32_t requestedScene = 0;
    Entity animation = NULL_ENTITY;
    if (clipMode->takeEntityAnimationRequest(requestedScene, animation)){
        clipMode->stopPreview();
        clipModeActive = false;
        selectEntity(animation, requestedScene);
    }
    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (sceneProject->id != lastSelectionScene || selected != lastSelection){
        applySelection(sceneProject, selected);
    }
    return clipModeActive;
}

void editor::AnimationWindow::drawClipMode(SceneProject* sceneProject){
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    clipMode->draw(sceneProject, focused);
}

bool editor::AnimationWindow::handleTestHook(const nlohmann::json& hook, nlohmann::json& out, std::string& error){
    if (!hook.is_object()){
        error = "an object {entity, clip, root, state, time, preview, select, drag, ...} (see test_animation_window.py)";
        return false;
    }
    if (!AnimationWindowClip::checkHookKeys(hook, true, error)) return false;
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene){
        error = "no scene is open";
        return false;
    }
    auto resolve = [&](const nlohmann::json& ref, Entity& entity) -> bool{
        if (!ref.is_string()){
            error = "a GameObject name or path (string)";
            return false;
        }
        const std::vector<Entity> found = EntityPath::resolve(*sceneProject, ref.get<std::string>());
        if (found.size() != 1){
            error = found.empty() ? "no GameObject '" + ref.get<std::string>() + "'" : "'" + ref.get<std::string>() + "' is ambiguous";
            return false;
        }
        entity = found.front();
        return true;
    };
    setOpen(true);
    // the selection decides the mode, as a click in the Hierarchy does
    if (hook.contains("entity")){
        if (hook["entity"].is_null()){
            project->clearSelectedEntities(sceneProject->id);
        }else{
            Entity entity = NULL_ENTITY;
            if (!resolve(hook["entity"], entity)){
                error = "entity: " + error;
                return false;
            }
            project->replaceSelectedEntities(sceneProject->id, {entity});
        }
        applySelection(sceneProject, project->getSelectedEntities(sceneProject->id));
    }
    if (hook.contains("entity_animation")){
        Entity animation = NULL_ENTITY;
        if (!resolve(hook["entity_animation"], animation)){
            error = "entity_animation: " + error;
            return false;
        }
        openEntityAnimation(sceneProject->id, animation);
    }
    // the Project window's double-click (target-less) or the Inspector's open (with root)
    if (hook.contains("clip")){
        if (!hook["clip"].is_string()){
            error = "clip: a .anim path or \"<model>#<clip>\"";
            return false;
        }
        Entity root = NULL_ENTITY;
        if (hook.contains("root") && !resolve(hook["root"], root)){
            error = "root: " + error;
            return false;
        }
        stopTransientPreviews();
        clipModeActive = true;
        if (!clipMode->open(sceneProject->id, root, hook["clip"].get<std::string>(), &error)){
            error = "clip: " + error;
            return false;
        }
    }
    if (hook.contains("entity_play")){
        // the entity mode's preview (play / stop), as its Play and Stop buttons
        if (!hook["entity_play"].is_boolean()){
            error = "entity_play: true plays the entity Animation's preview, false stops it";
            return false;
        }
        if (clipModeActive || selectedEntity == NULL_ENTITY){
            error = "entity_play: the window is not showing an entity Animation";
            return false;
        }
        if (hook["entity_play"].get<bool>()) externalPlay(selectedEntity, selectedSceneId);
        else externalStop();
    }
    if (hook.contains("entity_move_frame")){
        const nlohmann::json& move = hook["entity_move_frame"];
        if (clipModeActive || !move.is_object() || !move.contains("index") || !move.contains("lane") ||
            !move["index"].is_number_integer() || !move["lane"].is_number_integer() || move["lane"].get<int>() < 0){
            error = "entity_move_frame: {index, lane} of the shown entity Animation";
            return false;
        }
        if (!moveFrameToLane(project->getScene(selectedSceneId), (size_t)move["index"].get<int>(), (uint32_t)move["lane"].get<int>(), &error)){
            error = "entity_move_frame: " + error;
            return false;
        }
    }
    if (clipModeActive){
        try{
            if (!clipMode->testHook(sceneProject, hook, out, error)) return false;
        }catch (const nlohmann::json::exception& e){
            // a value of the wrong type
            error = std::string("bad value: ") + e.what();
            return false;
        }
    }else{
        if (!AnimationWindowClip::checkHookKeys(hook, false, error)) return false;
        out = clipMode->stateJson(sceneProject);
        // entity mode: the clip state is not shown; name the entity Animation
        out["mode"] = "entity";
        Scene* scene = sceneProject->scene;
        out["entity_animation"] = selectedEntity != NULL_ENTITY && scene->isEntityCreated(selectedEntity)
            ? nlohmann::json(scene->getEntityName(selectedEntity)) : nlohmann::json();
        out["entity_preview"] = isPreviewing;
        Scene* shown = project->getScene(selectedSceneId) ? project->getScene(selectedSceneId)->scene : nullptr;
        nlohmann::json lanes = nlohmann::json::array();
        nlohmann::json tracks = nlohmann::json::array();
        if (shown && selectedEntity != NULL_ENTITY && shown->isEntityCreated(selectedEntity)){
            if (AnimationComponent* anim = shown->findComponent<AnimationComponent>(selectedEntity)){
                autoAssignTracks(*anim, project->getScene(selectedSceneId));
                for (size_t i = 0; i < anim->actions.size(); i++){
                    lanes.push_back(laneOf(*anim, i));
                    tracks.push_back(anim->actions[i].track);
                }
            }
        }
        // the lanes shown (view only) and the stored ActionFrame::track values
        out["entity_lanes"] = lanes;
        out["entity_tracks"] = tracks;
    }
    if (hook.value("entity_mode_entries", false)){
        // the entity mode dropdown's clip entries for the scene selection (R31)
        const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        out["entity_mode_entries"] = selected.empty() ? std::vector<std::string>() : clipMode->entityModeEntries(sceneProject, selected.front());
    }
    return true;
}
