// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene clock (Scene::getSceneTime) library:water's waves run on, and Water::getTime
// (docs/superpowers/specs/2026-11-04-water-reflections-design.md 6.1): it adds the frame's
// scaled delta, holds while the scene is paused, takes the editor's stopped-scene clock, and a
// pinned shader time (golden tests) replaces it.

#include "Engine.h"
#include "Scene.h"
#include "object/environment/Water.h"

#include <gtest/gtest.h>

#include <limits>

using namespace jasperengine;

TEST(SceneTime, AdvancesByTheDelta){
    Scene scene;
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 0.0);
    scene.advanceSceneTime(0.25);
    scene.advanceSceneTime(0.5);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 0.75);
    // nothing for a held frame (time scale 0), a negative or a non-finite delta
    scene.advanceSceneTime(0.0);
    scene.advanceSceneTime(-1.0);
    scene.advanceSceneTime(std::numeric_limits<double>::infinity());
    scene.advanceSceneTime(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 0.75);
    EXPECT_FLOAT_EQ(Water::getTime(&scene), 0.75f);
}

TEST(SceneTime, HeldWhilePaused){
    Scene scene;
    scene.advanceSceneTime(1.0);
    scene.setSceneTimePaused(true);
    EXPECT_TRUE(scene.isSceneTimePaused());
    scene.advanceSceneTime(1.0);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 1.0) << "the clock moved while the scene was paused";
    // the editor's clock of a stopped scene moves it all the same (Always Refresh)
    scene.advanceSceneTime(0.5, true);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 1.5);
    scene.setSceneTimePaused(false);
    scene.advanceSceneTime(1.0);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 2.5);
}

TEST(SceneTime, PinnedShaderTimeOverrides){
    Scene scene;
    scene.advanceSceneTime(3.0);
    Engine::setPinnedShaderTime(7.25);
    EXPECT_TRUE(Engine::hasPinnedShaderTime());
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 7.25);
    EXPECT_FLOAT_EQ(Water::getTime(&scene), 7.25f);
    Engine::setPinnedShaderTime(-1.0);
    EXPECT_FALSE(Engine::hasPinnedShaderTime());
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 3.0) << "unpinned, the scene's own clock is back";
    scene.setSceneTime(10.0);
    EXPECT_DOUBLE_EQ(scene.getSceneTime(), 10.0);
}
