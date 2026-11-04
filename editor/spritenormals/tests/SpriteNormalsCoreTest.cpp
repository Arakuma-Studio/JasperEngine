// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-spritenormals-tests: the generator core (editor/spritenormals). Golden parity with
// the upstream spritefusion-sprite-normals binary (tests/fixtures, README.md there), upstream's
// own invariants (tests/generation.rs, distance.rs), determinism across thread counts, the
// JasperEngine options, the grid and the performance bound.

#include "spritenormals/SpriteMedia.h"
#include "object/ui/Image.h"
#include "spritenormals/SpriteNormals.h"
#include "spritenormals/SpriteNormalsField.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>

using namespace jasperengine::editor::spritenormals;
namespace fs = std::filesystem;
namespace texsynth = jasperengine::editor::texsynth;

namespace {

    const fs::path kFixtures = SPRITENORMALS_FIXTURES;

    Image loadPng(const fs::path& path) {
        texsynth::Image image;
        std::string error;
        EXPECT_TRUE(texsynth::loadImage(path, image, error)) << error;
        return texsynth::toRgba8(image);
    }

    Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        Image image = Image::make(w, h, 4, 8);
        for (int i = 0; i < w * h; i++) {
            uint8_t* p = image.data.data() + 4 * i;
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
        return image;
    }

    Image fromFunction(int w, int h, const std::function<std::array<uint8_t, 4>(int, int)>& f) {
        Image image = Image::make(w, h, 4, 8);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const std::array<uint8_t, 4> c = f(x, y);
                std::memcpy(image.pixel(x, y), c.data(), 4);
            }
        }
        return image;
    }

    // upstream tests/generation.rs disk(31): RGB (145, 99, 63), opaque where hypot <= 12
    Image disk(int size = 31) {
        const float center = static_cast<float>(size / 2);
        return fromFunction(size, size, [&](int x, int y) {
            const bool in = std::hypot(static_cast<float>(x) - center, static_cast<float>(y) - center) <= center - 3.0f;
            return std::array<uint8_t, 4>{145, 99, 63, static_cast<uint8_t>(in ? 255 : 0)};
        });
    }

    Image crop(const Image& image, int x0, int y0, int w, int h) {
        Image out = Image::make(w, h, 4, 8);
        for (int y = 0; y < h; y++) std::memcpy(out.pixel(0, y), image.pixel(x0, y0 + y), static_cast<size_t>(w) * 4);
        return out;
    }

    void paste(Image& into, const Image& image, int x0, int y0) {
        for (int y = 0; y < image.height; y++) std::memcpy(into.pixel(x0, y0 + y), image.pixel(0, y), static_cast<size_t>(image.width) * 4);
    }

    // The upstream binary's defaults (settings.rs): the "detailed" preset with radius 4
    Settings upstreamDefaults() {
        Settings s = presetSettings(Preset::Detailed);
        s.radius = 4.0f;
        return s;
    }

    Result run(const Image& color, const Settings& settings, std::vector<Cell> cells = {}, int threads = 1, bool sequence = false) {
        Request request;
        request.color = color;
        request.settings = settings;
        request.cells = std::move(cells);
        request.threads = threads;
        request.sequence = sequence;
        Result result = generate(request);
        EXPECT_EQ(result.status, Status::Ok) << (result.errors.empty() ? std::string() : result.errors[0].message);
        return result;
    }

    std::vector<Cell> gridCells(int w, int h, int fw, int fh, int count = -1) {
        std::vector<Cell> cells;
        const int columns = w / fw;
        const int n = count < 0 ? columns * (h / fh) : count;
        for (int i = 0; i < n; i++) cells.push_back({(i % columns) * fw, (i / columns) * fh, fw, fh});
        return cells;
    }

    struct Comparison {
        size_t pixels = 0;
        size_t exact = 0;
        int maxRgb = 0;
        int maxAlpha = 0;
    };

    Comparison compare(const Image& actual, const Image& expected) {
        Comparison c;
        EXPECT_EQ(actual.width, expected.width);
        EXPECT_EQ(actual.height, expected.height);
        if (actual.width != expected.width || actual.height != expected.height) return c;
        c.pixels = actual.pixelCount();
        for (size_t i = 0; i < c.pixels; i++) {
            const uint8_t* a = actual.data.data() + 4 * i;
            const uint8_t* e = expected.data.data() + 4 * i;
            if (std::memcmp(a, e, 4) == 0) c.exact++;
            for (int k = 0; k < 3; k++) c.maxRgb = std::max(c.maxRgb, std::abs(int(a[k]) - int(e[k])));
            c.maxAlpha = std::max(c.maxAlpha, std::abs(int(a[3]) - int(e[3])));
        }
        return c;
    }

    // ---- the golden fixtures (tests/fixtures/make_fixtures.py: same names and command lines)

    struct Golden {
        const char* name;
        const char* input;            // inputs/<input>; animations: expected/<name>_n.color.png
        bool animation;
        int frameWidth, frameHeight;  // --frame-size (0: none)
        std::function<void(Settings&)> adjust;
    };

    void variant(Settings& s) {
        s.strength = 2.5f;
        s.detail = 4.0f;
        s.smoothing = 1.0f;
        s.volume = 2.0f;
        s.radius = 8.0f;
        s.softness = 0.5f;
    }

    const std::vector<Golden>& goldens() {
        static const std::vector<Golden> list = {
            {"ball", "ball.png", false, 0, 0, nullptr},
            {"highlight", "highlight.png", false, 0, 0, nullptr},
            {"donut", "donut.png", false, 0, 0, nullptr},
            {"character", "character.png", false, 0, 0, nullptr},
            {"thin", "thin.png", false, 0, 0, nullptr},
            {"flat", "flat.png", false, 0, 0, nullptr},
            {"flat_outline", "flat_outline.png", false, 0, 0, nullptr},
            {"flat2", "flat2.png", false, 0, 0, nullptr},
            {"alpha", "alpha.png", false, 0, 0, nullptr},
            {"sheet", "sheet.png", false, 32, 32, nullptr},
            {"tiles", "tiles.png", false, 0, 0, nullptr},
            {"walk", "walk.gif", true, 0, 0, nullptr},
            {"spin", "spin.gif", true, 0, 0, nullptr},
            {"twoframes", "twoframes.png", true, 0, 0, nullptr},
            {"character_variant", "character.png", false, 0, 0, variant},
            {"ball_variant", "ball.png", false, 0, 0, variant},
            {"character_directx", "character.png", false, 0, 0, [](Settings& s) { s.directx = true; }},
            {"ball_directx", "ball.png", false, 0, 0, [](Settings& s) { s.directx = true; }},
            {"tiny_1x1", "tiny_1x1.png", false, 0, 0, nullptr},
            {"tiny_1x19", "tiny_1x19.png", false, 0, 0, nullptr},
            {"tiny_23x1", "tiny_23x1.png", false, 0, 0, nullptr},
            {"tiny_diagonal", "tiny_diagonal.png", false, 0, 0, nullptr},
            {"disk", "disk.png", false, 0, 0, nullptr},
            {"gradient_alpha", "gradient_alpha.png", false, 0, 0, nullptr},
            {"ramp", "ramp.png", false, 0, 0, [](Settings& s) { s.volume = 0.0f; }},
            {"sprite9", "sprite9.png", false, 0, 0, nullptr},
            {"islands_sheet", "islands_sheet.png", false, 0, 0, nullptr},
        };
        return list;
    }

    nlohmann::json readJson(const fs::path& path) {
        std::ifstream in(path);
        return nlohmann::json::parse(in);
    }

    // The request of a fixture: the colour image and its cells. Animations use the upstream
    // binary's own colour atlas and frames JSON (so parity does not depend on our decoders;
    // SpriteNormalsMediaTest compares loadSource's atlas with the same files)
    bool goldenRequest(const Golden& g, Image& color, std::vector<Cell>& cells, bool& sequence) {
        sequence = false;
        cells.clear();
        if (g.animation) {
            const std::string stem = std::string(g.name) + "_n";
            color = loadPng(kFixtures / "expected" / (stem + ".color.png"));
            const nlohmann::json frames = readJson(kFixtures / "expected" / (stem + ".frames.json"))["animation"];
            const int fw = frames["frame_width"], fh = frames["frame_height"], columns = frames["columns"], count = frames["frame_count"];
            for (int i = 0; i < count; i++) cells.push_back({(i % columns) * fw, (i / columns) * fh, fw, fh});
            sequence = true;
            return !color.empty();
        }
        color = loadPng(kFixtures / "inputs" / g.input);
        if (g.frameWidth > 0) cells = gridCells(color.width, color.height, g.frameWidth, g.frameHeight);
        return !color.empty();
    }
}

// ---------------------------------------------------------------- golden parity

// The acceptance gate (Phase 2 spec 12.1): alpha exact, RGB within 1, >= 99.5% of the pixels
// exactly equal, for every fixture made by the upstream binary
TEST(SpriteNormalsParity, GoldenFixturesMatchUpstream) {
    size_t allPixels = 0, allExact = 0;
    for (const Golden& g : goldens()) {
        SCOPED_TRACE(g.name);
        Image color;
        std::vector<Cell> cells;
        bool sequence;
        if (!goldenRequest(g, color, cells, sequence)) {
            ADD_FAILURE() << g.name << ": the fixture could not be read";
            continue;
        }
        Settings settings = upstreamDefaults();
        if (g.adjust) g.adjust(settings);
        const Result result = run(color, settings, cells, 1, sequence);
        const Image expected = loadPng(kFixtures / "expected" / (std::string(g.name) + "_n.png"));
        const Comparison c = compare(result.normal, expected);
        std::printf("[ parity   ] %-18s %4dx%-4d exact %6zu/%-6zu (%.3f%%) max rgb diff %d, alpha diff %d\n", g.name, expected.width,
                    expected.height, c.exact, c.pixels, c.pixels ? 100.0 * c.exact / c.pixels : 0.0, c.maxRgb, c.maxAlpha);
        EXPECT_EQ(c.maxAlpha, 0);
        EXPECT_LE(c.maxRgb, 1);
        EXPECT_GE(static_cast<double>(c.exact), 0.995 * static_cast<double>(c.pixels));
        allPixels += c.pixels;
        allExact += c.exact;
    }
    std::printf("[ parity   ] all fixtures: %zu/%zu pixels exact\n", allExact, allPixels);
}

// Upstream pads each island by ceil(3 max(softness, 2 smoothing)) + 3; the port pads by 1. Every
// field is 0 outside the island and the blurs extend with zeros, so a fixture placed with a wide
// transparent margin gives the same bytes as without it
TEST(SpriteNormalsParity, PaddingOfOneIsExact) {
    for (const char* name : {"ball.png", "character.png", "thin.png", "tiles.png"}) {
        SCOPED_TRACE(name);
        const Image color = loadPng(kFixtures / "inputs" / name);
        Image wide = Image::make(color.width + 40, color.height + 40, 4, 8);
        paste(wide, color, 20, 20);
        const Result a = run(color, upstreamDefaults());
        const Result b = run(wide, upstreamDefaults());
        EXPECT_EQ(a.normal.data, crop(b.normal, 20, 20, color.width, color.height).data);
    }
}

TEST(SpriteNormalsParity, ThreadCountNeverChangesTheBytes) {
    for (const Golden& g : goldens()) {
        SCOPED_TRACE(g.name);
        Image color;
        std::vector<Cell> cells;
        bool sequence;
        if (!goldenRequest(g, color, cells, sequence)) {
            ADD_FAILURE() << g.name << ": the fixture could not be read";
            continue;
        }
        Settings settings = upstreamDefaults();
        if (g.adjust) g.adjust(settings);
        const Result one = run(color, settings, cells, 1, sequence);
        const Result eight = run(color, settings, cells, 8, sequence);
        EXPECT_EQ(one.normal.data, eight.normal.data);
        // the advanced paths too
        Settings advanced = presetSettings(Preset::Default);
        advanced.ignoreOutline = true;
        advanced.height = true;
        advanced.regions.push_back({{0xffffff, 0x000000}, 0.1f, 0.5f, 2.0f, false});
        const Result a1 = run(color, advanced, cells, 1, sequence);
        const Result a8 = run(color, advanced, cells, 8, sequence);
        EXPECT_EQ(a1.normal.data, a8.normal.data);
        EXPECT_EQ(a1.height.data, a8.height.data);
    }
}

// ---------------------------------------------------------------- upstream invariants (tests/generation.rs)

TEST(SpriteNormalsUpstream, NormalsPointOutwardAndAreUnitVectors) {
    const Result r = run(disk(), upstreamDefaults());
    const Image& n = r.normal;
    EXPECT_LT(n.pixel(7, 15)[0], 100);
    EXPECT_GT(n.pixel(23, 15)[0], 155);
    EXPECT_GT(n.pixel(15, 7)[1], 155);    // OpenGL: the top of the disk faces up (green high)
    EXPECT_LT(n.pixel(15, 23)[1], 100);
    const uint8_t centre[4] = {128, 128, 255, 255};
    EXPECT_EQ(std::memcmp(n.pixel(15, 15), centre, 4), 0);
    for (size_t i = 0; i < n.pixelCount(); i++) {
        float length = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float c = static_cast<float>(n.data[4 * i + k]) / 127.5f - 1.0f;
            length += c * c;
        }
        EXPECT_NEAR(std::sqrt(length), 1.0f, 0.01f);
    }
}

TEST(SpriteNormalsUpstream, ZeroStrengthAndUniformShadingAreNeutral) {
    const Image input = disk();
    Settings a = upstreamDefaults();
    a.strength = 0.0f;
    Settings b = upstreamDefaults();
    b.volume = 0.0f;
    for (const Settings& s : {a, b}) {
        const Result r = run(input, s);
        for (size_t i = 0; i < input.pixelCount(); i++) {
            const uint8_t expected[4] = {128, 128, 255, input.data[4 * i + 3]};
            ASSERT_EQ(std::memcmp(r.normal.data.data() + 4 * i, expected, 4), 0) << i;
        }
    }
}

TEST(SpriteNormalsUpstream, BroadPaintedShadingRetainsRelief) {
    const Image input = fromFunction(64, 32, [](int x, int) {
        const uint8_t v = static_cast<uint8_t>(40 + x * 3);
        return std::array<uint8_t, 4>{v, v, v, 255};
    });
    Settings s = upstreamDefaults();
    s.volume = 0.0f;
    const Result r = run(input, s);
    for (int x = 12; x < 52; x++) {
        EXPECT_LT(r.normal.pixel(x, 16)[0], 123);
        EXPECT_LE(std::abs(int(r.normal.pixel(x, 16)[1]) - 128), 1);
    }
}

TEST(SpriteNormalsUpstream, AlphaIsPreservedAndHiddenRgbIgnored) {
    const Image input = fromFunction(16, 16, [](int x, int y) {
        return std::array<uint8_t, 4>{90, 160, 210, static_cast<uint8_t>(y * 16 + x)};
    });
    const Result r = run(input, upstreamDefaults());
    for (size_t i = 0; i < input.pixelCount(); i++) {
        EXPECT_EQ(r.normal.data[4 * i + 3], input.data[4 * i + 3]);
        if (input.data[4 * i + 3] <= 7) {
            EXPECT_EQ(r.normal.data[4 * i], 128);
            EXPECT_EQ(r.normal.data[4 * i + 1], 128);
            EXPECT_EQ(r.normal.data[4 * i + 2], 255);
        }
    }
    const Image original = disk();
    Image changed = original;
    for (size_t i = 0; i < changed.pixelCount(); i++) {
        if (changed.data[4 * i + 3] == 0) {
            changed.data[4 * i] = 255;
            changed.data[4 * i + 1] = 20;
            changed.data[4 * i + 2] = 200;
        }
    }
    EXPECT_EQ(run(original, upstreamDefaults()).normal.data, run(changed, upstreamDefaults()).normal.data);
    const Result empty = run(solid(3, 9, 255, 80, 10, 0), upstreamDefaults());
    for (size_t i = 0; i < 27; i++) {
        const uint8_t neutral[4] = {128, 128, 255, 0};
        EXPECT_EQ(std::memcmp(empty.normal.data.data() + 4 * i, neutral, 4), 0);
    }
    EXPECT_EQ(empty.islands, 0);
}

TEST(SpriteNormalsUpstream, PaddingAndNearbyIslandsDoNotChangeASprite) {
    const Image sprite = fromFunction(9, 9, [](int x, int) { return std::array<uint8_t, 4>{static_cast<uint8_t>(30 + x * 20), 80, 120, 255}; });
    const Result alone = run(sprite, upstreamDefaults());
    Image sheet = Image::make(42, 34, 4, 8);
    paste(sheet, sprite, 5, 5);
    paste(sheet, solid(21, 23, 255, 255, 255, 255), 15, 5);
    const Result together = run(sheet, upstreamDefaults());
    EXPECT_EQ(alone.normal.data, crop(together.normal, 5, 5, 9, 9).data);
    EXPECT_EQ(together.islands, 2);
}

TEST(SpriteNormalsUpstream, SheetCellsAreIsolated) {
    const Image sheet = fromFunction(32, 16, [](int x, int) {
        return x < 16 ? std::array<uint8_t, 4>{220, 70, 40, 255} : std::array<uint8_t, 4>{30, 80, 200, 255};
    });
    const Result r = run(sheet, upstreamDefaults(), gridCells(32, 16, 16, 16));
    for (int x : {0, 16}) {
        const Result cell = run(crop(sheet, x, 0, 16, 16), upstreamDefaults());
        EXPECT_EQ(cell.normal.data, crop(r.normal, x, 0, 16, 16).data);
    }
}

TEST(SpriteNormalsUpstream, TinyAndDiagonalShapesAreDeterministic) {
    for (auto [w, h] : {std::pair{1, 1}, std::pair{1, 19}, std::pair{23, 1}, std::pair{20, 20}}) {
        const Image input = fromFunction(w, h, [&](int x, int y) {
            return std::array<uint8_t, 4>{80, 80, 80, static_cast<uint8_t>(w == 1 || h == 1 || x == y ? 255 : 0)};
        });
        EXPECT_EQ(run(input, upstreamDefaults()).normal.data, run(input, upstreamDefaults()).normal.data);
    }
}

TEST(SpriteNormalsUpstream, DirectxOnlyFlipsGreen) {
    const Image input = disk();
    Settings dx = upstreamDefaults();
    dx.directx = true;
    const Result gl = run(input, upstreamDefaults());
    const Result d = run(input, dx);
    for (size_t i = 0; i < input.pixelCount(); i++) {
        const uint8_t* a = gl.normal.data.data() + 4 * i;
        const uint8_t* b = d.normal.data.data() + 4 * i;
        EXPECT_EQ(a[0], b[0]);
        EXPECT_EQ(a[2], b[2]);
        EXPECT_EQ(a[3], b[3]);
        EXPECT_LE(std::abs(int(a[1]) + int(b[1]) - 255), 1);
    }
}

TEST(SpriteNormalsUpstream, InvalidSettingsAndDimensionsAreRejected) {
    const Image input = disk();
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 9.0f}) {
        Request request;
        request.color = input;
        request.settings = upstreamDefaults();
        request.settings.strength = value;
        const Result r = generate(request);
        EXPECT_EQ(r.status, Status::InvalidRequest);
        ASSERT_FALSE(r.errors.empty());
        EXPECT_EQ(r.errors[0].kind, "invalid_settings");
        EXPECT_EQ(r.errors[0].field, "strength");
    }
    Request request;
    request.color = input;
    request.settings = upstreamDefaults();
    request.settings.radius = 0.0f;
    EXPECT_EQ(generate(request).errors.at(0).field, "radius");
    // cells outside the image, empty, overlapping
    for (const std::vector<Cell>& cells : {std::vector<Cell>{{0, 0, 32, 32}}, std::vector<Cell>{{0, 0, 0, 1}},
                                           std::vector<Cell>{{0, 0, 16, 16}, {8, 8, 16, 16}}, std::vector<Cell>{{-1, 0, 4, 4}}}) {
        request.settings = upstreamDefaults();
        request.cells = cells;
        const Result r = generate(request);
        EXPECT_EQ(r.status, Status::InvalidRequest);
        ASSERT_FALSE(r.errors.empty());
        EXPECT_EQ(r.errors[0].kind, "frames_mismatch") << r.errors[0].message;
    }
    request.cells = {{0, 0, 16, 16}, {8, 8, 16, 16}};
    EXPECT_EQ(generate(request).errors.at(0).message, "Cells 0 and 1 overlap.");
    request.cells.clear();
    request.color = Image();
    EXPECT_EQ(generate(request).errors.at(0).kind, "invalid_source");
    request.color = Image::make(4, 4, 3, 8);
    EXPECT_EQ(generate(request).errors.at(0).kind, "invalid_source");
    request.color = Image::make(4097, 4097, 4, 8);
    const Result big = generate(request);
    ASSERT_EQ(big.errors.size(), 1u);
    EXPECT_EQ(big.errors[0].kind, "too_large");
    EXPECT_NE(big.errors[0].message.find("16785409"), std::string::npos) << big.errors[0].message;
    EXPECT_NE(big.errors[0].message.find("16777216"), std::string::npos);
}

// ---------------------------------------------------------------- field operations

namespace {
    // upstream distance.rs: an LCG over sizes 1..12 x 1..16, a pixel inside 3 times out of 4
    template <typename Distance>
    void edtAgainstBruteForce(detail::Border border, Distance distance) {
        uint32_t state = 75;
        for (int height = 1; height < 13; height++) {
            for (int width = 1; width < 17; width++) {
                std::vector<uint8_t> mask(static_cast<size_t>(width * height));
                for (uint8_t& m : mask) {
                    state = state * 1664525u + 1013904223u;
                    m = state % 4 != 0;
                }
                const detail::Field field = detail::edt(mask, width, height, border);
                for (int i = 0; i < width * height; i++) {
                    float expected = std::numeric_limits<float>::infinity();
                    for (int o = 0; o < width * height; o++) {
                        if (mask[static_cast<size_t>(o)]) continue;
                        expected = std::min(expected, distance(i % width - o % width, i / width - o / width, width, height));
                    }
                    if (!mask[static_cast<size_t>(i)]) expected = 0.0f;
                    const float actual = field.data[static_cast<size_t>(i)];
                    ASSERT_TRUE((std::isinf(actual) && std::isinf(expected)) || std::fabs(actual - expected) < 1e-5f)
                        << width << "x" << height << " pixel " << i << ": " << actual << " vs " << expected;
                }
            }
        }
    }
}

TEST(SpriteNormalsField, EdtMatchesBruteForce) {
    auto plane = [](int dx, int dy, int, int) { return std::hypot(static_cast<float>(dx), static_cast<float>(dy)); };
    edtAgainstBruteForce(detail::Border::Zero, plane);
    // Clamp (Extend): outside the field counts as mask, so only the field's own background counts
    edtAgainstBruteForce(detail::Border::Clamp, plane);
    edtAgainstBruteForce(detail::Border::Wrap, [](int dx, int dy, int w, int h) {
        const int ax = std::min(std::abs(dx), w - std::abs(dx)), ay = std::min(std::abs(dy), h - std::abs(dy));
        return std::hypot(static_cast<float>(ax), static_cast<float>(ay));
    });
}

TEST(SpriteNormalsField, GaussianSobelComponentsOklab) {
    detail::Field f = detail::Field::zero(9, 7);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 9; x++) f.data[static_cast<size_t>(y * 9 + x)] = static_cast<float>(x) * 2.0f;
    // identity below sigma 0.05
    EXPECT_EQ(detail::gaussian(f, 0.04f).data, f.data);
    // a constant field stays constant under Wrap and Clamp; Zero darkens its border
    detail::Field c = detail::Field::zero(9, 7);
    std::fill(c.data.begin(), c.data.end(), 3.0f);
    for (detail::Border b : {detail::Border::Wrap, detail::Border::Clamp}) {
        for (float v : detail::gaussian(c, 2.0f, b).data) EXPECT_NEAR(v, 3.0f, 1e-5f);
    }
    EXPECT_LT(detail::gaussian(c, 2.0f).data[0], 2.0f);
    // the ramp rises 2 per pixel to the right: dx = 2, dy = 0 (interior, and Clamp at the border)
    float dx, dy;
    detail::sobel(f, 4, 3, dx, dy);
    EXPECT_FLOAT_EQ(dx, 2.0f);
    EXPECT_FLOAT_EQ(dy, 0.0f);
    detail::sobel(f, 0, 0, dx, dy, detail::Border::Clamp);
    EXPECT_FLOAT_EQ(dx, 1.0f);   // (2 - 0) / 2: clamp-to-edge halves the border slope
    detail::sobel(f, 0, 3, dx, dy, detail::Border::Wrap);
    EXPECT_FLOAT_EQ(dx, (2.0f - 16.0f) * 0.5f);
    // masked: hidden values never leak
    detail::Field w = detail::Field::zero(9, 7), v = detail::Field::zero(9, 7);
    for (size_t i = 0; i < v.data.size(); i++) {
        w.data[i] = i % 2 ? 1.0f : 0.0f;
        v.data[i] = i % 2 ? 5.0f : 1000.0f;
    }
    for (float m : detail::maskedGaussian(v, w, 1.5f).data) EXPECT_NEAR(m, 5.0f, 1e-4f);
    // 8-connected components in upstream's order
    const std::vector<uint8_t> mask = {1, 0, 0, 1,
                                       0, 1, 0, 1,
                                       0, 0, 0, 0,
                                       1, 1, 0, 1};
    const std::vector<detail::Component> parts = detail::components(mask, 4, 4);
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0].pixels, (std::vector<int>{0, 5}));
    EXPECT_EQ(parts[1].pixels, (std::vector<int>{3, 7}));
    EXPECT_EQ(parts[2].pixels, (std::vector<int>{12, 13}));
    EXPECT_EQ(parts[3].pixels, (std::vector<int>{15}));
    EXPECT_EQ(parts[0].maxX, 1);
    EXPECT_EQ(parts[0].maxY, 1);
    // Oklab
    EXPECT_NEAR(detail::oklabL(255, 255, 255), 1.0f, 1e-4f);
    EXPECT_EQ(detail::oklabL(0, 0, 0), 0.0f);
    float L, a, b;
    detail::oklab(255, 0, 0, L, a, b);
    EXPECT_NEAR(L, 0.628f, 1e-3f);
    EXPECT_NEAR(a, 0.2249f, 1e-3f);
    EXPECT_NEAR(b, 0.1258f, 1e-3f);
    EXPECT_FLOAT_EQ(detail::srgbToLinearTable()[255], 1.0f);
    uint8_t n[3];
    detail::encodeNormal(0.0f, 0.0f, 1.5f, false, n);
    EXPECT_EQ(n[0], 128);
    EXPECT_EQ(n[1], 128);
    EXPECT_EQ(n[2], 255);
    detail::encodeNormal(1.0f, 1.0f, 1.0f, false, n);   // rises right and down: faces left and up
    EXPECT_LT(n[0], 128);
    EXPECT_GT(n[1], 128);
    detail::encodeNormal(1.0f, 1.0f, 1.0f, true, n);
    EXPECT_LT(n[1], 128);
}

// ---------------------------------------------------------------- settings

TEST(SpriteNormalsSettings, PresetsNamesAndColours) {
    const Settings d = presetSettings(Preset::Detailed);
    EXPECT_EQ(d.preset, Preset::Detailed);
    EXPECT_EQ(d.strength, 1.5f);
    EXPECT_EQ(d.detail, 6.0f);
    EXPECT_EQ(d.smoothing, 0.6f);
    EXPECT_EQ(d.volume, 1.15f);
    EXPECT_EQ(d.softness, 1.15f);
    EXPECT_FALSE(d.radius.has_value());
    const Settings c = presetSettings(Preset::Chunky);
    EXPECT_EQ(c.strength, 2.5f);
    EXPECT_EQ(c.volume, 2.0f);
    const Settings s = presetSettings(Preset::Soft);
    EXPECT_EQ(s.strength, 1.0f);
    EXPECT_EQ(s.detail, 2.0f);
    EXPECT_EQ(presetSettings(Preset::Crisp).smoothing, 0.6f);
    const Settings def = presetSettings(Preset::Default);
    EXPECT_EQ(def.detail, 3.0f);
    EXPECT_EQ(def.smoothing, 1.0f);
    EXPECT_EQ(def.alphaThreshold, 0.03f);
    // Settings{} is the default preset
    EXPECT_EQ(Settings().strength, def.strength);
    EXPECT_EQ(Settings().detail, def.detail);
    for (Preset p : {Preset::Soft, Preset::Default, Preset::Crisp, Preset::Detailed, Preset::Chunky, Preset::Toon}) {
        EXPECT_EQ(presetSettings(p).quantize.on(), p == Preset::Toon) << toString(p) << ": only toon quantizes";
        Preset back;
        EXPECT_TRUE(parsePreset(toString(p), back));
        EXPECT_EQ(back, p);
        EXPECT_TRUE(validateSettings(presetSettings(p)).empty());
    }
    Profile profile;
    EXPECT_TRUE(parseProfile("dome", profile));
    EXPECT_EQ(profile, Profile::Dome);
    EXPECT_STREQ(toString(Profile::Bevel), "bevel");
    Edges edges;
    EXPECT_TRUE(parseEdges("WRAP", edges));
    EXPECT_EQ(edges, Edges::Wrap);
    EXPECT_STREQ(toString(Edges::Extend), "extend");
    EXPECT_FALSE(parseEdges("mirror", edges));
    Preset preset;
    EXPECT_FALSE(parsePreset("sharp", preset));
    uint32_t rgb = 0;
    EXPECT_TRUE(parseColor("#C0ffEE", rgb));
    EXPECT_EQ(rgb, 0xc0ffeeu);
    EXPECT_TRUE(parseColor("1a1c2c", rgb));
    EXPECT_EQ(rgb, 0x1a1c2cu);
    EXPECT_FALSE(parseColor("#12345", rgb));
    EXPECT_FALSE(parseColor("#12345g", rgb));
    EXPECT_EQ(colorText(0x00ff0a), "#00ff0a");
    EXPECT_STREQ(toString(Status::InvalidRequest), "invalid_request");
}

TEST(SpriteNormalsSettings, ValidationNamesEverySettingAndRange) {
    Settings s;
    s.strength = 9.0f;
    s.detail = -1.0f;
    s.smoothing = 32.5f;
    s.volume = std::numeric_limits<float>::quiet_NaN();
    s.radius = 0.25f;
    s.softness = 40.0f;
    s.alphaThreshold = 1.0f;
    Region bad;
    bad.colors = {};
    bad.tolerance = 0.6f;
    bad.detail = 9.0f;
    bad.lift = -17.0f;
    s.regions = {Region{{0xffffff}}, bad};
    s.regions[0].colors.push_back(0x1000000);
    const std::vector<Problem> problems = validateSettings(s);
    std::vector<std::string> fields;
    for (const Problem& p : problems) {
        EXPECT_EQ(p.kind, "invalid_settings");
        EXPECT_FALSE(p.fix.empty());
        fields.push_back(p.field);
    }
    EXPECT_EQ(fields, (std::vector<std::string>{"strength", "detail", "smoothing", "volume", "radius", "softness", "alpha_threshold",
                                                "regions[0].colors[1]", "regions[1].colors", "regions[1].tolerance", "regions[1].detail",
                                                "regions[1].lift"}));
    EXPECT_EQ(problems[0].message, "strength must be between 0 and 8 (got 9)");
    EXPECT_EQ(problems[3].message, "volume must be a number between 0 and 8 (got nan)");
    EXPECT_EQ(problems[4].message, "radius must be between 0.5 and 512 (got 0.25)");
    EXPECT_EQ(problems[6].message, "alpha_threshold must be between 0 and 0.99 (got 1)");
    EXPECT_EQ(problems[2].message, "smoothing must be between 0 and 32 (got 32.5)");
    EXPECT_EQ(problems[5].message, "softness must be between 0 and 32 (got 40)");
    Settings many;
    many.regions.assign(17, Region{{0x000000}});
    ASSERT_EQ(validateSettings(many).size(), 1u);
    EXPECT_EQ(validateSettings(many)[0].field, "regions");
    Settings colours;
    colours.regions.push_back(Region{});
    colours.regions[0].colors.assign(17, 0x102030);
    EXPECT_EQ(validateSettings(colours).at(0).field, "regions[0].colors");
    // the limits themselves are valid
    Settings version;
    version.version = 3;
    ASSERT_EQ(validateSettings(version).size(), 1u);
    EXPECT_EQ(validateSettings(version)[0].field, "version");
    EXPECT_EQ(validateSettings(version)[0].message, "version must be 1 (as before high-resolution support) or 2 (got 3)");
    Settings edge;
    edge.strength = 8.0f;
    edge.smoothing = 32.0f;
    edge.softness = 32.0f;
    edge.detail = 8.0f;
    edge.version = 1;
    edge.radius = 512.0f;
    edge.alphaThreshold = 0.99f;
    edge.regions.push_back(Region{{0x000000}, 0.5f, 8.0f, -16.0f, true});
    EXPECT_TRUE(validateSettings(edge).empty());
}

// ---------------------------------------------------------------- grid

TEST(SpriteNormalsGrid, SliceGridLikeSetSpriteFrames) {
    std::vector<Cell> cells;
    std::string error;
    Grid g;
    g.tileWidth = 32;
    g.tileHeight = 32;
    ASSERT_TRUE(sliceGrid(g, 128, 64, cells, error)) << error;
    ASSERT_EQ(cells.size(), 8u);
    EXPECT_EQ(cells[5], (Cell{32, 32, 32, 32}));
    // offset and padding: columns = floor((avail + padding) / (tile + padding))
    g.offsetX = 2;
    g.offsetY = 1;
    g.paddingX = 2;
    g.paddingY = 3;
    ASSERT_TRUE(sliceGrid(g, 104, 70, cells, error));
    ASSERT_EQ(cells.size(), 6u);   // (102 + 2) / 34 = 3 columns, (69 + 3) / 35 = 2 rows
    EXPECT_EQ(cells[4], (Cell{36, 36, 32, 32}));
    // columns and rows: tile = floor((avail - padding (n - 1)) / n), the last ones clipped
    Grid cr;
    cr.columns = 3;
    cr.rows = 2;
    ASSERT_TRUE(sliceGrid(cr, 100, 50, cells, error));
    ASSERT_EQ(cells.size(), 6u);
    EXPECT_EQ(cells[2], (Cell{66, 0, 33, 25}));
    Grid clip;
    clip.tileWidth = 40;
    clip.tileHeight = 40;
    clip.columns = 2;
    clip.rows = 1;
    ASSERT_TRUE(sliceGrid(clip, 70, 40, cells, error));
    EXPECT_EQ(cells[1], (Cell{40, 0, 30, 40}));
    // both given: the tile size is used, columns / rows cap the counts (sliceSheet)
    Grid both;
    both.tileWidth = 32;
    both.tileHeight = 32;
    both.columns = 2;
    ASSERT_TRUE(sliceGrid(both, 128, 64, cells, error));
    ASSERT_EQ(cells.size(), 4u);   // 2 columns (capped from 4) x 2 rows (from the tile size)
    EXPECT_EQ(cells[3], (Cell{32, 32, 32, 32}));
    both.rows = 1;
    ASSERT_TRUE(sliceGrid(both, 128, 64, cells, error));
    EXPECT_EQ(cells.size(), 2u);
    // huge tile sizes, paddings and offsets never overflow
    Grid huge;
    huge.tileWidth = 2000000000;
    huge.tileHeight = 2000000000;
    EXPECT_FALSE(sliceGrid(huge, 64, 64, cells, error));
    huge.tileWidth = huge.tileHeight = 16;
    huge.columns = 3;
    huge.paddingX = 2000000000;
    huge.paddingY = 2000000000;
    ASSERT_TRUE(sliceGrid(huge, 64, 64, cells, error)) << error;
    EXPECT_EQ(cells, (std::vector<Cell>{{0, 0, 16, 16}}));   // the next columns start beyond the image
    Grid far;
    far.columns = 2;
    far.rows = 2;
    far.offsetX = 2000000000;
    EXPECT_FALSE(sliceGrid(far, 64, 64, cells, error));
    Grid manyRows;
    manyRows.tileWidth = 1;
    manyRows.tileHeight = 1;
    manyRows.paddingX = 0;
    manyRows.count = 2;
    ASSERT_TRUE(sliceGrid(manyRows, 2000000000, 2000000000, cells, error)) << error;   // count caps before anything is made
    EXPECT_EQ(cells.size(), 2u);
    // 0 / 0 (tile 4, padding -4 on a 4 px image): no NaN cast, the negative padding is refused
    Grid nan;
    nan.tileWidth = 4;
    nan.tileHeight = 4;
    nan.paddingX = -4;
    nan.paddingY = -4;
    EXPECT_FALSE(sliceGrid(nan, 4, 4, cells, error));
    EXPECT_EQ(error, "The grid is empty: the image is 4x4 pixels.");
    // count caps
    g = Grid();
    g.tileWidth = 16;
    g.tileHeight = 16;
    g.count = 3;
    ASSERT_TRUE(sliceGrid(g, 64, 64, cells, error));
    EXPECT_EQ(cells.size(), 3u);
    // empty grids
    g.count = 0;
    g.tileWidth = 40;
    g.tileHeight = 40;
    EXPECT_FALSE(sliceGrid(g, 30, 30, cells, error));
    EXPECT_EQ(error, "The grid is empty: the image is 30x30 pixels.");
    EXPECT_FALSE(sliceGrid(Grid(), 30, 30, cells, error));
    Grid tiny;
    tiny.tileWidth = 1;
    tiny.tileHeight = 1;
    EXPECT_FALSE(sliceGrid(tiny, 100, 100, cells, error));   // 10000 > kMaxCells
    EXPECT_NE(error.find("4096"), std::string::npos);
}

TEST(SpriteNormalsGrid, DuplicateCellsAreProcessedOnce) {
    const Image color = loadPng(kFixtures / "inputs" / "sheet.png");
    std::vector<Cell> cells = gridCells(128, 32, 32, 32);
    const Result once = run(color, upstreamDefaults(), cells);
    cells.push_back(cells[1]);
    cells.insert(cells.begin(), cells[2]);
    const Result twice = run(color, upstreamDefaults(), cells);
    EXPECT_EQ(once.normal.data, twice.normal.data);
    EXPECT_EQ(once.islands, twice.islands);
    // pixels outside every cell stay neutral
    const Result partial = run(color, upstreamDefaults(), {{32, 0, 32, 32}});
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 128; x++) {
            if (x >= 32 && x < 64) continue;
            ASSERT_EQ(partial.normal.pixel(x, y)[0], 128);
            ASSERT_EQ(partial.normal.pixel(x, y)[2], 255);
        }
    }
    EXPECT_EQ(crop(partial.normal, 32, 0, 32, 32).data, crop(once.normal, 32, 0, 32, 32).data);
}

// ---------------------------------------------------------------- options

namespace {
    // The heights of a uniform 40x40 square (centred on 64x64) along its middle row: the height map
    // with softness 0 and no detail is the profile itself
    std::vector<float> profileRow(Profile profile, float volume, float radius) {
        Image color = Image::make(64, 64, 4, 8);
        paste(color, solid(40, 40, 120, 90, 60, 255), 12, 12);
        Settings s = presetSettings(Preset::Default);
        s.profile = profile;
        s.volume = volume;
        s.radius = radius;
        s.softness = 0.0f;
        s.detail = 0.0f;
        s.strength = 1.0f;
        s.height = true;
        const Result r = run(color, s);
        std::vector<float> row;
        const uint16_t* h = reinterpret_cast<const uint16_t*>(r.height.data.data());
        for (int x = 12; x < 32; x++) {
            row.push_back(r.heightMin + static_cast<float>(h[32 * 64 + x]) / 65535.0f * (r.heightMax - r.heightMin));
        }
        return row;
    }

    float tiltDegrees(const uint8_t* p, bool directx = false) {
        float n[3];
        for (int k = 0; k < 3; k++) n[k] = static_cast<float>(p[k]) / 127.5f - 1.0f;
        if (directx) n[1] = -n[1];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return std::acos(std::clamp(n[2] / len, -1.0f, 1.0f)) * 57.29578f;
    }

    // A seamless 32x32 tile: periodic shading and two transparent holes, one crossing the wrap edge
    Image seamlessTile() {
        return fromFunction(32, 32, [](int x, int y) {
            const float s = 0.5f + 0.25f * std::sin(x * 0.19634954f * 2.0f) + 0.2f * std::cos(y * 0.19634954f * 3.0f);
            const uint8_t v = static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 220.0f + 20.0f);
            const auto hole = [&](int cx, int cy, int r) {
                const int dx = std::min(std::abs(x - cx), 32 - std::abs(x - cx)), dy = std::min(std::abs(y - cy), 32 - std::abs(y - cy));
                return dx * dx + dy * dy <= r * r;
            };
            const bool transparent = hole(10, 12, 4) || hole(0, 28, 5);
            return std::array<uint8_t, 4>{v, static_cast<uint8_t>(v * 3 / 4), 80, static_cast<uint8_t>(transparent ? 0 : 255)};
        });
    }

    Image shift(const Image& image, int sx, int sy) {
        Image out = Image::make(image.width, image.height, 4, 8);
        for (int y = 0; y < image.height; y++)
            for (int x = 0; x < image.width; x++)
                std::memcpy(out.pixel((x + sx) % image.width, (y + sy) % image.height), image.pixel(x, y), 4);
        return out;
    }
}

TEST(SpriteNormalsOptions, ProfileHeightsAtKnownInsets) {
    // the edge pixel is 1 px from the background: inset 0.5, then 1.5, 2.5, ...
    const std::vector<float> bevel = profileRow(Profile::Bevel, 1.0f, 4.0f);
    for (int i = 0; i < 20; i++) EXPECT_NEAR(bevel[static_cast<size_t>(i)], std::min(i + 0.5f, 4.0f), 2e-3f) << i;
    const std::vector<float> dome = profileRow(Profile::Dome, 2.0f, 6.0f);
    for (int i = 0; i < 20; i++) {
        const float t = i + 0.5f;
        const float expected = t >= 6.0f ? 12.0f : 2.0f * std::sqrt(36.0f - (6.0f - t) * (6.0f - t));
        EXPECT_NEAR(dome[static_cast<size_t>(i)], expected, 3e-3f) << i;
    }
    const std::vector<float> rounded = profileRow(Profile::Rounded, 1.15f, 4.0f);
    for (int i = 0; i < 20; i++) EXPECT_NEAR(rounded[static_cast<size_t>(i)], 1.15f * 4.0f * std::log1p((i + 0.5f) / 4.0f), 3e-3f) << i;
}

TEST(SpriteNormalsOptions, BevelAndDomeHaveFlatCentresRoundedKeepsRising) {
    const Image color = solid(64, 64, 120, 90, 60, 255);
    for (Profile p : {Profile::Bevel, Profile::Dome, Profile::Rounded}) {
        Settings s = presetSettings(Preset::Default);
        s.profile = p;
        s.radius = 6.0f;
        const Result r = run(color, s);
        const uint8_t* centre = r.normal.pixel(20, 32);
        const uint8_t* edge = r.normal.pixel(1, 32);
        EXPECT_LT(edge[0], 110) << toString(p);   // every profile bevels the border
        if (p == Profile::Rounded) {
            EXPECT_LT(centre[0], 128) << "rounded keeps rising 20 px in";
        } else {
            EXPECT_EQ(centre[0], 128) << toString(p);
            EXPECT_EQ(centre[1], 128) << toString(p);
            EXPECT_EQ(centre[2], 255) << toString(p);
        }
    }
}

TEST(SpriteNormalsOptions, WrapIsSeamless) {
    const Image tile = seamlessTile();
    Settings s = presetSettings(Preset::Default);
    s.edges = Edges::Wrap;
    s.radius = 4.0f;
    const Result a = run(tile, s);
    const Result b = run(shift(tile, 16, 16), s);
    EXPECT_EQ(a.islands, 1);
    // the map of the shifted tile is the shifted map: the wrap edge is like any other pixel, so
    // a 3x3 tiling of the map has no seam
    EXPECT_EQ(shift(a.normal, 16, 16).data, b.normal.data);
    // Transparent edges would bevel the tile border instead
    s.edges = Edges::Transparent;
    const Result t = run(tile, s);
    EXPECT_NE(shift(t.normal, 16, 16).data, run(shift(tile, 16, 16), s).normal.data);
}

TEST(SpriteNormalsOptions, ExtendHasNoBorderBevel) {
    const Image tile = fromFunction(48, 48, [](int x, int y) {
        const uint8_t v = static_cast<uint8_t>(110 + 40 * std::sin(x * 0.3f) * std::cos(y * 0.23f));
        return std::array<uint8_t, 4>{v, v, static_cast<uint8_t>(v / 2), 255};
    });
    Settings s = presetSettings(Preset::Default);
    s.edges = Edges::Extend;
    const Result r = run(tile, s);
    EXPECT_FALSE(r.radiusPerIsland && r.islands != 1);
    float interior = 0.0f, border = 0.0f;
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            const float t = tiltDegrees(r.normal.pixel(x, y));
            const bool nearBorder = x < 2 || y < 2 || x >= 46 || y >= 46;
            (nearBorder ? border : interior) = std::max(nearBorder ? border : interior, t);
        }
    }
    EXPECT_LE(border, interior + 0.5f);
    // Transparent: the border bevel is much steeper than the shading
    s.edges = Edges::Transparent;
    const Result bevel = run(tile, s);
    EXPECT_GT(tiltDegrees(bevel.normal.pixel(0, 24)), interior + 10.0f);
}

TEST(SpriteNormalsOptions, OpaqueCellsWithWrapOrExtendGetNoShape) {
    // no transparent pixel: the distance is infinite, the shape a constant 0, flat colour is flat
    for (Edges e : {Edges::Wrap, Edges::Extend}) {
        Settings s = presetSettings(Preset::Chunky);
        s.edges = e;
        const Result r = run(solid(16, 16, 100, 150, 200, 255), s);
        for (size_t i = 0; i < 256; i++) {
            ASSERT_EQ(r.normal.data[4 * i], 128) << toString(e);
            ASSERT_EQ(r.normal.data[4 * i + 2], 255) << toString(e);
        }
    }
}

TEST(SpriteNormalsOptions, AutoRadius) {
    // version 1 (maps made before high-resolution support): 0.15 x the island's shorter side.
    // A ball filling 32x32: one island, 0.15 x 32 = 4.8
    const Image ball = fromFunction(32, 32, [](int x, int y) {
        const bool in = std::hypot(x + 0.5f - 16.0f, y + 0.5f - 16.0f) <= 16.0f;
        return std::array<uint8_t, 4>{200, 80, 60, static_cast<uint8_t>(in ? 255 : 0)};
    });
    Settings s = presetSettings(Preset::Default);
    s.version = 1;
    Result r = run(ball, s);
    EXPECT_TRUE(r.radiusPerIsland);
    EXPECT_FLOAT_EQ(r.radiusMin, 4.8f);
    EXPECT_FLOAT_EQ(r.radiusMax, 4.8f);
    // a 200x40 bar: 0.15 x 40 = 6; a 10x10 island beside it: clamped to 4
    Image bars = Image::make(220, 40, 4, 8);
    paste(bars, solid(200, 40, 90, 90, 90, 255), 0, 0);
    paste(bars, solid(10, 10, 90, 90, 90, 255), 205, 5);
    r = run(bars, s);
    EXPECT_EQ(r.islands, 2);
    EXPECT_FLOAT_EQ(r.radiusMin, 4.0f);
    EXPECT_FLOAT_EQ(r.radiusMax, 6.0f);
    // per island: the bar's normals are those of the bar alone with radius 6
    Settings six = s;
    six.radius = 6.0f;
    EXPECT_EQ(crop(r.normal, 0, 0, 200, 40).data, run(solid(200, 40, 90, 90, 90, 255), six).normal.data);
    // a sequence: one value, from the median of the largest islands' shorter sides (40, 60, 100 -> 60 -> 9)
    Image strip = Image::make(300, 100, 4, 8);
    paste(strip, solid(40, 80, 90, 90, 90, 255), 0, 0);
    paste(strip, solid(5, 5, 90, 90, 90, 255), 50, 90);   // smaller island: ignored
    paste(strip, solid(60, 60, 90, 90, 90, 255), 100, 0);
    paste(strip, solid(100, 100, 90, 90, 90, 255), 200, 0);
    const std::vector<Cell> cells = {{0, 0, 100, 100}, {100, 0, 100, 100}, {200, 0, 100, 100}};
    r = run(strip, s, cells, 1, true);
    EXPECT_FALSE(r.radiusPerIsland);
    EXPECT_FLOAT_EQ(r.radiusMin, 9.0f);
    EXPECT_FLOAT_EQ(r.radiusMax, 9.0f);
    // even count: the mean of the two middle values ((40 + 60) / 2 x 0.15 = 7.5)
    r = run(strip, s, {cells[0], cells[1]}, 1, true);
    EXPECT_FLOAT_EQ(r.radiusMin, 7.5f);
    // nothing anywhere: 4
    r = run(Image::make(8, 8, 4, 8), s, {{0, 0, 4, 4}, {4, 4, 4, 4}}, 1, true);
    EXPECT_FLOAT_EQ(r.radiusMin, 4.0f);
    // explicit: as is
    s.radius = 2.5f;
    r = run(ball, s);
    EXPECT_FALSE(r.radiusPerIsland);
    EXPECT_FLOAT_EQ(r.radiusMax, 2.5f);
    // Wrap / Extend on a still: the cell's own size
    s.radius.reset();
    s.edges = Edges::Wrap;
    r = run(seamlessTile(), s);
    EXPECT_FLOAT_EQ(r.radiusMin, 4.8f);
}

TEST(SpriteNormalsOptions, IgnoreOutlineFindsTheOutline) {
    Settings s = presetSettings(Preset::Default);
    s.ignoreOutline = true;
    for (const char* name : {"character.png", "flat_outline.png", "ball.png"}) {
        const Result r = run(loadPng(kFixtures / "inputs" / name), s);
        ASSERT_TRUE(r.outlineColor.has_value()) << name;
        EXPECT_EQ(colorText(*r.outlineColor), "#1a1020") << name;   // the evaluation sprites' outline (26, 16, 32)
        EXPECT_TRUE(r.warnings.empty()) << name;
    }
    // a sprite without an outline (the disk: one mid-tone colour, Oklab L 0.53): a warning
    const Result none = run(disk(), s);
    EXPECT_FALSE(none.outlineColor.has_value());
    ASSERT_EQ(none.warnings.size(), 1u);
    EXPECT_EQ(none.warnings[0].kind, "outline_not_found");
    EXPECT_EQ(none.warnings[0].message, "No outline colour covers half of the silhouette rim; nothing was excluded.");
    // the outline no longer feeds the relief: the map differs from the plain one
    const Image character = loadPng(kFixtures / "inputs" / "character.png");
    Settings plain = presetSettings(Preset::Default);
    EXPECT_NE(run(character, s).normal.data, run(character, plain).normal.data);
}

TEST(SpriteNormalsOptions, RegionDetailZeroCalmsPaintedHighlights) {
    const Image color = loadPng(kFixtures / "inputs" / "highlight.png");
    Settings s = presetSettings(Preset::Detailed);
    const Result plain = run(color, s);
    Region highlight;
    highlight.colors = {0xffffff, 0xffe4c8};   // the painted specular blob
    highlight.detail = 0.0f;
    s.regions.push_back(highlight);
    const Result calmed = run(color, s);
    EXPECT_TRUE(calmed.warnings.empty());
    // the highlight's painted light direction no longer becomes a lean: mean |red - 128| and
    // |green - 128| over the highlight pixels drop
    double redBefore = 0.0, redAfter = 0.0, greenBefore = 0.0, greenAfter = 0.0;
    int count = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const uint8_t* p = color.pixel(x, y);
            if (!((p[0] == 255 && p[1] == 255 && p[2] == 255) || (p[0] == 255 && p[1] == 228 && p[2] == 200))) continue;
            redBefore += std::abs(int(plain.normal.pixel(x, y)[0]) - 128);
            redAfter += std::abs(int(calmed.normal.pixel(x, y)[0]) - 128);
            greenBefore += std::abs(int(plain.normal.pixel(x, y)[1]) - 128);
            greenAfter += std::abs(int(calmed.normal.pixel(x, y)[1]) - 128);
            count++;
        }
    }
    ASSERT_GT(count, 4);
    std::printf("[ regions  ] highlight.png, %d highlight pixels: mean |red - 128| %.1f -> %.1f, |green - 128| %.1f -> %.1f\n", count,
                redBefore / count, redAfter / count, greenBefore / count, greenAfter / count);
    EXPECT_LT(redAfter, redBefore);
    EXPECT_LT(greenAfter, greenBefore);
}

TEST(SpriteNormalsOptions, RegionLiftFlatAndUnmatched) {
    // a 32x32 square with an 8x8 patch of another colour in the middle
    Image color = solid(32, 32, 100, 100, 100, 255);
    paste(color, solid(8, 8, 200, 40, 40, 255), 12, 12);
    Settings s = presetSettings(Preset::Default);
    s.detail = 0.0f;
    s.volume = 0.0f;
    Region lift;
    lift.colors = {0xc82828};
    lift.lift = 4.0f;
    s.regions = {lift};
    Result r = run(color, s);
    EXPECT_LT(r.normal.pixel(12, 16)[0], 118);   // left border of the lifted patch faces left
    EXPECT_GT(r.normal.pixel(19, 16)[0], 138);   // right border faces right
    EXPECT_GT(r.normal.pixel(16, 12)[1], 138);   // top border faces up (OpenGL)
    EXPECT_EQ(r.normal.pixel(4, 4)[0], 128);     // far from it: flat
    s.regions[0].lift = -4.0f;
    r = run(color, s);
    EXPECT_GT(r.normal.pixel(12, 16)[0], 138);   // sunk: the slope reverses
    // flat: exactly +Z on the region, whatever the rest does
    Settings f = presetSettings(Preset::Chunky);
    Region flat;
    flat.colors = {0xc82828};
    flat.flat = true;
    f.regions = {flat};
    r = run(color, f);
    for (int y = 12; y < 20; y++) {
        for (int x = 12; x < 20; x++) {
            const uint8_t* p = r.normal.pixel(x, y);
            ASSERT_EQ(p[0], 128);
            ASSERT_EQ(p[1], 128);
            ASSERT_EQ(p[2], 255);
            ASSERT_EQ(p[3], 255);
        }
    }
    EXPECT_NE(r.normal.pixel(1, 16)[0], 128);
    // a region matching nothing warns, naming its index, colour and tolerance
    Region nothing;
    nothing.colors = {0xc0ffee};
    f.regions.push_back(nothing);
    r = run(color, f);
    ASSERT_EQ(r.warnings.size(), 1u);
    EXPECT_EQ(r.warnings[0].kind, "region_unmatched");
    EXPECT_EQ(r.warnings[0].field, "regions[1]");
    EXPECT_EQ(r.warnings[0].message, "regions[1] (#c0ffee) matched no pixel; widen its tolerance (0.03).");
    // the first matching region wins
    Region wide;
    wide.colors = {0xc82828};
    wide.tolerance = 0.5f;
    f.regions = {wide, flat};
    r = run(color, f);
    ASSERT_EQ(r.warnings.size(), 1u);
    EXPECT_EQ(r.warnings[0].field, "regions[1]");
}

TEST(SpriteNormalsOptions, HeightMapRangeAndMonotonicity) {
    Settings s = presetSettings(Preset::Default);
    s.height = true;
    s.detail = 0.0f;
    const Result r = run(disk(), s);
    ASSERT_EQ(r.height.channels, 1);
    ASSERT_EQ(r.height.bitDepth, 16);
    ASSERT_EQ(r.height.width, 31);
    EXPECT_LT(r.heightMin, r.heightMax);
    const uint16_t* h = reinterpret_cast<const uint16_t*>(r.height.data.data());
    EXPECT_EQ(h[15 * 31 + 15], 65535);   // the centre is the highest
    EXPECT_EQ(h[0], 0);                  // outside the disk
    for (int x = 4; x < 15; x++) EXPECT_LT(h[15 * 31 + x], h[15 * 31 + x + 1]) << x;
    // the range is strength x combined: doubling the strength doubles it, same 16-bit values
    s.strength = 3.0f;
    const Result twice = run(disk(), s);
    EXPECT_NEAR(twice.heightMax, 2.0f * r.heightMax, 1e-4f);
    EXPECT_EQ(twice.height.data, r.height.data);
    // directx changes nothing; a flat colour sprite with no volume is one level: 32768
    s.directx = true;
    EXPECT_EQ(run(disk(), s).height.data, twice.height.data);
    s.volume = 0.0f;
    const Result level = run(disk(), s);
    EXPECT_EQ(level.heightMin, level.heightMax);
    EXPECT_EQ(reinterpret_cast<const uint16_t*>(level.height.data.data())[15 * 31 + 15], 32768);
    // no height map unless asked
    EXPECT_TRUE(run(disk(), presetSettings(Preset::Default)).height.empty());
}

TEST(SpriteNormalsOptions, CancelProgressAndThreads) {
    const Image color = loadPng(kFixtures / "inputs" / "sheet.png");
    std::atomic<bool> cancel{true};
    Control control;
    control.cancel = &cancel;
    Request request;
    request.color = color;
    request.cells = gridCells(128, 32, 32, 32);
    Result r = generate(request, control);
    EXPECT_EQ(r.status, Status::Cancelled);
    EXPECT_EQ(r.errors.at(0).kind, "cancelled");
    EXPECT_TRUE(r.normal.empty());
    cancel = false;
    std::vector<double> fractions;
    control.onProgress = [&](const Progress& p) { fractions.push_back(p.fraction); };
    control.progressIntervalSeconds = 0.0;
    request.threads = 1;
    r = generate(request, control);
    ASSERT_EQ(r.status, Status::Ok);
    ASSERT_EQ(fractions.size(), static_cast<size_t>(r.islands));
    EXPECT_EQ(fractions.back(), 1.0);
    EXPECT_TRUE(std::is_sorted(fractions.begin(), fractions.end()));
    EXPECT_EQ(r.threads, 1);
    // the thread count is capped by the work items
    request.threads = 64;
    EXPECT_EQ(generate(request).threads, std::min(64, r.islands));
    request.threads = 0;
    EXPECT_GE(generate(request).threads, 1);
    // cancelled from the progress callback: the run stops after the items running
    control.onProgress = [&](const Progress&) { cancel = true; };
    request.threads = 1;
    EXPECT_EQ(generate(request, control).status, Status::Cancelled);
}

TEST(SpriteNormalsPerformance, Opaque512UnderBudget) {
#ifdef NDEBUG
    const Image blob = fromFunction(512, 512, [](int x, int y) {
        const bool in = std::hypot(x - 255.5f, y - 255.5f) <= 250.0f;
        const uint8_t v = static_cast<uint8_t>(60 + (x * 7 + y * 3) % 120);
        return std::array<uint8_t, 4>{v, static_cast<uint8_t>(v / 2), 90, static_cast<uint8_t>(in ? 255 : 0)};
    });
    run(blob, presetSettings(Preset::Default));   // warm up
    double best = 1e9;
    for (int i = 0; i < 3; i++) best = std::min(best, run(blob, presetSettings(Preset::Default)).seconds);
    std::printf("[ perf     ] 512x512 blob, 1 thread: %.1f ms\n", best * 1000.0);
    EXPECT_LT(best, 0.25);
#else
    GTEST_SKIP() << "Release builds only";
#endif
}

// ---------------------------------------------------------------- quantization ("Toon steps")

namespace {

    constexpr double kPi = 3.14159265358979323846;

    Quantize quantizeOf(int directions, int tilts, float flatDeg, float rotationDeg) {
        Quantize q;
        q.directions = directions;
        q.tilts = tilts;
        q.flatDeg = flatDeg;
        q.rotationDeg = rotationDeg;
        return q;
    }

    std::array<double, 3> fromAngles(double azimuthDeg, double tiltDeg) {
        const double a = azimuthDeg * kPi / 180.0, t = tiltDeg * kPi / 180.0;
        return {std::sin(t) * std::cos(a), std::sin(t) * std::sin(a), std::cos(t)};
    }

    // azimuth in [0, 360) and tilt of a quantized direction
    void anglesOfDirection(const std::array<double, 3>& n, double& azimuth, double& tilt) {
        azimuth = std::atan2(n[1], n[0]) * 180.0 / kPi;
        if (azimuth < 0.0) azimuth += 360.0;
        if (azimuth >= 360.0 - 1e-9) azimuth = 0.0;
        tilt = std::acos(std::min(std::max(n[2], -1.0), 1.0)) * 180.0 / kPi;
    }

    std::array<double, 3> snap(const Quantize& q, double azimuthDeg, double tiltDeg) {
        const std::array<double, 3> n = fromAngles(azimuthDeg, tiltDeg);
        return quantizeNormal(q, n[0], n[1], n[2]);
    }

    void expectAngles(const std::array<double, 3>& n, double azimuth, double tilt) {
        double a, t;
        anglesOfDirection(n, a, t);
        EXPECT_NEAR(t, tilt, 1e-9);
        if (tilt <= 0.0) return;   // flat: no azimuth
        double off = std::fmod(a - azimuth + 720.0, 360.0);   // the angle between them, in (-180, 180]
        if (off > 180.0) off -= 360.0;
        EXPECT_NEAR(off, 0.0, 1e-9) << "azimuth " << a << ", expected " << azimuth;
    }

    // the encoded palette (what generate() writes for each palette direction)
    std::vector<std::array<uint8_t, 3>> encodedPalette(const Quantize& q, bool directx) {
        std::vector<std::array<uint8_t, 3>> bytes;
        for (const std::array<double, 3>& n : quantizePalette(q)) {
            const float v[3] = {static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])};
            std::array<uint8_t, 3> b{};
            detail::encodeUnit(v, directx, b.data());
            bytes.push_back(b);
        }
        return bytes;
    }

    const std::vector<Quantize>& quantizeVariants() {
        static const std::vector<Quantize> list = {quantizeOf(8, 2, 10.0f, 0.0f), quantizeOf(4, 1, 0.0f, 45.0f), quantizeOf(16, 4, 20.0f, 22.5f),
                                                   quantizeOf(6, 3, 5.0f, 10.0f), quantizeOf(12, 2, 45.0f, 359.0f)};
        return list;
    }
}

TEST(SpriteNormalsQuantize, OffIsByteIdenticalOnEveryFixture) {
    for (const Golden& g : goldens()) {
        for (bool directx : {false, true}) {
            SCOPED_TRACE(std::string(g.name) + (directx ? " directx" : " opengl"));
            Image color;
            std::vector<Cell> cells;
            bool sequence;
            ASSERT_TRUE(goldenRequest(g, color, cells, sequence));
            Settings plain = upstreamDefaults();
            if (g.adjust) g.adjust(plain);
            plain.directx = directx;
            // off: whatever the other quantize values say
            Settings off = plain;
            off.quantize = quantizeOf(0, 4, 45.0f, 123.0f);
            const Result a = run(color, plain, cells, 1, sequence);
            const Result b = run(color, off, cells, 4, sequence);
            EXPECT_EQ(a.normal.data, b.normal.data);
            if (!directx && !g.adjust) {
                // and the upstream fixture itself (bit-exact with glibc) is what off writes
                const Image expected = loadPng(kFixtures / "expected" / (std::string(g.name) + "_n.png"));
                const Comparison c = compare(b.normal, expected);
                EXPECT_LE(c.maxRgb, 1);
            }
        }
    }
}

TEST(SpriteNormalsQuantize, EveryOpaquePixelIsAPaletteNormal) {
    for (const Quantize& q : quantizeVariants()) {
        const std::vector<std::array<double, 3>> palette = quantizePalette(q);
        ASSERT_EQ(palette.size(), static_cast<size_t>(1 + q.directions * q.tilts));
        for (bool directx : {false, true}) {
            const std::vector<std::array<uint8_t, 3>> bytes = encodedPalette(q, directx);
            for (const Golden& g : goldens()) {
                SCOPED_TRACE(std::string(g.name) + " directions " + std::to_string(q.directions) + (directx ? " directx" : ""));
                Image color;
                std::vector<Cell> cells;
                bool sequence;
                ASSERT_TRUE(goldenRequest(g, color, cells, sequence));
                Settings s = presetSettings(Preset::Toon);
                s.quantize = q;
                s.directx = directx;
                const Result r = run(color, s, cells, 2, sequence);
                size_t offPalette = 0, opaque = 0;
                std::vector<int> used(bytes.size(), 0);
                for (size_t i = 0; i < r.normal.pixelCount(); i++) {
                    const uint8_t* p = r.normal.data.data() + 4 * i;
                    EXPECT_EQ(p[3], color.data[4 * i + 3]);
                    if (!(static_cast<float>(p[3]) / 255.0f > s.alphaThreshold)) {
                        EXPECT_TRUE(p[0] == 128 && p[1] == 128 && p[2] == 255);
                        continue;
                    }
                    opaque++;
                    bool found = false;
                    for (size_t k = 0; k < bytes.size() && !found; k++) {
                        // decoded within 1 LSB of a palette normal
                        found = std::abs(p[0] - bytes[k][0]) <= 1 && std::abs(p[1] - bytes[k][1]) <= 1 && std::abs(p[2] - bytes[k][2]) <= 1;
                        if (found) used[k]++;
                    }
                    if (!found) offPalette++;
                }
                EXPECT_EQ(offPalette, 0u) << "of " << opaque << " opaque pixels";
                // decoded palette directions are unit vectors and z > 0
                for (const std::array<double, 3>& n : palette) {
                    EXPECT_NEAR(n[0] * n[0] + n[1] * n[1] + n[2] * n[2], 1.0, 1e-12);
                    EXPECT_GT(n[2], 0.0);
                }
            }
        }
    }
}

TEST(SpriteNormalsQuantize, FlatConeBoundaryAndTiltRings) {
    const Quantize q = quantizeOf(8, 2, 10.0f, 0.0f);   // rings [10, 50) and [50, 90): centres 30 and 70
    expectAngles(snap(q, 37.0, 9.9), 0.0, 0.0);
    EXPECT_EQ(snap(q, 37.0, 9.9)[2], 1.0) << "under the flat cone: exactly (0, 0, 1)";
    expectAngles(snap(q, 37.0, 10.1), 45.0, 30.0);
    expectAngles(snap(q, 37.0, 49.9), 45.0, 30.0);
    expectAngles(snap(q, 37.0, 50.1), 45.0, 70.0);
    expectAngles(snap(q, 37.0, 89.99), 45.0, 70.0);
    expectAngles(quantizeNormal(q, 1.0, 0.0, 0.0), 0.0, 70.0);   // t = 90: the last ring
    // flat 0: nothing is flat but a normal without azimuth
    const Quantize none = quantizeOf(4, 1, 0.0f, 0.0f);
    expectAngles(snap(none, 10.0, 0.5), 0.0, 45.0);
    const std::array<double, 3> up = quantizeNormal(none, 0.0, 0.0, 1.0);
    EXPECT_EQ(up[0], 0.0);
    EXPECT_EQ(up[1], 0.0);
    EXPECT_EQ(up[2], 1.0);
    // flat 45, 4 rings: [45, 90) in 11.25 degree rings
    const Quantize wide = quantizeOf(16, 4, 45.0f, 0.0f);
    expectAngles(snap(wide, 0.0, 44.9), 0.0, 0.0);
    expectAngles(snap(wide, 0.0, 45.1), 0.0, 45.0 + 11.25 / 2.0);
    expectAngles(snap(wide, 0.0, 80.0), 0.0, 45.0 + 3.5 * 11.25);
    // off: unchanged
    const std::array<double, 3> raw = fromAngles(33.0, 21.0);
    EXPECT_EQ(quantizeNormal(Quantize(), raw[0], raw[1], raw[2]), raw);
    EXPECT_TRUE(quantizePalette(Quantize()).empty());
}

TEST(SpriteNormalsQuantize, RotationAndWrapAroundAtHalfTurn) {
    const Quantize four = quantizeOf(4, 1, 0.0f, 0.0f);
    expectAngles(snap(four, 30.0, 40.0), 0.0, 45.0);
    expectAngles(snap(four, 100.0, 40.0), 90.0, 45.0);
    const Quantize turned = quantizeOf(4, 1, 0.0f, 45.0f);
    expectAngles(snap(turned, 30.0, 40.0), 45.0, 45.0);
    expectAngles(snap(turned, 100.0, 40.0), 135.0, 45.0);
    expectAngles(snap(turned, 350.0, 40.0), 315.0, 45.0);
    // +-180 (atan2 of y = +0 and -0) snap alike, ties rounding away from the rotation
    const std::array<double, 3> plus = quantizeNormal(turned, -0.5, 0.0, std::sqrt(0.75));
    const std::array<double, 3> minus = quantizeNormal(turned, -0.5, -0.0, std::sqrt(0.75));
    EXPECT_EQ(plus, minus);
    expectAngles(plus, 225.0, 45.0);
    // either side of the half turn and of 0 / 360
    const Quantize eight = quantizeOf(8, 2, 10.0f, 0.0f);
    expectAngles(snap(eight, 179.0, 40.0), 180.0, 30.0);
    expectAngles(snap(eight, -179.0, 40.0), 180.0, 30.0);
    expectAngles(snap(eight, 350.0, 40.0), 0.0, 30.0);
    expectAngles(snap(eight, -10.0, 40.0), 0.0, 30.0);
    expectAngles(snap(eight, 21.0, 40.0), 0.0, 30.0);
    expectAngles(snap(eight, 24.0, 40.0), 45.0, 30.0);
    const Quantize near360 = quantizeOf(12, 1, 0.0f, 359.0f);
    expectAngles(snap(near360, 0.0, 60.0), 359.0, 45.0);
    expectAngles(snap(near360, 10.0, 60.0), 359.0, 45.0);   // 11 past 359, 19 short of 29
    expectAngles(snap(near360, 20.0, 60.0), 29.0, 45.0);
    // 360 is exactly 0, for direct Settings callers too: the same palette and the same bytes
    EXPECT_EQ(quantizePalette(quantizeOf(8, 2, 10.0f, 360.0f)), quantizePalette(quantizeOf(8, 2, 10.0f, 0.0f)));
    for (double az : {0.0, 22.5, 100.0, 181.0, 359.0}) {
        const std::array<double, 3> n = fromAngles(az, 40.0);
        EXPECT_EQ(quantizeNormal(quantizeOf(8, 2, 10.0f, 360.0f), n[0], n[1], n[2]), quantizeNormal(eight, n[0], n[1], n[2])) << az;
    }
    {
        const Image color = loadPng(kFixtures / "inputs" / "character.png");
        Settings whole = presetSettings(Preset::Toon);
        whole.quantize.rotationDeg = 360.0f;
        EXPECT_EQ(run(color, whole).normal.data, run(color, presetSettings(Preset::Toon)).normal.data);
    }
    // the palette's order: flat, then rings innermost first, azimuths from the rotation
    const std::vector<std::array<double, 3>> palette = quantizePalette(turned);
    ASSERT_EQ(palette.size(), 5u);
    expectAngles(palette[0], 0.0, 0.0);
    expectAngles(palette[1], 45.0, 45.0);
    expectAngles(palette[4], 315.0, 45.0);
}

TEST(SpriteNormalsQuantize, ThreadCountNeverChangesTheBytes) {
    for (const Golden& g : goldens()) {
        SCOPED_TRACE(g.name);
        Image color;
        std::vector<Cell> cells;
        bool sequence;
        ASSERT_TRUE(goldenRequest(g, color, cells, sequence));
        for (const Quantize& q : quantizeVariants()) {
            Settings s = presetSettings(Preset::Toon);
            s.quantize = q;
            s.height = true;
            const Result one = run(color, s, cells, 1, sequence);
            const Result eight = run(color, s, cells, 8, sequence);
            EXPECT_EQ(one.normal.data, eight.normal.data);
            EXPECT_EQ(one.height.data, eight.height.data);
        }
    }
}

TEST(SpriteNormalsQuantize, DirectxFlipsGreenAfterQuantizing) {
    for (const Golden& g : goldens()) {
        SCOPED_TRACE(g.name);
        Image color;
        std::vector<Cell> cells;
        bool sequence;
        ASSERT_TRUE(goldenRequest(g, color, cells, sequence));
        Settings s = presetSettings(Preset::Toon);
        const Result gl = run(color, s, cells, 1, sequence);
        s.directx = true;
        const Result dx = run(color, s, cells, 1, sequence);
        for (size_t i = 0; i < gl.normal.pixelCount(); i++) {
            const uint8_t* a = gl.normal.data.data() + 4 * i;
            const uint8_t* b = dx.normal.data.data() + 4 * i;
            ASSERT_EQ(a[0], b[0]);
            ASSERT_EQ(a[2], b[2]);
            ASSERT_EQ(a[3], b[3]);
            // the same quantized direction, green negated (within the 1 LSB of the 127.5 tie)
            ASSERT_LE(std::abs(int(b[1]) - (255 - int(a[1]))), 1) << "pixel " << i;
        }
    }
}

TEST(SpriteNormalsQuantize, HeightMapIsTheGeneratorsAndMetricsInputsAreFinal) {
    const Image color = loadPng(kFixtures / "inputs" / "character.png");
    Settings s = presetSettings(Preset::Toon);
    s.height = true;
    const Result quantized = run(color, s);
    s.quantize.directions = 0;
    const Result smooth = run(color, s);
    EXPECT_EQ(quantized.height.data, smooth.height.data) << "the height map is not quantized";
    EXPECT_EQ(quantized.heightMin, smooth.heightMin);
    EXPECT_EQ(quantized.heightMax, smooth.heightMax);
    EXPECT_NE(quantized.normal.data, smooth.normal.data);
    EXPECT_TRUE(quantized.warnings.empty());
}

TEST(SpriteNormalsQuantize, ValidationNamesTheQuantizeFields) {
    Settings s = presetSettings(Preset::Toon);
    EXPECT_TRUE(validateSettings(s).empty());
    s.quantize = quantizeOf(5, 0, 46.0f, 361.0f);
    std::vector<Problem> problems = validateSettings(s);
    ASSERT_EQ(problems.size(), 4u);
    EXPECT_EQ(problems[0].field, "quantize.directions");
    EXPECT_EQ(problems[0].message, "quantize.directions must be 0 (off), 4, 6, 8, 12 or 16 (got 5)");
    EXPECT_EQ(problems[1].field, "quantize.tilts");
    EXPECT_EQ(problems[1].message, "quantize.tilts must be between 1 and 4 (got 0)");
    EXPECT_EQ(problems[2].field, "quantize.flat_deg");
    EXPECT_EQ(problems[2].message, "quantize.flat_deg must be between 0 and 45 (got 46)");
    EXPECT_EQ(problems[3].field, "quantize.rotation_deg");
    EXPECT_EQ(problems[3].message, "quantize.rotation_deg must be between 0 and 360 (got 361)");
    for (const Problem& p : problems) {
        EXPECT_EQ(p.kind, "invalid_settings");
        EXPECT_FALSE(p.fix.empty());
    }
    s.quantize = quantizeOf(16, 5, -1.0f, -0.5f);
    problems = validateSettings(s);
    ASSERT_EQ(problems.size(), 3u);
    EXPECT_EQ(problems[0].field, "quantize.tilts");
    s.quantize = quantizeOf(4, 1, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    problems = validateSettings(s);
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[1].message, "quantize.rotation_deg must be a number between 0 and 360 (got inf)");
    // the limits themselves are valid; off validates the other values too
    for (int d : {0, 4, 6, 8, 12, 16}) {
        s.quantize = quantizeOf(d, 4, 45.0f, 360.0f);   // 360 is 0 (a whole turn)
        EXPECT_TRUE(validateSettings(s).empty()) << d;
        s.quantize = quantizeOf(d, 1, 0.0f, 0.0f);
        EXPECT_TRUE(validateSettings(s).empty()) << d;
    }
    s.quantize = quantizeOf(0, 9, 10.0f, 0.0f);
    EXPECT_EQ(validateSettings(s).size(), 1u);
    // a refused request writes nothing
    Request request;
    request.color = disk();
    request.settings = s;
    EXPECT_EQ(generate(request).status, Status::InvalidRequest);
}
