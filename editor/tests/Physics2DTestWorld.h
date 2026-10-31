// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A 2D physics scene for the physics-2d tests (Physics2DJointsTest, Physics2DCollidersTest):
// bodies, colliders and joints built in memory, stepped at 1/60 s, no display or GPU.

#pragma once

#include "PhysicsTestScene.h"
#include "Scene.h"
#include "component/Joint2DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "object/physics/Joint2D.h"
#include "object/physics/Rigidbody2D.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include <cmath>
#include <memory>

namespace jasperengine::physics2dtest {

    constexpr float STEP = 1.0f / 60.0f;

    class World2D {
    public:
        // System::instance() (Log, files) and the Lua state, as Engine::systemInit makes them
        const bool systemInstalled = physicstest::installTestSystem();
        std::unique_ptr<Scene> scenePtr = std::make_unique<Scene>();
        Scene& scene = *scenePtr;

        explicit World2D(Vector2 gravity = Vector2(0.0f, -9.81f)) {
            scene.setPhysicsInterpolation(false);
            scene.setGravity2D(gravity);
        }

        PhysicsSystem* physics() { return scene.getSystem<PhysicsSystem>().get(); }

        Entity entity(const Vector3& position, Entity parent = NULL_ENTITY, float rollDegrees = 0.0f) {
            Entity e = scene.createEntity();
            Transform transform;
            transform.position = position;
            if (rollDegrees != 0.0f) transform.rotation = Quaternion(rollDegrees, Vector3(0, 0, 1));
            scene.addComponent<Transform>(e, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, e, false);
            return e;
        }

        Entity body(const Vector3& position, BodyType type, Entity parent = NULL_ENTITY, float rollDegrees = 0.0f) {
            Entity e = entity(position, parent, rollDegrees);
            Rigidbody2DComponent component;
            component.type = type;
            scene.addComponent<Rigidbody2DComponent>(e, component);
            return e;
        }

        Rigidbody2D rb(Entity e) { return Rigidbody2D(&scene, e); }
        Rigidbody2DComponent& component(Entity e) { return scene.getComponent<Rigidbody2DComponent>(e); }

        Entity joint(const Joint2DComponent& joint, Entity parent = NULL_ENTITY) {
            Entity e = entity(Vector3(0, 0, 0), parent);
            scene.addComponent<Joint2DComponent>(e, joint);
            return e;
        }

        Joint2D j(Entity e) { return Joint2D(&scene, e); }
        Joint2DComponent& jointComponent(Entity e) { return scene.getComponent<Joint2DComponent>(e); }

        void resolve() {
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                if (transform.needUpdate) {
                    scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                    transform.needUpdate = false;
                }
            }
        }

        void load() {
            resolve();
            physics()->load();
        }

        void step(int steps = 1) {
            for (int i = 0; i < steps; i++) {
                physics()->fixedUpdate(STEP);
                resolve();
            }
        }
    };

    inline float rad(float degrees) { return degrees * (float)M_PI / 180.0f; }

}
