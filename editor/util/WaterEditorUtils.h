// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/WaterVolumeComponent.h"
#include "math/Vector3.h"

#include <vector>

namespace jasperengine::editor {

    // The edit-time picture of a Buoyancy and a Water Volume (inspector warnings and Scene view
    // gizmos; docs/specs/S22-gameplay.md R40), from the components alone: no Jolt shape exists
    // outside Play.
    namespace WaterEditorUtils {

        // The local bounds of a Rigidbody's colliders (each one's own frame ignored for its turn):
        // false when it has none whose size is known (convex hulls, meshes, height fields)
        bool colliderBounds(const Scene* scene, Entity entity, Vector3& min, Vector3& max);
        // Their volume times scale and their mass (density x volume), world units; false for none
        bool colliderMass(const Scene* scene, Entity entity, float& volume, float& mass);
        // The columns' bottoms in local space, as WaterSystem places them (CUSTOM: the points)
        std::vector<Vector3> columnBottoms(const Scene* scene, Entity entity, const BuoyancyComponent& buoyancy,
                                           Vector3& boundsMin, Vector3& boundsMax);
        // the density of the Water Volume nearest the entity (any, the scene's first), 0 for none
        float waterDensity(const Scene* scene, Entity entity);
        // The spacing of the entity's mesh vertices over its XZ extent (world units, a grid's
        // estimate: sqrt(area / vertices)), 0 without a mesh
        float meshVertexSpacing(const Scene* scene, Entity entity);
        // The largest waveLength of its library:water material (WAVES), 0 without waves
        float materialWaveLength(const Scene* scene, Entity entity);
    }

}
