// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A heat map over a Terrain in the Scene view (T5 R51, R57), for a detail layer's density and for
// trees per cell: System-pool, editor-overlay meshes as NavigationOverlay's. Off, it holds no entity
// and adds no pass. T1's TerrainOverlay::registerSource replaces how a source is chosen when it lands
// (T5 Deviation 2); these two sources stay.

#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine::editor {

    struct SceneProject;

    class TerrainOverlay {
    public:
        enum class Kind : uint8_t { None, Density, TreeDensity };

        // None hides it and frees its mesh
        static void show(SceneProject* sceneProject, Entity terrain, Kind kind, int layer, float opacity);
        static Kind shownKind(uint32_t sceneId);
        // The largest value of the last bake: the legend's top, per square unit
        static float maxValue(uint32_t sceneId);
        // The triangles the last bake made (tests)
        static size_t triangleCount(uint32_t sceneId);
        // The scene is going away
        static void forget(uint32_t sceneId);
    };

}
