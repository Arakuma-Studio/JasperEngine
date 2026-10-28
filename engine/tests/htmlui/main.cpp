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

    const int result = RUN_ALL_TESTS();
    // the engine shuts down after every global environment's TearDown (some still read files
    // through the System, which Engine::systemShutdown clears) and before the statics are
    // destroyed, as a game quits
    jasperengine::test::GpuEnvironment::shutdown();
    return result;
}
