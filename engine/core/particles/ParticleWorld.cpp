// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleWorld.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

using namespace jasperengine;
using namespace jasperengine::particles;

uint32_t particles::defaultWorldBudget(){
#if defined(__EMSCRIPTEN__)
    return kBudgetWeb;
#elif defined(JASPERENGINE_ANDROID) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return kBudgetMobile;
#else
    return kBudgetDesktop;
#endif
}

namespace {

    uint32_t projectWorldBudget = 0;

}

ParticleWorld::ParticleWorld(): worldBudget(0){
}

uint32_t ParticleWorld::budget() const{
    if (worldBudget != 0)
        return worldBudget;
    return projectWorldBudget != 0 ? projectWorldBudget : defaultWorldBudget();
}

void ParticleWorld::setProjectBudget(uint32_t budget){
    projectWorldBudget = budget;
}

uint32_t ParticleWorld::projectBudget(){
    return projectWorldBudget;
}

double particles::cullingStep(CullingMode mode, bool looping, bool visible, double dt, int tickRate, CullState& state, uint32_t& maxTicks){
    maxTicks = kMaxTicksPerFrame;
    if (mode == CullingMode::Automatic)
        mode = looping ? CullingMode::PauseAndCatchUp : CullingMode::AlwaysSimulate;
    switch (mode){
        case CullingMode::AlwaysSimulate:
            state.missed = 0.0;
            return dt;
        case CullingMode::Pause:
            state.missed = 0.0;
            return visible ? dt : 0.0;
        case CullingMode::PauseAndCatchUp:
        default: {
            if (!visible){
                state.missed += dt;
                return 0.0;
            }
            const double catchUp = std::min(state.missed, kMaxCatchUpSeconds);
            state.missed = 0.0;
            if (catchUp > 0.0){
                // the catch-up's ticks on top of the frame's
                const double rate = tickRate > 0 ? tickRate : 1.0 / kMaxVariableStep;
                maxTicks += (uint32_t)std::ceil(catchUp * rate);
            }
            return dt + catchUp;
        }
    }
}

void ParticleWorld::shareBudget(std::vector<EmitterTask>& tasks) const{
    // what each wants: its peak population with what its children will need (or what it holds
    // now); its budget covers its children, each emitter still held to its own Max Particles
    const uint32_t total = budget();
    uint64_t want = 0;
    for (EmitterTask& t : tasks){
        const ParticleProgram* prog = t.runtime ? t.runtime->program() : nullptr;
        t.budget = prog ? std::min(t.runtime->totalMaxParticles(), t.input.budget) : 0u;
        t.emissionScale = t.input.emissionScale;
        want += prog ? std::max<uint64_t>(t.runtime->totalCount(), std::min(t.runtime->treeCapacityHint(), t.budget)) : 0u;
    }
    if (want <= total)
        return;
    // over the budget: the higher priorities first, within a priority the larger on screen;
    // each gets what it wants while the budget lasts, the rest a share of what is left
    order.resize(tasks.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){
        if (tasks[a].priority != tasks[b].priority) return tasks[a].priority > tasks[b].priority;
        return tasks[a].significance > tasks[b].significance;
    });
    uint64_t remaining = total;
    size_t i = 0;
    while (i < order.size()){
        // one priority tier
        size_t j = i;
        uint64_t tierWant = 0;
        double tierSignificance = 0.0;
        while (j < order.size() && tasks[order[j]].priority == tasks[order[i]].priority){
            EmitterTask& t = tasks[order[j]];
            const ParticleProgram* prog = t.runtime->program();
            tierWant += prog ? std::max<uint64_t>(t.runtime->totalCount(), std::min(t.runtime->treeCapacityHint(), t.budget)) : 0u;
            tierSignificance += std::max(t.significance, 1e-4f);
            j++;
        }
        const double fill = tierWant > 0 ? std::min(1.0, (double)remaining / (double)tierWant) : 1.0;
        uint64_t given = 0;
        for (size_t k = i; k < j; k++){
            EmitterTask& t = tasks[order[k]];
            const ParticleProgram* prog = t.runtime->program();
            const uint64_t need = prog ? std::max<uint64_t>(t.runtime->totalCount(), std::min(t.runtime->treeCapacityHint(), t.budget)) : 0u;
            uint64_t share;
            if (fill >= 1.0){
                share = need;
            }else{
                // the tier's remainder by significance (a bigger effect on screen keeps more)
                share = (uint64_t)std::floor((double)remaining * std::max(t.significance, 1e-4f) / tierSignificance);
                share = std::min(share, need);
            }
            t.budget = (uint32_t)std::min<uint64_t>(share, t.budget);
            t.emissionScale = need > 0 ? t.input.emissionScale * (float)std::min(1.0, (double)share / (double)need) : 0.0f;
            given += t.budget;
        }
        remaining = remaining > given ? remaining - given : 0;
        i = j;
    }
}

void ParticleWorld::simulate(std::vector<EmitterTask>& tasks, jobs::JobSystem* jobsArg){
    const auto start = std::chrono::steady_clock::now();
    jobs::JobSystem& js = jobsArg ? *jobsArg : jobs::system();
    lastStats = WorldStats();
    lastStats.emitters = (uint32_t)tasks.size();
    lastStats.budget = budget();
    if (tasks.empty()){
        lastStats.simulateMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        return;
    }
    shareBudget(tasks);
    uint32_t rounds = 0;
    for (EmitterTask& t : tasks){
        t.input.budget = t.budget;
        t.input.emissionScale = t.emissionScale;
        t.ticks = t.runtime ? t.runtime->planFrame(t.input, t.dt, t.maxTicks) : 0u;
        rounds = std::max(rounds, t.ticks);
        lastStats.ticks += t.ticks;
    }
    for (uint32_t r = 0; r < rounds; r++){
        active.clear();
        units.clear();
        for (uint32_t i = 0; i < tasks.size(); i++){
            if (tasks[i].ticks <= r)
                continue;
            active.push_back(i);
            tasks[i].runtime->beginTick(tasks[i].input, r);
            const uint32_t chunks = tasks[i].runtime->chunkCount();
            for (uint32_t c = 0; c < chunks; c++) units.push_back({i, c});
        }
        lastStats.units += (uint32_t)units.size();
        // the update chunks of every emitter in one parallelFor; small emitters' single chunks
        // are claimed several at a time (grain) so a unit holds about a chunk of particles
        size_t particles = 0;
        for (uint32_t i : active) particles += tasks[i].runtime->count();
        const size_t grain = units.empty() ? 1 : std::max<size_t>(1, (size_t)kChunkSize * units.size() / std::max<size_t>(particles, 1));
        jobs::ParallelForOptions options;
        options.grain = std::min<size_t>(grain, 64);
        js.parallelFor(units.size(), [&](size_t u){
            tasks[units[u].task].runtime->updateChunk(units[u].chunk);
        }, options);
        // then every emitter's finish (deaths, spawns, trails, children), each on its own
        js.parallelFor(active.size(), [&](size_t k){
            EmitterTask& t = tasks[active[k]];
            t.runtime->finishTick(t.input);
        });
    }
    for (EmitterTask& t : tasks){
        if (t.runtime){
            t.runtime->endFrame();
            lastStats.live += t.runtime->totalCount();
        }
    }
    lastStats.overBudget = lastStats.live > lastStats.budget;
    lastStats.simulateMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

uint32_t ParticleWorld::cullFields(const ForceFieldSample* fields, uint32_t count, const float bmin[3], const float bmax[3],
                                   uint32_t layerMask, ForceFieldSample* out, uint32_t max){
    uint32_t n = 0;
    for (uint32_t f = 0; f < count && n < max; f++){
        const ForceFieldSample& s = fields[f];
        if (!(s.layers & layerMask))
            continue;
        if (s.shape != ForceFieldShape::Infinite){
            bool overlaps = true;
            for (int k = 0; k < 3; k++)
                overlaps = overlaps && s.aabbMin[k] <= bmax[k] && s.aabbMax[k] >= bmin[k];
            if (!overlaps)
                continue;
        }
        out[n++] = s;
    }
    return n;
}
