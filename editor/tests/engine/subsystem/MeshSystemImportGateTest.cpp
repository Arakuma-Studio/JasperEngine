// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model whose FBX import is queued or running waits (spec S98 REQ-33): MeshSystem asks the
// editor's import gate before it loads an FBX model, logs nothing while the import is pending, and
// loads (here: fails as before, the file being absent) once retryFailedModelLoads runs it after the
// import ends. Without a gate (a game) nothing is asked.

#include <gtest/gtest.h>

#include "LogCapture.h"
#include "support/EditorTestSupport.h"

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include <atomic>
#include <string>

using namespace jasperengine;

namespace {

    class MeshSystemImportGate : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            editor::test::ensureEngine();
        }
        // a synchronous load fails at once (an asynchronous one later): the failure is seen in the test
        void SetUp() override {
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
        }
        void TearDown() override {
            MeshSystem::setModelImportGate(nullptr);
            Engine::setAsyncLoading(wasAsync);
        }
        bool wasAsync = false;
    };

}

TEST_F(MeshSystemImportGate, APendingImportWaitsWithoutAnError) {
    std::atomic<bool> pending{true};
    std::atomic<int> asked{0};
    MeshSystem::setModelImportGate([&](const std::string& filename) {
        asked++;
        return pending.load() && filename == "models/importing.fbx";
    });

    Scene scene;
    Model model(&scene);
    ModelComponent& component = model.getComponent<ModelComponent>();
    component.filename = "models/importing.fbx";
    component.needUpdateModel = true;

    jasperengine::test::LogCapture log;
    scene.getSystem<MeshSystem>()->update(0);
    EXPECT_EQ(log.count(D_LOG_ERROR, "Model file not found"), 0u) << log.describeErrors();
    EXPECT_EQ(log.count(D_LOG_ERROR, "not retrying"), 0u) << log.describeErrors();
    EXPECT_FALSE(model.getComponent<ModelComponent>().needUpdateModel) << "it waits for the retry, not every frame";
    EXPECT_GE(asked.load(), 1);

    // the import ended (it failed: no file): the retry loads it, and the failure is reported as before
    pending = false;
    scene.getSystem<MeshSystem>()->retryFailedModelLoads("models/importing.fbx");
    EXPECT_TRUE(model.getComponent<ModelComponent>().needUpdateModel);
    scene.getSystem<MeshSystem>()->update(0);
    EXPECT_EQ(log.count(D_LOG_ERROR, "not retrying"), 1u) << log.describeErrors();
}

TEST_F(MeshSystemImportGate, OnlyFbxModelsAreAsked) {
    std::atomic<int> asked{0};
    MeshSystem::setModelImportGate([&](const std::string&) {
        asked++;
        return true;
    });
    EXPECT_FALSE(MeshSystem::isModelImportPending("models/box.gltf"));
    EXPECT_EQ(asked.load(), 0);
    EXPECT_TRUE(MeshSystem::isModelImportPending("models/Rock.FBX"));
    EXPECT_EQ(asked.load(), 1);

    MeshSystem::setModelImportGate(nullptr);
    EXPECT_FALSE(MeshSystem::isModelImportPending("models/Rock.fbx")) << "no gate: a game asks nothing";
}
