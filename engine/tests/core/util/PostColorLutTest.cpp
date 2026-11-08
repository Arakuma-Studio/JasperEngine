// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The colour LUT bake with a user LUT (S13 AC3, AC4, AC5): an identity .cube is neutral, a
// .cube and the equivalent PNG strip bake alike, and a log .cube applies at the scene stage
// (before the curve, in the ALL and CURVE bakes, never in GRADE). CPU only.

#include "util/CubeLut.h"
#include "util/PostColorLut.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    using Look = std::function<Vector3(float, float, float)>;

    const PostColorLut::Stage STAGES[] = {PostColorLut::Stage::ALL, PostColorLut::Stage::CURVE, PostColorLut::Stage::GRADE};

    const char* stageName(PostColorLut::Stage stage){
        switch (stage){
            case PostColorLut::Stage::ALL: return "ALL";
            case PostColorLut::Stage::CURVE: return "CURVE";
            case PostColorLut::Stage::GRADE: return "GRADE";
        }
        return "?";
    }

    // a 3D CubeLut of side n holding look(r, g, b) of the normalised coordinate
    CubeLut tableLut(const std::string& title, int n, const Look& look){
        CubeLut lut;
        lut.title = title;
        lut.size3D = n;
        lut.lut3D.reserve((size_t)n * n * n * 3);
        for (int b = 0; b < n; b++){
            for (int g = 0; g < n; g++){
                for (int r = 0; r < n; r++){
                    const Vector3 c = look((float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1));
                    lut.lut3D.insert(lut.lut3D.end(), {c.x, c.y, c.z});
                }
            }
        }
        return lut;
    }

    Vector3 identity(float r, float g, float b){
        return Vector3(r, g, b);
    }

    // a display look with each channel fed by another one: a layout mistake shows
    Vector3 swizzledLook(float r, float g, float b){
        return Vector3(0.05f + 0.9f * g, 0.1f + 0.8f * b * b, 0.2f + 0.7f * r);
    }

    // a log-space look (ACEScct values in and out): darker, warmer
    Vector3 logLook(float r, float g, float b){
        return Vector3(0.9f * r + 0.03f, 0.88f * g + 0.01f, 0.85f * b);
    }

    PostProcessingSettings neutralGrading(){
        PostProcessingSettings s;
        s.colorGradingEnabled = true;
        s.lutTexture = "look.cube";
        return s;
    }

    PostProcessingSettings strongGrading(){
        PostProcessingSettings s = neutralGrading();
        s.exposure = 0.5f;
        s.tonemap = TonemapMode::ACES;
        s.temperature = 0.15f;
        s.tint = -0.05f;
        s.contrast = 1.15f;
        s.saturation = 0.9f;
        s.lift = Vector3(0.02f, 0.0f, 0.01f);
        s.gamma = Vector3(1.0f, 1.1f, 0.95f);
        s.gain = Vector3(1.0f, 0.95f, 1.05f);
        return s;
    }

    std::vector<uint8_t> bake(const PostProcessingSettings& settings, const CubeLut* lut, PostColorLut::Stage stage, int n){
        std::vector<uint8_t> rgba;
        PostColorLut::bake(settings, lut, stage, n, rgba);
        return rgba;
    }

    // the largest channel difference of the texels below the bake's top (a top texel decodes to
    // 65504, the shaper's stand-in for unbounded light: beyond every LUT's input range, where a
    // scene-stage LUT scales the colour into range and back and float precision cannot keep a
    // channel 1e6 times smaller than another)
    int maxDifferenceBelowTop(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int n){
        std::vector<uint8_t> x = a, y = b;
        for (size_t i = 0; i + 3 < x.size() && i + 3 < y.size(); i += 4){
            const size_t texel = i / 4;
            if ((int)(texel % n) == n - 1 || (int)(texel / ((size_t)n * n)) == n - 1 || (int)(texel / n % n) == n - 1){
                for (int c = 0; c < 4; c++)
                    x[i + c] = y[i + c];
            }
        }
        int worst = 0;
        for (size_t i = 0; i < std::min(x.size(), y.size()); i++)
            worst = std::max(worst, std::abs((int)x[i] - (int)y[i]));
        return worst;
    }

    // the largest channel difference; with `n` the first worst texel is printed (r, g, b, channel)
    int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int n = 0){
        EXPECT_EQ(a.size(), b.size());
        int worst = 0;
        size_t at = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); i++){
            const int d = std::abs((int)a[i] - (int)b[i]);
            if (d > worst){
                worst = d;
                at = i;
            }
        }
        if (n > 0 && worst > 1){
            const size_t texel = at / 4;
            std::printf("worst texel r %d g %d b %d channel %d: %d vs %d\n", (int)(texel % n), (int)(texel / ((size_t)n * n)),
                        (int)(texel / n % n), (int)(at % 4), a[at], b[at]);
        }
        return worst;
    }

    // the texel at (r, g, b) of a bake of side n
    Vector3 texel(const std::vector<uint8_t>& rgba, int n, int r, int g, int b){
        const size_t i = ((size_t)g * n * n + (size_t)b * n + (size_t)r) * 4;
        return Vector3(rgba[i] / 255.0f, rgba[i + 1] / 255.0f, rgba[i + 2] / 255.0f);
    }

}

TEST(PostColorLut, IdentityCubeIsNeutral){
    const CubeLut lut = tableLut("", 33, identity);
    for (const PostProcessingSettings& settings : {neutralGrading(), strongGrading()}){
        LutStage stage = LutStage::DISPLAY;
        ASSERT_TRUE(PostColorLut::getLutStage(settings, &lut, stage));
        EXPECT_EQ(stage, LutStage::DISPLAY);
        for (int n : {16, 32}){
            for (PostColorLut::Stage bakeStage : STAGES){
                EXPECT_LE(maxDifference(bake(settings, &lut, bakeStage, n), bake(settings, nullptr, bakeStage, n)), 1)
                    << "size " << n << " stage " << stageName(bakeStage) << " exposure " << settings.exposure;
            }
        }
    }
    // a display LUT counts as a grade, not as a curve
    const PostProcessingSettings neutral = neutralGrading();
    EXPECT_TRUE(PostColorLut::hasGrade(neutral, &lut));
    EXPECT_FALSE(PostColorLut::hasCurve(neutral, &lut));
    EXPECT_FALSE(PostColorLut::hasGrade(neutral, nullptr)) << "a LUT that did not load grades nothing";
    PostProcessingSettings off = neutral;
    off.lutContribution = 0.0f;
    EXPECT_FALSE(PostColorLut::hasGrade(off, &lut));
    off = neutral;
    off.colorGradingEnabled = false;
    EXPECT_FALSE(PostColorLut::hasGrade(off, &lut));
}

// a Unity PNG strip and a .cube holding the same table bake the same display-stage LUT
TEST(PostColorLut, CubeMatchesStrip){
    const int n = 16;
    PostColorLut::Strip strip;
    strip.size = n;
    strip.width = n * n;
    strip.rgb.resize((size_t)strip.width * n * 3);
    std::string text = "LUT_3D_SIZE " + std::to_string(n) + "\n";
    for (int b = 0; b < n; b++){
        for (int g = 0; g < n; g++){
            for (int r = 0; r < n; r++){
                const Vector3 c = swizzledLook((float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1));
                const int q[3] = {(int)std::lround(c.x * 255.0f), (int)std::lround(c.y * 255.0f), (int)std::lround(c.z * 255.0f)};
                // Unity's layout: slices left to right by blue, red along x, green 0 the bottom row
                const size_t i = ((size_t)(n - 1 - g) * strip.width + (size_t)b * n + r) * 3;
                char line[64];
                std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", q[0] / 255.0, q[1] / 255.0, q[2] / 255.0);
                text += line;
                for (int k = 0; k < 3; k++)
                    strip.rgb[i + k] = (uint8_t)q[k];
            }
        }
    }
    CubeLut fromStrip;
    PostColorLut::stripToCubeLut(strip, fromStrip);
    CubeLut fromCube;
    std::string error;
    ASSERT_TRUE(parseCubeLut(text, fromCube, error)) << error;

    EXPECT_TRUE(fromStrip.fromStrip);
    ASSERT_EQ(fromStrip.size3D, n);
    ASSERT_EQ(fromStrip.lut3D.size(), fromCube.lut3D.size());
    for (size_t i = 0; i < fromCube.lut3D.size(); i++)
        ASSERT_NEAR(fromStrip.lut3D[i], fromCube.lut3D[i], 1e-6f) << i;
    EXPECT_EQ(stageOf(LutInputEncoding::AUTO, fromStrip), LutStage::DISPLAY);
    EXPECT_EQ(stageOf(LutInputEncoding::AUTO, fromCube), LutStage::DISPLAY);

    for (const PostProcessingSettings& settings : {neutralGrading(), strongGrading()}){
        for (PostColorLut::Stage stage : {PostColorLut::Stage::ALL, PostColorLut::Stage::GRADE}){
            const std::vector<uint8_t> cube = bake(settings, &fromCube, stage, 32);
            EXPECT_LE(maxDifference(cube, bake(settings, &fromStrip, stage, 32)), 1) << stageName(stage);
            EXPECT_GT(maxDifference(cube, bake(settings, nullptr, stage, 32)), 20) << "the look applies (" << stageName(stage) << ")";
        }
    }

    // the strip is sRGB in and out: at its corners (where piecewise sRGB and the engine's pow
    // 1/2.2 agree on the coordinate) a bake texel is the strip's colour decoded from sRGB
    const PostProcessingSettings neutral = neutralGrading();
    const std::vector<uint8_t> grade = bake(neutral, &fromStrip, PostColorLut::Stage::GRADE, n);
    auto srgbToEngine = [](float v){
        const float linear = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        return std::pow(linear, 1.0f / 2.2f);
    };
    for (int b : {0, n - 1}){
        for (int g : {0, n - 1}){
            for (int r : {0, n - 1}){
                const size_t i = ((size_t)(n - 1 - g) * strip.width + (size_t)b * n + r) * 3;
                const Vector3 out = texel(grade, n, r, g, b);
                EXPECT_NEAR(out.x, srgbToEngine(strip.rgb[i] / 255.0f), 1.0f / 255.0f);
                EXPECT_NEAR(out.y, srgbToEngine(strip.rgb[i + 1] / 255.0f), 1.0f / 255.0f);
                EXPECT_NEAR(out.z, srgbToEngine(strip.rgb[i + 2] / 255.0f), 1.0f / 255.0f);
            }
        }
    }
}

// an ACEScct LUT applies to the exposed linear scene colour before the curve
TEST(PostColorLut, SceneStageLogLut){
    const CubeLut identityLog = tableLut("ACEScct identity", 33, identity);
    ASSERT_EQ(resolveEncoding(LutInputEncoding::AUTO, identityLog), LutInputEncoding::ACESCCT);
    PostProcessingSettings settings = strongGrading();
    LutStage stage = LutStage::DISPLAY;
    ASSERT_TRUE(PostColorLut::getLutStage(settings, &identityLog, stage));
    EXPECT_EQ(stage, LutStage::SCENE);

    // neutral: ACEScct covers linear 0..222 (brighter colours go through it scaled into range)
    for (const PostProcessingSettings& s : {neutralGrading(), settings}){
        for (int n : {16, 32}){
            for (PostColorLut::Stage bakeStage : STAGES){
                EXPECT_LE(maxDifferenceBelowTop(bake(s, &identityLog, bakeStage, n), bake(s, nullptr, bakeStage, n), n), 1)
                    << "size " << n << " stage " << stageName(bakeStage);
            }
        }
    }
    // a bright saturated colour past the range keeps its hue (a per-channel clamp made it white)
    const Vector3 red = applySceneStage(identityLog, LutInputEncoding::ACESCCT, Vector3(1000.0f, 1.0f, 0.5f));
    EXPECT_NEAR(red.x, 1000.0f, 1.0f);
    EXPECT_NEAR(red.y, 1.0f, 0.05f);
    EXPECT_NEAR(red.z, 0.5f, 0.05f);
    // the same with the encoding chosen in the settings rather than from the title
    CubeLut untitled = identityLog;
    untitled.title.clear();
    settings.lutInputEncoding = LutInputEncoding::ACESCCT;
    EXPECT_LE(maxDifferenceBelowTop(bake(settings, &untitled, PostColorLut::Stage::CURVE, 32), bake(settings, nullptr, PostColorLut::Stage::CURVE, 32), 32), 1);
    settings.lutInputEncoding = LutInputEncoding::AUTO;

    // a scene LUT is a curve, even without exposure or tone mapping, and no grade
    const PostProcessingSettings neutral = neutralGrading();
    EXPECT_TRUE(PostColorLut::hasCurve(neutral, &identityLog));
    EXPECT_FALSE(PostColorLut::hasGrade(neutral, &identityLog));
    EXPECT_FALSE(PostColorLut::hasCurve(neutral));
    EXPECT_TRUE(PostColorLut::hasGrade(neutral)) << "the settings alone read a set lutTexture as a display LUT";
    PostProcessingSettings off = neutral;
    off.colorGradingEnabled = false;
    EXPECT_FALSE(PostColorLut::hasCurve(off, &identityLog));

    // a look lands in the CURVE (and ALL) bakes, and GRADE ignores it
    const CubeLut look = tableLut("ACEScct look", 33, logLook);
    for (const PostProcessingSettings& s : {neutral, settings}){
        EXPECT_GT(maxDifference(bake(s, &look, PostColorLut::Stage::CURVE, 32), bake(s, nullptr, PostColorLut::Stage::CURVE, 32)), 8);
        EXPECT_GT(maxDifference(bake(s, &look, PostColorLut::Stage::ALL, 32), bake(s, nullptr, PostColorLut::Stage::ALL, 32)), 8);
        EXPECT_EQ(maxDifference(bake(s, &look, PostColorLut::Stage::GRADE, 32), bake(s, nullptr, PostColorLut::Stage::GRADE, 32)), 0);
    }

    // before the tone curve: a CURVE texel is encode(toneMap(lut(exposure * x)))
    const int n = 32;
    const std::vector<uint8_t> curve = bake(settings, &look, PostColorLut::Stage::CURVE, n);
    for (int i : {3, 12, 20, 28}){
        const float c = (float)i / (n - 1);
        const float x = c * c / (1.0f - c * c);
        const float exposure = std::exp2(settings.exposure);
        const Vector3 scene = applySceneStage(look, LutInputEncoding::ACESCCT, Vector3(x, x, x) * exposure);
        const Vector3 display = PostColorLut::toneMap(settings.tonemap, scene);
        const Vector3 out = texel(curve, n, i, i, i);
        EXPECT_NEAR(out.x, std::pow(display.x, 1.0f / 2.2f), 1.0f / 255.0f) << i;
        EXPECT_NEAR(out.y, std::pow(display.y, 1.0f / 2.2f), 1.0f / 255.0f) << i;
        EXPECT_NEAR(out.z, std::pow(display.z, 1.0f / 2.2f), 1.0f / 255.0f) << i;
    }

    // contribution mixes it in the linear scene colour
    settings.lutContribution = 0.5f;
    const std::vector<uint8_t> half = bake(settings, &look, PostColorLut::Stage::CURVE, n);
    for (int i : {12, 20}){
        const float c = (float)i / (n - 1);
        const float x = c * c / (1.0f - c * c) * std::exp2(settings.exposure);
        const Vector3 scene = applySceneStage(look, LutInputEncoding::ACESCCT, Vector3(x, x, x));
        const Vector3 mixed = Vector3(x, x, x) + (scene - Vector3(x, x, x)) * 0.5f;
        const Vector3 display = PostColorLut::toneMap(settings.tonemap, mixed);
        const Vector3 out = texel(half, n, i, i, i);
        EXPECT_NEAR(out.x, std::pow(display.x, 1.0f / 2.2f), 1.0f / 255.0f) << i;
        EXPECT_NEAR(out.z, std::pow(display.z, 1.0f / 2.2f), 1.0f / 255.0f) << i;
    }
}

// The SSR linear store clears to the shaper code the tail turns back into the clear colour:
// evaluate(invert(display)) gives the display colour back, for a curve and a curve with a grade,
// and the code agrees with the strip's texels where they meet
TEST(PostColorLut, InvertGivesTheCodeOfADisplayColour){
    PostProcessingSettings settings;
    settings.tonemap = TonemapMode::ACES;
    settings.exposure = 1.0f;
    const Vector3 displays[] = {Vector3(0.08f, 0.08f, 0.1f), Vector3(0.5f, 0.3f, 0.7f), Vector3(0.9f, 0.95f, 0.2f)};
    for (const Vector3& display : displays){
        const Vector3 code = PostColorLut::invert(settings, nullptr, PostColorLut::Stage::CURVE, display);
        const Vector3 back = PostColorLut::evaluate(settings, nullptr, PostColorLut::Stage::CURVE, code);
        for (unsigned c = 0; c < 3; c++)
            EXPECT_NEAR(back[c], display[c], 1e-4f) << "channel " << c;
    }
    // (a saturation of 0.5 halves the chroma: colours within reach of it)
    settings.colorGradingEnabled = true;
    settings.saturation = 0.5f;
    const Vector3 greyish[] = {Vector3(0.08f, 0.08f, 0.1f), Vector3(0.3f, 0.32f, 0.35f), Vector3(0.6f, 0.55f, 0.5f)};
    for (const Vector3& display : greyish){
        const Vector3 code = PostColorLut::invert(settings, nullptr, PostColorLut::Stage::ALL, display);
        const Vector3 back = PostColorLut::evaluate(settings, nullptr, PostColorLut::Stage::ALL, code);
        for (unsigned c = 0; c < 3; c++)
            EXPECT_NEAR(back[c], display[c], 2e-3f) << "graded, channel " << c;
    }
    // the strip's texel at a code is its evaluate, rounded to 8 bits
    std::vector<uint8_t> strip;
    const int n = 16;
    PostColorLut::bake(settings, nullptr, PostColorLut::Stage::ALL, n, strip);
    const int r = 5, g = 9, b = 3;
    const Vector3 texel = PostColorLut::evaluate(settings, nullptr, PostColorLut::Stage::ALL,
                                                 Vector3((float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1)));
    const size_t i = ((size_t)g * n * n + (size_t)b * n + (size_t)r) * 4;
    EXPECT_EQ(strip[i], (uint8_t)std::lround(texel.x * 255.0f));
    EXPECT_EQ(strip[i + 1], (uint8_t)std::lround(texel.y * 255.0f));
    EXPECT_EQ(strip[i + 2], (uint8_t)std::lround(texel.z * 255.0f));
}
