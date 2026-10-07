// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/ImageCompare.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 40;
    const int height = 20;

    std::vector<uint8_t> makeImage(uint8_t r, uint8_t g, uint8_t b){
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        for (size_t i = 0; i < rgba.size(); i += 4){
            rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = 255;
        }
        return rgba;
    }

    void setPixel(std::vector<uint8_t>& rgba, int x, int y, uint8_t r, uint8_t g, uint8_t b){
        uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
        p[0] = r; p[1] = g; p[2] = b;
    }

    // goldens of this suite live in the build directory, never in golden/; the update
    // switch of the run (JASPER_HTMLUI_UPDATE_GOLDEN) is put back after each test
    class ImageCompareTest : public ::testing::Test {
    protected:
        void SetUp() override{
            directory = std::string(JASPER_HTMLUI_TEST_BINARY_DIR) + "/image-compare-test";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory);

            const char* update = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
            hadUpdate = update != nullptr;
            savedUpdate = update ? update : "";
            unsetenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        }

        void TearDown() override{
            if (hadUpdate)
                setenv("JASPER_HTMLUI_UPDATE_GOLDEN", savedUpdate.c_str(), 1);
            else
                unsetenv("JASPER_HTMLUI_UPDATE_GOLDEN");
            std::filesystem::remove_all(directory);
        }

        std::string golden(const std::string& name) const{
            return directory + "/" + name + ".png";
        }

        CompareResult compare(const std::string& name, const std::vector<uint8_t>& rgba, const CompareOptions& options = {}){
            return compareWithGoldenFile(golden(name), directory + "/output", name, rgba, width, height, options);
        }

        std::string directory;
        bool hadUpdate = false;
        std::string savedUpdate;
    };

}

TEST_F(ImageCompareTest, PngRoundTrip){
    std::vector<uint8_t> image = makeImage(10, 20, 30);
    setPixel(image, 0, 0, 255, 0, 0);
    ASSERT_TRUE(writePng(golden("roundtrip"), image, width, height));

    std::vector<uint8_t> read;
    int w = 0, h = 0;
    ASSERT_TRUE(readPng(golden("roundtrip"), read, w, h));
    EXPECT_EQ(w, width);
    EXPECT_EQ(h, height);
    EXPECT_EQ(read, image);

    EXPECT_FALSE(readPng(golden("not-there"), read, w, h));
    EXPECT_FALSE(writePng(golden("bad-size"), image, width + 1, height));
}

TEST_F(ImageCompareTest, Identical){
    const std::vector<uint8_t> image = makeImage(30, 30, 30);
    ASSERT_TRUE(writePng(golden("identical"), image, width, height));

    CompareResult result = compare("identical", image);
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_EQ(result.maxDiff, 0);
    EXPECT_EQ(result.badPixels, 0u);
    EXPECT_DOUBLE_EQ(result.meanDiff, 0.0);
    EXPECT_TRUE(result.actualPath.empty());
    EXPECT_FALSE(std::filesystem::exists(directory + "/output/identical.actual.png"));
}

TEST_F(ImageCompareTest, WithinTolerance){
    ASSERT_TRUE(writePng(golden("tolerance"), makeImage(100, 100, 100), width, height));

    // every pixel off by the threshold itself: not bad, mean 24 is over the default 1.0
    CompareResult result = compare("tolerance", makeImage(124, 100, 100));
    EXPECT_EQ(result.badPixels, 0u);
    EXPECT_FALSE(result.passed);

    // a small mean difference everywhere passes
    result = compare("tolerance", makeImage(101, 100, 99));
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_EQ(result.maxDiff, 1);

    // one bad pixel of 800 is 0.125 %: passes with a looser bad-pixel limit only
    std::vector<uint8_t> onePixel = makeImage(100, 100, 100);
    setPixel(onePixel, 5, 5, 200, 100, 100);
    result = compare("tolerance", onePixel);
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.badPixels, 1u);

    CompareOptions loose;
    loose.maxBadFraction = 0.002;
    result = compare("tolerance", onePixel, loose);
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(ImageCompareTest, FailureWritesActualAndDiff){
    ASSERT_TRUE(writePng(golden("failure"), makeImage(0, 0, 0), width, height));

    std::vector<uint8_t> image = makeImage(0, 0, 0);
    for (int x = 0; x < 10; x++)
        setPixel(image, x, 3, 255, 255, 255);

    CompareResult result = compare("failure", image);
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.badPixels, 10u);
    EXPECT_EQ(result.maxDiff, 255);
    EXPECT_NE(result.message.find("failure"), std::string::npos);

    std::vector<uint8_t> actual;
    int w = 0, h = 0;
    ASSERT_TRUE(readPng(directory + "/output/failure.actual.png", actual, w, h));
    EXPECT_EQ(actual, image);

    std::vector<uint8_t> diff;
    ASSERT_TRUE(readPng(directory + "/output/failure.diff.png", diff, w, h));
    // red where it differs, the dimmed golden elsewhere
    const uint8_t* bad = &diff[(size_t(3) * width + 0) * 4];
    EXPECT_EQ(bad[0], 255);
    EXPECT_LT(bad[1], 64);
    const uint8_t* good = &diff[(size_t(10) * width + 10) * 4];
    EXPECT_EQ(good[0], 0);
    EXPECT_EQ(good[0], good[1]);
}

TEST_F(ImageCompareTest, SizeMismatchFails){
    ASSERT_TRUE(writePng(golden("size"), makeImage(0, 0, 0), width, height));
    std::vector<uint8_t> small(size_t(width / 2) * height * 4, 0);

    CompareResult result = compareWithGoldenFile(golden("size"), directory + "/output", "size", small, width / 2, height);
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.message.find("differs from the golden"), std::string::npos);
}

TEST_F(ImageCompareTest, MissingGoldenFails){
    CompareResult result = compare("missing", makeImage(1, 2, 3));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.message.find("is missing"), std::string::npos);
    // the actual image is kept so it can be inspected or promoted
    EXPECT_TRUE(std::filesystem::exists(directory + "/output/missing.actual.png"));
    EXPECT_FALSE(std::filesystem::exists(golden("missing")));
}

TEST_F(ImageCompareTest, UpdateMode){
    setenv("JASPER_HTMLUI_UPDATE_GOLDEN", "1", 1);
    ASSERT_TRUE(isGoldenUpdateMode());

    const std::vector<uint8_t> image = makeImage(7, 8, 9);
    CompareResult result = compare("update", image);
#if defined(SOKOL_VULKAN)
    // goldens come from glcore only
    EXPECT_FALSE(result.passed);
    EXPECT_FALSE(std::filesystem::exists(golden("update")));
#else
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_TRUE(result.updated);
    std::vector<uint8_t> written;
    int w = 0, h = 0;
    ASSERT_TRUE(readPng(golden("update"), written, w, h));
    EXPECT_EQ(written, image);
#endif

    unsetenv("JASPER_HTMLUI_UPDATE_GOLDEN");
    EXPECT_FALSE(isGoldenUpdateMode());
    setenv("JASPER_HTMLUI_UPDATE_GOLDEN", "0", 1);
    EXPECT_FALSE(isGoldenUpdateMode());
}

TEST_F(ImageCompareTest, EmptyImageFails){
    CompareResult result = compareWithGoldenFile(golden("empty"), directory + "/output", "empty", {}, 0, 0);
    EXPECT_FALSE(result.passed);
}

TEST_F(ImageCompareTest, SubdirectoryNames){
    // names with '/' (p1/..., compat/...) keep their directories for goldens and outputs
    CompareResult result = compare("sub/dir/missing", makeImage(1, 2, 3));
    EXPECT_FALSE(result.passed);
    EXPECT_TRUE(std::filesystem::exists(directory + "/output/sub/dir/missing.actual.png"));

#if !defined(SOKOL_VULKAN)
    setenv("JASPER_HTMLUI_UPDATE_GOLDEN", "1", 1);
    result = compare("sub/dir/update", makeImage(4, 5, 6));
    EXPECT_TRUE(result.passed) << result.message;
    EXPECT_TRUE(std::filesystem::exists(golden("sub/dir/update")));
    unsetenv("JASPER_HTMLUI_UPDATE_GOLDEN");
    result = compare("sub/dir/update", makeImage(4, 5, 6));
    EXPECT_TRUE(result.passed) << result.message;
#endif
}
