// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The paint layer of sprite normal maps (SpriteNormalsPaint, Phase 5 spec 7.1): compositing
// (bit-exact where unpainted, on every golden fixture and both green conventions), the four
// operations and their selection, direction parsing, the jasper.spritenormals.paint chunk, the
// window's brush geometry and wand, determinism.

#include <gtest/gtest.h>

#include "spritenormals/SpriteMedia.h"
#include "spritenormals/SpriteNormalsField.h"
#include "spritenormals/SpriteNormalsPaint.h"
#include "spritenormals/SpriteNormalsPreview.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "texsynth/SynthesisRecipe.h"

#include "json.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace sn = jasperengine::editor::spritenormals;
namespace ts = jasperengine::editor::texsynth;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

    const fs::path kFixtures = SPRITENORMALS_FIXTURES;

    sn::Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        sn::Image image = sn::Image::make(w, h, 4, 8);
        for (size_t i = 0; i < image.pixelCount(); i++) {
            image.data[i * 4 + 0] = r;
            image.data[i * 4 + 1] = g;
            image.data[i * 4 + 2] = b;
            image.data[i * 4 + 3] = a;
        }
        return image;
    }

    void put(sn::Image& image, int x, int y, std::array<uint8_t, 4> v) { std::memcpy(image.pixel(x, y), v.data(), 4); }
    std::array<uint8_t, 4> at(const sn::Image& image, int x, int y) {
        const uint8_t* p = image.pixel(x, y);
        return {p[0], p[1], p[2], p[3]};
    }
    std::array<uint8_t, 4> at(const sn::PaintOverlay& o, int x, int y) {
        const uint8_t* p = &o.rgba[(static_cast<size_t>(y) * o.width + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
    void put(sn::PaintOverlay& o, int x, int y, std::array<uint8_t, 4> v) {
        std::memcpy(&o.rgba[(static_cast<size_t>(y) * o.width + x) * 4], v.data(), 4);
    }

    using Px = std::array<uint8_t, 4>;

    sn::PaintOp op(sn::PaintOpKind kind, sn::PaintWhere where, float strength = 1.0f) {
        sn::PaintOp o;
        o.kind = kind;
        o.where = std::move(where);
        o.strength = strength;
        return o;
    }

    sn::PaintWhere rect(int x, int y, int w, int h) {
        sn::PaintWhere where;
        where.rect = std::array<int, 4>{x, y, w, h};
        return where;
    }

    std::vector<sn::PaintOp> ops(const char* text) {
        std::vector<sn::PaintOp> out;
        std::vector<sn::Problem> problems;
        EXPECT_TRUE(sn::paintOpsFromJson(json::parse(text), out, problems)) << (problems.empty() ? std::string() : problems[0].message);
        return out;
    }

    // the first problem of parsing `text` as paint ops
    sn::Problem parseProblem(const char* text) {
        std::vector<sn::PaintOp> out;
        std::vector<sn::Problem> problems;
        EXPECT_FALSE(sn::paintOpsFromJson(json::parse(text), out, problems)) << text;
        EXPECT_TRUE(out.empty());
        return problems.empty() ? sn::Problem() : problems[0];
    }

    // Every input fixture as the generator's request (sheet.png sliced 32 x 32, animations by frame)
    struct Fixture {
        std::string name;
        sn::Request request;
    };

    std::vector<Fixture> fixtures() {
        std::vector<Fixture> list;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(kFixtures / "inputs")) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            sn::Source source;
            std::vector<sn::Problem> problems;
            if (!sn::loadSource(file, source, problems)) {
                ADD_FAILURE() << file << ": " << (problems.empty() ? "" : problems[0].message);
                continue;
            }
            Fixture f;
            f.name = file.filename().string();
            f.request.color = source.color;
            f.request.cells = source.cells;
            f.request.sequence = !source.cells.empty();
            if (f.name == "sheet.png") {
                std::string error;
                sn::Grid grid;
                grid.tileWidth = grid.tileHeight = 32;
                EXPECT_TRUE(sn::sliceGrid(grid, source.color.width, source.color.height, f.request.cells, error)) << error;
                f.request.sequence = true;
            }
            f.request.threads = 2;
            list.push_back(f);
        }
        return list;
    }

    void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    fs::path tempFile(const std::string& name) { return fs::temp_directory_path() / ("jasper-paint-" + std::to_string(getpid()) + "-" + name); }

    // the PNG chunk types in order, and where the chunk with `keyword` ends
    std::vector<std::string> chunkKeys(const std::vector<uint8_t>& png, size_t* paintEnd = nullptr) {
        std::vector<std::string> keys;
        size_t pos = 8;
        while (pos + 12 <= png.size()) {
            const uint32_t length = (uint32_t(png[pos]) << 24) | (uint32_t(png[pos + 1]) << 16) | (uint32_t(png[pos + 2]) << 8) | png[pos + 3];
            std::string type(reinterpret_cast<const char*>(&png[pos + 4]), 4);
            if (type == "iTXt") {
                type += ":" + std::string(reinterpret_cast<const char*>(&png[pos + 8]));
                if (paintEnd && type == std::string("iTXt:") + sn::kPaintKeyword) *paintEnd = pos + 12 + length;
            }
            keys.push_back(type);
            pos += 12 + length;
        }
        return keys;
    }

    // A minimal recipe (encodeWithRecipeAndPaint only needs an object to write)
    json recipe() { return {{"format", sn::kRecipeFormat}, {"version", 1}, {"role", "normal"}, {"source", {{"path", "hero.png"}}}}; }

    // A chunk text with `json` instead of the canonical one, spliced into a 4x4 PNG
    std::vector<uint8_t> pngWithPaintText(const std::string& text) {
        std::vector<uint8_t> plain, png;
        std::string error;
        EXPECT_TRUE(ts::encodePng(solid(4, 4, 128, 128, 255, 255), plain));
        EXPECT_TRUE(ts::insertRecipeChunk(plain, text, png, error, sn::kPaintKeyword)) << error;
        return png;
    }

    sn::PaintOverlay sampleOverlay(int w, int h) {
        sn::PaintOverlay o = sn::PaintOverlay::blank(w, h);
        put(o, 0, 0, {200, 60, 240, 255});
        put(o, 1, 0, {90, 170, 230, 128});
        put(o, w - 1, h - 1, {128, 128, 255, 7});
        return o;
    }
}

// ---------------------------------------------------------------- 1. composite identity

// An empty overlay changes nothing: generatePainted without paint is generate(); compositing an
// empty overlay over the OpenGL generation (a DirectX one flipped) gives generate()'s bytes in
// both conventions; and an active layer whose ops select nothing leaves every pixel as generated
TEST(SpriteNormalsPaint, EmptyOverlayIsByteIdenticalOnEveryFixture) {
    const std::vector<Fixture> list = fixtures();
    ASSERT_GE(list.size(), 20u);
    for (const Fixture& f : list) {
        for (bool directx : {false, true}) {
            for (sn::Preset preset : {sn::Preset::Default, sn::Preset::Detailed}) {
                SCOPED_TRACE(f.name + (directx ? " directx " : " opengl ") + sn::toString(preset));
                sn::Request request = f.request;
                request.settings = sn::presetSettings(preset);
                request.settings.directx = directx;
                const sn::Result plain = sn::generate(request);
                ASSERT_EQ(plain.status, sn::Status::Ok);

                sn::PaintLayer none;
                const sn::Result inactive = sn::generatePainted(request, none);
                EXPECT_EQ(inactive.normal.data, plain.normal.data);

                sn::Image gl = plain.normal;
                if (directx) sn::flipGreen(gl, request.color, request.settings.alphaThreshold);
                const sn::Image composed = sn::composite(gl, sn::PaintOverlay(), request.color, request.settings.alphaThreshold, directx);
                EXPECT_EQ(composed.data, plain.normal.data);
                const sn::Image blank = sn::composite(gl, sn::PaintOverlay::blank(gl.width, gl.height), request.color,
                                                      request.settings.alphaThreshold, directx);
                EXPECT_EQ(blank.data, plain.normal.data);

                sn::PaintLayer nothing;
                nothing.ops = {op(sn::PaintOpKind::Flatten, rect(-10, -10, 2, 2))};
                const sn::Result active = sn::generatePainted(request, nothing);
                ASSERT_EQ(active.status, sn::Status::Ok);
                EXPECT_EQ(active.normal.data, plain.normal.data);
                ASSERT_EQ(nothing.stats.ops.size(), 1u);
                EXPECT_EQ(nothing.stats.ops[0].selected, 0);
                EXPECT_EQ(nothing.stats.painted, 0);
            }
        }
    }
}

// ---------------------------------------------------------------- 2. coverage

TEST(SpriteNormalsPaint, CoverageZeroFullAndHalf) {
    const sn::Image source = solid(3, 1, 10, 20, 30, 255);
    sn::Image gl = solid(3, 1, 0, 0, 0, 255);
    put(gl, 0, 0, {128, 128, 255, 255});
    put(gl, 1, 0, {10, 128, 140, 255});
    put(gl, 2, 0, {0, 128, 128, 255});
    sn::PaintOverlay o = sn::PaintOverlay::blank(3, 1);

    // A = 0: the generation exactly; A = 255: the paint exactly
    EXPECT_EQ(sn::composite(gl, o, source, 0.03f, false).data, gl.data);
    put(o, 0, 0, {255, 128, 128, 255});
    put(o, 1, 0, {245, 128, 140, 255});
    put(o, 2, 0, {128, 255, 128, 255});
    sn::Image full = sn::composite(gl, o, source, 0.03f, false);
    EXPECT_EQ(at(full, 0, 0), (Px{255, 128, 128, 255}));
    EXPECT_EQ(at(full, 1, 0), (Px{245, 128, 140, 255}));
    EXPECT_EQ(at(full, 2, 0), (Px{128, 255, 128, 255}));

    // A = 128 (a = 0.50196): m = (1-a) g + a p, normalised, z at least 0.0175, by hand:
    // flat + right -> (0.70984, 0.00552, 0.70434) -> (218, 128, 217)
    // left + right (near-opposite, z 0.098) -> (0.03681, 0.03994, 0.99852) -> (132, 133, 255)
    // left-flat + up-flat: n.z 0.00553 < 0.0175 -> z clamped, renormalised -> (38, 218, 130)
    for (int x = 0; x < 3; x++) o.rgba[static_cast<size_t>(x) * 4 + 3] = 128;
    sn::Image half = sn::composite(gl, o, source, 0.03f, false);
    EXPECT_EQ(at(half, 0, 0), (Px{218, 128, 217, 255}));
    EXPECT_EQ(at(half, 1, 0), (Px{132, 133, 255, 255}));
    EXPECT_EQ(at(half, 2, 0), (Px{38, 218, 130, 255}));
}

// ---------------------------------------------------------------- 3. transparency

TEST(SpriteNormalsPaint, TransparentPixelsStayNeutralAndAlphaPassesThrough) {
    sn::Image source = sn::Image::make(256, 1, 4, 8);
    for (int x = 0; x < 256; x++) put(source, x, 0, {50, 60, 70, static_cast<uint8_t>(x)});
    sn::Image gl = solid(256, 1, 140, 120, 250, 0);   // the generation's alpha is never read
    sn::PaintOverlay o = sn::PaintOverlay::blank(256, 1);
    for (int x = 0; x < 256; x++) put(o, x, 0, {200, 60, 240, 255});
    const sn::PaintOverlay before = o;
    for (bool directx : {false, true}) {
        const sn::Image out = sn::composite(gl, o, source, 0.03f, directx);
        for (int x = 0; x < 256; x++) {
            SCOPED_TRACE(x);
            EXPECT_EQ(at(out, x, 0)[3], x);
            if (x / 255.0f <= 0.03f) EXPECT_EQ(at(out, x, 0), (Px{128, 128, 255, static_cast<uint8_t>(x)}));
            else EXPECT_EQ(at(out, x, 0), (Px{200, static_cast<uint8_t>(directx ? 195 : 60), 240, static_cast<uint8_t>(x)}));
        }
    }
    EXPECT_EQ(o, before) << "the overlay keeps the hidden pixels' paint";
    // a lower cutoff shows them again
    EXPECT_EQ(at(sn::composite(gl, o, source, 0.0f, false), 5, 0), (Px{200, 60, 240, 5}));
}

// ---------------------------------------------------------------- 4. DirectX

TEST(SpriteNormalsPaint, DirectxFlipsOnlyGreenAfterCompositingAndPaintStaysOpenGL) {
    sn::Image source = solid(4, 4, 100, 100, 100, 255);
    put(source, 3, 3, {0, 0, 0, 0});
    sn::Image gl = solid(4, 4, 150, 90, 240, 255);
    sn::PaintOverlay o = sampleOverlay(4, 4);
    sn::Image a = sn::composite(gl, o, source, 0.03f, true);
    sn::Image b = sn::composite(gl, o, source, 0.03f, false);
    sn::flipGreen(b, source, 0.03f);
    EXPECT_EQ(a.data, b.data);
    const sn::Image plain = sn::composite(gl, o, source, 0.03f, false);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(a.data[i * 4], plain.data[i * 4]);
        EXPECT_EQ(a.data[i * 4 + 2], plain.data[i * 4 + 2]);
        if (i == 15) EXPECT_EQ(a.data[i * 4 + 1], 128) << "transparent stays (128,128,255)";
        else EXPECT_EQ(a.data[i * 4 + 1], 255 - plain.data[i * 4 + 1]);
    }

    // painting "up the image" on a DirectX map stores OpenGL green up (218) and writes it down (37)
    sn::Request request;
    request.color = solid(8, 8, 120, 80, 60, 255);
    request.settings = sn::presetSettings(sn::Preset::Default);
    request.settings.directx = true;
    sn::PaintLayer layer;
    layer.ops = ops(R"([{"op":"angle","where":{"rect":[0,0,1,1]},"direction":{"azimuth_deg":90,"tilt_deg":45}}])");
    const sn::Result r = sn::generatePainted(request, layer);
    ASSERT_EQ(r.status, sn::Status::Ok);
    EXPECT_EQ(at(layer.overlay, 0, 0), (Px{128, 218, 218, 255}));
    EXPECT_EQ(at(r.normal, 0, 0), (Px{128, 37, 218, 255}));
    // and every unpainted pixel is the DirectX generation's own byte
    const sn::Result plainDx = sn::generate(request);
    for (int i = 1; i < 64; i++) EXPECT_EQ(std::memcmp(&r.normal.data[i * 4], &plainDx.normal.data[i * 4], 4), 0) << i;
}

// ---------------------------------------------------------------- 5. operations

TEST(SpriteNormalsPaint, AngleFlattenEraseAndTheStrengthRule) {
    const sn::Image source = solid(4, 1, 90, 90, 90, 255);
    const sn::Image gl = solid(4, 1, 150, 110, 240, 255);
    const sn::PaintContext ctx{source, gl, {}, 0.03f};
    sn::PaintOverlay o;
    sn::PaintStats stats;
    std::vector<sn::Problem> problems = sn::applyPaintOps(
        o,
        ops(R"([{"op":"angle","where":{"pixels":[[0,0]]},"direction":[1,0,1]},
                {"op":"flatten","where":{"pixels":[[1,0]]},"strength":0.5},
                {"op":"angle","where":{"pixels":[[2,0]]},"direction":{"azimuth_deg":180,"tilt_deg":45},"strength":0.25}])"),
        ctx, &stats);
    EXPECT_TRUE(problems.empty());
    ASSERT_TRUE(o.sized(4, 1));
    EXPECT_EQ(at(o, 0, 0), (Px{218, 128, 218, 255}));
    EXPECT_EQ(at(o, 1, 0), (Px{128, 128, 255, 128}));   // round(255 * 0.5) = 128 (half away from zero)
    EXPECT_EQ(at(o, 2, 0), (Px{37, 128, 218, 64}));     // round(63.75)
    EXPECT_EQ(at(o, 3, 0), (Px{128, 128, 255, 0}));
    ASSERT_EQ(stats.ops.size(), 3u);
    EXPECT_EQ(stats.ops[0].selected, 1);
    EXPECT_EQ(stats.ops[0].changed, 1);
    EXPECT_EQ(stats.painted, 3);
    EXPECT_EQ(stats.hidden, 0);

    // A = max(A, round(255 s)): a weaker flatten keeps the coverage, a stronger one raises it
    sn::applyPaintOps(o, ops(R"([{"op":"flatten","where":{"pixels":[[0,0]]},"strength":0.2}])"), ctx, &stats);
    EXPECT_EQ(at(o, 0, 0), (Px{128, 128, 255, 255}));
    sn::applyPaintOps(o, ops(R"([{"op":"flatten","where":{"pixels":[[2,0]]},"strength":0.1}])"), ctx, &stats);
    EXPECT_EQ(at(o, 2, 0), (Px{128, 128, 255, 64}));
    // repeating an op changes nothing the second time
    sn::applyPaintOps(o, ops(R"([{"op":"flatten","where":{"pixels":[[2,0]]},"strength":0.1}])"), ctx, &stats);
    EXPECT_EQ(stats.ops[0].selected, 1);
    EXPECT_EQ(stats.ops[0].changed, 0);

    // Erase: A = round(A (1 - s)); twice 0.5 from 255 = 64 (as round(255 * 0.25)); 0 resets RGB
    sn::applyPaintOps(o, ops(R"([{"op":"erase","where":{"pixels":[[0,0]]},"strength":0.5},{"op":"erase","where":{"pixels":[[0,0]]},"strength":0.5}])"),
                      ctx, &stats);
    EXPECT_EQ(at(o, 0, 0)[3], 64);
    sn::applyPaintOps(o, ops(R"([{"op":"angle","where":{"pixels":[[3,0]]},"direction":[0,1,1]},{"op":"erase","where":{"pixels":[[3,0]]}}])"), ctx, &stats);
    EXPECT_EQ(at(o, 3, 0), (Px{128, 128, 255, 0}));
    EXPECT_EQ(stats.ops[1].changed, 1);
}

namespace {
    std::array<uint8_t, 3> meanOf(const std::vector<std::array<uint8_t, 3>>& values) {
        double sum[3] = {0, 0, 0};
        for (const auto& v : values) {
            for (int k = 0; k < 3; k++) sum[k] += v[k] / 127.5 - 1.0;
        }
        const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        std::array<uint8_t, 3> out;
        for (int k = 0; k < 3; k++) out[k] = static_cast<uint8_t>(std::round((sum[k] / length * 0.5 + 0.5) * 255.0));
        return out;
    }
}

// 5 x 5: cells x 0..2 and x 3..4; column x = 1 transparent, so x = 0 is another island of the
// same cell. A radius-2 Smooth at (2, 2) averages column 2 only.
TEST(SpriteNormalsPaint, SmoothAveragesTheCompositeOfItsIslandAndCell) {
    sn::Image source = solid(5, 5, 90, 90, 90, 255);
    for (int y = 0; y < 5; y++) put(source, 1, y, {0, 0, 0, 0});
    sn::Image gl = solid(5, 5, 0, 128, 140, 255);   // far off: any of them counted would show
    const std::vector<std::array<uint8_t, 3>> column = {{150, 128, 250}, {128, 150, 250}, {128, 128, 255}, {110, 128, 250}, {128, 128, 255}};
    for (int y = 0; y < 5; y++) put(gl, 2, y, {column[y][0], column[y][1], column[y][2], 255});
    const sn::PaintContext ctx{source, gl, {{0, 0, 3, 5}, {3, 0, 2, 5}}, 0.03f};
    sn::PaintOverlay o;
    sn::PaintStats stats;
    sn::applyPaintOps(o, ops(R"([{"op":"smooth","where":{"pixels":[[2,2]]},"radius":2}])"), ctx, &stats);
    const std::array<uint8_t, 3> expected = meanOf(column);
    EXPECT_EQ(at(o, 2, 2), (Px{expected[0], expected[1], expected[2], 255}));

    // radius 1: rows 1..3 of the column
    o = sn::PaintOverlay();
    sn::applyPaintOps(o, ops(R"([{"op":"smooth","where":{"pixels":[[2,2]]},"radius":1,"strength":0.5}])"), ctx, &stats);
    const std::array<uint8_t, 3> small = meanOf({column[1], column[2], column[3]});
    EXPECT_EQ(at(o, 2, 2), (Px{small[0], small[1], small[2], 128}));

    // Smooth reads the composite after the ops before it: (2, 0) painted first
    o = sn::PaintOverlay();
    sn::applyPaintOps(o, ops(R"([{"op":"angle","where":{"pixels":[[2,0]]},"direction":[0,-1,1]},{"op":"smooth","where":{"pixels":[[2,2]]},"radius":2}])"), ctx,
                      &stats);
    std::vector<std::array<uint8_t, 3>> painted = column;
    painted[0] = {128, 37, 218};
    const std::array<uint8_t, 3> after = meanOf(painted);
    EXPECT_EQ(at(o, 2, 2), (Px{after[0], after[1], after[2], 255}));
    EXPECT_NE(after, expected);
}

// ---------------------------------------------------------------- 6. selection

TEST(SpriteNormalsPaint, SelectionRectPixelsColoursFramesIntersectAndSkipTransparent) {
    // 8 x 4, two 4 x 4 frames; colours: (200,100,50) on the left half, (201,100,50) at x = 4,
    // (20,200,20) elsewhere; (7, 3) transparent
    sn::Image source = solid(8, 4, 20, 200, 20, 255);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) put(source, x, y, {200, 100, 50, 255});
        put(source, 4, y, {201, 100, 50, 255});
    }
    put(source, 7, 3, {20, 200, 20, 0});
    const sn::Image gl = solid(8, 4, 128, 128, 255, 255);
    const sn::PaintContext ctx{source, gl, {{0, 0, 4, 4}, {4, 0, 4, 4}}, 0.03f};
    auto select = [&](const char* where) {
        std::vector<sn::PaintOp> list = ops((std::string(R"([{"op":"flatten","where":)") + where + "}]").c_str());
        std::vector<int> pixels;
        EXPECT_TRUE(sn::selectPixels(list.at(0).where, ctx, pixels));
        return pixels;
    };
    // rect clipped to the image
    EXPECT_EQ(select(R"({"rect":[6,2,10,10]})"), (std::vector<int>{2 * 8 + 6, 2 * 8 + 7, 3 * 8 + 6}));
    // a pixel list: outside the image dropped, duplicates once, ascending
    EXPECT_EQ(select(R"({"pixels":[[1,1],[1,1],[-1,0],[9,0],[0,0]]})"), (std::vector<int>{0, 9}));
    // colours: exact matches one, a tolerance (Oklab) both
    EXPECT_EQ(select(R"({"colors":["#c86432"]})").size(), 16u);
    EXPECT_EQ(select(R"({"colors":["#c86432"],"tolerance":0.02})").size(), 20u);
    EXPECT_EQ(select(R"({"colors":["#C86432","#14c814"]})").size(), 16u + 11u);
    // a frame: rect and pixels relative to its cell, clipped to it
    EXPECT_EQ(select(R"({"frame":1,"rect":[0,0,1,2]})"), (std::vector<int>{4, 12}));
    EXPECT_EQ(select(R"({"frame":1,"rect":[3,3,5,5]})"), (std::vector<int>{})) << "(7, 3) is transparent";
    EXPECT_EQ(select(R"({"frame":1,"pixels":[[0,0],[4,0]]})"), (std::vector<int>{4})) << "(8, 0) is outside frame 1";
    EXPECT_EQ(select(R"({"frame":1})").size(), 15u);
    // members intersect
    EXPECT_EQ(select(R"({"rect":[2,0,4,1],"colors":["#c86432"]})"), (std::vector<int>{2, 3}));
    EXPECT_EQ(select(R"({"frame":0,"colors":["#14c814"]})"), (std::vector<int>{}));

    // an empty selection is a warning naming the op, not an error
    sn::PaintOverlay o;
    sn::PaintStats stats;
    const std::vector<sn::Problem> problems =
        sn::applyPaintOps(o, ops(R"([{"op":"flatten","where":{"frame":0}},{"op":"flatten","where":{"frame":0,"colors":["#14c814"]}}])"), ctx, &stats);
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "paint_empty_selection");
    EXPECT_EQ(problems[0].field, "paint[1]");
    EXPECT_EQ(stats.ops[0].selected, 16);
    EXPECT_EQ(stats.ops[1].selected, 0);
    EXPECT_EQ(stats.painted, 16);

    // a frame that is not one: invalid_paint, nothing applied
    sn::PaintOverlay untouched;
    const std::vector<sn::Problem> bad = sn::applyPaintOps(untouched, ops(R"([{"op":"flatten","where":{"frame":0}},{"op":"flatten","where":{"frame":2}}])"), ctx);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].kind, "invalid_paint");
    EXPECT_EQ(bad[0].field, "paint[1].where.frame");
    EXPECT_NE(bad[0].message.find("0 to 1"), std::string::npos) << bad[0].message;
    EXPECT_TRUE(untouched.empty());

    // hidden: painted pixels now transparent
    sn::PaintOverlay hidden = sn::PaintOverlay::blank(8, 4);
    put(hidden, 7, 3, {128, 128, 255, 255});
    sn::applyPaintOps(hidden, {}, ctx, &stats);
    EXPECT_EQ(stats.painted, 1);
    EXPECT_EQ(stats.hidden, 1);
}

// ---------------------------------------------------------------- 7. directions

TEST(SpriteNormalsPaint, DirectionsFromVectorsAndAngles) {
    auto direction = [](const char* d) {
        const std::vector<sn::PaintOp> list = ops((std::string(R"([{"op":"angle","where":{"frame":0},"direction":)") + d + "}]").c_str());
        return list.empty() ? std::array<float, 3>{0, 0, 0} : list[0].direction;
    };
    auto near = [](std::array<float, 3> a, std::array<float, 3> b) {
        for (int k = 0; k < 3; k++) {
            if (std::abs(a[k] - b[k]) > 1e-5f) return false;
        }
        return true;
    };
    const float h = std::sqrt(0.5f);
    EXPECT_TRUE(near(direction("[0,0,2]"), {0, 0, 1}));
    EXPECT_TRUE(near(direction("[3,0,4]"), {0.6f, 0, 0.8f}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":0,"tilt_deg":45})"), {h, 0, h}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":90,"tilt_deg":45})"), {0, h, h}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":180,"tilt_deg":45})"), {-h, 0, h}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":270,"tilt_deg":45})"), {0, -h, h}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":450,"tilt_deg":45})"), direction(R"({"azimuth_deg":90,"tilt_deg":45})")));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":-90,"tilt_deg":45})"), direction(R"({"azimuth_deg":270,"tilt_deg":45})")));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":30,"tilt_deg":0})"), {0, 0, 1}));
    EXPECT_TRUE(near(direction(R"({"azimuth_deg":0,"tilt_deg":85})"), {std::sin(85.0f * 3.14159265f / 180.0f), 0, std::cos(85.0f * 3.14159265f / 180.0f)}));
    // 90 and 270 encode x exactly neutral
    EXPECT_EQ(sn::encodeDirection(direction(R"({"azimuth_deg":270,"tilt_deg":45})")), (std::array<uint8_t, 3>{128, 37, 218}));
    EXPECT_EQ(sn::encodeDirection(direction(R"({"azimuth_deg":90,"tilt_deg":45})")), (std::array<uint8_t, 3>{128, 218, 218}));
    float az = 0, tilt = 0;
    sn::anglesOf(direction(R"({"azimuth_deg":135,"tilt_deg":40})"), az, tilt);
    EXPECT_NEAR(az, 135.0f, 1e-3f);
    EXPECT_NEAR(tilt, 40.0f, 1e-3f);

    EXPECT_EQ(parseProblem(R"([{"op":"angle","where":{"frame":0},"direction":[1,0,0]}])").field, "paint[0].direction");
    EXPECT_NE(parseProblem(R"([{"op":"angle","where":{"frame":0},"direction":[1,0,-1]}])").message.find("z > 0"), std::string::npos);
    EXPECT_NE(parseProblem(R"([{"op":"angle","where":{"frame":0},"direction":[0,0,0]}])").message.find("length 0"), std::string::npos);
    const sn::Problem tilt90 = parseProblem(R"([{"op":"angle","where":{"frame":0},"direction":{"azimuth_deg":0,"tilt_deg":90}}])");
    EXPECT_EQ(tilt90.kind, "invalid_paint");
    EXPECT_EQ(tilt90.field, "paint[0].direction.tilt_deg");
    EXPECT_NE(tilt90.message.find("85"), std::string::npos);
    EXPECT_EQ(parseProblem(R"([{"op":"angle","where":{"frame":0}}])").field, "paint[0].direction");
}

TEST(SpriteNormalsPaint, OpsJsonNamesEveryMistake) {
    EXPECT_NE(parseProblem(R"([{"op":"paint","where":{"frame":0}}])").message.find("angle, flatten, smooth or erase"), std::string::npos);
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"frame":0},"colour":1}])").field, "paint[0].colour");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten"}])").field, "paint[0].where");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{}}])").field, "paint[0].where");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"rect":[0,0,0,2]}}])").field, "paint[0].where.rect");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"pixels":[]}}])").field, "paint[0].where.pixels");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"pixels":[[1,2,3]]}}])").field, "paint[0].where.pixels[0]");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"colors":["white"]}}])").field, "paint[0].where.colors[0]");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"frame":0,"tolerance":0.1}}])").field, "paint[0].where.tolerance");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"colors":["#ffffff"],"tolerance":0.6}}])").field, "paint[0].where.tolerance");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"frame":-1}}])").field, "paint[0].where.frame");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"frame":0},"strength":1.5}])").field, "paint[0].strength");
    EXPECT_EQ(parseProblem(R"([{"op":"flatten","where":{"frame":0},"direction":[0,0,1]}])").field, "paint[0].direction");
    EXPECT_EQ(parseProblem(R"([{"op":"angle","where":{"frame":0},"direction":[0,0,1],"radius":2}])").field, "paint[0].radius");
    EXPECT_EQ(parseProblem(R"([{"op":"smooth","where":{"frame":0},"radius":5}])").field, "paint[0].radius");
    EXPECT_EQ(parseProblem(R"({"op":"flatten"})").field, "paint");
    // every op's first problem, not only the first op's
    std::vector<sn::PaintOp> out;
    std::vector<sn::Problem> problems;
    EXPECT_FALSE(sn::paintOpsFromJson(json::parse(R"([{"op":"x","where":{"frame":0}},{"op":"flatten","where":{"frame":0}},{"op":"erase"}])"), out, problems));
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[0].field, "paint[0].op");
    EXPECT_EQ(problems[1].field, "paint[2].where");
    // 257 operations
    json many = json::array();
    for (int i = 0; i < 257; i++) many.push_back({{"op", "flatten"}, {"where", {{"frame", 0}}}});
    problems.clear();
    EXPECT_FALSE(sn::paintOpsFromJson(many, out, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].message.find("257"), std::string::npos);
    many.erase(many.begin());
    problems.clear();
    EXPECT_TRUE(sn::paintOpsFromJson(many, out, problems));
    EXPECT_EQ(out.size(), 256u);

    // canonical JSON round trip (the fingerprint)
    const std::vector<sn::PaintOp> list =
        ops(R"([{"op":"angle","where":{"rect":[1,2,3,4],"frame":1},"direction":[0,0,1],"strength":0.5},{"op":"smooth","where":{"colors":["#ABCDEF"]},"radius":3}])");
    const json canonical = sn::paintOpsToJson(list);
    EXPECT_EQ(canonical.dump(),
              R"([{"direction":[0,0,1],"op":"angle","strength":0.5,"where":{"frame":1,"rect":[1,2,3,4]}},{"op":"smooth","radius":3,"strength":1,"where":{"colors":["#abcdef"],"tolerance":0}}])");
    std::vector<sn::PaintOp> again;
    EXPECT_TRUE(sn::paintOpsFromJson(canonical, again, problems));
    EXPECT_EQ(sn::paintOpsToJson(again), canonical);
}

// ---------------------------------------------------------------- 8. storage

TEST(SpriteNormalsPaint, ChunkRoundTripCanonicalAndOrdered) {
    sn::PaintOverlay o = sampleOverlay(5, 3);
    put(o, 2, 1, {1, 2, 3, 0});   // unpainted: its RGB is normalised away
    std::string text;
    sn::Problem problem;
    ASSERT_TRUE(sn::encodePaintText(o, text, problem)) << problem.message;
    EXPECT_EQ(text.rfind(R"({"format":"jasperengine.spritenormals.paint","version":1,"size":[5,3],"painted":3,"encoding":"png-base64","data":")", 0), 0u)
        << text.substr(0, 120);
    EXPECT_EQ(text.back(), '}');
    // equal overlays (they differ only under A = 0) -> equal text
    sn::PaintOverlay same = sampleOverlay(5, 3);
    std::string other;
    ASSERT_TRUE(sn::encodePaintText(same, other, problem));
    EXPECT_EQ(other, text);

    std::vector<uint8_t> png;
    const sn::Image normal = solid(5, 3, 128, 128, 255, 255);
    ASSERT_TRUE(sn::encodeWithRecipeAndPaint(normal, recipe(), &o, png, problem)) << problem.message;
    EXPECT_EQ(chunkKeys(png).at(0), "IHDR");
    EXPECT_EQ(chunkKeys(png).at(1), std::string("iTXt:") + sn::kRecipeKeyword);
    EXPECT_EQ(chunkKeys(png).at(2), std::string("iTXt:") + sn::kPaintKeyword);
    std::string why;
    const std::optional<sn::PaintOverlay> back = sn::readPaint(png.data(), png.size(), &why);
    ASSERT_TRUE(back.has_value()) << why;
    sn::PaintOverlay expected = o;
    expected.normalise();
    EXPECT_EQ(*back, expected);
    EXPECT_EQ(at(*back, 2, 1), (Px{128, 128, 255, 0}));
    EXPECT_TRUE(sn::readRecipe(png.data(), png.size()).has_value());
    // the PNG still decodes as the normal map
    ts::Image decoded;
    std::string error;
    ASSERT_TRUE(ts::loadImageFromMemory(png.data(), png.size(), decoded, error)) << error;
    EXPECT_EQ(decoded.data, normal.data);

    // an empty overlay writes no chunk
    std::vector<uint8_t> plain;
    EXPECT_FALSE(sn::encodePaintText(sn::PaintOverlay::blank(5, 3), text, problem));
    EXPECT_TRUE(problem.kind.empty());
    const sn::PaintOverlay blank = sn::PaintOverlay::blank(5, 3);
    ASSERT_TRUE(sn::encodeWithRecipeAndPaint(normal, recipe(), &blank, plain, problem));
    EXPECT_EQ(chunkKeys(plain).size(), 4u) << "IHDR, recipe, IDAT, IEND";
    EXPECT_FALSE(sn::readPaint(plain.data(), plain.size(), &why).has_value());
    EXPECT_TRUE(why.empty());
}

TEST(SpriteNormalsPaint, UnreadableChunksAreReportedNotThrown) {
    std::string text;
    sn::Problem problem;
    ASSERT_TRUE(sn::encodePaintText(sampleOverlay(4, 4), text, problem));
    const size_t data = text.find("\"data\":\"") + 8;
    auto unreadable = [](const std::string& chunk) {
        const std::vector<uint8_t> png = pngWithPaintText(chunk);
        std::string why;
        EXPECT_FALSE(sn::readPaint(png.data(), png.size(), &why).has_value()) << chunk.substr(0, 100);
        EXPECT_FALSE(why.empty());
        return why;
    };
    std::string corrupt = text;
    corrupt[data + 5] = '*';
    EXPECT_NE(unreadable(corrupt).find("base64"), std::string::npos);
    std::string wrongSize = text;
    wrongSize.replace(wrongSize.find("[4,4]"), 5, "[5,4]");
    EXPECT_NE(unreadable(wrongSize).find("5x4"), std::string::npos);
    std::string v2 = text;
    v2.replace(v2.find("\"version\":1"), 11, "\"version\":2");
    EXPECT_NE(unreadable(v2).find("version 2"), std::string::npos);
    std::string format = text;
    format.replace(format.find("spritenormals.paint"), 19, "spritenormals.other");
    unreadable(format);
    unreadable("not json");
    unreadable(R"({"format":"jasperengine.spritenormals.paint","version":1,"size":[4,4],"painted":1,"encoding":"png-base64","data":"aGVsbG8="})");
    // unknown keys are ignored
    std::string extra = text;
    extra.insert(1, R"("future":{"layers":2},)");
    const std::vector<uint8_t> png = pngWithPaintText(extra);
    std::string why;
    EXPECT_TRUE(sn::readPaint(png.data(), png.size(), &why).has_value()) << why;
}

// Every pixel painted with noise: the embedded PNG can't compress below 8 MiB
TEST(SpriteNormalsPaint, OversizedPaintIsRefusedWithItsLimit) {
    sn::PaintOverlay o;
    o.width = o.height = 2048;
    o.rgba.resize(2048u * 2048u * 4u);
    std::mt19937 random(7);
    for (size_t i = 0; i < o.rgba.size(); i++) o.rgba[i] = static_cast<uint8_t>(random() | ((i % 4 == 3) ? 1u : 0u));
    std::string text;
    sn::Problem problem;
    EXPECT_FALSE(sn::encodePaintText(o, text, problem));
    EXPECT_EQ(problem.kind, "paint_too_large");
    EXPECT_NE(problem.message.find("8 MiB"), std::string::npos) << problem.message;
    EXPECT_EQ(problem.fix, "Paint fewer pixels, or split the sheet.");
}

TEST(SpriteNormalsPaint, SummaryReadsOnlyTheHead) {
    const sn::PaintOverlay o = sampleOverlay(6, 2);
    std::vector<uint8_t> png;
    sn::Problem problem;
    ASSERT_TRUE(sn::encodeWithRecipeAndPaint(solid(6, 2, 128, 128, 255, 255), recipe(), &o, png, problem));
    size_t end = 0;
    chunkKeys(png, &end);
    ASSERT_GT(end, 0u);
    // truncated right after the paint chunk: no image data at all
    png.resize(end);
    const fs::path file = tempFile("head.png");
    writeBytes(file, png);
    std::string why;
    const std::optional<sn::PaintSummary> summary = sn::readPaintSummary(file, &why);
    ASSERT_TRUE(summary.has_value()) << why;
    EXPECT_EQ(summary->painted, 3);
    EXPECT_EQ(summary->width, 6);
    EXPECT_EQ(summary->height, 2);
    // a map without paint: nothing, no problem
    std::vector<uint8_t> plain;
    ASSERT_TRUE(sn::encodeWithRecipeAndPaint(solid(6, 2, 128, 128, 255, 255), recipe(), nullptr, plain, problem));
    writeBytes(file, plain);
    EXPECT_FALSE(sn::readPaintSummary(file, &why).has_value());
    EXPECT_TRUE(why.empty()) << why;
    // a garbled head: the problem
    writeBytes(file, pngWithPaintText(R"({"format":"jasperengine.spritenormals.paint","version":3,"size":[4,4],"painted":1,"data":""})"));
    EXPECT_FALSE(sn::readPaintSummary(file, &why).has_value());
    EXPECT_NE(why.find("version 3"), std::string::npos) << why;
    fs::remove(file);
}

// ---------------------------------------------------------------- 9. determinism

TEST(SpriteNormalsPaint, ThreadCountNeverChangesPaintedBytes) {
    for (const Fixture& f : fixtures()) {
        if (f.name != "character.png" && f.name != "walk.gif" && f.name != "sheet.png") continue;
        SCOPED_TRACE(f.name);
        std::vector<uint8_t> first;
        for (int threads : {1, 3, 8}) {
            sn::Request request = f.request;
            request.settings = sn::presetSettings(sn::Preset::Default);
            request.settings.directx = true;
            request.threads = threads;
            sn::PaintLayer layer;
            layer.ops = ops(R"([{"op":"angle","where":{"rect":[0,0,9,9]},"direction":{"azimuth_deg":200,"tilt_deg":30},"strength":0.6},
                                {"op":"smooth","where":{"frame":0},"radius":2,"strength":0.8},{"op":"erase","where":{"rect":[2,2,3,3]},"strength":0.5}])");
            const sn::Result r = sn::generatePainted(request, layer);
            ASSERT_EQ(r.status, sn::Status::Ok);
            if (first.empty()) first = r.normal.data;
            else EXPECT_EQ(r.normal.data, first) << threads << " threads";
        }
    }
}

// ---------------------------------------------------------------- the window's geometry

TEST(SpriteNormalsPaint, BrushFootprintsLinesAndCells) {
    using Pixels = std::vector<std::array<int, 2>>;
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Round, 1, 5, 5), (Pixels{{5, 5}}));
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Round, 2, 5, 5), (Pixels{{4, 4}, {5, 4}, {4, 5}, {5, 5}}));
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Square, 2, 5, 5), (Pixels{{4, 4}, {5, 4}, {4, 5}, {5, 5}}));
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Round, 3, 5, 5).size(), 9u);
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Square, 3, 5, 5).size(), 9u);
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Round, 4, 5, 5).size(), 12u) << "4 x 4 without its corners";
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Square, 4, 5, 5).front(), (std::array<int, 2>{3, 3}));
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Round, 5, 5, 5).size(), 21u);
    EXPECT_EQ(sn::brushFootprint(sn::PaintBrush::Shape::Square, 64, 0, 0).size(), 4096u);

    EXPECT_EQ(sn::linePixels({0, 0}, {3, 3}), (Pixels{{0, 0}, {1, 1}, {2, 2}, {3, 3}}));
    EXPECT_EQ(sn::linePixels({0, 0}, {4, 1}), (Pixels{{0, 0}, {1, 0}, {2, 1}, {3, 1}, {4, 1}})) << "the tie at x = 2 steps";
    EXPECT_EQ(sn::linePixels({0, 0}, {1, 5}).size(), 6u) << "one pixel per row: no gaps";
    EXPECT_EQ(sn::linePixels({2, 2}, {2, 2}), (Pixels{{2, 2}}));
    EXPECT_EQ(sn::linePixels({3, 0}, {0, 0}).back(), (std::array<int, 2>{0, 0}));

    const std::vector<sn::Cell> cells = {{0, 0, 4, 4}, {4, 0, 4, 4}};
    EXPECT_EQ(sn::cellAt(cells, 8, 4, 5, 1), 1);
    EXPECT_EQ(sn::cellAt(cells, 8, 4, 8, 1), -1);
    EXPECT_EQ(sn::cellAt({}, 8, 4, 7, 3), 0);
}

TEST(SpriteNormalsPaint, WandExactToleranceContiguousAndFrames) {
    // 8 x 4, frames 4 x 4; red (200,0,0) at x = 0 and x = 2 of each frame, nearly red (205,0,0)
    // at x = 1 of frame 0 rows 0..1; grey elsewhere
    sn::Image source = solid(8, 4, 90, 90, 90, 255);
    for (int y = 0; y < 4; y++) {
        for (int x : {0, 2, 4, 6}) put(source, x, y, {200, 0, 0, 255});
    }
    put(source, 1, 0, {205, 0, 0, 255});
    put(source, 1, 1, {205, 0, 0, 255});
    const sn::Image gl = solid(8, 4, 128, 128, 255, 255);
    const sn::PaintContext ctx{source, gl, {{0, 0, 4, 4}, {4, 0, 4, 4}}, 0.03f};
    EXPECT_EQ(sn::wandSelect(ctx, 0, 0, 0.0f, true, false).size(), 4u) << "column 0 only (x = 2 is not connected)";
    EXPECT_EQ(sn::wandSelect(ctx, 0, 0, 0.0f, false, false).size(), 8u);
    EXPECT_EQ(sn::wandSelect(ctx, 0, 0, 0.03f, true, false).size(), 10u) << "the near red joins and connects column 2";
    EXPECT_EQ(sn::wandSelect(ctx, 0, 0, 0.0f, false, true).size(), 16u);
    EXPECT_EQ(sn::wandSelect(ctx, 0, 0, 0.0f, true, true).size(), 4u);
    EXPECT_EQ(sn::wandSelect(ctx, 3, 0, 0.0f, true, true).size(), 4u) << "red columns cut the grey apart";
    EXPECT_EQ(sn::wandSelect(ctx, 3, 0, 0.0f, false, true).size(), 14u);
    EXPECT_EQ(sn::wandSelect(ctx, 3, 0, 0.0f, false, false).size(), 6u);
    sn::Image holes = source;
    put(holes, 0, 0, {200, 0, 0, 0});
    const sn::PaintContext holed{holes, gl, ctx.cells, 0.03f};
    EXPECT_TRUE(sn::wandSelect(holed, 0, 0, 0.0f, true, false).empty()) << "a transparent click selects nothing";
}

// ---------------------------------------------------------------- metrics and the contact sheet

TEST(SpriteNormalsPaint, MetricsCountPaintedVisiblePixels) {
    sn::Image source = solid(10, 10, 90, 90, 90, 255);
    for (int x = 0; x < 10; x++) put(source, x, 9, {0, 0, 0, 0});
    sn::Request request;
    request.color = source;
    request.settings = sn::presetSettings(sn::Preset::Chunky);
    sn::PaintLayer layer;
    layer.ops = ops(R"([{"op":"flatten","where":{"rect":[0,0,10,10]}}])");
    const sn::Result r = sn::generatePainted(request, layer);
    ASSERT_EQ(r.status, sn::Status::Ok);
    const sn::Metrics m = sn::measure(r.normal, {}, false, 0.03f, &layer.overlay);
    EXPECT_EQ(m.opaquePixels, 90);
    EXPECT_EQ(m.paintedPixels, 90);
    EXPECT_DOUBLE_EQ(m.paintedPct, 100.0);
    EXPECT_DOUBLE_EQ(m.flatPct, 100.0) << "the metrics are of the composite";
    const sn::Assessment a = sn::assess(request.settings, m, 0);
    EXPECT_EQ(a.lines.back(), "Painted: 90 px (100.0%)");
    // without paint: no line, zero
    const sn::Metrics plain = sn::measure(sn::generate(request).normal, {}, false, 0.03f);
    EXPECT_EQ(plain.paintedPixels, 0);
    EXPECT_LT(plain.flatPct, 100.0);
    for (const std::string& line : sn::assess(request.settings, plain, 0).lines) EXPECT_EQ(line.find("Painted"), std::string::npos);
}

TEST(SpriteNormalsPaint, ContactSheetOutlinesPaintInTheNormalColumnOnly) {
    sn::Image source = solid(16, 16, 90, 140, 90, 255);
    sn::Request request;
    request.color = source;
    request.settings = sn::presetSettings(sn::Preset::Default);
    sn::PaintLayer layer;
    layer.ops = ops(R"([{"op":"angle","where":{"rect":[6,6,3,3]},"direction":[1,0,1]}])");
    const sn::Result r = sn::generatePainted(request, layer);
    ASSERT_EQ(r.status, sn::Status::Ok);
    const sn::Image plainSheet = sn::contactSheet(source, r.normal, {}, false, 0.03f);
    const sn::Image sheet = sn::contactSheet(source, r.normal, {}, false, 0.03f, sn::SheetOptions(), &layer.overlay);
    ASSERT_EQ(sheet.width, plainSheet.width);
    auto isMagenta = [](const uint8_t* p) { return p[0] == 255 && p[1] == 0 && p[2] == 255; };
    // scale 8: tile w = 128, gap 4; the normal tile spans x 132..259
    int magenta = 0, outside = 0;
    for (int y = 0; y < sheet.height; y++) {
        for (int x = 0; x < sheet.width; x++) {
            const bool differs = std::memcmp(sheet.pixel(x, y), plainSheet.pixel(x, y), 4) != 0;
            if (!differs) continue;
            EXPECT_TRUE(isMagenta(sheet.pixel(x, y)));
            magenta++;
            if (x < 131 || x > 260) outside++;
        }
    }
    EXPECT_EQ(outside, 0) << "the lit columns stay clean";
    EXPECT_EQ(magenta, 4 * 24) << "a 1 px ring around the 24 x 24 painted block";
    // the painted pixels themselves are not covered
    EXPECT_FALSE(isMagenta(sheet.pixel(132 + 7 * 8 + 3, 7 * 8 + 3)));
    EXPECT_TRUE(isMagenta(sheet.pixel(132 + 6 * 8 - 1, 7 * 8 + 3)));
}

// ---------------------------------------------------------------- fix round 1

// a rectangle composited again gives composite()'s bytes; Smooth with a cached topology (and
// only its region composited) gives the same overlay as without
TEST(SpriteNormalsPaint, RectCompositeAndCachedTopologyMatchTheFullOnes) {
    sn::Request request;
    request.color = sn::Image::make(40, 24, 4, 8);
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 40; x++) {
            const bool in = (x % 20 - 10) * (x % 20 - 10) + (y - 12) * (y - 12) < 80;
            put(request.color, x, y, {static_cast<uint8_t>(90 + x * 3), static_cast<uint8_t>(60 + y * 5), 40, static_cast<uint8_t>(in ? 255 : 0)});
        }
    }
    request.cells = {{0, 0, 20, 24}, {20, 0, 20, 24}};
    request.settings = sn::presetSettings(sn::Preset::Default);
    for (bool directx : {false, true}) {
        request.settings.directx = directx;
        sn::Image gl = sn::generate(request).normal;
        if (directx) sn::flipGreen(gl, request.color, 0.03f);
        sn::PaintOverlay o = sn::PaintOverlay::blank(40, 24);
        const sn::PaintContext plain{request.color, gl, request.cells, 0.03f};
        sn::applyPaintOps(o, ops(R"([{"op":"angle","where":{"rect":[2,4,30,10]},"direction":[1,1,2],"strength":0.6}])"), plain);
        sn::Image full = sn::composite(gl, sn::PaintOverlay::blank(40, 24), request.color, 0.03f, directx);
        sn::compositeRect(full, gl, o, request.color, 0.03f, directx, 2, 4, 32, 14);
        EXPECT_EQ(full.data, sn::composite(gl, o, request.color, 0.03f, directx).data);

        sn::PaintOverlay a = o, b = o;
        const sn::PaintTopology topology = sn::paintTopology(request.color, request.cells, 0.03f);
        const sn::PaintContext cached{request.color, gl, request.cells, 0.03f, &topology};
        const auto smooth = ops(R"([{"op":"smooth","where":{"rect":[0,0,40,24]},"radius":2},{"op":"smooth","where":{"frame":1},"radius":1,"strength":0.5}])");
        sn::applyPaintOps(a, smooth, plain);
        sn::applyPaintOps(b, smooth, cached);
        EXPECT_EQ(a, b);
        EXPECT_GT(a.paintedCount(), o.paintedCount());
    }
}

TEST(SpriteNormalsPaint, OversizedDataIsRefusedBeforeDecoding) {
    const std::string data(4 * ((sn::kMaxPaintPngBytes + 2) / 3) + 4, 'A');
    const std::vector<uint8_t> png =
        pngWithPaintText(R"({"format":"jasperengine.spritenormals.paint","version":1,"size":[4,4],"painted":1,"encoding":"png-base64","data":")" + data + "\"}");
    std::string why;
    EXPECT_FALSE(sn::readPaint(png.data(), png.size(), &why).has_value());
    EXPECT_NE(why.find("8 MiB"), std::string::npos) << why;
}

// the head reader peeks each text chunk's keyword: a paint chunk before the recipe is skipped
// when the recipe is read, and read when the paint is
TEST(SpriteNormalsPaint, HeadReadersSkipOtherKeywords) {
    std::vector<uint8_t> plain, painted, both;
    ASSERT_TRUE(ts::encodePng(solid(4, 4, 128, 128, 255, 255), plain));
    std::string text, error;
    sn::Problem problem;
    ASSERT_TRUE(sn::encodePaintText(sampleOverlay(4, 4), text, problem));
    std::string recipeText;
    ASSERT_TRUE(sn::recipeText(recipe(), recipeText, error));
    // recipe first in the file, then paint (inserted in that order: paint ends up before it)
    ASSERT_TRUE(ts::insertRecipeChunk(plain, recipeText, painted, error, sn::kRecipeKeyword));
    ASSERT_TRUE(ts::insertRecipeChunk(painted, text, both, error, sn::kPaintKeyword));
    EXPECT_EQ(chunkKeys(both).at(1), std::string("iTXt:") + sn::kPaintKeyword);
    const fs::path file = tempFile("peek.png");
    writeBytes(file, both);
    EXPECT_TRUE(sn::readRecipeHead(file, &error).has_value()) << error;
    const std::optional<sn::PaintSummary> summary = sn::readPaintSummary(file, &error);
    ASSERT_TRUE(summary.has_value()) << error;
    EXPECT_EQ(summary->painted, 3);
    EXPECT_EQ(ts::readRecipeTextHead(file, &error, "jasper.spritenormals.pa"), std::nullopt) << "a keyword prefix is not the keyword";
    fs::remove(file);
}

// ---------------------------------------------------------------- Toon steps under paint

// Quantization happens in generate(): paint composites over the quantized result, so painted
// pixels keep exactly the painted direction (never snapped) and unpainted ones are the quantized
// bytes, in both conventions
TEST(SpriteNormalsPaint, PaintOverQuantizedOutputKeepsThePaintedValues) {
    for (const Fixture& f : fixtures()) {
        if (f.name != "character.png" && f.name != "ball.png" && f.name != "walk.gif") continue;
        for (bool directx : {false, true}) {
            SCOPED_TRACE(f.name + (directx ? " directx" : " opengl"));
            sn::Request request = f.request;
            request.settings = sn::presetSettings(sn::Preset::Toon);
            request.settings.directx = directx;
            const sn::Result plain = sn::generate(request);
            ASSERT_EQ(plain.status, sn::Status::Ok);
            sn::PaintLayer layer;
            // a direction no palette has (azimuth 17, tilt 23), full strength, on the whole image
            layer.ops = ops(R"([{"op":"angle","where":{"rect":[0,0,4096,4096]},"direction":{"azimuth_deg":17,"tilt_deg":23}}])");
            const sn::Result painted = sn::generatePainted(request, layer);
            ASSERT_EQ(painted.status, sn::Status::Ok);
            const std::array<uint8_t, 3> want = sn::encodeDirection(sn::directionFromAngles(17.0f, 23.0f));
            size_t checked = 0;
            for (int y = 0; y < plain.normal.height; y++) {
                for (int x = 0; x < plain.normal.width; x++) {
                    const Px got = at(painted.normal, x, y);
                    if (!(static_cast<float>(at(request.color, x, y)[3]) / 255.0f > request.settings.alphaThreshold)) {
                        EXPECT_EQ(got, at(plain.normal, x, y));
                        continue;
                    }
                    ASSERT_EQ(got[0], want[0]);
                    ASSERT_EQ(got[1], directx ? 255 - want[1] : want[1]);
                    ASSERT_EQ(got[2], want[2]);
                    checked++;
                }
            }
            EXPECT_GT(checked, 0u);
            // erased again: the quantized bytes
            sn::PaintLayer erased;
            erased.overlay = layer.overlay;
            erased.ops = ops(R"([{"op":"erase","where":{"rect":[0,0,4096,4096]}}])");
            EXPECT_EQ(sn::generatePainted(request, erased).normal.data, plain.normal.data);
        }
    }
}
