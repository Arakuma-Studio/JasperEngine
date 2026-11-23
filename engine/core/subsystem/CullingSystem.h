// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene's Culling Groups and Simulation Culling (docs/specs/culling-group-and-simulation-
// distance.md D1-D13).
// - Registered after PhysicsSystem: its update runs after Render made this frame's world
//   transforms and camera planes; its load comes after the bodies are made.
// - Frame N's update decides: the groups' states and onStateChanged, the components' states,
//   the flags the scripts' guard, the Animator and the bodies read from then on, and the
//   onSimulationCulled / onSimulationResumed messages. The fixed steps of frame N+1 see the frozen
//   bodies (one frame of latency, as Unity's CullingGroup).
// - preUpdate, at the start of every frame before the scripts' starts and the fixed steps: the
//   first evaluation of the scene (nothing over unloaded ground simulates for a frame) and the
//   GameObjects with a Simulation Culling made since the last update (instantiated ones).
// - Edit mode pauses it (nothing is evaluated); a scene without a group or a component returns
//   after one check, allocates nothing, and its scripts' guard costs one relaxed load.
// - Groups of more than 4,096 spheres are evaluated in chunks of 1,024 on S06's parallelFor,
//   their events merged in index order (inline on single-threaded web).

#ifndef CULLING_SYSTEM_H
#define CULLING_SYSTEM_H

#include "Export.h"
#include "ecs/SubSystem.h"
#include "math/Plane.h"
#include "math/Vector3.h"
#include "object/CullingGroup.h"
#include "component/SimulationCullingComponent.h"

#include <array>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

namespace jasperengine{

    class LineRenderer;
    class TerrainWorldSystem;
    template <typename Ret> class LuaFunction;

    class JASPERENGINE_API CullingSystem : public SubSystem {
    public:
        struct Stats {
            uint32_t groups = 0;
            uint32_t spheres = 0;
            uint32_t visible = 0;
            // spheres per band (0 nearest .. MAX_DISTANCES), of the enabled groups
            std::array<uint32_t, CullingGroup::MAX_DISTANCES + 1> perBand{};
            double evaluateMs = 0.0;
            uint32_t components = 0;
            uint32_t full = 0;
            uint32_t reduced = 0;
            uint32_t culled = 0;
            uint32_t groundNotLoaded = 0;
            uint32_t focuses = 0;
            // update callbacks the guard skipped in the last frame
            uint32_t scriptUpdatesSkipped = 0;
            uint32_t transitionsDeferred = 0;
        };

        // non-ground transitions applied in one frame at most (R18)
        static constexpr uint32_t MAX_TRANSITIONS_PER_FRAME = 64;
        // script ground checks in one frame at most (D12)
        static constexpr uint32_t MAX_GROUND_CHECKS_PER_FRAME = 256;
        static constexpr uint32_t PARALLEL_THRESHOLD = 4096;
        static constexpr uint32_t PARALLEL_CHUNK = 256;

        explicit CullingSystem(Scene* scene);
        ~CullingSystem() override;

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // the frame's start (Engine::runFrame)
        void preUpdate();
        // the scene's system while it has a Simulation Culling component (null otherwise)
        static CullingSystem* activeFor(Scene* scene);
        // the whole evaluation now, every transition applied (tests, the start)
        void evaluateNow();

        Stats getStats() const;
        // the groups' storage (R24): spheres, followed spheres and their event buffers
        size_t getMemoryBytes() const;
        // spheres evaluated since the scene started (a scene without groups keeps 0)
        uint64_t getEvaluatedSpheres() const { return evaluatedSpheres; }
        // tests: groups above PARALLEL_THRESHOLD on parallelFor (default) or inline
        void setParallelEvaluation(bool parallel) { parallelEvaluation = parallel; }

        // Engine::clearComponentSubscriptions (Stop) and Scene::removeSubscriptionsByTag: the
        // groups' onStateChanged callbacks and the ground check go while their code is still
        // loaded; at Stop the groups, focuses and frozen state of the session go too
        void clearSubscriptions();
        void removeSubscriptionsByTag(const std::string& substring);

        // the debug drawing of every scene (Game view Gizmos > Culling, configure_game_view
        // culling_debug): the components' and groups' spheres and the focuses as crosses
        static void setDebugDraw(bool draw);
        static bool isDebugDraw();
        // lines drawn by the last update (0 off)
        size_t getDebugLineCount() const;

        // ---- groups (CullingGroup forwards here)
        struct Follow {
            uint32_t index;
            Entity entity;
            Vector3 offset;
        };
        struct GroupSlot {
            uint32_t generation = 1;
            bool alive = false;
            bool enabled = true;
            bool followWarned = false;
            uint64_t owner = 0;
            // the spheres (structure of arrays) and their state: bits 0-7 the state (bit 7
            // visible, bits 0-3 the band, 0xF unknown), above them the follow slot + 1
            std::vector<float> x, y, z, r;
            std::vector<uint32_t> meta;
            std::vector<Follow> follows;
            std::array<float, CullingGroup::MAX_DISTANCES> distances{};
            uint8_t distanceCount = 0;
            float hysteresis = 0.0f;
            Entity targetCamera = NULL_ENTITY;
            bool hasReferencePoint = false;
            Vector3 referencePoint;
            Entity referenceEntity = NULL_ENTITY;
            std::unique_ptr<FunctionSubscribe<void(CullingGroupEvent)>> onStateChanged;
            // Lua's onStateChanged: five values a call, no table
            std::shared_ptr<LuaFunction<void>> luaCallback;
            std::vector<CullingGroupEvent> events;
            std::vector<std::vector<CullingGroupEvent>> chunkEvents;

            template<typename F> void forEachEvent(F&& f) { if (onStateChanged) f(*onStateChanged); }
        };

        CullingGroup createGroup();
        // null for a disposed or stale handle
        GroupSlot* findGroup(uint32_t slot, uint32_t generation);
        const GroupSlot* findGroup(uint32_t slot, uint32_t generation) const;
        void disposeGroup(uint32_t slot, uint32_t generation);
        // the group's owner: a Lua script instance (SubscriptionOwnerIndex key) releases it
        void setGroupOwner(uint32_t slot, uint32_t generation, uint64_t ownerKey);
        void setLuaCallback(uint32_t slot, uint32_t generation, std::shared_ptr<LuaFunction<void>> callback);
        bool hasLuaCallback(uint32_t slot, uint32_t generation) const;

        // ---- Simulation Culling (SimulationCulling forwards here)
        int addFocus(const Vector3& position, Entity entity);
        bool setFocus(int id, const Vector3& position);
        bool removeFocus(int id);
        void setCameraIsFocus(bool cameraIsFocus);
        bool isCameraFocus() const { return cameraIsFocus; }
        void setGroundCheck(std::function<bool(const Vector3&, float)> check);

        // the guard's reads (D7): bits of the entity's flags
        enum SimFlag : uint8_t {
            SimScriptsCulled = 1u << 0,
            SimReduced = 1u << 1,
            SimAnimatorCulled = 1u << 2,
            SimPhysicsFrozen = 1u << 3
        };
        uint8_t getSimFlags(Entity entity) const { return entity < simFlags.size() ? simFlags[entity] : 0; }
        SimulationGate gate(Entity entity, int family, double& reducedDelta);
        bool reducedDelta(Entity entity, double& delta) const;

    private:
        struct ScriptFocus {
            int id = 0;
            Vector3 position;
            Entity entity = NULL_ENTITY;
        };
        struct CameraPlanes {
            float nx[6], ny[6], nz[6], d[6];
            bool skipFar;
            // enabled, active, a game camera with planes Render made
            bool usable;
            Entity entity;
        };
        struct ReducedTiming {
            uint8_t interval = 4;
            uint64_t frame = UINT64_MAX;
            double lastTime = 0.0;
            double delta = 0.0;
        };
        struct Transition {
            Entity entity;
            SimulationState state;
            SimulationCullReason reason;
            float distance;
            // never deferred: ground culls, reason changes, disabled components
            bool now;
        };
        struct Message {
            Entity entity;
            bool culled;
            SimulationCullReason reason;
        };

        std::vector<GroupSlot> groups;
        uint32_t liveGroups = 0;
        bool parallelEvaluation = true;
        uint64_t evaluatedSpheres = 0;

        // per entity id, sized when the first component appears
        std::vector<uint8_t> simFlags;
        std::vector<ReducedTiming> reducedTimings;
        std::vector<ScriptFocus> scriptFocuses;
        int nextFocusId = 1;
        bool cameraIsFocus = true;
        // held by a shared_ptr: a check that replaces or clears itself keeps running on its copy
        std::shared_ptr<std::function<bool(const Vector3&, float)>> groundCheck;
        std::vector<Entity> pendingNew;
        // GameObjects whose component went while it held flags
        std::vector<Entity> removedComponents;
        std::vector<Transition> transitions;
        std::vector<Message> messages;
        std::vector<Vector3> focusPoints;
        std::vector<Entity> focusEntities;
        std::vector<CameraPlanes> cameraPlanes;
        bool registeredActive = false;
        bool started = false;
        uint64_t frameCounter = 0;
        uint32_t groundCursor = 0;
        uint32_t skippedThisFrame = 0;
        Stats stats;

        Entity debugLinesEntity = NULL_ENTITY;
        bool debugLinesShown = false;

        bool hasWork() const;
        void start();
        void refreshActive();
        void gatherCameras();
        bool sphereVisible(const Vector3& center, float radius, Entity targetCamera, bool anyCamera) const;
        Vector3 worldPositionOf(Entity entity) const;
        Vector3 referenceOf(const GroupSlot& group) const;

        void evaluateGroups();
        void evaluateGroup(uint32_t slot, GroupSlot& group);
        // the followed spheres where their GameObjects are now; a destroyed one stops following
        void updateFollows(GroupSlot& group);
        // `gone` non-null: the range reads its followed spheres' positions first, and a GameObject
        // without a Transform sets it (updateFollows then drops it)
        void evaluateRange(GroupSlot& group, uint32_t begin, uint32_t end, const Vector3& reference, Entity camera, bool anyCamera,
                           std::vector<CullingGroupEvent>& out, std::atomic<bool>* gone = nullptr);
        void dispatchGroupEvents(uint32_t slot);

        void gatherFocuses();
        void ensureSimFlags(Entity entity);
        float resolveRadius(Entity entity, SimulationCullingComponent& component);
        // `all`: a first evaluation, never capped
        // `worlds`: the scene's TerrainWorldSystem when it has worlds, found once a pass (null otherwise)
        bool groundLoaded(TerrainWorldSystem* worlds, const Vector3& position, float radius, bool withinDistance, bool all, uint32_t& scriptChecks);
        void evaluateSimulation(bool all, const std::vector<Entity>* only);
        void applyTransition(const Transition& transition);
        void applyFlags(Entity root, const SimulationCullingComponent& component, SimulationState state);
        void setEntityFlags(Entity entity, uint8_t flags, uint8_t interval, bool physics);
        void sendMessages();
        void flushRemoved();

        void updateDebugLines();
    };

}

#endif //CULLING_SYSTEM_H
