// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EmitterRuntime.h"

#include "Log.h"
#include "jobs/JobSystem.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEval.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // the kernels' working memory, one per thread (allocated on its first tick)
    struct SpawnStaging {
        alignas(64) float data[ChCount][kSpawnBatch];
        alignas(16) float fraction[kSpawnBatch];
        alignas(16) float dt[kSpawnBatch];
    };
    struct ThreadScratch {
        ChunkScratch chunk;
        SpawnStaging staging;
    };
    ThreadScratch& threadScratch(){
        thread_local std::unique_ptr<ThreadScratch> scratch;
        if (!scratch)
            scratch = std::make_unique<ThreadScratch>();
        return *scratch;
    }

    jobs::JobSystem& jobsOf(const EmitterInput& input){
        return input.jobs ? *input.jobs : jobs::system();
    }

    // column-major 4x4 helpers
    void lerpMatrix(const float* a, const float* b, float t, float* out){
        for (int i = 0; i < 16; i++) out[i] = a[i] + (b[i] - a[i]) * t;
    }

    void inverseAffine(const float* m, float* out){
        // the inverse of the 3x3 part, then the translation
        const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
        const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
        const float inv = std::fabs(det) > 1.0e-20f ? 1.0f / det : 0.0f;
        float r[9];
        r[0] = (e * k - f * h) * inv; r[3] = -(b * k - c * h) * inv; r[6] = (b * f - c * e) * inv;
        r[1] = -(d * k - f * g) * inv; r[4] = (a * k - c * g) * inv; r[7] = -(a * f - c * d) * inv;
        r[2] = (d * h - e * g) * inv; r[5] = -(a * h - b * g) * inv; r[8] = (a * e - b * d) * inv;
        out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 0.0f;
        out[4] = r[3]; out[5] = r[4]; out[6] = r[5]; out[7] = 0.0f;
        out[8] = r[6]; out[9] = r[7]; out[10] = r[8]; out[11] = 0.0f;
        const float tx = m[12], ty = m[13], tz = m[14];
        out[12] = -(r[0] * tx + r[3] * ty + r[6] * tz);
        out[13] = -(r[1] * tx + r[4] * ty + r[7] * tz);
        out[14] = -(r[2] * tx + r[5] * ty + r[8] * tz);
        out[15] = 1.0f;
    }

    // 4x4 to the kernels' 3x4 (axes then translation)
    void to3x4(const float* m, float* out){
        out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
        out[3] = m[4]; out[4] = m[5]; out[5] = m[6];
        out[6] = m[8]; out[7] = m[9]; out[8] = m[10];
        out[9] = m[12]; out[10] = m[13]; out[11] = m[14];
    }

    // the rotation of a 4x4 (its axes normalized), 3x3 column-major
    void rotationOf(const float* m, float* out){
        for (int c = 0; c < 3; c++){
            const float x = m[c * 4], y = m[c * 4 + 1], z = m[c * 4 + 2];
            const float len = std::sqrt(x * x + y * y + z * z);
            const float inv = len > 1.0e-20f ? 1.0f / len : 0.0f;
            out[c * 3] = x * inv;
            out[c * 3 + 1] = y * inv;
            out[c * 3 + 2] = z * inv;
        }
    }

    // the quaternion (x, y, z, w) of a rotation matrix (3x3 column-major)
    void quatOf(const float* m, float q[4]){
        const float trace = m[0] + m[4] + m[8];
        if (trace > 0.0f){
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q[3] = 0.25f * s;
            q[0] = (m[5] - m[7]) / s;
            q[1] = (m[6] - m[2]) / s;
            q[2] = (m[1] - m[3]) / s;
        }else if (m[0] > m[4] && m[0] > m[8]){
            const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
            q[3] = (m[5] - m[7]) / s;
            q[0] = 0.25f * s;
            q[1] = (m[3] + m[1]) / s;
            q[2] = (m[6] + m[2]) / s;
        }else if (m[4] > m[8]){
            const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
            q[3] = (m[6] - m[2]) / s;
            q[0] = (m[3] + m[1]) / s;
            q[1] = 0.25f * s;
            q[2] = (m[7] + m[5]) / s;
        }else{
            const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
            q[3] = (m[1] - m[3]) / s;
            q[0] = (m[6] + m[2]) / s;
            q[1] = (m[7] + m[5]) / s;
            q[2] = 0.25f * s;
        }
    }

    void transpose3(const float* m, float* out){
        out[0] = m[0]; out[1] = m[3]; out[2] = m[6];
        out[3] = m[1]; out[4] = m[4]; out[5] = m[7];
        out[6] = m[2]; out[7] = m[5]; out[8] = m[8];
    }

    void point4(const float* m, const float in[3], float out[3]){
        const float x = in[0], y = in[1], z = in[2];
        out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
        out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }

    void dir4(const float* m, const float in[3], float out[3]){
        const float x = in[0], y = in[1], z = in[2];
        out[0] = m[0] * x + m[4] * y + m[8] * z;
        out[1] = m[1] * x + m[5] * y + m[9] * z;
        out[2] = m[2] * x + m[6] * y + m[10] * z;
    }

    void dir3(const float* m, const float in[3], float out[3]){
        const float x = in[0], y = in[1], z = in[2];
        out[0] = m[0] * x + m[3] * y + m[6] * z;
        out[1] = m[1] * x + m[4] * y + m[7] * z;
        out[2] = m[2] * x + m[5] * y + m[8] * z;
    }

    uint32_t rint32(float v){ return (uint32_t)std::max(0.0f, std::floor(v + 0.5f)); }

}

EmitterRuntime::EmitterRuntime(){
    std::memcpy(prevFrame, kIdentity, sizeof(prevFrame));
    std::memcpy(curFrame, kIdentity, sizeof(curFrame));
    std::memcpy(tickTransform, kIdentity, sizeof(tickTransform));
    std::memcpy(lastTickTransform, kIdentity, sizeof(lastTickTransform));
    std::memcpy(simWorld, kIdentity, sizeof(simWorld));
    std::memcpy(worldSim, kIdentity, sizeof(worldSim));
}

EmitterRuntime::~EmitterRuntime(){
    unbind();
}

void EmitterRuntime::bind(std::shared_ptr<const CompiledEffect> effect, uint32_t capacityOverride){
    compiled = std::move(effect);
    prog = compiled && compiled->valid ? &compiled->program : nullptr;
    if (!prog){
        channels.release(0.0);
        return;
    }
    const uint32_t cap = capacityOverride ? std::min(capacityOverride, prog->maxParticles) : prog->capacityHint();
    configureStorage(cap);
    if (prog->trails.enabled && prog->trails.mode == TrailMode::PerParticle){
        const uint32_t trailed = (uint32_t)std::ceil((double)prog->maxParticles * std::clamp(prog->trails.ratio, 0.0f, 1.0f));
        // as many again for the trails of dead particles fading out
        trailStore.configure(std::max(2u * trailed, 1u), prog->trails.maxPoints);
    }else{
        trailStore.configure(0, 2);
    }
    fieldsSim.reserve(kMaxForceFields);
    pendingEmits.reserve(16);
    requests.reserve(64);
    requestsProcessing.reserve(64);
    triggers.reserve(16);
    collisions.reserve(prog->maxCollisionEvents);
    deaths.reserve(kMaxDeathEvents);
    subEmits.reserve(64);
    sounds.reserve(kMaxSoundEvents);
    burstGroups.reserve(kMaxBursts * 8);
    collisionContacts = false;
    for (const Op& op : prog->updateOps){
        if (op.code == OpCode::Collision && prog->param<CollisionParams>(op.params).events)
            collisionContacts = true;
    }
    // the shared planes' grid cells: 2 m, or more for fast effects (a few ticks of the fastest start)
    const float tick = prog->tickRate > 0 ? 1.0f / prog->tickRate : (float)kMaxVariableStep;
    planeCellInv = 1.0f / std::clamp(prog->maxSpeedHint * tick * 4.0f, 2.0f, 16.0f);
    reset(seed);
}

bool EmitterRuntime::rebind(std::shared_ptr<const CompiledEffect> effect){
    if (!prog || !effect || !effect->valid || !prog->sameStructure(effect->program))
        return false;
    // the children are bound per entry: what an entry names is structure, its other fields are
    // read when it fires
    const std::vector<SubEmitterEntry>& before = compiled->document.subEmitters;
    const std::vector<SubEmitterEntry>& after = effect->document.subEmitters;
    if (prog->subEmitters){
        if (before.size() != after.size())
            return false;
        for (size_t e = 0; e < before.size(); e++){
            // what an entry compiles into its child (its channels) is structure too
            if (before[e].effect != after[e].effect || before[e].inheritRotation != after[e].inheritRotation ||
                before[e].followParent != after[e].followParent || before[e].followRotation != after[e].followRotation ||
                before[e].followScale != after[e].followScale)
                return false;
        }
    }
    compiled = std::move(effect);
    prog = &compiled->program;
    return true;
}

void EmitterRuntime::setChildren(std::vector<std::unique_ptr<EmitterRuntime>> list){
    children = std::move(list);
    for (size_t i = 0; i < children.size(); i++){
        if (children[i]){
            children[i]->playState = PlayState::Playing;
            children[i]->emitting = false;
            children[i]->reset(childSeed(seed, (uint32_t)i));
        }
    }
}

void EmitterRuntime::bindSubEmitters(const SubEmitterResolver& resolve, int depth){
    std::vector<std::unique_ptr<EmitterRuntime>> list;
    // three levels at most: the effect, its children and theirs
    if (prog && prog->subEmitters && depth < kMaxSubEmitterDepth){
        for (const SubEmitterEntry& entry : compiled->document.subEmitters){
            std::shared_ptr<const CompiledEffect> effect = resolve(entry);
            if (!effect || !effect->valid){
                list.push_back(nullptr);
                continue;
            }
            auto child = std::make_unique<EmitterRuntime>();
            child->bind(std::move(effect));
            child->bindSubEmitters(resolve, depth + 1);
            list.push_back(std::move(child));
        }
    }
    setChildren(std::move(list));
}

bool EmitterRuntime::rebindSubEmitters(const SubEmitterResolver& resolve, int depth){
    if (!prog || !prog->subEmitters || depth >= kMaxSubEmitterDepth)
        return children.empty();
    const std::vector<SubEmitterEntry>& entries = compiled->document.subEmitters;
    if (entries.size() != children.size())
        return false;
    for (size_t e = 0; e < entries.size(); e++){
        std::shared_ptr<const CompiledEffect> effect = resolve(entries[e]);
        const bool readable = effect && effect->valid;
        if (readable != (children[e] != nullptr))
            return false;
        if (readable && (!children[e]->rebind(std::move(effect)) || !children[e]->rebindSubEmitters(resolve, depth + 1)))
            return false;
    }
    return true;
}

void EmitterRuntime::unbind(){
    channels.release(0.0);
    trailStore.configure(0, 2);
    children.clear();
    compiled.reset();
    prog = nullptr;
}

void EmitterRuntime::configureStorage(uint32_t newCapacity){
    channels.configure(prog->channels, newCapacity, StoragePool::shared(), 0.0);
    deadStorage.assign(newCapacity + 8, 0u);
    deadList.reserve(newCapacity + 8);
    if (prog->hasCollision && (prog->collisionKind == CollisionKind::World || prog->collisionKind == CollisionKind::World2D)){
        rayStorage.assign(newCapacity + 8, RayRequest());
        chosen.reserve(newCapacity + 8);
    }else{
        rayStorage.clear();
    }
}

void EmitterRuntime::growTo(uint32_t needed){
    if (needed <= channels.capacity() || channels.capacity() >= prog->maxParticles)
        return;
    uint32_t cap = std::max(channels.capacity(), 64u);
    while (cap < needed) cap *= 2;
    cap = std::min(cap, prog->maxParticles);
    // a new block, the particles copied over (outside the steady state)
    ParticleChannels::Snapshot copy;
    channels.save(copy);
    configureStorage(cap);
    channels.restore(copy);
}

uint32_t EmitterRuntime::childSeed(uint32_t parent, uint32_t index){
    return hashRandom(parent, index + 1);
}

void EmitterRuntime::reset(uint32_t newSeed){
    seed = newSeed;
    channels.count = 0;
    if (trailStore.ringCount() > 0) trailStore.clear();
    spawnCounter = 0;
    ticks = 0;
    elapsed = 0.0;
    cycleTime = prog ? -pickStartDelay() : 0.0;
    arcPhase = radiusPhase = arcRate = radiusRate = 0.0f;
    noiseScroll = 0.0f;
    for (int c = 0; c < 3; c++){ lastTickVelocity[c] = 0.0f; tickVelocityDelta[c] = 0.0f; }
    spreadBase = spreadCount = 0;
    burstCheckedTo = -1.0;
    loopIndex = 0;
    accumulator = 0.0;
    dropped = 0.0;
    rateAccumulator = 0.0;
    distanceAccumulator = 0.0;
    ticksThisFrame = 0;
    alpha = 1.0f;
    lastTick = 0.0f;
    started = false;
    boundsValid = false;
    prewarm = SeekState();
    pendingEmits.clear();
    requests.clear();
    triggers.clear();
    collisions.clear();
    deaths.clear();
    subEmits.clear();
    sounds.clear();
    std::fill(std::begin(planeGrid), std::end(planeGrid), CachedPlane());
    for (size_t i = 0; i < children.size(); i++){
        if (children[i]) children[i]->reset(childSeed(newSeed, (uint32_t)i));
    }
}

double EmitterRuntime::pickStartDelay() const{
    ScalarDriver d;
    d.seed = seed;
    return std::max(0.0, (double)curveScalar(*prog, prog->startDelay, d));
}

// ---------------------------------------------------------------- playback

void EmitterRuntime::play(){
    if (!prog)
        return;
    if (playState == PlayState::Paused){
        playState = PlayState::Playing;
        return;
    }
    if (playState == PlayState::Playing && emitting)
        return;
    if (playState == PlayState::Stopped || !emitting){
        if (playState == PlayState::Stopped)
            reset(seed);
        emitting = true;
        playState = PlayState::Playing;
        if (prog->prewarm){
            prewarm.pending = true;
            prewarm.tick = prog->tickRate > 0 ? 1.0 / prog->tickRate : kMaxVariableStep;
            prewarm.target = prog->duration;
            prewarm.total = (uint32_t)std::floor((prewarm.target + prewarm.tick * 1e-6) / prewarm.tick);
            prewarm.completed = 0;
        }
    }
}

void EmitterRuntime::pause(){
    if (playState == PlayState::Playing)
        playState = PlayState::Paused;
}

void EmitterRuntime::stop(bool clear){
    emitting = false;
    if (clear){
        reset(seed);
        playState = PlayState::Stopped;
    }
}

void EmitterRuntime::clear(){
    channels.count = 0;
    if (trailStore.ringCount() > 0) trailStore.clear();
    boundsValid = false;
    collisions.clear();
    deaths.clear();
    subEmits.clear();
    for (auto& child : children){
        if (child) child->clear();
    }
}

void EmitterRuntime::restart(){
    reset(seed);
    playState = PlayState::Stopped;
    play();
}

bool EmitterRuntime::isEmitting() const{
    return playState != PlayState::Stopped && emitting;
}

uint32_t EmitterRuntime::totalCount() const{
    uint32_t total = channels.count;
    for (const auto& c : children){
        if (c) total += c->totalCount();
    }
    return total;
}

uint32_t EmitterRuntime::totalCapacity() const{
    uint32_t total = channels.capacity();
    for (const auto& c : children){
        if (c) total += c->totalCapacity();
    }
    return total;
}

uint32_t EmitterRuntime::totalMaxParticles() const{
    uint32_t total = prog ? prog->maxParticles : 0u;
    for (const auto& c : children){
        if (c) total += c->totalMaxParticles();
    }
    return total;
}

uint32_t EmitterRuntime::treeCapacityHint() const{
    if (!prog)
        return 0u;
    const uint32_t own = prog->capacityHint();
    if (children.empty())
        return own;
    // the children from the parents' peak itself: the storage hint's padding would multiply
    const uint64_t parents = (uint64_t)std::ceil(prog->peakEstimate());
    return (uint32_t)std::min<uint64_t>((uint64_t)own + childrenCapacityHint(parents), UINT32_MAX);
}

uint64_t EmitterRuntime::childrenCapacityHint(uint64_t parents) const{
    const std::vector<SubEmitterEntry>& entries = compiled->document.subEmitters;
    uint64_t total = 0;
    for (size_t e = 0; e < children.size() && e < entries.size(); e++){
        const EmitterRuntime* child = children[e].get();
        if (!child || !child->prog || entries[e].emitCount <= 0)
            continue;
        const double asked = (double)parents * (double)entries[e].emitCount * (double)std::clamp(entries[e].probability, 0.0f, 1.0f);
        const uint64_t held = (uint64_t)std::min(std::ceil(asked), (double)child->prog->maxParticles);
        total += held + child->childrenCapacityHint(held);
    }
    return total;
}

bool EmitterRuntime::isAlive() const{
    if (channels.count > 0 || trailStore.usedRings() > 0)
        return true;
    for (const auto& c : children){
        if (c && c->isAlive()) return true;
    }
    return false;
}

void EmitterRuntime::emit(uint32_t n, const EmitOverrides* overrides){
    if (!prog || n == 0)
        return;
    PendingEmit e;
    e.count = n;
    if (overrides){
        e.hasOverrides = true;
        e.overrides = *overrides;
    }
    pendingEmits.push_back(e);
    // emitting from a stopped system plays it (without its own emission)
    if (playState == PlayState::Stopped){
        playState = PlayState::Playing;
        emitting = false;
    }
}

void EmitterRuntime::sendTrigger(uint32_t name){
    if (!prog)
        return;
    const ParticleEffectDocument& d = compiled->document;
    for (size_t t = 0; t < d.triggers.size(); t++){
        const EffectTrigger& trigger = d.triggers[t];
        if (nameHash(trigger.name) != name)
            continue;
        switch (trigger.action){
            case TriggerAction::Play: play(); break;
            case TriggerAction::Pause: pause(); break;
            case TriggerAction::Stop: stop(false); break;
            case TriggerAction::StopAndClear: stop(true); break;
            case TriggerAction::None: break;
        }
        if (!trigger.soundEvent.empty())
            recordSound(ParticleSoundEvent::Source::Trigger, (uint32_t)t, nullptr);
    }
    // its Trigger sub-emitters fire on the next tick (after the actions: a play resets)
    bool fires = false;
    if (prog->subEmitters){
        for (const SubEmitterEntry& entry : d.subEmitters)
            fires = fires || (entry.trigger == SubEmitterTrigger::Trigger && nameHash(entry.triggerName) == name);
    }
    if (!fires || triggers.size() >= 64)
        return;
    triggers.push_back(name);
    // a stopped emitter runs that tick, without its own emission (as emit() does)
    if (playState == PlayState::Stopped){
        playState = PlayState::Playing;
        emitting = false;
    }
}

void EmitterRuntime::recordSound(ParticleSoundEvent::Source source, uint32_t index, const float* position){
    if (!recordEffects || sounds.size() >= kMaxSoundEvents)
        return;
    ParticleSoundEvent event;
    event.source = source;
    event.index = (uint16_t)index;
    event.atEmitter = position == nullptr;
    if (position)
        std::memcpy(event.position, position, sizeof(event.position));
    sounds.push_back(event);
}

bool EmitterRuntime::queueSpawn(const SpawnRequest& request){
    if (requests.size() >= kMaxSpawnRequests)
        return false;
    requests.push_back(request);
    return true;
}

// ---------------------------------------------------------------- frames and ticks

uint32_t EmitterRuntime::limit(const EmitterInput& input) const{
    // what its children hold comes out of the budget it shares with them
    uint32_t held = 0;
    for (const auto& c : children){
        if (c) held += c->totalCount();
    }
    const uint32_t budget = input.budget > held ? input.budget - held : 0u;
    return std::min({prog->maxParticles, budget, kMaxParticlesPerEmitter});
}

uint32_t EmitterRuntime::planFrame(const EmitterInput& input, double dt, uint32_t maxTicks){
    ticksThisFrame = 0;
    // the events a frame reports gather over all its ticks (caps per frame)
    clearEvents();
    if (!prog || playState != PlayState::Playing)
        return 0;
    std::memcpy(curFrame, input.transform ? input.transform : kIdentity, sizeof(curFrame));
    if (!started){
        std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
        std::memcpy(tickTransform, curFrame, sizeof(tickTransform));
        std::memcpy(lastTickTransform, curFrame, sizeof(lastTickTransform));
        started = true;
        if (prewarm.pending && prog->prewarmSync){
            // all of it now
            continueSeekTicks(input, prewarm, 1.0e30);
        }
    }
    if (prewarm.pending){
        // spread over the first frames at most 1 ms each; the effect is hidden meanwhile
        continueSeekTicks(input, prewarm, 1.0);
        std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
        return 0;
    }
    if (!(dt > 0.0) || !std::isfinite(dt))
        return 0;
    const double speed = std::max(0.0, (double)input.speed * prog->simulationSpeed);
    const double scaled = dt * speed;
    if (scaled <= 0.0)
        return 0;
    if (prog->tickRate > 0){
        const double tick = 1.0 / prog->tickRate;
        frameCarried = accumulator;
        accumulator += scaled;
        const double available = std::floor((accumulator + tick * 1e-6) / tick);
        const uint32_t steps = (uint32_t)std::min<double>(available, maxTicks);
        dropped += (available - steps) * tick;
        accumulator = std::max(0.0, accumulator - available * tick);
        frameTick = tick;
        frameScaled = scaled;
        ticksThisFrame = steps;
    }else{
        const double accepted = std::min(scaled, kMaxVariableStep * maxTicks);
        const uint32_t steps = (uint32_t)std::max(1.0, std::ceil(accepted / kMaxVariableStep - 1e-9));
        dropped += scaled - accepted;
        frameTick = accepted / steps;
        frameScaled = accepted;
        frameCarried = 0.0;
        accumulator = 0.0;
        ticksThisFrame = steps;
    }
    return ticksThisFrame;
}

void EmitterRuntime::endFrame(){
    if (!prog)
        return;
    std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
    if (prog->tickRate > 0 && prog->interpolate && frameTick > 0.0)
        alpha = (float)std::clamp(accumulator / frameTick, 0.0, 1.0);
    else
        alpha = 1.0f;
    shareInterpolation();
}

void EmitterRuntime::shareInterpolation(){
    for (auto& c : children){
        if (!c || !c->prog)
            continue;
        c->alpha = c->prog->interpolate ? alpha : 1.0f;
        c->shareInterpolation();
    }
}

void EmitterRuntime::setFrameTransforms(const EmitterInput& input, double fraction){
    std::memcpy(lastTickTransform, tickTransform, sizeof(tickTransform));
    lerpMatrix(prevFrame, curFrame, (float)std::clamp(fraction, 0.0, 1.0), tickTransform);
    if (prog->space == SimulationSpace::World){
        std::memcpy(simWorld, kIdentity, sizeof(simWorld));
        std::memcpy(worldSim, kIdentity, sizeof(worldSim));
    }else{
        std::memcpy(simWorld, tickTransform, sizeof(simWorld));
        inverseAffine(tickTransform, worldSim);
    }
    const float dtc = tickDt > 0.0f ? tickDt : 1.0f;
    float moved[3] = {tickTransform[12] - lastTickTransform[12], tickTransform[13] - lastTickTransform[13], tickTransform[14] - lastTickTransform[14]};
    tickDistance = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    for (int c = 0; c < 3; c++) emitterVelocity[c] = moved[c] / dtc;
    if (input.hasVelocity && prog->emitterVelocityMode == EmitterVelocityMode::Rigidbody){
        // the body's velocity: what Inherit Velocity takes and Rate over Distance counts
        for (int c = 0; c < 3; c++) emitterVelocity[c] = input.velocity[c];
        tickDistance = std::sqrt(input.velocity[0] * input.velocity[0] + input.velocity[1] * input.velocity[1] +
                                 input.velocity[2] * input.velocity[2]) * tickDt;
    }
    for (int c = 0; c < 3; c++) tickVelocityDelta[c] = started ? emitterVelocity[c] - lastTickVelocity[c] : 0.0f;

    // force fields and collision planes into simulation space
    fieldsSim.clear();
    for (uint32_t f = 0; f < input.fieldCount && f < kMaxForceFields; f++){
        ForceFieldSample s = input.fields[f];
        if (prog->space == SimulationSpace::Local){
            // field space from simulation space: toField(world) * simWorld
            float fieldWorld[16] = {s.toField[0], s.toField[1], s.toField[2], 0, s.toField[3], s.toField[4], s.toField[5], 0,
                                    s.toField[6], s.toField[7], s.toField[8], 0, s.toField[9], s.toField[10], s.toField[11], 1};
            float composed[16];
            for (int col = 0; col < 4; col++){
                for (int row = 0; row < 4; row++){
                    float v = 0.0f;
                    for (int k = 0; k < 4; k++) v += fieldWorld[k * 4 + row] * simWorld[col * 4 + k];
                    composed[col * 4 + row] = v;
                }
            }
            to3x4(composed, s.toField);
            float c[3];
            worldToSimPoint(s.center, c);
            std::memcpy(s.center, c, sizeof(c));
            worldToSimDir(s.axis, c);
            const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            for (int k = 0; k < 3; k++) s.axis[k] = len > 0.0f ? c[k] / len : 0.0f;
        }
        fieldsSim.push_back(s);
    }
    planeCount = 0;
    for (const Op& op : prog->updateOps){
        if (op.code != OpCode::Collision)
            continue;
        const CollisionParams& cp = prog->param<CollisionParams>(op.params);
        for (uint32_t p = 0; p < cp.planeCount && planeCount < kMaxCollisionPlanes; p++)
            std::memcpy(planesSim[planeCount++], cp.planes[p], sizeof(float) * 4);
    }
    for (uint32_t p = 0; p < input.planeCount && planeCount < kMaxCollisionPlanes; p++){
        // a world plane (n, d) into simulation space: n' = R^T n, d' = d + n . t
        const float* wp = input.planes[p];
        float n[3], point[3];
        const float onPlane[3] = {-wp[0] * wp[3], -wp[1] * wp[3], -wp[2] * wp[3]};
        worldToSimDir(wp, n);
        worldToSimPoint(onPlane, point);
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 0.0f)
            continue;
        for (int k = 0; k < 3; k++) n[k] /= len;
        planesSim[planeCount][0] = n[0];
        planesSim[planeCount][1] = n[1];
        planesSim[planeCount][2] = n[2];
        planesSim[planeCount][3] = -(n[0] * point[0] + n[1] * point[1] + n[2] * point[2]);
        planeCount++;
    }
    worldCollision = input.collision;
    simCollision.context = this;
    simCollision.terrain = (worldCollision && worldCollision->terrain) ? &EmitterRuntime::simTerrain : nullptr;
    simCollision.ray = (worldCollision && worldCollision->ray) ? &EmitterRuntime::simRay : nullptr;
}

void EmitterRuntime::simToWorldPoint(const float in[3], float out[3]) const{ point4(simWorld, in, out); }
void EmitterRuntime::simToWorldDir(const float in[3], float out[3]) const{ dir4(simWorld, in, out); }
void EmitterRuntime::worldToSimPoint(const float in[3], float out[3]) const{ point4(worldSim, in, out); }
void EmitterRuntime::worldToSimDir(const float in[3], float out[3]) const{ dir4(worldSim, in, out); }

bool EmitterRuntime::simTerrain(const void* context, const float point[3], float surface[3], float normal[3]){
    const EmitterRuntime& self = *static_cast<const EmitterRuntime*>(context);
    float wp[3], ws[3], wn[3];
    self.simToWorldPoint(point, wp);
    if (!self.worldCollision->terrain(self.worldCollision->context, wp, ws, wn))
        return false;
    self.worldToSimPoint(ws, surface);
    float n[3];
    self.worldToSimDir(wn, n);
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) normal[k] = len > 0.0f ? n[k] / len : (k == 1 ? 1.0f : 0.0f);
    return true;
}

bool EmitterRuntime::simRay(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, CollisionHit& hit){
    const EmitterRuntime& self = *static_cast<const EmitterRuntime*>(context);
    float wf[3], wt[3];
    self.simToWorldPoint(from, wf);
    self.simToWorldPoint(to, wt);
    CollisionHit worldHit;
    if (!self.worldCollision->ray(self.worldCollision->context, wf, wt, radius, layers, worldHit))
        return false;
    self.worldToSimPoint(worldHit.position, hit.position);
    float n[3];
    self.worldToSimDir(worldHit.normal, n);
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) hit.normal[k] = len > 0.0f ? n[k] / len : (k == 1 ? 1.0f : 0.0f);
    return true;
}

void EmitterRuntime::beginTick(const EmitterInput& input, uint32_t index){
    currentInput = &input;
    tickDt = (float)frameTick;
    if (prog->shapePhases || prog->noiseScroll){
        // the speeds over the emitter time, one pick per play (spawn index 0 of the seed)
        ScalarDriver d;
        d.seed = seed;
        d.emitterTime = prog->duration > 0.0f ? (float)std::clamp(cycleTime / prog->duration, 0.0, 1.0) : 0.0f;
        if (prog->shapePhases){
            arcRate = curveScalar(*prog, prog->arcSpeed, d);
            radiusRate = curveScalar(*prog, prog->radiusSpeed, d);
        }
        if (prog->noiseScroll)
            noiseScroll += curveScalar(*prog, prog->noiseScrollSpeed, d) * tickDt;
    }
    double fraction = 1.0;
    if (frameScaled > 0.0){
        fraction = prog->tickRate > 0 ? ((double)(index + 1) * frameTick - frameCarried) / frameScaled
                                      : (double)(index + 1) / std::max(ticksThisFrame, 1u);
    }
    setFrameTransforms(input, fraction);
    activeChunkSize = std::max(4u, input.chunkSize & ~3u);
    const uint32_t chunks = chunkCount();
    if (chunkOut.size() < chunks)
        chunkOut.resize(chunks);
    // a chunk holds every contact of its particles (one each a tick), so what is kept never
    // depends on the chunking: the caps apply afterwards, in particle order
    const uint32_t maxEvents = collisionContacts ? activeChunkSize : 0u;
    if (maxEvents && eventStorage.size() < (size_t)chunks * maxEvents)
        eventStorage.resize((size_t)chunks * maxEvents);
    for (uint32_t c = 0; c < chunks; c++){
        ChunkOutput& o = chunkOut[c];
        o = ChunkOutput();
        o.dead = deadStorage.data() + (size_t)c * activeChunkSize;
        o.rays = rayStorage.empty() ? nullptr : rayStorage.data() + (size_t)c * activeChunkSize;
        o.events = maxEvents ? eventStorage.data() + (size_t)c * maxEvents : nullptr;
        o.maxEvents = maxEvents;
    }
}

uint32_t EmitterRuntime::chunkCount() const{
    return (channels.count + activeChunkSize - 1) / activeChunkSize;
}

void EmitterRuntime::fillContext(KernelContext& ctx, const EmitterInput* input) const{
    for (int c = 0; c < ChCount; c++) ctx.ch[c] = channels.get((Channel)c);
    ctx.program = prog;
    ctx.pool = reinterpret_cast<const unsigned char*>(prog->pool.data());
    ctx.seed = seed;
    ctx.tick = (uint32_t)ticks;
    ctx.dt = tickDt;
    ctx.emitterTime = prog->duration > 0.0f ? (float)std::clamp(cycleTime / prog->duration, 0.0, 1.0) : 0.0f;
    ctx.time = (float)elapsed;
    float worldToSimRot[9];
    if (prog->space == SimulationSpace::World){
        to3x4(tickTransform, ctx.emitterToSim);
        to3x4(lastTickTransform, ctx.prevEmitterToSim);
        rotationOf(tickTransform, ctx.emitterToSimDir);
        rotationOf(lastTickTransform, ctx.prevEmitterToSimDir);
        quatOf(ctx.emitterToSimDir, ctx.emitterToSimQuat);
        const float identity3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(ctx.worldToSimDir, identity3, sizeof(identity3));
    }else{
        const float identity34[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        const float identity3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(ctx.emitterToSim, identity34, sizeof(identity34));
        std::memcpy(ctx.prevEmitterToSim, identity34, sizeof(identity34));
        std::memcpy(ctx.emitterToSimDir, identity3, sizeof(identity3));
        std::memcpy(ctx.prevEmitterToSimDir, identity3, sizeof(identity3));
        float rot[9];
        rotationOf(tickTransform, rot);
        transpose3(rot, worldToSimRot);
        std::memcpy(ctx.worldToSimDir, worldToSimRot, sizeof(worldToSimRot));
    }
    const float* g = input ? input->gravity : nullptr;
    const float gravity[3] = {g ? g[0] : 0.0f, g ? g[1] : -9.81f, g ? g[2] : 0.0f};
    dir3(ctx.worldToSimDir, gravity, ctx.gravity);
    float vel[3];
    dir3(ctx.worldToSimDir, emitterVelocity, vel);
    std::memcpy(ctx.emitterVelocity, vel, sizeof(vel));
    dir3(ctx.worldToSimDir, tickVelocityDelta, vel);
    std::memcpy(ctx.emitterVelocityDelta, vel, sizeof(vel));
    ctx.arcPhase = arcPhase;
    ctx.arcRate = arcRate;
    ctx.radiusPhase = radiusPhase;
    ctx.radiusRate = radiusRate;
    ctx.spreadBase = spreadBase;
    ctx.spreadCount = spreadCount;
    ctx.noiseScroll = noiseScroll;
    ctx.sizeScale = input ? input->sizeScale : 1.0f;
    ctx.fields = fieldsSim.data();
    ctx.fieldCount = (uint32_t)fieldsSim.size();
    ctx.collision = (simCollision.terrain || simCollision.ray) ? &simCollision : nullptr;
    std::memcpy(ctx.collisionPlanes, planesSim, sizeof(planesSim));
    ctx.collisionPlaneCount = planeCount;
    ctx.planeGrid = planeGrid;
    ctx.planeCellInv = planeCellInv;
}

void EmitterRuntime::updateChunk(uint32_t chunk){
    const uint32_t begin = chunk * activeChunkSize;
    const uint32_t end = std::min(begin + activeChunkSize, channels.count);
    if (begin >= end)
        return;
    ThreadScratch& scratch = threadScratch();
    KernelContext ctx;
    fillContext(ctx, currentInput);
    ctx.scratch = &scratch.chunk;
    ctx.out = &chunkOut[chunk];
    // the previous tick's values, for interpolation and the collision rays
    const uint32_t n = end - begin;
    std::memcpy(ctx.ch[ChPrevX] + begin, ctx.ch[ChPosX] + begin, n * sizeof(float));
    std::memcpy(ctx.ch[ChPrevY] + begin, ctx.ch[ChPosY] + begin, n * sizeof(float));
    std::memcpy(ctx.ch[ChPrevZ] + begin, ctx.ch[ChPosZ] + begin, n * sizeof(float));
    if (ctx.ch[ChPrevRotation])
        std::memcpy(ctx.ch[ChPrevRotation] + begin, ctx.ch[ChRotation] + begin, n * sizeof(float));
    if (ctx.ch[ChPrevSizeX])
        std::memcpy(ctx.ch[ChPrevSizeX] + begin, ctx.ch[ChSizeX] + begin, n * sizeof(float));
    scratch.chunk.hasAccel = false;
    scratch.chunk.hasAnimated = false;
    const KernelTable& kernels = activeKernels();
    for (const Op& op : prog->updateOps)
        kernels.update(ctx, op, begin, end);
}

void EmitterRuntime::clearEvents(){
    collisions.clear();
    deaths.clear();
    subEmits.clear();
}

void EmitterRuntime::finishTick(const EmitterInput& input){
    const uint32_t chunks = chunkCount();
    worldCollisionStage(input);
    // collision events in chunk order: Lua and the Collision sub-emitters
    for (uint32_t c = 0; c < chunks; c++){
        const ChunkOutput& o = chunkOut[c];
        for (uint32_t e = 0; e < o.eventCount; e++){
            const CollisionEvent& ev = o.events[e];
            if (recordEffects && prog->collisionEvents && collisions.size() < prog->maxCollisionEvents){
                CollisionEvent world = ev;
                simToWorldPoint(ev.position, world.position);
                simToWorldDir(ev.normal, world.normal);
                simToWorldDir(ev.velocity, world.velocity);
                collisions.push_back(world);
            }
            if (prog->subEmitters)
                raiseSubEmitters(SubEmitterTrigger::Collision, ev.index, ev.normal);
        }
    }
    // the bounds of the survivors
    boundsValid = false;
    for (int k = 0; k < 3; k++){ boundsLo[k] = 3.0e38f; boundsHi[k] = -3.0e38f; }
    for (uint32_t c = 0; c < chunks; c++){
        const ChunkOutput& o = chunkOut[c];
        for (int k = 0; k < 3; k++){
            boundsLo[k] = std::min(boundsLo[k], o.boundsMin[k]);
            boundsHi[k] = std::max(boundsHi[k], o.boundsMax[k]);
        }
    }
    deathStage();
    compact();
    emissionStage(input);
    trailStage();
    for (int k = 0; k < 3; k++)
        boundsValid = boundsValid || boundsLo[k] <= boundsHi[k];

    lastTick = tickDt;
    elapsed += tickDt;
    arcPhase += arcRate * tickDt;
    radiusPhase += radiusRate * tickDt;
    std::memcpy(lastTickVelocity, emitterVelocity, sizeof(lastTickVelocity));
    if (emitting){
        // the cycle wraps after the tick that starts at or past its end (the old system's
        // order: its bursts at the start come on the next tick), then the clock moves
        if (prog->looping && prog->duration > 0.0f){
            while (cycleTime >= prog->duration){
                cycleTime -= prog->duration;
                loopIndex++;
                burstCheckedTo = -1.0;
            }
        }
        cycleTime += tickDt;
        if (!prog->looping && cycleTime >= prog->duration)
            emitting = false;
    }
    ticks++;
    childrenStage(input);
    // a finished effect stops (a looping one only by stop())
    if (!emitting && pendingEmits.empty() && requests.empty() && !isAlive())
        playState = PlayState::Stopped;
}

void EmitterRuntime::worldCollisionStage(const EmitterInput& input){
    lastRays = 0;
    if (rayStorage.empty() || !worldCollision || !worldCollision->ray)
        return;
    const CollisionParams* cp = nullptr;
    for (const Op& op : prog->updateOps){
        if (op.code == OpCode::Collision) cp = &prog->param<CollisionParams>(op.params);
    }
    if (!cp)
        return;
    // the requests capped per tick: the most overdue first (the particles without a plane,
    // newborns among them, then the oldest planes), so every particle gets its turn within
    // about total / cap ticks; ties by slot, so the choice is the same whatever the chunking
    const uint32_t chunks = chunkCount();
    uint32_t total = 0;
    for (uint32_t c = 0; c < chunks; c++) total += chunkOut[c].rayCount;
    if (total == 0)
        return;
    const uint32_t served = std::min(total, cp->maxQueries);
    if (chosen.size() < total) chosen.resize(total);
    {
        uint32_t n = 0;
        for (uint32_t c = 0; c < chunks; c++){
            for (uint32_t r = 0; r < chunkOut[c].rayCount; r++) chosen[n++] = &chunkOut[c].rays[r];
        }
        const uint32_t* planeTicks = channels.getU(ChPlaneTick);
        auto overdue = [planeTicks](const RayRequest* a, const RayRequest* b){
            const uint32_t ta = planeTicks[a->particle], tb = planeTicks[b->particle];
            return ta != tb ? ta < tb : a->particle < b->particle;
        };
        if (served < total)
            std::nth_element(chosen.begin(), chosen.begin() + served, chosen.begin() + total, overdue);
        // in slot order: the results are applied in the same order every run
        std::sort(chosen.begin(), chosen.begin() + served, [](const RayRequest* a, const RayRequest* b){ return a->particle < b->particle; });
    }
    // the probes look ahead along the motion, so the plane found (or the clear path) serves until
    // the particle's next turn: from where it was last tick to where its velocity and its
    // acceleration of the last tick (gravity, forces, drag) take it. Under a constant
    // acceleration the path is a parabola whose lowest point along the acceleration is an end,
    // so a surface it crosses is crossed by the chord as well.
    // a particle waits at most about total / served ticks past its plane's age for its next turn
    const uint32_t period = (total + served - 1) / std::max(served, 1u);
    const float lookaheadTicks = (float)(period + cp->planeCacheTicks) * 1.5f;
    const float dt = tickDt > 0.0f ? tickDt : 1.0f;
    const float horizon = lookaheadTicks * dt;
    float* px = channels.get(ChPosX); float* py = channels.get(ChPosY); float* pz = channels.get(ChPosZ);
    float* vx = channels.get(ChVelX); float* vy = channels.get(ChVelY); float* vz = channels.get(ChVelZ);
    auto requestAt = [&](uint32_t k) -> RayRequest& { return *chosen[k]; };
    // the rays (physics reads are thread-safe outside its step: in parallel, results by index)
    if (rayResults.size() < served) rayResults.resize(served);
    jobsOf(input).parallelFor(served, [&](size_t k){
        RayRequest& r = requestAt((uint32_t)k);
        const uint32_t i = r.particle;
        const float v[3] = {vx[i], vy[i], vz[i]};
        float to[3];
        for (int c = 0; c < 3; c++){
            // velocity-Verlet moved it by the mean of the two velocities: twice the difference
            // to the current one is the acceleration
            const float accel = 2.0f * (v[c] - (r.to[c] - r.from[c]) / dt) / dt;
            to[c] = r.to[c] + v[c] * horizon + 0.5f * accel * horizon * horizon;
        }
        RayResult& out = rayResults[k];
        out.hit = simRay(this, r.from, to, r.radius, cp->layerMask, out.h);
    }, jobs::ParallelForOptions{.grain = 16});
    lastRays = served;
    float* age = channels.get(ChAge);
    const float* invLife = channels.get(ChInvLife);
    const uint32_t* spawn = channels.getU(ChSpawnIndex);
    ScalarDriver d;
    d.seed = seed;
    for (uint32_t k = 0; k < served; k++){
        const RayRequest& r = requestAt(k);
        const RayResult& res = rayResults[k];
        const uint32_t i = r.particle;
        if (!res.hit){
            // a clear path: until the particle's next turn it keeps the surface it found before
            // (resting on it), or else an empty plane (never hit)
            channels.getU(ChPlaneTick)[i] = (uint32_t)ticks + 1;
            if (channels.get(ChPlaneD)[i] < kEmptyPlaneD && (channels.get(ChPlaneNX)[i] != 0.0f || channels.get(ChPlaneNY)[i] != 0.0f || channels.get(ChPlaneNZ)[i] != 0.0f))
                continue;
            channels.get(ChPlaneNX)[i] = 0.0f;
            channels.get(ChPlaneNY)[i] = 0.0f;
            channels.get(ChPlaneNZ)[i] = 0.0f;
            channels.get(ChPlaneD)[i] = kEmptyPlaneD;
            continue;
        }
        const float* n = res.h.normal;
        const float dPlane = -(n[0] * res.h.position[0] + n[1] * res.h.position[1] + n[2] * res.h.position[2]);
        // the particle's own plane and its cell's
        channels.get(ChPlaneNX)[i] = n[0];
        channels.get(ChPlaneNY)[i] = n[1];
        channels.get(ChPlaneNZ)[i] = n[2];
        channels.get(ChPlaneD)[i] = dPlane;
        channels.getU(ChPlaneTick)[i] = (uint32_t)ticks + 1;
        // the cells in front of the hit (just off the surface and a quarter cell out: a particle
        // moves less than that a tick, so where it was the tick it crosses is one of them) and
        // the particle's own: the particles that pass through any of them take the plane
        const float cellSize = 1.0f / planeCellInv;
        const float* h = res.h.position;
        const float nearOff = cellSize * 1.0e-3f, farOff = cellSize * 0.25f;
        const uint32_t keys[3] = {Kernels<simd::Scalar>::planeCell(h[0] + n[0] * nearOff, h[1] + n[1] * nearOff, h[2] + n[2] * nearOff, planeCellInv),
                                  Kernels<simd::Scalar>::planeCell(h[0] + n[0] * farOff, h[1] + n[1] * farOff, h[2] + n[2] * farOff, planeCellInv),
                                  Kernels<simd::Scalar>::planeCell(r.from[0], r.from[1], r.from[2], planeCellInv)};
        for (uint32_t key : keys){
            CachedPlane& cell = planeGrid[key & (kPlaneGridSize - 1)];
            cell.key = key;
            cell.tick = (uint32_t)ticks + 1;
            cell.plane[0] = n[0]; cell.plane[1] = n[1]; cell.plane[2] = n[2]; cell.plane[3] = dPlane;
        }
        // inside the surface now (the planes may have answered already): the response
        const float radius = r.radius;
        const float p[3] = {px[i], py[i], pz[i]};
        const float d1 = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + dPlane - radius;
        if (d1 >= 0.0f || age[i] * invLife[i] >= 1.0f)
            continue;
        d.age = age[i];
        d.normAge = age[i] * invLife[i];
        d.spawn = spawn[i];
        d.speed = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        d.emitterTime = 0.0f;
        const float vn = vx[i] * n[0] + vy[i] * n[1] + vz[i] * n[2];
        const float t[3] = {vx[i] - n[0] * vn, vy[i] - n[1] * vn, vz[i] - n[2] * vn};
        const float bounce = curveScalar(*prog, cp->bounce, d);
        const float keep = 1.0f - std::clamp(curveScalar(*prog, cp->dampen, d), 0.0f, 1.0f);
        const float rn = vn < 0.0f ? -vn * bounce : vn;
        vx[i] = (t[0] + n[0] * rn) * keep;
        vy[i] = (t[1] + n[1] * rn) * keep;
        vz[i] = (t[2] + n[2] * rn) * keep;
        const float contact[3] = {p[0] - n[0] * (d1 + radius), p[1] - n[1] * (d1 + radius), p[2] - n[2] * (d1 + radius)};
        px[i] = contact[0] + n[0] * radius;
        py[i] = contact[1] + n[1] * radius;
        pz[i] = contact[2] + n[2] * radius;
        if (!cp->lifetimeLoss.isZero())
            age[i] += std::clamp(curveScalar(*prog, cp->lifetimeLoss, d), 0.0f, 1.0f) / invLife[i];
        const float speed = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        const bool killed = cp->kill || speed < cp->minKillSpeed || speed > cp->maxKillSpeed;
        if (killed){
            age[i] = std::max(age[i], 1.0f / invLife[i] + 1.0f);
            // into its chunk's dead list (ascending)
            ChunkOutput& o = chunkOut[i / activeChunkSize];
            uint32_t pos = o.deadCount;
            while (pos > 0 && o.dead[pos - 1] > i){
                o.dead[pos] = o.dead[pos - 1];
                pos--;
            }
            if (pos == 0 || o.dead[pos - 1] != i){
                o.dead[pos] = i;
                o.deadCount++;
            }else{
                // it was there already: undo the shift
                for (uint32_t q = pos; q < o.deadCount; q++) o.dead[q] = o.dead[q + 1];
            }
        }
        if (cp->events){
            ChunkOutput& o = chunkOut[i / activeChunkSize];
            if (o.eventCount < o.maxEvents){
                CollisionEvent& e = o.events[o.eventCount++];
                std::memcpy(e.position, contact, sizeof(contact));
                std::memcpy(e.normal, n, sizeof(float) * 3);
                e.velocity[0] = vx[i]; e.velocity[1] = vy[i]; e.velocity[2] = vz[i];
                e.particleId = spawn[i];
                e.index = i;
                e.killed = killed;
            }
        }
    }
}

void EmitterRuntime::deathStage(){
    deadList.clear();
    const uint32_t chunks = chunkCount();
    for (uint32_t c = 0; c < chunks; c++){
        const ChunkOutput& o = chunkOut[c];
        deadList.insert(deadList.end(), o.dead, o.dead + o.deadCount);
    }
    if (deadList.empty())
        return;
    const bool trails = trailStore.ringCount() > 0;
    const uint32_t* slots = trails ? channels.getU(ChTrailSlot) : nullptr;
    const float* px = channels.get(ChPosX); const float* py = channels.get(ChPosY); const float* pz = channels.get(ChPosZ);
    const float* vx = channels.get(ChVelX); const float* vy = channels.get(ChVelY); const float* vz = channels.get(ChVelZ);
    const uint32_t* spawn = channels.getU(ChSpawnIndex);
    for (uint32_t i : deadList){
        if (recordEffects && deaths.size() < kMaxDeathEvents){
            ParticleDeathEvent e;
            const float p[3] = {px[i], py[i], pz[i]}, v[3] = {vx[i], vy[i], vz[i]};
            simToWorldPoint(p, e.position);
            simToWorldDir(v, e.velocity);
            e.particleId = spawn[i];
            deaths.push_back(e);
        }
        if (prog->subEmitters)
            raiseSubEmitters(SubEmitterTrigger::Death, i, nullptr);
        if (slots && slots[i] != kNoTrail)
            trailStore.release(slots[i], !prog->trails.dieWithParticles);
    }
}

void EmitterRuntime::compact(){
    // descending: the last particle is never one of the dead still to remove
    for (size_t k = deadList.size(); k > 0; k--){
        const uint32_t i = deadList[k - 1];
        const uint32_t last = channels.count - 1;
        if (i != last)
            channels.move(last, i);
        channels.count--;
    }
}

void EmitterRuntime::emissionStage(const EmitterInput& input){
    const uint32_t room = limit(input) > channels.count ? limit(input) - channels.count : 0u;
    uint32_t budget = room;
    // Trigger sub-emitters: from every live particle
    if (!triggers.empty()){
        if (prog->subEmitters){
            for (uint32_t name : triggers){
                for (size_t e = 0; e < compiled->document.subEmitters.size(); e++){
                    const SubEmitterEntry& entry = compiled->document.subEmitters[e];
                    if (entry.trigger != SubEmitterTrigger::Trigger || nameHash(entry.triggerName) != name)
                        continue;
                    // from every live particle, or from the emitter when there is none
                    if (channels.count == 0)
                        raiseEntryAtEmitter((uint32_t)e);
                    for (uint32_t i = 0; i < channels.count; i++)
                        raiseEntry((uint32_t)e, SubEmitterTrigger::Trigger, i, nullptr);
                }
            }
        }
        triggers.clear();
    }

    const bool emitsNow = emitting && cycleTime >= 0.0 && (prog->looping || cycleTime < prog->duration) && prog->emission.enabled;
    // the last tick of an effect that does not loop fires the bursts up to its end too (a burst
    // at the duration itself: Unity fires it once; origin/main's rule)
    const double burstUntil = (!prog->looping && cycleTime + tickDt >= prog->duration) ? std::max(cycleTime, (double)prog->duration) : cycleTime;
    if (emitsNow && budget > 0){
        ScalarDriver d;
        d.seed = seed;
        d.emitterTime = prog->duration > 0.0f ? (float)std::clamp(cycleTime / prog->duration, 0.0, 1.0) : 0.0f;
        const EmissionParams& em = prog->emission;
        // bursts: each firing at the first tick that starts at or after its time (its time in
        // (checked, cycle time]), a burst at the very end of a looping cycle included; a cycle
        // that wraps fires its early bursts on the next tick
        uint32_t burstCount = 0;
        burstGroups.clear();
        for (size_t b = 0; b < em.bursts.size(); b++){
            const BurstParam& bp = em.bursts[b];
            const uint32_t cycles = bp.cycles <= 0 ? 0xffffffffu : (uint32_t)bp.cycles;
            for (uint32_t c = 0; c < cycles; c++){
                const double t = bp.time + (double)c * bp.interval;
                if ((prog->looping && t > prog->duration + 1e-9) || t > burstUntil + 1e-9)
                    break;
                if (t <= burstCheckedTo + 1e-9)
                    continue;
                const uint32_t key = seed ^ randomSalt((uint32_t)ModuleId::Emission, 100u + (uint32_t)b);
                const uint32_t draw = loopIndex * 65536u + c;
                if (bp.probability < 1.0f && random01(key, draw) >= bp.probability)
                    continue;
                uint32_t n;
                if (bp.countCurve){
                    // a count over the emitter's duration, read at the burst's time
                    ScalarDriver bd;
                    bd.seed = seed;
                    bd.spawn = draw;
                    bd.emitterTime = prog->duration > 0.0f ? (float)std::clamp(t / prog->duration, 0.0, 1.0) : 0.0f;
                    n = rint32(std::max(0.0f, curveScalar(*prog, bp.count, bd)));
                }else if (bp.countMax > bp.countMin){
                    n = std::min((uint32_t)std::floor(bp.countMin + (bp.countMax - bp.countMin + 1.0f) * random01(key ^ 0x51ed270bu, draw)), rint32(bp.countMax));
                }else{
                    n = rint32(bp.countMin);
                }
                n = (uint32_t)std::floor(n * input.emissionScale + 0.5f);
                if (n){
                    burstGroups.push_back(n);
                    burstCount += n;
                }
                if (b < compiled->document.emission.bursts.size() && !compiled->document.emission.bursts[b].soundEvent.empty())
                    recordSound(ParticleSoundEvent::Source::Burst, (uint32_t)b, nullptr);
            }
        }
        burstCount = std::min(burstCount, budget);
        if (burstCount){
            growTo(channels.count + burstCount);
            // each burst its own spawns (Burst Spread spreads one burst's particles over the shape)
            uint32_t left = burstCount;
            for (uint32_t group : burstGroups){
                const uint32_t total = std::min(group, left);
                for (uint32_t done = 0; done < total;){
                    const uint32_t n = std::min(total - done, kSpawnBatch);
                    spreadBase = done;
                    spreadCount = total;
                    spawnBatch(input, n, nullptr, nullptr, nullptr, nullptr);
                    done += n;
                }
                left -= total;
                if (!left) break;
            }
            spreadBase = spreadCount = 0;
            budget -= burstCount;
        }
        // the rate (and over distance): fractions spread over the tick, pre-advanced to its end
        const float rate = std::max(0.0f, curveScalar(*prog, em.rate, d, (uint32_t)ticks * 0x2f6b9c1du));
        rateAccumulator += (double)rate * tickDt * input.emissionScale;
        if (!em.rateOverDistance.isZero()){
            if (tickDistance > kTeleportDistance){
                if (!teleportWarned){
                    Log::warn("[Particles] the emitter jumped %.1f units in one tick: Rate over Distance spawned nothing for it", tickDistance);
                    teleportWarned = true;
                }
            }else{
                const float perUnit = std::max(0.0f, curveScalar(*prog, em.rateOverDistance, d));
                distanceAccumulator += (double)perUnit * tickDistance * input.emissionScale;
            }
        }
        uint32_t n = (uint32_t)std::floor(rateAccumulator) + (uint32_t)std::floor(distanceAccumulator);
        rateAccumulator -= std::floor(rateAccumulator);
        distanceAccumulator -= std::floor(distanceAccumulator);
        if (em.maxPerTick) n = std::min(n, em.maxPerTick);
        n = std::min(n, budget);
        if (n){
            growTo(channels.count + n);
            SpawnStaging& staging = threadScratch().staging;
            for (uint32_t done = 0; done < n;){
                const uint32_t batch = std::min(n - done, kSpawnBatch);
                for (uint32_t k = 0; k < batch; k++){
                    const uint32_t i = done + k;
                    // the i-th of n spawns at (i + 1) / (n + 1) of the tick
                    staging.fraction[k] = (float)(i + 1) / (float)(n + 1);
                    staging.dt[k] = tickDt * (float)(n - i) / (float)(n + 1);
                }
                spawnBatch(input, batch, staging.fraction, staging.dt, nullptr, nullptr);
                done += batch;
            }
            budget -= n;
        }
    }

    if (emitsNow)
        burstCheckedTo = burstUntil;

    // emit() calls
    for (const PendingEmit& e : pendingEmits){
        const uint32_t n = std::min(e.count, budget);
        if (!n) continue;
        growTo(channels.count + n);
        for (uint32_t done = 0; done < n;){
            const uint32_t batch = std::min(n - done, kSpawnBatch);
            spawnBatch(input, batch, nullptr, nullptr, nullptr, e.hasOverrides ? &e.overrides : nullptr);
            done += batch;
        }
        budget -= n;
    }
    pendingEmits.clear();

    // a parent's requests (sub-emitter children)
    std::swap(requests, requestsProcessing);
    requests.clear();
    for (const SpawnRequest& r : requestsProcessing){
        const uint32_t n = std::min(r.count, budget);
        if (!n) continue;
        growTo(channels.count + n);
        for (uint32_t done = 0; done < n;){
            const uint32_t batch = std::min(n - done, kSpawnBatch);
            spawnBatch(input, batch, nullptr, nullptr, &r, nullptr);
            done += batch;
        }
        budget -= n;
    }
    requestsProcessing.clear();
}

void EmitterRuntime::spawnBatch(const EmitterInput& input, uint32_t n, const float* fractions, const float* preAdvance,
                                const SpawnRequest* request, const EmitOverrides* overrides){
    if (n == 0 || channels.count + n > channels.capacity())
        return;
    SpawnStaging& staging = threadScratch().staging;
    KernelContext ctx;
    fillContext(ctx, &input);
    for (int c = 0; c < ChCount; c++)
        ctx.ch[c] = channels.get((Channel)c) ? staging.data[c] : nullptr;
    uint32_t* spawn = reinterpret_cast<uint32_t*>(ctx.ch[ChSpawnIndex]);
    for (uint32_t k = 0; k < n; k++) spawn[k] = spawnCounter + k;
    // pad the last block so its unused lanes hold valid values
    for (uint32_t k = n; k < ((n + 3) & ~3u); k++) spawn[k] = spawnCounter + k;
    if (!fractions){
        for (uint32_t k = 0; k < ((n + 3) & ~3u); k++) staging.fraction[k] = 1.0f;
    }else if (fractions != staging.fraction){
        std::memcpy(staging.fraction, fractions, n * sizeof(float));
    }
    for (uint32_t k = n; k < ((n + 3) & ~3u); k++) staging.fraction[k] = 1.0f;
    ctx.spawnFraction = staging.fraction;
    if (request){
        // at the parent particle: the child's shape around it, the child emitter's rotation
        float origin[3];
        worldToSimPoint(request->position, origin);
        if (prog->space == SimulationSpace::World){
            float m[16];
            std::memcpy(m, tickTransform, sizeof(m));
            m[12] = request->position[0]; m[13] = request->position[1]; m[14] = request->position[2];
            to3x4(m, ctx.emitterToSim);
        }else{
            ctx.emitterToSim[9] = origin[0]; ctx.emitterToSim[10] = origin[1]; ctx.emitterToSim[11] = origin[2];
        }
        std::memcpy(ctx.prevEmitterToSim, ctx.emitterToSim, sizeof(ctx.emitterToSim));
        std::memcpy(ctx.prevEmitterToSimDir, ctx.emitterToSimDir, sizeof(ctx.emitterToSimDir));
    }
    // the spawn ops' scratch (Align to Direction keeps the shape's directions there)
    ctx.scratch = &threadScratch().chunk;
    const KernelTable& kernels = activeKernels();
    for (const Op& op : prog->spawnOps)
        kernels.spawn(ctx, op, 0, n);

    // overrides and inheritance, per particle
    if (overrides || request){
        float* invLife = ctx.ch[ChInvLife];
        float* startSize = ctx.ch[ChStartSize];
        uint32_t* startColor = reinterpret_cast<uint32_t*>(ctx.ch[ChStartColor]);
        const float* decode = ParticleCompiler::srgbDecodeTable();
        auto pack = [](const float c[4]){
            uint32_t v = 0;
            for (int ch = 0; ch < 3; ch++){
                const float e = std::pow(std::clamp(c[ch], 0.0f, 1.0f), 1.0f / 2.2f);
                v |= (uint32_t)(e * 255.0f + 0.5f) << (ch * 8);
            }
            return v | ((uint32_t)(std::clamp(c[3], 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
        };
        for (uint32_t k = 0; k < n; k++){
            float color[4] = {decode[startColor[k] & 255u], decode[(startColor[k] >> 8) & 255u], decode[(startColor[k] >> 16) & 255u],
                              (float)(startColor[k] >> 24) / 255.0f};
            bool colorChanged = false;
            if (overrides){
                if (overrides->hasPosition){
                    float p[3];
                    worldToSimPoint(overrides->position, p);
                    ctx.ch[ChPosX][k] = ctx.ch[ChPrevX][k] = p[0];
                    ctx.ch[ChPosY][k] = ctx.ch[ChPrevY][k] = p[1];
                    ctx.ch[ChPosZ][k] = ctx.ch[ChPrevZ][k] = p[2];
                }
                if (overrides->hasVelocity){
                    float v[3];
                    worldToSimDir(overrides->velocity, v);
                    ctx.ch[ChVelX][k] = v[0]; ctx.ch[ChVelY][k] = v[1]; ctx.ch[ChVelZ][k] = v[2];
                }
                if (overrides->hasSize){
                    startSize[k] = overrides->size;
                    if (ctx.ch[ChSizeX]) ctx.ch[ChSizeX][k] = ctx.ch[ChPrevSizeX][k] = overrides->size;
                }
                if (overrides->hasColor){
                    std::memcpy(color, overrides->color, sizeof(color));
                    colorChanged = true;
                }
                if (overrides->hasLifetime)
                    invLife[k] = 1.0f / std::max(overrides->lifetime, 1.0e-4f);
            }
            if (request){
                if (request->inherit & InheritColor){
                    for (int c = 0; c < 4; c++) color[c] *= request->color[c];
                    colorChanged = true;
                }
                if (request->inherit & InheritSize){
                    startSize[k] *= request->size * request->factor;
                    if (ctx.ch[ChSizeX]) ctx.ch[ChSizeX][k] = ctx.ch[ChPrevSizeX][k] = startSize[k];
                }
                if ((request->inherit & InheritRotation) && ctx.ch[ChStartRotation])
                    ctx.ch[ChStartRotation][k] = request->rotation;
                if (request->inherit & InheritLifetime)
                    invLife[k] /= std::max(request->lifetime * request->factor, 1.0e-4f);
                if (request->inherit & InheritVelocity){
                    float v[3];
                    worldToSimDir(request->velocity, v);
                    ctx.ch[ChVelX][k] += v[0] * request->factor;
                    ctx.ch[ChVelY][k] += v[1] * request->factor;
                    ctx.ch[ChVelZ][k] += v[2] * request->factor;
                }
            }
            if (colorChanged){
                startColor[k] = pack(color);
                if (ctx.ch[ChColorR]){
                    ctx.ch[ChColorR][k] = color[0]; ctx.ch[ChColorG][k] = color[1];
                    ctx.ch[ChColorB][k] = color[2]; ctx.ch[ChColorA][k] = color[3];
                }
            }
        }
    }

    // Follow Parent: the parent each child moves with, and its rotation and size now
    if (ctx.ch[ChParentId]){
        uint32_t* parent = reinterpret_cast<uint32_t*>(ctx.ch[ChParentId]);
        for (uint32_t k = 0; k < n; k++) parent[k] = request ? request->parentId : kNoParent;
        if (ctx.ch[ChFollowQuatX]){
            const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            const float* q = request ? request->parentQuat : identity;
            for (uint32_t k = 0; k < n; k++){
                ctx.ch[ChFollowQuatX][k] = q[0]; ctx.ch[ChFollowQuatY][k] = q[1];
                ctx.ch[ChFollowQuatZ][k] = q[2]; ctx.ch[ChFollowQuatW][k] = q[3];
            }
        }
        if (ctx.ch[ChFollowSize]){
            for (uint32_t k = 0; k < n; k++) ctx.ch[ChFollowSize][k] = request ? request->size : 1.0f;
        }
    }

    // the pre-advance: the update ops over the part of the tick after each spawn (R3, the
    // old spawn sub-step)
    if (preAdvance){
        if (preAdvance != staging.dt)
            std::memcpy(staging.dt, preAdvance, n * sizeof(float));
        for (uint32_t k = n; k < ((n + 3) & ~3u); k++) staging.dt[k] = 0.0f;
        ThreadScratch& scratch = threadScratch();
        ctx.scratch = &scratch.chunk;
        ctx.dtLanes = staging.dt;
        ctx.preAdvance = true;
        ctx.out = nullptr;
        scratch.chunk.hasAccel = false;
        scratch.chunk.hasAnimated = false;
        for (const Op& op : prog->updateOps){
            if (op.code == OpCode::Kill)
                continue;
            kernels.update(ctx, op, 0, n);
        }
    }

    // into the channels
    const uint32_t first = channels.count;
    channels.copyFrom(ctx.ch, 0, first, n);
    channels.count += n;
    spawnCounter += n;
    // the bounds take the newborns in
    const float* px = channels.get(ChPosX); const float* py = channels.get(ChPosY); const float* pz = channels.get(ChPosZ);
    for (uint32_t i = first; i < first + n; i++){
        boundsLo[0] = std::min(boundsLo[0], px[i]); boundsHi[0] = std::max(boundsHi[0], px[i]);
        boundsLo[1] = std::min(boundsLo[1], py[i]); boundsHi[1] = std::max(boundsHi[1], py[i]);
        boundsLo[2] = std::min(boundsLo[2], pz[i]); boundsHi[2] = std::max(boundsHi[2], pz[i]);
    }
    // trails and Birth sub-emitters
    if (trailStore.ringCount() > 0){
        uint32_t* slots = channels.getU(ChTrailSlot);
        const uint32_t* spawnIdx = channels.getU(ChSpawnIndex);
        const uint32_t key = seed ^ randomSalt((uint32_t)ModuleId::Trails, 9);
        ScalarDriver d;
        d.seed = seed;
        for (uint32_t i = first; i < first + n; i++){
            slots[i] = kNoTrail;
            if (prog->trails.ratio < 1.0f && random01(key, spawnIdx[i]) >= prog->trails.ratio)
                continue;
            slots[i] = trailStore.acquire();
            if (slots[i] == kNoTrail)
                continue;
            d.spawn = spawnIdx[i];
            float life = curveScalar(*prog, prog->trails.lifetime, d);
            if (prog->trails.sizeAffectsLifetime)
                life *= channels.get(ChStartSize)[i];
            trailStore.setLifetime(slots[i], std::max(life, 1.0e-3f));
            trailStore.setStyle(slots[i], channels.getU(ChStartColor)[i], channels.get(ChStartSize)[i]);
            // its first point comes with the tick's trail stage
        }
    }
    if (prog->subEmitters){
        for (uint32_t i = first; i < first + n; i++)
            raiseSubEmitters(SubEmitterTrigger::Birth, i, nullptr);
    }
}

void EmitterRuntime::trailStage(){
    if (trailStore.ringCount() == 0)
        return;
    const uint32_t* slots = channels.getU(ChTrailSlot);
    const float* px = channels.get(ChPosX); const float* py = channels.get(ChPosY); const float* pz = channels.get(ChPosZ);
    const float now = (float)elapsed + tickDt;
    const bool toWorld = prog->trails.worldSpace && prog->space == SimulationSpace::Local;
    for (uint32_t i = 0; i < channels.count; i++){
        if (slots[i] == kNoTrail)
            continue;
        float p[3] = {px[i], py[i], pz[i]};
        if (toWorld){
            const float sim[3] = {p[0], p[1], p[2]};
            simToWorldPoint(sim, p);
        }
        trailStore.record(slots[i], p[0], p[1], p[2], now, prog->trails.minVertexDistance);
    }
    trailStore.expire(now);
}

void EmitterRuntime::raiseSubEmitters(SubEmitterTrigger trigger, uint32_t index, const float* normal){
    const std::vector<SubEmitterEntry>& entries = compiled->document.subEmitters;
    for (size_t e = 0; e < entries.size() && e < children.size(); e++){
        if (entries[e].trigger == trigger)
            raiseEntry((uint32_t)e, trigger, index, normal);
    }
}

void EmitterRuntime::raiseEntry(uint32_t e, SubEmitterTrigger trigger, uint32_t index, const float*){
    const SubEmitterEntry& entry = compiled->document.subEmitters[e];
    EmitterRuntime* target = e < children.size() ? children[e].get() : nullptr;
    if (!target || !target->prog || entry.emitCount <= 0)
        return;
    const uint32_t spawnIdx = channels.getU(ChSpawnIndex)[index];
    if (entry.probability < 1.0f && random01(seed ^ randomSalt((uint32_t)ModuleId::SubEmitters, e), spawnIdx) >= entry.probability)
        return;
    SpawnRequest r;
    const float p[3] = {channels.get(ChPosX)[index], channels.get(ChPosY)[index], channels.get(ChPosZ)[index]};
    const float v[3] = {channels.get(ChVelX)[index], channels.get(ChVelY)[index], channels.get(ChVelZ)[index]};
    simToWorldPoint(p, r.position);
    simToWorldDir(v, r.velocity);
    appearance(index, r.color, r.size, r.rotation);
    r.lifetime = 1.0f / channels.get(ChInvLife)[index];
    r.count = (uint32_t)entry.emitCount;
    r.parentId = spawnIdx;
    r.inherit = (entry.inheritColor ? InheritColor : 0) | (entry.inheritSize ? InheritSize : 0) | (entry.inheritRotation ? InheritRotation : 0) |
                (entry.inheritLifetime ? InheritLifetime : 0) | (entry.inheritVelocity ? InheritVelocity : 0);
    r.factor = entry.inheritFactor;
    if (entry.followParent && entry.followRotation)
        particleRotation(index, r.parentQuat);
    if (!entry.soundEvent.empty())
        recordSound(ParticleSoundEvent::Source::SubEmitter, e, r.position);
    if (target->queueSpawn(r) && recordEffects && subEmits.size() < 64){
        SubEmitEvent ev;
        ev.trigger = trigger;
        ev.entry = e;
        ev.count = r.count;
        std::memcpy(ev.position, r.position, sizeof(r.position));
        subEmits.push_back(ev);
    }
}

void EmitterRuntime::raiseEntryAtEmitter(uint32_t e){
    const SubEmitterEntry& entry = compiled->document.subEmitters[e];
    EmitterRuntime* target = e < children.size() ? children[e].get() : nullptr;
    if (!target || !target->prog || entry.emitCount <= 0)
        return;
    // the draw of a particle-less fire: the tick's
    if (entry.probability < 1.0f && random01(seed ^ randomSalt((uint32_t)ModuleId::SubEmitters, e) ^ 0x7f4a7c15u, (uint32_t)ticks) >= entry.probability)
        return;
    SpawnRequest r;
    r.position[0] = tickTransform[12];
    r.position[1] = tickTransform[13];
    r.position[2] = tickTransform[14];
    std::memcpy(r.velocity, emitterVelocity, sizeof(r.velocity));
    r.count = (uint32_t)entry.emitCount;
    r.parentId = kNoParent;
    // nothing to inherit from
    r.inherit = 0;
    if (!entry.soundEvent.empty())
        recordSound(ParticleSoundEvent::Source::SubEmitter, e, r.position);
    if (target->queueSpawn(r) && recordEffects && subEmits.size() < 64){
        SubEmitEvent ev;
        ev.trigger = SubEmitterTrigger::Trigger;
        ev.entry = e;
        ev.count = r.count;
        std::memcpy(ev.position, r.position, sizeof(r.position));
        subEmits.push_back(ev);
    }
}

void EmitterRuntime::driverOf(uint32_t i, ScalarDriver& d) const{
    d.seed = seed;
    d.age = channels.get(ChAge)[i];
    d.normAge = d.age * channels.get(ChInvLife)[i];
    d.spawn = channels.getU(ChSpawnIndex)[i];
    const float vx = channels.get(ChVelX)[i], vy = channels.get(ChVelY)[i], vz = channels.get(ChVelZ)[i];
    d.speed = std::sqrt(vx * vx + vy * vy + vz * vz);
}

float EmitterRuntime::sizeOf(uint32_t i, const ScalarDriver& d) const{
    const RenderParams& rp = prog->render;
    float size = rp.storedSize ? channels.get(ChSizeX)[i] : channels.get(ChStartSize)[i];
    if (rp.sizeOverLifetime) size *= curveScalar(*prog, rp.sizeX, d);
    if (rp.sizeBySpeed) size *= curveScalar(*prog, rp.speedSizeX, d);
    if (rp.noiseSize) size *= channels.get(ChNoiseSize)[i];
    return size;
}

float EmitterRuntime::rollOf(uint32_t i, const ScalarDriver& d) const{
    const RenderParams& rp = prog->render;
    if (rp.storedRotation)
        return channels.get(ChRotation)[i];
    float rotation = rp.startRotationStored ? channels.get(ChStartRotation)[i] : curveScalar(*prog, rp.startRotation, d);
    if (rp.rotationOverLifetime){
        if (rp.rotationRate.kind == CurveParam::Const || rp.rotationRate.kind == CurveParam::Range)
            rotation += curveScalar(*prog, rp.rotationRate, d) * d.age;
        else
            rotation += curveScalar(*prog, rp.rotationRate, d) / channels.get(ChInvLife)[i];
    }
    return rotation;
}

void EmitterRuntime::particleRotation(uint32_t i, float q[4]) const{
    if (prog->has(ChQuatX)){
        q[0] = channels.get(ChQuatX)[i]; q[1] = channels.get(ChQuatY)[i];
        q[2] = channels.get(ChQuatZ)[i]; q[3] = channels.get(ChQuatW)[i];
        return;
    }
    ScalarDriver d;
    driverOf(i, d);
    const float half = 0.5f * rollOf(i, d);
    q[0] = 0.0f; q[1] = 0.0f; q[2] = std::sin(half); q[3] = std::cos(half);
}

void EmitterRuntime::appearance(uint32_t i, float color[4], float& size, float& rotation) const{
    const RenderParams& rp = prog->render;
    ScalarDriver d;
    driverOf(i, d);
    if (rp.storedColor){
        color[0] = channels.get(ChColorR)[i]; color[1] = channels.get(ChColorG)[i];
        color[2] = channels.get(ChColorB)[i]; color[3] = channels.get(ChColorA)[i];
    }else{
        const uint32_t c = channels.getU(ChStartColor)[i];
        const float* decode = ParticleCompiler::srgbDecodeTable();
        color[0] = decode[c & 255u]; color[1] = decode[(c >> 8) & 255u]; color[2] = decode[(c >> 16) & 255u];
        color[3] = (float)(c >> 24) / 255.0f;
    }
    if (rp.colorOverLifetime){
        float m[4];
        gradientScalar(*prog, rp.color, d, m);
        for (int k = 0; k < 4; k++) color[k] *= m[k];
    }
    if (rp.colorBySpeed){
        float m[4];
        gradientScalar(*prog, rp.speedColor, d, m);
        for (int k = 0; k < 4; k++) color[k] *= m[k];
    }
    size = sizeOf(i, d);
    rotation = rollOf(i, d);
}

void EmitterRuntime::followStage(EmitterRuntime& child, const SubEmitterEntry& entry){
    // the parents by spawn index, once a tick (after this tick's deaths and compaction)
    if (!followIndexed){
        const uint32_t* spawn = channels.getU(ChSpawnIndex);
        followIndex.resize(channels.count);
        for (uint32_t i = 0; i < channels.count; i++) followIndex[i] = ((uint64_t)spawn[i] << 32) | i;
        std::sort(followIndex.begin(), followIndex.end());
        followIndexed = true;
    }
    const bool rotate = entry.followRotation && child.prog->followsRotation;
    const bool scale = entry.followScale && child.prog->followsScale;
    // the same simulation space (the child's transform is the parent's tick's): no conversion
    const bool sameSpace = child.prog->space == prog->space;
    uint32_t* parentOf = child.channels.getU(ChParentId);
    float* cx = child.channels.get(ChPosX); float* cy = child.channels.get(ChPosY); float* cz = child.channels.get(ChPosZ);
    float* vx = child.channels.get(ChVelX); float* vy = child.channels.get(ChVelY); float* vz = child.channels.get(ChVelZ);
    const float* px = channels.get(ChPosX); const float* py = channels.get(ChPosY); const float* pz = channels.get(ChPosZ);
    const float* qx = channels.get(ChPrevX); const float* qy = channels.get(ChPrevY); const float* qz = channels.get(ChPrevZ);
    for (uint32_t i = 0; i < child.channels.count; i++){
        const uint32_t parent = parentOf[i];
        if (parent == kNoParent)
            continue;
        const uint64_t key = (uint64_t)parent << 32;
        const auto found = std::lower_bound(followIndex.begin(), followIndex.end(), key);
        if (found == followIndex.end() || (uint32_t)(*found >> 32) != parent){
            // its parent died: it stays where it is from now on
            parentOf[i] = kNoParent;
            continue;
        }
        const uint32_t p = (uint32_t)(*found & 0xffffffffu);
        // the child in the parent's space, as an offset from where the parent was a tick ago
        float c[3] = {cx[i], cy[i], cz[i]}, v[3] = {vx[i], vy[i], vz[i]};
        if (!sameSpace){
            float w[3];
            child.simToWorldPoint(c, w);
            worldToSimPoint(w, c);
            child.simToWorldDir(v, w);
            worldToSimDir(w, v);
        }
        float offset[3] = {c[0] - qx[p], c[1] - qy[p], c[2] - qz[p]};
        if (rotate){
            // the parent's turn since the last tick: now * conjugate(then)
            float now[4];
            particleRotation(p, now);
            float* then[4] = {child.channels.get(ChFollowQuatX) + i, child.channels.get(ChFollowQuatY) + i,
                              child.channels.get(ChFollowQuatZ) + i, child.channels.get(ChFollowQuatW) + i};
            const float b[4] = {-*then[0], -*then[1], -*then[2], *then[3]};
            const float turn[4] = {now[3] * b[0] + now[0] * b[3] + now[1] * b[2] - now[2] * b[1],
                                   now[3] * b[1] - now[0] * b[2] + now[1] * b[3] + now[2] * b[0],
                                   now[3] * b[2] + now[0] * b[1] - now[1] * b[0] + now[2] * b[3],
                                   now[3] * b[3] - now[0] * b[0] - now[1] * b[1] - now[2] * b[2]};
            auto turnVector = [&turn](float x[3]){
                // x + 2w (q x x) + 2 q x (q x x)
                const float t[3] = {2.0f * (turn[1] * x[2] - turn[2] * x[1]), 2.0f * (turn[2] * x[0] - turn[0] * x[2]), 2.0f * (turn[0] * x[1] - turn[1] * x[0])};
                const float r[3] = {x[0] + turn[3] * t[0] + turn[1] * t[2] - turn[2] * t[1], x[1] + turn[3] * t[1] + turn[2] * t[0] - turn[0] * t[2],
                                    x[2] + turn[3] * t[2] + turn[0] * t[1] - turn[1] * t[0]};
                x[0] = r[0]; x[1] = r[1]; x[2] = r[2];
            };
            turnVector(offset);
            turnVector(v);
            for (int k = 0; k < 4; k++) *then[k] = now[k];
        }
        if (scale){
            ScalarDriver d;
            driverOf(p, d);
            const float now = sizeOf(p, d);
            float& then = child.channels.get(ChFollowSize)[i];
            if (then > 0.0f){
                const float ratio = now / then;
                for (int k = 0; k < 3; k++) offset[k] *= ratio;
            }
            then = now;
        }
        c[0] = px[p] + offset[0]; c[1] = py[p] + offset[1]; c[2] = pz[p] + offset[2];
        if (!sameSpace){
            float w[3];
            simToWorldPoint(c, w);
            child.worldToSimPoint(w, c);
            simToWorldDir(v, w);
            child.worldToSimDir(w, v);
        }
        cx[i] = c[0]; cy[i] = c[1]; cz[i] = c[2];
        vx[i] = v[0]; vy[i] = v[1]; vz[i] = v[2];
        // its chunk's bounds take it where it is now
        ChunkOutput& o = child.chunkOut[i / child.activeChunkSize];
        for (int k = 0; k < 3; k++){
            o.boundsMin[k] = std::min(o.boundsMin[k], c[k]);
            o.boundsMax[k] = std::max(o.boundsMax[k], c[k]);
        }
    }
}

void EmitterRuntime::childrenStage(const EmitterInput& input){
    followIndexed = false;
    // the tree shares its budget: what the parent and the other entries hold, a child has not
    uint32_t held = totalCount();
    for (size_t c = 0; c < children.size(); c++){
        EmitterRuntime* child = children[c].get();
        if (!child || !child->prog)
            continue;
        // the children run on the parent's tick, at the parent's transform
        EmitterInput childInput = input;
        childInput.transform = tickTransform;
        childInput.seed = child->seed;
        const uint32_t childBefore = child->totalCount();
        const uint32_t others = held - childBefore;
        childInput.budget = input.budget > others ? input.budget - others : 0u;
        child->playState = PlayState::Playing;
        std::memcpy(child->prevFrame, tickTransform, sizeof(tickTransform));
        std::memcpy(child->curFrame, tickTransform, sizeof(tickTransform));
        if (!child->started){
            std::memcpy(child->tickTransform, tickTransform, sizeof(tickTransform));
            std::memcpy(child->lastTickTransform, tickTransform, sizeof(tickTransform));
            child->started = true;
        }
        child->recordEffects = recordEffects;
        child->frameTick = tickDt;
        child->frameScaled = 0.0;
        child->ticksThisFrame = 1;
        child->clearEvents();
        child->beginTick(childInput, 0);
        const uint32_t chunks = child->chunkCount();
        if (chunks == 1){
            child->updateChunk(0);
        }else if (chunks > 1){
            jobsOf(input).parallelFor(chunks, [child](size_t k){ child->updateChunk((uint32_t)k); });
        }
        // after their own motion, before their newborns: the children that follow move on with
        // their parents
        const std::vector<SubEmitterEntry>& entries = compiled->document.subEmitters;
        if (child->prog->followsParent && c < entries.size() && entries[c].followParent && chunks > 0)
            followStage(*child, entries[c]);
        child->finishTick(childInput);
        held = others + child->totalCount();
    }
}

// ---------------------------------------------------------------- whole ticks

void EmitterRuntime::runTickSerial(const EmitterInput& input){
    const uint32_t chunks = chunkCount();
    if (chunks == 1){
        updateChunk(0);
    }else if (chunks > 1){
        jobsOf(input).parallelFor(chunks, [this](size_t k){ updateChunk((uint32_t)k); });
    }
    finishTick(input);
}

void EmitterRuntime::advance(const EmitterInput& input, double dt){
    const uint32_t steps = planFrame(input, dt);
    for (uint32_t k = 0; k < steps; k++){
        beginTick(input, k);
        runTickSerial(input);
    }
    endFrame();
}

void EmitterRuntime::step(const EmitterInput& input){
    // a paused system steps too (the editor's step); a stopped one has nothing to run
    if (!prog || playState == PlayState::Stopped)
        return;
    std::memcpy(curFrame, input.transform ? input.transform : kIdentity, sizeof(curFrame));
    if (!started){
        std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
        std::memcpy(tickTransform, curFrame, sizeof(tickTransform));
        std::memcpy(lastTickTransform, curFrame, sizeof(lastTickTransform));
        started = true;
    }
    prewarm = SeekState();
    frameTick = prog->tickRate > 0 ? 1.0 / prog->tickRate : kMaxVariableStep;
    frameScaled = 0.0;
    ticksThisFrame = 1;
    clearEvents();
    beginTick(input, 0);
    runTickSerial(input);
    std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
    accumulator = 0.0;
    alpha = 1.0f;
    shareInterpolation();
}

void EmitterRuntime::beginSeek(const EmitterInput& input, double time, SeekState& s){
    s = SeekState();
    if (!prog)
        return;
    reset(seed);
    playState = PlayState::Playing;
    emitting = true;
    std::memcpy(curFrame, input.transform ? input.transform : kIdentity, sizeof(curFrame));
    std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
    std::memcpy(tickTransform, curFrame, sizeof(tickTransform));
    std::memcpy(lastTickTransform, curFrame, sizeof(lastTickTransform));
    started = true;
    if (!(time > 0.0))
        return;
    s.target = time;
    s.tick = prog->tickRate > 0 ? 1.0 / prog->tickRate : kMaxVariableStep;
    s.total = (uint32_t)std::floor((s.target + s.tick * 1e-6) / s.tick);
    s.pending = s.total > 0;
}

bool EmitterRuntime::continueSeek(const EmitterInput& input, SeekState& s, double budgetMs){
    continueSeekTicks(input, s, budgetMs);
    return !s.pending;
}

void EmitterRuntime::continueSeekTicks(const EmitterInput& input, SeekState& s, double budgetMs){
    if (!s.pending || !prog)
        return;
    const auto begin = std::chrono::steady_clock::now();
    const bool record = recordEffects;
    recordEffects = false;
    while (s.completed < s.total){
        frameTick = s.tick;
        frameScaled = 0.0;
        ticksThisFrame = 1;
        beginTick(input, 0);
        runTickSerial(input);
        s.completed++;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() >= budgetMs)
            break;
    }
    recordEffects = record;
    collisions.clear();
    deaths.clear();
    subEmits.clear();
    s.pending = s.completed < s.total;
    if (!s.pending){
        // the remainder of the target drives the interpolation
        accumulator = std::max(0.0, s.target - s.total * s.tick);
        alpha = prog->interpolate ? (float)std::clamp(accumulator / s.tick, 0.0, 1.0) : 1.0f;
        shareInterpolation();
    }
}

void EmitterRuntime::seekStep(const EmitterInput& input, double tick){
    if (!prog)
        return;
    if (playState == PlayState::Stopped){
        playState = PlayState::Playing;
        emitting = true;
    }
    std::memcpy(curFrame, input.transform ? input.transform : kIdentity, sizeof(curFrame));
    if (!started){
        std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
        std::memcpy(tickTransform, curFrame, sizeof(tickTransform));
        std::memcpy(lastTickTransform, curFrame, sizeof(lastTickTransform));
        started = true;
    }
    const bool record = recordEffects;
    recordEffects = false;
    frameTick = tick;
    frameScaled = 0.0;
    ticksThisFrame = 1;
    beginTick(input, 0);
    runTickSerial(input);
    recordEffects = record;
    collisions.clear();
    deaths.clear();
    subEmits.clear();
    std::memcpy(prevFrame, curFrame, sizeof(prevFrame));
}

void EmitterRuntime::settleSeek(double remainder, double tick){
    accumulator = std::max(0.0, remainder);
    alpha = (prog && prog->interpolate && tick > 0.0) ? (float)std::clamp(accumulator / tick, 0.0, 1.0) : 1.0f;
    shareInterpolation();
}

void EmitterRuntime::seek(const EmitterInput& input, double time){
    SeekState s;
    beginSeek(input, time, s);
    continueSeekTicks(input, s, 1.0e30);
}

// ---------------------------------------------------------------- queries

void EmitterRuntime::bounds(float outMin[3], float outMax[3]) const{
    const float pad = prog ? 0.5f * prog->maxStartSize * prog->maxSizeFactor * 1.7320508f : 0.0f;
    for (int k = 0; k < 3; k++){
        outMin[k] = boundsLo[k] - pad;
        outMax[k] = boundsHi[k] + pad;
    }
}

uint64_t EmitterRuntime::stateHash() const{
    uint64_t h = channels.hash() ^ ((uint64_t)spawnCounter * 0x9e3779b97f4a7c15ull);
    for (const auto& c : children){
        if (c) h = (h * 1099511628211ull) ^ c->stateHash();
    }
    return h;
}

size_t EmitterRuntime::Checkpoint::bytes() const{
    size_t n = sizeof(Checkpoint) + channels.data.size() * sizeof(uint32_t) + trails.memoryBytes();
    for (const Checkpoint& c : children) n += c.bytes();
    return n;
}

void EmitterRuntime::saveCheckpoint(Checkpoint& out) const{
    channels.save(out.channels);
    out.trails = trailStore;
    out.elapsed = elapsed;
    out.cycleTime = cycleTime;
    out.burstCheckedTo = burstCheckedTo;
    out.rateAccumulator = rateAccumulator;
    out.distanceAccumulator = distanceAccumulator;
    out.arcPhase = arcPhase;
    out.radiusPhase = radiusPhase;
    out.noiseScroll = noiseScroll;
    std::memcpy(out.lastTickVelocity, lastTickVelocity, sizeof(lastTickVelocity));
    out.ticks = ticks;
    out.spawnCounter = spawnCounter;
    out.loopIndex = loopIndex;
    out.emitting = emitting;
    out.lastTick = lastTick;
    out.boundsValid = boundsValid;
    std::memcpy(out.boundsLo, boundsLo, sizeof(boundsLo));
    std::memcpy(out.boundsHi, boundsHi, sizeof(boundsHi));
    std::memcpy(out.tickTransform, tickTransform, sizeof(tickTransform));
    std::memcpy(out.lastTickTransform, lastTickTransform, sizeof(lastTickTransform));
    std::memcpy(out.planeGrid, planeGrid, sizeof(planeGrid));
    out.children.resize(children.size());
    for (size_t c = 0; c < children.size(); c++){
        if (children[c]) children[c]->saveCheckpoint(out.children[c]);
    }
}

void EmitterRuntime::restoreCheckpoint(const Checkpoint& in){
    if (!prog)
        return;
    growTo(in.channels.count);
    channels.restore(in.channels);
    trailStore = in.trails;
    elapsed = in.elapsed;
    cycleTime = in.cycleTime;
    burstCheckedTo = in.burstCheckedTo;
    rateAccumulator = in.rateAccumulator;
    distanceAccumulator = in.distanceAccumulator;
    arcPhase = in.arcPhase;
    radiusPhase = in.radiusPhase;
    noiseScroll = in.noiseScroll;
    std::memcpy(lastTickVelocity, in.lastTickVelocity, sizeof(lastTickVelocity));
    ticks = in.ticks;
    spawnCounter = in.spawnCounter;
    loopIndex = in.loopIndex;
    emitting = in.emitting;
    lastTick = in.lastTick;
    boundsValid = in.boundsValid;
    std::memcpy(boundsLo, in.boundsLo, sizeof(boundsLo));
    std::memcpy(boundsHi, in.boundsHi, sizeof(boundsHi));
    std::memcpy(tickTransform, in.tickTransform, sizeof(tickTransform));
    std::memcpy(lastTickTransform, in.lastTickTransform, sizeof(lastTickTransform));
    std::memcpy(planeGrid, in.planeGrid, sizeof(planeGrid));
    playState = PlayState::Playing;
    started = true;
    accumulator = 0.0;
    pendingEmits.clear();
    requests.clear();
    for (size_t c = 0; c < children.size() && c < in.children.size(); c++){
        if (children[c]) children[c]->restoreCheckpoint(in.children[c]);
    }
}

uint32_t particles::nameHash(std::string_view name){
    uint32_t h = 2166136261u;
    for (char c : name){
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h;
}
