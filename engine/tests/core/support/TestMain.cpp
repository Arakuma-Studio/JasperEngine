// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Engine.h"
#include "jobs/JobSystem.h"

#include "JobsTestSystem.h"
#include "TestPack.h"

#include <gtest/gtest.h>

int main(int argc, char** argv){
    ::testing::InitGoogleTest(&argc, argv);
    // the scheduler's threads exist when death tests fork
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    // the resource pack is opened once, at the first packed lookup: write it before any read
    testsupport::writeTestPack("resources.pak");
    // the engine without a window (its System, the main thread, the process job system)
    jasperengine::Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    const int result = RUN_ALL_TESTS();
    jasperengine::jobs::shutdown();
    return result;
}
