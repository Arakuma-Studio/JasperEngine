// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The budgets of the sprite tools (docs/specs/S47-agent-sprite-drawing.md R38, AC15): printed as
// [bench] lines, enforced with perfSlack() on the benchmark run only. Each step is what the
// edit_sprite handler runs: a working copy, the ops, encode, reading the bytes back, the patch and
// the undo's re-encode, and the write.

#include "SpriteDrawTestSupport.h"

#include "io/AtomicFile.h"
#include "io/watch/FileWatcher.h"
#include "pixelart/draw/SpriteDocCache.h"
#include "pixelart/draw/SpritePatch.h"
#include "pixelart/draw/SpriteRender.h"
#include "pixelart/draw/SpriteRotate.h"
#include "support/EditorTestSupport.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::s47test::json;

namespace {

    double elapsedMs(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void budget(const char* name, double ms, double limit) {
        std::printf("[bench] %s: %.2f ms (budget %.0f)\n", name, ms, limit);
        const double slack = editor::test::perfSlack();
        if (slack > 0) {
            EXPECT_LE(ms, limit * slack) << name;
        }
    }

    draw::SpriteDoc framedSprite(int size, aseprite::ColorMode mode, int frames) {
        draw::SpriteDoc sprite = s47test::makeSprite(size, size, mode, frames);
        std::mt19937 random(7);
        // every frame gets a painted cel, so a whole file is more than empty headers
        for (int f = 1; f <= frames; f++) {
            json ops = json::array();
            for (int i = 0; i < 12; i++) {
                const int x = random() % std::max(1, size - 8), y = random() % std::max(1, size - 8);
                ops.push_back({{"op", "rect"}, {"rect", {x, y, 7, 5}}, {"color", 1 + (i + f) % 5}, {"fill", true}, {"frame", f}});
            }
            s47test::apply(sprite, ops);
        }
        return sprite;
    }

    // the handler's whole edit: returns the ops' time and the total
    void timedEdit(draw::SpriteDoc& session, const json& ops, double& opsMs, double& totalMs) {
        const auto total = std::chrono::steady_clock::now();
        draw::SpriteDoc working = session;
        const auto opsStart = std::chrono::steady_clock::now();
        const s47test::Run run = s47test::apply(working, ops);
        opsMs = elapsedMs(opsStart);
        ASSERT_TRUE(run.ok) << run.problem.message;
        std::vector<uint8_t> bytes;
        PixelProblem problem;
        ASSERT_TRUE(draw::encodeSprite(working, bytes, problem)) << problem.message;
        draw::SpriteDoc written;
        ASSERT_TRUE(draw::decodeSprite("bench.aseprite", bytes, written, problem)) << problem.message;
        std::shared_ptr<draw::SpritePatchSet> patch = draw::makePatch(session, working, run.result.structureChanged);
        draw::SpriteDoc undone = written;
        draw::applyPatch(undone, *patch, false);
        std::vector<uint8_t> undoneBytes;
        ASSERT_TRUE(draw::encodeSprite(undone, undoneBytes, problem)) << problem.message;
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "s47-bench.aseprite";
        ASSERT_TRUE(jasperengine::io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).ok());
        totalMs = elapsedMs(total);
        session = std::move(written);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

}

TEST(SpriteDrawBench, FiftyPaintOpsOn128) {
    draw::SpriteDoc sprite = framedSprite(128, aseprite::ColorMode::Indexed, 8);
    json ops = json::array();
    for (int i = 0; i < 50; i++) ops.push_back({{"op", "line"}, {"points", {{i, 0}, {i + 20, 40}}}, {"color", 1 + i % 5}, {"frame", 1 + i % 8}});
    double opsMs = 0, totalMs = 0;
    timedEdit(sprite, ops, opsMs, totalMs);
    budget("edit_sprite 50 paint ops 128x128x8 indexed (ops)", opsMs, 10);
    budget("edit_sprite 50 paint ops 128x128x8 indexed (total)", totalMs, 40);
}

TEST(SpriteDrawBench, OneCelOfA512RgbaFile) {
    draw::SpriteDoc sprite = framedSprite(512, aseprite::ColorMode::Rgba, 32);
    PixelProblem problem;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(draw::encodeSprite(sprite, bytes, problem));
    draw::SpriteDoc session;
    ASSERT_TRUE(draw::decodeSprite("bench.aseprite", bytes, session, problem));
    double opsMs = 0, totalMs = 0;
    timedEdit(session, json::array({{{"op", "pixel"}, {"points", {{3, 3}}}, {"color", "#ff0000"}}}), opsMs, totalMs);
    budget("edit_sprite 512x512x32 rgba, one cel", totalMs, 120);
}

TEST(SpriteDrawBench, Read4096Pixels) {
    const draw::SpriteDoc sprite = framedSprite(128, aseprite::ColorMode::Indexed, 1);
    const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
    const auto start = std::chrono::steady_clock::now();
    const EncodedGrid grid = encodeRegion(canvas, {0, 0, 64, 64}, GridEncoding::Auto, sprite.doc.paletteRgba(0), 0);
    budget("read_sprite_pixels 4096 pixels", elapsedMs(start), 2);
    EXPECT_EQ(grid.rows.size(), 64u);
}

TEST(SpriteDrawBench, RenderSixtyFourFramesAtFourX) {
    const draw::SpriteDoc sprite = framedSprite(64, aseprite::ColorMode::Indexed, 64);
    SheetOptions options;
    for (int f = 0; f < 64; f++) options.frames.push_back(f);
    options.scale = 4;
    nlohmann::json layout;
    const auto start = std::chrono::steady_clock::now();
    const PixelImage sheet = renderSheet(sprite, options, layout);
    const double renderMs = elapsedMs(start);
    const std::string png = encodeSheetPng(sheet);
    budget("render_sprite 64 frames 64x64 at 4x (with its PNG)", elapsedMs(start), 60);
    std::printf("[bench] the render alone: %.2f ms, PNG %zu KB\n", renderMs, png.size() / 1024);
    EXPECT_FALSE(png.empty());
}

TEST(SpriteDrawBench, RotateCleanEdge128) {
    const draw::SpriteDoc sprite = framedSprite(128, aseprite::ColorMode::Indexed, 1);
    const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
    const std::vector<uint8_t> rgba(canvas.pixels().begin(), canvas.pixels().end());
    const auto start = std::chrono::steady_clock::now();
    const std::vector<int32_t> map = rotateMap(128, 128, rgba, 30.0, 63.5, 63.5, RotateMethod::CleanEdge);
    budget("rotate cleanedge 128x128", elapsedMs(start), 40);
    EXPECT_EQ(map.size(), 128u * 128u);
}

TEST(SpriteDrawBench, StatOnEveryCall) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s47-bench-stat.bin";
    {
        std::vector<uint8_t> bytes(1024, 1);
        ASSERT_TRUE(jasperengine::io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).ok());
    }
    const auto start = std::chrono::steady_clock::now();
    jasperengine::watch::PathStat stat;
    for (int i = 0; i < 100; i++) jasperengine::watch::statPath(path, stat, false);
    budget("stat of a sprite file", elapsedMs(start) / 100.0, 0.1);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
