// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The tree part (binned build over entity bounds, bottom-up refit, ray and frustum traversal)
// is adapted from OpenEngine's scene TLAS (Engine/Modules/Scene/Source/TlasBuilder.cpp,
// TlasRefit.cpp, TlasTraverse.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License (NOTICE.md), without its sectors and asynchronous requests: here
// it is refitted when a query runs, from what changed since the last one.

#ifndef SCENEBVH_H
#define SCENEBVH_H

#include "Export.h"
#include "ecs/ChangeTracking.h"
#include "ecs/Signature.h"
#include "math/AABB.h"
#include "math/Plane.h"
#include "math/Ray.h"
#include "picking/PickCache.h"
#include "picking/PickTypes.h"

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jasperengine {

    class Scene;

    // A binary BVH over boxes, one box per leaf. Nodes are in depth-first order: a subtree is
    // the index range [node, node + 2 * leaves - 1), children come after their parent.
    class JASPERENGINE_API SceneBvhTree {
    public:
        struct Node {
            float mn[3];
            float mx[3];
            uint32_t left = UINT32_MAX;   // inner: children; leaf: left is the leaf index, right UINT32_MAX
            uint32_t right = UINT32_MAX;
            uint32_t parent = UINT32_MAX;
            uint32_t leafCount = 0;       // leaves under it
        };

        // a refit of more leaves than this runs as a parallelFor over subtrees (S06)
        static constexpr size_t PARALLEL_REFIT_LEAVES = 4096;

        void build(std::span<const AABB> boxes);
        void clear();

        // leaf boxes changed (setLeafBox first): their nodes and their ancestors
        void setLeafBox(uint32_t leaf, const AABB& box);
        void refit(std::span<const uint32_t> leaves);
        // every node from the leaf boxes (parallel: subtrees on the job system's workers)
        void refitAll(bool parallel);

        bool empty() const { return nodes.empty(); }
        size_t getLeafCount() const { return leafNode.size(); }
        const std::vector<Node>& getNodes() const { return nodes; }
        AABB getBounds() const;
        AABB getLeafBox(uint32_t leaf) const;

        // Leaves whose box the ray (origin + t * direction, t in [0, tMax]) meets, nearest box
        // first. visit(leaf, tEnter) returns the new tMax (the hit found, or the same tMax
        // to keep going): boxes past it are skipped.
        template<typename Visit>
        void raycast(const Vector3& origin, const Vector3& direction, float tMax, Visit&& visit) const;
        // Leaves whose box is not wholly outside one of the planes (positive side inside)
        template<typename Visit>
        void frustum(const Plane planes[6], Visit&& visit) const;

    private:
        std::vector<Node> nodes;
        std::vector<uint32_t> leafNode;

        static bool slab(const Node& node, const float origin[3], const float invDir[3], float tMax, float& tEnter);
        void refitNode(uint32_t node);
    };

    // The pickable objects of one scene under a SceneBvhTree, kept up to date at query time:
    // a query refits the leaves marked dirty (RenderSystem recomputed their bounds, a
    // Changed<Transform> since the last query, a layer change), rebuilds when entities or
    // their pickable components came or went or after REBUILD_AFTER_REFITS refits, and
    // recomputes the volatile ones (points, lines, particles, 2D occluders). Created on the
    // first query (Scene::getPickBvh), main thread only; a scene nobody queries pays nothing.
    class JASPERENGINE_API SceneBvh {
    public:
        static constexpr uint32_t REBUILD_AFTER_REFITS = 64;
        // the most hits a raycast with `all` returns
        static constexpr size_t MAX_ALL_HITS = 256;

        struct Leaf {
            AABB bounds;                   // world
            Entity entity = NULL_ENTITY;
            PickKind kind = PickKind::MESH;
            uint32_t subIndex = 0;         // the instance of an INSTANCE leaf
            bool volatileBounds = false;   // recomputed by every query
        };

        struct Stats {
            size_t leaves = 0;
            uint64_t builds = 0;
            uint64_t refits = 0;
            double lastBuildMs = 0.0;
            double lastRefitMs = 0.0;
        };

        explicit SceneBvh(Scene* scene);
        ~SceneBvh();
        SceneBvh(const SceneBvh&) = delete;
        SceneBvh& operator=(const SceneBvh&) = delete;

        // The entity's bounds changed (RenderSystem, a layer change): refit at the next query
        void markDirty(Entity entity);
        // An entity or one of its components came or went: rebuild at the next query
        void markStructureChanged();
        // the components are ones picking reads (an add or remove of them is a new structure)
        bool tracks(const Signature& components) const { return trackedComponents.intersects(components); }

        // Closest hit along ray (direction's length is the range), or with `all` every hit
        // (one per leaf, nearest first, at most MAX_ALL_HITS). Hits are appended to `out`.
        void raycast(const Ray& ray, const PickFilter& filter, std::vector<PickHit>& out, bool all);
        // the same with a unit direction and a range (world units; FLT_MAX: unbounded)
        void raycast(const Vector3& origin, const Vector3& direction, float maxDistance, const PickFilter& filter,
                     std::vector<PickHit>& out, bool all);
        // Entities whose leaves' bounds touch the frustum (positive side inside): the
        // broadphase of a box select. Each entity once.
        void frustum(const Plane planes[6], const PickFilter& filter, std::vector<Entity>& out);
        // The world bounds of the leaves the filter passes (Frame All)
        AABB getBounds(const PickFilter& filter);

        // Queues background builds for the meshes of these entities (all when empty), so a
        // later query does not wait. A mesh still loading is queued once it has data.
        void prepare(std::span<const Entity> entities = {});
        // Editor: every mesh is prepared when it loads (R9); processPrepareQueue runs it
        void setPrepareOnLoad(bool prepareOnLoad);
        bool isPrepareOnLoad() const { return prepareOnLoad; }
        void processPrepareQueue();

        // The cache entry the mesh of an entity is bound to (null for none, a small mesh or
        // one without CPU data); binds it (queuing its build) when needed
        PickCache::EntryPtr meshEntry(Entity entity, bool create = true);

        const std::vector<Leaf>& getLeaves();
        const SceneBvhTree& getTree() const { return tree; }
        Stats getStats() const;
        Scene* getScene() const { return scene; }

        // Posed copies of skinned and morphed meshes, per entity, by pose epoch
        struct PosedMesh {
            uint64_t geometryHash = 0;
            uint64_t poseHash = 0;
            std::shared_ptr<MeshBvh> bvh;          // a refitted copy of the bind-pose tree
            std::vector<uint32_t> indices;          // the gather's, to refit the next pose
            std::vector<Vector3> bindPositions;
            std::vector<Vector3> posed;
        };
        // the posed tree of an entity's mesh at its current pose (refitted when the pose
        // changed), null when its bind-pose tree is not built yet
        const MeshBvh* posedTree(Entity entity, const MeshBvh& bindPose, uint64_t geometryHash);

    private:
        struct MeshBinding {
            uint64_t hash = 0;
            PickCache::EntryPtr entry;
            uint32_t triangles = 0;
        };

        Scene* scene;
        Signature trackedComponents;
        SceneBvhTree tree;
        std::vector<Leaf> leaves;
        // entity -> its first leaf and how many (instances have one each)
        std::unordered_map<Entity, std::pair<uint32_t, uint32_t>> entityLeaves;
        std::vector<uint32_t> volatileLeaves;
        std::unordered_set<Entity> dirtyEntities;
        std::unordered_map<Entity, MeshBinding> meshBindings;
        std::unordered_map<Entity, PosedMesh> posedMeshes;
        std::vector<Entity> prepareQueue;
        ChangeGate transformGate;
        bool built = false;
        bool structureChanged = true;
        bool prepareOnLoad = false;
        uint32_t refitsSinceBuild = 0;
        Stats stats;

        void collectLeaves();
        bool computeLeafBounds(const Leaf& leaf, AABB& bounds) const;
        void update();
        void rebuild();
        void releaseBindings();
        bool passes(const Leaf& leaf, const PickFilter& filter) const;
        MeshBinding* bindMesh(Entity entity, bool create, bool inlineBuild);
    };

    // ---- templates

    template<typename Visit>
    void SceneBvhTree::raycast(const Vector3& origin, const Vector3& direction, float tMax, Visit&& visit) const{
        if (nodes.empty()){
            return;
        }
        const float o[3] = {origin.x, origin.y, origin.z};
        float inv[3];
        const float d[3] = {direction.x, direction.y, direction.z};
        for (int a = 0; a < 3; a++){
            float value = d[a];
            if (value > -1e-30f && value < 1e-30f){
                value = value < 0.0f ? -1e-30f : 1e-30f;
            }
            inv[a] = 1.0f / value;
        }

        struct Entry { uint32_t node; float tEnter; };
        Entry stack[128];
        int top = 0;
        float tEnter;
        if (!slab(nodes[0], o, inv, tMax, tEnter)){
            return;
        }
        stack[top++] = {0u, tEnter};
        while (top > 0){
            const Entry entry = stack[--top];
            if (entry.tEnter > tMax){
                continue;
            }
            const Node& node = nodes[entry.node];
            if (node.right == UINT32_MAX){
                tMax = visit(node.left, entry.tEnter);
                continue;
            }
            float tLeft, tRight;
            const bool hitLeft = slab(nodes[node.left], o, inv, tMax, tLeft);
            const bool hitRight = slab(nodes[node.right], o, inv, tMax, tRight);
            // the nearer child is popped first
            if (hitLeft && hitRight){
                if (tLeft <= tRight){
                    if (top < 127) stack[top++] = {node.right, tRight};
                    stack[top++] = {node.left, tLeft};
                }else{
                    if (top < 127) stack[top++] = {node.left, tLeft};
                    stack[top++] = {node.right, tRight};
                }
            }else if (hitLeft){
                stack[top++] = {node.left, tLeft};
            }else if (hitRight){
                stack[top++] = {node.right, tRight};
            }
        }
    }

    template<typename Visit>
    void SceneBvhTree::frustum(const Plane planes[6], Visit&& visit) const{
        if (nodes.empty()){
            return;
        }
        struct Entry { uint32_t node; bool inside; };
        std::vector<Entry> stack;
        stack.push_back({0u, false});
        while (!stack.empty()){
            const Entry entry = stack.back();
            stack.pop_back();
            const Node& node = nodes[entry.node];
            bool inside = entry.inside;
            if (!inside){
                const Vector3 centre((node.mn[0] + node.mx[0]) * 0.5f, (node.mn[1] + node.mx[1]) * 0.5f, (node.mn[2] + node.mx[2]) * 0.5f);
                const Vector3 half((node.mx[0] - node.mn[0]) * 0.5f, (node.mx[1] - node.mn[1]) * 0.5f, (node.mx[2] - node.mn[2]) * 0.5f);
                bool outside = false;
                bool all = true;
                for (int p = 0; p < 6 && !outside; p++){
                    const Plane::Side side = planes[p].getSide(centre, half);
                    outside = side == Plane::NEGATIVE_SIDE;
                    all = all && side == Plane::POSITIVE_SIDE;
                }
                if (outside){
                    continue;
                }
                inside = all;
            }
            if (node.right == UINT32_MAX){
                visit(node.left);
                continue;
            }
            stack.push_back({node.right, inside});
            stack.push_back({node.left, inside});
        }
    }

}

#endif // SCENEBVH_H
