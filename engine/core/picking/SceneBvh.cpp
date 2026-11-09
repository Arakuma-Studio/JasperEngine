// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The tree's binned build, refit and traversals are adapted from OpenEngine's scene TLAS
// (Engine/Modules/Scene/Source/TlasBuilder.cpp, TlasRefit.cpp, TlasTraverse.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md).

#include "SceneBvh.h"
#include "BinnedSah.h"
#include "MeshBvhSimd.h"
#include "MeshPose.h"
#include "MeshRaycast.h"

#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "subsystem/ParticleSystemState.h"
#include "particles/AnalyticParticles.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "jobs/JobSystem.h"
#include "subsystem/RenderSystem.h"
#include "profiler/Profile.h"
#include "util/STBText.h"

#include <cstring>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::picking;

namespace {

    constexpr float FLOAT_MAX = std::numeric_limits<float>::max();

    // a tree as deep as this splits at the median (the traversal stack holds 128)
    constexpr uint32_t TREE_SAH_DEPTH = 48;

    void boxOf(const AABB& aabb, float mn[3], float mx[3]){
        if (aabb.isNull()){
            mn[0] = mn[1] = mn[2] = FLOAT_MAX;
            mx[0] = mx[1] = mx[2] = -FLOAT_MAX;
            return;
        }
        if (aabb.isInfinite()){
            mn[0] = mn[1] = mn[2] = -FLOAT_MAX;
            mx[0] = mx[1] = mx[2] = FLOAT_MAX;
            return;
        }
        const Vector3& a = aabb.getMinimum();
        const Vector3& b = aabb.getMaximum();
        mn[0] = a.x; mn[1] = a.y; mn[2] = a.z;
        mx[0] = b.x; mx[1] = b.y; mx[2] = b.z;
    }

    double millisecondsSince(std::chrono::steady_clock::time_point start){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // what GraphicUtils::getRectTransformCenter gives the editor: a text's baseline and
    // centring move its rect
    Vector2 rectCenter(Scene* scene, Entity entity, const RectTransformComponent& layout){
        Vector2 center(0, 0);
        if (const TextComponent* text = scene->findComponent<TextComponent>(entity)){
            if (text->pivotBaseline && text->stbtext){
                center.y = (float)text->stbtext->getAscent();
            }
            if (text->pivotCentered){
                center.x = layout.width / 2.0f;
            }
        }
        return center;
    }

    Matrix4 instanceMatrix(const InstancedMeshComponent& instmesh, const InstanceData& instance){
        // as SceneRender::getInstanceOBB and the renderer: a billboard ignores the rotation
        const Matrix4 rotation = instmesh.instancedBillboard ? Matrix4() : instance.rotation.getRotationMatrix();
        return Matrix4::translateMatrix(instance.position) * rotation * Matrix4::scaleMatrix(instance.scale);
    }

    AABB instanceLocalBounds(const MeshComponent& mesh){
        AABB local = mesh.verticesAABB;
        if (local.isNull() || (local.getMinimum() == Vector3::ZERO && local.getMaximum() == Vector3::ZERO)){
            local = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f));
        }
        return local;
    }

}

// ---- SceneBvhTree

void SceneBvhTree::clear(){
    nodes.clear();
    leafNode.clear();
}

AABB SceneBvhTree::getBounds() const{
    if (nodes.empty() || nodes[0].mn[0] > nodes[0].mx[0]){
        return AABB();
    }
    return AABB(nodes[0].mn[0], nodes[0].mn[1], nodes[0].mn[2], nodes[0].mx[0], nodes[0].mx[1], nodes[0].mx[2]);
}

AABB SceneBvhTree::getLeafBox(uint32_t leaf) const{
    if (leaf >= leafNode.size()){
        return AABB();
    }
    const Node& node = nodes[leafNode[leaf]];
    if (node.mn[0] > node.mx[0]){
        return AABB();
    }
    return AABB(node.mn[0], node.mn[1], node.mn[2], node.mx[0], node.mx[1], node.mx[2]);
}

namespace {

    // 10 bits of each axis interleaved (the Morton order of a centroid in the cloud's bounds)
    uint32_t spreadBits(uint32_t v){
        v = (v | (v << 16)) & 0x030000FFu;
        v = (v | (v << 8)) & 0x0300F00Fu;
        v = (v | (v << 4)) & 0x030C30C3u;
        v = (v | (v << 2)) & 0x09249249u;
        return v;
    }

}

void SceneBvhTree::build(std::span<const AABB> boxes){
    clear();
    const size_t count = boxes.size();
    if (count == 0){
        return;
    }

    std::vector<float> centroids(count * 3);
    std::vector<float> bounds(count * 6);
    float cloudMin[3] = { FLOAT_MAX,  FLOAT_MAX,  FLOAT_MAX};
    float cloudMax[3] = {-FLOAT_MAX, -FLOAT_MAX, -FLOAT_MAX};
    for (size_t i = 0; i < count; i++){
        float mn[3], mx[3];
        boxOf(boxes[i], mn, mx);
        for (int a = 0; a < 3; a++){
            bounds[i * 6 + a] = mn[a];
            bounds[i * 6 + 3 + a] = mx[a];
            // an empty box sorts at the origin, an unbounded one too
            const float c = (mn[a] <= mx[a] && mn[a] > -FLOAT_MAX && mx[a] < FLOAT_MAX) ? (mn[a] + mx[a]) * 0.5f : 0.0f;
            centroids[i * 3 + a] = c;
            cloudMin[a] = c < cloudMin[a] ? c : cloudMin[a];
            cloudMax[a] = c > cloudMax[a] ? c : cloudMax[a];
        }
    }
    // each leaf's Morton code in the cloud's centroid bounds (the lower levels split by it)
    std::vector<uint32_t> codes(count);
    for (size_t i = 0; i < count; i++){
        uint32_t code = 0;
        for (int a = 0; a < 3; a++){
            const float extent = cloudMax[a] - cloudMin[a];
            const float t = extent > 0.0f ? (centroids[i * 3 + a] - cloudMin[a]) / extent : 0.0f;
            const uint32_t q = (uint32_t)std::min(1023.0f, std::max(0.0f, t * 1023.0f));
            code |= spreadBits(q) << (2 - a);
        }
        codes[i] = code;
    }

    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < (uint32_t)count; i++){
        order[i] = i;
    }

    nodes.reserve(count * 2 - 1);
    leafNode.assign(count, 0);

    // Pre-order: a node's index is taken when it is popped, so each subtree is one range.
    // Large ranges split by binned SAH (binned on a sample of at most SAH_SAMPLE, the
    // partition gathering each side's centroid bounds); a range of LBVH_RANGE or fewer is
    // sorted by Morton code once and split at its highest differing code bit (a binary
    // search per node, no pass over the range).
    constexpr uint32_t LBVH_RANGE = 512;
    constexpr uint32_t SAH_SAMPLE = 2048;
    struct Work {
        uint32_t first, count, parent, depth;
        bool right;
        bool sorted;
        float cmn[3], cmx[3];
    };
    std::vector<Work> work;
    {
        Work root{0u, (uint32_t)count, UINT32_MAX, 0u, false, false, {}, {}};
        for (int a = 0; a < 3; a++){
            root.cmn[a] = cloudMin[a];
            root.cmx[a] = cloudMax[a];
        }
        work.push_back(root);
    }
    auto rangeBounds = [&](uint32_t first, uint32_t rangeCount, float cmn[3], float cmx[3]){
        for (int a = 0; a < 3; a++){
            cmn[a] = FLOAT_MAX;
            cmx[a] = -FLOAT_MAX;
        }
        for (uint32_t i = 0; i < rangeCount; i++){
            const float* c = &centroids[(size_t)order[first + i] * 3];
            for (int a = 0; a < 3; a++){
                cmn[a] = c[a] < cmn[a] ? c[a] : cmn[a];
                cmx[a] = c[a] > cmx[a] ? c[a] : cmx[a];
            }
        }
    };
    std::vector<uint32_t> sample;
    while (!work.empty()){
        const Work item = work.back();
        work.pop_back();

        const uint32_t index = (uint32_t)nodes.size();
        nodes.emplace_back();
        Node& node = nodes.back();
        node.parent = item.parent;
        node.leafCount = item.count;
        if (item.parent != UINT32_MAX){
            (item.right ? nodes[item.parent].right : nodes[item.parent].left) = index;
        }

        if (item.count == 1){
            const uint32_t leaf = order[item.first];
            nodes[index].left = leaf;
            nodes[index].right = UINT32_MAX;
            for (int a = 0; a < 3; a++){
                nodes[index].mn[a] = bounds[(size_t)leaf * 6 + a];
                nodes[index].mx[a] = bounds[(size_t)leaf * 6 + 3 + a];
            }
            leafNode[leaf] = index;
            continue;
        }

        Work left{item.first, 0u, index, item.depth + 1, false, item.sorted, {}, {}};
        Work right{item.first, 0u, index, item.depth + 1, true, item.sorted, {}, {}};
        uint32_t leftCount = 0;
        uint32_t* begin = order.data() + item.first;

        if (item.sorted || item.count <= LBVH_RANGE || item.depth >= TREE_SAH_DEPTH){
            if (!item.sorted){
                std::sort(begin, begin + item.count, [&](uint32_t x, uint32_t y){
                    return codes[x] < codes[y] || (codes[x] == codes[y] && x < y);
                });
                left.sorted = right.sorted = true;
            }
            const uint32_t firstCode = codes[begin[0]];
            const uint32_t lastCode = codes[begin[item.count - 1]];
            if (firstCode == lastCode){
                leftCount = item.count / 2;
            }else{
                // the first element with the highest differing bit set
                const uint32_t bit = 31u - (uint32_t)std::countl_zero(firstCode ^ lastCode);
                const uint32_t above = ~((2u << bit) - 1u);   // the bits over `bit` (codes are 30-bit)
                const uint32_t pivot = (firstCode & above) | (1u << bit);
                uint32_t lo = 0, hi = item.count - 1;
                while (lo < hi){
                    const uint32_t mid = (lo + hi) / 2;
                    if (codes[begin[mid]] < pivot) lo = mid + 1; else hi = mid;
                }
                leftCount = lo;
                if (leftCount == 0 || leftCount >= item.count){
                    leftCount = item.count / 2;
                }
            }
        }else{
            for (int a = 0; a < 3; a++){
                left.cmn[a] = right.cmn[a] = FLOAT_MAX;
                left.cmx[a] = right.cmx[a] = -FLOAT_MAX;
            }
            BinnedSahRange range;
            if (item.count > SAH_SAMPLE){
                const uint32_t stride = item.count / SAH_SAMPLE;
                sample.clear();
                for (uint32_t i = 0; i < item.count; i += stride){
                    sample.push_back(begin[i]);
                }
                range.order = sample;
            }else{
                range.order = std::span<const uint32_t>(begin, item.count);
            }
            range.centroids = centroids;
            range.bounds = bounds;
            for (int a = 0; a < 3; a++){
                range.centroidMin[a] = item.cmn[a];
                range.centroidMax[a] = item.cmx[a];
            }
            const BinnedSahCandidate candidate = findBestBinnedSahSplit(range);
            bool split = false;
            if (candidate.found){
                // the predicate runs once per element: it also gathers each side's centroid bounds
                uint32_t* middle = std::partition(begin, begin + item.count, [&](uint32_t id){
                    const float* c = &centroids[(size_t)id * 3];
                    const bool goesLeft = binnedSahBinOf(c[candidate.axis], candidate.axisMin, candidate.scale) <= candidate.bin;
                    Work& side = goesLeft ? left : right;
                    for (int a = 0; a < 3; a++){
                        side.cmn[a] = c[a] < side.cmn[a] ? c[a] : side.cmn[a];
                        side.cmx[a] = c[a] > side.cmx[a] ? c[a] : side.cmx[a];
                    }
                    return goesLeft;
                });
                leftCount = (uint32_t)(middle - begin);
                split = leftCount > 0 && leftCount < item.count;
            }
            if (!split){
                int axis = 0;
                for (int a = 1; a < 3; a++){
                    if (item.cmx[a] - item.cmn[a] > item.cmx[axis] - item.cmn[axis]) axis = a;
                }
                leftCount = item.count / 2;
                std::nth_element(begin, begin + leftCount, begin + item.count, [&](uint32_t x, uint32_t y){
                    const float cx = centroids[(size_t)x * 3 + axis];
                    const float cy = centroids[(size_t)y * 3 + axis];
                    return cx < cy || (cx == cy && x < y);
                });
                rangeBounds(item.first, leftCount, left.cmn, left.cmx);
                rangeBounds(item.first + leftCount, item.count - leftCount, right.cmn, right.cmx);
            }
        }

        left.count = leftCount;
        right.first = item.first + leftCount;
        right.count = item.count - leftCount;
        // the left subtree is popped (numbered) first
        work.push_back(right);
        work.push_back(left);
    }

    refitAll(false);
}

void SceneBvhTree::setLeafBox(uint32_t leaf, const AABB& box){
    if (leaf >= leafNode.size()){
        return;
    }
    Node& node = nodes[leafNode[leaf]];
    boxOf(box, node.mn, node.mx);
}

void SceneBvhTree::refitNode(uint32_t index){
    Node& node = nodes[index];
    const Node& left = nodes[node.left];
    const Node& right = nodes[node.right];
    for (int a = 0; a < 3; a++){
        node.mn[a] = left.mn[a] < right.mn[a] ? left.mn[a] : right.mn[a];
        node.mx[a] = left.mx[a] > right.mx[a] ? left.mx[a] : right.mx[a];
    }
}

void SceneBvhTree::refit(std::span<const uint32_t> leaves){
    if (leaves.size() > PARALLEL_REFIT_LEAVES){
        refitAll(true);
        return;
    }
    for (const uint32_t leaf : leaves){
        if (leaf >= leafNode.size()){
            continue;
        }
        uint32_t index = nodes[leafNode[leaf]].parent;
        while (index != UINT32_MAX){
            Node& node = nodes[index];
            const float old[6] = {node.mn[0], node.mn[1], node.mn[2], node.mx[0], node.mx[1], node.mx[2]};
            refitNode(index);
            // unchanged: what is above already holds it
            if (old[0] == node.mn[0] && old[1] == node.mn[1] && old[2] == node.mn[2] &&
                    old[3] == node.mx[0] && old[4] == node.mx[1] && old[5] == node.mx[2]){
                break;
            }
            index = node.parent;
        }
    }
}

void SceneBvhTree::refitAll(bool parallel){
    if (nodes.empty()){
        return;
    }
    const size_t count = nodes.size();
    if (!parallel || leafNode.size() <= PARALLEL_REFIT_LEAVES || !jobs::hasSystem() || jobs::system().getWorkerCount() == 0){
        for (size_t i = count; i-- > 0;){
            if (nodes[i].right != UINT32_MAX){
                refitNode((uint32_t)i);
            }
        }
        return;
    }

    // subtrees of about leaves / 64 leaves each refit in parallel (each is an index range,
    // children after parents), then the nodes above them
    const uint32_t target = (uint32_t)std::max<size_t>(leafNode.size() / 64, 256);
    std::vector<uint32_t> roots;
    std::vector<uint32_t> above;
    std::vector<uint32_t> stack{0u};
    while (!stack.empty()){
        const uint32_t index = stack.back();
        stack.pop_back();
        const Node& node = nodes[index];
        if (node.right == UINT32_MAX){
            continue;
        }
        if (node.leafCount <= target){
            roots.push_back(index);
            continue;
        }
        above.push_back(index);
        stack.push_back(node.left);
        stack.push_back(node.right);
    }
    jobs::parallelFor(roots.size(), [&](size_t r){
        const uint32_t root = roots[r];
        const size_t end = (size_t)root + 2u * nodes[root].leafCount - 1u;
        for (size_t i = end; i-- > root;){
            if (nodes[i].right != UINT32_MAX){
                refitNode((uint32_t)i);
            }
        }
    });
    // parents before children in `above` (pre-order): refit from the back
    for (size_t i = above.size(); i-- > 0;){
        refitNode(above[i]);
    }
}

bool SceneBvhTree::slab(const Node& node, const float origin[3], const float invDir[3], float tMax, float& tEnter){
    float t0 = 0.0f;
    float t1 = tMax;
    for (int a = 0; a < 3; a++){
        if (node.mn[a] > node.mx[a]){
            return false; // empty
        }
        // parallel to the slab: inside it (its faces included) or never
        if (invDir[a] >= 1e29f || invDir[a] <= -1e29f){
            if (origin[a] < node.mn[a] || origin[a] > node.mx[a]){
                return false;
            }
            continue;
        }
        float tNear = (node.mn[a] - origin[a]) * invDir[a];
        float tFar = (node.mx[a] - origin[a]) * invDir[a];
        if (tNear > tFar){
            std::swap(tNear, tFar);
        }
        tFar *= SLAB_ROBUST;
        t0 = tNear > t0 ? tNear : t0;
        t1 = tFar < t1 ? tFar : t1;
        if (t0 > t1){
            return false;
        }
    }
    tEnter = t0;
    return true;
}

// ---- SceneBvh

SceneBvh::SceneBvh(Scene* scene): scene(scene){
    trackedComponents.set(scene->getComponentId<Transform>());
    trackedComponents.set(scene->getComponentId<MeshComponent>());
    trackedComponents.set(scene->getComponentId<InstancedMeshComponent>());
    trackedComponents.set(scene->getComponentId<TerrainComponent>());
    trackedComponents.set(scene->getComponentId<UIComponent>());
    trackedComponents.set(scene->getComponentId<RectTransformComponent>());
    trackedComponents.set(scene->getComponentId<PolygonComponent>());
    trackedComponents.set(scene->getComponentId<PointsComponent>());
    trackedComponents.set(scene->getComponentId<LineRendererComponent>());
    trackedComponents.set(scene->getComponentId<ShadowCaster2DComponent>());
    trackedComponents.set(scene->getComponentId<ParticleSystemComponent>());
}

SceneBvh::~SceneBvh(){
    releaseBindings();
}

void SceneBvh::releaseBindings(){
    PickCache& cache = PickCache::get();
    for (auto& binding : meshBindings){
        cache.release(binding.second.entry);
    }
    meshBindings.clear();
    posedMeshes.clear();
}

void SceneBvh::markDirty(Entity entity){
    dirtyEntities.insert(entity);
    if (prepareOnLoad){
        prepareQueue.push_back(entity);
    }
}

void SceneBvh::markStructureChanged(){
    structureChanged = true;
}

void SceneBvh::setPrepareOnLoad(bool value){
    if (value && !prepareOnLoad){
        // what is already loaded
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            prepareQueue.push_back(meshes->getEntity(i));
        }
    }
    prepareOnLoad = value;
}

void SceneBvh::processPrepareQueue(){
    if (prepareQueue.empty()){
        return;
    }
    std::vector<Entity> queue;
    queue.swap(prepareQueue);
    std::sort(queue.begin(), queue.end());
    queue.erase(std::unique(queue.begin(), queue.end()), queue.end());
    for (Entity entity : queue){
        if (scene->isEntityCreated(entity) && scene->findComponent<MeshComponent>(entity)){
            bindMesh(entity, true, false);
        }
    }
}

void SceneBvh::prepare(std::span<const Entity> entities){
    if (entities.empty()){
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            bindMesh(meshes->getEntity(i), true, false);
        }
        return;
    }
    for (Entity entity : entities){
        if (scene->isEntityCreated(entity) && scene->findComponent<MeshComponent>(entity)){
            bindMesh(entity, true, false);
        }
    }
}

SceneBvh::MeshBinding* SceneBvh::bindMesh(Entity entity, bool create, bool inlineBuild){
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh){
        return nullptr;
    }
    PickCache& cache = PickCache::get();

    MeshBinding* binding = nullptr;
    auto found = meshBindings.find(entity);
    if (found != meshBindings.end()){
        binding = &found->second;
        // still the geometry it was bound to
        if (mesh->pickGeometryHash != 0 && mesh->pickGeometryHash == binding->hash){
            return binding;
        }
    }

    PickGeometryLayout layout;
    const bool hasData = resolveSources(*mesh, layout);
    const uint32_t triangles = hasData ? countTriangles(layout) : 0u;
    if (triangles == 0 || triangles > PickCache::getMaxTriangles()){
        // no CPU triangles (or too many): it picks by its bounds
        if (binding){
            cache.release(binding->entry);
            posedMeshes.erase(entity);
            meshBindings.erase(found);
        }
        return nullptr;
    }
    if (mesh->pickGeometryHash == 0){
        mesh->pickGeometryHash = hashSources(layout);
    }
    const uint64_t hash = mesh->pickGeometryHash;
    if (binding && binding->hash == hash && binding->entry){
        return binding;
    }

    PickCache::EntryPtr entry = cache.acquire(hash);
    if (!entry){
        if (!create){
            return nullptr;
        }
        PickGeometry geometry;
        if (!gather(*mesh, geometry)){
            return nullptr;
        }
        // a small mesh has no tree to wait for: its triangles are copied here
        entry = cache.create(hash, std::move(geometry), inlineBuild || triangles < MeshBvh::BRUTE_FORCE_TRIANGLES);
    }

    if (!binding){
        binding = &meshBindings[entity];
    }else{
        cache.release(binding->entry);
    }
    binding->hash = hash;
    binding->entry = std::move(entry);
    binding->triangles = triangles;
    posedMeshes.erase(entity);
    return binding;
}

PickCache::EntryPtr SceneBvh::meshEntry(Entity entity, bool create){
    MeshBinding* binding = bindMesh(entity, create, false);
    return binding ? binding->entry : nullptr;
}

const MeshBvh* SceneBvh::posedTree(Entity entity, const MeshBvh& bindPose, uint64_t geometryHash){
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh){
        return nullptr;
    }
    const uint64_t pose = poseHash(*mesh);
    PosedMesh& posed = posedMeshes[entity];
    if (posed.bvh && posed.geometryHash == geometryHash && posed.poseHash == pose){
        return posed.bvh.get();
    }

    PickGeometryLayout layout;
    if (posed.geometryHash != geometryHash || !posed.bvh){
        PickGeometry geometry;
        if (!gather(*mesh, geometry, &layout)){
            posedMeshes.erase(entity);
            return nullptr;
        }
        posed.bvh = std::make_shared<MeshBvh>(bindPose);
        posed.leafCorners = posed.bvh->leafCorners(geometry.indices);
        posed.indices = std::move(geometry.indices);
        posed.bindPositions = std::move(geometry.positions);
        posed.geometryHash = geometryHash;
    }else if (!resolveSources(*mesh, layout)){
        posedMeshes.erase(entity);
        return nullptr;
    }

    JE_PROFILE_SCOPE("MeshBvh pose");
    poseGeometry(*mesh, layout, posed.bindPositions, posed.posed);
    posed.bvh->refitCorners(posed.posed, posed.leafCorners);
    posed.poseHash = pose;
    return posed.bvh.get();
}

void SceneBvh::collectLeaves(){
    leaves.clear();
    entityLeaves.clear();
    volatileLeaves.clear();

    auto add = [&](Entity entity, PickKind kind, uint32_t subIndex, bool isVolatile){
        Leaf leaf;
        leaf.entity = entity;
        leaf.kind = kind;
        leaf.subIndex = subIndex;
        leaf.volatileBounds = isVolatile;
        computeLeafBounds(leaf, leaf.bounds);
        const uint32_t index = (uint32_t)leaves.size();
        auto it = entityLeaves.find(entity);
        if (it == entityLeaves.end()){
            entityLeaves.emplace(entity, std::make_pair(index, 1u));
        }else{
            it->second.second++;
        }
        if (isVolatile){
            volatileLeaves.push_back(index);
        }
        leaves.push_back(leaf);
    };

    auto transforms = scene->getComponentArray<Transform>();
    const ComponentId meshId = scene->getComponentId<MeshComponent>();
    const ComponentId instancedId = scene->getComponentId<InstancedMeshComponent>();
    const ComponentId terrainId = scene->getComponentId<TerrainComponent>();
    const ComponentId uiId = scene->getComponentId<UIComponent>();
    const ComponentId pointsId = scene->getComponentId<PointsComponent>();
    const ComponentId linesId = scene->getComponentId<LineRendererComponent>();
    const ComponentId occluderId = scene->getComponentId<ShadowCaster2DComponent>();
    const ComponentId particlesId = scene->getComponentId<ParticleSystemComponent>();
#ifdef JASPERENGINE_EDITOR
    // the editor's gizmos, icons and grid live in the scene they annotate: never picked
    const RenderSystem* renderSystem = scene->getSystem<RenderSystem>().get();
#endif
    for (size_t i = 0; i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
#ifdef JASPERENGINE_EDITOR
        if (renderSystem->isEditorOverlay(entity)){
            continue;
        }
#endif
        const Signature signature = scene->getSignature(entity);
        if (signature.test(particlesId)){
            // a particle system (S20, it renders itself): its simulated bounds, never per particle
            add(entity, PickKind::BOUNDS, 0, true);
        }
        if (signature.test(meshId)){
            if (signature.test(terrainId)){
                add(entity, PickKind::TERRAIN, 0, false);
            }else if (signature.test(instancedId)){
                const InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
                const size_t drawn = std::min<size_t>(instmesh.instances.size(), instmesh.maxInstances);
                for (size_t instance = 0; instance < drawn; instance++){
                    add(entity, PickKind::INSTANCE, (uint32_t)instance, false);
                }
            }else{
                add(entity, PickKind::MESH, 0, false);
            }
        }else if (signature.test(uiId)){
            add(entity, PickKind::UI, 0, false);
        }else if (signature.test(pointsId) || signature.test(linesId) || signature.test(occluderId)){
            add(entity, PickKind::BOUNDS, 0, true);
        }
    }
}

bool SceneBvh::computeLeafBounds(const Leaf& leaf, AABB& bounds) const{
    bounds = AABB();
    const Entity entity = leaf.entity;
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        return false;
    }

    switch (leaf.kind){
        case PickKind::TERRAIN:{
            const TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            const float half = terrain.terrainSize * 0.5f;
            bounds = transform->modelMatrix * AABB(-half, std::min(0.0f, terrain.maxHeight), -half,
                                                    half, std::max(0.0f, terrain.maxHeight), half);
            return true;
        }
        case PickKind::INSTANCE:{
            const InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
            if (leaf.subIndex >= instmesh.instances.size()){
                return false;
            }
            const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            bounds = (transform->modelMatrix * instanceMatrix(instmesh, instmesh.instances[leaf.subIndex])) * instanceLocalBounds(mesh);
            return true;
        }
        case PickKind::MESH:{
            const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            bounds = mesh.worldAABB;
            if (bounds.isNull()){
                const AABB local = mesh.skinnedAABB.isNull() ? mesh.aabb : mesh.skinnedAABB;
                if (!local.isNull()){
                    bounds = transform->modelMatrix * local;
                }
            }
            return !bounds.isNull();
        }
        case PickKind::UI:{
            const UIComponent& ui = scene->getComponent<UIComponent>(entity);
            bounds = ui.worldAABB;
            if (!scene->findComponent<PolygonComponent>(entity)){
                if (const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity)){
                    if (layout->width > 0 && layout->height > 0){
                        const Vector2 center = rectCenter(scene, entity, *layout);
                        bounds = transform->modelMatrix * AABB(-center.x, -center.y, 0, layout->width - center.x, layout->height - center.y, 0);
                    }
                }
            }
            return !bounds.isNull();
        }
        case PickKind::BOUNDS:{
            if (const ParticleSystemComponent* system = scene->findComponent<ParticleSystemComponent>(entity)){
                // the world bounds of its particles last frame (ParticleSubsystem's), none before
                // it has simulated any
                if (!system->state || !system->state->hasBounds){
                    return false;
                }
                const particles::ParticleSystemState& state = *system->state;
                if (state.runtime.isAnalytic()){
                    // an Analytic system's culling bounds hold every particle it may ever draw:
                    // picking takes the CPU twin's live particles now (S49 R19)
                    float lo[3], hi[3];
                    if (!particles::AnalyticParticles::liveBounds(*state.runtime.analytic(), lo, hi))
                        return false;
                    const float* m = state.runtime.simToWorld();
                    bounds.setNull();
                    for (int corner = 0; corner < 8; corner++){
                        const float p[3] = {(corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2]};
                        bounds.merge(Vector3(m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                                             m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]));
                    }
                    return true;
                }
                bounds = AABB(state.boundsMin[0], state.boundsMin[1], state.boundsMin[2], state.boundsMax[0], state.boundsMax[1], state.boundsMax[2]);
                return true;
            }
            AABB local;
            if (const PointsComponent* points = scene->findComponent<PointsComponent>(entity)){
                for (const PointData& point : points->points){
                    if (point.visible){
                        local.merge(point.position);
                    }
                }
            }
            if (const LineRendererComponent* lines = scene->findComponent<LineRendererComponent>(entity)){
                for (const LineData& line : lines->lines){
                    local.merge(line.pointA);
                    local.merge(line.pointB);
                }
            }
            if (const ShadowCaster2DComponent* occluder = scene->findComponent<ShadowCaster2DComponent>(entity)){
                AABB shape(Vector3(0, 0, -1), Vector3(0, 0, 1));
                if (occluder->shape == ShadowCaster2DShape::POLYGON && !occluder->points.empty()){
                    shape.setNull();
                    for (const Vector2& point : occluder->points){
                        shape.merge(Vector3(point.x, point.y, 0.0f));
                    }
                    Vector3 mn = shape.getMinimum(); mn.z = -1.0f;
                    Vector3 mx = shape.getMaximum(); mx.z = 1.0f;
                    shape.setExtents(mn, mx);
                }
                local.merge(shape);
            }
            if (local.isNull()){
                return false;
            }
            bounds = transform->modelMatrix * local;
            return true;
        }
        case PickKind::ICON:
            return false;
    }
    return false;
}

void SceneBvh::rebuild(){
    const auto start = std::chrono::steady_clock::now();
    collectLeaves();
    std::vector<AABB> boxes(leaves.size());
    for (size_t i = 0; i < leaves.size(); i++){
        boxes[i] = leaves[i].bounds;
    }
    tree.build(boxes);

    // bindings of meshes that went
    PickCache& cache = PickCache::get();
    for (auto it = meshBindings.begin(); it != meshBindings.end();){
        if (!scene->isEntityCreated(it->first) || !scene->findComponent<MeshComponent>(it->first)){
            cache.release(it->second.entry);
            posedMeshes.erase(it->first);
            it = meshBindings.erase(it);
        }else{
            ++it;
        }
    }

    dirtyEntities.clear();
    built = true;
    structureChanged = false;
    refitsSinceBuild = 0;
    stats.builds++;
    stats.leaves = leaves.size();
    stats.lastBuildMs = millisecondsSince(start);
    // what changed before this build is in it
    transformGate.last = scene->changeTick();
}

void SceneBvh::update(){
    if (!built || structureChanged || refitsSinceBuild >= REBUILD_AFTER_REFITS){
        rebuild();
        return;
    }

    // Changed<Transform> since the last query: their leaves move too
    {
        const Tick now = scene->changeTick();
        auto transforms = scene->getComponentArray<Transform>();
        if (transforms->getLastChangedTick() > transformGate.last){
            for (size_t i = 0; i < transforms->size(); i++){
                if (transforms->getChangedTick(i) > transformGate.last){
                    dirtyEntities.insert(transforms->getEntity(i));
                }
            }
        }
        transformGate.last = now;
    }

    if (dirtyEntities.empty() && volatileLeaves.empty()){
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<uint32_t> changed;
    changed.reserve(dirtyEntities.size() + volatileLeaves.size());
    auto refresh = [&](uint32_t index){
        Leaf& leaf = leaves[index];
        AABB bounds;
        computeLeafBounds(leaf, bounds);
        if (bounds != leaf.bounds){
            leaf.bounds = bounds;
            tree.setLeafBox(index, bounds);
            changed.push_back(index);
        }
    };
    for (Entity entity : dirtyEntities){
        auto it = entityLeaves.find(entity);
        if (it == entityLeaves.end()){
            // pickable now and not when built (an instanced mesh given its first instances)
            const InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
            if (instmesh && !instmesh->instances.empty() && scene->findComponent<Transform>(entity)){
                rebuild();
                return;
            }
            continue;
        }
        // an instanced mesh whose instance count changed is a new structure
        if (const InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity)){
            if (leaves[it->second.first].kind == PickKind::INSTANCE &&
                    std::min<size_t>(instmesh->instances.size(), instmesh->maxInstances) != it->second.second){
                rebuild();
                return;
            }
        }
        for (uint32_t i = 0; i < it->second.second; i++){
            refresh(it->second.first + i);
        }
    }
    dirtyEntities.clear();
    for (uint32_t index : volatileLeaves){
        refresh(index);
    }

    if (!changed.empty()){
        tree.refit(changed);
        refitsSinceBuild++;
        stats.refits++;
        stats.lastRefitMs = millisecondsSince(start);
    }
}

const std::vector<SceneBvh::Leaf>& SceneBvh::getLeaves(){
    update();
    return leaves;
}

SceneBvh::Stats SceneBvh::getStats() const{
    Stats result = stats;
    result.leaves = leaves.size();
    return result;
}

bool SceneBvh::passes(const Leaf& leaf, const PickFilter& filter) const{
    // a mesh or an instance without triangles to test (no CPU data, still building) answers
    // with its bounds
    const bool asBounds = (leaf.kind == PickKind::MESH || leaf.kind == PickKind::INSTANCE) && (filter.kinds & PICK_BOUNDS);
    if (!(filter.kinds & (1u << (uint32_t)leaf.kind)) && !asBounds){
        return false;
    }
    if (filter.restrictTo != NULL_ENTITY && leaf.entity != filter.restrictTo){
        return false;
    }
    if (!scene->isEntityCreated(leaf.entity)){
        return false;
    }
    const int layer = scene->getEntityLayer(leaf.entity);
    if (!(filter.layerMask & (1u << (layer & 31)))){
        return false;
    }
    if (filter.excludeEditorFlags && (scene->getEntityEditorFlags(leaf.entity) & filter.excludeEditorFlags)){
        return false;
    }
    if (std::find(filter.ignore.begin(), filter.ignore.end(), leaf.entity) != filter.ignore.end()){
        return false;
    }
    if (filter.accept && !filter.accept(leaf.entity)){
        return false;
    }
    if (!filter.includeInactive){
        // what is drawn (Project's isEntityDrawn): visible, active in the hierarchy, enabled
        const Transform* transform = scene->findComponent<Transform>(leaf.entity);
        if (!transform || !transform->visible){
            return false;
        }
        if (scene->hasInactiveEntities() && !scene->isEntityActiveInHierarchy(leaf.entity)){
            return false;
        }
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(leaf.entity)){
            if (!mesh->enabled) return false;
        }else if (const UIComponent* ui = scene->findComponent<UIComponent>(leaf.entity)){
            if (!ui->enabled) return false;
        }
        if (const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(leaf.entity)){
            if (!terrain->enabled) return false;
        }
        if (leaf.kind == PickKind::INSTANCE){
            const InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(leaf.entity);
            if (!instmesh.enabled || leaf.subIndex >= instmesh.instances.size() || !instmesh.instances[leaf.subIndex].visible){
                return false;
            }
        }
    }
    return true;
}

void SceneBvh::raycast(const Ray& ray, const PickFilter& filter, std::vector<PickHit>& out, bool all){
    const Vector3 direction = ray.getDirection();
    const float range = direction.length();
    if (!(range > 0.0f) || !std::isfinite(range)){
        return;
    }
    raycast(ray.getOrigin(), direction / range, range, filter, out, all);
}

void SceneBvh::raycast(const Vector3& origin, const Vector3& direction, float range, const PickFilter& filter,
                       std::vector<PickHit>& out, bool all){
    JE_PROFILE_SCOPE("SceneBvh raycast");
    update();
    if (tree.empty() || !(range > 0.0f)){
        return;
    }
    const size_t first = out.size();
    PickHit best;
    bool found = false;
    tree.raycast(origin, direction, range, [&](uint32_t index, float tEnter) -> float {
        const float bound = found && !all ? best.distance : range;
        (void)tEnter;
        const Leaf& leaf = leaves[index];
        if (!passes(leaf, filter)){
            return bound;
        }
        PickHit hit;
        if (!testLeaf(*this, leaf, origin, direction, bound, filter, hit)){
            return bound;
        }
        if (!(filter.kinds & (1u << (uint32_t)hit.kind))){
            return bound;
        }
        if (all){
            out.push_back(hit);
            return range;
        }
        if (!found || hit.distance < best.distance){
            best = hit;
            found = true;
        }
        return best.distance;
    });

    if (all){
        std::sort(out.begin() + (std::ptrdiff_t)first, out.end(), [](const PickHit& a, const PickHit& b){
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.entity != b.entity) return a.entity < b.entity;
            return a.instance < b.instance;
        });
        if (out.size() - first > MAX_ALL_HITS){
            out.resize(first + MAX_ALL_HITS);
        }
    }else if (found){
        out.push_back(best);
    }
}

void SceneBvh::frustum(const Plane planes[6], const PickFilter& filter, std::vector<Entity>& out){
    update();
    std::unordered_set<Entity> seen;
    tree.frustum(planes, [&](uint32_t index){
        const Leaf& leaf = leaves[index];
        if (!passes(leaf, filter)){
            return;
        }
        if (seen.insert(leaf.entity).second){
            out.push_back(leaf.entity);
        }
    });
}

AABB SceneBvh::getBounds(const PickFilter& filter){
    update();
    AABB bounds;
    for (const Leaf& leaf : leaves){
        if (!passes(leaf, filter)){
            continue;
        }
        if (!leaf.bounds.isNull() && !leaf.bounds.isInfinite()){
            bounds.merge(leaf.bounds);
        }
    }
    return bounds;
}
