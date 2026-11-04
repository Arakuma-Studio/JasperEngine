// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A mesh's bone palette is the skin's (mesh-memory spec 6.4): as many matrices as the skin has
// joints, none on a mesh without a skin, and no fixed limit on the joints (a skin of 200 joints
// loads and draws). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // A plate skinned to the last of `joints` joints (each an identity node), as a .gltf and .bin
    std::string writeSkinnedPlate(TempDir& dir, const std::string& name, int joints){
        std::string bin;
        std::string views, accessors;
        int count = 0;
        const auto add = [&](const void* data, size_t size, int target, const std::string& type, int componentType, size_t elements, const std::string& extra){
            while (bin.size() % 4) bin.push_back('\0');
            const size_t offset = bin.size();
            bin.append(static_cast<const char*>(data), size);
            views += std::string(count ? "," : "") + R"({"buffer": 0, "byteOffset": )" + std::to_string(offset) + R"(, "byteLength": )" + std::to_string(size) +
                     (target ? R"(, "target": )" + std::to_string(target) : std::string()) + "}";
            accessors += std::string(count ? "," : "") + R"({"bufferView": )" + std::to_string(count) + R"(, "componentType": )" + std::to_string(componentType) +
                         R"(, "count": )" + std::to_string(elements) + R"(, "type": ")" + type + "\"" + extra + "}";
            return count++;
        };
        const std::array<float, 12> positions = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
        const uint16_t last = (uint16_t)(joints - 1);
        const std::array<uint16_t, 16> jointIds = {last, 0, 0, 0, last, 0, 0, 0, last, 0, 0, 0, last, 0, 0, 0};
        const std::array<float, 16> weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        const std::array<uint16_t, 6> indices = {0, 2, 1, 0, 3, 2};
        std::string inverseBind;
        for (int j = 0; j < joints; j++){
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            inverseBind.append(reinterpret_cast<const char*>(identity), sizeof(identity));
        }
        const int position = add(positions.data(), sizeof(positions), 34962, "VEC3", 5126, 4, R"(, "min": [-1, 0, -1], "max": [1, 0, 1])");
        const int joint = add(jointIds.data(), sizeof(jointIds), 34962, "VEC4", 5123, 4, "");
        const int weight = add(weights.data(), sizeof(weights), 34962, "VEC4", 5126, 4, "");
        const int index = add(indices.data(), sizeof(indices), 34963, "SCALAR", 5123, 6, "");
        const int matrices = add(inverseBind.data(), inverseBind.size(), 0, "MAT4", 5126, (size_t)joints, "");

        std::string nodes = R"({"mesh": 0, "skin": 0})";
        std::string jointList, sceneNodes = "0";
        for (int j = 1; j <= joints; j++){
            nodes += R"(, {"name": "joint)" + std::to_string(j) + R"("})";
            jointList += std::string(j > 1 ? "," : "") + std::to_string(j);
            sceneNodes += "," + std::to_string(j);
        }
        const std::string binName = name + ".bin";
        std::ofstream(dir.file(binName), std::ios::binary) << bin;
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [)" << sceneNodes << R"(]}],
"nodes": [)" << nodes << R"(],
"skins": [{"joints": [)" << jointList << R"(], "inverseBindMatrices": )" << matrices << R"(}],
"meshes": [{"primitives": [{"attributes": {"POSITION": )" << position << R"(, "JOINTS_0": )" << joint << R"(, "WEIGHTS_0": )" << weight <<
            R"(}, "indices": )" << index << R"(}]}],
"buffers": [{"uri": ")" << binName << R"(", "byteLength": )" << bin.size() << R"(}], "bufferViews": [)" << views << R"(], "accessors": [)" << accessors << "]}";
        return dir.file(gltf).generic_string();
    }

    // the mesh of a loaded model that has geometry (a skinned node may be the root's child)
    const MeshComponent* drawnMesh(Scene& scene){
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.numSubmeshes > 0) return &mesh;
        }
        return nullptr;
    }

    struct SkinnedScene {
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Model model{&scene};
        bool loaded = false;

        explicit SkinnedScene(const std::string& file){
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.0f, 2.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setShadows(false);
            Engine::setScene(&scene);
            loaded = model.loadModel(file);
            Scene* current = &scene;
            if (loaded)
                loaded = GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
            GpuEnvironment::drawFrames(2);
        }
        ~SkinnedScene(){
            Engine::removeScene(&scene);
        }
    };

}

TEST(SkinPaletteGpu, PaletteSizedToTheSkin){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const LogCapture log;
    SkinnedScene skinned(writeSkinnedPlate(dir, "skin70", 70));
    ASSERT_TRUE(skinned.loaded);
    const MeshComponent* mesh = drawnMesh(skinned.scene);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->bonesMatrix.stored(), 70u);
    EXPECT_EQ(mesh->bonesUploadRows, 70u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();

    // a mesh without a skin has no palette, and no palette resource
    Primitive box(&skinned.scene);
    box.createBox(1.0f, 1.0f, 1.0f);
    GpuEnvironment::drawFrames(2);
    MeshComponent& boxMesh = skinned.scene.getComponent<MeshComponent>(box.getEntity());
    EXPECT_EQ(boxMesh.bonesMatrix.stored(), 0u);
    EXPECT_FALSE(boxMesh.bonesTexture.isCreated());
    EXPECT_FALSE(boxMesh.bonesBuffer.isCreated());
}

TEST(SkinPaletteGpu, SkinOverTheOldLimitLoads){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const LogCapture log;
    SkinnedScene skinned(writeSkinnedPlate(dir, "skin200", 200));
    ASSERT_TRUE(skinned.loaded) << log.describeErrors();
    const MeshComponent* mesh = drawnMesh(skinned.scene);
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->bonesMatrix.stored(), 200u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}
