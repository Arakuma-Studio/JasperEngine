// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEWORLD_H
#define PARTICLES_PARTICLEWORLD_H

// The scheduler of a scene's emitters (docs/specs/S20-particles.md D2, R15, R19): the world
// particle budget shared out by priority and significance, the frame's ticks of every emitter
// run together (all update chunks of a tick round in one parallelFor, packed several small
// emitters to a unit, then every emitter's finish in another), culling and catch-up, and the
// force fields each emitter sees. Single-thread web runs the same jobs inline.

#include "Export.h"
#include "particles/EmitterRuntime.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace particles {

    // world particle budgets (R15), the project setting's defaults
    inline constexpr uint32_t kBudgetDesktop = 262144;
    inline constexpr uint32_t kBudgetMobile = 65536;
    inline constexpr uint32_t kBudgetWeb = 131072;
    JASPERENGINE_API uint32_t defaultWorldBudget();

    // what culling leaves an emitter to simulate this frame (R15): Pause stops it off screen,
    // Pause and Catch-up also keeps the time it missed and runs up to 0.5 s of it on view,
    // Automatic is that for looping effects and Always Simulate for the others
    struct CullState {
        double missed = 0.0;
    };
    inline constexpr double kMaxCatchUpSeconds = 0.5;
    JASPERENGINE_API double cullingStep(CullingMode mode, bool looping, bool visible, double dt, int tickRate, CullState& state, uint32_t& maxTicks);

    struct EmitterTask {
        EmitterRuntime* runtime = nullptr;
        EmitterInput input;
        int priority = 0;
        // its share of the screen (0 to 1): a larger one keeps more of its emission
        float significance = 1.0f;
        double dt = 0.0;                   // this frame's time for it (0: paused or culled)
        uint32_t maxTicks = kMaxTicksPerFrame;
        // results
        uint32_t ticks = 0;
        uint32_t budget = 0;
        float emissionScale = 1.0f;
    };

    struct WorldStats {
        uint32_t emitters = 0;
        uint32_t live = 0;
        uint32_t budget = 0;
        uint32_t ticks = 0;
        uint32_t units = 0;
        double simulateMicros = 0.0;
        bool overBudget = false;
    };

    class JASPERENGINE_API ParticleWorld {
    public:
        ParticleWorld();

        // this world's own budget, over the project's
        void setBudget(uint32_t budget){ worldBudget = budget; }
        uint32_t budget() const;
        // Project Settings > World Particle Budget, for every world without its own (0: the
        // platform's default); read each frame, so a change reaches the scenes that are running
        static void setProjectBudget(uint32_t budget);
        static uint32_t projectBudget();

        // runs the frame of every task (their runtimes advance by task.dt)
        void simulate(std::vector<EmitterTask>& tasks, jobs::JobSystem* jobs = nullptr);

        // the budget's share-out (R15), alone: each task's live-particle budget and emission
        // scale for this frame
        void shareBudget(std::vector<EmitterTask>& tasks) const;

        const WorldStats& stats() const { return lastStats; }

        // the fields of `fields` (world space) that overlap a box: written to `out`, at most `max`
        static uint32_t cullFields(const ForceFieldSample* fields, uint32_t count, const float boundsMin[3], const float boundsMax[3],
                                   uint32_t layerMask, ForceFieldSample* out, uint32_t max);

    private:
        struct Unit {
            uint32_t task;
            uint32_t chunk;
        };
        uint32_t worldBudget;
        std::vector<Unit> units;
        std::vector<uint32_t> active;
        mutable std::vector<uint32_t> order;
        WorldStats lastStats;
    };

}}

#endif // PARTICLES_PARTICLEWORLD_H
