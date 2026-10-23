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

    int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        EXPECT_EQ(a.size(), b.size());
        int worst = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); i++)
            worst = std::max(worst, std::abs((int)a[i] - (int)b[i]));
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

    // neutral: ACEScct covers linear 0..222, past which ACES is white anyway
    for (const PostProcessingSettings& s : {neutralGrading(), settings}){
        for (int n : {16, 32}){
            for (PostColorLut::Stage bakeStage : STAGES){
                EXPECT_LE(maxDifference(bake(s, &identityLog, bakeStage, n), bake(s, nullptr, bakeStage, n)), 1)
                    << "size " << n << " stage " << stageName(bakeStage);
            }
        }
    }
    // the same with the encoding chosen in the settings rather than from the title
    CubeLut untitled = identityLog;
    untitled.title.clear();
    settings.lutInputEncoding = LutInputEncoding::ACESCCT;
    EXPECT_LE(maxDifference(bake(settings, &untitled, PostColorLut::Stage::CURVE, 32), bake(settings, nullptr, PostColorLut::Stage::CURVE, 32)), 1);
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
