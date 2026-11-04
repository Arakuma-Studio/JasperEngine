// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GroundQuery.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Matrix4.h"
#include "object/TerrainEdit.h"

#if JASPERENGINE_VOXEL
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelSample.h"
#endif

using namespace jasperengine;

voxel::GroundSample voxel::sampleGround(Scene* scene, float x, float z, float yHint) {
    GroundSample out;
    bool throughHole = false;
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++) {
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(terrains->getEntity(i));
        if (!transform) continue;
        const Vector3 local = transform->modelMatrix.inverse() * Vector3(x, 0.0f, z);
        const terrain::SurfaceSample sample = querySurface(terrain, local.x, local.z);
        if (!sample.inside || !sample.present) continue;
        if (sample.hole) {
            throughHole = true;
            continue;
        }
        out.height = (transform->modelMatrix * Vector3(local.x, sample.height, local.z)).y;
        out.normal = sample.normal;
        out.source = GroundSource::Terrain;
        return out;
    }
#if JASPERENGINE_VOXEL
    std::shared_ptr<VoxelSystem> voxels = scene->findSystem<VoxelSystem>();
    if (voxels) {
        auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
        for (size_t i = 0; i < volumes->size(); i++) {
            const Entity owner = volumes->getEntity(i);
            Store* store = voxels->getStore(owner);
            if (!store) continue;
            Vec3d origin;
            if (!voxels->worldToLocal(owner, Vector3(x, yHint, z), origin)) continue;
            const Quatd rotation = voxels->localRotationOf(owner, Quaternion());
            (void)rotation;
            const RaycastHit hit = raycast(*store, origin, Vec3d{0.0, -1.0, 0.0}, static_cast<double>(store->desc().dimensions.y) * store->desc().voxelSize * 2.0);
            if (!hit.hit) continue;
            const Vector3 world = voxels->localToWorld(owner, hit.point);
            out.height = world.y;
            out.normal = Vector3(static_cast<float>(hit.normal.x), static_cast<float>(hit.normal.y), static_cast<float>(hit.normal.z));
            out.source = GroundSource::Voxel;
            return out;
        }
    }
#endif
    (void)throughHole;
    (void)yHint;
    return out;
}
