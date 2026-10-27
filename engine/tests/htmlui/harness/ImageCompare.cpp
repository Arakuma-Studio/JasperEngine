// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImageCompare.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;

std::string jasperengine::test::getGoldenDirectory(){
    return JASPER_HTMLUI_TEST_SOURCE_DIR "/golden";
}

std::string jasperengine::test::getOutputDirectory(){
    return JASPER_HTMLUI_TEST_BINARY_DIR "/htmlui-test-output";
}

bool jasperengine::test::isGoldenUpdateMode(){
    const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
    return value && std::strcmp(value, "1") == 0;
}

bool jasperengine::test::readPng(const std::string& path, std::vector<uint8_t>& rgba, int& width, int& height){
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels){
        rgba.clear();
        width = height = 0;
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

bool jasperengine::test::writePng(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height){
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4)
        return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    return stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
}

CompareResult jasperengine::test::compareWithGoldenFile(const std::string& goldenPath, const std::string& outputDirectory,
                                                        const std::string& name, const std::vector<uint8_t>& rgba, int width, int height,
                                                        const CompareOptions& options){
    CompareResult result;

    if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4){
        result.message = "'" + name + "': no image to compare (" + std::to_string(width) + "x" + std::to_string(height) + ")";
        return result;
    }

    // JASPER_HTMLUI_WRITE_ACTUAL=1: every compared image is kept (<name>.actual.png in the output
    // directory), on every backend, to compare two builds' images exactly
    const char* writeActual = std::getenv("JASPER_HTMLUI_WRITE_ACTUAL");
    if (writeActual && std::strcmp(writeActual, "1") == 0)
        writePng(outputDirectory + "/" + name + ".actual.png", rgba, width, height);

    if (isGoldenUpdateMode()){
#if defined(SOKOL_VULKAN)
        result.message = "'" + name + "': goldens are only updated on glcore (HTMLUI_BACKEND=glcore)";
        return result;
#else
        if (!writePng(goldenPath, rgba, width, height)){
            result.message = "'" + name + "': could not write golden " + goldenPath;
            return result;
        }
        result.passed = true;
        result.updated = true;
        result.message = "'" + name + "': golden updated " + goldenPath;
        return result;
#endif
    }

    const std::string actualPath = outputDirectory + "/" + name + ".actual.png";
    const std::string diffPath = outputDirectory + "/" + name + ".diff.png";

    std::vector<uint8_t> golden;
    int goldenWidth = 0;
    int goldenHeight = 0;
    if (!readPng(goldenPath, golden, goldenWidth, goldenHeight)){
        if (writePng(actualPath, rgba, width, height))
            result.actualPath = actualPath;
        result.message = "'" + name + "': golden " + goldenPath + " is missing (actual image: " + actualPath +
                         "; create it with JASPER_HTMLUI_UPDATE_GOLDEN=1)";
        return result;
    }

    if (goldenWidth != width || goldenHeight != height){
        if (writePng(actualPath, rgba, width, height))
            result.actualPath = actualPath;
        result.message = "'" + name + "': size " + std::to_string(width) + "x" + std::to_string(height) +
                         " differs from the golden " + std::to_string(goldenWidth) + "x" + std::to_string(goldenHeight);
        return result;
    }

    // the compared units: pixels, or the mean colours of blockSize x blockSize blocks
    const int block = std::max(1, options.blockSize);
    const int columns = (width + block - 1) / block;
    const int rows = (height + block - 1) / block;
    std::vector<int> actualUnits;
    std::vector<int> goldenUnits;
    if (block > 1){
        actualUnits.assign(size_t(columns) * rows * 4, 0);
        goldenUnits.assign(size_t(columns) * rows * 4, 0);
        for (int by = 0; by < rows; by++){
            for (int bx = 0; bx < columns; bx++){
                const int x0 = bx * block, y0 = by * block;
                const int x1 = std::min(width, x0 + block), y1 = std::min(height, y0 + block);
                const double count = double(x1 - x0) * double(y1 - y0);
                double a[4] = {0.0, 0.0, 0.0, 0.0};
                double g[4] = {0.0, 0.0, 0.0, 0.0};
                for (int y = y0; y < y1; y++){
                    for (int x = x0; x < x1; x++){
                        const size_t i = (size_t(y) * width + x) * 4;
                        for (int c = 0; c < 4; c++){
                            a[c] += rgba[i + c];
                            g[c] += golden[i + c];
                        }
                    }
                }
                for (int c = 0; c < 4; c++){
                    actualUnits[(size_t(by) * columns + bx) * 4 + c] = int(std::lround(a[c] / count));
                    goldenUnits[(size_t(by) * columns + bx) * 4 + c] = int(std::lround(g[c] / count));
                }
            }
        }
    }
    auto unitDifference = [&](size_t unit){
        int d = 0;
        for (int c = 0; c < 4; c++){
            const int a = block > 1 ? actualUnits[unit * 4 + c] : int(rgba[unit * 4 + c]);
            const int g = block > 1 ? goldenUnits[unit * 4 + c] : int(golden[unit * 4 + c]);
            d = std::max(d, std::abs(a - g));
        }
        return d;
    };

    const size_t pixelCount = static_cast<size_t>(width) * height;
    const size_t unitCount = block > 1 ? size_t(columns) * rows : pixelCount;
    std::vector<int> unitDiffs(unitCount);
    double sum = 0.0;
    for (size_t u = 0; u < unitCount; u++){
        const int d = unitDifference(u);
        unitDiffs[u] = d;
        sum += d;
        result.maxDiff = std::max(result.maxDiff, d);
        if (d > options.pixelThreshold)
            result.badPixels++;
    }

    // golden at 30 % gray, red where the pixel (its block) is bad
    std::vector<uint8_t> diff(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; i++){
        const uint8_t* g = &golden[i * 4];
        const int x = int(i % size_t(width)), y = int(i / size_t(width));
        const size_t unit = block > 1 ? size_t(y / block) * columns + size_t(x / block) : i;
        const int d = unitDiffs[unit];
        const int gray = int((0.299 * g[0] + 0.587 * g[1] + 0.114 * g[2]) * 0.3);
        uint8_t* out = &diff[i * 4];
        if (d > options.pixelThreshold){
            out[0] = uint8_t(std::min(255, 96 + d * 159 / 255));
            out[1] = uint8_t(gray / 2);
            out[2] = uint8_t(gray / 2);
        }else{
            out[0] = out[1] = out[2] = uint8_t(gray);
        }
        out[3] = 255;
    }

    // the colour distributions: per channel the share of the pixels whose 16-bin histogram bin differs
    bool histogramPasses = true;
    if (options.maxHistogramDistance >= 0.0){
        for (int c = 0; c < 4; c++){
            long long actualBins[16] = {};
            long long goldenBins[16] = {};
            for (size_t i = 0; i < pixelCount; i++){
                actualBins[rgba[i * 4 + c] >> 4]++;
                goldenBins[golden[i * 4 + c] >> 4]++;
            }
            long long moved = 0;
            for (int b = 0; b < 16; b++)
                moved += std::llabs(actualBins[b] - goldenBins[b]);
            result.histogramDistance = std::max(result.histogramDistance, 100.0 * double(moved) / 2.0 / double(pixelCount));
        }
        histogramPasses = result.histogramDistance <= options.maxHistogramDistance;
    }

    result.meanDiff = sum / double(unitCount);
    result.badFraction = double(result.badPixels) / double(unitCount);
    result.passed = result.badFraction <= options.maxBadFraction && result.meanDiff <= options.maxMeanDiff && histogramPasses;

    std::ostringstream message;
    message << "'" << name << "': ";
    if (block > 1)
        message << "in " << block << "x" << block << " blocks (" << columns << "x" << rows << "), ";
    message << result.badPixels << (block > 1 ? " blocks" : " pixels") << " over the threshold " << options.pixelThreshold
            << " (" << result.badFraction * 100.0 << " %, limit " << options.maxBadFraction * 100.0 << " %), mean difference "
            << result.meanDiff << " (limit " << options.maxMeanDiff << "), largest " << result.maxDiff;
    if (options.maxHistogramDistance >= 0.0)
        message << ", histogram distance " << result.histogramDistance << " % (limit " << options.maxHistogramDistance << " %)";

    if (!result.passed){
        if (writePng(actualPath, rgba, width, height))
            result.actualPath = actualPath;
        if (writePng(diffPath, diff, width, height))
            result.diffPath = diffPath;
        message << "; actual " << actualPath << ", diff " << diffPath;
    }
    result.message = message.str();

    return result;
}

CompareResult jasperengine::test::compareWithGolden(const std::string& name, const std::vector<uint8_t>& rgba, int width, int height,
                                                    const CompareOptions& options){
    return compareWithGoldenFile(getGoldenDirectory() + "/" + name + ".png", getOutputDirectory(), name, rgba, width, height, options);
}

CompareResult jasperengine::test::compareTargetWithGolden(const std::string& name, const CompareOptions& options){
    if (!GpuEnvironment::isReady()){
        CompareResult result;
        result.message = "'" + name + "': no GPU target to read back";
        return result;
    }

    Framebuffer& target = GpuEnvironment::target();
    const int width = int(target.getWidth());
    const int height = int(target.getHeight());

    std::vector<uint8_t> rgba;
    if (!readTexture(target.getRender().getColorTexture(), width, height, rgba)){
        CompareResult result;
        result.message = "'" + name + "': the target could not be read back";
        return result;
    }

    return compareWithGolden(name, rgba, width, height, options);
}
