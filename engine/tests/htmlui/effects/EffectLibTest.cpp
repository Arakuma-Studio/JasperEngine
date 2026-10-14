// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects' include library against the shaders.com kit's reference math (phase-5 spec 4.1
// deliverable 5, 5.1): fixtures/effects/fx/lib-test.frag evaluates jfx_hash22, jfx_perlin12,
// jfx_mx_noise3, jfx_fbm_perlin, jfx_linear_to_srgb and jfx_rgb_to_oklab at fixed inputs, one case
// per pixel of a 32 x 3 element, and the 8-bit result must be within 1/255 of the values
// tools/htmlui/kit_reference.py computes (effects/kit_reference.json).

#include "EffectTestCommon.h"

#include "json.hpp"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

TEST(EffectLibTest, LibraryMatchesKitReference){
    const nlohmann::json reference = nlohmann::json::parse(readText(sourcePath("effects/kit_reference.json")), nullptr, false);
    ASSERT_FALSE(reference.is_discarded()) << "effects/kit_reference.json does not parse";
    const int cases = reference.value("cases", 0);
    ASSERT_EQ(cases, 32);
    ASSERT_EQ(reference["rows"].size(), 3u);

    TestSystem::get().clearLogs();
    EffectScene scene(64, 8, 1);
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    std::string error;
    ASSERT_TRUE(scene.loadFile("fixtures/effects/lib-test.html", &error)) << error;
    const EffectManifest* m = EffectRegistry::get().find("lib-test");
    ASSERT_NE(m, nullptr) << "the document's @effect lib-test was not declared";
    ASSERT_TRUE(warmUpEffect(*m)) << "lib-test.frag does not compile";

    TestImage image;
    {
        FixedEffectClock clock(0.0);
        ASSERT_TRUE(scene.capture(image));
    }
    ASSERT_GE(image.width, cases);
    ASSERT_GE(image.height, 3);

    static const char* const kNames[3][3] = {
        {"hash22.x", "hash22.y", "perlin12"},
        {"mxNoiseFloat3", "fbmPerlin(4)", "linearToSrgb"},
        {"oklab.L", "oklab.a", "oklab.b"},
    };
    int failures = 0;
    for (int row = 0; row < 3; row++){
        for (int i = 0; i < cases; i++){
            const nlohmann::json& values = reference["rows"][row][i];
            const size_t at = ((size_t)row * image.width + i) * 4;
            for (int k = 0; k < 3; k++){
                const double expected = values[k].get<double>();
                const int want = (int)std::lround(std::clamp(expected, 0.0, 1.0) * 255.0);
                const int got = image.rgba[at + k];
                if (std::abs(got - want) > 1 && failures++ < 20)
                    ADD_FAILURE() << kNames[row][k] << " case " << i << ": the library gives " << got << "/255, the kit " << want
                                  << "/255 (" << expected << ")";
            }
            EXPECT_EQ(image.rgba[at + 3], 255);
        }
    }
    EXPECT_EQ(failures, 0) << failures << " values differ from tools/htmlui/kit_reference.py by more than 1/255";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
