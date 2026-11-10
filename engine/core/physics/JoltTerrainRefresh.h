// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOLT_TERRAIN_REFRESH_H
#define JOLT_TERRAIN_REFRESH_H

// T1 R79: a terrain's height and hole edits reach its heightfield colliders before the next physics
// step, through HeightFieldShape::SetHeights on the samples the edit reaches, with the build's own
// per-sample function (JoltShapeBuilder::sampleHeightField), and the bodies over it wake. What
// cannot be written in place (a height past the shape's range, most of the shape, a compound body)
// is built again, the path every other collider change takes.

#include "Export.h"
#include "ecs/Entity.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Jolt.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#endif

namespace jasperengine {

    class Scene;
    class PhysicsSystem;
    class Vector3;
    struct TerrainComponent;

    // A block of a heightfield's sample lattice, in samples
    struct HeightFieldBlock {
        unsigned int x = 0, y = 0, w = 0, h = 0;

        bool empty() const { return w == 0 || h == 0; }
    };

    class JASPERENGINE_API JoltTerrainRefresh {
    public:
        // Over a quarter of a shape's samples, building it again costs no more (T1 §9)
        static constexpr float REBUILD_SHARE = 0.25f;

        // The samples of a samples x samples lattice over the terrain whose height reads the
        // terrain-local rectangle: its cells grown by one (a sample depends on the four squares
        // around it, T3 R22, R28), widened to whole blocks of blockSize and clipped to the shape's
        // paddedSamples. A negative mirror flips the axis, as the build does.
        static HeightFieldBlock blockOver(float terrainSize, unsigned int samples, unsigned int paddedSamples, unsigned int blockSize,
                                          float minX, float minZ, float maxX, float maxZ, float mirrorX, float mirrorZ);

#ifdef JASPERENGINE_PHYSICS_3D
        // Writes the block's new heights into the shape; false when it must be built again instead
        // (a new height past the shape's range, or more than REBUILD_SHARE of it). samplesSize is
        // the count the shape was built with, before Jolt padded it to whole blocks.
        static bool refreshShape(Scene* scene, TerrainComponent& terrain, const Vector3& scale, unsigned int samplesSize,
                                 JPH::HeightFieldShape& shape, const HeightFieldBlock& block, JPH::TempAllocator& allocator);
#endif

        // Before a step: every heightfield collider of an edited terrain takes the edit, and the
        // terrain's pending rectangle is cleared. Costs a pointer test per terrain otherwise.
        static void apply(Scene* scene, PhysicsSystem& physics);
    };

}

#endif // JOLT_TERRAIN_REFRESH_H
