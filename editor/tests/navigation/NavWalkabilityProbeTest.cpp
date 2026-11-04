// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// "Why is this point not walkable?" (docs/specs/S42-navigation-diagnostics.md R11, R12; AC9): one
// fixture point per reason on a baked level: a 60 degree ramp, a 1.2 m ceiling, a point 0.2 m
// from a wall, a 0.8 m^2 platform, a Not Walkable volume, a layer-excluded mesh, a dynamic body,
// outside the volume, empty ground, a moved box, a 0.6 m ledge, a masked area, a small island
// and a surface never baked.

#include "JasperEngine.h"
#include "navigation/NavWalkabilityProbe.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct ProbeLevel {
        Scene scene;
        std::vector<std::unique_ptr<Primitive>> objects;
        Entity surface = NULL_ENTITY;
        Entity volumeSurface = NULL_ENTITY;
        Entity unbakedSurface = NULL_ENTITY;
        Entity notWalkable = NULL_ENTITY;
        Entity hidden = NULL_ENTITY;
        Entity body = NULL_ENTITY;
        Entity moved = NULL_ENTITY;
        Entity ramp = NULL_ENTITY;

        Primitive& box(const Vector3& center, const Vector3& size) {
            objects.push_back(std::make_unique<Primitive>(&scene));
            Primitive& object = *objects.back();
            object.createBox(size.x, size.y, size.z);
            object.setPosition(center);
            return object;
        }

        Primitive& floor(const Vector3& center, float width, float depth) {
            objects.push_back(std::make_unique<Primitive>(&scene));
            Primitive& object = *objects.back();
            object.createPlane(width, depth);
            object.setPosition(center);
            return object;
        }

        Entity surfaceEntity(const char* name) {
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity);
            scene.setEntityName(entity, name);
            NavMeshSurfaceComponent component;
            component.overrideCellSize = true;
            component.cellSize = 0.2f;
            component.overrideTileSize = true;
            component.tileSize = 32;
            scene.addComponent<NavMeshSurfaceComponent>(entity, component);
            return entity;
        }

        void resolve() {
            auto transforms = scene.getComponentArray<Transform>();
            for (int pass = 0; pass < 2; pass++) {
                for (size_t i = 0; i < transforms->size(); i++) {
                    Transform& transform = transforms->getComponentFromIndex(i);
                    transform.needUpdate = true;
                    scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
                }
            }
        }

        ProbeLevel() {
            floor(Vector3(), 60.0f, 60.0f);
            // a far 4 x 4 m floor: the collected bounds reach x = 52 (empty ground at x = 40)
            floor(Vector3(50.0f, 0.0f, 0.0f), 4.0f, 4.0f);
            // a 60 degree ramp (a thin slab turned about z)
            Primitive& slab = box(Vector3(-20.0f, 2.0f, -20.0f), Vector3(8.0f, 0.2f, 4.0f));
            slab.setRotation(0.0f, 0.0f, 60.0f);
            ramp = slab.getEntity();
            // a slab whose underside is 1.2 m over the floor
            box(Vector3(20.0f, 1.4f, -20.0f), Vector3(4.0f, 0.4f, 4.0f));
            // a wall: its face at x = 0.2
            box(Vector3(0.0f, 1.5f, 20.0f), Vector3(0.4f, 3.0f, 6.0f));
            // a 1.9 m platform, 1 m up: 0.9 x 0.9 m once eroded, under the 2 m^2 Minimum Region Area
            box(Vector3(-20.0f, 0.5f, 20.0f), Vector3(1.9f, 1.0f, 1.9f));
            // a 6 m platform 0.6 m up: a ledge over the 0.4 m Step Height
            box(Vector3(20.0f, 0.3f, 0.0f), Vector3(6.0f, 0.6f, 6.0f));
            // a Not Walkable volume
            notWalkable = scene.createEntity();
            scene.addComponent<Transform>(notWalkable);
            scene.getComponent<Transform>(notWalkable).position = Vector3(20.0f, 0.0f, 20.0f);
            NavMeshModifierComponent modifier;
            modifier.useVolume = true;
            modifier.overrideArea = true;
            modifier.area = nav::AREA_NOT_WALKABLE;
            modifier.volumeSize = Vector3(4.0f, 4.0f, 4.0f);
            scene.addComponent<NavMeshModifierComponent>(notWalkable, modifier);
            // a Jump strip (masked out by the probe's filter)
            const Entity jump = scene.createEntity();
            scene.addComponent<Transform>(jump);
            scene.getComponent<Transform>(jump).position = Vector3(-20.0f, 0.0f, 0.0f);
            modifier.area = nav::AREA_JUMP;
            scene.addComponent<NavMeshModifierComponent>(jump, modifier);
            // a box on layer 5, which the surface leaves out
            hidden = box(Vector3(0.0f, 0.5f, -20.0f), Vector3(4.0f, 1.0f, 4.0f)).getEntity();
            scene.setEntityLayer(hidden, 5);
#ifdef JASPERENGINE_PHYSICS_3D
            body = box(Vector3(10.0f, 0.5f, -20.0f), Vector3(2.0f, 1.0f, 2.0f)).getEntity();
            RigidbodyComponent rigidbody;
            rigidbody.type = BodyType::DYNAMIC;
            scene.addComponent<RigidbodyComponent>(body, rigidbody);
#endif
            moved = box(Vector3(-10.0f, 0.5f, 10.0f), Vector3(2.0f, 1.0f, 2.0f)).getEntity();

            surface = surfaceEntity("Level");
            scene.getComponent<NavMeshSurfaceComponent>(surface).includeLayers = ~(1u << 5);
            volumeSurface = surfaceEntity("Room");
            NavMeshSurfaceComponent& room = scene.getComponent<NavMeshSurfaceComponent>(volumeSurface);
            room.collectObjects = NavCollectObjects::VOLUME;
            room.volumeSize = Vector3(10.0f, 4.0f, 10.0f);
            unbakedSurface = surfaceEntity("Unbaked");
            resolve();
        }
    };

    bool has(const NavProbeResult& result, const std::string& code) {
        return std::any_of(result.reasons.begin(), result.reasons.end(), [&](const NavProbeReason& r) { return r.code == code; });
    }

    const NavProbeReason* find(const NavProbeResult& result, const std::string& code) {
        for (const NavProbeReason& reason : result.reasons) {
            if (reason.code == code)
                return &reason;
        }
        return nullptr;
    }

    std::string codes(const NavProbeResult& result) {
        std::string text;
        for (const NavProbeReason& reason : result.reasons)
            text += reason.code + " (" + reason.message + ") ";
        return text;
    }

}

TEST(NavWalkabilityProbe, ExplainsEachReason){
    ProbeLevel level;
    Scene& scene = level.scene;
    auto system = scene.getSystem<NavigationSystem>();
    system->setKeepBakeData(true);
    ASSERT_TRUE(system->bakeNow(level.surface, NavBakeMode::FULL).ok);
    ASSERT_TRUE(system->bakeNow(level.volumeSurface, NavBakeMode::FULL).ok);
    // the box moves after the bake: its old place is stale
    scene.getComponent<Transform>(level.moved).position = Vector3(-10.0f, 0.5f, 13.0f);
    level.resolve();

    nav::NavCollectOptions record;
    record.recordSources = true;
    const auto input = system->inspectInputs(level.surface, record, true).input;
    ASSERT_TRUE(input);
    auto explain = [&](const Vector3& point, uint16_t mask = nav::ALL_AREAS, Entity surface = NULL_ENTITY) {
        nav::QueryFilter filter;
        filter.areaMask = mask;
        return NavWalkabilityProbe::explain(&scene, *system, surface != NULL_ENTITY ? surface : level.surface, point, filter, input);
    };

    // on the floor: nothing to explain
    const NavProbeResult open = explain(Vector3(-5.0f, 0.0f, -5.0f));
    EXPECT_TRUE(open.location.onMesh);
    EXPECT_TRUE(open.reasons.empty()) << codes(open);

    // the ramp's top face, at its middle
    const Vector3 rampUp = scene.getComponent<Transform>(level.ramp).worldRotation * Vector3(0.0f, 1.0f, 0.0f);
    ASSERT_NEAR(rampUp.y, 0.5f, 0.01f);
    const NavProbeResult ramp = explain(Vector3(-20.0f, 2.0f, -20.0f) + rampUp * 0.1f);
    ASSERT_TRUE(has(ramp, "too_steep")) << codes(ramp);
    EXPECT_NEAR(find(ramp, "too_steep")->value, 60.0f, 1.0f);
    EXPECT_NEAR(find(ramp, "too_steep")->limit, 45.0f, 1e-3f);

    const NavProbeResult ceiling = explain(Vector3(20.0f, 0.0f, -20.0f));
    ASSERT_TRUE(has(ceiling, "low_clearance")) << codes(ceiling);
    EXPECT_NEAR(find(ceiling, "low_clearance")->value, 1.2f, 0.05f);
    EXPECT_NEAR(find(ceiling, "low_clearance")->limit, 2.0f, 1e-3f);

    const NavProbeResult wall = explain(Vector3(0.4f, 0.0f, 20.0f));
    EXPECT_TRUE(has(wall, "near_wall")) << codes(wall);
    EXPECT_NEAR(find(wall, "near_wall")->limit, 0.5f, 1e-3f);

    const NavProbeResult platform = explain(Vector3(-20.0f, 1.0f, 20.0f));
    EXPECT_TRUE(has(platform, "small_region")) << codes(platform);
    EXPECT_NEAR(find(platform, "small_region")->limit, 2.0f, 1e-3f);
    EXPECT_NEAR(find(platform, "small_region")->value, 0.81f, 0.3f);

    const NavProbeResult volume = explain(Vector3(20.0f, 0.0f, 20.0f));
    ASSERT_TRUE(has(volume, "not_walkable_area")) << codes(volume);
    EXPECT_EQ(find(volume, "not_walkable_area")->entity, level.notWalkable);

    const NavProbeResult layer = explain(Vector3(0.0f, 1.0f, -20.0f));
    ASSERT_TRUE(has(layer, "excluded")) << codes(layer);
    EXPECT_EQ(find(layer, "excluded")->entity, level.hidden);
    EXPECT_EQ(find(layer, "excluded")->detail, "layer");

#ifdef JASPERENGINE_PHYSICS_3D
    const NavProbeResult dynamic = explain(Vector3(10.0f, 1.0f, -20.0f));
    ASSERT_TRUE(has(dynamic, "excluded")) << codes(dynamic);
    EXPECT_EQ(find(dynamic, "excluded")->entity, level.body);
    EXPECT_EQ(find(dynamic, "excluded")->detail, "moving body");
#endif

    // the Room surface collects a 10 m volume around the origin
    const NavProbeResult outside = explain(Vector3(20.0f, 0.0f, -10.0f), nav::ALL_AREAS, level.volumeSurface);
    EXPECT_TRUE(has(outside, "outside_volume")) << codes(outside);

    const NavProbeResult empty = explain(Vector3(40.0f, 0.0f, 0.0f));
    EXPECT_TRUE(has(empty, "no_geometry")) << codes(empty);
    EXPECT_TRUE(has(explain(Vector3(80.0f, 0.0f, 0.0f)), "outside_bounds"));

    const NavProbeResult stale = explain(Vector3(-10.0f, 0.0f, 10.0f));
    EXPECT_TRUE(has(stale, "stale_tile")) << codes(stale);
    EXPECT_EQ(stale.reasons.front().code, "stale_tile");

    // on the 0.6 m platform, 0.2 m from its edge
    const NavProbeResult ledge = explain(Vector3(17.2f, 0.6f, 0.0f));
    EXPECT_TRUE(has(ledge, "step_too_high")) << codes(ledge);
    EXPECT_NEAR(find(ledge, "step_too_high")->limit, 0.4f, 1e-3f);

    // on the mesh: only what still applies there
    const NavProbeResult masked = explain(Vector3(-20.0f, 0.0f, 0.0f), (uint16_t)(nav::ALL_AREAS & ~(1u << nav::AREA_JUMP)));
    ASSERT_TRUE(has(masked, "filtered")) << codes(masked);
    EXPECT_EQ(find(masked, "filtered")->detail, "Jump");
    const NavProbeResult island = explain(Vector3(50.0f, 0.0f, 0.0f));
    ASSERT_TRUE(has(island, "small_island")) << codes(island);
    EXPECT_EQ(island.reasons.size(), 1u) << codes(island);

    const NavProbeResult unbaked = explain(Vector3(), nav::ALL_AREAS, level.unbakedSurface);
    ASSERT_EQ(unbaked.reasons.size(), 1u);
    EXPECT_EQ(unbaked.reasons.front().code, "not_baked");

    // without a given input the probe collects one itself
    nav::QueryFilter all;
    EXPECT_TRUE(has(NavWalkabilityProbe::explain(&scene, *system, level.surface, Vector3(20.0f, 0.0f, 20.0f), all), "not_walkable_area"));
}
