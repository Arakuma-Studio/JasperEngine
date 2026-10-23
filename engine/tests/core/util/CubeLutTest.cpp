// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Modules/Rendering/Tests/CubeLutFileParserTests.cpp

// .cube parsing, the input encoding proposal, the .jlut cook and the LUT cache (S13 AC1, AC2,
// AC6, AC21, AC22). CPU only; fixtures in engine/tests/fixtures/cubelut (OpenEngine's).

#include "util/CubeLut.h"
#include "util/PostColorLut.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    // the benchmarks' budgets are for optimised builds: debug and sanitizer builds get 4x
#if !defined(NDEBUG) || defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    const double BENCH_SLACK = 4.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
    const double BENCH_SLACK = 4.0;
#else
    const double BENCH_SLACK = 1.0;
#endif
#else
    const double BENCH_SLACK = 1.0;
#endif

    std::string fixturePath(const char* name){
        return std::string(JASPER_TEST_FIXTURES_DIR) + "/cubelut/" + name;
    }

    std::string readText(const std::string& path){
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void writeBytes(const std::filesystem::path& path, const void* data, size_t size){
        std::ofstream out(path, std::ios::binary);
        out.write((const char*)data, (std::streamsize)size);
    }

    CubeLut parseFixture(const char* name){
        CubeLut lut;
        std::string error;
        EXPECT_TRUE(parseCubeLut(readText(fixturePath(name)), lut, error)) << name << ": " << error;
        return lut;
    }

    // the error parsing text fails with
    std::string parseError(const std::string& text){
        CubeLut lut;
        std::string error;
        EXPECT_FALSE(parseCubeLut(text, lut, error)) << text;
        EXPECT_TRUE(lut.empty());
        return error;
    }

    // count rows of 3D data
    std::string rows(int count){
        std::string text;
        for (int i = 0; i < count; i++)
            text += "0.5 0.5 0.5\n";
        return text;
    }

    // the proposal for a 0 to 1 table with a title (the input range the fixtures default to)
    LutInputEncoding proposeTitle(const char* title, std::string* reason = nullptr){
        CubeLut lut;
        lut.size3D = 2;
        lut.lut3D.assign(2 * 2 * 2 * 3, 0.0f);
        lut.title = title;
        std::string why;
        const LutInputEncoding encoding = proposeEncoding(lut, why);
        if (reason)
            *reason = why;
        return encoding;
    }

    using Look = std::function<Vector3(float, float, float)>;

    // .cube text of a 3D table of side n holding look(r, g, b) of the normalised coordinate
    std::string cubeText(const std::string& title, int n, const Look& look){
        std::string text = "TITLE \"" + title + "\"\nLUT_3D_SIZE " + std::to_string(n) + "\n";
        text.reserve(text.size() + (size_t)n * n * n * 27);
        char line[64];
        for (int b = 0; b < n; b++){
            for (int g = 0; g < n; g++){
                for (int r = 0; r < n; r++){
                    const Vector3 c = look((float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1));
                    const int length = std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", c.x, c.y, c.z);
                    text.append(line, (size_t)length);
                }
            }
        }
        return text;
    }

    // a gentle display look: lifted blacks, a little crosstalk and an S bend on red
    Vector3 displayLook(float r, float g, float b){
        const float pi = 3.14159265f;
        return Vector3(0.03f + 0.9f * r + 0.05f * g + 0.02f * std::sin(pi * r), 0.02f + 0.92f * g + 0.04f * b, 0.04f + 0.85f * b + 0.06f * r);
    }

    // a log-space look (ACEScct values in and out)
    Vector3 logLook(float r, float g, float b){
        const float pi = 3.14159265f;
        return Vector3(0.02f + 0.95f * r + 0.02f * std::sin(pi * r), 0.01f + 0.98f * g, 0.03f + 0.9f * b + 0.05f * r);
    }

    // the colour grade the round-trip and benchmark bakes use
    PostProcessingSettings gradedSettings(){
        PostProcessingSettings s;
        s.exposure = 0.5f;
        s.tonemap = TonemapMode::ACES;
        s.colorGradingEnabled = true;
        s.contrast = 1.1f;
        s.saturation = 0.9f;
        s.lift = Vector3(0.02f, 0.0f, 0.0f);
        s.gamma = Vector3(1.0f, 1.05f, 1.0f);
        s.gain = Vector3(1.0f, 0.97f, 1.03f);
        s.lutTexture = "look.cube";
        return s;
    }

    int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        EXPECT_EQ(a.size(), b.size());
        int worst = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); i++)
            worst = std::max(worst, std::abs((int)a[i] - (int)b[i]));
        return worst;
    }

    // the fastest of some runs, in milliseconds
    double bestMs(int runs, const std::function<void()>& run){
        double best = std::numeric_limits<double>::max();
        for (int i = 0; i < runs; i++){
            const auto start = std::chrono::steady_clock::now();
            run();
            const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
            best = std::min(best, elapsed.count());
        }
        return best;
    }

    void reportBench(const char* name, double ms, double budget){
        std::printf("[   BENCH  ] %s: %.3f ms (budget %.1f ms)\n", name, ms, budget * BENCH_SLACK);
        EXPECT_LE(ms, budget * BENCH_SLACK) << name;
    }

    // a scratch directory removed with the test
    struct TempDir{
        std::filesystem::path path;

        explicit TempDir(const char* name){
            path = std::filesystem::temp_directory_path() / (std::string("jasper-cubelut-") + name);
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir(){
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

}

// ---- parsing (OpenEngine's parser tests)

TEST(CubeLut, ParsesFixtures){
    struct Expected{
        const char* file;
        const char* title;
        float rangeMax;
    };
    const Expected fixtures[] = {
        {"acescct_look.cube", "Example log look (ACEScct, generated for the look-dial-ux mock-up)", 1.0f},
        {"arri_logc_look.cube", "ARRI LogC creative look", 1.0f},
        {"cineon_print_to_rec709.cube", "Cineon print emulation to Rec709", 1.0f},
        {"davinci_intermediate_grade.cube", "DaVinci Intermediate grade", 1.0f},
        {"display_linear_bake.cube", "URP ShadowsMidtonesHighlights bake, display linear (unity-scene-convert)", 1.0f},
        {"hdr_range.cube", "", 16.0f},
        {"unnamed_display.cube", "URP ShadowsMidtonesHighlights bake (unity-scene-convert)", 1.0f},
    };
    for (const Expected& expected : fixtures){
        const CubeLut lut = parseFixture(expected.file);
        EXPECT_EQ(lut.title, expected.title) << expected.file;
        EXPECT_EQ(lut.size1D, 0) << expected.file;
        EXPECT_EQ(lut.size3D, 2) << expected.file;
        EXPECT_FLOAT_EQ(lut.in3D[0], 0.0f) << expected.file;
        EXPECT_FLOAT_EQ(lut.in3D[1], expected.rangeMax) << expected.file;
        EXPECT_EQ(lut.lut3D.size(), 8u * 3u) << expected.file;
        EXPECT_FALSE(lut.empty()) << expected.file;
        EXPECT_FALSE(lut.fromStrip) << expected.file;
    }
}

TEST(CubeLut, Parses3DRedFastest){
    const char* text = "LUT_3D_SIZE 2\n"
                       "LUT_3D_INPUT_RANGE 0.0 1.0\n"
                       "0.0 0.0 0.0\n1.0 0.0 0.0\n0.0 1.0 0.0\n1.0 1.0 0.0\n"
                       "0.0 0.0 1.0\n1.0 0.0 1.0\n0.0 1.0 1.0\n1.0 1.0 1.0\n";
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut(text, lut, error)) << error;
    EXPECT_EQ(lut.size1D, 0);
    EXPECT_EQ(lut.size3D, 2);
    ASSERT_EQ(lut.lut3D.size(), 8u * 3u);
    EXPECT_FLOAT_EQ(lut.lut3D[0], 0.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[3], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[21], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[22], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[23], 1.0f);
    // an identity table samples to its input
    const Vector3 c = lut.sample(Vector3(0.25f, 0.5f, 0.75f));
    EXPECT_NEAR(c.x, 0.25f, 1e-6f);
    EXPECT_NEAR(c.y, 0.5f, 1e-6f);
    EXPECT_NEAR(c.z, 0.75f, 1e-6f);
}

TEST(CubeLut, Parses1DOnly){
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut("LUT_1D_SIZE 2\n0.1 0.2 0.3\n0.4 0.5 0.6\n", lut, error)) << error;
    EXPECT_EQ(lut.size1D, 2);
    EXPECT_EQ(lut.size3D, 0);
    ASSERT_EQ(lut.lut1D.size(), 6u);
    EXPECT_FLOAT_EQ(lut.lut1D[0], 0.1f);
    EXPECT_FLOAT_EQ(lut.lut1D[3], 0.4f);
    const Vector3 c = lut.sample(Vector3(0.5f, 0.0f, 1.0f));
    EXPECT_NEAR(c.x, 0.25f, 1e-6f);
    EXPECT_NEAR(c.y, 0.2f, 1e-6f);
    EXPECT_NEAR(c.z, 0.6f, 1e-6f);
}

TEST(CubeLut, ScalarDomainSetsInputRanges){
    const std::string text = "DOMAIN_MIN -0.5 -0.5 -0.5\nDOMAIN_MAX 1.5 1.5 1.5\nLUT_3D_SIZE 2\n" + rows(8);
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut(text, lut, error)) << error;
    EXPECT_FLOAT_EQ(lut.in3D[0], -0.5f);
    EXPECT_FLOAT_EQ(lut.in3D[1], 1.5f);
    EXPECT_FLOAT_EQ(lut.in1D[0], -0.5f);
    EXPECT_FLOAT_EQ(lut.in1D[1], 1.5f);
}

// Resolve's sample: a 1D shaper inverting each channel, then a 3D table inverting it back
TEST(CubeLut, ParsesShaperThen3D){
    std::string text = "# Sample 3D cube file containing 1D shaper LUT and 3D LUT.\n"
                       "LUT_1D_SIZE 6\nLUT_1D_INPUT_RANGE 0.0 1.0\nLUT_3D_SIZE 3\nLUT_3D_INPUT_RANGE 0.0 1.0\n"
                       "1.0 1.0 1.0\n0.8 0.8 0.8\n0.6 0.6 0.6\n0.4 0.4 0.4\n0.2 0.2 0.2\n0.0 0.0 0.0\n";
    for (int b = 2; b >= 0; b--){
        for (int g = 2; g >= 0; g--){
            for (int r = 2; r >= 0; r--){
                char line[32];
                std::snprintf(line, sizeof(line), "%.1f %.1f %.1f\n", r * 0.5, g * 0.5, b * 0.5);
                text += line;
            }
        }
    }
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut(text, lut, error)) << error;
    EXPECT_EQ(lut.size1D, 6);
    EXPECT_EQ(lut.size3D, 3);
    EXPECT_EQ(lut.lut1D.size(), 18u);
    EXPECT_EQ(lut.lut3D.size(), 27u * 3u);
    EXPECT_FLOAT_EQ(lut.lut1D[0], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut1D[15], 0.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[0], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut3D[78], 0.0f);
    for (const Vector3& c : {Vector3(0.2f, 0.5f, 0.9f), Vector3(0.0f, 1.0f, 0.35f)}){
        const Vector3 out = lut.sample(c);
        EXPECT_NEAR(out.x, c.x, 1e-5f);
        EXPECT_NEAR(out.y, c.y, 1e-5f);
        EXPECT_NEAR(out.z, c.z, 1e-5f);
    }
}

TEST(CubeLut, SamplesInItsInputRange){
    const CubeLut lut = parseFixture("hdr_range.cube");
    const Vector3 mid = lut.sample(Vector3(8.0f, 0.0f, 16.0f));
    EXPECT_NEAR(mid.x, 0.5f, 1e-6f);
    EXPECT_NEAR(mid.y, 0.0f, 1e-6f);
    EXPECT_NEAR(mid.z, 1.0f, 1e-6f);
    // outside the range: clamped to it
    const Vector3 clamped = lut.sample(Vector3(-1.0f, 40.0f, 4.0f));
    EXPECT_NEAR(clamped.x, 0.0f, 1e-6f);
    EXPECT_NEAR(clamped.y, 1.0f, 1e-6f);
    EXPECT_NEAR(clamped.z, 0.25f, 1e-6f);
}

TEST(CubeLut, CommentsAndBlankLinesInData){
    const std::string text = "LUT_3D_SIZE 2\n\n0 0 0\n# allowed\n" + rows(3) + "\r\n   \n" + rows(4) + "# trailing comment\n\n";
    CubeLut lut;
    std::string error;
    EXPECT_TRUE(parseCubeLut(text, lut, error)) << error;
}

TEST(CubeLut, Utf8BomIsAccepted){
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut("\xEF\xBB\xBFLUT_1D_SIZE 2\n0.1 0.2 0.3\n0.4 0.5 0.6\n", lut, error)) << error;
    EXPECT_EQ(lut.size1D, 2);
}

// the TITLE line's text, the proposal's main evidence
TEST(CubeLut, TitleIsKept){
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut("TITLE \"Film look \"\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n", lut, error)) << error;
    EXPECT_EQ(lut.title, "Film look ");
    ASSERT_TRUE(parseCubeLut("TITLE unquoted words\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n", lut, error)) << error;
    EXPECT_EQ(lut.title, "unquoted words");
}

// numbers are read the same in every locale, in every decimal form
TEST(CubeLut, ParsesNumberForms){
    CubeLut lut;
    std::string error;
    ASSERT_TRUE(parseCubeLut("LUT_1D_SIZE 2\n.5 -0 +1.\n1.5E+1 25e-2 0.000125\n", lut, error)) << error;
    EXPECT_FLOAT_EQ(lut.lut1D[0], 0.5f);
    EXPECT_FLOAT_EQ(lut.lut1D[1], 0.0f);
    EXPECT_FLOAT_EQ(lut.lut1D[2], 1.0f);
    EXPECT_FLOAT_EQ(lut.lut1D[3], 15.0f);
    EXPECT_FLOAT_EQ(lut.lut1D[4], 0.25f);
    EXPECT_FLOAT_EQ(lut.lut1D[5], 0.000125f);
    EXPECT_EQ(parseError("LUT_1D_SIZE 2\n0,5 0 0\n1 1 1\n"), "line 2: Invalid RGB line in 1D LUT section.");
    EXPECT_EQ(parseError("LUT_1D_SIZE 2\n0 0 0\n1 1 1e\n"), "line 3: Invalid RGB line in 1D LUT section.");
}

TEST(CubeLut, RejectsMalformed){
    // missing size
    EXPECT_EQ(parseError("# no size\nLUT_3D_INPUT_RANGE 0.0 1.0\n0 0 0\n"), "line 3: Missing LUT_1D_SIZE and/or LUT_3D_SIZE.");
    EXPECT_EQ(parseError(""), "line 1: Missing LUT_1D_SIZE and/or LUT_3D_SIZE.");
    // 3D size 130 (the cap is 129), and other sizes out of range or malformed
    EXPECT_EQ(parseError("TITLE \"big\"\nLUT_3D_SIZE 130\n0 0 0\n"), "line 2: LUT_3D_SIZE out of supported range (2-129).");
    EXPECT_EQ(parseError("LUT_3D_SIZE 1\n0 0 0\n"), "line 1: LUT_3D_SIZE out of supported range (2-129).");
    EXPECT_EQ(parseError("LUT_1D_SIZE 65537\n0 0 0\n"), "line 1: LUT_1D_SIZE out of supported range (2-65536).");
    EXPECT_EQ(parseError("LUT_3D_SIZE abc\n0 0 0\n"), "line 1: Invalid LUT_3D_SIZE line.");
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 0\n" + rows(8)), "line 2: Invalid LUT_3D_INPUT_RANGE line.");
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 1 0\n" + rows(8)), "line 3: The input range's minimum must be below its maximum.");
    // per-channel DOMAIN
    EXPECT_EQ(parseError("DOMAIN_MIN 0.0 0.1 0.0\nDOMAIN_MAX 1.0 1.0 1.0\nLUT_3D_SIZE 2\n" + rows(8)),
              "line 1: Per-channel DOMAIN_MIN/DOMAIN_MAX is not supported.");
    EXPECT_EQ(parseError("DOMAIN_MAX 1.0 1.0\nLUT_3D_SIZE 2\n" + rows(8)), "line 1: Invalid DOMAIN_MIN/DOMAIN_MAX line.");
    // incomplete data
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\n" + rows(7)), "line 8: Incomplete 3D LUT data (7 of 8 entries).");
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\n"), "line 1: Incomplete 3D LUT data (0 of 8 entries).");
    EXPECT_EQ(parseError("LUT_1D_SIZE 3\n0 0 0\n1 1 1\n"), "line 3: Incomplete 1D LUT data (2 of 3 entries).");
    EXPECT_EQ(parseError("LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n" + rows(5)), "line 9: Incomplete 3D LUT data (5 of 8 entries).");
    // trailing data
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\n" + rows(8) + "1 1 1\n"), "line 10: Unexpected content after LUT data.");
    // a malformed row
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\n0 0 0\n1 x 0\n"), "line 3: Invalid RGB line in 3D LUT section.");
    EXPECT_EQ(parseError("LUT_3D_SIZE 2\n0 0\n"), "line 2: Invalid RGB line in 3D LUT section.");
}

// ---- input encoding (OpenEngine's proposal tests, one per row of its rule)

TEST(CubeLut, ProposesEncoding){
    struct Expected{
        const char* file;
        LutInputEncoding encoding;
        LutStage stage;
    };
    const Expected fixtures[] = {
        {"arri_logc_look.cube", LutInputEncoding::LOGC3, LutStage::SCENE},
        {"acescct_look.cube", LutInputEncoding::ACESCCT, LutStage::SCENE},
        {"davinci_intermediate_grade.cube", LutInputEncoding::DAVINCI_INTERMEDIATE, LutStage::SCENE},
        {"cineon_print_to_rec709.cube", LutInputEncoding::CINEON, LutStage::SCENE},
        {"display_linear_bake.cube", LutInputEncoding::LINEAR, LutStage::DISPLAY},
        {"hdr_range.cube", LutInputEncoding::LINEAR, LutStage::SCENE},
        {"unnamed_display.cube", LutInputEncoding::SRGB, LutStage::DISPLAY},
    };
    for (const Expected& expected : fixtures){
        const CubeLut lut = parseFixture(expected.file);
        std::string reason;
        EXPECT_EQ(proposeEncoding(lut, reason), expected.encoding) << expected.file << ": " << reason;
        EXPECT_EQ(resolveEncoding(LutInputEncoding::AUTO, lut), expected.encoding) << expected.file;
        EXPECT_EQ(stageOf(LutInputEncoding::AUTO, lut), expected.stage) << expected.file;
        EXPECT_FALSE(reason.empty()) << expected.file;
    }
    std::string reason;
    proposeEncoding(parseFixture("acescct_look.cube"), reason);
    EXPECT_EQ(reason, "the title names ACEScct");
    proposeEncoding(parseFixture("hdr_range.cube"), reason);
    EXPECT_EQ(reason, "the input range reaches 16");
    proposeEncoding(parseFixture("unnamed_display.cube"), reason);
    EXPECT_EQ(reason, "a 0 to 1 input range and no encoding named in the title");

    // a word that merely contains an encoding's name names nothing ("Warrior", "Nonlinear")
    for (const char* title : {"Warrior teal", "Carribean sunset", "Nonlinear contrast"})
        EXPECT_EQ(proposeTitle(title), LutInputEncoding::SRGB) << title;

    // a title "X to Y" converts from X: the input side is the encoding before "to"
    EXPECT_EQ(proposeTitle("sRGB to Linear"), LutInputEncoding::SRGB);
    EXPECT_EQ(proposeTitle("Rec709 to LogC"), LutInputEncoding::SRGB);
    EXPECT_EQ(proposeTitle("LogC to Rec709"), LutInputEncoding::LOGC3);
    EXPECT_EQ(proposeTitle("Print film to LogC"), LutInputEncoding::SRGB) << "an output side alone names no input";
    EXPECT_EQ(proposeTitle("ACES cct grade"), LutInputEncoding::ACESCCT);
    EXPECT_EQ(proposeTitle("DaVinci Wide Gamut look"), LutInputEncoding::DAVINCI_INTERMEDIATE);
    EXPECT_EQ(proposeTitle("Rec 709 punch"), LutInputEncoding::SRGB);

    // an encoding the engine does not have is named in the reason, not read as a neighbouring curve
    for (const auto& [title, name] : {std::pair{"Alexa LogC4", "LogC4"}, std::pair{"S-Log3 look", "S-Log3"},
                                      std::pair{"ACEScc grade", "ACEScc"}, std::pair{"V-Log film", "V-Log"}}){
        EXPECT_EQ(proposeTitle(title, &reason), LutInputEncoding::SRGB) << title;
        EXPECT_NE(reason.find(std::string(name) + ", which is not supported"), std::string::npos) << reason;
    }

    // a setting other than AUTO is used as it is; a strip is always sRGB display colour
    const CubeLut log = parseFixture("arri_logc_look.cube");
    EXPECT_EQ(resolveEncoding(LutInputEncoding::CINEON, log), LutInputEncoding::CINEON);
    CubeLut strip = log;
    strip.fromStrip = true;
    EXPECT_EQ(resolveEncoding(LutInputEncoding::LOGC3, strip), LutInputEncoding::SRGB);
    EXPECT_EQ(stageOf(LutInputEncoding::LOGC3, strip), LutStage::DISPLAY);

    // display input is sRGB, or linear up to 1; a log curve, or linear over a scene range, is scene
    const CubeLut unit = parseFixture("unnamed_display.cube");
    const CubeLut wide = parseFixture("hdr_range.cube");
    EXPECT_EQ(stageOf(LutInputEncoding::SRGB, unit), LutStage::DISPLAY);
    EXPECT_EQ(stageOf(LutInputEncoding::LINEAR, unit), LutStage::DISPLAY);
    EXPECT_EQ(stageOf(LutInputEncoding::LINEAR, wide), LutStage::SCENE);
    EXPECT_EQ(stageOf(LutInputEncoding::SRGB, wide), LutStage::DISPLAY);
    for (LutInputEncoding encoding : {LutInputEncoding::LOGC3, LutInputEncoding::ACESCCT, LutInputEncoding::CINEON,
                                      LutInputEncoding::DAVINCI_INTERMEDIATE})
        EXPECT_EQ(stageOf(encoding, unit), LutStage::SCENE) << (int)encoding;
}

// an identity table read in each log encoding gives the linear colour back over its range
TEST(CubeLut, LogEncodingsRoundTrip){
    CubeLut identity;
    identity.size3D = 2;
    for (int i = 0; i < 8; i++)
        identity.lut3D.insert(identity.lut3D.end(), {(float)(i & 1), (float)((i >> 1) & 1), (float)((i >> 2) & 1)});
    for (LutInputEncoding encoding : {LutInputEncoding::LINEAR, LutInputEncoding::SRGB, LutInputEncoding::LOGC3, LutInputEncoding::ACESCCT,
                                      LutInputEncoding::CINEON, LutInputEncoding::DAVINCI_INTERMEDIATE}){
        for (const Vector3& c : {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.18f, 0.05f, 0.5f), Vector3(0.9f, 0.002f, 0.3f)}){
            const Vector3 out = applySceneStage(identity, encoding, c);
            EXPECT_NEAR(out.x, c.x, 1e-3f + c.x * 1e-3f) << (int)encoding;
            EXPECT_NEAR(out.y, c.y, 1e-3f + c.y * 1e-3f) << (int)encoding;
            EXPECT_NEAR(out.z, c.z, 1e-3f + c.z * 1e-3f) << (int)encoding;
        }
    }
    for (LutInputEncoding encoding : {LutInputEncoding::SRGB, LutInputEncoding::LINEAR}){
        const Vector3 c(0.02f, 0.5f, 1.0f);
        const Vector3 out = applyDisplayStage(identity, encoding, c);
        EXPECT_NEAR(out.x, c.x, 1e-5f) << (int)encoding;
        EXPECT_NEAR(out.y, c.y, 1e-5f) << (int)encoding;
        EXPECT_NEAR(out.z, c.z, 1e-5f) << (int)encoding;
    }
}

// ---- .jlut and loading

TEST(CubeLut, JlutRoundTrip){
    // float16 values survive (rounded), the 1D table is kept, a small 3D table is not resampled
    CubeLut small;
    small.title = "Shaper and table";
    small.size1D = 2;
    small.lut1D = {0.0f, 0.5f, 1.0f, 2.0f, 70000.0f, -0.25f};
    small.in1D[1] = 4.0f;
    small.size3D = 2;
    small.lut3D = {0.0f, 1e-5f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f,
                   1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f, 2.1f, 2.2f};
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(writeJlut(small, bytes));
    CubeLut back;
    std::string error;
    ASSERT_TRUE(readJlut(bytes.data(), bytes.size(), back, error)) << error;
    EXPECT_EQ(back.title, small.title);
    EXPECT_EQ(back.size1D, 2);
    EXPECT_EQ(back.size3D, 2);
    EXPECT_FLOAT_EQ(back.in1D[1], 4.0f);
    ASSERT_EQ(back.lut1D.size(), 6u);
    ASSERT_EQ(back.lut3D.size(), 24u);
    EXPECT_FLOAT_EQ(back.lut1D[4], 65504.0f) << "past float16's range: clamped";
    EXPECT_FLOAT_EQ(back.lut1D[5], -0.25f);
    EXPECT_NEAR(back.lut3D[1], 1e-5f, 1e-7f) << "a float16 subnormal";
    for (size_t i = 0; i < small.lut3D.size(); i++)
        EXPECT_NEAR(back.lut3D[i], small.lut3D[i], std::fabs(small.lut3D[i]) * 1e-3f + 1e-7f) << i;

    // a 65^3 .cube cooks to at most 300 KB (33^3), and bakes within 2/255 of the .cube
    const std::pair<const char*, Look> looks[] = {{"Display look", displayLook}, {"ACEScct look", logLook}};
    for (const auto& [title, look] : looks){
        CubeLut cube;
        ASSERT_TRUE(parseCubeLut(cubeText(title, 65, look), cube, error)) << error;
        ASSERT_TRUE(writeJlut(cube, bytes));
        EXPECT_LE(bytes.size(), 300u * 1024u) << title;
        CubeLut cooked;
        ASSERT_TRUE(readJlut(bytes.data(), bytes.size(), cooked, error)) << error;
        EXPECT_EQ(cooked.size3D, JLUT_MAX_3D_SIZE);
        EXPECT_EQ(cooked.title, cube.title);
        EXPECT_EQ(resolveEncoding(LutInputEncoding::AUTO, cooked), resolveEncoding(LutInputEncoding::AUTO, cube)) << title;
        const PostProcessingSettings settings = gradedSettings();
        for (PostColorLut::Stage stage : {PostColorLut::Stage::ALL, PostColorLut::Stage::CURVE, PostColorLut::Stage::GRADE}){
            std::vector<uint8_t> fromCube, fromJlut;
            PostColorLut::bake(settings, &cube, stage, 32, fromCube);
            PostColorLut::bake(settings, &cooked, stage, 32, fromJlut);
            EXPECT_LE(maxDifference(fromCube, fromJlut), 2) << title << " stage " << (int)stage;
        }
    }

    // a damaged file is refused
    std::vector<uint8_t> damaged = bytes;
    damaged.pop_back();
    EXPECT_FALSE(readJlut(damaged.data(), damaged.size(), back, error));
    EXPECT_EQ(error, "Truncated or damaged .jlut file.");
    EXPECT_TRUE(back.empty());
    damaged = bytes;
    damaged[0] = 'X';
    EXPECT_FALSE(readJlut(damaged.data(), damaged.size(), back, error));
    EXPECT_EQ(error, "Not a .jlut file.");
    damaged = bytes;
    damaged[4] = 9;
    EXPECT_FALSE(readJlut(damaged.data(), damaged.size(), back, error));
    EXPECT_EQ(error, "Unsupported .jlut version 9.");
    EXPECT_FALSE(writeJlut(CubeLut(), bytes));
}

// loadColorLut reads through the engine's io (absolute paths here), a .cube's cooked sibling
// first; the cache parses once per path until invalidated
TEST(CubeLut, LoadsFilesAndCaches){
    const TempDir dir("load");
    const std::string text = cubeText("ACEScct look", 9, logLook);
    const std::filesystem::path cubePath = dir.path / "look.cube";
    writeBytes(cubePath, text.data(), text.size());

    CubeLut lut;
    std::string error;
    ASSERT_TRUE(loadColorLut(cubePath.string(), lut, &error)) << error;
    EXPECT_EQ(lut.size3D, 9);
    EXPECT_EQ(lut.title, "ACEScct look");

    const std::shared_ptr<const CubeLut> first = getCachedColorLut(cubePath.string());
    ASSERT_TRUE(first);
    EXPECT_EQ(getCachedColorLut(cubePath.string()), first);
    EXPECT_EQ(getCachedColorLutError(cubePath.string()), "");

    // an exported game's cooked sibling replaces the .cube once the cache lets go of it
    CubeLut cookedSource = lut;
    cookedSource.title = "cooked";
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(writeJlut(cookedSource, bytes));
    writeBytes(dir.path / "look.jlut", bytes.data(), bytes.size());
    EXPECT_EQ(getCachedColorLut(cubePath.string()), first);
    invalidateColorLutCache(cubePath.string());
    const std::shared_ptr<const CubeLut> second = getCachedColorLut(cubePath.string());
    ASSERT_TRUE(second);
    EXPECT_NE(second, first);
    EXPECT_EQ(second->title, "cooked");
    ASSERT_TRUE(loadColorLut((dir.path / "look.jlut").string(), lut, &error)) << error;
    EXPECT_EQ(lut.title, "cooked");

    // failures: kept with their error, null
    const std::filesystem::path badPath = dir.path / "bad.cube";
    const std::string bad = "LUT_3D_SIZE 2\n" + rows(3);
    writeBytes(badPath, bad.data(), bad.size());
    EXPECT_FALSE(getCachedColorLut(badPath.string()));
    EXPECT_EQ(getCachedColorLutError(badPath.string()), "line 4: Incomplete 3D LUT data (3 of 8 entries).");
    EXPECT_FALSE(loadColorLut((dir.path / "missing.cube").string(), lut, &error));
    EXPECT_EQ(error, "File not found.");
    EXPECT_TRUE(lut.empty());
    EXPECT_FALSE(getCachedColorLut(""));

    invalidateColorLutCache();
    EXPECT_EQ(getCachedColorLutError(badPath.string()), "");
}

// ---- benchmarks (AC22, R13): optimised build budgets, 4x in debug and sanitizer builds

TEST(CubeLutBench, Parse65){
    const std::string text = cubeText("Display look", 65, displayLook);
    CubeLut lut;
    std::string error;
    const double ms = bestMs(3, [&]{
        EXPECT_TRUE(parseCubeLut(text, lut, error)) << error;
    });
    EXPECT_EQ(lut.size3D, 65);
    reportBench("CubeLutBench.Parse65", ms, 150.0);
}

TEST(CubeLutBench, Bake32){
    CubeLut display, scene;
    std::string error;
    ASSERT_TRUE(parseCubeLut(cubeText("Display look", 33, displayLook), display, error)) << error;
    ASSERT_TRUE(parseCubeLut(cubeText("ACEScct look", 33, logLook), scene, error)) << error;
    PostProcessingSettings settings = gradedSettings();
    settings.temperature = 0.1f;
    std::vector<uint8_t> rgba;
    const double displayMs = bestMs(5, [&]{
        PostColorLut::bake(settings, &display, PostColorLut::Stage::ALL, 32, rgba);
    });
    reportBench("CubeLutBench.Bake32 (display .cube, stage ALL)", displayMs, 10.0);
    const double sceneMs = bestMs(5, [&]{
        PostColorLut::bake(settings, &scene, PostColorLut::Stage::ALL, 32, rgba);
    });
    reportBench("CubeLutBench.Bake32 (scene .cube, stage ALL)", sceneMs, 10.0);
}

TEST(CubeLutBench, LoadJlut){
    const TempDir dir("bench");
    CubeLut cube;
    std::string error;
    ASSERT_TRUE(parseCubeLut(cubeText("Display look", 65, displayLook), cube, error)) << error;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(writeJlut(cube, bytes));
    const std::string path = (dir.path / "look.jlut").string();
    writeBytes(path, bytes.data(), bytes.size());
    CubeLut lut;
    const double ms = bestMs(5, [&]{
        EXPECT_TRUE(loadColorLut(path, lut, &error)) << error;
    });
    EXPECT_EQ(lut.size3D, JLUT_MAX_3D_SIZE);
    reportBench("CubeLutBench.LoadJlut", ms, 5.0);
}
