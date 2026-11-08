// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the pixels go through before the GPU format (spec S14 R1, R4, R6): alpha source, channel
// selection, normal XY, float HDR, the size rules and the uncompressed conversions. Shared by the
// cook and the raw load path.

#include "texture/TextureData.h"
#include "texture/TexturePrepare.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    // a TextureData over pixels the test owns
    struct Image {
        std::vector<uint8_t> pixels;
        TextureData data;
        Image(int width, int height, ColorFormat format, int channels, std::vector<uint8_t> bytes)
            : pixels(std::move(bytes)), data(width, height, (unsigned int)pixels.size(), format, channels, pixels.data()){}
    };

    Image rgba(int width, int height, std::function<void(int, int, uint8_t*)> fill){
        std::vector<uint8_t> bytes((size_t)width * height * 4);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                fill(x, y, &bytes[((size_t)y * width + x) * 4]);
        return Image(width, height, ColorFormat::RGBA, 4, std::move(bytes));
    }

    PreparedTexture prepare(const TextureData& data, const TextureProcessing& processing){
        PreparedTexture out;
        std::string error;
        EXPECT_TRUE(TexturePrepare::prepare({&data}, processing, out, error)) << error;
        return out;
    }

}

TEST(TexturePrepare, HalfFloatsRoundTrip){
    for (float v : {0.0f, 1.0f, -2.0f, 0.5f, 65504.0f, 6.103515625e-05f, 5.9604645e-08f, 1000.0f})
        EXPECT_EQ(halfToFloat(floatToHalf(v)), v) << v;
    EXPECT_TRUE(std::isinf(halfToFloat(floatToHalf(1e6f))));
    EXPECT_EQ(floatToHalf(1.0f), 0x3C00);
    // round to nearest even: 1 + 2^-11 is halfway between two halves and goes down
    EXPECT_EQ(floatToHalf(1.0f + 1.0f / 2048.0f), 0x3C00);
    EXPECT_EQ(floatToHalf(1.0f + 3.0f / 2048.0f), 0x3C02);
}

TEST(TexturePrepare, Rgb9e5KeepsThreePercent){
    std::mt19937 random(3);
    std::uniform_real_distribution<float> value(0.0f, 16.0f);
    for (int i = 0; i < 2000; i++){
        const float r = value(random), g = value(random) * 0.1f, b = value(random) * 0.5f;
        float dr, dg, db;
        unpackRGB9E5(packRGB9E5(r, g, b), dr, dg, db);
        const float largest = std::max(r, std::max(g, b));
        EXPECT_NEAR(dr, r, largest / 256.0f);
        EXPECT_NEAR(dg, g, largest / 256.0f);
        EXPECT_NEAR(db, b, largest / 256.0f);
    }
    float r, g, b;
    unpackRGB9E5(packRGB9E5(-1.0f, 0.0f, NAN), r, g, b);
    EXPECT_EQ(r, 0.0f);
    EXPECT_EQ(b, 0.0f);
}

TEST(TexturePrepare, SizeRules){
    TextureProcessing p;
    int w = 0, h = 0;
    p.maxSize = 2048;
    TexturePrepare::targetSize(4096, 1024, p, w, h);
    EXPECT_EQ(w, 2048); EXPECT_EQ(h, 512);
    TexturePrepare::targetSize(1000, 600, p, w, h);
    EXPECT_EQ(w, 1000); EXPECT_EQ(h, 600);
    p.sizeMultiple = 4;
    TexturePrepare::targetSize(1001, 603, p, w, h);
    EXPECT_EQ(w, 1000); EXPECT_EQ(h, 604);
    TexturePrepare::targetSize(2, 1, p, w, h);
    EXPECT_EQ(w, 4); EXPECT_EQ(h, 4);
    p.sizeMultiple = 1;
    p.strategy = TextureSizeStrategy::Resize;
    TexturePrepare::targetSize(1000, 600, p, w, h);
    EXPECT_EQ(w, 1024); EXPECT_EQ(h, 1024);
    p.maxSize = 512;
    TexturePrepare::targetSize(1000, 300, p, w, h);
    EXPECT_EQ(w, 512); EXPECT_EQ(h, 256);
    p = TextureProcessing();
    p.cube = true;
    TexturePrepare::targetSize(256, 200, p, w, h);
    EXPECT_EQ(w, 256); EXPECT_EQ(h, 256);
}

TEST(TexturePrepare, AlphaSources){
    Image image = rgba(4, 4, [](int x, int, uint8_t* p){ p[0] = p[1] = p[2] = (uint8_t)(x * 80); p[3] = 255; });
    TextureProcessing p;
    p.mipmaps = false;
    PreparedTexture opaque = prepare(image.data, p);
    EXPECT_FALSE(opaque.hasAlpha);
    p.alphaSource = TextureAlphaSource::FromGray;
    PreparedTexture gray = prepare(image.data, p);
    EXPECT_TRUE(gray.hasAlpha);
    EXPECT_EQ(gray.faces[0][0].pixels[3], 0);
    EXPECT_EQ(gray.faces[0][0].pixels[3 * 4 + 3], 240);
    Image translucent = rgba(2, 2, [](int, int, uint8_t* p){ p[0] = 10; p[3] = 7; });
    p.alphaSource = TextureAlphaSource::None;
    PreparedTexture none = prepare(translucent.data, p);
    EXPECT_FALSE(none.hasAlpha);
    EXPECT_EQ(none.faces[0][0].pixels[3], 255);
}

TEST(TexturePrepare, SingleChannelTakesTheChosenChannel){
    Image image = rgba(4, 4, [](int x, int y, uint8_t* p){ p[0] = 1; p[1] = (uint8_t)(x * 10 + y); p[2] = 3; p[3] = 200; });
    TextureProcessing p;
    p.type = TextureImportType::SingleChannel;
    p.srgb = false;
    p.channel = TextureChannel::G;
    p.mipmaps = false;
    PreparedTexture out = prepare(image.data, p);
    EXPECT_EQ(out.layout, PreparedTexture::Layout::R8);
    EXPECT_FALSE(out.srgb);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(out.faces[0][0].pixels[i], (uint8_t)((i % 4) * 10 + i / 4));
    p.channel = TextureChannel::A;
    EXPECT_EQ(prepare(image.data, p).faces[0][0].pixels[5], 200);
}

TEST(TexturePrepare, NormalMapsBecomeUnitXY){
    // tilted normals stored as RGB, slightly off unit length (8-bit noise)
    Image image = rgba(8, 8, [](int x, int, uint8_t* p){ p[0] = (uint8_t)(128 + x * 10); p[1] = 100; p[2] = 230; p[3] = 255; });
    TextureProcessing p;
    p.type = TextureImportType::Normal;
    p.srgb = false;
    PreparedTexture out = prepare(image.data, p);
    EXPECT_EQ(out.layout, PreparedTexture::Layout::RG8);
    for (const MipLevel& level : out.faces[0]){
        for (size_t i = 0; i < level.pixels.size(); i += 2){
            const float x = level.pixels[i] / 255.0f * 2.0f - 1.0f, y = level.pixels[i + 1] / 255.0f * 2.0f - 1.0f;
            EXPECT_LE(x * x + y * y, 1.0f + 0.02f);
        }
    }
    // flip green: Y inverts (DirectX-style maps)
    p.flipGreen = true;
    PreparedTexture flipped = prepare(image.data, p);
    const float y = out.faces[0][0].pixels[1] / 255.0f * 2.0f - 1.0f;
    const float yFlipped = flipped.faces[0][0].pixels[1] / 255.0f * 2.0f - 1.0f;
    EXPECT_NEAR(yFlipped, -y, 0.02f);
}

TEST(TexturePrepare, HdrStaysFloatAndDisplayEncoded){
    std::vector<uint8_t> bytes(4 * 4 * 4 * sizeof(float));
    float* v = reinterpret_cast<float*>(bytes.data());
    for (int i = 0; i < 16; i++){
        v[i * 4 + 0] = 4.0f;
        v[i * 4 + 1] = 1.0f;
        v[i * 4 + 2] = 0.25f;
        v[i * 4 + 3] = 1.0f;
    }
    Image image(4, 4, ColorFormat::RGBA32F, 4, std::move(bytes));
    TextureProcessing p;
    p.type = TextureImportType::HDR;
    p.srgb = false;
    PreparedTexture out = prepare(image.data, p);
    EXPECT_EQ(out.layout, PreparedTexture::Layout::RGBAF);
    const float* stored = reinterpret_cast<const float*>(out.faces[0][0].pixels.data());
    EXPECT_NEAR(stored[0], std::pow(4.0f, 1.0f / 2.2f), 1e-5f);
    EXPECT_NEAR(stored[1], 1.0f, 1e-6f);
    // the last level of a constant image is the same colour
    const float* last = reinterpret_cast<const float*>(out.faces[0].back().pixels.data());
    EXPECT_NEAR(last[2], std::pow(0.25f, 1.0f / 2.2f), 1e-4f);
    std::vector<std::vector<uint8_t>> half;
    std::string error;
    ASSERT_TRUE(TexturePrepare::toUncompressed(out, GpuTextureFormat::RGBA16F, half, error)) << error;
    uint16_t h;
    std::memcpy(&h, half[0].data(), 2);
    EXPECT_NEAR(halfToFloat(h), std::pow(4.0f, 1.0f / 2.2f), 2e-3f);
}

TEST(TexturePrepare, UncompressedConversions){
    Image image = rgba(2, 2, [](int x, int y, uint8_t* p){ p[0] = x ? 255 : 0; p[1] = y ? 255 : 0; p[2] = 128; p[3] = 64; });
    TextureProcessing p;
    p.mipmaps = false;
    p.srgb = false;
    PreparedTexture out = prepare(image.data, p);
    std::vector<std::vector<uint8_t>> levels;
    std::string error;
    ASSERT_TRUE(TexturePrepare::toUncompressed(out, GpuTextureFormat::RGB565, levels, error));
    uint16_t texel;
    std::memcpy(&texel, levels[0].data() + 2, 2);     // x = 1, y = 0: red
    EXPECT_EQ(texel >> 11, 31);
    EXPECT_EQ((texel >> 5) & 63, 0);
    EXPECT_EQ(texel & 31, 16);
    ASSERT_TRUE(TexturePrepare::toUncompressed(out, GpuTextureFormat::RGBA4, levels, error));
    std::memcpy(&texel, levels[0].data() + 6, 2);     // x = 1, y = 1
    EXPECT_EQ(texel >> 12, 15);
    EXPECT_EQ((texel >> 8) & 15, 15);
    EXPECT_EQ(texel & 15, 4);
    EXPECT_FALSE(TexturePrepare::toUncompressed(out, GpuTextureFormat::R16, levels, error));
}

TEST(TexturePrepare, DefaultsKeepTheDecodedLayout){
    std::vector<uint8_t> heights(4 * 4 * 2);
    for (size_t i = 0; i < heights.size() / 2; i++){
        const uint16_t v = (uint16_t)(i * 4000);
        std::memcpy(&heights[i * 2], &v, 2);
    }
    Image height(4, 4, ColorFormat::RED16, 1, heights);
    const TextureProcessing p = TextureProcessing::defaultFor(height.data, false, false);
    EXPECT_TRUE(p.keepSourceChannels);
    PreparedTexture out = prepare(height.data, p);
    EXPECT_EQ(out.layout, PreparedTexture::Layout::R16);
    EXPECT_EQ(out.faces[0][0].pixels, heights);
    EXPECT_EQ(TexturePrepare::uncompressedFormat(out.layout), GpuTextureFormat::R16);

    Image colour = rgba(4, 4, [](int, int, uint8_t* p){ p[0] = 9; p[3] = 255; });
    const TextureProcessing c = TextureProcessing::defaultFor(colour.data, true, false);
    EXPECT_TRUE(c.srgb);
    EXPECT_TRUE(c.alphaIsTransparency);
    EXPECT_EQ(prepare(colour.data, c).layout, PreparedTexture::Layout::RGBA8);
}

TEST(TexturePrepare, CubeFacesShareTheSizeRules){
    // reserved: an Image's TextureData points into its own pixels
    std::vector<Image> faces;
    faces.reserve(6);
    for (int f = 0; f < 6; f++)
        faces.push_back(rgba(8, 8, [f](int, int, uint8_t* p){ p[0] = (uint8_t)(f * 40); p[3] = 255; }));
    std::vector<const TextureData*> pointers;
    for (const Image& face : faces)
        pointers.push_back(&face.data);
    TextureProcessing p;
    p.cube = true;
    p.maxSize = 4;
    PreparedTexture out;
    std::string error;
    ASSERT_TRUE(TexturePrepare::prepare(pointers, p, out, error)) << error;
    ASSERT_EQ(out.faces.size(), 6u);
    EXPECT_EQ(out.width, 4);
    std::vector<std::vector<uint8_t>> levels;
    ASSERT_TRUE(TexturePrepare::toUncompressed(out, GpuTextureFormat::RGBA8, levels, error));
    EXPECT_EQ(levels[0].size(), (size_t)4 * 4 * 4 * 6);
    // face 3 starts after three faces of level 0
    EXPECT_NEAR(levels[0][(size_t)4 * 4 * 4 * 3], 120, 1);
}

// S42 AC2 (R6): Alpha Clip after the alpha source and before any resize: cutoff 0.5 maps 127 to 0
// and 128 to 255, and a cleared texel's colour is bled from its visible neighbours (Alpha Is
// Transparency). The raw load and the cook share TexturePrepare, so they agree.
TEST(TexturePrepare, AlphaClip){
    // 127 (just under the cutoff), 128 (at it), 255 and 0
    Image image = rgba(4, 1, [](int x, int, uint8_t* p){
        static const uint8_t alpha[4] = {127, 128, 255, 0};
        p[0] = (uint8_t)(40 + x * 50); p[1] = 20; p[2] = 200; p[3] = alpha[x];
    });
    TextureProcessing p;
    p.type = TextureImportType::Sprite;
    p.mipmaps = false;
    p.alphaClip = true;
    p.alphaCutoff = 0.5f;
    PreparedTexture clipped = prepare(image.data, p);
    const std::vector<uint8_t>& out = clipped.faces[0][0].pixels;
    EXPECT_EQ(out[0 * 4 + 3], 0);
    EXPECT_EQ(out[1 * 4 + 3], 255);
    EXPECT_EQ(out[2 * 4 + 3], 255);
    EXPECT_EQ(out[3 * 4 + 3], 0);

    // a cleared texel takes its visible neighbour's colour when alpha borders are bled
    p.alphaIsTransparency = true;
    PreparedTexture bled = prepare(image.data, p);
    const std::vector<uint8_t>& b = bled.faces[0][0].pixels;
    EXPECT_EQ(b[0 * 4 + 0], b[1 * 4 + 0]) << "texel 0 (cleared) bled from texel 1";
    EXPECT_EQ(b[0 * 4 + 2], b[1 * 4 + 2]);

    // red control: without Alpha Clip the partial alpha stays
    p.alphaClip = false;
    p.alphaIsTransparency = false;
    PreparedTexture kept = prepare(image.data, p);
    EXPECT_EQ(kept.faces[0][0].pixels[0 * 4 + 3], 127);
    EXPECT_EQ(kept.faces[0][0].pixels[1 * 4 + 3], 128);

    // the clip happens before the resize: a Nearest downscale of clipped texels stays binary
    Image big = rgba(8, 8, [](int x, int y, uint8_t* q){ q[0] = q[1] = q[2] = 100; q[3] = (uint8_t)((x + y) * 16); });
    TextureProcessing resize;
    resize.type = TextureImportType::Sprite;
    resize.mipmaps = false;
    resize.alphaClip = true;
    resize.maxSize = 4;
    resize.resizeFilter = MipFilter::Nearest;
    PreparedTexture small = prepare(big.data, resize);
    ASSERT_EQ(small.width, 4);
    for (size_t i = 3; i < small.faces[0][0].pixels.size(); i += 4)
        EXPECT_TRUE(small.faces[0][0].pixels[i] == 0 || small.faces[0][0].pixels[i] == 255);
}
