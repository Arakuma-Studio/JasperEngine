// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S78 (docs/specs/S78-sprite-normals-improvements.md) drawn into the headless target (glcore; Vulkan
// on lavapipe with GRAPHIC_BACKEND=vulkan):
//   - Light Steps (R2-R4, AC2): a normal-mapped Sprite and a Tilemap tile under one Light 2D. Off,
//     the image is the one of before (its golden was captured before Light Steps existed); at 6
//     each object shows at most 7 light levels; the variant takes 'Lst' while the steps are on.
//   - the 2D ambient without a Light 2D (R6, R7, AC5): a dark blue ambient tints a Sprite with no
//     Light 2D in the scene as one Light 2D out of range would; a neutral one draws the image and
//     the variant of before (no 'L2d').
//
// The scene: a 640x360 target, an orthographic camera at one world unit per pixel (y up), white
// textures and a hemisphere normal map (OpenGL convention) made in code, 2D ambient 0 under the
// light, one Light 2D (white, intensity 1, range 260, falloff 1, height 64, no shadows).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light2D.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace light2d_steps_gpu_test {

    constexpr int kTextureSize = 64;
    constexpr float kObjectSize = 128.0f;
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    const uint64_t kLight2DBit = shaderPropertyBit(22);
    const uint64_t kNormalMapBit = shaderPropertyBit(7);

    std::vector<uint8_t>& whiteTexels(){
        static std::vector<uint8_t> texels(size_t(kTextureSize) * kTextureSize * 4, 255);
        return texels;
    }

    // a hemisphere: n = (nx, ny, sqrt(1 - r2)) inside the disc, +Z outside; ny up the image
    std::vector<uint8_t>& domeTexels(){
        static std::vector<uint8_t> texels = [](){
            std::vector<uint8_t> pixels(size_t(kTextureSize) * kTextureSize * 4);
            const float half = kTextureSize * 0.5f;
            for (int y = 0; y < kTextureSize; y++){
                for (int x = 0; x < kTextureSize; x++){
                    const float nx = (x + 0.5f - half) / half;
                    const float ny = (half - (y + 0.5f)) / half;
                    const float r2 = nx * nx + ny * ny;
                    float n[3] = {0.0f, 0.0f, 1.0f};
                    if (r2 < 1.0f){
                        n[0] = nx;
                        n[1] = ny;
                        n[2] = std::sqrt(1.0f - r2);
                    }
                    uint8_t* p = &pixels[(size_t(y) * kTextureSize + x) * 4];
                    for (int c = 0; c < 3; c++)
                        p[c] = uint8_t(std::lround((n[c] * 0.5f + 0.5f) * 255.0f));
                    p[3] = 255;
                }
            }
            return pixels;
        }();
        return texels;
    }

    TextureData textureData(std::vector<uint8_t>& texels){
        return TextureData(kTextureSize, kTextureSize, (unsigned int)texels.size(), ColorFormat::RGBA, 4, texels.data());
    }

    void setTextures(Submesh& submesh, bool normalMap){
        submesh.material.baseColorTexture.setData("light2dsteps|white", textureData(whiteTexels()));
        if (normalMap)
            submesh.material.normalTexture.setData("light2dsteps|dome", textureData(domeTexels()));
        submesh.needUpdateTexture = true;
    }

    // the screen box (rows top first, x1/y1 exclusive) of an object centred at world (x, y)
    struct Box {
        int x0, y0, x1, y1;
    };

    Box boxAt(float x, float y){
        const int cx = kWidth / 2 + int(std::lround(x));
        const int cy = kHeight / 2 - int(std::lround(y));
        const int half = int(kObjectSize) / 2;
        return {cx - half, cy - half, cx + half, cy + half};
    }

    class Light2DStepsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        std::unique_ptr<Sprite> sprite;
        std::unique_ptr<Tilemap> tilemap;
        std::unique_ptr<Light2D> light;

        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::resetView();
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setOrtho(-kWidth * 0.5f, kWidth * 0.5f, -kHeight * 0.5f, kHeight * 0.5f, -100.0f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            light.reset();
            sprite.reset();
            tilemap.reset();
            Engine::removeScene(&scene);
        }

        void addSprite(float x, bool normalMap){
            sprite = std::make_unique<Sprite>(&scene);
            sprite->setSize(kObjectSize, kObjectSize);
            sprite->setPivotPreset(PivotPreset::CENTER);
            sprite->setPosition(x, 0.0f, 0.0f);
            setTextures(sprite->getComponent<MeshComponent>().submeshes[0], normalMap);
        }

        // one tile, the tileset's normal map on its submesh
        void addTile(float x){
            tilemap = std::make_unique<Tilemap>(&scene);
            tilemap->addRect(0, "tile", Rect(0.0f, 0.0f, 1.0f, 1.0f));
            tilemap->addTile(0, Vector2(0.0f, 0.0f), kObjectSize, kObjectSize);
            tilemap->setPosition(x - kObjectSize * 0.5f, -kObjectSize * 0.5f, 0.0f);
            MeshComponent& mesh = tilemap->getComponent<MeshComponent>();
            setTextures(mesh.submeshes[0], true);
            mesh.numSubmeshes = 1;
        }

        void addLight(float x, float y, float range){
            light = std::make_unique<Light2D>(&scene);
            light->setColor(1.0f, 1.0f, 1.0f);
            light->setIntensity(1.0f);
            light->setRange(range);
            light->setFalloff(1.0f);
            light->setHeight(64.0f);
            light->setShadows(false);
            light->setPosition(x, y, 0.0f);
        }

        // the sprite at the left and the tile at the right of one Light 2D between them
        void litPair(){
            scene.setAmbientLight2D(0.0f);
            addSprite(-120.0f, true);
            addTile(120.0f);
            addLight(0.0f, 30.0f, 260.0f);
        }

        // frames until every mesh is loaded with the variant it asks for
        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.numSubmeshes == 0)
                        continue;
                    if (!mesh.loaded || mesh.needReload || mesh.needVariantReload)
                        return false;
                    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                        if (mesh.submeshes[s].needUpdateTexture && mesh.submeshes[s].vertexCount > 0)
                            return false;
                    }
                }
                return true;
            }, 600);
        }

        std::vector<uint8_t> read(){
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(GpuEnvironment::target().getRender().getColorTexture(), kWidth, kHeight, rgba));
            return rgba;
        }

        static const uint8_t* at(const std::vector<uint8_t>& rgba, int x, int y){
            return &rgba[(size_t(y) * kWidth + x) * 4];
        }

        // the distinct grey levels of a box (its texture and the light are white: r = g = b)
        static std::set<int> levels(const std::vector<uint8_t>& rgba, const Box& box){
            std::set<int> found;
            for (int y = box.y0; y < box.y1; y++){
                for (int x = box.x0; x < box.x1; x++)
                    found.insert(at(rgba, x, y)[0]);
            }
            return found;
        }

        static std::string describe(const std::set<int>& values){
            std::string text;
            for (int v : values)
                text += (text.empty() ? "" : " ") + std::to_string(v);
            return text;
        }

        uint64_t spriteProperties(){
            return sprite->getComponent<MeshComponent>().submeshes[0].shaderProperties;
        }

        uint64_t tileProperties(){
            return tilemap->getComponent<MeshComponent>().submeshes[0].shaderProperties;
        }
    };

}

using namespace light2d_steps_gpu_test;

// R3: off draws the variants and the image of before (light2d_steps_off was captured before
// Light Steps existed)
TEST_F(Light2DStepsGpu, OffDrawsTheImageOfBefore){
    litPair();
    ASSERT_TRUE(settle());
    EXPECT_NE(spriteProperties() & kLight2DBit, 0u);
    EXPECT_NE(spriteProperties() & kNormalMapBit, 0u);
    EXPECT_GT(levels(read(), boxAt(-120.0f, 0.0f)).size(), 20u) << "a smooth light has many levels";
    EXPECT_GOLDEN("light2d_steps_off");
    EXPECT_NO_ENGINE_ERRORS();
}

// R2, R4: at 6 steps each object shows at most 7 light levels (0 and the six bands); the variant
// takes 'Lst' while on and gives it up when the steps go back to 0
TEST_F(Light2DStepsGpu, SixStepsBandTheLight){
    const uint64_t lst = ShaderPool::getLight2DStepsProperty();
    litPair();
    ASSERT_TRUE(settle());
    const uint64_t smoothSprite = spriteProperties();
    const uint64_t smoothTile = tileProperties();
    EXPECT_EQ(smoothSprite & lst, 0u);
    const std::vector<uint8_t> smooth = read();

    scene.setLight2DSteps(6);
    ASSERT_TRUE(settle());
    EXPECT_EQ(spriteProperties(), smoothSprite | lst);
    EXPECT_EQ(tileProperties(), smoothTile | lst);
    const std::vector<uint8_t> stepped = read();
    for (float x : {-120.0f, 120.0f}){
        const std::set<int> found = levels(stepped, boxAt(x, 0.0f));
        EXPECT_LE(found.size(), 7u) << "the object at x " << x << ": " << describe(found);
        EXPECT_GE(found.size(), 3u) << "the object at x " << x << " is banded, not flat: " << describe(found);
        EXPECT_EQ(found.count(0), 1u) << "0 stays 0: the side facing away is black at x " << x;
    }
    // ceil: no pixel is darker than its smooth light
    size_t darker = 0;
    for (const float x : {-120.0f, 120.0f}){
        const Box box = boxAt(x, 0.0f);
        for (int y = box.y0; y < box.y1; y++){
            for (int px = box.x0; px < box.x1; px++){
                const int before = at(smooth, px, y)[0];
                const int after = at(stepped, px, y)[0];
                darker += after + 1 < before ? 1 : 0;
            }
        }
    }
    EXPECT_EQ(darker, 0u);
    EXPECT_GOLDEN("light2d_steps_6");

    scene.setLight2DSteps(0);
    ASSERT_TRUE(settle());
    EXPECT_EQ(spriteProperties(), smoothSprite);
    EXPECT_EQ(tileProperties(), smoothTile);
    EXPECT_NO_ENGINE_ERRORS();
}

// R6, R7: no Light 2D, a dark blue ambient (0.3, 0.3, 0.4) tints the sprite; the 2D path takes
// no normal map with no light to read it
TEST_F(Light2DStepsGpu, AmbientTintsASpriteWithoutLights){
    scene.setAmbientLight2D(1.0f, Vector3(0.3f, 0.3f, 0.4f));
    addSprite(0.0f, true);
    ASSERT_TRUE(settle());
    EXPECT_NE(spriteProperties() & kLight2DBit, 0u);
    EXPECT_EQ(spriteProperties() & kNormalMapBit, 0u);
    const std::vector<uint8_t> tinted = read();
    const uint8_t* centre = at(tinted, kWidth / 2, kHeight / 2);
    EXPECT_NEAR(centre[0], 77, 3);
    EXPECT_NEAR(centre[1], 77, 3);
    EXPECT_NEAR(centre[2], 102, 3);
    EXPECT_GOLDEN("light2d_ambient_no_light");

    // the image one Light 2D out of range draws
    addLight(1000.0f, 0.0f, 10.0f);
    ASSERT_TRUE(settle());
    EXPECT_NE(spriteProperties() & kNormalMapBit, 0u);
    const std::vector<uint8_t> outOfRange = read();
    for (int c = 0; c < 3; c++)
        EXPECT_NEAR(at(outOfRange, kWidth / 2, kHeight / 2)[c], centre[c], 1) << "channel " << c;

    // the light removed: still tinted
    light.reset();
    ASSERT_TRUE(settle());
    EXPECT_NE(spriteProperties() & kLight2DBit, 0u);
    const std::vector<uint8_t> removed = read();
    for (int c = 0; c < 3; c++)
        EXPECT_NEAR(at(removed, kWidth / 2, kHeight / 2)[c], centre[c], 1) << "channel " << c;

    // back to white: the variant and the image of before
    scene.setAmbientLight2D(1.0f, Vector3(1.0f, 1.0f, 1.0f));
    ASSERT_TRUE(settle());
    EXPECT_EQ(spriteProperties() & kLight2DBit, 0u);
    const std::vector<uint8_t> neutral = read();
    EXPECT_GE(at(neutral, kWidth / 2, kHeight / 2)[0], 254);
    EXPECT_GE(at(neutral, kWidth / 2, kHeight / 2)[2], 254);
    EXPECT_NO_ENGINE_ERRORS();
}

// R6: a neutral ambient with no Light 2D draws the variant and the image of before
// (light2d_ambient_neutral was captured before the ambient applied without lights)
TEST_F(Light2DStepsGpu, NeutralAmbientWithoutLightsDrawsTheImageOfBefore){
    addSprite(0.0f, true);
    ASSERT_TRUE(settle());
    EXPECT_EQ(spriteProperties() & kLight2DBit, 0u);
    EXPECT_GOLDEN("light2d_ambient_neutral");
    EXPECT_NO_ENGINE_ERRORS();
}
