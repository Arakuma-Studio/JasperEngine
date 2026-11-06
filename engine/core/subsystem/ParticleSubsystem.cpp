// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSubsystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "math/Ray.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleMeshes.h"
#include "particles/ParticleModules.h"
#include "manager/ObjectManager.h"
#include "registry/PhysicsLayers.h"
#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float kTwoPi = 6.28318530718f;

    // an immediate simulation (seek, step, the editor's preview) sees the Transform as it was set,
    // not as the last frame left it: a moved GameObject (or parent) has its matrices worked out
    // now, as the frame's transform pass will; true when they changed
    bool refreshTransform(Scene* scene, Entity entity){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            return false;
        const bool parentChanged = transform->parent != NULL_ENTITY && refreshTransform(scene, transform->parent);
        if (!transform->needUpdate && !parentChanged)
            return false;
        scene->getSystem<RenderSystem>()->updateTransform(*transform, entity);
        return true;
    }

    void copyMatrix(const Matrix4& m, float out[16]){
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) out[c * 4 + r] = m[c][r];
    }

    // a seed for a play (Auto Random Seed): the scene's own sequence, never 0
    uint32_t nextSeed(){
        static uint32_t counter = 0x9e3779b9u;
        counter = counter * 1664525u + 1013904223u;
        return counter | 1u;
    }

    Vector3 point(const float p[3]){ return Vector3(p[0], p[1], p[2]); }

}

ParticleSubsystem::ParticleSubsystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<ParticleSystemComponent>());
    signature.set(scene->getComponentId<Transform>());
    declareHookType(scene->getComponentId<ParticleSystemComponent>());

    provider3D.terrain = &ParticleSubsystem::sampleTerrain;
    provider3D.ray = &ParticleSubsystem::castWorldRay;
    provider3D.context = this;
    provider2D.terrain = &ParticleSubsystem::sampleTerrain;
    provider2D.ray = &ParticleSubsystem::castWorldRay2D;
    provider2D.context = this;
    tasks.reserve(64);
    taskEntities.reserve(64);
    fields.reserve(kMaxForceFields);
}

void ParticleSubsystem::load(){
}

void ParticleSubsystem::draw(){
}

void ParticleSubsystem::destroy(){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        if (particles.state){
            // the scene stays when only its view went (Android, a lost context): bound again on its
            // next update, playing again if it was
            particles.state->runtime.unbind();
            particles.needReload = true;
        }
    }
}

void ParticleSubsystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<ParticleSystemComponent>())
        return;
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    particles.needReload = true;
}

void ParticleSubsystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<ParticleSystemComponent>())
        return;
    if (ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity)){
        if (particles->state)
            particles->state->runtime.unbind();
    }
}

// ---------------------------------------------------------------- effects

const ParticleEffectDocument* ParticleSubsystem::getDocument(const ParticleSystemComponent& particles){
    if (particles.effect.empty())
        return &particles.inlineEffect;
    return ParticleEffectLibrary::get(particles.effect.path).document.get();
}

namespace {

    // the sub-emitter files of an effect, down the levels EmitterRuntime binds
    uint64_t subEmitterSignature(const ParticleEffectDocument& document, int depth){
        uint64_t h = 1469598103934665603ull;
        if (depth >= kMaxSubEmitterDepth || !document.subEmittersModule.present || !document.subEmittersModule.enabled)
            return h;
        for (const SubEmitterEntry& entry : document.subEmitters){
            for (unsigned char c : entry.effect) h = (h ^ c) * 1099511628211ull;
            const EffectAsset& asset = ParticleEffectLibrary::get(entry.effect);
            h = (h ^ asset.version) * 1099511628211ull;
            if (asset.document)
                h = (h ^ subEmitterSignature(*asset.document, depth + 1)) * 1099511628211ull;
        }
        return h;
    }

}

CompileEnvironment ParticleSubsystem::compileEnvironment(Entity entity){
    CompileEnvironment environment;
    environment.mesh = [this, entity](const MeshSource& source, MeshGeometry& out){
        ParticleMeshData data;
        switch (source.type){
            case MeshSourceType::Primitive:
                ParticleMeshes::primitive(source.primitive, data);
                break;
            case MeshSourceType::Model:
                if (!ParticleMeshes::model(source.model, source.submesh, data))
                    return false;
                break;
            case MeshSourceType::GameObject: {
                MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                if (!mesh || !ParticleMeshes::meshComponent(*mesh, source.submesh, data))
                    return false;
                break;
            }
        }
        ParticleMeshes::geometry(data, out);
        return true;
    };
    return environment;
}

bool ParticleSubsystem::subEmittersOutOfDate(const ParticleSystemComponent& particles) const{
    const ParticleSystemState* state = particles.state.get();
    return state && state->effect && state->effect->program.subEmitters && state->subEmitterGeneration != ParticleEffectLibrary::generation();
}

bool ParticleSubsystem::ensureEffect(Entity entity, ParticleSystemComponent& particles){
    if (!particles.state)
        particles.state = std::make_shared<ParticleSystemState>();
    ParticleSystemState& state = *particles.state;

    const std::string path = particles.effect.empty() ? std::string() : particles.effect.path;
    const ParticleEffectDocument* document = nullptr;
    uint64_t version = 0;
    if (!path.empty()){
        const EffectAsset& asset = ParticleEffectLibrary::get(path);
        document = asset.document.get();
        version = asset.version;
        if (!document){
            if (state.effectPath != path || state.effectVersion != version){
                state.diagnostics = asset.diagnostics;
                state.effectPath = path;
                state.effectVersion = version;
            }
            particles.needReload = false;
            return state.effect != nullptr;
        }
    }else{
        document = &particles.inlineEffect;
    }
    const bool sameEffect = !particles.needReload && state.effect && state.effectPath == path && state.effectVersion == version;
    if (sameEffect && !subEmittersOutOfDate(particles))
        return true;

    CompileEnvironment environment = compileEnvironment(entity);
    const auto resolver = [&environment](const SubEmitterEntry& entry) -> std::shared_ptr<const CompiledEffect> {
        const EffectAsset& asset = ParticleEffectLibrary::get(entry.effect);
        if (!asset.document)
            return nullptr;
        CompileEnvironment childEnvironment = environment;
        childEnvironment.inheritsRotation = entry.inheritRotation;
        std::shared_ptr<CompiledEffect> child = ParticleCompiler::compile(*asset.document, childEnvironment);
        return child->valid ? child : nullptr;
    };

    bool bindAgain = false;
    if (sameEffect){
        // the library moved on: only a change in this tree's files costs anything more
        state.subEmitterGeneration = ParticleEffectLibrary::generation();
        const uint64_t signature = subEmitterSignature(state.effect->document, 1);
        if (signature == state.subEmitterSignature)
            return true;
        state.subEmitterSignature = signature;
        // (a seek's checkpoints hold what the old child spawned)
        if (state.preview)
            state.preview->invalidate();
        if (state.runtime.rebindSubEmitters(resolver))
            return true;
        bindAgain = true;
    }

    // the document with the component's exposed values written into it
    ParticleEffectDocument values = *document;
    for (const ParticleExposedValue& v : particles.exposedValues){
        if (!ParticleModules::setExposed(values, v.name, v.value))
            Log::warn("[Particles] %s: no exposed parameter '%s'", path.empty() ? "inline effect" : path.c_str(), v.name.c_str());
    }

    std::shared_ptr<CompiledEffect> compiled = ParticleCompiler::compile(values, environment);
    particles.needReload = false;
    state.effectPath = path;
    state.effectVersion = version;
    state.diagnostics = compiled->diagnostics;
    if (!compiled->valid){
        Log::error("[Particles] %s: %s", path.empty() ? "inline effect" : path.c_str(), ParticleEffectCodec::format(compiled->diagnostics).c_str());
        return state.effect != nullptr;
    }

    const uint64_t signature = subEmitterSignature(compiled->document, 1);
    const bool sameFiles = signature == state.subEmitterSignature;
    state.subEmitterSignature = signature;
    state.subEmitterGeneration = ParticleEffectLibrary::generation();

    // a parameter edit keeps the live particles; another structure starts over
    if (!bindAgain && state.effect && state.runtime.rebind(compiled) && (sameFiles || state.runtime.rebindSubEmitters(resolver))){
        state.effect = compiled;
    }else{
        const bool wasPlaying = state.runtime.isPlaying();
        state.runtime.bind(compiled);
        state.runtime.bindSubEmitters(resolver);
        state.effect = compiled;
        state.seed = particles.seed != 0 ? particles.seed : (compiled->document.seed != 0 ? compiled->document.seed : nextSeed());
        state.runtime.reset(state.seed);
        if (state.preview)
            state.preview->invalidate();
        if (wasPlaying)
            state.runtime.play();
    }
    state.render.needReload = true;
    return true;
}

ParticleSystemState* ParticleSubsystem::stateOf(Entity entity, bool load){
    ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
    if (!particles)
        return nullptr;
    if (load){
        if (!ensureEffect(entity, *particles))
            return nullptr;
    }
    return particles->state && particles->state->effect ? particles->state.get() : nullptr;
}

ParticleSystemState* ParticleSubsystem::getState(Entity entity){
    return stateOf(entity, true);
}

void ParticleSubsystem::reloadEffect(const std::string& path){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        if (!particles.effect.empty() && particles.effect.path == path)
            particles.needReload = true;
    }
}

// ---------------------------------------------------------------- playback

void ParticleSubsystem::play(Entity entity){
    if (ParticleSystemState* state = getState(entity)){
        if (state->runtime.state() == PlayState::Stopped && !state->runtime.isAlive()){
            ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
            if (particles.seed == 0 && state->effect->document.seed == 0){
                state->seed = nextSeed();
                state->runtime.reset(state->seed);
            }
        }
        state->runtime.play();
        state->started = true;
        state->running = true;
    }
}

void ParticleSubsystem::pause(Entity entity){
    if (ParticleSystemState* state = getState(entity))
        state->runtime.pause();
}

void ParticleSubsystem::stop(Entity entity, bool clear){
    if (ParticleSystemState* state = getState(entity)){
        state->runtime.stop(clear);
        state->started = true;
    }
}

void ParticleSubsystem::clear(Entity entity){
    // the particles (and trails) go; playing or stopped as it was
    if (ParticleSystemState* state = getState(entity))
        state->runtime.clear();
}

void ParticleSubsystem::resetForPlay(){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        if (!particles.state)
            continue;
        particles.state->runtime.stop(true);
        particles.state->started = false;
        particles.state->running = false;
        particles.state->hasBounds = false;
        particles.state->cull = CullState();
    }
    stopActions.clear();
}

void ParticleSubsystem::restart(Entity entity){
    if (ParticleSystemState* state = getState(entity)){
        state->runtime.restart();
        state->started = true;
    }
}

void ParticleSubsystem::emit(Entity entity, uint32_t count, const EmitOverrides* overrides){
    if (ParticleSystemState* state = getState(entity)){
        // a stopped system plays only what is emitted (EmitterRuntime::emit), not its own emission
        state->runtime.emit(count, overrides);
        state->started = true;
    }
}

void ParticleSubsystem::seek(Entity entity, double time){
    ParticleSystemState* state = getState(entity);
    if (!state)
        return;
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    EmitterInput input;
    refreshTransform(scene, entity);
    prepareInput(entity, particles, scene->getComponent<Transform>(entity), input);
    state->runtime.seek(input, time);
    state->started = true;
}

void ParticleSubsystem::step(Entity entity, uint32_t ticks){
    ParticleSystemState* state = getState(entity);
    if (!state)
        return;
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    EmitterInput input;
    refreshTransform(scene, entity);
    prepareInput(entity, particles, scene->getComponent<Transform>(entity), input);
    if (state->runtime.state() != PlayState::Playing)
        state->runtime.play();
    for (uint32_t t = 0; t < ticks; t++) state->runtime.step(input);
    state->started = true;
    updateBounds(*state);
}

void ParticleSubsystem::sendTrigger(Entity entity, const std::string& name){
    if (ParticleSystemState* state = getState(entity))
        state->runtime.sendTrigger(nameHash(name));
}

bool ParticleSubsystem::isPlaying(Entity entity){
    ParticleSystemState* state = getState(entity);
    return state && state->runtime.isPlaying();
}

bool ParticleSubsystem::isPaused(Entity entity){
    ParticleSystemState* state = getState(entity);
    return state && state->runtime.isPaused();
}

bool ParticleSubsystem::isEmitting(Entity entity){
    ParticleSystemState* state = getState(entity);
    return state && state->runtime.isEmitting();
}

uint32_t ParticleSubsystem::getParticleCount(Entity entity){
    ParticleSystemState* state = getState(entity);
    return state ? state->runtime.totalCount() : 0u;
}

double ParticleSubsystem::getTime(Entity entity){
    ParticleSystemState* state = getState(entity);
    return state ? state->runtime.time() : 0.0;
}

// ---------------------------------------------------------------- preview

void ParticleSubsystem::previewSeek(Entity entity, double time){
    ParticleSystemState* state = getState(entity);
    if (!state)
        return;
    if (!state->preview)
        state->preview = std::make_unique<ParticlePreview>();
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    EmitterInput input;
    refreshTransform(scene, entity);
    prepareInput(entity, particles, scene->getComponent<Transform>(entity), input);
    state->preview->seek(state->runtime, input, std::max(0.0, time));
    state->started = true;
}

bool ParticleSubsystem::previewUpdate(Entity entity, double budgetMs){
    ParticleSystemState* state = getState(entity);
    if (!state || !state->preview)
        return true;
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    EmitterInput input;
    refreshTransform(scene, entity);
    prepareInput(entity, particles, scene->getComponent<Transform>(entity), input);
    const bool done = state->preview->update(state->runtime, input, budgetMs);
    updateBounds(*state);
    return done;
}

void ParticleSubsystem::previewAdvance(Entity entity, double dt){
    ParticleSystemState* state = getState(entity);
    if (!state)
        return;
    ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
    if (!state->started && particles.playOnAwake){
        state->runtime.play();
        state->started = true;
    }
    EmitterInput input;
    refreshTransform(scene, entity);
    prepareInput(entity, particles, scene->getComponent<Transform>(entity), input);
    const auto start = std::chrono::steady_clock::now();
    state->runtime.advance(input, dt);
    state->simulateMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    state->ticks = state->runtime.ticksLastFrame();
    updateBounds(*state);
}

// ---------------------------------------------------------------- stats

ParticleSystemStats ParticleSubsystem::getStats(Entity entity){
    ParticleSystemStats stats;
    ParticleSystemState* state = getState(entity);
    if (!state)
        return stats;
    const EmitterRuntime& r = state->runtime;
    stats.valid = true;
    stats.live = r.totalCount();
    stats.capacity = r.totalCapacity();
    stats.budget = state->budget;
    stats.emissionScale = state->emissionScale;
    stats.ticks = state->ticks;
    stats.tickCount = r.tickCount();
    stats.time = r.time();
    stats.droppedTime = r.droppedTime();
    stats.simulateMicros = state->simulateMicros;
    stats.renderMicros = state->renderMicros;
    stats.culled = state->culled;
    stats.playing = r.isPlaying();
    stats.paused = r.isPaused();
    stats.emitting = r.isEmitting();
    stats.sortedViews = state->sortedViews;
    stats.drawnViews = state->drawnViews;
    stats.raysLastTick = r.raysLastTick();
    stats.bytesPerParticle = r.program() ? (uint32_t)r.program()->bytesPerParticle() : 0u;
    return stats;
}

void ParticleSubsystem::markVisible(Entity entity){
    if (ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity)){
        if (particles->state)
            particles->state->visibleFrame = SystemRender::getFrameCount();
    }
}

// ---------------------------------------------------------------- the frame

// the velocity of the GameObject's Rigidbody (3D, else 2D; one that is made already)
bool ParticleSubsystem::bodyVelocity(Entity entity, Vector3& velocity){
#ifdef JASPERENGINE_PHYSICS_3D
    if (const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(entity); body && !body->body.IsInvalid()){
        velocity = Rigidbody(scene, entity).getLinearVelocity();
        return true;
    }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    if (const Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(entity); body && b2Body_IsValid(body->body)){
        const Vector2 v = Rigidbody2D(scene, entity).getLinearVelocity();
        velocity = Vector3(v.x, v.y, 0.0f);
        return true;
    }
#endif
    (void)entity;
    (void)velocity;
    return false;
}

void ParticleSubsystem::prepareInput(Entity entity, ParticleSystemComponent& particles, Transform& transform, EmitterInput& input){
    ParticleSystemState& state = *particles.state;
    copyMatrix(transform.modelMatrix, state.transform);
    input.transform = state.transform;
    input.seed = state.seed;
    input.speed = std::max(0.0f, particles.playbackSpeed);
    input.jobs = &jobs::system();

    const ParticleProgram* prog = state.runtime.program();
    // the scaling mode's factor on start sizes: Hierarchy the world scale, Local the GameObject's own
    float sizeScale = 1.0f;
    if (prog){
        if (prog->scalingMode == ScalingMode::Hierarchy)
            sizeScale = (std::fabs(transform.worldScale.x) + std::fabs(transform.worldScale.y) + std::fabs(transform.worldScale.z)) / 3.0f;
        else if (prog->scalingMode == ScalingMode::Local)
            sizeScale = (std::fabs(transform.scale.x) + std::fabs(transform.scale.y) + std::fabs(transform.scale.z)) / 3.0f;
    }
    input.sizeScale = sizeScale;
    // the gravity the Gravity Modifier scales: the 3D world's, or the 2D world's in the scene's
    // units (its meters times the points per meter)
    Vector3 gravity = scene->getGravity3D();
    if (prog && prog->gravitySource == GravitySource::Physics2D){
        const Vector2 g2 = scene->getGravity2D();
        float pointsPerMeter = 64.0f;
#ifdef JASPERENGINE_PHYSICS_2D
        if (PhysicsSystem* physics = scene->findSystem<PhysicsSystem>().get())
            pointsPerMeter = physics->getPointsToMeterScale2D();
#endif
        gravity = Vector3(g2.x * pointsPerMeter, g2.y * pointsPerMeter, 0.0f);
    }
    input.gravity[0] = gravity.x;
    input.gravity[1] = gravity.y;
    input.gravity[2] = gravity.z;
    // Emitter Velocity Rigidbody: the body's velocity (one that is made already), else the motion
    input.hasVelocity = false;
    if (prog && prog->emitterVelocityMode == EmitterVelocityMode::Rigidbody){
        Vector3 v;
        if (bodyVelocity(entity, v)){
            input.hasVelocity = true;
            input.velocity[0] = v.x; input.velocity[1] = v.y; input.velocity[2] = v.z;
        }
    }

    // the force fields that overlap it (its bounds, or everywhere before it has any)
    state.fields.clear();
    if (prog && !fields.empty() && state.effect->document.externalForces.present && state.effect->document.externalForces.enabled){
        if (state.fields.capacity() < kMaxForceFields)
            state.fields.reserve(kMaxForceFields);
        state.fields.resize(kMaxForceFields);
        const float everywhereMin[3] = {-3.0e38f, -3.0e38f, -3.0e38f}, everywhereMax[3] = {3.0e38f, 3.0e38f, 3.0e38f};
        const uint32_t n = ParticleWorld::cullFields(fields.data(), (uint32_t)fields.size(), state.hasBounds ? state.boundsMin : everywhereMin,
                                                     state.hasBounds ? state.boundsMax : everywhereMax,
                                                     state.effect->document.externalForces.layerMask, state.fields.data(), kMaxForceFields);
        state.fields.resize(n);
    }
    input.fields = state.fields.data();
    input.fieldCount = (uint32_t)state.fields.size();

    // world collisions and the component's plane GameObjects
    input.collision = nullptr;
    if (prog && prog->hasCollision){
        if (prog->collisionKind == CollisionKind::World2D)
            input.collision = &provider2D;
        else if (prog->collisionKind == CollisionKind::World || prog->collisionKind == CollisionKind::Terrain)
            input.collision = &provider3D;
    }
    state.planeCount = 0;
    for (Entity planeEntity : particles.collisionPlanes){
        if (state.planeCount >= kMaxCollisionPlanes)
            break;
        Transform* planeTransform = scene->findComponent<Transform>(planeEntity);
        if (!planeTransform)
            continue;
        Vector3 n = planeTransform->worldRotation * Vector3(0.0f, 1.0f, 0.0f);
        if (n.length() <= 0.0f)
            continue;
        n.normalize();
        const Vector3& p = planeTransform->worldPosition;
        float* plane = state.planes[state.planeCount++];
        plane[0] = n.x; plane[1] = n.y; plane[2] = n.z;
        plane[3] = -(n.x * p.x + n.y * p.y + n.z * p.z);
    }
    input.planes = state.planeCount ? state.planes : nullptr;
    input.planeCount = state.planeCount;
}

void ParticleSubsystem::gatherFields(){
    fields.clear();
    fieldTime += Engine::getDeltatime();
    auto components = scene->getComponentArray<ForceFieldComponent>();
    for (size_t i = 0; i < components->size() && fields.size() < kMaxForceFields; i++){
        const ForceFieldComponent& f = components->getComponentFromIndex(i);
        const Entity entity = components->getEntity(i);
        if (!f.enabled || !scene->isEntityActiveInHierarchy(entity))
            continue;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;
        ForceFieldSample s;
        s.shape = (ForceFieldShape)f.shape;
        s.type = (ForceFieldType)f.kind;
        s.layers = f.layers;
        Vector3 size(1.0f, 1.0f, 1.0f);
        switch (f.shape){
            case ForceFieldShapeType::Sphere: size = Vector3(f.size.x, f.size.x, f.size.x); break;
            case ForceFieldShapeType::Box: size = f.size; break;
            case ForceFieldShapeType::Cylinder: size = Vector3(f.size.x, f.size.y, f.size.x); break;
            case ForceFieldShapeType::Infinite: break;
        }
        const Matrix4 shapeToWorld = transform->modelMatrix * Matrix4::scaleMatrix(size);
        const Matrix4 worldToShape = shapeToWorld.inverse();
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 3; r++) s.toField[c * 3 + r] = worldToShape[c][r];
        const Vector3& center = transform->worldPosition;
        s.center[0] = center.x; s.center[1] = center.y; s.center[2] = center.z;
        Vector3 axis = transform->worldRotation * f.direction;
        if (axis.length() > 0.0f) axis.normalize();
        s.axis[0] = axis.x; s.axis[1] = axis.y; s.axis[2] = axis.z;
        // a pulsing wind: its strength over the pulse, this frame
        float strength = f.strength;
        if (f.kind == ForceFieldKind::Directional && f.pulseFrequency > 0.0f)
            strength *= 0.5f + 0.5f * std::sin(kTwoPi * f.pulseFrequency * (float)fieldTime);
        s.strength = strength;
        s.turbulence = f.turbulence;
        s.frequency = f.frequency;
        for (int k = 0; k < 17; k++) s.falloff[k] = f.falloff.evaluate((float)k / 16.0f);
        // the shape's world box (Infinite: everywhere)
        if (f.shape == ForceFieldShapeType::Infinite){
            for (int k = 0; k < 3; k++){ s.aabbMin[k] = -3.0e38f; s.aabbMax[k] = 3.0e38f; }
        }else{
            for (int k = 0; k < 3; k++){ s.aabbMin[k] = 3.0e38f; s.aabbMax[k] = -3.0e38f; }
            for (int corner = 0; corner < 8; corner++){
                const Vector3 local((corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f, (corner & 4) ? 1.0f : -1.0f);
                const Vector3 w = shapeToWorld * local;
                const float wp[3] = {w.x, w.y, w.z};
                for (int k = 0; k < 3; k++){
                    s.aabbMin[k] = std::min(s.aabbMin[k], wp[k]);
                    s.aabbMax[k] = std::max(s.aabbMax[k], wp[k]);
                }
            }
        }
        s.seed = (uint32_t)entity * 0x9e3779b9u + 1u;
        fields.push_back(s);
    }
}

void ParticleSubsystem::gatherTerrains(){
    terrains.clear();
    auto components = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < components->size(); i++){
        TerrainComponent& terrain = components->getComponentFromIndex(i);
        const Entity entity = components->getEntity(i);
        if (!scene->isEntityActiveInHierarchy(entity))
            continue;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;
        TerrainRef ref;
        ref.terrain = &terrain;
        ref.localToWorld = transform->modelMatrix;
        ref.worldToLocal = transform->modelMatrix.inverse();
        ref.halfSize = terrain.terrainSize * 0.5f;
        terrains.push_back(ref);
    }
}

bool ParticleSubsystem::sampleTerrain(const void* context, const float p[3], float surface[3], float normal[3]){
    const ParticleSubsystem* self = static_cast<const ParticleSubsystem*>(context);
    for (const TerrainRef& t : self->terrains){
        const Vector3 local = t.worldToLocal * point(p);
        if (std::fabs(local.x) > t.halfSize || std::fabs(local.z) > t.halfSize)
            continue;
        float height = 0.0f;
        Vector3 n;
        MeshSystem::sampleTerrainSurface(*t.terrain, local.x, local.z, height, n);
        const Vector3 world = t.localToWorld * Vector3(local.x, height, local.z);
        // normals by the inverse transpose
        Vector3 wn(t.worldToLocal[0][0] * n.x + t.worldToLocal[0][1] * n.y + t.worldToLocal[0][2] * n.z,
                   t.worldToLocal[1][0] * n.x + t.worldToLocal[1][1] * n.y + t.worldToLocal[1][2] * n.z,
                   t.worldToLocal[2][0] * n.x + t.worldToLocal[2][1] * n.y + t.worldToLocal[2][2] * n.z);
        if (wn.length() > 0.0f) wn.normalize();
        surface[0] = world.x; surface[1] = world.y; surface[2] = world.z;
        normal[0] = wn.x; normal[1] = wn.y; normal[2] = wn.z;
        return true;
    }
    return false;
}

bool ParticleSubsystem::castWorldRay(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, CollisionHit& hit){
#ifdef JASPERENGINE_PHYSICS_3D
    const ParticleSubsystem* self = static_cast<const ParticleSubsystem*>(context);
    Vector3 direction = point(to) - point(from);
    const float length = direction.length();
    if (length <= 0.0f)
        return false;
    // extended by the radius: the contact is a radius before the surface
    direction = direction * ((length + radius) / length);
    const RayReturn result = Physics::raycast(self->scene, Ray(point(from), direction), layers);
    if (!result.hit)
        return false;
    hit.position[0] = result.point.x; hit.position[1] = result.point.y; hit.position[2] = result.point.z;
    hit.normal[0] = result.normal.x; hit.normal[1] = result.normal.y; hit.normal[2] = result.normal.z;
    return true;
#else
    (void)context; (void)from; (void)to; (void)radius; (void)layers; (void)hit;
    return false;
#endif
}

bool ParticleSubsystem::castWorldRay2D(const void* context, const float from[3], const float to[3], float radius, uint32_t layers, CollisionHit& hit){
#ifdef JASPERENGINE_PHYSICS_2D
    const ParticleSubsystem* self = static_cast<const ParticleSubsystem*>(context);
    Vector3 direction = point(to) - point(from);
    direction.z = 0.0f;
    const float length = direction.length();
    if (length <= 0.0f)
        return false;
    direction = direction * ((length + radius) / length);
    const RayReturn result = Physics2D::raycast(self->scene, Ray(Vector3(from[0], from[1], 0.0f), direction), layers);
    if (!result.hit)
        return false;
    hit.position[0] = result.point.x; hit.position[1] = result.point.y; hit.position[2] = from[2];
    hit.normal[0] = result.normal.x; hit.normal[1] = result.normal.y; hit.normal[2] = 0.0f;
    return true;
#else
    (void)context; (void)from; (void)to; (void)radius; (void)layers; (void)hit;
    return false;
#endif
}

namespace {

    // an emitter's particles and its children's, each in its own simulation space, into a world box
    void addEmitterBounds(const EmitterRuntime& emitter, bool withSelf, float lo[3], float hi[3], bool& any){
        if (withSelf && emitter.hasBounds()){
            float simLo[3], simHi[3];
            emitter.bounds(simLo, simHi);
            const float* m = emitter.simToWorld();
            for (int corner = 0; corner < 8; corner++){
                const float p[3] = {(corner & 1) ? simHi[0] : simLo[0], (corner & 2) ? simHi[1] : simLo[1], (corner & 4) ? simHi[2] : simLo[2]};
                for (int k = 0; k < 3; k++){
                    const float w = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
                    lo[k] = std::min(lo[k], w);
                    hi[k] = std::max(hi[k], w);
                }
            }
            any = true;
        }
        for (size_t c = 0; c < emitter.childCount(); c++){
            if (const EmitterRuntime* child = emitter.child(c))
                addEmitterBounds(*child, true, lo, hi, any);
        }
    }

}

void ParticleSubsystem::updateBounds(ParticleSystemState& state){
    const ParticleEffectDocument& d = state.effect->document;
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++){ lo[k] = 3.0e38f; hi[k] = -3.0e38f; }
    bool any = false;
    if (d.boundsMode == BoundsMode::Authored){
        // the authored box, in the emitter's space
        const float c[3] = {d.boundsCenter.x, d.boundsCenter.y, d.boundsCenter.z};
        const float h[3] = {d.boundsSize.x * 0.5f, d.boundsSize.y * 0.5f, d.boundsSize.z * 0.5f};
        const float* e = state.transform;
        for (int corner = 0; corner < 8; corner++){
            const float p[3] = {c[0] + ((corner & 1) ? h[0] : -h[0]), c[1] + ((corner & 2) ? h[1] : -h[1]), c[2] + ((corner & 4) ? h[2] : -h[2])};
            for (int k = 0; k < 3; k++){
                const float w = e[k] * p[0] + e[4 + k] * p[1] + e[8 + k] * p[2] + e[12 + k];
                lo[k] = std::min(lo[k], w);
                hi[k] = std::max(hi[k], w);
            }
        }
        any = true;
    }
    // the authored box answers for this emitter only: its children keep their own particles' box
    addEmitterBounds(state.runtime, d.boundsMode != BoundsMode::Authored, lo, hi, any);
    if (!any){
        state.hasBounds = false;
        return;
    }
    std::memcpy(state.boundsMin, lo, sizeof(lo));
    std::memcpy(state.boundsMax, hi, sizeof(hi));
    state.hasBounds = true;
}

float ParticleSubsystem::significanceOf(const ParticleSystemState& state, float& cameraDistance) const{
    cameraDistance = 0.0f;
    const Entity cameraEntity = scene->getCamera();
    const Transform* cameraTransform = scene->findComponent<Transform>(cameraEntity);
    const CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity);
    if (!cameraTransform || !camera)
        return 1.0f;
    const Vector3 center = state.hasBounds ? Vector3((state.boundsMin[0] + state.boundsMax[0]) * 0.5f, (state.boundsMin[1] + state.boundsMax[1]) * 0.5f,
                                                     (state.boundsMin[2] + state.boundsMax[2]) * 0.5f)
                                           : Vector3(state.transform[12], state.transform[13], state.transform[14]);
    cameraDistance = (center - cameraTransform->worldPosition).length();
    if (!state.hasBounds)
        return 1.0f;
    const float radius = 0.5f * Vector3(state.boundsMax[0] - state.boundsMin[0], state.boundsMax[1] - state.boundsMin[1],
                                        state.boundsMax[2] - state.boundsMin[2]).length();
    if (camera->type != CameraType::CAMERA_PERSPECTIVE)
        return 1.0f;
    const float view = std::max(cameraDistance, 0.001f) * 2.0f * std::tan(camera->yfov * 0.5f);
    return std::clamp(2.0f * radius / std::max(view, 0.001f), 0.0f, 1.0f);
}

void ParticleSubsystem::dispatchEvents(ParticleSystemComponent& particles){
    ParticleSystemState& state = *particles.state;
    if (particles.onCollision.hasSubscribers()){
        const auto events = state.runtime.collisionEvents();
        if (!events.empty()){
            collisionBatch.clear();
            for (const CollisionEvent& e : events)
                collisionBatch.push_back({point(e.position), point(e.normal), point(e.velocity), e.particleId, e.killed});
            particles.onCollision.call(collisionBatch);
        }
    }
    if (particles.onParticleDeath.hasSubscribers()){
        const auto events = state.runtime.deathEvents();
        if (!events.empty()){
            deathBatch.clear();
            for (const ParticleDeathEvent& e : events)
                deathBatch.push_back({point(e.position), point(e.velocity), e.particleId});
            particles.onParticleDeath.call(deathBatch);
        }
    }
    if (particles.onSubEmit.hasSubscribers()){
        const auto events = state.runtime.subEmitEvents();
        if (!events.empty()){
            static const char* const triggers[] = {"birth", "death", "collision", "trigger"};
            subEmitBatch.clear();
            for (const SubEmitEvent& e : events)
                subEmitBatch.push_back({point(e.position), (int)e.entry, (int)e.count, triggers[(int)e.trigger]});
            particles.onSubEmit.call(subEmitBatch);
        }
    }
}

namespace {

    // the frame stats (main thread): simulation time and alive particles since the last take
    double frameSimulationSeconds = 0.0;
    size_t frameAlive = 0;

}

double ParticleSubsystem::takeFrameSimulation(size_t* alive){
    const double ms = frameSimulationSeconds * 1000.0;
    if (alive) *alive = frameAlive;
    frameSimulationSeconds = 0.0;
    frameAlive = 0;
    return ms;
}

void ParticleSubsystem::update(double dt){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    // a scene without particle systems pays nothing more (R26)
    if (systems->size() == 0)
        return;
    // Edit mode (the editor pauses the system): the effects bind so the views show their
    // state; only the editor's preview simulates
    if (paused){
        for (size_t i = 0; i < systems->size(); i++){
            ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
            if (particles.needReload || !particles.state || subEmittersOutOfDate(particles))
                ensureEffect(systems->getEntity(i), particles);
        }
        return;
    }

    // the frame's render stats start over (RenderSystem adds its draws to them)
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        if (particles.state){
            particles.state->renderMicros = 0.0;
            particles.state->drawnViews = 0;
            particles.state->sortedViews = 0;
        }
    }

    gatherFields();
    gatherTerrains();

    tasks.clear();
    taskEntities.clear();
    const uint64_t frame = SystemRender::getFrameCount();
    const double unscaledDt = Engine::getUnscaledDeltatime();
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        const Entity entity = systems->getEntity(i);
        if (!scene->isEntityActiveInHierarchy(entity)){
            // a deactivated GameObject stops and clears its system (Unity); Play On Awake plays it
            // again when it comes back. Its stop action does not run: nothing finished
            if (particles.state && (particles.state->started || particles.state->runtime.isAlive())){
                particles.state->runtime.stop(true);
                particles.state->started = false;
                particles.state->running = false;
            }
            continue;
        }
        if (!particles.enabled)
            continue;
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            continue;
        if (!ensureEffect(entity, particles))
            continue;
        ParticleSystemState& state = *particles.state;
        if (!state.started){
            state.started = true;
            if (particles.playOnAwake){
                state.runtime.play();
                state.running = true;
            }
        }
        EmitterTask task;
        task.runtime = &state.runtime;

        const ParticleEffectDocument& d = state.effect->document;
        float cameraDistance = 0.0f;
        state.significance = significanceOf(state, cameraDistance);
        task.priority = d.priority;
        task.significance = state.significance;
        // distance LOD: the emission over the camera distance, culled past the far end
        bool inRange = true;
        if (d.lod.enabled){
            const float t = d.lod.maxDistance > 0.0f ? cameraDistance / d.lod.maxDistance : 0.0f;
            inRange = t <= 1.0f;
            task.input.emissionScale = std::max(0.0f, d.lod.emissionScale.evaluate(std::min(t, 1.0f), 0.5f));
        }
        // a camera saw it last frame (a new system counts as seen until it has bounds; so does
        // an empty one: nothing of it is drawn to be seen, and its old bounds would hold it
        // paused for good once stopped and cleared)
        const bool visible = inRange && (!state.hasBounds || state.runtime.totalCount() == 0 || frame <= state.visibleFrame + 1);
        uint32_t maxTicks = kMaxTicksPerFrame;
        // Unscaled Time (the effect's or the component's): Time.timeScale does not slow it (UI,
        // pause menus)
        const double frameDt = (d.useUnscaledTime || particles.useUnscaledTime) ? unscaledDt : dt;
        task.dt = cullingStep(d.cullingMode, d.looping, visible, frameDt, d.tickRate, state.cull, maxTicks);
        task.maxTicks = maxTicks;
        state.culled = !visible;
        if (task.dt <= 0.0){
            // paused by its culling: nothing to simulate this frame (R15: a culled emitter is nearly free)
            state.ticks = 0;
            continue;
        }
        prepareInput(entity, particles, *transform, task.input);
        tasks.push_back(task);
        taskEntities.push_back(entity);
    }
    if (tasks.empty()){
        checkStopActions();
        return;
    }

#ifdef JASPERENGINE_PHYSICS_3D
    // world collisions query Jolt from the workers: never while it steps
    if (scene->getSystem<PhysicsSystem>()->isSteppingWorld3D()){
        Log::error("[Particles] the physics world is stepping: the particle simulation waits for the next frame");
        return;
    }
#endif
    const auto simulateStart = std::chrono::steady_clock::now();
    world.simulate(tasks, &jobs::system());
    frameSimulationSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - simulateStart).count();

    const double perEmitter = world.stats().simulateMicros / (double)tasks.size();
    for (size_t t = 0; t < tasks.size(); t++){
        const Entity entity = taskEntities[t];
        ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
        ParticleSystemState& state = *particles.state;
        state.ticks = tasks[t].ticks;
        state.budget = tasks[t].budget;
        state.emissionScale = tasks[t].emissionScale;
        state.simulateMicros = perEmitter;
        frameAlive += state.runtime.totalCount();
        updateBounds(state);
        dispatchEvents(particles);
    }
    checkStopActions();
}

// the stop actions of the systems that have finished (this frame, or stopped by a script since
// the last one): after the events, outside the loops over the component arrays
void ParticleSubsystem::checkStopActions(){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        if (!particles.state || !particles.state->effect)
            continue;
        ParticleSystemState& state = *particles.state;
        const bool runningNow = state.runtime.state() != PlayState::Stopped || state.runtime.isAlive();
        if (state.running && !runningNow)
            queueStopAction(systems->getEntity(i), state);
        state.running = runningNow;
    }
    runStopActions();
}

void ParticleSubsystem::queueStopAction(Entity entity, ParticleSystemState& state){
    if (state.effect->program.stopAction == StopAction::None)
        return;
    if (std::find(stopActions.begin(), stopActions.end(), entity) == stopActions.end())
        stopActions.push_back(entity);
}

void ParticleSubsystem::runStopActions(){
    if (stopActions.empty())
        return;
    // a callback can play a system again, deactivate or destroy others: a copy, each checked
    std::vector<Entity> entities;
    entities.swap(stopActions);
    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity))
            continue;
        ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
        if (!particles || !particles->state || !particles->state->effect)
            continue;
        // played again meanwhile: nothing has stopped any more
        if (particles->state->runtime.state() != PlayState::Stopped)
            continue;
        switch (particles->state->effect->program.stopAction){
            case StopAction::None:
                break;
            case StopAction::Disable:
                scene->setEntityActive(entity, false);
                break;
            case StopAction::Destroy:
                ObjectManager::destroy(scene, entity, 0.0f);
                break;
            case StopAction::Callback:
                ScriptMessages::send(scene, entity, ScriptMessage::ParticleSystemStopped, nullptr,
                                     [](ScriptBase& script){ script.onParticleSystemStopped(); });
                break;
        }
    }
    // (a callback that queued more: they run next frame)
    if (stopActions.capacity() < entities.capacity())
        stopActions.reserve(entities.capacity());
}
