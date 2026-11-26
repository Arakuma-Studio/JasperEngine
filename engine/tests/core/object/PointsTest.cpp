// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Points sprite frames (docs/specs/S94-rendering-anim-gaps.md F1, F2): their rects are in UV
// units, and a pixel rect (what Sprite:addFrame takes) warns once per Points (the port's R54).
// An old script's Points on the GameObject of a migrated Particle System (the S20 migration
// removed its empty Points) forwards its frame, blend and depth write calls to the effect.

#include "Log.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "object/ParticleSystem.h"
#include "object/Points.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* const kUnitWarning = "addSpriteFrame rect is in UV units (0..1), got";
    const char* const kForwardNote = "ps.textureSheetAnimation";

    // what the S20 migration leaves of an old system and its Points target: a GameObject with a
    // Particle System (an inline effect, no frames) and no PointsComponent
    struct MigratedTarget {
        Scene scene;
        ParticleSystem system{&scene};

        MigratedTarget(){
            system.setPlayOnAwake(false);
            EXPECT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 45,
                "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 0},
                            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 40}]},
                            {"type": "renderer", "texture": "confetti.png", "blend": "alpha"}]})"));
        }
    };

    class PointsTest : public ::testing::Test {};

}

TEST_F(PointsTest, SpriteFrameOutsideUnitWarnsOnce){
    Scene scene;
    LogCapture log;
    Points sheet(&scene);
    sheet.addSpriteFrame(0.0f, 0.0f, 0.25f, 0.5f);
    sheet.addSpriteFrame(Rect(0.75f, 0.5f, 0.25f, 0.5f));
    sheet.addSpriteFrame(Rect(0.0f, 0.0f, 1.0f, 1.0f));
    EXPECT_EQ(log.count(D_LOG_WARN, kUnitWarning), 0u) << "UV rects say nothing";

    Points pixels(&scene);
    pixels.addSpriteFrame(0, "a", Rect(0.0f, 0.0f, 32.0f, 32.0f));
    pixels.addSpriteFrame("b", 32.0f, 0.0f, 32.0f, 32.0f);
    ASSERT_EQ(log.count(D_LOG_WARN, kUnitWarning), 1u) << "two pixel rects, one warning for the Points";
    EXPECT_EQ(log.count(D_LOG_WARN, "got (0, 0, 32, 32)"), 1u) << "it shows the rect it got";
    EXPECT_EQ(pixels.getComponent<PointsComponent>().numFramesRect, 2u) << "the frames are kept as given";

    Points other(&scene);
    other.addSpriteFrame(Rect(64.0f, 0.0f, 32.0f, 32.0f));
    EXPECT_EQ(log.count(D_LOG_WARN, kUnitWarning), 2u) << "another Points warns for itself";
}

TEST_F(PointsTest, FramesOfAnOldTargetBecomeTheEffectSprites){
    // ARCADE scripts/k/KFx.lua: Points(scene, target):addSpriteFrame(i, name, rect) for a 4 x 2
    // sheet, then ParticleSystem:setSpriteIntializer(0, 7)
    MigratedTarget k;
    LogCapture log;
    Points points(&k.scene, k.system.getEntity());
    for (int i = 0; i < 8; i++)
        points.addSpriteFrame(i, "c" + std::to_string(i), Rect((float)(i % 4) * 0.25f, (float)(i / 4) * 0.5f, 0.25f, 0.5f));
    k.system.setSpriteIntializer(0, 7);

    const particles::TextureSheetAnimationModule& sheet = k.system.getEffectDocument().textureSheetAnimation;
    ASSERT_EQ(sheet.sprites.size(), 8u);
    EXPECT_EQ(sheet.sprites[5], (particles::SpriteRect{0.25f, 0.5f, 0.25f, 0.5f}));
    EXPECT_TRUE(sheet.present);
    EXPECT_TRUE(sheet.enabled) << "the start frame is picked from the 8";
    EXPECT_EQ(sheet.mode, particles::TextureSheetMode::Sprites);
    EXPECT_EQ(sheet.startFrameList.size(), 8u);
    EXPECT_FALSE(k.scene.findComponent<PointsComponent>(k.system.getEntity())) << "no Points is made back";
    EXPECT_EQ(log.count(D_LOG_WARN, kForwardNote), 1u) << "one deprecation note for the GameObject";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(PointsTest, FramesBeforeTheInitializerAndRemovals){
    // MFx's order (frames, then setSpriteModifier) and the initializer before the frames
    MigratedTarget m;
    m.system.setSpriteIntializer(0, 2);
    Points points(&m.scene, m.system.getEntity());
    points.addSpriteFrame(0.0f, 0.0f, 0.5f, 0.5f);
    points.addSpriteFrame(0.5f, 0.0f, 0.5f, 0.5f);
    points.addSpriteFrame(Rect(0.0f, 0.5f, 0.5f, 0.5f));
    particles::TextureSheetAnimationModule sheet = m.system.getEffectDocument().textureSheetAnimation;
    ASSERT_EQ(sheet.sprites.size(), 3u) << "appended in order";
    EXPECT_TRUE(sheet.enabled) << "the initializer set before the frames animates them once they exist";
    EXPECT_EQ(sheet.sprites[1], (particles::SpriteRect{0.5f, 0.0f, 0.5f, 0.5f}));

    points.removeSpriteFrame(0);
    sheet = m.system.getEffectDocument().textureSheetAnimation;
    ASSERT_EQ(sheet.sprites.size(), 2u);
    EXPECT_EQ(sheet.sprites[0], (particles::SpriteRect{0.5f, 0.0f, 0.5f, 0.5f}));
    EXPECT_FALSE(sheet.enabled) << "its start list picks frame 2, gone: the effect still compiles";

    LogCapture log;
    points.removeSpriteFrame("c0");
    EXPECT_EQ(m.system.getEffectDocument().textureSheetAnimation.sprites.size(), 2u);
    EXPECT_EQ(log.count(D_LOG_WARN, "have no names"), 1u);

    // an effect's sprites are UV rects (a pixel one fails its validation): refused, said each time
    points.addSpriteFrame(Rect(0.0f, 0.0f, 64.0f, 64.0f));
    points.addSpriteFrame(Rect(64.0f, 0.0f, 64.0f, 64.0f));
    EXPECT_EQ(log.count(D_LOG_ERROR, "its rect is in UV units (0..1), got (64, 0, 64, 64)"), 1u);
    EXPECT_EQ(log.count(D_LOG_ERROR, "not added"), 2u);
    EXPECT_EQ(m.system.getEffectDocument().textureSheetAnimation.sprites.size(), 2u);

    // a rect a rounding past the edge is the edge
    points.addSpriteFrame(Rect(0.5f, 0.5f, 0.5f, 0.5005f));
    sheet = m.system.getEffectDocument().textureSheetAnimation;
    ASSERT_EQ(sheet.sprites.size(), 3u);
    EXPECT_EQ(sheet.sprites[2].height, 0.5005f);
    EXPECT_TRUE(sheet.enabled) << "frames 0 to 2 exist again";
    points.addSpriteFrame(Rect(-0.0005f, 0.0f, 1.0f, 1.0005f));
    EXPECT_EQ(m.system.getEffectDocument().textureSheetAnimation.sprites[3], (particles::SpriteRect{0.0f, 0.0f, 1.0f, 1.0f}));
}

TEST_F(PointsTest, BlendAndDepthWriteOfAnOldTargetAreTheRenderers){
    // ARCADE scripts/i/IFx.lua and b/BCannon.lua: the old target's blend set at run time
    MigratedTarget i;
    Points points(&i.scene, i.system.getEntity());
    points.setBlendMode(MaterialBlendMode::ADDITIVE);
    points.setDepthWrite(MaterialDepthWrite::DISABLED);
    EXPECT_EQ(i.system.getEffectDocument().renderer.blend, particles::ParticleBlend::Additive);
    EXPECT_FALSE(i.system.getEffectDocument().renderer.depthWrite);
    EXPECT_EQ(points.getBlendMode(), MaterialBlendMode::ADDITIVE);
    EXPECT_EQ(points.getDepthWrite(), MaterialDepthWrite::DISABLED);
    points.setDepthWrite(MaterialDepthWrite::ENABLED);
    EXPECT_TRUE(i.system.getEffectDocument().renderer.depthWrite);
    EXPECT_EQ(i.system.getEffectDocument().renderer.texture, "confetti.png") << "the rest of the Renderer stays";
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(PointsTest, OwnPointsDoNotForward){
    // a GameObject with both keeps its Points' own frames (the migration kept a Points with points of its own)
    MigratedTarget both;
    both.scene.addComponent<PointsComponent>(both.system.getEntity(), {});
    LogCapture log;
    Points points(&both.scene, both.system.getEntity());
    points.addSpriteFrame(0.0f, 0.0f, 0.5f, 0.5f);
    points.setBlendMode(MaterialBlendMode::ADDITIVE);
    EXPECT_EQ(points.getComponent<PointsComponent>().numFramesRect, 1u);
    EXPECT_EQ(points.getComponent<PointsComponent>().blendMode, MaterialBlendMode::ADDITIVE);
    EXPECT_TRUE(both.system.getEffectDocument().textureSheetAnimation.sprites.empty());
    EXPECT_EQ(both.system.getEffectDocument().renderer.blend, particles::ParticleBlend::Alpha);
    EXPECT_EQ(log.count(D_LOG_WARN, kForwardNote), 0u);
}
