// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The world settings of PhysicsSystem (docs/specs/S96-physics-gaps.md WP-E): the 2D world's
// units, rotation cap and tuning and the 3D solver, applied to the live worlds.

#include "PhysicsSystem.h"

#include "Scene.h"
#include "component/Rigidbody2DComponent.h"

#include <vector>

using namespace jasperengine;

const ScenePhysics2DSettings& PhysicsSystem::getWorldSettings2D() const{
    return worldSettings2D;
}

void PhysicsSystem::setWorldSettings2D(const ScenePhysics2DSettings& settings){
    const ScenePhysics2DSettings clean = sanitizePhysicsWorldSettings(settings);
    if (clean == worldSettings2D){
        return;
    }
    worldSettings2D = clean;
    worldSettings2DDirty = true;
#ifdef JASPERENGINE_PHYSICS_2D
    // the scale at once, so what reads it before the next step (drawn poses, queries, scripts)
    // agrees with the bodies; inside a step (a callback) after it
    if (!steppingWorld2D && clean.pointsPerMeter != pointsToMeterScale2D){
        rescaleBodies2D(clean.pointsPerMeter);
    }
#endif
}

const ScenePhysics3DSettings& PhysicsSystem::getWorldSettings3D() const{
    return worldSettings3D;
}

void PhysicsSystem::setWorldSettings3D(const ScenePhysics3DSettings& settings){
    const ScenePhysics3DSettings clean = sanitizePhysicsWorldSettings(settings);
    if (clean == worldSettings3D){
        return;
    }
    worldSettings3D = clean;
    worldSettings3DDirty = true;
}

void PhysicsSystem::applyWorldSettings(){
    if (worldSettings2DDirty){
        worldSettings2DDirty = false;
#ifdef JASPERENGINE_PHYSICS_2D
        if (worldSettings2D.pointsPerMeter != pointsToMeterScale2D){
            rescaleBodies2D(worldSettings2D.pointsPerMeter);
        }
        // a world made later takes them when it is made (ensureWorld2D)
        if (b2World_IsValid(world2D)){
            applyPhysicsWorldSettings(worldSettings2D, world2D);
        }
#endif
    }
    if (worldSettings3DDirty){
        worldSettings3DDirty = false;
#ifdef JASPERENGINE_PHYSICS_3D
        if (world3DReady){
            JPH::PhysicsSettings joltSettings = world3D.GetPhysicsSettings();
            applyPhysicsWorldSettings(worldSettings3D, joltSettings);
            world3D.SetPhysicsSettings(joltSettings);
        }
#endif
    }
}

#ifdef JASPERENGINE_PHYSICS_2D
void PhysicsSystem::rescaleBodies2D(float pointsPerMeter){
    // metres of the old scale to metres of the new one
    const float ratio = pointsToMeterScale2D / pointsPerMeter;
    pointsToMeterScale2D = pointsPerMeter;
    if (!b2World_IsValid(world2D)){
        return;
    }

    // the entities first: making a body again may move the component arrays
    std::vector<Entity> entities;
    auto bodies = scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies->size(); i++){
        if (b2Body_IsValid(bodies->getComponentFromIndex(i).body)){
            entities.push_back(bodies->getEntity(i));
        }
    }

    for (Entity entity : entities){
        Rigidbody2DComponent& before = scene->getComponent<Rigidbody2DComponent>(entity);
        // the pose of the last step (a drawn Transform may be an interpolated one behind it)
        const b2Transform pose = b2Body_GetTransform(before.body);
        const b2Transform previous = before.previousTransform;
        const b2Vec2 linearVelocity = b2Body_GetLinearVelocity(before.body);
        const float angularVelocity = b2Body_GetAngularVelocity(before.body);
        const bool awake = b2Body_IsAwake(before.body);
        const bool enabled = b2Body_IsEnabled(before.body);
        const bool newBody = before.newBody;
        const bool followingPosition = before.followingPosition;
        const bool followingRotation = before.followingRotation;
        const bool followReached = before.followReached;
        const b2Vec2 followedLinearVelocity = before.followedLinearVelocity;
        const b2Vec2 preStepLinearVelocity = before.preStepLinearVelocity;

        // its shapes, mass and joints in metres of the new scale (the joints, which went with
        // the body, are made again by the next step's joint pass)
        before.needReloadBody = true;
        loadRigidbody2D(entity);

        Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(entity);
        if (!b2Body_IsValid(body.body)){
            continue;
        }
        b2Body_SetTransform(body.body, b2MulSV(ratio, pose.p), pose.q);
        if (body.type != BodyType::STATIC){
            b2Body_SetLinearVelocity(body.body, b2MulSV(ratio, linearVelocity));
            b2Body_SetAngularVelocity(body.body, angularVelocity);
            b2Body_SetAwake(body.body, awake);
        }
        if (!enabled){
            b2Body_Disable(body.body);
        }
        body.previousTransform = {b2MulSV(ratio, previous.p), previous.q};
        body.newBody = newBody;
        body.followingPosition = followingPosition;
        body.followingRotation = followingRotation;
        body.followReached = followReached;
        body.followedLinearVelocity = b2MulSV(ratio, followedLinearVelocity);
        body.preStepLinearVelocity = b2MulSV(ratio, preStepLinearVelocity);
    }
}
#endif
