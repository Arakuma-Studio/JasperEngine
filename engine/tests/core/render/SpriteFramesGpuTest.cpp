// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Sprite's frame on the headless device (docs/specs/S63-saved-output-determinism.md R1): the
// generated code holds a frame as it was authored, so a frame in pixels has to draw what the same
// frame normalised draws, also when it was chosen before the texture's size was known.

#include "Engine.h"
#include "Scene.h"
#include "component/SpriteComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int SHEET_WIDTH = 64;
    const int SHEET_HEIGHT = 32;

    class SpriteFramesGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        // two cells of 32 x 32: red, then green
        std::vector<uint8_t> pixels;

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.activate();
            Engine::setScene(&scene);
            pixels.assign((size_t)SHEET_WIDTH * SHEET_HEIGHT * 4, 255);
            for (int y = 0; y < SHEET_HEIGHT; y++){
                for (int x = 0; x < SHEET_WIDTH; x++){
                    uint8_t* p = &pixels[((size_t)y * SHEET_WIDTH + x) * 4];
                    p[0] = x < SHEET_WIDTH / 2 ? 255 : 0;
                    p[1] = x < SHEET_WIDTH / 2 ? 0 : 255;
                    p[2] = 0;
                }
            }
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        TextureData sheet(){
            return TextureData(SHEET_WIDTH, SHEET_HEIGHT, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
        }

        // over the whole canvas, whatever its size: the frame fills the target
        void place(Sprite& sprite){
            sprite.setSize(8000, 8000);
            sprite.setPivotPreset(PivotPreset::CENTER);
            sprite.setPosition(0.0f, 0.0f, 0.0f);
        }

        std::vector<uint8_t> draw(){
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }));
            GpuEnvironment::drawFrames(2);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        void expectGreenMiddle(const std::vector<uint8_t>& rgba){
            Framebuffer& target = GpuEnvironment::target();
            const size_t middle = ((size_t)target.getHeight() / 2 * target.getWidth() + target.getWidth() / 2) * 4;
            ASSERT_GT(rgba.size(), middle + 2);
            EXPECT_LT(rgba[middle], 40) << "the frame's cell is the sheet's second";
            EXPECT_GT(rgba[middle + 1], 200) << "the frame's cell is the sheet's second";
        }
    };

}

TEST_F(SpriteFramesGpu, AFrameInPixelsDrawsWhatTheNormalisedFrameDraws){
    JASPER_REQUIRE_GPU();
    Sprite sprite(&scene);
    sprite.setTexture("sprite-frames-sheet", sheet());
    place(sprite);

    sprite.addFrame("pixels", 32.0f, 0.0f, 32.0f, 32.0f);
    sprite.setFrame("pixels");
    const std::vector<uint8_t> inPixels = draw();
    expectGreenMiddle(inPixels);

    sprite.addFrame("normalised", 0.5f, 0.0f, 0.5f, 1.0f);
    sprite.setFrame("normalised");
    EXPECT_TRUE(draw() == inPixels);
    EXPECT_NO_ENGINE_ERRORS();
}

// what a game does when it shows a frame before its texture finished loading
TEST_F(SpriteFramesGpu, AFrameChosenBeforeTheTextureSizeIsKnownDrawsTheSame){
    JASPER_REQUIRE_GPU();
    Sprite loaded(&scene);
    loaded.setTexture("sprite-frames-sheet", sheet());
    place(loaded);
    loaded.addFrame("pixels", 32.0f, 0.0f, 32.0f, 32.0f);
    loaded.setFrame("pixels");
    const std::vector<uint8_t> expected = draw();
    expectGreenMiddle(expected);
    loaded.setVisible(false);

    Sprite waiting(&scene);
    place(waiting);
    waiting.addFrame("pixels", 32.0f, 0.0f, 32.0f, 32.0f);
    waiting.setFrame("pixels");
    EXPECT_TRUE(scene.getComponent<SpriteComponent>(waiting.getEntity()).needUpdateFrameRect) << "no texture yet: the frame waits for its size";
    waiting.setTexture("sprite-frames-sheet", sheet());

    EXPECT_TRUE(draw() == expected);
    EXPECT_FALSE(scene.getComponent<SpriteComponent>(waiting.getEntity()).needUpdateFrameRect);
    EXPECT_NO_ENGINE_ERRORS();
}
