// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (runtime-core spec 6.6)
// - Purpose: the authored state of the GameObjects an edit-mode preview of actions writes (the
//   Animation window's entity Animations, the Inspector's action preview), restored when the
//   preview seeks or stops. A value a command writes while the preview runs (an Inspector field,
//   MCP set_properties, a gizmo drag, undo / redo) is part of the authored state from then on:
//   the restore keeps it, as the clip preview does (animation clips 9.R6).
// - Writes come through ClipRecordHooks' component write listeners (before/afterComponentWrite,
//   called by PropertyCmd, ObjectTransformCmd, ComponentValuesCmd, LatticePointsCmd and
//   MorphWeightsCmd for execute, undo and redo):
//   * before a write the written value goes back to its authored value, so the command records
//     it as its undo value (never a previewed pose);
//   * after it, the new value is kept: a property path with its reflected value; a whole
//     component (property "") or a path the reflection cannot copy as the component's authored
//     YAML (the component was put back to its authored state before, so it holds no preview).
// - Thread-safety: main thread. Not copyable: it registers listeners while it holds entities.

#pragma once

#include "ecs/Entity.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    enum class ComponentType : int;

    class ActionPreviewSnapshot{
    public:
        ActionPreviewSnapshot();
        ~ActionPreviewSnapshot();
        ActionPreviewSnapshot(const ActionPreviewSnapshot&) = delete;
        ActionPreviewSnapshot& operator=(const ActionPreviewSnapshot&) = delete;

        // The types a preview of actions does not restore: the Animation and the key data, so keys
        // authored while a scrub preview is paused (Snapshot, right-click keying) are kept. The one
        // part of them a preview writes, a keyframe track's playhead (index, interpolation), is
        // still put back.
        static const std::vector<ComponentType>& readOnlyTypes();

        // Takes `entity`'s components (once; later calls for it do nothing), leaving out the
        // `excluded` types (never restored); the first entity starts listening to writes of sceneId
        void add(uint32_t sceneId, Scene* scene, Entity entity, const std::vector<ComponentType>& excluded = readOnlyTypes());
        bool contains(Entity entity) const;
        bool empty() const;
        const std::vector<Entity>& entities() const;

        // Puts every taken GameObject back to its authored state (also its preview particles;
        // relative keyframe tracks take their base again when the preview starts them)
        void restore(Scene* scene) const;

        // Forgets everything and stops listening (the preview ended, or its scene is gone)
        void clear();

        // A command wrote a taken component since the last call: the owner shows its preview
        // again (a paused one seeks to its playhead)
        bool takeChanged();

        struct Data;
    private:
        std::unique_ptr<Data> data;
    };

}
