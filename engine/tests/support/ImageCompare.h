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
        // The coarse comparisons, for images whose pixels legitimately differ between GPUs (the
        // hardware-GPU tolerances of EffectGoldenTest: simulations that diverge, hash noise built
        // on sin()). blockSize > 1 compares the mean colours of blockSize x blockSize blocks
        // instead of pixels (both images downscaled the same way, a partial block at the right and
        // bottom edges averages the pixels it has): the three limits above then count and measure
        // blocks. A block size of at least the image's width and height compares the images' mean
        // colours.
        int blockSize = 1;
        // >= 0: also the images' colour distributions: per RGBA channel a 16-bin histogram of the
        // values, the distance is the largest total-variation distance over the channels (the
        // share of the pixels, in percent, whose bin would have to change); < 0: not compared
        double maxHistogramDistance = -1.0;
        // >= 0: also the images' edge distributions: a 16-bin histogram of every pixel's largest
        // channel difference with its right and lower neighbours, compared as the colour
        // histograms are (in percent of the pixels): how much of the image is flat, soft or a
        // sharp edge. It tells a pattern's scale (a cell or line size) without its layout.
        double maxGradientHistogramDistance = -1.0;
    };

    struct CompareResult {
        bool passed = false;
        bool updated = false;           // update mode wrote the golden
        std::string message;
        int maxDiff = 0;
        double meanDiff = 0.0;
        size_t badPixels = 0;           // pixels (blocks with a blockSize) over the threshold
        double badFraction = 0.0;       // of the compared units: pixels, or blocks with a blockSize
        double histogramDistance = 0.0; // percent; computed when maxHistogramDistance >= 0
        double gradientHistogramDistance = 0.0; // percent; computed when maxGradientHistogramDistance >= 0
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

    // Upstream's per-test tolerances for hardware GPUs (measured on NVIDIA GPUs: their sin(),
    // filtering and anti-aliasing differ from the goldens' software renderer): `hardware` on a
    // hardware GPU, `reference` (the default comparison) on the goldens' renderer
    // (GpuEnvironment::isReferenceRenderer), so the goldens stay as strict there as they were
    CompareOptions hardwareTolerance(const CompareOptions& hardware, const CompareOptions& reference = {});

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
