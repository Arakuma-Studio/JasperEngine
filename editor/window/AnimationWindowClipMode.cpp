// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's mode switch (animation clips spec 4.4, 9.R31): which mode the
// selection gives, the hooks opening a clip or an entity Animation in the window (Project window
// double-click, Inspector buttons).
// Precedence, re-evaluated when the scene's selection changes (per scene):
//   1. a selected Animation entity (AnimationComponent): entity mode on it;
//   2. a GameObject with an Animation Player on it or an ancestor: clip mode on that root;
//   3. a GameObject without one that entity Animations target (glTF entity animations): entity
//      mode on the first of them (the dropdown also offers Create New Clip...);
//   4. any other GameObject: clip mode's empty state ("create an Animation Clip");
//   5. nothing selected: the mode stays (a clip opened target-less keeps showing).
// Entity mode keeps its own "first Animation of the scene" fallback; clip mode never uses it.

#include "AnimationWindow.h"
#include "audio/TimelineAudioPreview.h"
#include "sprite/SpriteSheetPool.h"
#include "AnimationWindowClip.h"

#include "Out.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "animation/PreviewOwner.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "imgui_internal.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

editor::AnimationWindow::~AnimationWindow(){
    AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    hooks.openClip = nullptr;
    hooks.openEntityAnimation = nullptr;
    // the entity preview's PreviewOwner stop captures this window
    releasePreviewOwners();
}

void editor::AnimationWindow::installClipHooks(){
    AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    hooks.openClip = [this](Project*, uint32_t sceneId, Entity root, const std::string& clip){
        openClip(sceneId, root, clip);
    };
    hooks.openEntityAnimation = [this](Project*, uint32_t sceneId, Entity animation){
        openEntityAnimation(sceneId, animation);
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

bool editor::AnimationWindow::dropClipFiles(SceneProject* sceneProject, const std::vector<std::string>& paths, std::string* error){
    // entity mode has no bound GameObject: the selected one (not an entity Animation) takes the clips
    Entity fallback = NULL_ENTITY;
    if (!clipModeActive && sceneProject && sceneProject->scene){
        const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        if (!selected.empty() && sceneProject->scene->isEntityCreated(selected.front()) &&
            !sceneProject->scene->findComponent<AnimationComponent>(selected.front())){
            fallback = selected.front();
        }
    }
    // the entity preview restores before the scene step
    if (!clipModeActive && isPreviewing) stopTransientPreviews();
    if (!clipMode->dropClips(sceneProject, fallback, paths, error)) return false;
    clipModeActive = true;
    return true;
}

void editor::AnimationWindow::acceptClipDrop(SceneProject* sceneProject){
    // the whole window is a target for .anim files (smaller targets inside it win their drops)
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (!ImGui::BeginDragDropTargetCustom(ImRect(min, ImVec2(min.x + size.x, min.y + size.y)), ImGui::GetID("##anim_clip_files_drop"))){
        return;
    }
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload && payload->IsDataType("resource_files")){
        const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
        const bool clips = std::any_of(paths.begin(), paths.end(), [](const std::string& path){
            return Util::isAnimationClipFile(path) || SpriteSheetPool::isAsepritePath(path);
        });
        if (clips && ImGui::AcceptDragDropPayload("resource_files")){
            std::string error;
            if (!dropClipFiles(sceneProject, paths, &error)) Out::error("Animation window: can't take the dropped clip: " + error);
        }
    }
    ImGui::EndDragDropTarget();
}
