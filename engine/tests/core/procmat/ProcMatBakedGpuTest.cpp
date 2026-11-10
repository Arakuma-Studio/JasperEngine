// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 AC16: a baked Procedural Material is an ordinary material to a game. The fixture
// tools/fixtures/procmat-baked holds a 30-node graph and what the bake CLI wrote for it (PNGs and a
// .material); a sphere draws with the slots and factors that .material names, on glcore and Vulkan.
// The same project goes through an export's texture cooks in tools/smoke-test.sh.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const std::string FIXTURE = JASPER_TEST_REPO_DIR "/tools/fixtures/procmat-baked/";

    // The top-level `key: value` lines of a .material, and each texture slot's `path` as `<slot>.path`.
    // The engine reads no .material (a game's are generated code), so the test reads the few keys a bake
    // sets, as the editor writes them.
    std::map<std::string, std::string> materialKeys(const std::string& path) {
        std::ifstream file(path);
        std::map<std::string, std::string> keys;
        std::string line, block;
        while (std::getline(file, line)) {
            const size_t colon = line.find(':');
            if (line.empty() || colon == std::string::npos) continue;
            std::string value = line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            if (line[0] != ' ') {
                block = line.substr(0, colon);
                keys[block] = value;
            } else if (line.compare(0, colon, "  path") == 0) {
                keys[block + ".path"] = value;
            }
        }
        return keys;
    }

    std::string slotPath(const std::map<std::string, std::string>& keys, const std::string& slot) {
        auto it = keys.find(slot + ".path");
        return it == keys.end() ? std::string() : it->second;
    }

}

TEST(ProcMatBakedGpu, ABakedMaterialDrawsOnASphere) {
    JASPER_REQUIRE_GPU();
    const std::map<std::string, std::string> keys = materialKeys(FIXTURE + "assets/brick_wall/brick_wall.material");
    ASSERT_FALSE(keys.empty()) << "the fixture's baked .material";
    // the bake's slots (applyBakeToMaterial): the ORM is both the occlusion and the metallic-roughness texture
    EXPECT_EQ(slotPath(keys, "baseColorTexture"), "assets/brick_wall/brick_wall_base_color.png");
    EXPECT_EQ(slotPath(keys, "normalTexture"), "assets/brick_wall/brick_wall_normal.png");
    EXPECT_EQ(slotPath(keys, "metallicRoughnessTexture"), "assets/brick_wall/brick_wall_orm.png");
    EXPECT_EQ(slotPath(keys, "occlusionTexture"), "assets/brick_wall/brick_wall_orm.png");
    EXPECT_EQ(slotPath(keys, "emissiveTexture"), "") << "the graph has no Emission";

    Scene scene;
    Camera camera(&scene);
    scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 0.6f, 3.2f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Engine::setScene(&scene);
    {
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(-0.5f, -0.6f, -0.8f);
        sun.setIntensity(2.0f);
        sun.setShadows(false);
        Primitive sphere(&scene);
        sphere.createSphere(1.2f, 64, 48);
        Material material = sphere.getMaterial();
        material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        material.metallicFactor = std::stof(keys.at("metallicFactor"));
        material.roughnessFactor = std::stof(keys.at("roughnessFactor"));
        material.baseColorTexture.setPath(FIXTURE + slotPath(keys, "baseColorTexture"));
        material.normalTexture.setPath(FIXTURE + slotPath(keys, "normalTexture"));
        material.metallicRoughnessTexture.setPath(FIXTURE + slotPath(keys, "metallicRoughnessTexture"));
        material.occlusionTexture.setPath(FIXTURE + slotPath(keys, "occlusionTexture"));
        sphere.setMaterial(material);
        Scene* current = &scene;
        ASSERT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        EXPECT_GOLDEN("procmat_baked_brick_wall");
        EXPECT_NO_ENGINE_ERRORS();
    }
    Engine::removeScene(&scene);
}
