// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshRaycast.h"
#include "MeshPose.h"
#include "PickCache.h"

#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/PolygonComponent.h"
#include "component/RectTransformComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "subsystem/MeshSystem.h"
#include "util/STBText.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::picking;

namespace {

    constexpr float FLOAT_MAX = std::numeric_limits<float>::max();
    // a terrain's ray is a segment: this long at most
    constexpr float TERRAIN_RANGE = 1.0e7f;

    // (M^-1)^T n: a normal of the space M maps from, in the space it maps to
    Vector3 transformNormal(const Matrix4& inverse, const Vector3& n){
        return Vector3(inverse.get(0, 0) * n.x + inverse.get(0, 1) * n.y + inverse.get(0, 2) * n.z,
                       inverse.get(1, 0) * n.x + inverse.get(1, 1) * n.y + inverse.get(1, 2) * n.z,
                       inverse.get(2, 0) * n.x + inverse.get(2, 1) * n.y + inverse.get(2, 2) * n.z);
    }

    Vector3 facing(Vector3 n, const Vector3& direction){
        if (n.squaredLength() <= 0.0f || !std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)){
            return -direction;
        }
        n.normalize();
        return n.dotProduct(direction) > 0.0f ? -n : n;
    }

    float determinant3(const Matrix4& m){
        return m.get(0, 0) * (m.get(1, 1) * m.get(2, 2) - m.get(2, 1) * m.get(1, 2)) -
               m.get(1, 0) * (m.get(0, 1) * m.get(2, 2) - m.get(2, 1) * m.get(0, 2)) +
               m.get(2, 0) * (m.get(0, 1) * m.get(1, 2) - m.get(1, 1) * m.get(0, 2));
    }

    // The side of a submesh's triangles the renderer draws, in MeshBvhRay terms (FRONT: the
    // counter-clockwise side), as RenderSystem::resolveRenderState culls: the material's cull
    // mode, then its shader's @state, then the submesh's faceCulling with the mesh's mode
    PickFaces drawnFaces(const MeshComponent& mesh, const Submesh& submesh, bool mirrored){
        MaterialCullMode cull = submesh.material.cullMode;
        if (cull == MaterialCullMode::AUTO && submesh.manifest){
            cull = submesh.manifestState.cull;
        }
        bool culling;
        CullingMode mode;
        if (cull == MaterialCullMode::AUTO){
            culling = submesh.faceCulling;
            mode = mesh.cullingMode;
        }else{
            culling = cull != MaterialCullMode::OFF;
            mode = cull == MaterialCullMode::FRONT ? CullingMode::FRONT : CullingMode::BACK;
        }
        if (!culling){
            return PickFaces::BOTH;
        }
        // the front faces' winding as seen in the world (a mirroring transform flips it)
        bool frontIsCounterClockwise = mesh.windingOrder == WindingOrder::CCW;
        if (mirrored){
            frontIsCounterClockwise = !frontIsCounterClockwise;
        }
        const bool drawnCounterClockwise = (mode == CullingMode::BACK) ? frontIsCounterClockwise : !frontIsCounterClockwise;
        return drawnCounterClockwise ? PickFaces::FRONT : PickFaces::BACK;
    }

    void resolveFaces(const MeshComponent& mesh, bool mirrored, bool hitBackfaces, std::vector<PickFaces>& faces){
        faces.assign(mesh.numSubmeshes, PickFaces::BOTH);
        if (hitBackfaces){
            return;
        }
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            faces[s] = drawnFaces(mesh, mesh.submeshes[s], mirrored);
        }
    }

    // the entry of a ray into a box (origin outside it), and the face it enters by
    bool enterBox(const AABB& box, const Vector3& origin, const Vector3& direction, float maxDistance, float& t, Vector3& normal){
        if (box.isNull() || box.isInfinite()){
            return false;
        }
        const Vector3& mn = box.getMinimum();
        const Vector3& mx = box.getMaximum();
        if (origin.x >= mn.x && origin.x <= mx.x && origin.y >= mn.y && origin.y <= mx.y && origin.z >= mn.z && origin.z <= mx.z){
            // inside what it bounds: nothing of it is in front to click
            return false;
        }
        float t0 = 0.0f, t1 = maxDistance;
        int axis = -1;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        const float lo[3] = {mn.x, mn.y, mn.z};
        const float hi[3] = {mx.x, mx.y, mx.z};
        for (int a = 0; a < 3; a++){
            if (std::fabs(d[a]) < 1e-30f){
                if (o[a] < lo[a] || o[a] > hi[a]) return false;
                continue;
            }
            float tNear = (lo[a] - o[a]) / d[a];
            float tFar = (hi[a] - o[a]) / d[a];
            if (tNear > tFar) std::swap(tNear, tFar);
            if (tNear > t0){
                t0 = tNear;
                axis = a;
            }
            t1 = std::min(t1, tFar);
            if (t0 > t1) return false;
        }
        if (axis < 0){
            return false;
        }
        t = t0;
        normal = Vector3::ZERO;
        normal[(unsigned)axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        return true;
    }

    bool boundsHit(const SceneBvh::Leaf& leaf, const Vector3& origin, const Vector3& direction, float maxDistance, bool pending, PickHit& hit){
        float t;
        Vector3 normal;
        if (!enterBox(leaf.bounds, origin, direction, maxDistance, t, normal)){
            return false;
        }
        hit = PickHit();
        hit.entity = leaf.entity;
        hit.kind = PickKind::BOUNDS;
        hit.instance = leaf.kind == PickKind::INSTANCE ? (int32_t)leaf.subIndex : -1;
        hit.distance = t;
        hit.point = origin + direction * t;
        hit.normal = normal;
        hit.faceNormal = normal;
        hit.pending = pending;
        return true;
    }

    Matrix4 instanceMatrix(const InstancedMeshComponent& instmesh, const InstanceData& instance){
        const Matrix4 rotation = instmesh.instancedBillboard ? Matrix4() : instance.rotation.getRotationMatrix();
        return Matrix4::translateMatrix(instance.position) * rotation * Matrix4::scaleMatrix(instance.scale);
    }

    // the interpolated normal of a mesh hit (posed corners turn with their bones), in the
    // mesh's space; false without vertex normals
    bool interpolatedNormal(const MeshComponent& mesh, uint32_t triangle, const MeshBvhHit& hit, bool posed, Vector3& normal){
        PickGeometryLayout layout;
        if (!resolveSources(mesh, layout)){
            return false;
        }
        uint32_t submesh = 0;
        uint32_t corners[3];
        if (!triangleCorners(layout, triangle, submesh, corners)){
            return false;
        }
        const float weights[3] = {1.0f - hit.u - hit.v, hit.u, hit.v};
        Vector3 sum = Vector3::ZERO;
        for (int c = 0; c < 3; c++){
            Vector3 n;
            if (!readVertexNormal(mesh, submesh, corners[c], n)){
                return false;
            }
            if (posed){
                VertexPoseInput input;
                if (readPoseInput(mesh, layout, submesh, corners[c], input) && input.skinned){
                    Matrix4 bone;
                    poseVertex(input, poseParameters(mesh, submesh), &bone);
                    // getSkinNormal: mat3(boneTransform) * normal
                    n = Vector3(bone.get(0, 0) * n.x + bone.get(1, 0) * n.y + bone.get(2, 0) * n.z,
                                bone.get(0, 1) * n.x + bone.get(1, 1) * n.y + bone.get(2, 1) * n.z,
                                bone.get(0, 2) * n.x + bone.get(1, 2) * n.y + bone.get(2, 2) * n.z);
                }
            }
            sum += n * weights[c];
        }
        normal = sum;
        return normal.squaredLength() > 0.0f;
    }

    uint32_t sourceVertexTotal(const MeshComponent& mesh){
        PickGeometryLayout layout;
        if (!resolveSources(mesh, layout)){
            return 0;
        }
        uint64_t total = 0;
        std::vector<const Buffer*> seen;
        for (const PickSubmeshSource& source : layout.submeshes){
            if (source.valid && std::find(seen.begin(), seen.end(), source.positionBuffer) == seen.end()){
                seen.push_back(source.positionBuffer);
                total += source.vertexCount;
            }
        }
        return (uint32_t)std::min<uint64_t>(total, UINT32_MAX);
    }

    bool testMesh(SceneBvh& bvh, const SceneBvh::Leaf& leaf, const Matrix4& model, const Vector3& origin, const Vector3& direction,
                  float maxDistance, const PickFilter& filter, PickHit& hit){
        Scene* scene = bvh.getScene();
        MeshComponent& mesh = scene->getComponent<MeshComponent>(leaf.entity);

        PickCache::EntryPtr entry = bvh.meshEntry(leaf.entity, true);
        if (!entry){
            // no CPU triangles to test (or too many): its bounds
            return (filter.kinds & PICK_BOUNDS) && boundsHit(leaf, origin, direction, maxDistance, false, hit);
        }
        const MeshBvh* tree = PickCache::ready(entry);
        if (!tree){
            if (!filter.waitForBvh){
                return (filter.kinds & PICK_BOUNDS) && boundsHit(leaf, origin, direction, maxDistance, true, hit);
            }
            tree = PickCache::get().wait(entry);
            if (!tree){
                return false;
            }
        }

        const bool instanced = leaf.kind == PickKind::INSTANCE;
        bool posed = false;
        bool clipToPose = false;
        if (!instanced && isPosed(mesh)){
            if (sourceVertexTotal(mesh) > MAX_POSED_VERTICES){
                // too big to pose per pick: bind-pose triangles inside the posed bounds
                clipToPose = !mesh.skinnedAABB.isNull();
            }else if (const MeshBvh* posedTree = bvh.posedTree(leaf.entity, *tree, entry->hash)){
                tree = posedTree;
                posed = true;
            }
        }

        const Matrix4 inverse = model.inverse();
        MeshBvhRay ray;
        ray.origin = inverse * origin;
        ray.direction = inverse * (origin + direction) - ray.origin;
        ray.tMax = maxDistance;
        thread_local std::vector<PickFaces> faces;
        resolveFaces(mesh, determinant3(model) < 0.0f, filter.hitBackfaces, faces);
        ray.faces = faces;

        MeshBvhHit meshHit;
        if (clipToPose){
            thread_local std::vector<MeshBvhHit> hits;
            tree->raycastAll(ray, hits);
            bool found = false;
            for (const MeshBvhHit& candidate : hits){
                const Vector3 local = ray.origin + ray.direction * candidate.t;
                if (mesh.skinnedAABB.contains(local)){
                    meshHit = candidate;
                    found = true;
                    break;
                }
            }
            if (!found){
                return false;
            }
        }else if (!tree->raycast(ray, meshHit)){
            return false;
        }

        hit = PickHit();
        hit.entity = leaf.entity;
        hit.kind = instanced ? PickKind::INSTANCE : PickKind::MESH;
        hit.submesh = meshHit.submesh;
        hit.triangle = meshHit.triangle;
        hit.instance = instanced ? (int32_t)leaf.subIndex : -1;
        hit.barycentric = Vector3(1.0f - meshHit.u - meshHit.v, meshHit.u, meshHit.v);
        hit.distance = meshHit.t;
        hit.point = origin + direction * meshHit.t;
        hit.faceNormal = facing(transformNormal(inverse, (meshHit.v1 - meshHit.v0).crossProduct(meshHit.v2 - meshHit.v0)), direction);
        Vector3 smooth;
        if (interpolatedNormal(mesh, meshHit.triangle, meshHit, posed, smooth)){
            Vector3 world = transformNormal(inverse, smooth);
            if (world.squaredLength() > 0.0f){
                world.normalize();
                // the side the ray met
                hit.normal = world.dotProduct(hit.faceNormal) < 0.0f ? -world : world;
            }else{
                hit.normal = hit.faceNormal;
            }
        }else{
            hit.normal = hit.faceNormal;
        }
        return true;
    }

    bool testTerrain(Scene* scene, const SceneBvh::Leaf& leaf, const Vector3& origin, const Vector3& direction, float maxDistance, PickHit& hit){
        TerrainComponent& terrain = scene->getComponent<TerrainComponent>(leaf.entity);
        Transform& transform = scene->getComponent<Transform>(leaf.entity);
        const float range = std::min(maxDistance, TERRAIN_RANGE);
        Vector3 point;
        if (!scene->getSystem<MeshSystem>()->raycastTerrainSurface(Ray(origin, direction * range), terrain, transform, point)){
            return false;
        }
        const float distance = (point - origin).dotProduct(direction);
        if (distance < 0.0f || distance > maxDistance){
            return false;
        }
        const Matrix4 inverse = transform.modelMatrix.inverse();
        const Vector3 local = inverse * point;
        float height = 0.0f;
        Vector3 localNormal(0.0f, 1.0f, 0.0f);
        scene->getSystem<MeshSystem>()->sampleTerrainSurface(terrain, local.x, local.z, height, localNormal);

        hit = PickHit();
        hit.entity = leaf.entity;
        hit.kind = PickKind::TERRAIN;
        hit.distance = distance;
        hit.point = point;
        hit.faceNormal = facing(transformNormal(inverse, localNormal), direction);
        hit.normal = hit.faceNormal;
        return true;
    }

    bool testUI(Scene* scene, const SceneBvh::Leaf& leaf, const Vector3& origin, const Vector3& direction, float maxDistance, PickHit& hit){
        const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(leaf.entity);
        const Transform& transform = scene->getComponent<Transform>(leaf.entity);
        if (layout && !scene->findComponent<PolygonComponent>(leaf.entity) && layout->width > 0 && layout->height > 0){
            // the rect, in its plane
            Vector2 center(0, 0);
            if (const TextComponent* text = scene->findComponent<TextComponent>(leaf.entity)){
                if (text->pivotBaseline && text->stbtext) center.y = (float)text->stbtext->getAscent();
                if (text->pivotCentered) center.x = layout->width / 2.0f;
            }
            const Matrix4 inverse = transform.modelMatrix.inverse();
            const Vector3 localOrigin = inverse * origin;
            const Vector3 localDirection = inverse * (origin + direction) - localOrigin;
            if (std::fabs(localDirection.z) < 1e-12f){
                return false;
            }
            const float t = -localOrigin.z / localDirection.z;
            if (t < 0.0f || t > maxDistance){
                return false;
            }
            const Vector3 local = localOrigin + localDirection * t;
            if (local.x < -center.x || local.x > layout->width - center.x || local.y < -center.y || local.y > layout->height - center.y){
                return false;
            }
            hit = PickHit();
            hit.entity = leaf.entity;
            hit.kind = PickKind::UI;
            hit.distance = t;
            hit.point = origin + direction * t;
            hit.faceNormal = facing(transformNormal(inverse, Vector3(0.0f, 0.0f, 1.0f)), direction);
            hit.normal = hit.faceNormal;
            return true;
        }
        // a UI element without a rect (a polygon): its bounds, flat or not
        float t0 = 0.0f, t1 = maxDistance;
        const AABB& box = leaf.bounds;
        if (box.isNull() || box.isInfinite()){
            return false;
        }
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        const float lo[3] = {box.getMinimum().x, box.getMinimum().y, box.getMinimum().z};
        const float hi[3] = {box.getMaximum().x, box.getMaximum().y, box.getMaximum().z};
        int axis = 2;
        for (int a = 0; a < 3; a++){
            if (std::fabs(d[a]) < 1e-30f){
                if (o[a] < lo[a] || o[a] > hi[a]) return false;
                continue;
            }
            float tNear = (lo[a] - o[a]) / d[a];
            float tFar = (hi[a] - o[a]) / d[a];
            if (tNear > tFar) std::swap(tNear, tFar);
            if (tNear > t0){ t0 = tNear; axis = a; }
            t1 = std::min(t1, tFar);
            if (t0 > t1) return false;
        }
        hit = PickHit();
        hit.entity = leaf.entity;
        hit.kind = PickKind::UI;
        hit.distance = t0;
        hit.point = origin + direction * t0;
        Vector3 normal = Vector3::ZERO;
        normal[(unsigned)axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        hit.faceNormal = normal;
        hit.normal = normal;
        return true;
    }

}

const char* jasperengine::pickKindName(PickKind kind){
    switch (kind){
        case PickKind::MESH: return "mesh";
        case PickKind::INSTANCE: return "instance";
        case PickKind::TERRAIN: return "terrain";
        case PickKind::UI: return "ui";
        case PickKind::BOUNDS: return "bounds";
        case PickKind::ICON: return "icon";
        case PickKind::TREE: return "tree";
        case PickKind::DETAIL: return "detail";
    }
    return "mesh";
}

bool picking::testMeshAt(SceneBvh& bvh, const SceneBvh::Leaf& leaf, const Matrix4& model, const Vector3& origin, const Vector3& direction,
                         float maxDistance, const PickFilter& filter, PickHit& hit){
    return testMesh(bvh, leaf, model, origin, direction, maxDistance, filter, hit);
}

bool picking::testLeaf(SceneBvh& bvh, const SceneBvh::Leaf& leaf, const Vector3& origin, const Vector3& direction,
                       float maxDistance, const PickFilter& filter, PickHit& hit){
    Scene* scene = bvh.getScene();
    switch (leaf.kind){
        case PickKind::MESH:{
            const Transform& transform = scene->getComponent<Transform>(leaf.entity);
            return testMesh(bvh, leaf, transform.modelMatrix, origin, direction, maxDistance, filter, hit);
        }
        case PickKind::INSTANCE:{
            const Transform& transform = scene->getComponent<Transform>(leaf.entity);
            const InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(leaf.entity);
            if (leaf.subIndex >= instmesh.instances.size()){
                return false;
            }
            return testMesh(bvh, leaf, transform.modelMatrix * instanceMatrix(instmesh, instmesh.instances[leaf.subIndex]),
                            origin, direction, maxDistance, filter, hit);
        }
        case PickKind::TERRAIN:
            return testTerrain(scene, leaf, origin, direction, maxDistance, hit);
        case PickKind::UI:
            return testUI(scene, leaf, origin, direction, maxDistance, hit);
        case PickKind::BOUNDS:
            return boundsHit(leaf, origin, direction, maxDistance, false, hit);
        case PickKind::ICON:
        // foliage has no leaves: SceneBvh walks its cells (T5 R35)
        case PickKind::TREE:
        case PickKind::DETAIL:
            return false;
    }
    return false;
}

// ---- the public API

RayReturn MeshRaycastHit::toRayReturn() const{
    RayReturn result;
    result.hit = hit;
    result.distance = distance;
    result.point = point;
    result.normal = normal;
    result.body = body;
    result.colliderIndex = colliderIndex;
    return result;
}

PickFilter MeshRaycast::makeFilter(uint32_t layerMask, const MeshRaycastOptions& options){
    PickFilter filter;
    filter.layerMask = layerMask;
    filter.kinds = options.kinds & ~PICK_ICON;
    filter.includeInactive = options.includeInactive;
    filter.hitBackfaces = options.hitBackfaces;
    filter.waitForBvh = options.waitForBvh;
    filter.ignore = std::span<const Entity>(options.ignore.data(), options.ignore.size());
    return filter;
}

MeshRaycastHit MeshRaycast::toHit(const PickHit& pick, float distanceScale){
    MeshRaycastHit hit;
    hit.hit = true;
    hit.distance = pick.distance * distanceScale;
    hit.point = pick.point;
    hit.normal = pick.normal;
    hit.faceNormal = pick.faceNormal;
    hit.entity = pick.entity;
    hit.body = pick.entity;
    hit.colliderIndex = pick.submesh;
    hit.submesh = pick.submesh;
    hit.triangle = pick.triangle;
    hit.instance = pick.instance;
    hit.barycentric = pick.barycentric;
    hit.kind = pick.kind;
    hit.pending = pick.pending;
    hit.foliageId = pick.foliageId;
    hit.prototype = pick.prototype;
    return hit;
}

MeshRaycastHit MeshRaycast::raycast(Scene* scene, const Ray& ray, uint32_t layerMask, const MeshRaycastOptions& options){
    if (!scene){
        return {};
    }
    const float length = ray.getDirection().length();
    if (!(length > 0.0f) || !std::isfinite(length)){
        return {};
    }
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(ray.getOrigin(), ray.getDirection() / length, length, makeFilter(layerMask, options), hits, false);
    return hits.empty() ? MeshRaycastHit() : toHit(hits.front(), 1.0f / length);
}

MeshRaycastHit MeshRaycast::raycast(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask,
                                    const MeshRaycastOptions& options){
    const float length = direction.length();
    if (!scene || !(length > 0.0f) || !std::isfinite(length) || !(maxDistance > 0.0f)){
        return {};
    }
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(origin, direction / length, std::min(maxDistance, FLOAT_MAX), makeFilter(layerMask, options), hits, false);
    return hits.empty() ? MeshRaycastHit() : toHit(hits.front(), 1.0f);
}

std::vector<MeshRaycastHit> MeshRaycast::raycastAll(Scene* scene, const Ray& ray, uint32_t layerMask, const MeshRaycastOptions& options){
    std::vector<MeshRaycastHit> result;
    const float length = ray.getDirection().length();
    if (!scene || !(length > 0.0f) || !std::isfinite(length)){
        return result;
    }
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(ray.getOrigin(), ray.getDirection() / length, length, makeFilter(layerMask, options), hits, true);
    result.reserve(hits.size());
    for (const PickHit& hit : hits){
        result.push_back(toHit(hit, 1.0f / length));
    }
    return result;
}

std::vector<MeshRaycastHit> MeshRaycast::raycastAll(Scene* scene, Vector3 origin, Vector3 direction, float maxDistance, uint32_t layerMask,
                                                    const MeshRaycastOptions& options){
    std::vector<MeshRaycastHit> result;
    const float length = direction.length();
    if (!scene || !(length > 0.0f) || !std::isfinite(length) || !(maxDistance > 0.0f)){
        return result;
    }
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(origin, direction / length, std::min(maxDistance, FLOAT_MAX), makeFilter(layerMask, options), hits, true);
    result.reserve(hits.size());
    for (const PickHit& hit : hits){
        result.push_back(toHit(hit, 1.0f));
    }
    return result;
}

MeshRaycastHit MeshRaycast::raycastEntity(Scene* scene, Entity entity, const Ray& ray, const MeshRaycastOptions& options){
    if (!scene || entity == NULL_ENTITY){
        return {};
    }
    const float length = ray.getDirection().length();
    if (!(length > 0.0f) || !std::isfinite(length)){
        return {};
    }
    PickFilter filter = makeFilter(LAYER_MASK_EVERYTHING, options);
    filter.restrictTo = entity;
    std::vector<PickHit> hits;
    scene->getPickBvh().raycast(ray.getOrigin(), ray.getDirection() / length, length, filter, hits, false);
    return hits.empty() ? MeshRaycastHit() : toHit(hits.front(), 1.0f / length);
}

void MeshRaycast::prepare(Scene* scene, const std::vector<Entity>& entities){
    if (scene){
        scene->getPickBvh().prepare(std::span<const Entity>(entities.data(), entities.size()));
    }
}

MeshRaycastStats MeshRaycast::getStats(){
    const PickCacheStats cache = PickCache::get().stats();
    MeshRaycastStats stats;
    stats.built = cache.built;
    stats.pending = cache.pending;
    stats.bytes = cache.bytes;
    stats.buildMsTotal = cache.buildMsTotal;
    stats.inlineWaits = cache.inlineWaits;
    return stats;
}
