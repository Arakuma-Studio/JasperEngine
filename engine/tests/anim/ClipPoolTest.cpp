// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AnimationClipPool (spec 3.1, 9.R12/R13): one key for the asset-relative and the absolute form
// of a file (so the editor document's absolutePath() invalidates what a player loaded through the
// component path), generations and revision, failures not cached, clearUnused / releaseUnused,
// invalidateModel, the Engine pool hooks, and threads hammering get / getOrLoad / invalidate.
// Resource packs: Data::open reads them only outside the editor build (ResourcePack.cpp,
// JASPERENGINE_EDITOR), so the pack path is not exercised here.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

#include "Engine.h"
#include "animation/AnimationClip.h"
#include "pool/AnimationClipPool.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    std::string clipText(float x, const std::string& extra = ""){
        return "{\"format\": \"jasperengine.anim\", \"version\": 1, " + extra + "\"tracks\": [{\"path\": \"\", \"channel\": \"position\", "
               "\"keys\": [[0, [0, 0, 0]], [1, [" + std::to_string(x) + ", 0, 0]]]}]}";
    }

    float endX(const std::shared_ptr<const AnimationClip>& clip){
        if (!clip || clip->getTracks().empty() || clip->getTracks()[0].keyCount < 2) return -1.0f;
        return clip->getTracks()[0].vectors[1].x;
    }

    // each test (and each --gtest_repeat run) gets fresh file names: generations outlive clear()
    class ClipPool : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "anim/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
        }
        void TearDown() override { AnimationClipPool::clear(); }

        // the asset-relative path of a test file
        std::string p(const std::string& name) const { return prefix + name; }

        std::string prefix;
    };

}

TEST_F(ClipPool, AssetRelativeAndAbsolutePathsShareOneKey){
    std::string absolute = animtest::writeAsset(p("walk.anim"), clipText(1));
    EXPECT_EQ(AnimationClipPool::getKey(p("walk.anim")), absolute);
    EXPECT_EQ(AnimationClipPool::getKey(absolute), absolute);
    EXPECT_EQ(AnimationClipPool::getKey("anim/../anim/./" + p("walk.anim").substr(5)), absolute);
    EXPECT_EQ(AnimationClipPool::getKey("asset://" + p("walk.anim")), absolute);
    // sub-clip references normalize the model part and keep the clip part
    EXPECT_EQ(AnimationClipPool::getKey("models/./hero.gltf#Walk"), animtest::assetDir() + "/models/hero.gltf#Walk");

    EXPECT_TRUE(AnimationClipPool::get(p("walk.anim")) == nullptr) << "get never loads";
    EXPECT_EQ(AnimationClipPool::getGeneration(p("walk.anim")), 0u);
    std::string error;
    std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(p("walk.anim"), &error);
    ASSERT_TRUE(clip) << error;
    EXPECT_FLOAT_EQ(endX(clip), 1.0f);
    EXPECT_EQ(AnimationClipPool::getOrLoad(absolute), clip);
    EXPECT_EQ(AnimationClipPool::get(absolute), clip);
    EXPECT_EQ(AnimationClipPool::size(), 1u);
    EXPECT_EQ(AnimationClipPool::getGeneration(absolute), 1u);
}

// The contract test: the document invalidates with absolutePath(), the player loaded through the
// component's asset-relative path and sees the new generation, re-fetching the edited file
TEST_F(ClipPool, InvalidateThroughTheAbsolutePathReachesTheComponentPath){
    std::string absolute = animtest::writeAsset(p("run.anim"), clipText(2));
    std::shared_ptr<const AnimationClip> held = AnimationClipPool::getOrLoad(p("run.anim"));
    ASSERT_TRUE(held);
    uint64_t generation = AnimationClipPool::getGeneration(p("run.anim"));
    uint64_t revision = AnimationClipPool::revision();

    animtest::writeAsset(p("run.anim"), clipText(5));
    AnimationClipPool::invalidate(absolute);
    EXPECT_GT(AnimationClipPool::getGeneration(p("run.anim")), generation);
    EXPECT_GT(AnimationClipPool::revision(), revision);
    EXPECT_TRUE(AnimationClipPool::get(p("run.anim")) == nullptr);
    EXPECT_FLOAT_EQ(endX(held), 2.0f) << "a held clip stays valid after invalidate";

    uint64_t invalidated = AnimationClipPool::getGeneration(p("run.anim"));
    std::shared_ptr<const AnimationClip> reloaded = AnimationClipPool::getOrLoad(p("run.anim"));
    ASSERT_TRUE(reloaded);
    EXPECT_NE(reloaded, held);
    EXPECT_FLOAT_EQ(endX(reloaded), 5.0f);
    EXPECT_GT(AnimationClipPool::getGeneration(absolute), invalidated) << "the reload bumps it again";
}

TEST_F(ClipPool, FailuresCarryTheErrorAndAreNotCached){
    std::string error;
    EXPECT_TRUE(AnimationClipPool::getOrLoad(p("missing.anim"), &error) == nullptr);
    EXPECT_NE(error.find("cannot read"), std::string::npos) << error;

    animtest::writeAsset(p("broken.anim"), "{\n  \"format\": \"jasperengine.anim\",\n  \"version\": 1,\n  oops\n}");
    EXPECT_TRUE(AnimationClipPool::getOrLoad(p("broken.anim"), &error) == nullptr);
    EXPECT_EQ(error.rfind("4:3: ", 0), 0u) << error;
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_EQ(AnimationClipPool::getGeneration(p("broken.anim")), 0u);

    // fixed on disk: the next call loads it (no failure was cached)
    animtest::writeAsset(p("broken.anim"), clipText(3));
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(p("broken.anim"), &error)), 3.0f) << error;

    std::string unity = "%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n";
    animtest::writeAsset(p("unity.anim"), unity);
    EXPECT_TRUE(AnimationClipPool::getOrLoad(p("unity.anim"), &error) == nullptr);
    EXPECT_NE(error.find("Unity .anim (YAML)"), std::string::npos) << error;
}

TEST_F(ClipPool, WarningsAreReturnedAndKeptOnTheClip){
    animtest::writeAsset(p("warn.anim"), clipText(1, "\"wrap\": \"bounce\", \"extra\": 1, "));
    std::vector<ClipWarning> warnings;
    std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(p("warn.anim"), nullptr, &warnings);
    ASSERT_TRUE(clip);
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0].path, "wrap");
    EXPECT_EQ(clip->getWarnings().size(), 2u);
    // a cached hit reports the clip's warnings too
    std::vector<ClipWarning> again;
    EXPECT_EQ(AnimationClipPool::getOrLoad(p("warn.anim"), nullptr, &again), clip);
    EXPECT_EQ(again.size(), 2u);
}

TEST_F(ClipPool, ClearUnusedAndReleaseUnusedKeepHeldClips){
    animtest::writeAsset(p("a.anim"), clipText(1));
    animtest::writeAsset(p("b.anim"), clipText(2));
    std::shared_ptr<const AnimationClip> held = AnimationClipPool::getOrLoad(p("a.anim"));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(p("b.anim")));
    EXPECT_EQ(AnimationClipPool::size(), 2u);
    AnimationClipPool::clearUnused();
    EXPECT_EQ(AnimationClipPool::size(), 1u);
    EXPECT_EQ(AnimationClipPool::get(p("a.anim")), held);

    AnimationClipPool::releaseUnused(p("a.anim"));
    EXPECT_EQ(AnimationClipPool::size(), 1u) << "still held";
    held.reset();
    AnimationClipPool::releaseUnused(p("a.anim"));
    EXPECT_EQ(AnimationClipPool::size(), 0u);
}

TEST_F(ClipPool, EnginePoolHooksClearClips){
    animtest::writeAsset(p("hook.anim"), clipText(1));
    std::shared_ptr<const AnimationClip> held = AnimationClipPool::getOrLoad(p("hook.anim"));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(p("a2.anim")) == nullptr);
    animtest::writeAsset(p("a2.anim"), clipText(1));
    ASSERT_TRUE(AnimationClipPool::getOrLoad(p("a2.anim")));
    Engine::clearUnusedPools();
    EXPECT_EQ(AnimationClipPool::size(), 1u);
    uint64_t generation = AnimationClipPool::getGeneration(p("hook.anim"));
    Engine::clearPools();
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_GT(AnimationClipPool::getGeneration(p("hook.anim")), generation) << "generations never go back";
    EXPECT_FLOAT_EQ(endX(held), 1.0f);
}

TEST_F(ClipPool, InvalidateModelKeepsClipFilesAndNeverLoadedAliasesAreNull){
    animtest::writeAsset(p("keep.anim"), clipText(1));
    std::shared_ptr<const AnimationClip> held = AnimationClipPool::getOrLoad(p("keep.anim"));
    uint64_t revision = AnimationClipPool::revision();
    AnimationClipPool::invalidateModel("models/hero.gltf");
    AnimationClipPool::invalidate("models/hero.gltf");   // a model path means its sub-clips
    EXPECT_GT(AnimationClipPool::revision(), revision);
    EXPECT_EQ(AnimationClipPool::get(p("keep.anim")), held);
    // a sub-clip alias of a model never loaded: unknown, so null / 0, and invalidate is a no-op
    EXPECT_TRUE(AnimationClipPool::get("models/hero.gltf#Walk") == nullptr);
    EXPECT_EQ(AnimationClipPool::getGeneration("models/hero.gltf#Walk"), 0u);
    AnimationClipPool::invalidate("models/hero.gltf#Walk");
    EXPECT_EQ(AnimationClipPool::size(), 1u);
}

// Threads hammering one small set of files with getOrLoad / get / invalidate / clearUnused while
// one writer rewrites a file: no crash or torn clip, every clip complete, generations of a key
// never seen going back, and concurrent first loads of one key end with one cached clip
TEST_F(ClipPool, ConcurrentAccess){
    const int files = 4;
    std::vector<std::string> relative;
    std::vector<std::string> absolute;
    for (int i = 0; i < files; i++){
        relative.push_back(p("thread" + std::to_string(i) + ".anim"));
        absolute.push_back(animtest::writeAsset(relative.back(), clipText((float)(i + 1))));
    }

    // first loads racing: everybody gets the cached one
    {
        std::vector<std::shared_ptr<const AnimationClip>> results(8);
        std::vector<std::thread> threads;
        for (size_t t = 0; t < results.size(); t++){
            threads.emplace_back([&, t](){ results[t] = AnimationClipPool::getOrLoad(relative[0]); });
        }
        for (std::thread& thread : threads) thread.join();
        std::shared_ptr<const AnimationClip> cached = AnimationClipPool::get(relative[0]);
        ASSERT_TRUE(cached);
        size_t same = 0;
        for (const auto& result : results){
            ASSERT_TRUE(result);
            if (result == cached) same++;
        }
        // a loser of the race returns the clip cached by the winner
        EXPECT_EQ(same, results.size());
        EXPECT_EQ(AnimationClipPool::getGeneration(relative[0]), 1u) << "one load cached, one bump";
    }

    std::atomic<bool> failed(false);
    std::atomic<int> loads(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; t++){
        threads.emplace_back([&, t](){
            uint64_t lastGeneration[files] = {0, 0, 0, 0};
            for (int i = 0; i < 400; i++){
                int f = (i + t) % files;
                const std::string& path = (i % 2) ? relative[f] : absolute[f];
                switch ((i * 7 + t) % 5){
                    case 0:
                    case 1:{
                        std::string error;
                        std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(path, &error);
                        // file 3 is rewritten by the writer below: a torn read may fail to parse,
                        // never crash or give a partial clip
                        if (clip){
                            loads++;
                            float x = endX(clip);
                            if (f != 3 && x != (float)(f + 1)) failed = true;
                            if (f == 3 && x != 4.0f && x != 40.0f) failed = true;
                        }else if (f != 3){
                            failed = true;
                        }
                        break;
                    }
                    case 2:{
                        std::shared_ptr<const AnimationClip> clip = AnimationClipPool::get(path);
                        if (clip && endX(clip) < 0) failed = true;
                        break;
                    }
                    case 3:
                        AnimationClipPool::invalidate(path);
                        break;
                    case 4:
                        if (i % 20 == 4) AnimationClipPool::clearUnused();
                        break;
                }
                uint64_t generation = AnimationClipPool::getGeneration(path);
                if (generation < lastGeneration[f]) failed = true;
                lastGeneration[f] = generation;
                (void)AnimationClipPool::revision();
                (void)AnimationClipPool::size();
            }
        });
    }
    std::thread writer([&](){
        for (int i = 0; i < 100; i++){
            animtest::writeAsset(relative[3], clipText(i % 2 ? 40.0f : 4.0f));
            AnimationClipPool::invalidate(absolute[3]);
        }
        animtest::writeAsset(relative[3], clipText(4.0f));
        AnimationClipPool::invalidate(absolute[3]);
    });
    for (std::thread& thread : threads) thread.join();
    writer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_GT(loads.load(), 0);

    // settled: every file loads with its final content
    for (int f = 0; f < files; f++){
        EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(relative[f])), (float)(f + 1)) << f;
    }
}
