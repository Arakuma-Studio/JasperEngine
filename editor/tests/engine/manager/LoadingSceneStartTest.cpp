// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The loading scene covers the start scene too (export-pipeline spec 6.8, Doriax 8b5625da4): the
// game's first loadScene, made before any frame, waits for the first frame, where the loading scene
// is shown, and the start scene loads under it. The scenes are built by SceneManager factories, the
// way the generated main of an exported game builds them.

#include "ScriptTestHarness.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class LoadingSceneStartTest : public ScriptTest {
    protected:
        Scene* start = nullptr;
        Scene* loading = nullptr;

        void registerStartAndLoading() {
            loading = newScene();
            SceneManager::registerScene(1, "Start", [this]() {
                start = newScene();
                Engine::setScene(start);
            });
            SceneManager::registerScene(2, "Loading", []() {}, [this]() { SceneManager::setScenePtr(2, loading); }, {2});
            SceneManager::setLoadingDelay(0.0f);
            // these frames do not draw: with no timeout, readiness does not wait on what a drawn
            // frame would load
            SceneManager::setLoadingTimeout(0.0f);
        }

        // frames until the start scene is the main one (false after `frames`)
        bool runUntilStarted(int frames) {
            // (before its factory ran, start and the main scene are both null)
            for (int i = 0; i < frames && (!start || Engine::getMainScene() != start); i++) frame();
            return start && Engine::getMainScene() == start;
        }
    };

}

TEST_F(LoadingSceneStartTest, CoversTheStartScene) {
    registerStartAndLoading();
    ASSERT_TRUE(SceneManager::setLoadingScene("Loading"));
    ASSERT_TRUE(SceneManager::loadScene("Start"));
    EXPECT_EQ(start, nullptr) << "the start scene waits for the first frame";
    EXPECT_TRUE(SceneManager::isLoadPending());

    frame();
    const std::vector<Scene*> covered = Engine::getSceneLayers();
    ASSERT_FALSE(covered.empty());
    EXPECT_EQ(covered.back(), loading) << "the loading scene is shown first";
    EXPECT_EQ(Engine::getBaseScene(), loading) << "with no main scene yet it draws as the base (it clears the frame)";

    ASSERT_TRUE(runUntilStarted(60));
    for (int i = 0; i < 60 && SceneManager::isLoading(); i++) frame();
    EXPECT_EQ(Engine::getBaseScene(), start);
    const std::vector<Scene*> layers = Engine::getSceneLayers();
    EXPECT_EQ(std::find(layers.begin(), layers.end(), loading), layers.end()) << "the cover goes once the start scene has loaded";
}

TEST_F(LoadingSceneStartTest, NoLoadingSceneStartsAtOnce) {
    registerStartAndLoading();
    ASSERT_TRUE(SceneManager::loadScene("Start"));
    EXPECT_NE(start, nullptr) << "no frame spent before the start";
    EXPECT_EQ(Engine::getMainScene(), start);
    EXPECT_FALSE(SceneManager::isLoadPending());
}

TEST_F(LoadingSceneStartTest, StartStackWithTheLoadingSceneIsNotCovered) {
    registerStartAndLoading();
    // the start scene is the loading scene itself: nothing to cover it with
    ASSERT_TRUE(SceneManager::setLoadingScene("Start"));
    ASSERT_TRUE(SceneManager::loadScene("Start"));
    EXPECT_EQ(Engine::getMainScene(), start);
}

TEST_F(LoadingSceneStartTest, NoErrorsWhileCovering) {
    registerStartAndLoading();
    ASSERT_TRUE(SceneManager::setLoadingScene("Loading"));
    ASSERT_TRUE(SceneManager::loadScene("Start"));
    ASSERT_TRUE(runUntilStarted(60));
    EXPECT_FALSE(logged("rror")) << "frames without a main scene log nothing";
}
