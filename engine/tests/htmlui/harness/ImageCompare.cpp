// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImageCompare.h"

#include "GpuEnvironment.h"
#include "Readback.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
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

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> diff(pixelCount * 4);
    double sum = 0.0;

    for (size_t i = 0; i < pixelCount; i++){
        const uint8_t* a = &rgba[i * 4];
        const uint8_t* g = &golden[i * 4];
        int d = 0;
        for (int c = 0; c < 4; c++){
            d = std::max(d, std::abs(int(a[c]) - int(g[c])));
        }
        sum += d;
        result.maxDiff = std::max(result.maxDiff, d);

        // golden at 30 % gray, red where the pixel is bad
        const int gray = int((0.299 * g[0] + 0.587 * g[1] + 0.114 * g[2]) * 0.3);
        uint8_t* out = &diff[i * 4];
        if (d > options.pixelThreshold){
            result.badPixels++;
            out[0] = uint8_t(std::min(255, 96 + d * 159 / 255));
            out[1] = uint8_t(gray / 2);
            out[2] = uint8_t(gray / 2);
        }else{
            out[0] = out[1] = out[2] = uint8_t(gray);
        }
        out[3] = 255;
    }

    result.meanDiff = sum / double(pixelCount);
    result.badFraction = double(result.badPixels) / double(pixelCount);
    result.passed = result.badFraction <= options.maxBadFraction && result.meanDiff <= options.maxMeanDiff;

    std::ostringstream message;
    message << "'" << name << "': " << result.badPixels << " pixels over the threshold " << options.pixelThreshold
            << " (" << result.badFraction * 100.0 << " %, limit " << options.maxBadFraction * 100.0 << " %), mean difference "
            << result.meanDiff << " (limit " << options.maxMeanDiff << "), largest " << result.maxDiff;

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
