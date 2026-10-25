// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AnimationClipPool's model sub-clips (spec 3.1, 9.R11/R12/R13) on the animated hero fixture:
// names and indices share one "<model key>#<index>" entry and generation, unknown clips fail
// without caching, MeshSystem::invalidateModelFile (the editor's model reload) and
// AnimationClipPool::invalidate/invalidateModel drop every sub-clip of the file and its alias
// table, a changed file is read again (also when a scene pooled its parse), clearUnused, and
// concurrent first loads.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Scene.h"
#include "animation/AnimationClip.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"

#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    std::string readFile(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string heroText(){
        return readFile(std::string(ANIM_POSE_FIXTURE_MODELS_DIR) + "/hero_animated.gltf");
    }

    // each test (and each --gtest_repeat run) gets its own copy: generations outlive clear()
    class SubClipPool : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            model = "models/pool" + std::to_string(run++) + "/hero_animated.gltf";
            animtest::writeAsset(model, heroText());
            AnimationClipPool::clear();
        }
        void TearDown() override { AnimationClipPool::clear(); }

        std::string clip(const std::string& name) const { return model + "#" + name; }

        std::string model;
    };

}

TEST_F(SubClipPool, NamesAndIndicesShareOneEntry){
    std::string error;
    std::vector<ClipWarning> warnings;
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(clip("Walk"), &error, &warnings);
    ASSERT_TRUE(walk) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(walk->getTracks().size(), 8u);
    EXPECT_NEAR(walk->getLength(), 1.0f, 1e-6f);
    EXPECT_EQ(walk->getWrap(), ClipWrapMode::Once);
    EXPECT_EQ(walk->getFrameRate(), 30.0f);
    EXPECT_EQ(walk->getSource().kind, "model");
    EXPECT_EQ(walk->getSource().name, "Walk");

    EXPECT_EQ(AnimationClipPool::getOrLoad(clip("0")), walk);
    EXPECT_EQ(AnimationClipPool::get(clip("Walk")), walk);
    EXPECT_EQ(AnimationClipPool::get(animtest::assetDir() + "/" + clip("0")), walk) << "the absolute model path";
    const uint64_t generation = AnimationClipPool::getGeneration(clip("Walk"));
    EXPECT_NE(generation, 0u);
    EXPECT_EQ(AnimationClipPool::getGeneration(clip("0")), generation) << "one entry, one generation";
    EXPECT_EQ(AnimationClipPool::size(), 1u);

    std::shared_ptr<const AnimationClip> unnamed = AnimationClipPool::getOrLoad(clip("2"), &error);
    ASSERT_TRUE(unnamed) << error;
    EXPECT_EQ(AnimationClipPool::getOrLoad(clip("Animation 2")), unnamed);
    std::shared_ptr<const AnimationClip> wave = AnimationClipPool::getOrLoad(clip("Wave"));
    std::shared_ptr<const AnimationClip> wave2 = AnimationClipPool::getOrLoad(clip("Wave 2"));
    ASSERT_TRUE(wave && wave2);
    EXPECT_NE(wave, wave2);
    EXPECT_EQ(AnimationClipPool::getOrLoad(clip("4")), wave2);
    EXPECT_EQ(AnimationClipPool::size(), 4u);

    // unknown clips fail and are not cached
    EXPECT_FALSE(AnimationClipPool::getOrLoad(clip("Run"), &error));
    EXPECT_NE(error.find("Run"), std::string::npos) << error;
    EXPECT_FALSE(AnimationClipPool::getOrLoad(clip("5"), &error));
    EXPECT_FALSE(AnimationClipPool::getOrLoad("models/nope.gltf#Walk", &error));
    EXPECT_NE(error.find("nope.gltf"), std::string::npos) << error;
    EXPECT_EQ(AnimationClipPool::size(), 4u);
}

TEST_F(SubClipPool, ModelInvalidationDropsEverySubClip){
    // a scene pools the parse, so the builder reads the pooled model
    Scene scene;
    Model holder(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(holder.getEntity(), model, false, false, false));
    ASSERT_TRUE(ModelPool::getGLTF(MeshSystem::getModelFilenameKey(model)));

    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(clip("Walk"));
    ASSERT_TRUE(walk);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    const uint64_t revision = AnimationClipPool::revision();
    const uint64_t generation = AnimationClipPool::getGeneration(clip("0"));
    EXPECT_NE(generation, 0u);

    // the file changes (Walk renamed) and the editor reloads the model: every sub-clip of it goes
    std::string text = heroText();
    const std::string from = "\"name\": \"Walk\"";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "\"name\": \"Stroll\"");
    animtest::writeAsset(model, text);
    MeshSystem::invalidateModelFile(model);
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_GT(AnimationClipPool::revision(), revision);
    EXPECT_FALSE(AnimationClipPool::get(clip("Walk")));
    EXPECT_EQ(walk->getTracks().size(), 8u) << "a held clip stays valid";

    std::string error;
    EXPECT_FALSE(AnimationClipPool::getOrLoad(clip("Walk"), &error)) << "the alias table was read again";
    std::shared_ptr<const AnimationClip> stroll = AnimationClipPool::getOrLoad(clip("Stroll"), &error);
    ASSERT_TRUE(stroll) << error;
    EXPECT_NE(stroll, walk);
    EXPECT_EQ(stroll->getSource().name, "Stroll");
    EXPECT_EQ(AnimationClipPool::getOrLoad(clip("0")), stroll);
    EXPECT_NE(AnimationClipPool::getGeneration(clip("0")), generation) << "a new generation after the reload";
    EXPECT_NE(AnimationClipPool::getGeneration(clip("0")), 0u);

    // invalidate() of the model file and invalidateModel() of its absolute path do the same
    AnimationClipPool::invalidate(model);
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Stroll")));
    AnimationClipPool::invalidateModel(animtest::assetDir() + "/" + model);
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_FALSE(AnimationClipPool::get(clip("0")));

    // a .anim next to it is not a sub-clip of the model
    animtest::writeAsset("models/keep.anim", "{\"format\": \"jasperengine.anim\", \"version\": 1, \"tracks\": []}");
    ASSERT_TRUE(AnimationClipPool::getOrLoad("models/keep.anim"));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    MeshSystem::invalidateModelFile(model);
    EXPECT_TRUE(AnimationClipPool::get("models/keep.anim"));
    EXPECT_EQ(AnimationClipPool::size(), 1u);
}

TEST_F(SubClipPool, ClearUnusedDropsUnheldSubClipsAndTheirAliases){
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(clip("Walk"));
    ASSERT_TRUE(walk);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    AnimationClipPool::clearUnused();
    EXPECT_EQ(AnimationClipPool::size(), 1u);
    EXPECT_EQ(AnimationClipPool::get(clip("Walk")), walk);
    EXPECT_FALSE(AnimationClipPool::get(clip("Idle")));

    walk.reset();
    AnimationClipPool::clearUnused();
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_EQ(AnimationClipPool::getGeneration(clip("Walk")), 0u) << "the alias table went with the last clip";
    EXPECT_TRUE(AnimationClipPool::getOrLoad(clip("Walk")));
}

TEST_F(SubClipPool, ConcurrentFirstLoadsShareOneClip){
    std::vector<std::shared_ptr<const AnimationClip>> clips(8);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < clips.size(); t++){
        threads.emplace_back([&, t](){
            clips[t] = AnimationClipPool::getOrLoad(clip(t % 2 ? "Idle" : "1"));
        });
    }
    for (std::thread& thread : threads) thread.join();
    ASSERT_TRUE(clips[0]);
    for (const auto& loaded : clips) EXPECT_EQ(loaded, clips[0]);
    const uint64_t generation = AnimationClipPool::getGeneration(clip("Idle"));
    EXPECT_NE(generation, 0u);
    EXPECT_EQ(AnimationClipPool::getGeneration(clip("1")), generation);
    EXPECT_EQ(AnimationClipPool::getOrLoad(clip("Idle")), clips[0]);
    EXPECT_EQ(AnimationClipPool::getGeneration(clip("Idle")), generation) << "a cache hit keeps the generation";
    EXPECT_EQ(AnimationClipPool::size(), 1u);
}
