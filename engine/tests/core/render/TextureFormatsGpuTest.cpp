// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 AC8 `texture_formats`: every cooked fixture (fixtures/texture_formats/<FORMAT>.ktx2, made
// by the editor's TextureFormatFixtures test with the CPU decode of each level next to it) is
// loaded as the runtime loads it: natively when the device samples the format, else through the
// R11 fallback. Each level is drawn one texel per pixel with nearest sampling, read back, and
// compared with the CPU decode: within 1/255 per channel (the shader's sRGB decode and encode
// round trip is the only change; 2/255 for a native S3TC decode, whose interpolation is the
// implementation's). Runs on the headless device (glcore, and Vulkan on lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"
#include "texture/Ktx2.h"
#include "texture/TextureData.h"
#include "texture/TextureDecode.h"
#include "texture/TexturePrepare.h"
#include "render/SystemRender.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;

namespace {

    // the decoded levels of the fixture being loaded, by size: what the editor's ASTC / ETC2
    // decoder gives (this binary has none), served to the fallback through TextureDecode
    std::map<std::pair<int, int>, std::vector<uint8_t>>& formatFixtureLevels(){
        static std::map<std::pair<int, int>, std::vector<uint8_t>> levels;
        return levels;
    }

    bool formatFixtureDecoder(GpuTextureFormat, int width, int height, const uint8_t*, size_t, std::vector<uint8_t>& out){
        auto it = formatFixtureLevels().find({width, height});
        if (it == formatFixtureLevels().end())
            return false;
        out = it->second;
        return true;
    }

    std::vector<uint8_t> formatRead(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    double formatChannel(GpuTextureFormat decoded, const uint8_t* texel, int c){
        switch (decoded){
            case GpuTextureFormat::R8: return c == 0 ? texel[0] / 255.0 : 0.0;
            case GpuTextureFormat::RG8: return c < 2 ? texel[c] / 255.0 : 0.0;
            case GpuTextureFormat::R16: {
                uint16_t v;
                std::memcpy(&v, texel, 2);
                return c == 0 ? v / 65535.0 : 0.0;
            }
            case GpuTextureFormat::RGBA16F: {
                uint16_t h;
                std::memcpy(&h, texel + c * 2, 2);
                return std::clamp((double)halfToFloat(h), 0.0, 1.0);
            }
            case GpuTextureFormat::RGB565: {
                uint16_t v;
                std::memcpy(&v, texel, 2);
                const int bits[3] = {(v >> 11) & 31, (v >> 5) & 63, v & 31};
                return c == 1 ? bits[1] / 63.0 : bits[c] / 31.0;
            }
            case GpuTextureFormat::RGBA4: {
                uint16_t v;
                std::memcpy(&v, texel, 2);
                return ((v >> (12 - 4 * c)) & 15) / 15.0;
            }
            case GpuTextureFormat::RGBA8: default: return texel[c] / 255.0;
        }
    }

    // what the unlit mesh shader writes for a texel: sRGBToLinear, then the output's linearTosRGB
    std::vector<uint8_t> formatExpected(GpuTextureFormat decoded, const uint8_t* level, int width, int height){
        const size_t stride = gpuFormatInfo(decoded).bytesPerBlock;
        std::vector<uint8_t> out((size_t)width * height * 4);
        for (size_t i = 0; i < (size_t)width * height; i++){
            for (int c = 0; c < 3; c++){
                const double linear = std::pow(formatChannel(decoded, level + i * stride, c), 2.2);
                out[i * 4 + c] = (uint8_t)std::lround(std::clamp(std::pow(linear, 1.0 / 2.2), 0.0, 1.0) * 255.0);
            }
            // the texel's alpha: a translucent one blends with the background (not compared)
            const bool hasAlpha = decoded == GpuTextureFormat::RGBA8 || decoded == GpuTextureFormat::RGBA4 || decoded == GpuTextureFormat::RGBA16F;
            out[i * 4 + 3] = hasAlpha ? (uint8_t)std::lround(std::clamp(formatChannel(decoded, level + i * stride, 3), 0.0, 1.0) * 255.0) : 255;
        }
        return out;
    }

    // the largest channel difference, the image as it is or flipped (the plane's UV orientation
    // is the renderer's business, not this test's)
    int formatDifference(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected, int width, int height){
        int best = 256;
        for (int flip = 0; flip < 4; flip++){
            int worst = 0;
            for (int y = 0; y < height && worst < best; y++){
                for (int x = 0; x < width; x++){
                    const int sx = (flip & 1) ? width - 1 - x : x;
                    const int sy = (flip & 2) ? height - 1 - y : y;
                    if (expected[((size_t)sy * width + sx) * 4 + 3] != 255)
                        continue;
                    for (int c = 0; c < 3; c++){
                        const int d = std::abs((int)actual[((size_t)y * width + x) * 4 + c] - (int)expected[((size_t)sy * width + sx) * 4 + c]);
                        worst = std::max(worst, d);
                    }
                }
            }
            best = std::min(best, worst);
        }
        return best;
    }

    class TextureFormatsGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.setPosition(0.0f, 10.0f, 0.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.setUp(0.0f, 0.0f, -1.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }
    };

}

TEST_F(TextureFormatsGpu, CookedFixturesSampleLikeTheirCpuDecode){
    JASPER_REQUIRE_GPU();
    const fs::path dir = fs::path(testPaths().assetDir) / "texture_formats";
    std::vector<fs::path> fixtures;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".ktx2")
            fixtures.push_back(entry.path());
    std::sort(fixtures.begin(), fixtures.end());
    ASSERT_GE(fixtures.size(), (size_t)kGpuTextureFormatCount) << "fixtures/texture_formats is incomplete";

    // the device's own formats, then a device that samples no compressed format (R11's fallback)
    const TextureFormatSupport device = SystemRender::getTextureFormatSupport();
    TextureFormatSupport uncompressed;
    for (GpuTextureFormat f : {GpuTextureFormat::R8, GpuTextureFormat::RG8, GpuTextureFormat::RGBA8, GpuTextureFormat::R16,
                               GpuTextureFormat::RGBA16F})
        uncompressed.set(f, SystemRender::isTextureFormatSupported(f));
    int native = 0, fallback = 0;
    for (int pass = 0; pass < 2; pass++)
    for (const fs::path& path : fixtures){
        const std::string name = path.stem().string();
        SCOPED_TRACE(name + (pass == 0 ? " (device formats)" : " (no compressed format)"));
        const std::vector<uint8_t> bytes = formatRead(path);
        const std::vector<uint8_t> decoded = formatRead(fs::path(path).replace_extension(".decoded"));
        Ktx2Info info;
        std::string error;
        ASSERT_TRUE(Ktx2::parseInfo(bytes, bytes.size(), info, error)) << error;
        const GpuTextureFormat decodedFormat = TextureDecode::decodedFormat(info.format);

        // each level's CPU decode, by size, and the fallback decoder serving them
        formatFixtureLevels().clear();
        std::vector<size_t> offsets;
        size_t at = 0;
        const size_t texelBytes = gpuFormatInfo(decodedFormat).bytesPerBlock;
        for (uint32_t level = 0; level < info.levelCount; level++){
            const int w = (int)info.levelWidth(level), h = (int)info.levelHeight(level);
            offsets.push_back(at);
            const size_t size = (size_t)w * h * texelBytes;
            ASSERT_LE(at + size, decoded.size());
            formatFixtureLevels()[{w, h}] = std::vector<uint8_t>(decoded.begin() + (long)at, decoded.begin() + (long)(at + size));
            at += size;
        }
        if (!TextureDecode::canDecode(info.format))
            TextureDecode::registerDecoder(info.format, formatFixtureDecoder);

        TextureData data;
        ASSERT_TRUE(data.loadKtx2(bytes.data(), (unsigned int)bytes.size(), 0, pass == 0 ? &device : &uncompressed, false, &error)) << error;
        const bool isNative = data.getGpuFormat() == info.format;
        (isNative ? native : fallback)++;
        std::printf("[ formats ] %-10s %s\n", name.c_str(), isNative ? "native" : (std::string("fallback ") + gpuFormatName(data.getGpuFormat())).c_str());

        Primitive plane(&scene);
        plane.createPlane(2.0f, 2.0f);
        plane.setReceiveLights(false);
        Material material = plane.getMaterial();
        material.baseColorTexture = Texture("texture-formats-" + name + "-" + std::to_string(pass), data);
        material.baseColorTexture.setMinFilter(TextureFilter::NEAREST_MIPMAP_NEAREST);
        material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        plane.setMaterial(material);
        camera.setOrtho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);

        for (uint32_t level = 0; level < info.levelCount; level++){
            const int w = (int)info.levelWidth(level), h = (int)info.levelHeight(level);
            if (std::min(w, h) < 4)
                break;
            SCOPED_TRACE("level " + std::to_string(level));
            GpuEnvironment::setView(w, h, w, h, Scaling::STRETCH);
            Scene* current = &scene;
            ASSERT_TRUE(GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }));
            Framebuffer& target = GpuEnvironment::target();
            ASSERT_EQ((int)target.getWidth(), w);
            std::vector<uint8_t> actual;
            ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), w, h, actual));
            const std::vector<uint8_t> expected = formatExpected(decodedFormat, decoded.data() + offsets[level], w, h);
            // S3TC leaves the palette's interpolation to the implementation (the D3D spec allows
            // ~3% of the endpoint range): a native BC1 / BC3 decode may round differently by 1
            const bool s3tc = isNative && (info.format == GpuTextureFormat::BC1 || info.format == GpuTextureFormat::BC3);
            EXPECT_LE(formatDifference(actual, expected, w, h), s3tc ? 2 : 1);
        }
        material.baseColorTexture.destroy();
    }
    std::printf("[ formats ] %d native, %d through the fallback\n", native, fallback);
    EXPECT_NO_ENGINE_ERRORS();
}
