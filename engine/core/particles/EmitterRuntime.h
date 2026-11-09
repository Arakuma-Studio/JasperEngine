// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The fixed-tick accumulation (whole ticks, a cap per frame, dropped time), the interpolation
// factor, the chunked seek and the queued spawns follow OpenEngine's ParticleRuntime
// (Engine/Modules/Particles/Source/ParticleRuntime.cpp), Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef PARTICLES_EMITTERRUNTIME_H
#define PARTICLES_EMITTERRUNTIME_H

// The simulation of one emitter (docs/specs/S20-particles.md R4, R5, R10, R14, R29, D2, D11):
// its channels, trails and sub-emitter children, no GPU objects. It advances in whole ticks of
// 1/tickRate (at most 4 a frame; the rest is dropped and counted), renders between the last
// two ticks, and gives the same channels for the same (effect, seed, transforms, ticks)
// whatever the worker count or chunk size. ParticleWorld runs many of them together; a lone
// one (tests, previews) runs its chunks through jobs::parallelFor itself.

#include "Export.h"
#include "particles/ParticleChannels.h"
#include "particles/ParticleKernelContext.h"
#include "particles/ParticleTrails.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace jasperengine { namespace jobs { class JobSystem; } }

namespace jasperengine { namespace particles {

    struct AnalyticProgram;
    struct ScalarDriver;

    inline constexpr uint32_t kMaxTicksPerFrame = 4;
    inline constexpr double kMaxVariableStep = 1.0 / 20.0;
    inline constexpr uint32_t kMaxSpawnRequests = 4096;
    inline constexpr uint32_t kMaxForceFields = 64;
    inline constexpr uint32_t kMaxDeathEvents = 64;
    inline constexpr uint32_t kMaxSoundEvents = 16;

    struct EmitterInput {
        // the emitter's transform, column-major 4x4 (null: the identity)
        const float* transform = nullptr;
        uint32_t seed = 1;
        // live particles the world budget allows this emitter now
        uint32_t budget = kMaxParticlesPerEmitter;
        float speed = 1.0f;                // playback speed
        float emissionScale = 1.0f;        // distance LOD and the world budget
        float sizeScale = 1.0f;            // the scaling mode's factor on start sizes
        float gravity[3] = {0.0f, -9.81f, 0.0f};   // world space
        // the emitter's velocity when something else gives it (Emitter Velocity Mode Rigidbody:
        // its body's), world space; without it the transform's motion is the velocity
        bool hasVelocity = false;
        float velocity[3] = {0.0f, 0.0f, 0.0f};
        // force fields that overlap the emitter, world space
        const ForceFieldSample* fields = nullptr;
        uint32_t fieldCount = 0;
        // world-space queries (terrain, physics rays)
        const CollisionProvider* collision = nullptr;
        // more collision planes (the component's plane GameObjects), world space: normal, d
        const float (*planes)[4] = nullptr;
        uint32_t planeCount = 0;
        jobs::JobSystem* jobs = nullptr;   // null: the process job system
        uint32_t chunkSize = kChunkSize;   // particles per update unit (a multiple of 4)
    };

    struct SeekState {
        double target = 0.0;
        double tick = 0.0;
        uint32_t completed = 0;
        uint32_t total = 0;
        bool pending = false;
    };

    // emit(): the values that replace the effect's for the emitted particles
    struct EmitOverrides {
        bool hasPosition = false, hasVelocity = false, hasSize = false, hasColor = false, hasLifetime = false;
        float position[3] = {0, 0, 0};   // world space
        float velocity[3] = {0, 0, 0};   // world space
        float size = 1.0f;
        float color[4] = {1, 1, 1, 1};   // linear
        float lifetime = 1.0f;
    };

    // a parent's event for a sub-emitter child: `count` particles at the parent particle
    struct SpawnRequest {
        float position[3] = {0, 0, 0};   // world space
        float velocity[3] = {0, 0, 0};
        float color[4] = {1, 1, 1, 1};   // linear
        float size = 1.0f;
        float rotation = 0.0f;
        float lifetime = 1.0f;
        uint32_t count = 1;
        uint32_t parentId = 0;           // kNoParent: fired at the emitter, from no particle
        uint8_t inherit = 0;             // InheritBits
        float factor = 1.0f;
        float parentQuat[4] = {0, 0, 0, 1};   // Follow Rotation: the parent's rotation (simulation space)
    };
    enum InheritBits : uint8_t { InheritColor = 1, InheritSize = 2, InheritRotation = 4, InheritLifetime = 8, InheritVelocity = 16 };

    // what the last frame's ticks raised (main thread reads them after the simulation)
    struct ParticleDeathEvent {
        float position[3] = {0, 0, 0};   // world space
        float velocity[3] = {0, 0, 0};
        uint32_t particleId = 0;
    };

    struct SubEmitEvent {
        SubEmitterTrigger trigger = SubEmitterTrigger::Birth;
        uint32_t entry = 0;
        uint32_t count = 0;
        float position[3] = {0, 0, 0};
    };

    // a Sound Event (S37) the effect asked for: a burst's, a sub-emitter entry's or a trigger's
    // (by its index in the effect's document), at a world position or at the emitter
    struct ParticleSoundEvent {
        enum class Source : uint8_t { Burst, SubEmitter, Trigger };
        Source source = Source::Burst;
        bool atEmitter = true;
        uint16_t index = 0;
        float position[3] = {0, 0, 0};   // world space, when not at the emitter
    };

    enum class PlayState : uint8_t { Stopped, Playing, Paused };

    // FNV-1a of a trigger name (sendTrigger)
    JASPERENGINE_API uint32_t nameHash(std::string_view name);

    class JASPERENGINE_API EmitterRuntime {
    public:
        EmitterRuntime();
        ~EmitterRuntime();
        EmitterRuntime(const EmitterRuntime&) = delete;
        EmitterRuntime& operator=(const EmitterRuntime&) = delete;

        // binds a compiled effect and resets; capacity 0 sizes it to the effect
        void bind(std::shared_ptr<const CompiledEffect> effect, uint32_t capacity = 0);
        // replaces the effect when its structure is the same, keeping live particles
        bool rebind(std::shared_ptr<const CompiledEffect> effect);
        // the sub-emitter children (one per entry, in the effect's order): bound by the caller
        // (ParticleWorld resolves the effects), owned here
        void setChildren(std::vector<std::unique_ptr<EmitterRuntime>> children);
        // binds a child per sub-emitter entry, through the children's children (at most 3
        // levels): `resolve` compiles an entry's effect (null when it cannot be read)
        using SubEmitterResolver = std::function<std::shared_ptr<const CompiledEffect>(const SubEmitterEntry& entry)>;
        void bindSubEmitters(const SubEmitterResolver& resolve, int depth = 1);
        // gives the bound children their effects again, keeping their particles; false when one
        // changed structure and they need bindSubEmitters (a child's file was edited)
        bool rebindSubEmitters(const SubEmitterResolver& resolve, int depth = 1);
        EmitterRuntime* child(size_t index) const { return index < children.size() ? children[index].get() : nullptr; }
        size_t childCount() const { return children.size(); }
        const CompiledEffect* effect() const { return compiled.get(); }
        const std::shared_ptr<const CompiledEffect>& effectPointer() const { return compiled; }
        const ParticleProgram* program() const { return compiled ? &compiled->program : nullptr; }
        // returns the storage to the pool
        void unbind();

        // clears particles, trails, events and the emission; the next play prewarms again
        void reset(uint32_t seed);

        // ---- playback (R14)
        void play();
        void pause();
        // stops emitting; clear also removes the particles
        void stop(bool clear);
        void restart();
        // removes the particles and trails (its children's too), the clocks and the emission
        // going on as they were (Unity's Clear)
        void clear();
        PlayState state() const { return playState; }
        bool isPlaying() const { return playState == PlayState::Playing; }
        bool isPaused() const { return playState == PlayState::Paused; }
        bool isEmitting() const;
        // still has particles (or children with particles)
        bool isAlive() const;

        // ---- simulation
        // advances by frame time `dt` in whole ticks (R4)
        void advance(const EmitterInput& input, double dt);
        // exactly one tick
        void step(const EmitterInput& input);
        // restarts and simulates to `time` over continueSeek calls (no gameplay events)
        void beginSeek(const EmitterInput& input, double time, SeekState& seek);
        // runs seek ticks for at most budgetMs; true when done
        bool continueSeek(const EmitterInput& input, SeekState& seek, double budgetMs);
        void seek(const EmitterInput& input, double time);
        // one tick of `tick` seconds the way a seek runs it (no gameplay events)
        void seekStep(const EmitterInput& input, double tick);
        // after seek steps: the time left over below a tick drives the interpolation
        void settleSeek(double remainder, double tick);
        // queued for the next tick
        void emit(uint32_t count, const EmitOverrides* overrides = nullptr);
        // the effect's triggers of that name act now (play, pause, stop); its Trigger
        // sub-emitters of that name fire on the next tick, from every live particle or, with none,
        // from the emitter (a stopped emitter runs that tick without its own emission)
        void sendTrigger(uint32_t nameHash);
        bool queueSpawn(const SpawnRequest& request);
        bool isPrewarming() const { return prewarm.pending; }

        // ---- what ParticleWorld drives (a tick in stages)
        // the ticks this frame will run (and their length); 0 when paused or nothing to do.
        // maxTicks above 4: a culled emitter's catch-up
        uint32_t planFrame(const EmitterInput& input, double dt, uint32_t maxTicks = kMaxTicksPerFrame);
        double plannedTick() const { return frameTick; }
        // prepares tick `index` of the frame (the emitter transform between the frames)
        void beginTick(const EmitterInput& input, uint32_t index);
        uint32_t chunkCount() const;
        uint32_t chunkSize() const { return activeChunkSize; }
        // one chunk's update ops (any thread, one chunk per call)
        void updateChunk(uint32_t chunk);
        // the rest of the tick: world collisions, events, deaths, emission, trails, children
        void finishTick(const EmitterInput& input);
        // after the frame's ticks: the interpolation factor
        void endFrame();

        // ---- Analytic simulation (S49 R15-R19): no channels and no ticks; the time and the
        // program the GPU evaluates (its seed's, its time set every frame and on seek)
        bool isAnalytic() const;
        const AnalyticProgram* analytic() const { return analyticProgram.get(); }
        // the live particles now: O(slots), on demand only (stats, scripts)
        uint32_t analyticAlive() const;

        // ---- results
        // the live particles of a Simulated emitter (an Analytic one keeps none: analyticAlive)
        uint32_t count() const { return channels.count; }
        uint32_t capacity() const { return channels.capacity(); }
        // the same with its sub-emitter children's, down the tree: what stats and the budget count
        uint32_t totalCount() const;
        uint32_t totalCapacity() const;
        // the most the tree may hold: each emitter is held to its own Max Particles
        uint32_t totalMaxParticles() const;
        // what the tree wants alive at its peak: its own capacity hint, and for each child what
        // that many parents ask for (a child's own Emission is ignored), up to its Max Particles
        uint32_t treeCapacityHint() const;
        float interpolation() const { return alpha; }
        // the length of the last tick (the time between the previous and the current channels)
        float lastTickSeconds() const { return lastTick; }
        double time() const { return elapsed; }
        // the time within the current cycle (negative during the start delay)
        double emitterTime() const { return cycleTime; }
        double droppedTime() const { return dropped; }
        uint32_t ticksLastFrame() const { return ticksThisFrame; }
        uint64_t tickCount() const { return ticks; }
        // the emitter jumped further than kTeleportDistance in a tick (Rate over Distance skipped it)
        bool jumpedTooFar() const { return teleportWarned; }
        const ParticleChannels& particleChannels() const { return channels; }
        const ParticleTrails& trails() const { return trailStore; }
        std::span<const CollisionEvent> collisionEvents() const { return {collisions.data(), collisions.size()}; }
        std::span<const ParticleDeathEvent> deathEvents() const { return {deaths.data(), deaths.size()}; }
        std::span<const SubEmitEvent> subEmitEvents() const { return {subEmits.data(), subEmits.size()}; }
        // the sounds asked for since the last takeSoundEvents (at most kMaxSoundEvents; seeks ask none)
        std::span<const ParticleSoundEvent> soundEvents() const { return {sounds.data(), sounds.size()}; }
        void takeSoundEvents() { sounds.clear(); }
        // computed bounds of the live particles in simulation space (min, max), padded by size
        void bounds(float outMin[3], float outMax[3]) const;
        // (an Analytic program's are its compiled bounds: a clear or a seek keeps them)
        bool hasBounds() const { return boundsValid || isAnalytic(); }
        // simulation space to world space of the last tick (the emitter transform in Local)
        const float* simToWorld() const { return simWorld; }
        // a particle's colour (linear), size and rotation as the renderer shows it, at its age
        void appearance(uint32_t index, float color[4], float& size, float& rotation) const;
        // the hash of every live channel, and of the children (determinism)
        uint64_t stateHash() const;
        uint32_t seedValue() const { return seed; }
        // counts its resets, clears and seeks: what was drawn of it before is not its state now
        uint64_t generation() const { return resetGeneration; }
        // particles spawned since the last reset, its children's not counted (what the preview's metrics call emitted)
        uint32_t spawnedCount() const { return spawnCounter; }
        uint32_t raysLastTick() const { return lastRays; }

        // a copy of the state (seek checkpoints)
        struct Checkpoint {
            ParticleChannels::Snapshot channels;
            ParticleTrails trails;
            double elapsed = 0.0, cycleTime = 0.0, burstCheckedTo = -1.0, rateAccumulator = 0.0, distanceAccumulator = 0.0;
            float arcPhase = 0.0f, radiusPhase = 0.0f, noiseScroll = 0.0f;
            float lastTickVelocity[3] = {0, 0, 0};
            uint64_t ticks = 0;
            uint32_t spawnCounter = 0;
            uint32_t loopIndex = 0;
            bool emitting = false;
            float lastTick = 0.0f;
            bool boundsValid = false;
            float boundsLo[3] = {0, 0, 0}, boundsHi[3] = {0, 0, 0};
            float tickTransform[16] = {};
            float lastTickTransform[16] = {};
            CachedPlane planeGrid[kPlaneGridSize];
            std::vector<Checkpoint> children;
            size_t bytes() const;
        };
        void saveCheckpoint(Checkpoint& out) const;
        void restoreCheckpoint(const Checkpoint& in);

    private:
        // the frame's collision, death and sub-emit events (they gather over its ticks)
        void clearEvents();
        void configureStorage(uint32_t newCapacity);
        void growTo(uint32_t needed);
        void setFrameTransforms(const EmitterInput& input, double fraction);
        // the start delay of this play (a random range: by the seed)
        double pickStartDelay() const;
        void fillContext(KernelContext& ctx, const EmitterInput* input) const;
        void runTickSerial(const EmitterInput& input);
        void worldCollisionStage(const EmitterInput& input);
        void deathStage();
        void compact();
        void emissionStage(const EmitterInput& input);
        void spawnBatch(const EmitterInput& input, uint32_t count, const float* fractions, const float* preAdvance,
                        const SpawnRequest* request, const EmitOverrides* overrides);
        void trailStage();
        void childrenStage(const EmitterInput& input);
        // the children render between the same two ticks as their parent
        void shareInterpolation();
        uint64_t childrenCapacityHint(uint64_t parents) const;
        void raiseSubEmitters(SubEmitterTrigger trigger, uint32_t index, const float* normal);
        void raiseEntry(uint32_t entry, SubEmitterTrigger trigger, uint32_t index, const float* normal);
        // a Trigger entry fired with no live particle: once, at the emitter
        void raiseEntryAtEmitter(uint32_t entry);
        void recordSound(ParticleSoundEvent::Source source, uint32_t index, const float* position);
        // a particle's size and roll as the renderer shows them (appearance's)
        float sizeOf(uint32_t index, const ScalarDriver& driver) const;
        float rollOf(uint32_t index, const ScalarDriver& driver) const;
        void driverOf(uint32_t index, ScalarDriver& driver) const;
        // a particle's rotation in simulation space: its quaternion, or its roll about Z
        void particleRotation(uint32_t index, float q[4]) const;
        // Follow Parent: moves `child`'s particles with the particles of this emitter that spawned them
        void followStage(EmitterRuntime& child, const SubEmitterEntry& entry);
        void continueSeekTicks(const EmitterInput& input, SeekState& seek, double budgetMs);
        static uint32_t childSeed(uint32_t parent, uint32_t index);
        void simToWorldPoint(const float in[3], float out[3]) const;
        void simToWorldDir(const float in[3], float out[3]) const;
        void worldToSimPoint(const float in[3], float out[3]) const;
        void worldToSimDir(const float in[3], float out[3]) const;
        uint32_t limit(const EmitterInput& input) const;
        static bool simRay(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, CollisionHit& hit);
        static bool simTerrain(const void* context, const float point[3], float surface[3], float normal[3]);

        std::shared_ptr<const CompiledEffect> compiled;
        const ParticleProgram* prog = nullptr;
        ParticleChannels channels;
        ParticleTrails trailStore;
        std::vector<std::unique_ptr<EmitterRuntime>> children;

        PlayState playState = PlayState::Stopped;
        bool emitting = false;
        uint64_t resetGeneration = 0;
        uint32_t seed = 1;
        uint32_t spawnCounter = 0;
        uint64_t ticks = 0;
        double elapsed = 0.0;           // seconds simulated since play
        double cycleTime = 0.0;         // time in the current cycle at the tick's start (from -startDelay)
        double burstCheckedTo = -1.0;   // the bursts of this cycle up to this time have fired
        uint32_t loopIndex = 0;
        double accumulator = 0.0;
        double dropped = 0.0;
        double rateAccumulator = 0.0;
        double distanceAccumulator = 0.0;
        double frameTick = 0.0;         // the planned ticks' length
        double frameCarried = 0.0;
        double frameScaled = 0.0;
        uint32_t ticksThisFrame = 0;
        float alpha = 1.0f;
        float lastTick = 0.0f;
        bool started = false;           // the first frame after play has set the transforms
        bool recordEffects = true;      // seeks raise no events
        bool boundsValid = false;
        SeekState prewarm;
        uint32_t lastRays = 0;
        uint32_t activeChunkSize = kChunkSize;

        // the emitter transforms: the previous frame's, this tick's and the last tick's
        float prevFrame[16];
        float curFrame[16];
        float tickTransform[16];
        float lastTickTransform[16];
        float simWorld[16];
        float worldSim[16];
        float tickDt = 0.0f;
        float emitterVelocity[3] = {0, 0, 0};   // world units per second
        // the emitter's velocity at the last tick (Inherit Velocity's Current mode takes the change)
        float lastTickVelocity[3] = {0, 0, 0};
        float tickVelocityDelta[3] = {0, 0, 0};
        // the shape's Loop / Ping-Pong phases at the tick's start, their rates this tick; the
        // noise field's scroll (a scroll speed that is not a constant)
        float arcPhase = 0.0f, radiusPhase = 0.0f, arcRate = 0.0f, radiusRate = 0.0f;
        float noiseScroll = 0.0f;
        // a burst's particles being spawned: the batch's first one's index in it and its size
        uint32_t spreadBase = 0, spreadCount = 0;
        // the bursts firing this tick, one entry each (their counts)
        std::vector<uint32_t> burstGroups;
        float tickDistance = 0.0f;
        // a jump of the emitter of more than kTeleportDistance in one tick spawns nothing over
        // distance (no burst of thousands; origin/main's rule), said once
        static constexpr float kTeleportDistance = 100.0f;
        bool teleportWarned = false;

        // Analytic: the program of this seed, the emitter time emission stopped at (1e30: on)
        std::unique_ptr<AnalyticProgram> analyticProgram;
        double analyticEnd = 1.0e30;
        bool analyticMovedWarned = false;
        bool analyticHasTransform = false;
        float analyticLastTransform[16] = {};
        float analyticGravity[3] = {0.0f, -9.81f, 0.0f};
        void analyticUpdate(const EmitterInput& input);

        // per tick, sized at bind: chunk outputs and their arrays
        std::vector<ChunkOutput> chunkOut;
        std::vector<uint32_t> deadStorage;
        std::vector<RayRequest> rayStorage;
        std::vector<CollisionEvent> eventStorage;
        std::vector<uint32_t> deadList;
        std::vector<ForceFieldSample> fieldsSim;
        struct RayResult {
            bool hit = false;
            CollisionHit h;
        };
        std::vector<RayResult> rayResults;
        std::vector<RayRequest*> chosen;
        CachedPlane planeGrid[kPlaneGridSize];
        float planeCellInv = 1.0f;
        bool collisionContacts = false;   // the collision op records its contacts
        CollisionProvider simCollision;
        const CollisionProvider* worldCollision = nullptr;
        float planesSim[kMaxCollisionPlanes][4] = {};
        uint32_t planeCount = 0;
        float boundsLo[3] = {0, 0, 0}, boundsHi[3] = {0, 0, 0};

        // queued work and events
        struct PendingEmit {
            uint32_t count = 0;
            bool hasOverrides = false;
            EmitOverrides overrides;
        };
        std::vector<PendingEmit> pendingEmits;
        std::vector<SpawnRequest> requests;
        std::vector<SpawnRequest> requestsProcessing;
        std::vector<uint32_t> triggers;
        std::vector<CollisionEvent> collisions;
        std::vector<ParticleDeathEvent> deaths;
        std::vector<SubEmitEvent> subEmits;
        std::vector<ParticleSoundEvent> sounds;
        // Follow Parent: this tick's live particles as (spawn index << 32 | slot), sorted
        std::vector<uint64_t> followIndex;
        bool followIndexed = false;
        const EmitterInput* currentInput = nullptr;
    };

}}

#endif // PARTICLES_EMITTERRUNTIME_H
