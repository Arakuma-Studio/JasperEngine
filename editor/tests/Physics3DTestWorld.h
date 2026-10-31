// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A 3D physics scene for the physics-3d tests (Physics3DBodyTest, Physics3DJointsTest,
// PhysicsMaterial3DTest, RagdollTest, Physics3DSerializationTest): bodies, colliders and joints
// built in memory, stepped at 1/60 s, no display or GPU.

#pragma once

#include "PhysicsTestScene.h"
#include "ScriptDataTestSupport.h"
#include "Scene.h"
#include "component/Joint3DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "object/physics/Joint3D.h"
#include "object/physics/Rigidbody.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::physics3dtest {

    constexpr float STEP = 1.0f / 60.0f;

    inline Quaternion degreesAbout(float degrees, const Vector3& axis) {
        return Quaternion(Angle::degToDefault(degrees), axis.normalized());
    }

    // The engine's log lines while it lives (Log goes through System::instance())
    class LogCapture {
    public:
        LogCapture() {
            physicstest::installTestSystem();
            previous = SystemTestAccess::swap(&system);
        }
        ~LogCapture() { SystemTestAccess::swap(previous); }

        const std::vector<std::string>& lines() const { return system.lines; }
        size_t count(const std::string& text) const {
            size_t n = 0;
            for (const std::string& line : system.lines) {
                if (line.find(text) != std::string::npos) n++;
            }
            return n;
        }

    private:
        class CaptureSystem : public physicstest::TestSystem {
        public:
            std::vector<std::string> lines;
            void platformLog(const int, const char* fmt, va_list args) override {
                char buffer[2048];
                std::vsnprintf(buffer, sizeof(buffer), fmt, args);
                lines.push_back(buffer);
            }
        };
        CaptureSystem system;
        System* previous = nullptr;
    };

    class World3D {
    public:
        // System::instance() (Log, files) and the Lua state, as Engine::systemInit makes them
        const bool systemInstalled = physicstest::installTestSystem();
        std::unique_ptr<Scene> scenePtr = std::make_unique<Scene>();
        Scene& scene = *scenePtr;

        explicit World3D(Vector3 gravity = Vector3(0.0f, -9.81f, 0.0f)) {
            scene.setPhysicsInterpolation(false);
            physics()->setGravity3D(gravity);
        }

        PhysicsSystem* physics() { return scene.getSystem<PhysicsSystem>().get(); }

        Entity entity(const Vector3& position, Entity parent = NULL_ENTITY, const Quaternion& rotation = Quaternion::IDENTITY) {
            Entity e = scene.createEntity();
            Transform transform;
            transform.position = position;
            transform.rotation = rotation;
            scene.addComponent<Transform>(e, transform);
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, e, false);
            return e;
        }

        Entity body(const Vector3& position, BodyType type, Entity parent = NULL_ENTITY, const Quaternion& rotation = Quaternion::IDENTITY) {
            Entity e = entity(position, parent, rotation);
            RigidbodyComponent component;
            component.type = type;
            scene.addComponent<RigidbodyComponent>(e, component);
            return e;
        }

        Entity box(const Vector3& position, const Vector3& size, BodyType type, Entity parent = NULL_ENTITY,
                   const Quaternion& rotation = Quaternion::IDENTITY) {
            Entity e = body(position, type, parent, rotation);
            rb(e).createBoxCollider(size.x, size.y, size.z);
            return e;
        }

        Entity sphere(const Vector3& position, float radius, BodyType type, Entity parent = NULL_ENTITY) {
            Entity e = body(position, type, parent);
            rb(e).createSphereCollider(radius);
            return e;
        }

        Entity floor(float y = 0.0f) {
            return box(Vector3(0, y - 0.5f, 0), Vector3(100, 1, 100), BodyType::STATIC);
        }

        Rigidbody rb(Entity e) { return Rigidbody(&scene, e); }
        RigidbodyComponent& component(Entity e) { return scene.getComponent<RigidbodyComponent>(e); }
        Transform& transform(Entity e) { return scene.getComponent<Transform>(e); }

        Entity joint(const Joint3DComponent& joint, Entity parent = NULL_ENTITY) {
            Entity e = entity(Vector3(0, 0, 0), parent);
            scene.addComponent<Joint3DComponent>(e, joint);
            return e;
        }

        Joint3D j(Entity e) { return Joint3D(&scene, e); }
        Joint3DComponent& jointComponent(Entity e) { return scene.getComponent<Joint3DComponent>(e); }

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

}
