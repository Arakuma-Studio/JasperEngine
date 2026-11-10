// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // a test-only definition, registered for one test and removed after it
    EffectDef testGlow() {
        EffectDef def;
        def.type = "test_glow";
        def.label = "Test Glow";
        def.category = "Glow";
        ParamDef size;
        size.key = "size";
        size.label = "Size";
        size.kind = ParamKind::Number;
        size.unit = ParamUnit::Px;
        size.min = 1;
        size.max = 400;
        size.defaultValue = 40;
        ParamDef color;
        color.key = "color";
        color.label = "Color";
        color.kind = ParamKind::Color;
        color.defaultValue = "#ffb000";
        color.light = true;
        ParamDef mode;
        mode.key = "mode";
        mode.label = "Mode";
        mode.kind = ParamKind::Enum;
        mode.options = {{"behind", "Behind"}, {"add", "Add"}};
        mode.defaultValue = "behind";
        mode.anim = FxAnim::Step;
        mode.interp = ParamInterp::Stepped;
        ParamDef ramp;
        ramp.key = "gradient";
        ramp.label = "Gradient";
        ramp.kind = ParamKind::Gradient;
        ParamDef angle;
        angle.key = "angle";
        angle.label = "Angle";
        angle.kind = ParamKind::Number;
        angle.unit = ParamUnit::Deg;
        angle.defaultValue = 60;
        def.params = {size, color, mode, ramp, angle};
        def.reach = [](const ParamValues& p) { return (float)p.number("size"); };
        def.addsLight = [](const ParamValues& p) { return p.string("mode") == "add"; };
        def.passVersion = 3;
        return withGlobalLight(def, "angle", LightRelation::Away);
    }

    class TitleEffectRegistryTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_TRUE(TitleEffectRegistry::get().add(testGlow())); }
        void TearDown() override { TitleEffectRegistry::get().removeEffect("test_glow"); }
    };

}

TEST_F(TitleEffectRegistryTest, TypesRegisterOnce) {
    EXPECT_FALSE(TitleEffectRegistry::get().add(testGlow()));
    EffectDef nameless;
    EXPECT_FALSE(TitleEffectRegistry::get().add(nameless));
    ASSERT_TRUE(findEffect("test_glow"));
    EXPECT_STREQ(findEffect("test_glow")->label, "Test Glow");
    EXPECT_EQ(findEffect("nope"), nullptr);
}

TEST_F(TitleEffectRegistryTest, DefinitionDrivesDocumentDefaults) {
    const std::string text = R"({"format": "jasperengine.titleart", "version": 1, "layers": [
        {"id": "00000001", "type": "text", "effects": [
            {"id": "00000002", "type": "test_glow", "params": {"size": 900, "mode": "sideways", "custom": 5}}]}]})";
    const TitleArtParseResult parsed = parseTitleArt(text);
    ASSERT_TRUE(parsed.ok);
    const Json& params = parsed.doc.root["layers"][0]["effects"][0]["params"];
    EXPECT_EQ(params["size"], 400) << "clamped to the definition's range";
    EXPECT_EQ(params["mode"], "behind") << "an unknown enum value takes the default";
    EXPECT_EQ(params["color"], "#ffb000") << "a missing param takes the definition's default";
    EXPECT_EQ(params["use_global_light"], true) << "withGlobalLight's param";
    EXPECT_EQ(params["custom"], 5) << "unknown params are kept";
    EXPECT_EQ(params["gradient"]["stops"].size(), 2u) << "object params through their schema kind";
    std::vector<std::string> keys;
    for (auto it = params.begin(); it != params.end(); ++it) keys.push_back(it.key());
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end())) << "params are written sorted by key";
    bool clampWarned = false;
    for (const TitleWarning& w : parsed.warnings) clampWarned |= w.path == "layers[0].effects[0].params.size";
    EXPECT_TRUE(clampWarned);
    EXPECT_EQ(writeTitleArt(parseTitleArt(writeTitleArt(parsed.doc)).doc), writeTitleArt(parsed.doc));
}

TEST_F(TitleEffectRegistryTest, ParamsAreFieldPaths) {
    TitleArtDoc doc = makeNewTitleArt();
    Json& layer = doc.layers()[0];
    layer["effects"] = Json::array({Json{{"id", "0000000e"}, {"type", "test_glow"}, {"params", Json::object()}}});
    normalizeTitleArt(doc);
    FieldSetResult set = setTitleArtField(doc.layers()[0], "layer", "fx:test_glow.size", 1000);
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_EQ(set.longPath, "effects[0].params.size");
    EXPECT_EQ(set.to, 400);
    EXPECT_TRUE(set.clamped);
    ASSERT_TRUE(set.param);
    EXPECT_EQ(set.param->unit, ParamUnit::Px);
    EXPECT_EQ(set.ownerLabel, "Test Glow");
    FieldSetResult typo = setTitleArtField(doc.layers()[0], "layer", "fx:test_glow.sise", 10);
    EXPECT_FALSE(typo.ok);
    EXPECT_EQ(typo.nearest, "size");
    FieldSetResult ramp = setTitleArtField(doc.layers()[0], "layer", "fx:test_glow.gradient.interp", "vivid");
    EXPECT_TRUE(ramp.ok) << ramp.error;
    FieldSetResult wrong = setTitleArtField(doc.layers()[0], "layer", "fx:test_glow.color", 5);
    EXPECT_EQ(wrong.code, "bad_value");
    FieldSetResult enabled = setTitleArtField(doc.layers()[0], "layer", "fx:test_glow.enabled", false);
    ASSERT_TRUE(enabled.ok) << enabled.error;
    EXPECT_EQ(doc.layers()[0]["effects"][0]["enabled"], false);
}

TEST_F(TitleEffectRegistryTest, GlobalLightDrivesTheBoundAngle) {
    const EffectDef* def = findEffect("test_glow");
    ASSERT_TRUE(def);
    ASSERT_TRUE(def->globalLight.angleKey);
    EXPECT_EQ(def->globalLight.convention, AngleConvention::ClockwiseFromRight);
    ASSERT_TRUE(findParam(def->params, "use_global_light"));
    // the param sits after the angle
    size_t angle = 0, use = 0;
    for (size_t i = 0; i < def->params.size(); i++) {
        if (std::string(def->params[i].key) == "angle") angle = i;
        if (std::string(def->params[i].key) == "use_global_light") use = i;
    }
    EXPECT_EQ(use, angle + 1);
    Json params = defaultParams(def->params);
    params["angle"] = 10;
    const ParamValues following(params, &def->globalLight, 120.0, 30.0);
    EXPECT_DOUBLE_EQ(following.number("angle"), 60.0) << "a light from 120 (upper left) casts a shadow falling 60 clockwise";
    params["use_global_light"] = false;
    const ParamValues own(params, &def->globalLight, 120.0, 30.0);
    EXPECT_DOUBLE_EQ(own.number("angle"), 10.0);
    GlobalLightBinding from{"angle", nullptr, LightRelation::From, AngleConvention::CounterClockwiseFromRight};
    EXPECT_DOUBLE_EQ(angleFromGlobalLight(from, 120.0), 120.0);
    GlobalLightBinding floor{"angle", nullptr, LightRelation::Floor, AngleConvention::ClockwiseFromRight};
    EXPECT_DOUBLE_EQ(angleFromGlobalLight(floor, 120.0), -60.0) << "away mirrored vertically: up-right";
}

TEST_F(TitleEffectRegistryTest, BakeVersionSumsThePassVersionsUsed) {
    TitleArtDoc doc = makeNewTitleArt();
    EXPECT_EQ(titleArtBakeVersion(doc), (uint32_t)TITLE_ART_BAKE_VERSION_BASE);
    doc.layers()[0]["effects"] = Json::array({Json{{"type", "test_glow"}}, Json{{"type", "test_glow"}}});
    normalizeTitleArt(doc);
    EXPECT_EQ(titleArtBakeVersion(doc), (uint32_t)TITLE_ART_BAKE_VERSION_BASE + 3) << "each type once";
}

TEST_F(TitleEffectRegistryTest, ParamValuesReadTypedValues) {
    const EffectDef* def = findEffect("test_glow");
    const Json params = defaultParams(def->params);
    const ParamValues values(params);
    EXPECT_DOUBLE_EQ(values.number("size"), 40.0);
    EXPECT_EQ(values.string("mode"), "behind");
    EXPECT_NEAR(values.color("color").r, 1.0f, 1e-6);
    EXPECT_EQ(values.gradient("gradient").stops.size(), 2u);
    EXPECT_DOUBLE_EQ(values.number("missing", 7.0), 7.0);
    EXPECT_FLOAT_EQ(def->reach(values), 40.0f);
    EXPECT_FALSE(def->addsLight(values));
}
