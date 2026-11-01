// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// MeshSystem's async model loads (Engine::isAsyncLoading): a load whose GameObject stopped
// waiting for it (its Model went back to the file it had loaded, as an undone asset move does)
// is collected once it is done, so its "Loading" build closes; the editor's edits wait for open
// builds, and one that never closed held them all.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"
#include "thread/ResourceProgress.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string readFile(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    bool buildOpen(const std::string& name){
        for (const ResourceBuildInfo& build : ResourceProgress::getAllActiveBuilds()){
            if (build.name == name) return true;
        }
        return false;
    }

    struct AsyncLoadingScope{
        explicit AsyncLoadingScope(bool enable): was(Engine::isAsyncLoading()){ Engine::setAsyncLoading(enable); }
        ~AsyncLoadingScope(){ Engine::setAsyncLoading(was); }
        bool was;
    };

}

TEST(AsyncModelLoad, ALoadNoGameObjectWaitsForIsCollectedAndItsBuildCloses){
    TempDir dir;
    const std::string hero = readFile(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/hero_tpose.gltf");
    ASSERT_FALSE(hero.empty());
    const std::string first = dir.write("quad_model.gltf", hero).generic_string();
    const std::string moved = dir.write("walker_model.gltf", hero).generic_string();

    Scene scene;
    Model model(&scene);
    const Entity entity = model.getEntity();
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    ASSERT_TRUE(meshSystem->loadGLTF(entity, first, false, false, false));
    ModelComponent& component = scene.getComponent<ModelComponent>(entity);
    component.needUpdateModel = false;

    AsyncLoadingScope async(true);
    // the Model names another file (a move): its load starts in the background
    component.filename = moved;
    component.needUpdateModel = true;
    meshSystem->update(0);
    ASSERT_TRUE(meshSystem->isAsyncModelLoadPending(entity, moved));
    EXPECT_TRUE(buildOpen("walker_model.gltf"));

    // and the first file again (the move undone): loaded already, nothing waits for the other load
    component.filename = first;
    component.needUpdateModel = true;
    meshSystem->update(0);
    EXPECT_FALSE(component.needUpdateModel) << "the file it has loaded";

    // the frames go on: once the read and parse are done, nobody takes the result
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (meshSystem->isAsyncModelLoadPending(entity, moved) && std::chrono::steady_clock::now() < until){
        meshSystem->update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_FALSE(meshSystem->isAsyncModelLoadPending(entity, moved)) << "the unclaimed load is collected";
    EXPECT_FALSE(buildOpen("walker_model.gltf")) << "its build is closed: the editor's Loading ends";
    EXPECT_FALSE(meshSystem->hasPendingAsyncModelLoads());

    // a GameObject that names the file later loads it again
    component.filename = moved;
    component.needUpdateModel = true;
    for (int i = 0; i < 5000 && component.needUpdateModel; i++){
        meshSystem->update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(component.needUpdateModel);
    EXPECT_EQ(component.loadedFilename, MeshSystem::getModelFilenameKey(moved));
    EXPECT_FALSE(buildOpen("walker_model.gltf"));
}

// a load a GameObject waits for is taken on the frame its result is ready, however long the
// read takes: the collection never takes a result someone polls
TEST(AsyncModelLoad, ALoadAGameObjectWaitsForIsNeverCollected){
    TempDir dir;
    const std::string hero = readFile(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models/hero_tpose.gltf");
    const std::string file = dir.write("waited_model.gltf", hero).generic_string();

    Scene scene;
    Model model(&scene);
    const Entity entity = model.getEntity();
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    AsyncLoadingScope async(true);
    ModelComponent& component = scene.getComponent<ModelComponent>(entity);
    component.filename = file;
    component.needUpdateModel = true;
    for (int i = 0; i < 5000 && component.needUpdateModel; i++){
        meshSystem->update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(component.needUpdateModel);
    EXPECT_EQ(component.loadedFilename, MeshSystem::getModelFilenameKey(file));
    EXPECT_FALSE(buildOpen("waited_model.gltf"));
}
