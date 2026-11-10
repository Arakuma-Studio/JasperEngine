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
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++) {
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        const Transform* transform = scene->findComponent<Transform>(terrains->getEntity(i));
        if (!transform) continue;
        const Vector3 local = transform->modelMatrix.inverse() * Vector3(x, 0.0f, z);
        const terraincore::SurfaceSample sample = querySurface(terrain, local.x, local.z);
        if (!sample.inside || !sample.present) continue;
        if (sample.hole) continue;
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
            // world down, as the volume's rotation and scale see it
            Vec3d origin, below;
            if (!voxels->worldToLocal(owner, Vector3(x, yHint, z), origin)) continue;
            voxels->worldToLocal(owner, Vector3(x, yHint - 1.0f, z), below);
            const Vec3d down{below.x - origin.x, below.y - origin.y, below.z - origin.z};
            const RaycastHit hit = raycast(*store, origin, down, static_cast<double>(store->desc().dimensions.y) * store->desc().voxelSize * 2.0);
            if (!hit.hit) continue;
            const Vector3 world = voxels->localToWorld(owner, hit.point);
            // volumes over one column: the first surface the ray meets is the highest
            if (out.source == GroundSource::Voxel && world.y <= out.height) continue;
            const Vector3 pointOnNormal = voxels->localToWorld(owner, Vec3d{hit.point.x + hit.normal.x, hit.point.y + hit.normal.y, hit.point.z + hit.normal.z});
            out.height = world.y;
            out.normal = (pointOnNormal - world).normalized();
            out.source = GroundSource::Voxel;
        }
    }
#else
    (void)yHint;
#endif
    return out;
}
