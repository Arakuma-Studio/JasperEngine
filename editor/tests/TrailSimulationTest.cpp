// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Trail Renderer simulation (lines and trails spec 6.4, 7): TrailSimulation with an injected
// clock (points added by distance, removed by age, emitting off, clear, autodestruct after the
// object stood still for `time`, time scale 0 freezing it, time 0 keeping nothing, the
// 4096-point cap, the ribbon's centre line head first with its parameters by age) and
// TrailSystem in a Scene (a deactivated GameObject or a disabled trail is cleared, the
// edit-mode preview, autodestruct asking GameObject.destroy). The ring storage.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/Transform.h"
#include "component/TrailRendererComponent.h"
#include "manager/ObjectManager.h"
#include "subsystem/TrailSystem.h"
#include "object/TrailRenderer.h"

#include <vector>

using namespace jasperengine;

namespace {

    // a trail moving along x by `speed` per second at `fps`, from x = 0
    struct Mover {
        TrailRendererComponent trail;
        float now = 0.0f;
        float x = 0.0f;

        unsigned int step(float dt, float dx){
            now += dt;
            x += dx;
            return TrailSimulation::step(trail, Vector3(x, 0.0f, 0.0f), now, dt);
        }
    };

}

TEST(TrailSimulationTest, PointRingGrowsAndKeepsOrder){
    TrailPointRing ring;
    for (int i = 0; i < 40; i++){
        ring.push({Vector3((float)i, 0, 0), (float)i});
        if (i % 3 == 0) ring.popOldest();
    }
    ASSERT_EQ(ring.size(), 26u);
    for (size_t i = 1; i < ring.size(); i++){
        EXPECT_LT(ring.at(i - 1).birthTime, ring.at(i).birthTime);
    }
    EXPECT_FLOAT_EQ(ring.newest().birthTime, 39.0f);
    ring.clear();
    EXPECT_TRUE(ring.empty());
    EXPECT_GE(ring.capacity(), 32u);
    ring.reset();
    EXPECT_EQ(ring.capacity(), 0u);
}

TEST(TrailSimulationTest, PointsAddedByDistanceAndRemovedByAge){
    Mover mover;
    mover.trail.time = 0.3f;
    mover.trail.minVertexDistance = 0.1f;
    // the first frame only records where the head is
    EXPECT_EQ(mover.step(0.0f, 0.0f) & TrailSimulation::Step_Changed, 0u);
    EXPECT_TRUE(mover.trail.points.empty());

    // 0.04 per frame at 60 fps: the start point, then one every 3 frames (0.12 > 0.1)
    mover.step(1.0f / 60.0f, 0.04f);
    ASSERT_EQ(mover.trail.points.size(), 1u);
    EXPECT_FLOAT_EQ(mover.trail.points.at(0).position.x, 0.0f);
    for (int i = 0; i < 2; i++) mover.step(1.0f / 60.0f, 0.04f);
    EXPECT_EQ(mover.trail.points.size(), 2u);
    EXPECT_NEAR(mover.trail.points.newest().position.x, 0.12f, 1e-5f);

    // after a second only the last 0.3 s of points remain (about 0.3 * 60 / 3 = 6)
    for (int i = 0; i < 60; i++) mover.step(1.0f / 60.0f, 0.04f);
    EXPECT_GE(mover.trail.points.size(), 5u);
    EXPECT_LE(mover.trail.points.size(), 7u);
    for (size_t i = 0; i < mover.trail.points.size(); i++){
        EXPECT_LE(mover.now - mover.trail.points.at(i).birthTime, 0.3f + 1e-5f);
    }

    // the centre line: the live head, then the points newest to oldest, by age
    std::vector<Vector3> points;
    std::vector<float> t;
    TrailSimulation::ribbonPoints(mover.trail, Vector3(mover.x, 0, 0), mover.now, points, t);
    ASSERT_EQ(points.size(), mover.trail.points.size() + 1);
    EXPECT_FLOAT_EQ(points[0].x, mover.x);
    EXPECT_FLOAT_EQ(t[0], 0.0f);
    for (size_t i = 1; i < t.size(); i++){
        EXPECT_GE(t[i], t[i - 1]);
        EXPECT_LE(t[i], 1.0f);
        EXPECT_LT(points[i].x, points[i - 1].x + 1e-6f);
    }
}

TEST(TrailSimulationTest, EmittingOffAndClear){
    Mover mover;
    mover.trail.time = 1.0f;
    mover.trail.minVertexDistance = 0.0f;
    mover.step(0.0f, 0.0f);
    for (int i = 0; i < 10; i++) mover.step(0.01f, 0.1f);
    const size_t emitted = mover.trail.points.size();
    EXPECT_GT(emitted, 5u);

    mover.trail.emitting = false;
    for (int i = 0; i < 10; i++) mover.step(0.01f, 0.1f);
    EXPECT_EQ(mover.trail.points.size(), emitted) << "no new point, the stored ones keep fading";
    // detached (Unity): the ribbon is the stored points only, not joined to where the object is
    {
        std::vector<Vector3> points;
        std::vector<float> t;
        TrailSimulation::ribbonPoints(mover.trail, Vector3(mover.x, 0, 0), mover.now, points, t);
        ASSERT_EQ(points.size(), mover.trail.points.size());
        EXPECT_FLOAT_EQ(points.front().x, mover.trail.points.newest().position.x);
        EXPECT_GT(t.front(), 0.0f);
        mover.trail.emitting = true;
        TrailSimulation::ribbonPoints(mover.trail, Vector3(mover.x, 0, 0), mover.now, points, t);
        ASSERT_EQ(points.size(), mover.trail.points.size() + 1);
        EXPECT_FLOAT_EQ(points.front().x, mover.x);
        mover.trail.emitting = false;
    }
    for (int i = 0; i < 110; i++) mover.step(0.01f, 0.1f);
    EXPECT_TRUE(mover.trail.points.empty());

    mover.trail.emitting = true;
    for (int i = 0; i < 5; i++) mover.step(0.01f, 0.1f);
    EXPECT_FALSE(mover.trail.points.empty());
    TrailSimulation::clear(mover.trail);
    EXPECT_TRUE(mover.trail.points.empty());
    // after a clear (a teleport) the trail starts where the object is, no streak back
    mover.x = 100.0f;
    mover.step(0.01f, 0.0f);
    mover.step(0.01f, 0.1f);
    ASSERT_FALSE(mover.trail.points.empty());
    EXPECT_GE(mover.trail.points.at(0).position.x, 100.0f);
}

TEST(TrailSimulationTest, TimeScaleZeroFreezesAndTimeZeroKeepsNothing){
    Mover mover;
    mover.trail.time = 0.5f;
    mover.trail.minVertexDistance = 0.0f;
    mover.step(0.0f, 0.0f);
    for (int i = 0; i < 10; i++) mover.step(0.02f, 0.1f);
    const size_t before = mover.trail.points.size();
    ASSERT_GT(before, 0u);

    // timeScale 0: dt 0, the clock stands; no point added or removed, the head still moves
    for (int i = 0; i < 100; i++){
        const unsigned int result = TrailSimulation::step(mover.trail, Vector3(mover.x + 0.5f * i, 0, 0), mover.now, 0.0f);
        if (i > 0) { EXPECT_TRUE(result & TrailSimulation::Step_Changed); }
    }
    EXPECT_EQ(mover.trail.points.size(), before);

    Mover zero;
    zero.trail.time = 0.0f;
    zero.step(0.0f, 0.0f);
    for (int i = 0; i < 10; i++) zero.step(0.02f, 0.1f);
    EXPECT_TRUE(zero.trail.points.empty());
}

TEST(TrailSimulationTest, AutodestructAfterStandingStill){
    Mover mover;
    mover.trail.time = 0.3f;
    mover.trail.autodestruct = true;
    mover.step(0.0f, 0.0f);
    for (int i = 0; i < 10; i++){
        EXPECT_FALSE(mover.step(0.05f, 0.2f) & TrailSimulation::Step_Destroy);
    }
    // standing still: the points age out, then `time` without moving
    bool destroyed = false;
    float when = 0.0f;
    for (int i = 0; i < 40 && !destroyed; i++){
        destroyed = mover.step(0.05f, 0.0f) & TrailSimulation::Step_Destroy;
        when = mover.now;
    }
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(mover.trail.points.empty());
    EXPECT_GE(when - 0.5f, 0.3f - 1e-4f);

    // without autodestruct nothing asks for it
    Mover still;
    still.trail.time = 0.1f;
    still.step(0.0f, 0.0f);
    for (int i = 0; i < 20; i++) EXPECT_FALSE(still.step(0.05f, 0.0f) & TrailSimulation::Step_Destroy);
}

TEST(TrailSimulationTest, PointCapDropsTheOldest){
    Mover mover;
    mover.trail.time = 1000.0f;
    mover.trail.minVertexDistance = 0.0f;
    mover.step(0.0f, 0.0f);
    bool warned = false;
    for (size_t i = 0; i < TrailSimulation::MAX_POINTS + 50; i++){
        if (mover.step(0.001f, 0.01f) & TrailSimulation::Step_CapacityWarning){
            EXPECT_FALSE(warned) << "warned once";
            warned = true;
        }
    }
    EXPECT_TRUE(warned);
    EXPECT_EQ(mover.trail.points.size(), TrailSimulation::MAX_POINTS);
    EXPECT_GT(mover.trail.points.at(0).position.x, 0.4f);
}

TEST(TrailSimulationTest, SystemClearsInactiveTrailsAndPreviews){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    TrailRendererComponent authored;
    authored.time = 1.0f;
    authored.minVertexDistance = 0.0f;
    scene.addComponent<TrailRendererComponent>(entity, authored);
    std::shared_ptr<TrailSystem> system = scene.getSystem<TrailSystem>();
    ASSERT_TRUE(system);

    auto moveTo = [&](float x){
        Transform& transform = scene.getComponent<Transform>(entity);
        transform.position = Vector3(x, 0, 0);
        transform.worldPosition = Vector3(x, 0, 0);
    };
    moveTo(0.0f);
    system->update(0.016);
    for (int i = 1; i <= 10; i++){
        moveTo(0.1f * i);
        system->update(0.016);
    }
    EXPECT_GT(scene.getComponent<TrailRendererComponent>(entity).points.size(), 5u);
    EXPECT_NEAR(system->getTime(), 11 * 0.016f, 1e-5f);

    // deactivated: cleared, and it starts again from where it is when reactivated
    scene.setEntityActive(entity, false);
    system->update(0.016);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
    scene.setEntityActive(entity, true);
    moveTo(50.0f);
    system->update(0.016);
    moveTo(50.5f);
    system->update(0.016);
    ASSERT_FALSE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
    EXPECT_GE(scene.getComponent<TrailRendererComponent>(entity).points.at(0).position.x, 50.0f);

    // disabled: cleared too
    scene.getComponent<TrailRendererComponent>(entity).enabled = false;
    system->update(0.016);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
    scene.getComponent<TrailRendererComponent>(entity).enabled = true;

    // edit mode (paused): only a previewed trail simulates; ending the preview clears it
    system->setPaused(true);
    moveTo(60.0f);
    system->update(0.016);
    moveTo(61.0f);
    system->update(0.016);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
    system->setPreview(entity, true);
    EXPECT_TRUE(system->isPreviewing(entity));
    moveTo(62.0f);
    system->update(0.016);
    moveTo(63.0f);
    system->update(0.016);
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
    system->setPreview(entity, false);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
}

TEST(TrailSimulationTest, SystemAutodestructDestroysTheGameObject){
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    TrailRendererComponent authored;
    authored.time = 0.1f;
    authored.autodestruct = true;
    scene.addComponent<TrailRendererComponent>(entity, authored);
    std::shared_ptr<TrailSystem> system = scene.getSystem<TrailSystem>();
    for (int i = 0; i < 20; i++){
        system->update(0.016);
        ObjectManager::update(0.016);
        if (!scene.isEntityCreated(entity)) break;
    }
    EXPECT_FALSE(scene.isEntityCreated(entity));
}

TEST(TrailSimulationTest, StillTrailKeepsFadingByAge){
    // the head stops: the points still age, so the ribbon (width and colour by age) changes
    // every frame until the last point expires, and not after
    Mover mover;
    mover.trail.time = 0.5f;
    mover.trail.minVertexDistance = 0.0f;
    mover.step(0.0f, 0.0f);
    for (int i = 0; i < 5; i++) mover.step(1.0f / 60.0f, 0.1f);
    ASSERT_FALSE(mover.trail.points.empty());
    for (int i = 0; i < 10; i++){
        mover.trail.needUpdateRibbon = false;
        EXPECT_TRUE(mover.step(1.0f / 60.0f, 0.0f) & TrailSimulation::Step_Changed) << "frame " << i;
        EXPECT_TRUE(mover.trail.needUpdateRibbon);
    }
    for (int i = 0; i < 60; i++) mover.step(1.0f / 60.0f, 0.0f);
    EXPECT_TRUE(mover.trail.points.empty());
    EXPECT_EQ(mover.step(1.0f / 60.0f, 0.0f) & TrailSimulation::Step_Changed, 0u);
    // time scale 0: nothing ages
    mover.step(1.0f / 60.0f, 0.1f);
    mover.step(1.0f / 60.0f, 0.1f);
    ASSERT_FALSE(mover.trail.points.empty());
    EXPECT_EQ(mover.step(0.0f, 0.0f) & TrailSimulation::Step_Changed, 0u);
}

TEST(TrailSimulationTest, DeactivatedAndReactivatedInOneFrameIsCleared){
    // a pooled bullet: deactivated, moved and reactivated before the next update draws no
    // streak from where it was (its own trail and a child's)
    Scene scene;
    const Entity parent = scene.createUserEntity();
    scene.addComponent<Transform>(parent, {});
    const Entity child = scene.createUserEntity();
    scene.addComponent<Transform>(child, {});
    scene.addEntityChild(parent, child, false);
    TrailRendererComponent authored;
    authored.time = 1.0f;
    authored.minVertexDistance = 0.0f;
    scene.addComponent<TrailRendererComponent>(parent, authored);
    scene.addComponent<TrailRendererComponent>(child, authored);
    std::shared_ptr<TrailSystem> system = scene.getSystem<TrailSystem>();
    for (int i = 0; i <= 10; i++){
        for (Entity e : {parent, child}){
            scene.getComponent<Transform>(e).worldPosition = Vector3(0.1f * i, 0, 0);
        }
        system->update(0.016);
    }
    ASSERT_FALSE(scene.getComponent<TrailRendererComponent>(parent).points.empty());
    ASSERT_FALSE(scene.getComponent<TrailRendererComponent>(child).points.empty());
    scene.setEntityActive(parent, false);
    scene.setEntityActive(parent, true);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(parent).points.empty());
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(child).points.empty());
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(parent).hasLastHeadPosition);
}

TEST(TrailSimulationTest, PreviewNeverAutodestructs){
    // edit mode: a previewed trail that idles past its time keeps its GameObject
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    TrailRendererComponent authored;
    authored.time = 0.1f;
    authored.autodestruct = true;
    scene.addComponent<TrailRendererComponent>(entity, authored);
    std::shared_ptr<TrailSystem> system = scene.getSystem<TrailSystem>();
    system->setPaused(true);
    system->setPreview(entity, true);
    for (int i = 0; i < 30; i++){
        system->update(0.016);
        ObjectManager::update(0.016);
    }
    EXPECT_TRUE(scene.isEntityCreated(entity));
    EXPECT_FALSE(scene.getComponent<TrailRendererComponent>(entity).destroyRequested);
}

TEST(TrailSimulationTest, HandleAddsNothingAddToAdds){
    // TrailRenderer(scene, entity) is a handle (Lua's object("X", "TrailRenderer") uses it):
    // it must not add a component; addTo is AddComponent
    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    { TrailRenderer handle(&scene, entity); }
    EXPECT_EQ(scene.findComponent<TrailRendererComponent>(entity), nullptr);
    TrailRenderer added = TrailRenderer::addTo(&scene, entity);
    ASSERT_NE(scene.findComponent<TrailRendererComponent>(entity), nullptr);
    added.setTime(0.5f);
    EXPECT_FLOAT_EQ(TrailRenderer::addTo(&scene, entity).getTime(), 0.5f);
    // disabling clears at once
    TrailSimulation::addPosition(scene.getComponent<TrailRendererComponent>(entity), Vector3(1, 0, 0), 0.0f);
    added.setEnabled(false);
    EXPECT_TRUE(scene.getComponent<TrailRendererComponent>(entity).points.empty());
}
