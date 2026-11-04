// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The codec's and the importer's budgets (docs/specs/S44-aseprite-import.md R26, R27, AC15) on
// documents built here at the spec's sizes: printed as [bench] lines, enforced with perfSlack()
// on the benchmark server only (a shared builder's load alone misses wall-clock budgets).

#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/aseprite/AsepriteImport.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;

namespace {

    double elapsedMs(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // R26's file: w x h RGBA, `frames` frames, `layers` layers of full-canvas cels (noisy pixels,
    // so deflate works as on painted art), `tags` tags splitting the frames
    Document benchDocument(int size, int frames, int layers, int tags, uint32_t seed) {
        Document d;
        d.width = size;
        d.height = size;
        d.mode = ColorMode::Rgba;
        d.frameDurations.assign(static_cast<size_t>(frames), 100);
        for (int l = 0; l < layers; l++) {
            Layer layer;
            layer.name = "Layer " + std::to_string(l);
            d.layers.push_back(layer);
        }
        d.rebuildTreeFromLevels();
        uint32_t state = seed;
        auto next = [&state]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<uint8_t>(state >> 24);
        };
        for (int f = 0; f < frames; f++) {
            for (int l = 0; l < layers; l++) {
                PixelImage image(size, size);
                for (int y = 0; y < size; y++) {
                    for (int x = 0; x < size; x++) {
                        // blocks of flat colour with some noise: a sprite, not static
                        const bool on = ((x / 8 + y / 8 + l + f) % 3) != 0;
                        const uint8_t n = next() & 0x0F;
                        image.set(x, y, on ? PixelRgba{static_cast<uint8_t>(l * 30 + n), static_cast<uint8_t>(f * 3), static_cast<uint8_t>(128 + n), 255}
                                           : PixelRgba{0, 0, 0, 0});
                    }
                }
                Cel cel;
                cel.layer = l;
                cel.frame = f;
                cel.image = std::make_shared<const PixelImage>(std::move(image));
                d.cels.push_back(cel);
            }
        }
        for (int t = 0; t < tags; t++) {
            Tag tag;
            tag.name = "tag" + std::to_string(t);
            tag.from = t * frames / tags;
            tag.to = (t + 1) * frames / tags - 1;
            d.tags.push_back(tag);
        }
        d.sortCels();
        return d;
    }

    void report(const char* what, double ms, double budget) {
        std::printf("[bench] AsepriteBench: %s %.2f ms (budget %.0f ms)\n", what, ms, budget);
        if (editor::test::perfSlack() > 0.0) {
            EXPECT_LT(ms, budget * editor::test::perfSlack()) << what;
        }
    }

}

// R26: read 256x256 x 64 frames x 8 layers <= 40 ms, compose one frame <= 1 ms, write with every
// cel changed <= 120 ms and unchanged <= 5 ms
TEST(AsepriteBench, Codec) {
    const Document built = benchDocument(256, 64, 8, 4, 1);
    std::vector<uint8_t> first;
    CodecError error;
    ASSERT_TRUE(write(built, first, &error)) << error.message;
    std::printf("[bench] AsepriteBench: the file is %.2f MB\n", static_cast<double>(first.size()) / (1024.0 * 1024.0));

    auto start = std::chrono::steady_clock::now();
    ReadResult read1 = read(first);
    report("read", elapsedMs(start), 40.0);
    ASSERT_TRUE(read1.ok()) << read1.error.message;

    start = std::chrono::steady_clock::now();
    const PixelImage frame = renderFrame(*read1.document, 10);
    report("compose one frame", elapsedMs(start), 1.0);
    EXPECT_EQ(frame.width(), 256);

    std::vector<uint8_t> same;
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(write(*read1.document, same, &error)) << error.message;
    report("write unchanged", elapsedMs(start), 5.0);
    EXPECT_EQ(same, first);

    Document changed = *read1.document;
    for (Cel& cel : changed.cels) {
        PixelImage image = *cel.image;
        image.set(0, 0, {1, 2, 3, 255});
        cel.image = std::make_shared<const PixelImage>(std::move(image));
    }
    std::vector<uint8_t> all;
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(write(changed, all, &error)) << error.message;
    report("write every cel changed", elapsedMs(start), 120.0);
}

// R27: a 64x64, 24-frame, 4-tag file imports (compose, trim, pack, clips) in <= 50 ms on one thread
TEST(AsepriteBench, Import) {
    const Document doc = benchDocument(64, 24, 4, 4, 7);
    AsepriteImportContext ctx;
    ctx.sourcePath = "sprites/hero64.aseprite";
    const auto start = std::chrono::steady_clock::now();
    const AsepriteImport import = AsepriteImport::run(doc, AsepriteImportSettings(), ctx);
    report("import 64x64x24", elapsedMs(start), 50.0);
    ASSERT_TRUE(import.ok()) << import.error.message;
    EXPECT_EQ(import.sheet.frames.size(), 24u);
    EXPECT_EQ(import.clips.size(), 4u);
}
