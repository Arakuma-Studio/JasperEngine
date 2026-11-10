// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 D-42: this file is written as a game's main.cpp is (tools/test-projects/terrain, engine/project):
// `using namespace jasperengine;` and the objects at file scope, named after their class. An engine
// namespace named like one of them makes every use of that object ambiguous, so this unit stops
// compiling if the terrain code's namespace is ever called `terrain` again.

#include "JasperEngine.h"

#include <gtest/gtest.h>

using namespace jasperengine;

static Terrain* terrain = nullptr;

TEST(TerrainUserCode, AFileScopeTerrainNamedTerrainIsNotAmbiguous) {
    EXPECT_EQ(terrain, nullptr);
}
