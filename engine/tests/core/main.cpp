// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-core-tests. The engine is initialised once without a GPU (no view is loaded),
// so scenes, their systems and the Lua state are available to every suite.

#include "TestSystem.h"
#include "Engine.h"

#include <gtest/gtest.h>

int main(int argc, char** argv){
    ::testing::InitGoogleTest(&argc, argv);

    // owned by the engine for the whole process, like the platform backends' System
    jasperengine::test::TestSystem* system = new jasperengine::test::TestSystem();
    jasperengine::Engine::systemInit(argc, argv, system);

    return RUN_ALL_TESTS();
}
