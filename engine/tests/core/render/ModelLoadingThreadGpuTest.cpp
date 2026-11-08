// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model looks the same on every thread (spec S65 R1, R2): a game's loading thread gives the main
// thread's entities and pixels, and only a thumbnail bakes its nodes into one mesh. This binary is a
// build without the editor. glcore, Vulkan and GLES.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "render/ObjectRender.h"
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string khronosFile(const std::string& sample){
        return std::string(JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/") + sample + "/glTF-Binary/" + sample + ".glb";
    }

    // mean over the pixels of the largest channel difference (0..255)
    double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += largest;
        }
        return sum / (double)pixels;
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // every attribute of every submesh has its buffer on the mesh (an OBJ's are all in the one it builds)
    void expectWholePrimitives(const MeshComponent& mesh, const std::string& what){
        if (mesh.buffer.getSize() > 0)
            return;
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

    // `count` plates in a row, each of its own node, mesh, material and position, normal, UV and
    // index buffer views
    std::string writePlates(TempDir& dir, const std::string& name, int count){
        std::string bin;
        std::string views, accessors, materials, nodes, meshes, roots;
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
            const float step = 8.0f / (float)count;
            const float x0 = -4.0f + step * (float)p, x1 = x0 + step * 0.8f;
            const std::array<float, 12> positions = {x0, 0.0f, -1.0f, x1, 0.0f, -1.0f, x1, 0.0f, 1.0f, x0, 0.0f, 1.0f};
            const std::array<float, 12> normals = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
            const std::array<float, 8> uvs = {0, 0, 1, 0, 1, 1, 0, 1};
            const std::array<uint16_t, 6> indices = {0, 2, 1, 0, 3, 2};
            const int position = add(positions.data(), sizeof(positions), 34962, "VEC3", 5126, 4,
                ", \"min\": [" + std::to_string(x0) + ", 0, -1], \"max\": [" + std::to_string(x1) + ", 0, 1]");
            const int normal = add(normals.data(), sizeof(normals), 34962, "VEC3", 5126, 4, "");
            const int uv = add(uvs.data(), sizeof(uvs), 34962, "VEC2", 5126, 4, "");
            const int index = add(indices.data(), sizeof(indices), 34963, "SCALAR", 5123, 6, "");
            const std::string primitive = R"({"attributes": {"POSITION": )" + std::to_string(position) + R"(, "NORMAL": )" + std::to_string(normal) +
                          R"(, "TEXCOORD_0": )" + std::to_string(uv) + R"(}, "indices": )" + std::to_string(index) + R"(, "material": )" + std::to_string(p) + "}";
            meshes += std::string(p ? "," : "") + R"({"primitives": [)" + primitive + "]}";
            nodes += std::string(p ? "," : "") + R"({"mesh": )" + std::to_string(p) + R"(, "name": "Plate )" + std::to_string(p) + R"("})";
            roots += std::string(p ? "," : "") + std::to_string(p);
            materials += std::string(p ? "," : "") + R"({"pbrMetallicRoughness": {"baseColorFactor": [)" + std::to_string(0.2f + 0.8f * (float)p / (float)count) +
                         R"(, 0.3, 0.2, 1], "metallicFactor": 0, "roughnessFactor": 1}})";
        }
        const std::string binName = name + ".bin";
        std::ofstream(dir.file(binName), std::ios::binary) << bin;
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [)" << roots << R"(]}], "nodes": [)" << nodes << R"(],
"meshes": [)" << meshes << R"(], "materials": [)" << materials << R"(],
"buffers": [{"uri": ")" << binName << R"(", "byteLength": )" << bin.size() << R"(}], "bufferViews": [)" << views << R"(], "accessors": [)" << accessors << "]}";
        return dir.file(gltf).generic_string();
    }

    // an OBJ of `count` of `total` plates, a material each
    std::string writeObjPlates(TempDir& dir, const std::string& name, int count, int total){
        const std::string objName = name + ".obj", mtlName = name + ".mtl";
        std::ofstream obj(dir.file(objName));
        std::ofstream mtl(dir.file(mtlName));
        obj << "mtllib " << mtlName << "\nvn 0 1 0\n";
        for (int p = 0; p < count; p++){
            const float step = 8.0f / (float)total;
            const float x0 = -4.0f + step * (float)p, x1 = x0 + step * 0.8f;
            mtl << "newmtl plate" << p << "\nKd " << 0.2f + 0.8f * (float)p / (float)total << " 0.3 0.2\nNs 0\nd 1\n";
            obj << "v " << x0 << " 0 -1\nv " << x1 << " 0 -1\nv " << x1 << " 0 1\nv " << x0 << " 0 1\n";
            const int v = p * 4;
            obj << "usemtl plate" << p << "\nf " << v + 1 << "//1 " << v + 3 << "//1 " << v + 2 << "//1\nf " << v + 1 << "//1 " << v + 4 << "//1 " << v + 3 << "//1\n";
        }
        return dir.file(objName).generic_string();
    }

    // A camera above the row of plates and a sun, for a model loaded on any thread
    struct ModelScene {
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        Model model{&scene};

        ModelScene(){
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 7.0f, 3.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.0f, -1.0f, 0.0f);
            sun.setShadows(false);
        }
    };

    // what a model made: its mesh entities by name and their submeshes, its bones and animations
    struct Hierarchy {
        std::vector<std::pair<std::string, unsigned int>> meshes;
        size_t bones = 0;
        size_t animations = 0;
    };

    Hierarchy hierarchyOf(Scene& scene, const Model& model){
        Hierarchy hierarchy;
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.numSubmeshes > 0)
                hierarchy.meshes.emplace_back(scene.getEntityName(meshes->getEntity(i)), mesh.numSubmeshes);
        }
        std::sort(hierarchy.meshes.begin(), hierarchy.meshes.end());
        const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
        hierarchy.bones = component.bonesIdMapping.size();
        hierarchy.animations = component.animations.size();
        return hierarchy;
    }

    // The frame of a scene loaded by now, then the scene off the engine
    std::vector<uint8_t> drawScene(Scene& scene, const std::string& what){
        Engine::setScene(&scene);
        Scene* current = &scene;
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << what;
        GpuEnvironment::drawFrames(3);
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++)
            expectWholePrimitives(meshes->getComponentFromIndex(i), what);
        std::vector<uint8_t> frame = readTarget();
        Engine::removeScene(&scene);
        return frame;
    }

    std::vector<uint8_t> drawModel(const std::string& file, unsigned int& submeshes){
        ModelScene loaded;
        EXPECT_TRUE(loaded.model.loadModel(file)) << file;
        submeshes = 0;
        auto meshes = loaded.scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++)
            submeshes += meshes->getComponentFromIndex(i).numSubmeshes;
        return drawScene(loaded.scene, file);
    }

    // runs `work` on a thread flagged as a game's loading thread, the frames going on here
    template <typename Work>
    void onALoadingThread(const std::string& what, Work work){
        std::atomic<bool> finished{false};
        std::thread loading([&](){
            {
                Engine::AsyncThreadScope scope;
                work();
            }
            finished.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline)
            GpuEnvironment::drawFrames(1);
        ASSERT_TRUE(finished.load()) << what << ": the loading thread never finished";
        loading.join();
    }

    // The same file on the main thread and on a game's loading thread: the hierarchy and the frame
    void expectSameOnTheLoadingThread(const std::string& file){
        ModelScene onMain;
        ASSERT_TRUE(onMain.model.loadModel(file)) << file;
        const Hierarchy mainHierarchy = hierarchyOf(onMain.scene, onMain.model);
        const std::vector<uint8_t> mainFrame = drawScene(onMain.scene, file);

        std::unique_ptr<ModelScene> onThread;
        bool loaded = false;
        onALoadingThread(file, [&](){
            onThread = std::make_unique<ModelScene>();
            loaded = onThread->model.loadModel(file);
        });
        ASSERT_TRUE(loaded) << file;
        const Hierarchy threadHierarchy = hierarchyOf(onThread->scene, onThread->model);
        EXPECT_EQ(threadHierarchy.meshes, mainHierarchy.meshes) << file;
        EXPECT_EQ(threadHierarchy.bones, mainHierarchy.bones) << file;
        EXPECT_EQ(threadHierarchy.animations, mainHierarchy.animations) << file;
        const std::vector<uint8_t> threadFrame = drawScene(onThread->scene, file);
        EXPECT_LT(meanDifference(threadFrame, mainFrame), 0.25) << file << ": the loading thread's frame is not the main thread's";
        onThread.reset();
        GpuEnvironment::drawFrames(2);
    }

}

class ManyNodeLoadingThread : public ::testing::TestWithParam<const char*> {};

// a Khronos sample of many nodes: the main thread's mesh a node, every primitive, and its frame
TEST_P(ManyNodeLoadingThread, GivesTheMainThreadsHierarchyAndPixels){
    JASPER_REQUIRE_GPU();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    expectSameOnTheLoadingThread(khronosFile(GetParam()));
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed);
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(Khronos, ManyNodeLoadingThread, ::testing::Values("SpecularTest", "AnisotropyRotationTest", "AnisotropyStrengthTest"));

// R1: many nodes off the main thread, more than a baked mesh ever held
TEST(ModelLoadingThreadGpu, LoadsAModelOfManyNodesWhole){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = 40;
    const std::string file = writePlates(dir, "nodes", plates);
    expectSameOnTheLoadingThread(file);

    std::unique_ptr<ModelScene> onThread;
    onALoadingThread(file, [&](){
        onThread = std::make_unique<ModelScene>();
        onThread->model.loadModel(file);
    });
    EXPECT_EQ(hierarchyOf(onThread->scene, onThread->model).meshes.size(), (size_t)plates);
    onThread.reset();
    EXPECT_NO_ENGINE_ERRORS();
}

// A skinned, animated model: its bones and animation entities too, and the pose they draw
TEST(ModelLoadingThreadGpu, GivesASkinnedModelItsSkeletonAndAnimations){
    JASPER_REQUIRE_GPU();
    expectSameOnTheLoadingThread(JASPER_TEST_REPO_DIR "/tools/test-projects/animator/assets/models/hero_animated.gltf");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(ModelLoadingThreadGpu, GivesAnObjTheMainThreadsPixels){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    expectSameOnTheLoadingThread(writeObjPlates(dir, "plates", 8, 8));
    EXPECT_NO_ENGINE_ERRORS();
}

// A scene the frames run belongs to the main thread: the file is read on the loading thread and the
// tree is made by the main thread, in a frame
TEST(ModelLoadingThreadGpu, InARunningSceneCommitsOnTheMainThread){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = 20;
    const std::string file = writePlates(dir, "running", plates);

    unsigned int submeshes = 0;
    const std::vector<uint8_t> reference = drawModel(file, submeshes);
    ASSERT_EQ(submeshes, (unsigned int)plates);

    ModelScene running;
    Engine::setScene(&running.scene);
    GpuEnvironment::drawFrames(2);
    const Entity entity = running.model.getEntity();
    Scene* scene = &running.scene;
    bool loaded = false;
    onALoadingThread(file, [&](){
        Model handle(scene, entity);
        loaded = handle.loadModel(file);
    });
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(GpuEnvironment::runFrames([&](){
        return hierarchyOf(running.scene, running.model).meshes.size() == (size_t)plates && running.scene.getSystem<RenderSystem>()->isAllLoaded();
    })) << "the main thread never made the tree";
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(hierarchyOf(running.scene, running.model).meshes.size(), (size_t)plates);
    EXPECT_LT(meanDifference(readTarget(), reference), 0.25);
    Engine::removeScene(&running.scene);
    EXPECT_NO_ENGINE_ERRORS();
}

// R1: an OBJ of more materials than a mesh held (24, the car of the mesh-memory design; MAX_SUBMESHES
// was 16), on both threads.
TEST(ModelLoadingThreadGpu, AnObjOfManyMaterialsLoadsWhole){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = 24;
    const std::string whole = writeObjPlates(dir, "whole", plates, plates);
    const std::string shorter = writeObjPlates(dir, "shorter", plates - 1, plates);

    unsigned int submeshes = 0;
    const std::vector<uint8_t> frame = drawModel(whole, submeshes);
    EXPECT_EQ(submeshes, (unsigned int)plates) << "materials were cut off";
    EXPECT_NO_ENGINE_ERRORS();

    const std::vector<uint8_t> without = drawModel(shorter, submeshes);
    EXPECT_GT(meanDifference(frame, without), 0.25) << "the last material's faces do not draw";
    expectSameOnTheLoadingThread(whole);
    EXPECT_NO_ENGINE_ERRORS();
}
