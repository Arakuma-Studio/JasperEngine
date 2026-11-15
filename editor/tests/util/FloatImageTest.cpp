// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// util/FloatImage (the Vertex Animation importers' texture reader, spec 4.6): EXR written by
// writeExr (half, float and uint; RGB, RGBA, grey, grey+alpha; scanline and tiled; every
// compression) read back bit-exactly; EXR channel names (layers, lowercase, a lone channel);
// 8- and 16-bit PNG with 1-4 channels normalised exactly (v/255, v/65535) and their gamma chunks
// ignored; TGA whatever its origin bit or RLE; the raw 8-bit values of lookup tables; and the
// errors (missing file, truncated EXR/PNG, unsupported or mislabelled format).

#include <gtest/gtest.h>

#include <algorithm>

#include "util/FloatImage.h"
#include "util/PngWriter.h"

#include "stb_image_write.h"
#include "tinyexr.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include "support/PosixCompat.h"
#include <vector>

using namespace jasperengine;
using editor::ExrWriteOptions;
using editor::FloatImage;
namespace fs = std::filesystem;

namespace {

    class FloatImageTest : public ::testing::Test {
    protected:
        fs::path dir;

        void SetUp() override {
            dir = fs::temp_directory_path() / ("jasper-float-image-test-" + std::to_string(::getpid()));
            fs::remove_all(dir);
            fs::create_directories(dir);
        }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }

        std::string file(const std::string& name) const { return (dir / name).string(); }
    };

    std::vector<uint8_t> readBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    bool sameBits(float a, float b) {
        uint32_t x, y;
        std::memcpy(&x, &a, 4);
        std::memcpy(&y, &b, 4);
        return x == y;
    }

    // every texel different; half: values a half holds exactly (multiples of 1/64 up to 1024,
    // negative ones, the half extremes); float: any float, including tiny and huge ones
    FloatImage makeImage(int width, int height, int channels, bool halfExact) {
        FloatImage image;
        image.width = width;
        image.height = height;
        image.sourceChannels = channels;
        image.rgba.resize(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < image.rgba.size(); i++) {
            const int k = static_cast<int>(i * 7919 % 131071);
            float v;
            if (halfExact) {
                v = static_cast<float>((k % 2048) - 1024) / 64.0f;
            } else {
                v = static_cast<float>(k - 65535) * 0.000123456789f + static_cast<float>(i) * 1e-7f;
            }
            image.rgba[i] = v;
        }
        if (halfExact) {
            image.rgba[0] = 65504.0f;               // largest half
            image.rgba[1] = -65504.0f;
            image.rgba[2] = std::ldexp(1.0f, -24);  // smallest subnormal half
            image.rgba[4] = -0.0f;
        } else {
            image.rgba[0] = 3.0e38f;
            image.rgba[1] = -1.0e-38f;
            image.rgba[2] = 1.401298464e-45f;       // smallest subnormal float
            image.rgba[4] = std::numeric_limits<float>::infinity();
        }
        return image;
    }

    // what reading `written` back must give: the channels it stores, the rest as the contract says
    std::vector<float> expectedRgba(const FloatImage& written) {
        std::vector<float> rgba = written.rgba;
        for (size_t p = 0; p < rgba.size() / 4; p++) {
            float* t = &rgba[p * 4];
            switch (written.sourceChannels) {
                case 1: t[1] = t[2] = t[0]; t[3] = 1.0f; break;
                case 2: t[1] = t[2] = t[0]; break;
                case 3: t[3] = 1.0f; break;
                default: break;
            }
        }
        return rgba;
    }

    ::testing::AssertionResult sameRgba(const std::vector<float>& expected, const std::vector<float>& actual) {
        if (expected.size() != actual.size())
            return ::testing::AssertionFailure() << "size " << actual.size() << " instead of " << expected.size();
        for (size_t i = 0; i < expected.size(); i++) {
            if (!sameBits(expected[i], actual[i]))
                return ::testing::AssertionFailure() << "value " << i << " (texel " << i / 4 << " channel " << i % 4 << ") is "
                                                     << actual[i] << " instead of " << expected[i];
        }
        return ::testing::AssertionSuccess();
    }

    // An EXR with the given channel names (float planes) written straight through tinyexr
    bool writeNamedExr(const std::string& path, int width, int height, const std::vector<std::string>& names,
                       const std::vector<std::vector<float>>& planes) {
        EXRHeader header;
        InitEXRHeader(&header);
        EXRImage image;
        InitEXRImage(&image);
        std::vector<EXRChannelInfo> infos(names.size());
        std::vector<int> types(names.size(), TINYEXR_PIXELTYPE_FLOAT);
        std::vector<unsigned char*> pointers(names.size());
        for (size_t c = 0; c < names.size(); c++) {
            std::memset(&infos[c], 0, sizeof(EXRChannelInfo));
            std::snprintf(infos[c].name, sizeof(infos[c].name), "%s", names[c].c_str());
            infos[c].pixel_type = TINYEXR_PIXELTYPE_FLOAT;
            infos[c].x_sampling = infos[c].y_sampling = 1;
            pointers[c] = reinterpret_cast<unsigned char*>(const_cast<float*>(planes[c].data()));
        }
        header.num_channels = static_cast<int>(names.size());
        header.channels = infos.data();
        header.pixel_types = types.data();
        header.requested_pixel_types = types.data();
        header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
        image.num_channels = static_cast<int>(names.size());
        image.images = pointers.data();
        image.width = width;
        image.height = height;
        unsigned char* memory = nullptr;
        const char* err = nullptr;
        const size_t size = SaveEXRImageToMemory(&image, &header, &memory, &err);
        if (err)
            FreeEXRErrorMessage(err);
        if (size == 0 || !memory)
            return false;
        writeBytes(path, std::vector<uint8_t>(memory, memory + size));
        std::free(memory);
        return true;
    }

    uint32_t crc32(const uint8_t* data, size_t size) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; i++) {
            crc ^= data[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xFFFFFFFFu;
    }

    // a PNG chunk inserted right after IHDR (which is 8 + 25 bytes into the file)
    std::vector<uint8_t> withChunk(const std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> chunk;
        const uint32_t length = static_cast<uint32_t>(payload.size());
        for (int s = 24; s >= 0; s -= 8)
            chunk.push_back(static_cast<uint8_t>(length >> s));
        for (int i = 0; i < 4; i++)
            chunk.push_back(static_cast<uint8_t>(type[i]));
        chunk.insert(chunk.end(), payload.begin(), payload.end());
        const uint32_t crc = crc32(chunk.data() + 4, chunk.size() - 4);
        for (int s = 24; s >= 0; s -= 8)
            chunk.push_back(static_cast<uint8_t>(crc >> s));
        // (copied into place: GCC 11's -Wstringop-overread misreads vector::insert's reallocation)
        std::vector<uint8_t> out(png.size() + chunk.size());
        std::copy(png.begin(), png.begin() + 33, out.begin());
        std::copy(chunk.begin(), chunk.end(), out.begin() + 33);
        std::copy(png.begin() + 33, png.end(), out.begin() + 33 + static_cast<std::ptrdiff_t>(chunk.size()));
        return out;
    }

    // 8-bit samples, every value 0..255 present
    std::vector<uint8_t> pattern8(int width, int height, int channels) {
        std::vector<uint8_t> data(static_cast<size_t>(width) * height * channels);
        for (size_t i = 0; i < data.size(); i++)
            data[i] = static_cast<uint8_t>((i * 89 + 3) & 0xFF);
        return data;
    }

    // RGBA8 the reader makes of 8-bit samples of `channels` channels
    std::vector<uint8_t> expand8(const std::vector<uint8_t>& samples, int channels) {
        const size_t count = samples.size() / channels;
        std::vector<uint8_t> rgba(count * 4);
        for (size_t p = 0; p < count; p++) {
            const uint8_t* s = &samples[p * channels];
            uint8_t* t = &rgba[p * 4];
            if (channels <= 2) {
                t[0] = t[1] = t[2] = s[0];
                t[3] = channels == 2 ? s[1] : 255;
            } else {
                t[0] = s[0];
                t[1] = s[1];
                t[2] = s[2];
                t[3] = channels == 4 ? s[3] : 255;
            }
        }
        return rgba;
    }

    void expectRaw8(const FloatImage& image, const std::vector<uint8_t>& rgba8) {
        ASSERT_EQ(image.raw8, rgba8);
        ASSERT_EQ(image.rgba.size(), rgba8.size());
        for (size_t i = 0; i < rgba8.size(); i++)
            ASSERT_TRUE(sameBits(image.rgba[i], static_cast<float>(rgba8[i]) / 255.0f)) << "value " << i;
    }

    // an uncompressed 8-bit TGA (type 2 true colour, 3 grey), rows stored top first when
    // topOrigin, else bottom first (the TGA default); `rows` is given top first, BGR(A) order
    std::vector<uint8_t> makeTga(int width, int height, int channels, const std::vector<uint8_t>& rowsTopFirst, bool topOrigin) {
        std::vector<uint8_t> tga(18, 0);
        tga[2] = channels == 1 ? 3 : 2;
        tga[12] = static_cast<uint8_t>(width & 0xFF);
        tga[13] = static_cast<uint8_t>(width >> 8);
        tga[14] = static_cast<uint8_t>(height & 0xFF);
        tga[15] = static_cast<uint8_t>(height >> 8);
        tga[16] = static_cast<uint8_t>(channels * 8);
        tga[17] = static_cast<uint8_t>((channels == 4 ? 8 : 0) | (topOrigin ? 0x20 : 0));
        const size_t row = static_cast<size_t>(width) * channels;
        for (int r = 0; r < height; r++) {
            const int y = topOrigin ? r : height - 1 - r;
            tga.insert(tga.end(), rowsTopFirst.begin() + static_cast<long>(y * row), rowsTopFirst.begin() + static_cast<long>((y + 1) * row));
        }
        return tga;
    }

}

// ---------------------------------------------------------------------------------------- EXR

TEST_F(FloatImageTest, ExrHalfAndFloatRoundTripExactly) {
    for (bool half : {true, false}) {
        for (int channels : {3, 4}) {
            SCOPED_TRACE(std::string(half ? "half " : "float ") + std::to_string(channels) + " channels");
            const FloatImage written = makeImage(37, 23, channels, half);
            const std::string path = file("image.exr");
            std::string error;
            ASSERT_TRUE(editor::writeExr(path, written, half, &error)) << error;

            FloatImage read;
            ASSERT_TRUE(editor::readFloatImage(path, read, &error)) << error;
            EXPECT_EQ(read.width, 37);
            EXPECT_EQ(read.height, 23);
            EXPECT_EQ(read.sourceChannels, channels);
            EXPECT_EQ(read.sourceBits, half ? 16 : 32);
            EXPECT_TRUE(read.sourceFloat);
            EXPECT_TRUE(read.raw8.empty());
            EXPECT_TRUE(sameRgba(expectedRgba(written), read.rgba));
        }
    }
}

TEST_F(FloatImageTest, ExrHalfRoundsFloatsToNearestHalf) {
    FloatImage written;
    written.width = 2;
    written.height = 1;
    written.sourceChannels = 4;
    // 1 + 2^-11 is halfway between 1 and the next half (1 + 2^-10): ties to even, 1
    written.rgba = {1.0f + std::ldexp(1.0f, -11), 0.1f, 1.0f / 3.0f, 2.0f, 1.0e6f, -7.25f, 0.5f, 1.0f};
    std::string error;
    ASSERT_TRUE(editor::writeExr(file("round.exr"), written, true, &error)) << error;
    FloatImage read;
    ASSERT_TRUE(editor::readFloatImage(file("round.exr"), read, &error)) << error;
    ASSERT_EQ(read.rgba.size(), 8u);
    EXPECT_EQ(read.rgba[0], 1.0f);
    EXPECT_EQ(read.rgba[1], 0.0999755859375f);    // the nearest half of 0.1
    EXPECT_EQ(read.rgba[2], 0.333251953125f);     // of 1/3
    EXPECT_EQ(read.rgba[3], 2.0f);
    EXPECT_TRUE(std::isinf(read.rgba[4]));          // over 65504
    EXPECT_EQ(read.rgba[5], -7.25f);
}

TEST_F(FloatImageTest, ExrEveryCompressionScanlineAndTiled) {
    using C = ExrWriteOptions::Compression;
    const struct { C compression; const char* name; bool lossyHalf; bool lossyFloat; } cases[] = {
        {C::None, "NONE", false, false}, {C::Rle, "RLE", false, false}, {C::Zips, "ZIPS", false, false},
        {C::Zip, "ZIP", false, false},   {C::Piz, "PIZ", false, false}, {C::Pxr24, "PXR24", false, true},
        {C::B44, "B44", true, false},    {C::B44a, "B44A", true, false},
    };
    for (const auto& c : cases) {
        for (bool half : {true, false}) {
            for (bool tiled : {false, true}) {
                SCOPED_TRACE(std::string(c.name) + (half ? " half" : " float") + (tiled ? " tiled" : " scanline"));
                // 70 x 45: partial tiles on the right and bottom, several scanline blocks
                const FloatImage written = makeImage(70, 45, 4, half);
                ExrWriteOptions options;
                options.compression = c.compression;
                options.pixelType = half ? ExrWriteOptions::PixelType::Half : ExrWriteOptions::PixelType::Float;
                options.tiled = tiled;
                options.tileSize = 16;
                const std::string path = file("compressed.exr");
                std::string error;
                ASSERT_TRUE(editor::writeExr(path, written, options, &error)) << error;
                FloatImage read;
                ASSERT_TRUE(editor::readFloatImage(path, read, &error)) << error;
                ASSERT_EQ(read.width, 70);
                ASSERT_EQ(read.height, 45);
                const bool lossy = half ? c.lossyHalf : c.lossyFloat;
                if (!lossy) {
                    EXPECT_TRUE(sameRgba(written.rgba, read.rgba));
                } else {
                    // lossy codecs: only that the data decodes in place (PXR24 keeps 24 bits of a
                    // float; B44 packs 4x4 half blocks) on a smooth image
                    FloatImage smooth = written;
                    for (int y = 0; y < smooth.height; y++)
                        for (int x = 0; x < smooth.width; x++)
                            for (int k = 0; k < 4; k++)
                                smooth.rgba[(static_cast<size_t>(y) * smooth.width + x) * 4 + k] = static_cast<float>(x + 2 * y + k) / 8.0f;
                    ASSERT_TRUE(editor::writeExr(path, smooth, options, &error)) << error;
                    ASSERT_TRUE(editor::readFloatImage(path, read, &error)) << error;
                    // (B44's error grows with the range of the 4x4 block, here about 1.5: within a
                    // couple of 1/8 steps)
                    for (size_t i = 0; i < smooth.rgba.size(); i++)
                        ASSERT_NEAR(read.rgba[i], smooth.rgba[i], 0.25f + 0.02f * std::fabs(smooth.rgba[i])) << "value " << i;
                }
            }
        }
    }
}

TEST_F(FloatImageTest, ExrGreyGreyAlphaAndUint) {
    for (int channels : {1, 2}) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        const FloatImage written = makeImage(9, 5, channels, false);
        std::string error;
        ASSERT_TRUE(editor::writeExr(file("grey.exr"), written, false, &error)) << error;
        FloatImage read;
        ASSERT_TRUE(editor::readFloatImage(file("grey.exr"), read, &error)) << error;
        EXPECT_EQ(read.sourceChannels, channels);
        EXPECT_TRUE(sameRgba(expectedRgba(written), read.rgba));
    }

    // UINT: integers (exact in float up to 2^24), negatives clamp to 0
    FloatImage written;
    written.width = 4;
    written.height = 2;
    written.sourceChannels = 4;
    for (int i = 0; i < 32; i++)
        written.rgba.push_back(static_cast<float>(i * 524287 % 16777216));
    written.rgba[5] = 16777216.0f;
    ExrWriteOptions options;
    options.pixelType = ExrWriteOptions::PixelType::Uint;
    std::string error;
    ASSERT_TRUE(editor::writeExr(file("uint.exr"), written, options, &error)) << error;
    FloatImage read;
    ASSERT_TRUE(editor::readFloatImage(file("uint.exr"), read, &error)) << error;
    EXPECT_FALSE(read.sourceFloat);
    EXPECT_EQ(read.sourceBits, 32);
    EXPECT_TRUE(sameRgba(written.rgba, read.rgba));
    written.rgba[0] = -3.0f;
    ASSERT_TRUE(editor::writeExr(file("uint.exr"), written, options, &error)) << error;
    ASSERT_TRUE(editor::readFloatImage(file("uint.exr"), read, &error)) << error;
    EXPECT_EQ(read.rgba[0], 0.0f);
}

TEST_F(FloatImageTest, ExrChannelNames) {
    const int w = 3, h = 2;
    const std::vector<float> r = {1, 2, 3, 4, 5, 6}, g = {10, 20, 30, 40, 50, 60}, b = {-1, -2, -3, -4, -5, -6}, a = {0.5f, 0.25f, 0, 1, 2, 3};
    std::string error;
    FloatImage read;

    // a layer prefix and lowercase names; alphabetical in the file (A, B, G, R)
    ASSERT_TRUE(writeNamedExr(file("layer.exr"), w, h, {"diffuse.a", "diffuse.b", "diffuse.g", "diffuse.r"}, {a, b, g, r}));
    ASSERT_TRUE(editor::readFloatImage(file("layer.exr"), read, &error)) << error;
    EXPECT_EQ(read.sourceChannels, 4);
    for (int p = 0; p < w * h; p++) {
        EXPECT_EQ(read.rgba[p * 4 + 0], r[p]);
        EXPECT_EQ(read.rgba[p * 4 + 1], g[p]);
        EXPECT_EQ(read.rgba[p * 4 + 2], b[p]);
        EXPECT_EQ(read.rgba[p * 4 + 3], a[p]);
    }

    // only R and G: B is 0, alpha 1, two channels used
    ASSERT_TRUE(writeNamedExr(file("rg.exr"), w, h, {"G", "R"}, {g, r}));
    ASSERT_TRUE(editor::readFloatImage(file("rg.exr"), read, &error)) << error;
    EXPECT_EQ(read.sourceChannels, 2);
    EXPECT_EQ(read.rgba[4 + 0], 2.0f);
    EXPECT_EQ(read.rgba[4 + 1], 20.0f);
    EXPECT_EQ(read.rgba[4 + 2], 0.0f);
    EXPECT_EQ(read.rgba[4 + 3], 1.0f);

    // one channel of any name is grey
    ASSERT_TRUE(writeNamedExr(file("depth.exr"), w, h, {"Z"}, {r}));
    ASSERT_TRUE(editor::readFloatImage(file("depth.exr"), read, &error)) << error;
    EXPECT_EQ(read.sourceChannels, 1);
    EXPECT_EQ(read.rgba[8 + 0], 3.0f);
    EXPECT_EQ(read.rgba[8 + 1], 3.0f);
    EXPECT_EQ(read.rgba[8 + 2], 3.0f);
    EXPECT_EQ(read.rgba[8 + 3], 1.0f);

    // several channels, none of them a colour: refused, naming them
    ASSERT_TRUE(writeNamedExr(file("other.exr"), w, h, {"U", "V"}, {r, g}));
    EXPECT_FALSE(editor::readFloatImage(file("other.exr"), read, &error));
    EXPECT_NE(error.find("no R, G, B or Y channel"), std::string::npos) << error;
    EXPECT_NE(error.find("U, V"), std::string::npos) << error;
    EXPECT_TRUE(read.rgba.empty());
}

// --------------------------------------------------------------------------------------- PNG

TEST_F(FloatImageTest, Png8BitEveryChannelCountExact) {
    for (int channels = 1; channels <= 4; channels++) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        const int w = 17, h = 16;
        const std::vector<uint8_t> samples = pattern8(w, h, channels);
        ASSERT_TRUE(stbi_write_png(file("image.png").c_str(), w, h, channels, samples.data(), w * channels));
        FloatImage read;
        std::string error;
        ASSERT_TRUE(editor::readFloatImage(file("image.png"), read, &error)) << error;
        EXPECT_EQ(read.width, w);
        EXPECT_EQ(read.height, h);
        EXPECT_EQ(read.sourceChannels, channels);
        EXPECT_EQ(read.sourceBits, 8);
        EXPECT_FALSE(read.sourceFloat);
        expectRaw8(read, expand8(samples, channels));
    }
}

TEST_F(FloatImageTest, Png16BitEveryChannelCountExact) {
    for (int channels = 1; channels <= 4; channels++) {
        SCOPED_TRACE(std::to_string(channels) + " channels");
        const int w = 11, h = 7;
        std::vector<uint16_t> samples(static_cast<size_t>(w) * h * channels);
        for (size_t i = 0; i < samples.size(); i++)
            samples[i] = static_cast<uint16_t>((i * 40503 + 17) & 0xFFFF);
        samples[0] = 0;
        samples[1] = 65535;
        samples[2] = 257;
        std::vector<unsigned char> png;
        ASSERT_TRUE(editor::encodePng16(w, h, channels, reinterpret_cast<const unsigned char*>(samples.data()),
                                        samples.size() * 2, png));
        writeBytes(file("image16.png"), std::vector<uint8_t>(png.begin(), png.end()));

        FloatImage read;
        std::string error;
        ASSERT_TRUE(editor::readFloatImage(file("image16.png"), read, &error)) << error;
        EXPECT_EQ(read.sourceChannels, channels);
        EXPECT_EQ(read.sourceBits, 16);
        EXPECT_FALSE(read.sourceFloat);
        EXPECT_TRUE(read.raw8.empty());
        ASSERT_EQ(read.rgba.size(), static_cast<size_t>(w) * h * 4);
        for (size_t p = 0; p < static_cast<size_t>(w) * h; p++) {
            const uint16_t* s = &samples[p * channels];
            uint32_t v[4];
            if (channels <= 2) {
                v[0] = v[1] = v[2] = s[0];
                v[3] = channels == 2 ? s[1] : 65535;
            } else {
                v[0] = s[0]; v[1] = s[1]; v[2] = s[2];
                v[3] = channels == 4 ? s[3] : 65535;
            }
            for (int c = 0; c < 4; c++)
                ASSERT_TRUE(sameBits(read.rgba[p * 4 + c], static_cast<float>(v[c]) / 65535.0f)) << "texel " << p << " channel " << c;
        }
    }
}

TEST_F(FloatImageTest, PngGammaAndSrgbChunksIgnored) {
    const int w = 8, h = 4;
    const std::vector<uint8_t> samples = pattern8(w, h, 3);
    ASSERT_TRUE(stbi_write_png(file("plain.png").c_str(), w, h, 3, samples.data(), w * 3));
    std::vector<uint8_t> png = readBytes(file("plain.png"));
    // gAMA 1/2.2 (45455) and sRGB perceptual: a colour-managed reader would change the values
    png = withChunk(png, "gAMA", {0x00, 0x00, 0xB1, 0x8F});
    png = withChunk(png, "sRGB", {0x00});
    writeBytes(file("gamma.png"), png);
    FloatImage read;
    std::string error;
    ASSERT_TRUE(editor::readFloatImage(file("gamma.png"), read, &error)) << error;
    expectRaw8(read, expand8(samples, 3));
}

TEST_F(FloatImageTest, RowsTopToBottom) {
    // a 1 x 3 image with distinct rows in every format: row 0 is the top
    const std::vector<uint8_t> rows = {10, 20, 30};
    ASSERT_TRUE(stbi_write_png(file("rows.png").c_str(), 1, 3, 1, rows.data(), 1));
    FloatImage read;
    std::string error;
    ASSERT_TRUE(editor::readFloatImage(file("rows.png"), read, &error)) << error;
    EXPECT_EQ(read.raw8[0], 10);
    EXPECT_EQ(read.raw8[8], 30);

    FloatImage exr;
    exr.width = 1;
    exr.height = 3;
    exr.sourceChannels = 1;
    exr.rgba = {1, 1, 1, 1, 2, 2, 2, 1, 3, 3, 3, 1};
    ASSERT_TRUE(editor::writeExr(file("rows.exr"), exr, false, &error)) << error;
    ASSERT_TRUE(editor::readFloatImage(file("rows.exr"), read, &error)) << error;
    EXPECT_EQ(read.rgba[0], 1.0f);
    EXPECT_EQ(read.rgba[8], 3.0f);
}

// --------------------------------------------------------------------------------------- TGA

TEST_F(FloatImageTest, TgaBothOriginsAndRle) {
    const int w = 3, h = 2;
    // top row first, BGR(A) as TGA stores it
    const std::vector<uint8_t> bgra = {1, 2, 3, 4,  5, 6, 7, 8,  9, 10, 11, 12,
                                       13, 14, 15, 16,  17, 18, 19, 20,  21, 22, 23, 24};
    std::vector<uint8_t> rgba(bgra.size());
    for (size_t p = 0; p < bgra.size() / 4; p++) {
        rgba[p * 4 + 0] = bgra[p * 4 + 2];
        rgba[p * 4 + 1] = bgra[p * 4 + 1];
        rgba[p * 4 + 2] = bgra[p * 4 + 0];
        rgba[p * 4 + 3] = bgra[p * 4 + 3];
    }
    for (bool top : {true, false}) {
        SCOPED_TRACE(top ? "top-left origin" : "bottom-left origin");
        writeBytes(file("image.tga"), makeTga(w, h, 4, bgra, top));
        FloatImage read;
        std::string error;
        ASSERT_TRUE(editor::readFloatImage(file("image.tga"), read, &error)) << error;
        EXPECT_EQ(read.sourceChannels, 4);
        EXPECT_EQ(read.sourceBits, 8);
        expectRaw8(read, rgba);
    }

    // grey, uncompressed
    const std::vector<uint8_t> grey = {0, 128, 255, 7, 77, 177};
    writeBytes(file("grey.tga"), makeTga(w, h, 1, grey, false));
    FloatImage read;
    std::string error;
    ASSERT_TRUE(editor::readFloatImage(file("grey.tga"), read, &error)) << error;
    EXPECT_EQ(read.sourceChannels, 1);
    expectRaw8(read, expand8(grey, 1));

    // stb_image_write's TGA (RLE), RGB and RGBA
    for (int channels : {3, 4}) {
        const std::vector<uint8_t> samples = pattern8(13, 9, channels);
        ASSERT_TRUE(stbi_write_tga(file("rle.TGA").c_str(), 13, 9, channels, samples.data()));
        ASSERT_TRUE(editor::readFloatImage(file("rle.TGA"), read, &error)) << error;
        EXPECT_EQ(read.sourceChannels, channels);
        expectRaw8(read, expand8(samples, channels));
    }
}

TEST_F(FloatImageTest, RawEightBitLookupValues) {
    // a lookup table: every byte value, read back as its integer (raw8), never rounded through floats
    const int w = 256, h = 2;
    std::vector<uint8_t> lookup(static_cast<size_t>(w) * h * 4);
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            uint8_t* t = &lookup[(static_cast<size_t>(y) * w + x) * 4];
            t[0] = static_cast<uint8_t>(x);
            t[1] = static_cast<uint8_t>(255 - x);
            t[2] = static_cast<uint8_t>(x ^ 0x5A);
            t[3] = static_cast<uint8_t>(y ? x : 255 - x);
        }
    }
    ASSERT_TRUE(stbi_write_png(file("lookup.png").c_str(), w, h, 4, lookup.data(), w * 4));
    FloatImage read;
    std::string error;
    ASSERT_TRUE(editor::readFloatImage(file("lookup.png"), read, &error)) << error;
    expectRaw8(read, lookup);
    for (int x = 0; x < w; x++)
        EXPECT_EQ(static_cast<int>(std::lround(read.rgba[static_cast<size_t>(x) * 4] * 255.0f)), x);
}

// ------------------------------------------------------------------------------------ errors

TEST_F(FloatImageTest, Errors) {
    FloatImage read;
    std::string error;

    EXPECT_FALSE(editor::readFloatImage(file("missing.exr"), read, &error));
    EXPECT_NE(error.find("missing.exr: file not found"), std::string::npos) << error;
    EXPECT_FALSE(editor::readFloatImage(file("missing.exr"), read, nullptr));

    // a truncated EXR: half of the data, then only part of the header
    const FloatImage written = makeImage(64, 64, 4, false);
    ASSERT_TRUE(editor::writeExr(file("full.exr"), written, false, &error)) << error;
    const std::vector<uint8_t> full = readBytes(file("full.exr"));
    ASSERT_GT(full.size(), 200u);
    for (size_t keep : {full.size() / 2, full.size() - 1, static_cast<size_t>(40), static_cast<size_t>(6)}) {
        SCOPED_TRACE("truncated to " + std::to_string(keep) + " bytes");
        writeBytes(file("truncated.exr"), std::vector<uint8_t>(full.begin(), full.begin() + static_cast<long>(keep)));
        error.clear();
        EXPECT_FALSE(editor::readFloatImage(file("truncated.exr"), read, &error));
        EXPECT_NE(error.find("truncated.exr"), std::string::npos) << error;
        EXPECT_TRUE(error.find("truncated") != std::string::npos || error.find("not a valid EXR") != std::string::npos) << error;
        EXPECT_TRUE(read.rgba.empty());
        EXPECT_EQ(read.width, 0);
    }

    // a truncated PNG
    const std::vector<uint8_t> samples = pattern8(32, 32, 4);
    ASSERT_TRUE(stbi_write_png(file("full.png").c_str(), 32, 32, 4, samples.data(), 32 * 4));
    const std::vector<uint8_t> png = readBytes(file("full.png"));
    writeBytes(file("truncated.png"), std::vector<uint8_t>(png.begin(), png.begin() + static_cast<long>(png.size() / 2)));
    EXPECT_FALSE(editor::readFloatImage(file("truncated.png"), read, &error));
    EXPECT_NE(error.find("truncated.png: corrupt or truncated PNG"), std::string::npos) << error;

    // formats not read: a BMP, a JPEG, an empty file; and files whose content isn't their extension
    ASSERT_TRUE(stbi_write_bmp(file("image.bmp").c_str(), 32, 32, 4, samples.data()));
    EXPECT_FALSE(editor::readFloatImage(file("image.bmp"), read, &error));
    EXPECT_NE(error.find("image.bmp: unsupported image format (EXR, PNG and TGA are read)"), std::string::npos) << error;
    ASSERT_TRUE(stbi_write_jpg(file("image.jpg").c_str(), 32, 32, 4, samples.data(), 90));
    EXPECT_FALSE(editor::readFloatImage(file("image.jpg"), read, &error));
    EXPECT_NE(error.find("unsupported image format"), std::string::npos) << error;
    writeBytes(file("empty.png"), {});
    EXPECT_FALSE(editor::readFloatImage(file("empty.png"), read, &error));
    EXPECT_NE(error.find("empty.png: the file is empty"), std::string::npos) << error;
    fs::copy_file(file("image.bmp"), file("fake.exr"));
    EXPECT_FALSE(editor::readFloatImage(file("fake.exr"), read, &error));
    EXPECT_NE(error.find("fake.exr: not an EXR file"), std::string::npos) << error;
    fs::copy_file(file("image.bmp"), file("fake.png"));
    EXPECT_FALSE(editor::readFloatImage(file("fake.png"), read, &error));
    EXPECT_NE(error.find("fake.png: not a PNG file"), std::string::npos) << error;
    // content wins over the extension: a PNG named .tga is read as a PNG
    fs::copy_file(file("full.png"), file("named.tga"));
    EXPECT_TRUE(editor::readFloatImage(file("named.tga"), read, &error)) << error;

    // writeExr refuses images that aren't width * height * 4 floats and impossible tiles
    FloatImage bad;
    bad.width = 4;
    bad.height = 4;
    bad.rgba.resize(10);
    EXPECT_FALSE(editor::writeExr(file("bad.exr"), bad, true, &error));
    EXPECT_NE(error.find("width * height * 4"), std::string::npos) << error;
    ExrWriteOptions tiled;
    tiled.tiled = true;
    tiled.tileSize = 128;
    EXPECT_FALSE(editor::writeExr(file("bad.exr"), written, tiled, &error));
    EXPECT_NE(error.find("tile size 128"), std::string::npos) << error;
    EXPECT_FALSE(editor::writeExr((dir / "no-such-dir" / "x.exr").string(), written, true, &error));
    EXPECT_NE(error.find("can't be written"), std::string::npos) << error;
}
