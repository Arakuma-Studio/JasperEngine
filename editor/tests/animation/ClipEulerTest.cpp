// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .anim {"euler": [x, y, z]} keys and edit_animation's euler rotations are one convention:
// anim::eulerDegreesToQuaternion (the clip parser) equals McpValues::parseQuaternion in the
// editor's degrees mode, and the clip parser stays in degrees when the engine uses radians.

#include <gtest/gtest.h>

#include "Engine.h"
#include "animation/AnimationClipIO.h"
#include "mcp/McpValues.h"

#include <cmath>

using namespace jasperengine;

TEST(ClipEuler, MatchesEditAnimationsEulerRotations){
    const float angles[][3] = {{0, 0, 0}, {30, 90, 0}, {-45, 10, 170}, {90, 90, 90}, {12.5f, -200, 359}};
    bool degrees = Engine::isUseDegrees();
    Engine::setUseDegrees(true);
    for (const auto& e : angles){
        Quaternion mcp;
        ASSERT_TRUE(editor::mcp::McpValues::parseQuaternion(editor::mcp::Json{{"euler", {e[0], e[1], e[2]}}}, mcp));
        Quaternion clip = anim::eulerDegreesToQuaternion(e[0], e[1], e[2]);
        EXPECT_NEAR(clip.w, mcp.w, 1e-6f) << e[0] << " " << e[1] << " " << e[2];
        EXPECT_NEAR(clip.x, mcp.x, 1e-6f);
        EXPECT_NEAR(clip.y, mcp.y, 1e-6f);
        EXPECT_NEAR(clip.z, mcp.z, 1e-6f);

        // radians mode: the clip helper still reads degrees
        Engine::setUseDegrees(false);
        Quaternion inRadiansMode = anim::eulerDegreesToQuaternion(e[0], e[1], e[2]);
        Engine::setUseDegrees(true);
        EXPECT_EQ(inRadiansMode.w, clip.w);
        EXPECT_EQ(inRadiansMode.y, clip.y);
    }
    Engine::setUseDegrees(degrees);
}
