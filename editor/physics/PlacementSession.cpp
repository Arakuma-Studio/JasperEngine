// SPDX-License-Identifier: MIT
//
// Physics Placement session: the editor side of the simulation shared by the Scene-view tool
// (PlacementSession), place_with_physics and place-physics (PlacementRun). See the header and
// docs/superpowers/specs/2026-10-23-physics-placement-design.md sections 5.4, 6, 7.4, 8.1 and 9.

#include "PlacementSession.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/ObjectTransformCmd.h"
#include "physics/ConvexDecomposition.h"
#include "picking/MeshRaycast.h"
#include "util/EntityPath.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;
using nlohmann::json;

namespace {

    constexpr double kPi = 3.14159265358979323846;
    constexpr float kDegToRad = static_cast<float>(kPi / 180.0);

    // Wall time of stepping the interactive world gets per editor frame
    constexpr double kFrameStepBudgetMs = 8.0;
    // A frame longer than this (a hitch, a breakpoint) is not caught up
    constexpr double kMaxFrameSeconds = 0.25;
    // Mouse rays are directions; the world raycast takes the direction's length as the range
    constexpr float kPointerRayLength = 1000.0f;
    // Separate is done when no pair overlaps deeper than this (spec section 6)
    constexpr float kSeparatedPenetration = 0.001f;
    // Deeper than this at the start: "use mode:separate first" (spec section 8.1)
    constexpr float kInitialOverlapWarning = 0.05f;
    // A target-driven mover is there when this close (approach phase of Move/Snap/Gather/Distribute)
    constexpr float kReachedDistance = 0.001f;
    constexpr float kReachedDegrees = 0.1f;
    // ...or blocked when this long below the sleep thresholds
    constexpr float kStalledSeconds = 0.25f;
    // Without Keep formation a mover whose target runs this far ahead of it (blocked) re-anchors
    constexpr float kFormationSlack = 0.1f;
    // Shift-drag snap: a mover more than kSnapBlockedDistance from its target whose distance did
    // not shrink by kSnapHeadway over the last kSnapBlockedSeconds is blocked, and hops there
    constexpr float kSnapBlockedDistance = 0.02f;
    constexpr float kSnapHeadway = 0.01f;
    constexpr double kSnapBlockedSeconds = 0.15;
    // Separate: extra damping and penetration recovery
    constexpr float kSeparateDamping = 5.0f;
    constexpr float kSeparateBoost = 4.0f;
    // The batch run grows its context region every this many steps (a fixed schedule: deterministic)
    constexpr uint32_t kContextEverySteps = 8;
    // Physics proxy preview cap (line segments)
    constexpr size_t kMaxProxySegments = 50000;

    float r5(double v) {
        const double rounded = std::round(v * 1e5) / 1e5;
        return rounded == 0.0 ? 0.0f : static_cast<float>(rounded);
    }

    json vecJson(const Vector3& v) { return json::array({r5(v.x), r5(v.y), r5(v.z)}); }
    json quatJson(const Quaternion& q) { return json::array({r5(q.x), r5(q.y), r5(q.z), r5(q.w)}); }

    bool finite(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    bool finite(const Quaternion& q) { return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z); }

    // Rotation of `radians` about a unit axis (Quaternion's own angle constructors take the
    // engine's configurable default angle unit)
    Quaternion axisAngle(const Vector3& axis, float radians) {
        const float s = std::sin(radians * 0.5f);
        return Quaternion(std::cos(radians * 0.5f), axis.x * s, axis.y * s, axis.z * s);
    }

    // Shortest rotation taking unit vector a onto unit vector b
    Quaternion fromTo(const Vector3& a, const Vector3& b) {
        const float d = a.dotProduct(b);
        if (d < -0.999999f) {
            Vector3 axis = a.perpendicular();
            axis.normalize();
            return axisAngle(axis, static_cast<float>(kPi));
        }
        const Vector3 c = a.crossProduct(b);
        Quaternion q(1.0f + d, c.x, c.y, c.z);
        q.normalize();
        return q;
    }

    // The angle of the rotation taking a onto b (atan2 of the relative rotation: exact for the
    // small angles acos loses)
    float angleDegrees(const Quaternion& a, const Quaternion& b) {
        const double aw = a.w, ax = a.x, ay = a.y, az = a.z;
        const double bw = b.w, bx = b.x, by = b.y, bz = b.z;
        // conj(a) * b
        const double w = aw * bw + ax * bx + ay * by + az * bz;
        const double x = aw * bx - ax * bw - ay * bz + az * by;
        const double y = aw * by + ax * bz - ay * bw - az * bx;
        const double z = aw * bz - ax * by + ay * bx - az * bw;
        const double v = std::sqrt(x * x + y * y + z * z);
        if (v == 0.0 && w == 0.0) return 0.0f;
        return static_cast<float>(2.0 * std::atan2(v, std::fabs(w)) * 180.0 / kPi);
    }

    std::string fixed(double v, int digits) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
        return buffer;
    }

    // "2 m", "0.5 m", "12 cm"
    std::string meters(double m) {
        if (m < 0.1) return fixed(m * 100.0, 0) + " cm";
        if (m < 10.0 && std::fabs(m - std::round(m)) > 1e-3) return fixed(m, 1) + " m";
        return fixed(m, 0) + " m";
    }

    std::string seconds(double s) {
        if (std::fabs(s - std::round(s)) < 1e-3) return fixed(s, 0) + " s";
        return fixed(s, 1) + " s";
    }

    std::string hex64(uint64_t v) {
        char buffer[17];
        std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(v));
        return buffer;
    }

    // FNV-1a 64
    struct Hasher {
        uint64_t h = 1469598103934665603ull;
        void bytes(const void* data, size_t size) {
            const unsigned char* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; i++) {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        }
        template <typename T> void value(const T& v) { bytes(&v, sizeof(T)); }
        void vec(const Vector3& v) { value(v.x); value(v.y); value(v.z); }
        void quat(const Quaternion& q) { value(q.w); value(q.x); value(q.y); value(q.z); }
    };

    // Local seeded PRNG (never the clock or std::random_device: runs are reproducible)
    struct SplitMix64 {
        uint64_t state;
        explicit SplitMix64(uint64_t seed): state(seed) {}
        uint64_t next() {
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        // [0, 1)
        double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
        // [-1, 1)
        float symmetric() { return static_cast<float>(unit() * 2.0 - 1.0); }
    };

    // ---- the scene's hierarchy, computed from the local values (the render pass may not have
    // run: CLI, tests, a pose written this frame)

    struct LocalPose {
        Vector3 position;
        Quaternion rotation;
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
    };
    using PoseOverrides = std::map<Entity, LocalPose>;

    // The chain from the root down to entity
    std::vector<Entity> ancestry(Scene* scene, Entity entity) {
        std::vector<Entity> chain;
        Entity current = entity;
        while (current != NULL_ENTITY && chain.size() < 256) {
            Transform* transform = scene->findComponent<Transform>(current);
            if (!transform) break;
            chain.push_back(current);
            current = transform->parent;
        }
        std::reverse(chain.begin(), chain.end());
        return chain;
    }

    LocalPose localOf(Scene* scene, Entity entity, const PoseOverrides* overrides) {
        if (overrides) {
            auto it = overrides->find(entity);
            if (it != overrides->end()) return it->second;
        }
        LocalPose pose;
        if (Transform* transform = scene->findComponent<Transform>(entity)) {
            pose.position = transform->position;
            pose.rotation = transform->rotation;
            pose.scale = transform->scale;
        }
        return pose;
    }

    struct WorldFrame {
        Matrix4 matrix;          // modelMatrix
        Quaternion rotation;     // worldRotation
    };

    WorldFrame worldFrameOf(Scene* scene, Entity entity, const PoseOverrides* overrides = nullptr) {
        WorldFrame frame;
        if (entity == NULL_ENTITY) return frame;
        for (Entity e : ancestry(scene, entity)) {
            const LocalPose local = localOf(scene, e, overrides);
            frame.matrix = frame.matrix * Matrix4::translateMatrix(local.position) * local.rotation.getRotationMatrix() *
                           Matrix4::scaleMatrix(local.scale);
            frame.rotation = frame.rotation * local.rotation;
        }
        return frame;
    }

    Entity parentOf(Scene* scene, Entity entity) {
        Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    PlacementPose worldPoseOf(Scene* scene, Entity entity) {
        const WorldFrame frame = worldFrameOf(scene, entity);
        PlacementPose pose;
        pose.position = frame.matrix * Vector3(0.0f, 0.0f, 0.0f);
        pose.rotation = frame.rotation;
        return pose;
    }

    // World pose to the parent's space (PhysicsSystem::updateTransformFromRigidbody), the parent
    // as `overrides` say it is
    void worldToLocal(Scene* scene, Entity entity, const PlacementPose& world, const PoseOverrides* overrides,
                      Vector3& position, Quaternion& rotation) {
        const Entity parent = parentOf(scene, entity);
        if (parent == NULL_ENTITY || !scene->findComponent<Transform>(parent)) {
            position = world.position;
            rotation = world.rotation;
            return;
        }
        const WorldFrame frame = worldFrameOf(scene, parent, overrides);
        position = frame.matrix.inverse() * world.position;
        rotation = frame.rotation.inverse() * world.rotation;
        rotation.normalize();
    }

    bool isDescendantOf(Scene* scene, Entity entity, Entity ancestor) {
        Entity current = parentOf(scene, entity);
        for (int depth = 0; current != NULL_ENTITY && depth < 256; depth++) {
            if (current == ancestor) return true;
            current = parentOf(scene, current);
        }
        return false;
    }

    // root and everything below it
    std::vector<Entity> subtreeOf(Scene* scene, Entity root) {
        std::vector<Entity> out{root};
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity e = transforms->getEntity(i);
            if (e != root && isDescendantOf(scene, e, root)) out.push_back(e);
        }
        return out;
    }

    // World bounds of the meshes of root's subtree (null when none)
    AABB subtreeBounds(Scene* scene, Entity root) {
        AABB bounds;
        for (Entity e : subtreeOf(scene, root)) {
            MeshComponent* mesh = scene->findComponent<MeshComponent>(e);
            if (!mesh || mesh->aabb.isNull() || mesh->aabb.isInfinite()) continue;
            bounds.merge(worldFrameOf(scene, e).matrix * mesh->aabb);
        }
        return bounds;
    }

    // Where the segment origin..origin+d enters box, as a fraction of it
    bool segmentEnters(const AABB& box, const Vector3& origin, const Vector3& d, float& t) {
        if (box.isNull() || box.isInfinite()) return false;
        float lo = 0.0f;
        float hi = 1.0f;
        for (int i = 0; i < 3; i++) {
            const float mn = box.getMinimum()[i];
            const float mx = box.getMaximum()[i];
            if (std::fabs(d[i]) < 1e-12f) {
                if (origin[i] < mn || origin[i] > mx) return false;
                continue;
            }
            float t1 = (mn - origin[i]) / d[i];
            float t2 = (mx - origin[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            lo = std::max(lo, t1);
            hi = std::min(hi, t2);
            if (lo > hi) return false;
        }
        t = lo;
        return true;
    }

    std::vector<std::pair<float, Vector3>> boundsEntries(Scene* scene, const Vector3& origin, const Vector3& d,
                                                         const std::unordered_set<Entity>& ignore, size_t maxCount) {
        std::vector<std::pair<float, Vector3>> entries;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity e = transforms->getEntity(i);
            if (ignore.count(e) || !scene->isEntityActiveInHierarchy(e)) continue;
            AABB local;
            if (TerrainComponent* terrain = scene->findComponent<TerrainComponent>(e)) {
                const float half = terrain->terrainSize * 0.5f;
                local = AABB(-half, std::min(0.0f, terrain->maxHeight), -half, half, std::max(0.0f, terrain->maxHeight), half);
            } else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(e)) {
                if (!mesh->enabled || mesh->numSubmeshes == 0 || mesh->aabb.isNull() || mesh->aabb.isInfinite()) continue;
                local = mesh->aabb;
            } else {
                continue;
            }
            float t = 0.0f;
            if (segmentEnters(worldFrameOf(scene, e).matrix * local, origin, d, t)) entries.push_back({t, origin + d * t});
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (entries.size() > maxCount) entries.resize(maxCount);
        const float length = d.length();
        for (auto& entry : entries) entry.first *= length;   // a fraction of the segment, to metres
        return entries;
    }

    float longestAxis(const AABB& box) {
        if (box.isNull() || box.isInfinite()) return 0.0f;
        const Vector3 size = box.getSize();
        return std::max(size.x, std::max(size.y, size.z));
    }

    AABB expanded(const AABB& box, float margin) {
        if (box.isNull() || box.isInfinite()) return box;
        return AABB(box.getMinimum() - Vector3(margin, margin, margin), box.getMaximum() + Vector3(margin, margin, margin));
    }

    AABB translated(const AABB& box, const Vector3& delta) {
        if (box.isNull() || box.isInfinite()) return box;
        return AABB(box.getMinimum() + delta, box.getMaximum() + delta);
    }

    bool entityAlive(Scene* scene, Entity entity) {
        return scene && entity != NULL_ENTITY && scene->isEntityCreated(entity) && scene->findComponent<Transform>(entity);
    }

    std::string displayName(Scene* scene, Entity entity) {
        const std::string name = scene->getEntityName(entity);
        return name.empty() ? ("Entity " + std::to_string(entity)) : name;
    }

    // "Crate", "Crate, Pebble and 3 more"
    std::string nameList(const std::vector<std::string>& names, size_t shown = 3) {
        std::string out;
        const size_t count = std::min(shown, names.size());
        for (size_t i = 0; i < count; i++) {
            if (i > 0) out += (i + 1 == count && names.size() <= shown) ? " and " : ", ";
            out += names[i];
        }
        if (names.size() > shown) out += " and " + std::to_string(names.size() - shown) + " more";
        return out;
    }

    // Everything the scene's transforms, hierarchy, layers and active flags are, hashed: a
    // context world built from the scene is stale when it changes (an ordinary gizmo drag between
    // two physics gestures moves the table the crate lands on)
    uint64_t sceneFingerprint(const SceneProject* sceneProject) {
        Hasher hasher;
        Scene* scene = sceneProject->scene;
        hasher.value(sceneProject->structureVersion);
        for (Entity entity : sceneProject->entities) {
            hasher.value(entity);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) continue;
            hasher.vec(transform->position);
            hasher.quat(transform->rotation);
            hasher.vec(transform->scale);
            hasher.value(transform->parent);
            // (S07's 256-type mask: its set bits)
            const Signature signature = scene->getSignature(entity);
            for (size_t bit = 0; bit < Signature::size(); bit++) {
                if (signature.test(bit)) hasher.value(static_cast<uint64_t>(bit));
            }
            hasher.value(scene->getEntityLayer(entity));
            hasher.value(scene->isEntityActive(entity));
        }
        return hasher.h;
    }

    // The interactive tool and the batch both commit through this: restore what the preview
    // wrote, then one ObjectTransformCmd for every entity that moved (old values explicit)
    struct CommitEntry {
        Entity entity = NULL_ENTITY;
        LocalPose before;
        LocalPose after;
    };

    // Unmoved, up to float noise, compared component by component: a body the simulation left
    // where it was reads back a hair off (world to local and back), which must not become an
    // undo step or a prefab override (a slide must not record a rotation override)
    bool samePosition(const Vector3& a, const Vector3& b) {
        for (int i = 0; i < 3; i++) {
            const float tolerance = 1e-6f + 1e-6f * std::max(std::fabs(a[i]), std::fabs(b[i]));
            if (std::fabs(a[i] - b[i]) > tolerance) return false;
        }
        return true;
    }

    bool sameRotation(const Quaternion& a, const Quaternion& b) {
        return angleDegrees(a, b) * kDegToRad < 1e-6f;
    }

    size_t applyCommit(Project* project, uint32_t sceneId, const std::vector<CommitEntry>& entries) {
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject || !sceneProject->scene) return 0;
        ObjectTransformCmd* cmd = new ObjectTransformCmd(project, sceneId);
        for (const CommitEntry& entry : entries) {
            // an unchanged component keeps its old value exactly, so it records no override
            const bool moved = !samePosition(entry.before.position, entry.after.position);
            const bool turned = !sameRotation(entry.before.rotation, entry.after.rotation);
            const bool scaled = entry.before.scale != entry.after.scale;
            if (!moved && !turned && !scaled) continue;
            cmd->addEntity(entry.entity, moved ? entry.after.position : entry.before.position,
                           turned ? entry.after.rotation : entry.before.rotation, entry.after.scale,
                           entry.before.position, entry.before.rotation, entry.before.scale);
        }
        const size_t count = cmd->getEntityCount();
        if (count == 0) {
            delete cmd;
            return 0;
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
        sceneProject->needUpdateRender = true;
        return count;
    }

    void requestEditorRedraw() {
        // only the GUI editor has an App as its host (the CLI and the tests run headless)
        if (App* app = dynamic_cast<App*>(&getEditorHost())) {
            app->requestRedraw();
        }
    }

    // At rest for a placement: asleep and, under gravity, touching something (a body the solver
    // lets sleep in mid-air under a weak pull is still falling)
    bool restingState(const PlacementBodyState& state, const Vector3& gravity) {
        if (!state.asleep) return false;
        if (gravity.squaredLength() < 1e-12f) return true;
        return !state.restingOn.empty() || state.contacts > 0;
    }

    // Every dynamic body at rest (or fallen out); *hovering: one is asleep in mid-air under
    // gravity (PlacementWorld no longer lets that happen; kept as a guard)
    bool allResting(const PlacementWorld& world, bool* hovering = nullptr) {
        const Vector3 gravity = world.getGravity();
        bool resting = true;
        for (const PlacementBodyState& state : world.getStates()) {
            if (state.fellOut || restingState(state, gravity)) continue;
            resting = false;
            if (state.asleep && hovering) *hovering = true;
        }
        return resting;
    }

    // allResting for a settle: a body asleep in mid-air is woken to come down
    bool settled(PlacementWorld& world) {
        bool hovering = false;
        const bool resting = allResting(world, &hovering);
        if (hovering) world.wakeAll();
        return resting;
    }

    constexpr const char* kContactsOverflowed =
        "too many contacts in the placement world: results may be wrong; place fewer objects at once";

    std::string colliderName(const PlacementBodyState& state) {
        if (state.shape == PlacementShapeKind::DECOMPOSED) return "decomposed(" + std::to_string(state.hullCount) + ")";
        return placementShapeKindName(state.shape);
    }

    // The decomposition cache of the open project (placementDecompositionCache)
    struct CacheSlot {
        std::filesystem::path projectPath;
        std::unique_ptr<ConvexDecompositionCache> cache;
        uint64_t generation = 0;   // grows each time a cache is dropped
    };
    CacheSlot& cacheSlot() {
        static CacheSlot slot;
        return slot;
    }

    // The interactive sessions alive (placementSceneBusy); main thread only
    struct SessionEntry {
        const PlacementSession* session = nullptr;
        Project* project = nullptr;
        size_t sceneId = 0;
        bool busy() const { return session->isBusy(); }
    };
    std::vector<SessionEntry>& liveSessions() {
        static std::vector<SessionEntry> sessions;
        return sessions;
    }

    // What a world built with `cache` may still use: the cache, unless it was dropped since
    // (placementProjectSwitching, another project)
    struct CacheUse {
        ConvexDecompositionCache* cache = nullptr;
        uint64_t generation = 0;
        bool valid() const { return !cache || generation == cacheSlot().generation; }
    };

    // The points where a segment enters the bounds of the scene's meshes and terrains, nearest
    // first (with their distance along it), from the local transforms (Ray's BOUNDS query reads
    // MeshComponent::worldAABB, which only the render pass refreshes)
    std::vector<std::pair<float, Vector3>> boundsEntries(Scene* scene, const Vector3& origin, const Vector3& d,
                                                         const std::unordered_set<Entity>& ignore, size_t maxCount);

    // A world raycast that hits the nearest surface in the scene, loaded or not: the context
    // around the first drawn triangle the ray meets (MeshRaycast, the Scene view's picking) is
    // loaded first, then the context around each bounds the ray enters, nearest first, while that
    // bounds is nearer than what the world hits (a floor loaded under a table not loaded yet must
    // not take it)
    bool raycastLoading(PlacementWorld& world, Scene* scene, const Ray& ray, const std::vector<Entity>& ignoreBodies,
                        const std::unordered_set<Entity>& ignoreBounds, const std::function<void(const AABB&)>& load,
                        PlacementRayHit& hit) {
        bool found = world.raycast(ray, hit, ignoreBodies);
        if (!load || !scene) return found;
        MeshRaycastOptions options;
        options.hitBackfaces = true;
        options.ignore.assign(ignoreBounds.begin(), ignoreBounds.end());
        options.kinds = MeshRaycast::MESH | MeshRaycast::INSTANCE | MeshRaycast::TERRAIN;
        const MeshRaycastHit surface = MeshRaycast::raycast(scene, ray, ~0u, options);
        if (surface.hit && (!found || surface.distance * ray.getDirection().length() < hit.distance)) {
            load(AABB(surface.point, surface.point));
            found = world.raycast(ray, hit, ignoreBodies);
        }
        for (const auto& entry : boundsEntries(scene, ray.getOrigin(), ray.getDirection(), ignoreBounds, 8)) {
            if (found && entry.first >= hit.distance) break;
            load(AABB(entry.second, entry.second));
            found = world.raycast(ray, hit, ignoreBodies);
        }
        return found;
    }

    // assess()-style: what happened, then what to do next
    std::string placementHint(const PlacementResult& result, bool applied) {
        if (!result.ok) return std::string();
        const size_t movers = std::count_if(result.entities.begin(), result.entities.end(), [](const PlacementEntityResult& e) { return e.mover; });
        const size_t pushed = result.entities.size() - movers;
        const std::string commit = applied ? "applied as one undo step (undo restores every pose)"
                                           : "re-run with apply:true to commit (one undo step)";
        std::string what;
        if (result.mode == PlacementMode::Separate) {
            what = result.allSettled ? "Separated " + std::to_string(movers) + (movers == 1 ? " object" : " objects") + " (no overlap above 1 mm)"
                                     : "Still overlapping; see warnings";
        } else if (result.allSettled) {
            // "on Terrain" when every mover rests on the same thing
            std::string on;
            bool same = movers > 0;
            for (const PlacementEntityResult& e : result.entities) {
                if (!e.mover) continue;
                if (e.restingOnPaths.size() != 1) {
                    same = false;
                    break;
                }
                if (on.empty()) on = e.restingOnPaths[0];
                else if (on != e.restingOnPaths[0]) {
                    same = false;
                    break;
                }
            }
            if (same && !on.empty()) {
                const size_t slash = on.rfind('/');
                on = " on " + (slash == std::string::npos ? on : on.substr(slash + 1));
            } else {
                on.clear();
            }
            what = (movers == 1 ? std::string("Settled") : "All " + std::to_string(movers) + " settled") + on;
        } else {
            size_t settled = 0;
            for (const PlacementEntityResult& e : result.entities) {
                if (e.mover && e.settled) settled++;
            }
            what = std::to_string(settled) + " of " + std::to_string(movers) + " settled; see warnings";
        }
        if (pushed > 0) what += " (" + std::to_string(pushed) + " pushed " + (pushed == 1 ? "neighbour" : "neighbours") + ")";
        return what + "; " + commit + ".";
    }

}

// ============================================================================ shared

std::vector<Entity> editor::placementRoots(Project* project, size_t sceneId, const std::vector<Entity>& selection) {
    std::vector<Entity> roots;
    if (!project) return roots;
    SceneProject* sceneProject = project->getScene(static_cast<uint32_t>(sceneId));
    if (!sceneProject || !sceneProject->scene) return roots;
    Scene* scene = sceneProject->scene;

    std::unordered_set<Entity> selected;
    for (Entity entity : selection) {
        if (entityAlive(scene, entity)) selected.insert(entity);
    }
    std::unordered_set<Entity> taken;
    for (Entity entity : selection) {
        if (!selected.count(entity) || taken.count(entity)) continue;
        // a selected descendant of a selected entity moves with it
        bool underSelected = false;
        Entity current = parentOf(scene, entity);
        for (int depth = 0; current != NULL_ENTITY && depth < 256; depth++) {
            if (selected.count(current)) {
                underSelected = true;
                break;
            }
            current = parentOf(scene, current);
        }
        if (underSelected) continue;
        taken.insert(entity);
        roots.push_back(entity);
    }
    return roots;
}

ConvexDecompositionCache* editor::placementDecompositionCache(Project* project) {
    if (!project) return nullptr;
    const std::filesystem::path projectPath = project->getProjectPath();
    if (projectPath.empty()) return nullptr;
    CacheSlot& slot = cacheSlot();
    if (!slot.cache || slot.projectPath != projectPath) {
        if (slot.cache) {
            slot.cache->cancelAll();
            slot.generation++;
        }
        slot.cache.reset(new ConvexDecompositionCache((project->getProjectInternalPath() / "cache" / "colliders").string()));
        slot.projectPath = projectPath;
    }
    return slot.cache.get();
}

void editor::placementProjectSwitching() {
    CacheSlot& slot = cacheSlot();
    if (slot.cache) {
        slot.cache->cancelAll();
        slot.generation++;
    }
    slot.cache.reset();
    slot.projectPath.clear();
}

uint64_t editor::placementSceneEditStamp(Project* project, size_t sceneId) {
    if (!project) return 0;
    const SceneProject* sceneProject = project->getScene(static_cast<uint32_t>(sceneId));
    if (!sceneProject || !sceneProject->scene) return 0;
    Hasher h;
    h.value(sceneFingerprint(sceneProject));   // structureVersion, every GameObject's Transform, ...
    h.value(static_cast<uint64_t>(CommandHandle::get(sceneId)->position()));
    return h.h == 0 ? 1 : h.h;
}

bool editor::placementSceneBusy(Project* project, size_t sceneId) {
    for (const SessionEntry& entry : liveSessions()) {
        if (entry.project == project && entry.sceneId == sceneId && entry.busy()) return true;
    }
    return false;
}

const char* editor::placementModeName(PlacementMode mode) {
    switch (mode) {
        case PlacementMode::Drop: return "drop";
        case PlacementMode::Separate: return "separate";
        case PlacementMode::Move: return "move";
        case PlacementMode::Snap: return "snap";
        case PlacementMode::Gather: return "gather";
        case PlacementMode::Distribute: return "distribute";
    }
    return "drop";
}

bool editor::parsePlacementMode(const char* name, PlacementMode& out) {
    if (!name) return false;
    for (PlacementMode mode : {PlacementMode::Drop, PlacementMode::Separate, PlacementMode::Move, PlacementMode::Snap,
                               PlacementMode::Gather, PlacementMode::Distribute}) {
        if (std::strcmp(name, placementModeName(mode)) == 0) {
            out = mode;
            return true;
        }
    }
    return false;
}

std::string PlacementSpec::fingerprint() const {
    // Everything that changes a run's result. Entity order does not (movers are sorted by path),
    // nor do the interactive-only settings (settle on release, keep formation, gravity toward the
    // surface, rotate pivot, region margin, max settle seconds) and the Scene-view gizmos.
    Hasher h;
    h.value(static_cast<uint64_t>(sceneId));
    std::vector<Entity> sorted = entities;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    h.value(static_cast<uint64_t>(sorted.size()));
    for (Entity e : sorted) h.value(e);
    h.value(static_cast<int>(mode));
    h.value(hasTargetPosition);
    if (hasTargetPosition) h.vec(targetPosition);
    h.value(hasTargetRotation);
    if (hasTargetRotation) h.quat(targetRotation);
    h.value(hasTargetDelta);
    if (hasTargetDelta) h.vec(targetDelta);
    h.value(targetLocal);
    h.value(hasSnapRay);
    if (hasSnapRay) {
        h.vec(snapRay.getOrigin());
        h.vec(snapRay.getDirection());
    }
    h.value(axis);
    h.value(amount);
    h.vec(gravity);
    h.value(settle);
    h.value(static_cast<int>(context));
    std::vector<Entity> contextSorted = contextEntities;
    std::sort(contextSorted.begin(), contextSorted.end());
    contextSorted.erase(std::unique(contextSorted.begin(), contextSorted.end()), contextSorted.end());
    h.value(static_cast<uint64_t>(contextSorted.size()));
    for (Entity e : contextSorted) h.value(e);
    h.value(contextMargin);
    h.value(jitter.position);
    h.value(jitter.rotationDeg);
    h.value(seed);
    h.value(maxSeconds);

    const PhysicsPlacementSettings& s = settings;
    h.value(static_cast<int>(s.feel));
    h.value(s.pushNeighbours);
    h.value(static_cast<int>(s.colliders));
    h.value(static_cast<int>(s.decomposition));
    h.value(static_cast<int>(s.quality));
    h.value(s.alignToSurface);
    h.value(s.snapOffset);
    h.value(s.ignoreLayerMask);
    h.value(s.maxMoverSize);
    const PlacementTuning tuning = resolvePlacementTuning(s);
    h.value(tuning.maxSpeed);
    h.value(tuning.contactSpeed);
    h.value(tuning.maxAngularSpeed);
    h.value(tuning.teleportDistance);
    h.value(tuning.substeps);
    h.value(tuning.velocityIterations);
    h.value(tuning.positionIterations);
    h.value(tuning.sleepLinear);
    h.value(tuning.sleepAngularDeg);
    h.value(tuning.sleepSeconds);
    h.value(s.killYDepth);
    return hex64(h.h);
}

std::vector<std::string> editor::checkPlacementSpec(Project* project, const PlacementSpec& spec) {
    std::vector<std::string> errors;
    if (!project) {
        errors.push_back("no project is open: open or create a project first");
        return errors;
    }
    const SceneProject* sceneProject = project->getScene(static_cast<uint32_t>(spec.sceneId));
    if (!sceneProject || !sceneProject->scene) {
        errors.push_back("unknown scene " + std::to_string(spec.sceneId) + ": give an open scene (list_scenes shows them)");
        return errors;
    }
    Scene* scene = sceneProject->scene;
    if (sceneProject->sceneType != SceneType::SCENE_3D) {
        errors.push_back("physics placement works in 3D scenes only; \"" + sceneProject->name + "\" is not a 3D scene");
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        errors.push_back("the scene is in Play mode: stop it first (physics placement edits the authored scene)");
    }

    if (spec.entities.empty()) {
        errors.push_back("no entities to place: give entities (ids or paths), or use_selection: true");
    }
    for (Entity entity : spec.entities) {
        if (!entityAlive(scene, entity)) {
            errors.push_back("entity " + std::to_string(entity) + " is not a GameObject of \"" + sceneProject->name +
                             "\": find_entities lists them");
        }
    }

    switch (spec.mode) {
        case PlacementMode::Move:
            if (!spec.hasTargetPosition && !spec.hasTargetDelta && !spec.hasTargetRotation) {
                errors.push_back("mode move needs a target: give target.position, target.delta or target.rotation");
            }
            if (spec.hasTargetPosition && spec.hasTargetDelta) {
                errors.push_back("target.position and target.delta both given: give one of them");
            }
            if ((spec.hasTargetPosition && !finite(spec.targetPosition)) || (spec.hasTargetDelta && !finite(spec.targetDelta))) {
                errors.push_back("target values must be finite numbers");
            }
            if (spec.hasTargetRotation && (!finite(spec.targetRotation) || spec.targetRotation.norm() < 1e-8f)) {
                errors.push_back("target.rotation must be a non-zero quaternion [x, y, z, w] or {\"euler\": [x, y, z]}");
            }
            break;
        case PlacementMode::Snap:
            if (!spec.hasSnapRay) {
                errors.push_back("mode snap needs a ray: give ray {origin, direction} (or {screen: [x, y]}) aimed at a surface");
            } else if (!finite(spec.snapRay.getOrigin()) || !finite(spec.snapRay.getDirection()) ||
                       spec.snapRay.getDirection().squaredLength() < 1e-12f) {
                errors.push_back("ray.direction must be a non-zero vector (and ray.origin finite)");
            }
            break;
        case PlacementMode::Distribute:
            if (spec.axis < 0 || spec.axis > 2) errors.push_back("axis must be x, y or z");
            break;
        case PlacementMode::Gather:
            if (!(spec.amount >= 0.0f && spec.amount <= 1.0f)) errors.push_back("amount must be between 0 and 1 (1 = all the way to the centre)");
            break;
        case PlacementMode::Drop:
        case PlacementMode::Separate:
            break;
    }

    if (!finite(spec.gravity)) errors.push_back("gravity must be three finite numbers");
    if (!(spec.maxSeconds > 0.0f && spec.maxSeconds <= 120.0f)) errors.push_back("max_seconds must be above 0 and at most 120 (simulated seconds)");
    if (!(spec.contextMargin >= 0.0f && spec.contextMargin <= 1000.0f)) errors.push_back("context_margin must be between 0 and 1000 m");
    if (!(spec.jitter.position >= 0.0f && spec.jitter.position <= 100.0f)) errors.push_back("jitter.position must be between 0 and 100 m");
    if (!(spec.jitter.rotationDeg >= 0.0f && spec.jitter.rotationDeg <= 180.0f)) errors.push_back("jitter.rotation_deg must be between 0 and 180");
    if (spec.context == PlacementSpec::Context::Entities) {
        if (spec.contextEntities.empty()) errors.push_back("context lists no entities: give entity refs, \"auto\" or \"none\"");
        for (Entity entity : spec.contextEntities) {
            if (!entityAlive(scene, entity)) {
                errors.push_back("context entity " + std::to_string(entity) + " is not a GameObject of \"" + sceneProject->name + "\"");
            }
        }
    }

    const PhysicsPlacementSettings& s = spec.settings;
    if (!(s.maxMoverSize > 0.0f)) errors.push_back("the size limit (max mover size) must be above 0 m");
    if (!(s.teleportDistance > 0.0f)) errors.push_back("teleport distance must be above 0 m");
    if (!(s.sleepLinear >= 0.0f) || !(s.sleepAngularDeg >= 0.0f) || !(s.sleepSeconds >= 0.0f)) {
        errors.push_back("sleep thresholds must not be negative");
    }
    if (!(s.killYDepth >= 0.0f)) errors.push_back("kill depth must not be negative");
    if (!std::isfinite(s.snapOffset)) errors.push_back("snap offset must be a finite number");
    const PlacementTuning tuning = resolvePlacementTuning(s);
    if (!(tuning.maxSpeed > 0.0f) || !(tuning.contactSpeed > 0.0f) || !(tuning.maxAngularSpeed > 0.0f)) {
        errors.push_back("max speed, contact speed and max angular speed must be above 0");
    }
    if (tuning.substeps < 1 || tuning.velocityIterations < 1 || tuning.positionIterations < 1) {
        errors.push_back("substeps and solver iterations must be at least 1");
    }
    return errors;
}

// ============================================================================ interactive

struct PlacementSession::Impl {
    Project* project = nullptr;
    uint32_t sceneId = 0;
    PhysicsPlacementSettings settings;

    // the private world, built lazily, and what it was built from
    std::unique_ptr<PlacementWorld> world;
    Scene* worldScene = nullptr;
    CacheUse worldCache;
    uint64_t worldFingerprint = 0;
    bool worldDirty = true;
    // the gesture's kill plane, from where the movers are and are sent (not from where they
    // fall: the context region follows a falling body down)
    float killY = -std::numeric_limits<float>::infinity();
    bool killYSet = false;
    uint64_t completionVersion = 0;

    // a world without context for "can this move?" (refusals of the idle selection)
    std::unique_ptr<PlacementWorld> probe;
    struct Probed {
        bool movable = false;
        std::string why;     // refusal, or the caveat of a movable one
    };
    std::map<Entity, Probed> refusalCache;

    PlacementPhase phase = PlacementPhase::Idle;
    PlacementGesture gesture = PlacementGesture::None;

    // the bodies of the running gesture (movers, then neighbours as they joined) and their
    // pre-gesture local and world poses
    struct Body {
        Entity entity = NULL_ENTITY;
        bool mover = true;
        LocalPose local;
        Entity parent = NULL_ENTITY;   // when the snapshot was taken
        PlacementPose start;
        Vector3 targetPosition;    // !keepFormation: the running per-mover target
        bool hasSnapTarget = false;
        PlacementPose snapTarget;
        std::deque<std::pair<double, float>> snapProgress;   // (snap clock, distance to the target)
    };
    std::vector<Body> bodies;
    std::unordered_map<Entity, size_t> bodyIndex;
    std::vector<Entity> movers;
    std::vector<Entity> moverSubtrees;   // the movers and everything below them
    Vector3 startCenter;
    AABB startBounds;              // movers' world bounds when the gesture began
    AABB swept;                    // every bound the movers had during the gesture
    float moverSize = 0.0f;

    // gesture inputs
    bool hasMoveTarget = false;
    Vector3 moveTarget;
    Vector3 lastMoveTarget;
    Quaternion rotateDelta;
    Vector3 gatherScale = Vector3(1.0f, 1.0f, 1.0f);
    bool hasSnapRay = false;
    Ray snapRay;
    bool hasSnapHit = false;
    Vector3 snapAnchor;            // where the group centre is pulled (Snap)
    Quaternion snapRotation;
    bool hasPointerRay = false;
    Ray pointerRay;
    Vector3 dropGravity;
    int distributeAxis = 0;

    // per-step targets of Distribute/Gather (fixed at begin)
    std::vector<PlacementPose> fixedTargets;

    uint64_t beginStructureVersion = 0;   // the scene's structure when the gesture began
    bool committed = false;               // the last gesture ended in a commit (finishNow)
    uint64_t gestureSteps = 0;            // fixed steps the gesture has run
    double accumulator = 0.0;
    double snapClock = 0.0;        // simulated seconds of the Snap gesture
    double settleSeconds = 0.0;
    bool approaching = false;      // Distribute/Gather: targets on until reached, then gravity
    double approachSeconds = 0.0;
    double stalledSeconds = 0.0;
    std::set<Entity> refiningMovers;

    // the idle selection
    std::vector<Entity> selection;
    std::vector<PlacementRefusal> refused;
    std::vector<PlacementRefusal> caveats;
    std::vector<PlacementOverlay::Distance> distances;

    SceneProject* sceneProject() const { return project ? project->getScene(sceneId) : nullptr; }

    Scene* scene() const {
        SceneProject* sp = sceneProject();
        return sp ? sp->scene : nullptr;
    }

    PlacementWorldSettings worldSettings() const {
        PlacementWorldSettings ws = makePlacementWorldSettings(settings, placementDecompositionCache(project));
        ws.killY = killY;
        return ws;
    }

    bool sceneUsable() const {
        SceneProject* sp = sceneProject();
        return sp && sp->scene && sp->sceneType == SceneType::SCENE_3D && sp->playState == ScenePlayState::STOPPED;
    }

    // The scene and the decomposition cache the world was built with are still there
    bool worldValid() const {
        SceneProject* sp = sceneProject();
        return world && sp && sp->scene && sp->scene == worldScene && worldCache.valid();
    }

    // The worlds go without being driven (their scene or cache is gone)
    void dropWorld() {
        world.reset();
        probe.reset();
        worldScene = nullptr;
        worldCache = CacheUse();
        refusalCache.clear();
    }

    // The world matches the scene (rebuilt when the scene changed since it was built)
    bool ensureWorld() {
        SceneProject* sp = sceneProject();
        if (!sp || !sp->scene) {
            dropWorld();
            return false;
        }
        const uint64_t fingerprint = sceneFingerprint(sp);
        if (!world || worldDirty || !worldValid() || worldFingerprint != fingerprint) {
            dropWorld();
            killY = -std::numeric_limits<float>::infinity();
            killYSet = false;
            const PlacementWorldSettings ws = worldSettings();
            world.reset(new PlacementWorld(sp->scene, ws));
            probe.reset(new PlacementWorld(sp->scene, ws));
            worldScene = sp->scene;
            worldCache.cache = ws.decompositionCache;
            worldCache.generation = cacheSlot().generation;
            worldFingerprint = fingerprint;
            worldDirty = false;
            completionVersion = ws.decompositionCache ? ws.decompositionCache->completionVersion() : 0;
        }
        return true;
    }

    void lowerKillY(const AABB& region) {
        if (!world || region.isNull() || region.isInfinite()) return;
        const float candidate = expanded(region, regionMargin()).getMinimum().y - settings.killYDepth;
        if (!killYSet || candidate < killY) {
            killY = candidate;
            killYSet = true;
            world->setSettings(worldSettings());
        }
    }

    float regionMargin() const { return std::max(settings.regionMargin, std::min(moverSize, settings.maxMoverSize)); }

    void growContext(const AABB& region) {
        if (!world || region.isNull() || region.isInfinite()) return;
        world->ensureContext(expanded(region, regionMargin()));
        // neighbours that just joined: their pose before they are pushed
        for (Entity neighbour : world->getNeighbours()) {
            if (bodyIndex.count(neighbour)) continue;
            Scene* sc = scene();
            if (!entityAlive(sc, neighbour)) continue;
            Body body;
            body.entity = neighbour;
            body.mover = false;
            body.local = localOf(sc, neighbour, nullptr);
            body.parent = parentOf(sc, neighbour);
            body.start = world->getState(neighbour).pose;
            bodyIndex[neighbour] = bodies.size();
            bodies.push_back(body);
            // the kill plane under where it starts (the scene still holds that pose: it was
            // never stepped)
            lowerKillY(subtreeBounds(sc, neighbour));
        }
    }

    Vector3 currentCenter() const {
        if (!world || movers.empty()) return startCenter;
        Vector3 sum;
        for (Entity m : movers) sum += world->getState(m).pose.position;
        return sum / static_cast<float>(movers.size());
    }

    const Body& bodyOf(Entity entity) const { return bodies[bodyIndex.at(entity)]; }

    // ---- targets

    void setTargetFor(Entity mover, const PlacementPose& pose) { world->setTarget(mover, pose); }

    void computeMoveTargets() {
        if (!hasMoveTarget) return;
        const Vector3 delta = moveTarget - lastMoveTarget;
        for (Entity m : movers) {
            Body& body = bodies[bodyIndex[m]];
            PlacementPose target;
            target.rotation = body.start.rotation;
            if (settings.keepFormation) {
                target.position = moveTarget + (body.start.position - startCenter);
            } else {
                // each mover follows the handle's motion from where it is; one that is held back
                // stops building up a lag (it does not try to catch its old place up later)
                const Vector3 current = world->getState(m).pose.position;
                if ((body.targetPosition - current).length() > kFormationSlack) body.targetPosition = current;
                body.targetPosition += delta;
                target.position = body.targetPosition;
            }
            setTargetFor(m, target);
        }
        lastMoveTarget = moveTarget;
    }

    void computeRotateTargets() {
        Quaternion r = rotateDelta;
        r.normalize();
        for (Entity m : movers) {
            const Body& body = bodyOf(m);
            PlacementPose target;
            if (settings.rotatePivot == PlacementRotatePivot::GroupCenter) {
                target.position = startCenter + r * (body.start.position - startCenter);
            } else {
                target.position = body.start.position;
            }
            target.rotation = r * body.start.rotation;
            target.rotation.normalize();
            setTargetFor(m, target);
        }
    }

    void computeGatherSpreadTargets() {
        Vector3 s = gatherScale;
        for (int i = 0; i < 3; i++) s[i] = std::min(2.0f, std::max(0.0f, std::isfinite(s[i]) ? s[i] : 1.0f));
        for (Entity m : movers) {
            const Body& body = bodyOf(m);
            PlacementPose target;
            const Vector3 offset = body.start.position - startCenter;
            target.position = startCenter + Vector3(s.x * offset.x, s.y * offset.y, s.z * offset.z);
            target.rotation = body.start.rotation;
            setTargetFor(m, target);
        }
    }

    // Where the ray meets something behind the movers; the context there is loaded first
    bool raycastBehindMovers(const Ray& input, PlacementRayHit& hit) {
        Vector3 direction = input.getDirection();
        if (direction.squaredLength() < 1e-12f || !finite(direction) || !finite(input.getOrigin())) return false;
        direction.normalize();
        const Ray ray(input.getOrigin(), direction * kPointerRayLength);
        const std::unordered_set<Entity> ignore(moverSubtrees.begin(), moverSubtrees.end());
        return raycastLoading(*world, scene(), ray, movers, ignore, [this](const AABB& region) { growContext(region); }, hit);
    }

    void computeSnapTargets() {
        if (hasSnapRay) {
            PlacementRayHit hit;
            if (raycastBehindMovers(snapRay, hit)) {
                Vector3 n = hit.normal;
                if (n.squaredLength() < 1e-12f) n = Vector3(0.0f, 1.0f, 0.0f);
                n.normalize();
                // rotation: the up axis onto the surface normal (one object: its own up; a group:
                // the world up, the formation turning with it)
                Quaternion r;
                if (settings.alignToSurface) {
                    const Vector3 up = movers.size() == 1 ? (bodyOf(movers[0]).start.rotation * Vector3(0.0f, 1.0f, 0.0f)).normalized()
                                                          : Vector3(0.0f, 1.0f, 0.0f);
                    r = fromTo(up, n);
                }
                // how far the group's start bounds reach below its centre, along the normal as the
                // rotated group sees it
                const Vector3 u = r.inverse() * n;
                float support = 0.0f;
                if (!startBounds.isNull() && !startBounds.isInfinite()) {
                    const Vector3 half = startBounds.getHalfSize();
                    support = (startCenter - startBounds.getCenter()).dotProduct(u) + std::fabs(u.x) * half.x +
                              std::fabs(u.y) * half.y + std::fabs(u.z) * half.z;
                }
                snapAnchor = hit.point + n * (support + settings.snapOffset);
                snapRotation = r;
                hasSnapHit = true;
            }
        }
        if (!hasSnapHit) return;
        for (Entity m : movers) {
            const Body& body = bodyOf(m);
            PlacementPose target;
            target.position = snapAnchor + snapRotation * (body.start.position - startCenter);
            target.rotation = snapRotation * body.start.rotation;
            target.rotation.normalize();
            setTargetFor(m, target);
            Body& snapped = bodies[bodyIndex[m]];
            snapped.snapTarget = target;
            snapped.hasSnapTarget = true;
        }
    }

    // Snap slides the movers over the surfaces under the cursor; one that makes no headway
    // toward its target (the side of the table it should be on top of) hops onto it
    void hopBlockedSnapMovers(double simulated) {
        snapClock += simulated;
        for (Entity m : movers) {
            Body& body = bodies[bodyIndex[m]];
            if (!body.hasSnapTarget) continue;
            const float distance = (world->getState(m).pose.position - body.snapTarget.position).length();
            std::deque<std::pair<double, float>>& progress = body.snapProgress;
            progress.push_back({snapClock, distance});
            // the reference: the newest sample at least the window old
            while (progress.size() >= 2 && progress[1].first <= snapClock - kSnapBlockedSeconds + 1e-9) progress.pop_front();
            const bool windowCovered = progress.front().first <= snapClock - kSnapBlockedSeconds + 1e-9;
            if (distance > kSnapBlockedDistance && windowCovered && distance > progress.front().second - kSnapHeadway) {
                world->teleport(m, body.snapTarget);
                progress.clear();
            }
        }
    }

    void updateDropGravity() {
        dropGravity = settings.gravity;
        if (!settings.gravityTowardSurface || !hasPointerRay) return;
        PlacementRayHit hit;
        if (!raycastBehindMovers(pointerRay, hit)) return;
        Vector3 direction = hit.point - currentCenter();
        if (direction.squaredLength() < 1e-8f) return;
        direction.normalize();
        dropGravity = direction * settings.gravity.length();
    }

    void updateTargets() {
        if (phase == PlacementPhase::Settling && !approaching) return;
        switch (gesture) {
            case PlacementGesture::Move: computeMoveTargets(); break;
            case PlacementGesture::Rotate: computeRotateTargets(); break;
            case PlacementGesture::GatherSpread: computeGatherSpreadTargets(); break;
            case PlacementGesture::Snap: computeSnapTargets(); break;
            case PlacementGesture::Drop:
                updateDropGravity();
                world->setGravity(dropGravity);
                break;
            case PlacementGesture::Distribute:
            case PlacementGesture::Gather:
                for (size_t i = 0; i < movers.size() && i < fixedTargets.size(); i++) setTargetFor(movers[i], fixedTargets[i]);
                break;
            case PlacementGesture::None: break;
        }
    }

    // Distribute / Gather button targets, from the start poses
    void computeFixedTargets() {
        fixedTargets.clear();
        for (Entity m : movers) fixedTargets.push_back(bodyOf(m).start);
        if (gesture == PlacementGesture::Distribute) {
            std::vector<size_t> order(movers.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            const int axis = distributeAxis;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return bodyOf(movers[a]).start.position[axis] < bodyOf(movers[b]).start.position[axis];
            });
            if (order.size() >= 2) {
                const float lo = bodyOf(movers[order.front()]).start.position[axis];
                const float hi = bodyOf(movers[order.back()]).start.position[axis];
                for (size_t k = 0; k < order.size(); k++) {
                    fixedTargets[order[k]].position[axis] = lo + (hi - lo) * static_cast<float>(k) / static_cast<float>(order.size() - 1);
                }
            }
        } else if (gesture == PlacementGesture::Gather) {
            // to the centre across gravity (gathering things on a floor never lifts them)
            Vector3 down = settings.gravity;
            const bool hasDown = down.squaredLength() > 1e-12f;
            if (hasDown) down.normalize();
            for (size_t i = 0; i < movers.size(); i++) {
                Vector3 pull = startCenter - fixedTargets[i].position;
                if (hasDown) pull -= down * pull.dotProduct(down);
                fixedTargets[i].position += pull;
            }
        }
    }

    // ---- reading back, writing preview poses

    void writePreview() {
        Scene* sc = scene();
        if (!sc || !world) return;
        // the world's dynamic bodies (getState of anything else is a default pose)
        std::unordered_set<Entity> live(world->getMovers().begin(), world->getMovers().end());
        live.insert(world->getNeighbours().begin(), world->getNeighbours().end());
        // parents first: a neighbour may hold a mover
        PoseOverrides written;
        std::vector<std::pair<size_t, Entity>> ordered;
        for (const Body& body : bodies) ordered.push_back({ancestry(sc, body.entity).size(), body.entity});
        std::sort(ordered.begin(), ordered.end());
        for (const auto& item : ordered) {
            const Entity entity = item.second;
            Transform* transform = sc->findComponent<Transform>(entity);
            if (!transform) continue;
            if (!live.count(entity)) continue;   // not a body (any more)
            const PlacementBodyState state = world->getState(entity);
            Vector3 position;
            Quaternion rotation;
            worldToLocal(sc, entity, state.pose, &written, position, rotation);
            if (position != transform->position || rotation != transform->rotation) {
                transform->position = position;
                transform->rotation = rotation;
                transform->needUpdate = true;
            }
            written[entity] = LocalPose{position, rotation, transform->scale};
        }
        if (SceneProject* sp = sceneProject()) sp->needUpdateRender = true;
    }

    // What still exists back where it was: its old local values, or (re-parented since) its old
    // world pose in the new parent's space; parents first
    void restoreSnapshot() {
        Scene* sc = scene();
        if (!sc) return;
        std::vector<std::pair<size_t, size_t>> ordered;
        for (size_t i = 0; i < bodies.size(); i++) {
            if (sc->isEntityCreated(bodies[i].entity)) ordered.push_back({ancestry(sc, bodies[i].entity).size(), i});
        }
        std::sort(ordered.begin(), ordered.end());
        for (const auto& item : ordered) {
            const Body& body = bodies[item.second];
            Transform* transform = sc->findComponent<Transform>(body.entity);
            if (!transform) continue;
            if (transform->parent == body.parent) {
                transform->position = body.local.position;
                transform->rotation = body.local.rotation;
                transform->scale = body.local.scale;
            } else {
                worldToLocal(sc, body.entity, body.start, nullptr, transform->position, transform->rotation);
            }
            transform->needUpdate = true;
        }
        if (SceneProject* sp = sceneProject()) sp->needUpdateRender = true;
    }

    // ---- lifecycle

    void resetGesture() {
        if (world) {
            world->clearTargets();
            for (Entity m : movers) world->removeMover(m);
            world->setGravity(Vector3(0.0f, 0.0f, 0.0f));
            world->setDamping(0.0f, 0.0f);
            world->stopAll();
            world->clearFlags();
        }
        bodies.clear();
        bodyIndex.clear();
        movers.clear();
        moverSubtrees.clear();
        fixedTargets.clear();
        refiningMovers.clear();
        hasMoveTarget = false;
        hasSnapRay = false;
        hasSnapHit = false;
        hasPointerRay = false;
        rotateDelta = Quaternion();
        snapRotation = Quaternion();
        gatherScale = Vector3(1.0f, 1.0f, 1.0f);
        accumulator = 0.0;
        snapClock = 0.0;
        settleSeconds = 0.0;
        approaching = false;
        approachSeconds = 0.0;
        stalledSeconds = 0.0;
        phase = PlacementPhase::Idle;
        gesture = PlacementGesture::None;
    }

    void commit() {
        committed = true;
        SceneProject* sp = sceneProject();
        Scene* sc = scene();
        std::vector<CommitEntry> entries;
        if (sc) {
            for (const Body& body : bodies) {
                Transform* transform = sc->findComponent<Transform>(body.entity);
                if (!transform || !sc->isEntityCreated(body.entity)) continue;
                CommitEntry entry;
                entry.entity = body.entity;
                entry.before = body.local;
                entry.after = LocalPose{transform->position, transform->rotation, transform->scale};
                entries.push_back(entry);
            }
        }
        // the Transforms hold the preview: put the pre-gesture poses back, then one command
        // moves everything (undo returns there, redo here)
        restoreSnapshot();
        if (sp) applyCommit(project, sceneId, entries);
        if (!worldValid()) dropWorld();
        resetGesture();
        // the world's context and neighbours are where the scene now is
        if (sp && world) worldFingerprint = sceneFingerprint(sp);
        refreshSelection();
    }

    // restore: put the pre-gesture poses back (false: the scene is not the gesture's any more)
    void cancelGesture(bool restore) {
        const bool hadNeighbours = std::any_of(bodies.begin(), bodies.end(), [](const Body& b) { return !b.mover; });
        if (restore) restoreSnapshot();
        // a world whose scene or decomposition cache is gone is dropped, never driven
        if (!restore || !worldValid()) dropWorld();
        resetGesture();
        // pushed neighbours are displaced in the world: build it again
        if (hadNeighbours) worldDirty = true;
        if (restore) refreshSelection();
    }

    // false (and the gesture dropped) when what the gesture works on went away
    bool validate() {
        SceneProject* sp = sceneProject();
        if (!sp || !sp->scene || sp->scene != worldScene) {
            // another scene (closed, reloaded, switched): its entities are not ours to restore
            cancelGesture(false);
            return false;
        }
        if (!worldCache.valid()) {
            // the decomposition cache went (project switch): the world can't be driven any more
            cancelGesture(true);
            return false;
        }
        if (sp->structureVersion != beginStructureVersion) {
            // something else restructured the scene (a delete, a re-parent): the gesture is off
            cancelGesture(true);
            worldDirty = true;
            return false;
        }
        if (sp->playState != ScenePlayState::STOPPED) {
            cancelGesture(true);
            return false;
        }
        for (const Body& body : bodies) {
            if (!entityAlive(sp->scene, body.entity) || parentOf(sp->scene, body.entity) != body.parent) {
                // deleted or re-parented mid-gesture: put the others back, rebuild the world
                cancelGesture(true);
                worldDirty = true;
                return false;
            }
        }
        return true;
    }

    void pollDecompositions() {
        ConvexDecompositionCache* cache = worldCache.cache;
        if (!cache || !world) return;
        const uint64_t version = cache->completionVersion();
        if (version != completionVersion) {
            completionVersion = version;
            world->refreshShapes();
        }
        for (Entity m : movers) {
            if (world->getState(m).decompositionPending) refiningMovers.insert(m);
        }
    }

    bool moversStill(double threshold) const {
        const float angular = settings.sleepAngularDeg * kDegToRad;
        for (Entity m : movers) {
            const PlacementBodyState state = world->getState(m);
            if (state.fellOut) continue;
            if (state.linearVelocity.length() > threshold || state.angularVelocity.length() > angular) return false;
        }
        return true;
    }

    bool moversReached() const {
        for (size_t i = 0; i < movers.size() && i < fixedTargets.size(); i++) {
            const PlacementBodyState state = world->getState(movers[i]);
            if (state.fellOut) continue;
            if ((state.pose.position - fixedTargets[i].position).length() > kReachedDistance) return false;
            if (angleDegrees(state.pose.rotation, fixedTargets[i].rotation) > kReachedDegrees) return false;
        }
        return true;
    }

    void enterSettle() {
        // released where it is: no momentum from the drive carries it off
        world->clearTargets();
        world->stopAll();
        world->setGravity(settings.gravity);
        world->wakeAll();
        settleSeconds = 0.0;
        approaching = false;
    }

    void step(double deltaSeconds) {
        if (phase == PlacementPhase::Idle || !world) return;
        if (!validate()) return;
        pollDecompositions();
        updateTargets();

        // context around everywhere the movers and pushed neighbours were, are and are heading
        const AABB bounds = world->getDynamicBounds();
        if (!bounds.isNull()) swept.merge(bounds);
        AABB region = swept;
        AABB sentTo;
        if (gesture == PlacementGesture::Move && hasMoveTarget) sentTo = translated(startBounds, moveTarget - startCenter);
        if (gesture == PlacementGesture::Snap && hasSnapHit) sentTo = translated(startBounds, snapAnchor - startCenter);
        region.merge(sentTo);
        lowerKillY(sentTo);
        growContext(region);

        const bool waitForColliders = settings.decomposition == PlacementDecomposition::ALWAYS && world->hasPendingDecompositions();
        const double fixedStep = world->getSettings().fixedStep;
        uint32_t steps = 0;
        if (!waitForColliders && fixedStep > 0.0) {
            accumulator += std::min(std::max(deltaSeconds, 0.0), kMaxFrameSeconds);
            const auto begin = std::chrono::steady_clock::now();
            while (accumulator >= fixedStep) {
                const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                if (elapsedMs >= kFrameStepBudgetMs) {
                    // the frame's budget is spent: what is left is dropped, not owed to the next
                    accumulator = 0.0;
                    break;
                }
                world->step();
                accumulator -= fixedStep;
                steps++;
                gestureSteps++;
            }
        }

        if (gesture == PlacementGesture::Snap && phase == PlacementPhase::Active && steps > 0) {
            hopBlockedSnapMovers(steps * fixedStep);
        }

        writePreview();

        if (phase == PlacementPhase::Settling && steps > 0) {
            const double simulated = steps * fixedStep;
            if (approaching) {
                approachSeconds += simulated;
                stalledSeconds = moversStill(settings.sleepLinear) ? stalledSeconds + simulated : 0.0;
                if (moversReached() || stalledSeconds >= kStalledSeconds || approachSeconds >= settings.maxSettleSeconds) {
                    enterSettle();
                }
            } else {
                settleSeconds += simulated;
                if (settled(*world) || settleSeconds >= settings.maxSettleSeconds) {
                    commit();
                    requestEditorRedraw();
                    return;
                }
            }
        }
        requestEditorRedraw();
    }

    // ---- the idle selection

    Probed probeMover(Entity root) {
        auto cached = refusalCache.find(root);
        if (cached != refusalCache.end()) return cached->second;
        Probed probed;
        probed.movable = probe->addMover(root, &probed.why);
        if (probed.movable) probe->removeMover(root);
        else if (probed.why.empty()) probed.why = "cannot be moved";
        refusalCache[root] = probed;
        return probed;
    }

    void refreshSelection() {
        refused.clear();
        caveats.clear();
        distances.clear();
        if (phase != PlacementPhase::Idle || !sceneUsable() || !ensureWorld()) return;
        Scene* sc = scene();
        const std::vector<Entity> roots = placementRoots(project, sceneId, selection);
        std::vector<Entity> movable;
        for (Entity root : roots) {
            const Probed probed = probeMover(root);
            if (probed.movable) {
                movable.push_back(root);
                if (!probed.why.empty()) caveats.push_back({root, probed.why});
            } else {
                refused.push_back({root, probed.why});
            }
        }
        if (!settings.showDistanceIndicators) return;
        for (Entity root : movable) {
            const AABB bounds = subtreeBounds(sc, root);
            const Vector3 center = bounds.isNull() ? worldPoseOf(sc, root).position : bounds.getCenter();
            const Vector3 half = bounds.isNull() ? Vector3() : bounds.getHalfSize();
            const float range = std::max(settings.regionMargin, std::min(longestAxis(bounds), settings.maxMoverSize));
            world->ensureContext(expanded(bounds.isNull() ? AABB(center, center) : bounds, range));
            const std::vector<Entity> ignore = subtreeOf(sc, root);
            for (int axis = 0; axis < 3; axis++) {
                for (float sign : {1.0f, -1.0f}) {
                    Vector3 direction;
                    direction[axis] = sign;
                    PlacementRayHit hit;
                    if (!world->raycast(Ray(center, direction * (half[axis] + range)), hit, ignore)) continue;
                    PlacementOverlay::Distance distance;
                    distance.from = center + direction * std::min(half[axis], hit.distance);
                    distance.to = hit.point;
                    distance.meters = std::max(0.0f, hit.distance - half[axis]);
                    distance.axis = axis;
                    distances.push_back(distance);
                }
            }
        }
    }

    bool begin(PlacementGesture newGesture, const std::vector<Entity>& newSelection) {
        if (phase != PlacementPhase::Idle && validate()) {
            // the running gesture ends first, as it is (committed)
            commit();
        }
        selection = newSelection;
        refused.clear();
        caveats.clear();
        distances.clear();
        if (newGesture == PlacementGesture::None || !sceneUsable() || !ensureWorld()) return false;
        Scene* sc = scene();

        const std::vector<Entity> roots = placementRoots(project, sceneId, newSelection);
        for (Entity root : roots) {
            std::string why;
            if (world->addMover(root, &why)) {
                movers.push_back(root);
                if (!why.empty()) caveats.push_back({root, why});
            } else {
                refused.push_back({root, why.empty() ? std::string("cannot be moved") : why});
            }
        }
        if (movers.empty()) return false;

        gesture = newGesture;
        phase = PlacementPhase::Active;
        beginStructureVersion = sceneProject()->structureVersion;
        gestureSteps = 0;
        Vector3 sum;
        for (Entity m : movers) {
            Body body;
            body.entity = m;
            body.mover = true;
            body.local = localOf(sc, m, nullptr);
            body.parent = parentOf(sc, m);
            body.start = world->getState(m).pose;
            body.targetPosition = body.start.position;
            sum += body.start.position;
            bodyIndex[m] = bodies.size();
            bodies.push_back(body);
        }
        startCenter = sum / static_cast<float>(movers.size());
        for (Entity m : movers) {
            for (Entity e : subtreeOf(sc, m)) moverSubtrees.push_back(e);
        }
        startBounds = world->getMoversBounds();
        swept = startBounds;
        moverSize = longestAxis(startBounds);
        killYSet = false;
        lowerKillY(startBounds);
        moveTarget = lastMoveTarget = startCenter;
        hasMoveTarget = false;
        world->clearFlags();
        world->setDamping(0.0f, 0.0f);
        world->setGravity(gesture == PlacementGesture::Drop ? settings.gravity : Vector3(0.0f, 0.0f, 0.0f));
        dropGravity = settings.gravity;
        if (gesture == PlacementGesture::Distribute || gesture == PlacementGesture::Gather) computeFixedTargets();
        growContext(startBounds);
        return true;
    }

    bool waitingForColliders() const {
        return world && settings.decomposition == PlacementDecomposition::ALWAYS && world->hasPendingDecompositions();
    }

    bool finishNow(double wallBudgetSeconds) {
        if (phase == PlacementPhase::Idle || !validate()) return false;
        committed = false;
        const auto begin = std::chrono::steady_clock::now();
        auto budgetLeft = [&]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() < wallBudgetSeconds;
        };
        // the background decomposition may still land within the budget
        auto waitForColliders = [&]() {
            while (waitingForColliders() && budgetLeft()) {
                pollDecompositions();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        if (gestureSteps == 0) {
            // nothing simulated yet: with no colliders to simulate with, nothing to commit
            waitForColliders();
            if (waitingForColliders()) {
                cancelGesture(true);
                return false;
            }
        }
        if (phase == PlacementPhase::Active) end();   // released: Drop commits, the others settle
        while (phase != PlacementPhase::Idle && budgetLeft()) {
            if (waitingForColliders()) {
                waitForColliders();
                continue;
            }
            // one fixed step a call: the settle's own ends (at rest, maxSettleSeconds) commit
            step(world->getSettings().fixedStep);
        }
        if (phase != PlacementPhase::Idle) commit();
        return committed;
    }

    void end() {
        if (phase != PlacementPhase::Active || !validate()) return;
        if (gesture == PlacementGesture::Drop) {
            commit();
            return;
        }
        if (gesture == PlacementGesture::Distribute || gesture == PlacementGesture::Gather) {
            // the targets stay on until the movers get there (or are blocked), then they settle
            phase = PlacementPhase::Settling;
            approaching = true;
            approachSeconds = 0.0;
            stalledSeconds = 0.0;
            requestEditorRedraw();
            return;
        }
        if (settings.settleOnRelease) {
            phase = PlacementPhase::Settling;
            enterSettle();
            requestEditorRedraw();
            return;
        }
        commit();
    }
};

PlacementSession::PlacementSession(Project* project, size_t sceneId): impl(new Impl) {
    impl->project = project;
    impl->sceneId = static_cast<uint32_t>(sceneId);
    liveSessions().push_back({this, project, sceneId});
}

PlacementSession::~PlacementSession() {
    cancel();
    std::vector<SessionEntry>& sessions = liveSessions();
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [this](const SessionEntry& e) { return e.session == this; }),
                   sessions.end());
}

size_t PlacementSession::getSceneId() const { return impl->sceneId; }

void PlacementSession::setSettings(const PhysicsPlacementSettings& settings) {
    const PhysicsPlacementSettings old = impl->settings;
    impl->settings = settings;
    const bool rebuild = old.colliders != settings.colliders || old.decomposition != settings.decomposition ||
                         old.quality != settings.quality || old.ignoreLayerMask != settings.ignoreLayerMask ||
                         old.maxMoverSize != settings.maxMoverSize || old.pushNeighbours != settings.pushNeighbours;
    if (rebuild) {
        // the bodies are built with these: the next gesture gets a new world (the running one
        // keeps its bodies, with the new feel)
        impl->worldDirty = true;
        impl->refusalCache.clear();
    }
    if (impl->world) impl->world->setSettings(impl->worldSettings());
    if (impl->probe && !rebuild) impl->probe->setSettings(impl->worldSettings());
    if (impl->phase == PlacementPhase::Idle && (rebuild || old.showDistanceIndicators != settings.showDistanceIndicators ||
                                                old.regionMargin != settings.regionMargin)) {
        impl->refreshSelection();
    }
}

const PhysicsPlacementSettings& PlacementSession::getSettings() const { return impl->settings; }

bool PlacementSession::begin(PlacementGesture gesture, const std::vector<Entity>& selection) {
    return impl->begin(gesture, selection);
}

bool PlacementSession::isGestureActive() const { return impl->phase == PlacementPhase::Active; }
bool PlacementSession::isBusy() const { return impl->phase != PlacementPhase::Idle; }
PlacementGesture PlacementSession::getGesture() const { return impl->gesture; }
PlacementPhase PlacementSession::getPhase() const { return impl->phase; }

void PlacementSession::setMoveTarget(const Vector3& center) {
    if (impl->phase != PlacementPhase::Active || !finite(center)) return;
    impl->moveTarget = center;
    impl->hasMoveTarget = true;
}

void PlacementSession::setRotateDelta(const Quaternion& delta) {
    if (impl->phase != PlacementPhase::Active || !finite(delta) || delta.norm() < 1e-12f) return;
    impl->rotateDelta = delta;
}

void PlacementSession::setGatherScale(const Vector3& scale) {
    if (impl->phase != PlacementPhase::Active || !finite(scale)) return;
    impl->gatherScale = scale;
}

void PlacementSession::setSnapRay(const Ray& ray) {
    if (impl->phase != PlacementPhase::Active) return;
    impl->snapRay = ray;
    impl->hasSnapRay = true;
}

void PlacementSession::setPointerRay(const Ray& ray) {
    impl->pointerRay = ray;
    impl->hasPointerRay = true;
}

void PlacementSession::end() { impl->end(); }

bool PlacementSession::finishNow(double wallBudgetSeconds) { return impl->finishNow(wallBudgetSeconds); }

void PlacementSession::cancel() {
    if (impl->phase == PlacementPhase::Idle) return;
    SceneProject* sp = impl->sceneProject();
    // a scene that is not the one the gesture began in: nothing of it to restore
    impl->cancelGesture(sp && sp->scene && sp->scene == impl->worldScene);
}

bool PlacementSession::distribute(const std::vector<Entity>& selection, int axis) {
    if (axis < 0 || axis > 2) return false;
    impl->distributeAxis = axis;
    if (!impl->begin(PlacementGesture::Distribute, selection)) return false;
    impl->end();
    return true;
}

bool PlacementSession::gather(const std::vector<Entity>& selection) {
    if (!impl->begin(PlacementGesture::Gather, selection)) return false;
    impl->end();
    return true;
}

void PlacementSession::tick(double deltaSeconds) {
    impl->step(deltaSeconds);
}

void PlacementSession::notifySceneChanged() {
    impl->worldDirty = true;
    impl->refusalCache.clear();
    if (impl->phase == PlacementPhase::Idle) impl->refreshSelection();
}

void PlacementSession::setSelection(const std::vector<Entity>& selection) {
    if (impl->phase != PlacementPhase::Idle) return;
    impl->selection = selection;
    impl->refreshSelection();
}

PlacementHud PlacementSession::getHud() const {
    PlacementHud hud;
    hud.phase = impl->phase;
    hud.gesture = impl->gesture;
    hud.movers = impl->movers.size();
    const bool live = impl->worldValid();
    hud.contextBodies = live ? impl->world->getContextBodyCount() : 0;
    hud.refused = impl->refused;
    hud.caveats = impl->caveats;
    if (impl->phase == PlacementPhase::Idle || !live) {
        hud.status = "Ready";
        return hud;
    }
    for (Entity m : impl->movers) {
        if (impl->world->getState(m).decompositionPending) hud.pendingDecompositions++;
    }
    if (hud.pendingDecompositions > 0) {
        const size_t total = std::max(impl->refiningMovers.size(), hud.pendingDecompositions);
        hud.status = "Refining colliders " + std::to_string(total - hud.pendingDecompositions) + "/" + std::to_string(total);
    } else if (impl->phase == PlacementPhase::Settling) {
        hud.status = "Settling\xE2\x80\xA6";
    } else if (allResting(*impl->world)) {
        hud.status = "Resting";
    } else {
        hud.status = "Simulating";
    }
    // Jolt dropped contacts this gesture (cleared when the next begins)
    if (impl->world->contactsOverflowed()) hud.status += " \xC2\xB7 too many contacts: place fewer objects at once";
    return hud;
}

void PlacementSession::collectOverlay(PlacementOverlay& out) const {
    out = PlacementOverlay();
    out.refused = impl->refused;
    PlacementWorld* world = impl->worldValid() ? impl->world.get() : nullptr;
    if (world && impl->settings.showProxies) world->collectShapeLines(out.proxySegments, false, kMaxProxySegments);
    if (impl->phase == PlacementPhase::Idle || !world) {
        if (impl->settings.showDistanceIndicators) out.distances = impl->distances;
        return;
    }
    out.active = true;
    out.region = world->getContextRegion();

    for (const Impl::Body& body : impl->bodies) {
        if (!body.mover) continue;
        out.ghosts.push_back({body.entity, body.start.position, body.start.rotation});
    }

    // contacts: where each touching mover meets the context, found along its contact normal
    for (Entity m : impl->movers) {
        const PlacementBodyState state = world->getState(m);
        if (!state.touchingContext || state.averageContactNormal.squaredLength() < 1e-12f) continue;
        const Vector3 n = state.averageContactNormal.normalized();
        const float reach = std::max(0.5f, impl->moverSize);
        PlacementRayHit hit;
        std::vector<Entity> ignore{m};
        Vector3 point = state.pose.position;
        if (world->raycast(Ray(state.pose.position, -n * reach), hit, ignore)) point = hit.point;
        out.contactPoints.push_back(point);
        bool slowing = false;
        if (world->hasTarget(m)) {
            const Vector3 velocity = state.linearVelocity;
            slowing = velocity.squaredLength() > 1e-8f ? velocity.normalized().dotProduct(n) < -0.1f : false;
            if (!slowing && impl->phase == PlacementPhase::Active) {
                // held against it: the target is on the other side of the contact
                const Vector3 toTarget = impl->gesture == PlacementGesture::Move && impl->hasMoveTarget
                                             ? impl->moveTarget - impl->currentCenter()
                                             : Vector3();
                slowing = toTarget.squaredLength() > 1e-6f && toTarget.normalized().dotProduct(n) < -0.1f;
            }
        }
        out.contactSlowing.push_back(slowing);
    }

    if ((impl->gesture == PlacementGesture::Move && impl->hasMoveTarget) || (impl->gesture == PlacementGesture::Snap && impl->hasSnapHit)) {
        out.showTeleportRing = impl->phase == PlacementPhase::Active;
        out.teleportCenter = impl->currentCenter();
        out.teleportRadius = world->getSettings().tuning.teleportDistance;
        const Vector3 handle = impl->gesture == PlacementGesture::Move ? impl->moveTarget : impl->snapAnchor;
        out.teleportExceeded = (handle - out.teleportCenter).length() > out.teleportRadius;
    }
}

Vector3 PlacementSession::getMoversCenter() const {
    if (impl->phase != PlacementPhase::Idle && impl->worldValid()) return impl->currentCenter();
    Scene* scene = impl->scene();
    const std::vector<Entity> roots = placementRoots(impl->project, impl->sceneId, impl->selection);
    if (!scene || roots.empty()) return Vector3();
    Vector3 sum;
    for (Entity root : roots) sum += worldPoseOf(scene, root).position;
    return sum / static_cast<float>(roots.size());
}

// ============================================================================ batch

struct PlacementRun::Impl {
    Project* project = nullptr;
    PlacementSpec spec;
    PlacementResult result;
    std::unique_ptr<PlacementWorld> world;
    Scene* scene = nullptr;
    bool started = false;
    bool done = false;

    std::vector<Entity> movers;                 // sorted by path
    std::map<Entity, std::string> paths;
    std::map<Entity, PlacementPose> before;     // movers and neighbours, before anything moved
    struct StartTransform {
        LocalPose local;
        Entity parent = NULL_ENTITY;
    };
    std::map<Entity, StartTransform> startTransforms;   // their Transforms then (commitPlacement checks them)
    std::vector<Entity> neighbourOrder;
    std::vector<PlacementPose> targets;         // parallel to movers (target modes)
    Vector3 startCenter;
    AABB startBounds;
    float margin = 2.0f;
    // the kill plane, from where the movers start and are sent (never from where they fall:
    // the context region follows a falling body down)
    float killY = -std::numeric_limits<float>::infinity();
    bool killYSet = false;
    CacheUse cacheUse;                          // the decomposition cache the world was built with
    std::map<Entity, std::string> caveats;      // movers accepted with a caveat

    // Placed: Snap without settle, put on the surface and not stepped
    enum class Stage { WaitColliders, Approach, Settle, Separate, Placed, Finished } stage = Stage::Settle;
    uint32_t maxSteps = 0;
    double stalledSeconds = 0.0;
    float initialPenetration = 0.0f;
    bool pendingAtStart = false;
    bool separated = false;
    bool reachedOrStalled = false;
    bool contactsOverflowed = false;            // a step ran out of Jolt's contact buffers
    std::map<Entity, uint32_t> settleStep;      // 0: not settled now
    size_t asleepCount = 0;
    size_t bodyCount = 0;

    PlacementWorldSettings worldSettings() const {
        PlacementWorldSettings ws = makePlacementWorldSettings(spec.settings, cacheUse.cache);
        ws.killY = killY;
        // an agent moving something toward a target wants the path, stopping at walls, never a
        // jump through them
        if (spec.mode == PlacementMode::Move) ws.tuning.teleportDistance = std::numeric_limits<float>::infinity();
        return ws;
    }

    // The scene and the decomposition cache the world was built with are still there
    bool usable() const {
        if (!world || !project) return false;
        const SceneProject* sp = project->getScene(static_cast<uint32_t>(spec.sceneId));
        return sp && sp->scene == scene && cacheUse.valid();
    }

    std::string pathOf(Entity entity) {
        auto it = paths.find(entity);
        if (it != paths.end()) return it->second;
        const SceneProject* sp = project->getScene(static_cast<uint32_t>(spec.sceneId));
        const std::string path = sp ? EntityPath::of(*sp, entity) : std::string();
        paths[entity] = path;
        return path;
    }

    bool usesGravityAtEnd() const {
        if (spec.mode == PlacementMode::Separate) return false;
        if (spec.mode == PlacementMode::Drop) return true;
        return spec.settle;
    }

    void growContext(const AABB& region) {
        if (spec.context != PlacementSpec::Context::Auto || region.isNull() || region.isInfinite()) return;
        world->ensureContext(expanded(region, margin));
        noteNeighbours();
    }

    // region: where the movers are or are sent (with its margin), or a context entity
    void lowerKillY(const AABB& region) {
        if (region.isNull() || region.isInfinite()) return;
        const float candidate = region.getMinimum().y - spec.settings.killYDepth;
        if (!killYSet || candidate < killY) {
            killY = candidate;
            killYSet = true;
            world->setSettings(worldSettings());
        }
    }

    // The world raycast, the context it reaches loaded first (context auto)
    bool raycastLoading(const Ray& ray, PlacementRayHit& hit) {
        std::unordered_set<Entity> ignore;
        for (Entity m : movers) {
            for (Entity e : subtreeOf(scene, m)) ignore.insert(e);
        }
        std::function<void(const AABB&)> load;
        if (spec.context == PlacementSpec::Context::Auto) load = [this](const AABB& region) { growContext(region); };
        return ::raycastLoading(*world, scene, ray, movers, ignore, load, hit);
    }

    void noteStartTransform(Entity entity) {
        StartTransform& start = startTransforms[entity];
        start.local = localOf(scene, entity, nullptr);
        start.parent = parentOf(scene, entity);
    }

    void noteNeighbours() {
        for (Entity neighbour : world->getNeighbours()) {
            if (before.count(neighbour)) continue;
            before[neighbour] = world->getState(neighbour).pose;
            noteStartTransform(neighbour);
            neighbourOrder.push_back(neighbour);
            // the kill plane under where it starts (its scene pose: it was never stepped)
            lowerKillY(expanded(subtreeBounds(scene, neighbour), margin));
        }
    }

    // The current start poses (after jitter)
    std::vector<PlacementPose> startPoses() const {
        std::vector<PlacementPose> poses;
        for (Entity m : movers) poses.push_back(world->getState(m).pose);
        return poses;
    }

    bool fail(const std::string& error) {
        result.ok = false;
        result.error = error;
        done = true;
        world.reset();
        return false;
    }

    bool setUpTargets(std::string& error) {
        const std::vector<PlacementPose> poses = startPoses();
        Vector3 sum;
        for (const PlacementPose& pose : poses) sum += pose.position;
        // targets are about the poses the run starts from (jitter applied)
        startCenter = sum / static_cast<float>(poses.size());
        targets.clear();
        switch (spec.mode) {
            case PlacementMode::Move: {
                // target.space local: in the space of the first mover's parent, in path order (the
                // run's own order: the fingerprint ignores the order entities are given in)
                Vector3 center = startCenter;
                Quaternion rotation;
                const Entity parent = parentOf(scene, movers[0]);
                const bool local = spec.targetLocal && parent != NULL_ENTITY;
                const WorldFrame frame = local ? worldFrameOf(scene, parent) : WorldFrame();
                if (spec.hasTargetPosition) center = local ? frame.matrix * spec.targetPosition : spec.targetPosition;
                if (spec.hasTargetDelta) {
                    center = startCenter + (local ? frame.matrix * spec.targetDelta - frame.matrix * Vector3(0.0f, 0.0f, 0.0f)
                                                  : spec.targetDelta);
                }
                if (spec.hasTargetRotation) {
                    rotation = spec.targetRotation.normalized();
                    if (local) rotation = (frame.rotation * rotation * frame.rotation.inverse()).normalized();
                }
                for (size_t i = 0; i < movers.size(); i++) {
                    PlacementPose target;
                    target.position = center + rotation * (poses[i].position - startCenter);
                    target.rotation = (rotation * poses[i].rotation).normalized();
                    targets.push_back(target);
                }
                break;
            }
            case PlacementMode::Snap: {
                Vector3 direction = spec.snapRay.getDirection();
                direction.normalize();
                const Ray ray(spec.snapRay.getOrigin(), direction * kPointerRayLength);
                PlacementRayHit hit;
                if (!raycastLoading(ray, hit)) {
                    error = "the snap ray hits nothing within " + meters(kPointerRayLength) +
                            ": aim ray {origin, direction} at a surface (context: auto loads what it hits)";
                    return false;
                }
                Vector3 n = hit.normal.squaredLength() > 1e-12f ? hit.normal.normalized() : Vector3(0.0f, 1.0f, 0.0f);
                Quaternion r;
                if (spec.settings.alignToSurface) {
                    const Vector3 up = movers.size() == 1 ? (poses[0].rotation * Vector3(0.0f, 1.0f, 0.0f)).normalized() : Vector3(0.0f, 1.0f, 0.0f);
                    r = fromTo(up, n);
                }
                const Vector3 u = r.inverse() * n;
                float support = 0.0f;
                if (!startBounds.isNull() && !startBounds.isInfinite()) {
                    const Vector3 half = startBounds.getHalfSize();
                    support = (startCenter - startBounds.getCenter()).dotProduct(u) + std::fabs(u.x) * half.x +
                              std::fabs(u.y) * half.y + std::fabs(u.z) * half.z;
                }
                const Vector3 anchor = hit.point + n * (support + spec.settings.snapOffset);
                for (size_t i = 0; i < movers.size(); i++) {
                    PlacementPose target;
                    target.position = anchor + r * (poses[i].position - startCenter);
                    target.rotation = (r * poses[i].rotation).normalized();
                    targets.push_back(target);
                }
                break;
            }
            case PlacementMode::Gather: {
                Vector3 down = spec.gravity;
                const bool hasDown = down.squaredLength() > 1e-12f;
                if (hasDown) down.normalize();
                for (size_t i = 0; i < movers.size(); i++) {
                    Vector3 pull = startCenter - poses[i].position;
                    if (hasDown) pull -= down * pull.dotProduct(down);
                    PlacementPose target = poses[i];
                    target.position += pull * spec.amount;
                    targets.push_back(target);
                }
                break;
            }
            case PlacementMode::Distribute: {
                targets = poses;
                const int axis = spec.axis;
                std::vector<size_t> order(movers.size());
                for (size_t i = 0; i < order.size(); i++) order[i] = i;
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return poses[a].position[axis] < poses[b].position[axis]; });
                if (order.size() >= 2) {
                    const float lo = poses[order.front()].position[axis];
                    const float hi = poses[order.back()].position[axis];
                    for (size_t k = 0; k < order.size(); k++) {
                        targets[order[k]].position[axis] = lo + (hi - lo) * static_cast<float>(k) / static_cast<float>(order.size() - 1);
                    }
                }
                break;
            }
            case PlacementMode::Drop:
            case PlacementMode::Separate:
                break;
        }
        if (!targets.empty()) {
            // where the group is sent: its context, and the kill plane under it
            Vector3 targetCenter;
            for (const PlacementPose& target : targets) targetCenter += target.position;
            targetCenter = targetCenter / static_cast<float>(targets.size());
            const AABB sent = translated(startBounds, targetCenter - startCenter);
            growContext(sent);
            lowerKillY(expanded(sent, margin));
        }
        return true;
    }

    void beginStage() {
        switch (spec.mode) {
            case PlacementMode::Drop:
                stage = Stage::Settle;
                world->setGravity(spec.gravity);
                break;
            case PlacementMode::Separate:
                stage = Stage::Separate;
                world->setGravity(Vector3(0.0f, 0.0f, 0.0f));
                world->setDamping(kSeparateDamping, kSeparateDamping);
                world->setSeparationBoost(kSeparateBoost);
                break;
            case PlacementMode::Snap:
                // placed on the surface the ray hit (not driven there: the path may be blocked),
                // then settled, which also resolves a small overlap at the spot
                for (size_t i = 0; i < movers.size(); i++) world->teleport(movers[i], targets[i]);
                reachedOrStalled = true;
                if (spec.settle) {
                    stage = Stage::Settle;
                    world->setGravity(spec.gravity);
                } else {
                    stage = Stage::Placed;
                }
                break;
            default:
                stage = Stage::Approach;
                world->setGravity(Vector3(0.0f, 0.0f, 0.0f));
                for (size_t i = 0; i < movers.size(); i++) world->setTarget(movers[i], targets[i]);
                break;
        }
        world->wakeAll();
    }

    bool reached() const {
        for (size_t i = 0; i < movers.size(); i++) {
            const PlacementBodyState state = world->getState(movers[i]);
            if (state.fellOut) continue;
            if ((state.pose.position - targets[i].position).length() > kReachedDistance) return false;
            if (angleDegrees(state.pose.rotation, targets[i].rotation) > kReachedDegrees) return false;
        }
        return true;
    }

    // Under gravity a body has settled when the world put it to sleep (a bounce's apex is slow
    // too); without, when it is below the sleep thresholds
    bool bodySettled(const PlacementBodyState& state) const {
        if (state.fellOut) return false;
        if (stage == Stage::Settle) return restingState(state, world->getGravity());
        if (state.asleep) return true;
        return state.linearVelocity.length() <= spec.settings.sleepLinear &&
               state.angularVelocity.length() <= spec.settings.sleepAngularDeg * kDegToRad;
    }

    // one fixed step and the bookkeeping after it; true when the run is over
    bool stepOnce() {
        if (stage == Stage::Placed) return true;
        if (stage == Stage::WaitColliders) {
            if (world->hasPendingDecompositions()) return false;
            world->refreshShapes();
            beginStage();
            return false;
        }
        const double fixedStep = world->getSettings().fixedStep;
        world->step();
        contactsOverflowed = contactsOverflowed || world->contactsOverflowed();
        const uint32_t step = world->getStepCount();
        if (step % kContextEverySteps == 0) growContext(world->getDynamicBounds());

        const std::vector<PlacementBodyState> states = world->getStates();
        asleepCount = 0;
        bodyCount = states.size();
        bool allStill = true;
        for (const PlacementBodyState& state : states) {
            const bool settledNow = bodySettled(state);
            if (state.asleep || state.fellOut) asleepCount++;
            if (!settledNow && !state.fellOut) allStill = false;
            uint32_t& at = settleStep[state.entity];
            if (settledNow) {
                if (at == 0) at = step;
            } else {
                at = 0;
            }
        }
        if (step == 1) initialPenetration = std::max(initialPenetration, world->getMaxPenetration());

        // the stage's own end first: one reached on the last step counts
        switch (stage) {
            case Stage::Separate:
                if (world->getMaxPenetration() <= kSeparatedPenetration) {
                    separated = true;
                    return true;
                }
                break;
            case Stage::Approach: {
                stalledSeconds = allStill ? stalledSeconds + fixedStep : 0.0;
                if (reached() || stalledSeconds >= kStalledSeconds) {
                    reachedOrStalled = true;
                    // there: the drive's last velocity must not carry them on
                    world->clearTargets();
                    world->stopAll();
                    if (!spec.settle) {
                        for (const PlacementBodyState& state : states) {
                            uint32_t& at = settleStep[state.entity];
                            if (!state.fellOut && at == 0) at = step;
                        }
                        return true;
                    }
                    world->setGravity(spec.gravity);
                    world->wakeAll();
                    settleStep.clear();
                    stage = Stage::Settle;
                }
                break;
            }
            case Stage::Settle:
                if (settled(*world)) return true;
                break;
            case Stage::WaitColliders:
            case Stage::Placed:
            case Stage::Finished:
                return true;
        }
        return step >= maxSteps;
    }

    // The scene or the decomposition cache went away mid-run: done, without touching either
    void abandon() {
        stage = Stage::Finished;
        done = true;
        result.ok = false;
        result.cancelled = true;
        result.error = "the scene or the project went away during the run";
        world.reset();
    }

    void finish(bool cancelled) {
        const bool placed = stage == Stage::Placed;   // Snap without settle: put there, not stepped
        stage = Stage::Finished;
        done = true;
        result.cancelled = cancelled;
        result.ok = !cancelled;
        if (cancelled) result.error = "cancelled";
        if (!world) return;
        result.stateHash = hex64(world->stateHash());
        result.simSeconds = world->getSimulatedSeconds();
        result.steps = world->getStepCount();
        result.contextBodies = world->getContextBodyCount();
        result.contextRegion = world->getContextRegion();

        const float maxSecondsShown = spec.maxSeconds;
        std::vector<std::string> fellOut, neverSlept, unreached, teleported, refining, caveated;

        std::vector<Entity> reported = movers;
        for (Entity neighbour : neighbourOrder) reported.push_back(neighbour);

        // parents first, so a child's local pose is taken in its parent's final pose
        std::vector<std::pair<size_t, size_t>> depthOrder;
        for (size_t i = 0; i < reported.size(); i++) depthOrder.push_back({ancestry(scene, reported[i]).size(), i});
        std::sort(depthOrder.begin(), depthOrder.end());
        PoseOverrides overrides;
        std::vector<PlacementEntityResult> entries(reported.size());
        std::vector<bool> keep(reported.size(), true);
        bool allSettled = true;
        for (const auto& item : depthOrder) {
            const size_t index = item.second;
            const Entity entity = reported[index];
            const PlacementBodyState state = world->getState(entity);
            PlacementEntityResult& e = entries[index];
            e.entity = entity;
            e.name = displayName(scene, entity);
            e.path = pathOf(entity);
            e.mover = state.mover;
            e.before = before[entity];
            const StartTransform& start = startTransforms[entity];
            e.beforeLocalPosition = start.local.position;
            e.beforeLocalRotation = start.local.rotation;
            e.beforeLocalScale = start.local.scale;
            e.beforeParent = start.parent;
            e.after = state.pose;
            worldToLocal(scene, entity, e.after, &overrides, e.afterLocalPosition, e.afterLocalRotation);
            Transform* transform = scene->findComponent<Transform>(entity);
            overrides[entity] = LocalPose{e.afterLocalPosition, e.afterLocalRotation, transform ? transform->scale : Vector3(1.0f, 1.0f, 1.0f)};
            e.movedMeters = (e.after.position - e.before.position).length();
            e.rotatedDegrees = angleDegrees(e.before.rotation, e.after.rotation);
            const auto settledAt = settleStep.find(entity);
            e.settled = !state.fellOut && ((settledAt != settleStep.end() && settledAt->second != 0) || (placed && state.mover));
            e.settleStep = e.settled && settledAt != settleStep.end() ? settledAt->second : 0;
            if (spec.mode == PlacementMode::Separate) e.settled = e.settled && separated;
            e.restingOn = state.restingOn;
            for (Entity below : state.restingOn) e.restingOnPaths.push_back(pathOf(below));
            e.maxPenetration = state.maxPenetration;
            e.collider = colliderName(state);
            e.fellOut = state.fellOut;
            e.teleported = state.teleported;
            if (!e.mover && e.movedMeters < 1e-4f && e.rotatedDegrees < 0.01f) {
                keep[index] = false;   // a neighbour nothing touched
                continue;
            }
            if (e.fellOut) {
                e.warnings.push_back("fell out of the world (below y = " + fixed(killY, 1) + "): nothing under it to land on");
                fellOut.push_back(e.name);
            } else if (!e.settled && usesGravityAtEnd()) {
                e.warnings.push_back("never came to rest within " + seconds(maxSecondsShown));
                neverSlept.push_back(e.name);
            } else if (e.mover && !targets.empty() && !spec.settle && !reachedOrStalled) {
                e.warnings.push_back("still on its way to the target after " + seconds(maxSecondsShown));
                unreached.push_back(e.name);
            }
            // Snap teleports on purpose
            if (e.teleported && spec.mode != PlacementMode::Snap) {
                e.warnings.push_back("teleported: its target was more than " + meters(world->getSettings().tuning.teleportDistance) + " away");
                teleported.push_back(e.name);
            }
            if (state.decompositionPending) {
                e.warnings.push_back("its collider was still being refined (convex hull used)");
                refining.push_back(e.name);
            }
            const auto caveat = caveats.find(entity);
            if (caveat != caveats.end()) {
                e.warnings.push_back(caveat->second);
                caveated.push_back(e.name + ": " + caveat->second);
            }
            if (!e.settled) allSettled = false;
        }
        for (size_t i = 0; i < reported.size(); i++) {
            if (keep[i]) result.entities.push_back(entries[i]);
        }
        if (spec.mode == PlacementMode::Separate) allSettled = allSettled && separated;
        else if (!usesGravityAtEnd()) allSettled = allSettled && reachedOrStalled;
        result.allSettled = allSettled && !cancelled;

        if (initialPenetration > kInitialOverlapWarning && spec.mode == PlacementMode::Snap) {
            result.warnings.push_back("the snap pose overlapped something by " + meters(initialPenetration) +
                                      ": aim the ray at a clearer spot, or raise the snap offset");
        } else if (initialPenetration > kInitialOverlapWarning && spec.mode != PlacementMode::Separate) {
            result.warnings.push_back("objects overlapped by " + meters(initialPenetration) + " at the start: use mode:separate first");
        }
        if (spec.mode == PlacementMode::Separate && !separated && !cancelled) {
            result.warnings.push_back("still overlapping by " + meters(world->getMaxPenetration()) + " after " + seconds(result.simSeconds) +
                                      ": raise max_seconds, or move them apart by hand");
        }
        if (!fellOut.empty()) {
            result.warnings.push_back(nameList(fellOut) + " fell out of the world: give " + (fellOut.size() == 1 ? "it" : "them") +
                                      " something to land on (raise context_margin, or check context)");
        }
        if (!neverSlept.empty()) {
            result.warnings.push_back(nameList(neverSlept) + " never came to rest within " + seconds(maxSecondsShown) + ": raise max_seconds");
        }
        if (!unreached.empty()) {
            result.warnings.push_back(nameList(unreached) + " did not reach the target within " + seconds(maxSecondsShown) +
                                      ": raise max_seconds, or a faster feel");
        }
        if (!teleported.empty()) {
            result.warnings.push_back(nameList(teleported) + " teleported (target farther than " +
                                      meters(world->getSettings().tuning.teleportDistance) + "): move in smaller steps, or raise the teleport distance");
        }
        for (const std::string& line : caveated) result.warnings.push_back(line);
        if (contactsOverflowed) result.warnings.push_back(kContactsOverflowed);
        if (!refining.empty() || pendingAtStart) {
            result.warnings.push_back("colliders were still being refined" + (refining.empty() ? std::string() : " for " + nameList(refining)) +
                                      ", so convex hulls were used and a re-run may differ: use decompose:\"always\" for a reproducible result");
        }
        result.hint = placementHint(result, false);
        world.reset();
    }
};

PlacementRun::PlacementRun(): impl(new Impl) {}
PlacementRun::~PlacementRun() = default;

bool PlacementRun::start(Project* project, const PlacementSpec& spec) {
    Impl& r = *impl;
    r = Impl();
    r.project = project;
    r.spec = spec;
    r.started = true;
    r.result.mode = spec.mode;
    r.result.sceneId = spec.sceneId;
    r.result.seed = spec.seed;
    r.result.specFingerprint = spec.fingerprint();

    const std::vector<std::string> errors = checkPlacementSpec(project, spec);
    if (!errors.empty()) {
        std::string joined;
        for (const std::string& error : errors) joined += (joined.empty() ? "" : "; ") + error;
        return r.fail(joined);
    }
    SceneProject* sp = project->getScene(static_cast<uint32_t>(spec.sceneId));
    r.scene = sp->scene;
    r.result.sceneStructureVersion = sp->structureVersion;
    r.result.sceneEditStamp = placementSceneEditStamp(project, spec.sceneId);

    // movers in path order: the same scene gives the same insertion order every run
    std::vector<Entity> roots = placementRoots(project, spec.sceneId, spec.entities);
    for (Entity root : roots) r.pathOf(root);
    std::stable_sort(roots.begin(), roots.end(), [&](Entity a, Entity b) {
        const std::string& pa = r.paths[a];
        const std::string& pb = r.paths[b];
        return pa != pb ? pa < pb : a < b;
    });

    r.cacheUse.cache = placementDecompositionCache(project);
    r.cacheUse.generation = cacheSlot().generation;
    r.world.reset(new PlacementWorld(r.scene, r.worldSettings()));
    std::vector<std::string> loading;
    for (Entity root : roots) {
        std::string why;
        if (r.world->addMover(root, &why)) {
            r.movers.push_back(root);
            r.before[root] = r.world->getState(root).pose;
            r.noteStartTransform(root);
            if (!why.empty()) r.caveats[root] = why;   // accepted with a caveat: a warning, not a refusal
        } else {
            if (why.empty()) why = "cannot be moved";
            r.result.refused.push_back({root, why});
            if (why.find("loading") != std::string::npos) loading.push_back(displayName(r.scene, root));
        }
    }
    if (!loading.empty()) {
        r.result.warnings.push_back(nameList(loading) + ": mesh still loading; wait for it to load and run again");
    }
    if (r.movers.empty()) {
        std::string reasons;
        for (const PlacementRefusal& refusal : r.result.refused) {
            reasons += (reasons.empty() ? "" : "; ") + r.pathOf(refusal.entity) + ": " + refusal.reason;
        }
        return r.fail("nothing can move (" + reasons + ")");
    }
    r.startBounds = r.world->getMoversBounds();
    r.margin = std::max(spec.contextMargin, std::min(longestAxis(r.startBounds), spec.settings.maxMoverSize));

    // context, and the kill plane under it and the movers
    r.lowerKillY(expanded(r.startBounds, r.margin));
    if (spec.context == PlacementSpec::Context::Entities) {
        for (Entity entity : spec.contextEntities) {
            if (r.world->isMover(entity)) continue;
            r.world->addContextEntity(entity);
            r.lowerKillY(subtreeBounds(r.scene, entity));
        }
    } else if (spec.context == PlacementSpec::Context::Auto) {
        r.growContext(r.startBounds);
    }

    // seeded jitter, before the first step
    if (spec.jitter.position > 0.0f || spec.jitter.rotationDeg > 0.0f) {
        SplitMix64 random(spec.seed);
        for (Entity m : r.movers) {
            PlacementPose pose = r.world->getState(m).pose;
            const Vector3 offset(random.symmetric(), random.symmetric(), random.symmetric());
            Vector3 axis(random.symmetric(), random.symmetric(), random.symmetric());
            const float angle = random.symmetric() * spec.jitter.rotationDeg * kDegToRad;
            if (axis.squaredLength() < 1e-6f) axis = Vector3(0.0f, 1.0f, 0.0f);
            axis.normalize();
            pose.position += offset * spec.jitter.position;
            if (spec.jitter.rotationDeg > 0.0f) pose.rotation = (axisAngle(axis, angle) * pose.rotation).normalized();
            r.world->teleport(m, pose);
        }
        r.growContext(r.world->getDynamicBounds());
        r.lowerKillY(expanded(r.world->getMoversBounds(), r.margin));
    }

    std::string error;
    if (!r.setUpTargets(error)) return r.fail(error);

    // nothing to land on: a ray along gravity from each mover, down to the kill plane, finds no
    // context (context auto loads what the ray reaches, as the fall would)
    if (r.usesGravityAtEnd() && spec.gravity.squaredLength() > 1e-12f) {
        const Vector3 down = spec.gravity.normalized();
        std::vector<std::string> nothingBelow;
        for (size_t i = 0; i < r.movers.size(); i++) {
            const Vector3 from = r.targets.empty() ? r.world->getState(r.movers[i]).pose.position : r.targets[i].position;
            float reach = longestAxis(r.startBounds) + spec.settings.killYDepth + r.margin;
            if (r.killYSet && down.y < -0.1f) reach = std::max(reach, (from.y - r.killY) / -down.y);
            PlacementRayHit hit;
            if (!r.raycastLoading(Ray(from, down * std::min(reach, kPointerRayLength)), hit)) {
                nothingBelow.push_back(displayName(r.scene, r.movers[i]));
            }
        }
        if (!nothingBelow.empty()) {
            r.result.warnings.push_back(
                "nothing to land on under " + nameList(nothingBelow) + ": " + (nothingBelow.size() == 1 ? "it falls" : "they fall") +
                " out of the world; " +
                (spec.context == PlacementSpec::Context::Auto ? std::string(nothingBelow.size() == 1 ? "move it" : "move them") + " over a surface, or add one below"
                                                               : std::string("add the surface below to context, or use context:\"auto\"")));
        }
    }

    r.maxSteps = static_cast<uint32_t>(std::ceil(spec.maxSeconds / r.world->getSettings().fixedStep - 1e-6));
    r.initialPenetration = r.world->getMaxPenetration();
    r.pendingAtStart = r.world->hasPendingDecompositions() && spec.settings.decomposition != PlacementDecomposition::ALWAYS;
    if (spec.settings.decomposition == PlacementDecomposition::ALWAYS && r.world->hasPendingDecompositions()) {
        r.stage = Impl::Stage::WaitColliders;
    } else {
        r.beginStage();
    }
    return true;
}

bool PlacementRun::advance(double budgetMs) {
    Impl& r = *impl;
    if (r.done) return true;
    if (!r.started || !r.world) {
        r.done = true;
        return true;
    }
    if (!r.usable()) {
        r.abandon();
        return true;
    }
    // the scene is read on this thread only, so an edit can only have come between two slices:
    // anything changed since the start fails the run (deterministic: an unedited scene always
    // gives the same run, sliced or not)
    if (placementSceneEditStamp(r.project, r.spec.sceneId) != r.result.sceneEditStamp) {
        r.fail("the scene changed during the simulation: run it again");
        return true;
    }
    const auto begin = std::chrono::steady_clock::now();
    for (;;) {
        const bool waiting = r.stage == Impl::Stage::WaitColliders;
        if (r.stepOnce()) {
            r.finish(false);
            return true;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        if (budgetMs > 0.0 && elapsedMs >= budgetMs) return false;
        // straight through (CLI): the background decomposition is waited for without spinning
        if (waiting && r.stage == Impl::Stage::WaitColliders) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool PlacementRun::isDone() const { return impl->done; }

void PlacementRun::cancel() {
    Impl& r = *impl;
    if (r.done) return;
    if (!r.started) {
        r.done = true;
        r.result.cancelled = true;
        r.result.error = "cancelled";
        return;
    }
    if (!r.usable()) {
        r.abandon();
        return;
    }
    r.finish(true);
}

float PlacementRun::progress() const {
    const Impl& r = *impl;
    if (r.done) return 1.0f;
    if (!r.world || r.maxSteps == 0) return 0.0f;
    return std::min(1.0f, static_cast<float>(r.world->getStepCount()) / static_cast<float>(r.maxSteps));
}

std::string PlacementRun::progressText() const {
    const Impl& r = *impl;
    if (r.done) {
        return r.result.cancelled ? "cancelled" : (r.result.ok ? "done, " + std::to_string(r.result.steps) + " steps" : "failed");
    }
    if (!r.world) return "not started";
    if (r.stage == Impl::Stage::WaitColliders) {
        ConvexDecompositionCache* cache = placementDecompositionCache(r.project);
        return "waiting for colliders, " + std::to_string(cache ? cache->pendingCount() : 0) + " pending";
    }
    const size_t bodies = r.bodyCount > 0 ? r.bodyCount : r.movers.size();
    return "step " + std::to_string(r.world->getStepCount()) + "/" + std::to_string(r.maxSteps) + ", " + std::to_string(r.asleepCount) +
           " of " + std::to_string(bodies) + " asleep";
}

const PlacementResult& PlacementRun::result() const { return impl->result; }

// ============================================================================ commit, JSON

bool editor::commitPlacement(Project* project, const PlacementResult& result, std::string& error) {
    if (!project) {
        error = "no project is open";
        return false;
    }
    if (!result.ok) {
        error = result.error.empty() ? "the run did not finish" : "the run did not finish: " + result.error;
        return false;
    }
    SceneProject* sceneProject = project->getScene(static_cast<uint32_t>(result.sceneId));
    if (!sceneProject || !sceneProject->scene) {
        error = "scene " + std::to_string(result.sceneId) + " is no longer open";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        error = "the scene is in Play mode: stop it first";
        return false;
    }
    if (placementSceneBusy(project, result.sceneId)) {
        error = "a physics gesture is running in the scene: wait for it to finish, then run it again";
        return false;
    }
    const std::string changed = "the scene changed during the simulation: run it again";
    if (sceneProject->structureVersion != result.sceneStructureVersion) {
        error = changed;
        return false;
    }
    Scene* scene = sceneProject->scene;
    std::vector<CommitEntry> entries;
    for (const PlacementEntityResult& e : result.entities) {
        Transform* transform = scene->findComponent<Transform>(e.entity);
        if (!entityAlive(scene, e.entity) || !transform) {
            error = (e.path.empty() ? e.name : e.path) + " is gone (deleted since the run): run it again";
            return false;
        }
        // the result is about the pose the run started from
        if (transform->parent != e.beforeParent || transform->position != e.beforeLocalPosition ||
            transform->rotation != e.beforeLocalRotation || transform->scale != e.beforeLocalScale) {
            error = changed;
            return false;
        }
        CommitEntry entry;
        entry.entity = e.entity;
        entry.before = LocalPose{transform->position, transform->rotation, transform->scale};
        entry.after = LocalPose{e.afterLocalPosition, e.afterLocalRotation, transform->scale};
        entries.push_back(entry);
    }
    // anything else the run read (the floor it landed on, ...)
    if (placementSceneEditStamp(project, result.sceneId) != result.sceneEditStamp) {
        error = changed;
        return false;
    }
    applyCommit(project, static_cast<uint32_t>(result.sceneId), entries);
    return true;
}

json editor::placementResultJson(Project* project, const PlacementResult& result, bool applied) {
    json out = json::object();
    out["applied"] = applied;
    out["mode"] = placementModeName(result.mode);
    out["seed"] = result.seed;
    out["state_hash"] = result.stateHash;
    out["sim_seconds"] = r5(result.simSeconds);
    out["steps"] = result.steps;
    out["all_settled"] = result.allSettled;
    if (!result.ok) {
        out["error"] = result.error;
        if (result.cancelled) out["cancelled"] = true;
    }

    json entities = json::array();
    for (const PlacementEntityResult& e : result.entities) {
        json entity = json::object();
        entity["id"] = e.entity;
        entity["name"] = e.name;
        entity["path"] = e.path;
        entity["before"] = {{"position", vecJson(e.before.position)}, {"rotation", quatJson(e.before.rotation)}};
        entity["after"] = {{"position", vecJson(e.after.position)},
                           {"rotation", quatJson(e.after.rotation)},
                           {"local", {{"position", vecJson(e.afterLocalPosition)}, {"rotation", quatJson(e.afterLocalRotation)}}}};
        entity["moved_m"] = r5(e.movedMeters);
        entity["rotated_deg"] = r5(e.rotatedDegrees);
        entity["settled"] = e.settled;
        entity["settle_step"] = e.settleStep;
        json resting = json::array();
        for (size_t i = 0; i < e.restingOn.size(); i++) {
            resting.push_back({{"id", e.restingOn[i]}, {"path", i < e.restingOnPaths.size() ? e.restingOnPaths[i] : std::string()}});
        }
        entity["resting_on"] = resting;
        entity["max_penetration_m"] = r5(e.maxPenetration);
        entity["collider"] = e.collider;
        entity["warnings"] = e.warnings;
        entities.push_back(entity);
    }
    out["entities"] = entities;

    json region = nullptr;
    if (!result.contextRegion.isNull() && !result.contextRegion.isInfinite()) {
        region = {{"min", vecJson(result.contextRegion.getMinimum())}, {"max", vecJson(result.contextRegion.getMaximum())}};
    }
    out["context"] = {{"bodies", result.contextBodies}, {"region", region}};

    json refused = json::array();
    for (const PlacementRefusal& refusal : result.refused) {
        std::string path;
        if (project) {
            if (const SceneProject* sp = project->getScene(static_cast<uint32_t>(result.sceneId))) {
                if (sp->scene && sp->scene->isEntityCreated(refusal.entity)) path = EntityPath::of(*sp, refusal.entity);
            }
        }
        refused.push_back({{"path", path.empty() ? std::to_string(refusal.entity) : path}, {"reason", refusal.reason}});
    }
    out["refused"] = refused;
    out["warnings"] = result.warnings;
    out["hint"] = applied ? placementHint(result, true) : (result.hint.empty() ? placementHint(result, false) : result.hint);
    return out;
}
