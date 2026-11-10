// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S84-title-library.md AC1-AC5, AC24, AC26: the library formats, references,
// availability, apply_look's keep-and-replace contract, capture and search, on a small built-in
// set given to the store (setBuiltinsForTesting) and on the committed library.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/library/TitleLibrary.h"
#include "titleart/library/TitleLibraryApply.h"
#include "titleart/library/TitleLibraryCliTools.h"
#include "titleart/library/TitleLibraryFormats.h"
#include "titleart/library/TitleLibrarySearch.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using titleart::Json;
using titleart::TitleArtDoc;
namespace fs = std::filesystem;

namespace {

    // a look of two rims with a glow (a px param), a part effect on the second rim and a glints generator
    const char* GOLD_LOOK = R"({
  "format": "jasperengine.titlelook", "version": 1, "id": "metal/test-gold", "revision": 1,
  "name": {"en": "Test Gold", "ja": "テストゴールド"}, "description": {"en": "Two rims.", "ja": "二つの縁。"},
  "family": "metal", "tags": ["gold", "rims", "glow"], "synonyms": {"en": ["gilded"], "ja": ["きん", "kin"]},
  "dimension": "2d", "applies": "style", "ref_size": 200,
  "style": {"face": {"fill": {"kind": "linear", "gradient": {"stops": [{"pos": 0, "color": "#ffd820"}, {"pos": 1, "color": "#a05a00"}]}},
                     "bevel": {"enabled": true}},
            "rims": [{"width": 1, "material": {"fill": {"color": "#0a0604"}}}, {"width": 3, "material": {"fill": {"color": "#ffc040"}}}]},
  "effects": [{"type": "outer_glow", "params": {"size": 20, "color": "#ffb000"}}],
  "part_effects": {"rim#1": [{"type": "outer_glow", "params": {"size": 10}}]},
  "generators": [{"type": "glints", "place": "above", "scope": "layer", "blend": "normal", "opacity": 1, "params": {}, "effects": []}],
  "group_effects": [], "links": {}, "lint_allow": [], "check_allow": [], "provenance": {"source": "jasperengine"}
})";

    const char* ZEBRA_LOOK = R"({
  "format": "jasperengine.titlelook", "version": 1, "id": "casino/test-zebra", "revision": 1,
  "name": {"en": "Test Zebra Gold", "ja": "テストゼブラ"}, "family": "casino", "tags": ["zebra", "gold", "pattern"],
  "dimension": "2d", "style": {"face": {"fill": {"color": "#ffd820"}, "pattern": {"kind": "zebra", "color": "#000000"}}},
  "provenance": {"source": "jasperengine"}
})";

    const char* DRIPS_LOOK = R"({
  "format": "jasperengine.titlelook", "version": 1, "id": "horror/test-drip", "revision": 1,
  "name": {"en": "Test Drip", "ja": "テスト垂れ"}, "family": "horror", "tags": ["drips", "blood", "red"],
  "dimension": "2d", "style": {}, "effects": [{"type": "drips", "params": {}}], "provenance": {"source": "jasperengine"}
})";

    const char* MANIFEST = R"({"families": [{"id": "metal", "name": {"en": "Metal & Chrome", "ja": "メタル・クローム"}},
                                             {"id": "casino", "name": {"en": "Casino", "ja": "カジノ"}},
                                             {"id": "horror", "name": {"en": "Horror", "ja": "ホラー"}}],
                               "aliases": {"metal/old-gold": "metal/test-gold"}})";

    std::map<std::string, std::string> builtins() {
        return {{"manifest.json", MANIFEST},
                {"looks/metal/test-gold.titlelook", GOLD_LOOK},
                {"looks/casino/test-zebra.titlelook", ZEBRA_LOOK},
                {"looks/horror/test-drip.titlelook", DRIPS_LOOK}};
    }

    class TitleLibraryFixture : public ::testing::Test {
    protected:
        void SetUp() override {
            registerTitleLibraryOps();
            scratch = fs::temp_directory_path() / ("titlelibrary-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(scratch);
            fs::create_directories(scratch / "project" / "assets");
            TitleLibrary::get().setUserDirForTesting(scratch / "user");
            TitleLibrary::get().setProject(scratch / "project", scratch / "project" / "assets");
            TitleLibrary::get().setBuiltinsForTesting(builtins());
        }
        void TearDown() override {
            TitleLibrary::get().setBuiltinsForTesting({});
            TitleLibrary::get().setRequirementCheckForTesting(nullptr);
            TitleLibrary::get().setProject({});
            TitleLibrary::get().setUserDirForTesting({});
            setDropMasksForTesting(false);
            std::error_code ec;
            fs::remove_all(scratch, ec);
        }
        fs::path scratch;
    };

    // a text layer with every kept member of §6.4 at a non-default value
    TitleArtDoc keptDoc() {
        const std::string text = R"({"format": "jasperengine.titleart", "version": 1,
          "layers": [
            {"id": "00000001", "type": "text", "name": "Title", "text": "HERO", "size": 400, "tracking": 12, "line_height": 1.4,
             "align": "start", "h_scale": 1.2, "shear": 5, "visible": true, "locked": true, "blend": "screen", "opacity": 0.8,
             "transform": {"position": [120, -40], "rotation": 7, "scale": 1.5},
             "warp": {"style": "arc", "bend": 30},
             "masks": [{"id": "00000031", "type": "gradient", "angle": 45}],
             "style": {"rims": [{"id": "00000011", "width": 9}]},
             "effects": [{"id": "00000021", "type": "drop_shadow"}]}
          ]})";
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    std::string member(const Json& layer, const char* key) {
        return layer.contains(key) ? titleart::writeTitleArtJson(layer[key]) : std::string("<absent>");
    }

    TitleOpsResult applyOps(const TitleArtDoc& doc, const Json& ops, uint64_t seed = 7) {
        TitleOpsOptions o;
        o.idSeed = seed;
        return TitleArtOps::apply(doc, ops, o);
    }

}

// ---- AC1: formats ----

TEST_F(TitleLibraryFixture, LookRoundTripsCanonically) {
    LookParseResult parsed = parseLook(GOLD_LOOK);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    const std::string text = writeLook(parsed.look);
    LookParseResult again = parseLook(text);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(writeLook(again.look), text);
    // sparse: the bevel's defaults and the rims' ids are not written
    const Json root = Json::parse(text);
    EXPECT_FALSE(root["style"]["face"]["bevel"].contains("size"));
    EXPECT_FALSE(root["style"]["rims"][0].contains("id"));
    EXPECT_TRUE(root["style"]["face"]["bevel"]["enabled"].get<bool>());
}

TEST_F(TitleLibraryFixture, UnknownMembersAndTypesSurvive) {
    Json j = Json::parse(GOLD_LOOK);
    j["future_member"] = Json{{"x", 1}};
    j["effects"].push_back(Json{{"type", "effect_from_the_future"}, {"params", {{"strength", 3}}}});
    j["style"]["face"]["sparkle_v2"] = true;
    LookParseResult parsed = parseLookJson(j);
    ASSERT_TRUE(parsed.ok);
    const Json out = Json::parse(writeLook(parsed.look));
    EXPECT_EQ(out["future_member"]["x"], 1);
    EXPECT_EQ(out["effects"][1]["type"], "effect_from_the_future");
    EXPECT_EQ(out["effects"][1]["params"]["strength"], 3);
    EXPECT_TRUE(out["style"]["face"]["sparkle_v2"].get<bool>());
}

TEST_F(TitleLibraryFixture, NewerVersionIsReadOnly) {
    Json j = Json::parse(GOLD_LOOK);
    j["version"] = TITLE_LOOK_VERSION + 1;
    LookParseResult parsed = parseLookJson(j);
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.look.readOnly);
    ASSERT_FALSE(parsed.warnings.empty());
    EXPECT_EQ(parsed.warnings.front().code, "version_newer");
}

TEST_F(TitleLibraryFixture, TemplateKeepsItsMemberAfterGuid) {
    TitleTemplate t;
    t.doc = titleart::makeNewTitleArt();
    t.meta = Json{{"id", "minimal/test"}, {"name", {{"en", "Test"}}}, {"family", "minimal"},
                  {"placeholders", Json::array({Json{{"name", "text"}, {"role", "title"}, {"sample", {{"en", "HERO"}, {"ja", "英雄"}}}}})}};
    const std::string text = writeTemplate(t);
    const Json root = Json::parse(text);
    auto it = root.begin();
    std::vector<std::string> keys;
    for (; it != root.end(); ++it) keys.push_back(it.key());
    ASSERT_GE(keys.size(), 4u);
    EXPECT_EQ(keys[2], "guid");
    EXPECT_EQ(keys[3], "template");
    TemplateParseResult parsed = parseTemplate(text);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(writeTemplate(parsed.tmpl), text);
    EXPECT_EQ(parsed.tmpl.placeholders().size(), 1u);
}

// AC1 (R5): a default change bumps the version with a step that writes the old default; without
// the step a v1 look would change (the red control)
TEST_F(TitleLibraryFixture, TitleLookMigrationsTestDefaultChangeKeepsOldLooks) {
    // a v1 look that omits rims[0].join (S80's default "miter")
    const Json v1 = Json::parse(R"({"format": "jasperengine.titlelook", "version": 1, "id": "metal/x", "dimension": "2d",
                                    "style": {"rims": [{"width": 2}]}})");
    // the hypothetical v2 changes the default join to "round"; the step fills the v1 default
    titleart::MigrationStep step;
    step.from = 1;
    step.to = 2;
    step.name = "test_join_default";
    step.apply = [](Json& root, titleart::MigrationLog& log) {
        if (!root.contains("style") || !root["style"].contains("rims")) return;
        for (Json& rim : root["style"]["rims"]) {
            if (!rim.contains("join")) {
                rim["join"] = "miter";
                log.note("style.rims: join miter (the v1 default)");
            }
        }
    };
    auto runV2 = [&](bool withStep) {
        Json root = v1;
        titleart::TitleJsonMigrations table(TITLE_LOOK_FORMAT, 1);
        if (withStep) EXPECT_TRUE(table.add(step));
        titleart::MigrationLog log;
        if (withStep) EXPECT_TRUE(table.run(root, 1, 2, log, nullptr));
        // the v2 reader: a rim without `join` takes the new default
        const Json& rim = root["style"]["rims"][0];
        return rim.value("join", std::string("round"));
    };
    EXPECT_EQ(runV2(true), "miter");
    EXPECT_NE(runV2(false), "miter");      // red control: the old look would render round joins
    // the step is idempotent
    Json twice = v1;
    titleart::MigrationLog log;
    step.apply(twice, log);
    const Json once = twice;
    step.apply(twice, log);
    EXPECT_EQ(once, twice);
}

// ---- AC2: references and availability ----

TEST_F(TitleLibraryFixture, Refs) {
    LibraryError error;
    EXPECT_NE(TitleLibrary::get().info(LibraryKind::Look, "metal/test-gold", &error), nullptr);
    const LibraryItemInfo* missing = TitleLibrary::get().info(LibraryKind::Look, "metal/test-gld", &error);
    EXPECT_EQ(missing, nullptr);
    EXPECT_EQ(error.code, "library_item_not_found");
    ASSERT_FALSE(error.suggestions.empty());
    EXPECT_EQ(error.suggestions.front(), "metal/test-gold");
    EXPECT_NE(error.next.find("list_title_library"), std::string::npos);
    LibraryError alias;
    const LibraryItemInfo* old = TitleLibrary::get().info(LibraryKind::Look, "metal/old-gold", &alias);
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->ref, "metal/test-gold");
    EXPECT_TRUE(alias.deprecated);
    // project and user scopes
    fs::create_directories(TitleLibrary::get().scopeDir(LibraryScope::Project, LibraryKind::Look));
    fs::create_directories(TitleLibrary::get().scopeDir(LibraryScope::User, LibraryKind::Look));
    std::ofstream(TitleLibrary::get().scopeDir(LibraryScope::Project, LibraryKind::Look) / "mine.titlelook") << ZEBRA_LOOK;
    std::ofstream(TitleLibrary::get().scopeDir(LibraryScope::User, LibraryKind::Look) / "theirs.titlelook") << ZEBRA_LOOK;
    EXPECT_NE(TitleLibrary::get().info(LibraryKind::Look, "project/mine", nullptr), nullptr);
    EXPECT_NE(TitleLibrary::get().info(LibraryKind::Look, "user/theirs", nullptr), nullptr);
}

TEST_F(TitleLibraryFixture, Availability) {
    TitleLibrary::get().setRequirementCheckForTesting([](const std::string& r) { return r != "effect:drips"; });
    const LibraryItemInfo* drip = TitleLibrary::get().info(LibraryKind::Look, "horror/test-drip", nullptr);
    ASSERT_NE(drip, nullptr);
    EXPECT_FALSE(drip->available);
    EXPECT_EQ(drip->unavailableReason, "needs effect drips (S83)");
    TitleOpsResult r = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "horror/test-drip"}});
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.problems.empty());
    EXPECT_EQ(r.problems.back().code, "library_item_unavailable");
}

// ---- AC3, AC4: apply_look ----

TEST_F(TitleLibraryFixture, LookKeepsContract) {
    const TitleArtDoc before = keptDoc();
    TitleOpsResult r = applyOps(before, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json* b = titleart::findLayer(before.root, "00000001");
    const Json* a = titleart::findLayer(r.doc.root, "00000001");
    ASSERT_TRUE(a && b);
    for (const char* key : {"id", "type", "name", "visible", "locked", "guide", "blend", "opacity", "clip", "transform", "masks", "text", "size",
                            "tracking", "line_height", "align", "h_scale", "shear", "warp", "distort", "weight", "font", "fallback_fonts"}) {
        EXPECT_EQ(member(*a, key), member(*b, key)) << key;
    }
    EXPECT_EQ((*a)["style"]["rims"].size(), 2u);
    EXPECT_EQ((*a)["look"]["id"], "metal/test-gold");
    // red control: an apply that loses the masks is caught by the same comparison
    setDropMasksForTesting(true);
    TitleOpsResult broken = applyOps(before, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    setDropMasksForTesting(false);
    ASSERT_TRUE(broken.ok);
    const Json* c = titleart::findLayer(broken.doc.root, "00000001");
    EXPECT_NE(member(*c, "masks"), member(*b, "masks"));
}

TEST_F(TitleLibraryFixture, PxScaleByRefSize) {
    TitleOpsResult r = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json* a = titleart::findLayer(r.doc.root, "00000001");
    // size 400 on a look authored for 200: px params double
    EXPECT_DOUBLE_EQ((*a)["effects"][0]["params"]["size"].get<double>(), 40.0);
    // the part effect landed on the second rim's id
    const std::string rim = (*a)["style"]["rims"][1]["id"].get<std::string>();
    ASSERT_TRUE((*a)["part_effects"].contains("rim:" + rim));
    EXPECT_DOUBLE_EQ((*a)["part_effects"]["rim:" + rim][0]["params"]["size"].get<double>(), 20.0);
}

TEST_F(TitleLibraryFixture, OwnedGeneratorsReplaced) {
    TitleArtDoc doc = keptDoc();
    // a user's own generator following the layer
    doc.root["layers"].push_back(Json{{"id", "000000aa"}, {"type", "generator"}, {"name", "Mine"}, {"follow", "00000001"},
                                      {"generator", {{"type", "glints"}, {"params", Json::object()}}}});
    titleart::normalizeTitleArt(doc, nullptr);
    TitleOpsResult once = applyOps(doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    ASSERT_TRUE(once.ok) << once.error();
    TitleOpsResult twice = applyOps(once.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    ASSERT_TRUE(twice.ok);
    int owned = 0, mine = 0;
    titleart::forEachLayer(twice.doc.root, [&](const Json& l, const Json*, int) {
        if (l.value("type", std::string()) != "generator") return true;
        if (l["look"].value("owner", std::string()) == "00000001") owned++;
        if (l.value("id", std::string()) == "000000aa") mine++;
        return true;
    });
    EXPECT_EQ(owned, 1);    // the look's glints, once
    EXPECT_EQ(mine, 1);     // the user's kept
}

TEST_F(TitleLibraryFixture, AutoGroup) {
    // group effects need the auto group: created, then dissolved by a look without them
    Json j = Json::parse(GOLD_LOOK);
    j["id"] = "metal/test-grouped";
    j["group_effects"] = Json::array({Json{{"type", "outer_glow"}, {"params", Json::object()}}});
    auto files = builtins();
    files["looks/metal/test-grouped.titlelook"] = j.dump();
    TitleLibrary::get().setBuiltinsForTesting(files);
    TitleOpsResult grouped = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-grouped"}});
    ASSERT_TRUE(grouped.ok) << grouped.error();
    ASSERT_EQ(grouped.doc.root["layers"].size(), 1u);
    const Json& group = grouped.doc.root["layers"][0];
    EXPECT_EQ(group["type"], "group");
    EXPECT_EQ(group["name"], "Title FX");
    EXPECT_EQ(group["look"]["owner"], "00000001");
    ASSERT_EQ(group["effects"].size(), 1u);
    EXPECT_TRUE(group["effects"][0]["from_look"].get<bool>());
    // reused by the same look
    TitleOpsResult again = applyOps(grouped.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-grouped"}});
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(again.details["0"]["group"], "kept");
    // dissolved by a look without group effects
    TitleOpsResult plain = applyOps(again.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "casino/test-zebra"}});
    ASSERT_TRUE(plain.ok) << plain.error();
    EXPECT_EQ(plain.details["0"]["group"], "dissolved");
    EXPECT_EQ(plain.doc.root["layers"][0]["type"], "text");
}

TEST_F(TitleLibraryFixture, KeepColorScheme) {
    const char* scheme = R"({"format": "jasperengine.titlecolors", "version": 1, "id": "test-red", "name": {"en": "Test Red"},
                             "paints": {"face": {"fill": {"kind": "solid", "color": "#ff0000"}}, "accents": [{"fill": {"color": "#ffffff"}}]},
                             "light_ramp": ["#200000", "#800000", "#ff0000", "#ff8080"]})";
    auto files = builtins();
    files["color-schemes/test-red.titlecolors"] = scheme;
    TitleLibrary::get().setBuiltinsForTesting(files);
    TitleOpsResult schemed = applyOps(keptDoc(), Json::array({Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}},
                                                           Json{{"op", "apply_color_scheme"}, {"layer", "Title"}, {"color_scheme", "test-red"}}}));
    ASSERT_TRUE(schemed.ok) << schemed.error();
    EXPECT_EQ(titleart::findLayer(schemed.doc.root, "00000001")->at("look")["color_scheme"], "test-red");
    TitleOpsResult kept = applyOps(schemed.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "casino/test-zebra"}});
    ASSERT_TRUE(kept.ok);
    EXPECT_TRUE(kept.details["0"]["kept_color_scheme"].get<bool>());
    EXPECT_EQ(titleart::findLayer(kept.doc.root, "00000001")->at("look")["color_scheme"], "test-red");
    TitleOpsResult cleared = applyOps(schemed.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "casino/test-zebra"}, {"keep_color_scheme", false}});
    ASSERT_TRUE(cleared.ok);
    EXPECT_EQ(titleart::findLayer(cleared.doc.root, "00000001")->at("look")["color_scheme"], "");
}

TEST_F(TitleLibraryFixture, FreshLinks) {
    Json j = Json::parse(GOLD_LOOK);
    j["id"] = "metal/test-linked";
    j["style"]["rims"][1]["material"]["link"] = "gold";
    j["links"] = Json{{"gold", {{"name", "Gold"}}}};
    auto files = builtins();
    files["looks/metal/test-linked.titlelook"] = j.dump();
    TitleLibrary::get().setBuiltinsForTesting(files);
    TitleArtDoc doc = keptDoc();
    doc.root["layers"].push_back(doc.root["layers"][0]);
    doc.root["layers"][1]["id"] = "00000002";
    doc.root["layers"][1]["name"] = "Second";
    titleart::normalizeTitleArt(doc, nullptr);
    TitleOpsResult r = applyOps(doc, Json::array({Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-linked"}},
                                               Json{{"op", "apply_look"}, {"layer", "Second"}, {"look", "metal/test-linked"}}}));
    ASSERT_TRUE(r.ok) << r.error();
    const std::string a = titleart::findLayer(r.doc.root, "00000001")->at("style")["rims"][1]["material"]["link"].get<std::string>();
    const std::string b = titleart::findLayer(r.doc.root, "00000002")->at("style")["rims"][1]["material"]["link"].get<std::string>();
    EXPECT_FALSE(a.empty());
    EXPECT_NE(a, b);
    EXPECT_EQ(r.doc.root["material_links"][a]["name"], "Gold");
    EXPECT_EQ(r.undoName, "Apply Look Test Gold (2 layers)");
}

// ---- AC26: determinism and previews ----

TEST_F(TitleLibraryFixture, Deterministic) {
    // the same look on the same document with two id seeds: equal after id normalisation
    TitleOpsResult a = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}}, 11);
    TitleOpsResult b = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}}, 99);
    ASSERT_TRUE(a.ok && b.ok);
    auto strip = [](Json v) {
        std::function<void(Json&)> walk = [&](Json& x) {
            if (x.is_object()) {
                Json out = Json::object();
                for (auto it = x.begin(); it != x.end(); ++it) {
                    std::string k = it.key();
                    if (k.rfind("rim:", 0) == 0) k = "rim:*";
                    Json value = it.value();
                    if (k == "id" || k == "owner" || k == "follow" || k == "guid") value = "*";
                    walk(value);
                    out[k] = value;
                }
                x = out;
            } else if (x.is_array()) {
                for (Json& e : x) walk(e);
            }
        };
        walk(v);
        return titleart::writeTitleArtJson(v);
    };
    EXPECT_EQ(strip(a.doc.root), strip(b.doc.root));
}

TEST_F(TitleLibraryFixture, PreviewWritesNothing) {
    TitleArtDoc out;
    std::string error;
    ASSERT_TRUE(preview(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}, {"apply_font", true}}, out, nullptr, &error)) << error;
    std::error_code ec;
    EXPECT_FALSE(fs::exists(scratch / "project" / "assets" / "TitleArt", ec));
}

TEST_F(TitleLibraryFixture, TemplateBuildsAreReproducible) {
    const Json recipe = Json::parse(R"({"id": "minimal/test-built", "name": {"en": "Built", "ja": "作成"}, "family": "minimal",
        "tags": ["a", "b", "c"], "texts": {"text": {"en": "HERO", "ja": "英雄"}},
        "ops": [{"op": "add_layer", "type": "text", "name": "Subtitle", "text": "SUB"},
                {"op": "apply_look", "layer": "Title", "look": "metal/test-gold"}]})");
    TitleTemplate a, b;
    std::string error;
    ASSERT_TRUE(buildTemplateFromRecipe(recipe, a, error)) << error;
    ASSERT_TRUE(buildTemplateFromRecipe(recipe, b, error)) << error;
    EXPECT_EQ(writeTemplate(a), writeTemplate(b));
    // red control: the unseeded new document differs in the Title layer's id
    EXPECT_NE(titleart::makeNewTitleArt().root["layers"][0]["id"], titleart::makeNewTitleArt().root["layers"][0]["id"]);
}

// ---- AC5: capture ----

TEST_F(TitleLibraryFixture, CaptureRoundTripsALook) {
    TitleOpsResult applied = applyOps(keptDoc(), Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-gold"}});
    ASSERT_TRUE(applied.ok) << applied.error();
    // capture at the look's own size: ref_size 400 and every px value doubled, so re-applying to
    // a layer of size 400 gives the same document
    CaptureOptions options;
    options.name.en = "Captured";
    options.id = "project/captured";
    TitleLook captured;
    std::string error;
    ASSERT_TRUE(captureLook(applied.doc, "00000001", options, captured, nullptr, &error)) << error;
    EXPECT_DOUBLE_EQ(captured.refSize(), 400.0);
    EXPECT_EQ(captured.member("style")["rims"].size(), 2u);
    EXPECT_EQ(captured.member("generators").size(), 1u);
    auto files = builtins();
    Json cj = captured.root;
    cj["id"] = "metal/captured";
    files["looks/metal/captured.titlelook"] = writeLook(TitleLook{cj});
    TitleLibrary::get().setBuiltinsForTesting(files);
    TitleOpsResult again = applyOps(applied.doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/captured"}});
    ASSERT_TRUE(again.ok) << again.error();
    const Json* a = titleart::findLayer(applied.doc.root, "00000001");
    const Json* b = titleart::findLayer(again.doc.root, "00000001");
    EXPECT_EQ(titleart::writeTitleArtJson(sparseObject((*a)["style"], "style2d", true)), titleart::writeTitleArtJson(sparseObject((*b)["style"], "style2d", true)));
    EXPECT_EQ((*a)["effects"][0]["params"], (*b)["effects"][0]["params"]);
}

TEST_F(TitleLibraryFixture, SaveLookProjectAndUserScopes) {
    TitleOpsResult saved = applyOps(keptDoc(), Json{{"op", "save_look"}, {"layer", "Title"}, {"name", "My Look"}});
    ASSERT_TRUE(saved.ok) << saved.error();
    ASSERT_FALSE(saved.files.empty());
    EXPECT_EQ(saved.files.back().path, TitleLibrary::get().scopeDir(LibraryScope::Project, LibraryKind::Look) / "my-look.titlelook");
    EXPECT_TRUE(saved.files.back().undoable);
    TitleOpsResult user = applyOps(keptDoc(), Json{{"op", "save_look"}, {"layer", "Title"}, {"name", "My Look"}, {"scope", "user"}});
    ASSERT_TRUE(user.ok);
    EXPECT_FALSE(user.files.back().undoable);
    // an existing id needs overwrite
    fs::create_directories(TitleLibrary::get().scopeDir(LibraryScope::Project, LibraryKind::Look));
    std::ofstream(TitleLibrary::get().scopeDir(LibraryScope::Project, LibraryKind::Look) / "my-look.titlelook") << saved.files.back().content;
    TitleOpsResult clash = applyOps(keptDoc(), Json{{"op", "save_look"}, {"layer", "Title"}, {"name", "My Look"}});
    EXPECT_FALSE(clash.ok);
    EXPECT_EQ(clash.problems.back().code, "library_item_exists");
    TitleOpsResult builtin = applyOps(keptDoc(), Json{{"op", "delete_library_item"}, {"ref", "metal/test-gold"}});
    EXPECT_FALSE(builtin.ok);
    EXPECT_EQ(builtin.problems.back().code, "library_item_builtin");
}

TEST_F(TitleLibraryFixture, SaveTemplateNeedsAPlaceholder) {
    TitleArtDoc doc = keptDoc();
    TitleOpsResult r = applyOps(doc, Json{{"op", "save_template"}, {"name", "Mine"}});
    EXPECT_FALSE(r.ok);
    ASSERT_FALSE(r.problems.empty());
    EXPECT_EQ(r.problems.back().code, "template_without_placeholder");
    // the fix (expose the text, then save) passes
    TitleOpsResult fixed = applyOps(doc, r.problems.back().fix);
    EXPECT_TRUE(fixed.ok) << fixed.error();
}

// ---- AC24: search ----

TEST_F(TitleLibraryFixture, Search) {
    auto find = [](const std::string& text) {
        LibraryQuery q;
        q.text = text;
        std::vector<std::string> refs;
        for (const LibraryHit& h : searchLibrary(q)) refs.push_back(h.item->ref);
        return refs;
    };
    auto has = [](const std::vector<std::string>& refs, const std::string& ref) { return std::find(refs.begin(), refs.end(), ref) != refs.end(); };
    EXPECT_TRUE(has(find("gold"), "metal/test-gold"));
    EXPECT_TRUE(has(find("きん"), "metal/test-gold"));
    EXPECT_TRUE(has(find("kin"), "metal/test-gold"));
    EXPECT_EQ(find("ＧＯＬＤ"), find("gold"));
    EXPECT_EQ(find("ゴールド"), find("ごーるど"));
    // every word must match: zebra gold finds the zebra look, not the plain gold one
    const auto both = find("gold zebra");
    EXPECT_TRUE(has(both, "casino/test-zebra"));
    EXPECT_FALSE(has(both, "metal/test-gold"));
    // derived terms
    EXPECT_TRUE(has(find("outer glow"), "metal/test-gold"));
    EXPECT_TRUE(has(find("ゼブラ"), "casino/test-zebra"));
}

// ---- the committed library loads with no warning (AC1) ----

TEST(TitleLibraryCommitted, EveryBuiltInParses) {
    TitleLibrary::get().setBuiltinsForTesting({});
    size_t looks = 0, schemes = 0, templates = 0;
    for (const auto& [path, text] : TitleLibrary::get().builtinFiles()) {
        if (path.size() > 10 && path.rfind(".titlelook") == path.size() - 10) {
            LookParseResult parsed = parseLook(text);
            ASSERT_TRUE(parsed.ok) << path << ": " << parsed.error;
            for (const LibraryWarning& w : parsed.warnings) ADD_FAILURE() << path << ": " << w.toString();
            EXPECT_EQ(writeLook(parsed.look), text) << path << " is not canonical (title-library normalize)";
            looks++;
        } else if (path.rfind(".titletemplate") != std::string::npos) {
            TemplateParseResult parsed = parseTemplate(text);
            ASSERT_TRUE(parsed.ok) << path << ": " << parsed.error;
            EXPECT_EQ(writeTemplate(parsed.tmpl), text) << path << " is not canonical";
            templates++;
        } else if (path.rfind(".titlecolors") != std::string::npos) {
            titleart::ColorScheme s;
            std::vector<std::string> warnings;
            ASSERT_TRUE(titleart::parseColorScheme(Json::parse(text), s, &warnings)) << path;
            for (const std::string& w : warnings) ADD_FAILURE() << path << ": " << w;
            EXPECT_EQ(titleart::writeColorSchemeText(s), text) << path << " is not canonical";
            schemes++;
        }
    }
    std::cout << "[library] " << looks << " looks, " << schemes << " schemes, " << templates << " templates" << std::endl;
}

// the look's `cjk` member thins rims only on Japanese text; Latin keeps the look as designed
TEST_F(TitleLibraryFixture, CjkScaleAppliesOnlyToJapaneseText) {
    Json look = Json::parse(GOLD_LOOK);
    look["id"] = "metal/test-cjk";
    look["style"]["rims"] = Json::array({Json{{"width", 8.0}}});
    look["cjk"] = Json{{"rim_scale", 0.5}};
    auto files = builtins();
    files["looks/metal/test-cjk.titlelook"] = look.dump();
    TitleLibrary::get().setBuiltinsForTesting(files);
    const auto widthOn = [&](const std::string& text) {
        TitleArtDoc doc = keptDoc();
        doc.root["layers"][0]["text"] = text;
        const TitleOpsResult r = applyOps(doc, Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", "metal/test-cjk"}});
        EXPECT_TRUE(r.ok) << r.error();
        return r.doc.root["layers"][0]["style"]["rims"][0]["width"].get<double>();
    };
    EXPECT_DOUBLE_EQ(widthOn("HERO"), 8.0);
    EXPECT_DOUBLE_EQ(widthOn("\xE5\xA4\xA7\xE5\xBD\x93\xE3\x82\x8A"), 4.0);
}

// S84 Deviation 30: the GPU subcommands' editors keep llvmpipe's compiled shaders under HOME (Mesa
// writes them under the passwd entry's home, which a container's HOME override does not reach)
TEST(TitleLibraryCli, MesaShaderCacheFollowsHome) {
    EXPECT_EQ(fs::path(mesaShaderCacheDir(nullptr, nullptr, "/src/build/.cache/home")), fs::path("/src/build/.cache/home/.cache/mesa_shader_cache"));
    EXPECT_EQ(mesaShaderCacheDir("/shared/mesa", nullptr, "/home/u"), "") << "the caller's MESA_SHADER_CACHE_DIR wins";
    EXPECT_EQ(mesaShaderCacheDir(nullptr, "/home/u/.xdg-cache", "/home/u"), "") << "Mesa follows XDG_CACHE_HOME itself";
    EXPECT_EQ(fs::path(mesaShaderCacheDir("", "", "/home/u")), fs::path("/home/u/.cache/mesa_shader_cache")) << "empty values are unset";
    EXPECT_EQ(mesaShaderCacheDir(nullptr, nullptr, nullptr), "");
}
