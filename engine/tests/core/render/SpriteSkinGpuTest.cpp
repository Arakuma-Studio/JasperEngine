// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite Skin on the headless device (docs/specs/S94-rendering-anim-gaps.md F49, AC46): a tall
// frame with a two-bone skin. At rest the skinned mesh covers what the frame's quad covers; the
// upper bone turned 60 degrees bends the top half off its column (golden on glcore, matched on
// Vulkan); Flip X mirrors the skinned mesh as it mirrors the quad. Without a Sprite Skin a skinned frame draws its quad with no bone resources and the same
// draws (F-PF1: disabled is free).

#include "Engine.h"
#include "Scene.h"
#include "buffer/InterleavedBuffer.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/SpriteSkinComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/GameObject.h"
#include "object/Sprite.h"
#include "object/SpriteSkin.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSkinning.h"
#include "subsystem/RenderSystem.h"
#include "texture/Texture.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace SpriteSkinGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kW = 16, kH = 64, kScale = 3;

    // a column of opaque colours: red down the image, green across it
    std::vector<uint8_t> columnPixels() {
        std::vector<uint8_t> rgba(static_cast<size_t>(kW * kH) * 4);
        for (int y = 0; y < kH; y++) {
            for (int x = 0; x < kW; x++) {
                uint8_t* p = &rgba[(static_cast<size_t>(y) * kW + x) * 4];
                p[0] = static_cast<uint8_t>(60 + y * 3);
                p[1] = static_cast<uint8_t>(40 + x * 12);
                p[2] = 200;
                p[3] = 255;
            }
        }
        return rgba;
    }

    // two bones up the column (y down: the root from the bottom to the middle, its child to the
    // top) over a 3 x 9 grid, the lower half on the root, the upper on the child, blended at the joint
    std::shared_ptr<SpriteSkinGeometry> twoBoneSkin() {
        auto skin = std::make_shared<SpriteSkinGeometry>();
        skin->bones.push_back({"lower", -1, Vector2(8, 64), Vector2(8, 32)});
        skin->bones.push_back({"upper", 0, Vector2(8, 32), Vector2(8, 0)});
        for (int row = 0; row <= 8; row++) {
            for (int column = 0; column < 3; column++) {
                const float y = static_cast<float>(row * 8);
                skin->vertices.emplace_back(static_cast<float>(column * 8), y);
                SpriteVertexWeights w;
                if (y > 32.0f) {
                    w.weights[0] = 1.0f;
                } else if (y < 32.0f) {
                    w.bones[0] = 1;
                    w.weights[0] = 1.0f;
                } else {
                    w.bones[1] = 1;
                    w.weights[0] = 0.5f;
                    w.weights[1] = 0.5f;
                }
                skin->weights.push_back(w);
            }
        }
        for (int row = 0; row < 8; row++) {
            for (int column = 0; column < 2; column++) {
                const uint16_t a = static_cast<uint16_t>(row * 3 + column), b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + 3), d = static_cast<uint16_t>(a + 4);
                skin->indices.insert(skin->indices.end(), {a, b, d, a, d, c});
            }
        }
        return skin;
    }

    class SpriteSkinGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override {
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override {
            Engine::removeScene(&scene);
        }

        bool render() {
            Scene* current = &scene;
            // a frame first: an edit's reload (a new layout, a variant to make) starts in it, so the
            // wait below sees the mesh loading
            GpuEnvironment::drawFrames(1);
            const bool ready = GpuEnvironment::runFrames([current]() { return current->getSystem<RenderSystem>()->isAllLoaded(); });
            GpuEnvironment::drawFrames(2);
            return ready;
        }

        std::vector<uint8_t> capture() {
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), int(target.getWidth()), int(target.getHeight()), rgba));
            return rgba;
        }

        // the sheet of one frame, with or without the skin, under `path`
        void registerSheet(const std::string& path, const std::filesystem::path& png, bool withSkin) {
            SpriteSheetData sheet;
            SpriteFrame frame;
            frame.name = "column";
            frame.rect = Rect(0.0f, 0.0f, kW, kH);
            frame.sourceSize = Vector2(kW, kH);
            frame.pivot = Vector2(kW / 2.0f, kH);
            if (withSkin) frame.skin = twoBoneSkin();
            sheet.frames.push_back(frame);
            sheet.pages.push_back({png.string(), kW, kH});
            auto shared = std::make_shared<const SpriteSheetData>(sheet);
            SpriteSheetPool::registerSource([path](const std::string& p) { return p == path; }, [shared](const std::string&) { return shared; });
        }

        std::unique_ptr<Sprite> makeSprite(const std::string& sheetPath, float x) {
            auto sprite = std::make_unique<Sprite>(&scene);
            sprite->setSheet(sheetPath);
            sprite->setFrame(0);
            sprite->setSize(kW * kScale, kH * kScale);
            sprite->setPivotPreset(PivotPreset::BOTTOM_CENTER);
            Texture& texture = sprite->getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
            texture.setMinFilter(TextureFilter::NEAREST);
            texture.setMagFilter(TextureFilter::NEAREST);
            sprite->setPosition(x, 20.0f, 0.0f);
            return sprite;
        }

        // the geometry input MeshSystem builds the Sprite's frame with
        static SpriteFrameGeometryInput inputOf(const SpriteComponent& sprite) {
            SpriteFrameGeometryInput input;
            input.width = sprite.width;
            input.height = sprite.height;
            input.pivot = sprite.pivotPreset;
            input.pixelsPerUnit = 1.0f;
            input.flipY = sprite.flipY;
            input.pageWidth = kW;
            input.pageHeight = kH;
            return input;
        }

        // (x, y) from the target's bottom-left, as the orthographic camera's units; rows read top first
        static bool isBackground(const std::vector<uint8_t>& rgba, int width, int x, int y) {
            const int height = static_cast<int>(rgba.size() / 4 / static_cast<size_t>(width));
            const uint8_t* p = &rgba[(static_cast<size_t>(height - 1 - y) * width + x) * 4];
            return p[0] == 255 && p[1] == 0 && p[2] == 255;
        }
    };

}

TEST_F(SpriteSkinGpu, TwoBoneBend) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sprite-skin-gpu");
    const std::filesystem::path png = dir.path() / "column.png";
    ASSERT_TRUE(writePng(png.string(), columnPixels(), kW, kH));
    const std::string skinned = "gpu-test://skinned-column", plain = "gpu-test://plain-column";
    registerSheet(skinned, png, true);
    registerSheet(plain, png, false);

    const int width = static_cast<int>(GpuEnvironment::target().getWidth());
    const float centre = static_cast<float>(width) / 2.0f;
    std::unique_ptr<Sprite> sprite = makeSprite(skinned, centre);
    SpriteSkin skin(&scene, sprite->getEntity());
    ASSERT_TRUE(render());
    ASSERT_TRUE(skin.isSkinned()) << "a skinned frame under a Sprite Skin draws through its bones";

    // the bones at their rest pose, the upper one below the lower one
    const SpriteSheetData& sheet = *SpriteSheetPool::get(skinned);
    const SpriteFrame& frame = sheet.frames[0];
    const SpriteFrameGeometryInput input = inputOf(sprite->getComponent<SpriteComponent>());
    GameObject lower(&scene), upper(&scene);
    lower.setParent(sprite.get(), false);
    upper.setParent(&lower, false);
    const SpriteBoneRest lowerRest = spriteBoneRestTransform(frame, *frame.skin, input, 0);
    const SpriteBoneRest upperRest = spriteBoneRestTransform(frame, *frame.skin, input, 1);
    lower.setPosition(lowerRest.position);
    lower.setRotation(0.0f, 0.0f, lowerRest.rotationZ);
    upper.setPosition(upperRest.position);
    upper.setRotation(0.0f, 0.0f, upperRest.rotationZ);
    skin.setRootBone(lower.getEntity());
    skin.setBones({lower.getEntity(), upper.getEntity()});
    ASSERT_TRUE(render());
    const std::vector<uint8_t> rest = capture();

    // at rest, the quad of the same frame covers the same pixels
    sprite->setSheet(plain);
    sprite->setFrame(0);
    ASSERT_TRUE(render());
    EXPECT_FALSE(skin.isSkinned()) << "a frame without a skin draws its quad";
    EXPECT_EQ(sprite->getComponent<MeshComponent>().buffer.getAttribute(AttributeType::BONEIDS), nullptr) << "the quad's layout has no bones";
    const std::vector<uint8_t> quad = capture();
    ASSERT_EQ(rest.size(), quad.size());
    size_t different = 0;
    for (size_t p = 0; p < rest.size(); p++) different += rest[p] != quad[p] ? 1 : 0;
    EXPECT_EQ(different, 0u) << "the skin at rest draws what the quad draws";

    // Flip X mirrors the skinned mesh about the pivot as it mirrors the quad, bones still at rest
    sprite->setFlipX(true);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> flippedQuad = capture();
    sprite->setSheet(skinned);
    sprite->setFrame(0);
    ASSERT_TRUE(render());
    ASSERT_TRUE(skin.isSkinned());
    const std::vector<uint8_t> flippedRest = capture();
    different = 0;
    for (size_t p = 0; p < flippedRest.size(); p++) different += flippedRest[p] != flippedQuad[p] ? 1 : 0;
    EXPECT_EQ(different, 0u) << "a flipped skin at rest draws what the flipped quad draws";
    sprite->setFlipX(false);

    upper.setRotation(0.0f, 0.0f, upperRest.rotationZ + 60.0f);
    ASSERT_TRUE(render());
    ASSERT_TRUE(skin.isSkinned());
    const std::vector<uint8_t> bent = capture();
    // near the top of the column: covered at rest, uncovered once the upper half turns away
    const int topY = 20 + kH * kScale - 12;
    EXPECT_FALSE(isBackground(rest, width, static_cast<int>(centre), topY)) << "the column's top at rest";
    EXPECT_TRUE(isBackground(bent, width, static_cast<int>(centre), topY)) << "the upper bone bent the top away";
    // the lower half stays where it was
    EXPECT_FALSE(isBackground(bent, width, static_cast<int>(centre), 20 + 12)) << "the lower half does not move";
    EXPECT_GOLDEN("sprite_skin_two_bone_bend");
    SpriteSheetPool::invalidate(skinned);
    SpriteSheetPool::invalidate(plain);
}

TEST_F(SpriteSkinGpu, AbsentAddsNothing) {
    JASPER_REQUIRE_GPU();
    TestTempDir dir("sprite-skin-absent");
    const std::filesystem::path png = dir.path() / "column.png";
    ASSERT_TRUE(writePng(png.string(), columnPixels(), kW, kH));
    const std::string skinned = "gpu-test://absent-skinned", plain = "gpu-test://absent-plain";
    registerSheet(skinned, png, true);
    registerSheet(plain, png, false);

    const float centre = static_cast<float>(GpuEnvironment::target().getWidth()) / 2.0f;
    std::unique_ptr<Sprite> sprite = makeSprite(plain, centre);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> plainImage = capture();
    const Engine::FrameStats plainStats = Engine::getFrameStats();

    // the skinned frame, no Sprite Skin: the quad, no palette, no bone attributes
    sprite->setSheet(skinned);
    sprite->setFrame(0);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> skinnedImage = capture();
    const Engine::FrameStats skinnedStats = Engine::getFrameStats();
    MeshComponent& mesh = sprite->getComponent<MeshComponent>();
    EXPECT_EQ(mesh.buffer.getAttribute(AttributeType::BONEIDS), nullptr);
    EXPECT_EQ(mesh.bonesMatrix.stored(), 0u);
    EXPECT_FALSE(mesh.submeshes[0].hasSkinning);
    EXPECT_EQ(skinnedStats.drawCalls, plainStats.drawCalls);
    EXPECT_EQ(skinnedStats.setPassCalls, plainStats.setPassCalls);
    EXPECT_EQ(skinnedStats.vertices, plainStats.vertices);
    ASSERT_EQ(plainImage.size(), skinnedImage.size());
    size_t different = 0;
    for (size_t p = 0; p < plainImage.size(); p++) different += plainImage[p] != skinnedImage[p] ? 1 : 0;
    EXPECT_EQ(different, 0u);
    SpriteSheetPool::invalidate(skinned);
    SpriteSheetPool::invalidate(plain);
}

}  // namespace SpriteSkinGpuTest_cpp
