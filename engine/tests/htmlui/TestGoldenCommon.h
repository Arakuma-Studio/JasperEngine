// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers of the full-path goldens (TestGolden*.cpp, golden/<name>.png): a scene with
// UIDocuments drawn by Engine::systemDraw (UIDocumentSystem, the context's offscreen pass,
// the composite in the camera pass) into the harness target, read back and compared. Each
// test also probes the pixels its fixture defines, so a golden cannot pin a wrong image.

#ifndef HTMLUI_TEST_GOLDEN_COMMON_H
#define HTMLUI_TEST_GOLDEN_COMMON_H

#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/SceneFixture.h"
#include "harness/TestSystem.h"

#include "Engine.h"
#include "Log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // the scene background of SceneFixture
    constexpr int goldenBackground = 0x1E;

    // a rectangle of pixels, x1/y1 exclusive
    struct PixelRect {
        int x0, y0, x1, y1;
    };

    // pixels of a region that differ from a reference color: their bounding box and count
    struct InkBox {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;   // inclusive; empty when x1 < x0
        size_t count = 0;

        bool empty() const{ return count == 0; }
        int width() const{ return empty() ? 0 : x1 - x0 + 1; }
        int height() const{ return empty() ? 0 : y1 - y0 + 1; }
        float centerX() const{ return (x0 + x1) * 0.5f; }
    };

    // Acutance (S16 AC10): the mean Sobel gradient magnitude of the luminance over the edge pixels
    // of the ink box, those whose gradient is at least a tenth of the box's largest. Sharp text has
    // steep, narrow edges and a stretched bitmap wide, shallow ones; taking the edge pixels only
    // keeps it independent of the text's size (more pixels inside the strokes do not dilute it).
    inline double acutance(const std::vector<uint8_t>& rgba, int width, int height, const InkBox& box){
        if (box.empty())
            return 0.0;
        auto luma = [&](int x, int y){
            x = std::clamp(x, 0, width - 1);
            y = std::clamp(y, 0, height - 1);
            const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        };
        std::vector<double> magnitudes;
        magnitudes.reserve((size_t)box.width() * (size_t)box.height());
        double largest = 0.0;
        for (int y = box.y0; y <= box.y1; y++){
            for (int x = box.x0; x <= box.x1; x++){
                const double gx = (luma(x + 1, y - 1) + 2.0 * luma(x + 1, y) + luma(x + 1, y + 1)) -
                                  (luma(x - 1, y - 1) + 2.0 * luma(x - 1, y) + luma(x - 1, y + 1));
                const double gy = (luma(x - 1, y + 1) + 2.0 * luma(x, y + 1) + luma(x + 1, y + 1)) -
                                  (luma(x - 1, y - 1) + 2.0 * luma(x, y - 1) + luma(x + 1, y - 1));
                const double magnitude = std::sqrt(gx * gx + gy * gy);
                magnitudes.push_back(magnitude);
                largest = std::max(largest, magnitude);
            }
        }
        double sum = 0.0;
        size_t edges = 0;
        for (double magnitude : magnitudes){
            if (magnitude >= 0.1 * largest && magnitude > 0.0){
                sum += magnitude;
                edges++;
            }
        }
        return edges ? sum / (double)edges : 0.0;
    }

    // the harness target read back, rows top first
    struct Screen {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;

        bool read(){
            Framebuffer& target = GpuEnvironment::target();
            width = int(target.getWidth());
            height = int(target.getHeight());
            return readTexture(target.getRender().getColorTexture(), width, height, rgba);
        }

        const uint8_t* at(int x, int y) const{
            return &rgba[(size_t(y) * width + x) * 4];
        }

        ::testing::AssertionResult is(int x, int y, int r, int g, int b, int tolerance = 2) const{
            if (x < 0 || y < 0 || x >= width || y >= height)
                return ::testing::AssertionFailure() << "pixel (" << x << ", " << y << ") is outside the " << width << "x" << height << " image";
            const uint8_t* p = at(x, y);
            if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance)
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "pixel (" << x << ", " << y << ") is (" << int(p[0]) << ", " << int(p[1]) << ", "
                                                 << int(p[2]) << "), expected (" << r << ", " << g << ", " << b << ") +-" << tolerance;
        }

        ::testing::AssertionResult isBackground(int x, int y) const{
            return is(x, y, goldenBackground, goldenBackground, goldenBackground);
        }

        // pixels whose largest channel difference from (r, g, b) is above threshold
        InkBox ink(const PixelRect& region, int r, int g, int b, int threshold = 40) const{
            InkBox box;
            for (int y = std::max(region.y0, 0); y < std::min(region.y1, height); y++){
                for (int x = std::max(region.x0, 0); x < std::min(region.x1, width); x++){
                    const uint8_t* p = at(x, y);
                    const int d = std::max(std::abs(p[0] - r), std::max(std::abs(p[1] - g), std::abs(p[2] - b)));
                    if (d <= threshold)
                        continue;
                    if (box.count == 0){
                        box.x0 = box.x1 = x;
                        box.y0 = box.y1 = y;
                    }else{
                        box.x0 = std::min(box.x0, x);
                        box.x1 = std::max(box.x1, x);
                        box.y0 = std::min(box.y0, y);
                        box.y1 = std::max(box.y1, y);
                    }
                    box.count++;
                }
            }
            return box;
        }

        InkBox inkOnBackground(const PixelRect& region, int threshold = 40) const{
            return ink(region, goldenBackground, goldenBackground, goldenBackground, threshold);
        }

        // pixels of a region within tolerance of (r, g, b)
        size_t countColor(const PixelRect& region, int r, int g, int b, int tolerance = 24) const{
            size_t count = 0;
            for (int y = std::max(region.y0, 0); y < std::min(region.y1, height); y++){
                for (int x = std::max(region.x0, 0); x < std::min(region.x1, width); x++){
                    const uint8_t* p = at(x, y);
                    if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance)
                        count++;
                }
            }
            return count;
        }

        // golden/<name>.png against this read-back image
        CompareResult compare(const std::string& name, const CompareOptions& options = {}) const{
            return compareWithGolden(name, rgba, width, height, options);
        }
    };

    // Every full-path test: a fresh 640x360 view (screen = canvas) and an empty log; at the
    // end no [HTML UI] error and, unless the test declares them, no [HTML UI] warning.
    class FullPathGoldenTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), expectedWarnings) << logText(D_LOG_WARN);
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
        }

        static std::string logText(int type){
            std::string text;
            for (const auto& line : TestSystem::get().getLogs()){
                if (line.type == type){
                    text += line.message;
                    text += "\n";
                }
            }
            return text;
        }

        // draws and reads the target back; ASSERT-friendly
        static ::testing::AssertionResult capture(Screen& screen, int frames = 1){
            GpuEnvironment::drawFrames(frames);
            if (!screen.read())
                return ::testing::AssertionFailure() << "readback of the target failed";
            return ::testing::AssertionSuccess();
        }

        size_t expectedWarnings = 0;
    };

#define EXPECT_SCREEN_GOLDEN(screen, name) \
    do { \
        ::jasperengine::test::CompareResult goldenResult_ = (screen).compare(name); \
        EXPECT_TRUE(goldenResult_.passed) << goldenResult_.message; \
    } while (0)

#define EXPECT_SCREEN_GOLDEN_OPTIONS(screen, name, options) \
    do { \
        ::jasperengine::test::CompareResult goldenResult_ = (screen).compare(name, options); \
        EXPECT_TRUE(goldenResult_.passed) << goldenResult_.message; \
    } while (0)

} }

#endif //HTMLUI_TEST_GOLDEN_COMMON_H
