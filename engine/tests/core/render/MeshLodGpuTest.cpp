// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Mesh detail levels through the renderer (spec S54): builds that cannot stick pending (R4, AC4),
// a cooked model's baked levels (R3, AC5), and the dithered cross-fade (R6, AC2; lod-crossfade.md
// §5 tests 9-11): complementary levels, the mid-fade golden, the fade variant only while fading.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/MeshLodPool.h"
#include "pool/ShaderPool.h"
#include "device/QualityResolver.h"
#include "device/DeviceSettings.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class MeshLodGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.05f, 0.05f, 0.25f, 1.0f);
            scene.setMeshLodEnabled(true);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 6.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
            Engine::setFixedDeltatime(1.0 / 60.0);
        }

        void TearDown() override{
            Engine::setFixedDeltatime(0.0);
            Engine::removeScene(&scene);
        }

        RenderSystem& render(){
            return *scene.getSystem<RenderSystem>();
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // draws frames until no mesh waits for its levels (false after maxFrames)
        bool settleLevels(int maxFrames){
            for (int i = 0; i < maxFrames; i++){
                GpuEnvironment::drawFrames(1);
                if (!render().hasPendingMeshLods())
                    return true;
            }
            return false;
        }
    };

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }
}

// R4, AC4: a source the builder refuses (an index past the vertices) settles to no levels
// (red before S54: the submesh stayed pending, rebuilt its source every frame)
TEST_F(MeshLodGpu, ARefusedSourceSettlesWithoutLevels){
    JASPER_REQUIRE_GPU();
    scene.setMeshLodEnabled(false);
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 32, 16);
    ASSERT_TRUE(settle());

    // the GPU keeps the valid indices it was given; the CPU copy the levels are built from breaks
    MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    Attribute* index = mesh.indices.getAttribute(AttributeType::INDEX);
    ASSERT_NE(index, nullptr);
    const uint16_t past = 0xFFFE;
    std::memcpy(mesh.indices.getData() + index->getOffset() + 2 * sizeof(uint16_t), &past, sizeof(past));
    scene.setMeshLodEnabled(true);

    EXPECT_TRUE(settleLevels(120)) << "the refused source stayed pending";
    EXPECT_EQ(mesh.numLods, 1u);
}

// R4, AC4: a build the editor's pool sweep (MeshLodPool::clearUnused) dropped after it finished
// and before the submesh took it is started again (red before S54: pending forever)
TEST_F(MeshLodGpu, ABuildDroppedBeforeItWasTakenIsBuiltAgain){
    JASPER_REQUIRE_GPU();
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 40, 20);
    sphere.setPosition(0.0f, 0.0f, -0.25f);   // geometry of its own (not another test's pool entry)
    MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    // loads it, and submits the build
    for (int i = 0; i < 300 && mesh.submeshes[0].lodKey == 0; i++)
        GpuEnvironment::drawFrames(1);
    const uint64_t key = mesh.submeshes[0].lodKey;
    ASSERT_NE(key, 0u);
    if (!mesh.submeshes[0].lodData){
        // the build finishes without the submesh polling it, then the sweep drops it
        ASSERT_TRUE(MeshLodPool::wait(key));
        ASSERT_TRUE(MeshLodPool::get(key));
        MeshLodPool::clearUnused();
        ASSERT_FALSE(MeshLodPool::has(key));
    }
    EXPECT_TRUE(settleLevels(600)) << "the dropped build stayed pending";
    EXPECT_GT(mesh.numLods, 1u);
}

namespace {
    // the data type of a submesh's attribute, of its own or of a buffer it reads (as the level
    // builder finds it); false when it has none
    bool attributeTypeOf(MeshComponent& mesh, unsigned int submesh, AttributeType type, AttributeDataType& out){
        auto own = mesh.submeshes[submesh].attributes.find(type);
        if (own != mesh.submeshes[submesh].attributes.end()){
            out = own->second.getDataType();
            return true;
        }
        for (int b = 0; b < mesh.numExternalBuffers; b++){
            if (Attribute* attribute = mesh.eBuffers[b].getAttribute(type)){
                out = attribute->getDataType();
                return true;
            }
        }
        if (Attribute* attribute = mesh.buffer.getAttribute(type)){
            out = attribute->getDataType();
            return true;
        }
        return false;
    }

    // a grid with core glTF's normalized 16-bit UVs (u = x / n, v = y / n) and no texture transform,
    // which would bake them to floats
    std::string writeNormalizedUvGrid(TempDir& dir, int n){
        std::vector<float> positions;
        std::vector<uint16_t> uvs;
        for (int y = 0; y <= n; y++){
            for (int x = 0; x <= n; x++){
                positions.insert(positions.end(), {-1.0f + 2.0f * x / n, -1.0f + 2.0f * y / n, 0.0f});
                uvs.insert(uvs.end(), {(uint16_t)(65535 * x / n), (uint16_t)(65535 * y / n)});
            }
        }
        std::vector<uint16_t> indices;
        for (int y = 0; y < n; y++){
            for (int x = 0; x < n; x++){
                const uint16_t a = (uint16_t)(y * (n + 1) + x), b = (uint16_t)(a + n + 1);
                indices.insert(indices.end(), {a, (uint16_t)(a + 1), b, b, (uint16_t)(a + 1), (uint16_t)(b + 1)});
            }
        }
        const size_t positionBytes = positions.size() * 4, uvBytes = uvs.size() * 2, indexBytes = indices.size() * 2;
        {
            std::ofstream out(dir.file("uvgrid.bin"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(positions.data()), (std::streamsize)positionBytes);
            out.write(reinterpret_cast<const char*>(uvs.data()), (std::streamsize)uvBytes);
            out.write(reinterpret_cast<const char*>(indices.data()), (std::streamsize)indexBytes);
        }
        const int vertices = (n + 1) * (n + 1);
        std::ofstream(dir.file("uvgrid.gltf")) << R"({
"asset": {"version": "2.0"},
"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2}]}],
"buffers": [{"uri": "uvgrid.bin", "byteLength": )" << positionBytes + uvBytes + indexBytes << R"(}],
"bufferViews": [
  {"buffer": 0, "byteOffset": 0, "byteLength": )" << positionBytes << R"(},
  {"buffer": 0, "byteOffset": )" << positionBytes << R"(, "byteLength": )" << uvBytes << R"(, "byteStride": 4},
  {"buffer": 0, "byteOffset": )" << positionBytes + uvBytes << R"(, "byteLength": )" << indexBytes << R"(}],
"accessors": [
  {"bufferView": 0, "componentType": 5126, "count": )" << vertices << R"(, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
  {"bufferView": 1, "componentType": 5123, "normalized": true, "count": )" << vertices << R"(, "type": "VEC2"},
  {"bufferView": 2, "componentType": 5123, "count": )" << indices.size() << R"(, "type": "SCALAR"}]
})";
        return dir.file("uvgrid.gltf").generic_string();
    }
}

// R2: a quantized model's 8-bit normals (S53 keeps them normalized on the GPU) guide the
// simplification like float ones (red before: the builder left them out). This gltfpack file's
// 16-bit UVs reach the mesh as floats: its base colour texture has a KHR_texture_transform, which
// the loader bakes into them; the next test covers normalized UVs
TEST_F(MeshLodGpu, AQuantizedModelsAttributesGuideItsLevels){
    JASPER_REQUIRE_GPU();
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(JASPER_TEST_SOURCE_DIR "/fixtures/gltf/s41/quantized.glb"));
    ASSERT_TRUE(settle());
    bool found = false;
    for (Entity entity : scene.getEntityList()){
        MeshComponent* mesh = scene.findComponent<MeshComponent>(entity);
        if (!mesh)
            continue;
        for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
            AttributeDataType normalType, uvType;
            if (!attributeTypeOf(*mesh, s, AttributeType::NORMAL, normalType) || normalType != AttributeDataType::BYTE ||
                    !attributeTypeOf(*mesh, s, AttributeType::TEXCOORD1, uvType))
                continue;
            found = true;
            MeshLodSource source;
            ASSERT_TRUE(RenderSystem::readSubmeshLodSource(*mesh, s, source));
            const size_t vertices = source.positions.size() / 3;
            ASSERT_EQ(source.normals.size(), vertices * 3) << "the 8-bit normals guide the collapses";
            ASSERT_EQ(source.uvs.size(), vertices * 2) << "the UVs guide the collapses";
            for (size_t v = 0; v < vertices; v++){
                const float* n = &source.normals[v * 3];
                EXPECT_NEAR(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1.0f, 0.03f) << "vertex " << v;
            }
            EXPECT_EQ(MeshLodPool::buildLevels(source, MeshLodRecipe::runtime())->numLevels > 1, mesh->numLods > 1);
        }
    }
    EXPECT_TRUE(found) << "the fixture's textured mesh keeps its 8-bit normals";
}

// R2: normalized 16-bit UVs are decoded for the simplifier, to the values the shader reads
TEST_F(MeshLodGpu, NormalizedUvsGuideTheLevels){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const int n = 8;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(writeNormalizedUvGrid(dir, n)));
    ASSERT_TRUE(settle());
    MeshComponent& mesh = model.getComponent<MeshComponent>();
    AttributeDataType uvType;
    ASSERT_TRUE(attributeTypeOf(mesh, 0, AttributeType::TEXCOORD1, uvType));
    ASSERT_EQ(uvType, AttributeDataType::UNSIGNED_SHORT) << "the loader keeps them normalized";
    MeshLodSource source;
    ASSERT_TRUE(RenderSystem::readSubmeshLodSource(mesh, 0, source));
    const size_t vertices = source.positions.size() / 3;
    ASSERT_EQ(vertices, (size_t)((n + 1) * (n + 1)));
    ASSERT_EQ(source.uvs.size(), vertices * 2) << "the 16-bit UVs guide the collapses";
    for (size_t v = 0; v < vertices; v++){
        EXPECT_NEAR(source.uvs[v * 2], (source.positions[v * 3] + 1.0f) * 0.5f, 1e-3f) << "vertex " << v;
        EXPECT_NEAR(source.uvs[v * 2 + 1], (source.positions[v * 3 + 1] + 1.0f) * 0.5f, 1e-3f) << "vertex " << v;
    }
}

namespace {
    // a flat grid of quads with a baked JE_mesh_lods level of two triangles (exact: error 0)
    std::string writeBakedGrid(TempDir& dir, int recipe){
        const int n = 8;
        std::vector<float> positions;
        std::vector<float> normals;
        for (int y = 0; y <= n; y++){
            for (int x = 0; x <= n; x++){
                positions.insert(positions.end(), {-1.0f + 2.0f * x / n, -1.0f + 2.0f * y / n, 0.0f});
                normals.insert(normals.end(), {0.0f, 0.0f, 1.0f});
            }
        }
        std::vector<uint16_t> indices;
        for (int y = 0; y < n; y++){
            for (int x = 0; x < n; x++){
                const uint16_t a = (uint16_t)(y * (n + 1) + x), b = (uint16_t)(a + n + 1);
                indices.insert(indices.end(), {a, (uint16_t)(a + 1), b, b, (uint16_t)(a + 1), (uint16_t)(b + 1)});
            }
        }
        const uint16_t corner = (uint16_t)n, top = (uint16_t)(n * (n + 1)), last = (uint16_t)((n + 1) * (n + 1) - 1);
        const std::vector<uint16_t> level = {0, corner, top, top, corner, last};
        const size_t positionBytes = positions.size() * 4, normalBytes = normals.size() * 4;
        const size_t indexBytes = indices.size() * 2, levelBytes = level.size() * 2;
        {
            std::ofstream out(dir.file("grid.bin"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(positions.data()), (std::streamsize)positionBytes);
            out.write(reinterpret_cast<const char*>(normals.data()), (std::streamsize)normalBytes);
            out.write(reinterpret_cast<const char*>(indices.data()), (std::streamsize)indexBytes);
            out.write(reinterpret_cast<const char*>(level.data()), (std::streamsize)levelBytes);
        }
        const int vertices = (n + 1) * (n + 1);
        std::ofstream(dir.file("grid.gltf")) << R"({
"asset": {"version": "2.0"},
"extensionsUsed": ["JE_mesh_lods"],
"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
  "extensions": {"JE_mesh_lods": {"recipe": )" << recipe << R"(, "levels": [{"indices": 3, "error": 0.0}]}}}]}],
"buffers": [{"uri": "grid.bin", "byteLength": )" << positionBytes + normalBytes + indexBytes + levelBytes << R"(}],
"bufferViews": [
  {"buffer": 0, "byteOffset": 0, "byteLength": )" << positionBytes << R"(},
  {"buffer": 0, "byteOffset": )" << positionBytes << R"(, "byteLength": )" << normalBytes << R"(},
  {"buffer": 0, "byteOffset": )" << positionBytes + normalBytes << R"(, "byteLength": )" << indexBytes << R"(},
  {"buffer": 0, "byteOffset": )" << positionBytes + normalBytes + indexBytes << R"(, "byteLength": )" << levelBytes << R"(}],
"accessors": [
  {"bufferView": 0, "componentType": 5126, "count": )" << vertices << R"(, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
  {"bufferView": 1, "componentType": 5126, "count": )" << vertices << R"(, "type": "VEC3"},
  {"bufferView": 2, "componentType": 5123, "count": )" << indices.size() << R"(, "type": "SCALAR"},
  {"bufferView": 3, "componentType": 5123, "count": )" << level.size() << R"(, "type": "SCALAR"}]
})";
        return dir.file("grid.gltf").generic_string();
    }
}

// R3, AC5: a cooked model's baked levels of this builder's recipe fill the pool: no simplify job
TEST_F(MeshLodGpu, ACookedModelsBakedLevelsNeedNoBuild){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const std::string file = writeBakedGrid(dir, kMeshLodRecipe);
    const uint64_t builds = MeshLodPool::getBuildCount();
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(file));
    ASSERT_TRUE(settle());
    MeshComponent& mesh = model.getComponent<MeshComponent>();
    ASSERT_TRUE(mesh.submeshes[0].lodData);
    EXPECT_TRUE(mesh.submeshes[0].lodData->baked);
    EXPECT_EQ(mesh.numLods, 2u);
    EXPECT_EQ(mesh.submeshes[0].lodData->levels[1].indexCount, 6u);
    EXPECT_EQ(MeshLodPool::getBuildCount(), builds) << "a simplify job ran for a cooked model";
    EXPECT_NO_ENGINE_ERRORS();
}

// the control: levels baked by another builder version are rebuilt at runtime
TEST_F(MeshLodGpu, BakedLevelsOfAnotherRecipeAreRebuilt){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const std::string file = writeBakedGrid(dir, kMeshLodRecipe + 1000);
    const uint64_t builds = MeshLodPool::getBuildCount();
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(file));
    ASSERT_TRUE(settle());
    MeshComponent& mesh = model.getComponent<MeshComponent>();
    ASSERT_TRUE(mesh.submeshes[0].lodData);
    EXPECT_FALSE(mesh.submeshes[0].lodData->baked);
    EXPECT_GT(MeshLodPool::getBuildCount(), builds);
}

namespace {

    // A sphere whose level changes once by a step of its LOD Bias under 2x (a bigger jump in a
    // frame is a camera cut, which snaps), the fade clock at 1/60 s a frame
    class LodFadeGpu : public MeshLodGpu {
    protected:
        Primitive sphere{&scene};
        Light sun{&scene};
        float biasFine = 1.0f;
        float biasCoarse = 1.0f;

        void build(bool lit){
            scene.setMeshLodCrossFade(true);
            scene.setMeshLodCrossFadeDuration(0.5f);
            sphere.createSphere(1.5f, 48, 24);
            sphere.setColor(0.85f, 0.55f, 0.2f);
            sphere.setReceiveLights(lit);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.6f);
            sun.setIntensity(1.5f);
            sun.setShadows(false);
            // a cold cache: what the sphere draws with builds in the background, its fade variant
            // too, whose program a later ShaderPool::get makes (waited for, not a count of frames)
            struct AsyncCompile{
                bool before = ShaderPool::isAsyncCompilation();
                AsyncCompile(){ ShaderPool::setAsyncCompilation(true); }
                ~AsyncCompile(){ ShaderPool::setAsyncCompilation(before); }
            } asyncCompile;
            ASSERT_TRUE(settle());

            // the budget the main view gives the sphere: threshold x pixel size at its distance
            MeshComponent& mesh = sphere.getComponent<MeshComponent>();
            ASSERT_GT(mesh.numLods, 2u);
            const CameraComponent& cameraComponent = camera.getComponent<CameraComponent>();
            const float viewHeight = (float)GpuEnvironment::target().getHeight();
            const float distance = mesh.worldAABB.distance(Vector3(0.0f, 0.0f, 6.0f));
            const float budget = scene.getMeshLodThreshold() * 2.0f * std::tan(cameraComponent.yfov * 0.5f) / viewHeight * distance;
            const float e1 = mesh.lodError[1], e2 = mesh.lodError[2];
            ASSERT_GT(e2, 1.6f * e1) << "the step must reach level 1 and stay there";
            biasFine = budget / (0.8f * e1);
            biasCoarse = budget / (1.25f * e1);
            sphere.setLodBias(biasFine);
            MeshComponent* fading = &mesh;
            const bool made = GpuEnvironment::runFrames([fading](){ return fading->lodFadeReady; }, 600);
            ASSERT_TRUE(made) << "the fade variant was never made";
            ASSERT_EQ(sphere.getCurrentLod(), 0);
        }

        uint32_t appliedPipeline(){
            return sphere.getComponent<MeshComponent>().submeshes[0].render.getAppliedPipelineId();
        }
    };
}

// tests 10 and 11, AC2: mid-fade and done goldens; the fade variant only while fading, the steady
// draws on their own pipeline (no discard), and no fade draw in a still scene
TEST_F(LodFadeGpu, ALevelChangeCrossFades){
    JASPER_REQUIRE_GPU();
    build(true);
    const uint32_t steady = appliedPipeline();
    EXPECT_EQ(render().getLodStats().fadeDraws, 0u);
    const MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.submeshes[0].shaderProperties & ShaderPool::getLodFadeProperty(), 0u) << "the steady variant has no dither";

    sphere.setLodBias(biasCoarse);
    GpuEnvironment::drawFrames(15);
    // the dither's phase is the backend's: GL counts pixels from the bottom left, the others from
    // the top left (lod-crossfade.md §4.1), so the mid-fade image is compared per 8x8 tile, which
    // holds every rank of the pattern once on every backend
    CompareOptions tiles;
    tiles.blockSize = 8;
    EXPECT_GOLDEN_OPTIONS("lod_crossfade_mid", tiles);
    EXPECT_NE(appliedPipeline(), steady) << "a fading draw uses the fade variant";
    EXPECT_EQ(render().getLodStats().fading, 1u);
    EXPECT_EQ(render().getLodStats().fadeDraws, 2u) << "one extra draw per fading submesh";
    EXPECT_NE(mesh.submeshes[0].lodFadeShaderProperties & ShaderPool::getLodFadeProperty(), 0u);

    GpuEnvironment::drawFrames(17);
    EXPECT_EQ(sphere.getCurrentLod(), 1);
    EXPECT_EQ(appliedPipeline(), steady) << "back on the steady pipeline after the fade";
    const std::vector<uint8_t> done = readTarget();
    EXPECT_GOLDEN("lod_crossfade_done");
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(render().getLodStats().fadeDraws, 0u);

    // the end of a fade is the level drawn alone, pixel for pixel
    sphere.forceLod(1);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(readTarget(), done);
    EXPECT_NO_ENGINE_ERRORS();
}

// test 9: mid-fade, every pixel both levels cover is drawn by one of them (no hole); red control:
// both sides keeping the same ranks leaves holes
TEST_F(LodFadeGpu, TheTwoLevelsOfAFadeLeaveNoHole){
    JASPER_REQUIRE_GPU();
    build(false);
    MeshComponent& mesh = sphere.getComponent<MeshComponent>();
    const std::vector<uint8_t> background = [&](){
        sphere.setVisible(false);
        GpuEnvironment::drawFrames(2);
        std::vector<uint8_t> image = readTarget();
        sphere.setVisible(true);
        return image;
    }();
    auto coverage = [&](int level){
        sphere.forceLod(level);
        GpuEnvironment::drawFrames(2);
        sphere.forceLod(-1);
        return readTarget();
    };
    const std::vector<uint8_t> fine = coverage(0);
    const std::vector<uint8_t> coarse = coverage(1);
    // back on level 0 (a fade from the forced level), then the step
    GpuEnvironment::drawFrames(40);
    ASSERT_EQ(sphere.getCurrentLod(), 0);
    ASSERT_FALSE(mesh.lodFade.plan.fading());
    sphere.setLodBias(biasCoarse);
    GpuEnvironment::drawFrames(15);
    ASSERT_TRUE(mesh.lodFade.plan.fading());
    const std::vector<uint8_t> mid = readTarget();

    size_t both = 0, holes = 0;
    auto covered = [&](const std::vector<uint8_t>& image, size_t p){
        return std::abs((int)image[p] - (int)background[p]) > 8 || std::abs((int)image[p + 2] - (int)background[p + 2]) > 8;
    };
    for (size_t p = 0; p + 3 < mid.size(); p += 4){
        if (covered(fine, p) && covered(coarse, p)){
            both++;
            if (!covered(mid, p))
                holes++;
        }
    }
    ASSERT_GT(both, 1000u);
    EXPECT_EQ(holes, 0u) << "pixels both levels cover that neither drew";
}

// C4: the device tier's LOD Bias (QualityResolver, S56) multiplies into the selection once
TEST_F(LodFadeGpu, TheDeviceTiersLodBiasReachesTheSelection){
    JASPER_REQUIRE_GPU();
    build(true);
    struct Restore{ ~Restore(){ QualityResolver::reset(); } } restore;
    // a tier (Ultra: otherwise the project) halving the LOD Bias: twice the error is hidden, so the
    // sphere drops to LOD 1
    DeviceSettings settings;
    settings.tier(QualityResolver::getPlatformGroup(), DeviceTier::Ultra).set(DeviceOverride::LodBias, 0.5f);
    QualityResolver::setSettings(settings);
    QualityResolver::setTier(DeviceTier::Ultra);
    ASSERT_FLOAT_EQ(QualityResolver::lodBias(), 0.5f);
    GpuEnvironment::drawFrames(40);   // (past a fade)
    EXPECT_EQ(sphere.getCurrentLod(), 1) << "the tier's bias reached the selection";
    // the project's own LOD Bias multiplies with it: 2 x 0.5 is the sphere's LOD 0 again
    settings.lodBias = 2.0f;
    QualityResolver::setSettings(settings);
    GpuEnvironment::drawFrames(40);   // (past a fade)
    EXPECT_EQ(sphere.getCurrentLod(), 0) << "one multiplication of both biases, not two";
}
