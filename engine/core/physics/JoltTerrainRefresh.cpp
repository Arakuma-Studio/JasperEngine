// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JoltTerrainRefresh.h"

#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Vector3.h"
#include "object/TerrainEdit.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/PhysicsSystem.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/Physics/Body/BodyInterface.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h"
#include "Jolt/Physics/Collision/ObjectLayer.h"
#include "Jolt/Geometry/AABox.h"
#endif

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;

HeightFieldBlock JoltTerrainRefresh::blockOver(float terrainSize, unsigned int samples, unsigned int paddedSamples, unsigned int blockSize,
                                               float minX, float minZ, float maxX, float maxZ, float mirrorX, float mirrorZ) {
    HeightFieldBlock block;
    if (samples < 2 || terrainSize <= 0.0f || blockSize == 0 || paddedSamples < samples) return block;
    const float half = terrainSize * 0.5f;
    const float spacing = terrainSize / static_cast<float>(samples - 1);

    // sample i sits at mirror * (-half + spacing * i)
    auto range = [&](float low, float high, float mirror, unsigned int& first, unsigned int& count) {
        const float a = mirror * low;
        const float b = mirror * high;
        const double lowSample = std::floor((std::min(a, b) + half) / spacing) - 1.0;
        const double highSample = std::ceil((std::max(a, b) + half) / spacing) + 1.0;
        const double clippedLow = std::max(0.0, lowSample);
        const double clippedHigh = std::min(static_cast<double>(samples - 1), highSample);
        if (clippedHigh < clippedLow) {
            count = 0;
            return;
        }
        const unsigned int start = static_cast<unsigned int>(clippedLow) / blockSize * blockSize;
        const unsigned int end = std::min(paddedSamples, (static_cast<unsigned int>(clippedHigh) / blockSize + 1) * blockSize);
        first = start;
        count = end - start;
    };
    range(minX, maxX, mirrorX, block.x, block.w);
    range(minZ, maxZ, mirrorZ, block.y, block.h);
    if (block.w == 0 || block.h == 0) block = HeightFieldBlock{};
    return block;
}

#ifdef JASPERENGINE_PHYSICS_3D

bool JoltTerrainRefresh::refreshShape(Scene* scene, TerrainComponent& terrain, const Vector3& scale, unsigned int samplesSize,
                                      JPH::HeightFieldShape& shape, const HeightFieldBlock& block, JPH::TempAllocator& allocator) {
    if (block.empty()) return true;
    const unsigned int padded = shape.GetSampleCount();
    if (static_cast<float>(block.w) * static_cast<float>(block.h) > REBUILD_SHARE * static_cast<float>(padded) * static_cast<float>(padded)) {
        return false;
    }

    // inside the samples the build had; the padding Jolt added past them stays without collision
    const unsigned int realW = block.x < samplesSize ? std::min(block.w, samplesSize - block.x) : 0;
    const unsigned int realH = block.y < samplesSize ? std::min(block.h, samplesSize - block.y) : 0;
    std::vector<float> local(static_cast<size_t>(realW) * realH);
    if (realW > 0 && realH > 0) {
        JoltShapeBuilder::sampleHeightField(scene, terrain, scale, samplesSize, block.x, block.y, realW, realH, local.data());
    }

    // the shape holds heights scaled by the entity, as the build gave it them
    const float scaleY = std::fabs(scale.y);
    const float lowest = shape.GetMinHeightValue();
    const float highest = shape.GetMaxHeightValue();
    std::vector<float> heights(static_cast<size_t>(block.w) * block.h, JPH::HeightFieldShapeConstants::cNoCollisionValue);
    for (unsigned int y = 0; y < realH; y++) {
        for (unsigned int x = 0; x < realW; x++) {
            const float value = local[static_cast<size_t>(y) * realW + x];
            if (value == JPH::HeightFieldShapeConstants::cNoCollisionValue) continue;
            const float scaled = value * scaleY;
            // SetHeights would clamp it: a terrain built without its range reserved is built again
            if (scaled < lowest || scaled > highest) return false;
            heights[static_cast<size_t>(y) * block.w + x] = scaled;
        }
    }
    shape.SetHeights(block.x, block.y, block.w, block.h, heights.data(), static_cast<intptr_t>(block.w), allocator);
    return true;
}

namespace {

    // The body-local box a refreshed block covers, in world space, for the bodies to wake
    JPH::AABox worldBoxOf(const Transform& transform, const TerrainComponent& terrain, const TerrainEditState& state) {
        const float low = std::min(0.0f, terrain.maxHeight);
        const float high = std::max(0.0f, terrain.maxHeight);
        JPH::AABox box;
        for (int corner = 0; corner < 8; corner++) {
            const Vector3 local((corner & 1) ? state.collisionMaxX : state.collisionMinX, (corner & 2) ? high : low,
                                (corner & 4) ? state.collisionMaxZ : state.collisionMinZ);
            const Vector3 world = transform.modelMatrix * local;
            box.Encapsulate(JPH::Vec3(world.x, world.y, world.z));
        }
        // a body resting on the old surface sits a little above it
        box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
        return box;
    }

}

#endif

void JoltTerrainRefresh::apply(Scene* scene, PhysicsSystem& physics) {
#ifdef JASPERENGINE_PHYSICS_3D
    auto terrains = scene->getComponentArray<TerrainComponent>();
    bool pending = false;
    for (size_t t = 0; t < terrains->size() && !pending; t++) {
        const TerrainComponent& terrain = terrains->getComponentFromIndex(t);
        pending = terrain.editState && terrain.editState->collisionDirty;
    }
    if (!pending) return;

    JPH::BodyInterface& bodies = physics.getBodyInterface3D();
    auto rigidbodies = scene->getComponentArray<RigidbodyComponent>();
    for (size_t t = 0; t < terrains->size(); t++) {
        TerrainComponent& terrain = terrains->getComponentFromIndex(t);
        if (!terrain.editState || !terrain.editState->collisionDirty) continue;
        TerrainEditState& state = *terrain.editState;
        const Entity terrainEntity = terrains->getEntity(t);

        for (size_t b = 0; b < rigidbodies->size(); b++) {
            RigidbodyComponent& body = rigidbodies->getComponentFromIndex(b);
            const Entity bodyEntity = rigidbodies->getEntity(b);
            for (Collider3D& collider : body.colliders) {
                if (collider.type != Collider3DType::HEIGHTFIELD) continue;
                if ((collider.sourceEntity == NULL_ENTITY ? bodyEntity : collider.sourceEntity) != terrainEntity) continue;
                // a collider not built yet will read the map as it is
                if (!collider.shape || body.body.IsInvalid() || collider.dirty) continue;

                const Vector3 scale = JoltShapeBuilder::entityWorldScale(scene, bodyEntity);
                const unsigned int samples = std::max(collider.samplesSize == 0 ? JoltShapeBuilder::defaultHeightFieldSamples(terrain) : collider.samplesSize, 4u);
                JPH::HeightFieldShape& shape = const_cast<JPH::HeightFieldShape&>(static_cast<const JPH::HeightFieldShape&>(*collider.shape));
                const unsigned int blockSize = shape.GetBlockSize();
                // the map's size moved under it (an import): the shape counts other samples now
                const bool sameLattice = shape.GetSampleCount() == (samples + blockSize - 1) / blockSize * blockSize;
                // in place only where the body's shape is the collider's own: a compound keeps its
                // children's bounds and a centre-of-mass offset wraps it
                const bool inPlace = sameLattice && bodies.GetShape(body.body).GetPtr() == collider.shape.GetPtr();

                const HeightFieldBlock block = blockOver(terrain.terrainSize, samples, shape.GetSampleCount(), blockSize,
                                                         state.collisionMinX, state.collisionMinZ, state.collisionMaxX, state.collisionMaxZ,
                                                         scale.x < 0.0f ? -1.0f : 1.0f, scale.z < 0.0f ? -1.0f : 1.0f);
                if (block.empty()) continue;

                const JPH::Vec3 centre = shape.GetCenterOfMass();
                if (inPlace && refreshShape(scene, terrain, scale, samples, shape, block, JoltShapeBuilder::sharedPhysicsTempAllocator())) {
                    bodies.NotifyShapeChanged(body.body, centre, false, JPH::EActivation::DontActivate);
                } else {
                    collider.dirty = true;
                    body.needUpdateColliders = true;
                }
            }
        }
        // what rests on the old surface wakes to the new one, sleeping or not
        if (Transform* transform = scene->findComponent<Transform>(terrainEntity)) {
            bodies.ActivateBodiesInAABox(worldBoxOf(*transform, terrain, state), {}, {});
        }
        state.collisionDirty = false;
    }
#else
    (void)scene;
    (void)physics;
#endif
}
