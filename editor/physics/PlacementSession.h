// SPDX-License-Identifier: MIT
//
// Physics Placement session (design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md
// sections 5.4, 6, 7 and 8). One simulation core for three front ends:
//
//   - PlacementSession: the interactive Scene-view tool. One per scene while the Physics toggle
//     (G) is on. Gestures (drag, Space hold, Shift-drag) preview poses live in the scene's
//     Transforms and commit one undo step each; Esc cancels and restores the pre-gesture poses.
//   - PlacementRun: a batch run for the place_with_physics MCP tool and the place-physics CLI.
//     Never touches the scene; commitPlacement() applies a result as one undo step.
//
// Everything runs on the editor main thread (or the CLI thread). The physics lives in a private
// PlacementWorld; the scene's own paused PhysicsSystem is never stepped.

#pragma once

#include "PlacementSettings.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Ray.h"
#include "math/Vector3.h"
#include "physics/PlacementWorld.h"

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // ---- Shared ------------------------------------------------------------------------------

    struct PlacementRefusal {
        Entity entity = NULL_ENTITY;
        std::string reason;   // "no mesh or collider", "too large (> 25 m), treated as environment", ...
    };

    // The movers of a selection: top-most selected roots (a selected descendant of a selected
    // entity is dropped), in selection order.
    std::vector<Entity> placementRoots(Project* project, size_t sceneId, const std::vector<Entity>& selection);

    // The project's decomposition cache (<project>/.jasperengine/cache/colliders), created on first
    // use; null without an open project. Reset on project switch.
    ConvexDecompositionCache* placementDecompositionCache(Project* project);
    void placementProjectSwitching();   // cancels pending decompositions, drops the cache

    // A cheap stamp of the scene's edit state: its structureVersion, its command history position
    // and a hash of every GameObject's Transform, hierarchy, components, layer and active flag.
    // Equal stamps: nothing a placement reads was edited in between. 0 for an unknown scene.
    uint64_t placementSceneEditStamp(Project* project, size_t sceneId);
    // An interactive PlacementSession is running a gesture or settle on the scene: its Transforms
    // hold preview poses (wait, or refuse, before reading or editing them).
    bool placementSceneBusy(Project* project, size_t sceneId);

    // ---- Interactive -------------------------------------------------------------------------

    enum class PlacementGesture {
        None,
        Move,          // W gizmo: the selection follows the handle
        Rotate,        // E gizmo: rotates about the pivot (settings.rotatePivot)
        GatherSpread,  // R gizmo: per-axis scale in [0, 2], <1 gathers, >1 spreads
        Drop,          // hold Space: gravity on the selection
        Snap,          // Shift + LMB drag: rides the surface under the cursor
        Distribute,    // panel button: equal spacing along an axis, then settle
        Gather         // panel button: pulled toward the centre, then settle
    };

    enum class PlacementPhase {
        Idle,          // no gesture
        Active,        // gesture held (mouse or Space down)
        Settling,      // released, falling until resting (settleOnRelease or Distribute/Gather)
    };

    struct PlacementHud {
        PlacementPhase phase = PlacementPhase::Idle;
        PlacementGesture gesture = PlacementGesture::None;
        size_t movers = 0;
        size_t contextBodies = 0;
        size_t pendingDecompositions = 0;
        std::string status;              // "Ready", "Simulating", "Settling…", "Resting", "Refining colliders 2/5"
        std::vector<PlacementRefusal> refused;   // for the current selection
        // Movers accepted with a caveat ("simulated in its bind pose", "simulated free: joints are
        // not recreated"), for the current selection or gesture
        std::vector<PlacementRefusal> caveats;
    };

    // World-space data the Scene view draws (SceneRender overlay lines and labels).
    struct PlacementOverlay {
        bool active = false;                     // a gesture or settle is running
        AABB region;                             // context region (only meaningful when active)
        std::vector<Vector3> contactPoints;
        std::vector<bool> contactSlowing;        // parallel to contactPoints: the mover is being slowed by it
        bool showTeleportRing = false;
        Vector3 teleportCenter;
        float teleportRadius = 0.0f;
        bool teleportExceeded = false;           // the handle is past the ring
        struct Ghost { Entity entity = NULL_ENTITY; Vector3 position; Quaternion rotation; };
        std::vector<Ghost> ghosts;               // pre-gesture poses of the movers
        std::vector<PlacementRefusal> refused;   // labels drawn at each entity
        std::vector<Vector3> proxySegments;      // pairs; settings.showProxies
        struct Distance { Vector3 from; Vector3 to; float meters = 0.0f; int axis = 0; };
        std::vector<Distance> distances;         // settings.showDistanceIndicators (idle selection only)
    };

    class PlacementSession {
    public:
        PlacementSession(Project* project, size_t sceneId);
        ~PlacementSession();   // cancels a running gesture (restores poses)

        PlacementSession(const PlacementSession&) = delete;
        PlacementSession& operator=(const PlacementSession&) = delete;

        size_t getSceneId() const;

        // Live: tuning applies to the running gesture.
        void setSettings(const PhysicsPlacementSettings& settings);
        const PhysicsPlacementSettings& getSettings() const;

        // Starts a gesture on the selection's top-most roots. False when nothing can move (the HUD
        // lists the refusals). A gesture already running is ended first (committed).
        bool begin(PlacementGesture gesture, const std::vector<Entity>& selection);
        bool isGestureActive() const;          // phase Active
        bool isBusy() const;                   // phase Active or Settling
        PlacementGesture getGesture() const;
        PlacementPhase getPhase() const;

        // Gesture input (world space). Move: where the selection centre should go, plus the
        // gizmo's rotation (identity unless Control Rotation). Rotate: the gizmo's rotation since
        // the gesture began. GatherSpread: the scale handle's per-axis value. Snap: the mouse ray.
        void setMoveTarget(const Vector3& center);
        void setRotateDelta(const Quaternion& delta);
        void setGatherScale(const Vector3& scale);
        void setSnapRay(const Ray& ray);
        // Drop with "toward the surface": the mouse ray picks the gravity direction.
        void setPointerRay(const Ray& ray);

        // Release: Move/Rotate/GatherSpread/Snap settle first when settleOnRelease, Drop commits
        // right away. Commit = one undo step (ObjectTransformCmd for every moved entity).
        void end();
        // Esc / Ctrl+Z during a gesture or settle: restores the pre-gesture poses, no undo step.
        void cancel();

        // Panel buttons: run as a gesture that begins and ends at once (then settles).
        bool distribute(const std::vector<Entity>& selection, int axis);   // 0 x, 1 y, 2 z
        bool gather(const std::vector<Entity>& selection);

        // Every editor frame: steps the world by wall-clock time (fixed steps, at most ~8 ms of
        // stepping per frame), writes preview poses into the scene, finishes a settle (commit) and
        // swaps in finished decompositions. Requests a redraw while busy.
        void tick(double deltaSeconds);

        // The scene's structure changed (entities added, deleted, reparented, components edited):
        // the private world is rebuilt before the next gesture.
        void notifySceneChanged();

        // The selection changed while idle: recomputes refusals and distance indicators.
        void setSelection(const std::vector<Entity>& selection);

        PlacementHud getHud() const;
        void collectOverlay(PlacementOverlay& out) const;

        // The centre the gizmo should sit at during a gesture (the movers' current centroid).
        Vector3 getMoversCenter() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // ---- Batch (MCP place_with_physics, CLI place-physics) ------------------------------------

    enum class PlacementMode {
        Drop,          // gravity until resting
        Separate,      // no gravity: push overlapping objects apart
        Move,          // toward target (rigid group), stops at contacts, then settle
        Snap,          // onto the surface hit by a ray, then settle
        Gather,        // toward the centre by amount (0..1), then settle
        Distribute     // equal spacing along axis, then settle
    };

    const char* placementModeName(PlacementMode mode);   // "drop", "separate", "move", "snap", "gather", "distribute"
    bool parsePlacementMode(const char* name, PlacementMode& out);

    struct PlacementSpec {
        size_t sceneId = 0;
        std::vector<Entity> entities;          // movers (placementRoots applied)
        PlacementMode mode = PlacementMode::Drop;

        // Move
        bool hasTargetPosition = false;
        Vector3 targetPosition;                // the group centre's target (world, or local to the
        bool hasTargetRotation = false;        //   parent of the first mover in EntityPath order,
                                               //   the run's own order, when targetLocal)
        Quaternion targetRotation;             // rotation applied to the group about its centre
        bool hasTargetDelta = false;
        Vector3 targetDelta;
        bool targetLocal = false;

        // Snap
        Ray snapRay;
        bool hasSnapRay = false;

        int axis = 0;                          // Distribute
        float amount = 1.0f;                   // Gather, 0..1

        Vector3 gravity = Vector3(0.0f, -9.81f, 0.0f);
        bool settle = true;                    // drop/move/snap/gather/distribute: run until resting after the gesture

        enum class Context { Auto, Entities, None } context = Context::Auto;
        std::vector<Entity> contextEntities;   // Context::Entities
        float contextMargin = 2.0f;

        struct Jitter { float position = 0.0f; float rotationDeg = 0.0f; } jitter;
        uint32_t seed = 1;
        float maxSeconds = 10.0f;              // simulated seconds

        PhysicsPlacementSettings settings;     // feel, colliders, decomposition, quality, push neighbours, layers, ...

        // A stable fingerprint of every field above (used to check apply == preview).
        std::string fingerprint() const;
    };

    struct PlacementEntityResult {
        Entity entity = NULL_ENTITY;
        std::string name;
        std::string path;                      // EntityPath
        bool mover = true;                     // false: a pushed neighbour
        PlacementPose before;                  // world
        Vector3 beforeLocalPosition;           // the Transform when the run started (commitPlacement
        Quaternion beforeLocalRotation;        //   refuses when it is not what the scene holds now)
        Vector3 beforeLocalScale = Vector3(1.0f, 1.0f, 1.0f);
        Entity beforeParent = NULL_ENTITY;
        PlacementPose after;                   // world
        Vector3 afterLocalPosition;            // parent space, what is committed
        Quaternion afterLocalRotation;
        float movedMeters = 0.0f;
        float rotatedDegrees = 0.0f;
        bool settled = false;
        uint32_t settleStep = 0;               // the step it fell asleep (0 = never)
        std::vector<Entity> restingOn;
        std::vector<std::string> restingOnPaths;
        float maxPenetration = 0.0f;
        std::string collider;                  // "convex_hull", "decomposed(6)", "box", "authored", ...
        bool fellOut = false;
        bool teleported = false;
        std::vector<std::string> warnings;
    };

    struct PlacementResult {
        bool ok = false;                       // false: see error
        std::string error;
        PlacementMode mode = PlacementMode::Drop;
        size_t sceneId = 0;
        uint64_t sceneStructureVersion = 0;    // the scene's structure when the run started
        uint64_t sceneEditStamp = 0;           // placementSceneEditStamp when the run started
        std::string specFingerprint;
        uint32_t seed = 1;
        std::string stateHash;                 // hex
        double simSeconds = 0.0;
        uint32_t steps = 0;
        bool allSettled = false;
        bool cancelled = false;
        std::vector<PlacementEntityResult> entities;
        size_t contextBodies = 0;
        AABB contextRegion;
        std::vector<PlacementRefusal> refused;
        std::vector<std::string> warnings;
        std::string hint;
    };

    // Fix-it problems with a spec (unknown scene, empty entities, move without a target, ...).
    std::vector<std::string> checkPlacementSpec(Project* project, const PlacementSpec& spec);

    // A batch run, advanced in bounded slices so the editor stays responsive (MCP) or straight
    // through (CLI: advance(0) runs to the end). Deterministic: same scene, spec and binary give
    // the same result and stateHash.
    class PlacementRun {
    public:
        PlacementRun();
        ~PlacementRun();

        PlacementRun(const PlacementRun&) = delete;
        PlacementRun& operator=(const PlacementRun&) = delete;

        // Builds the world. False and result().error on a bad spec.
        bool start(Project* project, const PlacementSpec& spec);
        // Steps for at most budgetMs of wall time (0 = until done). True when done. The scene is
        // only read on the calling (main) thread, so it can only be edited between two calls:
        // each call first checks placementSceneEditStamp against the start's and fails the run
        // ("the scene changed during the simulation: run it again") when it moved.
        bool advance(double budgetMs);
        bool isDone() const;
        void cancel();                          // done with cancelled = true
        float progress() const;                 // 0..1
        std::string progressText() const;       // "step 140/1200, 22 of 30 asleep"
        const PlacementResult& result() const;  // complete once isDone()

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // Applies a finished result as one undo step in the scene's history (ObjectTransformCmd for
    // every entity, prefab overrides recorded only for what moved: position, rotation and scale
    // compared one by one). Fails with "the scene changed during the simulation: run it again"
    // when the scene's edit stamp or structure changed since the run started, or an entity's
    // Transform or parent is no longer what the run started from; fails when an entity is gone
    // or a physics gesture is running in the scene.
    bool commitPlacement(Project* project, const PlacementResult& result, std::string& error);

    // The structured result shared by MCP and the CLI (spec section 8.1). Values rounded to 1e-5.
    nlohmann::json placementResultJson(Project* project, const PlacementResult& result, bool applied);

}
