// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_IMAGE_COMPARE_H
#define JASPER_TEST_IMAGE_COMPARE_H

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    struct CompareOptions {
        int pixelThreshold = 24;        // a pixel is bad when its largest channel difference is above this
        double maxBadFraction = 0.0005; // of all pixels
        double maxMeanDiff = 1.0;       // mean of the per-pixel largest channel difference
    };

    struct CompareResult {
        bool passed = false;
        bool updated = false;           // update mode wrote the golden
        std::string message;
        int maxDiff = 0;
        double meanDiff = 0.0;
        size_t badPixels = 0;
        double badFraction = 0.0;
        std::string actualPath;         // written on failure
        std::string diffPath;           // written on failure
    };

    // <TestPaths::goldenDir>/<name>.png; on failure writes <TestPaths::outputDir>/<name>.actual.png
    // and <name>.diff.png (the golden at 30 % gray, red where the difference is above the
    // threshold). With JASPER_TEST_UPDATE_GOLDEN=1 (the HTML UI suite: also
    // JASPER_HTMLUI_UPDATE_GOLDEN=1) it writes the golden instead and passes (glcore only).
    CompareResult compareWithGolden(const std::string& name, const std::vector<uint8_t>& rgba, int width, int height,
                                    const CompareOptions& options = {});

    // the same against any golden file, failure images in outputDirectory
    CompareResult compareWithGoldenFile(const std::string& goldenPath, const std::string& outputDirectory,
                                        const std::string& name, const std::vector<uint8_t>& rgba, int width, int height,
                                        const CompareOptions& options = {});

    // reads GpuEnvironment::target() back and compares it with golden/<name>.png
    CompareResult compareTargetWithGolden(const std::string& name, const CompareOptions& options = {});

    std::string getGoldenDirectory();
    std::string getOutputDirectory();
    bool isGoldenUpdateMode();

    // RGBA8, rows top first
    bool readPng(const std::string& path, std::vector<uint8_t>& rgba, int& width, int& height);
    bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height);

} }

#define EXPECT_GOLDEN(name) \
    do { \
        ::jasperengine::test::CompareResult goldenResult_ = ::jasperengine::test::compareTargetWithGolden(name); \
        EXPECT_TRUE(goldenResult_.passed) << goldenResult_.message; \
    } while (0)

#define EXPECT_GOLDEN_OPTIONS(name, options) \
    do { \
        ::jasperengine::test::CompareResult goldenResult_ = ::jasperengine::test::compareTargetWithGolden(name, options); \
        EXPECT_TRUE(goldenResult_.passed) << goldenResult_.message; \
    } while (0)

#endif //JASPER_TEST_IMAGE_COMPARE_H
