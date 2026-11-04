// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Rotations given to MCP tools (McpValues::parseQuaternion, shared by set_properties,
// set_transform, colliders, clip keys and tweens): stored as unit quaternions. Four numbers far
// from unit length (Euler degrees typed as a quaternion: [90, 0, 0, 1]) were stored as given and
// scaled what they rotated; readback hid it (it shows a normalized copy).

#include <gtest/gtest.h>

#include "mcp/McpValues.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor::mcp;

namespace {

    float length(const Quaternion& q){
        return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    }

}

TEST(McpQuaternion, EulerDegreesAsAQuaternionAreRefused){
    Quaternion q;
    std::string why;
    EXPECT_FALSE(McpValues::parseQuaternion(Json::parse("[90, 0, 0, 1]"), q, &why));
    EXPECT_NE(why.find("euler"), std::string::npos) << why;
    EXPECT_NE(why.find("[90, 0, 0]"), std::string::npos) << why;
    EXPECT_FALSE(McpValues::parseQuaternion(Json::parse("{\"quat\": [0, 180, 0, 1]}"), q, &why));
}

TEST(McpQuaternion, NearUnitIsNormalized){
    Quaternion q;
    ASSERT_TRUE(McpValues::parseQuaternion(Json::parse("[0, 0, 0.71, 0.71]"), q));
    EXPECT_NEAR(length(q), 1.0f, 1e-6f);
    EXPECT_NEAR(q.z, std::sqrt(0.5f), 1e-5f);
    ASSERT_TRUE(McpValues::parseQuaternion(Json::parse("{\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}"), q));
    EXPECT_EQ(q, Quaternion::IDENTITY);
}

TEST(McpQuaternion, ZeroPartialAndOffUnitAreRefused){
    Quaternion q;
    std::string why;
    EXPECT_FALSE(McpValues::parseQuaternion(Json::parse("[0, 0, 0, 0]"), q, &why));
    EXPECT_NE(why.find("zero"), std::string::npos) << why;
    // a partial object kept the old w: refused, all four keys or euler
    EXPECT_FALSE(McpValues::parseQuaternion(Json::parse("{\"x\": 90, \"y\": 0, \"z\": 0}"), q, &why));
    EXPECT_NE(why.find("x, y, z and w"), std::string::npos) << why;
    // 12% off unit: a typo, not a rotation
    EXPECT_FALSE(McpValues::parseQuaternion(Json::parse("[0, 0.5, 0, 1]"), q, &why));
    EXPECT_NE(why.find("unit"), std::string::npos) << why;
}

TEST(McpQuaternion, EulerIsUnchanged){
    Quaternion q;
    ASSERT_TRUE(McpValues::parseQuaternion(Json::parse("{\"euler\": [0, 90, 0]}"), q));
    const Quaternion expected(0.0f, 90.0f, 0.0f);
    EXPECT_NEAR(q.x, expected.x, 1e-6f);
    EXPECT_NEAR(q.y, expected.y, 1e-6f);
    EXPECT_NEAR(q.z, expected.z, 1e-6f);
    EXPECT_NEAR(q.w, expected.w, 1e-6f);
}
