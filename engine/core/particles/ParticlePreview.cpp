// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticlePreview.h"

#include <chrono>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    double tickOf(const EmitterRuntime& runtime){
        const ParticleProgram* prog = runtime.program();
        return prog && prog->tickRate > 0 ? 1.0 / prog->tickRate : kMaxVariableStep;
    }

}

void ParticlePreview::invalidate(){
    checkpoints.clear();
    bytes = 0;
    state = SeekState();
}

void ParticlePreview::seek(EmitterRuntime& runtime, const EmitterInput& input, double time){
    targetTime = std::max(0.0, time);
    simulated = 0.0;
    const double tick = tickOf(runtime);
    // in whole ticks: the times of a seek and of its checkpoints never drift apart
    const uint64_t targetTick = (uint64_t)std::floor(targetTime / tick + 1e-6);
    // the latest checkpoint at or before it
    const Stored* from = nullptr;
    for (const Stored& c : checkpoints){
        if (c.tick <= targetTick && (!from || c.tick > from->tick))
            from = &c;
    }
    if (from){
        runtime.restoreCheckpoint(from->state);
        startTick = from->tick;
    }else{
        SeekState fresh;
        runtime.beginSeek(input, 0.0, fresh);
        startTick = 0;
        // the state at 0 is the first checkpoint
        keepCheckpoint(runtime);
    }
    state = SeekState();
    state.target = targetTime;
    state.tick = tick;
    state.completed = 0;
    state.total = (uint32_t)(targetTick - startTick);
    state.pending = true;
}

bool ParticlePreview::update(EmitterRuntime& runtime, const EmitterInput& input, double budgetMs){
    if (!state.pending)
        return true;
    const auto begin = std::chrono::steady_clock::now();
    const uint64_t perCheckpoint = (uint64_t)std::max(1.0, std::floor(kCheckpointSeconds / state.tick + 0.5));
    while (state.completed < state.total){
        runtime.seekStep(input, state.tick);
        state.completed++;
        simulated += state.tick;
        // a checkpoint every second of simulated time
        if (runtime.tickCount() % perCheckpoint == 0)
            keepCheckpoint(runtime);
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() >= budgetMs)
            break;
    }
    if (state.completed >= state.total){
        runtime.settleSeek(targetTime - (double)(startTick + state.total) * state.tick, state.tick);
        state.pending = false;
    }
    return !state.pending;
}

void ParticlePreview::keepCheckpoint(EmitterRuntime& runtime){
    const uint64_t tick = runtime.tickCount();
    for (const Stored& c : checkpoints){
        if (c.tick == tick)
            return;
    }
    Stored s;
    s.tick = tick;
    runtime.saveCheckpoint(s.state);
    s.bytes = s.state.bytes();
    // the cap: the oldest go first (the one at 0 stays: every seek can start from it)
    while (!checkpoints.empty() && bytes + s.bytes > maxBytes){
        auto oldest = checkpoints.begin();
        if (oldest->tick == 0 && checkpoints.size() > 1)
            ++oldest;
        else if (oldest->tick == 0)
            break;
        bytes -= oldest->bytes;
        checkpoints.erase(oldest);
    }
    if (bytes + s.bytes > maxBytes)
        return;
    bytes += s.bytes;
    checkpoints.push_back(std::move(s));
}
