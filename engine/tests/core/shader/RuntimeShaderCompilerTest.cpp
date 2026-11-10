// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime shader compiler across a restart of the process job system, as an engine restart
// does it (jobs::shutdown, then jobs::initialize) and as the tests that change the worker count do.

#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "pool/ShaderPool.h"
#include "shader/RuntimeShaderCompiler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace jasperengine;

namespace {
    // the asset readers take the process job system at start(): restarted with it, or a later
    // test's reads go to the shut-down pool (NoiseTestSupport's WorkerScope)
    void restartJobs(const jobs::Config& config){
        AssetIOService::instance().stop();
        jobs::shutdown();
        jobs::initialize(config);
        AssetIOService::instance().start();
    }
}

// A variant first asked for after the restart compiles on the new pool. Red before the fix: the
// compile queue stayed on the shut-down pool, which cancels what it is given, so the variant was
// building forever (LodFadeGpu.AFadingMeshUnderTiledLightsKeepsItsLighting hung for its tiled
// variant in the whole core binary, after the T2 tests' worker scopes restarted the pool)
TEST(RuntimeShaderCompiler, AVariantAskedForAfterAJobSystemRestartCompiles){
    if (!RuntimeShaderCompiler::isSupported() || !RuntimeShaderCompiler::isEnabled())
        GTEST_SKIP() << "this build compiles no shader at runtime";
    struct Restore{
        bool async = ShaderPool::isAsyncCompilation();
        ~Restore(){
            restartJobs(jobs::Config{});
            ShaderPool::setAsyncCompilation(async);
        }
    } restore;

    // the queue made on the pool running now (shutdown pushes nothing), then that pool replaced
    RuntimeShaderCompiler::shutdown();
    jobs::Config config;
    config.workers = 2;
    restartJobs(config);
    ShaderPool::setAsyncCompilation(true);
    ASSERT_TRUE(ShaderPool::isAsyncCompilation()) << "the compile goes to a worker";

    // not from an earlier run's cache either
    const ShaderType type = ShaderType::POINTS;
    const ShaderKey key = ShaderPool::getShaderKey(type, 0, 0);
    std::error_code ec;
    std::filesystem::remove(RuntimeShaderCompiler::getCacheDirectory() + "/" + ShaderPool::getShaderStr(type, 0, 0) + "_" +
                            ShaderPool::getShaderLangStr() + ".sdat", ec);
    ASSERT_EQ(RuntimeShaderCompiler::request(key).state, ResourceLoadState::Loading) << "queued, not settled where it was asked for";

    // (the deadline is a hang guard: the compile takes a fraction of a second)
    const auto start = std::chrono::steady_clock::now();
    ResourceLoadState state = ResourceLoadState::Loading;
    while (state == ResourceLoadState::Loading && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        state = RuntimeShaderCompiler::request(key).state;
    }
    EXPECT_EQ(state, ResourceLoadState::Finished) << RuntimeShaderCompiler::getError(key);
}
