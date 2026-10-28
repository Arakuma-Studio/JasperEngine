// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PreviewParticles.h"

#include "AppSettings.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "subsystem/ParticleSubsystem.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    bool isUnder(Scene* scene, Entity root, Entity entity){
        for (int guard = 0; entity != NULL_ENTITY && guard < 100000; guard++){
            if (entity == root) return true;
            const Transform* transform = scene->findComponent<Transform>(entity);
            entity = transform ? transform->parent : NULL_ENTITY;
        }
        return false;
    }

}

void PreviewParticles::sync(Scene* scene, Entity root, double time, bool forward){
    if (!scene || root == NULL_ENTITY) return;
    if (!AppSettings::getSceneViewSettings().previewParticles){
        stop(scene);
        return;
    }
    ParticleSubsystem* particles = scene->getSystem<ParticleSubsystem>().get();
    if (!particles) return;
    std::vector<Entity> found;
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    for (size_t i = 0; i < systems->size(); i++){
        const Entity entity = systems->getEntity(i);
        if (isUnder(scene, root, entity)) found.push_back(entity);
    }
    // the ones no longer under it (moved, deleted) stop
    for (Entity entity : touched){
        if (std::find(found.begin(), found.end(), entity) == found.end() && scene->isEntityCreated(entity) &&
                scene->findComponent<ParticleSystemComponent>(entity)){
            particles->stop(entity, true);
        }
    }
    bool known = found.size() == touched.size();
    for (Entity entity : found) known = known && std::find(touched.begin(), touched.end(), entity) != touched.end();
    const bool advance = forward && known && !seeking && lastTime >= 0.0 && time >= lastTime;
    for (Entity entity : found){
        if (advance) particles->previewAdvance(entity, time - lastTime);
        else particles->previewSeek(entity, time);
    }
    if (!advance && !found.empty()) seeking = true;
    touched = found;
    lastTime = time;
    pump(scene);
}

void PreviewParticles::pump(Scene* scene){
    if (!seeking || !scene) return;
    ParticleSubsystem* particles = scene->getSystem<ParticleSubsystem>().get();
    if (!particles) return;
    // the Scene view overlay's budget: a few milliseconds a frame over every system
    constexpr double budgetMs = 4.0;
    bool done = true;
    size_t alive = 0;
    for (Entity entity : touched) alive += scene->isEntityCreated(entity) && scene->findComponent<ParticleSystemComponent>(entity);
    for (Entity entity : touched){
        if (!scene->isEntityCreated(entity) || !scene->findComponent<ParticleSystemComponent>(entity)) continue;
        done = particles->previewUpdate(entity, budgetMs / (double)std::max<size_t>(1, alive)) && done;
    }
    seeking = !done;
}

void PreviewParticles::stop(Scene* scene){
    if (scene){
        if (ParticleSubsystem* particles = scene->getSystem<ParticleSubsystem>().get()){
            for (Entity entity : touched){
                if (scene->isEntityCreated(entity) && scene->findComponent<ParticleSystemComponent>(entity)) particles->stop(entity, true);
            }
        }
    }
    touched.clear();
    lastTime = -1.0;
    seeking = false;
}
