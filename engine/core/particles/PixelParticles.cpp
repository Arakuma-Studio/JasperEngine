// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelParticles.h"

#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "particles/ParticleEffectCodec.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"

#include <map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    // (scene, effect) -> its emitter; the main thread's (the Lua state's, the players')
    std::map<std::pair<Scene*, std::string>, Entity>& emitters(){
        static std::map<std::pair<Scene*, std::string>, Entity> table;
        return table;
    }

    bool alive(Scene* scene, Entity entity){
        return entity != NULL_ENTITY && scene->isEntityCreated(entity) && scene->isPooledSystemEntity(entity) &&
               scene->findComponent<ParticleSystemComponent>(entity) != nullptr;
    }

}

uint32_t PixelParticles::emitterOf(Scene* scene, const std::string& effect){
    auto found = emitters().find({scene, effect});
    return (found != emitters().end() && alive(scene, found->second)) ? found->second : NULL_ENTITY;
}

void PixelParticles::forget(Scene* scene){
    for (auto it = emitters().begin(); it != emitters().end();){
        if (it->first.first == scene)
            it = emitters().erase(it);
        else
            ++it;
    }
}

bool PixelParticles::emitAt(Scene* scene, const std::string& effect, const std::vector<Vector3>& positions,
                            const std::vector<Vector3>& velocities, std::string* error){
    if (!scene || positions.empty())
        return scene != nullptr;
    Entity entity = emitterOf(scene, effect);
    if (entity == NULL_ENTITY){
        ParticleSystemComponent component;
        component.playOnAwake = false;
        if (!effect.empty() && effect.front() == '{'){
            std::vector<Diagnostic> diagnostics;
            if (!ParticleEffectCodec::read(effect, component.inlineEffect, diagnostics)){
                if (error) *error = "the effect cannot be read: " + ParticleEffectCodec::format(diagnostics);
                return false;
            }
        }else{
            component.effect = effect;
        }
        entity = scene->createSystemEntity();
        scene->addComponent<Transform>(entity, Transform());
        scene->addComponent<ParticleSystemComponent>(entity, component);
        emitters()[{scene, effect}] = entity;
    }
    std::shared_ptr<ParticleSubsystem> subsystem = scene->getSystem<ParticleSubsystem>();
    ParticleSystemState* state = subsystem->getState(entity);
    if (!state || !state->effect || !state->effect->valid){
        if (error) *error = "the effect '" + effect + "' cannot be read";
        return false;
    }
    if (state->effect->document.simulationMode == SimulationMode::Analytic){
        // an Analytic effect has no particles to add (R21a)
        if (error) *error = "analytic_no_events: an Analytic effect cannot emit on request; use a Simulated one";
        return false;
    }
    for (size_t i = 0; i < positions.size(); i++){
        EmitOverrides overrides;
        overrides.hasPosition = true;
        overrides.position[0] = positions[i].x;
        overrides.position[1] = positions[i].y;
        overrides.position[2] = positions[i].z;
        if (i < velocities.size()){
            overrides.hasVelocity = true;
            overrides.velocity[0] = velocities[i].x;
            overrides.velocity[1] = velocities[i].y;
            overrides.velocity[2] = velocities[i].z;
        }
        subsystem->emit(entity, 1, &overrides);
    }
    return true;
}
