// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-spritenormals-tests: high-resolution sprites (2026-10-30-sprite-normals-hires-design.md):
// resolution scaling, the thickness auto radius, the inflate profile, the outer band, version 1
// recipes, determinism, the new presets and the roughness / crease assessment, on the fixtures of
// tests/fixtures/hires (README.md there: synthetic ground truth and four downscaled CC0 sprites).

#include "spritenormals/SpriteMedia.h"
#include "object/ui/Image.h"
#include "spritenormals/SpriteNormals.h"
#include "spritenormals/SpriteNormalsField.h"
#include "spritenormals/SpriteNormalsPaint.h"
#include "spritenormals/SpriteNormalsPreview.h"
#include "spritenormals/SpriteNormalsRecipe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>
#include <chrono>

using namespace jasperengine::editor::spritenormals;
namespace fs = std::filesystem;
namespace texsynth = jasperengine::editor::texsynth;

namespace {

    const fs::path kHires = fs::path(SPRITENORMALS_FIXTURES) / "hires";
    constexpr double kPi = 3.14159265358979323846;

    Image load(const fs::path& path) {
        texsynth::Image image;
        std::string error;
        EXPECT_TRUE(texsynth::loadImage(path, image, error)) << error;
        return texsynth::toRgba8(image);
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

    // a shaded disc of radius r centred in a w x h image (brighter up-left), alpha 255 inside
    Image disc(int w, int h, float r, uint8_t alphaInside = 255) {
        return fromFunction(w, h, [=](int x, int y) {
            const float dx = x + 0.5f - w * 0.5f, dy = y + 0.5f - h * 0.5f;
            if (dx * dx + dy * dy > r * r) return std::array<uint8_t, 4>{0, 0, 0, 0};
            const float shade = std::clamp(0.65f - (dx + dy) / (4.0f * r), 0.15f, 1.0f);
            return std::array<uint8_t, 4>{static_cast<uint8_t>(200 * shade + 40), static_cast<uint8_t>(120 * shade + 30),
                                          static_cast<uint8_t>(60 * shade + 20), alphaInside};
        });
    }

    Result run(const Image& color, const Settings& settings, int threads = 1, std::vector<Cell> cells = {}, bool sequence = false,
               bool shape = false) {
        Request request;
        request.color = color;
        request.settings = settings;
        request.threads = threads;
        request.cells = std::move(cells);
        request.sequence = sequence;
        request.shapeNormals = shape;
        Result result = generate(request);
        EXPECT_EQ(result.status, Status::Ok) << (result.errors.empty() ? std::string() : result.errors[0].message);
        return result;
    }

    void decode(const uint8_t* p, double n[3]) {
        for (int k = 0; k < 3; k++) n[k] = p[k] / 255.0 * 2.0 - 1.0;
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; k++) n[k] /= l;
    }

    // mean angle (degrees) between a generated map and an encoded truth over the truth's alpha > 0
    double meanError(const Image& normal, const Image& truth) {
        double sum = 0.0;
        int64_t count = 0;
        for (int y = 0; y < truth.height; y++) {
            for (int x = 0; x < truth.width; x++) {
                if (truth.pixel(x, y)[3] == 0) continue;
                double a[3], b[3];
                decode(normal.pixel(x, y), a);
                decode(truth.pixel(x, y), b);
                sum += std::acos(std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0)) * 180.0 / kPi;
                count++;
            }
        }
        return count ? sum / count : 0.0;
    }

    Settings preset(Preset p) { return presetSettings(p); }

    // the settings a version 1 recipe regenerates with (specFromRecipe: version 1, profile rounded, then the recipe's)
    Settings versionOne(Preset p = Preset::Default) {
        Settings s = presetSettings(p);
        s.version = 1;
        s.profile = Profile::Rounded;
        return s;
    }

    Assessment assessOf(const Result& r, const Settings& s) {
        const Metrics m = measure(r.normal, {}, s.directx, s.alphaThreshold, nullptr, r.shape.empty() ? nullptr : &r.shape);
        return assess(s, m, 0, &r);
    }

    bool warns(const Assessment& a, const char* kind) {
        for (const Problem& p : a.warnings) {
            if (p.kind == kind) return true;
        }
        return false;
    }

}

// ---------------------------------------------------------------- rule 1: resolution scaling

TEST(SpriteNormalsHires, ScaleAndProfileAutoAreNoOpsUpTo128) {
    const Image sprite = disc(140, 140, 64.0f);   // island 128 px
    Settings s = preset(Preset::Default);
    const Result scaled = run(sprite, s);
    EXPECT_FLOAT_EQ(scaled.scaleMin, 1.0f);
    EXPECT_FLOAT_EQ(scaled.scaleMax, 1.0f);
    EXPECT_EQ(scaled.profilesUsed, 1u << static_cast<int>(Profile::Rounded));
    Settings off = s;
    off.resolutionScale = ResolutionScale::Off;
    EXPECT_EQ(run(sprite, off).normal.data, scaled.normal.data);
    Settings rounded = s;
    rounded.profile = Profile::Rounded;
    EXPECT_EQ(run(sprite, rounded).normal.data, scaled.normal.data);
    // at 128 px version 2 differs from version 1 only by the auto radius: the same radius, the same bytes
    Settings v1 = versionOne();
    v1.radius = 9.0f;
    rounded.radius = 9.0f;
    EXPECT_EQ(run(sprite, v1).normal.data, run(sprite, rounded).normal.data);
}

TEST(SpriteNormalsHires, ScaleMultipliesSmoothingSoftnessAndDetail) {
    // an island of exactly 256 x 300 px: k = 2
    const Image sprite = fromFunction(270, 310, [](int x, int y) {
        const bool in = x >= 7 && x < 263 && y >= 5 && y < 305;
        const uint8_t v = static_cast<uint8_t>(80 + (x * 7 + y * 3) % 120);
        return std::array<uint8_t, 4>{v, static_cast<uint8_t>(v / 2), 90, static_cast<uint8_t>(in ? 255 : 0)};
    });
    Settings s = preset(Preset::Default);
    const Result scaled = run(sprite, s, 4);
    EXPECT_FLOAT_EQ(scaled.scaleMin, 2.0f);
    EXPECT_EQ(scaled.profilesUsed, 1u << static_cast<int>(Profile::Inflate));
    Settings off = s;
    off.resolutionScale = ResolutionScale::Off;
    off.smoothing *= 2.0f;
    off.softness *= 2.0f;
    off.detail *= 2.0f;
    const Result manual = run(sprite, off, 4);
    EXPECT_FLOAT_EQ(manual.scaleMax, 1.0f);
    EXPECT_EQ(manual.normal.data, scaled.normal.data);
    // version 1 never scales
    Settings v1 = versionOne();
    EXPECT_FLOAT_EQ(run(sprite, v1).scaleMax, 1.0f);
}

// ---------------------------------------------------------------- rule 2: limits

TEST(SpriteNormalsHires, LargeSmoothingAndSoftnessRunTheBoxCascade) {
    Settings s = preset(Preset::Default);
    s.smoothing = 32.0f;
    s.softness = 32.0f;
    const Result r = run(disc(300, 300, 140.0f), s, 3);   // k = 2.2: sigma 70
    EXPECT_TRUE(validateSettings(s).empty());
    int flat = 0;
    for (size_t i = 0; i < r.normal.pixelCount(); i++) flat += r.normal.data[i * 4 + 2] == 255 ? 1 : 0;
    EXPECT_GT(flat, 0);
    // the cascade is the Gaussian's variance: a blurred step matches the exact one within 2%
    detail::Field step = detail::Field::zero(400, 1);
    for (int x = 200; x < 400; x++) step.data[static_cast<size_t>(x)] = 1.0f;
    const detail::Field box = detail::gaussian(step, 24.0f, detail::Border::Clamp);
    detail::Field exact = step;
    {
        int radius = static_cast<int>(std::ceil(24.0f * 3.0f));
        for (int x = 0; x < 400; x++) {
            double sum = 0.0, weight = 0.0;
            for (int o = -radius; o <= radius; o++) {
                const double w = std::exp(-0.5 * (o / 24.0) * (o / 24.0));
                sum += w * step.data[static_cast<size_t>(std::clamp(x + o, 0, 399))];
                weight += w;
            }
            exact.data[static_cast<size_t>(x)] = static_cast<float>(sum / weight);
        }
    }
    for (int x = 0; x < 400; x++) EXPECT_NEAR(box.data[static_cast<size_t>(x)], exact.data[static_cast<size_t>(x)], 0.02f) << x;
}

// ---------------------------------------------------------------- rule 3: the thickness auto radius

TEST(SpriteNormalsHires, AutoRadiusIsThreeTenthsOfTheThickness) {
    Settings s = preset(Preset::Default);
    s.profile = Profile::Rounded;
    // a disc: 0.3 x its inscribed radius == 0.15 x its diameter (within the pixel grid)
    Result r = run(disc(270, 270, 128.0f), s);
    EXPECT_NEAR(r.radiusMax, 0.15f * 256.0f, 1.0f);
    // a ring of tube half-width 36: 0.3 x 36 (the bbox measure of version 1 would give 0.15 x 256)
    const Image ring = fromFunction(270, 270, [](int x, int y) {
        const float d = std::hypot(x + 0.5f - 135.0f, y + 0.5f - 135.0f);
        return std::array<uint8_t, 4>{150, 150, 150, static_cast<uint8_t>(std::fabs(d - 92.0f) < 36.0f ? 255 : 0)};
    });
    r = run(ring, s);
    EXPECT_NEAR(r.radiusMax, 0.3f * 36.0f, 0.6f);
    EXPECT_NEAR(run(ring, versionOne()).radiusMax, 0.15f * 256.0f, 0.2f);
    // at least 4, at most 512
    r = run(fromFunction(300, 20, [](int x, int y) { return std::array<uint8_t, 4>{90, 90, 90, static_cast<uint8_t>(y > 2 && y < 9 && x > 1 ? 255 : 0)}; }), s);
    EXPECT_FLOAT_EQ(r.radiusMin, 4.0f);
    // a sequence: the median over the frames of their largest island's thickness (40, 60, 100 -> 30 -> 9)
    Image strip = Image::make(300, 100, 4, 8);
    auto fill = [&](int x0, int y0, int w, int h) {
        for (int y = y0; y < y0 + h; y++) {
            for (int x = x0; x < x0 + w; x++) {
                uint8_t* p = strip.pixel(x, y);
                p[0] = p[1] = p[2] = 90;
                p[3] = 255;
            }
        }
    };
    fill(0, 0, 40, 80);
    fill(100, 0, 60, 60);
    fill(200, 0, 100, 100);
    r = run(strip, preset(Preset::Default), 1, {{0, 0, 100, 100}, {100, 0, 100, 100}, {200, 0, 100, 100}}, true);
    EXPECT_FALSE(r.radiusPerIsland);
    EXPECT_FLOAT_EQ(r.radiusMin, 9.0f);
}

// ---------------------------------------------------------------- rule 4: inflate

TEST(SpriteNormalsHires, InflateIsAHemisphereOnADisc) {
    // h = 2 sqrt(u) is the hemisphere of the disc: strength 1, volume 2 (kInflateHeight 1)
    const float radius = 100.0f;
    const Image sprite = disc(220, 220, radius);
    Settings s = preset(Preset::Default);
    s.profile = Profile::Inflate;
    s.strength = 1.0f;
    s.volume = 2.0f;
    s.detail = 0.0f;
    s.softness = 0.0f;
    s.resolutionScale = ResolutionScale::Off;
    const Result r = run(sprite, s, 2);
    // the pixel mask's boundary lies half a pixel past its last centres
    const double re = radius + 0.5;
    double sum = 0.0;
    int count = 0;
    for (int y = 0; y < 220; y++) {
        for (int x = 0; x < 220; x++) {
            const double dx = x + 0.5 - 110.0, dy = y + 0.5 - 110.0;
            const double rho = std::sqrt(dx * dx + dy * dy);
            if (rho > 0.9 * radius) continue;
            const double z = std::sqrt(re * re - rho * rho);
            const double truth[3] = {dx / re, -dy / re, z / re};
            double n[3];
            decode(r.normal.pixel(x, y), n);
            sum += std::acos(std::clamp(n[0] * truth[0] + n[1] * truth[1] + n[2] * truth[2], -1.0, 1.0)) * 180.0 / kPi;
            count++;
        }
    }
    EXPECT_LT(sum / count, 1.0);
    // the solver: a disc's exact u = (r^2 - rho^2) / 4 at the centre, within 1%
    std::vector<uint8_t> mask(201 * 201, 0);
    for (int y = 0; y < 201; y++) {
        for (int x = 0; x < 201; x++) mask[static_cast<size_t>(y) * 201 + x] = std::hypot(x - 100.0, y - 100.0) < 90.0;
    }
    std::vector<double> u;
    int iterations = 0;
    ASSERT_EQ(detail::poisson(mask, 201, 201, detail::Border::Zero, u, nullptr, nullptr, &iterations), detail::PoissonStatus::Solved);
    EXPECT_NEAR(u[100 * 201 + 100], 90.5 * 90.5 / 4.0, 0.01 * 90.5 * 90.5 / 4.0);
    EXPECT_LE(iterations, 20);
    // no pixel held at 0 (wrap / extend, opaque): no solution, flat
    std::vector<uint8_t> all(64 * 64, 1);
    EXPECT_EQ(detail::poisson(all, 64, 64, detail::Border::Wrap, u), detail::PoissonStatus::Singular);
    EXPECT_EQ(detail::poisson(all, 64, 64, detail::Border::Clamp, u), detail::PoissonStatus::Singular);
    // Neumann (extend): an opaque strip with a transparent left column rises toward the right
    std::vector<uint8_t> strip(64 * 8, 1);
    for (int y = 0; y < 8; y++) strip[static_cast<size_t>(y) * 64] = 0;
    ASSERT_EQ(detail::poisson(strip, 64, 8, detail::Border::Clamp, u), detail::PoissonStatus::Solved);
    EXPECT_GT(u[4 * 64 + 63], u[4 * 64 + 32]);
    EXPECT_NEAR(u[0 * 64 + 40], u[7 * 64 + 40], 1e-6 * u[7 * 64 + 40]);
}

// ---------------------------------------------------------------- rule 5: the outer band

TEST(SpriteNormalsHires, OuterBandDropsGlowsAndKeepsGlass) {
    // a 200 px core disc, a soft glow around it (alpha 60) and a glass window inside it (alpha 110)
    auto sprite = [](bool glow, uint8_t glass) {
        return fromFunction(300, 300, [=](int x, int y) {
            const float d = std::hypot(x + 0.5f - 150.0f, y + 0.5f - 150.0f);
            if (d < 40.0f) return std::array<uint8_t, 4>{120, 160, 200, glass};
            if (d < 100.0f) return std::array<uint8_t, 4>{180, 90, 60, 255};
            if (glow && d < 116.0f) return std::array<uint8_t, 4>{255, 220, 120, 60};
            return std::array<uint8_t, 4>{0, 0, 0, 0};
        });
    };
    Settings s = preset(Preset::Default);
    s.profile = Profile::Rounded;
    s.radius = 20.0f;
    s.detail = 0.0f;
    s.softness = 1.0f;
    s.resolutionScale = ResolutionScale::Off;
    auto core = [](const Image& n) {
        std::vector<uint8_t> out;
        for (int y = 0; y < 300; y++) {
            for (int x = 0; x < 300; x++) {
                if (std::hypot(x + 0.5f - 150.0f, y + 0.5f - 150.0f) < 96.0f) out.insert(out.end(), n.pixel(x, y), n.pixel(x, y) + 3);
            }
        }
        return out;
    };
    const Result plain = run(sprite(false, 110), s);
    const Result glowing = run(sprite(true, 110), s);
    EXPECT_EQ(core(glowing.normal), core(plain.normal)) << "the glow is outside the shape";
    EXPECT_EQ(core(run(sprite(false, 255), s).normal), core(plain.normal)) << "the glass stays inside it";
    // the glow keeps its alpha and gets a normal: tilted outward next to the rim, flat beyond the softness
    const uint8_t* g = glowing.normal.pixel(150 + 100, 150);
    EXPECT_EQ(g[3], 60);
    EXPECT_GT(g[0], 128);
    const uint8_t* far = glowing.normal.pixel(150 + 110, 150);
    EXPECT_EQ(far[3], 60);
    EXPECT_EQ(far[0], 128);
    // version 1: the glow is part of the shape (the core changes)
    Settings v1 = s;
    v1.version = 1;
    EXPECT_NE(core(run(sprite(true, 110), v1).normal), core(run(sprite(false, 110), v1).normal));
}

// ---------------------------------------------------------------- version 1 recipes

TEST(SpriteNormalsHires, VersionOneRecipesReproduceTheOldPixels) {
    for (const char* name : {"sphere_256", "potion"}) {
        const fs::path png = kHires / "v1" / (std::string(name) + "_n.png");
        std::string error;
        const std::optional<nlohmann::json> recipe = readRecipe(png, &error);
        ASSERT_TRUE(recipe) << error;
        EXPECT_EQ((*recipe)["version"], 1);
        Settings s;
        s.version = 1;
        s.profile = Profile::Rounded;
        ASSERT_TRUE(settingsFromJson((*recipe)["settings"], s, error)) << error;
        EXPECT_EQ(s.version, 1);
        const Image color = load(kHires / (std::string(name) + ".png"));
        const Image expected = load(png);
        for (int threads : {1, 4}) EXPECT_EQ(run(color, s, threads).normal.data, expected.data) << name << " threads " << threads;
        // version 1 with the preset's profile auto: rounded (version 1 had no auto), the same bytes
        Settings fromPreset = preset(Preset::Default);
        fromPreset.version = 1;
        EXPECT_EQ(run(color, fromPreset).normal.data, expected.data) << name;
        // the same settings with version 2 differ (the sprite is above 128 px)
        Settings v2 = s;
        v2.version = 2;
        EXPECT_NE(run(color, v2).normal.data, expected.data) << name;
        // a version 1 generation writes a version 1 recipe again, settings.version 1 kept
        RecipeInput in;
        in.role = "normal";
        in.sourcePath = name;
        in.settings = s;
        const nlohmann::json again = makeRecipe(in);
        EXPECT_EQ(again["version"], 1);
        EXPECT_EQ(again["settings"]["version"], 1);
    }
    // version 2 recipes record every new setting, and no settings version
    RecipeInput in;
    in.role = "normal";
    in.sourcePath = "a.png";
    in.settings = preset(Preset::Painted);
    const nlohmann::json recipe = makeRecipe(in);
    EXPECT_EQ(recipe["version"], 2);
    EXPECT_EQ(recipe["settings"]["profile"], "inflate");
    EXPECT_EQ(recipe["settings"]["resolution_scale"], "auto");
    EXPECT_FALSE(recipe["settings"].contains("version"));
    Settings back;
    std::string error;
    ASSERT_TRUE(settingsFromJson(recipe["settings"], back, error)) << error;
    EXPECT_EQ(back.profile, Profile::Inflate);
    EXPECT_EQ(back.version, 2);
    // the names
    EXPECT_FALSE(settingsFromJson(nlohmann::json{{"profile", "pillow"}}, back, error));
    EXPECT_EQ(error, "profile \"pillow\" is not one of auto, rounded, dome, bevel, inflate");
    EXPECT_FALSE(settingsFromJson(nlohmann::json{{"resolution_scale", "on"}}, back, error));
    EXPECT_EQ(error, "resolution_scale \"on\" is not one of auto, off");
    EXPECT_FALSE(settingsFromJson(nlohmann::json{{"version", 1.5}}, back, error));
}

// ---------------------------------------------------------------- determinism

TEST(SpriteNormalsHires, ThreadCountNeverChangesTheBytes) {
    const Image potion = load(kHires / "potion.png");
    const Image cobbles = load(kHires / "cobbles_256.png");
    const Image crate = load(kHires / "crate.png");
    for (Preset p : {Preset::Default, Preset::Props, Preset::Tile}) {
        for (const Image* image : {&potion, &cobbles, &crate}) {
            Settings s = preset(p);
            s.height = true;
            const Result one = run(*image, s, 1, {}, false, true);
            for (int threads : {2, 3, 8}) {
                const Result many = run(*image, s, threads, {}, false, true);
                EXPECT_EQ(many.normal.data, one.normal.data) << toString(p) << " threads " << threads;
                EXPECT_EQ(many.shape.data, one.shape.data);
                EXPECT_EQ(many.height.data, one.height.data);
            }
        }
    }
    // a sequence of large frames (the thickness pre-pass)
    Image sheet = Image::make(600, 300, 4, 8);
    const Image a = disc(300, 300, 140.0f), b = disc(300, 300, 120.0f);
    for (int y = 0; y < 300; y++) {
        std::memcpy(sheet.pixel(0, y), a.pixel(0, y), 300 * 4);
        std::memcpy(sheet.pixel(300, y), b.pixel(0, y), 300 * 4);
    }
    const std::vector<Cell> cells = {{0, 0, 300, 300}, {300, 0, 300, 300}};
    const Result one = run(sheet, preset(Preset::Default), 1, cells, true);
    EXPECT_EQ(run(sheet, preset(Preset::Default), 5, cells, true).normal.data, one.normal.data);
    EXPECT_FALSE(one.radiusPerIsland);
}

TEST(SpriteNormalsHires, TileWrapsSeamlessly) {
    // rolling the tile rolls the map, byte for byte
    const Image tile = load(kHires / "cobbles_256.png");
    const int dx = 37, dy = 53;
    Image rolled = Image::make(256, 256, 4, 8);
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) std::memcpy(rolled.pixel((x + dx) % 256, (y + dy) % 256), tile.pixel(x, y), 4);
    }
    const Settings s = preset(Preset::Tile);
    EXPECT_EQ(s.edges, Edges::Wrap);
    const Result a = run(tile, s), b = run(rolled, s);
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) ASSERT_EQ(std::memcmp(a.normal.pixel(x, y), b.normal.pixel((x + dx) % 256, (y + dy) % 256), 4), 0) << x << "," << y;
    }
}

// ---------------------------------------------------------------- presets

TEST(SpriteNormalsHires, PresetsAndNames) {
    EXPECT_EQ(presetNames(), "soft, default, crisp, detailed, chunky, toon, painted, props, tile");
    for (Preset p : allPresets()) {
        Preset back;
        ASSERT_TRUE(parsePreset(toString(p), back));
        EXPECT_EQ(back, p);
        EXPECT_TRUE(validateSettings(presetSettings(p)).empty()) << toString(p);
        EXPECT_EQ(presetSettings(p).version, 2);
    }
    EXPECT_EQ(preset(Preset::Default).profile, Profile::Auto);
    EXPECT_EQ(preset(Preset::Toon).profile, Profile::Auto);
    EXPECT_EQ(preset(Preset::Painted).profile, Profile::Inflate);
    EXPECT_EQ(preset(Preset::Props).profile, Profile::Dome);
    EXPECT_EQ(preset(Preset::Tile).edges, Edges::Wrap);
    for (const char* name : {"auto", "rounded", "dome", "bevel", "inflate"}) {
        Profile p;
        ASSERT_TRUE(parseProfile(name, p));
        EXPECT_STREQ(toString(p), name);
    }
    ResolutionScale r;
    ASSERT_TRUE(parseResolutionScale("OFF", r));
    EXPECT_EQ(r, ResolutionScale::Off);
}

// ---------------------------------------------------------------- regression: the evaluation set (fixtures/hires)

TEST(SpriteNormalsHires, SyntheticGroundTruthImproves) {
    struct Case {
        const char* name;
        Settings now;
        double nowBelow;      // error of the new settings under
        double before;        // the version 1 default's error over (measured 8.7 / 19.1 / 25.0 / 12.9)
    };
    Settings v1Tile = versionOne();
    v1Tile.edges = Edges::Wrap;
    const Case cases[] = {
        {"sphere_256", preset(Preset::Default), 4.5, 7.5},      // measured 3.2
        {"torus_256", preset(Preset::Default), 8.0, 16.0},      // 6.2
        {"roundbox_256", preset(Preset::Props), 2.5, 20.0},     // 1.4
        {"cobbles_256", preset(Preset::Tile), 11.8, 12.4},      // 11.05
    };
    for (const Case& c : cases) {
        const Image color = load(kHires / (std::string(c.name) + ".png"));
        const Image truth = load(kHires / (std::string(c.name) + "_true.png"));
        const double now = meanError(run(color, c.now, 4).normal, truth);
        const double before = meanError(run(color, std::string(c.name) == "cobbles_256" ? v1Tile : versionOne(), 4).normal, truth);
        EXPECT_LT(now, c.nowBelow) << c.name;
        EXPECT_GT(before, c.before) << c.name;
    }
}

TEST(SpriteNormalsHires, RealSpritesHaveNoCreasesAndRoundedOnesWarn) {
    for (const char* name : {"crate", "potion", "lavapotion", "barbarian"}) {
        const Image color = load(kHires / (std::string(name) + ".png"));
        for (Preset p : {Preset::Default, Preset::Painted}) {
            const Settings s = preset(p);
            const Result r = run(color, s, 4, {}, false, true);
            const Metrics m = measure(r.normal, {}, false, s.alphaThreshold, nullptr, &r.shape);
            EXPECT_TRUE(m.highResolution) << name;
            EXPECT_GT(m.innerPixels, 1000) << name;
            EXPECT_LT(m.creasePct, 1.0) << name << " " << toString(p);       // measured 0
            EXPECT_LT(m.roughnessDeg, kStreakyDeg) << name;                    // the final map: 1.2-6.8 (the relief)
            const Assessment a = assess(s, m, 0, &r);
            EXPECT_FALSE(warns(a, "creased") || warns(a, "streaky")) << name;
        }
        // the rounded profile (and every version 1 map) above 128 px: creased, whatever the surface
        Settings rounded = preset(Preset::Default);
        rounded.profile = Profile::Rounded;
        for (const Settings& s : {rounded, versionOne()}) {
            const Result r = run(color, s, 4, {}, false, true);
            const Assessment a = assessOf(r, s);
            EXPECT_TRUE(warns(a, "creased")) << name << " v" << s.version;   // measured 4.5-23%
            EXPECT_NE(a.hint, "Looks fine.");
        }
    }
    // the props preset on the crate: flat-faced, no warning
    const Image crate = load(kHires / "crate.png");
    const Result r = run(crate, preset(Preset::Props), 4, {}, false, true);
    const Assessment a = assessOf(r, preset(Preset::Props));
    EXPECT_FALSE(warns(a, "creased") || warns(a, "streaky"));
    double centre[3];
    decode(r.normal.pixel(crate.width / 2 + 40, crate.height / 2 + 40), centre);
    EXPECT_GT(centre[2], std::cos(20.0 * kPi / 180.0)) << "a flat face (the relief only)";
}

TEST(SpriteNormalsHires, PixelArtIsNotMeasured) {
    // 128 px or less: no roughness, no crease (an 8 px inset leaves the apex), no warning
    const Image ball = disc(32, 32, 14.0f);
    const Settings s = preset(Preset::Default);
    const Result r = run(ball, s, 1, {}, false, true);
    const Metrics m = measure(r.normal, {}, false, s.alphaThreshold, nullptr, &r.shape);
    EXPECT_EQ(m.innerPixels, 0);
    EXPECT_DOUBLE_EQ(m.scale, 1.0);
    EXPECT_FALSE(warns(assess(s, m, 0, &r), "creased"));
}

TEST(SpriteNormalsHires, CancelStopsALargeSolve) {
    std::atomic<bool> cancel{true};
    Request request;
    request.color = disc(600, 600, 280.0f);
    request.settings = preset(Preset::Painted);
    Control control;
    control.cancel = &cancel;
    EXPECT_EQ(generate(request, control).status, Status::Cancelled);
    std::vector<uint8_t> mask(300 * 300, 1);
    for (int i = 0; i < 300; i++) mask[static_cast<size_t>(i)] = 0;
    std::vector<double> u;
    EXPECT_EQ(detail::poisson(mask, 300, 300, detail::Border::Zero, u, nullptr, &cancel), detail::PoissonStatus::Cancelled);
}

// ---------------------------------------------------------------- review round 1

TEST(SpriteNormalsHires, PropsKeepRingsRoundAndBoxesFlat) {
    // dome auto radius min(T, 0.15 S): a ring's tube stays round (measured 2.1 deg; 0.3 T gave 20), a box stays flat
    const Image torus = load(kHires / "torus_256.png");
    const Image box = load(kHires / "roundbox_256.png");
    const Result t = run(torus, preset(Preset::Props), 2);
    EXPECT_LT(meanError(t.normal, load(kHires / "torus_256_true.png")), 3.5);
    // the radius is the tube's thickness (its half-width, 36 px less the anti-aliased rim), under 0.15 x 256
    EXPECT_NEAR(t.radiusMax, 34.0f, 2.0f);
    EXPECT_LT(t.radiusMax, 0.15f * 256.0f);
    EXPECT_LT(meanError(run(box, preset(Preset::Props), 2).normal, load(kHires / "roundbox_256_true.png")), 2.5);
    // 0.15 S bounds it on a disc (a flat top inside the rim)
    Settings dome = preset(Preset::Props);
    EXPECT_NEAR(run(disc(300, 300, 140.0f), dome).radiusMax, 0.15f * 280.0f, 1.0f);
}

TEST(SpriteNormalsHires, ScaleFollowsTheThickness) {
    // k = max(1, 2 T / 128): a 512 px ring with a 40 px tube (T 20) is not scaled, a 300 px disc (T 140) is
    const Image ring = fromFunction(520, 520, [](int x, int y) {
        const float d = std::hypot(x + 0.5f - 260.0f, y + 0.5f - 260.0f);
        return std::array<uint8_t, 4>{150, 150, 150, static_cast<uint8_t>(std::fabs(d - 230.0f) < 20.0f ? 255 : 0)};
    });
    Result r = run(ring, preset(Preset::Default));
    EXPECT_FLOAT_EQ(r.scaleMax, 1.0f);
    EXPECT_EQ(r.profilesUsed, 1u << static_cast<int>(Profile::Inflate)) << "the gate is still the shorter side (500 > 128)";
    r = run(disc(300, 300, 140.0f), preset(Preset::Default));
    EXPECT_NEAR(r.scaleMax, 2.0f * 140.0f / 128.0f, 0.03f);
    // an opaque wrap tile has no inside distance: its shorter side (256 -> 2)
    EXPECT_FLOAT_EQ(run(load(kHires / "cobbles_256.png"), preset(Preset::Tile)).scaleMax, 2.0f);
}

TEST(SpriteNormalsHires, AHoleInGlassIsNotOutside) {
    // a disc with a translucent window (alpha 77) and one fully transparent pixel in it: the hole is
    // enclosed, so the glass stays inside the shape exactly as opaque glass with the same hole does
    auto sprite = [](uint8_t glass) {
        return fromFunction(300, 300, [=](int x, int y) {
            const float d = std::hypot(x + 0.5f - 150.0f, y + 0.5f - 150.0f);
            if (x == 150 && y == 150) return std::array<uint8_t, 4>{0, 0, 0, 0};
            if (d < 40.0f) return std::array<uint8_t, 4>{120, 160, 200, glass};
            if (d < 100.0f) return std::array<uint8_t, 4>{180, 90, 60, 255};
            return std::array<uint8_t, 4>{0, 0, 0, 0};
        });
    };
    Settings s = preset(Preset::Default);
    s.profile = Profile::Rounded;
    s.radius = 20.0f;
    s.detail = 0.0f;
    s.resolutionScale = ResolutionScale::Off;
    const Result glass = run(sprite(77), s), opaque = run(sprite(255), s);
    for (int y = 60; y < 240; y++) {
        for (int x = 60; x < 240; x++) {
            if (std::hypot(x + 0.5f - 150.0f, y + 0.5f - 150.0f) >= 96.0f) continue;
            ASSERT_EQ(std::memcmp(glass.normal.pixel(x, y), opaque.normal.pixel(x, y), 3), 0) << x << "," << y;
        }
    }
}

TEST(SpriteNormalsHires, CancelMidSolveIsCancelled) {
    // one large island: the cancel flag set while it solves gives Cancelled, never Ok with a flat shape
    std::atomic<bool> cancel{false};
    Request request;
    request.color = disc(2048, 2048, 1000.0f);
    request.settings = preset(Preset::Painted);
    Control control;
    control.cancel = &cancel;
    Result result;
    std::thread worker([&] { result = generate(request, control); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    cancel = true;
    worker.join();
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_TRUE(result.normal.empty());
}

TEST(SpriteNormalsHires, PinholeMasksConverge) {
    // a 1024 disc with a transparent pixel every 4 px: solved (no iteration cap), the residual tiny
    const int S = 1024;
    std::vector<uint8_t> mask(static_cast<size_t>(S) * S, 0);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) mask[static_cast<size_t>(y) * S + x] = std::hypot(x - 511.5, y - 511.5) < 480.0 && !(x % 4 == 0 && y % 4 == 0);
    }
    detail::Workers workers(4);
    std::vector<double> u;
    int iterations = 0;
    ASSERT_EQ(detail::poisson(mask, S, S, detail::Border::Zero, u, &workers, nullptr, &iterations), detail::PoissonStatus::Solved);
    EXPECT_LE(iterations, 20);
    double sum = 0.0;
    int64_t count = 0;
    for (int y = 1; y < S - 1; y++) {
        for (int x = 1; x < S - 1; x++) {
            const size_t i = static_cast<size_t>(y) * S + x;
            if (!mask[i]) continue;
            auto v = [&](size_t j) { return mask[j] ? u[j] : 0.0; };
            const double a = 4.0 * u[i] - v(i - 1) - v(i + 1) - v(i - S) - v(i + S);
            sum += (a - 1.0) * (a - 1.0);
            count++;
        }
    }
    EXPECT_LT(std::sqrt(sum / count), 1e-3) << "within 0.1% of the right-hand side";
}

TEST(SpriteNormalsHires, ThinHighResolutionShapesAreMeasured) {
    // a 600 px hoop with a 24 px tube: the inset is half its depth (6), not 8k: measured, rounded creases
    const Image hoop = fromFunction(620, 620, [](int x, int y) {
        const float d = std::hypot(x + 0.5f - 310.0f, y + 0.5f - 310.0f);
        return std::array<uint8_t, 4>{150, 150, 150, static_cast<uint8_t>(std::fabs(d - 280.0f) < 12.0f ? 255 : 0)};
    });
    Settings rounded = preset(Preset::Default);
    rounded.profile = Profile::Rounded;
    const Result r = run(hoop, rounded, 2, {}, false, true);
    const Metrics m = measure(r.normal, {}, false, rounded.alphaThreshold, nullptr, &r.shape);
    EXPECT_TRUE(m.highResolution);
    EXPECT_GT(m.innerPixels, 1000);
    EXPECT_GT(m.creasePct, kCreasedPct);
    const Result smooth = run(hoop, preset(Preset::Default), 2, {}, false, true);
    EXPECT_LT(measure(smooth.normal, {}, false, 0.03f, nullptr, &smooth.shape).creasePct, 1.0);
}

TEST(SpriteNormalsHires, PaintingOverACreaseClearsIt) {
    // creases are measured on the silhouette's share with the painted pixels from the map: a map
    // whose every pixel is painted flat has no crease left, whatever the shape underneath
    const Image potion = load(kHires / "potion.png");
    Settings rounded = preset(Preset::Default);
    rounded.profile = Profile::Rounded;
    const Result r = run(potion, rounded, 4, {}, false, true);
    EXPECT_GT(measure(r.normal, {}, false, 0.03f, nullptr, &r.shape).creasePct, kCreasedPct);
    PaintOverlay paint = PaintOverlay::blank(potion.width, potion.height);
    Image painted = r.normal;
    for (size_t i = 0; i < potion.pixelCount(); i++) {
        paint.rgba[i * 4 + 3] = 255;
        painted.data[i * 4] = 128;
        painted.data[i * 4 + 1] = 128;
        painted.data[i * 4 + 2] = 255;
    }
    const Metrics m = measure(painted, {}, false, 0.03f, &paint, &r.shape);
    EXPECT_LT(m.creasePct, 0.01);
    EXPECT_LT(m.roughnessDeg, 0.01) << "the final map is flat";
}

// ---------------------------------------------------------------- review round 2

TEST(SpriteNormalsHires, PaintedPatchesAreNoCreases) {
    // creases are measured on the silhouette's share with painted pixels (and 3 sigma around them)
    // left out: a flat patch on an inflated sphere is no crease; paint over a rounded crease clears it
    const Image sphere = disc(600, 600, 280.0f);
    const Result r = run(sphere, preset(Preset::Default), 4, {}, false, true);
    PaintOverlay paint = PaintOverlay::blank(600, 600);
    Image painted = r.normal;
    for (int y = 150; y < 450; y++) {
        for (int x = 150; x < 450; x++) {
            const size_t i = static_cast<size_t>(y) * 600 + x;
            paint.rgba[i * 4] = 128;
            paint.rgba[i * 4 + 1] = 128;
            paint.rgba[i * 4 + 2] = 255;
            paint.rgba[i * 4 + 3] = 255;
            painted.data[i * 4] = 128;
            painted.data[i * 4 + 1] = 128;
            painted.data[i * 4 + 2] = 255;
        }
    }
    const Metrics m = measure(painted, {}, false, 0.03f, &paint, &r.shape);
    EXPECT_LT(m.creasePct, kCreasedPct);
    EXPECT_FALSE(warns(assess(preset(Preset::Default), m, 0, &r), "creased"));
    // a rounded square's X crease painted over along both diagonals: cleared
    const Image square = fromFunction(600, 600, [](int x, int y) {
        return std::array<uint8_t, 4>{170, 120, 80, static_cast<uint8_t>(x >= 20 && x < 580 && y >= 20 && y < 580 ? 255 : 0)};
    });
    Settings rounded = preset(Preset::Default);
    rounded.profile = Profile::Rounded;
    const Result q = run(square, rounded, 4, {}, false, true);
    EXPECT_GT(measure(q.normal, {}, false, 0.03f, nullptr, &q.shape).creasePct, kCreasedPct);
    PaintOverlay strokes = PaintOverlay::blank(600, 600);
    for (int y = 0; y < 600; y++) {
        for (int x = 0; x < 600; x++) {
            if (std::abs(x - y) < 24 || std::abs(x + y - 599) < 24) strokes.rgba[(static_cast<size_t>(y) * 600 + x) * 4 + 3] = 255;
        }
    }
    EXPECT_LT(measure(q.normal, {}, false, 0.03f, &strokes, &q.shape).creasePct, kCreasedPct);
}

TEST(SpriteNormalsHires, WrapTilesDoNotDependOnTheCut) {
    // a 512 wrap tile with a soft-rimmed hole: rolling it so the hole straddles the seam rolls the
    // map (up to float summation order: at most 1 per channel on a handful of pixels)
    const int N = 512;
    auto tile = [&](int shift) {
        return fromFunction(N, N, [=](int x, int y) {
            const int sx = ((x - shift) % N + N) % N;
            const float dx = std::min(std::abs(sx - 256.0f), N - std::abs(sx - 256.0f)), dy = std::min(std::abs(y - 256.0f), N - std::abs(y - 256.0f));
            const float r = std::hypot(dx, dy);
            const uint8_t v = static_cast<uint8_t>(100 + (sx * 7 + y * 13) % 40);
            return std::array<uint8_t, 4>{v, v, v, static_cast<uint8_t>(r < 30.0f ? 0 : (r < 45.0f ? 80 : 255))};
        });
    };
    Settings s = preset(Preset::Default);
    s.edges = Edges::Wrap;
    const Result a = run(tile(0), s, 4), b = run(tile(256), s, 4);
    int differing = 0, worst = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const uint8_t* pa = a.normal.pixel(x, y);
            const uint8_t* pb = b.normal.pixel((x + 256) % N, y);
            int d = 0;
            for (int k = 0; k < 3; k++) d = std::max(d, std::abs(int(pa[k]) - int(pb[k])));
            worst = std::max(worst, d);
            differing += d > 0 ? 1 : 0;
        }
    }
    EXPECT_LE(worst, 1);
    EXPECT_LT(differing, N * N / 1000);
}

TEST(SpriteNormalsHires, AGlowInsideARingIsLikeAGlowOutside) {
    // a ring (core |r - 250| < 60) with a soft glow (alpha <= 100) on both sides: the glow inside the
    // ring's hole is outside the shape exactly as the outer one (rule C: a hole bigger than a pinhole)
    const Image ring = fromFunction(800, 800, [](int x, int y) {
        const float d = std::fabs(std::hypot(x + 0.5f - 400.0f, y + 0.5f - 400.0f) - 250.0f) - 60.0f;
        const uint8_t a = d < 0.0f ? 255 : (d < 40.0f ? static_cast<uint8_t>(100.0f * (1.0f - d / 40.0f)) : 0);
        return std::array<uint8_t, 4>{200, 200, 200, a};
    });
    Settings s = preset(Preset::Default);
    s.detail = 0.0f;
    s.height = true;
    const Result r = run(ring, s, 4);
    // the heights 20 px into each glow (r = 170, r = 330), and at the core's edges: the glows are flat
    auto heightAt = [&](int x) {
        const uint16_t v = reinterpret_cast<const uint16_t*>(r.height.data.data())[static_cast<size_t>(400) * 800 + x];
        return r.heightMin + (r.heightMax - r.heightMin) * v / 65535.0f;
    };
    EXPECT_NEAR(heightAt(400 + 170), heightAt(400 + 330), 0.05f);
    EXPECT_LT(heightAt(400 + 170), 0.05f * r.heightMax);
}

TEST(SpriteNormalsHires, TranslucentSpritesKeepTheirShape) {
    // a ghost: a 500 px disc at alpha 100, no solid core: the outer band would remove it all, so it
    // keeps its shape (as with version 1), not a flat map
    const Image ghost = disc(520, 520, 250.0f, 100);
    const Result v2 = run(ghost, preset(Preset::Default), 4, {}, false, true);
    Settings v1 = versionOne();
    const Result old = run(ghost, v1, 4);
    const Metrics m2 = measure(v2.normal, {}, false, 0.03f, nullptr, &v2.shape), m1 = measure(old.normal, {}, false, 0.03f);
    EXPECT_GT(m2.meanTiltDeg, 30.0);
    EXPECT_NEAR(m2.meanTiltDeg, m1.meanTiltDeg, 10.0);
    EXPECT_FALSE(warns(assess(preset(Preset::Default), m2, 0, &v2), "mostly_flat"));
}

TEST(SpriteNormalsHires, MetricsDoNotDependOnTheirThreads) {
    const Image potion = load(kHires / "potion.png");
    Settings rounded = preset(Preset::Default);
    rounded.profile = Profile::Rounded;
    const Result r = run(potion, rounded, 4, {}, false, true);
    const Metrics a = measure(r.normal, {}, false, 0.03f, nullptr, &r.shape, 1);
    const Metrics b = measure(r.normal, {}, false, 0.03f, nullptr, &r.shape, 7);
    EXPECT_EQ(a.roughnessDeg, b.roughnessDeg);
    EXPECT_EQ(a.creasePct, b.creasePct);
    EXPECT_EQ(a.noisePct, b.noisePct);
    EXPECT_EQ(a.meanTiltDeg, b.meanTiltDeg);
}

// ---------------------------------------------------------------- final polish

TEST(SpriteNormalsHires, HoleThresholdIsTheIslandsInEveryEdgesMode) {
    // a 330 px ring (core 45..150) with soft glows (alpha 70) inside and outside, on a 1024 canvas:
    // the 40 px hole is deeper than 5% of the ring (16.5), not of the canvas (51): its glow is flat
    // like the outer glow with transparent, extend and wrap edges
    const Image ring = fromFunction(1024, 1024, [](int x, int y) {
        const float r = std::hypot(x - 512.0f, y - 512.0f);
        const uint8_t a = r >= 45.0f && r < 150.0f ? 255 : ((r >= 20.0f && r < 45.0f) || (r >= 150.0f && r < 165.0f) ? 70 : 0);
        return std::array<uint8_t, 4>{200, 200, 200, a};
    });
    for (Edges e : {Edges::Transparent, Edges::Extend, Edges::Wrap}) {
        Settings s = preset(Preset::Default);
        s.detail = 0.0f;
        s.height = true;
        s.edges = e;
        const Result r = run(ring, s, 4);
        auto heightAt = [&](int x) {
            const uint16_t v = reinterpret_cast<const uint16_t*>(r.height.data.data())[static_cast<size_t>(512) * 1024 + x];
            return r.heightMin + (r.heightMax - r.heightMin) * v / 65535.0f;
        };
        EXPECT_LT(heightAt(512 + 32), 0.01f * r.heightMax) << toString(e) << " inner glow";
        EXPECT_LT(heightAt(512 + 157), 0.01f * r.heightMax) << toString(e) << " outer glow";
        EXPECT_GT(heightAt(512 + 97), 0.5f * r.heightMax) << toString(e) << " core";
    }
}

TEST(SpriteNormalsHires, SparsePaintLeavesCreasesUnmeasured) {
    // 81 painted dots on a rounded square: their exclusion bands cover most of it, so the creases are
    // not measured (no warning, a line saying why), not reported as 0
    const Image square = fromFunction(600, 600, [](int x, int y) {
        return std::array<uint8_t, 4>{170, 120, 80, static_cast<uint8_t>(x >= 20 && x < 580 && y >= 20 && y < 580 ? 255 : 0)};
    });
    Settings rounded = preset(Preset::Default);
    rounded.profile = Profile::Rounded;
    const Result r = run(square, rounded, 4, {}, false, true);
    PaintOverlay dots = PaintOverlay::blank(600, 600);
    for (int j = 0; j < 9; j++) {
        for (int i = 0; i < 9; i++) dots.rgba[(static_cast<size_t>(40 + 60 * j) * 600 + 40 + 60 * i) * 4 + 3] = 255;
    }
    const Metrics m = measure(r.normal, {}, false, 0.03f, &dots, &r.shape);
    EXPECT_FALSE(m.creaseMeasured);
    EXPECT_EQ(m.creasePct, 0.0);
    const Assessment a = assess(rounded, m, 0, &r);
    EXPECT_FALSE(warns(a, "creased"));
    EXPECT_NE(std::find(a.lines.begin(), a.lines.end(), "Creases not measured: paint covers most of the sprite"), a.lines.end());
    // one painted corner patch leaves most of it measured: still creased
    PaintOverlay corner = PaintOverlay::blank(600, 600);
    for (int y = 250; y < 350; y++) {
        for (int x = 30; x < 60; x++) corner.rgba[(static_cast<size_t>(y) * 600 + x) * 4 + 3] = 255;
    }
    const Metrics c = measure(r.normal, {}, false, 0.03f, &corner, &r.shape);
    EXPECT_TRUE(c.creaseMeasured);
    EXPECT_GT(c.creasePct, kCreasedPct);
}

TEST(SpriteNormalsHires, SoftEdgeSolidAndTheMostlyFlatHint) {
    // the cliff cases: a glass disc (alpha 80, a solid 20 px rim) with a 25 px clear hole (deep enough
    // to be outside: the glass becomes outer band), and a translucent disc with a 10% solid core.
    // Flat, and the hint names soft_edge "solid"; with it they are round again.
    const Image glass = fromFunction(600, 600, [](int x, int y) {
        const float r = std::hypot(x - 300.0f, y - 300.0f);
        if (std::hypot(x - 300.0f, y - 200.0f) < 25.0f) return std::array<uint8_t, 4>{180, 180, 180, 0};
        return std::array<uint8_t, 4>{180, 180, 180, static_cast<uint8_t>(r < 230.0f ? 80 : (r < 250.0f ? 255 : 0))};
    });
    const Image core = fromFunction(600, 600, [](int x, int y) {
        const float r = std::hypot(x - 300.0f, y - 300.0f);
        return std::array<uint8_t, 4>{180, 180, 180, static_cast<uint8_t>(r < 80.0f ? 255 : (r < 250.0f ? 100 : 0))};
    });
    for (const Image* image : {&glass, &core}) {
        Settings s = preset(Preset::Default);
        s.detail = 0.0f;
        const Result r = run(*image, s, 4, {}, false, true);
        EXPECT_GT(r.outerBandShare, 0.5f);
        const Assessment a = assessOf(r, s);
        ASSERT_TRUE(warns(a, "mostly_flat"));
        EXPECT_NE(a.hint.find("soft_edge \"solid\""), std::string::npos) << a.hint;
        s.softEdge = SoftEdge::Solid;
        const Result solid = run(*image, s, 4, {}, false, true);
        EXPECT_FLOAT_EQ(solid.outerBandShare, 0.0f);
        const Metrics m = measure(solid.normal, {}, false, 0.03f, nullptr, &solid.shape);
        EXPECT_GT(m.meanTiltDeg, 30.0);
        EXPECT_FALSE(warns(assess(s, m, 0, &solid), "mostly_flat"));
    }
    // solid changes only the silhouette: an image without soft pixels is byte-identical
    Settings a = preset(Preset::Default), b = a;
    b.softEdge = SoftEdge::Solid;
    const Image hard = disc(300, 300, 140.0f);
    EXPECT_EQ(run(hard, a).normal.data, run(hard, b).normal.data);
    // the names and the recipe
    SoftEdge e;
    ASSERT_TRUE(parseSoftEdge("Solid", e));
    EXPECT_EQ(e, SoftEdge::Solid);
    EXPECT_FALSE(parseSoftEdge("hard", e));
    EXPECT_EQ(settingsToJson(b)["soft_edge"], "solid");
    Settings back;
    std::string error;
    ASSERT_TRUE(settingsFromJson(settingsToJson(b), back, error)) << error;
    EXPECT_EQ(back.softEdge, SoftEdge::Solid);
    EXPECT_FALSE(settingsFromJson(nlohmann::json{{"soft_edge", "hard"}}, back, error));
    EXPECT_EQ(error, "soft_edge \"hard\" is not one of auto, solid");
}
