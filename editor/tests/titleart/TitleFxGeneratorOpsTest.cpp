// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's generator side of edit_title and check_title (docs/specs/S83-title-effects-generators.md
// R6, R7, R45, D3, §6.11): add_layer {type: "generator", generator, preset?, follow?, place?,
// group?} applies a preset's params and layer look (the first one without `preset`), puts a
// following generator above or below its layer and wraps an Add-blended companion in "<layer>
// FX"; unknown presets fail naming the valid ones; fx_follow_missing reports a generator whose
// layer is gone and its fix op passes when applied. The embedded preset files parse and set only
// known params inside their ranges.

#include <gtest/gtest.h>

#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/fx/FxGeneratorChecks.h"
#include "titleart/fx/FxPresets.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    // a following generator of the test's own (no preset file)
    titleart::GeneratorDef testFollower() {
        titleart::GeneratorDef def;
        def.type = "test_follower";
        def.label = "Test Follower";
        def.category = "Energy";
        def.follow = titleart::FollowMode::Required;
        return def;
    }

    // Base (a shape at the bottom), Title (text) above it
    TitleArtDoc sample() {
        const std::string text = R"({"format": "jasperengine.titleart", "version": 1,
          "layers": [
            {"id": "00000001", "type": "shape", "name": "Base"},
            {"id": "00000002", "type": "text", "name": "Title", "size": 200}
          ]})";
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    const TitleOpProblem* failure(const TitleOpsResult& r) {
        for (const TitleOpProblem& p : r.problems) {
            if (p.severity == "error") return &p;
        }
        return nullptr;
    }

    class TitleFxGeneratorOpsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(titleart::TitleEffectRegistry::get().add(testFollower()));
            doc = sample();
        }
        void TearDown() override { titleart::TitleEffectRegistry::get().removeGenerator("test_follower"); }
        TitleArtDoc doc;
    };

}

TEST_F(TitleFxGeneratorOpsTest, PresetFilesAreClean) {
    const std::vector<std::string> problems = checkTitleFxPresets();
    for (const std::string& p : problems) ADD_FAILURE() << p;
    EXPECT_FALSE(titleFxPresets("shockwave").empty());
}

TEST_F(TitleFxGeneratorOpsTest, AddGeneratorAppliesItsPreset) {
    const TitleFxPreset* neon = findTitleFxPreset("shockwave", "neon");
    ASSERT_TRUE(neon);
    const TitleOpsResult r = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "shockwave"}, {"preset", "neon"}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& layer = r.doc.layers().back();
    EXPECT_EQ(layer.value("name", std::string()), neon->name);
    EXPECT_EQ(layer["generator"]["params"]["rings"], 5);
    EXPECT_EQ(layer["generator"]["params"]["profile"], "hard");
    EXPECT_EQ(layer.value("blend", std::string()), "add");
    ASSERT_TRUE(layer["effects"].is_array());
    ASSERT_EQ(layer["effects"].size(), 1u);
    EXPECT_EQ(layer["effects"][0]["type"], "outer_glow");
    EXPECT_EQ(layer["effects"][0]["params"]["mode"], "add");
    // no preset: the first one (R7)
    const TitleOpsResult plain = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "shockwave"}});
    ASSERT_TRUE(plain.ok) << plain.error();
    EXPECT_EQ(plain.doc.layers().back().value("name", std::string()), titleFxPresets("shockwave").front().name);
    // an unknown preset fails naming the nearest
    const TitleOpsResult bad = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "shockwave"}, {"preset", "neom"}});
    ASSERT_FALSE(bad.ok);
    ASSERT_TRUE(failure(bad));
    EXPECT_EQ(failure(bad)->nearest, "neon");
}

TEST_F(TitleFxGeneratorOpsTest, FollowPlaceAndGroup) {
    // below its layer, as a preset or the op places it
    TitleOpsResult below = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"}, {"place", "below"}});
    ASSERT_TRUE(below.ok) << below.error();
    ASSERT_EQ(below.doc.layers().size(), 3u);
    EXPECT_EQ(below.doc.layers()[1].value("follow", std::string()), "00000002");
    EXPECT_EQ(below.doc.layers()[2].value("name", std::string()), "Title");
    // an Add-blended companion above a layer with a sibling below it: "auto" wraps the two in "Title FX"
    TitleOpsResult grouped = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"},
                                                          {"group", "auto"}, {"values", {{"blend", "add"}}}});
    ASSERT_TRUE(grouped.ok) << grouped.error();
    ASSERT_EQ(grouped.doc.layers().size(), 2u);
    const Json& group = grouped.doc.layers()[1];
    EXPECT_EQ(group.value("type", std::string()), "group");
    EXPECT_EQ(group.value("name", std::string()), "Title FX");
    ASSERT_EQ(group["children"].size(), 2u);
    EXPECT_EQ(group["children"][0].value("name", std::string()), "Title");
    EXPECT_EQ(group["children"][1].value("follow", std::string()), "00000002");
    // a Normal companion is not wrapped by "auto"; "always" wraps it; "none" never does
    TitleOpsResult normal = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"}, {"group", "auto"}});
    ASSERT_TRUE(normal.ok) << normal.error();
    EXPECT_EQ(normal.doc.layers().size(), 3u);
    TitleOpsResult always = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"}, {"group", "always"}});
    ASSERT_TRUE(always.ok) << always.error();
    EXPECT_EQ(always.doc.layers().size(), 2u);
    // `follow` on a generator that follows nothing, and S83 members on another layer type, fail
    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "shockwave"}, {"follow", "Title"}}).ok);
    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "shape"}, {"preset", "x"}}).ok);
    EXPECT_FALSE(TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"}, {"group", "maybe"}}).ok);
}

TEST_F(TitleFxGeneratorOpsTest, FollowMissingReportsAndFixes) {
    TitleOpsResult added = TitleArtOps::apply(doc, Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", "test_follower"}, {"follow", "Title"}});
    ASSERT_TRUE(added.ok) << added.error();
    EXPECT_TRUE(titleFollowProblems(added.doc).empty());
    // the followed layer removed: S80 clears the reference, so the required follow is empty
    TitleOpsResult removed = TitleArtOps::apply(added.doc, Json{{"op", "remove_layer"}, {"layer", "Title"}});
    ASSERT_TRUE(removed.ok) << removed.error();
    ASSERT_EQ(titleFollowProblems(removed.doc).size(), 1u);
    registerBuiltinTitleChecks();
    TitleCheckContext context(removed.doc, "test.titleart");
    std::vector<TitleCheckResult> results;
    const CheckDef* check = TitleArtChecks::find("fx_follow_missing");
    ASSERT_TRUE(check);
    check->run(context, results);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, TitleCheckStatus::Fail);
    ASSERT_TRUE(results[0].fix.is_object());
    // the fix op points it at the nearest layer and the check passes after it
    const TitleOpsResult fixed = TitleArtOps::apply(removed.doc, results[0].fix["ops"]);
    ASSERT_TRUE(fixed.ok) << fixed.error();
    EXPECT_TRUE(titleFollowProblems(fixed.doc).empty());
}
