// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pachislop effect id map (docs/specs/S83-title-effects-generators.md R51, §6.9, D16, AC23):
// every mapped type and param exists in the registry, Vec2 joins name Vec2 params, the negated key
// is Extrude's light (the only effect angle whose convention differs); conversions rename, join,
// negate and set as the map says, and an unknown key fails.

#include <gtest/gtest.h>

#include "titleart/TitleEffectRegistry.h"
#include "titleart/fx/PachislopFxMap.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

TEST(PachislopFxMapTest, EveryEntryNamesOurParams) {
    const auto& map = pachislopEffectMap();
    ASSERT_GE(map.size(), 25u) << "S80's eight and S83's seventeen ported effects";
    for (const char* t : {"deepGlow", "contourLines", "motionBlur", "lensBlur", "tiltShift", "radialBlur", "extrude2d", "motionTrail", "halftone",
                          "perspectiveShadow", "floorReflection", "wave", "chromaticAberration", "grain", "glitch", "material", "shatter"}) {
        EXPECT_TRUE(map.count(t)) << t;
    }
    for (const auto& [name, e] : map) {
        SCOPED_TRACE(name);
        const titleart::EffectDef* def = titleart::findEffect(e.type);
        ASSERT_NE(def, nullptr) << e.type;
        for (const auto& [from, to] : e.params) EXPECT_NE(titleart::findParam(def->params, to), nullptr) << from << " -> " << to;
        for (const auto& [to, xy] : e.vec2) {
            const titleart::ParamDef* p = titleart::findParam(def->params, to);
            ASSERT_NE(p, nullptr) << to;
            EXPECT_EQ(p->kind, titleart::ParamKind::Vec2);
            EXPECT_EQ(xy.size(), 2u);
        }
        for (auto it = e.set.begin(); it != e.set.end(); ++it) EXPECT_NE(titleart::findParam(def->params, it.key()), nullptr) << it.key();
        for (const std::string& n : e.negate) {
            EXPECT_TRUE(e.params.count(n)) << n;
            EXPECT_EQ(name, "extrude2d") << "only Extrude's Light From changes convention among the effects";
        }
    }
}

TEST(PachislopFxMapTest, Converts) {
    const PachislopFxConversion extrude = convertPachislopEffect("extrude2d", Json{{"light", -120}, {"length", 30}});
    ASSERT_TRUE(extrude.ok) << extrude.error;
    EXPECT_EQ(extrude.type, "extrude");
    EXPECT_EQ(extrude.params["light_angle"], 120.0);
    const PachislopFxConversion shatter = convertPachislopEffect("shatter", Json{{"x", 40}, {"y", -10}, {"cracksOnly", true}});
    ASSERT_TRUE(shatter.ok) << shatter.error;
    EXPECT_EQ(shatter.params["impact"], Json::array({40, -10}));
    EXPECT_EQ(shatter.params["cracks_only"], true);
    const PachislopFxConversion glitch = convertPachislopEffect("glitch", Json{{"amount", 0.5}});
    ASSERT_TRUE(glitch.ok);
    EXPECT_EQ(glitch.params["mode"], "slices");
    const PachislopFxConversion halftone = convertPachislopEffect("halftone", Json{{"mode", "softLight"}, {"color", "#FF2A6D"}});
    ASSERT_TRUE(halftone.ok);
    EXPECT_EQ(halftone.params["mode"], "soft_light");
    EXPECT_EQ(halftone.params["color"], "#FF2A6D") << "colours are not option names";
    EXPECT_EQ(halftone.params["dot"], "round");
    EXPECT_FALSE(convertPachislopEffect("deepGlow", Json{{"bogus", 1}}).ok);
    EXPECT_FALSE(convertPachislopEffect("notAnEffect", Json::object()).ok);
}
