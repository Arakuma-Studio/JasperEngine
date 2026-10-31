// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Version 2 Particle Systems in the ActionSystem (particles spec 6.5): the module simulation
// (particles/ParticleSimulation) runs from here in Play, the editor preview and exported builds
// alike, and its particles are written to the system's renderer: the InstancedMeshComponent of
// a Mesh target (by default the system's own GameObject: a quad) or a Points target. Version 1
// systems keep their own code path (ActionSystem.cpp) untouched.

#include "ActionSystem.h"
#include "script/ScriptMessages.h"
#include "Scene.h"
#include "Log.h"
#include "Engine.h"
#include "manager/ObjectManager.h"
#include "particles/ParticleRandom.h"
#include "particles/ParticleSimulation.h"
#include "script/LuaBinding.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#endif

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    Vector3 mul(const Vector3& a, const Vector3& b){
        return Vector3(a.x * b.x, a.y * b.y, a.z * b.z);
    }

    InstanceAlignment instanceAlignment(const ParticleRendererModule& renderer){
        switch (renderer.renderMode){
            case ParticleSystemRenderMode::Stretch: return InstanceAlignment::Stretched;
            case ParticleSystemRenderMode::HorizontalBillboard: return InstanceAlignment::Horizontal;
            case ParticleSystemRenderMode::VerticalBillboard: return InstanceAlignment::Vertical;
            case ParticleSystemRenderMode::Billboard:
            case ParticleSystemRenderMode::Mesh:
                break;
        }
        switch (renderer.alignment){
            case ParticleSystemRenderSpace::View: return InstanceAlignment::View;
            case ParticleSystemRenderSpace::World: return InstanceAlignment::World;
            case ParticleSystemRenderSpace::Local: return InstanceAlignment::Local;
            case ParticleSystemRenderSpace::Facing: return InstanceAlignment::Facing;
            case ParticleSystemRenderSpace::Velocity: return InstanceAlignment::Velocity;
        }
        return InstanceAlignment::View;
    }

}

bool ActionSystem::isParticleSystemV2(Entity entity) const{
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    return ps && ps->version >= 2;
}

Entity ActionSystem::particleTarget(Entity entity) const{
    const ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    if (!action || action->target == NULL_ENTITY || !scene->isEntityCreated(action->target)){
        return entity;
    }
    return action->target;
}

ParticleEmitterFrame ActionSystem::particleEmitterFrame(Entity entity){
    ParticleEmitterFrame frame;
    // the emitter: the system's own GameObject, or the one a legacy layout targets
    Entity emitter = entity;
    if (!scene->findComponent<Transform>(entity)){
        emitter = particleTarget(entity);
    }
    Transform* transform = scene->findComponent<Transform>(emitter);
    if (transform){
        updateParticleTargetTransform(*transform);
        frame.position = transform->worldPosition;
        frame.rotation = transform->worldRotation;
        frame.worldScale = transform->worldScale;
        frame.localScale = transform->scale;
    }
    // emitterVelocityMode Rigidbody: the body's velocity (one that is made already)
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (ps && ps->main.emitterVelocityMode == ParticleSystemEmitterVelocityMode::Rigidbody){
#ifdef JASPERENGINE_PHYSICS_3D
        if (const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(emitter); body && !body->body.IsInvalid()){
            frame.rigidbodyVelocity = Rigidbody(scene, emitter).getLinearVelocity();
            frame.hasRigidbodyVelocity = true;
        }
#endif
#ifdef JASPERENGINE_PHYSICS_2D
        if (const Rigidbody2DComponent* body2D = scene->findComponent<Rigidbody2DComponent>(emitter); !frame.hasRigidbodyVelocity && body2D && b2Body_IsValid(body2D->body)){
            const Vector2 v = Rigidbody2D(scene, emitter).getLinearVelocity();
            frame.rigidbodyVelocity = Vector3(v.x, v.y, 0.0f);
            frame.hasRigidbodyVelocity = true;
        }
#endif
    }
    return frame;
}

ParticleEnvironment ActionSystem::particleEnvironment(){
    ParticleEnvironment env;
    env.gravity3D = scene->getGravity3D();
    const Vector2 g2 = scene->getGravity2D();
    float pointsPerMeter = 64.0f;
#ifdef JASPERENGINE_PHYSICS_2D
    if (PhysicsSystem* physics = scene->findSystem<PhysicsSystem>().get()) pointsPerMeter = physics->getPointsToMeterScale2D();
#endif
    env.gravity2D = Vector3(g2.x * pointsPerMeter, g2.y * pointsPerMeter, 0.0f);
    return env;
}

void ActionSystem::collectParticleSystems(Entity entity, bool withChildren, std::vector<Entity>& out){
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return;
    if (isParticleSystemV2(entity)) out.push_back(entity);
    if (!withChildren) return;
    // the children in hierarchy order (Transform order is depth first)
    auto transforms = scene->getComponentArray<Transform>();
    if (!transforms->hasEntity(entity)) return;
    const size_t start = transforms->getIndex(entity);
    for (size_t i = start + 1; i < transforms->size(); i++){
        Entity child = transforms->getEntity(i);
        // the subtree ends at the first one that is not a descendant
        bool descendant = false;
        Entity parent = transforms->getComponentFromIndex(i).parent;
        while (parent != NULL_ENTITY){
            if (parent == entity){ descendant = true; break; }
            Transform* t = scene->findComponent<Transform>(parent);
            parent = t ? t->parent : NULL_ENTITY;
        }
        if (!descendant) break;
        if (isParticleSystemV2(child)) out.push_back(child);
    }
}

std::vector<Entity> ActionSystem::particleSystemsOf(Entity entity, bool withChildren){
    std::vector<Entity> systems;
    collectParticleSystems(entity, withChildren, systems);
    return systems;
}

void ActionSystem::prepareParticleRenderer(Entity entity, ParticleSystemComponent& ps){
    const Entity target = particleTarget(entity);
    const unsigned int capacity = std::min<unsigned int>(ps.main.maxParticles, 100000u);
    if (scene->findComponent<MeshComponent>(target)){
        if (!scene->findComponent<InstancedMeshComponent>(target)){
            scene->getSystem<MeshSystem>()->createInstancedMesh(target);
        }
        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(target);
        MeshComponent& mesh = scene->getComponent<MeshComponent>(target);
        if (instmesh.maxInstances != std::max(1u, capacity)){
            instmesh.maxInstances = std::max(1u, capacity);
            if (mesh.loaded) mesh.needReload = true;
        }
        // particles leave the bounds of the mesh: no per-instance culling of a moving batch
        instmesh.instances.reserve(capacity);
    }else if (PointsComponent* points = scene->findComponent<PointsComponent>(target)){
        if (points->maxPoints != std::max(1u, capacity)){
            points->maxPoints = std::max(1u, capacity);
            points->needReload = true;
        }
        points->points.reserve(capacity);
    }
}

void ActionSystem::syncParticleRenderer(Entity entity, ParticleSystemComponent& ps){
    const Entity target = particleTarget(entity);
    const ParticleSimState& s = ps.state;
    const size_t count = s.count();

    const ParticleEmitterFrame emitter = particleEmitterFrame(entity);
    Transform* targetTransform = scene->findComponent<Transform>(target);
    if (targetTransform && targetTransform != scene->findComponent<Transform>(entity)){
        updateParticleTargetTransform(*targetTransform);
    }
    const Matrix4 simToWorld = particles::simulationToWorld(ps, emitter);
    Matrix4 worldToTarget;
    if (targetTransform){
        const Matrix4 inverse = targetTransform->modelMatrix.inverse();
        if (inverse.isValid()) worldToTarget = inverse;
    }
    const Matrix4 simToTarget = worldToTarget * simToWorld;
    const Vector3 sizeScale = particles::sizeScale(ps, emitter);
    const ParticleRendererModule& renderer = ps.renderer;
    const ParticleTextureSheetAnimationModule& sheet = ps.textureSheetAnimation;
    const bool reverse = renderer.sortMode == ParticleSystemSortMode::OldestInFront;
    // the shape's alignToDirection rotation (and Local systems' particles) turn with the emitter
    const bool localRotation = ps.main.simulationSpace == ParticleSystemSimulationSpace::Local;

    auto sheetRect = [&](size_t i, const HybridArray<SpriteFrameData, MAX_SPRITE_FRAMES>* frames, unsigned int numFrames) -> Rect {
        if (!sheet.enabled) return Rect(0, 0, 1, 1);
        const int frame = (int)s.frame[i];
        if (sheet.mode == ParticleSystemAnimationMode::Sprites){
            if (sheet.sprites.empty() || !frames) return Rect(0, 0, 1, 1);
            const int id = sheet.sprites[(size_t)std::max(0, frame) % sheet.sprites.size()];
            if (id >= 0 && frames->validIndex(id) && (unsigned int)id < numFrames) return (*frames)[id].rect;
            return Rect(0, 0, 1, 1);
        }
        return particles::textureSheetRect(sheet, frame, particles::textureSheetRow(ps, i));
    };

    auto worldSize = [&](size_t i) -> Vector3 {
        Vector3 size = mul(s.size[i], sizeScale);
        const uint32_t seed = s.seed[i];
        if (renderer.flip.x > 0.0f && particles::particleRandom(seed, particles::RANDOM_FLIP_X) < renderer.flip.x) size.x = -size.x;
        if (renderer.flip.y > 0.0f && particles::particleRandom(seed, particles::RANDOM_FLIP_Y) < renderer.flip.y) size.y = -size.y;
        if (renderer.flip.z > 0.0f && particles::particleRandom(seed, particles::RANDOM_FLIP_Z) < renderer.flip.z) size.z = -size.z;
        if (sheet.enabled && sheet.flipU > 0.0f && particles::particleRandom(seed, particles::RANDOM_FLIP_U) < sheet.flipU) size.x = -size.x;
        if (sheet.enabled && sheet.flipV > 0.0f && particles::particleRandom(seed, particles::RANDOM_FLIP_V) < sheet.flipV) size.y = -size.y;
        return size;
    };

    if (InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(target)){
        InstanceParticleRender& render = instmesh->particle;
        render.alignment = instanceAlignment(renderer);
        render.pivot = renderer.pivot;
        render.lengthScale = renderer.lengthScale;
        render.velocityScale = renderer.velocityScale;
        render.cameraVelocityScale = renderer.cameraVelocityScale;
        render.minSize = renderer.minParticleSize;
        render.maxSize = renderer.maxParticleSize;
        render.sort = renderer.sortMode == ParticleSystemSortMode::Distance ? InstanceSortMode::ByDistance : InstanceSortMode::Keep;
        render.sortingFudge = renderer.sortingFudge;
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(target)){
            mesh->transparentSortPriority = renderer.sortPriority;
        }
        SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target);

        instmesh->instances.resize(std::min(count, (size_t)std::max(1u, instmesh->maxInstances)));
        const size_t drawn = instmesh->instances.size();
        for (size_t k = 0; k < drawn; k++){
            const size_t i = reverse ? count - 1 - k : k;
            InstanceData& d = instmesh->instances[k];
            d.position = simToTarget * s.position[i];
            d.scale = worldSize(i);
            const Vector3& r = s.rotation[i];
            Quaternion rotation = particles::eulerRadians(r);
            if (ps.shape.alignToDirection) rotation = s.alignRotation[i] * rotation;
            // World / Velocity / Facing frames are the world's: a Local system's particle turns
            // with its emitter there
            if (localRotation && (render.alignment == InstanceAlignment::World)) rotation = emitter.rotation * rotation;
            d.rotation = rotation;
            d.roll = r.z;
            d.velocity = particles::simulationVectorToWorld(ps, emitter, s.totalVelocity[i]);
            d.color = s.color[i];
            d.textureRect = sheetRect(i, sprite ? &sprite->framesRect : nullptr, sprite ? sprite->numFramesRect : 0);
            d.visible = true;
        }
        instmesh->needUpdateInstances = true;
    }else if (PointsComponent* points = scene->findComponent<PointsComponent>(target)){
        points->sortingFudge = renderer.sortingFudge;
        points->points.resize(std::min(count, (size_t)std::max(1u, points->maxPoints)));
        const size_t drawn = points->points.size();
        for (size_t k = 0; k < drawn; k++){
            const size_t i = reverse ? count - 1 - k : k;
            PointData& p = points->points[k];
            p.position = simToTarget * s.position[i];
            // a point's size is a world diameter whatever its GameObject's scale
            const Vector3 size = worldSize(i);
            p.size = std::fabs(size.x);
            p.rotation = s.rotation[i].z;
            p.color = s.color[i];
            p.textureRect = sheetRect(i, &points->framesRect, points->numFramesRect);
            p.visible = true;
        }
        points->needUpdate = true;
    }
}

void ActionSystem::particleSystemStart(Entity entity, ParticleSystemComponent& ps){
    prepareParticleRenderer(entity, ps);
    // the component may have moved (an InstancedMeshComponent added)
    ParticleSystemComponent* current = scene->findComponent<ParticleSystemComponent>(entity);
    if (!current) return;
    if (!current->state.playing || current->state.paused || !current->state.emitting){
        particles::play(*current, particleEmitterFrame(entity), particleEnvironment());
    }
    syncParticleRenderer(entity, *current);
}

void ActionSystem::particleSystemHardStop(Entity entity){
    ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps || ps->version < 2) return;
    const bool wasPlaying = ps->state.playing;
    particles::stop(*ps, ParticleSystemStopBehavior::StopEmittingAndClear);
    syncParticleRenderer(entity, *ps);
    if (wasPlaying){
        queueParticleStopAction(entity);
    }
}

void ActionSystem::queueParticleStopAction(Entity entity){
    const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps || ps->main.stopAction == ParticleSystemStopAction::None) return;
    if (std::find(pendingParticleStops.begin(), pendingParticleStops.end(), entity) == pendingParticleStops.end()){
        pendingParticleStops.push_back(entity);
    }
}

void ActionSystem::runParticleStopActions(){
    if (pendingParticleStops.empty()) return;
    std::vector<Entity> entities;
    entities.swap(pendingParticleStops);
    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity)) continue;
        const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
        if (!ps) continue;
        // played again meanwhile (a callback, a script): nothing stopped any more
        if (ps->state.playing) continue;
        switch (ps->main.stopAction){
            case ParticleSystemStopAction::None:
                break;
            case ParticleSystemStopAction::Disable:
                scene->setEntityActive(entity, false);
                break;
            case ParticleSystemStopAction::Destroy:
                ObjectManager::destroy(scene, entity, 0.0f);
                break;
            case ParticleSystemStopAction::Callback:
                ScriptMessages::send(scene, entity, ScriptMessage::ParticleSystemStopped, nullptr,
                                     [](ScriptBase& script){ script.onParticleSystemStopped(); });
                break;
        }
    }
}

void ActionSystem::finishParticleSystem(Entity entity){
    queueParticleStopAction(entity);
    ActionComponent* action = scene->findComponent<ActionComponent>(entity);
    if (action && (action->state == ActionState::Running || action->state == ActionState::Paused)){
        action->startTrigger = false;
        actionStop(entity);
    }
}

void ActionSystem::stepParticleSystem(double dt, double unscaledDt, Entity entity){
    ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps || ps->version < 2 || !ps->enabled) return;
    ActionComponent* action = nullptr;

    if (ps->state.playing || ps->state.count() > 0){
        // (a renderer added to the target can move component arrays)
        prepareParticleRenderer(entity, *ps);
        ps = scene->findComponent<ParticleSystemComponent>(entity);
        if (!ps) return;
        action = scene->findComponent<ActionComponent>(entity);
        const float speed = (action ? action->speed : 1.0f) * ps->main.simulationSpeed;
        const double delta = ps->useUnscaledTime ? unscaledDt : dt;
        const float step = (float)(delta * std::max(0.0f, speed));
        const bool finished = particles::step(*ps, step, particleEmitterFrame(entity), particleEnvironment());
        action = scene->findComponent<ActionComponent>(entity);
        syncParticleRenderer(entity, *ps);
        if (finished){
            finishParticleSystem(entity);
            return;
        }
    }
    // stopped by stop() with nothing left: the action ends too
    ps = scene->findComponent<ParticleSystemComponent>(entity);
    action = scene->findComponent<ActionComponent>(entity);
    if (ps && action && !ps->state.playing && action->state == ActionState::Running && !action->startTrigger){
        actionStop(entity);
    }
}

void ActionSystem::updateParticleSystems(double dt){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    if (systems->size() == 0) return;
    std::vector<Entity> entities;
    entities.reserve(systems->size());
    for (size_t i = 0; i < systems->size(); i++){
        if (systems->getComponentFromIndex(i).version >= 2) entities.push_back(systems->getEntity(i));
    }
    const double unscaled = Engine::getUnscaledDeltatime();

    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity)) continue;
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
        if (!ps || ps->version < 2) continue;

        if (!scene->isEntityActiveInHierarchy(entity)){
            // a deactivated GameObject stops and clears its system (Unity); playOnAwake plays it
            // again when it comes back
            if (ps->state.awoken || ps->state.playing || ps->state.count() > 0){
                ps->state.awoken = false;
                particles::reset(*ps);
                syncParticleRenderer(entity, *ps);
                if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
                    action->state = ActionState::Stopped;
                    action->timecount = 0;
                    action->startTrigger = action->stopTrigger = action->pauseTrigger = false;
                }
            }
            continue;
        }
        if (!ps->enabled) continue;

        if (!ps->state.awoken){
            ps->state.awoken = true;
            // nothing drawn from a copy or an edit-mode preview before it plays
            if (!ps->state.playing && ps->state.count() == 0){
                syncParticleRenderer(entity, *ps);
                ps = scene->findComponent<ParticleSystemComponent>(entity);
            }
            ActionComponent* action = scene->findComponent<ActionComponent>(entity);
            if (ps->main.playOnAwake && action && action->state == ActionState::Stopped && !action->startTrigger){
                action->startTrigger = true;
                if (!actionStateChange(entity, *action)) continue;
            }
        }
        stepParticleSystem(dt, unscaled, entity);
    }
}

void ActionSystem::updateParticlePreview(double dt, const std::vector<Entity>& entities){
    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity)) continue;
        if (ActionComponent* action = scene->findComponent<ActionComponent>(entity)){
            if (!actionStateChange(entity, *action)) continue;
        }
        stepParticleSystem(dt, dt, entity);
    }
    // edit mode: a preview never disables, destroys or calls game code
    pendingParticleStops.clear();
}

// ------------------------------------------------------------------ control (Lua ParticleSystem)

void ActionSystem::particleSystemPlay(Entity entity, bool withChildren){
    for (Entity system : particleSystemsOf(entity, withChildren)){
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system);
        if (!ps) continue;
        prepareParticleRenderer(system, *ps);
        ps = scene->findComponent<ParticleSystemComponent>(system);
        particles::play(*ps, particleEmitterFrame(system), particleEnvironment());
        ps->state.awoken = true;
        syncParticleRenderer(system, *ps);
        if (ActionComponent* action = scene->findComponent<ActionComponent>(system)){
            // onStart at the next update, as Action.start()
            action->stopTrigger = false;
            if (action->state != ActionState::Running) action->startTrigger = true;
        }
    }
}

void ActionSystem::particleSystemStop(Entity entity, bool withChildren, ParticleSystemStopBehavior behavior){
    for (Entity system : particleSystemsOf(entity, withChildren)){
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system);
        if (!ps) continue;
        const bool wasPlaying = ps->state.playing;
        const bool stopped = particles::stop(*ps, behavior);
        syncParticleRenderer(system, *ps);
        ActionComponent* action = scene->findComponent<ActionComponent>(system);
        if (action) action->startTrigger = false;
        if (stopped && wasPlaying){
            queueParticleStopAction(system);
            // onStop at the next update
            if (action && (action->state == ActionState::Running || action->state == ActionState::Paused)) action->stopTrigger = true;
        }
    }
}

void ActionSystem::particleSystemPause(Entity entity, bool withChildren){
    for (Entity system : particleSystemsOf(entity, withChildren)){
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system);
        if (!ps) continue;
        particles::pause(*ps);
        ActionComponent* action = scene->findComponent<ActionComponent>(system);
        if (action && action->state == ActionState::Running && ps->state.paused) action->pauseTrigger = true;
    }
}

void ActionSystem::particleSystemClear(Entity entity, bool withChildren){
    for (Entity system : particleSystemsOf(entity, withChildren)){
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system);
        if (!ps) continue;
        particles::clear(*ps);
        syncParticleRenderer(system, *ps);
    }
}

int ActionSystem::particleSystemEmit(Entity entity, int count){
    ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps || ps->version < 2 || count <= 0) return 0;
    prepareParticleRenderer(entity, *ps);
    ps = scene->findComponent<ParticleSystemComponent>(entity);
    const int spawned = particles::emit(*ps, count, particleEmitterFrame(entity), particleEnvironment());
    syncParticleRenderer(entity, *ps);
    return spawned;
}

void ActionSystem::particleSystemSimulate(Entity entity, float time, bool withChildren, bool restart){
    for (Entity system : particleSystemsOf(entity, withChildren)){
        ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system);
        if (!ps) continue;
        prepareParticleRenderer(system, *ps);
        ps = scene->findComponent<ParticleSystemComponent>(system);
        particles::simulate(*ps, time, restart, particleEmitterFrame(system), particleEnvironment());
        ps->state.awoken = true;
        syncParticleRenderer(system, *ps);
        if (ActionComponent* action = scene->findComponent<ActionComponent>(system)){
            // Simulate leaves the system paused (Unity)
            action->startTrigger = action->stopTrigger = action->pauseTrigger = false;
            if (ps->state.playing) action->state = ActionState::Paused;
        }
    }
}

void ActionSystem::particleSystemReset(Entity entity){
    ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(entity);
    if (!ps || ps->version < 2) return;
    particles::reset(*ps);
    syncParticleRenderer(entity, *ps);
}
