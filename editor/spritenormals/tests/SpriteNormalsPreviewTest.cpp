// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU preview of sprite normal maps (SpriteNormalsPreview, Phase 2 spec 7): renderLit
// against values worked out by hand from the engine's getLight2DIntensity (lighting2d.glsl:
// OpenGL green, N.L with the light's height), orbitLight, the contact sheet's layout rules,
// the metrics on synthetic maps and the assessment's lines and thresholds.

#include <gtest/gtest.h>

#include "spritenormals/SpriteNormalsPreview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sn = jasperengine::editor::spritenormals;

namespace {

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

    std::vector<int> px(const sn::Image& image, int x, int y) {
        const uint8_t* p = image.pixel(x, y);
        return {p[0], p[1], p[2], p[3]};
    }

    // One light at (x, y) height 10, range 1000, no ambient
    sn::LightSetup oneLight(float x, float y, bool directx = false) {
        sn::LightSetup setup;
        sn::PreviewLight light;
        light.x = x;
        light.y = y;
        light.height = 10.0f;
        light.range = 1000.0f;
        setup.lights.push_back(light);
        setup.ambientIntensity = 0.0f;
        setup.directx = directx;
        return setup;
    }

    // A hemisphere of radius r (outward normals, OpenGL green up) in a size x size map
    sn::Image dome(int size, float r) {
        sn::Image image = solid(size, size, 128, 128, 255, 0);
        const float c = size * 0.5f;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const float dx = (x + 0.5f - c) / r, dy = -(y + 0.5f - c) / r;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= 1.0f) continue;
                uint8_t* p = image.pixel(x, y);
                const float n[3] = {dx, dy, std::sqrt(1.0f - d2)};
                for (int k = 0; k < 3; k++) p[k] = static_cast<uint8_t>(std::lround((n[k] * 0.5f + 0.5f) * 255.0f));
                p[3] = 255;
            }
        }
        return image;
    }

}

// ---- renderLit

TEST(SpriteNormalsPreview, FlatNormalUnderALightOverItKeepsTheColour) {
    const sn::Image color = solid(1, 1, 200, 100, 50, 255);
    const sn::Image normal = solid(1, 1, 128, 128, 255, 255);
    // straight above: N.L = 1 (to within the neutral texel's 1/255 tilt), attenuation 1
    EXPECT_EQ(px(sn::renderLit(normal, color, oneLight(0.5f, 0.5f)), 0, 0), (std::vector<int>{200, 100, 50, 255}));
    // ambient alone (0.15): pow(pow(c, 2.2) * 0.15, 1/2.2)
    sn::LightSetup ambient = oneLight(0.5f, 0.5f);
    ambient.lights.clear();
    ambient.ambientIntensity = 0.15f;
    EXPECT_EQ(px(sn::renderLit(normal, color, ambient), 0, 0), (std::vector<int>{84, 42, 21, 255}));
}

TEST(SpriteNormalsPreview, TiltedNormalFacesItsLight) {
    const sn::Image color = solid(1, 1, 200, 100, 50, 255);
    // 45 degrees to the right: (218, 128, 218)
    const sn::Image right = solid(1, 1, 218, 128, 218, 255);
    // light 10 px to the right at height 10: N.L ~ 1, attenuation 1 - 10/1000
    EXPECT_EQ(px(sn::renderLit(right, color, oneLight(10.5f, 0.5f)), 0, 0), (std::vector<int>{199, 100, 50, 255}));
    // to the left: N.L = 0, no ambient: black
    EXPECT_EQ(px(sn::renderLit(right, color, oneLight(-9.5f, 0.5f)), 0, 0), (std::vector<int>{0, 0, 0, 255}));

    // green up the image (OpenGL) faces a light above it (smaller image y), not one below
    const sn::Image up = solid(1, 1, 128, 218, 218, 255);
    EXPECT_EQ(px(sn::renderLit(up, color, oneLight(0.5f, -9.5f)), 0, 0), (std::vector<int>{199, 100, 50, 255}));
    EXPECT_EQ(px(sn::renderLit(up, color, oneLight(0.5f, 10.5f)), 0, 0), (std::vector<int>{0, 0, 0, 255}));
    // the same bytes read as DirectX point down: the response flips
    EXPECT_EQ(px(sn::renderLit(up, color, oneLight(0.5f, -9.5f, true)), 0, 0), (std::vector<int>{0, 0, 0, 255}));
    EXPECT_EQ(px(sn::renderLit(up, color, oneLight(0.5f, 10.5f, true)), 0, 0), (std::vector<int>{199, 100, 50, 255}));

    // beyond the range: the light adds nothing
    sn::LightSetup far = oneLight(10.5f, 0.5f);
    far.lights[0].range = 10.0f;
    EXPECT_EQ(px(sn::renderLit(right, color, far), 0, 0), (std::vector<int>{0, 0, 0, 255}));
    // height 0 is the engine's pure radial light: N.L ignored
    sn::LightSetup radial = oneLight(-9.5f, 0.5f);
    radial.lights[0].height = 0.0f;
    EXPECT_EQ(px(sn::renderLit(right, color, radial), 0, 0), (std::vector<int>{199, 100, 50, 255}));
}

TEST(SpriteNormalsPreview, PixelsAtTheCutoffStayTransparent) {
    sn::Image color = solid(2, 1, 200, 100, 50, 7);   // 7/255 = 0.027 <= 0.03
    color.pixel(1, 0)[3] = 8;                           // 8/255 = 0.031 > 0.03
    const sn::Image normal = solid(2, 1, 128, 128, 255, 255);
    const sn::Image lit = sn::renderLit(normal, color, oneLight(1.0f, 0.5f));
    EXPECT_EQ(px(lit, 0, 0), (std::vector<int>{0, 0, 0, 0}));
    EXPECT_EQ(px(lit, 1, 0)[3], 8) << "alpha is the colour's";
    EXPECT_GT(px(lit, 1, 0)[0], 150);
    // mismatched sizes: nothing
    EXPECT_TRUE(sn::renderLit(solid(3, 1, 0, 0, 0, 255), color, oneLight(0, 0)).empty());
}

TEST(SpriteNormalsPreview, OrbitLightPositions) {
    sn::LightSetup s = sn::orbitLight(32, 16, 0.0f, true, 0.1f);
    ASSERT_EQ(s.lights.size(), 1u);
    EXPECT_NEAR(s.lights[0].x, 16.0f + 24.0f, 1e-4);
    EXPECT_NEAR(s.lights[0].y, 8.0f, 1e-4);
    EXPECT_FLOAT_EQ(s.lights[0].height, 16.0f);
    EXPECT_FLOAT_EQ(s.lights[0].range, 96.0f);
    EXPECT_TRUE(s.directx);
    EXPECT_FLOAT_EQ(s.alphaThreshold, 0.1f);
    s = sn::orbitLight(32, 16, 90.0f);
    EXPECT_NEAR(s.lights[0].x, 16.0f, 1e-4);
    EXPECT_NEAR(s.lights[0].y, 8.0f - 24.0f, 1e-4) << "90 = up the image";
    s = sn::orbitLight(32, 16, 225.0f);
    EXPECT_LT(s.lights[0].x, 16.0f);
    EXPECT_GT(s.lights[0].y, 8.0f) << "225 = lower left";
}

// ---- contact sheet

TEST(SpriteNormalsPreview, SheetFramesSpreadOverTheAnimation) {
    EXPECT_EQ(sn::sheetFrames(3, 8), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(sn::sheetFrames(20, 8), (std::vector<int>{0, 3, 5, 8, 11, 14, 16, 19}));
    EXPECT_EQ(sn::sheetFrames(9, 8).front(), 0);
    EXPECT_EQ(sn::sheetFrames(9, 8).back(), 8);
    EXPECT_TRUE(sn::sheetFrames(0, 8).empty());
}

TEST(SpriteNormalsPreview, ContactSheetSizeRules) {
    // one 32x32 frame: s = min(8, (1024 - 20) / 192 = 5, 1024 / 32) = 5
    sn::Image sheet = sn::contactSheet(solid(32, 32, 255, 0, 0, 255), solid(32, 32, 128, 128, 255, 255), {}, false, 0.03f);
    EXPECT_EQ(sheet.width, 6 * 160 + 20);
    EXPECT_EQ(sheet.height, 160);
    EXPECT_EQ(sheet.channels, 4);

    // four frames of a 64x64 sheet: s = min(8, 5, (1024 - 12) / 128 = 7) = 5
    std::vector<sn::Cell> four = {{0, 0, 32, 32}, {32, 0, 32, 32}, {0, 32, 32, 32}, {32, 32, 32, 32}};
    sheet = sn::contactSheet(solid(64, 64, 255, 0, 0, 255), solid(64, 64, 128, 128, 255, 255), four, false, 0.03f);
    EXPECT_EQ(sheet.width, 980);
    EXPECT_EQ(sheet.height, 4 * 160 + 12);

    // twenty frames: 8 shown, s = min(8, 5, (1024 - 28) / 256 = 3) = 3
    std::vector<sn::Cell> twenty;
    for (int i = 0; i < 20; i++) twenty.push_back({i * 32, 0, 32, 32});
    sheet = sn::contactSheet(solid(640, 32, 0, 255, 0, 255), solid(640, 32, 128, 128, 255, 255), twenty, false, 0.03f);
    EXPECT_EQ(sheet.width, 6 * 96 + 20);
    EXPECT_EQ(sheet.height, 8 * 96 + 28);

    // a 2000x10 strip: scaled down (every 12th pixel: 167 px tiles) to fit 1024
    sheet = sn::contactSheet(solid(2000, 10, 0, 0, 255, 255), solid(2000, 10, 128, 128, 255, 255), {}, false, 0.03f);
    EXPECT_LE(sheet.width, 1024);
    EXPECT_EQ(sheet.width, 6 * 167 + 20);
    EXPECT_EQ(sheet.height, 1);

    // smaller limits are honoured
    sn::SheetOptions small;
    small.maxWidth = 200;
    small.maxHeight = 100;
    small.maxScale = 2;
    sheet = sn::contactSheet(solid(32, 32, 255, 0, 0, 255), solid(32, 32, 128, 128, 255, 255), {}, false, 0.03f, small);
    EXPECT_LE(sheet.width, 200);
    EXPECT_LE(sheet.height, 100);
}

TEST(SpriteNormalsPreview, ContactSheetCheckerboardAndTiles) {
    // a fully transparent 16x16 frame: the checkerboard shows through every tile
    sn::Image sheet = sn::contactSheet(solid(16, 16, 255, 255, 255, 0), solid(16, 16, 128, 128, 255, 0), {}, false, 0.03f);
    EXPECT_EQ(px(sheet, 0, 0), (std::vector<int>{0x2a, 0x2a, 0x30, 255}));
    EXPECT_EQ(px(sheet, 8, 0), (std::vector<int>{0x22, 0x22, 0x27, 255}));
    EXPECT_EQ(px(sheet, 8, 8), (std::vector<int>{0x2a, 0x2a, 0x30, 255}));
    for (size_t i = 3; i < sheet.data.size(); i += 4) ASSERT_EQ(sheet.data[i], 255) << "opaque";

    // an opaque red frame: tile 0 is red, tile 1 the normal as stored, the lit tiles lit from
    // their corner (upper left brighter at its upper left than at its lower right)
    const sn::Image color = solid(16, 16, 255, 0, 0, 255);
    sheet = sn::contactSheet(color, solid(16, 16, 128, 128, 255, 255), {}, false, 0.03f);
    const int s = 8;   // min(8, 1004 / 96 = 10, 1024 / 16 = 64)
    EXPECT_EQ(sheet.width, 6 * 16 * s + 20);
    EXPECT_EQ(px(sheet, 3, 3), (std::vector<int>{255, 0, 0, 255}));
    EXPECT_EQ(px(sheet, 16 * s + 4 + 5, 5), (std::vector<int>{128, 128, 255, 255}));
    const int lit135 = 2 * (16 * s + 4);
    EXPECT_GT(px(sheet, lit135 + 2, 2)[0], px(sheet, lit135 + 16 * s - 3, 16 * s - 3)[0]);
    const int lit315 = 4 * (16 * s + 4);
    EXPECT_LT(px(sheet, lit315 + 2, 2)[0], px(sheet, lit315 + 16 * s - 3, 16 * s - 3)[0]);
}

// ---- metrics

TEST(SpriteNormalsPreview, MetricsOfSyntheticMaps) {
    // all flat and opaque
    sn::Metrics m = sn::measure(solid(16, 16, 128, 128, 255, 255), {}, false, 0.03f);
    EXPECT_DOUBLE_EQ(m.flatPct, 100.0);
    EXPECT_DOUBLE_EQ(m.coveragePct, 100.0);
    EXPECT_DOUBLE_EQ(m.noisePct, 0.0);
    EXPECT_DOUBLE_EQ(m.steepPct, 0.0);
    EXPECT_LT(m.leanDeg, 1.0);
    EXPECT_FALSE(m.hasTransparency);
    EXPECT_EQ(m.opaquePixels, 256);

    // a hemisphere: mean tilt pi/4 (45 degrees) in the continuum, no lean, some transparency
    m = sn::measure(dome(64, 28.0f), {}, false, 0.03f);
    EXPECT_GT(m.meanTiltDeg, 38.0);
    EXPECT_LT(m.meanTiltDeg, 52.0);
    EXPECT_LT(m.leanDeg, 1.0);
    EXPECT_TRUE(m.hasTransparency);
    EXPECT_NEAR(m.coveragePct, 100.0 * 3.14159 * 28 * 28 / (64 * 64), 2.0);
    EXPECT_LT(m.noisePct, 2.0);
    EXPECT_GT(m.steepPct, 0.0);

    // noise: every other pixel tilted hard the other way
    sn::Image noisy = solid(16, 16, 128, 128, 255, 255);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint8_t* p = noisy.pixel(x, y);
            const bool odd = ((x + y) % 2) != 0;
            p[0] = odd ? 240 : 16;
            p[2] = 160;
        }
    }
    m = sn::measure(noisy, {}, false, 0.03f);
    EXPECT_GT(m.noisePct, 80.0);

    // a lean: every pixel tilted 20 degrees to the right
    const float t = 20.0f * 3.14159265f / 180.0f;
    sn::Image leaning = solid(8, 8, static_cast<uint8_t>(std::lround((std::sin(t) * 0.5f + 0.5f) * 255.0f)), 128,
                              static_cast<uint8_t>(std::lround((std::cos(t) * 0.5f + 0.5f) * 255.0f)), 255);
    m = sn::measure(leaning, {}, false, 0.03f);
    EXPECT_NEAR(m.leanDeg, 20.0, 1.0);
    EXPECT_DOUBLE_EQ(m.flatPct, 0.0);

    // soft edges, and cells touching their edges
    sn::Image cells = solid(16, 8, 128, 128, 255, 0);
    for (int y = 2; y < 6; y++) {
        for (int x = 2; x < 6; x++) cells.pixel(x, y)[3] = 255;   // cell 0: inside
        for (int x = 8; x < 12; x++) cells.pixel(x, y)[3] = 128;  // cell 1: touches its left edge, soft
    }
    m = sn::measure(cells, {{0, 0, 8, 8}, {8, 0, 8, 8}}, false, 0.03f);
    EXPECT_EQ(m.framesTouchingEdges, 1);
    EXPECT_DOUBLE_EQ(m.softEdgePct, 50.0);
    EXPECT_DOUBLE_EQ(m.coveragePct, 100.0 * 32 / 128);
}

// ---- assessment

TEST(SpriteNormalsPreview, AssessLines) {
    sn::Metrics m;
    m.meanTiltDeg = 24.4;
    m.flatPct = 12.2;
    m.steepPct = 3.0;
    m.noisePct = 1.0;
    m.leanDeg = 2.0;
    m.coveragePct = 40.0;
    m.hasTransparency = true;
    m.framesTouchingEdges = 0;
    sn::Assessment a = sn::assess(sn::Settings(), m, 0);
    EXPECT_EQ(a.lines, (std::vector<std::string>{"Shape: auto (mean tilt 24°, 12% flat)", "Steep pixels: 3%", "Noise: 1%", "Lean: 2°",
                                                 "Coverage: 40%"}));
    EXPECT_TRUE(a.warnings.empty());
    EXPECT_EQ(a.hint, "Looks fine.");
    m.framesTouchingEdges = 2;
    a = sn::assess(sn::Settings(), m, 8);
    ASSERT_EQ(a.lines.size(), 6u);
    EXPECT_EQ(a.lines[5], "Frames: 8 (2 touch their cell's edge)");
}

TEST(SpriteNormalsPreview, AssessWarningThresholds) {
    auto kinds = [](const sn::Assessment& a) {
        std::vector<std::string> out;
        for (const sn::Problem& p : a.warnings) out.push_back(p.kind);
        return out;
    };
    sn::Metrics calm;
    calm.hasTransparency = true;
    sn::Settings settings;
    settings.strength = 2.0f;
    struct Case {
        const char* kind;
        void (*set)(sn::Metrics&, bool above);
    };
    const Case cases[] = {
        {"mostly_flat", [](sn::Metrics& m, bool above) { m.flatPct = above ? 60.5 : 60.0; }},
        {"too_steep", [](sn::Metrics& m, bool above) { m.steepPct = above ? 25.5 : 25.0; }},
        {"noisy", [](sn::Metrics& m, bool above) { m.noisePct = above ? 15.1 : 15.0; }},
        {"leaning", [](sn::Metrics& m, bool above) { m.leanDeg = above ? 8.1 : 8.0; }},
        {"soft_edges", [](sn::Metrics& m, bool above) { m.softEdgePct = above ? 10.2 : 10.0; }},
        {"frames_touch_edges", [](sn::Metrics& m, bool above) { m.framesTouchingEdges = above ? 1 : 0; }},
        {"no_alpha", [](sn::Metrics& m, bool above) { m.hasTransparency = !above; }},
        // high resolution (hires design spec, rule 6): measured above 128 px only
        {"creased", [](sn::Metrics& m, bool above) { m.highResolution = true; m.scale = 4.0; m.innerPixels = 100; m.creasePct = above ? 2.1 : 2.0; }},
        {"streaky", [](sn::Metrics& m, bool above) { m.highResolution = true; m.scale = 4.0; m.innerPixels = 100; m.roughnessDeg = above ? 13.1 : 13.0; }},
    };
    for (const Case& c : cases) {
        sn::Metrics m = calm;
        c.set(m, false);
        EXPECT_TRUE(sn::assess(settings, m, 4).warnings.empty()) << c.kind << " just below";
        c.set(m, true);
        const sn::Assessment a = sn::assess(settings, m, 4);
        EXPECT_EQ(kinds(a), std::vector<std::string>{c.kind}) << c.kind << " just above";
        ASSERT_FALSE(a.warnings.empty());
        EXPECT_EQ(a.hint, a.warnings[0].fix);
        EXPECT_TRUE(a.warnings[0].field.empty());
    }
    // exact wording
    sn::Metrics m = calm;
    m.steepPct = 31.0;
    sn::Assessment a = sn::assess(settings, m, 0);
    ASSERT_EQ(a.warnings.size(), 1u);
    EXPECT_EQ(a.warnings[0].message, "31% of the pixels are steeper than 60°.");
    EXPECT_EQ(a.warnings[0].fix, "Lower strength (try 1.2), or raise smoothing.");
    m = calm;
    m.leanDeg = 12.4;
    EXPECT_EQ(sn::assess(settings, m, 0).warnings[0].message, "The map leans 12° on average: the painted light direction became a slope.");
    m = calm;
    m.framesTouchingEdges = 3;
    EXPECT_EQ(sn::assess(settings, m, 4).warnings[0].message, "3 frames touch their cell's edge, which gets a rim there.");
    // wrap / extend edges: no_alpha and frames_touch_edges do not apply
    m = calm;
    m.hasTransparency = false;
    m.framesTouchingEdges = 2;
    settings.edges = sn::Edges::Extend;
    EXPECT_TRUE(sn::assess(settings, m, 4).warnings.empty());
    // the hint is the first warning's fix in table order
    settings.edges = sn::Edges::Transparent;
    m.flatPct = 90.0;
    a = sn::assess(settings, m, 4);
    EXPECT_EQ(kinds(a), (std::vector<std::string>{"no_alpha", "mostly_flat", "frames_touch_edges"}));
    // (S78: the fix names the stones preset for floors between dark gaps; deliberately changed)
    EXPECT_EQ(a.hint, "For tiles use edges wrap or extend; for stones or bricks between dark gaps the stones preset (gaps).");
}

TEST(SpriteNormalsPreview, AssessHighResolution) {
    sn::Metrics m;
    m.hasTransparency = true;
    m.highResolution = true;
    m.scale = 8.0;
    m.innerPixels = 5000;
    m.roughnessDeg = 14.04;
    m.creasePct = 14.93;
    sn::Settings settings;   // auto profile
    sn::Result result;
    result.profilesUsed = (1u << static_cast<int>(sn::Profile::Rounded)) | (1u << static_cast<int>(sn::Profile::Inflate));
    sn::Assessment a = sn::assess(settings, m, 0, &result);
    EXPECT_EQ(a.lines[0], "Shape: auto -> rounded, inflate (mean tilt 0°, 0% flat)");
    EXPECT_EQ(a.lines[4], "Roughness: 14.0° (creases 14.9%)");
    // a crease outranks streaks (one warning, the crease's fix)
    ASSERT_EQ(a.warnings.size(), 1u);
    EXPECT_EQ(a.warnings[0].kind, "creased");
    EXPECT_EQ(a.warnings[0].message, "14.9% of the inside lies on sharp creases (ridges down the middle of the shapes).");
    EXPECT_EQ(a.hint, "Use profile inflate (round art) or dome (flat-faced props), or the painted or props preset, or raise softness.");
    m.creasePct = 0.5;
    a = sn::assess(settings, m, 0, &result);
    ASSERT_EQ(a.warnings.size(), 1u);
    EXPECT_EQ(a.warnings[0].kind, "streaky");
    EXPECT_EQ(a.warnings[0].message,
              "The light is bumpy inside the sprite (roughness 14.0° on the map): streaks from a wobbly outline, or brushwork and paint turned into bumps.");
    EXPECT_EQ(a.hint, "Raise smoothing (brushwork, painted detail) or lower detail; for streaks from the outline raise softness or use profile inflate.");
    // not measured (128 px or less): no line, no warning
    m.innerPixels = 0;
    m.roughnessDeg = 0.0;
    m.creasePct = 0.0;
    m.scale = 1.0;
    m.highResolution = false;
    a = sn::assess(settings, m, 0);
    EXPECT_EQ(a.lines.size(), 5u);
    EXPECT_TRUE(a.warnings.empty());
    // soft edges above 128 px with version 2: the outer band handles them (no warning); version 1 still warns
    m.softEdgePct = 30.0;
    m.highResolution = true;   // (k may be 1 on thin art: the gate is the size)
    EXPECT_TRUE(sn::assess(settings, m, 0).warnings.empty());
    settings.version = 1;
    ASSERT_EQ(sn::assess(settings, m, 0).warnings.size(), 1u);
    EXPECT_EQ(sn::assess(settings, m, 0).warnings[0].kind, "soft_edges");
}

// ---- Light Steps (S78 R2, R19): the runtime's per-light bands, on the CPU

TEST(SpriteNormalsPreview, LightStepFormula) {
    // ceil(t * N) / N; 0 stays 0; off (0) leaves t
    EXPECT_EQ(sn::lightStep(0.0f, 6), 0.0f);
    EXPECT_EQ(sn::lightStep(0.37f, 0), 0.37f);
    EXPECT_FLOAT_EQ(sn::lightStep(0.01f, 6), 1.0f / 6.0f);
    EXPECT_FLOAT_EQ(sn::lightStep(0.5f, 4), 0.5f);
    EXPECT_FLOAT_EQ(sn::lightStep(0.51f, 4), 0.75f);
    EXPECT_FLOAT_EQ(sn::lightStep(1.0f, 16), 1.0f);
    EXPECT_FLOAT_EQ(sn::lightStep(0.2f, 1), 1.0f);
    for (int n = 1; n <= sn::kMaxLightSteps; n++) {
        for (int i = 0; i <= 100; i++) {
            const float t = static_cast<float>(i) / 100.0f;
            EXPECT_EQ(sn::lightStep(t, n), std::ceil(t * static_cast<float>(n)) / static_cast<float>(n)) << t << " " << n;
        }
    }
}

TEST(SpriteNormalsPreview, RenderLitStepsEachLightBeforeItsColourAndIntensity) {
    const sn::Image color = solid(1, 1, 200, 100, 50, 255);
    const sn::Image normal = solid(1, 1, 128, 128, 255, 255);
    // one light 50 px to the side at height 10, range 200, intensity 0.8, colour (1, 0.5, 0.25), ambient 0.1
    sn::LightSetup setup = oneLight(50.5f, 0.5f);
    setup.lights[0].range = 200.0f;
    setup.lights[0].intensity = 0.8f;
    setup.lights[0].green = 0.5f;
    setup.lights[0].blue = 0.25f;
    setup.ambientIntensity = 0.1f;
    auto expected = [&](int steps) {
        float n[3] = {128 / 255.0f * 2.0f - 1.0f, 128 / 255.0f * 2.0f - 1.0f, 255 / 255.0f * 2.0f - 1.0f};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& v : n) v /= len;
        const float tx = 50.0f, ty = 0.0f, d = 50.0f;
        const float atten = std::pow(1.0f - d / 200.0f, 1.0f);
        const float ndotl = std::clamp((n[0] * tx + n[1] * ty + n[2] * 10.0f) / std::sqrt(tx * tx + ty * ty + 100.0f), 0.0f, 1.0f);
        float t = atten * ndotl;
        if (steps > 0) t = std::ceil(t * static_cast<float>(steps)) / static_cast<float>(steps);
        const float k = 0.8f * t;
        const float light[3] = {0.1f + 1.0f * k, 0.1f + 0.5f * k, 0.1f + 0.25f * k};
        std::vector<int> out;
        const uint8_t c[3] = {200, 100, 50};
        for (int ch = 0; ch < 3; ch++) out.push_back(static_cast<int>(std::lround(std::pow(std::clamp(std::pow(c[ch] / 255.0f, 2.2f) * light[ch], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f)));
        out.push_back(255);
        return out;
    };
    for (int steps : {0, 1, 3, 6, 16}) {
        setup.lightSteps = steps;
        EXPECT_EQ(px(sn::renderLit(normal, color, setup), 0, 0), expected(steps)) << steps;
    }
    // off is the formula before Light Steps (the same bytes as a setup that never heard of them)
    setup.lightSteps = 0;
    const sn::Image before = sn::renderLit(normal, color, setup);
    setup.lightSteps = 6;
    EXPECT_NE(px(sn::renderLit(normal, color, setup), 0, 0), px(before, 0, 0));
}

TEST(SpriteNormalsPreview, LightStepsBandTheLitDome) {
    // a hemisphere lit from the side: N steps give at most N + 1 levels (the bands and unlit), the
    // ambient (unstepped) under all of them
    const sn::Image normal = dome(64, 30.0f);
    const sn::Image color = solid(64, 64, 200, 200, 200, 255);
    sn::LightSetup setup = sn::orbitLight(64, 64, 135.0f);
    auto levels = [&](int steps) {
        setup.lightSteps = steps;
        const sn::Image lit = sn::renderLit(normal, color, setup);
        std::vector<int> seen;
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                if (normal.pixel(x, y)[3] == 0) continue;
                const int v = lit.pixel(x, y)[0];
                if (std::find(seen.begin(), seen.end(), v) == seen.end()) seen.push_back(v);
            }
        }
        return seen.size();
    };
    EXPECT_GT(levels(0), 40u);
    for (int steps : {1, 4, 6}) {
        EXPECT_LE(levels(steps), static_cast<size_t>(steps + 1)) << steps;
        EXPECT_GE(levels(steps), 2u) << steps;
    }
    // the contact sheet's lit columns take the steps
    sn::SheetOptions options;
    const sn::Image smooth = sn::contactSheet(color, normal, {}, false, 0.03f, options);
    options.lightSteps = 4;
    const sn::Image stepped = sn::contactSheet(color, normal, {}, false, 0.03f, options);
    ASSERT_EQ(smooth.width, stepped.width);
    EXPECT_NE(smooth.data, stepped.data);
}

// ---- boxy (S78 R13)

namespace {

    sn::Image boxImage(int size, int margin, bool circle) {
        sn::Image image = solid(size + 2 * margin, size + 2 * margin, 0, 0, 0, 0);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const float dx = x + 0.5f - size * 0.5f, dy = y + 0.5f - size * 0.5f;
                if (circle && dx * dx + dy * dy > size * size * 0.25f) continue;
                uint8_t* p = image.pixel(margin + x, margin + y);
                p[0] = 150, p[1] = 110, p[2] = 70, p[3] = 255;
            }
        }
        return image;
    }

    std::vector<std::string> warningKinds(const sn::Settings& s, const sn::Image& image, sn::Result* out = nullptr) {
        sn::Request request;
        request.color = image;
        request.settings = s;
        request.threads = 2;
        const sn::Result r = sn::generate(request);
        EXPECT_EQ(r.status, sn::Status::Ok);
        // (boxy needs no metrics: the generator's island facts)
        sn::Metrics m;
        m.hasTransparency = true;
        const sn::Assessment a = sn::assess(s, m, 0, &r);
        std::vector<std::string> kinds;
        for (const sn::Problem& p : a.warnings) kinds.push_back(p.kind);
        if (out) *out = r;
        return kinds;
    }

    bool has(const std::vector<std::string>& kinds, const char* kind) { return std::find(kinds.begin(), kinds.end(), kind) != kinds.end(); }

}

TEST(SpriteNormalsPreview, BoxyOnRoundedAndInflateAtAnySize) {
    for (int size : {16, 256}) {
        const sn::Image square = boxImage(size, 4, false);
        for (sn::Profile profile : {sn::Profile::Rounded, sn::Profile::Inflate, sn::Profile::Auto}) {
            sn::Settings s;
            s.profile = profile;
            sn::Result r;
            ASSERT_TRUE(has(warningKinds(s, square, &r), "boxy")) << size << " " << sn::toString(profile);
            EXPECT_EQ(r.boxyIslands, 1);
            EXPECT_EQ(r.boxySide, size);
            const sn::Assessment a = sn::assess(s, sn::Metrics(), 0, &r);
            const std::string radius = std::to_string(size == 16 ? 1 : 13);   // max(1, round(0.05 x side))
            bool found = false;
            for (const sn::Problem& p : a.warnings) {
                if (p.kind != "boxy") continue;
                found = true;
                EXPECT_EQ(p.fix, "Use preset props, or profile bevel with radius " + radius + ".");
            }
            EXPECT_TRUE(found);
        }
        // not for the plateau profiles, nor a disc, nor a tile
        for (sn::Profile profile : {sn::Profile::Dome, sn::Profile::Bevel}) {
            sn::Settings s;
            s.profile = profile;
            EXPECT_FALSE(has(warningKinds(s, square), "boxy")) << size << " " << sn::toString(profile);
        }
        EXPECT_FALSE(has(warningKinds(sn::presetSettings(sn::Preset::Props), square), "boxy")) << size;
        sn::Settings rounded;
        rounded.profile = sn::Profile::Rounded;
        EXPECT_FALSE(has(warningKinds(rounded, boxImage(size, 4, true)), "boxy")) << size << " disc";
        sn::Settings wrap = rounded;
        wrap.edges = sn::Edges::Wrap;
        EXPECT_FALSE(has(warningKinds(wrap, square), "boxy")) << size << " wrap";
    }
    EXPECT_EQ(sn::boxyBevelRadius(8), 1);
    EXPECT_EQ(sn::boxyBevelRadius(30), 2);
    EXPECT_EQ(sn::boxyBevelRadius(512), 26);
}
