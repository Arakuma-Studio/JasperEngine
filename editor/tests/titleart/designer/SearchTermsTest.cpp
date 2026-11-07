// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's search vocabulary (docs/specs/S82-title-designer.md R26, R85, AC9):
// editor/resources/titleart/search-terms.json as embedded in the editor.

#include "titleart/designer/TitleSearchFold.h"
#include "titleart/designer/TitleSearchTerms.h"

#include "titleart/TitleEffectRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    bool has(const std::vector<std::string>& terms, const std::string& term) {
        return std::find(terms.begin(), terms.end(), term) != terms.end();
    }

}

// Red control: a type registered without a names_ja entry (delete "outer_glow" from the file,
// or register an S83 type the file does not list) fails here with its id.
TEST(SearchTermsTest, EveryEffectHasJapaneseName) {
    const SearchTerms& terms = SearchTerms::builtin();
    ta::TitleEffectRegistry& registry = ta::TitleEffectRegistry::get();
    ASSERT_FALSE(registry.effects().empty());
    for (const ta::EffectDef* effect : registry.effects()) {
        EXPECT_FALSE(terms.japaneseName(effect->type).empty()) << "effect " << effect->type << " has no names_ja entry";
    }
    for (const ta::GeneratorDef* generator : registry.generators()) {
        EXPECT_FALSE(terms.japaneseName(generator->type).empty()) << "generator " << generator->type << " has no names_ja entry";
    }
    // the S83 catalogue (docs/specs/S83-title-effects-generators.md §6.6, §6.7) is covered before
    // it registers, so this test stays green when it lands
    for (const char* type : {"light_glow", "contour_lines", "motion_blur", "lens_blur", "tilt_shift", "radial_blur",
                             "extrude", "motion_trail", "halftone", "perspective_shadow", "floor_reflection", "wave_warp",
                             "chromatic_aberration", "grain", "glitch", "material", "shatter", "distress", "drips",
                             "scanlines", "pixelate", "brush_stroke", "frost", "neon_tube", "chalk", "stitch", "holographic",
                             "particles", "ink_splatter", "speed_lines", "light_streaks", "shockwave", "magic_circle",
                             "brush_swirl", "glints", "lens_flare", "backlight", "fire", "smoke", "aura", "lightning",
                             "sunburst", "fractal_noise"}) {
        EXPECT_FALSE(terms.japaneseName(type).empty()) << type;
    }
    EXPECT_EQ(terms.japaneseName("outer_glow"), "光彩 (外側)");
    EXPECT_TRUE(terms.japaneseName("no_such_effect").empty());
}

// Red control: a file with "version": 2, or a group that is not an array, fails to parse (the
// builtin() then is empty and every expectation below fails).
TEST(SearchTermsTest, ParsesVersionOneWithFoldedSymmetricGroups) {
    SearchTerms parsed;
    std::string error;
    ASSERT_TRUE(SearchTerms::parse(SearchTerms::builtinText(), parsed, &error)) << error;
    EXPECT_EQ(parsed.version(), 1);
    EXPECT_GE(parsed.groups().size(), 22u);       // Pachislop's groups and ours
    EXPECT_EQ(SearchTerms::builtin().version(), 1);

    const SearchTerms& terms = SearchTerms::builtin();
    // stored folded: フチ is looked up as ふち, ボカシ and ぼかし are one term
    EXPECT_TRUE(has(terms.synonymsOf(foldForSearch("フチ")), "rim"));
    EXPECT_TRUE(has(terms.synonymsOf(foldForSearch("フチ")), "outline"));
    EXPECT_TRUE(has(terms.synonymsOf("rim"), foldForSearch("フチ")));     // both directions
    EXPECT_TRUE(has(terms.synonymsOf(foldForSearch("ボカシ")), "blur"));
    EXPECT_TRUE(has(terms.synonymsOf("blur"), "ぼかし"));
    EXPECT_TRUE(has(terms.synonymsOf("光彩"), "glow"));
    EXPECT_TRUE(has(terms.synonymsOf("縦書き"), "vertical"));
    EXPECT_TRUE(has(terms.synonymsOf("長体"), "horizontal scale"));
    EXPECT_TRUE(has(terms.synonymsOf("縦中横"), "tate chu yoko"));
    EXPECT_TRUE(has(terms.synonymsOf("dry brush"), "かすれ"));            // a phrase
    EXPECT_FALSE(has(terms.synonymsOf("rim"), "rim"));                     // not itself
    EXPECT_TRUE(terms.synonymsOf("zzz").empty());

    EXPECT_FALSE(SearchTerms::parse(R"({"version": 2, "groups": [], "names_ja": {}})", parsed, &error));
    EXPECT_FALSE(SearchTerms::parse(R"({"version": 1, "groups": ["rim"], "names_ja": {}})", parsed, &error));
    EXPECT_FALSE(SearchTerms::parse(R"({"version": 1, "groups": [], "names_ja": {"a": 1}})", parsed, &error));
    EXPECT_FALSE(SearchTerms::parse("{not json", parsed, &error));
    ASSERT_TRUE(SearchTerms::parse(R"({"version": 1, "groups": [["Rim", "フチ", "ふち"]], "names_ja": {"outline": "境界線"}})", parsed, &error));
    ASSERT_EQ(parsed.groups().size(), 1u);
    EXPECT_EQ(parsed.groups()[0], (std::vector<std::string>{"rim", "ふち"}));
    EXPECT_EQ(parsed.japaneseName("outline"), "境界線");
}
