// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 R19: a 2048² BC7 KTX2 with its mips (zstd) loads in <= 12 ms on the loader thread (read,
// inflate, levels) and <= 0.5 ms of main-thread upload, at least 3x faster than today's 2048² PNG
// decode with its mip chain. The BC7 payload is made of a few hundred distinct blocks (what an RDO
// encode leaves; this binary has no encoder). Times are printed as [bench] lines; budgets are
// enforced with JASPER_TEST_PERF_SLACK (the benchmark server E).

#include "render/TextureRender.h"
#include "texture/Ktx2.h"
#include "texture/MipChain.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "TempDir.h"
#include "support/AllocationCounter.h"

#include "stb_image.h"
#include "stb_image_write.h"
#include "zstd.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    double loadMedian(std::vector<double> values){
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    double loadMs(std::chrono::steady_clock::time_point start){
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool loadZstd(std::span<const uint8_t> in, std::vector<uint8_t>& out, int level, void*){
        out.resize(ZSTD_compressBound(in.size()));
        const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
        if (ZSTD_isError(written))
            return false;
        out.resize(written);
        return true;
    }

    std::vector<uint8_t> loadFile(const std::string& path){
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

}

TEST(TextureLoadBenchGpu, Bc7Ktx2AgainstPng){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    std::mt19937 random(14);
    std::vector<std::array<uint8_t, 16>> palette(384);
    for (auto& block : palette)
        for (uint8_t& byte : block)
            byte = (uint8_t)random();

    // the cooked file
    std::vector<std::vector<uint8_t>> levels;
    for (int size = 2048; ; size /= 2){
        const int blocks = std::max(1, (size + 3) / 4);
        std::vector<uint8_t> level((size_t)blocks * blocks * 16);
        for (size_t b = 0; b < (size_t)blocks * blocks; b++)
            std::memcpy(&level[b * 16], palette[random() % palette.size()].data(), 16);
        levels.push_back(std::move(level));
        if (size == 1)
            break;
    }
    Ktx2::WriteDesc desc;
    desc.format = GpuTextureFormat::BC7;
    desc.srgb = true;
    desc.width = desc.height = 2048;
    for (const std::vector<uint8_t>& level : levels)
        desc.levels.push_back(std::span<const uint8_t>(level));
    std::vector<uint8_t> file;
    std::string error;
    ASSERT_TRUE(Ktx2::write(desc, file, error, 15, loadZstd, nullptr)) << error;
    const std::string ktx2Path = dir.file("photo.ktx2").generic_string();
    std::ofstream(ktx2Path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());

    // the PNG it replaces: 2048² noise-like colour
    std::vector<uint8_t> rgba((size_t)2048 * 2048 * 4);
    for (size_t i = 0; i < rgba.size(); i++)
        rgba[i] = (uint8_t)((i % 4 == 3) ? 255 : (random() & 0x3F) + (uint8_t)((i / 8192) & 0x7F));
    const std::string pngPath = dir.file("photo.png").generic_string();
    ASSERT_TRUE(stbi_write_png(pngPath.c_str(), 2048, 2048, 4, rgba.data(), 2048 * 4));

    std::vector<double> loader, upload, png;
    for (int run = 0; run < 5; run++){
        // loader thread: read, inflate, levels
        auto start = std::chrono::steady_clock::now();
        const std::vector<uint8_t> bytes = loadFile(ktx2Path);
        TextureData data;
        ASSERT_TRUE(data.loadKtx2(bytes.data(), (unsigned int)bytes.size(), 0, nullptr, false, &error)) << error;
        loader.push_back(loadMs(start));
        ASSERT_EQ(data.getGpuFormat(), GpuTextureFormat::BC7) << "the device samples no BC7: the bench needs it";

        // main thread: the upload
        const std::vector<TextureLevel>& uploadLevels = data.getLevels();
        ASSERT_EQ(uploadLevels.size(), levels.size());
        TextureRender render;
        start = std::chrono::steady_clock::now();
        ASSERT_TRUE(render.createTextureLevels("bench-ktx2", TextureType::TEXTURE_2D, GpuTextureFormat::BC7, 2048, 2048, 1, uploadLevels,
                                               TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT));
        upload.push_back(loadMs(start));
        render.destroyTexture();

        // today's path: decode the PNG, build the mips, upload
        start = std::chrono::steady_clock::now();
        int w = 0, h = 0, c = 0;
        unsigned char* pixels = stbi_load(pngPath.c_str(), &w, &h, &c, 4);
        ASSERT_TRUE(pixels);
        std::vector<MipLevel> chain(1);
        chain[0].width = w;
        chain[0].height = h;
        chain[0].pixels.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);
        MipChainOptions options;
        options.srgb = true;
        options.alphaWeighted = true;
        MipChain::build(chain, options);
        std::vector<TextureLevel> pngLevels;
        for (const MipLevel& level : chain)
            pngLevels.push_back({level.width, level.height, level.pixels.data(), level.pixels.size()});
        TextureRender pngRender;
        ASSERT_TRUE(pngRender.createTextureLevels("bench-png", TextureType::TEXTURE_2D, GpuTextureFormat::RGBA8, w, h, 1, pngLevels,
                                                  TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::REPEAT));
        png.push_back(loadMs(start));
        pngRender.destroyTexture();
    }
    const double loaderMs = loadMedian(loader), uploadMs = loadMedian(upload), pngMs = loadMedian(png);
    std::printf("[bench] 2048 BC7 KTX2 (%zu bytes): loader %.2f ms (budget 12), main-thread upload %.3f ms (budget 0.5)\n",
                file.size(), loaderMs, uploadMs);
    std::printf("[bench] 2048 PNG decode + mips + upload %.1f ms: %.1fx the KTX2's time (budget 3x)\n", pngMs, pngMs / (loaderMs + uploadMs));
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(loaderMs, 12.0 * slack);
        EXPECT_LE(uploadMs, 0.5 * slack);
        EXPECT_GE(pngMs / (loaderMs + uploadMs), 3.0 / slack);
    }
}
