// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fog resolve (docs/specs/S101-vapor.md R7-R11, R21, R43, AC3): the layer order, the blend rules,
// the Fog Zones' weight and order, blendTo, and the runtime writes layer; its cost with 16 zones
// and a blend (FogResolveBench).

#include "render/FogResolve.h"
#include "render/FogProfile.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "component/Transform.h"
#include "object/environment/Fog.h"
#include "object/environment/FogZone.h"
#include "Engine.h"
#include "Scene.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace fog_resolve_test {

    FogProfileData profileWith(FogField field, const FogComponent& values){
        FogProfileData profile;
        profile.setFrom(values, field);
        return profile;
    }

    FogProfileData colorProfile(const Vector3& color){
        FogComponent values;
        values.color = color;
        return profileWith(FogField::Color, values);
    }

    // a zone entity with a world transform (the resolve reads the world values)
    Entity makeZone(Scene& scene, const Vector3& position, const FogZoneComponent& zone){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = position;
        transform.worldPosition = position;
        scene.addComponent<FogZoneComponent>(entity, zone);
        return entity;
    }

    Entity makeFog(Scene& scene, const FogComponent& fog = FogComponent()){
        const Entity entity = scene.createEntity();
        scene.addComponent<FogComponent>(entity, fog);
        return entity;
    }

    bool nearly(const Vector3& a, const Vector3& b, float epsilon = 1e-5f){
        return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon && std::abs(a.z - b.z) <= epsilon;
    }

    // the library's profiles a test sets, cleared after it
    struct LibraryScope{
        LibraryScope(){ FogProfileLibrary::clear(); }
        ~LibraryScope(){ FogProfileLibrary::clear(); }
    };

}

using namespace fog_resolve_test;

TEST(FogResolveTest, LayerOrder){
    FogComponent fog;
    fog.color = Vector3(0.1f, 0.1f, 0.1f);
    fog.density = 0.5f;
    fog.linearEnd = 33.0f;

    FogComponent profileValues;
    profileValues.color = Vector3(0.2f, 0.2f, 0.2f);
    profileValues.density = 0.25f;
    FogProfileData profile = profileWith(FogField::Color, profileValues);
    profile.setFrom(profileValues, FogField::Density);

    const FogProfileData zoneProfile = colorProfile(Vector3(0.3f, 0.3f, 0.3f));
    const FogZoneSample zone{Entity(42), 0, 1.0f, &zoneProfile};

    FogRuntimeWrites runtime;
    runtime.values.color = Vector3(0.4f, 0.4f, 0.4f);
    runtime.set.set(static_cast<size_t>(FogField::Color));

    FogResolve out;
    // 1. the Fog's own fields
    resolveFog(fog, nullptr, nullptr, {}, nullptr, out);
    EXPECT_EQ(out.resolved.color, fog.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Fog);
    // 2. its profile, for what the profile sets
    resolveFog(fog, &profile, nullptr, {}, nullptr, out);
    EXPECT_EQ(out.resolved.color, profileValues.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Profile);
    EXPECT_FLOAT_EQ(out.resolved.density, 0.25f);
    EXPECT_FLOAT_EQ(out.resolved.linearEnd, 33.0f);
    EXPECT_EQ(out.sourceOf(FogField::LinearEnd).layer, FogLayer::Fog);
    // 4. a zone over it: only what the zone sets
    resolveFog(fog, &profile, nullptr, std::span<const FogZoneSample>(&zone, 1), nullptr, out);
    EXPECT_EQ(out.resolved.color, zoneProfile.values.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Zone);
    EXPECT_EQ(out.sourceOf(FogField::Color).zone, Entity(42));
    EXPECT_EQ(fogLayerSourceName(out.sourceOf(FogField::Color)), "zone:42");
    EXPECT_FLOAT_EQ(out.resolved.density, 0.25f);
    EXPECT_EQ(out.sourceOf(FogField::Density).layer, FogLayer::Profile);
    // 5. the runtime writes on top of everything
    resolveFog(fog, &profile, nullptr, std::span<const FogZoneSample>(&zone, 1), &runtime, out);
    EXPECT_EQ(out.resolved.color, runtime.values.color);
    EXPECT_EQ(fogLayerSourceName(out.sourceOf(FogField::Color)), "runtime");
    EXPECT_FLOAT_EQ(out.resolved.density, 0.25f);

    // 3. the blend sits between the profile and the zones: it replaces the profile's layer
    FogComponent from = fog;
    from.color = Vector3(0.0f, 0.0f, 0.0f);
    const FogProfileData target = colorProfile(Vector3(1.0f, 1.0f, 1.0f));
    const FogBlendSample blend{&from, &target, 0.5f};
    resolveFog(fog, &profile, &blend, {}, nullptr, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.5f, 0.5f, 0.5f)));
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Blend);
    resolveFog(fog, &profile, &blend, std::span<const FogZoneSample>(&zone, 1), nullptr, out);
    EXPECT_EQ(out.resolved.color, zoneProfile.values.color);

    // the same order on a scene: a Fog linked to a profile, a zone around the camera, a write in Play
    LibraryScope library;
    FogProfileLibrary::set("FogProfiles/base.fogprofile", profile);
    FogProfileLibrary::set("FogProfiles/zone.fogprofile", zoneProfile);
    Scene scene;
    FogComponent linked = fog;
    linked.profile = "FogProfiles/base.fogprofile";
    const Entity fogEntity = makeFog(scene, linked);
    FogZoneComponent zoneComponent;
    zoneComponent.profile = "FogProfiles/zone.fogprofile";
    const Entity zoneEntity = makeZone(scene, Vector3(0.0f, 0.0f, 0.0f), zoneComponent);
    FogComponent& sceneFog = scene.getComponent<FogComponent>(fogEntity);
    EXPECT_TRUE(FogScene::needsResolve(&scene, sceneFog));
    std::vector<FogZoneState> zones;
    FogScene::resolve(&scene, sceneFog, Vector3(0.0f, 0.0f, 0.0f), out, &zones);
    EXPECT_EQ(out.resolved.color, zoneProfile.values.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).zone, zoneEntity);
    ASSERT_EQ(zones.size(), 1u);
    EXPECT_FLOAT_EQ(zones[0].weight, 1.0f);
    // far from the zone the profile shows
    FogScene::resolve(&scene, sceneFog, Vector3(100.0f, 0.0f, 0.0f), out);
    EXPECT_EQ(out.resolved.color, profileValues.color);
}

TEST(FogResolveTest, ExtinctionSpaceBlend){
    FogComponent fog;
    fog.attenuationDistance = 100.0f;
    FogComponent values;
    values.attenuationDistance = 10.0f;
    const FogProfileData dense = profileWith(FogField::AttenuationDistance, values);

    // half way in extinction: 1 / ((1/100 + 1/10) / 2) = 18.18 m, not the 55 m of a distance lerp
    const FogZoneSample half{Entity(1), 0, 0.5f, &dense};
    FogResolve out;
    resolveFog(fog, nullptr, nullptr, std::span<const FogZoneSample>(&half, 1), nullptr, out);
    EXPECT_NEAR(out.resolved.attenuationDistance, 1.0f / 0.055f, 1e-3f);
    for (float w : {0.0f, 0.25f, 0.75f, 1.0f}){
        const FogZoneSample zone{Entity(1), 0, w, &dense};
        resolveFog(fog, nullptr, nullptr, std::span<const FogZoneSample>(&zone, 1), nullptr, out);
        const float extinction = 1.0f / out.resolved.attenuationDistance;
        EXPECT_NEAR(extinction, 0.01f + (0.1f - 0.01f) * w, 1e-6f) << w;
    }

    // a blend follows the same rule
    FogComponent from = fog;
    const FogBlendSample blend{&from, &dense, 0.5f};
    resolveFog(fog, nullptr, &blend, {}, nullptr, out);
    EXPECT_NEAR(out.resolved.attenuationDistance, 1.0f / 0.055f, 1e-3f);

    // colours in linear RGB, plain lerp
    const FogProfileData red = colorProfile(Vector3(1.0f, 0.0f, 0.0f));
    fog.color = Vector3(0.0f, 0.0f, 1.0f);
    const FogZoneSample quarter{Entity(1), 0, 0.25f, &red};
    resolveFog(fog, nullptr, nullptr, std::span<const FogZoneSample>(&quarter, 1), nullptr, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.25f, 0.0f, 0.75f)));
}

TEST(FogResolveTest, EnumSwitchAtHalf){
    FogComponent fog;
    fog.colorMode = FogColorMode::COLOR;
    fog.skyFill = true;
    FogComponent values;
    values.colorMode = FogColorMode::LIT;
    values.skyFill = false;
    FogProfileData profile = profileWith(FogField::ColorMode, values);
    profile.setFrom(values, FogField::SkyFill);

    FogResolve out;
    for (float w : {0.0f, 0.25f, 0.49f}){
        const FogZoneSample zone{Entity(1), 0, w, &profile};
        resolveFog(fog, nullptr, nullptr, std::span<const FogZoneSample>(&zone, 1), nullptr, out);
        EXPECT_EQ(out.resolved.colorMode, FogColorMode::COLOR) << w;
        EXPECT_TRUE(out.resolved.skyFill) << w;
        EXPECT_EQ(out.sourceOf(FogField::ColorMode).layer, FogLayer::Fog) << w;
    }
    for (float w : {0.5f, 0.75f, 1.0f}){
        const FogZoneSample zone{Entity(1), 0, w, &profile};
        resolveFog(fog, nullptr, nullptr, std::span<const FogZoneSample>(&zone, 1), nullptr, out);
        EXPECT_EQ(out.resolved.colorMode, FogColorMode::LIT) << w;
        EXPECT_FALSE(out.resolved.skyFill) << w;
        EXPECT_EQ(out.sourceOf(FogField::ColorMode).layer, FogLayer::Zone) << w;
    }
    // the blend switches at t = 0.5 too
    FogComponent from = fog;
    for (float t : {0.49f, 0.5f}){
        const FogBlendSample blend{&from, &profile, t};
        resolveFog(fog, nullptr, &blend, {}, nullptr, out);
        EXPECT_EQ(out.resolved.colorMode, t >= 0.5f ? FogColorMode::LIT : FogColorMode::COLOR) << t;
    }
}

TEST(FogResolveTest, ZoneWeightLinearInBlendDistance){
    FogZoneComponent zone;
    zone.size = Vector3(10.0f, 10.0f, 10.0f);
    zone.blendDistance = 5.0f;
    Transform transform;
    transform.worldPosition = Vector3(0.0f, 0.0f, 0.0f);

    // inside and on the surface: the zone's weight; then linear over the blend distance
    const float expected[][2] = {{0.0f, 1.0f}, {4.9f, 1.0f}, {5.0f, 1.0f}, {6.25f, 0.75f}, {7.5f, 0.5f}, {8.75f, 0.25f}, {10.0f, 0.0f}, {20.0f, 0.0f}};
    for (const auto& row : expected){
        const Vector3 camera(row[0], 0.0f, 0.0f);
        EXPECT_NEAR(fogZoneWeight(zone, fogZoneDistance(zone, transform, camera)), row[1], 1e-5f) << row[0];
    }
    // the distance is to the box, not its centre: a corner direction
    EXPECT_NEAR(fogZoneDistance(zone, transform, Vector3(8.0f, 9.0f, 0.0f)), std::sqrt(3.0f * 3.0f + 4.0f * 4.0f), 1e-5f);
    // the weight scales it
    zone.weight = 0.4f;
    EXPECT_NEAR(fogZoneWeight(zone, fogZoneDistance(zone, transform, Vector3(7.5f, 0.0f, 0.0f))), 0.2f, 1e-5f);
    zone.weight = 1.0f;

    // blend distance 0: a hard switch at the surface
    zone.blendDistance = 0.0f;
    EXPECT_FLOAT_EQ(fogZoneWeight(zone, fogZoneDistance(zone, transform, Vector3(4.99f, 0.0f, 0.0f))), 1.0f);
    EXPECT_FLOAT_EQ(fogZoneWeight(zone, fogZoneDistance(zone, transform, Vector3(5.01f, 0.0f, 0.0f))), 0.0f);
    zone.blendDistance = 5.0f;

    // a sphere of that size: radius 5
    zone.shape = FogZoneShape::SPHERE;
    EXPECT_NEAR(fogZoneDistance(zone, transform, Vector3(0.0f, 7.5f, 0.0f)), 2.5f, 1e-5f);
    EXPECT_NEAR(fogZoneWeight(zone, 2.5f), 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(fogZoneDistance(zone, transform, Vector3(3.0f, 3.0f, 0.0f)), 0.0f);

    // the Transform scales, turns and moves it, as Local Volumetric Fog
    zone.shape = FogZoneShape::BOX;
    transform.worldPosition = Vector3(100.0f, 0.0f, 0.0f);
    transform.worldScale = Vector3(2.0f, 1.0f, 1.0f);
    transform.worldRotation = Quaternion(90.0f, Vector3(0.0f, 1.0f, 0.0f));
    // the long local x axis lies along world z now: 10 m half extent there, 5 m along world x
    EXPECT_NEAR(fogZoneDistance(zone, transform, Vector3(100.0f, 0.0f, 9.0f)), 0.0f, 1e-4f);
    EXPECT_NEAR(fogZoneDistance(zone, transform, Vector3(107.5f, 0.0f, 0.0f)), 2.5f, 1e-4f);

    // the scene's zones at a camera
    LibraryScope library;
    FogProfileLibrary::set("z.fogprofile", colorProfile(Vector3(1.0f, 0.0f, 0.0f)));
    Scene scene;
    FogZoneComponent component;
    component.profile = "z.fogprofile";
    const Entity entity = makeZone(scene, Vector3(0.0f, 0.0f, 0.0f), component);
    const std::vector<FogZoneState> states = FogScene::zoneStates(&scene, Vector3(7.5f, 0.0f, 0.0f));
    ASSERT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0].entity, entity);
    EXPECT_NEAR(states[0].distance, 2.5f, 1e-5f);
    EXPECT_NEAR(states[0].weight, 0.5f, 1e-5f);
    // a zone whose profile does not read has no effect (R6) and says so
    scene.getComponent<FogZoneComponent>(entity).profile = "gone.fogprofile";
    const std::vector<FogZoneState> missing = FogScene::zoneStates(&scene, Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_TRUE(missing[0].profileMissing);
    EXPECT_FLOAT_EQ(missing[0].weight, 0.0f);
}

TEST(FogResolveTest, PriorityThenEntityId){
    const FogProfileData red = colorProfile(Vector3(1.0f, 0.0f, 0.0f));
    const FogProfileData green = colorProfile(Vector3(0.0f, 1.0f, 0.0f));
    FogComponent fog;
    FogResolve out;

    // the higher priority applies last, whatever order the zones come in
    const FogZoneSample byPriority[] = {{Entity(1), 2, 1.0f, &red}, {Entity(2), 1, 1.0f, &green}};
    resolveFog(fog, nullptr, nullptr, byPriority, nullptr, out);
    EXPECT_EQ(out.resolved.color, red.values.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).zone, Entity(1));

    // the same priority: the higher entity id applies last
    const FogZoneSample byEntity[] = {{Entity(10), 0, 1.0f, &red}, {Entity(5), 0, 1.0f, &green}};
    resolveFog(fog, nullptr, nullptr, byEntity, nullptr, out);
    EXPECT_EQ(out.resolved.color, red.values.color);
    const FogZoneSample byEntityReversed[] = {{Entity(5), 0, 1.0f, &green}, {Entity(10), 0, 1.0f, &red}};
    resolveFog(fog, nullptr, nullptr, byEntityReversed, nullptr, out);
    EXPECT_EQ(out.resolved.color, red.values.color);

    // half weights stack in that order: green over red over the fog
    fog.color = Vector3(0.0f, 0.0f, 0.0f);
    const FogZoneSample halves[] = {{Entity(3), 5, 0.5f, &green}, {Entity(4), 1, 0.5f, &red}};
    resolveFog(fog, nullptr, nullptr, halves, nullptr, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.25f, 0.5f, 0.0f)));

    // on a scene, ordered the same way
    LibraryScope library;
    FogProfileLibrary::set("red.fogprofile", red);
    FogProfileLibrary::set("green.fogprofile", green);
    Scene scene;
    const Entity fogEntity = makeFog(scene);
    FogZoneComponent high;
    high.priority = 3;
    high.profile = "red.fogprofile";
    FogZoneComponent low;
    low.priority = -1;
    low.profile = "green.fogprofile";
    const Entity highEntity = makeZone(scene, Vector3(0.0f, 0.0f, 0.0f), high);
    makeZone(scene, Vector3(0.0f, 0.0f, 0.0f), low);
    FogScene::resolve(&scene, scene.getComponent<FogComponent>(fogEntity), Vector3(0.0f, 0.0f, 0.0f), out);
    EXPECT_EQ(out.resolved.color, red.values.color);
    EXPECT_EQ(out.sourceOf(FogField::Color).zone, highEntity);
}

TEST(FogResolveTest, BlendToRetargetNoJump){
    LibraryScope library;
    FogProfileLibrary::set("a.fogprofile", colorProfile(Vector3(1.0f, 0.0f, 0.0f)));
    FogProfileLibrary::set("b.fogprofile", colorProfile(Vector3(0.0f, 0.0f, 1.0f)));
    Scene scene;
    FogComponent own;
    own.color = Vector3(0.0f, 0.0f, 0.0f);
    const Entity entity = makeFog(scene, own);
    FogComponent& fog = scene.getComponent<FogComponent>(entity);
    const Vector3 camera(0.0f, 0.0f, 0.0f);
    FogResolve out;

    FogScene::blendTo(fog, "a.fogprofile", 2.0);
    EXPECT_TRUE(FogScene::blendState(fog).active);
    EXPECT_TRUE(FogScene::needsResolve(&scene, fog));
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.0f, 0.0f, 0.0f)));
    FogScene::advance(&scene, 1.0);
    EXPECT_NEAR(FogScene::blendState(fog).progress, 0.5f, 1e-6f);
    FogScene::resolve(&scene, fog, camera, out);
    const Vector3 midway = out.resolved.color;
    EXPECT_TRUE(nearly(midway, Vector3(0.5f, 0.0f, 0.0f)));

    // re-targeted half way: it starts from where it is, no jump
    FogScene::blendTo(fog, "b.fogprofile", 2.0);
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_TRUE(nearly(out.resolved.color, midway));
    EXPECT_EQ(FogScene::blendState(fog).targetKey, "b.fogprofile");
    FogScene::advance(&scene, 1.0);
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.25f, 0.0f, 0.5f)));
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Blend);
    // no step anywhere along it
    Vector3 previous = out.resolved.color;
    for (int i = 0; i < 10; i++){
        FogScene::advance(&scene, 0.1);
        FogScene::resolve(&scene, fog, camera, out);
        EXPECT_LE((out.resolved.color - previous).length(), 0.08f) << i;
        previous = out.resolved.color;
    }
    // (ten steps of 0.1 s fall a hair short of 2 s in floating point)
    FogScene::advance(&scene, 0.01);

    // at the end the target is the Fog's runtime profile (not its saved one)
    EXPECT_FALSE(FogScene::blendState(fog).active);
    EXPECT_EQ(FogScene::profileKey(fog), "b.fogprofile");
    EXPECT_TRUE(fog.profile.path.empty());
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_EQ(out.resolved.color, Vector3(0.0f, 0.0f, 1.0f));
    EXPECT_EQ(out.sourceOf(FogField::Color).layer, FogLayer::Profile);

    // seconds <= 0 applies at once
    FogScene::blendTo(fog, "a.fogprofile", 0.0);
    EXPECT_FALSE(FogScene::blendState(fog).active);
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_EQ(out.resolved.color, Vector3(1.0f, 0.0f, 0.0f));

    // setting the profile directly applies at once and cancels a blend
    FogScene::blendTo(fog, "b.fogprofile", 5.0);
    FogScene::advance(&scene, 1.0);
    FogScene::setRuntimeProfile(fog, "a.fogprofile");
    EXPECT_FALSE(FogScene::blendState(fog).active);
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_EQ(out.resolved.color, Vector3(1.0f, 0.0f, 0.0f));
}

TEST(FogResolveTest, BlendToNilReturnsToOwnFields){
    LibraryScope library;
    FogComponent values;
    values.color = Vector3(1.0f, 1.0f, 1.0f);
    values.attenuationDistance = 10.0f;
    FogProfileData profile = profileWith(FogField::Color, values);
    profile.setFrom(values, FogField::AttenuationDistance);
    FogProfileLibrary::set("white.fogprofile", profile);

    Scene scene;
    FogComponent own;
    own.color = Vector3(0.0f, 0.2f, 0.0f);
    own.attenuationDistance = 400.0f;
    own.profile = "white.fogprofile";
    const Entity entity = makeFog(scene, own);
    FogComponent& fog = scene.getComponent<FogComponent>(entity);
    const Vector3 camera(0.0f, 0.0f, 0.0f);
    FogResolve out;
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_EQ(out.resolved.color, values.color);

    FogScene::blendTo(fog, "", 4.0);
    FogScene::advance(&scene, 2.0);
    FogScene::resolve(&scene, fog, camera, out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.5f, 0.6f, 0.5f)));
    // extinction space on the way back too
    EXPECT_NEAR(1.0f / out.resolved.attenuationDistance, (0.1f + 0.0025f) * 0.5f, 1e-6f);
    FogScene::advance(&scene, 2.5);
    FogScene::resolve(&scene, fog, camera, out);
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
        EXPECT_TRUE(FogFields::equal(out.resolved, own, static_cast<FogField>(i))) << FogFields::name(static_cast<FogField>(i));
        EXPECT_EQ(out.source[i].layer, FogLayer::Fog) << FogFields::name(static_cast<FogField>(i));
    }
    // the runtime profile is none; the saved reference is untouched
    EXPECT_EQ(FogScene::profileKey(fog), "");
    EXPECT_EQ(fog.profile.path, "white.fogprofile");
    // Stop drops it: the saved profile again
    FogScene::resetRuntime(&scene);
    EXPECT_EQ(FogScene::profileKey(fog), "white.fogprofile");
}

TEST(FogResolveTest, RuntimeWriteWinsOverProfileAndIsClearedOnStop){
    LibraryScope library;
    FogComponent values;
    values.color = Vector3(1.0f, 0.0f, 0.0f);
    values.density = 0.2f;
    FogProfileData profile = profileWith(FogField::Color, values);
    profile.setFrom(values, FogField::Density);
    FogProfileLibrary::set("red.fogprofile", profile);
    FogProfileLibrary::set("zone.fogprofile", colorProfile(Vector3(0.0f, 1.0f, 0.0f)));

    Scene scene;
    Fog fog(&scene);
    fog.setColor(0.0f, 0.0f, 0.0f);
    fog.setProfile("red.fogprofile");
    FogComponent& component = scene.getComponent<FogComponent>(fog.getEntity());
    // outside a running game the writes are the Fog's own, as before
    EXPECT_EQ(component.profile.path, "red.fogprofile");
    EXPECT_EQ(component.color, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(FogScene::isFieldSetByProfile(component, FogField::LinearEnd));
    EXPECT_TRUE(FogScene::isFieldSetByProfile(component, FogField::Color));
    FogZone zone(&scene);
    zone.setProfile("zone.fogprofile");
    scene.getComponent<Transform>(zone.getEntity()).worldPosition = Vector3(0.0f, 0.0f, 0.0f);

    // in a running game a script's write goes above the profile and the zones
    Engine::setScene(&scene);
    ASSERT_TRUE(FogScene::isRunning(&scene));
    fog.setColor(Vector3(0.0f, 0.0f, 1.0f));
    fog.setDensity(0.7f);
    FogResolve out;
    FogScene::resolve(&scene, component, Vector3(0.0f, 0.0f, 0.0f), out);
    EXPECT_TRUE(nearly(out.resolved.color, Vector3(0.0f, 0.0f, 1.0f)));
    EXPECT_EQ(fogLayerSourceName(out.sourceOf(FogField::Color)), "runtime");
    EXPECT_FLOAT_EQ(out.resolved.density, 0.7f);
    // the Fog's own fields are untouched; the property reads what the script wrote
    EXPECT_EQ(component.color, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(nearly(Color::sRGBToLinear(fog.getColor()), Vector3(0.0f, 0.0f, 1.0f), 1e-4f));
    EXPECT_TRUE(nearly(fog.getResolved().color, Vector3(0.0f, 0.0f, 1.0f)));

    // clearOverride gives the field back to the layers beneath
    EXPECT_TRUE(fog.clearOverride("color"));
    FogScene::resolve(&scene, component, Vector3(0.0f, 0.0f, 0.0f), out);
    EXPECT_EQ(out.resolved.color, Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(out.resolved.density, 0.7f);
    EXPECT_FALSE(fog.clearOverride("volumetric"));
    fog.clearOverrides();
    FogScene::resolve(&scene, component, Vector3(0.0f, 0.0f, 0.0f), out);
    EXPECT_FLOAT_EQ(out.resolved.density, 0.2f);

    // a profile set while running is a runtime one: the saved reference stays
    fog.setColor(Vector3(0.0f, 0.0f, 1.0f));
    fog.setProfile("");
    EXPECT_EQ(fog.getProfile(), "");
    EXPECT_EQ(component.profile.path, "red.fogprofile");

    // Stop: every runtime layer goes, nothing of it was ever in the component's saved fields
    Engine::removeScene(&scene);
    FogScene::resetRuntime(&scene);
    EXPECT_EQ(component.runtime.get(), nullptr);
    EXPECT_EQ(fog.getProfile(), "red.fogprofile");
    FogScene::resolve(&scene, component, Vector3(100.0f, 0.0f, 0.0f), out);
    EXPECT_EQ(out.resolved.color, Vector3(1.0f, 0.0f, 0.0f));

    // a copy of the component never carries the runtime state
    Engine::setScene(&scene);
    fog.setDensity(0.9f);
    ASSERT_NE(component.runtime.get(), nullptr);
    const FogComponent copy = component;
    EXPECT_EQ(copy.runtime.get(), nullptr);
    Engine::removeScene(&scene);
}

TEST(FogResolveTest, NothingToLayerNeedsNoResolve){
    // R11: a Fog with no profile, no zone, no blend and no write skips the resolve
    Scene scene;
    const Entity entity = makeFog(scene);
    FogComponent& fog = scene.getComponent<FogComponent>(entity);
    testsupport::ThreadAllocationCount allocations;
    EXPECT_FALSE(FogScene::needsResolve(&scene, fog));
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_EQ(fog.runtime.get(), nullptr);
    // each layer turns it on
    fog.profile = "x.fogprofile";
    EXPECT_TRUE(FogScene::needsResolve(&scene, fog));
    fog.profile.clear();
    makeZone(scene, Vector3(0.0f, 0.0f, 0.0f), FogZoneComponent());
    EXPECT_TRUE(FogScene::needsResolve(&scene, fog));
}

TEST(FogResolveBench, SixteenZonesAndABlend){
    // R21: the resolve with 16 zones and a blend, <= 0.02 ms on the mid desktop
    LibraryScope library;
    Scene scene;
    FogComponent own;
    own.type = FogType::HEIGHT;
    const Entity fogEntity = makeFog(scene, own);
    for (int i = 0; i < 16; i++){
        FogComponent values;
        values.color = Vector3(0.05f * (float)i, 0.5f, 0.2f);
        values.attenuationDistance = 20.0f + (float)i;
        values.colorMode = FogColorMode::LIT;
        FogProfileData profile = profileWith(FogField::Color, values);
        profile.setFrom(values, FogField::AttenuationDistance);
        profile.setFrom(values, FogField::ColorMode);
        const std::string path = "FogProfiles/zone" + std::to_string(i) + ".fogprofile";
        FogProfileLibrary::set(path, profile);
        FogZoneComponent zone;
        zone.priority = i % 3;
        zone.blendDistance = 40.0f;
        zone.profile = path;
        makeZone(scene, Vector3((float)i * 4.0f, 0.0f, 0.0f), zone);
    }
    FogProfileLibrary::set("FogProfiles/night.fogprofile", FogProfiles::findBuiltin("night")->data);
    FogComponent& fog = scene.getComponent<FogComponent>(fogEntity);
    FogScene::blendTo(fog, "FogProfiles/night.fogprofile", 10.0);
    FogScene::advance(&scene, 3.0);

    FogResolve out;
    const Vector3 camera(30.0f, 0.0f, 0.0f);
    FogScene::resolve(&scene, fog, camera, out);   // warm the scratch buffers
    int active = 0;
    for (const FogZoneState& state : FogScene::zoneStates(&scene, camera))
        active += state.weight > 0.0f ? 1 : 0;
    EXPECT_EQ(active, 16);

    constexpr int runs = 20000;
    testsupport::ThreadAllocationCount allocations;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; i++)
        FogScene::resolve(&scene, fog, camera, out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
    std::printf("[bench] FogScene::resolve, 16 zones and a blend: %.5f ms (budget 0.02 ms)\n", ms);
    EXPECT_EQ(allocations.count(), 0u);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 0.02 * testsupport::perfSlack());
    }
}
