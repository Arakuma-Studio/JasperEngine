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
#include "animation/AnimationClipIO.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimationClipPoolTesting.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
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
    EXPECT_GT(AnimationClipPool::getGeneration(absolute), 0u);
    EXPECT_EQ(AnimationClipPool::getGeneration(p("walk.anim")), AnimationClipPool::getGeneration(absolute));
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
        uint64_t revision = AnimationClipPool::revision();
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
        EXPECT_EQ(AnimationClipPool::revision(), revision + 1) << "one load cached, one bump";
        EXPECT_GT(AnimationClipPool::getGeneration(relative[0]), 0u);
    }

    std::atomic<bool> failed(false);
    std::mutex whyMutex;
    std::string why;
    const auto fail = [&](const std::string& reason){
        failed = true;
        std::lock_guard<std::mutex> lock(whyMutex);
        if (why.empty()) why = reason;
    };
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
                            if (f != 3 && x != (float)(f + 1)) fail("file " + std::to_string(f) + " read as " + std::to_string(x));
                            if (f == 3 && x != 4.0f && x != 40.0f) fail("the rewritten file read as " + std::to_string(x));
                        }else if (f != 3){
                            fail("file " + std::to_string(f) + " did not load: " + error);
                        }
                        break;
                    }
                    case 2:{
                        std::shared_ptr<const AnimationClip> clip = AnimationClipPool::get(path);
                        if (clip && endX(clip) < 0) fail("a cached clip of file " + std::to_string(f) + " is partial");
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
                if (generation < lastGeneration[f])
                    fail("file " + std::to_string(f) + "'s generation went from " + std::to_string(lastGeneration[f]) + " back to " + std::to_string(generation));
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
    EXPECT_FALSE(failed.load()) << why;
    EXPECT_GT(loads.load(), 0);

    // settled: every file loads with its final content
    for (int f = 0; f < files; f++){
        EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(relative[f])), (float)(f + 1)) << f;
    }
}

// Exact in-flight check: an invalidate between a load and its insert keeps the (stale) result out
// of the cache, and the caller still gets it
TEST_F(ClipPool, InvalidateDuringAnInFlightLoadIsNotCached){
    std::string absolute = animtest::writeAsset(p("flight.anim"), clipText(1));
    bool fired = false;
    AnimationClipPoolTestHooks hooks;
    hooks.beforeInsert = [&](const std::string& key){
        if (fired) return;
        fired = true;
        EXPECT_EQ(key, absolute);
        animtest::writeAsset(p("flight.anim"), clipText(2));
        AnimationClipPool::invalidate(absolute);
    };
    setAnimationClipPoolTestHooks(hooks);
    std::shared_ptr<const AnimationClip> first = AnimationClipPool::getOrLoad(p("flight.anim"));
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    ASSERT_TRUE(fired);
    EXPECT_FLOAT_EQ(endX(first), 1.0f) << "the caller gets what it loaded";
    EXPECT_TRUE(AnimationClipPool::get(p("flight.anim")) == nullptr) << "not cached";
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(p("flight.anim"))), 2.0f);

    // a clear() in flight too
    fired = false;
    hooks.beforeInsert = [&](const std::string&){ fired = true; AnimationClipPool::clear(); };
    AnimationClipPool::invalidate(absolute);
    setAnimationClipPoolTestHooks(hooks);
    ASSERT_TRUE(AnimationClipPool::getOrLoad(p("flight.anim")));
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    EXPECT_TRUE(fired);
    EXPECT_EQ(AnimationClipPool::size(), 0u);
}

namespace {

    struct AsyncLoadingOn{
        AsyncLoadingOn(): was(Engine::isAsyncLoading()){ Engine::setAsyncLoading(true); }
        ~AsyncLoadingOn(){
            Engine::setAsyncLoading(was);
            setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
        }
        bool was;
    };

    // over pollOrLoad's limit for a load in the call, and the same clip
    std::string largeClipText(float x){
        return clipText(x) + std::string(70000, ' ');
    }

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

// AC7 (R16): in async loading a large .anim parses on a worker: the caller gets `loading`, then the same
// clip as a load on this thread, and the same error for a file that does not load
TEST_F(ClipPool, PollOrLoadParsesAnimOnAWorker){
    animtest::writeAsset(p("async.anim"), largeClipText(3));
    std::shared_ptr<const AnimationClip> sync = AnimationClipPool::getOrLoad(p("async.anim"));
    ASSERT_TRUE(sync);
    const std::string syncBytes = anim::writeClip(sync->toData());
    const std::string syncWarnings = std::to_string(sync->getWarnings().size());
    sync.reset();
    AnimationClipPool::clear();

    AsyncLoadingOn async;
    std::atomic<bool> release{false};
    std::thread::id builtOn;
    AnimationClipPoolTestHooks hooks;
    hooks.beforeInsert = [&](const std::string&){
        builtOn = std::this_thread::get_id();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!release && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    setAnimationClipPoolTestHooks(hooks);
    bool loading = false;
    std::string error;
    EXPECT_FALSE(AnimationClipPool::pollOrLoad(p("async.anim"), loading, &error));
    EXPECT_TRUE(loading) << "the call does not parse";
    EXPECT_FALSE(AnimationClipPool::pollOrLoad(p("async.anim"), loading, &error));
    EXPECT_TRUE(loading) << "one job, polled";
    release = true;
    std::shared_ptr<const AnimationClip> clip = pollUntilDone(p("async.anim"), &error);
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    ASSERT_TRUE(clip) << error;
    EXPECT_NE(builtOn, std::this_thread::get_id()) << "parsed on a worker";
    EXPECT_EQ(anim::writeClip(clip->toData()), syncBytes) << "the same clip as on this thread";
    EXPECT_EQ(std::to_string(clip->getWarnings().size()), syncWarnings);
    EXPECT_EQ(AnimationClipPool::get(p("async.anim")), clip) << "cached";
    EXPECT_GT(AnimationClipPool::getGeneration(p("async.anim")), 0u);

    // the same failure as on this thread, and it is not cached
    animtest::writeAsset(p("broken.anim"), "{ not json" + std::string(70000, ' '));
    std::string syncError;
    Engine::setAsyncLoading(false);
    EXPECT_FALSE(AnimationClipPool::getOrLoad(p("broken.anim"), &syncError));
    Engine::setAsyncLoading(true);
    std::string asyncError;
    EXPECT_FALSE(pollUntilDone(p("broken.anim"), &asyncError));
    EXPECT_EQ(asyncError, syncError);
    EXPECT_FALSE(syncError.empty());
    animtest::writeAsset(p("broken.anim"), largeClipText(4));
    EXPECT_TRUE(pollUntilDone(p("broken.anim")));

    // a small file and a missing one: in the call, with no job and no frame's wait
    animtest::writeAsset(p("small.anim"), clipText(6));
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::pollOrLoad(p("small.anim"), loading)), 6.0f);
    EXPECT_FALSE(loading);
    std::string missing;
    EXPECT_FALSE(AnimationClipPool::pollOrLoad(p("missing.anim"), loading, &missing));
    EXPECT_FALSE(loading);
    EXPECT_FALSE(missing.empty());
}

// AC7 (R16): no threads (web without pthreads): the same clip in the call, on the calling thread
TEST_F(ClipPool, PollOrLoadWithoutThreadsLoadsInTheCall){
    animtest::writeAsset(p("nothreads.anim"), largeClipText(5));
    AsyncLoadingOn async;
    std::thread::id builtOn;
    AnimationClipPoolTestHooks hooks;
    hooks.singleThreaded = [](){ return true; };
    hooks.beforeInsert = [&](const std::string&){ builtOn = std::this_thread::get_id(); };
    setAnimationClipPoolTestHooks(hooks);
    bool loading = true;
    std::shared_ptr<const AnimationClip> clip = AnimationClipPool::pollOrLoad(p("nothreads.anim"), loading);
    EXPECT_FALSE(loading);
    ASSERT_TRUE(clip);
    EXPECT_EQ(builtOn, std::this_thread::get_id());
    EXPECT_FLOAT_EQ(endX(clip), 5.0f);
    std::string error;
    EXPECT_FALSE(AnimationClipPool::pollOrLoad(p("gone.anim"), loading, &error));
    EXPECT_FALSE(loading);
    EXPECT_FALSE(error.empty());

    // without async loading, threads or not, it is the call too
    Engine::setAsyncLoading(false);
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    AnimationClipPool::clear();
    EXPECT_TRUE(AnimationClipPool::pollOrLoad(p("nothreads.anim"), loading));
    EXPECT_FALSE(loading);
}

// R16: an invalidate during a worker's parse is honoured like one during a call, and the handle
// of a finished job does not keep a clip alive that its users released
TEST_F(ClipPool, PollOrLoadInvalidateAndRelease){
    animtest::writeAsset(p("flight.anim"), largeClipText(1));
    AsyncLoadingOn async;
    std::atomic<int> stage{0};
    AnimationClipPoolTestHooks hooks;
    hooks.beforeInsert = [&](const std::string&){
        if (stage == 0){
            stage = 1;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (stage == 1 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    setAnimationClipPoolTestHooks(hooks);
    bool loading = false;
    EXPECT_FALSE(AnimationClipPool::pollOrLoad(p("flight.anim"), loading));
    EXPECT_TRUE(loading);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (stage == 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(stage, 1) << "the worker parsed";
    animtest::writeAsset(p("flight.anim"), largeClipText(2));
    AnimationClipPool::invalidate(p("flight.anim"));
    stage = 2;
    std::shared_ptr<const AnimationClip> clip = pollUntilDone(p("flight.anim"));
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
    ASSERT_TRUE(clip);
    EXPECT_FLOAT_EQ(endX(clip), 2.0f) << "the file after the invalidate";

    std::weak_ptr<const AnimationClip> weak = clip;
    clip.reset();
    // the finished job's own references go when it returns
    const auto freed = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!weak.expired() && std::chrono::steady_clock::now() < freed){
        AnimationClipPool::clearUnused();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(weak.expired()) << "nothing but the pool held it";
}

// Generations come from one counter: no two (key, version) pairs share a number
TEST_F(ClipPool, GenerationsAreUniqueAcrossKeys){
    std::set<uint64_t> seen;
    std::vector<std::string> paths;
    for (int i = 0; i < 3; i++){
        paths.push_back(p("gen" + std::to_string(i) + ".anim"));
        animtest::writeAsset(paths.back(), clipText((float)i));
    }
    for (int round = 0; round < 5; round++){
        for (const std::string& path : paths){
            ASSERT_TRUE(AnimationClipPool::getOrLoad(path));
            uint64_t loaded = AnimationClipPool::getGeneration(path);
            EXPECT_TRUE(seen.insert(loaded).second) << path << " " << loaded;
            AnimationClipPool::invalidate(path);
            uint64_t invalidated = AnimationClipPool::getGeneration(path);
            EXPECT_GT(invalidated, loaded);
            EXPECT_TRUE(seen.insert(invalidated).second) << path << " " << invalidated;
        }
    }
}

namespace {

    // A fake model for the sub-clip branch: the animation names in index order, and the clip of
    // each name ends at x = its "length"
    struct FakeModel{
        std::vector<std::string> names;
        std::map<std::string, float> ends;
        int builds = 0;
        std::function<void()> duringBuild;
    };

    AnimationClipPoolTestHooks fakeModelHooks(FakeModel& model){
        AnimationClipPoolTestHooks hooks;
        hooks.listModelClips = [&model](const std::string&, std::vector<anim::ModelClipInfo>& out, std::string*){
            out.clear();
            for (size_t i = 0; i < model.names.size(); i++){
                anim::ModelClipInfo info;
                info.index = (int)i;
                info.name = model.names[i];
                out.push_back(info);
            }
            return true;
        };
        hooks.buildSubClip = [&model](const std::string& reference, ClipData& out, std::vector<ClipWarning>&, std::string* error){
            anim::SubClipReference parsed;
            if (!anim::parseSubClipReference(reference, parsed) || !model.ends.count(parsed.clip)){
                if (error) *error = "no animation " + reference;
                return false;
            }
            model.builds++;
            if (model.duringBuild) model.duringBuild();
            out = ClipData();
            ClipTrackData track;
            track.channel = ClipChannel::Position;
            ClipKey a;
            a.value = {0, 0, 0};
            ClipKey b;
            b.time = 1;
            b.value = {model.ends[parsed.clip], 0, 0};
            track.keys = {a, b};
            out.tracks = {track};
            return true;
        };
        return hooks;
    }

}

TEST_F(ClipPool, SubClipAliasesShareOneEntry){
    FakeModel model;
    model.names = {"Walk", "3", "Run"};
    model.ends = {{"Walk", 1}, {"3", 2}, {"Run", 3}};
    setAnimationClipPoolTestHooks(fakeModelHooks(model));
    std::string hero = p("hero.gltf");
    EXPECT_TRUE(AnimationClipPool::get(hero + "#Walk") == nullptr) << "aliases unknown before the model's first load";
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk");
    ASSERT_TRUE(walk);
    EXPECT_FLOAT_EQ(endX(walk), 1.0f);
    EXPECT_EQ(AnimationClipPool::getOrLoad(hero + "#0"), walk) << "index and name: one entry";
    EXPECT_EQ(AnimationClipPool::get("./" + hero + "#Walk"), walk);
    EXPECT_EQ(AnimationClipPool::getGeneration(hero + "#0"), AnimationClipPool::getGeneration(hero + "#Walk"));
    // "3" is a name here, so it is animation 1, not index 3
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(hero + "#3")), 2.0f);
    EXPECT_EQ(model.builds, 2);
    std::string error;
    EXPECT_TRUE(AnimationClipPool::getOrLoad(hero + "#Jump", &error) == nullptr);
    EXPECT_NE(error.find("Jump"), std::string::npos) << error;
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
}

// A reimport that reorders the animations: Walk moves from index 0 to index 1, whose entry (Run)
// a player also used. The generation a player stored for Walk must not come back.
TEST_F(ClipPool, ReorderAfterReimportRebindsAliasesAndChangesGenerations){
    FakeModel model;
    model.names = {"Walk", "Run"};
    model.ends = {{"Walk", 1}, {"Run", 2}};
    setAnimationClipPoolTestHooks(fakeModelHooks(model));
    std::string hero = p("hero.gltf");
    std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad(hero + "#Walk");
    std::shared_ptr<const AnimationClip> run = AnimationClipPool::getOrLoad(hero + "#Run");
    ASSERT_TRUE(walk && run);
    uint64_t walkGeneration = AnimationClipPool::getGeneration(hero + "#Walk");
    uint64_t runGeneration = AnimationClipPool::getGeneration(hero + "#Run");

    model.names = {"Run", "Walk"};
    model.ends = {{"Walk", 10}, {"Run", 20}};
    uint64_t revision = AnimationClipPool::revision();
    AnimationClipPool::invalidate(hero);   // a model path: invalidateModel
    EXPECT_GT(AnimationClipPool::revision(), revision);
    EXPECT_TRUE(AnimationClipPool::get(hero + "#Walk") == nullptr);
    EXPECT_EQ(AnimationClipPool::getGeneration(hero + "#Walk"), 0u) << "the alias table went with the model";

    std::shared_ptr<const AnimationClip> walk2 = AnimationClipPool::getOrLoad(hero + "#Walk");
    ASSERT_TRUE(walk2);
    EXPECT_FLOAT_EQ(endX(walk2), 10.0f);
    EXPECT_EQ(AnimationClipPool::getOrLoad(hero + "#1"), walk2) << "Walk is animation 1 now";
    uint64_t walk2Generation = AnimationClipPool::getGeneration(hero + "#Walk");
    EXPECT_NE(walk2Generation, walkGeneration);
    EXPECT_NE(walk2Generation, runGeneration);
    EXPECT_GT(walk2Generation, std::max(walkGeneration, runGeneration));
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(hero + "#Run")), 20.0f);
    EXPECT_FLOAT_EQ(endX(walk), 1.0f) << "held clips stay valid";
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
}

// invalidateModel while a sub-clip builds: the result resolved through the old alias table is
// returned but neither it nor the old table is cached
TEST_F(ClipPool, InvalidateModelDuringASubClipBuildIsNotCached){
    FakeModel model;
    model.names = {"Walk", "Run"};
    model.ends = {{"Walk", 1}, {"Run", 2}};
    std::string hero = p("hero.gltf");
    model.duringBuild = [&](){
        model.duringBuild = nullptr;
        model.names = {"Run", "Walk"};
        model.ends = {{"Walk", 10}, {"Run", 20}};
        AnimationClipPool::invalidateModel(hero);
    };
    setAnimationClipPoolTestHooks(fakeModelHooks(model));
    std::shared_ptr<const AnimationClip> stale = AnimationClipPool::getOrLoad(hero + "#Walk");
    ASSERT_TRUE(stale);
    EXPECT_EQ(AnimationClipPool::size(), 0u);
    EXPECT_TRUE(AnimationClipPool::get(hero + "#0") == nullptr);
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::getOrLoad(hero + "#Walk")), 10.0f);
    EXPECT_FLOAT_EQ(endX(AnimationClipPool::get(hero + "#1")), 10.0f);
    setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
}
