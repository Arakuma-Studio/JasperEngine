// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TrailSystem.h"

#include "Scene.h"
#include "Log.h"
#include "component/Transform.h"
#include "manager/ObjectManager.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

// ---------------------------------------------------------------- TrailPointRing

void TrailPointRing::push(const TrailPoint& point){
    if (count == ring.size()){
        // grow by powers of two, unrolled so the oldest is at 0 again
        std::vector<TrailPoint> grown(ring.empty() ? 16 : ring.size() * 2);
        for (size_t i = 0; i < count; i++){
            grown[i] = at(i);
        }
        ring.swap(grown);
        first = 0;
    }
    ring[(first + count) % ring.size()] = point;
    count++;
}

void TrailPointRing::popOldest(){
    if (count == 0){
        return;
    }
    first = (first + 1) % ring.size();
    count--;
    if (count == 0){
        first = 0;
    }
}

void TrailPointRing::clear(){
    first = 0;
    count = 0;
}

void TrailPointRing::reset(){
    clear();
    std::vector<TrailPoint>().swap(ring);
}

// ---------------------------------------------------------------- TrailSimulation

namespace {

    // the head counts as moved past this (world units)
    const float MOVE_EPSILON = 1e-6f;

}

void TrailSimulation::clear(TrailRendererComponent& trail){
    trail.clearPoints();
}

void TrailSimulation::addPosition(TrailRendererComponent& trail, const Vector3& position, float now){
    if (trail.points.size() >= MAX_POINTS){
        trail.points.popOldest();
    }
    trail.points.push({position, now});
    trail.needUpdateRibbon = true;
}

float TrailSimulation::ageT(const TrailRendererComponent& trail, float birthTime, float now){
    if (!(trail.time > 0.0f)){
        return 1.0f;
    }
    const float t = (now - birthTime) / trail.time;
    return std::min(1.0f, std::max(0.0f, t));
}

unsigned int TrailSimulation::step(TrailRendererComponent& trail, const Vector3& head, float now, float dt){
    unsigned int result = Step_None;

    const bool moved = trail.hasLastHeadPosition && trail.lastHeadPosition.distance(head) > MOVE_EPSILON;
    if (moved){
        // the live head is the ribbon's first vertex
        result |= Step_Changed;
    }

    if (dt > 0.0f){
        const float time = std::max(0.0f, trail.time);
        while (!trail.points.empty() && (now - trail.points.at(0).birthTime) > time){
            trail.points.popOldest();
            result |= Step_Changed;
        }

        if (trail.emitting && time > 0.0f && trail.hasLastHeadPosition){
            const float minDistance = std::max(0.0f, trail.minVertexDistance);
            if (trail.points.empty()){
                // starting to move: the trail starts where the head was
                if (moved){
                    addPosition(trail, trail.lastHeadPosition, now - dt);
                    result |= Step_Changed;
                }
            }
            if (!trail.points.empty() && trail.points.newest().position.distance(head) > std::max(minDistance, MOVE_EPSILON)){
                if (trail.points.size() >= MAX_POINTS && !trail.warnedCapacity){
                    trail.warnedCapacity = true;
                    result |= Step_CapacityWarning;
                }
                addPosition(trail, head, now);
                result |= Step_Changed;
            }
        }

        // the points age: the width and colour by age change even while the head stays still
        if (!trail.points.empty()){
            result |= Step_Changed;
        }

        if (moved){
            trail.idleTime = 0.0f;
        }else{
            trail.idleTime += dt;
        }
        if (trail.autodestruct && trail.points.empty() && trail.idleTime >= time){
            result |= Step_Destroy;
        }
    }

    trail.lastHeadPosition = head;
    trail.hasLastHeadPosition = true;
    if (result & Step_Changed){
        trail.needUpdateRibbon = true;
    }
    return result;
}

void TrailSimulation::ribbonPoints(const TrailRendererComponent& trail, const Vector3& head, float now,
                                   std::vector<Vector3>& points, std::vector<float>& pointT){
    points.clear();
    pointT.clear();
    if (trail.points.empty()){
        return;
    }
    points.reserve(trail.points.size() + 1);
    pointT.reserve(trail.points.size() + 1);
    // not emitting: detached from the object (Unity), the stored points only
    if (trail.emitting){
        points.push_back(head);
        pointT.push_back(0.0f);
    }
    for (size_t i = trail.points.size(); i-- > 0;){
        const TrailPoint& point = trail.points.at(i);
        points.push_back(point.position);
        pointT.push_back(ageT(trail, point.birthTime, now));
    }
}

// ---------------------------------------------------------------- TrailSystem

TrailSystem::TrailSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<TrailRendererComponent>());
}

float TrailSystem::getTime() const{
    return (float)clock;
}

void TrailSystem::setPreview(Entity entity, bool preview){
    if (preview){
        previewEntities.insert(entity);
    }else if (previewEntities.erase(entity) > 0){
        clear(entity);
    }
}

void TrailSystem::clearPreviews(){
    for (Entity entity : std::vector<Entity>(previewEntities.begin(), previewEntities.end())){
        setPreview(entity, false);
    }
}

bool TrailSystem::isPreviewing(Entity entity) const{
    return previewEntities.count(entity) > 0;
}

void TrailSystem::clear(Entity entity){
    if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity)){
        TrailSimulation::clear(*trail);
    }
}

void TrailSystem::getRibbonPoints(Entity entity, const TrailRendererComponent& trail, std::vector<Vector3>& points, std::vector<float>& pointT) const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    const Vector3 head = transform ? transform->worldPosition : trail.lastHeadPosition;
    const float now = isPreviewing(entity) ? (float)previewClock : (float)clock;
    TrailSimulation::ribbonPoints(trail, head, now, points, pointT);
}

void TrailSystem::updateTrail(Entity entity, TrailRendererComponent& trail, float now, float dt, bool allowDestroy){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform){
        return;
    }
    const unsigned int result = TrailSimulation::step(trail, transform->worldPosition, now, dt);
    if (result & TrailSimulation::Step_CapacityWarning){
        Log::warn("Trail Renderer of entity %u reached %zu points: the oldest are dropped (raise its Min Vertex Distance or lower its Time)",
                  (unsigned int)entity, TrailSimulation::MAX_POINTS);
    }
    if ((result & TrailSimulation::Step_Destroy) && allowDestroy && !trail.destroyRequested){
        // once: the destroy runs at the end of the frame
        trail.destroyRequested = true;
        ObjectManager::destroy(scene, entity);
    }
}

void TrailSystem::load(){
}

void TrailSystem::draw(){
}

void TrailSystem::destroy(){
    previewEntities.clear();
}

void TrailSystem::update(double dt){
    if (!paused){
        clock += dt;
    }else if (!previewEntities.empty()){
        previewClock += dt;
    }

    auto trails = scene->getComponentArray<TrailRendererComponent>();
    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    for (size_t i = 0; i < trails->size(); i++){
        TrailRendererComponent& trail = trails->getComponentFromIndex(i);
        const Entity entity = trails->getEntity(i);

        const bool active = trail.enabled && scene->isEntityActiveInHierarchy(entity);
        if (!active){
            // cleared: reactivated elsewhere it starts from there
            if (!trail.points.empty() || trail.hasLastHeadPosition){
                TrailSimulation::clear(trail);
            }
        }else if (!paused){
            updateTrail(entity, trail, (float)clock, (float)dt, true);
        }else if (isPreviewing(entity)){
            // an edit-mode preview never destroys the edited GameObject (autodestruct is Play's)
            updateTrail(entity, trail, (float)previewClock, (float)dt, false);
        }

        if (render){
            render->updateTrail(entity, trail, (float)dt);
        }
    }
}

void TrailSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<TrailRendererComponent>()){
        previewEntities.erase(entity);
    }
}
