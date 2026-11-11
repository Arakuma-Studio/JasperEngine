// SPDX-License-Identifier: MIT
//
// The placement world (design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md
// sections 5.2, 6 and 9): a Jolt world of its own, stepped by the caller, that reads the scene
// and never writes to it. Shapes come from JoltShapeBuilder, the same builders the scene's
// PhysicsSystem uses.

#include "PlacementWorld.h"

#include "ConvexDecomposition.h"
#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/Joint3DComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/SplineSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/MeshTriangles.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "JoltShapeBuilder.h"

#include "Jolt/Jolt.h"
#include "Jolt/Math/EigenValueSymmetric.h"
#include "Jolt/Math/Matrix.h"
#include "Jolt/Math/Vector.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Body/BodyFilter.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/CollisionCollectorImpl.h"
#include "Jolt/Physics/Collision/ContactListener.h"
#include "Jolt/Physics/Collision/NarrowPhaseQuery.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterTable.h"
#include "Jolt/Physics/Collision/RayCast.h"
#include "Jolt/Physics/Collision/ShapeCast.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h"
#include "Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"

#include "meshoptimizer.h"
#endif
#if JASPERENGINE_TERRAIN_WORLD
#include "component/TerrainWorldComponent.h"
#include "physics/TerrainWorldCollision.h"
#include "subsystem/TerrainWorldRuntime.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

const char* jasperengine::placementShapeKindName(PlacementShapeKind kind) {
    switch (kind) {
        case PlacementShapeKind::NONE: return "none";
        case PlacementShapeKind::AUTHORED: return "authored";
        case PlacementShapeKind::BOX: return "box";
        case PlacementShapeKind::SPHERE: return "sphere";
        case PlacementShapeKind::CAPSULE: return "capsule";
        case PlacementShapeKind::CONVEX_HULL: return "convex_hull";
        case PlacementShapeKind::DECOMPOSED: return "decomposed";
        case PlacementShapeKind::MESH: return "mesh";
        case PlacementShapeKind::HEIGHTFIELD: return "heightfield";
    }
    return "none";
}

namespace {

    uint64_t fnvMix(uint64_t hash, uint64_t value) {
        for (int i = 0; i < 8; i++) {
            hash ^= (value >> (i * 8)) & 0xFFu;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    int64_t quantise(float value) {
        if (!std::isfinite(value)) return INT64_MIN;
        return (int64_t)std::llround((double)value * 1e5);
    }

}

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    // An unselected object no larger than this, and not a static Rigidbody, is pushed around
    // by the movers when pushNeighbours is on (spec section 3.4)
    constexpr float NEIGHBOUR_MAX_SIZE = 2.0f;
    // the context a neighbour stands on: around its bounds, at least this far (m)
    constexpr float NEIGHBOUR_SUPPORT_MARGIN = 0.5f;
    // rounds of neighbours found under neighbours (each round one level of a stack)
    constexpr int MAX_SUPPORT_ROUNDS = 16;

    // world pose of a transform the render pass has not updated yet: from the topmost stale
    // ancestor down, so a moved parent carries its children along
    void resolveTransform(Scene* scene, Entity entity) {
        Entity chain[64];
        size_t count = 0;
        size_t stale = SIZE_MAX;
        for (Entity e = entity; e != NULL_ENTITY && count < 64;) {
            Transform* transform = scene->findComponent<Transform>(e);
            if (!transform) break;
            if (transform->needUpdate) stale = count;
            chain[count++] = e;
            e = transform->parent;
        }
        if (stale == SIZE_MAX) return;
        RenderSystem* render = scene->getSystem<RenderSystem>().get();
        for (size_t i = stale + 1; i-- > 0;) {
            render->updateTransform(scene->getComponent<Transform>(chain[i]), chain[i]);
        }
    }

    // The entity and its descendants, parents first (the Transform array keeps a branch together)
    std::vector<Entity> subtreeOf(Scene* scene, Entity root) {
        std::vector<Entity> entities;
        auto transforms = scene->getComponentArray<Transform>();
        if (!transforms->hasEntity(root)) {
            entities.push_back(root);
            return entities;
        }
        const size_t first = transforms->getIndex(root);
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first; i <= last && i < transforms->size(); i++) {
            entities.push_back(transforms->getEntity(i));
        }
        return entities;
    }

    bool isAncestorOf(Scene* scene, Entity ancestor, Entity entity) {
        Transform* transform = scene->findComponent<Transform>(entity);
        for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < 256; depth++) {
            if (transform->parent == ancestor) return true;
            transform = scene->findComponent<Transform>(transform->parent);
        }
        return false;
    }

    // A mesh the 3D renderer draws as triangles (not a sprite, tilemap, 2D polygon or the
    // terrain's own grid, which its heightfield stands for)
    MeshComponent* triangleMesh(Scene* scene, Entity entity) {
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || !mesh->enabled || mesh->numSubmeshes == 0) return nullptr;
        if (scene->findComponent<TerrainComponent>(entity) || scene->findComponent<SpriteComponent>(entity) ||
            scene->findComponent<TilemapComponent>(entity) || scene->findComponent<TilemapLayerComponent>(entity) ||
            scene->findComponent<MeshPolygonComponent>(entity)) {
            return nullptr;
        }
        for (size_t i = 0; i < mesh->numSubmeshes; i++) {
            if (mesh->submeshes[i].primitiveType == PrimitiveType::TRIANGLES) return mesh;
        }
        return nullptr;
    }

    InstancedMeshComponent* instancedMesh(Scene* scene, Entity entity) {
        InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
        return (instanced && instanced->enabled) ? instanced : nullptr;
    }

    TerrainComponent* terrainOf(Scene* scene, Entity entity) {
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        return (terrain && terrain->enabled) ? terrain : nullptr;
    }

    // Colliders that take part in collisions (an enabled Rigidbody, not a sensor)
    RigidbodyComponent* authoredBody(Scene* scene, Entity entity) {
        RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity);
        return (body && body->enabled && !body->sensor && !body->colliders.empty()) ? body : nullptr;
    }

    bool layerIgnored(Scene* scene, Entity entity, uint32_t mask) {
        const int layer = scene->getEntityLayer(entity);
        return layer >= 0 && layer < 32 && ((mask >> layer) & 1u) != 0;
    }

    bool editorOverlay(Scene* scene, Entity entity) {
#ifdef JASPERENGINE_EDITOR
        return scene->getSystem<RenderSystem>()->isEditorOverlay(entity);
#else
        // a game build has no editor overlays (RenderSystem keeps them in the editor only)
        (void)scene;
        (void)entity;
        return false;
#endif
    }

    bool isSkinned(MeshComponent* mesh) {
        for (size_t i = 0; i < mesh->numSubmeshes; i++) {
            if (mesh->submeshes[i].hasSkinning) return true;
        }
        return false;
    }

    // Conservative world bounds of what an entity contributes (its own components only)
    AABB entityWorldBounds(Scene* scene, Entity entity) {
        AABB bounds;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return bounds;
        resolveTransform(scene, entity);
        const Matrix4& model = transform->modelMatrix;

        if (TerrainComponent* terrain = terrainOf(scene, entity)) {
#if JASPERENGINE_TERRAIN_WORLD
            // T4 R4: a World translates only, over the box of its tiles
            if (terrain->world && terrain->world->started()) {
                jasperengine::terraincore::TileCoord low, high;
                if (terrain->world->occupancy().bounds(low, high)) {
                    const float size = terrain->world->lattice().tileSize();
                    const Vector3& at = transform->worldPosition;
                    bounds.merge(AABB(at.x + low.x * size, at.y, at.z + low.z * size, at.x + (high.x + 1) * size, at.y + terrain->world->maxHeight(),
                                      at.z + (high.z + 1) * size));
                }
                return bounds;
            }
#endif
            const float half = terrain->terrainSize * 0.5f;
            bounds.merge(model * AABB(-half, std::min(0.0f, terrain->maxHeight), -half, half, std::max(0.0f, terrain->maxHeight), half));
        } else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            if (mesh->enabled && mesh->numSubmeshes > 0) {
                bounds.merge(model * (isSkinned(mesh) ? mesh->verticesAABB : mesh->aabb));
                if (InstancedMeshComponent* instanced = instancedMesh(scene, entity)) {
                    for (const InstanceData& instance : instanced->instances) {
                        if (!instance.visible) continue;
                        const Matrix4 instanceMatrix = Matrix4::translateMatrix(instance.position) *
                                                       instance.rotation.getRotationMatrix() * Matrix4::scaleMatrix(instance.scale);
                        bounds.merge(model * (instanceMatrix * mesh->verticesAABB));
                    }
                }
            }
        }

        if (RigidbodyComponent* body = authoredBody(scene, entity)) {
            // the colliders are sized by worldScale, and placed by modelMatrix, whose largest
            // stretch is no more than its Frobenius norm
            const Vector3 scale = transform->worldScale;
            float frobenius = 0.0f;
            for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) frobenius += model.get(c, r) * model.get(c, r);
            const float maxScale = std::max(std::sqrt(frobenius), std::max(std::fabs(scale.x), std::max(std::fabs(scale.y), std::fabs(scale.z))));
            for (size_t i = 0; i < body->colliders.size(); i++) {
                const Collider3D& c = body->colliders[i];
                if ((c.type == Collider3DType::CONVEX_HULL || c.type == Collider3DType::MESH) &&
                    c.source != Collider3DSource::RAW_VERTICES && c.source != Collider3DSource::RAW_MESH) {
                    const Entity source = c.sourceEntity == NULL_ENTITY ? entity : c.sourceEntity;
                    if (source != entity) bounds.merge(entityWorldBounds(scene, source));
                    continue;
                }
                if (c.type == Collider3DType::HEIGHTFIELD) {
                    const Entity source = c.sourceEntity == NULL_ENTITY ? entity : c.sourceEntity;
                    if (source != entity) bounds.merge(entityWorldBounds(scene, source));
                    continue;
                }
                float radius = 0.0f;
                if (c.type == Collider3DType::BOX) {
                    radius = 0.5f * std::sqrt(c.width * c.width + c.height * c.height + c.depth * c.depth);
                } else if (c.type == Collider3DType::SPHERE) {
                    radius = c.radius;
                } else if (c.type == Collider3DType::CAPSULE || c.type == Collider3DType::CYLINDER) {
                    radius = c.halfHeight + c.radius;
                } else if (c.type == Collider3DType::TAPERED_CAPSULE) {
                    radius = c.halfHeight + std::max(c.topRadius, c.bottomRadius);
                } else {
                    for (size_t v = 0; v < c.vertices.size(); v++) radius = std::max(radius, (c.vertices[v] - c.position).length() + c.position.length());
                }
                const Vector3 center = model * c.position;
                const float r = radius * maxScale;
                bounds.merge(AABB(center - Vector3(r, r, r), center + Vector3(r, r, r)));
            }
        }

        return bounds;
    }

    float longestAxis(const AABB& bounds) {
        if (bounds.isNull()) return 0.0f;
        const Vector3 size = bounds.getSize();
        return std::max(size.x, std::max(size.y, size.z));
    }

    // Exact-value weld: the same position twice becomes one vertex (UV seams, flat-shaded
    // corners). Degenerate triangles, triangles with a non-finite corner and vertices no
    // triangle uses are dropped.
    void weldExact(std::vector<Vector3>& positions, std::vector<uint32_t>& indices) {
        constexpr uint32_t DROPPED = UINT32_MAX;
        std::map<std::tuple<float, float, float>, uint32_t> unique;
        std::vector<uint32_t> remap(positions.size(), DROPPED);
        std::vector<Vector3> distinct;
        distinct.reserve(positions.size());
        for (size_t i = 0; i < positions.size(); i++) {
            const Vector3& p = positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
            auto found = unique.emplace(std::make_tuple(p.x, p.y, p.z), (uint32_t)distinct.size());
            if (found.second) distinct.push_back(p);
            remap[i] = found.first->second;
        }

        std::vector<uint32_t> used(distinct.size(), DROPPED);
        std::vector<Vector3> welded;
        std::vector<uint32_t> kept;
        kept.reserve(indices.size());
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            uint32_t corner[3];
            bool valid = true;
            for (int c = 0; c < 3; c++) {
                corner[c] = indices[t + c] < remap.size() ? remap[indices[t + c]] : DROPPED;
                valid = valid && corner[c] != DROPPED;
            }
            if (!valid || corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) continue;
            for (int c = 0; c < 3; c++) {
                if (used[corner[c]] == DROPPED) {
                    used[corner[c]] = (uint32_t)welded.size();
                    welded.push_back(distinct[corner[c]]);
                }
                kept.push_back(used[corner[c]]);
            }
        }
        positions.swap(welded);
        indices.swap(kept);
    }

}

namespace {

    namespace Layers {
        constexpr JPH::ObjectLayer STATIC = 0;
        constexpr JPH::ObjectLayer MOVING = 1;
        constexpr JPH::uint COUNT = 2;
    }

    namespace BroadPhaseLayers {
        constexpr JPH::BroadPhaseLayer STATIC(0);
        constexpr JPH::BroadPhaseLayer MOVING(1);
        constexpr JPH::uint COUNT = 2;
    }

    // the step's contact buffers come from the shared 10 MiB temp allocator (864 bytes a
    // contact constraint, measured), so these stay inside it; a step that needs more reports it
    // (contactsOverflowed)
    constexpr JPH::uint MAX_BODIES = 4096;
    constexpr JPH::uint MAX_BODY_PAIRS = 8192;
    constexpr JPH::uint MAX_CONTACT_CONSTRAINTS = 8192;

    // Jolt's default slop lets a resting body sink 2 cm; placement wants millimetres
    constexpr float PENETRATION_SLOP = 0.0005f;
    constexpr float DEFAULT_BAUMGARTE = 0.2f;
    constexpr float DEFAULT_MAX_PENETRATION_DISTANCE = 0.2f;
    constexpr float DEFAULT_DAMPING = 0.05f;
    constexpr float FRICTION = 0.5f;
    // a driven mover that starts this deep in a body passes through it until they are apart
    constexpr float GHOST_DEPTH = 0.005f;
    // rolling resistance coefficient: a ball on a slope steeper than atan(this) still rolls
    constexpr float ROLLING_RESISTANCE = 0.2f;
    // thinner than this (m), a body gets more solver iterations (solverStepsFor)
    constexpr float THIN_BODY = 0.05f;
    // a manifold this close (negative depth: speculative) counts as touching
    constexpr float TOUCH_DISTANCE = 0.002f;
    // the contact normal (into the body) within ~45 degrees of up: resting on the other body
    constexpr float RESTING_COS = 0.7f;
    // how far (m) a simplified context mesh may stray from the original surface
    constexpr float MESH_SIMPLIFY_ERROR = 0.01f;
    // primitive fits closer than this (relative volume over the hull) replace the hull
    constexpr float PRIMITIVE_FIT_ERROR = 0.05f;
    // a mesh filling less of its hull than this is concave enough to want a decomposition
    constexpr float CONCAVE_FILL = 0.9f;

    JPH::Vec3 toJolt(const Vector3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    Vector3 fromJolt(JPH::Vec3Arg v) { return Vector3(v.GetX(), v.GetY(), v.GetZ()); }
    Quaternion fromJolt(JPH::QuatArg q) { return Quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

    // Jolt wants unit quaternions; an invalid one is identity
    JPH::Quat toJolt(const Quaternion& q) {
        const double lengthSquared = (double)q.w * q.w + (double)q.x * q.x + (double)q.y * q.y + (double)q.z * q.z;
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-12) return JPH::Quat::sIdentity();
        const double inverse = 1.0 / std::sqrt(lengthSquared);
        return JPH::Quat((float)(q.x * inverse), (float)(q.y * inverse), (float)(q.z * inverse), (float)(q.w * inverse));
    }

    uint32_t bodyKey(const JPH::BodyID& id) { return id.GetIndexAndSequenceNumber(); }

    // Every manifold Jolt reports, kept until it is removed. Jolt calls the listener from its
    // job threads during PhysicsSystem::Update, hence the lock; the main thread reads the map
    // between updates. A std::map keeps the iteration order (and so the sums made from it)
    // the same however the threads interleaved.
    //
    // Jolt also removes every contact of a body that falls asleep, though nothing moved apart.
    // Removals are only recorded during the update; settle() then keeps a removed contact whose
    // bodies are all asleep (a dormant contact, what the body rests on) and drops the others.
    // A dormant contact goes once a body of it wakes: Jolt reports the contacts it still has
    // afresh in that update, and a pair that came apart is not reported again.
    class PlacementContactTracker final : public JPH::ContactListener {
    public:
        struct Contact {
            uint32_t body1 = 0;
            uint32_t body2 = 0;
            JPH::Float3 normal;   // from body 1 toward body 2
            float depth = 0.0f;   // negative: speculative, not touching yet
            bool dormant = false; // removed by Jolt as its bodies fell asleep
        };
        typedef std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> Key;

        // Pairs that don't collide: a driven mover and a body it started inside (or was teleported
        // into), until they come apart. Changed only between updates; Jolt's threads only read it.
        typedef std::pair<uint32_t, uint32_t> Pair;
        static Pair pairOf(uint32_t a, uint32_t b) { return a < b ? Pair(a, b) : Pair(b, a); }
        std::set<Pair> ghosts;

        JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg,
                                              const JPH::CollideShapeResult&) override {
            if (!ghosts.empty() && ghosts.count(pairOf(bodyKey(body1.GetID()), bodyKey(body2.GetID())))) {
                return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
            }
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
            record(body1, body2, manifold);
        }

        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override {
            record(body1, body2, manifold);
        }

        // The bodies can't be read here (Jolt has them locked, some mid-update): decided in settle()
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            std::lock_guard<std::mutex> lock(mutex);
            removed.push_back(Key(bodyKey(pair.GetBody1ID()), pair.GetSubShapeID1().GetValue(),
                                  bodyKey(pair.GetBody2ID()), pair.GetSubShapeID2().GetValue()));
        }

        // After each update, on the main thread. active(body key) is whether the body is awake
        // (false for static bodies and bodies no longer in the world).
        template<typename F>
        void settle(F active) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const Key& key : removed) {
                auto found = contacts.find(key);
                if (found == contacts.end()) continue;
                if (!active(found->second.body1) && !active(found->second.body2)) found->second.dormant = true;
                else contacts.erase(found);
            }
            removed.clear();
            for (auto it = contacts.begin(); it != contacts.end();) {
                if (it->second.dormant && (active(it->second.body1) || active(it->second.body2))) it = contacts.erase(it);
                else ++it;
            }
        }

        // A body left the world or changed shape: its sub-shape ids are gone with it
        void forgetBody(const JPH::BodyID& id) {
            const uint32_t key = bodyKey(id);
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = contacts.begin(); it != contacts.end();) {
                if (it->second.body1 == key || it->second.body2 == key) it = contacts.erase(it);
                else ++it;
            }
            removed.erase(std::remove_if(removed.begin(), removed.end(), [key](const Key& k) {
                return std::get<0>(k) == key || std::get<2>(k) == key;
            }), removed.end());
            for (auto it = ghosts.begin(); it != ghosts.end();) {
                if (it->first == key || it->second == key) it = ghosts.erase(it);
                else ++it;
            }
        }

        template<typename F>
        void forEach(F f) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& entry : contacts) f(entry.second);
        }

    private:
        void record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold) {
            Contact contact;
            contact.body1 = bodyKey(body1.GetID());
            contact.body2 = bodyKey(body2.GetID());
            manifold.mWorldSpaceNormal.StoreFloat3(&contact.normal);
            contact.depth = manifold.mPenetrationDepth;
            const Key key(contact.body1, manifold.mSubShapeID1.GetValue(), contact.body2, manifold.mSubShapeID2.GetValue());
            std::lock_guard<std::mutex> lock(mutex);
            contacts[key] = contact;
        }

        std::mutex mutex;
        std::map<Key, Contact> contacts;
        std::vector<Key> removed;   // during the update in flight
    };

    // One convex piece of a body, placed in the body's space
    struct ShapePart {
        JPH::ShapeRefC shape;
        JPH::Vec3 position = JPH::Vec3::sZero();
        JPH::Quat rotation = JPH::Quat::sIdentity();
        PlacementShapeKind kind = PlacementShapeKind::NONE;
        int hulls = 0;
    };

    struct DecompositionRequest {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
    };

    // A mesh's geometry in its own space: welded, unscaled (the decomposition input) and scaled
    // by the entity's world scale (the shape input)
    struct PartGeometry {
        std::vector<Vector3> local;
        std::vector<Vector3> scaled;
        std::vector<uint32_t> indices;
    };

    // Principal axes of a point set (columns of a rotation) with the box they span
    struct OrientedBox {
        JPH::Mat44 rotation = JPH::Mat44::sIdentity();
        JPH::Vec3 center = JPH::Vec3::sZero();
        JPH::Vec3 halfExtent = JPH::Vec3::sZero();
    };

    OrientedBox axisAlignedBox(const std::vector<Vector3>& points) {
        JPH::AABox box;
        for (const Vector3& p : points) box.Encapsulate(toJolt(p));
        OrientedBox out;
        out.center = box.GetCenter();
        out.halfExtent = box.GetExtent();
        return out;
    }

    OrientedBox principalBox(const std::vector<Vector3>& points) {
        OrientedBox out;
        if (points.empty()) return out;

        double mean[3] = {0, 0, 0};
        for (const Vector3& p : points) { mean[0] += p.x; mean[1] += p.y; mean[2] += p.z; }
        for (double& m : mean) m /= (double)points.size();

        double covariance[3][3] = {};
        for (const Vector3& p : points) {
            const double d[3] = {p.x - mean[0], p.y - mean[1], p.z - mean[2]};
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) covariance[r][c] += d[r] * d[c];
        }

        JPH::Matrix<3, 3> matrix;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) matrix(r, c) = (float)(covariance[r][c] / (double)points.size());
        JPH::Matrix<3, 3> eigenVectors;
        eigenVectors.SetIdentity();   // EigenValueSymmetric starts from it
        JPH::Vector<3> eigenValues;
        if (!JPH::EigenValueSymmetric(matrix, eigenVectors, eigenValues)) {
            return axisAlignedBox(points);
        }

        JPH::Vec3 axis[3];
        for (int c = 0; c < 3; c++) axis[c] = JPH::Vec3(eigenVectors(0, c), eigenVectors(1, c), eigenVectors(2, c)).NormalizedOr(JPH::Vec3::sAxisX());
        // a right-handed frame, so it is a rotation
        axis[1] = (axis[1] - axis[1].Dot(axis[0]) * axis[0]).NormalizedOr(axis[0].GetNormalizedPerpendicular());
        axis[2] = axis[0].Cross(axis[1]);

        JPH::Vec3 low = JPH::Vec3::sReplicate(FLT_MAX), high = JPH::Vec3::sReplicate(-FLT_MAX);
        for (const Vector3& p : points) {
            const JPH::Vec3 v = toJolt(p);
            const JPH::Vec3 projected(v.Dot(axis[0]), v.Dot(axis[1]), v.Dot(axis[2]));
            low = JPH::Vec3::sMin(low, projected);
            high = JPH::Vec3::sMax(high, projected);
        }

        out.rotation = JPH::Mat44(JPH::Vec4(axis[0], 0), JPH::Vec4(axis[1], 0), JPH::Vec4(axis[2], 0), JPH::Vec4(0, 0, 0, 1));
        out.center = out.rotation.Multiply3x3(0.5f * (low + high));
        out.halfExtent = 0.5f * (high - low);
        return out;
    }

    // A convex radius that rounds no more than a tenth of the thinnest side, so a pencil or a
    // screw keeps its shape (Jolt's 5 cm default would round it away)
    float convexRadiusFor(float thinnestSide) {
        return std::max(0.0f, std::min(JPH::cDefaultConvexRadius, 0.1f * thinnestSide));
    }

    // How far a vertex may be left outside a hull: Jolt's 1 mm would cut a pencil's 7 mm
    // section down to a lopsided few
    float hullToleranceFor(float thinnestSide) {
        return std::max(1e-5f, std::min(1e-3f, 0.01f * thinnestSide));
    }

    // A generated hull: its convex radius and tolerance follow its thinnest side
    JPH::ShapeRefC thinHull(const std::vector<Vector3>& points, float thinnestSide) {
        return JoltShapeBuilder::buildConvexHullShape(points, nullptr, convexRadiusFor(thinnestSide), hullToleranceFor(thinnestSide));
    }

    JPH::ShapeRefC withDensity(JPH::ShapeRefC shape) {
        // density 1: mass is the volume (spec section 5.2); each shape here is new and unshared
        if (shape && shape->GetType() == JPH::EShapeType::Convex) {
            const_cast<JPH::ConvexShape*>(static_cast<const JPH::ConvexShape*>(shape.GetPtr()))->SetDensity(1.0f);
        }
        return shape;
    }

    JPH::ShapeRefC boxShape(JPH::Vec3Arg halfExtent) {
        const float thinnest = 2.0f * halfExtent.ReduceMin();
        JPH::BoxShapeSettings settings(halfExtent, convexRadiusFor(thinnest));
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        return result.IsValid() ? withDensity(result.Get()) : JPH::ShapeRefC();
    }

    // Signed volume and closedness of a welded triangle mesh. Closedness is judged on
    // positions snapped to a small grid, so a seam whose sides differ in the last bit still
    // closes.
    float closedMeshVolume(const std::vector<Vector3>& positions, const std::vector<uint32_t>& indices, float size, bool& closed) {
        const double quantum = std::max(1e-9, (double)size * 1e-5);
        std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> snapped;
        std::vector<uint32_t> id(positions.size());
        for (size_t i = 0; i < positions.size(); i++) {
            const Vector3& p = positions[i];
            auto key = std::make_tuple((int64_t)std::llround(p.x / quantum), (int64_t)std::llround(p.y / quantum), (int64_t)std::llround(p.z / quantum));
            id[i] = snapped.emplace(key, (uint32_t)snapped.size()).first->second;
        }

        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        double volume = 0.0;
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const Vector3& a = positions[indices[t]];
            const Vector3& b = positions[indices[t + 1]];
            const Vector3& c = positions[indices[t + 2]];
            volume += (double)a.dotProduct(b.crossProduct(c)) / 6.0;
            const uint32_t v[3] = {id[indices[t]], id[indices[t + 1]], id[indices[t + 2]]};
            for (int e = 0; e < 3; e++) {
                const uint32_t from = v[e], to = v[(e + 1) % 3];
                if (from == to) continue;
                edges[std::make_pair(std::min(from, to), std::max(from, to))]++;
            }
        }
        closed = !edges.empty();
        for (const auto& edge : edges) {
            if (edge.second != 2) { closed = false; break; }
        }
        return (float)std::fabs(volume);
    }


    struct DynamicShape {
        JPH::ShapeRefC shape;
        PlacementShapeKind kind = PlacementShapeKind::NONE;
        int hullCount = 0;
        std::vector<uint64_t> keys;
        std::vector<DecompositionRequest> requests;
        std::string warning;
    };

    // What one entity of a body's subtree gives it
    struct PartSource {
        Entity entity = NULL_ENTITY;
        RigidbodyComponent* body = nullptr;   // authored colliders to use
        MeshComponent* mesh = nullptr;        // a mesh to generate a shape from
    };

    // A mesh part's generated shape: primitive set when a primitive fits, else hull
    struct GeneratedFit {
        JPH::ShapeRefC primitive;
        PlacementShapeKind kind = PlacementShapeKind::NONE;
        JPH::Vec3 position = JPH::Vec3::sZero();
        JPH::Quat rotation = JPH::Quat::sIdentity();
        JPH::ShapeRefC hull;
        float hullVolume = 0.0f;
        float longest = 0.0f;
    };

    // The shapes of bodies, read from a scene with a world's settings; no world needed, so the
    // editor's bake buttons reach the same geometry and cache keys the movers use
    struct BodyShapes {
        Scene* scene;
        const PlacementWorldSettings& settings;

    // The pose of entity relative to the body at (bodyPosition, bodyRotation); its scale (and
    // shear) is in linearOf(). False when the entity has no Transform (gone).
    bool relativePose(Entity entity, JPH::RVec3Arg bodyPosition, JPH::QuatArg bodyRotation, JPH::Vec3& position, JPH::Quat& rotation) const {
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return false;
        const JPH::Quat inverse = bodyRotation.Conjugated();
        position = inverse * JPH::Vec3(toJolt(transform->worldPosition) - bodyPosition);
        rotation = (inverse * toJolt(transform->worldRotation)).Normalized();
        return true;
    }

    // Entity space into its part frame (at its world position and rotation): the linear part of
    // its modelMatrix, so a turned child of a non-uniformly scaled parent is shaped as drawn
    Matrix4 linearOf(Entity entity) const {
        resolveTransform(scene, entity);
        return JoltShapeBuilder::entityLinearMatrix(scene, entity);
    }

    // Authored colliders of one entity as parts in the body's space. Movers can't be triangle
    // meshes or heightfields: an authored mesh becomes the hull of the same vertices, a
    // heightfield is left out.
    void authoredParts(Entity entity, RigidbodyComponent& body, bool dynamic, JPH::Vec3Arg partPosition, JPH::QuatArg partRotation,
                       std::vector<ShapePart>& parts) const {
        const Matrix4 linear = linearOf(entity);
        for (size_t i = 0; i < body.colliders.size(); i++) {
            Collider3D collider = body.colliders[i];
            if (dynamic) {
                if (collider.type == Collider3DType::HEIGHTFIELD) continue;
                if (collider.type == Collider3DType::MESH) {
                    collider.type = Collider3DType::CONVEX_HULL;
                    if (collider.source == Collider3DSource::RAW_MESH) collider.source = Collider3DSource::RAW_VERTICES;
                }
            }
            JPH::ShapeRefC shape = JoltShapeBuilder::buildColliderShape(scene, entity, collider, nullptr);
            if (!shape) continue;
            if (dynamic) {
                if (shape->MustBeStatic()) continue;
                withDensity(shape);
            }
            ShapePart part;
            part.shape = shape;
            part.position = partPosition + partRotation * toJolt(linear * collider.position);
            part.rotation = (partRotation * toJolt(collider.rotation)).Normalized();
            part.kind = PlacementShapeKind::AUTHORED;
            parts.push_back(part);
        }
    }

    bool partGeometry(Entity entity, MeshComponent* mesh, PartGeometry& out) const {
        MeshTriangles::appendMeshTriangles(mesh, Matrix4(), out.local, out.indices);
        weldExact(out.local, out.indices);
        if (out.local.empty()) return false;
        const Matrix4 linear = linearOf(entity);
        out.scaled.resize(out.local.size());
        for (size_t i = 0; i < out.local.size(); i++) out.scaled[i] = linear * out.local[i];
        return true;
    }

    DecompositionParams decompositionParams() const {
        DecompositionParams params;
        params.maxHulls = settings.decompositionMaxHulls;
        params.resolution = settings.decompositionResolution;
        params.maxVerticesPerHull = settings.decompositionMaxVerticesPerHull;
        return params;
    }

    // A mesh part's generated shape, before any decomposition (spec section 5.2): a fitted
    // primitive when it is within 5% of the hull's volume (a box for flat meshes and for meshes
    // too small for a hull), else the convex hull. False when there is nothing to build.
    bool fitGenerated(const PartGeometry& geometry, GeneratedFit& out) const {
        const std::vector<Vector3>& points = geometry.scaled;
        const OrientedBox aligned = axisAlignedBox(points);
        const OrientedBox principal = principalBox(points);
        const float longest = 2.0f * std::max(aligned.halfExtent.ReduceMax(), principal.halfExtent.ReduceMax());
        out.longest = longest;
        if (longest <= 0.0f) return false;

        // flat (a plane, a sheet of paper): an oriented box with a minimum thickness
        const float alignedThin = 2.0f * aligned.halfExtent.ReduceMin();
        const float principalThin = 2.0f * principal.halfExtent.ReduceMin();
        if (std::min(alignedThin, principalThin) < 0.01f * longest) {
            const OrientedBox& flat = alignedThin <= principalThin ? aligned : principal;
            const float minHalf = 0.5f * std::max(0.01f * longest, 0.001f);
            const JPH::Vec3 half = JPH::Vec3::sMax(flat.halfExtent, JPH::Vec3::sReplicate(minHalf));
            out.primitive = boxShape(half);
            out.kind = PlacementShapeKind::BOX;
            out.position = flat.center;
            out.rotation = flat.rotation.GetQuaternion();
            return out.primitive != nullptr;
        }

        const float thinnest = std::min(alignedThin, principalThin);
        out.hull = withDensity(thinHull(points, thinnest));
        if (!out.hull) {
            // too small or thin for Jolt's hull builder: its bounding box
            out.primitive = boxShape(JPH::Vec3::sMax(aligned.halfExtent, JPH::Vec3::sReplicate(0.0005f)));
            out.kind = PlacementShapeKind::BOX;
            out.position = aligned.center;
            out.rotation = JPH::Quat::sIdentity();
            return out.primitive != nullptr;
        }
        const float hullVolume = out.hull->GetVolume();
        out.hullVolume = hullVolume;

        // primitive fits, each enclosing every vertex; the error is its extra volume over the hull
        struct Fit { PlacementShapeKind kind; float error; JPH::ShapeRefC shape; JPH::Vec3 position; JPH::Quat rotation; };
        std::vector<Fit> fits;
        if (hullVolume > 0.0f) {
            {   // sphere about the bounds' centre
                float radius = 0.0f;
                for (const Vector3& p : points) radius = std::max(radius, (toJolt(p) - aligned.center).Length());
                const float volume = 4.0f / 3.0f * JPH::JPH_PI * radius * radius * radius;
                JPH::SphereShapeSettings settings(radius);
                JPH::ShapeSettings::ShapeResult result = settings.Create();
                if (result.IsValid()) fits.push_back({PlacementShapeKind::SPHERE, volume / hullVolume - 1.0f, withDensity(result.Get()), aligned.center, JPH::Quat::sIdentity()});
            }
            {   // the tighter of the axis-aligned and principal boxes
                const float alignedVolume = 8.0f * aligned.halfExtent.GetX() * aligned.halfExtent.GetY() * aligned.halfExtent.GetZ();
                const float principalVolume = 8.0f * principal.halfExtent.GetX() * principal.halfExtent.GetY() * principal.halfExtent.GetZ();
                const bool useAligned = alignedVolume <= principalVolume * 1.0001f;
                const OrientedBox& box = useAligned ? aligned : principal;
                if (JPH::ShapeRefC shape = boxShape(box.halfExtent)) {
                    fits.push_back({PlacementShapeKind::BOX, std::min(alignedVolume, principalVolume) / hullVolume - 1.0f, shape, box.center,
                                    useAligned ? JPH::Quat::sIdentity() : box.rotation.GetQuaternion()});
                }
            }
            {   // capsule along the longest principal axis
                int axisIndex = 0;
                for (int i = 1; i < 3; i++) if (principal.halfExtent[i] > principal.halfExtent[axisIndex]) axisIndex = i;
                const JPH::Vec3 axis = principal.rotation.GetColumn3(axisIndex);
                float radius = 0.0f;
                for (const Vector3& p : points) {
                    const JPH::Vec3 d = toJolt(p) - principal.center;
                    radius = std::max(radius, (d - d.Dot(axis) * axis).Length());
                }
                float halfHeight = 0.0f;
                for (const Vector3& p : points) {
                    const JPH::Vec3 d = toJolt(p) - principal.center;
                    const float along = d.Dot(axis);
                    const float across = (d - along * axis).Length();
                    halfHeight = std::max(halfHeight, std::fabs(along) - std::sqrt(std::max(0.0f, radius * radius - across * across)));
                }
                if (radius > 0.0f && halfHeight > 1e-4f * radius) {
                    const float volume = JPH::JPH_PI * radius * radius * 2.0f * halfHeight + 4.0f / 3.0f * JPH::JPH_PI * radius * radius * radius;
                    JPH::CapsuleShapeSettings settings(halfHeight, radius);
                    JPH::ShapeSettings::ShapeResult result = settings.Create();
                    if (result.IsValid()) {
                        fits.push_back({PlacementShapeKind::CAPSULE, volume / hullVolume - 1.0f, withDensity(result.Get()), principal.center,
                                        JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis)});
                    }
                }
            }
        }
        // the simplest wins a tie: sphere, box, capsule
        const Fit* best = nullptr;
        for (const Fit& fit : fits) {
            if (!best || fit.error < best->error - 1e-3f) best = &fit;
        }
        if (best && best->error < PRIMITIVE_FIT_ERROR) {
            out.primitive = best->shape;
            out.kind = best->kind;
            out.position = best->position;
            out.rotation = best->rotation;
        }
        return true;

    }

    // The generated collider of a mesh part (policy AUTO): the fitted primitive, else the
    // cached decomposition of a concave mesh, else the convex hull, with a decomposition
    // requested meanwhile
    void generatedParts(Entity entity, MeshComponent* mesh, JPH::Vec3Arg partPosition, JPH::QuatArg partRotation,
                        std::vector<ShapePart>& parts, std::vector<uint64_t>& keys, std::vector<DecompositionRequest>& requests) const {
        PartGeometry geometry;
        GeneratedFit fit;
        if (!partGeometry(entity, mesh, geometry) || !fitGenerated(geometry, fit)) return;

        auto place = [&](JPH::ShapeRefC shape, JPH::Vec3Arg localPosition, JPH::QuatArg localRotation, PlacementShapeKind kind, int hulls) {
            ShapePart part;
            part.shape = shape;
            part.position = partPosition + partRotation * localPosition;
            part.rotation = (partRotation * localRotation).Normalized();
            part.kind = kind;
            part.hulls = hulls;
            parts.push_back(part);
        };

        if (fit.primitive) {
            place(fit.primitive, fit.position, fit.rotation, fit.kind, 0);
            return;
        }

        ConvexDecompositionCache* cache = settings.decompositionCache;
        if (cache && settings.decomposition != PlacementDecomposition::NEVER) {
            bool wanted = settings.decomposition == PlacementDecomposition::ALWAYS;
            if (!wanted) {
                bool closed = false;
                const float volume = closedMeshVolume(geometry.scaled, geometry.indices, fit.longest, closed);
                wanted = !closed || volume < CONCAVE_FILL * fit.hullVolume;
            }
            if (wanted) {
                const DecompositionParams params = decompositionParams();
                const uint64_t key = hashDecompositionInput(geometry.local, geometry.indices, params);
                std::vector<ConvexHullPoints> hulls;
                if (cache->find(key, hulls)) {
                    // a linear map keeps every hull convex
                    const Matrix4 linear = linearOf(entity);
                    std::vector<ShapePart> pieces;
                    for (const ConvexHullPoints& h : hulls) {
                        std::vector<Vector3> scaled(h.points.size());
                        for (size_t i = 0; i < h.points.size(); i++) scaled[i] = linear * h.points[i];
                        OrientedBox bounds = axisAlignedBox(scaled);
                        JPH::ShapeRefC piece = withDensity(thinHull(scaled, 2.0f * bounds.halfExtent.ReduceMin()));
                        if (!piece) continue;
                        ShapePart part;
                        part.shape = piece;
                        part.position = partPosition;
                        part.rotation = partRotation;
                        part.kind = PlacementShapeKind::DECOMPOSED;
                        part.hulls = 1;
                        pieces.push_back(part);
                    }
                    if (!pieces.empty()) {
                        parts.insert(parts.end(), pieces.begin(), pieces.end());
                        return;
                    }
                } else if (cache->status(key) != DecompositionStatus::FAILED || cache->failureReason(key) == "cancelled") {
                    // a cancelled one is asked for again (request() queues it again)
                    keys.push_back(key);
                    requests.push_back({geometry.local, geometry.indices});
                }
            }
        }

        place(fit.hull, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), PlacementShapeKind::CONVEX_HULL, 1);
    }

    // The entities of root's subtree a body is built from, and what each gives: its authored
    // colliders, its mesh to generate a shape from (policy rules), both or neither. False and
    // *why when root's subtree can't move (terrain, instanced meshes, geometry still loading).
    bool collectSources(Entity root, std::vector<Entity>& entities, std::vector<PartSource>& sources, std::string* why) const {
        const PlacementColliderPolicy policy = settings.colliders;
        resolveTransform(scene, root);

        for (Entity e : subtreeOf(scene, root)) {
            if (editorOverlay(scene, e)) continue;
            if (e != root && (!scene->isEntityActiveInHierarchy(e) || layerIgnored(scene, e, settings.ignoreLayerMask))) continue;
            entities.push_back(e);
        }

        if (terrainOf(scene, root)) {
            if (why) *why = "terrain, treated as environment";
            return false;
        }
        for (Entity e : entities) {
            if (instancedMesh(scene, e)) {
                if (why) *why = "instanced mesh, treated as environment";
                return false;
            }
        }
        for (Entity e : entities) {
            if (MeshTriangles::isGeometryLoading(scene, e)) {
                if (why) *why = "mesh still loading";
                return false;
            }
        }

        // meshes an authored collider already stands for (AUTO generates nothing for them)
        std::set<Entity> authoredSources;
        for (Entity e : entities) {
            RigidbodyComponent* body = authoredBody(scene, e);
            if (!body) continue;
            for (size_t i = 0; i < body->colliders.size(); i++) {
                const Collider3D& c = body->colliders[i];
                if ((c.type == Collider3DType::CONVEX_HULL || c.type == Collider3DType::MESH) &&
                    c.source != Collider3DSource::RAW_VERTICES && c.source != Collider3DSource::RAW_MESH) {
                    for (Entity source : MeshTriangles::meshSourceEntities(scene, c.sourceEntity == NULL_ENTITY ? e : c.sourceEntity)) {
                        authoredSources.insert(source);
                    }
                }
            }
        }

        for (Entity e : entities) {
            if (terrainOf(scene, e)) continue;
            PartSource source;
            source.entity = e;
            source.body = policy != PlacementColliderPolicy::GENERATED ? authoredBody(scene, e) : nullptr;
            const bool generate = policy == PlacementColliderPolicy::GENERATED || policy == PlacementColliderPolicy::BOTH ||
                                  (policy == PlacementColliderPolicy::AUTO && !source.body && !authoredSources.count(e));
            source.mesh = generate ? triangleMesh(scene, e) : nullptr;
            if (source.body || source.mesh) sources.push_back(source);
        }
        return true;
    }

    // The checks addMover makes of root itself before building
    bool canMove(Entity root, std::string* why) const {
        if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root) || !scene->findComponent<Transform>(root)) {
            if (why) *why = "no mesh or collider";
            return false;
        }
        if (!scene->isEntityActiveInHierarchy(root)) {
            if (why) *why = "inactive";
            return false;
        }
        if (layerIgnored(scene, root, settings.ignoreLayerMask)) {
            if (why) *why = "ignored layer";
            return false;
        }
        return true;
    }

    // Every generated mesh part of root's subtree a decomposition would replace (no primitive
    // fits it), with its cache key and geometry, for baking. False when root can't move.
    template<typename F>
    bool forEachDecomposable(Entity root, std::vector<std::string>* notes, F visit) const {
        std::string why;
        std::vector<Entity> entities;
        std::vector<PartSource> sources;
        if (!canMove(root, &why) || !collectSources(root, entities, sources, &why)) {
            if (notes) notes->push_back(scene && scene->isEntityCreated(root) ? scene->getEntityName(root) + ": " + why : why);
            return false;
        }
        const DecompositionParams params = decompositionParams();
        for (const PartSource& source : sources) {
            if (!source.mesh) continue;
            PartGeometry geometry;
            GeneratedFit fit;
            if (!partGeometry(source.entity, source.mesh, geometry) || !fitGenerated(geometry, fit)) {
                if (notes) notes->push_back(scene->getEntityName(source.entity) + ": no geometry");
                continue;
            }
            if (fit.primitive) {
                if (notes) notes->push_back(scene->getEntityName(source.entity) + ": fits a " + placementShapeKindName(fit.kind) + ", nothing to decompose");
                continue;
            }
            visit(source.entity, hashDecompositionInput(geometry.local, geometry.indices, params), geometry);
        }
        return true;
    }

    bool buildDynamicShape(Entity root, DynamicShape& out, std::string* why) const {
        const PlacementColliderPolicy policy = settings.colliders;
        std::vector<Entity> entities;
        std::vector<PartSource> sources;
        Transform* rootTransform = scene->isEntityCreated(root) ? scene->findComponent<Transform>(root) : nullptr;
        if (!rootTransform) {
            if (why) *why = "no mesh or collider";
            return false;
        }
        if (!collectSources(root, entities, sources, why)) return false;
        const JPH::RVec3 rootPosition(toJolt(rootTransform->worldPosition));
        const JPH::Quat rootRotation = toJolt(rootTransform->worldRotation);

        std::vector<ShapePart> parts;
        bool skinned = false;
        for (const PartSource& source : sources) {
            resolveTransform(scene, source.entity);
            JPH::Vec3 position;
            JPH::Quat rotation;
            if (!relativePose(source.entity, rootPosition, rootRotation, position, rotation)) continue;
            if (source.body) authoredParts(source.entity, *source.body, true, position, rotation, parts);
            if (source.mesh) {
                skinned = skinned || isSkinned(source.mesh);
                generatedParts(source.entity, source.mesh, position, rotation, parts, out.keys, out.requests);
            }
        }

        if (parts.empty()) {
            if (why) *why = policy == PlacementColliderPolicy::AUTHORED ? "no authored collider" : "no mesh or collider";
            return false;
        }

        if (parts.size() == 1 && parts[0].position.IsNearZero(0.0f) && parts[0].rotation.IsClose(JPH::Quat::sIdentity(), 0.0f)) {
            out.shape = parts[0].shape;
        } else if (parts.size() == 1) {
            JPH::RotatedTranslatedShapeSettings settings(parts[0].position, parts[0].rotation, parts[0].shape);
            JPH::ShapeSettings::ShapeResult result = settings.Create();
            if (result.IsValid()) out.shape = result.Get();
        } else {
            JPH::StaticCompoundShapeSettings compound;
            for (const ShapePart& part : parts) compound.AddShape(part.position, part.rotation, part.shape);
            JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.IsValid()) out.shape = result.Get();
        }
        if (!out.shape) {
            if (why) *why = "no mesh or collider";
            return false;
        }

        // what the body reports: decomposed if any part is, authored if any part is, the
        // primitive when every part is the same one, else hull
        bool anyDecomposed = false, anyAuthored = false, same = true;
        for (const ShapePart& part : parts) {
            anyDecomposed = anyDecomposed || part.kind == PlacementShapeKind::DECOMPOSED;
            anyAuthored = anyAuthored || part.kind == PlacementShapeKind::AUTHORED;
            same = same && part.kind == parts[0].kind;
            out.hullCount += part.hulls;
        }
        out.kind = anyDecomposed ? PlacementShapeKind::DECOMPOSED : anyAuthored ? PlacementShapeKind::AUTHORED
                 : same ? parts[0].kind : PlacementShapeKind::CONVEX_HULL;

        // caveats: accepted, but the caller should say so
        std::vector<std::string> warnings;
        if (skinned) warnings.push_back("skinned mesh: simulated in its bind pose");
        auto joints = scene->getComponentArray<Joint3DComponent>();
        for (size_t i = 0; i < joints->size(); i++) {
            const Joint3DComponent& joint = joints->getComponentFromIndex(i);
            if (std::find(entities.begin(), entities.end(), joint.bodyA) != entities.end() ||
                std::find(entities.begin(), entities.end(), joint.bodyB) != entities.end()) {
                warnings.push_back("has joints: simulated free (joints are not recreated)");
                break;
            }
        }
        for (size_t i = 0; i < warnings.size(); i++) out.warning += (i ? "; " : "") + warnings[i];
        return true;
    }

    };

}

struct PlacementWorld::Impl {
    Scene* scene = nullptr;
    PlacementWorldSettings settings;
    Vector3 gravity = Vector3(0.0f, -9.81f, 0.0f);
    AABB contextRegion;
    uint32_t stepCount = 0;
    double simulatedSeconds = 0.0;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    float separationBoost = 1.0f;
    bool overflowed = false;   // a step ran out of contact or body-pair buffers (contactsOverflowed)

    struct DynamicBody {
        Entity entity = NULL_ENTITY;
        bool mover = false;
        JPH::BodyID id;
        float size = 0.0f;                   // longest side of the shape's bounds
        float rollingRadius = 0.0f;          // half its shortest side: a ball's radius
        PlacementShapeKind shape = PlacementShapeKind::NONE;
        int hullCount = 0;
        std::vector<uint64_t> decompositionKeys;

        bool hasTarget = false;
        PlacementPose target;
        std::vector<uint32_t> ghosts;        // bodies it passes through while driven (keys)

        bool fellOut = false;
        PlacementPose frozenPose;
        bool mayFallAsleep = true;           // Jolt's AllowSleeping, off while falling (afterStep)
        float secondsUntouched = 0.0f;       // since its last contact
        bool teleported = false;
        bool asleep = false;
        float stillSeconds = 0.0f;

        // from the contacts after the last update
        std::vector<Entity> touching;        // bodies touched, by entity
        bool sunk = false;                   // deep in context that pushes it down (raiseSunkBodies)
        bool onContextOnly = true;           // touches only static context (resistRolling)
        uint32_t contacts = 0;
        float maxPenetration = 0.0f;
        JPH::Vec3 contextNormalSum = JPH::Vec3::sZero();
        bool touchingContext = false;
        std::vector<Entity> restingOn;
    };

    struct ContextBody {
        JPH::BodyID id;
    };

    struct BodyRef {
        Entity entity = NULL_ENTITY;
        DynamicBody* dynamic = nullptr;   // null: context
    };

    // Jolt's world and the filters it reads, which must outlive it
    JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers{Layers::COUNT, BroadPhaseLayers::COUNT};
    JPH::ObjectLayerPairFilterTable layerPairs{Layers::COUNT};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    PlacementContactTracker contactTracker;
    JPH::PhysicsSystem physics;

    std::vector<std::unique_ptr<DynamicBody>> moverBodies;       // insertion order
    std::vector<std::unique_ptr<DynamicBody>> neighbourBodies;   // insertion order
    std::vector<Entity> movers;
    std::vector<Entity> neighbours;
    std::map<Entity, DynamicBody*> dynamicByEntity;
    std::map<Entity, std::vector<ContextBody>> contextByEntity;
    size_t contextBodyCount = 0;
    std::unordered_map<uint32_t, BodyRef> bodies;
    // entities ensureContext has dealt with (context, neighbour parts, or nothing to add)
    std::unordered_set<Entity> processed;
#if JASPERENGINE_TERRAIN_WORLD
    // T4 R23: the fine chunks of each Terrain World in the world (tile key << 32 | chunk x << 16 | chunk z);
    // a World is never processed whole, as each region reaches chunks of its own
    std::map<Entity, std::set<uint64_t>> worldChunks;
#endif

    Impl() {
        broadPhaseLayers.MapObjectToBroadPhaseLayer(Layers::STATIC, BroadPhaseLayers::STATIC);
        broadPhaseLayers.MapObjectToBroadPhaseLayer(Layers::MOVING, BroadPhaseLayers::MOVING);
        layerPairs.EnableCollision(Layers::MOVING, Layers::STATIC);
        layerPairs.EnableCollision(Layers::MOVING, Layers::MOVING);
        objectVsBroadPhase.reset(new JPH::ObjectVsBroadPhaseLayerFilterTable(broadPhaseLayers, BroadPhaseLayers::COUNT, layerPairs, Layers::COUNT));

        physics.Init(MAX_BODIES, 0, MAX_BODY_PAIRS, MAX_CONTACT_CONSTRAINTS, broadPhaseLayers, *objectVsBroadPhase, layerPairs);
        physics.SetContactListener(&contactTracker);
    }

    ~Impl() {
        // bodies first, while the listener and filters they reference are alive
        JPH::BodyInterface& bi = bodyInterface();
        for (auto& entry : bodies) {
            const JPH::BodyID id(entry.first);
            if (bi.IsAdded(id)) bi.RemoveBody(id);
            bi.DestroyBody(id);
        }
        bodies.clear();
        physics.SetContactListener(nullptr);
    }

    JPH::BodyInterface& bodyInterface() { return physics.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bodyInterface() const { return physics.GetBodyInterfaceNoLock(); }

    void applyPhysicsSettings() {
        const PlacementTuning& tuning = settings.tuning;
        JPH::PhysicsSettings s;
        s.mNumVelocitySteps = (JPH::uint)std::max(1, tuning.velocityIterations);
        s.mNumPositionSteps = (JPH::uint)std::max(1, tuning.positionIterations);
        s.mPenetrationSlop = PENETRATION_SLOP;
        s.mBaumgarte = std::min(1.0f, DEFAULT_BAUMGARTE * separationBoost);
        s.mMaxPenetrationDistance = DEFAULT_MAX_PENETRATION_DISTANCE * separationBoost;
        s.mTimeBeforeSleep = std::max(0.0f, tuning.sleepSeconds);
        s.mPointVelocitySleepThreshold = std::max(1e-4f, tuning.sleepLinear);
        physics.SetPhysicsSettings(s);
    }

    std::vector<DynamicBody*> dynamicBodies() const {
        std::vector<DynamicBody*> out;
        for (const auto& body : moverBodies) out.push_back(body.get());
        for (const auto& body : neighbourBodies) out.push_back(body.get());
        return out;
    }

    DynamicBody* findDynamic(Entity entity) const {
        auto found = dynamicByEntity.find(entity);
        return found != dynamicByEntity.end() ? found->second : nullptr;
    }

    bool insideMover(Entity entity) const {
        Entity current = entity;
        for (int depth = 0; current != NULL_ENTITY && depth < 256; depth++) {
            DynamicBody* body = findDynamic(current);
            if (body && body->mover) return true;
            Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        return false;
    }

    bool ancestorOfMover(Entity entity) const {
        for (Entity mover : movers) {
            if (isAncestorOf(scene, entity, mover)) return true;
        }
        return false;
    }

    BodyShapes shapes() const { return BodyShapes{scene, settings}; }

    bool buildDynamicShape(Entity root, DynamicShape& out, std::string* why) const {
        return shapes().buildDynamicShape(root, out, why);
    }

    std::unique_ptr<DynamicBody> createDynamicBody(Entity root, bool mover, DynamicShape& built) {
        Transform* transform = scene->findComponent<Transform>(root);
        if (!transform) return nullptr;
        JPH::BodyCreationSettings creation(built.shape, JPH::RVec3(toJolt(transform->worldPosition)), toJolt(transform->worldRotation),
                                           JPH::EMotionType::Dynamic, Layers::MOVING);
        creation.mMotionQuality = JPH::EMotionQuality::LinearCast;
        creation.mFriction = FRICTION;
        creation.mRestitution = 0.0f;
        creation.mLinearDamping = linearDamping > 0.0f ? linearDamping : DEFAULT_DAMPING;
        creation.mAngularDamping = angularDamping > 0.0f ? angularDamping : DEFAULT_DAMPING;
        creation.mMaxLinearVelocity = speedLimit();
        solverStepsFor(2.0f * std::max(0.0005f, 0.5f * built.shape->GetLocalBounds().GetSize().ReduceMin()),
                       creation.mNumVelocityStepsOverride, creation.mNumPositionStepsOverride);
        creation.mUserData = root;

        JPH::BodyInterface& bi = bodyInterface();
        JPH::Body* jbody = bi.CreateBody(creation);
        if (!jbody) return nullptr;

        std::unique_ptr<DynamicBody> body(new DynamicBody());
        body->entity = root;
        body->mover = mover;
        body->id = jbody->GetID();
        const JPH::AABox bounds = built.shape->GetLocalBounds();
        body->size = bounds.GetSize().ReduceMax();
        body->rollingRadius = std::max(0.001f, 0.5f * bounds.GetSize().ReduceMin());
        body->shape = built.kind;
        body->hullCount = built.hullCount;
        body->decompositionKeys = built.keys;
        bi.AddBody(body->id, JPH::EActivation::Activate);
        bodies[bodyKey(body->id)] = BodyRef{root, body.get()};
        return body;
    }

    // A mover's decompositions are urgent: the user drags it now, or a run waits on it. One a
    // bake queued already moves up ahead of the bake's others.
    void issueRequests(const DynamicShape& built, bool urgent) const {
        ConvexDecompositionCache* cache = settings.decompositionCache;
        if (!cache) return;
        const DecompositionParams params = shapes().decompositionParams();
        for (size_t i = 0; i < built.requests.size(); i++) {
            if (urgent && i < built.keys.size() && cache->status(built.keys[i]) == DecompositionStatus::PENDING) {
                cache->prioritize(built.keys[i]);
            } else {
                cache->request(built.requests[i].positions, built.requests[i].indices, params, urgent);
            }
        }
    }

    void destroyBody(const JPH::BodyID& id) {
        JPH::BodyInterface& bi = bodyInterface();
        if (bi.IsAdded(id)) bi.RemoveBody(id);
        contactTracker.forgetBody(id);
        bi.DestroyBody(id);
        bodies.erase(bodyKey(id));
    }

    void removeContextOf(Entity entity) {
#if JASPERENGINE_TERRAIN_WORLD
        worldChunks.erase(entity);
#endif
        auto found = contextByEntity.find(entity);
        if (found == contextByEntity.end()) return;
        for (const ContextBody& body : found->second) destroyBody(body.id);
        contextBodyCount -= found->second.size();
        contextByEntity.erase(found);
    }

    void removeDynamic(std::vector<std::unique_ptr<DynamicBody>>& list, std::vector<Entity>& order, Entity entity) {
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i]->entity != entity) continue;
            destroyBody(list[i]->id);
            dynamicByEntity.erase(entity);
            list.erase(list.begin() + i);
            order.erase(order.begin() + i);
            break;
        }
        // its entities can be context (or neighbours) again
        for (Entity e : subtreeOf(scene, entity)) processed.erase(e);
    }

    // ---- context ------------------------------------------------------------------------

    JPH::ShapeRefC contextMeshShape(Entity entity, MeshComponent* mesh) const {
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;
        // in the body's frame, mirrored triangles wound back (appendMeshTriangles)
        MeshTriangles::appendMeshTriangles(mesh, shapes().linearOf(entity), vertices, indices);
        weldExact(vertices, indices);
        if (indices.empty()) return nullptr;

        const size_t triangles = indices.size() / 3;
        if (settings.meshSimplifyAbove >= 1.0f && triangles > (size_t)settings.meshSimplifyAbove) {
            const size_t target = (size_t)settings.meshSimplifyAbove * 3;
            static_assert(sizeof(Vector3) == 3 * sizeof(float), "meshopt reads the positions as packed floats");
            std::vector<unsigned int> simplified(indices.size());
            // at most 1 cm off the surface, in the body's (world-scaled) units
            const size_t count = meshopt_simplify(simplified.data(), indices.data(), indices.size(), &vertices[0].x, vertices.size(),
                                                  sizeof(Vector3), target, MESH_SIMPLIFY_ERROR, meshopt_SimplifyErrorAbsolute, nullptr);
            if (count >= 3) {
                simplified.resize(count);
                indices.assign(simplified.begin(), simplified.end());
            }
        }
        return JoltShapeBuilder::buildMeshShape(vertices, indices, nullptr);
    }

    // Instances of an instanced mesh as boxes (spec section 9): each instance's mesh bounds
    // through its own transform and the entity's linear map, a box or (sheared) parallelepiped
    JPH::ShapeRefC instancedShape(Entity entity, MeshComponent* mesh, InstancedMeshComponent& instanced) const {
        const Matrix4 linear = shapes().linearOf(entity);
        const Vector3* corners = mesh->verticesAABB.getCorners();
        JPH::StaticCompoundShapeSettings compound;
        int added = 0;
        for (const InstanceData& instance : instanced.instances) {
            if (!instance.visible) continue;
            const Matrix4 toBody = linear * Matrix4::translateMatrix(instance.position) * instance.rotation.getRotationMatrix() *
                                   Matrix4::scaleMatrix(instance.scale);
            std::vector<Vector3> points;
            for (int c = 0; c < 8; c++) points.push_back(toBody * corners[c]);
            const OrientedBox bounds = axisAlignedBox(points);
            // the hull of the corners sits where they are; too thin for a hull, a box at their centre
            JPH::ShapeRefC box = thinHull(points, 2.0f * bounds.halfExtent.ReduceMin());
            JPH::Vec3 position = JPH::Vec3::sZero();
            if (!box) {
                box = boxShape(JPH::Vec3::sMax(bounds.halfExtent, JPH::Vec3::sReplicate(0.0005f)));
                position = bounds.center;
            }
            if (!box) continue;
            compound.AddShape(position, JPH::Quat::sIdentity(), box);
            added++;
        }
        if (added == 0) return nullptr;
        if (added == 1) {
            const JPH::StaticCompoundShapeSettings::SubShapeSettings& only = compound.mSubShapes[0];
            JPH::RotatedTranslatedShapeSettings single(only.mPosition, only.mRotation, only.mShapePtr);
            JPH::ShapeSettings::ShapeResult result = single.Create();
            return result.IsValid() ? result.Get() : JPH::ShapeRefC();
        }
        JPH::ShapeSettings::ShapeResult result = compound.Create();
        return result.IsValid() ? result.Get() : JPH::ShapeRefC();
    }

    // Static bodies for one entity's own components. False while its geometry is still
    // loading, so a later call tries again.
    bool addStaticContext(Entity entity, std::vector<JPH::BodyID>& added) {
        if (MeshTriangles::isGeometryLoading(scene, entity)) return false;
        resolveTransform(scene, entity);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return true;

        const PlacementColliderPolicy policy = settings.colliders;
        std::vector<JPH::ShapeRefC> contextShapes;

        RigidbodyComponent* body = policy != PlacementColliderPolicy::GENERATED ? authoredBody(scene, entity) : nullptr;
        if (body) {
            std::vector<ShapePart> parts;
            shapes().authoredParts(entity, *body, false, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), parts);
            if (parts.size() == 1) {
                JPH::RotatedTranslatedShapeSettings single(parts[0].position, parts[0].rotation, parts[0].shape);
                JPH::ShapeSettings::ShapeResult result = single.Create();
                if (result.IsValid()) contextShapes.push_back(result.Get());
            } else if (parts.size() > 1) {
                JPH::StaticCompoundShapeSettings compound;
                for (const ShapePart& part : parts) compound.AddShape(part.position, part.rotation, part.shape);
                JPH::ShapeSettings::ShapeResult result = compound.Create();
                if (result.IsValid()) contextShapes.push_back(result.Get());
            }
        }

        const bool generate = policy == PlacementColliderPolicy::GENERATED || policy == PlacementColliderPolicy::BOTH ||
                              (policy == PlacementColliderPolicy::AUTO && !body);
        if (generate) {
            if (TerrainComponent* terrain = terrainOf(scene, entity)) {
                if (!terrain->heightMap.empty() && !terrain->heightMap.isFramebuffer() && !terrain->heightMap.hasData()) {
                    // the heightmap's pixels went with its upload: load them back, as the terrain
                    // picking does (MeshSystem::raycastTerrainSurface), which also reads a
                    // heightmap that won't load as flat; a load in flight is waited for
                    terrain->heightMap.setReleaseDataAfterLoad(false);
                    if (terrain->heightMap.load().state == ResourceLoadState::Loading) return false;
                }
                JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(scene, *terrain, transform->worldScale, 0, nullptr);
                if (shape) contextShapes.push_back(shape);
            } else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity); mesh && mesh->enabled && instancedMesh(scene, entity)) {
                JPH::ShapeRefC shape = instancedShape(entity, mesh, *instancedMesh(scene, entity));
                if (shape) contextShapes.push_back(shape);
            } else if (MeshComponent* triangles = triangleMesh(scene, entity)) {
                JPH::ShapeRefC shape = contextMeshShape(entity, triangles);
                if (shape) contextShapes.push_back(shape);
            }
        }

        // no room for all of them: none now, so a later call (with room) adds them
        if (physics.GetNumBodies() + contextShapes.size() > MAX_BODIES) return false;
        JPH::BodyInterface& bi = bodyInterface();
        for (const JPH::ShapeRefC& shape : contextShapes) {
            JPH::BodyCreationSettings creation(shape, JPH::RVec3(toJolt(transform->worldPosition)), toJolt(transform->worldRotation),
                                               JPH::EMotionType::Static, Layers::STATIC);
            creation.mFriction = FRICTION;
            creation.mUserData = entity;
            JPH::Body* jbody = bi.CreateBody(creation);
            if (!jbody) break;   // body limit
            contextByEntity[entity].push_back(ContextBody{jbody->GetID()});
            contextBodyCount++;
            bodies[bodyKey(jbody->GetID())] = BodyRef{entity, nullptr};
            added.push_back(jbody->GetID());
        }
        return true;
    }

    void finishAdding(std::vector<JPH::BodyID>& added) {
        if (added.empty()) return;
        JPH::BodyInterface& bi = bodyInterface();
        JPH::BodyInterface::AddState state = bi.AddBodiesPrepare(added.data(), (int)added.size());
        bi.AddBodiesFinalize(added.data(), (int)added.size(), state, JPH::EActivation::DontActivate);
        added.clear();
    }

    // Something a shape can be built from, and not an editor overlay (selection outlines, the
    // terrain brush, the Physics Placement lines drawn into the edited scene)
    bool contributes(Entity entity) const {
        if (editorOverlay(scene, entity)) return false;
        return terrainOf(scene, entity) || authoredBody(scene, entity) || triangleMesh(scene, entity) ||
               (scene->findComponent<MeshComponent>(entity) && instancedMesh(scene, entity));
    }

    // Movable (spec section 3.4): a Rigidbody that is not static, or no larger than 2 m with no
    // static Rigidbody, terrain or instanced mesh in it. Never a mover or a mover's ancestor.
    bool movableObject(Entity entity, std::map<Entity, bool>& cache) const {
        auto found = cache.find(entity);
        if (found != cache.end()) return found->second;
        bool movable = false;
        if (!insideMover(entity) && !ancestorOfMover(entity) && scene->findComponent<Transform>(entity)) {
            RigidbodyComponent* rigidbody = scene->findComponent<RigidbodyComponent>(entity);
            if (rigidbody && rigidbody->enabled && rigidbody->type != BodyType::STATIC) {
                movable = true;
            } else if (!(rigidbody && rigidbody->enabled)) {
                AABB bounds;
                bool geometry = false, blocked = false;
                for (Entity e : subtreeOf(scene, entity)) {
                    if (!scene->isEntityActiveInHierarchy(e) || layerIgnored(scene, e, settings.ignoreLayerMask)) continue;
                    RigidbodyComponent* inner = scene->findComponent<RigidbodyComponent>(e);
                    if (terrainOf(scene, e) || instancedMesh(scene, e) || (inner && inner->enabled && inner->type == BodyType::STATIC)) {
                        blocked = true;
                        break;
                    }
                    if (contributes(e)) {
                        geometry = true;
                        bounds.merge(entityWorldBounds(scene, e));
                    }
                }
                movable = !blocked && geometry && longestAxis(bounds) <= NEIGHBOUR_MAX_SIZE;
            }
        }
        cache[entity] = movable;
        return movable;
    }

    // The object a movable entity belongs to: the highest movable ancestor reached without
    // crossing an unmovable one
    Entity neighbourRoot(Entity entity, std::map<Entity, bool>& cache) const {
        Entity root = entity;
        for (int depth = 0; depth < 256; depth++) {
            Transform* transform = scene->findComponent<Transform>(root);
            if (!transform || transform->parent == NULL_ENTITY || !movableObject(transform->parent, cache)) break;
            root = transform->parent;
        }
        return root;
    }

    // The world bounds of a built shape placed at the root's scene pose
    float worldSize(Entity root, const DynamicShape& built) const {
        Transform* transform = scene->findComponent<Transform>(root);
        if (!transform) return std::numeric_limits<float>::infinity();
        const JPH::AABox bounds = built.shape->GetWorldSpaceBounds(
            JPH::Mat44::sRotationTranslation(toJolt(transform->worldRotation), toJolt(transform->worldPosition)) *
            JPH::Mat44::sTranslation(built.shape->GetCenterOfMass()), JPH::Vec3::sReplicate(1.0f));
        return bounds.GetSize().ReduceMax();
    }

    bool addNeighbour(Entity root) {
        DynamicShape built;
        if (!buildDynamicShape(root, built, nullptr)) return false;
        // a large dynamic Rigidbody (a platform, a vehicle) stays environment, as it would as a mover
        if (worldSize(root, built) > settings.maxMoverSize) return false;
        // neighbours inside it (a cup on a table that only now came free) become part of it
        std::vector<Entity> inside;
        for (Entity neighbour : neighbours) {
            if (isAncestorOf(scene, root, neighbour)) inside.push_back(neighbour);
        }
        for (Entity neighbour : inside) removeDynamic(neighbourBodies, neighbours, neighbour);
        std::unique_ptr<DynamicBody> body = createDynamicBody(root, false, built);
        if (!body) return false;
        issueRequests(built, false);
        for (Entity e : subtreeOf(scene, root)) {
            removeContextOf(e);
            processed.insert(e);
        }
        dynamicByEntity[root] = body.get();
        neighbours.push_back(root);
        neighbourBodies.push_back(std::move(body));
        return true;
    }

#if JASPERENGINE_TERRAIN_WORLD
    // T4 R23: a Terrain World's fine chunks that region reaches, from its tiles read now (sampled as
    // its own collider samples them), one static body each, so placed objects rest on the drawn surface
    void addWorldChunks(Entity entity, TerrainComponent& terrainComponent, const AABB& region, std::vector<JPH::BodyID>& added) {
        TerrainWorldRuntime* runtime = terrainComponent.world;
        Transform* transform = scene->findComponent<Transform>(entity);
        terraincore::TileCoord low, high;
        if (!runtime || !runtime->started() || !transform || !runtime->occupancy().bounds(low, high)) return;
        resolveTransform(scene, entity);
        const terraincore::WorldLattice& lattice = runtime->lattice();
        const TerrainWorldComponent* component = scene->findComponent<TerrainWorldComponent>(entity);
        const int wanted = component ? component->collisionChunkQuads : 64;
        const int quads = std::clamp(terraincore::isPowerOfTwo(wanted) ? wanted : 64, 32, lattice.tileQuads);
        const int perTile = lattice.tileQuads / quads;
        const float chunkSize = static_cast<float>(quads) * lattice.spacing;
        const float tileSize = lattice.tileSize();
        const Vector3 origin = transform->worldPosition;
        // the region over the World's tiles, in chunks
        const float minX = std::max(region.getMinimum().x - origin.x, static_cast<float>(low.x) * tileSize);
        const float maxX = std::min(region.getMaximum().x - origin.x, static_cast<float>(high.x + 1) * tileSize);
        const float minZ = std::max(region.getMinimum().z - origin.z, static_cast<float>(low.z) * tileSize);
        const float maxZ = std::min(region.getMaximum().z - origin.z, static_cast<float>(high.z + 1) * tileSize);
        if (minX > maxX || minZ > maxZ) return;
        std::set<uint64_t>& made = worldChunks[entity];
        JPH::BodyInterface& bi = bodyInterface();
        for (int gz = static_cast<int>(std::floor(minZ / chunkSize)); gz <= static_cast<int>(std::floor(maxZ / chunkSize)); gz++) {
            for (int gx = static_cast<int>(std::floor(minX / chunkSize)); gx <= static_cast<int>(std::floor(maxX / chunkSize)); gx++) {
                const terraincore::TileCoord tile(terraincore::floorDiv(gx, perTile), terraincore::floorDiv(gz, perTile));
                const int cx = gx - tile.x * perTile, cz = gz - tile.z * perTile;
                if (!tile.addressable() || !runtime->occupancy().present(tile)) continue;
                const uint64_t key = (static_cast<uint64_t>(tile.key()) << 32) | (static_cast<uint64_t>(static_cast<uint16_t>(cx)) << 16) | static_cast<uint16_t>(cz);
                if (made.count(key)) continue;
                if (physics.GetNumBodies() >= MAX_BODIES) return;
                std::vector<float> samples;
                if (!TerrainWorldCollision::chunkSamplesNow(*runtime, tile, quads, cx, cz, samples)) continue;
                JPH::HeightFieldShapeSettings field(samples.data(), JPH::Vec3::sZero(), JPH::Vec3(lattice.spacing, 1.0f, lattice.spacing),
                                                    static_cast<JPH::uint32>(quads + 1));
                field.mBitsPerSample = std::min<JPH::uint32>(8u, std::max<JPH::uint32>(1u, field.CalculateBitsPerSampleForError(TerrainWorldCollision::CHUNK_MAX_ERROR)));
                JPH::ShapeSettings::ShapeResult result = field.Create();
                if (!result.IsValid()) continue;
                const Vector3 at(origin.x + static_cast<float>(gx) * chunkSize, origin.y, origin.z + static_cast<float>(gz) * chunkSize);
                JPH::BodyCreationSettings creation(result.Get(), JPH::RVec3(toJolt(at)), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::STATIC);
                creation.mFriction = FRICTION;
                creation.mUserData = entity;
                JPH::Body* body = bi.CreateBody(creation);
                if (!body) return;
                made.insert(key);
                contextByEntity[entity].push_back(ContextBody{body->GetID()});
                contextBodyCount++;
                bodies[bodyKey(body->GetID())] = BodyRef{entity, nullptr};
                added.push_back(body->GetID());
            }
        }
    }
#endif

    // Adds the context in region (or only's subtree, static); returns the neighbours it made
    std::vector<Entity> gatherContext(const AABB* region, Entity only) {
        std::vector<Entity> newNeighbours;
        std::vector<JPH::BodyID> added;
        std::map<Entity, bool> movableCache;
        std::set<Entity> foliage;
        auto terrains = scene->getComponentArray<TerrainComponent>();
        MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
        for (size_t i = 0; i < terrains->size(); i++) {
            for (Entity e : meshSystem->getFoliageEntities(terrains->getEntity(i))) foliage.insert(e);
        }
        // a spline's chunks collide through their owner's colliders (SplineSystem), not as props
        if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()) {
            auto splineArray = scene->getComponentArray<SplineComponent>();
            for (size_t i = 0; i < splineArray->size(); i++) {
                for (Entity e : splines->getGeneratedEntities(splineArray->getEntity(i))) foliage.insert(e);
            }
        }

        std::vector<Entity> candidates;
        if (only != NULL_ENTITY) {
            candidates = subtreeOf(scene, only);
        } else {
            auto transforms = scene->getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) candidates.push_back(transforms->getEntity(i));
        }

        for (Entity entity : candidates) {
#if JASPERENGINE_TERRAIN_WORLD
            if (TerrainComponent* terrain = terrainOf(scene, entity); terrain && terrain->world) {
                if (!scene->isEntityActiveInHierarchy(entity) || layerIgnored(scene, entity, settings.ignoreLayerMask)) continue;
                // a whole World is too much: a region, else what the context reaches so far
                const AABB* reach = region ? region : (contextRegion.isNull() ? nullptr : &contextRegion);
                if (reach) addWorldChunks(entity, *terrain, *reach, added);
                continue;
            }
#endif
            // cheapest first: most entities give no shape
            if (processed.count(entity) || !contributes(entity) || foliage.count(entity)) continue;
            if (!scene->isEntityActiveInHierarchy(entity) || layerIgnored(scene, entity, settings.ignoreLayerMask)) continue;
            if (insideMover(entity) || findDynamic(entity)) continue;
            if (region && !region->intersects(entityWorldBounds(scene, entity))) continue;

            if (region && settings.pushNeighbours && movableObject(entity, movableCache)) {
                const Entity root = neighbourRoot(entity, movableCache);
                if (!findDynamic(root) && addNeighbour(root)) {
                    newNeighbours.push_back(root);
                    continue;
                }
            }

            if (addStaticContext(entity, added)) processed.insert(entity);
        }
        finishAdding(added);
        return newNeighbours;
    }

    // A neighbour falls unless what it stands on is in the world too, and the region it was
    // found in need not reach that far down: the context around every new neighbour (its
    // bounds grown by half its size, at least 0.5 m) joins, which can find neighbours of its
    // own, until no new one appears
    void supportNeighbours(std::vector<Entity> pending) {
        const JPH::BodyLockInterfaceNoLock& locks = physics.GetBodyLockInterfaceNoLock();
        for (int round = 0; round < MAX_SUPPORT_ROUNDS && !pending.empty(); round++) {
            std::vector<Entity> found;
            for (Entity neighbour : pending) {
                DynamicBody* body = findDynamic(neighbour);
                if (!body) continue;
                AABB support;
                {
                    JPH::BodyLockRead lock(locks, body->id);
                    if (!lock.Succeeded()) continue;
                    const JPH::AABox box = lock.GetBody().GetWorldSpaceBounds();
                    const float margin = std::max(NEIGHBOUR_SUPPORT_MARGIN, 0.5f * box.GetSize().ReduceMax());
                    support = AABB(fromJolt(box.mMin) - Vector3(margin, margin, margin), fromJolt(box.mMax) + Vector3(margin, margin, margin));
                }
                contextRegion.merge(support);
                for (Entity e : gatherContext(&support, NULL_ENTITY)) found.push_back(e);
            }
            pending.swap(found);
        }
    }

    // ---- stepping -----------------------------------------------------------------------

    JPH::Vec3 upDirection() const {
        const JPH::Vec3 g = toJolt(gravity);
        return g.LengthSq() > 1e-12f ? -g.Normalized() : JPH::Vec3::sAxisY();
    }

    void settleContacts() {
        const JPH::BodyInterface& bi = bodyInterface();
        contactTracker.settle([&](uint32_t key) { return bi.IsActive(JPH::BodyID(key)); });
    }

    void summariseContacts() {
        for (DynamicBody* body : dynamicBodies()) {
            body->contacts = 0;
            body->maxPenetration = 0.0f;
            body->contextNormalSum = JPH::Vec3::sZero();
            body->touchingContext = false;
            body->restingOn.clear();
            body->touching.clear();
            body->sunk = false;
            body->onContextOnly = true;
        }
        const JPH::Vec3 up = upDirection();
        contactTracker.forEach([&](const PlacementContactTracker::Contact& contact) {
            auto first = bodies.find(contact.body1);
            auto second = bodies.find(contact.body2);
            if (first == bodies.end() || second == bodies.end()) return;
            const JPH::Vec3 normal(contact.normal);
            auto account = [&](const BodyRef& self, const BodyRef& other, JPH::Vec3Arg intoSelf) {
                DynamicBody* body = self.dynamic;
                if (!body || body->fellOut) return;
                if (!other.dynamic || !other.dynamic->mover) {
                    body->contextNormalSum += intoSelf;
                    body->touchingContext = true;
                }
                if (!other.dynamic && contact.depth > GHOST_DEPTH && intoSelf.Dot(up) < -0.5f) { body->sunk = true; }
                if (contact.depth < -TOUCH_DISTANCE) return;
                body->touching.push_back(other.entity);
                if (other.dynamic) body->onContextOnly = false;
                body->maxPenetration = std::max(body->maxPenetration, contact.depth);
                if (intoSelf.Dot(up) > RESTING_COS) body->restingOn.push_back(other.entity);
            };
            // the normal points from body 1 to body 2: into body 2, out of body 1
            account(second->second, first->second, normal);
            account(first->second, second->second, -normal);
        });
        for (DynamicBody* body : dynamicBodies()) {
            for (std::vector<Entity>* list : {&body->restingOn, &body->touching}) {
                std::sort(list->begin(), list->end());
                list->erase(std::unique(list->begin(), list->end()), list->end());
            }
            body->contacts = (uint32_t)body->touching.size();
        }
    }

    // The speed cap at a contact of normal n (unit, into the body): maxSpeed along or away from
    // it, down to contactSpeed straight into it. Smaller bodies stop sooner: the full contact
    // speed from 1 m up, a tenth at 10 cm.
    float contactCap(const DynamicBody& body, JPH::Vec3Arg v, JPH::Vec3Arg n) const {
        const PlacementTuning& tuning = settings.tuning;
        const float speed = v.Length();
        if (speed <= 0.0f) return tuning.maxSpeed;
        const float into = std::max(0.0f, -v.Dot(n) / speed);
        const float sizeFactor = std::min(1.0f, std::max(0.1f, body.size));
        return tuning.maxSpeed + (tuning.contactSpeed * sizeFactor - tuning.maxSpeed) * into;
    }

    // Context (and neighbours) for a mover's sweep: not itself, not the other movers, which move
    // with it
    class SweepFilter final : public JPH::BodyFilter {
    public:
        SweepFilter(const Impl& world, const DynamicBody& self): world(world), self(self) {}
        bool ShouldCollide(const JPH::BodyID& id) const override {
            if (id == self.id) return false;
            if (std::find(self.ghosts.begin(), self.ghosts.end(), bodyKey(id)) != self.ghosts.end()) return false;
            auto found = world.bodies.find(bodyKey(id));
            return found == world.bodies.end() || !found->second.dynamic || !found->second.dynamic->mover;
        }
    private:
        const Impl& world;
        const DynamicBody& self;
    };

    // Context only
    class StaticOnlyFilter final : public JPH::BodyFilter {
    public:
        explicit StaticOnlyFilter(const Impl& world): world(world) {}
        bool ShouldCollide(const JPH::BodyID& id) const override {
            auto found = world.bodies.find(bodyKey(id));
            return found != world.bodies.end() && !found->second.dynamic;
        }
    private:
        const Impl& world;
    };

    // The bodies a dynamic body's shape is deeper than minDepth in, by key
    std::vector<uint32_t> overlapping(const DynamicBody& body, float minDepth) const {
        std::vector<uint32_t> out;
        JPH::ShapeRefC shape;
        JPH::RMat44 transform;
        {
            JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), body.id);
            if (!lock.Succeeded()) return out;
            shape = lock.GetBody().GetShape();
            transform = lock.GetBody().GetCenterOfMassTransform();
        }
        JPH::CollideShapeSettings collideSettings;
        collideSettings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
        const JPH::IgnoreSingleBodyFilter self(body.id);
        physics.GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sReplicate(1.0f), transform, collideSettings,
                                                         JPH::RVec3::sZero(), hits, {}, {}, self);
        for (const JPH::CollideShapeResult& hit : hits.mHits) {
            if (hit.mPenetrationDepth > minDepth) out.push_back(bodyKey(hit.mBodyID2));
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // A driven mover passes through what it starts deep inside (or was teleported into): pulled
    // out by its target it would otherwise ram the other body, or be held in it, every step,
    // and launch it (or itself, when released) out of the world
    void beginGhosts(DynamicBody& body) {
        endGhosts(body);
        body.ghosts = overlapping(body, GHOST_DEPTH);
        for (uint32_t other : body.ghosts) contactTracker.ghosts.insert(PlacementContactTracker::pairOf(bodyKey(body.id), other));
    }

    void endGhosts(DynamicBody& body) {
        for (uint32_t other : body.ghosts) contactTracker.ghosts.erase(PlacementContactTracker::pairOf(bodyKey(body.id), other));
        body.ghosts.clear();
    }

    // A free body sunk deep in static context (half into a floor slab, say) is pushed out the
    // way the solver sees, which can be down through the slab, and falls out of the world: one
    // whose deep contacts with context push it down (summariseContacts) is lifted out against
    // gravity instead, onto the first surface above it
    void raiseSunkBodies() {
        const JPH::Vec3 up = upDirection();
        JPH::BodyInterface& bi = bodyInterface();
        for (DynamicBody* body : dynamicBodies()) {
            if (body->fellOut || body->hasTarget || !body->sunk) continue;

            JPH::ShapeRefC shape;
            JPH::RMat44 transform;
            JPH::AABox bounds;
            {
                JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), body->id);
                if (!lock.Succeeded()) continue;
                shape = lock.GetBody().GetShape();
                transform = lock.GetBody().GetCenterOfMassTransform();
                bounds = lock.GetBody().GetWorldSpaceBounds();
            }
            const StaticOnlyFilter staticOnly(*this);

            // down onto the first surface met, from a body height above (higher while that start
            // is inside something too)
            float rise = std::fabs(JPH::Vec3(bounds.GetSize()).Dot(up.Abs())) + 2.0f * body->maxPenetration;
            float lift = -1.0f;
            for (int attempt = 0; attempt < 4 && lift < 0.0f; attempt++, rise *= 2.0f) {
                const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(up * rise)) * transform, -up * rise);
                JPH::ShapeCastSettings castSettings;
                castSettings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
                castSettings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
                JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> landing;
                physics.GetNarrowPhaseQueryNoLock().CastShape(cast, castSettings, JPH::RVec3::sZero(), landing, {}, {}, staticOnly);
                if (!landing.HadHit()) break;   // nothing to land on: left to the solver
                if (landing.mHit.mFraction > 1e-4f) lift = rise * (1.0f - landing.mHit.mFraction) + PENETRATION_SLOP;
            }
            if (lift < 0.0f) continue;
            bi.SetPosition(body->id, bi.GetPosition(body->id) + JPH::RVec3(up * lift), JPH::EActivation::Activate);
            bi.SetLinearAndAngularVelocity(body->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            forgetContacts(*body);
            body->maxPenetration = 0.0f;
            body->sunk = false;
            body->stillSeconds = 0.0f;
            body->asleep = false;
        }
    }

    // The body moved or changed shape outside a step: its contacts start over, and so do its ghosts
    void forgetContacts(DynamicBody& body) {
        contactTracker.forgetBody(body.id);
        body.ghosts.clear();
        if (body.hasTarget && !body.fellOut) beginGhosts(body);
    }

    // Once apart they collide again
    void releaseGhosts() {
        for (const auto& owned : moverBodies) {
            DynamicBody& body = *owned;
            if (body.ghosts.empty()) continue;
            const std::vector<uint32_t> still = overlapping(body, 0.0f);
            std::vector<uint32_t> kept;
            for (uint32_t other : body.ghosts) {
                if (std::binary_search(still.begin(), still.end(), other) && bodies.count(other)) kept.push_back(other);
                else contactTracker.ghosts.erase(PlacementContactTracker::pairOf(bodyKey(body.id), other));
            }
            body.ghosts.swap(kept);
        }
    }

    // A contact the step would only reach part way through: the gap at full speed, the rest
    // at the contact cap, so the body arrives at contact speed instead of ramming in and being
    // pushed back out. Contacts already touching are the contact cap's.
    JPH::Vec3 sweepLimited(const DynamicBody& body, JPH::Vec3Arg v, float dt) const {
        const float speed = v.Length();
        const float distance = speed * dt;
        if (distance < 1e-5f) return v;

        JPH::ShapeRefC shape;
        JPH::RMat44 start;
        {
            JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), body.id);
            if (!lock.Succeeded()) return v;
            shape = lock.GetBody().GetShape();
            start = lock.GetBody().GetCenterOfMassTransform();
        }
        const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), start, v * dt);
        JPH::ShapeCastSettings castSettings;
        castSettings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
        castSettings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hits;
        const SweepFilter filter(*this, body);
        physics.GetNarrowPhaseQueryNoLock().CastShape(cast, castSettings, JPH::RVec3::sZero(), hits, {}, {}, filter);

        float first = 1.0f;
        JPH::Vec3 normal = JPH::Vec3::sZero();
        for (const JPH::ShapeCastResult& hit : hits.mHits) {
            if (hit.mFraction <= 1e-4f || hit.mFraction >= first) continue;
            first = hit.mFraction;
            // the axis pushes the other shape away from this one
            normal = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());
        }
        if (first >= 1.0f || normal.IsNearZero()) return v;
        const float cap = contactCap(body, v, normal);
        if (speed <= cap) return v;
        const float travel = first * distance + (1.0f - first) * cap * dt;
        return v * (travel / distance);
    }

    // Spec section 6: velocity targets toward the pose, capped, slowed only into contacts,
    // teleported past teleportDistance. Run before every substep.
    void driveTargets(float dt) {
        const PlacementTuning& tuning = settings.tuning;
        JPH::BodyInterface& bi = bodyInterface();
        for (const auto& owned : moverBodies) {
            DynamicBody* body = owned.get();
            if (!body->hasTarget || body->fellOut) continue;

            const JPH::RVec3 position = bi.GetPosition(body->id);
            const JPH::Quat rotation = bi.GetRotation(body->id);
            const JPH::Vec3 targetPosition = toJolt(body->target.position);
            const JPH::Quat targetRotation = toJolt(body->target.rotation);
            const JPH::Vec3 error = JPH::Vec3(targetPosition - position);

            if (error.Length() > tuning.teleportDistance) {
                bi.SetPositionAndRotation(body->id, JPH::RVec3(targetPosition), targetRotation, JPH::EActivation::Activate);
                bi.SetLinearAndAngularVelocity(body->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                forgetContacts(*body);
                body->teleported = true;
                body->stillSeconds = 0.0f;
                body->asleep = false;
                continue;
            }

            // the centre of mass toward where it sits at the target pose, so a turn and a move
            // together still land the root on its target
            const JPH::Vec3 localCenter = bi.GetShape(body->id)->GetCenterOfMass();
            const JPH::Vec3 centerTarget = targetPosition + targetRotation * localCenter;
            JPH::Vec3 v = JPH::Vec3(centerTarget - bi.GetCenterOfMassPosition(body->id)) / dt;

            // w = 2 log(q* conj(q)) / dt, the shorter way round
            JPH::Quat delta = (targetRotation * rotation.Conjugated()).Normalized();
            if (delta.GetW() < 0.0f) delta = -delta;
            JPH::Vec3 axis;
            float angle = 0.0f;
            delta.GetAxisAngle(axis, angle);
            JPH::Vec3 w = angle > 1e-7f ? axis * (angle / dt) : JPH::Vec3::sZero();

            float speed = v.Length();
            if (speed > tuning.maxSpeed) {
                v *= tuning.maxSpeed / speed;
                speed = tuning.maxSpeed;
            }
            if (body->touchingContext && speed > 0.0f && !body->contextNormalSum.IsNearZero()) {
                const float cap = contactCap(*body, v, body->contextNormalSum.Normalized());
                if (speed > cap) v *= cap / speed;
            }
            v = sweepLimited(*body, v, dt);
            const float angularSpeed = w.Length();
            if (angularSpeed > tuning.maxAngularSpeed) w *= tuning.maxAngularSpeed / angularSpeed;

            if (!bi.IsActive(body->id) && v.IsNearZero(1e-12f) && w.IsNearZero(1e-12f)) continue;
            if (!bi.IsActive(body->id)) bi.ActivateBody(body->id);
            bi.SetLinearAndAngularVelocity(body->id, v, w);
        }
    }

    // Rolling resistance: a free body on the ground under gravity (touching static context only:
    // in a stack the solver balances each box against the others, and taking spin out of them
    // there tips the stack) feels a torque against its spin of ROLLING_RESISTANCE x its weight x
    // its rolling radius (half its shortest side),
    // so a pebble comes to rest on a gentle slope (a ball stops below atan(0.2), about 11
    // degrees) instead of creeping on forever, a ball still rolls down a steeper ramp and on
    // across the floor, and a long body (a pencil) still topples: about its long axes its
    // inertia dwarfs that torque
    void resistRolling(float dt) {
        const float g = gravity.length();
        if (g <= 0.0f) return;
        // a body barely turning is left to the solver and to sleep
        const float turning = settings.tuning.sleepAngularDeg * JPH::JPH_PI / 180.0f;
        const JPH::BodyLockInterfaceNoLock& locks = physics.GetBodyLockInterfaceNoLock();
        for (DynamicBody* body : dynamicBodies()) {
            if (body->fellOut || body->hasTarget || body->contacts == 0 || !body->onContextOnly) continue;
            JPH::BodyLockWrite lock(locks, body->id);
            if (!lock.Succeeded() || !lock.GetBody().IsActive()) continue;
            JPH::Body& jbody = lock.GetBody();
            const JPH::Vec3 w = jbody.GetAngularVelocity();
            const float speed = w.Length();
            const float inverseMass = jbody.GetMotionProperties()->GetInverseMass();
            if (speed <= turning || inverseMass <= 0.0f) continue;
            const float torque = ROLLING_RESISTANCE * g * body->rollingRadius / inverseMass;
            // about the spin axis only (the inverse inertia along it), so the spin slows without
            // turning: a full inverse-inertia kick would tip a box in a stack sideways
            const JPH::Vec3 axis = w / speed;
            const float slowing = torque * dt * axis.Dot(jbody.GetInverseInertia().Multiply3x3(axis));
            jbody.SetAngularVelocityClamped(w * (std::max(turning, speed - slowing) / speed));
        }
    }

    void afterStep() {
        const PlacementTuning& tuning = settings.tuning;
        const float sleepAngular = tuning.sleepAngularDeg * JPH::JPH_PI / 180.0f;
        JPH::BodyInterface& bi = bodyInterface();
        for (DynamicBody* body : dynamicBodies()) {
            if (body->fellOut) continue;

            const JPH::RVec3 position = bi.GetPosition(body->id);
            if (position.GetY() < settings.killY) {
                body->frozenPose.position = fromJolt(JPH::Vec3(position));
                body->frozenPose.rotation = fromJolt(bi.GetRotation(body->id));
                body->fellOut = true;
                body->asleep = false;
                bi.RemoveBody(body->id);
                forgetContacts(*body);
                continue;
            }

            // Under gravity a free body that has touched nothing for a while is falling, however
            // slowly (weak gravity keeps it under the sleep thresholds for a time): neither rule
            // may put it to sleep. Half the sleep time, so a resting body's micro-bounces off its
            // support (a few steps without a contact) don't count, while a still body in the air
            // is caught before it could have slept. Without gravity (Separate) a body may come to
            // rest in the air.
            body->secondsUntouched = body->contacts > 0 ? 0.0f : body->secondsUntouched + settings.fixedStep;
            const bool falling = !body->hasTarget && gravity.squaredLength() > 1e-12f &&
                                 body->secondsUntouched > 0.5f * tuning.sleepSeconds;
           
            if (falling == body->mayFallAsleep) {
                body->mayFallAsleep = !falling;
                JPH::BodyLockWrite lock(physics.GetBodyLockInterfaceNoLock(), body->id);
                if (lock.Succeeded()) lock.GetBody().SetAllowSleeping(!falling);
            }
            if (falling) {
                if (!bi.IsActive(body->id)) bi.ActivateBody(body->id);
                body->stillSeconds = 0.0f;
                body->asleep = false;
                continue;
            }

            if (!bi.IsActive(body->id)) {
                body->asleep = true;
                continue;
            }
            const float linear = bi.GetLinearVelocity(body->id).Length();
            const float angular = bi.GetAngularVelocity(body->id).Length();
            if (linear < tuning.sleepLinear && angular < sleepAngular) body->stillSeconds += settings.fixedStep;
            else body->stillSeconds = 0.0f;
            body->asleep = body->stillSeconds >= tuning.sleepSeconds;
            // at rest by our thresholds: let Jolt stop integrating it too (a target keeps driving it)
            if (body->asleep && !body->hasTarget) bi.DeactivateBody(body->id);
        }
    }

    void setGravityFactor(DynamicBody* body) {
        bodyInterface().SetGravityFactor(body->id, body->hasTarget ? 0.0f : 1.0f);
    }

    // A thin body (a plate, a pencil) gets twice the solver iterations, at least 16 and 4, in
    // whatever island it is in: with the presets' few, a stack of six 12 mm plates sank 6 mm into
    // itself, half a plate. 0 = the world's.
    void solverStepsFor(float thinnestSide, JPH::uint& velocitySteps, JPH::uint& positionSteps) const {
        if (thinnestSide >= THIN_BODY) {
            velocitySteps = 0;
            positionSteps = 0;
            return;
        }
        const PlacementTuning& tuning = settings.tuning;
        velocitySteps = (JPH::uint)std::min(255, std::max(16, 2 * tuning.velocityIterations));
        positionSteps = (JPH::uint)std::min(255, std::max(4, 2 * tuning.positionIterations));
    }

    // No dynamic body outruns maxSpeed: not a driven mover, a pushed neighbour, a body pushed out
    // of an overlap, nor a fall
    float speedLimit() const { return std::max(0.01f, settings.tuning.maxSpeed); }

    void applySpeedLimit() {
        const JPH::BodyLockInterfaceNoLock& locks = physics.GetBodyLockInterfaceNoLock();
        for (DynamicBody* body : dynamicBodies()) {
            JPH::BodyLockWrite lock(locks, body->id);
            if (!lock.Succeeded()) continue;
            JPH::Body& jbody = lock.GetBody();
            jbody.GetMotionProperties()->SetMaxLinearVelocity(speedLimit());
            JPH::uint velocitySteps = 0, positionSteps = 0;
            solverStepsFor(2.0f * body->rollingRadius, velocitySteps, positionSteps);
            jbody.GetMotionProperties()->SetNumVelocityStepsOverride(velocitySteps);
            jbody.GetMotionProperties()->SetNumPositionStepsOverride(positionSteps);
            jbody.SetLinearVelocityClamped(jbody.GetLinearVelocity());
        }
    }

    void applyDamping() {
        const JPH::BodyLockInterfaceNoLock& locks = physics.GetBodyLockInterfaceNoLock();
        for (DynamicBody* body : dynamicBodies()) {
            JPH::BodyLockWrite lock(locks, body->id);
            if (!lock.Succeeded()) continue;
            JPH::MotionProperties* motion = lock.GetBody().GetMotionProperties();
            motion->SetLinearDamping(linearDamping > 0.0f ? linearDamping : DEFAULT_DAMPING);
            motion->SetAngularDamping(angularDamping > 0.0f ? angularDamping : DEFAULT_DAMPING);
        }
    }

    bool pendingDecomposition(const DynamicBody& body) const {
        if (!settings.decompositionCache) return false;
        for (uint64_t key : body.decompositionKeys) {
            if (settings.decompositionCache->status(key) == DecompositionStatus::PENDING) return true;
        }
        return false;
    }

    PlacementBodyState stateOf(const DynamicBody& body) const {
        const JPH::BodyInterface& bi = bodyInterface();
        PlacementBodyState state;
        state.entity = body.entity;
        state.mover = body.mover;
        if (body.fellOut) {
            state.pose = body.frozenPose;
        } else {
            state.pose.position = fromJolt(JPH::Vec3(bi.GetPosition(body.id)));
            state.pose.rotation = fromJolt(bi.GetRotation(body.id));
            state.linearVelocity = fromJolt(bi.GetLinearVelocity(body.id));
            state.angularVelocity = fromJolt(bi.GetAngularVelocity(body.id));
        }
        state.asleep = !body.fellOut && (body.asleep || !bi.IsActive(body.id));
        state.fellOut = body.fellOut;
        state.teleported = body.teleported;
        state.touchingContext = body.touchingContext;
        state.averageContactNormal = body.contextNormalSum.IsNearZero() ? Vector3(0, 0, 0) : fromJolt(body.contextNormalSum.Normalized());
        state.restingOn = body.restingOn;
        state.contacts = body.contacts;
        state.maxPenetration = body.maxPenetration;
        state.shape = body.shape;
        state.hullCount = body.hullCount;
        state.decompositionPending = pendingDecomposition(body);
        return state;
    }
};

#else

struct PlacementWorld::Impl {
    Scene* scene = nullptr;
    PlacementWorldSettings settings;
    Vector3 gravity = Vector3(0.0f, -9.81f, 0.0f);
    AABB contextRegion;
    uint32_t stepCount = 0;
    double simulatedSeconds = 0.0;
    std::vector<Entity> movers;
    std::vector<Entity> neighbours;
};

#endif

// Jolt's allocator and factory come first: Impl's members (the layer tables, the PhysicsSystem)
// allocate as they are constructed, and a world may be made before any Scene, whose
// PhysicsSystem would otherwise have set Jolt up
PlacementWorld::PlacementWorld(Scene* scene, const PlacementWorldSettings& settings): impl([]() {
#ifdef JASPERENGINE_PHYSICS_3D
    JoltShapeBuilder::ensureJoltGlobalInit();
#endif
    return new Impl();
}()) {
    impl->scene = scene;
    impl->settings = settings;
#ifdef JASPERENGINE_PHYSICS_3D
    impl->applyPhysicsSettings();
    impl->physics.SetGravity(toJolt(impl->gravity));
#endif
}

PlacementWorld::~PlacementWorld() = default;

Scene* PlacementWorld::getScene() const {
    return impl->scene;
}

void PlacementWorld::setSettings(const PlacementWorldSettings& settings) {
    impl->settings = settings;
#ifdef JASPERENGINE_PHYSICS_3D
    impl->applyPhysicsSettings();
    impl->applySpeedLimit();
#endif
}

const PlacementWorldSettings& PlacementWorld::getSettings() const {
    return impl->settings;
}

bool PlacementWorld::addMover(Entity root, std::string* why) {
    if (why) why->clear();
#ifdef JASPERENGINE_PHYSICS_3D
    Scene* scene = impl->scene;
    auto refuse = [why](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };

    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root) || !scene->findComponent<Transform>(root)) {
        return refuse("no mesh or collider");
    }
    if (Impl::DynamicBody* body = impl->findDynamic(root); body && body->mover) return refuse("already a mover");
    for (Entity mover : impl->movers) {
        if (isAncestorOf(scene, mover, root)) return refuse("part of another mover");
        if (isAncestorOf(scene, root, mover)) return refuse("contains another mover");
    }
    std::string reason;
    if (!impl->shapes().canMove(root, &reason)) return refuse(reason);

    DynamicShape built;
    if (!impl->buildDynamicShape(root, built, &reason)) return refuse(reason);

    // the size limit on the world bounds of the shape at the root's pose
    if (impl->worldSize(root, built) > impl->settings.maxMoverSize) {
        char text[96];
        std::snprintf(text, sizeof(text), "too large (> %g m), treated as environment", impl->settings.maxMoverSize);
        return refuse(text);
    }

    // it stops being context, or a neighbour (the neighbours around it are rebuilt later)
    for (Entity e : subtreeOf(scene, root)) {
        impl->removeContextOf(e);
        impl->processed.erase(e);
    }
    std::vector<Entity> overlapping;
    for (Entity neighbour : impl->neighbours) {
        if (neighbour == root || isAncestorOf(scene, neighbour, root) || isAncestorOf(scene, root, neighbour)) overlapping.push_back(neighbour);
    }
    for (Entity neighbour : overlapping) impl->removeDynamic(impl->neighbourBodies, impl->neighbours, neighbour);

    std::unique_ptr<Impl::DynamicBody> body = impl->createDynamicBody(root, true, built);
    if (!body) return refuse("too many bodies in the placement world");
    impl->issueRequests(built, true);
    impl->dynamicByEntity[root] = body.get();
    impl->movers.push_back(root);
    impl->moverBodies.push_back(std::move(body));
    if (why) *why = built.warning;
    return true;
#else
    (void)root;
    if (why) *why = "3D physics is not available in this build";
    return false;
#endif
}

void PlacementWorld::removeMover(Entity root) {
#ifdef JASPERENGINE_PHYSICS_3D
    if (Impl::DynamicBody* body = impl->findDynamic(root); body && body->mover) {
        impl->removeDynamic(impl->moverBodies, impl->movers, root);
    }
#else
    (void)root;
#endif
}

bool PlacementWorld::isMover(Entity root) const {
    return std::find(impl->movers.begin(), impl->movers.end(), root) != impl->movers.end();
}

const std::vector<Entity>& PlacementWorld::getMovers() const {
    return impl->movers;
}

void PlacementWorld::ensureContext(const AABB& region) {
    if (region.isNull()) return;
    impl->contextRegion.merge(region);
#ifdef JASPERENGINE_PHYSICS_3D
    if (impl->scene) impl->supportNeighbours(impl->gatherContext(&region, NULL_ENTITY));
#endif
}

void PlacementWorld::addContextEntity(Entity entity) {
#ifdef JASPERENGINE_PHYSICS_3D
    if (impl->scene && entity != NULL_ENTITY && impl->scene->isEntityCreated(entity)) impl->gatherContext(nullptr, entity);
#else
    (void)entity;
#endif
}

size_t PlacementWorld::getContextBodyCount() const {
#ifdef JASPERENGINE_PHYSICS_3D
    return impl->contextBodyCount;
#else
    return 0;
#endif
}

const std::vector<Entity>& PlacementWorld::getNeighbours() const {
    return impl->neighbours;
}

AABB PlacementWorld::getContextRegion() const {
    return impl->contextRegion;
}

void PlacementWorld::refreshShapes() {
#ifdef JASPERENGINE_PHYSICS_3D
    Scene* scene = impl->scene;
    // bodies whose entity is gone leave the world
    std::vector<Impl::DynamicBody*> gone;
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (!scene || !scene->isEntityCreated(body->entity) || !scene->findComponent<Transform>(body->entity)) gone.push_back(body);
    }
    for (Impl::DynamicBody* body : gone) {
        const Entity entity = body->entity;
        if (body->mover) impl->removeDynamic(impl->moverBodies, impl->movers, entity);
        else impl->removeDynamic(impl->neighbourBodies, impl->neighbours, entity);
    }

    // only a body with a decomposition that has landed since changes shape; the others keep
    // theirs, and their contacts
    ConvexDecompositionCache* cache = impl->settings.decompositionCache;
    JPH::BodyInterface& bi = impl->bodyInterface();
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        bool landed = false;
        for (uint64_t key : body->decompositionKeys) landed = landed || (cache && cache->status(key) == DecompositionStatus::READY);
        if (!landed) continue;
        DynamicShape built;
        if (!impl->buildDynamicShape(body->entity, built, nullptr)) continue;
        // the scene's transform may have moved since; the body keeps its own pose
        const JPH::RVec3 position = bi.GetPosition(body->id);
        const JPH::Quat rotation = bi.GetRotation(body->id);
        const JPH::Vec3 linear = bi.GetLinearVelocity(body->id);
        const JPH::Vec3 angular = bi.GetAngularVelocity(body->id);
        bi.SetShape(body->id, built.shape, true, JPH::EActivation::DontActivate);
        bi.SetPositionAndRotation(body->id, position, rotation, JPH::EActivation::DontActivate);
        impl->forgetContacts(*body);
        // awake, so Jolt finds the new shape's contacts on the next step
        if (!body->fellOut) {
            bi.ActivateBody(body->id);
            bi.SetLinearAndAngularVelocity(body->id, linear, angular);
            body->stillSeconds = 0.0f;
            body->asleep = false;
        }
        body->size = built.shape->GetLocalBounds().GetSize().ReduceMax();
        body->rollingRadius = std::max(0.001f, 0.5f * built.shape->GetLocalBounds().GetSize().ReduceMin());
        body->shape = built.kind;
        body->hullCount = built.hullCount;
        body->decompositionKeys = built.keys;
        impl->issueRequests(built, body->mover);
    }
#endif
}

bool PlacementWorld::hasPendingDecompositions() const {
#ifdef JASPERENGINE_PHYSICS_3D
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (impl->pendingDecomposition(*body)) return true;
    }
#endif
    return false;
}

size_t PlacementWorld::requestDecompositions(Scene* scene, Entity root, const PlacementWorldSettings& settings,
                                             std::vector<std::string>* notes, bool urgent) {
#ifdef JASPERENGINE_PHYSICS_3D
    ConvexDecompositionCache* cache = settings.decompositionCache;
    if (!cache) {
        if (notes) notes->push_back("no decomposition cache");
        return 0;
    }
    const BodyShapes shapes{scene, settings};
    const DecompositionParams params = shapes.decompositionParams();
    size_t count = 0;
    shapes.forEachDecomposable(root, notes, [&](Entity part, uint64_t key, const PartGeometry& geometry) {
        if (cache->status(key) != DecompositionStatus::READY) cache->request(geometry.local, geometry.indices, params, urgent);
        if (cache->status(key) == DecompositionStatus::FAILED) {
            if (notes) notes->push_back(scene->getEntityName(part) + ": " + cache->failureReason(key));
            return;
        }
        count++;
    });
    return count;
#else
    (void)scene;
    (void)root;
    (void)settings;
    (void)urgent;
    if (notes) notes->push_back("3D physics is not available in this build");
    return 0;
#endif
}

bool PlacementWorld::decompositionReady(Scene* scene, Entity root, const PlacementWorldSettings& settings) {
#ifdef JASPERENGINE_PHYSICS_3D
    ConvexDecompositionCache* cache = settings.decompositionCache;
    if (!cache) return false;
    bool ready = true;
    const bool movable = BodyShapes{scene, settings}.forEachDecomposable(root, nullptr, [&](Entity, uint64_t key, const PartGeometry&) {
        ready = ready && cache->status(key) == DecompositionStatus::READY;
    });
    return movable && ready;
#else
    (void)scene;
    (void)root;
    (void)settings;
    return false;
#endif
}

void PlacementWorld::setGravity(const Vector3& gravity) {
    impl->gravity = gravity;
#ifdef JASPERENGINE_PHYSICS_3D
    impl->physics.SetGravity(toJolt(gravity));
#endif
}

Vector3 PlacementWorld::getGravity() const {
    return impl->gravity;
}

void PlacementWorld::setTarget(Entity mover, const PlacementPose& target) {
#ifdef JASPERENGINE_PHYSICS_3D
    Impl::DynamicBody* body = impl->findDynamic(mover);
    if (!body || !body->mover) return;
    const bool starting = !body->hasTarget;
    body->hasTarget = true;
    body->target = target;
    impl->setGravityFactor(body);
    if (starting && !body->fellOut) impl->beginGhosts(*body);
#else
    (void)mover;
    (void)target;
#endif
}

void PlacementWorld::clearTarget(Entity mover) {
#ifdef JASPERENGINE_PHYSICS_3D
    Impl::DynamicBody* body = impl->findDynamic(mover);
    if (!body || !body->mover || !body->hasTarget) return;
    body->hasTarget = false;
    impl->endGhosts(*body);
    impl->setGravityFactor(body);
#else
    (void)mover;
#endif
}

void PlacementWorld::clearTargets() {
    for (Entity mover : impl->movers) clearTarget(mover);
}

bool PlacementWorld::hasTarget(Entity mover) const {
#ifdef JASPERENGINE_PHYSICS_3D
    Impl::DynamicBody* body = impl->findDynamic(mover);
    return body && body->hasTarget;
#else
    (void)mover;
    return false;
#endif
}

void PlacementWorld::teleport(Entity entity, const PlacementPose& pose) {
#ifdef JASPERENGINE_PHYSICS_3D
    Impl::DynamicBody* body = impl->findDynamic(entity);
    if (!body) return;
    JPH::BodyInterface& bi = impl->bodyInterface();
    if (body->fellOut) {
        body->fellOut = false;
        bi.AddBody(body->id, JPH::EActivation::DontActivate);
    }
    bi.SetPositionAndRotation(body->id, JPH::RVec3(toJolt(pose.position)), toJolt(pose.rotation), JPH::EActivation::Activate);
    bi.SetLinearAndAngularVelocity(body->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    impl->forgetContacts(*body);
    body->stillSeconds = 0.0f;
    body->asleep = false;
#else
    (void)entity;
    (void)pose;
#endif
}

void PlacementWorld::stopAll() {
#ifdef JASPERENGINE_PHYSICS_3D
    JPH::BodyInterface& bi = impl->bodyInterface();
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (!body->fellOut) bi.SetLinearAndAngularVelocity(body->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }
#endif
}

void PlacementWorld::setDamping(float linear, float angular) {
#ifdef JASPERENGINE_PHYSICS_3D
    impl->linearDamping = std::max(0.0f, linear);
    impl->angularDamping = std::max(0.0f, angular);
    impl->applyDamping();
#else
    (void)linear;
    (void)angular;
#endif
}

void PlacementWorld::setSeparationBoost(float factor) {
#ifdef JASPERENGINE_PHYSICS_3D
    impl->separationBoost = std::max(1.0f, factor);
    impl->applyPhysicsSettings();
#else
    (void)factor;
#endif
}

void PlacementWorld::wakeAll() {
#ifdef JASPERENGINE_PHYSICS_3D
    JPH::BodyInterface& bi = impl->bodyInterface();
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (body->fellOut) continue;
        bi.ActivateBody(body->id);
        body->stillSeconds = 0.0f;
        body->asleep = false;
    }
#endif
}

void PlacementWorld::clearFlags() {
#ifdef JASPERENGINE_PHYSICS_3D
    for (Impl::DynamicBody* body : impl->dynamicBodies()) body->teleported = false;
    impl->overflowed = false;
#endif
}

void PlacementWorld::step() {
#ifdef JASPERENGINE_PHYSICS_3D
    const int substeps = std::max(1, impl->settings.tuning.substeps);
    const float dt = impl->settings.fixedStep / (float)substeps;
    if (dt <= 0.0f) return;
    for (int i = 0; i < substeps; i++) {
        impl->driveTargets(dt);
        impl->resistRolling(dt);
        const JPH::EPhysicsUpdateError errors = impl->physics.Update(dt, 1, &JoltShapeBuilder::sharedPhysicsTempAllocator(), &JoltShapeBuilder::sharedPhysicsJobSystem());
        if (errors != JPH::EPhysicsUpdateError::None) impl->overflowed = true;
        impl->releaseGhosts();
        impl->settleContacts();
        impl->summariseContacts();
    }
    impl->raiseSunkBodies();
    impl->afterStep();
#endif
    impl->stepCount++;
    impl->simulatedSeconds += impl->settings.fixedStep;
}

uint32_t PlacementWorld::getStepCount() const {
    return impl->stepCount;
}

double PlacementWorld::getSimulatedSeconds() const {
    return impl->simulatedSeconds;
}

bool PlacementWorld::allAsleep() const {
#ifdef JASPERENGINE_PHYSICS_3D
    const JPH::BodyInterface& bi = impl->bodyInterface();
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (body->fellOut) continue;
        if (!body->asleep && bi.IsActive(body->id)) return false;
    }
#endif
    return true;
}

bool PlacementWorld::contactsOverflowed() const {
#ifdef JASPERENGINE_PHYSICS_3D
    return impl->overflowed;
#else
    return false;
#endif
}

float PlacementWorld::getMaxPenetration() const {
    float deepest = 0.0f;
#ifdef JASPERENGINE_PHYSICS_3D
    impl->contactTracker.forEach([&](const PlacementContactTracker::Contact& contact) { deepest = std::max(deepest, contact.depth); });
#endif
    return deepest;
}

PlacementBodyState PlacementWorld::getState(Entity body) const {
#ifdef JASPERENGINE_PHYSICS_3D
    if (Impl::DynamicBody* dynamic = impl->findDynamic(body)) return impl->stateOf(*dynamic);
#endif
    PlacementBodyState state;
    state.entity = body;
    return state;
}

std::vector<PlacementBodyState> PlacementWorld::getStates() const {
    std::vector<PlacementBodyState> states;
#ifdef JASPERENGINE_PHYSICS_3D
    for (Impl::DynamicBody* body : impl->dynamicBodies()) states.push_back(impl->stateOf(*body));
#endif
    return states;
}

AABB PlacementWorld::getMoversBounds() const {
    AABB bounds;
#ifdef JASPERENGINE_PHYSICS_3D
    const JPH::BodyLockInterfaceNoLock& locks = impl->physics.GetBodyLockInterfaceNoLock();
    for (const auto& body : impl->moverBodies) {
        JPH::BodyLockRead lock(locks, body->id);
        if (!lock.Succeeded()) continue;
        const JPH::AABox box = lock.GetBody().GetWorldSpaceBounds();
        bounds.merge(AABB(fromJolt(box.mMin), fromJolt(box.mMax)));
    }
#endif
    return bounds;
}

AABB PlacementWorld::getDynamicBounds() const {
    AABB bounds;
#ifdef JASPERENGINE_PHYSICS_3D
    const JPH::BodyLockInterfaceNoLock& locks = impl->physics.GetBodyLockInterfaceNoLock();
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        JPH::BodyLockRead lock(locks, body->id);
        if (!lock.Succeeded()) continue;
        const JPH::AABox box = lock.GetBody().GetWorldSpaceBounds();
        bounds.merge(AABB(fromJolt(box.mMin), fromJolt(box.mMax)));
    }
#endif
    return bounds;
}

bool PlacementWorld::raycast(const Ray& ray, PlacementRayHit& hit, const std::vector<Entity>& ignore) const {
#ifdef JASPERENGINE_PHYSICS_3D
    JPH::IgnoreMultipleBodiesFilter filter;
    for (Entity entity : ignore) {
        if (Impl::DynamicBody* body = impl->findDynamic(entity)) filter.IgnoreBody(body->id);
        auto context = impl->contextByEntity.find(entity);
        if (context != impl->contextByEntity.end()) {
            for (const Impl::ContextBody& body : context->second) filter.IgnoreBody(body.id);
        }
    }

    const JPH::RRayCast cast(JPH::RVec3(toJolt(ray.getOrigin())), toJolt(ray.getDirection()));
    JPH::RayCastResult result;
    if (!impl->physics.GetNarrowPhaseQueryNoLock().CastRay(cast, result, {}, {}, filter)) return false;

    JPH::BodyLockRead lock(impl->physics.GetBodyLockInterfaceNoLock(), result.mBodyID);
    if (!lock.Succeeded()) return false;
    const JPH::Body& body = lock.GetBody();
    const JPH::RVec3 point = cast.GetPointOnRay(result.mFraction);
    hit.entity = (Entity)body.GetUserData();
    hit.point = fromJolt(JPH::Vec3(point));
    hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    hit.distance = result.mFraction * ray.getDirection().length();
    return true;
#else
    (void)ray;
    (void)hit;
    (void)ignore;
    return false;
#endif
}

uint64_t PlacementWorld::stateHash() const {
    uint64_t hash = 1469598103934665603ull;
    for (const PlacementBodyState& state : getStates()) {
        hash = fnvMix(hash, state.entity);
        // q and -q are the same turn
        const float sign = state.pose.rotation.w < 0.0f ? -1.0f : 1.0f;
        const float values[13] = {state.pose.position.x, state.pose.position.y, state.pose.position.z,
                                  sign * state.pose.rotation.w, sign * state.pose.rotation.x, sign * state.pose.rotation.y, sign * state.pose.rotation.z,
                                  state.linearVelocity.x, state.linearVelocity.y, state.linearVelocity.z,
                                  state.angularVelocity.x, state.angularVelocity.y, state.angularVelocity.z};
        for (float value : values) hash = fnvMix(hash, (uint64_t)quantise(value));
        hash = fnvMix(hash, (state.asleep ? 1u : 0u) | (state.fellOut ? 2u : 0u));
    }
    return hash;
}

void PlacementWorld::collectShapeLines(std::vector<Vector3>& outSegments, bool moversOnly, size_t maxSegments) const {
    outSegments.clear();
#ifdef JASPERENGINE_PHYSICS_3D
    std::vector<JPH::BodyID> ids;
    for (Impl::DynamicBody* body : impl->dynamicBodies()) {
        if (body->mover || !moversOnly) ids.push_back(body->id);
    }
    if (!moversOnly) {
        for (const auto& entry : impl->contextByEntity) {
            for (const Impl::ContextBody& body : entry.second) ids.push_back(body.id);
        }
    }

    const JPH::BodyLockInterfaceNoLock& locks = impl->physics.GetBodyLockInterfaceNoLock();
    constexpr int batch = 64;
    JPH::Float3 vertices[batch * 3];
    for (const JPH::BodyID& id : ids) {
        if (outSegments.size() / 2 >= maxSegments) return;
        JPH::BodyLockRead lock(locks, id);
        if (!lock.Succeeded()) continue;
        const JPH::TransformedShape shape = lock.GetBody().GetTransformedShape();
        JPH::TransformedShape::GetTrianglesContext context;
        shape.GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::RVec3::sZero());

        // a shared edge once per body
        std::set<std::pair<std::tuple<int64_t, int64_t, int64_t>, std::tuple<int64_t, int64_t, int64_t>>> edges;
        auto key = [](const JPH::Float3& p) { return std::make_tuple(quantise(p.x), quantise(p.y), quantise(p.z)); };
        for (int count; (count = shape.GetTrianglesNext(context, batch, vertices)) > 0;) {
            for (int t = 0; t < count; t++) {
                for (int e = 0; e < 3; e++) {
                    const JPH::Float3& a = vertices[t * 3 + e];
                    const JPH::Float3& b = vertices[t * 3 + (e + 1) % 3];
                    auto ka = key(a), kb = key(b);
                    if (ka == kb || !edges.insert(ka < kb ? std::make_pair(ka, kb) : std::make_pair(kb, ka)).second) continue;
                    if (outSegments.size() / 2 >= maxSegments) return;
                    outSegments.push_back(Vector3(a.x, a.y, a.z));
                    outSegments.push_back(Vector3(b.x, b.y, b.z));
                }
            }
        }
    }
#else
    (void)moversOnly;
    (void)maxSegments;
#endif
}
