// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The golden driver of the effects (phase-5 spec 5.2, 5.3): every effect EffectRegistry lists
// (auto-enumerated: a new effect is tested by adding its .frag) is rendered in its fixture with
// the effect clock fixed, compared with golden/effects/<name>.t1.png (t = 1.5 s) and, when it
// is animated, <name>.t2.png (t = 4 s), and checked against the invariants I1-I7.

#include "EffectTestCommon.h"

#include "harness/ImageCompare.h"

#include "htmlui/HtmlUIClock.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    constexpr double kT1 = 1.5;
    constexpr double kT2 = 4.0;

    // paint effects that also get a gradient-text golden (<name>.text): the filter form through
    // -jfx-text-fill (the wave-2 packages add theirs: aurora, conic-spin, gradient-pan, plasma,
    // holographic, sheen)
    const std::set<std::string> kTextEffects = {"stripes"};

    bool updatingGoldens(){
        const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        return value && std::string(value) == "1";
    }

    std::vector<std::string> builtinNames(){
        std::vector<std::string> names;
        for (const EffectManifest* m : EffectRegistry::get().builtins())
            names.push_back(m->name);
        return names;
    }

    std::string goldenFile(const std::string& name){
        return sourcePath("golden/" + name + ".png");
    }

    // renders one declaration of a fixture at t, with the test pointer for pointer effects
    bool render(EffectScene& scene, const EffectManifest& m, const std::string& fixture, const std::string& declaration, double t,
                TestImage& image, std::string* error = nullptr){
        if (!scene.load(fixture, declaration, error))
            return false;
        EffectClock::setFixedTime(t);
        if (m.pointer != EffectPointer::None)
            setTestPointer(scene, m);
        return scene.capture(image);
    }

    struct EffectGoldenTest : ::testing::TestWithParam<std::string> {
        void SetUp() override{
            TestSystem::get().clearLogs();
        }
        void TearDown() override{
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
        }
    };

}

TEST_P(EffectGoldenTest, GoldensAndInvariants){
    const EffectManifest* m = EffectRegistry::get().find(GetParam());
    ASSERT_NE(m, nullptr);
    EffectScene scene;
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUpEffect(*m)) << m->name << ": its variants do not compile";
    const std::string fixture = fixtureOf(*m);
    const std::string declaration = declarationOf(*m);
    std::string error;

    // t1, twice (I2: two renders of the same frame are identical), then t2
    TestImage t1, again, t2;
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT1, t1, &error)) << error;
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT1, again, &error)) << error;
    EXPECT_EQ(maxDifference(t1, again), 0) << m->name << ": two renders at the same fixed time differ (I2)";
    ASSERT_TRUE(render(scene, *m, fixture, declaration, kT2, t2, &error)) << error;
    const CompareResult golden = compareWithGolden("effects/" + m->name + ".t1", t1.rgba, t1.width, t1.height);
    EXPECT_TRUE(golden.passed) << golden.message;

    // I3: a .t2 golden exists exactly when the effect is animated
    const bool animated = maxDifference(t1, t2) > 1;
    const std::string t2Name = "effects/" + m->name + ".t2";
    if (updatingGoldens()){
        if (animated)
            compareWithGolden(t2Name, t2.rgba, t2.width, t2.height);
        else
            std::remove(goldenFile(t2Name).c_str());
    }else if (fileExists(goldenFile(t2Name))){
        EXPECT_TRUE(animated) << m->name << ": has a .t2 golden but renders the same at t1 and t2 (I3)";
        const CompareResult second = compareWithGolden(t2Name, t2.rgba, t2.width, t2.height);
        EXPECT_TRUE(second.passed) << second.message;
    }else{
        EXPECT_FALSE(animated) << m->name << ": renders differently at t1 and t2 but has no .t2 golden (I3)";
    }

    // I1: opacity 0 is the render without the effect
    TestImage transparent, without;
    ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "opacity 0"), kT1, transparent, &error)) << error;
    ASSERT_TRUE(render(scene, *m, fixture, noEffectDeclaration(*m), kT1, without, &error)) << error;
    EXPECT_LE(maxDifference(transparent, without), 1) << m->name << ": opacity 0 differs from no effect (I1)";

    // I4 (transitions): progress 0% is no effect, 100% removes the area, invert swaps them
    if (m->category == "transition"){
        TestImage start, end, invertedStart, invertedEnd;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 0%"), kT1, start, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 100%"), kT1, end, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 0%, invert on"), kT1, invertedStart, &error)) << error;
        ASSERT_TRUE(render(scene, *m, fixture, declarationOf(*m, "progress 100%, invert on"), kT1, invertedEnd, &error)) << error;
        EXPECT_LE(maxDifference(start, without), 1) << m->name << ": progress 0% is not the content (I4)";
        EXPECT_LE(maxDifference(invertedEnd, without), 1) << m->name << ": inverted progress 100% is not the content (I4)";
        EXPECT_LE(maxDifference(end, invertedStart), 1) << m->name << ": invert does not swap the ends (I4)";
    }

    // the gradient-text golden of the listed paint effects
    if (kTextEffects.count(m->name)){
        TestImage text;
        ASSERT_TRUE(render(scene, *m, "text", "background-image: effect(" + m->name + ")", kT1, text, &error)) << error;
        const CompareResult textGolden = compareWithGolden("effects/" + m->name + ".text", text.rgba, text.width, text.height);
        EXPECT_TRUE(textGolden.passed) << textGolden.message;
    }

    // I6: a paint effect in filter form over nothing leaves nothing
    if (m->kind == EffectKind::Paint){
        TestImage empty;
        ASSERT_TRUE(render(scene, *m, "empty", "filter: effect(" + m->name + ")", kT1, empty, &error)) << error;
        int alpha = 0;
        for (size_t i = 3; i < empty.rgba.size(); i += 4)
            alpha = std::max(alpha, (int)empty.rgba[i]);
        EXPECT_EQ(alpha, 0) << m->name << ": its filter form paints over transparent content (I6)";
    }

    // I5: 0x0, 1x1, 1x400 and 400x1 elements render without a renderer error (the log is checked in TearDown)
    {
        EffectScene sizes(420, 420);
        TestImage image;
        ASSERT_TRUE(render(sizes, *m, "sizes", declaration, kT1, image, &error)) << error;
        if (m->kind == EffectKind::Paint){
            // and in filter form
            ASSERT_TRUE(render(sizes, *m, "sizes", "filter: effect(" + m->name + ")", kT1, image, &error)) << error;
        }
    }

    // I7: a manifest, a golden, a catalogue entry, and a LICENSE entry for a shaders.com port
    EXPECT_FALSE(m->summary.empty());
    EXPECT_TRUE(fileExists(goldenFile("effects/" + m->name + ".t1")));
    const std::string catalogue = readText(sourcePath("../../../docs/mcp/guide/html-ui-effects.md"));
    EXPECT_NE(catalogue.find("### `" + m->name + "`"), std::string::npos) << m->name << " has no entry in html-ui-effects.md (I7)";
    EXPECT_TRUE(fileExists(sourcePath("../../../docs/mcp/guide/img/effects/" + m->name + ".png"))) << m->name << " has no thumbnail (I7)";
    if (m->source.rfind("shaders.com", 0) == 0){
        const std::string license = readText(sourcePath("../../shaderlib/effects/LICENSE.shaders-com.txt"));
        EXPECT_NE(license.find("  " + m->name + " "), std::string::npos) << m->name << " is not listed in LICENSE.shaders-com.txt (I7)";
    }
}

INSTANTIATE_TEST_SUITE_P(EveryEffect, EffectGoldenTest, ::testing::ValuesIn(builtinNames()),
                         [](const ::testing::TestParamInfo<std::string>& info){
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
