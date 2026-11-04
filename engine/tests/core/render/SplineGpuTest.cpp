// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the splines build, drawn on the headless device (docs/specs/S18-splines.md AC15). Each
// scene is made the way a game makes it (the components on GameObjects, SplineSystem building
// the chunks and pieces), and the frame is compared with golden/<name>.png, captured on glcore:
//
//  spline_road_terrain  a crowned road conformed to rolling terrain (TerrainHeightAndSlope)
//  spline_fence_wall    a stepped stone wall up a slope and a post-and-rail fence (OBJ pieces)
//  spline_river_flow    a WaterBanks river flooded over a valley, library:water FLOW + MESH_DEPTH
//                       (the shader clock pinned, so the ripples stand still)
//  spline_2d_shapes     2D (up axis Z, orthographic): a filled concave star and a Flat edge
//
// glcore matches its own goldens within the default (tight) bounds; another backend is held to
// the cross-backend bound against the same images.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Terrain.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/SplineSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr double PINNED_TIME = 4.5;
    constexpr int HEIGHT_RESOLUTION = 129;
    constexpr float TERRAIN_SIZE = 128.0f;

    CompareOptions backendOptions(){
        CompareOptions options;
        if (std::strcmp(HeadlessContext::getBackendName(), "glcore") != 0){
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    spline::SplineKnot knotAt(float x, float y, float z, float radius){
        spline::SplineKnot knot;
        knot.position = Vector3(x, y, z);
        knot.radius = radius;
        return knot;
    }

    Material litColor(float r, float g, float b){
        Material material;
        material.baseColorFactor = Vector4(r, g, b, 1.0f);
        material.lighting = MaterialLighting::PIXEL;
        return material;
    }

    class SplineGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};
        std::unique_ptr<Terrain> terrain;
        std::vector<Entity> owners;
        // the heightmap's pixels: a TextureData made from memory points at them, so they live as
        // long as the scene
        std::vector<unsigned char> heightPixels;

        void SetUp() override{
            Engine::setPinnedShaderTime(PINNED_TIME);
            scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.35f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
            TextureDataPool::remove("spline-gpu-height");
        }

        // a 128 x 128 terrain at the origin of the heights given in 0..1 by f(x, z) (world XZ)
        void makeTerrain(float maxHeight, const std::function<float(float, float)>& f){
            terrain = std::make_unique<Terrain>(&scene);
            terrain->setSize(TERRAIN_SIZE);
            terrain->setMaxHeight(maxHeight);
            std::vector<unsigned char>& pixels = heightPixels;
            pixels.assign(static_cast<size_t>(HEIGHT_RESOLUTION) * HEIGHT_RESOLUTION * 2, 0);
            for (int j = 0; j < HEIGHT_RESOLUTION; j++){
                for (int i = 0; i < HEIGHT_RESOLUTION; i++){
                    // texel i at (i + 0.5) / N of the size (texel-centre sampling)
                    const float x = ((static_cast<float>(i) + 0.5f) / HEIGHT_RESOLUTION - 0.5f) * TERRAIN_SIZE;
                    const float z = ((static_cast<float>(j) + 0.5f) / HEIGHT_RESOLUTION - 0.5f) * TERRAIN_SIZE;
                    const float h = std::fmin(std::fmax(f(x, z), 0.0f), 1.0f);
                    const unsigned q = static_cast<unsigned>(std::lround(h * 65535.0f));
                    const size_t at = (static_cast<size_t>(j) * HEIGHT_RESOLUTION + i) * 2;
                    pixels[at] = static_cast<unsigned char>(q & 0xFF);
                    pixels[at + 1] = static_cast<unsigned char>(q >> 8);
                }
            }
            TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain->getEntity());
            component.heightMap.setData("spline-gpu-height", TextureData(HEIGHT_RESOLUTION, HEIGHT_RESOLUTION,
                                        static_cast<unsigned int>(pixels.size()), ColorFormat::RED16, 1, pixels.data()));
            component.heightMap.setReleaseDataAfterLoad(false);
            component.needUpdateTexture = true;
            component.needUpdateTerrain = true;
            terrain->setColor(0.42f, 0.55f, 0.3f);
        }

        // a GameObject with a Spline and a Mesh, the Mesh's material the template of what it builds
        Entity splineObject(const std::vector<spline::SplineKnot>& knots, const Material& material,
                            spline::SplineType type = spline::SplineType::CatmullRom, bool closed = false, bool flat2D = false){
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            SplineComponent spline;
            if (flat2D){
                spline.spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
            }
            spline.spline.setType(type);
            spline.spline.setKnots(knots);
            spline.spline.setClosed(closed);
            scene.addComponent<SplineComponent>(entity, spline);
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].material = material;
            scene.addComponent<MeshComponent>(entity, mesh);
            owners.push_back(entity);
            return entity;
        }

        bool render(){
            Scene* current = &scene;
            const std::vector<Entity> built = owners;
            const bool ready = GpuEnvironment::runFrames([current, built](){
                std::shared_ptr<SplineSystem> system = current->getSystem<SplineSystem>();
                for (Entity owner : built){
                    if (system->isRebuildPending(owner)){
                        return false;
                    }
                    if (current->findComponent<SplineExtrudeComponent>(owner) && system->getGeneratedEntities(owner).empty()){
                        return false;
                    }
                    if (current->findComponent<SplineInstantiateComponent>(owner) && system->getInstantiateStats(owner).instances == 0){
                        return false;
                    }
                }
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }, 600);
            // the chunks made on the last frame have loaded and drawn
            GpuEnvironment::drawFrames(3);
            return ready && scene.getSystem<RenderSystem>()->isAllLoaded();
        }

        void expectGolden(const std::string& name){
            for (Entity owner : owners){
                std::shared_ptr<SplineSystem> system = scene.getSystem<SplineSystem>();
                for (const SplineWarning& warning : system->getExtrudeWarnings(owner)){
                    ADD_FAILURE() << name << ": " << warning.message;
                }
                for (const SplineWarning& warning : system->getInstantiateWarnings(owner)){
                    ADD_FAILURE() << name << ": " << warning.message;
                }
            }
            const CompareResult result = compareTargetWithGolden(name, backendOptions());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

}

TEST_F(SplineGpu, RoadOnTerrain){
    JASPER_REQUIRE_GPU();
    makeTerrain(12.0f, [](float x, float z){
        return 0.4f + 0.18f * std::sin(x * 0.07f) * std::cos(z * 0.05f) + 0.08f * std::sin((x + z) * 0.11f);
    });
    std::vector<spline::SplineKnot> knots;
    for (int i = 0; i <= 6; i++){
        const float x = -48.0f + 16.0f * static_cast<float>(i);
        knots.push_back(knotAt(x, 6.0f, 10.0f * std::sin(static_cast<float>(i) * 0.9f), 3.0f));
    }
    const Entity road = splineObject(knots, litColor(0.2f, 0.2f, 0.22f));
    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::Crown;
    extrude.shoulderWidth = 0.8f;
    extrude.conform = SplineConform::TerrainHeightAndSlope;
    extrude.verticalOffset = 0.05f;
    scene.addComponent<SplineExtrudeComponent>(road, extrude);
    SplineComponent& spline = scene.getComponent<SplineComponent>(road);
    spline.terrain.terrain = terrain->getEntity();

    camera.setPosition(-30.0f, 22.0f, 38.0f);
    camera.setTarget(4.0f, 4.0f, 0.0f);
    camera.setFarClip(500.0f);
    camera.activate();

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GT(scene.getSystem<SplineSystem>()->getExtrudeStats(road).vertices, 0u);
    expectGolden("spline_road_terrain");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SplineGpu, FenceAndWall){
    JASPER_REQUIRE_GPU();
    makeTerrain(10.0f, [](float x, float z){
        // a slope rising to +X
        return 0.3f + 0.003f * x + 0.02f * std::sin(z * 0.2f);
    });
    // the wall: Stepped courses up the slope
    std::vector<spline::SplineKnot> wallKnots = {knotAt(-20, 3, -4, 1), knotAt(-6, 3, -7, 1), knotAt(8, 3, -3, 1), knotAt(20, 3, -8, 1)};
    const Entity wall = splineObject(wallKnots, litColor(0.6f, 0.56f, 0.5f));
    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::Rectangle;
    extrude.thickness = 0.7f;
    extrude.height = 1.6f;
    extrude.grade = SplineGrade::Stepped;
    extrude.stepHeight = 0.4f;
    extrude.conform = SplineConform::TerrainHeight;
    extrude.endCaps = true;
    scene.addComponent<SplineExtrudeComponent>(wall, extrude);

    // the fence: posts on the knots and every pitch, rails between them
    std::vector<spline::SplineKnot> fenceKnots = {knotAt(-20, 3, 4, 1), knotAt(-4, 3, 7, 1), knotAt(10, 3, 3, 1), knotAt(20, 3, 6, 1)};
    const Entity fence = splineObject(fenceKnots, litColor(0.45f, 0.3f, 0.18f), spline::SplineType::Linear);
    // pieces keep their models' own materials: an Instantiate has no Mesh template
    scene.removeComponent<MeshComponent>(fence);
    SplineInstantiateComponent instantiate;
    instantiate.mode = SplineInstantiateMode::Fence;
    instantiate.posts.push_back({AssetRef("splines/post.obj"), 1.0f});
    instantiate.spans.push_back({AssetRef("splines/rail.obj"), 1.0f});
    instantiate.postPitch = 2.4f;
    instantiate.conform = SplineConform::TerrainHeight;
    scene.addComponent<SplineInstantiateComponent>(fence, instantiate);

    camera.setPosition(-6.0f, 14.0f, 30.0f);
    camera.setTarget(0.0f, 3.0f, 0.0f);
    camera.setFarClip(300.0f);
    camera.activate();

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GT(scene.getSystem<SplineSystem>()->getInstantiateStats(fence).instances, 10u);
    expectGolden("spline_fence_wall");
    EXPECT_NO_ENGINE_ERRORS();
}

// T3 R26: an item whose origin is over a hole goes, deterministically; a road bridges one and warns
TEST_F(SplineGpu, InstantiateDropsOverHole){
    JASPER_REQUIRE_GPU();
    makeTerrain(10.0f, [](float, float){ return 0.3f; });
    std::vector<spline::SplineKnot> knots = {knotAt(-40, 3, 0, 1), knotAt(40, 3, 0, 1)};
    const Entity road = splineObject(knots, litColor(0.3f, 0.3f, 0.3f));
    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::Crown;
    extrude.conform = SplineConform::TerrainHeight;
    scene.addComponent<SplineExtrudeComponent>(road, extrude);

    const Entity row = splineObject(knots, litColor(0.45f, 0.3f, 0.18f), spline::SplineType::Linear);
    scene.removeComponent<MeshComponent>(row);
    SplineInstantiateComponent instantiate;
    instantiate.mode = SplineInstantiateMode::Spacing;
    instantiate.items.push_back({AssetRef("splines/post.obj"), 1.0f});
    instantiate.spacing = 4.0f;
    instantiate.conform = SplineConform::TerrainHeight;
    instantiate.seed = 7;
    scene.addComponent<SplineInstantiateComponent>(row, instantiate);
    camera.setPosition(0.0f, 30.0f, 40.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    ASSERT_TRUE(render());
    std::shared_ptr<SplineSystem> system = scene.getSystem<SplineSystem>();
    const uint32_t solid = system->getInstantiateStats(row).instances;
    ASSERT_GT(solid, 10u);
    for (const SplineWarning& warning : system->getExtrudeWarnings(road)){
        EXPECT_NE(warning.kind, SplineWarningKind::OverHole);
    }

    // a hole across the middle of the row: x from -16 to 16
    std::vector<unsigned char> holes(static_cast<size_t>(16) * 16, 255);
    for (int y = 0; y < 16; y++){
        for (int x = 6; x < 10; x++){
            holes[static_cast<size_t>(y) * 16 + x] = 0;
        }
    }
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain->getEntity());
    component.holeMap.setData("spline-gpu-holes", TextureData(16, 16, static_cast<unsigned int>(holes.size()), ColorFormat::RED, 1, holes.data()));
    component.needUpdateTexture = true;
    component.holeMapRevision++;
    system->rebuildNow(road);
    system->rebuildNow(row);
    ASSERT_TRUE(render());

    const uint32_t cut = system->getInstantiateStats(row).instances;
    // 32 m of the 80 m row is over the hole; the items that stay did not move, so a second build agrees
    EXPECT_LE(cut, solid - 3u);
    EXPECT_GE(cut, solid / 2u);
    system->rebuildNow(row);
    ASSERT_TRUE(render());
    EXPECT_EQ(system->getInstantiateStats(row).instances, cut);
    bool warned = false;
    for (const SplineWarning& warning : system->getExtrudeWarnings(road)){
        warned = warned || warning.kind == SplineWarningKind::OverHole;
    }
    EXPECT_TRUE(warned) << "a road over a hole bridges it and says so";
    TextureDataPool::remove("spline-gpu-holes");
}

TEST_F(SplineGpu, RiverFlowsBetweenItsBanks){
    JASPER_REQUIRE_GPU();
    // a valley winding along z = 6 sin(x / 15), deepest on its line
    makeTerrain(10.0f, [](float x, float z){
        const float d = (z - 6.0f * std::sin(x / 15.0f)) / 18.0f;
        return 0.28f + 0.42f * std::fmin(d * d, 1.0f) + 0.002f * x;
    });
    std::vector<spline::SplineKnot> knots;
    for (int i = 0; i <= 8; i++){
        const float x = -50.0f + 12.5f * static_cast<float>(i);
        // the surface falls downstream with the valley floor (0.2 per 100)
        knots.push_back(knotAt(x, 10.0f * (0.36f + 0.002f * x), 6.0f * std::sin(x / 15.0f), 6.0f));
    }
    Material water;
    water.name = "River";
    water.shader = "library:water";
    water.shaderFeatures = {"BUILTIN_TEXTURES", "FLOW", "MESH_DEPTH"};
    water.lighting = MaterialLighting::PIXEL;
    const Entity river = splineObject(knots, water);
    SplineExtrudeComponent extrude;
    extrude.shape = SplineExtrudeShape::WaterBanks;
    extrude.maxHalfWidth = 14.0f;
    extrude.flow = true;
    scene.addComponent<SplineExtrudeComponent>(river, extrude);
    scene.getComponent<SplineComponent>(river).terrain.terrain = terrain->getEntity();

    camera.setPosition(-34.0f, 16.0f, 26.0f);
    camera.setTarget(0.0f, 2.0f, 0.0f);
    camera.setFarClip(400.0f);
    camera.activate();

    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_TRUE(scene.getSystem<SplineSystem>()->hasWaterSurface(river));
    expectGolden("spline_river_flow");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(SplineGpu, TwoDimensionalShapes){
    JASPER_REQUIRE_GPU();
    // a concave five-pointed star, filled
    std::vector<spline::SplineKnot> star;
    for (int i = 0; i < 10; i++){
        const float angle = 1.5707963f + static_cast<float>(i) * 0.62831853f;
        const float r = (i % 2 == 0) ? 6.0f : 2.6f;
        star.push_back(knotAt(-6.0f + r * std::cos(angle), 1.5f + r * std::sin(angle), 0.0f, 0.5f));
    }
    const Entity fill = splineObject(star, litColor(0.95f, 0.75f, 0.2f), spline::SplineType::Linear, true, true);
    SplineExtrudeComponent fillShape;
    fillShape.shape = SplineExtrudeShape::Fill;
    scene.addComponent<SplineExtrudeComponent>(fill, fillShape);

    // a rolling ground edge
    std::vector<spline::SplineKnot> edge;
    for (int i = 0; i <= 8; i++){
        const float x = -15.0f + 3.75f * static_cast<float>(i);
        edge.push_back(knotAt(x, -7.0f + 1.5f * std::sin(static_cast<float>(i) * 0.8f), 0.0f, 0.4f));
    }
    const Entity ground = splineObject(edge, litColor(0.25f, 0.6f, 0.35f), spline::SplineType::CatmullRom, false, true);
    SplineExtrudeComponent edgeShape;
    edgeShape.shape = SplineExtrudeShape::Flat;
    scene.addComponent<SplineExtrudeComponent>(ground, edgeShape);

    // and a Bezier loop drawn as a tube seen end on: its outline
    std::vector<spline::SplineKnot> loop = {knotAt(8, 4, 0, 0.35f), knotAt(12, 0, 0, 0.35f), knotAt(8, -4, 0, 0.35f), knotAt(4, 0, 0, 0.35f)};
    loop[0].tangentOut = Vector3(2.2f, 0, 0);  loop[0].tangentIn = Vector3(-2.2f, 0, 0);
    loop[1].tangentOut = Vector3(0, -2.2f, 0); loop[1].tangentIn = Vector3(0, 2.2f, 0);
    loop[2].tangentOut = Vector3(-2.2f, 0, 0); loop[2].tangentIn = Vector3(2.2f, 0, 0);
    loop[3].tangentOut = Vector3(0, 2.2f, 0);  loop[3].tangentIn = Vector3(0, -2.2f, 0);
    const Entity ring = splineObject(loop, litColor(0.3f, 0.45f, 0.9f), spline::SplineType::CubicBezier, true, true);
    SplineExtrudeComponent ringShape;
    ringShape.shape = SplineExtrudeShape::Tube;
    ringShape.tubeSides = 12;
    scene.addComponent<SplineExtrudeComponent>(ring, ringShape);

    sun.setDirection(0.2f, -0.3f, -1.0f);
    camera.setOrtho(-16.0f, 16.0f, -12.0f, 12.0f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 0.0f, 20.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    ASSERT_TRUE(render()) << "the scene never finished loading";
    expectGolden("spline_2d_shapes");
    EXPECT_NO_ENGINE_ERRORS();
}
