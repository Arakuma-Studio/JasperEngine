// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Read/Write (docs/specs/S53-model-import-cook.md R11, AC8) through the renderer: a cooked model
// nothing at runtime reads frees the file's mesh bytes after upload and keeps drawing, through a
// reload and a clone, without reading the file again; every reader gets the bytes back; with
// keepCpuData the bytes stay and each reader reads them; the editor never releases. AC7's Medium
// golden: the model cook's Medium output (fixtures/gltf/s41/cooked_medium.glb, held to the cook by
// the editor's ModelCookLoad tests) draws what its source draws.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "model/CookedModels.h"
#include "model/ModelCpuData.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "particles/ParticleMeshes.h"
#include "picking/PickGeometry.h"
#include "pool/MeshLodPool.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/MeshTriangles.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestSystem.h"
#include "subsystem/GltfTriangleSoup.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace jasperengine {
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* const kAsset = "asset://models/source.glb";

    size_t differingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return (size_t)-1;
        size_t count = 0;
        for (size_t i = 0; i < a.size(); i += 4){
            bool differs = false;
            for (size_t c = 0; c < 3; c++)
                differs = differs || std::abs((int)a[i + c] - (int)b[i + c]) > 2;
            count += differs ? 1 : 0;
        }
        return count;
    }

    class EditorLikeSystem : public TestSystem {
    public:
        bool isEditor() override { return true; }
    };

    class ModelCpuDataGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light light{&scene};
        bool keepCpuData = false;

        void SetUp() override{
            scene.setBackgroundColor(0.05f, 0.05f, 0.25f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 1.0f, 6.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(-0.4f, -1.0f, -0.6f);
            light.setIntensity(1.5f);
            Engine::setScene(&scene);
            Engine::setFixedDeltatime(1.0 / 60.0);
            // what an exported game's index.json says of the model
            CookedModels::setProvider([this](const std::string& assetPath, CookedModelEntry& out){
                if (assetPath != "models/source.glb")
                    return false;
                out.file = s41Fixture("source.glb");
                out.keepCpuData = keepCpuData;
                return true;
            });
        }

        void TearDown() override{
            CookedModels::setProvider(nullptr);
            Engine::setFixedDeltatime(0.0);
            Engine::removeScene(&scene);
            ModelPool::invalidate(MeshSystem::getModelFilenameKey(kAsset));
            ModelCpuData::clear();
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
        }

        std::vector<uint8_t> frame(){
            GpuEnvironment::drawFrames(2);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        // the mesh entities of the model's file, in their components
        std::vector<MeshComponent*> meshesOf(const tinygltf::Model* parse){
            std::vector<MeshComponent*> out;
            auto meshes = scene.getComponentArray<MeshComponent>();
            for (int i = 0; i < (int)meshes->size(); i++){
                MeshComponent& mesh = meshes->getComponentFromIndex(i);
                if (mesh.numExternalBuffers > 0 && mesh.eBuffers[0].getShareOwner().lock().get() == static_cast<const void*>(parse))
                    out.push_back(&mesh);
            }
            return out;
        }

        static uint64_t fileBytes(const tinygltf::Model& parse){
            uint64_t bytes = 0;
            for (const tinygltf::Buffer& buffer : parse.buffers)
                if (buffer.name != MeshSystem::kSyntheticBufferName)
                    bytes += buffer.data.capacity();
            return bytes;
        }

        static size_t triangleIndices(MeshComponent& mesh){
            size_t count = 0;
            for (const MeshTriangles::MeshSourceView& view : MeshTriangles::collectMeshSourceViews(&mesh))
                if (view.indexBuffer && view.indexBuffer->getData())
                    count += view.indexAttr.getCount();
            return count;
        }
    };

}

// R11: the file's bytes are gone, the image is the same, and neither a reload of the mesh nor a
// frame reads the file again
TEST_F(ModelCpuDataGpu, ReleasesTheFilesBytesAfterUploadAndKeepsDrawing){
    JASPER_REQUIRE_GPU();
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> before = frame();
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    ASSERT_TRUE(parse);
    const uint64_t held = fileBytes(*parse);
    EXPECT_GT(held, 12000u);
    EXPECT_EQ(ModelCpuData::getPendingCount(), 1u);

    ModelCpuData::update({&scene}, true);
    EXPECT_EQ(ModelCpuData::getPendingCount(), 0u) << ModelCpuData::describeHeld();
    EXPECT_EQ(fileBytes(*parse), 0u) << "the parse's vectors hold no memory";
    EXPECT_GE(ModelCpuData::getReleasedBytes(), held / 2);
    const std::vector<MeshComponent*> meshes = meshesOf(parse);
    ASSERT_FALSE(meshes.empty());
    for (MeshComponent* mesh : meshes)
        for (unsigned int b = 0; b < mesh->numExternalBuffers; b++)
            if (parse->buffers[(size_t)mesh->eBuffers[b].getModelBuffer()].name != MeshSystem::kSyntheticBufferName)
                EXPECT_TRUE(mesh->eBuffers[b].isDataReleased() && !mesh->eBuffers[b].getData());

    EXPECT_EQ(differingPixels(before, frame()), 0u);

    // a reload (a material or a view change) binds the same GPU buffers
    for (MeshComponent* mesh : meshes)
        mesh->needReload = true;
    ASSERT_TRUE(settle());
    EXPECT_EQ(differingPixels(before, frame()), 0u);
    EXPECT_EQ(fileBytes(*parse), 0u) << "a reload read the file again: " << ModelCpuData::describeLastRead();
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// R11: each reader the analysis could not see gets the bytes back, once, and they stay
TEST_F(ModelCpuDataGpu, AReaderGetsTheBytesBackAndTheyStay){
    JASPER_REQUIRE_GPU();
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    const std::vector<MeshComponent*> meshes = meshesOf(parse);
    ASSERT_FALSE(meshes.empty());
    const size_t indices = triangleIndices(*meshes[0]);
    EXPECT_GT(indices, 0u);
    ModelCpuData::update({&scene}, true);
    ASSERT_EQ(fileBytes(*parse), 0u);

    // Mesh Collider, navigation bake: MeshTriangles
    EXPECT_EQ(triangleIndices(*meshes[0]), indices);
    EXPECT_GT(fileBytes(*parse), 0u);
    EXPECT_EQ(ModelCpuData::getReleasedBytes(), 0u);
    // picking and raycasts on another mesh of the file
    ASSERT_GE(meshes.size(), 2u);
    PickGeometryLayout layout;
    EXPECT_TRUE(picking::resolveSources(*meshes[1], layout));
    for (unsigned int b = 0; b < meshes[1]->numExternalBuffers; b++)
        EXPECT_FALSE(meshes[1]->eBuffers[b].isDataReleased());
    // never released again
    ModelCpuData::update({&scene}, true);
    EXPECT_GT(fileBytes(*parse), 0u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// R11: a later load of the file reads it again, and draws the same
TEST_F(ModelCpuDataGpu, ALaterLoadOfTheFileDraws){
    JASPER_REQUIRE_GPU();
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> before = frame();
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    ModelCpuData::update({&scene}, true);
    ASSERT_EQ(fileBytes(*parse), 0u);

    Model second(&scene);
    ASSERT_TRUE(second.loadModel(kAsset));
    second.setVisible(false);
    ASSERT_TRUE(settle());
    EXPECT_GT(fileBytes(*parse), 0u);
    EXPECT_EQ(differingPixels(before, frame()), 0u);

    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// R11: the GPU buffers gone with nothing else holding them (a lost context): the file is read again
TEST_F(ModelCpuDataGpu, ALostGpuBufferReadsTheFileAgain){
    JASPER_REQUIRE_GPU();
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> before = frame();
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    ModelCpuData::update({&scene}, true);
    ASSERT_EQ(fileBytes(*parse), 0u);
    for (MeshComponent* mesh : meshesOf(parse)){
        for (unsigned int b = 0; b < mesh->numExternalBuffers; b++)
            mesh->eBuffers[b].getRender()->destroyBuffer();
        mesh->needReload = true;
    }
    ASSERT_TRUE(settle());
    EXPECT_GT(fileBytes(*parse), 0u);
    EXPECT_EQ(differingPixels(before, frame()), 0u);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}

// R11: with a reader (keepCpuData) nothing goes, and the readers read
TEST_F(ModelCpuDataGpu, KeepCpuDataKeepsTheBytesForEachReader){
    JASPER_REQUIRE_GPU();
    keepCpuData = true;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    const uint64_t held = fileBytes(*parse);
    EXPECT_EQ(ModelCpuData::getPendingCount(), 0u);
    ModelCpuData::update({&scene}, true);
    EXPECT_EQ(fileBytes(*parse), held);
    const std::vector<MeshComponent*> meshes = meshesOf(parse);
    ASSERT_FALSE(meshes.empty());
    // Mesh Collider and navigation bake
    EXPECT_GT(triangleIndices(*meshes[0]), 0u);
    // picking
    PickGeometryLayout layout;
    EXPECT_TRUE(picking::resolveSources(*meshes[0], layout));
    // a particle mesh shape
    particles::ParticleMeshData shape;
    std::string error;
    EXPECT_TRUE(particles::ParticleMeshes::model(kAsset, 0, shape, &error)) << error;
    // runtime levels read the mesh's bytes
    MeshLodSource source;
    EXPECT_TRUE(RenderSystem::readSubmeshLodSource(*meshes[0], 0, source));
    EXPECT_FALSE(source.positions.empty());
    EXPECT_EQ(fileBytes(*parse), held);
}

// R11: a particle mesh shape reads the file itself, released or not
TEST_F(ModelCpuDataGpu, AParticleMeshShapeReadsTheFileAfterARelease){
    JASPER_REQUIRE_GPU();
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(kAsset));
    ASSERT_TRUE(settle());
    // (raw: a reference of the test's own would count as something still holding the parse)
    const tinygltf::Model* parse = model.getComponent<ModelComponent>().gltfModel.get();
    ModelCpuData::update({&scene}, true);
    ASSERT_EQ(fileBytes(*parse), 0u);
    particles::ParticleMeshData shape;
    std::string error;
    EXPECT_TRUE(particles::ParticleMeshes::model(kAsset, 0, shape, &error)) << error;
    EXPECT_EQ(fileBytes(*parse), 0u);
}

// R11: the editor always keeps them, whatever the index says
TEST_F(ModelCpuDataGpu, TheEditorNeverReleases){
    JASPER_REQUIRE_GPU();
    auto parse = std::make_shared<tinygltf::Model>();
    EditorLikeSystem editor;
    System* previous = SystemTestAccess::swap(&editor);
    ModelCpuData::markReleasable(parse, kAsset, "editor-key");
    SystemTestAccess::swap(previous);
    EXPECT_EQ(ModelCpuData::getPendingCount(), 0u);
}

// AC7: a golden of the cook at Medium, which its source's image matches within the golden's limits
TEST_F(ModelCpuDataGpu, MediumCookDrawsWhatItsSourceDraws){
    JASPER_REQUIRE_GPU();
    Model cooked(&scene);
    ASSERT_TRUE(cooked.loadModel(s41Fixture("cooked_medium.glb")));
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(2);
    EXPECT_GOLDEN("model_cook_medium");
    cooked.setVisible(false);

    Model source(&scene);
    ASSERT_TRUE(source.loadModel(s41Fixture("source.glb")));
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(2);
    if (!std::getenv("JASPER_TEST_UPDATE_GOLDEN")){
        EXPECT_GOLDEN("model_cook_medium");
    }
}
