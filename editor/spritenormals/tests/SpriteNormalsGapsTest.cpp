// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-spritenormals-tests: dark gaps ("Dark Gaps Are Edges") and the stones preset
// (docs/specs/S78-sprite-normals-improvements.md R8-R12, AC6) on two procedural seamless stone
// tiles made here: a 32 px pixel-art one (bricks between 1 px dark gaps) and a 512 px painted one
// (Voronoi stones with soft dark grout and brush noise). Each stone must read as its own rounded
// slab: flat in its middle, its rim sloping away from it.

#include "spritenormals/SpriteNormals.h"
#include "spritenormals/SpriteNormalsRecipe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine::editor::spritenormals;

namespace {

    constexpr double kPi = 3.14159265358979323846;

    // A stone tile: its colours and, per pixel, which stone it is (-1: gap) and its depth inside
    // the stone (px to the nearest gap; the stone's middle is its deepest pixel)
    struct StoneTile {
        Image color;
        std::vector<int> stone;
        std::vector<float> depth;
        int stones = 0;
    };

    uint32_t hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    float noise01(int x, int y, uint32_t seed) { return static_cast<float>(hash(static_cast<uint32_t>(x) * 73856093U ^ static_cast<uint32_t>(y) * 19349663U ^ seed) & 0xffffU) / 65535.0f; }

    // smooth periodic value noise of period `size`, cells of `cell` px
    float valueNoise(float x, float y, int size, int cell, uint32_t seed) {
        const int n = size / cell;
        const float fx = x / static_cast<float>(cell), fy = y / static_cast<float>(cell);
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        auto at = [&](int ix, int iy) { return noise01(((ix % n) + n) % n, ((iy % n) + n) % n, seed); };
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
        const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
        return a + (b - a) * sy;
    }

    uint8_t byte(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

    // 32 px pixel art: four rows of bricks 7 px high (odd widths 5-11), 1 px dark gaps, every row
    // shifted; a lighter top edge and a 2-colour dither on the face (a hand-made tile's palette)
    StoneTile pixelArtTile() {
        StoneTile t;
        const int size = 32;
        t.color = Image::make(size, size, 4, 8);
        t.stone.assign(static_cast<size_t>(size) * size, -1);
        t.depth.assign(static_cast<size_t>(size) * size, 0.0f);
        // (each row: 4 bricks and 4 gaps around the 32 px circle)
        const int widths[4][4] = {{7, 9, 7, 5}, {9, 5, 7, 7}, {11, 5, 7, 5}, {5, 9, 9, 5}};
        const int shift[4] = {0, 4, 9, 2};
        int id = 0;
        for (int row = 0; row < 4; row++) {
            int x = shift[row];
            for (int s = 0; s < 4; s++) {
                const int width = widths[row][s];
                for (int yy = 0; yy < 7; yy++) {
                    for (int xx = 0; xx < width; xx++) {
                        const int px = (x + xx) % size, py = row * 8 + yy;
                        const size_t i = static_cast<size_t>(py) * size + px;
                        t.stone[i] = id;
                        // (the brick's centre is its deepest pixel: ties along a long brick's middle go to it)
                        t.depth[i] = static_cast<float>(std::min({xx + 1, width - xx, yy + 1, 7 - yy})) -
                                     1e-3f * (std::fabs(static_cast<float>(xx) - 0.5f * static_cast<float>(width - 1)) + std::fabs(static_cast<float>(yy) - 3.0f));
                    }
                }
                x += width + 1;
                id++;
            }
        }
        t.stones = id;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const size_t i = static_cast<size_t>(y) * size + x;
                uint8_t* p = t.color.pixel(x, y);
                const int s = t.stone[i];
                if (s < 0) {
                    p[0] = 38, p[1] = 34, p[2] = 44;
                } else {
                    const bool top = y % 8 == 0;
                    const int tint = static_cast<int>(hash(static_cast<uint32_t>(s)) % 3) * 8;
                    const bool dither = ((x + y) & 1) && noise01(x, y, 7) > 0.6f;
                    const int base = top ? 182 : (dither ? 138 : 150);
                    p[0] = byte(static_cast<float>(base + tint));
                    p[1] = byte(static_cast<float>(base - 6 + tint / 2));
                    p[2] = byte(static_cast<float>(base - 16));
                }
                p[3] = 255;
            }
        }
        return t;
    }

    // 512 px painted: 5 x 5 jittered Voronoi stones on the torus, grout where the two nearest seeds
    // are within 7 px of equal distance (about 14 px wide, darker at its middle), stones shaded
    // darker toward their rim with soft brush noise
    StoneTile makePaintedTile(int size) {
        StoneTile t;
        t.color = Image::make(size, size, 4, 8);
        t.stone.assign(static_cast<size_t>(size) * size, -1);
        t.depth.assign(static_cast<size_t>(size) * size, 0.0f);
        const int grid = 5;
        const float cell = static_cast<float>(size) / grid;
        std::vector<std::array<float, 2>> seeds;
        for (int gy = 0; gy < grid; gy++) {
            for (int gx = 0; gx < grid; gx++) {
                const uint32_t h = hash(static_cast<uint32_t>(gy * grid + gx) + 11u);
                const float jx = (static_cast<float>(h & 0xff) / 255.0f - 0.5f) * 0.5f, jy = (static_cast<float>((h >> 8) & 0xff) / 255.0f - 0.5f) * 0.5f;
                seeds.push_back({(gx + 0.5f + jx) * cell, (gy + 0.5f + jy) * cell});
            }
        }
        t.stones = static_cast<int>(seeds.size());
        const float half = 7.0f * static_cast<float>(size) / 512.0f;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                float d1 = 1e9f, d2 = 1e9f;
                int nearest = 0;
                for (size_t s = 0; s < seeds.size(); s++) {
                    float dx = std::fabs(x + 0.5f - seeds[s][0]), dy = std::fabs(y + 0.5f - seeds[s][1]);
                    dx = std::min(dx, static_cast<float>(size) - dx);
                    dy = std::min(dy, static_cast<float>(size) - dy);
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d < d1) {
                        d2 = d1;
                        d1 = d;
                        nearest = static_cast<int>(s);
                    } else if (d < d2) {
                        d2 = d;
                    }
                }
                // (d2 - d1) / 2 is about the distance to the cell's border
                const float edge = 0.5f * (d2 - d1);
                const size_t i = static_cast<size_t>(y) * size + x;
                uint8_t* p = t.color.pixel(x, y);
                const float brush = valueNoise(static_cast<float>(x), static_cast<float>(y), size, 16, 3u) * 0.6f +
                                    valueNoise(static_cast<float>(x), static_cast<float>(y), size, 4, 5u) * 0.4f;
                if (edge < half) {
                    const float v = 30.0f + 22.0f * (edge / half) + 10.0f * brush;
                    p[0] = byte(v), p[1] = byte(v * 0.95f), p[2] = byte(v * 1.1f);
                } else {
                    t.stone[i] = nearest;
                    t.depth[i] = edge - half;
                    const float rim = std::min(1.0f, (edge - half) / (12.0f * static_cast<float>(size) / 512.0f));
                    const float tint = static_cast<float>(hash(static_cast<uint32_t>(nearest) + 99u) % 24);
                    const float v = 120.0f + 40.0f * rim + 30.0f * (brush - 0.5f) + tint;
                    p[0] = byte(v + 6.0f), p[1] = byte(v), p[2] = byte(v - 12.0f);
                }
                p[3] = 255;
            }
        }
        return t;
    }

    // (made once: several tests use it)
    const StoneTile& paintedTile() {
        static const StoneTile tile = makePaintedTile(512);
        return tile;
    }

    Result run(const Image& color, const Settings& settings, int threads = 1, std::vector<Cell> cells = {}) {
        Request request;
        request.color = color;
        request.settings = settings;
        request.threads = threads;
        request.cells = std::move(cells);
        Result result = generate(request);
        EXPECT_EQ(result.status, Status::Ok) << (result.errors.empty() ? std::string() : result.errors[0].message);
        return result;
    }

    // OpenGL normal of an RGBA8 texel in image axes: x right, y DOWN (green up flipped), z out
    std::array<double, 3> imageNormal(const Image& normal, int x, int y) {
        const uint8_t* p = normal.pixel(x, y);
        double n[3] = {p[0] / 255.0 * 2.0 - 1.0, p[1] / 255.0 * 2.0 - 1.0, p[2] / 255.0 * 2.0 - 1.0};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return {n[0] / l, -n[1] / l, n[2] / l};
    }

    double tiltDeg(const std::array<double, 3>& n) { return std::acos(std::clamp(n[2], -1.0, 1.0)) * 180.0 / kPi; }

    // Per stone: the tilt at its middle (its deepest pixel), and over its rim (pixels at most
    // `rimDepth` deep) the mean tilt and the share whose normal leans away from the middle
    struct StoneShape {
        double middleTiltMax = 0.0;      // the largest over the stones
        double rimTiltMean = 0.0;        // over every rim pixel
        double rimOutwardShare = 0.0;    // of the rim pixels leaning away from their stone's middle
    };

    StoneShape stoneShape(const StoneTile& t, const Image& normal, float rimDepth) {
        const int size = t.color.width;
        std::vector<int> middle(static_cast<size_t>(t.stones), -1);
        for (size_t i = 0; i < t.stone.size(); i++) {
            const int s = t.stone[i];
            if (s >= 0 && (middle[static_cast<size_t>(s)] < 0 || t.depth[i] > t.depth[static_cast<size_t>(middle[static_cast<size_t>(s)])])) middle[static_cast<size_t>(s)] = static_cast<int>(i);
        }
        StoneShape out;
        double tiltSum = 0.0;
        int rim = 0, outward = 0;
        for (int s = 0; s < t.stones; s++) {
            const int m = middle[static_cast<size_t>(s)];
            if (m < 0) continue;
            out.middleTiltMax = std::max(out.middleTiltMax, tiltDeg(imageNormal(normal, m % size, m / size)));
        }
        for (size_t i = 0; i < t.stone.size(); i++) {
            const int s = t.stone[i];
            if (s < 0 || t.depth[i] > rimDepth) continue;
            const int m = middle[static_cast<size_t>(s)];
            const int x = static_cast<int>(i) % size, y = static_cast<int>(i) / size;
            // away from the middle, on the torus
            double ax = x - m % size, ay = y - m / size;
            if (ax > size / 2) ax -= size;
            if (ax < -size / 2) ax += size;
            if (ay > size / 2) ay -= size;
            if (ay < -size / 2) ay += size;
            const std::array<double, 3> n = imageNormal(normal, x, y);
            tiltSum += tiltDeg(n);
            rim++;
            if (n[0] * ax + n[1] * ay > 0.0) outward++;
        }
        out.rimTiltMean = rim ? tiltSum / rim : 0.0;
        out.rimOutwardShare = rim ? static_cast<double>(outward) / rim : 0.0;
        return out;
    }

    // the image rolled by (dx, dy) on the torus
    Image roll(const Image& image, int dx, int dy) {
        Image out = Image::make(image.width, image.height, 4, 8);
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                std::memcpy(out.pixel((x + dx) % image.width, (y + dy) % image.height), image.pixel(x, y), 4);
            }
        }
        return out;
    }

    int maxDifference(const Image& a, const Image& b) {
        int worst = 0;
        for (size_t i = 0; i < a.data.size(); i++) worst = std::max(worst, std::abs(static_cast<int>(a.data[i]) - static_cast<int>(b.data[i])));
        return worst;
    }

    Settings shapeOnly(Preset preset, float share) {
        Settings s = presetSettings(preset);
        s.detail = 0.0f;
        s.gaps.share = share;
        return s;
    }

    bool warns(const Result& r, const std::string& kind) {
        for (const Problem& w : r.warnings) {
            if (w.kind == kind) return true;
        }
        return false;
    }

}

// ---------------------------------------------------------------- settings, preset, recipe

TEST(SpriteNormalsGaps, OffByDefaultValidatedAndRecordedOnlyWhenOn) {
    EXPECT_FALSE(Settings().gaps.on());
    for (Preset p : allPresets()) {
        EXPECT_EQ(presetSettings(p).gaps.on(), p == Preset::Stones) << toString(p) << ": only stones has gaps";
        // recipes of every other preset are unchanged: no "gaps" key
        EXPECT_EQ(settingsToJson(presetSettings(p)).contains("gaps"), p == Preset::Stones) << toString(p);
    }
    Settings s;
    for (float bad : {-0.1f, 0.95f, std::nanf("")}) {
        s.gaps.share = bad;
        const std::vector<Problem> problems = validateSettings(s);
        ASSERT_EQ(problems.size(), 1u) << bad;
        EXPECT_EQ(problems[0].field, "gaps.share");
        EXPECT_EQ(problems[0].kind, "invalid_settings");
    }
    s.gaps.share = 0.9f;
    EXPECT_TRUE(validateSettings(s).empty());
    s.gaps.share = 0.25f;
    const nlohmann::json j = settingsToJson(s);
    ASSERT_TRUE(j.contains("gaps"));
    EXPECT_EQ(j["gaps"].dump(), "{\"share\":0.25}");
    // round trip, over another preset's values
    Settings back = presetSettings(Preset::Crisp);
    std::string error;
    ASSERT_TRUE(settingsFromJson(j, back, error)) << error;
    EXPECT_EQ(back.gaps, s.gaps);
    // an override to off on stones is recorded (readers resolve over the preset)
    Settings off = presetSettings(Preset::Stones);
    off.gaps.share = 0.0f;
    EXPECT_EQ(settingsToJson(off)["gaps"].dump(), "{\"share\":0}");
    Settings resolved = presetSettings(Preset::Stones);
    ASSERT_TRUE(settingsFromJson(settingsToJson(off), resolved, error)) << error;
    EXPECT_FALSE(resolved.gaps.on());
    // wrong types name the key
    EXPECT_FALSE(settingsFromJson(nlohmann::json::parse("{\"gaps\": 0.3}"), back, error));
    EXPECT_EQ(error, "gaps is a number, expected an object {share}");
    EXPECT_FALSE(settingsFromJson(nlohmann::json::parse("{\"gaps\": {\"share\": \"lots\"}}"), back, error));
    EXPECT_EQ(error, "gaps.share is a string, expected a number");
    // a recipe with gaps validates
    RecipeInput in;
    in.role = "normal";
    in.settings = presetSettings(Preset::Stones);
    std::string why;
    EXPECT_TRUE(validateRecipe(makeRecipe(in), why)) << why;
}

TEST(SpriteNormalsGaps, StonesPresetIsAppendedAndTileIsUnchanged) {
    ASSERT_EQ(allPresets().back(), Preset::Stones);
    EXPECT_EQ(static_cast<int>(Preset::Stones), static_cast<int>(Preset::Tile) + 1);
    EXPECT_STREQ(toString(Preset::Stones), "stones");
    Preset p;
    ASSERT_TRUE(parsePreset("Stones", p));
    EXPECT_EQ(p, Preset::Stones);
    EXPECT_EQ(presetNames(), "soft, default, crisp, detailed, chunky, toon, painted, props, tile, stones");
    const Settings s = presetSettings(Preset::Stones);
    EXPECT_EQ(s.edges, Edges::Wrap);
    EXPECT_EQ(s.profile, Profile::Dome);
    EXPECT_EQ(s.gaps.share, 0.3f);
    EXPECT_EQ(s.softness, 1.5f);
    EXPECT_TRUE(validateSettings(s).empty());
    const Settings tile = presetSettings(Preset::Tile);
    EXPECT_EQ(tile.strength, 2.0f);
    EXPECT_EQ(tile.detail, 3.0f);
    EXPECT_EQ(tile.smoothing, 1.0f);
    EXPECT_EQ(tile.volume, 1.15f);
    EXPECT_EQ(tile.edges, Edges::Wrap);
    EXPECT_EQ(tile.profile, Profile::Auto);
    EXPECT_FALSE(tile.gaps.on());
}

// ---------------------------------------------------------------- the generator

TEST(SpriteNormalsGaps, OffIsByteIdenticalToTheSettingsWithoutIt) {
    const StoneTile t = pixelArtTile();
    for (Preset p : {Preset::Tile, Preset::Default, Preset::Props}) {
        Settings s = presetSettings(p);
        const Result before = run(t.color, s);
        s.gaps.share = 0.0f;
        EXPECT_EQ(run(t.color, s).normal.data, before.normal.data) << toString(p);
    }
}

TEST(SpriteNormalsGaps, PixelArtStonesBecomeSlabs) {
    const StoneTile t = pixelArtTile();
    // the shape alone (no painted relief): without gaps an opaque wrap tile is flat; with them every
    // stone gets a rim sloping away from its flat middle
    const Result off = run(t.color, shapeOnly(Preset::Stones, 0.0f));
    for (size_t i = 0; i < t.stone.size(); i++) {
        ASSERT_EQ(off.normal.data[i * 4], 128);
        ASSERT_EQ(off.normal.data[i * 4 + 1], 128);
        ASSERT_EQ(off.normal.data[i * 4 + 2], 255);
    }
    const Result on = run(t.color, shapeOnly(Preset::Stones, 0.3f));
    EXPECT_FALSE(warns(on, "gaps_unmatched"));
    const StoneShape shape = stoneShape(t, on.normal, 1.0f);
    // (the 30% quantile takes some brick corners too: a 5 px brick's middle tilts up to about 11 degrees)
    EXPECT_LT(shape.middleTiltMax, 15.0);
    EXPECT_GT(shape.rimTiltMean, 25.0);
    EXPECT_GT(shape.rimOutwardShare, 0.95);
    // the preset (its painted relief on top): still a slab per stone
    const Result preset = run(t.color, presetSettings(Preset::Stones));
    const StoneShape lit = stoneShape(t, preset.normal, 1.0f);
    EXPECT_LT(lit.middleTiltMax, 20.0);
    EXPECT_GT(lit.rimTiltMean, 25.0);
    EXPECT_GT(lit.rimOutwardShare, 0.9);
    // the gap pixels keep their alpha
    for (size_t i = 0; i < t.stone.size(); i++) EXPECT_EQ(preset.normal.data[i * 4 + 3], 255);
}

TEST(SpriteNormalsGaps, PaintedStonesBecomeSlabs) {
    const StoneTile& t = paintedTile();
    const Result off = run(t.color, shapeOnly(Preset::Stones, 0.0f), 4);
    EXPECT_LT(stoneShape(t, off.normal, 4.0f).rimTiltMean, 1.0);
    const Result on = run(t.color, shapeOnly(Preset::Stones, 0.3f), 4);
    EXPECT_FALSE(warns(on, "gaps_unmatched"));
    const StoneShape shape = stoneShape(t, on.normal, 4.0f);
    EXPECT_LT(shape.middleTiltMax, 5.0);
    EXPECT_GT(shape.rimTiltMean, 20.0);
    EXPECT_GT(shape.rimOutwardShare, 0.95);
    const Result preset = run(t.color, presetSettings(Preset::Stones), 4);
    const StoneShape lit = stoneShape(t, preset.normal, 4.0f);
    EXPECT_LT(lit.middleTiltMax, 20.0);
    EXPECT_GT(lit.rimTiltMean, 20.0);
    EXPECT_GT(lit.rimOutwardShare, 0.9);
}

TEST(SpriteNormalsGaps, EveryEdgesModeAndProfile) {
    const StoneTile t = pixelArtTile();
    for (Edges edges : {Edges::Wrap, Edges::Extend, Edges::Transparent}) {
        for (Profile profile : {Profile::Dome, Profile::Bevel, Profile::Rounded, Profile::Inflate}) {
            Settings s = shapeOnly(Preset::Stones, 0.3f);
            s.edges = edges;
            s.profile = profile;
            const Result r = run(t.color, s);
            EXPECT_TRUE(r.warnings.empty()) << toString(edges) << " " << toString(profile) << ": " << (r.warnings.empty() ? "" : r.warnings[0].message);
            const StoneShape shape = stoneShape(t, r.normal, 1.0f);
            EXPECT_GT(shape.rimOutwardShare, 0.85) << toString(edges) << " " << toString(profile);
            EXPECT_GT(shape.rimTiltMean, 15.0) << toString(edges) << " " << toString(profile);
        }
    }
}

TEST(SpriteNormalsGaps, TransparentSpritesGetGapsWithinTheirIsland) {
    // two light stones touching through a dark seam, on transparency: one island; the seam is an edge
    const Image color = [] {
        Image image = Image::make(40, 24, 4, 8);
        for (int y = 2; y < 21; y++) {
            for (int x = 2; x < 37; x++) {
                uint8_t* p = image.pixel(x, y);
                const bool seam = x == 19 || x == 20;
                p[0] = seam ? 40 : 170, p[1] = seam ? 36 : 160, p[2] = seam ? 44 : 140, p[3] = 255;
            }
        }
        return image;
    }();
    Settings s = presetSettings(Preset::Props);
    s.detail = 0.0f;
    const Result off = run(color, s);
    s.gaps.share = 0.05f;   // the seam is 38 of the 665 pixels
    const Result on = run(color, s);
    // beside the seam each stone's rim falls toward it (normals point downhill: the left stone's
    // to the right, the right stone's to the left); without gaps the seam is inside the plateau
    const double leftOff = imageNormal(off.normal, 18, 11)[0], leftOn = imageNormal(on.normal, 18, 11)[0];
    const double rightOn = imageNormal(on.normal, 21, 11)[0];
    EXPECT_NEAR(leftOff, 0.0, 0.02);
    EXPECT_GT(leftOn, 0.3);
    EXPECT_LT(rightOn, -0.3);
    EXPECT_EQ(on.islands, 1);
}

TEST(SpriteNormalsGaps, WrapTilesSeamlessly) {
    // the result never depends on where the tile is cut: generating the rolled tile gives the
    // rolled map, so a 3x3 tiling has no seam
    for (int size : {32, 512}) {
        const StoneTile t = size == 32 ? pixelArtTile() : paintedTile();
        const Settings s = presetSettings(Preset::Stones);
        const Result a = run(t.color, s, 4);
        const int dx = size / 2 + 3, dy = size / 3 + 1;
        const Result b = run(roll(t.color, dx, dy), s, 4);
        EXPECT_LE(maxDifference(roll(a.normal, dx, dy), b.normal), 1) << size;
        // across the tile's border the map changes as much as inside it
        auto step = [&](int x0, int x1) {
            double sum = 0.0;
            for (int y = 0; y < size; y++) {
                for (int c = 0; c < 3; c++) sum += std::abs(static_cast<int>(a.normal.pixel(x0, y)[c]) - static_cast<int>(a.normal.pixel(x1, y)[c]));
            }
            return sum / size;
        };
        double inside = 0.0;
        for (int x = 0; x + 1 < size; x++) inside = std::max(inside, step(x, x + 1));
        EXPECT_LE(step(size - 1, 0), inside) << size;
    }
}

TEST(SpriteNormalsGaps, DeterministicForAnyThreadCount) {
    for (int size : {32, 512}) {
        const StoneTile t = size == 32 ? pixelArtTile() : paintedTile();
        const Result one = run(t.color, presetSettings(Preset::Stones), 1);
        for (int threads : {4, 16}) EXPECT_EQ(run(t.color, presetSettings(Preset::Stones), threads).normal.data, one.normal.data) << size << " " << threads;
    }
    // the inflate solve between gaps (512 px: SpriteNormalsHires.ThreadCountNeverChangesTheBytes covers the solver)
    Settings inflate = presetSettings(Preset::Stones);
    inflate.profile = Profile::Inflate;
    const Image tile = pixelArtTile().color;
    const Result i1 = run(tile, inflate, 1);
    for (int threads : {4, 16}) EXPECT_EQ(run(tile, inflate, threads).normal.data, i1.normal.data) << "inflate " << threads;
}

TEST(SpriteNormalsGaps, UnmatchedOnAFlatImage) {
    Image flat = Image::make(32, 32, 4, 8);
    for (size_t i = 0; i < flat.pixelCount(); i++) {
        flat.data[i * 4] = 120, flat.data[i * 4 + 1] = 110, flat.data[i * 4 + 2] = 100, flat.data[i * 4 + 3] = 255;
    }
    Settings s = presetSettings(Preset::Stones);
    const Result r = run(flat, s);
    ASSERT_TRUE(warns(r, "gaps_unmatched"));
    for (const Problem& w : r.warnings) {
        if (w.kind != "gaps_unmatched") continue;
        EXPECT_EQ(w.field, "gaps.share");
        EXPECT_NE(w.message.find("1 of 1 cell"), std::string::npos) << w.message;
        EXPECT_NE(w.fix.find("gaps.share"), std::string::npos) << w.fix;
    }
    // no gap was applied: the same map as without gaps
    s.gaps.share = 0.0f;
    EXPECT_EQ(run(flat, s).normal.data, r.normal.data);
    // per cell: a sheet of a stone tile and a flat one names 1 of 2
    Image sheet = Image::make(64, 32, 4, 8);
    const StoneTile t = pixelArtTile();
    for (int y = 0; y < 32; y++) {
        std::memcpy(sheet.pixel(0, y), t.color.pixel(0, y), 32 * 4);
        std::memcpy(sheet.pixel(32, y), flat.pixel(0, y), 32 * 4);
    }
    const Result two = run(sheet, presetSettings(Preset::Stones), 1, {{0, 0, 32, 32}, {32, 0, 32, 32}});
    bool named = false;
    for (const Problem& w : two.warnings) named = named || (w.kind == "gaps_unmatched" && w.message.find("1 of 2 cells") != std::string::npos);
    EXPECT_TRUE(named);
    // a stone tile: no warning
    EXPECT_FALSE(warns(run(t.color, presetSettings(Preset::Stones)), "gaps_unmatched"));
}
