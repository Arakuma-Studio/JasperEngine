// SPDX-License-Identifier: MIT
//
// Scenes for the physics tests (JoltShapeBuilderTest, PlacementWorldTest): engine primitives
// and terrains built in memory, no files, display or GPU.

#pragma once

#include "Scene.h"
#include "Engine.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "util/Angle.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::physicstest {

    // Radians to the engine's default angle unit, which Quaternion's angle constructors take
    inline Quaternion axisAngle(const Vector3& axis, float radians) {
        return Quaternion(Angle::radToDefault(radians), axis.normalized());
    }

    // Log goes through System::instance(), which Engine::systemInit sets up; the tests run without
    // an engine otherwise
    class TestSystem : public System {
    public:
        int getScreenWidth() override { return 0; }
        int getScreenHeight() override { return 0; }
    };

    inline bool installTestSystem() {
        static TestSystem system;
        static const bool installed = [](){ Engine::systemInit(0, nullptr, &system); return true; }();
        return installed;
    }

    class TestScene {
    public:
        const bool systemInstalled = installTestSystem();
        Scene scene;

        ~TestScene() {
            // the handles go before the scene
            primitives.clear();
        }

        Entity entity(const Vector3& position = Vector3(0, 0, 0), Entity parent = NULL_ENTITY) {
            const Entity e = scene.createEntity();
            scene.addComponent<Transform>(e, Transform());
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, e, false);
            scene.getComponent<Transform>(e).position = position;
            return e;
        }

        Primitive& primitive(const Vector3& position, Entity parent = NULL_ENTITY) {
            primitives.push_back(std::make_unique<Primitive>(&scene));
            Primitive& p = *primitives.back();
            if (parent != NULL_ENTITY) scene.addEntityChild(parent, p.getEntity(), false);
            p.setPosition(position);
            return p;
        }

        Entity box(const Vector3& size, const Vector3& position, Entity parent = NULL_ENTITY) {
            Primitive& p = primitive(position, parent);
            p.createBox(size.x, size.y, size.z);
            return p.getEntity();
        }

        Entity sphere(float radius, const Vector3& position) {
            Primitive& p = primitive(position);
            p.createSphere(radius);
            return p.getEntity();
        }

        Entity cylinder(float radius, float height, const Vector3& position) {
            Primitive& p = primitive(position);
            p.createCylinder(radius, height);
            return p.getEntity();
        }

        Entity capsule(float radius, float height, const Vector3& position) {
            Primitive& p = primitive(position);
            p.createCapsule(radius, height);
            return p.getEntity();
        }

        Entity torus(float radius, float ringRadius, const Vector3& position) {
            Primitive& p = primitive(position);
            p.createTorus(radius, ringRadius);
            return p.getEntity();
        }

        Entity plane(float width, float depth, const Vector3& position) {
            Primitive& p = primitive(position);
            p.createPlane(width, depth);
            return p.getEntity();
        }

        Transform& transform(Entity e) { return scene.getComponent<Transform>(e); }

        void setRotation(Entity e, const Quaternion& rotation) {
            transform(e).rotation = rotation;
            transform(e).needUpdate = true;
        }

        void setScale(Entity e, const Vector3& scale) {
            transform(e).scale = scale;
            transform(e).needUpdate = true;
        }

        // An authored collider on e (a Rigidbody added on first use)
        Collider3D& collider(Entity e, Collider3DType type, BodyType bodyType = BodyType::STATIC) {
            if (!scene.findComponent<RigidbodyComponent>(e)) {
                RigidbodyComponent body;
                body.type = bodyType;
                scene.addComponent<RigidbodyComponent>(e, body);
            }
            RigidbodyComponent& body = scene.getComponent<RigidbodyComponent>(e);
            Collider3D& c = body.colliders[body.numColliders++];
            c = Collider3D();
            c.type = type;
            return c;
        }

        // A terrain whose 8-bit heightmap is height(u, v) * 255 for u, v in [0, 1] (texel
        // centres at i / (size - 1), the mapping MeshSystem::sampleTerrainSurface reads)
        template<typename F>
        Entity terrain(int mapSize, float terrainSize, float maxHeight, const Vector3& position, F height) {
            const Entity e = entity(position);
            pixels.push_back(std::make_unique<std::vector<uint8_t>>(static_cast<size_t>(mapSize) * mapSize));
            std::vector<uint8_t>& data = *pixels.back();
            for (int z = 0; z < mapSize; z++) {
                for (int x = 0; x < mapSize; x++) {
                    const float h = height(x / float(mapSize - 1), z / float(mapSize - 1));
                    data[static_cast<size_t>(z) * mapSize + x] = static_cast<uint8_t>(std::lround(std::fmin(std::fmax(h, 0.0f), 1.0f) * 255.0f));
                }
            }

            TerrainComponent terrain;
            terrain.terrainSize = terrainSize;
            terrain.maxHeight = maxHeight;
            static int counter = 0;
            terrain.heightMap.setData("physics-test-heightmap-" + std::to_string(++counter),
                                      TextureData(mapSize, mapSize, static_cast<unsigned int>(data.size()), ColorFormat::RED, 1, data.data()));
            terrain.heightMapLoaded = true;
            terrain.needUpdateTerrain = false;
            scene.addComponent<TerrainComponent>(e, terrain);
            return e;
        }

        // World transforms resolved, as the render pass would
        void resolve() {
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t i = 0; i < transforms->size(); i++) {
                scene.getSystem<RenderSystem>()->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
            }
        }

    private:
        std::vector<std::unique_ptr<Primitive>> primitives;
        std::vector<std::unique_ptr<std::vector<uint8_t>>> pixels;
    };

}
