// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The real limits of a model, read from its file at import (spec S45 R4): what the name, the count
// and the limit of each say, without loading it.

#include "Engine.h"
#include "asset/ModelSubAssets.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string gltf(const std::string& rest) {
        return R"({"asset": {"version": "2.0"}, )" + rest + "}";
    }

    std::string numbers(size_t count) {
        std::string list;
        for (size_t i = 0; i < count; i++) list += (i ? "," : "") + std::to_string(i);
        return list;
    }

    std::string targets(size_t count, const std::string& target) {
        std::string list;
        for (size_t i = 0; i < count; i++) list += (i ? "," : "") + target;
        return list;
    }

}

TEST(ModelLimitsScan, AModelWithinThemHasNone) {
    ModelContents contents;
    const std::string json = gltf(R"("skins": [{"joints": [)" + numbers(MAX_BONES) + R"(]}], "meshes": [{"primitives": [{"targets": [)" +
                                  targets(MAX_MORPHTARGETS, R"({"POSITION": 0})") + "]}]}]");
    ASSERT_TRUE(scanModelBytes(".gltf", json, contents));
    EXPECT_TRUE(contents.limits.empty());
}

TEST(ModelLimitsScan, ASkinOverTheBonesNamesItsCountAndTheLimit) {
    ModelContents contents;
    ASSERT_TRUE(scanModelBytes(".gltf", gltf(R"("skins": [{"name": "Armature", "joints": [)" + numbers(MAX_BONES + 3) + "]}]"), contents));
    ASSERT_EQ(contents.limits.size(), 1u);
    EXPECT_EQ(contents.limits[0].what, "skin 'Armature'");
    EXPECT_EQ(contents.limits[0].kind, "bones");
    EXPECT_EQ(contents.limits[0].count, (size_t)MAX_BONES + 3);
    EXPECT_EQ(contents.limits[0].limit, (size_t)MAX_BONES);
    const std::string message = modelLimitMessage("assets/hero.glb", contents.limits[0]);
    EXPECT_NE(message.find("Model assets/hero.glb: skin 'Armature' has " + std::to_string(MAX_BONES + 3) +
                           " bones, more than the maximum allowed (" + std::to_string(MAX_BONES) + ")"), std::string::npos) << message;
}

// the loader's rule: what a target has lowers what the next ones may be
TEST(ModelLimitsScan, BlendShapesCountAsTheLoaderKeepsThem) {
    ModelContents contents;
    ASSERT_TRUE(scanModelBytes(".gltf", gltf(R"("meshes": [{"name": "Face", "primitives": [{"targets": [)" +
                                             targets(MAX_MORPHTARGETS + 1, R"({"POSITION": 0})") + "]}]}]"), contents));
    ASSERT_EQ(contents.limits.size(), 1u);
    EXPECT_EQ(contents.limits[0].what, "mesh 'Face'");
    EXPECT_EQ(contents.limits[0].count, (size_t)MAX_MORPHTARGETS + 1);
    EXPECT_EQ(contents.limits[0].limit, (size_t)MAX_MORPHTARGETS);

    ASSERT_TRUE(scanModelBytes(".gltf", gltf(R"("meshes": [{"primitives": [{"targets": [)" +
                                             targets(5, R"({"POSITION": 0, "NORMAL": 0})") + "]}]}]"), contents));
    ASSERT_EQ(contents.limits.size(), 1u);
    EXPECT_EQ(contents.limits[0].what, "mesh '#0'");
    EXPECT_EQ(contents.limits[0].limit, 4u);

    ASSERT_TRUE(scanModelBytes(".gltf", gltf(R"("meshes": [{"primitives": [{"targets": [)" +
                                             targets(3, R"({"POSITION": 0, "NORMAL": 0, "TANGENT": 0})") + "]}]}]"), contents));
    ASSERT_EQ(contents.limits.size(), 1u);
    EXPECT_EQ(contents.limits[0].limit, 2u);
}

TEST(ModelLimitsScan, MoreBuffersThanTheGpuPoolHolds) {
    const size_t limit = static_cast<size_t>(Engine::getGpuResourceLimits().buffers);
    std::string accessors, attributes;
    for (size_t i = 0; i <= limit; i++) {
        accessors += std::string(i ? "," : "") + R"({"bufferView": )" + std::to_string(i) + "}";
    }
    std::string primitives;
    for (size_t i = 0; i <= limit; i++) {
        primitives += std::string(i ? "," : "") + R"({"attributes": {"POSITION": )" + std::to_string(i) + "}}";
    }
    ModelContents contents;
    ASSERT_TRUE(scanModelBytes(".gltf", gltf(R"("accessors": [)" + accessors + R"(], "meshes": [{"primitives": [)" + primitives + "]}]"), contents));
    ASSERT_EQ(contents.limits.size(), 1u);
    EXPECT_EQ(contents.limits[0].kind, "vertex and index buffers");
    EXPECT_EQ(contents.limits[0].count, limit + 1);
    EXPECT_EQ(contents.limits[0].limit, limit);
}

TEST(ModelLimitsScan, AnObjHasNone) {
    ModelContents contents;
    std::string obj;
    for (int m = 0; m < MAX_SUBMESHES + 10; m++) obj += "usemtl m" + std::to_string(m) + "\nf 1 2 3\n";
    ASSERT_TRUE(scanModelBytes(".obj", obj, contents));
    EXPECT_TRUE(contents.limits.empty());
}
