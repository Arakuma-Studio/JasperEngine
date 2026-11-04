// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .palette codec (docs/specs/S43-palettes.md R1, R2, R46, AC1).

#include "pixelart/Palette.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    // the spec's §6.2 example, as the writer lays it out
    const char* const kExample = R"({
  "format": "jasperengine.palette",
  "version": 1,
  "name": "Hero",
  "author": "",
  "description": "",
  "source": {"kind": "lospec", "id": "endesga-32", "url": "https://lospec.com/palette-list/endesga-32", "fetched": "2026-11-04"},
  "columns": 8,
  "colors": ["#00000000", "#1a1c2c", "#5d275d", "#b13e53", "#ef7d57", "#ffcd75", "#a7f070", "#38b764"],
  "names": {"3": "cape mid", "4": "cape light"},
  "locked": [0, 1],
  "transparent": 0,
  "ramps": [
    {"name": "cape", "colors": [2, 3, 4, 5]},
    {"name": "water", "colors": [6, 7], "cycle": 4}
  ],
  "variants": [
    {"name": "Team Blue", "colors": [null, null, "#29366f", "#3b5dc9", "#41a6f6", "#73eff7"]},
    {"name": "Hurt", "colors": [null, "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff"]}
  ]
}
)";

    std::string withColors(const std::string& colors, const std::string& extra = "") {
        return std::string("{\"format\": \"jasperengine.palette\", \"version\": 1, \"colors\": ") + colors + extra + "}";
    }

    const Diagnostic* findKind(const std::vector<Diagnostic>& diagnostics, const std::string& kind) {
        for (const Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.kind == kind) return &diagnostic;
        }
        return nullptr;
    }

    void expectError(const std::string& text, const std::string& kind, const std::string& field) {
        Palette palette;
        std::vector<Diagnostic> diagnostics;
        EXPECT_FALSE(PaletteCodec::read(text, palette, diagnostics)) << text;
        const Diagnostic* found = findKind(diagnostics, kind);
        ASSERT_NE(found, nullptr) << kind << " in " << text;
        EXPECT_EQ(found->severity, Diagnostic::Severity::Error);
        EXPECT_EQ(found->field, field);
        EXPECT_FALSE(found->message.empty());
        EXPECT_FALSE(found->fix.empty());
    }

}

TEST(PaletteCodec, ExampleReadsAndWritesByteIdentically) {
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(PaletteCodec::read(kExample, palette, diagnostics));
    EXPECT_FALSE(hasErrors(diagnostics));
    EXPECT_EQ(palette.name, "Hero");
    EXPECT_EQ(palette.source.kind, "lospec");
    EXPECT_EQ(palette.source.id, "endesga-32");
    EXPECT_EQ(palette.count(), 8);
    EXPECT_EQ(palette.colors[0], (Rgba8{0, 0, 0, 0}));
    EXPECT_EQ(palette.colors[3], (Rgba8{0xb1, 0x3e, 0x53, 255}));
    EXPECT_EQ(palette.colorName(3), "cape mid");
    EXPECT_TRUE(palette.isLocked(1));
    EXPECT_EQ(palette.transparent, 0);
    ASSERT_EQ(palette.ramps.size(), 2u);
    EXPECT_FLOAT_EQ(palette.ramps[1].cycle, 4.0f);
    ASSERT_EQ(palette.variants.size(), 2u);
    EXPECT_EQ(palette.variantColor(0, 0), palette.colors[0]);
    EXPECT_EQ(palette.variantColor(0, 2), (Rgba8{0x29, 0x36, 0x6f, 255}));
    EXPECT_EQ(palette.variantColor(0, 7), palette.colors[7]);   // past a shorter list: the base
    EXPECT_EQ(PaletteCodec::write(palette), std::string(kExample));
}

TEST(PaletteCodec, LongColourListsWrapAtColumns) {
    Palette palette;
    palette.name = "Wide";
    palette.columns = 4;
    for (int i = 0; i < 10; i++) palette.colors.push_back(Rgba8{(uint8_t)i, 0, 0, 255});
    const std::string text = PaletteCodec::write(palette);
    EXPECT_NE(text.find("  \"colors\": [\n    \"#000000\", \"#010000\", \"#020000\", \"#030000\",\n"), std::string::npos) << text;
    Palette back;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(PaletteCodec::read(text, back, diagnostics));
    EXPECT_EQ(back, palette);
    EXPECT_EQ(PaletteCodec::write(back), text);
}

TEST(PaletteCodec, EachValidationErrorHasItsKindAndField) {
    expectError(withColors("[\"#zzzzzz\"]"), "invalid_value", "colors[0]");
    expectError(withColors("[\"#000000\"]", ", \"locked\": [3]"), "index_out_of_range", "locked[0]");
    expectError(withColors("[\"#000000\"]", ", \"transparent\": 1"), "index_out_of_range", "transparent");
    expectError(withColors("[\"#000000\", \"#ffffff\"]", ", \"ramps\": [{\"name\": \"a\", \"colors\": [0, 2]}]"), "index_out_of_range",
                "ramps[0].colors[1]");
    expectError(withColors("[\"#000000\", \"#ffffff\"]", ", \"ramps\": [{\"name\": \"a\", \"colors\": [0]}, {\"name\": \"a\", \"colors\": [1]}]"),
                "duplicate_name", "ramps[1].name");
    expectError(withColors("[\"#000000\", \"#ffffff\"]", ", \"variants\": [{\"name\": \"v\", \"colors\": []}, {\"name\": \"v\", \"colors\": []}]"),
                "duplicate_name", "variants[1].name");
    expectError(withColors("[\"#000000\"]", ", \"variants\": [{\"name\": \"v\", \"colors\": [null, \"#ffffff\"]}]"), "variant_too_long",
                "variants[0].colors");
    expectError(withColors("[\"#000000\", \"#ffffff\"]",
                           ", \"ramps\": [{\"name\": \"a\", \"colors\": [0, 1], \"cycle\": 2}, {\"name\": \"b\", \"colors\": [1], \"cycle\": 1}]"),
                "cycle_overlap", "ramps[1].colors[0]");
    expectError(withColors("[]"), "invalid_value", "colors");
    expectError("{\"format\": \"jasperengine.palette\", \"version\": 1, \"colors\": [", "parse_error", "");
    expectError("{\"format\": \"other\", \"version\": 1, \"colors\": []}", "parse_error", "format");
}

TEST(PaletteCodec, TooManyColours) {
    std::string colors = "[";
    for (int i = 0; i < 257; i++) colors += std::string(i ? ", " : "") + "\"#000000\"";
    colors += "]";
    expectError(withColors(colors), "too_many_colors", "colors");
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(PaletteCodec::read(withColors(colors), palette, diagnostics));
    const Diagnostic* found = findKind(diagnostics, "too_many_colors");
    ASSERT_NE(found, nullptr);
    EXPECT_NE(found->message.find("257"), std::string::npos);
    EXPECT_EQ(found->fix, "Reduce Colours… or Import the first 256");
}

TEST(PaletteCodec, DuplicateColourIsAWarning) {
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    EXPECT_TRUE(PaletteCodec::read(withColors("[\"#102030\", \"#ffffff\", \"#102030\"]"), palette, diagnostics));
    const Diagnostic* found = findKind(diagnostics, "duplicate_color");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->severity, Diagnostic::Severity::Warning);
    EXPECT_EQ(found->field, "colors[2]");
}

TEST(PaletteCodec, UnknownKeysSurviveARewrite) {
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(PaletteCodec::read(withColors("[\"#000000\"]", ", \"futureKey\": {\"a\": [1, 2]}, \"other\": \"x\""), palette, diagnostics));
    EXPECT_NE(findKind(diagnostics, "unknown_keys"), nullptr);
    palette.name = "Renamed";
    const std::string text = PaletteCodec::write(palette);
    EXPECT_NE(text.find("\"futureKey\": {\"a\":[1,2]}"), std::string::npos) << text;
    EXPECT_NE(text.find("\"other\": \"x\""), std::string::npos) << text;
    Palette back;
    diagnostics.clear();
    ASSERT_TRUE(PaletteCodec::read(text, back, diagnostics));
    EXPECT_EQ(back.unknown, palette.unknown);
    EXPECT_EQ(PaletteCodec::write(back), text);
}

TEST(PaletteCodec, NewerVersionLoadsReadOnlyWithAWarning) {
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(PaletteCodec::read("{\"format\": \"jasperengine.palette\", \"version\": 2, \"colors\": [\"#000000\"], \"newThing\": 1}",
                                   palette, diagnostics));
    EXPECT_TRUE(palette.readOnly);
    const Diagnostic* found = findKind(diagnostics, "newer_version");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->severity, Diagnostic::Severity::Warning);
    EXPECT_EQ(found->field, "version");
    EXPECT_EQ(palette.count(), 1);
}

TEST(PaletteCodec, RampHelpersAndRemap) {
    Palette palette;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(PaletteCodec::read(kExample, palette, diagnostics));
    const std::vector<Rgba8> cape = palette.rampColors("cape");
    ASSERT_EQ(cape.size(), 4u);
    EXPECT_EQ(cape[0], palette.colors[2]);
    const std::vector<Rgba8> fitted = palette.rampColors("cape", 2);
    ASSERT_EQ(fitted.size(), 2u);
    EXPECT_EQ(fitted[0], palette.colors[2]);
    EXPECT_EQ(fitted[1], palette.colors[5]);
    EXPECT_TRUE(palette.rampColors("missing").empty());
    EXPECT_EQ(palette.nearest(Rgba8{0xb0, 0x3f, 0x52}), 3);

    // reverse the colours: everything follows its colour
    std::vector<int> order;
    for (int i = palette.count() - 1; i >= 0; i--) order.push_back(i);
    Palette reversed = palette;
    reversed.remap(order);
    EXPECT_EQ(reversed.colors[7], palette.colors[0]);
    EXPECT_EQ(reversed.transparent, 7);
    EXPECT_EQ(reversed.colorName(4), "cape mid");
    EXPECT_TRUE(reversed.isLocked(6));
    EXPECT_EQ(reversed.rampColors("cape"), palette.rampColors("cape"));
    EXPECT_EQ(reversed.variantColor(0, 5), palette.variantColor(0, 2));
    diagnostics.clear();
    EXPECT_TRUE(PaletteCodec::validate(reversed, diagnostics));

    // drop colour 3: its references go
    Palette dropped = palette;
    dropped.remap({0, 1, 2, 4, 5, 6, 7});
    EXPECT_EQ(dropped.count(), 7);
    EXPECT_EQ(dropped.colorName(3), "cape light");
    EXPECT_EQ(dropped.ramps[0].colors, (std::vector<uint16_t>{2, 3, 4}));
}

TEST(PaletteLibrary, SetAndVersions) {
    Palette palette;
    palette.colors = {Rgba8{1, 2, 3}};
    const uint32_t before = PaletteLibrary::generation();
    EXPECT_EQ(PaletteLibrary::version("tests/library.palette"), 0u);
    PaletteLibrary::set("tests/library.palette", palette);
    const uint32_t first = PaletteLibrary::version("tests/library.palette");
    EXPECT_NE(first, 0u);
    EXPECT_NE(PaletteLibrary::generation(), before);
    ASSERT_NE(PaletteLibrary::get("tests/library.palette"), nullptr);
    EXPECT_EQ(PaletteLibrary::get("tests/library.palette")->colors[0], (Rgba8{1, 2, 3}));
    PaletteLibrary::set("tests/library.palette", palette);
    EXPECT_NE(PaletteLibrary::version("tests/library.palette"), first);
    std::vector<Diagnostic> diagnostics;
    EXPECT_EQ(PaletteLibrary::get("tests/does-not-exist.palette", &diagnostics), nullptr);
    EXPECT_NE(findKind(diagnostics, "not_found"), nullptr);
}
