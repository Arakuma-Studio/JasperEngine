// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Photoshop layer styles as Title Art styles (docs/specs/S87-title-localisation-interchange.md
// R32, §6.9, AC16): one case per §6.9 row over fixtures/psd/styles.asl and layers8.psd, written
// by tools/titleart/make_psd_fixtures.py (an independent Python writer of the descriptors). The
// improvements over Pachislop are asserted (gradient alpha stops, midpoints as `mid`, rim opacity,
// custom pattern images, every lmfx instance); the red control converts in the Pachislop-literal
// mode and the alpha and `mid` assertions fail there.

#include <gtest/gtest.h>

#include "titleart/interchange/psd/PsStyleConvert.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine::editor::psd;

namespace {

    std::vector<uint8_t> fixture(const char* name) {
        std::ifstream in(std::string(JASPER_TEST_REPO_DIR) + "/editor/tests/titleart/fixtures/psd/" + name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const PsStyleFile& library() {
        static PsStyleFile file = [] {
            PsStyleFile f;
            const std::vector<uint8_t> bytes = fixture("styles.asl");
            std::string error;
            EXPECT_TRUE(readPsStyles(bytes.data(), bytes.size(), f, error)) << error;
            return f;
        }();
        return file;
    }

    const PsStyleSource& style(const std::string& name) {
        for (const PsStyleSource& s : library().styles) {
            if (s.name == name) return s;
        }
        ADD_FAILURE() << "no style " << name;
        static PsStyleSource none;
        return none;
    }

    ConvertedPsStyle convert(const std::string& name, bool literal = false, std::vector<std::string>* saved = nullptr) {
        PsConvertOptions options;
        options.pachislopLiteral = literal;
        options.savePattern = [saved](const PsPattern& p) {
            if (saved) saved->push_back(p.name + " " + std::to_string(p.width) + "x" + std::to_string(p.height));
            return std::string("Title_assets/patterns/") + p.name + ".png";
        };
        ConvertedPsStyle out;
        std::string error;
        EXPECT_TRUE(convertPsStyle(style(name), psReferenceSize(style(name)), library().patterns, options, out, error)) << error;
        return out;
    }

    bool contains(const std::vector<std::string>& list, const std::string& text) {
        for (const std::string& s : list) if (s.find(text) != std::string::npos) return true;
        return false;
    }

    const Json* effectOf(const ConvertedPsStyle& c, const char* type, int nth = 0) {
        for (const Json& e : c.effects) {
            if (e["type"] == type && nth-- == 0) return &e;
        }
        return nullptr;
    }

    // the two improvements the red control removes: transparency stops and midpoints kept
    bool alphaAndMidKept(const ConvertedPsStyle& c) {
        const Json& stops = c.style["face"]["fill"]["gradient"]["stops"];
        if (stops.size() != 3) return false;
        return std::abs(stops[2]["alpha"].get<double>() - 0.2) < 1e-6 && std::abs(stops[1]["alpha"].get<double>() - 0.6) < 1e-6 &&
               std::abs(stops[0]["mid"].get<double>() - 0.25) < 1e-6;
    }

}

TEST(PsStyleConvert, ReadsTheLibrary) {
    ASSERT_EQ(library().kind, "asl");
    EXPECT_EQ(library().styles.size(), 17u);
    EXPECT_TRUE(contains(library().skipped, "Blending Only: no layer effects")) << "a style with blending options only is listed as skipped";
    ASSERT_EQ(library().patterns.size(), 1u);
    EXPECT_EQ(library().patterns[0].name, "Swirl");
    EXPECT_EQ(library().patterns[0].width, 8);
    EXPECT_EQ(library().patterns[0].rgba[(1 * 8 + 2) * 4 + 0], 64) << "the pattern's red plane: x * 32";
    EXPECT_EQ(style("Two Strokes").suggestedPx, 100) << "a library style has no font size: 100 px";
}

// Stroke(s): rims by reach, a stroke reaching no further than one above it dropped, lmfx kept;
// S87: stroke opacity as the rim material's opacity
TEST(PsStyleConvert, StrokesBecomeRims) {
    const ConvertedPsStyle c = convert("Two Strokes");
    ASSERT_EQ(c.style["rims"].size(), 2u) << "the inside 3 px stroke reaches less than the 4 px one above it";
    EXPECT_DOUBLE_EQ(c.style["rims"][0]["width"].get<double>(), 4.0);
    EXPECT_DOUBLE_EQ(c.style["rims"][1]["width"].get<double>(), 1.0) << "centre 10 px reaches 5: 1 % past the first";
    EXPECT_DOUBLE_EQ(c.style["rims"][0]["material"]["opacity"].get<double>(), 0.6);
    EXPECT_EQ(c.style["rims"][0]["material"]["fill"]["color"], "#ffffff");
    EXPECT_EQ(c.style["rims"][0]["join"], "round");
    EXPECT_EQ(c.summary.front(), "2 rims");
    const ConvertedPsStyle literal = convert("Two Strokes", true);
    EXPECT_FALSE(literal.style["rims"][0]["material"].contains("opacity"));
    EXPECT_TRUE(contains(literal.notes, "stroke opacity 60 % drawn opaque"));
}

// Gradient overlay: the face gradient, PS angle (counter-clockwise) to clockwise; S87: opacity
// stops as stop alpha, midpoints as `mid`
TEST(PsStyleConvert, GradientOverlayKeepsAlphaAndMidpoints) {
    const ConvertedPsStyle c = convert("Gradient Face");
    const Json& fill = c.style["face"]["fill"];
    EXPECT_EQ(fill["kind"], "linear");
    EXPECT_DOUBLE_EQ(fill["angle"].get<double>(), 270.0);
    EXPECT_TRUE(alphaAndMidKept(c));
    EXPECT_EQ(fill["gradient"]["stops"][0]["color"], "#ffe678");
    EXPECT_FALSE(contains(c.notes, "transparency stops ignored"));
}

// AC16's red control: the Pachislop-literal conversion fails the alpha and `mid` assertions
TEST(PsStyleConvert, PachislopLiteralFailsTheImprovements) {
    const ConvertedPsStyle literal = convert("Gradient Face", true);
    EXPECT_FALSE(alphaAndMidKept(literal));
    EXPECT_EQ(literal.style["face"]["fill"]["gradient"]["stops"].size(), 4u) << "the 25 % midpoint became an extra mixed stop";
    EXPECT_TRUE(contains(literal.notes, "transparency stops ignored"));
}

// Colour overlay: a solid mixed over the base by opacity; Lab (D50) converted as psStyle.ts does
TEST(PsStyleConvert, ColorOverlayMixes) {
    const ConvertedPsStyle c = convert("Color Face");
    EXPECT_EQ(c.style["face"]["fill"]["kind"], "solid");
    EXPECT_NE(c.style["face"]["fill"]["color"], "#ffffff");
    EXPECT_TRUE(contains(c.summary, "colour face"));
}

// Pattern overlay: a procedural pattern by name; S87: an unmatched bitmap becomes `custom`
TEST(PsStyleConvert, PatternOverlays) {
    const ConvertedPsStyle zebra = convert("Zebra Pattern");
    EXPECT_EQ(zebra.style["face"]["pattern"]["kind"], "zebra");
    EXPECT_DOUBLE_EQ(zebra.style["face"]["pattern"]["scale"].get<double>(), 24.0);
    EXPECT_DOUBLE_EQ(zebra.style["face"]["pattern"]["opacity"].get<double>(), 0.8);
    std::vector<std::string> saved;
    const ConvertedPsStyle bitmap = convert("Bitmap Pattern", false, &saved);
    EXPECT_EQ(bitmap.style["face"]["pattern"]["kind"], "custom");
    EXPECT_EQ(bitmap.style["face"]["pattern"]["image"]["path"], "Title_assets/patterns/Swirl.png");
    EXPECT_EQ(saved, std::vector<std::string>({"Swirl 8x8"}));
    const ConvertedPsStyle literal = convert("Bitmap Pattern", true);
    EXPECT_FALSE(literal.style["face"].contains("pattern"));
    EXPECT_TRUE(contains(literal.notes, "bitmap patterns have no equivalent"));
}

// Bevel & Emboss: technique to profile (chisel soft noted), gloss contour by name, custom contour;
// S87: direction Down as bevel.direction (Pachislop turned the light 180 degrees)
TEST(PsStyleConvert, Bevels) {
    const ConvertedPsStyle down = convert("Bevel Down");
    const Json& bevel = down.style["face"]["bevel"];
    EXPECT_EQ(bevel["profile"], "chamfer");
    EXPECT_EQ(bevel["direction"], "down");
    EXPECT_DOUBLE_EQ(bevel["light_angle"].get<double>(), 135.0);
    EXPECT_EQ(bevel["gloss"], "ring");
    EXPECT_DOUBLE_EQ(bevel["size"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(bevel["highlight_opacity"].get<double>(), 0.8);
    EXPECT_TRUE(contains(down.notes, "Chisel Soft drawn as a chamfer"));
    const ConvertedPsStyle literal = convert("Bevel Down", true);
    EXPECT_DOUBLE_EQ(literal.style["face"]["bevel"]["light_angle"].get<double>(), 315.0);
    EXPECT_FALSE(literal.style["face"]["bevel"].contains("direction"));
    const ConvertedPsStyle custom = convert("Custom Bevel");
    EXPECT_EQ(custom.style["face"]["bevel"]["profile"], "custom");
    EXPECT_EQ(custom.style["face"]["bevel"]["profile_curve"]["points"].size(), 4u);
}

// Emboss and Pillow Emboss: an S83 material effect over the layer's colours (Pillow: bevel down,
// noted); Stroke Emboss: every rim; Outer Bevel: the outermost rim
TEST(PsStyleConvert, EmbossStylesAndRimBevels) {
    const ConvertedPsStyle emboss = convert("Emboss");
    const Json* m = effectOf(emboss, "material");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ((*m)["params"]["color_mode"], "layer");
    EXPECT_TRUE((*m)["params"]["material"]["bevel"]["enabled"].get<bool>());
    EXPECT_FALSE(emboss.style["face"].contains("bevel"));
    const ConvertedPsStyle pillow = convert("Pillow");
    ASSERT_NE(effectOf(pillow, "material"), nullptr);
    EXPECT_EQ((*effectOf(pillow, "material"))["params"]["material"]["bevel"]["direction"], "down");
    EXPECT_TRUE(contains(pillow.notes, "pillow emboss"));
    const ConvertedPsStyle stroke = convert("Stroke Emboss");
    EXPECT_TRUE(stroke.style["rims"][0]["material"]["bevel"]["enabled"].get<bool>());
    const ConvertedPsStyle outer = convert("Outer Bevel");
    EXPECT_TRUE(outer.style["rims"].back()["material"]["bevel"]["enabled"].get<bool>());
    EXPECT_FALSE(outer.style["face"].contains("bevel"));
}

// Satin
TEST(PsStyleConvert, Satin) {
    const ConvertedPsStyle c = convert("Satin");
    const Json& satin = c.style["face"]["satin"];
    EXPECT_EQ(satin["blend"], "multiply");
    EXPECT_DOUBLE_EQ(satin["distance"].get<double>(), 11.0);
    EXPECT_DOUBLE_EQ(satin["size"].get<double>(), 14.0);
    EXPECT_TRUE(satin["invert"].get<bool>());
}

// Inner Glow: S80's inner_glow effect with its source (Pachislop drew Center from the edge)
TEST(PsStyleConvert, InnerGlowKeepsItsSource) {
    const ConvertedPsStyle c = convert("Center Glow");
    const Json* glow = effectOf(c, "inner_glow");
    ASSERT_NE(glow, nullptr);
    EXPECT_EQ((*glow)["params"]["source"], "center");
    EXPECT_EQ((*glow)["params"]["color"], "#008000") << "HSB 120, 100 %, 50 %";
    EXPECT_EQ((*glow)["params"]["mode"], "screen");
    EXPECT_DOUBLE_EQ((*glow)["params"]["size"].get<double>(), 8.0);
}

// Outer Glow in a light blend: mode add
TEST(PsStyleConvert, OuterGlow) {
    const ConvertedPsStyle c = convert("Screen Glow");
    const Json* glow = effectOf(c, "outer_glow");
    ASSERT_NE(glow, nullptr);
    EXPECT_EQ((*glow)["params"]["mode"], "add");
    EXPECT_DOUBLE_EQ((*glow)["params"]["spread"].get<double>(), 0.2);
    EXPECT_DOUBLE_EQ((*glow)["params"]["size"].get<double>(), 30.0);
}

// Drop and inner shadows: every instance of the lmfx list; the angle the shadow falls, clockwise
TEST(PsStyleConvert, ShadowsKeepEveryInstance) {
    const ConvertedPsStyle c = convert("Shadows");
    ASSERT_NE(effectOf(c, "drop_shadow", 0), nullptr);
    ASSERT_NE(effectOf(c, "drop_shadow", 1), nullptr) << "both drop shadows of dropShadowMulti";
    EXPECT_DOUBLE_EQ((*effectOf(c, "drop_shadow", 0))["params"]["angle"].get<double>(), 90.0) << "180 - 90 (the list's bottom first)";
    EXPECT_DOUBLE_EQ((*effectOf(c, "drop_shadow", 1))["params"]["angle"].get<double>(), 60.0);
    const Json* inner = effectOf(c, "inner_shadow");
    ASSERT_NE(inner, nullptr);
    EXPECT_TRUE((*inner)["params"]["use_global_light"].get<bool>());
    EXPECT_DOUBLE_EQ((*inner)["params"]["angle"].get<double>(), 60.0) << "the global light 120: 180 - 120";
    EXPECT_TRUE(contains(c.notes, "inner shadow: blend mode")) << "a non-Normal shadow blend is a note";
}

// Scale Effects and units: 6 pt at 72 ppi x 200 % = 12 px of a 100 px reference
TEST(PsStyleConvert, ScaleAndUnits) {
    const ConvertedPsStyle c = convert("Scaled Points");
    ASSERT_EQ(c.style["rims"].size(), 1u);
    EXPECT_DOUBLE_EQ(c.style["rims"][0]["width"].get<double>(), 12.0);
}

// Shape Burst is read natively (Pachislop monkey-patched ag-psd for it)
TEST(PsStyleConvert, ShapeBurstStroke) {
    const ConvertedPsStyle c = convert("Shape Burst");
    EXPECT_EQ(c.style["rims"][0]["material"]["fill"]["kind"], "shape_burst");
}

// Fill opacity and knockout, a PS text layer's font size as the reference, its colour as the base
TEST(PsStyleConvert, PsdLayerSources) {
    const std::vector<uint8_t> bytes = fixture("layers8.psd");
    PsStyleFile file;
    std::string error;
    ASSERT_TRUE(readPsStyles(bytes.data(), bytes.size(), file, error)) << error;
    ASSERT_EQ(file.kind, "psd");
    const PsStyleSource* title = nullptr;
    const PsStyleSource* hidden = nullptr;
    for (const PsStyleSource& s : file.styles) {
        if (s.name == "Title") title = &s;
        if (s.name == "Hidden Styled") hidden = &s;
    }
    ASSERT_NE(title, nullptr);
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(title->origin, "text layer");
    EXPECT_DOUBLE_EQ(title->fontPx, 48.0) << "24 pt x a transform of 2 at 72 ppi";
    EXPECT_EQ(title->sizeNote, "font 24 pt x 2");
    EXPECT_EQ(title->baseColor, "#ffcc00");
    EXPECT_NEAR(hidden->fillOpacity, 128 / 255.0, 1e-9);
    EXPECT_TRUE(hidden->knockout);
    ConvertedPsStyle c;
    ASSERT_TRUE(convertPsStyle(*hidden, 10, file.patterns, {}, c, error)) << error;
    EXPECT_NEAR(c.style["face"]["opacity"].get<double>(), 128 / 255.0, 1e-9);
    EXPECT_TRUE(c.style["knockout"].get<bool>());
    EXPECT_FALSE(convertPsStyle(*hidden, 0, file.patterns, {}, c, error)) << "a reference size of 0 is refused";
}

// R47: truncated and mutated libraries fail with a message, never a crash
TEST(PsStyleConvert, TruncatedFilesFail) {
    const std::vector<uint8_t> bytes = fixture("styles.asl");
    for (size_t cut : {size_t(3), size_t(8), size_t(40), bytes.size() / 2, bytes.size() - 3}) {
        PsStyleFile f;
        std::string error;
        const bool ok = readPsStyles(bytes.data(), cut, f, error);
        if (!ok) {
            EXPECT_FALSE(error.empty()) << cut;
        }
    }
}
