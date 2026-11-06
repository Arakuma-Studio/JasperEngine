// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Palette Swap's lookup tables on the CPU (docs/specs/S43-palettes.md R13-R16, D3, AC8): the
// cell grid holds every real palette (the built-ins and 400 Lospec palettes), a crafted cluster
// overflows with the colours named, and the CPU twin of the shader swaps, cycles and keeps
// what is not a source colour.

#include "pixelart/Palette.h"
#include "pixelart/PaletteMaterial.h"
#include "pixelart/PaletteSwap.h"
#include "shader/ShaderManifest.h"
#include "texture/Material.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    namespace fs = std::filesystem;

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    fs::path repo() {
        return fs::path(JASPER_TEST_REPO_DIR);
    }

    const Diagnostic* findKind(const std::vector<Diagnostic>& diagnostics, const std::string& kind) {
        for (const Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.kind == kind) return &diagnostic;
        }
        return nullptr;
    }

    Palette paletteOf(const std::vector<Rgba8>& colors) {
        Palette palette;
        palette.name = "test";
        palette.colors = colors;
        return palette;
    }

}

TEST(PaletteSwapCache, EveryBuiltinBindsWithoutOverflow) {
    const fs::path directory = repo() / "editor/resources/palettes";
    int count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".palette") continue;
        Palette palette;
        std::vector<Diagnostic> diagnostics;
        ASSERT_TRUE(PaletteCodec::read(readFile(entry.path()), palette, diagnostics)) << entry.path();
        if (palette.count() > kSwapMaxSourceColors) continue;   // not a swap source (R14)
        PaletteSwapData data;
        diagnostics.clear();
        EXPECT_TRUE(buildPaletteSwapData(palette, {}, data, diagnostics)) << entry.path();
        EXPECT_EQ(findKind(diagnostics, "palette_cell_overflow"), nullptr) << entry.path();
        count++;
    }
    EXPECT_GE(count, 37);
}

TEST(PaletteSwapCache, FourHundredLospecPalettesBindWithoutOverflow) {
    nlohmann::json root = nlohmann::json::parse(readFile(repo() / "editor/tests/fixtures/palettes/lospec-400.json"), nullptr, false);
    ASSERT_TRUE(root.is_object());
    const nlohmann::json& palettes = root["palettes"];
    ASSERT_EQ(palettes.size(), 400u);
    for (const nlohmann::json& item : palettes) {
        Palette palette;
        palette.name = item["slug"].get<std::string>();
        for (const nlohmann::json& hex : item["colors"]) {
            Rgba8 color;
            ASSERT_TRUE(parseHex(hex.get<std::string>(), color)) << palette.name;
            palette.colors.push_back(color);
        }
        PaletteSwapData data;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(buildPaletteSwapData(palette, {}, data, diagnostics)) << palette.name;
        EXPECT_EQ(findKind(diagnostics, "palette_cell_overflow"), nullptr) << palette.name;
        // every colour of it swaps from itself (the first of exact duplicates)
        for (int i = 0; i < palette.count(); i++) {
            EXPECT_EQ(data.sourceIndex(palette.colors[i]), palette.indexOf(palette.colors[i])) << palette.name << " " << i;
        }
    }
}

TEST(PaletteSwapCache, AFiveColourClusterOverflowsNamingTheColours) {
    const Palette palette = paletteOf({{8, 8, 8}, {9, 8, 8}, {10, 9, 8}, {11, 8, 15}, {12, 14, 8}, {200, 0, 0}});
    PaletteSwapData data;
    std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(buildPaletteSwapData(palette, {}, data, diagnostics));
    const Diagnostic* overflow = findKind(diagnostics, "palette_cell_overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->severity, Diagnostic::Severity::Error);
    for (const char* hex : {"#080808", "#090808", "#0a0908", "#0b080f", "#0c0e08"}) {
        EXPECT_NE(overflow->message.find(hex), std::string::npos) << hex << " in: " << overflow->message;
    }
    EXPECT_EQ(overflow->message.find("#c80000"), std::string::npos);
    EXPECT_EQ(overflow->fix, "merge these colours or use a smaller source palette");
    // the identity: nothing swaps
    EXPECT_EQ(data.sourceIndex(Rgba8{200, 0, 0}), -1);
}

TEST(PaletteSwapCache, MoreThan255ColoursIsTooLarge) {
    std::vector<Rgba8> colors;
    for (int i = 0; i < 256; i++) colors.push_back(Rgba8{(uint8_t)i, (uint8_t)(i * 7), (uint8_t)(i * 13)});
    PaletteSwapData data;
    std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(buildPaletteSwapData(paletteOf(colors), {}, data, diagnostics));
    EXPECT_NE(findKind(diagnostics, "palette_too_large"), nullptr);
}

TEST(PaletteSwapCache, SwapRowsAlphaAndUntouchedTexels) {
    Palette source = paletteOf({{0, 0, 0, 0}, {10, 10, 10}, {200, 40, 40}, {40, 200, 40}, {10, 10, 10}});
    const std::vector<PaletteSwapRow> targets = {
        {"blue", {{0, 0, 0, 0}, {1, 1, 1}, {40, 40, 200}, {40, 40, 200}, {9, 9, 9}}},
        {"hidden", {{0, 0, 0, 0}, {1, 1, 1}, {255, 255, 255, 0}}},   // shorter: the rest keep the source
    };
    PaletteSwapData data;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(buildPaletteSwapData(source, targets, data, diagnostics));
    EXPECT_NE(findKind(diagnostics, "duplicate_color"), nullptr);
    EXPECT_EQ(data.rowCount, 3);
    EXPECT_EQ(data.rowNames, (std::vector<std::string>{"", "blue", "hidden"}));

    // row 0 is the source itself
    EXPECT_EQ(data.swap(Rgba8{200, 40, 40}, 0), (Rgba8{200, 40, 40}));
    EXPECT_EQ(data.swap(Rgba8{200, 40, 40}, 1), (Rgba8{40, 40, 200}));
    // an exact duplicate swaps as its first index
    EXPECT_EQ(data.swap(Rgba8{10, 10, 10}, 1), (Rgba8{1, 1, 1}));
    // a target alpha of 0 hides; the texel's alpha multiplies the target's
    EXPECT_EQ(data.swap(Rgba8{200, 40, 40}, 2).a, 0);
    EXPECT_EQ(data.swap(Rgba8{40, 200, 40, 128}, 1), (Rgba8{40, 40, 200, 128}));
    // past a shorter target: the source's colour
    EXPECT_EQ(data.swap(Rgba8{40, 200, 40}, 2), (Rgba8{40, 200, 40}));
    // rows past the last repeat it (the shader clamps to 0-15)
    EXPECT_EQ(data.swap(Rgba8{200, 40, 40}, 9).a, 0);
    // off-palette and near-palette texels are untouched
    EXPECT_EQ(data.swap(Rgba8{201, 40, 40}, 1), (Rgba8{201, 40, 40}));
    EXPECT_EQ(data.swap(Rgba8{7, 70, 170, 99}, 1), (Rgba8{7, 70, 170, 99}));
    // a fully transparent slot is not a colour: opaque black stays black
    EXPECT_EQ(data.swap(Rgba8{0, 0, 0, 255}, 1), (Rgba8{0, 0, 0, 255}));
}

TEST(PaletteSwapCache, CyclingRotatesForwardBackwardAndFreezes) {
    Palette source = paletteOf({{10, 0, 0}, {20, 0, 0}, {30, 0, 0}, {40, 0, 0}, {99, 99, 99}});
    source.ramps.push_back({"water", {0, 1, 2, 3}, 4.0f});
    PaletteSwapData data;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(buildPaletteSwapData(source, {}, data, diagnostics));
    EXPECT_EQ(data.rowsHeight, kSwapCycleRow + 2);
    const Rgba8 first = source.colors[0];
    // 4 steps a second: every 0.25 s the next colour of the ramp
    for (int step = 0; step < 8; step++) {
        EXPECT_EQ(data.swap(first, 0, step * 0.25f, 1.0f, true), source.colors[step % 4]) << step;
        EXPECT_EQ(data.swap(first, 0, step * 0.25f, -1.0f, true), source.colors[(4 - step % 4) % 4]) << step;
        EXPECT_EQ(data.swap(first, 0, step * 0.25f, 0.0f, true), first) << step;
    }
    // a colour of no cycling ramp stays
    EXPECT_EQ(data.swap(source.colors[4], 0, 3.0f, 1.0f, true), source.colors[4]);
}

TEST(PaletteSwapCache, RowsResolveVariantsThenTargets) {
    Palette hero = paletteOf({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    hero.name = "Hero";
    hero.variants.push_back({"Team Blue", {std::nullopt, Rgba8{0, 0, 255}}});
    Palette other = paletteOf({{90, 90, 90}});
    other.name = "Other";
    Palette night = paletteOf({{9, 9, 9}, {8, 8, 8}, {7, 7, 7}});
    night.name = "Night palette";
    night.variants.push_back({"Night", {Rgba8{1, 1, 1}, Rgba8{2, 2, 2}, Rgba8{3, 3, 3}}});
    PaletteLibrary::set("tests/rows-hero.palette", hero);
    PaletteLibrary::set("tests/rows-other.palette", other);
    PaletteLibrary::set("tests/rows-night.palette", night);

    PaletteBinding binding;
    binding.source = "tests/rows-hero.palette";
    binding.targets = {PaletteTarget::parse("tests/rows-other.palette"), PaletteTarget::parse("tests/rows-night.palette#Night"),
                       PaletteTarget::parse("tests/missing.palette")};
    std::vector<Diagnostic> diagnostics;
    const std::vector<PaletteSwapRow> rows = resolvePaletteRows(hero, binding, diagnostics);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].name, "Team Blue");
    EXPECT_EQ(rows[0].colors, (std::vector<Rgba8>{{1, 2, 3}, {0, 0, 255}, {7, 8, 9}}));
    EXPECT_EQ(rows[1].name, "Other");
    EXPECT_EQ(rows[1].colors, (std::vector<Rgba8>{{90, 90, 90}, {4, 5, 6}, {7, 8, 9}}));   // missing indices keep the source
    EXPECT_EQ(rows[2].name, "Night");
    EXPECT_EQ(rows[2].colors, (std::vector<Rgba8>{{1, 1, 1}, {2, 2, 2}, {3, 3, 3}}));
    EXPECT_EQ(rows[3].colors, hero.colors);
    EXPECT_NE(findKind(diagnostics, "palette_target_missing"), nullptr);

    binding.useVariants = false;
    diagnostics.clear();
    EXPECT_EQ(resolvePaletteRows(hero, binding, diagnostics).size(), 3u);
}

TEST(PaletteSwapCache, RuntimePalettesLiveWhileHeld) {
    auto palette = std::make_shared<Palette>(paletteOf({{5, 5, 5}}));
    const std::string key = PaletteLibrary::addRuntime(palette);
    EXPECT_EQ(key.rfind("runtime:", 0), 0u);
    ASSERT_NE(PaletteLibrary::get(key), nullptr);
    palette.reset();
    EXPECT_EQ(PaletteLibrary::get(key), nullptr);
}

// a runtime palette given to a row (Lua: sprite:setPaletteTarget(2, Palette.new{...}), the
// script keeping no reference) lives as long as the material's binding names it
TEST(PaletteSwapCache, RuntimeTargetLivesWithTheMaterial) {
    PaletteLibrary::set("tests/held-source.palette", paletteOf({{10, 10, 10}, {20, 20, 20}}));
    Material material;
    PaletteBinding binding;
    binding.source = "tests/held-source.palette";
    material.shaderPalettes = ShaderPalettes{{"u_palette", binding}};
    auto palette = std::make_shared<Palette>(paletteOf({{200, 0, 0}, {0, 200, 0}}));
    const std::string key = PaletteLibrary::addRuntime(palette);
    std::string error;
    ASSERT_TRUE(setPaletteTarget(material, 1, key, &error)) << error;
    palette.reset();   // the script's last reference: a collectgarbage() before the first draw
    ASSERT_NE(PaletteLibrary::get(key), nullptr) << "the material's binding holds the runtime palette";
    std::vector<Diagnostic> diagnostics;
    const PaletteBinding* held = findPaletteBinding(material);
    ASSERT_NE(held, nullptr);
    const std::vector<PaletteSwapRow> rows = resolvePaletteRows(paletteOf({{10, 10, 10}, {20, 20, 20}}), *held, diagnostics);
    ASSERT_EQ(rows.size(), 1u);   // the rows after row 0: the target alone
    EXPECT_EQ(rows[0].colors, (std::vector<Rgba8>{{200, 0, 0}, {0, 200, 0}}));
    EXPECT_EQ(findKind(diagnostics, "palette_target_missing"), nullptr);
    // a copy of the material (Play's copy of the scene) keeps it too; the last one lets it go
    {
        Material copy = material;
        material = Material();
        EXPECT_NE(PaletteLibrary::get(key), nullptr);
    }
    EXPECT_EQ(PaletteLibrary::get(key), nullptr);
}

// a runtime palette made (Palette.new every frame) changes no binding: nothing redraws
TEST(PaletteSwapCache, MakingRuntimePalettesChangesNoGeneration) {
    const uint32_t before = PaletteLibrary::generation();
    std::vector<std::shared_ptr<Palette>> held;
    for (int i = 0; i < 10; i++) {
        held.push_back(std::make_shared<Palette>(paletteOf({{(uint8_t)i, 1, 2}})));
        PaletteLibrary::addRuntime(held.back());
    }
    EXPECT_EQ(PaletteLibrary::generation(), before);
    // nor does a first read: no binding has seen the file before
    PaletteLibrary::get("tests/never-read-before.palette");
    EXPECT_EQ(PaletteLibrary::generation(), before);
}

TEST(PaletteSwapManifest, ParsesThePaletteDirective) {
    ShaderManifest manifest;
    manifest.parse("// @shader mesh\n// @name \"T\"\n// @feature A \"A\"\n// @feature B \"B\"\n"
                   "// @palette u_palette \"Palette\" feature=A,B\n// @palette u_bad feature=C\nvoid main(){}\n", "t.frag");
    manifest.checkComplete("t.frag", true);
    ASSERT_EQ(manifest.palettes.size(), 2u);
    const ShaderManifestPalette* palette = manifest.findPalette("u_palette");
    ASSERT_NE(palette, nullptr);
    EXPECT_EQ(palette->label, "Palette");
    EXPECT_EQ(palette->features, (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(palette->cellsTexture(), "u_paletteCells");
    EXPECT_EQ(palette->rowsTexture(), "u_paletteRows");
    EXPECT_TRUE(palette->isEnabled({"B"}));
    EXPECT_FALSE(palette->isEnabled({"C"}));
    bool undeclared = false;
    for (const ShaderManifestDiagnostic& diagnostic : manifest.diagnostics) {
        undeclared = undeclared || diagnostic.message.find("undeclared feature 'C'") != std::string::npos;
    }
    EXPECT_TRUE(undeclared);
}

// R29: building one (source, targets) set costs <= 0.3 ms on a mid desktop: printed (judged on an
// idle machine, docs/testing.md)
TEST(PaletteSwapCache, BuildTime) {
    Palette source;
    source.name = "64";
    for (int i = 0; i < 64; i++) source.colors.push_back(unpackRgb(((uint32_t)i * 2654435761u) & 0xFFFFFFu));
    source.ramps.push_back({"cycle", {0, 1, 2, 3}, 4.0f});
    std::vector<PaletteSwapRow> targets;
    for (int t = 0; t < 15; t++) targets.push_back({"row " + std::to_string(t), source.colors});
    const int runs = 50;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; i++) {
        PaletteSwapData data;
        std::vector<Diagnostic> diagnostics;
        ASSERT_TRUE(buildPaletteSwapData(source, targets, data, diagnostics));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
    std::printf("[bench] palette swap build, 64 colours, 15 targets, 1 cycling ramp: %.3f ms\n", ms);
}
