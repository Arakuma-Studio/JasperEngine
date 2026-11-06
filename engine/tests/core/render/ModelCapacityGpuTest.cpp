// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A mesh holds as many submeshes and buffers as its model has (mesh-memory spec): no capacity in a
// game or the editor. The main thread and a game's loading thread give each mesh node of a model its
// own mesh (S65); a preview off the main thread bakes the nodes into one mesh (MeshSystem::loadGLTF's
// flatten path), which held at most 16 submeshes and 30 buffers before and left the rest out.
// Whole primitives either way (a primitive without some of its buffers drew with no position and
// made a pipeline with a hole in its attributes). glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "render/ObjectRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string khronosFile(const std::string& sample){
        return std::string(JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/") + sample + "/glTF-Binary/" + sample + ".glb";
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // every attribute of every submesh has its buffer on the mesh
    void expectWholePrimitives(const MeshComponent& mesh, const std::string& what){
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const Submesh& submesh = mesh.submeshes[s];
            EXPECT_TRUE(submesh.attributes.count(AttributeType::POSITION)) << what << ": submesh " << s << " has no position";
            for (const auto& attribute : submesh.attributes){
                bool found = false;
                for (unsigned int b = 0; b < mesh.numExternalBuffers; b++)
                    found = found || mesh.eBuffers[b].getName() == attribute.second.getBufferName();
                EXPECT_TRUE(found) << what << ": submesh " << s << " names a buffer the mesh does not have: " << attribute.second.getBufferName();
            }
        }
    }

    // A model as ProjectWindow's thumbnail strand makes its preview: loaded off the main thread at
    // 256 px and drawn there, the frames going on, the preview freed on the main thread. What
    // `look` is given to see runs once it was drawn.
    struct Preview {
        Scene scene{EntityPool::System};
        Camera camera{&scene};
        Light light{&scene};
        Model model{&scene};
    };

    template <typename Look>
    void previewOnAStrand(const std::string& file, Look look){
        Scene frames;
        Camera camera(&frames);
        camera.activate();
        Engine::setScene(&frames);

        Preview* preview = nullptr;
        std::atomic<bool> finished{false};
        bool loaded = false;
        std::thread strand([&](){
            {
                Engine::AsyncThreadScope loadScope;
                MeshSystem::setImageDecodeMaxDimension(256);
                preview = new Preview();
                preview->scene.setCamera(&preview->camera);
                preview->camera.setType(CameraType::CAMERA_PERSPECTIVE);
                preview->camera.setFramebufferSize(64, 64);
                preview->camera.setRenderToTexture(true);
                preview->light.setType(LightType::DIRECTIONAL);
                preview->light.setDirection(-0.3f, -0.5f, -1.0f);
                preview->light.setShadows(false);
                loaded = preview->model.loadModel(file);
                MeshSystem::setImageDecodeMaxDimension(0);
                if (loaded){
                    // the framing of the model the preview knows: the mesh's box from its front
                    const MeshComponent& mesh = preview->scene.getComponent<MeshComponent>(preview->model.getEntity());
                    const Vector3 center = mesh.verticesAABB.getCenter();
                    const float radius = std::max(mesh.verticesAABB.getHalfSize().length(), 0.001f);
                    preview->camera.setPosition(center.x, center.y, center.z + radius * 3.0f);
                    preview->camera.setTarget(center.x, center.y, center.z);
                    Engine::AsyncThreadScope drawScope;
                    Engine::executeSceneOnce(&preview->scene);
                }
            }
            finished.store(true);
        });

        // (a commit of the strand's queue waits for a frame, and a preview is drawn in one)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline)
            GpuEnvironment::drawFrames(1);
        ASSERT_TRUE(finished.load()) << file << ": the strand never finished";
        strand.join();
        while (preview && Engine::isSceneRunning(&preview->scene))
            GpuEnvironment::drawFrames(1);
        ASSERT_TRUE(loaded) << file;
        look(*preview);
        Engine::removeScene(&preview->scene);
        delete preview;
        GpuEnvironment::drawFrames(2);
        Engine::removeScene(&frames);
    }

    class ManyNodeModel : public ::testing::TestWithParam<const char*> {};

    // `count` plates in a row, each of its own position, normal, UV and index buffer views
    std::string writePlates(TempDir& dir, const std::string& name, int count){
        struct Vertex { float position[3]; float normal[3]; float uv[2]; };
        std::string bin;
        std::string views, accessors, primitives, materials;
        const auto add = [&](const void* data, size_t size, int target, const std::string& accessorType, int componentType, size_t elements, const std::string& bounds){
            const size_t offset = bin.size();
            bin.append(static_cast<const char*>(data), size);
            const int view = std::count(views.begin(), views.end(), '{');
            views += std::string(view ? "," : "") + R"({"buffer": 0, "byteOffset": )" + std::to_string(offset) + R"(, "byteLength": )" + std::to_string(size) +
                     R"(, "target": )" + std::to_string(target) + "}";
            accessors += std::string(view ? "," : "") + R"({"bufferView": )" + std::to_string(view) + R"(, "componentType": )" + std::to_string(componentType) +
                         R"(, "count": )" + std::to_string(elements) + R"(, "type": ")" + accessorType + "\"" + bounds + "}";
            return view;
        };
        for (int p = 0; p < count; p++){
            const float x0 = -4.0f + (float)p, x1 = x0 + 0.8f;
            const std::array<float, 12> positions = {x0, 0.0f, -1.0f, x1, 0.0f, -1.0f, x1, 0.0f, 1.0f, x0, 0.0f, 1.0f};
            const std::array<float, 12> normals = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
            const std::array<float, 8> uvs = {0, 0, 1, 0, 1, 1, 0, 1};
            const std::array<uint16_t, 6> indices = {0, 2, 1, 0, 3, 2};
            const int position = add(positions.data(), sizeof(positions), 34962, "VEC3", 5126, 4,
                ", \"min\": [" + std::to_string(x0) + ", 0, -1], \"max\": [" + std::to_string(x1) + ", 0, 1]");
            const int normal = add(normals.data(), sizeof(normals), 34962, "VEC3", 5126, 4, "");
            const int uv = add(uvs.data(), sizeof(uvs), 34962, "VEC2", 5126, 4, "");
            const int index = add(indices.data(), sizeof(indices), 34963, "SCALAR", 5123, 6, "");
            primitives += std::string(p ? "," : "") + R"({"attributes": {"POSITION": )" + std::to_string(position) + R"(, "NORMAL": )" + std::to_string(normal) +
                          R"(, "TEXCOORD_0": )" + std::to_string(uv) + R"(}, "indices": )" + std::to_string(index) + R"(, "material": )" + std::to_string(p) + "}";
            materials += std::string(p ? "," : "") + R"({"pbrMetallicRoughness": {"baseColorFactor": [)" + std::to_string(0.2f + 0.1f * (float)p) +
                         R"(, 0.3, 0.2, 1], "metallicFactor": 0, "roughnessFactor": 1}})";
        }
        const std::string binName = name + ".bin";
        std::ofstream(dir.file(binName), std::ios::binary) << bin;
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"primitives": [)" << primitives << R"(]}], "materials": [)" << materials << R"(],
"buffers": [{"uri": ")" << binName << R"(", "byteLength": )" << bin.size() << R"(}], "bufferViews": [)" << views << R"(], "accessors": [)" << accessors << "]}";
        return dir.file(gltf).generic_string();
    }

    std::vector<uint8_t> drawModel(const std::string& file, unsigned int& submeshes){
        Scene scene;
        Camera camera(&scene);
        camera.setType(CameraType::CAMERA_PERSPECTIVE);
        camera.setPosition(0.0f, 7.0f, 3.0f);
        camera.setTarget(0.0f, 0.0f, 0.0f);
        camera.activate();
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        sun.setDirection(0.0f, -1.0f, 0.0f);
        sun.setShadows(false);
        Engine::setScene(&scene);
        Model model(&scene);
        EXPECT_TRUE(model.loadModel(file)) << file;
        Scene* current = &scene;
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << file;
        GpuEnvironment::drawFrames(3);
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
        submeshes = mesh.numSubmeshes;
        expectWholePrimitives(mesh, file);
        std::vector<uint8_t> frame = readTarget();
        Engine::removeScene(&scene);
        return frame;
    }

}

// the preview of a model of many nodes: no error, whole primitives, and it draws
TEST_P(ManyNodeModel, APreviewOffTheMainThreadLogsNoErrorAndDraws){
    JASPER_REQUIRE_GPU();
    const std::string sample = GetParam();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    previewOnAStrand(khronosFile(sample), [&](Preview& preview){
        const MeshComponent& mesh = preview.scene.getComponent<MeshComponent>(preview.model.getEntity());
        EXPECT_GT(mesh.numSubmeshes, 0u) << sample;
        EXPECT_LE(mesh.numExternalBuffers, (unsigned int)mesh.eBuffers.stored()) << sample;
        expectWholePrimitives(mesh, sample);

        Framebuffer* framebuffer = preview.camera.getFramebuffer();
        ASSERT_NE(framebuffer, nullptr);
        std::vector<uint8_t> rgba;
        readTexture(framebuffer->getRender().getColorTexture(), (int)framebuffer->getWidth(), (int)framebuffer->getHeight(), rgba);
        ASSERT_FALSE(rgba.empty());
        int differ = 0;
        for (size_t i = 4; i < rgba.size(); i += 4)
            differ += (rgba[i] != rgba[0] || rgba[i + 1] != rgba[1] || rgba[i + 2] != rgba[2]) ? 1 : 0;
        EXPECT_GT(differ, 16) << sample << ": the preview drew nothing";
    });
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed) << sample << ": a pipeline left a vertex input without its buffer";
    EXPECT_NO_ENGINE_ERRORS();
}

// the same model in a scene on the main thread: a mesh a node, every primitive of the file, no error
TEST_P(ManyNodeModel, InASceneEveryPrimitiveLoads){
    JASPER_REQUIRE_GPU();
    const std::string sample = GetParam();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    Scene scene;
    Camera camera(&scene);
    camera.activate();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setShadows(false);
    Engine::setScene(&scene);
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(khronosFile(sample))) << sample;
    Scene* current = &scene;
    ASSERT_TRUE(GpuEnvironment::runFrames([current](){
        return current->getSystem<RenderSystem>()->isAllLoaded();
    })) << sample;
    unsigned int submeshes = 0;
    auto meshes = scene.getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++){
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        submeshes += mesh.numSubmeshes;
        expectWholePrimitives(mesh, sample);
    }
    // SpecularTest 24, AnisotropyRotationTest 11, AnisotropyStrengthTest 50
    EXPECT_EQ(submeshes, sample == "SpecularTest" ? 24u : sample == "AnisotropyRotationTest" ? 11u : 50u);
    Engine::removeScene(&scene);
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed);
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(Khronos, ManyNodeModel, ::testing::Values("SpecularTest", "AnisotropyRotationTest", "AnisotropyStrengthTest"));

// A model of more primitives and buffers than any fixed capacity held (40 plates: 40 submeshes,
// 160 buffers) loads whole on the main thread, without an error
TEST(ModelCapacityGpu, AModelOverTheOldCapacitiesLoadsWhole){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const std::string plates = writePlates(dir, "plates", 40);
    const LogCapture log;
    unsigned int submeshes = 0;
    drawModel(plates, submeshes);
    EXPECT_EQ(submeshes, 40u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// The preview baked into one mesh off the main thread keeps every primitive of a 50-primitive model
TEST(ModelCapacityGpu, APreviewOffTheMainThreadKeepsEveryPrimitive){
    JASPER_REQUIRE_GPU();
    const LogCapture log;
    previewOnAStrand(khronosFile("AnisotropyStrengthTest"), [&](Preview& preview){
        const MeshComponent& mesh = preview.scene.getComponent<MeshComponent>(preview.model.getEntity());
        EXPECT_EQ(mesh.numSubmeshes, 50u);
        expectWholePrimitives(mesh, "AnisotropyStrengthTest");
    });
    EXPECT_EQ(log.count(D_LOG_WARN, "left out"), 0u);
}
