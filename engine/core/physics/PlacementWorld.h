// SPDX-License-Identifier: MIT
//
// PlacementWorld: a private, throwaway Jolt world for edit-time physics placement (Physics
// Placement, design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md
// sections 5.2 and 6).
//
// It reads a Scene (world transforms, meshes, terrains, authored colliders) and never writes to
// it: the caller reads poses back and decides what to commit. Every JPH:: call stays inside
// libjasperengine (Jolt is a static library linked into the shared engine; editor code calling
// Jolt would use a second copy), so this header has no Jolt types.
//
// Movers are dynamic bodies, one per top-most root, whose shape is the compound of the
// subtree's colliders or meshes with world scale baked in. Context is static: the scene around
// the movers, gathered lazily by world AABB. Neighbours (pushNeighbours) are extra dynamic
// bodies for movable unselected objects inside the context region.
//
// Not thread-safe: create, drive and destroy it on one thread (the editor main thread, or the
// CLI thread). Several worlds may exist at once.

#pragma once

#include "Export.h"
#include "Entity.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vector3.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    class ConvexDecompositionCache;
    struct DecompositionParams;

    // Which colliders a body is built from (Grabbit's four rules, renamed, plus AUTO).
    enum class PlacementColliderPolicy {
        AUTO,       // authored colliders if any; else a fitted primitive; else decomposition (if
                    // cached) or convex hull for movers / triangle mesh for context
        AUTHORED,   // authored colliders only; a mover without any is refused
        GENERATED,  // ignore authored colliders
        BOTH        // authored plus generated
    };

    enum class PlacementShapeKind {
        NONE,
        AUTHORED,
        BOX,
        SPHERE,
        CAPSULE,
        CONVEX_HULL,
        DECOMPOSED,
        MESH,
        HEIGHTFIELD
    };

    JASPERENGINE_API const char* placementShapeKindName(PlacementShapeKind kind);

    enum class PlacementDecomposition {
        NEVER,      // movers use hulls / primitives only
        CACHED,     // use a decomposition when the cache already has it, request one otherwise
        ALWAYS      // like CACHED; the caller waits for pending requests before stepping
    };

    struct PlacementPose {
        Vector3 position;     // world space
        Quaternion rotation;  // world space
    };

    // Feel of the target-tracking control law and solver (spec section 6).
    struct PlacementTuning {
        float maxSpeed = 10.0f;           // m/s, any mover
        float contactSpeed = 1.0f;        // m/s, moving into a contact
        float maxAngularSpeed = 10.0f;    // rad/s
        float teleportDistance = 5.0f;    // m; a target farther than this teleports the mover
        int substeps = 1;                 // collision steps per fixed step
        int velocityIterations = 8;
        int positionIterations = 2;
        float sleepLinear = 0.02f;        // m/s
        float sleepAngularDeg = 2.0f;     // deg/s
        float sleepSeconds = 0.25f;       // below both thresholds this long = asleep
    };

    struct PlacementWorldSettings {
        PlacementTuning tuning;
        float fixedStep = 1.0f / 120.0f;  // seconds per step()
        PlacementColliderPolicy colliders = PlacementColliderPolicy::AUTO;
        PlacementDecomposition decomposition = PlacementDecomposition::CACHED;
        ConvexDecompositionCache* decompositionCache = nullptr;  // not owned; null = no decomposition
        uint32_t decompositionMaxHulls = 8;                       // DecompositionParams for requests
        uint32_t decompositionResolution = 400000;
        uint32_t decompositionMaxVerticesPerHull = 64;
        float maxMoverSize = 25.0f;       // m, longest world AABB axis; larger roots are refused
        uint32_t ignoreLayerMask = 0;     // bit i = layer i ignored (neither mover nor context)
        bool pushNeighbours = false;      // movable unselected objects in the region become dynamic
        float killY = -std::numeric_limits<float>::infinity();  // dynamic bodies below freeze (fellOut)
        float meshSimplifyAbove = 20000;  // context triangle meshes above this many triangles are simplified
    };

    struct PlacementBodyState {
        Entity entity = NULL_ENTITY;
        bool mover = false;               // false: a pushNeighbours neighbour
        PlacementPose pose;
        Vector3 linearVelocity;
        Vector3 angularVelocity;          // rad/s
        bool asleep = false;
        bool fellOut = false;             // went below killY and was frozen
        bool teleported = false;          // teleported at least once since the last clearFlags()
        bool touchingContext = false;
        Vector3 averageContactNormal;     // unit, pointing away from the context into the body; zero when none
        std::vector<Entity> restingOn;    // context or dynamic bodies it touches from above, stable order
        uint32_t contacts = 0;            // bodies it touches
        float maxPenetration = 0.0f;      // m, deepest current contact
        PlacementShapeKind shape = PlacementShapeKind::NONE;
        int hullCount = 0;                // DECOMPOSED: hull count; CONVEX_HULL: 1
        bool decompositionPending = false;
    };

    struct PlacementRayHit {
        Entity entity = NULL_ENTITY;      // context entity or mover root
        Vector3 point;
        Vector3 normal;
        float distance = 0.0f;
    };

    class JASPERENGINE_API PlacementWorld {
    public:
        PlacementWorld(Scene* scene, const PlacementWorldSettings& settings);
        ~PlacementWorld();

        PlacementWorld(const PlacementWorld&) = delete;
        PlacementWorld& operator=(const PlacementWorld&) = delete;

        Scene* getScene() const;

        // Applies live (tuning, killY, pushNeighbours for later ensureContext calls). Changing
        // colliders or decomposition only affects bodies added afterwards or refreshShapes().
        void setSettings(const PlacementWorldSettings& settings);
        const PlacementWorldSettings& getSettings() const;

        // ---- Bodies -------------------------------------------------------------------------

        // Adds a dynamic mover for root's subtree at its current world pose. False (and *why)
        // when refused: no mesh or collider, larger than maxMoverSize, ignored layer, inactive,
        // mesh still loading, already a mover, a descendant or ancestor of a mover. If the root
        // was context it stops being context.
        // True with a non-empty *why when accepted with a caveat the caller should pass on (a
        // skinned mesh simulates in its bind pose; joints are not recreated); else *why is cleared.
        bool addMover(Entity root, std::string* why = nullptr);
        void removeMover(Entity root);
        bool isMover(Entity root) const;
        const std::vector<Entity>& getMovers() const;   // insertion order

        // Adds static context for every entity whose world AABB intersects region, except movers
        // and their subtrees, ignored layers and inactive entities. Incremental: entities already
        // in the world are skipped, so calling it every frame with a growing region is cheap.
        // With pushNeighbours, movable entities (with a Rigidbody, or no larger than 2 m and not
        // static-flagged) become dynamic neighbours instead.
        void ensureContext(const AABB& region);
        // Adds one entity (and its subtree) as static context regardless of region.
        void addContextEntity(Entity entity);
        size_t getContextBodyCount() const;
        const std::vector<Entity>& getNeighbours() const;
        AABB getContextRegion() const;   // union of every region passed to ensureContext

        // Rebuilds every mover's (and neighbour's) shape: a decomposition that finished since is
        // picked up. Poses and velocities are kept.
        void refreshShapes();
        bool hasPendingDecompositions() const;

        // ---- Baking (the editor's Bake selection / Bake scene) -------------------------------

        // Requests the convex decomposition of every generated mesh part of root's subtree, as
        // addMover would build the mover with these settings (the same walk, geometry and cache
        // keys), without the concavity test (as PlacementDecomposition::ALWAYS). Parts a
        // primitive fits and parts already cached are not requested. Returns the parts requested
        // or already cached; 0 without settings.decompositionCache or when root can't be a mover.
        // notes (optional) gets a line per part left out and the reason root was refused.
        static size_t requestDecompositions(Scene* scene, Entity root, const PlacementWorldSettings& settings,
                                            std::vector<std::string>* notes = nullptr);
        // Every part requestDecompositions would ask for is cached (READY); true when no part
        // needs one, false without a cache or when root can't be a mover.
        static bool decompositionReady(Scene* scene, Entity root, const PlacementWorldSettings& settings);

        // ---- Driving ------------------------------------------------------------------------

        void setGravity(const Vector3& gravity);
        Vector3 getGravity() const;

        // Target tracking (spec section 6): each step the mover is driven toward the pose,
        // speed-capped, slowed only when moving into a contact, teleported past teleportDistance.
        void setTarget(Entity mover, const PlacementPose& target);
        void clearTarget(Entity mover);
        void clearTargets();
        bool hasTarget(Entity mover) const;

        // Moves a dynamic body (mover or neighbour) instantly, velocities zeroed.
        void teleport(Entity body, const PlacementPose& pose);
        // Zeroes every dynamic body's velocities.
        void stopAll();
        // Extra damping for every dynamic body (Separate uses high damping); 0 = Jolt defaults.
        void setDamping(float linear, float angular);
        // Penetration recovery speed multiplier for Separate (1 = default).
        void setSeparationBoost(float factor);
        void wakeAll();
        void clearFlags();   // resets teleported

        // One fixed step of settings.fixedStep seconds.
        void step();
        uint32_t getStepCount() const;
        double getSimulatedSeconds() const;

        // Every dynamic body (movers and neighbours) asleep or fallen out.
        bool allAsleep() const;
        // Deepest current penetration between any two bodies, m.
        float getMaxPenetration() const;

        // ---- Reading back -------------------------------------------------------------------

        PlacementBodyState getState(Entity body) const;
        // Movers in insertion order, then neighbours in insertion order.
        std::vector<PlacementBodyState> getStates() const;
        AABB getMoversBounds() const;   // world AABB of every mover's shape

        // Triangle-accurate ray against context and dynamic bodies, ignoring the listed entities'
        // bodies (pass mover roots to hit only what lies behind them). The ray's direction length
        // is the max distance.
        bool raycast(const Ray& ray, PlacementRayHit& hit, const std::vector<Entity>& ignore = {}) const;

        // Hash of every dynamic body's pose and velocity, quantised to 1e-5. Same scene, same
        // calls, same binary: same hash.
        uint64_t stateHash() const;

        // Debug drawing for the editor: world-space line segments of every shape (pairs of
        // points), movers first. maxSegments caps the output.
        void collectShapeLines(std::vector<Vector3>& outSegments, bool moversOnly, size_t maxSegments) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
