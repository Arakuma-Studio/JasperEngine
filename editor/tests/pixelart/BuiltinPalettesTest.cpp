// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in palettes (docs/specs/S43-palettes.md R9, AC4): the 27 pixelfx ramps, the 10
// classics and pixelfx: Effects, embedded from editor/resources/palettes. The pixelfx ramps are
// checked against pixelfx-ramps.json, extracted once from pixelfx's src/core/color.js PAL.

#include "pixelart/BuiltinPalettes.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace builtinpalettes_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;

    const char* const kPixelfxRamps[] = {"fire", "ember", "lava", "smoke", "dust", "ice", "water", "electric", "volt",
                                         "poison", "holy", "heal", "arcane", "shadow", "blood", "earth", "stone", "gold",
                                         "leaf", "wood", "metal", "bone", "neon", "cyber", "matrix", "candy", "night"};
    const char* const kClassics[] = {"game-boy", "pico-8", "cga", "ega", "commodore-64", "zx-spectrum", "sweetie-16", "endesga-32",
                                     "resurrect-64", "aap-64"};

    std::vector<std::string> builtinHexes(const std::vector<px::Rgba8>& colors) {
        std::vector<std::string> hexes;
        for (const px::Rgba8& color : colors) hexes.push_back(px::toHex(color));
        return hexes;
    }

    // {"fire": ["#1f0b10", ...], ...} in color.js order
    std::vector<std::pair<std::string, std::vector<std::string>>> pixelfxRamps() {
        std::ifstream file(std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "palettes" / "pixelfx-ramps.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const nlohmann::ordered_json root = nlohmann::ordered_json::parse(text, nullptr, false);
        std::vector<std::pair<std::string, std::vector<std::string>>> ramps;
        if (!root.is_object()) return ramps;
        for (auto it = root.begin(); it != root.end(); ++it) {
            std::vector<std::string> colors;
            for (const auto& color : it.value()) colors.push_back(color.get<std::string>());
            ramps.push_back({it.key(), colors});
        }
        return ramps;
    }

    TEST(BuiltinPalettes, ThirtyEightSortedById) {
        const std::vector<BuiltinPalette>& palettes = builtinPalettes();
        std::set<std::string> expected(std::begin(kPixelfxRamps), std::end(kPixelfxRamps));
        expected.insert(std::begin(kClassics), std::end(kClassics));
        expected.insert("pixelfx-effects");
        ASSERT_EQ(expected.size(), 38u);
        std::set<std::string> ids;
        for (const BuiltinPalette& builtin : palettes) ids.insert(builtin.id);
        EXPECT_EQ(ids, expected);
        EXPECT_EQ(palettes.size(), 38u);
        EXPECT_TRUE(std::is_sorted(palettes.begin(), palettes.end(), [](const BuiltinPalette& a, const BuiltinPalette& b) { return a.id < b.id; }));
    }

    TEST(BuiltinPalettes, EveryOneValidatesAndRewritesByteForByte) {
        for (const BuiltinPalette& builtin : builtinPalettes()) {
            std::vector<px::Diagnostic> diagnostics;
            EXPECT_TRUE(px::PaletteCodec::validate(builtin.palette, diagnostics)) << builtin.id;
            for (const px::Diagnostic& diagnostic : diagnostics) {
                EXPECT_TRUE(diagnostic.severity != px::Diagnostic::Severity::Error) << builtin.id << ": " << diagnostic.message;
            }
            const std::string_view text = builtinPaletteText(builtin.id);
            ASSERT_FALSE(text.empty()) << builtin.id;
            px::Palette reread;
            std::vector<px::Diagnostic> readDiagnostics;
            ASSERT_TRUE(px::PaletteCodec::read(text, reread, readDiagnostics)) << builtin.id;
            EXPECT_EQ(px::PaletteCodec::write(reread), std::string(text)) << builtin.id;
            EXPECT_EQ(reread, builtin.palette) << builtin.id;

            EXPECT_EQ(builtin.palette.source.kind, "builtin") << builtin.id;
            EXPECT_EQ(builtin.palette.source.id, builtin.id);
            EXPECT_FALSE(builtin.palette.name.empty()) << builtin.id;
            EXPECT_FALSE(builtin.palette.author.empty()) << builtin.id;
            EXPECT_FALSE(builtin.palette.description.empty()) << builtin.id;
        }
        for (const char* id : kClassics) {
            const BuiltinPalette* builtin = findBuiltinPalette(id);
            ASSERT_NE(builtin, nullptr) << id;
            EXPECT_EQ(builtin->palette.source.url.rfind("https://", 0), 0u) << id;
        }
    }

    TEST(BuiltinPalettes, FoundByIdOrAlias) {
        const BuiltinPalette* pico = findBuiltinPalette("pico-8");
        ASSERT_NE(pico, nullptr);
        EXPECT_EQ(findBuiltinPalette("builtin:pico-8"), pico);
        EXPECT_EQ(pico->palette.count(), 16);
        EXPECT_EQ(findBuiltinPalette("builtin:no-such"), nullptr);
        EXPECT_EQ(findBuiltinPalette(""), nullptr);
        EXPECT_TRUE(builtinPaletteText("builtin:no-such").empty());
        EXPECT_EQ(builtinHexes(findBuiltinPalette("game-boy")->palette.colors),
                  (std::vector<std::string>{"#0f380f", "#306230", "#8bac0f", "#9bbc0f"}));
        EXPECT_EQ(findBuiltinPalette("ega")->palette.count(), 64);
        EXPECT_EQ(findBuiltinPalette("commodore-64")->palette.count(), 16);
        EXPECT_EQ(findBuiltinPalette("zx-spectrum")->palette.count(), 15);
    }

    TEST(BuiltinPalettes, PixelfxRampsEqualColorJs) {
        const auto ramps = pixelfxRamps();
        ASSERT_EQ(ramps.size(), 27u);
        for (size_t i = 0; i < ramps.size(); i++) {
            const auto& [name, colors] = ramps[i];
            EXPECT_EQ(name, kPixelfxRamps[i]);
            const BuiltinPalette* builtin = findBuiltinPalette(name);
            ASSERT_NE(builtin, nullptr) << name;
            const px::Palette& palette = builtin->palette;
            EXPECT_EQ(builtinHexes(palette.colors), colors) << name;
            EXPECT_EQ(palette.columns, (int)colors.size()) << name;
            EXPECT_EQ(palette.author, "pixelfx");
            ASSERT_EQ(palette.ramps.size(), 1u) << name;
            EXPECT_EQ(palette.ramps[0].name, name);
            EXPECT_EQ(builtinHexes(palette.rampColors(name)), colors) << name;
        }
    }

    TEST(BuiltinPalettes, PixelfxEffectsHoldsAllThirtyRamps) {
        const BuiltinPalette* effects = findBuiltinPalette("builtin:pixelfx-effects");
        ASSERT_NE(effects, nullptr);
        const px::Palette& palette = effects->palette;
        EXPECT_EQ(palette.name, "pixelfx: Effects");
        EXPECT_LE(palette.count(), 256);
        // exact duplicates share one index
        std::set<std::string> unique;
        for (const std::string& hex : builtinHexes(palette.colors)) unique.insert(hex);
        EXPECT_EQ(unique.size(), (size_t)palette.count());

        auto ramps = pixelfxRamps();
        // pixelfx's src/core/sim.js:13-16
        ramps.push_back({"hot", {"#fff3a6", "#ffe26b", "#ff8a2a", "#f2561d"}});
        ramps.push_back({"fire_cooling", {"#ffffff", "#fff3a6", "#ffe26b", "#ffc23d", "#ff8a2a", "#f2561d", "#c9281f", "#8a1f17", "#4a1712"}});
        ramps.push_back({"smoke_thin", {"#7b6f72", "#5d5660", "#433d48", "#2b2630", "#1a171e"}});
        ASSERT_EQ(palette.ramps.size(), 30u);
        ASSERT_EQ(ramps.size(), 30u);
        for (size_t i = 0; i < ramps.size(); i++) {
            EXPECT_EQ(palette.ramps[i].name, ramps[i].first);
            EXPECT_EQ(builtinHexes(palette.rampColors(ramps[i].first)), ramps[i].second) << ramps[i].first;
        }
    }

}
