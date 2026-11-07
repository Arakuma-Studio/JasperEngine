// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "component/BuoyancyComponent.h"
#include "component/WaterVolumeComponent.h"
#include "math/AABB.h"
#include "math/Vector3.h"

#include <vector>

namespace jasperengine::editor {

    // The edit-time picture of a Buoyancy and a Water Volume (inspector warnings and Scene view
    // gizmos; docs/specs/S22-gameplay.md R40), from the components alone: no Jolt shape exists
    // outside Play.
    namespace WaterEditorUtils {

        // The local bounds of a Rigidbody's colliders (each one's own frame ignored for its turn):
        // false when it has none whose size is known (convex hulls, meshes, height fields)
        bool colliderBounds(Scene* scene, Entity entity, Vector3& min, Vector3& max);
        // Their volume times scale and their mass (density x volume), world units; false for none
        bool colliderMass(Scene* scene, Entity entity, float& volume, float& mass);
        // The columns' bottoms in local space, as WaterSystem places them (CUSTOM: the points)
        std::vector<Vector3> columnBottoms(Scene* scene, Entity entity, const BuoyancyComponent& buoyancy,
                                           Vector3& boundsMin, Vector3& boundsMax);
        // the density of the Water Volume nearest the entity (any, the scene's first), 0 for none
        float waterDensity(Scene* scene, Entity entity);
        // The spacing of the entity's mesh vertices over its XZ extent (world units, a grid's
        // estimate: sqrt(area / vertices)), 0 without a mesh
        float meshVertexSpacing(Scene* scene, Entity entity);
        // The largest waveLength of its library:water material (WAVES), 0 without waves
        float materialWaveLength(Scene* scene, Entity entity);

        // Water Excluders and Water Wakes (docs/superpowers/specs/2026-11-04-water-boats-design.md)
        // The local bounds of the meshes a Convex Hull reads (the GameObject's mesh, or its
        // model's mesh nodes); null without
        AABB meshSourceLocalBounds(Scene* scene, Entity entity);
        // whether a mesh of the scene draws with a shader asking for waterexclusion (or waterwake)
        bool sceneDrawsWaterInteraction(Scene* scene, bool wake);
        // whether a floating dynamic Rigidbody (a buoyancy factor or a Buoyancy) is at or under an
        // excluder that belongs to no Rigidbody (WaterInteractionSystem::getExcluderOwner): kept dry
        // by it, it would never float
        bool excluderDriesItsOwnBodies(Scene* scene, Entity excluder);
    }

}
