// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 R26: an animated PNG's frames, delays and dispose/blend for create_sprite {from}.

#include "pixelart/PixelImage.h"
#include "pixelart/draw/SpriteImportFrames.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;

namespace {

    void be32(std::vector<uint8_t>& out, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
    }

    void pngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
        be32(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(out, 0);   // nothing checks the CRC of what this test feeds its own reader
    }

    // a zlib stream of stored blocks: no compressor needed to write a valid IDAT
    std::vector<uint8_t> stored(const std::vector<uint8_t>& raw) {
        std::vector<uint8_t> out = {0x78, 0x01};
        out.push_back(1);   // BFINAL, stored
        const uint16_t len = static_cast<uint16_t>(raw.size());
        out.push_back(static_cast<uint8_t>(len & 0xff));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len & 0xff));
        out.push_back(static_cast<uint8_t>((~len >> 8) & 0xff));
        out.insert(out.end(), raw.begin(), raw.end());
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        be32(out, (b << 16) | a);
        return out;
    }

    // 2x1 RGBA pixels, filter 0
    std::vector<uint8_t> rawRow(std::initializer_list<uint8_t> rgba) {
        std::vector<uint8_t> row = {0};
        row.insert(row.end(), rgba);
        return row;
    }

}

TEST(SpriteImportFrames, AnimatedPngFramesDelaysAndDisposeBackground) {
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, 2);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    pngChunk(file, "IHDR", ihdr);
    std::vector<uint8_t> actl;
    be32(actl, 2);
    be32(actl, 0);
    pngChunk(file, "acTL", actl);

    auto fctl = [](uint32_t seq, uint16_t delayNum, uint16_t delayDen, uint8_t dispose, uint8_t blend) {
        std::vector<uint8_t> d;
        be32(d, seq);
        be32(d, 2);
        be32(d, 1);
        be32(d, 0);
        be32(d, 0);
        d.push_back(static_cast<uint8_t>(delayNum >> 8));
        d.push_back(static_cast<uint8_t>(delayNum));
        d.push_back(static_cast<uint8_t>(delayDen >> 8));
        d.push_back(static_cast<uint8_t>(delayDen));
        d.push_back(dispose);
        d.push_back(blend);
        return d;
    };
    pngChunk(file, "fcTL", fctl(0, 25, 100, 1, 0));   // 250 ms, dispose to background
    pngChunk(file, "IDAT", stored(rawRow({255, 0, 0, 255, 0, 0, 0, 0})));
    pngChunk(file, "fcTL", fctl(1, 5, 100, 0, 1));    // 50 ms, blend over what is left
    std::vector<uint8_t> fdat;
    be32(fdat, 2);
    const std::vector<uint8_t> z = stored(rawRow({0, 0, 0, 0, 0, 255, 0, 255}));
    fdat.insert(fdat.end(), z.begin(), z.end());
    pngChunk(file, "fdAT", fdat);
    pngChunk(file, "IEND", {});

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s47-apng-test.png";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    ImportedFrames frames;
    PixelProblem problem;
    ASSERT_TRUE(readFramesFile(path, frames, problem)) << problem.message;
    ASSERT_EQ(frames.frames.size(), 2u);
    EXPECT_EQ(frames.durations, (std::vector<uint16_t>{250, 50}));
    EXPECT_EQ(frames.frames[0].at(0, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(frames.frames[0].at(1, 0)[3], 0);
    EXPECT_EQ(frames.frames[1].at(0, 0)[3], 0) << "dispose to background cleared frame 1's region";
    EXPECT_EQ(frames.frames[1].at(1, 0), (PixelRgba{0, 255, 0, 255}));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SpriteImportFrames, PlainPngIsOneFrame) {
    PixelImage image(1, 1);
    image.set(0, 0, {10, 20, 30, 255});
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s47-plain-test.png";
    ASSERT_TRUE(image.savePng(path));
    ImportedFrames frames;
    PixelProblem problem;
    ASSERT_TRUE(readFramesFile(path, frames, problem)) << problem.message;
    ASSERT_EQ(frames.frames.size(), 1u);
    EXPECT_EQ(frames.durations[0], 100);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
