// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ColorValue against css-parsing-tests color_*.json (phase-2 §4 p2-css): validity (null when
// browsers reject the color) and the channel values of the serialization the tests expect,
// in the color's own space; then the 8-bit sRGB hex the rewriter writes.

#include "CompatTestCommon.h"

#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/NamedColors.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    // an expected serialization: "rgb(1, 2, 3)", "rgba(...)", "lab(10 20 30 / 0.5)", "color(srgb 0.1 0.2 0.3)"
    struct Expected {
        std::string function;          // rgb, lab, lch, oklab, oklch, color
        std::string space;             // color(): the space
        double channels[3] = {0, 0, 0};
        bool none[3] = {false, false, false};
        double alpha = 1;
    };

    bool parseExpected(const std::string& text, Expected& out){
        const size_t open = text.find('(');
        if (open == std::string::npos || text.back() != ')')
            return false;
        out.function = text.substr(0, open);
        std::string body = text.substr(open + 1, text.size() - open - 2);
        for (char& c : body){
            if (c == ',' || c == '/')
                c = ' ';
        }
        std::istringstream stream(body);
        std::vector<std::string> words;
        std::string word;
        while (stream >> word)
            words.push_back(word);
        size_t i = 0;
        if (out.function == "color"){
            if (words.empty())
                return false;
            out.space = words[i++];
        }
        if (out.function == "rgba" || out.function == "rgb")
            out.function = "rgb";
        for (int c = 0; c < 3; c++){
            if (i >= words.size())
                return false;
            if (words[i] == "none")
                out.none[c] = true;
            else
                out.channels[c] = std::atof(words[i].c_str());
            i++;
        }
        if (i < words.size())
            out.alpha = words[i] == "none" ? 0.0 : std::atof(words[i].c_str());
        return true;
    }

    bool unsupportedInput(const std::string& input){
        // device-cmyk() and custom color profiles (color(--x ...)) need @color-profile, which the engine has no use for
        return input.find("device-cmyk") != std::string::npos || input.find("color(--") != std::string::npos;
    }

    std::string describe(const CssColor& color){
        std::ostringstream s;
        s << ColorValue::spaceName(color.space) << (color.legacy ? " legacy" : "") << " " << color.c[0] << " " << color.c[1] << " "
          << color.c[2] << " / " << color.alpha;
        return s.str();
    }

    // compares a parsed color with an expected serialization; "" when equal
    std::string compareColor(const CssColor& color, const Expected& expected){
        CssColor actual = color;
        double scale = 1.0;
        double tolerance = 1e-3;
        if (expected.function == "rgb"){
            if (!(color.space == ColorSpace::Srgb))
                return "space " + std::string(ColorValue::spaceName(color.space)) + " instead of rgb";
            scale = 255.0;
            tolerance = 0.02;
        }else{
            const std::string space = expected.function == "color" ? expected.space : expected.function;
            std::optional<ColorSpace> wanted = ColorValue::findSpace(space);
            if (!wanted || *wanted != color.space)
                return "space " + std::string(ColorValue::spaceName(color.space)) + " instead of " + space;
        }
        for (int c = 0; c < 3; c++){
            const double value = actual.c[c] * scale;
            if (expected.none[c]){
                if (!actual.none[c] && std::fabs(value) > tolerance)
                    return "channel " + std::to_string(c) + " should be none";
                continue;
            }
            const double relative = std::max(1.0, std::fabs(expected.channels[c]));
            if (std::fabs(value - expected.channels[c]) > tolerance * relative)
                return "channel " + std::to_string(c) + " is " + std::to_string(value) + ", expected " + std::to_string(expected.channels[c]);
        }
        if (std::fabs(actual.alpha - expected.alpha) > 2e-3)
            return "alpha is " + std::to_string(actual.alpha) + ", expected " + std::to_string(expected.alpha);
        return std::string();
    }

    void runFile(const std::string& name){
        SCOPED_TRACE(name);
        const nlohmann::json tests = loadParsingTests(name);
        ASSERT_TRUE(tests.is_array()) << name;
        size_t checked = 0;
        for (size_t i = 0; i + 1 < tests.size(); i += 2){
            const std::string input = tests[i].get<std::string>();
            nlohmann::json expected = tests[i + 1];
            if (unsupportedInput(input))
                continue;
            if (expected.is_array())
                expected = expected[0];   // light-dark(): the light color
            ColorParseInfo info;
            std::optional<CssColor> color = ColorValue::parse(input, &info);
            checked++;
            if (expected.is_null()){
                EXPECT_FALSE(color.has_value()) << "'" << input << "' should be rejected, parsed as " << (color ? describe(*color) : "");
                continue;
            }
            const std::string text = expected.get<std::string>();
            if (!color){
                ADD_FAILURE() << "'" << input << "' should parse as " << text << " (" << info.error << ")";
                continue;
            }
            Expected parsed;
            ASSERT_TRUE(parseExpected(text, parsed)) << text;
            const std::string problem = compareColor(*color, parsed);
            EXPECT_TRUE(problem.empty()) << "'" << input << "' -> " << describe(*color) << ": " << problem << " (expected " << text << ")";
        }
        EXPECT_GT(checked, 0u);
    }

}

TEST(CompatColor, Hexadecimal){
    runFile("color_hexadecimal_3.json");
    runFile("color_hexadecimal_4.json");
}

TEST(CompatColor, Keywords){
    runFile("color_keywords_3.json");
    runFile("color_keywords_4.json");
}

TEST(CompatColor, Hsl){
    runFile("color_hsl_3.json");
    runFile("color_hsl_4.json");
}

TEST(CompatColor, Hwb){
    runFile("color_hwb_4.json");
}

TEST(CompatColor, LabLch){
    runFile("color_lab_4.json");
    runFile("color_lch_4.json");
}

TEST(CompatColor, OklabOklch){
    runFile("color_oklab_4.json");
    runFile("color_oklch_4.json");
}

TEST(CompatColor, ColorFunction){
    runFile("color_function_4.json");
    runFile("color_functions_5.json");
}

TEST(CompatColor, NamedColorTable){
    // 148 named colors and transparent; sorted for the bisection
    EXPECT_EQ(namedColors().size(), 149u);
    for (size_t i = 1; i < namedColors().size(); i++)
        EXPECT_LT(std::string(namedColors()[i - 1].name), std::string(namedColors()[i].name));
    for (size_t i = 1; i < systemColors().size(); i++)
        EXPECT_LT(std::string(systemColors()[i - 1].name), std::string(systemColors()[i].name));
    EXPECT_EQ(findNamedColor("RebeccaPurple")->rgba, 0x663399ffu);
    EXPECT_EQ(findSystemColor("Highlight")->rgba, 0x3390ffffu);
    EXPECT_EQ(findSystemColor("canvastext")->rgba, 0x000000ffu);
    EXPECT_EQ(findNamedColor("currentcolor"), nullptr);
}

TEST(CompatColor, HexOutput){
    auto hex = [](const char* css){
        ColorParseInfo info;
        std::optional<CssColor> color = ColorValue::parse(css, &info);
        return color ? ColorValue::hex(ColorValue::toRgba8(*color)) : std::string("null");
    };
    EXPECT_EQ(hex("rgba(255, 0, 0, .5)"), "#ff000080");             // RmlUi would read alpha 0.5 as 0
    EXPECT_EQ(hex("rgb(0 0 0 / 50%)"), "#00000080");
    EXPECT_EQ(hex("hsl(120 50% 50% / .5)"), "#40bf4080");
    EXPECT_EQ(hex("#abc"), "#aabbccff");
    EXPECT_EQ(hex("#abcd"), "#aabbccdd");
    EXPECT_EQ(hex("transparent"), "#00000000");
    EXPECT_EQ(hex("oklch(62.8% 0.2577 29.23)"), "#ff0000ff");
    EXPECT_EQ(hex("lab(54.29 80.82 69.89)"), "#ff0000ff");
    EXPECT_EQ(hex("color(srgb 1 0.5 0)"), "#ff8000ff");
    EXPECT_EQ(hex("color-mix(in srgb, red, blue)"), "#800080ff");
    EXPECT_EQ(hex("color-mix(in srgb, red 25%, blue)"), "#4000bfff");
    EXPECT_EQ(hex("color-mix(in srgb, red 20%, transparent)"), "#ff000033");
    EXPECT_EQ(hex("rgb(from #336699 r g b / 25%)"), "#33669940");
    EXPECT_EQ(hex("hsl(from red calc(h + 120) s l)"), "#00ff00ff");
    EXPECT_EQ(hex("light-dark(white, black)"), "#ffffffff");
    EXPECT_EQ(hex("Canvas"), "#ffffffff");
    EXPECT_EQ(hex("rgb(300 -20 128)"), "#ff0080ff");

    bool clipped = false;
    ColorParseInfo info;
    std::optional<CssColor> p3 = ColorValue::parse("color(display-p3 1 0 0)", &info);
    ASSERT_TRUE(p3.has_value());
    ColorValue::toRgba8(*p3, &clipped);
    EXPECT_TRUE(clipped);

    ColorParseInfo current;
    EXPECT_FALSE(ColorValue::parse("currentColor", &current).has_value());
    EXPECT_TRUE(current.currentColor);
    ColorParseInfo dynamic;
    EXPECT_FALSE(ColorValue::parse("color-mix(in srgb, var(--x), red)", &dynamic).has_value());
    EXPECT_TRUE(dynamic.dynamic);
    EXPECT_TRUE(dynamic.dynamicVar);
    EXPECT_FALSE(dynamic.currentColorMix);
    // a color-mix() of currentColor is valid, with a value known for an element only (p3-values)
    ColorParseInfo mix;
    EXPECT_FALSE(ColorValue::parse("color-mix(in srgb, currentColor 30%, red)", &mix).has_value());
    EXPECT_TRUE(mix.currentColorMix);
    EXPECT_FALSE(mix.dynamicVar);
    ColorParseInfo nested;
    EXPECT_FALSE(ColorValue::parse("color-mix(in oklch, color-mix(in srgb, currentcolor, red), blue 10%)", &nested).has_value());
    EXPECT_TRUE(nested.currentColorMix);
    ColorParseInfo bad;
    EXPECT_FALSE(ColorValue::parse("color-mix(in srgb, currentcolor 150%, red)", &bad).has_value());
    EXPECT_FALSE(bad.currentColorMix);
}
