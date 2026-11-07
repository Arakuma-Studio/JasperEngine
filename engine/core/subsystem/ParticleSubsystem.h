// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLE_SUBSYSTEM_H
#define PARTICLE_SUBSYSTEM_H

// Runs a scene's particle systems (docs/specs/S20-particles.md D2, D12, R6-R8, R14-R16): each
// frame it gathers the force fields, binds the effects (assets and inline), decides culling
// from what the cameras saw last frame, hands every emitter to ParticleWorld (the budget, the
// ticks of all of them in parallel jobs), then delivers the frame's collision, death and
// sub-emitter events to the listeners on the main thread. RenderSystem draws them.

#include "SubSystem.h"

#include "component/ForceFieldComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/TerrainComponent.h"
#include "math/Matrix4.h"
#include "particles/ParticleWorld.h"
#include "subsystem/ParticleSystemState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    struct Transform;

    // what get_particle_stats reports of one system
    struct ParticleSystemStats{
        bool valid = false;
        uint32_t live = 0;
        uint32_t capacity = 0;
        uint32_t budget = 0;
        float emissionScale = 1.0f;
        uint32_t ticks = 0;
        uint64_t tickCount = 0;
        double time = 0.0;
        double droppedTime = 0.0;
        double simulateMicros = 0.0;
        double renderMicros = 0.0;
        bool culled = false;
        bool playing = false;
        bool paused = false;
        bool emitting = false;
        uint32_t sortedViews = 0;
        uint32_t drawnViews = 0;
        uint32_t raysLastTick = 0;
        uint32_t bytesPerParticle = 0;
        uint32_t seed = 0;       // the seed in use: the component's, the effect's or the one this play drew
    };

    class JASPERENGINE_API ParticleSubsystem : public SubSystem {
    public:
        ParticleSubsystem(Scene* scene);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        // the time every scene's systems spent simulating since the last call, in milliseconds,
        // and the particles alive after it (the frame stats' particles_sim_ms line)
        static double takeFrameSimulation(size_t* alive = nullptr);

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // ---- playback (the component's API, Lua, MCP; Unity's ParticleSystem)
        void play(Entity entity);
        void pause(Entity entity);
        // stops emitting; clear also removes the particles
        void stop(Entity entity, bool clear);
        // Unity's Clear: the particles go, it plays on (or stays stopped, or paused)
        void clear(Entity entity);
        void restart(Entity entity);
        // every system back to not started, without particles: Play begins as a load does
        // (Play On Awake plays them; a Scene view preview's play or stop does not carry over)
        void resetForPlay();
        void emit(Entity entity, uint32_t count, const particles::EmitOverrides* overrides = nullptr);
        // restarts and simulates to `time` at once (a preview scrub uses previewSeek)
        void seek(Entity entity, double time);
        // exactly `ticks` ticks now
        void step(Entity entity, uint32_t ticks);
        void sendTrigger(Entity entity, const std::string& name);

        bool isPlaying(Entity entity);
        // the scheduler's pause of the whole system stays visible beside the per-entity one
        using SubSystem::isPaused;
        bool isPaused(Entity entity);
        bool isEmitting(Entity entity);
        uint32_t getParticleCount(Entity entity);
        double getTime(Entity entity);

        // ---- the editor's preview (the Scene view overlay, the Inspector, the animation window):
        // a scrub to `time` over frames (checkpoints, at most budgetMs per update), and frame
        // time for systems simulating in Edit mode (the selected ones)
        void previewSeek(Entity entity, double time);
        bool previewUpdate(Entity entity, double budgetMs);
        void previewAdvance(Entity entity, double dt);

        // the system's runtime (bound to its effect), null when it has none or cannot load it
        particles::ParticleSystemState* getState(Entity entity);
        ParticleSystemStats getStats(Entity entity);
        // A hash of the run: its seed, tick and particles, children included. Equal is the same state
        // of the same run. Reads every particle, so it is not part of getStats.
        uint64_t getStateHash(Entity entity);
        const particles::WorldStats& getWorldStats() const { return world.stats(); }
        particles::ParticleWorld& getWorld() { return world; }

        // an asset's file changed (or the editor edited it): its users rebind
        void reloadEffect(const std::string& path);
        // the effect document a system runs (its asset's or its inline one), null without one
        static const particles::ParticleEffectDocument* getDocument(const ParticleSystemComponent& particles);

        // RenderSystem: a camera saw the system's bounds this frame
        void markVisible(Entity entity);

    private:
        // binds (or rebinds) the component's effect; false when there is none to run
        bool ensureEffect(Entity entity, ParticleSystemComponent& particles);
        particles::CompileEnvironment compileEnvironment(Entity entity);
        // the library moved on since its sub-emitter files were last compared
        bool subEmittersOutOfDate(const ParticleSystemComponent& particles) const;
        particles::ParticleSystemState* stateOf(Entity entity, bool load);
        void prepareInput(Entity entity, ParticleSystemComponent& particles, Transform& transform, particles::EmitterInput& input);
        bool bodyVelocity(Entity entity, Vector3& velocity);
        void gatherFields();
        void gatherTerrains();
        void dispatchEvents(ParticleSystemComponent& particles);
        void updateBounds(particles::ParticleSystemState& state);
        float significanceOf(const particles::ParticleSystemState& state, float& cameraDistance) const;

        static bool castWorldRay(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, particles::CollisionHit& hit);
        static bool castWorldRay2D(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, particles::CollisionHit& hit);
        static bool sampleTerrain(const void* context, const float point[3], float surface[3], float normal[3]);

        particles::ParticleWorld world;
        // the systems whose stop action runs at the end of this frame (Disable, Destroy, Callback)
        std::vector<Entity> stopActions;
        void queueStopAction(Entity entity, particles::ParticleSystemState& state);
        void checkStopActions();
        void runStopActions();
        std::vector<particles::EmitterTask> tasks;
        std::vector<Entity> taskEntities;
        std::vector<particles::ForceFieldSample> fields;
        double fieldTime = 0.0;

        // the collision providers' scene data (main thread writes it before the simulation)
        struct TerrainRef{
            TerrainComponent* terrain = nullptr;
            Matrix4 worldToLocal;
            Matrix4 localToWorld;
            float halfSize = 0.0f;
        };
        std::vector<TerrainRef> terrains;
        particles::CollisionProvider provider3D;
        particles::CollisionProvider provider2D;

        // the frame's events, copied for the listeners
        std::vector<ParticleCollisionInfo> collisionBatch;
        std::vector<ParticleDeathInfo> deathBatch;
        std::vector<ParticleSubEmitInfo> subEmitBatch;
    };

}

#endif //PARTICLE_SUBSYSTEM_H
