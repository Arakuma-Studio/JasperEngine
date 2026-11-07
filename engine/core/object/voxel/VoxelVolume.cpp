// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelVolume.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/GameObject.h"
#include "util/MeshTriangles.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelSample.h"
#include "voxel/VoxelVoxelize.h"

#include <cmath>

using namespace jasperengine;

VoxelShape VoxelShape::sphere(Vector3 center, float radius){
    VoxelShape shape;
    shape.kind = voxel::ShapeKind::Sphere;
    shape.center = center;
    shape.radius = radius;
    return shape;
}

VoxelShape VoxelShape::box(Vector3 center, Vector3 halfExtents, Quaternion rotation){
    VoxelShape shape;
    shape.kind = voxel::ShapeKind::Box;
    shape.center = center;
    shape.halfExtents = halfExtents;
    shape.rotation = rotation;
    return shape;
}

VoxelShape VoxelShape::capsule(Vector3 from, Vector3 to, float radius){
    VoxelShape shape;
    shape.kind = voxel::ShapeKind::Capsule;
    shape.from = from;
    shape.to = to;
    shape.radius = radius;
    return shape;
}

VoxelShape VoxelShape::mesh(GameObject go){
    VoxelShape shape;
    shape.kind = voxel::ShapeKind::Mesh;
    shape.meshEntity = go.getEntity();
    shape.meshScene = go.getScene();
    return shape;
}

VoxelShape VoxelShape::stamp(const std::string& path, Vector3 position, Quaternion rotation, float scale){
    VoxelShape shape;
    shape.kind = voxel::ShapeKind::Stamp;
    shape.stampPath = path;
    shape.center = position;
    shape.rotation = rotation;
    shape.scale = scale;
    return shape;
}

VoxelVolume::VoxelVolume(Scene* scene, Entity entity): EntityHandle(scene, entity){}
VoxelVolume::~VoxelVolume() = default;
VoxelVolume::VoxelVolume(const VoxelVolume& rhs) = default;
VoxelVolume& VoxelVolume::operator=(const VoxelVolume& rhs) = default;
VoxelVolume::VoxelVolume(VoxelVolume&& rhs) noexcept = default;
VoxelVolume& VoxelVolume::operator=(VoxelVolume&& rhs) noexcept = default;

VoxelSystem* VoxelVolume::system() const{
    return scene->getSystem<VoxelSystem>().get();
}

bool VoxelVolume::isValid() const{
    return scene->isEntityCreated(entity) && scene->findComponent<VoxelVolumeComponent>(entity) != nullptr;
}

VoxelVolume VoxelVolume::fromGameObject(GameObject go){
    Scene* goScene = go.getScene();
    Entity entity = go.getEntity();
    if (std::shared_ptr<VoxelSystem> voxels = goScene->findSystem<VoxelSystem>()){
        const Entity owner = voxels->getGeneratedOwner(entity);
        if (owner != NULL_ENTITY) entity = owner;
    }
    return VoxelVolume(goScene, entity);
}

namespace {

    // A stamp's level 0, kept while the shape is in use. R26: an asset whose SDF is the shape.
    std::shared_ptr<const voxel::StampField> stampField(const std::string& path){
        const std::shared_ptr<VoxelAsset> asset = VoxelAssets::acquire(path);
        if (!asset->store) return nullptr;
        voxel::Store& store = *asset->store;
        const voxel::Int3 samples = store.levelSamples(0);
        auto field = std::make_shared<voxel::StampField>();
        field->size = samples;
        field->voxelSize = store.desc().voxelSize;
        field->sdf.resize(static_cast<size_t>(samples.x) * samples.y * samples.z);
        size_t i = 0;
        for (int z = 0; z < samples.z; z++){
            for (int y = 0; y < samples.y; y++){
                for (int x = 0; x < samples.x; x++) field->sdf[i++] = store.sdfAt(0, x, y, z);
            }
        }
        field->originOffset[0] = samples.x * field->voxelSize * 0.5;
        field->originOffset[1] = samples.y * field->voxelSize * 0.5;
        field->originOffset[2] = samples.z * field->voxelSize * 0.5;
        return field;
    }

    // R36: voxelized on first use
    std::shared_ptr<const voxel::StampField> meshField(Scene* scene, Entity entity, double voxelSize){
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh) return nullptr;
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;
        MeshTriangles::appendMeshTriangles(mesh, Matrix4(), vertices, indices);
        std::vector<float> positions;
        positions.reserve(vertices.size() * 3);
        for (const Vector3& v : vertices){
            positions.push_back(v.x);
            positions.push_back(v.y);
            positions.push_back(v.z);
        }
        voxel::VoxelizeOptions options;
        options.voxelSize = voxelSize;
        return voxel::voxelizeMesh(positions, indices, options);
    }

}

VoxelEditReport VoxelVolume::apply(voxel::EditKind kind, const VoxelShape& shape, const VoxelEditOptions& options){
    VoxelEditReport report;
    if (!isValid()){
        report.error = voxel::editErrorCode(voxel::EditError::NotLoaded);
        return report;
    }
    VoxelSystem* voxels = system();
    voxel::Store* store = voxels->getStore(entity);
    if (!store){
        report.error = voxel::editErrorCode(voxel::EditError::NotLoaded);
        return report;
    }
    voxel::EditOp op;
    op.kind = kind;
    op.blend = options.blend;
    op.material = options.material;
    op.weight = options.weight;
    op.falloff = options.falloff;
    op.amount = options.amount;
    op.iterations = options.iterations;
    op.shape.kind = shape.kind;
    op.shape.radius = shape.radius;
    op.shape.halfExtents = voxel::Vec3d{shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z};
    op.shape.scale = shape.scale;
    op.shape.rotation = voxels->localRotationOf(entity, shape.rotation);
    voxels->worldToLocal(entity, shape.center, op.shape.center);
    voxels->worldToLocal(entity, shape.from, op.shape.from);
    voxels->worldToLocal(entity, shape.to, op.shape.to);
    if (shape.kind == voxel::ShapeKind::Stamp) op.shape.field = stampField(shape.stampPath);
    if (shape.kind == voxel::ShapeKind::Mesh && shape.meshScene) {
        op.shape.field = meshField(shape.meshScene, shape.meshEntity, store->desc().voxelSize);
        if (const Transform* transform = shape.meshScene->findComponent<Transform>(shape.meshEntity)) {
            voxels->worldToLocal(entity, transform->worldPosition, op.shape.center);
            op.shape.rotation = voxels->localRotationOf(entity, transform->worldRotation);
        }
    }
    const voxel::EditResult result = voxels->edit(entity, std::span<const voxel::EditOp>(&op, 1), voxel::EditOptions{});
    report.ok = result.ok;
    report.error = voxel::editErrorCode(result.error);
    if (!result.ok) return report;
    report.voxelsChanged = static_cast<int>(result.voxelsChanged);
    report.surfaceVoxelsChanged = static_cast<int>(result.surfaceVoxelsChanged);
    report.blocksTouched = result.blocksTouched;
    report.memoryDelta = static_cast<int>(result.memoryDelta);
    report.clampedVoxels = static_cast<int>(result.clampedVoxels);
    report.removedVolume = static_cast<float>(result.removedVolume);
    report.addedVolume = static_cast<float>(result.addedVolume);
    if (!result.changedVoxels.empty()){
        const float voxelSize = store->desc().voxelSize;
        const AABB local(result.changedVoxels.min.x * voxelSize, result.changedVoxels.min.y * voxelSize, result.changedVoxels.min.z * voxelSize,
                         result.changedVoxels.max.x * voxelSize, result.changedVoxels.max.y * voxelSize, result.changedVoxels.max.z * voxelSize);
        report.bounds = voxels->modelMatrixOf(entity) * local;
    }
    return report;
}

VoxelEditReport VoxelVolume::dig(const VoxelShape& shape, const VoxelEditOptions& options){ return apply(voxel::EditKind::Dig, shape, options); }
VoxelEditReport VoxelVolume::fill(const VoxelShape& shape, const VoxelEditOptions& options){ return apply(voxel::EditKind::Add, shape, options); }
VoxelEditReport VoxelVolume::smooth(const VoxelShape& shape, const VoxelEditOptions& options){ return apply(voxel::EditKind::Smooth, shape, options); }
VoxelEditReport VoxelVolume::paint(const VoxelShape& shape, const VoxelEditOptions& options){ return apply(voxel::EditKind::Paint, shape, options); }

VoxelRayHit VoxelVolume::raycast(Vector3 origin, Vector3 direction, float maxDistance){
    VoxelRayHit out;
    VoxelSystem* voxels = system();
    voxel::Store* store = voxels->getStore(entity);
    if (!store) return out;
    voxel::Vec3d localOrigin, localEnd;
    voxels->worldToLocal(entity, origin, localOrigin);
    voxels->worldToLocal(entity, origin + direction, localEnd);
    const voxel::Vec3d localDirection{localEnd.x - localOrigin.x, localEnd.y - localOrigin.y, localEnd.z - localOrigin.z};
    const voxel::RaycastHit hit = voxel::raycast(*store, localOrigin, localDirection, maxDistance);
    out.hit = hit.hit;
    if (!hit.hit) return out;
    out.point = voxels->localToWorld(entity, hit.point);
    const Vector3 pointOnNormal = voxels->localToWorld(entity, voxel::Vec3d{hit.point.x + hit.normal.x, hit.point.y + hit.normal.y, hit.point.z + hit.normal.z});
    out.normal = (pointOnNormal - out.point).normalized();
    out.distance = static_cast<float>(hit.distance);
    out.slot = hit.slot;
    return out;
}

float VoxelVolume::getValueAt(Vector3 point){
    VoxelSystem* voxels = system();
    voxel::Store* store = voxels->getStore(entity);
    if (!store) return 0.0f;
    voxel::Vec3d local;
    voxels->worldToLocal(entity, point, local);
    return static_cast<float>(voxel::valueAt(*store, local));
}

VoxelMaterialHit VoxelVolume::getMaterialAt(Vector3 point){
    VoxelMaterialHit out;
    VoxelSystem* voxels = system();
    voxel::Store* store = voxels->getStore(entity);
    if (!store) return out;
    voxel::Vec3d local;
    voxels->worldToLocal(entity, point, local);
    const voxel::MaterialSample sample = voxel::materialAtPoint(*store, local);
    out.slot = sample.slot;
    out.second = sample.second;
    out.weight = sample.weight;
    return out;
}

void VoxelVolume::ensureCollision(const AABB& bounds){ system()->ensureCollision(entity, bounds); }
bool VoxelVolume::isCollisionReady(const AABB& bounds){ return system()->isCollisionReady(entity, bounds); }

VoxelVolumeStats VoxelVolume::getStats() const{
    const VoxelStats stats = system()->getStats(entity);
    VoxelVolumeStats out;
    out.blocks = static_cast<int>(stats.blocks.total);
    out.uniformBlocks = static_cast<int>(stats.blocks.uniform);
    out.nonUniformBlocks = static_cast<int>(stats.blocks.nonUniform);
    out.modifiedBlocks = static_cast<int>(stats.blocks.modified);
    out.memoryBytes = static_cast<int>(stats.memory.total());
    out.meshBytes = static_cast<int>(stats.meshBytes);
    out.triangles = static_cast<int>(stats.triangles);
    out.chunks = stats.chunks;
    out.collisionBodies = stats.collisionBodies;
    out.jobsPending = stats.jobsPending;
    return out;
}

bool VoxelVolume::saveChanges(const std::string& slot){ return system()->saveChanges(entity, slot, lastError); }
bool VoxelVolume::loadChanges(const std::string& slot){ return system()->loadChanges(entity, slot, lastError); }
bool VoxelVolume::hasSavedChanges(const std::string& slot) const{ return system()->hasSavedChanges(entity, slot); }
void VoxelVolume::revertChanges(){ system()->revertChanges(entity); }

FunctionSubscribe<void(VoxelEditEvent)>& VoxelVolume::getOnEdited(){ return getComponent<VoxelVolumeComponent>().onEdited; }
FunctionSubscribe<void(AABB)>& VoxelVolume::getOnCollisionUpdated(){ return getComponent<VoxelVolumeComponent>().onCollisionUpdated; }
FunctionSubscribe<void(GameObject)>& VoxelVolume::getOnBodyRescued(){ return getComponent<VoxelVolumeComponent>().onBodyRescued; }
