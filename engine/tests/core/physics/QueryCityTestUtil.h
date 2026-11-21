// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// spatial-queries' "query_city" (PhysicsQueryTest, PhysicsQueryBench): bodies of every kind on a
// grid over a 200 m x 200 m static mesh ground; a quarter of them triggers, half the rest asleep.
// 2D: the same layout in Box2D, in points. Seeded, so every run builds the same world.

#ifndef QUERY_CITY_TEST_UTIL_H
#define QUERY_CITY_TEST_UTIL_H

#include "Scene.h"
#include "component/Transform.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "../ecs/EcsTestUtil.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#include "object/physics/Rigidbody.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "component/Rigidbody2DComponent.h"
#endif

#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace jasperengine::test {

    struct QueryCity {
        std::unique_ptr<Scene> scene;
        std::vector<Entity> bodies;
        Entity ground = NULL_ENTITY;
        float extent = 100.0f;   // the ground spans -extent..extent (2D: points)
    };

    inline Scene* newPhysicsScene(std::unique_ptr<Scene>& scene){
        scene = std::make_unique<Scene>();
        disableSubSystems(*scene);
        scene->getScheduler().setEnabled("PhysicsSystem", true);
        return scene.get();
    }

    inline Entity placedEntity(Scene& scene, const Vector3& position, const Quaternion& rotation = Quaternion::IDENTITY){
        const Entity entity = makeTransformEntity(scene);
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = position;
        transform.rotation = rotation;
        transform.needUpdate = true;
        scene.getSystem<RenderSystem>()->updateTransform(transform, entity);
        return entity;
    }

    inline void stepScene(Scene& scene, int steps){
        for (int i = 0; i < steps; i++){
            scene.fixedUpdate(1.0f / 60.0f);
            auto transforms = scene.getComponentArray<Transform>();
            for (size_t t = 0; t < transforms->size(); t++){
                Transform& transform = transforms->getComponentFromIndex(t);
                if (transform.needUpdate)
                    scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(t));
            }
        }
    }

#ifdef JASPERENGINE_PHYSICS_3D
    inline Collider3D cityCollider(std::mt19937& rng, bool trigger){
        std::uniform_int_distribution<int> kind(0, 2);
        std::uniform_real_distribution<float> size(0.3f, 0.7f);
        Collider3D collider;
        switch (kind(rng)){
            case 0: collider.type = Collider3DType::SPHERE; collider.radius = size(rng); break;
            case 1: collider.type = Collider3DType::BOX; collider.width = 2.0f * size(rng); collider.height = 2.0f * size(rng); collider.depth = 2.0f * size(rng); break;
            default: collider.type = Collider3DType::CAPSULE; collider.radius = 0.5f * size(rng); collider.halfHeight = size(rng); break;
        }
        collider.isTrigger = trigger;
        return collider;
    }

    // count bodies (a 45 x 45 grid holds 2,025) 1..3 m above the ground, none touching another
    inline QueryCity buildQueryCity3D(size_t count, uint32_t seed = 7){
        QueryCity city;
        Scene& scene = *newPhysicsScene(city.scene);
        PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
        physics->setGravity3D(0.0f, 0.0f, 0.0f);

        // the ground: a two-triangle mesh collider (a surface)
        city.ground = placedEntity(scene, Vector3(0.0f, 0.0f, 0.0f));
        RigidbodyComponent ground;
        ground.type = BodyType::STATIC;
        ground.colliders.resize(1);
        ground.colliders[0].type = Collider3DType::MESH;
        ground.colliders[0].source = Collider3DSource::RAW_MESH;
        const float e = city.extent;
        ground.colliders[0].vertices = {Vector3(-e, 0, -e), Vector3(e, 0, -e), Vector3(e, 0, e), Vector3(-e, 0, e)};
        ground.colliders[0].indices = {0, 2, 1, 0, 3, 2};
        scene.addComponent<RigidbodyComponent>(city.ground, ground);

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> jitter(-0.6f, 0.6f);
        std::uniform_real_distribution<float> height(1.0f, 3.0f);
        std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
        const int side = (int)std::ceil(std::sqrt((double)count));
        const float spacing = 2.0f * e / (float)side;
        for (size_t i = 0; i < count; i++){
            const float x = -e + spacing * (0.5f + (float)(i % side)) + jitter(rng);
            const float z = -e + spacing * (0.5f + (float)(i / side)) + jitter(rng);
            const Quaternion rotation(angle(rng), Vector3(0.3f, 1.0f, 0.2f).normalized());
            const Entity entity = placedEntity(scene, Vector3(x, height(rng), z), rotation);
            RigidbodyComponent body;
            // a quarter triggers (every eighth body has a solid collider too: a trigger companion);
            // half the rest dynamic, put to sleep
            const bool trigger = i % 4 == 0;
            body.type = trigger || i % 2 == 0 ? BodyType::STATIC : BodyType::DYNAMIC;
            body.colliders.push_back(cityCollider(rng, trigger));
            if (i % 8 == 0){
                Collider3D solid = cityCollider(rng, false);
                solid.position = Vector3(0.0f, 0.0f, 0.0f);
                body.colliders.push_back(solid);
            }
            scene.addComponent<RigidbodyComponent>(entity, body);
            city.bodies.push_back(entity);
        }
        stepScene(scene, 1);
        for (Entity entity : city.bodies){
            if (scene.getComponent<RigidbodyComponent>(entity).type == BodyType::DYNAMIC)
                Rigidbody(&scene, entity).sleep();
        }
        stepScene(scene, 1);
        return city;
    }
#endif

#ifdef JASPERENGINE_PHYSICS_2D
    // the same in Box2D: points, 64 points per metre
    inline QueryCity buildQueryCity2D(size_t count, uint32_t seed = 7){
        QueryCity city;
        city.extent = 100.0f * 64.0f;
        Scene& scene = *newPhysicsScene(city.scene);
        PhysicsSystem* physics = scene.getSystem<PhysicsSystem>().get();
        physics->setPointsToMeterScale2D(64.0f);
        physics->setGravity2D(0.0f, 0.0f);

        city.ground = placedEntity(scene, Vector3(0.0f, -city.extent, 0.0f));
        Rigidbody2DComponent ground;
        ground.type = BodyType::STATIC;
        ground.colliders.resize(1);
        ground.colliders[0].type = Collider2DType::SEGMENT;
        ground.colliders[0].pointA = Vector2(-city.extent, 0.0f);
        ground.colliders[0].pointB = Vector2(city.extent, 0.0f);
        scene.addComponent<Rigidbody2DComponent>(city.ground, ground);

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> jitter(-0.6f * 64.0f, 0.6f * 64.0f);
        std::uniform_real_distribution<float> size(0.3f * 64.0f, 0.7f * 64.0f);
        std::uniform_int_distribution<int> kind(0, 2);
        const int side = (int)std::ceil(std::sqrt((double)count));
        const float spacing = 2.0f * city.extent / (float)side;
        for (size_t i = 0; i < count; i++){
            const float x = -city.extent + spacing * (0.5f + (float)(i % side)) + jitter(rng);
            const float y = -city.extent + spacing * (0.5f + (float)(i / side)) + jitter(rng);
            const Entity entity = placedEntity(scene, Vector3(x, y, 0.0f));
            Rigidbody2DComponent body;
            const bool trigger = i % 4 == 0;
            body.type = trigger || i % 2 == 0 ? BodyType::STATIC : BodyType::DYNAMIC;
            body.colliders.resize(i % 8 == 0 ? 2 : 1);
            for (Collider2D& collider : body.colliders){
                const float s = size(rng);
                switch (kind(rng)){
                    case 0: collider.type = Collider2DType::CIRCLE; collider.radius = s; break;
                    case 1: collider.type = Collider2DType::POLYGON; collider.pointA = Vector2(-s, -0.5f * s); collider.pointB = Vector2(s, 0.5f * s); break;
                    default: collider.type = Collider2DType::CAPSULE; collider.pointA = Vector2(0.0f, -s); collider.pointB = Vector2(0.0f, s); collider.radius = 0.4f * s; break;
                }
            }
            body.colliders[0].sensor = trigger;
            scene.addComponent<Rigidbody2DComponent>(entity, body);
            city.bodies.push_back(entity);
        }
        stepScene(scene, 2);
        return city;
    }
#endif

}

#endif // QUERY_CITY_TEST_UTIL_H
