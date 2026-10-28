// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's picking (docs/specs/S26-scene-view.md R11-R15): what a click hits, in every
// scene the view draws (icons first, then the triangles, terrains, UI rects and bounds of each
// layer, the top one first), the pick root of the nearest hit (its prefab instance, model or
// collapsed Hierarchy row), the candidates a click cycles through, and the click flow that
// chooses among them (root first, then drill in, then walk back through what is behind).
// The click flow and the candidate list follow OpenEngine's SceneViewController::
// PickViaClickThrough and Picking/PickRootResolver.cpp (Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License, NOTICE.md); our roots come from scene metadata.

#include "Project.h"
#include "math/Plane.h"
#include "picking/PickTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct ScenePickEntry {
        uint32_t sceneId = NULL_PROJECT_SCENE;   // the scene of the GameObject (a layer's own id)
        Entity entity = NULL_ENTITY;
        PickHit hit;                              // kind ICON for an icon

        bool operator==(const ScenePickEntry& other) const { return sceneId == other.sceneId && entity == other.entity; }
        bool operator!=(const ScenePickEntry& other) const { return !(*this == other); }
    };

    struct ScenePickRequest {
        float x = 0.0f;                 // engine pixels of the view
        float y = 0.0f;
        bool exactPick = false;         // the toolbar's Exact Pick: no roots
        bool waitForBvh = true;         // false: a mesh still building is reported pending
        float iconRadius = 10.0f;       // engine pixels (ICON_PICK_RADIUS times the overlay scale)
        // the outermost collapsed Hierarchy row holding an entity (the entity itself when none)
        std::function<Entity(uint32_t sceneId, Entity entity)> collapsedRow;
    };

    struct ScenePickResult {
        // icons, then each layer's hits top layer first (3D nearest first, 2D and UI the last
        // drawn first), one per GameObject
        std::vector<ScenePickEntry> hits;
        // what clicks at this spot cycle through: icons, then [root, the prefab roots between it
        // and the exact hit, the exact hit, the GameObjects behind it]
        std::vector<ScenePickEntry> candidates;
        // the hit that decides is a mesh whose BVH is still building (waitForBvh false)
        bool pending = false;
        ScenePickEntry pendingHit;
        // icons lead the candidates (no drilling then)
        size_t iconCount = 0;
    };

    // What a click does to the selection
    struct ScenePickClick {
        enum class Action { NONE, CLEAR, SELECT, ADD, TOGGLE };
        Action action = Action::NONE;
        ScenePickEntry target;
        int cycleIndex = -1;            // position in the candidates (the n of the "n/m" pill)
        int cycleCount = 0;
    };

    // The selection a click is decided against, and the click before it
    struct ScenePickContext {
        bool shift = false;
        bool ctrl = false;              // Cmd on macOS
        bool exactPick = false;
        // the active (last) selected GameObject and its scene, NULL_ENTITY for none
        ScenePickEntry primary;
        std::function<bool(const ScenePickEntry&)> isSelected;
        // whether `entity` is `ancestor` or below it in `sceneId`
        std::function<bool(uint32_t sceneId, Entity entity, Entity ancestor)> isInside;
        // the previous click's position and whether a Ctrl+wheel step is waiting to be confirmed
        float lastX = -1.0e9f;
        float lastY = -1.0e9f;
        bool depthAnchor = false;
        float x = 0.0f;
        float y = 0.0f;
    };

    class ScenePicker {
    public:
        // clicks within this many pixels of the previous one cycle (R13)
        static constexpr float SAME_SPOT_PIXELS = 4.0f;

        explicit ScenePicker(Project* project);

        // Everything the view of viewSceneId hits at (x, y)
        ScenePickResult pick(uint32_t viewSceneId, const ScenePickRequest& request);

        // R13, R14: what a click with these hits does, given the selection
        static ScenePickClick decideClick(const ScenePickResult& result, const ScenePickContext& context);

        // R12: the outermost of the hit's prefab instance roots (nested ones outward), its model
        // root and its collapsed Hierarchy row; never the root of an open Prefab Mode stage
        Entity pickRoot(uint32_t sceneId, Entity exact, const ScenePickRequest& request) const;
        // the prefab instance roots strictly between root and exact, outermost first
        std::vector<Entity> intermediateRoots(uint32_t sceneId, Entity root, Entity exact) const;
        bool isInside(uint32_t sceneId, Entity entity, Entity ancestor) const;

        // the filter of a scene the view draws: its pickable GameObjects on the view's pickable
        // layers, without what the Scene view hides or locks
        PickFilter makeFilter(const SceneProject* layerScene, const SceneProject* viewScene, bool waitForBvh) const;

        // R15 broadphase: the side planes of a rectangle (NDC) of a view-projection, the
        // near and far planes always passing
        static void rectPlanes(const Matrix4& viewProjection, Vector2 minNdc, Vector2 maxNdc, Plane planes[6]);

        // The closest hit of `entities` along a ray (the thin findBestEntityByRay of D1): what
        // the old AABB picker answered, through the BVH. distance is the fraction of the ray;
        // 2D and UI keep the last drawn hit.
        Entity bestOnRay(const std::vector<Entity>& entities, Scene* scene, const Ray& ray, Scene* sizeScene,
                         SceneType sceneType, float& distance, size_t& index, bool iconsDrawn) const;

    private:
        Project* project;

        void pickLayer(const Project::SceneViewLayer& layer, SceneProject* viewScene, const Ray& ray, bool waitForBvh,
                       std::vector<ScenePickEntry>& out, bool& pending, ScenePickEntry& pendingHit) const;
    };

}
