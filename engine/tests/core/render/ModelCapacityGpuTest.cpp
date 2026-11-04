// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model looks the same in every build and on every thread (S45). This binary is the shipped case:
// a build without the editor, whose MeshComponent holds MAX_SUBMESHES submeshes and
// MAX_EXTERNAL_BUFFERS buffers inline. A mesh goes past those on the heap, a game's loading thread
// gives the main thread's node tree, and only a thumbnail (a preview load: images capped, off the
// main thread) bakes its nodes into one mesh. glcore, Vulkan and GLES.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
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

    unsigned int primitivesOf(const std::string& sample){
        return sample == "SpecularTest" ? 24u : sample == "AnisotropyRotationTest" ? 11u : 50u;
    }

    // `count` of `total` plates in a row, each of its own position, normal, UV and index buffer
    // views and its own material: one primitive of one mesh each, or a node and mesh each
    std::string writePlates(TempDir& dir, const std::string& name, int count, int total, bool eachOnItsNode = false){
        std::string bin;
        std::string views, accessors, primitives, materials, nodes, meshes, roots;
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
            const float step = 8.0f / (float)total;
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
            if (eachOnItsNode){
                meshes += std::string(p ? "," : "") + R"({"primitives": [)" + primitive + "]}";
                nodes += std::string(p ? "," : "") + R"({"mesh": )" + std::to_string(p) + R"(, "name": "Plate )" + std::to_string(p) + R"("})";
                roots += std::string(p ? "," : "") + std::to_string(p);
            }else{
                primitives += std::string(p ? "," : "") + primitive;
            }
            materials += std::string(p ? "," : "") + R"({"pbrMetallicRoughness": {"baseColorFactor": [)" + std::to_string(0.2f + 0.8f * (float)p / (float)total) +
                         R"(, 0.3, 0.2, 1], "metallicFactor": 0, "roughnessFactor": 1}})";
        }
        if (!eachOnItsNode){
            meshes = R"({"primitives": [)" + primitives + "]}";
            nodes = R"({"mesh": 0})";
            roots = "0";
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

// the preview of a model of many nodes: no error, whole primitives, and it draws
TEST_P(ManyNodeModel, APreviewOffTheMainThreadLogsNoErrorAndDraws){
    JASPER_REQUIRE_GPU();
    const std::string sample = GetParam();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    previewOnAStrand(khronosFile(sample), [&](Preview& preview){
        const MeshComponent& mesh = preview.scene.getComponent<MeshComponent>(preview.model.getEntity());
        EXPECT_EQ(mesh.numSubmeshes, primitivesOf(sample)) << sample << ": the preview left primitives out";
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
    EXPECT_EQ(submeshes, primitivesOf(sample));
    Engine::removeScene(&scene);
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed);
    EXPECT_NO_ENGINE_ERRORS();
}

// a game's loading thread: the main thread's mesh a node, every primitive, and its frame
TEST_P(ManyNodeModel, ALoadingThreadGivesTheMainThreadsHierarchyAndPixels){
    JASPER_REQUIRE_GPU();
    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    expectSameOnTheLoadingThread(khronosFile(GetParam()));
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed);
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(Khronos, ManyNodeModel, ::testing::Values("SpecularTest", "AnisotropyRotationTest", "AnisotropyStrengthTest"));

// R1: many nodes on a loading thread in a build of fixed capacities: more nodes than either capacity
TEST(ModelCapacityGpu, ALoadingThreadLoadsAModelOfManyNodesWhole){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = (int)std::max<size_t>(MAX_SUBMESHES, MAX_EXTERNAL_BUFFERS) + 10;
    const std::string file = writePlates(dir, "nodes", plates, plates, true);
    expectSameOnTheLoadingThread(file);

    std::unique_ptr<ModelScene> onThread;
    onALoadingThread(file, [&](){
        onThread = std::make_unique<ModelScene>();
        onThread->model.loadModel(file);
    });
    EXPECT_EQ(hierarchyOf(onThread->scene, onThread->model).meshes.size(), (size_t)plates);
    EXPECT_NO_ENGINE_ERRORS();
}

// A skinned, animated model: its bones and animation entities too, and the pose they draw
TEST(ModelCapacityGpu, ALoadingThreadGivesASkinnedModelItsSkeletonAndAnimations){
    JASPER_REQUIRE_GPU();
    const std::string file = JASPER_TEST_REPO_DIR "/tools/test-projects/animator/assets/models/hero_animated.gltf";
    expectSameOnTheLoadingThread(file);
    EXPECT_NO_ENGINE_ERRORS();
}

// A scene the frames run belongs to the main thread: the file is read on the loading thread and the
// tree is made by the main thread, in a frame
TEST(ModelCapacityGpu, ALoadingThreadInARunningSceneCommitsOnTheMainThread){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = (int)MAX_SUBMESHES + 4;
    const std::string file = writePlates(dir, "running", plates, plates, true);

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

// R3: one mesh past both inline capacities: every primitive with all its buffers, no error, and the
// last plate is on screen (the frame is not the one of a model without it)
TEST(ModelCapacityGpu, AMeshGoesPastItsInlineCapacitiesWithNothingLeftOut){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = (int)std::max<size_t>(MAX_SUBMESHES, MAX_EXTERNAL_BUFFERS / 4) + 10;
    const std::string whole = writePlates(dir, "whole", plates, plates);
    const std::string shorter = writePlates(dir, "shorter", plates - 1, plates);

    const int unfed = ObjectRender::getUnfedVertexInputLoads();
    unsigned int submeshes = 0;
    const std::vector<uint8_t> frame = drawModel(whole, submeshes);
    EXPECT_EQ(submeshes, (unsigned int)plates) << "a primitive was left out";
    EXPECT_EQ(ObjectRender::getUnfedVertexInputLoads(), unfed) << "a pipeline left a vertex input without its buffer";
    EXPECT_NO_ENGINE_ERRORS();

    const std::vector<uint8_t> without = drawModel(shorter, submeshes);
    EXPECT_GT(meanDifference(frame, without), 0.25) << "the last plate does not draw";
    EXPECT_NO_ENGINE_ERRORS();
}

// R1: an OBJ of more materials than MAX_SUBMESHES, on either thread
TEST(ModelCapacityGpu, AnObjOfMoreMaterialsThanTheInlineCapacityLoadsWhole){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int plates = (int)MAX_SUBMESHES + 4;
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
