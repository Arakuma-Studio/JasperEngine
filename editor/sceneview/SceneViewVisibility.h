// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Unity's SceneVisibility and the Layers dropdown (docs/specs/S26-scene-view.md R22, R23):
// GameObjects and layers hidden in the Scene view, or locked against picking there. The
// state is the scene's SceneViewState (saved per user); EntityMetadata::editorFlags carries it
// to the renderer and the pickers, which honour it only for the Scene view's camera. Not
// undoable, as in Unity.

#include "Project.h"

#include <cstddef>
#include <vector>

namespace jasperengine::editor::sceneview {

    // the entity, and with `children` everything below it
    void setHidden(SceneProject& sceneProject, Entity entity, bool hidden, bool children);
    void setPickLocked(SceneProject& sceneProject, Entity entity, bool locked, bool children);
    // every hidden GameObject shown again (Alt+H, the "N hidden" banner)
    void showAll(SceneProject& sceneProject);
    bool isHidden(const SceneProject& sceneProject, Entity entity);
    bool isPickLocked(const SceneProject& sceneProject, Entity entity);
    size_t hiddenCount(const SceneProject& sceneProject);

    // The view state onto the scene: the editor flags of the hidden and locked GameObjects
    // (those that still exist) and the editor camera's layer mask and editor view flag
    void applyToScene(SceneProject& sceneProject);
    void applyLayerFilter(SceneProject& sceneProject, uint32_t visible, uint32_t pickable);

    // what the Scene view draws gizmos and icons for: the GameObjects not hidden there, by
    // their own flag or their layer
    std::vector<Entity> visibleEntities(const SceneProject& sceneProject, const std::vector<Entity>& entities);
    bool hasHiddenContent(const SceneProject& sceneProject);

}
