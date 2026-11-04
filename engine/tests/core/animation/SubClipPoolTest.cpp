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

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimationClipPoolTesting.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"
#include "tiny_gltf.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    std::string readFile(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string heroText(){
        return readFile(std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/hero_animated.gltf");
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

namespace {

    std::string decodeBase64(const std::string& text){
        static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        // (unsigned, the bits not yet emitted only: a signed accumulator overflowed)
        uint32_t value = 0;
        int bits = -8;
        for (char c : text){
            const size_t digit = alphabet.find(c);
            if (digit == std::string::npos) break;
            value = ((value << 6) | (uint32_t)digit) & 0xFFFFFFu;
            bits += 6;
            if (bits >= 0){
                out.push_back((char)((value >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }

}

// FX4 (final fixes review M2, M3): the kept parse holds only the animation data (clips built from
// it equal the full parse's), and is stamped with the files it was read from: rewriting a .gltf's
// external .bin without an invalidate is read again
TEST_F(SubClipPool, KeptParseIsTrimmedAndStampsItsExternalBuffers){
    // the fixture with its buffer in an external .bin
    nlohmann::json gltf = nlohmann::json::parse(heroText());
    const std::string uri = gltf["buffers"][0]["uri"].get<std::string>();
    const std::string bin = decodeBase64(uri.substr(uri.find(',') + 1));
    ASSERT_EQ(bin.size(), gltf["buffers"][0]["byteLength"].get<size_t>());
    const std::string folder = model.substr(0, model.rfind('/') + 1);
    gltf["buffers"][0]["uri"] = "hero.bin";
    const std::string external = folder + "external.gltf";
    animtest::writeAsset(folder + "hero.bin", bin);
    animtest::writeAsset(external, gltf.dump());

    tinygltf::Model full;
    std::string error;
    ASSERT_TRUE(MeshSystem::parseGLTFFile(external, full, &error)) << error;
    const size_t before = getModelFileParseCount();
    for (int index = 0; index < (int)full.animations.size(); index++){
        ClipData fromCache, fromFull;
        std::vector<ClipWarning> warnings;
        std::vector<anim::ModelClipInfo> infos;
        ASSERT_TRUE(anim::listModelFileClips(external, infos, &error)) << error;
        ASSERT_TRUE(anim::buildModelSubClip(external + "#" + infos[index].name, fromCache, warnings, &error)) << error;
        ASSERT_TRUE(anim::buildSubClip(full, index, fromFull, warnings, &error)) << error;
        fromFull.source.path = fromCache.source.path;     // the file form names its model
        EXPECT_EQ(anim::writeClip(fromCache), anim::writeClip(fromFull)) << "animation " << index << " from the trimmed parse";
    }
    EXPECT_EQ(getModelFileParseCount(), before + 1) << "one parse for every list and build";

    // the .bin rewritten, no invalidate: read again. Different bytes and an explicit time (10 s
    // earlier), so coarse file timestamps can't hide the change (FX4b)
    const std::string binPath = animtest::assetDir() + "/" + folder + "hero.bin";
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(binPath);
    std::string changed = bin;
    changed.back() = (char)(changed.back() ^ 1);
    animtest::writeAsset(folder + "hero.bin", changed);
    std::filesystem::last_write_time(binPath, written - std::chrono::seconds(10));
    std::vector<anim::ModelClipInfo> infos;
    ASSERT_TRUE(anim::listModelFileClips(external, infos, &error)) << error;
    EXPECT_EQ(getModelFileParseCount(), before + 2) << "the external buffer's stamp changed";
    ASSERT_TRUE(anim::listModelFileClips(external, infos, &error)) << error;
    EXPECT_EQ(getModelFileParseCount(), before + 2) << "unchanged since: the kept parse";

    // FX4b: an external buffer written after the parse started (its time not before the parse's
    // start: here in the future) keeps that parse out of the cache, so the next read parses again
    animtest::writeAsset(folder + "hero.bin", bin);
    std::filesystem::last_write_time(binPath, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
    ASSERT_TRUE(anim::listModelFileClips(external, infos, &error)) << error;
    EXPECT_EQ(getModelFileParseCount(), before + 3);
    ASSERT_TRUE(anim::listModelFileClips(external, infos, &error)) << error;
    EXPECT_EQ(getModelFileParseCount(), before + 4) << "not kept: read again";
    std::filesystem::last_write_time(binPath, written - std::chrono::seconds(20));
}

// Engine review I1: the sub-clips of a model no scene has loaded (an animation library file) share
// one parse of the file; invalidateModelParse / invalidateModelFile / invalidateModel drop it, so
// the next load reads the changed file once again
TEST_F(SubClipPool, OneParsePerUnpooledModel){
    ASSERT_FALSE(ModelPool::getGLTF(MeshSystem::getModelFilenameKey(model)));
    const size_t before = getModelFileParseCount();
    std::vector<std::shared_ptr<const AnimationClip>> held;
    for (const char* name : {"Walk", "Idle", "Wave", "2"}){
        std::string error;
        held.push_back(AnimationClipPool::getOrLoad(clip(name), &error));
        ASSERT_TRUE(held.back()) << name << ": " << error;
    }
    EXPECT_EQ(getModelFileParseCount(), before + 1) << "the alias table and four clips: one parse";

    // the file changes and the editor drops the parse (entities kept): read again, once
    std::string text = heroText();
    const std::string from = "\"name\": \"Walk\"";
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), "\"name\": \"Stroll\"");
    animtest::writeAsset(model, text);
    MeshSystem::invalidateModelParse(model);
    std::string error;
    EXPECT_FALSE(AnimationClipPool::getOrLoad(clip("Walk"), &error)) << "the changed file was read";
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Stroll"), &error)) << error;
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle"), &error)) << error;
    EXPECT_EQ(getModelFileParseCount(), before + 2);

    MeshSystem::invalidateModelFile(model);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Stroll")));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Wave")));
    EXPECT_EQ(getModelFileParseCount(), before + 3);

    AnimationClipPool::invalidateModel(animtest::assetDir() + "/" + model);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    EXPECT_EQ(getModelFileParseCount(), before + 4) << "the absolute path drops the same parse";

    // another model's drop and a sub-clip invalidate keep it; clear() drops every kept parse
    AnimationClipPool::invalidateModel("models/other/elsewhere.gltf");
    AnimationClipPool::invalidate(clip("Idle"));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    EXPECT_EQ(getModelFileParseCount(), before + 4);
    AnimationClipPool::clear();
    ASSERT_TRUE(AnimationClipPool::getOrLoad(clip("Idle")));
    EXPECT_EQ(getModelFileParseCount(), before + 5);

    // a file written again without an invalidate (its size changed) is not read from the kept parse
    text.replace(text.find("\"name\": \"Stroll\""), std::string("\"name\": \"Stroll\"").size(), "\"name\": \"Amble\"");
    animtest::writeAsset(model, text);
    std::vector<anim::ModelClipInfo> infos;
    ASSERT_TRUE(anim::listModelFileClips(model, infos, &error)) << error;
    ASSERT_FALSE(infos.empty());
    EXPECT_EQ(infos[0].name, "Amble");
    EXPECT_EQ(getModelFileParseCount(), before + 6);
}

namespace {

    // a model loading on another thread: MeshSystem::loadGLTF of a pooled model holds the
    // pooled-glTF mutex for its whole build
    class PooledLockHolder{
    public:
        PooledLockHolder(): thread([this](){
            std::lock_guard<std::recursive_mutex> lock(MeshSystem::getPooledGLTFMutex());
            locked = true;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!released && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }){
            while (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ~PooledLockHolder(){ release(); }
        void release(){
            released = true;
            if (thread.joinable()) thread.join();
        }
    private:
        std::atomic<bool> locked{false};
        std::atomic<bool> released{false};
        std::thread thread;
    };

    // Engine::setAsyncLoading for one test
    struct AsyncLoadingScope{
        explicit AsyncLoadingScope(bool enable): was(Engine::isAsyncLoading()){ Engine::setAsyncLoading(enable); }
        ~AsyncLoadingScope(){ Engine::setAsyncLoading(was); }
        bool was;
    };

    // polls until the build is done (10 s at most)
    std::shared_ptr<const AnimationClip> pollUntilDone(const std::string& path, std::string* error = nullptr){
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool loading = true;
        std::shared_ptr<const AnimationClip> clip;
        while (loading && std::chrono::steady_clock::now() < until){
            clip = AnimationClipPool::pollOrLoad(path, loading, error);
            if (loading) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_FALSE(loading) << path << " still building";
        return clip;
    }

}

// Final review M1: in async loading a sub-clip's first build runs on the thread pool, so a caller
// on the main thread (a player, a scene preload) never waits for the pooled-glTF mutex a model
// loading on another thread holds; small .anim files and the non-async mode load in the call
TEST_F(SubClipPool, PollOrLoadBuildsSubClipsOffTheCallingThread){
    Scene scene;
    Model holder(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(holder.getEntity(), model, false, false, false));
    ASSERT_TRUE(ModelPool::getGLTF(MeshSystem::getModelFilenameKey(model)));
    AsyncLoadingScope async(true);

    std::shared_ptr<const AnimationClip> walk;
    {
        PooledLockHolder loader;
        const auto start = std::chrono::steady_clock::now();
        bool loading = false;
        std::string error;
        EXPECT_FALSE(AnimationClipPool::pollOrLoad(clip("Walk"), loading, &error));
        EXPECT_TRUE(loading) << "the build waits for the lock on the pool's thread";
        EXPECT_FALSE(AnimationClipPool::pollOrLoad(clip("Walk"), loading, &error));
        EXPECT_TRUE(loading) << "polled again: the same build";
        EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 5.0)
            << "the polls waited for the loader";
        EXPECT_FALSE(AnimationClipPool::get(clip("Walk")));
        loader.release();
        std::string pollError;
        walk = pollUntilDone(clip("Walk"), &pollError);
        ASSERT_TRUE(walk) << pollError;
    }
    EXPECT_EQ(walk->getTracks().size(), 8u);
    EXPECT_EQ(AnimationClipPool::get(clip("0")), walk) << "cached as getOrLoad caches it";
    bool loading = true;
    EXPECT_EQ(AnimationClipPool::pollOrLoad(clip("Walk"), loading), walk);
    EXPECT_FALSE(loading) << "cached: no build";

    // a failure comes back from the build with its error, and is not cached
    std::string error;
    EXPECT_FALSE(pollUntilDone(clip("Run"), &error));
    EXPECT_NE(error.find("Run"), std::string::npos) << error;

    // a build in flight when the model is invalidated is forgotten: the next poll builds again
    {
        PooledLockHolder loader;
        EXPECT_FALSE(AnimationClipPool::pollOrLoad(clip("Idle"), loading));
        EXPECT_TRUE(loading);
        AnimationClipPool::invalidateModel(model);
        loader.release();
        std::shared_ptr<const AnimationClip> idle = pollUntilDone(clip("Idle"), &error);
        ASSERT_TRUE(idle) << error;
        EXPECT_EQ(idle->getSource().name, "Idle");
    }

    // a small .anim loads in the call, async or not (a large one: ClipPool)
    animtest::writeAsset("models/async.anim", "{\"format\": \"jasperengine.anim\", \"version\": 1, \"tracks\": []}");
    {
        PooledLockHolder loader;
        EXPECT_TRUE(AnimationClipPool::pollOrLoad("models/async.anim", loading));
        EXPECT_FALSE(loading);
    }
    // without async loading a sub-clip builds in the call too
    AnimationClipPool::clear();
    Engine::setAsyncLoading(false);
    EXPECT_TRUE(AnimationClipPool::pollOrLoad(clip("Wave"), loading));
    EXPECT_FALSE(loading);
}
