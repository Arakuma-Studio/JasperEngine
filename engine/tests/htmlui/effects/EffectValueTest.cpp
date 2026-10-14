// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effect parameter values, every type of phase-5 spec 3.3 (5.1 EffectValueTest).

#include "htmlui/effects/EffectValue.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    constexpr float kPi = 3.14159265358979f;

    EffectParamDef def(EffectParamType type, std::vector<std::string> options = {}){
        EffectParamDef d;
        d.name = "p";
        d.type = type;
        d.options = std::move(options);
        return d;
    }

    EffectValue parsed(const EffectParamDef& d, const std::string& css){
        EffectValue v;
        std::string error;
        EXPECT_TRUE(EffectValueParser::parse(d, css, v, error)) << css << ": " << error;
        return v;
    }

    bool fails(const EffectParamDef& d, const std::string& css){
        EffectValue v;
        std::string error;
        const bool ok = EffectValueParser::parse(d, css, v, error);
        EXPECT_TRUE(ok || !error.empty());
        return !ok;
    }

    float srgb(float linear){
        return EffectValueParser::linearToSrgb(linear);
    }

}

TEST(EffectValueTest, Numbers){
    EXPECT_FLOAT_EQ(parsed(def(EffectParamType::Number), "2.5").f, 2.5f);
    EXPECT_FLOAT_EQ(parsed(def(EffectParamType::Number), "-.5").f, -0.5f);
    EXPECT_TRUE(fails(def(EffectParamType::Number), "2px"));
    EXPECT_TRUE(fails(def(EffectParamType::Number), "two"));
    EXPECT_FLOAT_EQ(parsed(def(EffectParamType::Integer), "3.6").f, 4.f);
    EXPECT_FLOAT_EQ(parsed(def(EffectParamType::Percentage), "50%").f, 0.5f);
    EXPECT_FLOAT_EQ(parsed(def(EffectParamType::Percentage), "0.25").f, 0.25f);
    EXPECT_TRUE(fails(def(EffectParamType::Percentage), "50"));
}

TEST(EffectValueTest, Angles){
    const EffectParamDef d = def(EffectParamType::Angle);
    EXPECT_NEAR(parsed(d, "90deg").f, kPi / 2, 1e-6);
    EXPECT_NEAR(parsed(d, "1rad").f, 1.f, 1e-6);
    EXPECT_NEAR(parsed(d, "100grad").f, kPi / 2, 1e-6);
    EXPECT_NEAR(parsed(d, "0.5turn").f, kPi, 1e-6);
    EXPECT_NEAR(parsed(d, "45").f, kPi / 4, 1e-6);   // a bare number is degrees
    EXPECT_TRUE(fails(d, "45px"));
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "0.25turn")), "90deg");
}

TEST(EffectValueTest, Times){
    const EffectParamDef d = def(EffectParamType::Time);
    EXPECT_FLOAT_EQ(parsed(d, "1.5s").f, 1.5f);
    EXPECT_FLOAT_EQ(parsed(d, "250ms").f, 0.25f);
    EXPECT_TRUE(fails(d, "2"));
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "250ms")), "0.25s");
}

TEST(EffectValueTest, LengthsResolve){
    const EffectParamDef d = def(EffectParamType::Length);
    EffectLengthContext context;
    context.fontSize = 20.f;
    context.rootFontSize = 10.f;
    context.viewport = Vector2(800.f, 600.f);
    context.areaShorterSide = 200.f;
    const std::pair<const char*, float> cases[] = {
        {"12px", 12.f}, {"2em", 40.f}, {"2rem", 20.f}, {"10vw", 80.f}, {"10vh", 60.f}, {"10vmin", 60.f}, {"10vmax", 80.f},
        {"25%", 50.f}, {"0", 0.f}};
    for (const auto& c : cases){
        const EffectValue v = parsed(d, c.first);
        EXPECT_FLOAT_EQ(EffectValueParser::resolveLength(v.length, context), c.second) << c.first;
    }
    EXPECT_TRUE(fails(d, "12"));
    EXPECT_TRUE(fails(d, "3cm"));
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "0")), "0px");
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "1.5em")), "1.5em");
}

TEST(EffectValueTest, ColorsEveryForm){
    const EffectParamDef d = def(EffectParamType::Color);
    // linear-sRGB, straight alpha
    EffectValue red = parsed(d, "#f00");
    EXPECT_FLOAT_EQ(red.color.x, 1.f);
    EXPECT_FLOAT_EQ(red.color.y, 0.f);
    EXPECT_FLOAT_EQ(red.color.w, 1.f);
    EffectValue half = parsed(d, "rgb(0 0 0 / 50%)");
    EXPECT_FLOAT_EQ(half.color.w, 0.5f);
    EXPECT_NEAR(parsed(d, "#ff000080").color.w, 128.f / 255.f, 1e-6);
    EXPECT_NEAR(parsed(d, "#f008").color.w, 136.f / 255.f, 1e-6);
    // sRGB 0.5 grey is linear 0.214
    EXPECT_NEAR(parsed(d, "hsl(0 0% 50%)").color.x, 0.2140f, 1e-3);
    EXPECT_NEAR(parsed(d, "hwb(0 100% 0%)").color.x, 1.f, 1e-5);
    EffectValue oklch = parsed(d, "oklch(62.8% 0.2577 29.23)");
    EXPECT_NEAR(srgb(oklch.color.x), 1.f, 0.01f);
    EXPECT_NEAR(srgb(oklch.color.y), 0.f, 0.02f);
    EXPECT_EQ(parsed(d, "transparent").color.w, 0.f);
    EXPECT_TRUE(parsed(d, "currentColor").currentColor);
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "rebeccapurple")), "#663399ff");
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "currentcolor")), "currentcolor");
    EXPECT_TRUE(fails(d, "notacolor"));
}

TEST(EffectValueTest, Positions){
    const EffectParamDef d = def(EffectParamType::Position);
    auto check = [&](const char* css, float fx, float px, float fy, float py){
        const EffectValue v = parsed(d, css);
        EXPECT_FLOAT_EQ(v.x.fraction, fx) << css;
        EXPECT_FLOAT_EQ(v.x.px, px) << css;
        EXPECT_FLOAT_EQ(v.y.fraction, fy) << css;
        EXPECT_FLOAT_EQ(v.y.px, py) << css;
    };
    check("center", 0.5f, 0.f, 0.5f, 0.f);
    check("left", 0.f, 0.f, 0.5f, 0.f);
    check("top", 0.5f, 0.f, 0.f, 0.f);
    check("25%", 0.25f, 0.f, 0.5f, 0.f);
    check("50% 0%", 0.5f, 0.f, 0.f, 0.f);
    check("10px 20px", 0.f, 10.f, 0.f, 20.f);
    check("top left", 0.f, 0.f, 0.f, 0.f);
    check("center bottom", 0.5f, 0.f, 1.f, 0.f);
    check("right 10px bottom 20%", 1.f, -10.f, 0.8f, 0.f);
    check("left 15% top", 0.15f, 0.f, 0.f, 0.f);
    EXPECT_TRUE(fails(d, "top 20%"));
    EXPECT_TRUE(fails(d, "left right"));
    EXPECT_TRUE(fails(d, "1 2 3 4 5"));
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "right 10px bottom 20%")), "calc(100% - 10px) 80%");
    // the canonical text reads back to the same value
    const EffectValue again = parsed(d, EffectValueParser::canonical(d, parsed(d, "right 10px bottom 20%")));
    EXPECT_FLOAT_EQ(again.x.fraction, 1.f);
    EXPECT_FLOAT_EQ(again.x.px, -10.f);
    EXPECT_FLOAT_EQ(EffectValueParser::resolveAxis(again.x, 200.f), 190.f);
}

TEST(EffectValueTest, EnumsAndBooleans){
    const EffectParamDef e = def(EffectParamType::Enum, {"linear", "oklch", "oklab"});
    EXPECT_FLOAT_EQ(parsed(e, "OKLab").f, 2.f);
    EXPECT_TRUE(fails(e, "hsl"));
    EXPECT_EQ(EffectValueParser::canonical(e, parsed(e, "oklch")), "oklch");
    const EffectParamDef b = def(EffectParamType::Boolean);
    for (const char* t : {"on", "true", "yes", "1"})
        EXPECT_FLOAT_EQ(parsed(b, t).f, 1.f) << t;
    for (const char* f : {"off", "false", "no", "0"})
        EXPECT_FLOAT_EQ(parsed(b, f).f, 0.f) << f;
    EXPECT_TRUE(fails(b, "2"));
    EXPECT_EQ(EffectValueParser::canonical(b, parsed(b, "on")), "true");
}

TEST(EffectValueTest, Palettes){
    const EffectParamDef d = def(EffectParamType::Palette);
    // implicit positions spread like CSS gradient stops
    const EffectValue three = parsed(d, "red blue 80% lime");
    ASSERT_EQ(three.palette.size(), 3u);
    EXPECT_FLOAT_EQ(three.palette[0].second, 0.f);
    EXPECT_FLOAT_EQ(three.palette[1].second, 0.8f);
    EXPECT_FLOAT_EQ(three.palette[2].second, 1.f);
    const EffectValue spread = parsed(d, "red 0% blue green yellow 90%");
    ASSERT_EQ(spread.palette.size(), 4u);
    EXPECT_FLOAT_EQ(spread.palette[1].second, 0.3f);
    EXPECT_FLOAT_EQ(spread.palette[2].second, 0.6f);
    EXPECT_EQ(EffectValueParser::canonical(d, parsed(d, "#00aaff #ff00aa")), "#00aaffff 0% #ff00aaff 100%");
    EXPECT_TRUE(fails(d, "red 50% blue 20%"));   // positions must not decrease
    EXPECT_TRUE(fails(d, "red"));                // 1 entry
    EXPECT_TRUE(fails(d, "red red red red red red red red red"));   // 9 entries
    EXPECT_TRUE(fails(d, "red currentColor"));
}

TEST(EffectValueTest, ClampingAndRoundTrip){
    EffectParamDef d = def(EffectParamType::Number);
    d.hasRange = true;
    d.minValue = 1.f;
    d.maxValue = 30.f;
    bool clamped = false;
    EXPECT_FLOAT_EQ(EffectValueParser::clampToRange(d, 40.f, clamped), 30.f);
    EXPECT_TRUE(clamped);
    EXPECT_FLOAT_EQ(EffectValueParser::clampToRange(d, 5.f, clamped), 5.f);
    EXPECT_FALSE(clamped);
    EffectValue v = parsed(d, "0.5");
    EXPECT_TRUE(EffectValueParser::clamp(d, v));
    EXPECT_FLOAT_EQ(v.f, 1.f);
    // canonical text reads back to the same value for every type
    struct Case { EffectParamType type; const char* css; };
    const Case cases[] = {{EffectParamType::Number, "0.1"}, {EffectParamType::Number, "1e-3"}, {EffectParamType::Percentage, "33.3%"},
                          {EffectParamType::Length, "2.25rem"}, {EffectParamType::Angle, "-30deg"}, {EffectParamType::Time, "1500ms"},
                          {EffectParamType::Color, "#12345678"}, {EffectParamType::Position, "20% 70%"},
                          {EffectParamType::Palette, "red 10% blue 50% lime"}};
    for (const Case& c : cases){
        const EffectParamDef cd = def(c.type);
        const EffectValue first = parsed(cd, c.css);
        const std::string text = EffectValueParser::canonical(cd, first);
        const EffectValue second = parsed(cd, text);
        EXPECT_EQ(EffectValueParser::canonical(cd, second), text) << c.css;
    }
    EXPECT_EQ(EffectValueParser::formatNumber(0.1f), "0.1");
    EXPECT_EQ(EffectValueParser::formatNumber(1.f / 3.f), "0.33333334");
}

TEST(EffectValueTest, VarPassthrough){
    const EffectParamDef d = def(EffectParamType::Length);
    EffectValue v;
    std::string error;
    ASSERT_TRUE(EffectValueParser::parse(d, "var(--radius, 12px)", v, error));
    EXPECT_TRUE(v.isVar);
    EXPECT_EQ(EffectValueParser::canonical(d, v), "var(--radius, 12px)");
    ASSERT_TRUE(EffectValueParser::parse(def(EffectParamType::Color), "VAR(--c)", v, error));
    EXPECT_TRUE(v.isVar);
}
