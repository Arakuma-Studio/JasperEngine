// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water's PLANAR_REFLECTION on the headless device (docs/superpowers/specs/
// 2026-11-04-water-reflections-design.md 6.4): the camera's reflection pass mirrors the scene in
// the water, one pass per water height (nearest first up to Max Planes), none with the scene's
// resolution Off (the environment reflection instead), for perspective and orthographic cameras.
// The water is a black mirror (opaque, its reflection unbent), so the pixels it draws are its
// reflection times Schlick's fresnel. Each pass draws the terrain cut of its own mirrored camera,
// the frame rewrites the shared MVPs per camera only while a reflection pass draws, and the
// passes' targets are the size the viewer's pass draws at (fixed resolution, render scale).

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "pool/TextureDataPool.h"
#include "render/SystemRender.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr double REFLECTION_PINNED_TIME = 2.0;

    // runs its function when the test leaves its scope, an ASSERT's early return too
    struct ReflectionRestore{
        std::function<void()> restore;
        explicit ReflectionRestore(std::function<void()> f): restore(std::move(f)){}
        ~ReflectionRestore(){ restore(); }
        ReflectionRestore(const ReflectionRestore&) = delete;
        ReflectionRestore& operator=(const ReflectionRestore&) = delete;
    };
    constexpr int REFLECTION_TERRAIN_TEXELS = 33;
    const char* const REFLECTION_TERRAIN_HEIGHTS = "water-reflection-terrain-height";

    struct ReflectionPixel{ int r = 0, g = 0, b = 0; };

    // the mean colour of a 7 x 7 block around where a world point is drawn
    ReflectionPixel reflectionPixelAt(Scene& scene, const Vector3& world){
        Framebuffer& target = GpuEnvironment::target();
        const int width = (int)target.getWidth();
        const int height = (int)target.getHeight();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), width, height, rgba);
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        const Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
        const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
        ReflectionPixel sum;
        int count = 0;
        for (int y = cy - 3; y <= cy + 3; y++){
            for (int x = cx - 3; x <= cx + 3; x++){
                if (x < 0 || y < 0 || x >= width || y >= height)
                    continue;
                const size_t i = ((size_t)y * width + x) * 4;
                sum.r += rgba[i];
                sum.g += rgba[i + 1];
                sum.b += rgba[i + 2];
                count++;
            }
        }
        if (count > 0){
            sum.r /= count;
            sum.g /= count;
            sum.b /= count;
        }
        return sum;
    }

    // a black, opaque mirror: REFLECTION with PLANAR_REFLECTION, white reflection colour, no foam
    Material mirrorWater(){
        Material material;
        material.name = "Mirror water";
        material.shader = "library:water";
        material.shaderFeatures = {"REFLECTION", "PLANAR_REFLECTION"};
        ShaderUniforms::set(material.shaderParams, "shallowColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "deepColor", Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "foamAmount", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        ShaderUniforms::set(material.shaderParams, "reflectionColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        ShaderUniforms::set(material.shaderParams, "reflectionDistortion", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        return material;
    }

    void makeRedCube(Primitive& cube){
        cube.createBox(1.0f, 1.0f, 1.0f);
        Material material;
        material.name = "Red";
        material.shader = "library:unlit";
        ShaderUniforms::set(material.shaderParams, "baseColor", Vector4(1.0f, 0.0f, 0.0f, 1.0f));
        cube.setMaterial(material);
    }

    class WaterReflectionGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            // the view the goldens and pixel positions were made at
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(REFLECTION_PINNED_TIME);
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        RenderSystem::PlanarReflectionStats stats(){
            return scene.getSystem<RenderSystem>()->getPlanarReflectionStats();
        }
    };

    void printReflectionPixel(const char* what, const ReflectionPixel& pixel){
        std::printf("[gpu] %s: (%d, %d, %d) on %s\n", what, pixel.r, pixel.g, pixel.b, HeadlessContext::getBackendName());
    }

}

// the red cube above the water shows in it at its mirrored place; Off draws the environment
TEST_F(WaterReflectionGpu, PlanarReflectionMirrorsTheScene){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.6f, 8.0f);
    camera.setTarget(0.0f, -0.2f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());

    Primitive cube(&scene);
    makeRedCube(cube);
    cube.setPosition(0.0f, 1.0f, 0.0f);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel mirrored = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored", mirrored);
    EXPECT_GT(mirrored.r, 60) << "no reflection of the cube in the water";
    EXPECT_GT(mirrored.r, 2 * std::max(mirrored.g, mirrored.b) + 10);
    const RenderSystem::PlanarReflectionStats drawn = stats();
    EXPECT_EQ(drawn.planes, 1u);
    EXPECT_EQ(drawn.passes, 1u);
    EXPECT_EQ(drawn.fallbacks, 0u);
    // (compareTargetWithGolden writes the actual image on a mismatch)
    const CompareResult golden = compareTargetWithGolden("water_planar_reflection", hardwareTolerance({24, 0.01, 2.0}));
    EXPECT_TRUE(golden.passed) << golden.message;

    // Off: no pass, the environment (without a sky, the reflection colour alone): the pixels
    // REFLECTION alone draws
    scene.setPlanarReflectionResolution(PlanarReflectionResolution::OFF);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel off = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("Off", off);
    EXPECT_EQ(stats().passes, 0u);
    Material reflectionAlone = mirrorWater();
    reflectionAlone.shaderFeatures = {"REFLECTION"};
    water.setMaterial(reflectionAlone);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel alone = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("REFLECTION alone", alone);
    EXPECT_LE(std::abs(off.r - alone.r), 2) << "Off does not draw what REFLECTION alone draws";
    EXPECT_LE(std::abs(off.g - alone.g), 2);
    EXPECT_LE(std::abs(off.b - alone.b), 2);
    EXPECT_NO_ENGINE_ERRORS();
}

// one pass per height: two tiles at one level share it; past Max Planes the farthest height
// falls back
TEST_F(WaterReflectionGpu, FallbackWhenOffOrCapped){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 3.0f, 10.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive left(&scene);
    left.createPlane(4.0f, 4.0f);
    left.setPosition(-2.5f, 0.0f, 0.0f);
    left.setMaterial(mirrorWater());
    Primitive right(&scene);
    right.createPlane(4.0f, 4.0f);
    right.setPosition(2.5f, 0.0f, 0.0f);
    right.setMaterial(mirrorWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 1u) << "two tiles at one level are one height";
    EXPECT_EQ(stats().passes, 1u);

    // two more heights, a cap of two: the farthest from the eye shows the environment
    Primitive lower(&scene);
    lower.createPlane(4.0f, 4.0f);
    lower.setPosition(0.0f, -1.0f, 4.0f);
    lower.setMaterial(mirrorWater());
    Primitive lowest(&scene);
    lowest.createPlane(4.0f, 4.0f);
    lowest.setPosition(0.0f, -2.0f, -4.0f);
    lowest.setMaterial(mirrorWater());
    scene.setMaxPlanarReflections(2);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 3u);
    EXPECT_EQ(stats().passes, 2u);
    EXPECT_EQ(stats().fallbacks, 1u);
    // the farthest height shows the environment (grey, the reflection colour alone); a reflected
    // one the empty, black scene
    const ReflectionPixel fallback = reflectionPixelAt(scene, Vector3(0.0f, -2.0f, -4.0f));
    const ReflectionPixel reflected = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 4.0f));
    printReflectionPixel("the capped height", fallback);
    printReflectionPixel("a reflected height", reflected);
    EXPECT_GT(fallback.r, reflected.r + 20) << "the capped height does not show the environment";
    EXPECT_LT(std::abs(fallback.r - fallback.b), 8);

    // seen from below a surface: no pass for it
    camera.setPosition(0.0f, -1.5f, 10.0f);
    camera.setTarget(0.0f, -1.5f, 0.0f);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_EQ(stats().planes, 1u) << "only the lowest water is under the eye";
    EXPECT_NO_ENGINE_ERRORS();
}

// an orthographic camera reflects the same way
TEST_F(WaterReflectionGpu, OrthographicViewer){
    JASPER_REQUIRE_GPU();
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 2.0f, 8.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());

    Primitive cube(&scene);
    makeRedCube(cube);
    cube.setPosition(0.0f, 1.0f, 0.0f);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    const ReflectionPixel mirrored = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored (orthographic)", mirrored);
    EXPECT_EQ(stats().passes, 1u);
    EXPECT_GT(mirrored.r, 2 * std::max(mirrored.g, mirrored.b) + 10) << "no reflection of the cube for an orthographic camera";
    EXPECT_NO_ENGINE_ERRORS();
}

// two heights, two passes: each selects the terrain's node cut for its own mirrored camera, in a
// terrain view of its own, and draws that view (a view's node buffer is uploaded once a frame:
// one view shared by the passes draws the last pass's cut in all of them on Vulkan, Metal and
// D3D11). The terrain is the only thing a reflection pass draws here, so its instances are the
// nodes of the cut it drew.
TEST_F(WaterReflectionGpu, EachPassDrawsTheTerrainOfItsOwnCamera){
    JASPER_REQUIRE_GPU();
    ReflectionRestore poolEntry([](){ TextureDataPool::remove(REFLECTION_TERRAIN_HEIGHTS); });
    const Vector3 eye(0.0f, 12.0f, 20.0f);
    camera.setPosition(eye.x, eye.y, eye.z);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    // a flat terrain at height 0 (its heights in memory: the pool holds a pointer to them)
    static std::vector<unsigned char> heights((size_t)REFLECTION_TERRAIN_TEXELS * REFLECTION_TERRAIN_TEXELS * 2, 0);
    Terrain terrain(&scene);
    terrain.setSize(64.0f);
    terrain.setMaxHeight(1.0f);
    terrain.setColor(0.4f, 0.5f, 0.3f);
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
    component.heightMap.setData(REFLECTION_TERRAIN_HEIGHTS, TextureData(REFLECTION_TERRAIN_TEXELS, REFLECTION_TERRAIN_TEXELS,
                                (unsigned int)heights.size(), ColorFormat::RED16, 1, heights.data()));
    component.heightMap.setReleaseDataAfterLoad(false);
    component.needUpdateTexture = true;
    component.needUpdateTerrain = true;

    // two waters under it (the terrain is above both planes, so both reflections hold it), at
    // heights far apart: their mirrored cameras, 14 and 32 m under the terrain, select other cuts
    Primitive shallow(&scene);
    shallow.createPlane(6.0f, 6.0f);
    shallow.setPosition(-4.0f, -1.0f, 0.0f);
    shallow.setMaterial(mirrorWater());
    Primitive deep(&scene);
    deep.createPlane(20.0f, 20.0f);
    deep.setPosition(6.0f, -10.0f, 0.0f);
    deep.setMaterial(mirrorWater());

    ASSERT_TRUE(render()) << "the scene never finished loading";
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(component.heightMapLoaded) << "the terrain never loaded";
    ASSERT_EQ(stats().passes, 2u) << "the two heights are not two passes";

    // the reflection passes of the last frame, in the order they drew (the nearest height first)
    const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
    std::vector<uint32_t> drawnInstances;
    for (const PassRecord& pass : list.passes){
        if (pass.executed && std::strcmp(pass.label, "camera rtt") == 0)
            drawnInstances.push_back(pass.instances);
    }
    ASSERT_EQ(drawnInstances.size(), 2u) << "not two reflection passes in the pass list";

    // each pass's camera (the eye mirrored about its height), the view holding its cut, the nodes
    // of that cut, and the terrain instances the pass drew
    const float levels[2] = {-1.0f, -10.0f};
    uint32_t cutNodes[2] = {0, 0};
    for (int p = 0; p < 2; p++){
        const Vector3 mirrored(eye.x, 2.0f * levels[p] - eye.y, eye.z);
        int selected = -1;
        for (int v = 1; v < MAX_TERRAIN_VIEWS; v++){
            if ((component.views[v].nodesEyePos - mirrored).length() < 1e-3f)
                selected = v;
        }
        ASSERT_NE(selected, -1) << "no terrain view holds the cut of the reflection camera at (" << mirrored.x << ", "
                                << mirrored.y << ", " << mirrored.z << ")";
        for (int b = 0; b < 2; b++)
            cutNodes[p] += (uint32_t)component.views[selected].nodesbuffer[b].getCount();
        std::printf("[gpu] reflection pass %d: its camera's cut in terrain view %d, %u nodes; the pass drew %u instances\n",
                    p, selected, cutNodes[p], drawnInstances[p]);
    }
    // (the pass list counts a draw of no instance as one: the terrain's two node buffers, one may be empty)
    auto apart = [](uint32_t a, uint32_t b){ return a > b ? a - b : b - a; };
    auto near = [&apart](uint32_t a, uint32_t b){ return apart(a, b) <= 2u; };
    ASSERT_GT(apart(cutNodes[0], cutNodes[1]), 4u) << "the two cameras select cuts of about one size: the test cannot tell them apart";
    EXPECT_TRUE(near(drawnInstances[0], cutNodes[0])) << "the first reflection pass did not draw its own camera's cut";
    EXPECT_TRUE(near(drawnInstances[1], cutNodes[1])) << "the second reflection pass did not draw its own camera's cut";
    EXPECT_NO_ENGINE_ERRORS();
}

// the MVPs are made per camera only in a frame that draws a reflection pass: with the planar
// water in view but seen from below (the eye under its surface: no reflection pass) a batched
// mesh's MVP waits for a draw alone, as in any one-camera frame (dynamic batching's lazy MVP), so
// a camera move leaves it
TEST_F(WaterReflectionGpu, MVPsPerCameraOnlyWhileAReflectionDraws){
    JASPER_REQUIRE_GPU();
    const bool batching = Engine::isDynamicBatching();
    ReflectionRestore batchingMode([batching](){ Engine::setDynamicBatching(batching); });
    Engine::setDynamicBatching(true);
    camera.setPosition(0.0f, -2.0f, 8.0f);
    camera.setTarget(0.0f, -2.0f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());

    // two cards of one texture (dynamic batching's sprites, of two sizes: no GPU instancing)
    static std::vector<uint8_t> pixels(4 * 4 * 4, 200);
    const TextureData card(4, 4, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    Sprite left(&scene);
    left.setTexture("water-reflection-card", card);
    left.setSize(1, 1);
    left.setPosition(-1.5f, -2.5f, 0.0f);
    Sprite right(&scene);
    right.setTexture("water-reflection-card", card);
    right.setSize(2, 1);
    right.setPosition(0.5f, -2.5f, 0.0f);

    ASSERT_TRUE(render()) << "the scene never finished loading";
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(scene.getSystem<RenderSystem>()->hasBatchRecord(left.getEntity()))
        << "the cards are not batched: " << getBatchBreakName(scene.getSystem<RenderSystem>()->getBatchEligibility(left.getEntity()));
    EXPECT_EQ(stats().passes, 0u);
    const Transform& transform = left.getComponent<Transform>();

    // under the water: one camera, the batched card's MVP left as it was
    const Matrix4 under = transform.modelViewProjectionMatrix;
    camera.setPosition(0.5f, -2.0f, 8.0f);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(stats().passes, 0u);
    EXPECT_TRUE(transform.modelViewProjectionMatrix == under) << "the MVPs were made per camera with no reflection pass drawn";

    // above it, the water in sight: its reflection pass draws, and every camera makes its MVPs
    camera.setPosition(0.0f, 3.0f, 8.0f);
    camera.setTarget(0.0f, -2.0f, 0.0f);
    GpuEnvironment::drawFrames(2);
    ASSERT_EQ(stats().passes, 1u);
    const Matrix4 above = transform.modelViewProjectionMatrix;
    camera.setPosition(0.5f, 3.0f, 8.0f);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(stats().passes, 1u);
    EXPECT_FALSE(transform.modelViewProjectionMatrix == above) << "a reflection pass drew and the MVPs were not made per camera";
    EXPECT_NO_ENGINE_ERRORS();
}

// the reflection target is the scene's resolution (Half here) of what the viewer's pass draws
// at: the fixed resolution's size, the render scale's (not the 640 x 360 view's)
TEST_F(WaterReflectionGpu, TargetSizeIsThePassSize){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.6f, 8.0f);
    camera.setTarget(0.0f, -0.2f, 0.0f);
    camera.activate();

    Primitive water(&scene);
    water.createPlane(30.0f, 30.0f);
    water.setMaterial(mirrorWater());
    Primitive cube(&scene);
    makeRedCube(cube);
    cube.setPosition(0.0f, 1.0f, 0.0f);
    ASSERT_EQ(scene.getPlanarReflectionResolution(), PlanarReflectionResolution::HALF);

    // the view itself
    ASSERT_TRUE(render()) << "the scene never finished loading";
    const Rect view = Engine::getViewRect();
    std::printf("[gpu] view %.0f x %.0f: reflection %u x %u\n", view.getWidth(), view.getHeight(), stats().width, stats().height);
    EXPECT_EQ(stats().width, (unsigned int)std::floor(view.getWidth() * 0.5f));
    EXPECT_EQ(stats().height, (unsigned int)std::floor(view.getHeight() * 0.5f));

    // a fixed resolution of the view's aspect
    scene.setFixedResolutionSize(400, 225);
    scene.setFixedResolutionEnabled(true);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    GpuEnvironment::drawFrames(1);
    std::printf("[gpu] fixed 400 x 225: reflection %u x %u\n", stats().width, stats().height);
    EXPECT_EQ(stats().width, 200u) << "the reflection target is not the fixed resolution's";
    EXPECT_EQ(stats().height, 112u);
    const ReflectionPixel fixed = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored (fixed resolution)", fixed);
    EXPECT_GT(fixed.r, 2 * std::max(fixed.g, fixed.b) + 10) << "no reflection of the cube at the fixed resolution";
    scene.setFixedResolutionEnabled(false);

    // half the render scale (the uber pass draws the scene at it)
    PostProcessingSettings post = scene.getPostProcessing();
    post.renderScale = 0.5f;
    scene.setPostProcessing(post);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    GpuEnvironment::drawFrames(1);
    std::printf("[gpu] render scale 0.5: reflection %u x %u\n", stats().width, stats().height);
    EXPECT_EQ(stats().width, (unsigned int)std::floor(view.getWidth() * 0.5f * 0.5f)) << "the reflection target is not the render scale's";
    EXPECT_EQ(stats().height, (unsigned int)std::floor(view.getHeight() * 0.5f * 0.5f));
    const ReflectionPixel scaled = reflectionPixelAt(scene, Vector3(0.0f, -1.0f, 0.0f));
    printReflectionPixel("the cube mirrored (render scale 0.5)", scaled);
    EXPECT_GT(scaled.r, 2 * std::max(scaled.g, scaled.b) + 10) << "no reflection of the cube at the render scale";
    EXPECT_NO_ENGINE_ERRORS();
}
