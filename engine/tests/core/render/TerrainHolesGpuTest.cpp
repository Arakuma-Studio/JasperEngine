// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R18-R21: one hole mask in every pass, the same texels as terraincore::isHole, read from the frame on
// the headless device with a camera straight down, so a pixel centre is a terrain-local point.

#include "TerrainGpuFixture.h"

#include "LogCapture.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Primitive.h"
#include "object/TerrainEdit.h"
#include "pool/ShaderPool.h"
#include "subsystem/MeshSystem.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainOps.h"

#include <span>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // World units per pixel of the frame: 160 x 90 metres, centred on the terrain
    constexpr float VIEW_WIDTH = 160.0f;
    constexpr float VIEW_HEIGHT = 90.0f;

    struct PixelColor {
        int r, g, b;
    };

    bool sameColor(const uint8_t* pixel, const PixelColor& color, int tolerance){
        return std::abs(pixel[0] - color.r) <= tolerance && std::abs(pixel[1] - color.g) <= tolerance && std::abs(pixel[2] - color.b) <= tolerance;
    }

    class TerrainHolesGpu : public TerrainGpuFixture {
    protected:
        std::vector<uint8_t> holeTexels;
        int holeWidth = 0;
        int holeHeight = 0;

        void SetUp() override{
            TerrainGpuFixture::SetUp();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setOrtho(-VIEW_WIDTH * 0.5f, VIEW_WIDTH * 0.5f, -VIEW_HEIGHT * 0.5f, VIEW_HEIGHT * 0.5f, 1.0f, 400.0f);
            camera.setPosition(0.0f, 200.0f, 0.0f);
            camera.setUp(0.0f, 0.0f, -1.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
        }

        void flatTerrain(float height){
            makeTerrain(1.0f, [](float, float){ return 0.0f; });
            terrain->setPosition(0.0f, height, 0.0f);
            terrain->setColor(0.1f, 0.8f, 0.2f);
            terrain->setReceiveLights(false);
        }

        // texels: f(x, y) says hole; a map of width x height
        void setHoles(int width, int height, const std::function<bool(int, int)>& hole){
            holeWidth = width;
            holeHeight = height;
            holeTexels.resize(static_cast<size_t>(width) * height);
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++){
                    holeTexels[static_cast<size_t>(y) * width + x] = hole(x, y) ? terraincore::HOLE_VALUE : terraincore::SOLID_VALUE;
                }
            }
            component().holeMap.setData(poolId("holes"), TextureData(width, height, static_cast<unsigned int>(holeTexels.size()),
                                                                     ColorFormat::RED, 1, holeTexels.data()));
            component().needUpdateTexture = true;
        }

        Vector2 worldOfPixel(int x, int y) const{
            return Vector2((static_cast<float>(x) + 0.5f) / FRAME_WIDTH * VIEW_WIDTH - VIEW_WIDTH * 0.5f,
                           (static_cast<float>(y) + 0.5f) / FRAME_HEIGHT * VIEW_HEIGHT - VIEW_HEIGHT * 0.5f);
        }

        // the point is this close, in texels, to a texel edge of the map (a pixel centre on an edge
        // goes either way by a rounding: isHole's rule, tested on the CPU, owns that case)
        bool nearTexelEdge(float u, float v, float tolerance) const{
            const float tx = u * static_cast<float>(holeWidth);
            const float ty = v * static_cast<float>(holeHeight);
            return std::fabs(tx - std::round(tx)) < tolerance || std::fabs(ty - std::round(ty)) < tolerance;
        }

        // every pixel on the terrain: through a hole it is `through`, else `solid`
        void expectMask(const std::vector<uint8_t>& frame, const PixelColor& solid, const PixelColor& through, const char* pass){
            const terraincore::HoleMapView view{holeTexels.data(), holeWidth, holeHeight};
            int wrong = 0;
            int compared = 0;
            int holes = 0;
            for (int y = 0; y < FRAME_HEIGHT; y++){
                for (int x = 0; x < FRAME_WIDTH; x++){
                    const Vector2 world = worldOfPixel(x, y);
                    const float u = (world.x + TERRAIN_SIZE * 0.5f) / TERRAIN_SIZE;
                    const float v = (world.y + TERRAIN_SIZE * 0.5f) / TERRAIN_SIZE;
                    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f || nearTexelEdge(u, v, 0.001f)){
                        continue;
                    }
                    compared++;
                    const bool hole = terraincore::isHole(view, u, v);
                    holes += hole ? 1 : 0;
                    const uint8_t* pixel = &frame[(static_cast<size_t>(y) * FRAME_WIDTH + x) * 4];
                    if (!sameColor(pixel, hole ? through : solid, 16)){
                        if (wrong < 5){
                            ADD_FAILURE() << pass << ": pixel " << x << ", " << y << " (" << int(pixel[0]) << ", " << int(pixel[1]) << ", " << int(pixel[2])
                                          << ") at u " << u << " v " << v << (hole ? " should see through" : " should be solid");
                        }
                        wrong++;
                    }
                }
            }
            EXPECT_EQ(wrong, 0) << pass << ": " << compared << " pixels compared, " << holes << " over holes";
            EXPECT_GT(holes, 0) << pass;
            EXPECT_GT(compared - holes, 0) << pass;
        }

        // an asymmetric map, with edges that fall inside pixels
        void asymmetricHoles(){
            setHoles(37, 23, [](int x, int y){
                return (x >= 4 && x < 12 && y >= 3 && y < 9) || (x >= 25 && y >= 15 && (x + y) % 2 == 0);
            });
        }

        void floorBelow(float r, float g, float b, Primitive& floor){
            floor.createPlane(VIEW_WIDTH * 2.0f, VIEW_WIDTH * 2.0f);
            floor.setPosition(0.0f, -20.0f, 0.0f);
            floor.setColor(r, g, b);
            floor.setReceiveLights(false);
        }
    };

}

TEST_F(TerrainHolesGpu, CpuMatchesGpu){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    asymmetricHoles();
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    expectMask(frame, PixelColor{26, 204, 51}, PixelColor{0, 0, 0}, "colour");
    EXPECT_NE(component().holeMap.getRender(nullptr), nullptr);
}

TEST_F(TerrainHolesGpu, EveryPassCuts){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    asymmetricHoles();
    Primitive floor(&scene);
    floorBelow(0.8f, 0.1f, 0.7f, floor);
    const PixelColor solid{26, 204, 51};
    const PixelColor through{204, 26, 179};

    // the colour pass over what lies below
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    expectMask(frame, solid, through, "colour");

    // a depth pre-pass that kept the hole would hide the floor there
    scene.setDepthPrepassEnabled(true);
    ASSERT_TRUE(render());
    ASSERT_TRUE(readFrame(frame));
    expectMask(frame, solid, through, "depth pre-pass");
    scene.setDepthPrepassEnabled(false);

    // the G-buffer: its base colour shows the floor through the hole
    scene.setSSREnabled(true);
    scene.setSSRDebugMode(5);
    ASSERT_TRUE(render());
    ASSERT_TRUE(readFrame(frame));
    expectMask(frame, solid, through, "G-buffer");
    scene.setSSRDebugMode(0);
    scene.setSSREnabled(false);

    // SSAO's depth: the floor darkens along the hole's rim only if the depth it reads has the hole
    scene.setSSAOEnabled(true);
    scene.setSSAODebug(true);
    ASSERT_TRUE(render());
    std::vector<uint8_t> cut;
    ASSERT_TRUE(readFrame(cut));
    component().holeMap = Texture();
    component().needUpdateTexture = true;
    ASSERT_TRUE(render());
    std::vector<uint8_t> uncut;
    ASSERT_TRUE(readFrame(uncut));
    int differ = 0;
    for (size_t i = 0; i < cut.size(); i += 4){
        differ += std::abs(cut[i] - uncut[i]) > 8 ? 1 : 0;
    }
    EXPECT_GT(differ, 100) << "SSAO drew the same with and without the hole";
    EXPECT_NO_ENGINE_ERRORS();
}

// A hole cut once the terrain draws (a brush in Play, a Voxel Volume's cave mouth) swaps it to its holed
// variant while the solid one draws, and the hole shows once that is built, on every backend: on
// Vulkan its pipelines are made on a worker (spec S32), frames after the cut
TEST_F(TerrainHolesGpu, ACutAfterTheFirstFrameDraws){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    Primitive floor(&scene);
    floorBelow(0.8f, 0.1f, 0.7f, floor);
    const PixelColor solid{26, 204, 51};
    const PixelColor through{204, 26, 179};
    const auto pixel = [](const std::vector<uint8_t>& frame, float x, float z){
        const int px = static_cast<int>((x + VIEW_WIDTH * 0.5f) / VIEW_WIDTH * FRAME_WIDTH);
        const int py = static_cast<int>((z + VIEW_HEIGHT * 0.5f) / VIEW_HEIGHT * FRAME_HEIGHT);
        return &frame[(static_cast<size_t>(py) * FRAME_WIDTH + px) * 4];
    };
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    EXPECT_TRUE(sameColor(pixel(frame, 0.0f, 0.0f), solid, 16)) << "no hole yet";
    const uint64_t holesBit = ShaderPool::getTerrainHolesProperty(ShaderType::MESH);
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
    EXPECT_EQ(mesh.submeshes[0].shaderProperties & holesBit, 0u);

    TerrainComponentStore store(&scene, terrain->getEntity(), component());
    terraincore::Op cut;
    cut.type = terraincore::OpType::CutHoles;
    cut.shape.type = terraincore::ShapeType::Circle;
    cut.shape.center = Vector2(0.0f, 0.0f);
    cut.shape.radius = 10.0f;
    ASSERT_TRUE(terraincore::applyOps(store, std::span<const terraincore::Op>(&cut, 1)).ok);

    ASSERT_TRUE(render()) << "the holed variant never came in";
    ASSERT_TRUE(readFrame(frame));
    EXPECT_NE(mesh.submeshes[0].shaderProperties & holesBit, 0u) << "still the solid variant";
    const uint8_t* centre = pixel(frame, 0.0f, 0.0f);
    EXPECT_TRUE(sameColor(centre, through, 16)) << "the hole's centre draws (" << int(centre[0]) << ", " << int(centre[1]) << ", " << int(centre[2]) << ")";
    EXPECT_TRUE(sameColor(pixel(frame, 40.0f, 0.0f), solid, 16)) << "the ground beside it stays";
    EXPECT_NO_ENGINE_ERRORS();
}

// The sun slants, so the terrain's shadow falls on the floor beside it: a texel of the map lets the
// light through to one spot, found by following the sun's ray back.
TEST_F(TerrainHolesGpu, ShadowMapHasTheHole){
    JASPER_REQUIRE_GPU();
    makeTerrain(1.0f, [](float, float){ return 0.0f; });
    terrain->setPosition(-60.0f, 30.0f, 0.0f);
    terrain->setSize(40.0f);
    setHoles(8, 8, [](int x, int y){ return x >= 3 && x < 5 && y >= 3 && y < 5; });
    Primitive floor(&scene);
    floor.createPlane(VIEW_WIDTH * 2.0f, VIEW_WIDTH * 2.0f);
    floor.setPosition(0.0f, 0.0f, 0.0f);
    floor.setColor(0.9f, 0.9f, 0.9f);
    sun.setDirection(1.0f, -1.0f, 0.0f);
    sun.setShadows(true);
    // near enough for the shadow distance of the directional light's cascades
    camera.setPosition(0.0f, 60.0f, 0.0f);
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));

    // a point of the floor at world (x, z) is under the terrain's (x - 30, z). The terrain spans x -80 to -40
    // at y 30, so its shadow is x -50 to -10, in view beside it; the hole, at its centre, shows at x -30
    const auto pixelAt = [&](float x, float z) -> const uint8_t* {
        const int px = static_cast<int>((x + VIEW_WIDTH * 0.5f) / VIEW_WIDTH * FRAME_WIDTH);
        const int py = static_cast<int>((z + VIEW_HEIGHT * 0.5f) / VIEW_HEIGHT * FRAME_HEIGHT);
        return &frame[(static_cast<size_t>(py) * FRAME_WIDTH + px) * 4];
    };
    const int inHole = pixelAt(-30.0f, 0.0f)[1];
    const int inShade = pixelAt(-15.0f, 0.0f)[1];
    const int inSun = pixelAt(40.0f, 30.0f)[1];
    std::printf("[gpu] shadow: hole %d, shade %d, sun %d\n", inHole, inShade, inSun);
    EXPECT_GT(inSun, inShade + 40) << "the terrain casts";
    EXPECT_GT(inHole, inShade + 40) << "the shadow map stayed solid over the hole";
    EXPECT_NEAR(inHole, inSun, 24);
}

TEST_F(TerrainHolesGpu, LibraryShaderCuts){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    asymmetricHoles();
    Material material = terrain->getMaterial();
    material.shader = "library:unlit";
    terrain->setMaterial(material);
    Primitive floor(&scene);
    floorBelow(0.8f, 0.1f, 0.7f, floor);
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
    EXPECT_NE(mesh.submeshes[0].shaderProperties & ShaderPool::getTerrainHolesProperty(ShaderType::MESH), 0u);
    const terraincore::HoleMapView view{holeTexels.data(), holeWidth, holeHeight};
    EXPECT_TRUE(terraincore::isHole(view, 8.0f / 37.0f, 6.0f / 23.0f));
    const Vector2 uvToPixel((8.0f / 37.0f * TERRAIN_SIZE - TERRAIN_SIZE * 0.5f + VIEW_WIDTH * 0.5f) / VIEW_WIDTH * FRAME_WIDTH,
                            (6.0f / 23.0f * TERRAIN_SIZE - TERRAIN_SIZE * 0.5f + VIEW_HEIGHT * 0.5f) / VIEW_HEIGHT * FRAME_HEIGHT);
    const uint8_t* pixel = &frame[(static_cast<size_t>(uvToPixel.y) * FRAME_WIDTH + static_cast<size_t>(uvToPixel.x)) * 4];
    EXPECT_TRUE(sameColor(pixel, PixelColor{204, 26, 179}, 16)) << int(pixel[0]) << ", " << int(pixel[1]) << ", " << int(pixel[2]);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(TerrainHolesGpu, NoHoleNoCost){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    ASSERT_TRUE(render());
    const auto holeBit = [&](const Submesh& submesh){
        return (submesh.shaderProperties & ShaderPool::getTerrainHolesProperty(ShaderType::MESH)) != 0;
    };
    {
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        EXPECT_FALSE(holeBit(mesh.submeshes[0]));
        EXPECT_EQ(mesh.submeshes[0].shader.get()->shaderData.getTextureIndex(TextureShaderType::TERRAINHOLES).first, -1);
        EXPECT_NE(mesh.submeshes[0].shader.get()->shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP).first, -1);
    }

    // the first cut reloads the variant, without a hole being asked for by anything else
    asymmetricHoles();
    ASSERT_TRUE(render());
    {
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        EXPECT_TRUE(holeBit(mesh.submeshes[0]));
        EXPECT_NE(mesh.submeshes[0].shader.get()->shaderData.getTextureIndex(TextureShaderType::TERRAINHOLES).first, -1);
    }
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    expectMask(frame, PixelColor{26, 204, 51}, PixelColor{0, 0, 0}, "after the first cut");

    // the last fill reloads it back
    component().holeMap = Texture();
    component().needUpdateTexture = true;
    ASSERT_TRUE(render());
    {
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        EXPECT_FALSE(holeBit(mesh.submeshes[0]));
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// T3 R21: a hole map takes the slot of the base material's occlusion texture, and the base's occlusion
// reads 1 in that variant; without a hole map the occlusion texture draws as it always did
TEST_F(TerrainHolesGpu, BaseOcclusionGivesItsSlot){
    JASPER_REQUIRE_GPU();
    makeTerrain(1.0f, [](float, float){ return 0.0f; });
    terrain->setTextureLayer(0, writeColorMap("grass", 90, 150, 60));
    setBlendMap(0, [](float, float){ return TerrainGpuColor{255, 0, 0, 255}; });
    Material material = terrain->getMaterial();
    material.occlusionTexture = Texture(writeMap("occlusion", [](int, int){ return TerrainGpuColor{0, 0, 0, 255}; }));
    material.baseColorTexture = Texture(writeColorMap("base", 200, 200, 200));
    terrain->setMaterial(material);
    ASSERT_TRUE(render());
    {
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        ShaderData& data = mesh.submeshes[0].shader.get()->shaderData;
        EXPECT_NE(data.getTextureIndexByName("u_occlusionTexture").first, -1);
        EXPECT_EQ(data.getTextureIndex(TextureShaderType::TERRAINHOLES).first, -1);
    }
    setHoles(4, 4, [](int x, int y){ return x == 1 && y == 1; });
    ASSERT_TRUE(render());
    {
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain->getEntity());
        ShaderData& data = mesh.submeshes[0].shader.get()->shaderData;
        EXPECT_EQ(data.getTextureIndexByName("u_occlusionTexture").first, -1) << "the hole map took its slot";
        EXPECT_NE(data.getTextureIndex(TextureShaderType::TERRAINHOLES).first, -1);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// T3 R29: a map that is not R8 warns once and draws no holes, as the CPU queries answer
TEST_F(TerrainHolesGpu, WrongFormatWarnsOnceAndDrawsNoHoles){
    JASPER_REQUIRE_GPU();
    flatTerrain(0.0f);
    component().holeMap = Texture(writeMap("rgba", [](int, int){ return TerrainGpuColor{0, 0, 0, 255}; }));
    component().needUpdateTexture = true;
    LogCapture log;
    ASSERT_TRUE(render());
    std::vector<uint8_t> frame;
    ASSERT_TRUE(readFrame(frame));
    const size_t center = (static_cast<size_t>(FRAME_HEIGHT / 2) * FRAME_WIDTH + FRAME_WIDTH / 2) * 4;
    EXPECT_TRUE(sameColor(&frame[center], PixelColor{26, 204, 51}, 16));
    EXPECT_EQ(log.count(D_LOG_WARN, "hole map"), 1u);
}
