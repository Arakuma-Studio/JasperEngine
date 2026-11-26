// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The vignette's falloffs on the headless device (docs/specs/S94-rendering-anim-gaps.md F19,
// AC19): Radii darkens as smoothstep(start, end, r), r the roundness-scaled radius (1 the middle of
// the screen's edge), untouched inside start and fully darkened past end; Smoothness keeps the
// engine's original maths. Both in the shaders' tail (T0) and in the uber pass (with FXAA). A white
// sky fills the view (the sky's shader applies the post-processing colour as every scene shader).

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/environment/Skybox.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class PostColorGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Skybox sky{&scene};

        void SetUp() override{
            GpuEnvironment::resetView();
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 0.0f, 0.0f);
            camera.setTarget(0.0f, 0.0f, -1.0f);
            camera.activate();
            sky.setColor(1.0f, 1.0f, 1.0f);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        // the centre row of a frame with these settings, and the view's size
        std::vector<uint8_t> centreRow(const PostProcessingSettings& settings, int& width, int& height){
            scene.setPostProcessing(settings);
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            }));
            GpuEnvironment::drawFrames(3);
            Framebuffer& target = GpuEnvironment::target();
            width = (int)target.getWidth();
            height = (int)target.getHeight();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), width, height, rgba);
            std::vector<uint8_t> row;
            if (rgba.size() < (size_t)width * height * 4)
                return row;
            const size_t start = (size_t)(height / 2) * width * 4;
            row.assign(rgba.begin() + (std::ptrdiff_t)start, rgba.begin() + (std::ptrdiff_t)(start + (size_t)width * 4));
            return row;
        }

        // every pixel of the centre row within tolerance of 255 * (1 - darkening(r))
        void expectRow(const PostProcessingSettings& settings, const std::function<float(float)>& darkening, int tolerance,
                       const std::string& what){
            int width = 0, height = 0;
            const std::vector<uint8_t> row = centreRow(settings, width, height);
            ASSERT_EQ(row.size(), (size_t)width * 4) << what;
            // the row's pixel centres are half a pixel off the view's centre vertically
            const float halfHeight = 0.5f * (float)height;
            const float dy = 0.5f / halfHeight;
            int worst = 0;
            int worstX = 0;
            for (int x = 0; x < width; x++){
                const float dx = ((float)x + 0.5f - 0.5f * (float)width) / halfHeight;
                const float r = std::sqrt(dx * dx + dy * dy);
                const int expected = (int)std::lround(255.0f * (1.0f - darkening(r)));
                const int diff = std::abs((int)row[(size_t)x * 4] - expected);
                if (diff > worst){
                    worst = diff;
                    worstX = x;
                }
            }
            EXPECT_LE(worst, tolerance) << what << ": the worst pixel is x " << worstX << " (" << (int)row[(size_t)worstX * 4] << ")";
        }

        static PostProcessingSettings vignette(VignetteFalloff falloff){
            PostProcessingSettings settings;
            settings.vignetteEnabled = true;
            settings.vignetteIntensity = 1.0f;
            settings.vignetteRoundness = 1.0f;
            settings.vignetteColor = Vector3(0.0f, 0.0f, 0.0f);
            settings.vignetteFalloff = falloff;
            settings.vignetteStart = 0.4f;
            settings.vignetteEnd = 0.9f;
            settings.vignetteSmoothness = 0.5f;
            return settings;
        }
    };

    float smoothstep(float a, float b, float x){
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

}

TEST_F(PostColorGpu, VignetteRadii){
    JASPER_REQUIRE_GPU();
    const PostProcessingSettings radii = vignette(VignetteFalloff::RADII);
    auto darkening = [](float r){ return smoothstep(0.4f, 0.9f, r); };
    expectRow(radii, darkening, 1, "radii in the shaders");
    // the same in the uber pass, which takes the vignette when it runs
    PostProcessingSettings uber = radii;
    uber.fxaaEnabled = true;
    expectRow(uber, darkening, 2, "radii in the uber pass");
    EXPECT_NO_ENGINE_ERRORS();
}

// Smoothness: the falloff over r squared from 0.6 (1 - smoothness), 0.2 + 1.6 smoothness wide
TEST_F(PostColorGpu, VignetteSmoothnessKeepsItsMaths){
    JASPER_REQUIRE_GPU();
    auto darkening = [](float r){
        const float start = 0.6f * (1.0f - 0.5f);
        const float width = 0.2f + 1.6f * 0.5f;
        const float w = std::clamp(r * r / width - start / width, 0.0f, 1.0f);
        return w * w * (3.0f - 2.0f * w);
    };
    expectRow(vignette(VignetteFalloff::SMOOTHNESS), darkening, 1, "smoothness");
    EXPECT_NO_ENGINE_ERRORS();
}
