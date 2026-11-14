// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A new engine run in one process (a recreated Android activity: the job system shuts down and a
// new one starts) still compiles shader variants. The compile queue was bound to the job system
// of the first run: a push to it after the relaunch was cancelled, so a variant first asked for
// then stayed Loading for ever. Title Art's programs are asked for frame after frame, so a title
// waited for its shaders on Android for good (S81's render gate, Deviation 37).

#include "../render/TitleRenderTestSupport.h"

#include "jobs/JobSystem.h"
#include "pool/ShaderPool.h"
#include "shader/RuntimeShaderCompiler.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace jasperengine;

namespace {

    ResourceLoadState settle(ShaderKey key){
        const auto start = std::chrono::steady_clock::now();
        ResourceLoadState state = ResourceLoadState::Loading;
        while (state == ResourceLoadState::Loading && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)){
            RuntimeShaderCompiler::beginFrame();
            state = RuntimeShaderCompiler::request(key).state;
            if (state == ResourceLoadState::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return state;
    }

}

TEST(RuntimeShaderCompilerRelaunchGpu, AVariantFirstAskedAfterAnEngineRelaunchCompiles) {
    JASPER_REQUIRE_GPU();
    if (!RuntimeShaderCompiler::isSupported()) GTEST_SKIP() << "this build has no runtime compiler";
    const bool enabled = RuntimeShaderCompiler::isEnabled(), async = RuntimeShaderCompiler::isAsync();
    RuntimeShaderCompiler::setEnabled(true);
    RuntimeShaderCompiler::setAsync(true);
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::TITLE_ART, 36);
    std::error_code ec;
    std::filesystem::remove_all(RuntimeShaderCompiler::getCacheDirectory(), ec);
    EXPECT_EQ(settle(key), ResourceLoadState::Finished) << "the first engine run compiles it";
    // what a recreated Android activity does: the runtime compiler and the job system shut down,
    // a new job system starts; the variant is not on disk, so it must compile
    RuntimeShaderCompiler::shutdown();
    std::filesystem::remove_all(RuntimeShaderCompiler::getCacheDirectory(), ec);
    jobs::shutdown();
    jobs::initialize(jobs::Config{});
    EXPECT_EQ(settle(key), ResourceLoadState::Finished) << "the relaunched engine compiles it too";
    RuntimeShaderCompiler::shutdown();
    RuntimeShaderCompiler::setEnabled(enabled);
    RuntimeShaderCompiler::setAsync(async);
}
