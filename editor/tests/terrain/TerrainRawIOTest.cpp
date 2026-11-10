// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R61, R68 (AC56): heightmap files, with their size, depth and byte order found from the bytes.

#include "terrain/TerrainRawIO.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    // A smooth hill, 0 at the corners to 1 in the middle
    std::vector<float> rawIOHill(int side) {
        std::vector<float> values(static_cast<size_t>(side) * side);
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                const float u = static_cast<float>(x) / static_cast<float>(side - 1) * 2.0f - 1.0f;
                const float v = static_cast<float>(y) / static_cast<float>(side - 1) * 2.0f - 1.0f;
                values[static_cast<size_t>(y) * side + x] = std::max(0.0f, 1.0f - (u * u + v * v) * 0.5f);
            }
        }
        values[0] = 0.0f;
        return values;
    }

}

TEST(TerrainRawIO, DetectSizeDepthOrderRange) {
    const int side = 33;
    std::vector<float> hill = rawIOHill(side);
    HeightExportOptions out;
    out.format = HeightFileFormat::R16;
    out.byteOrder = HeightByteOrder::Big;
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(TerrainRawIO::encode(hill, side, side, out, bytes, error)) << error;
    ASSERT_EQ(bytes.size(), static_cast<size_t>(side) * side * 2);

    const HeightImport in = TerrainRawIO::decode("dem.raw", bytes, HeightImportOptions());
    ASSERT_TRUE(in.ok) << in.error;
    EXPECT_EQ(in.format, HeightFileFormat::R16) << "only 16-bit samples make a square of this length";
    EXPECT_EQ(in.width, side);
    EXPECT_EQ(in.height, side);
    EXPECT_EQ(in.byteOrder, HeightByteOrder::Big) << "the smooth order wins";
    EXPECT_TRUE(in.formatGuessed && in.sizeGuessed && in.byteOrderGuessed);
    EXPECT_EQ(in.dataMin, 0.0);
    EXPECT_EQ(in.dataMax, 65535.0);
    EXPECT_FLOAT_EQ(in.normalized[(side / 2) * side + side / 2], 1.0f) << "auto range: the data's own top is the terrain's";

    HeightImportOptions given;
    given.byteOrder = HeightByteOrder::Little;
    const HeightImport wrong = TerrainRawIO::decode("dem.raw", bytes, given);
    ASSERT_TRUE(wrong.ok);
    EXPECT_FALSE(wrong.byteOrderGuessed) << "a given order is kept, even a wrong one";
    EXPECT_NE(wrong.normalized, in.normalized);

    HeightImportOptions source;
    source.range = HeightRange::Source;
    source.sourceMin = 0.0f;
    source.sourceMax = 32767.0f;
    const HeightImport clipped = TerrainRawIO::decode("dem.r16", bytes, source);
    ASSERT_TRUE(clipped.ok);
    EXPECT_GT(clipped.clippedHigh, 0) << "values past the given range clamp and are counted";
}

TEST(TerrainRawIO, AmbiguousRefuses) {
    // 64 bytes: an 8 x 8 map of bytes or a 4 x 4 map of floats
    const std::vector<uint8_t> bytes(64, 7);
    const HeightImport in = TerrainRawIO::decode("odd.raw", bytes, HeightImportOptions());
    EXPECT_FALSE(in.ok);
    EXPECT_NE(in.error.find("r8 8x8"), std::string::npos) << in.error;
    EXPECT_NE(in.error.find("r32f 4x4"), std::string::npos) << in.error;

    HeightImportOptions given;
    given.format = HeightFileFormat::R8;
    EXPECT_TRUE(TerrainRawIO::decode("odd.raw", bytes, given).ok) << "the format settles it";

    const HeightImport none = TerrainRawIO::decode("odd.raw", std::vector<uint8_t>(63, 0), HeightImportOptions());
    EXPECT_FALSE(none.ok);
    EXPECT_NE(none.error.find("width and height"), std::string::npos) << none.error;
}

TEST(TerrainRawIO, RoundTripEveryFormat) {
    const int side = 17;
    const std::vector<float> hill = rawIOHill(side);
    struct Case {
        HeightFileFormat format;
        const char* name;
        float tolerance;
    };
    // half a step, and a float's rounding on top
    for (const Case& c : {Case{HeightFileFormat::R8, "h.r8", 0.5f / 255.0f + 1e-6f}, Case{HeightFileFormat::R16, "h.r16", 0.5f / 65535.0f + 1e-6f},
                          Case{HeightFileFormat::R32F, "h.r32", 1e-7f}, Case{HeightFileFormat::Png, "h.png", 0.5f / 65535.0f + 1e-6f}}) {
        HeightExportOptions out;
        out.format = c.format;
        std::vector<uint8_t> bytes;
        std::string error;
        ASSERT_TRUE(TerrainRawIO::encode(hill, side, side, out, bytes, error)) << c.name << ": " << error;
        HeightImportOptions options;
        options.format = c.format;
        options.range = HeightRange::Full;
        const HeightImport in = TerrainRawIO::decode(c.name, bytes, options);
        ASSERT_TRUE(in.ok) << c.name << ": " << in.error;
        ASSERT_EQ(in.width, side) << c.name;
        float worst = 0.0f;
        for (size_t i = 0; i < hill.size(); i++) worst = std::max(worst, std::fabs(in.normalized[i] - hill[i]));
        EXPECT_LE(worst, c.tolerance) << c.name;
    }
}

TEST(TerrainRawIO, FlipVertically) {
    const std::vector<float> rows = {0.0f, 0.0f, 1.0f, 1.0f};
    HeightExportOptions out;
    out.flipVertically = true;
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(TerrainRawIO::encode(rows, 2, 2, out, bytes, error)) << error;
    HeightImportOptions options;
    options.format = HeightFileFormat::R16;
    options.width = 2;
    options.height = 2;
    options.byteOrder = HeightByteOrder::Little;
    options.range = HeightRange::Full;
    const HeightImport plain = TerrainRawIO::decode("f.r16", bytes, options);
    ASSERT_TRUE(plain.ok);
    EXPECT_EQ(plain.normalized, (std::vector<float>{1.0f, 1.0f, 0.0f, 0.0f})) << "written upside down";
    options.flipVertically = true;
    EXPECT_EQ(TerrainRawIO::decode("f.r16", bytes, options).normalized, rows) << "and read back the right way";
}
