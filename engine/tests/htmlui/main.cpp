// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-htmlui-tests. The engine is initialised here without a GPU, so CPU-only
// suites run without a display; GPU suites call GpuEnvironment::ensure() (JASPER_REQUIRE_GPU)
// and run under Xvfb (tools/build.sh htmlui-tests).

#include "harness/GpuEnvironment.h"

#include <gtest/gtest.h>

#include <cstdlib>

int main(int argc, char** argv){
    ::testing::InitGoogleTest(&argc, argv);

    // the engine's Lua state used off its thread aborts the run (LuaBinding::ThreadCheck)
    setenv("JASPER_LUA_THREAD_CHECK", "abort", 0);

    jasperengine::test::GpuEnvironment::initEngine(argc, argv);
    ::testing::AddGlobalTestEnvironment(new jasperengine::test::GpuTestEnvironment());

    return RUN_ALL_TESTS();
}
