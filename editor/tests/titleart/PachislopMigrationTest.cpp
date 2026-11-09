// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pachislop's project migration, ported (docs/specs/S87-title-localisation-interchange.md R28,
// D11, AC13): the cases of Pachislop's tests/unit/{project, effectDedupe, generatorParams,
// materialEffects, contentEffects}.test.ts (by SafEight, used with the author's permission) on
// constructed JSON, their names kept, and "migrating twice equals once" for each step.

#include <gtest/gtest.h>

#include "titleart/interchange/pachislop/PachislopMigrate.h"
#include "titleart/interchange/pachislop/PachislopProject.h"

#include <cmath>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

namespace {

    Json fx(const std::string& type, Json params = Json::object(), bool enabled = true) {
        return Json{{"id", type + "-id"}, {"type", type}, {"enabled", enabled}, {"params", std::move(params)}};
    }

    // nlohmann's comparison ignores member order: Pachislop's toEqual
    bool same(const Json& a, const Json& b) {
        return nlohmann::json(a) == nlohmann::json(b);
    }

    // a current Material3D: every field the migration fills already present
    Json mat3(const std::string& preset, const std::string& color) {
        return Json{{"preset", preset}, {"color", color}, {"metalness", 1}, {"roughness", 0.3}, {"gradientAxis", "xy"}, {"gradientBend", 0},
                    {"glassDepth", 0}, {"environment", Json::object()}, {"surface", Json{{"kind", "none"}, {"amount", 0.5}, {"scale", 20}, {"seed", 1}}}};
    }

    // ---- a v4-shaped project (the fields v4 lacked left out) ----

    Json mat3V4(const std::string& preset, const std::string& color) {
        return Json{{"preset", preset}, {"color", color}, {"metalness", 1}, {"roughness", 0.3}};
    }

    Json material2DV4(const std::string& color) {
        return Json{{"fill", Json{{"kind", "solid"}, {"color", color}}},
                    {"bevel", Json{{"enabled", true}, {"profile", "smooth"}, {"size", 3}}},
                    {"satin", Json{{"enabled", false}}},
                    {"texture", Json{{"kind", "none"}}},
                    {"gloss", Json{{"enabled", false}}}};
    }

    Json legacyStyle3D() {
        return Json{{"depth", 14}, {"face", mat3V4("gold", "#ffcc33")}, {"side", mat3V4("gold", "#ffcc33")},
                    {"bands", Json::array({Json{{"id", "ring"}, {"width", 2.5}, {"depth", 2.5}, {"profile", "round"}, {"segments", 6}, {"curve", 0},
                                                {"material", mat3V4("gold", "#ffcc33")}}})},
                    {"backplates", Json::array({Json{{"id", "b1"}, {"enabled", true}, {"grow", 4}, {"join", "miter"}, {"fillCounters", true},
                                                     {"depth", 10}, {"zGap", 0}, {"bevel", Json{{"width", 1.5}, {"depth", 1.5}, {"profile", "round"},
                                                                                                {"segments", 6}, {"curve", 0}}},
                                                     {"face", mat3V4("chrome", "#dddddd")}, {"side", mat3V4("chrome", "#dddddd")}}})}};
    }

    Json v4Project(int version) {
        Json t2 = Json{{"id", "t2"}, {"name", "BIG BONUS"}, {"kind", "title2d"}, {"visible", true}, {"blend", "normal"},
                       {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}},
                       {"effects", Json::array({fx("dropShadow", Json{{"angle", 60}, {"distance", 10}, {"size", 14}})})},
                       {"source", Json{{"kind", "text"}, {"text", "BIG BONUS"}, {"fontId", "dela-gothic-one"}, {"size", 200},
                                       {"chars", Json{{"2", Json{{"dx", 5}, {"dy", -3}, {"scale", 0.8}, {"rotate", 10},
                                                                 {"face", material2DV4("#ff0000")}}}}}}},
                       {"style", Json{{"face", material2DV4("#ffd700")},
                                      {"rims", Json::array({Json{{"id", "r1"}, {"enabled", true}, {"width", 4}, {"join", "round"},
                                                                 {"material", material2DV4("#000000")}}})}}}};
        Json t3 = Json{{"id", "t3"}, {"name", "確定"}, {"kind", "title3d"}, {"visible", true},
                       {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", Json::array()},
                       {"source", Json{{"kind", "text"}, {"text", "確定"}, {"fontId", "dela-gothic-one"}, {"size", 200},
                                       {"chars", Json{{"0", Json{{"dx", 1}, {"dy", 0}, {"scale", 1.2}, {"rotate", 0}, {"face3d", mat3V4("gold", "#ffcc33")}}}}}}},
                       {"style3d", legacyStyle3D()}};
        Json img = Json{{"id", "img"}, {"name", "img"}, {"kind", "title2d"}, {"visible", true},
                        {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", Json::array()},
                        {"source", Json{{"kind", "image"}, {"assetId", "asset-1"}, {"size", 300}}}, {"style", Json{{"face", material2DV4("#ffffff")}, {"rims", Json::array()}}}};
        Json bg = Json{{"id", "bg"}, {"name", "Background"}, {"kind", "background"}, {"visible", true},
                       {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", Json::array()},
                       {"fill", Json{{"kind", "solid"}, {"color", "#000000"}}}};
        return Json{{"version", version}, {"name", "T"},
                    {"comp", Json{{"width", 1920}, {"height", 1080}, {"environment", Json{{"preset", "studio"}, {"intensity", 1}}}}},
                    {"layers", Json::array({bg, t3, t2, img})}};
    }

    Json migrated(const Json& project) {
        Json out;
        std::string error;
        EXPECT_TRUE(migrateProject(project, out, error)) << error;
        return out;
    }

    const Json& layerOf(const Json& p, size_t i) { return p["layers"][i]; }

    // a v10 project with one 3D text title in the v4-v10 shape
    Json stored(Json style, Json chars = Json::object(), Json partEffects = Json::object()) {
        Json s = Json{{"face", mat3("gold", "#ffcc33")}, {"depth", 10}, {"bevelPosition", "outside"}, {"bands", Json::array()}, {"backplates", Json::array()},
                      {"mode", "extrude"}};
        for (auto it = style.begin(); it != style.end(); ++it) s[it.key()] = it.value();
        Json t3 = Json{{"id", "t3"}, {"name", "確定"}, {"kind", "title3d"}, {"transform", Json{{"x", 0}, {"y", 0}, {"scale", 1}, {"opacity", 1}}},
                       {"effects", Json::array()}, {"source", Json{{"kind", "text"}, {"text", "確定"}, {"chars", chars}}}, {"style3d", s},
                       {"partEffects", partEffects}};
        return Json{{"version", 10}, {"name", "T"}, {"comp", Json{{"width", 1920}, {"height", 1080}, {"environment", Json::object()}}},
                    {"layers", Json::array({t3})}};
    }

    Json ring(const std::string& id, double width, double depth, const Json& material, const std::string& profile = "round") {
        return Json{{"id", id}, {"width", width}, {"depth", depth}, {"profile", profile}, {"segments", 6}, {"curve", 0}, {"material", material}};
    }

    Json plate(const std::string& id, Json o = Json::object()) {
        Json p = Json{{"id", id}, {"enabled", true}, {"grow", 4}, {"join", "round"}, {"fillCounters", true},
                      {"plate", Json{{"kind", "box"}, {"close", 0}, {"corner", 2}, {"slant", 8}}}, {"depth", 7}, {"zGap", 1.5},
                      {"bevel", Json{{"width", 0.8}, {"depth", 1.2}, {"profile", "chamfer"}, {"segments", 3}, {"curve", 0.2}}},
                      {"face", mat3("blackGloss", "#101010")}};
        for (auto it = o.begin(); it != o.end(); ++it) p[it.key()] = it.value();
        return p;
    }

    const Json DARK = mat3("custom", "#3a1000");

}

// ==================================================================== project.test.ts: migrateProject

TEST(PachislopMigrationTest, AV4OrV5ProjectGainsEveryCurrentFieldWithTheOldBehaviour) {
    for (int version : {4, 5}) {
        SCOPED_TRACE(version);
        const Json old = v4Project(version);
        const std::string text = old.dump();
        for (const char* newer : {"useGlobalLight", "tateChuYoko", "bevelPosition", "gradientAxis", "\"surface\"", "\"clip\"", "partEffects", "\"trace\"",
                                  "\"dpi\"", "\"erode\"", "\"plate\"", "\"weight\""}) {
            EXPECT_EQ(text.find(newer), std::string::npos) << "the sample has none of the newer fields: " << newer;
        }
        const Json got = migrated(old);
        EXPECT_EQ(got["version"], 11);
        EXPECT_EQ(old.dump(), text) << "the input is not modified";
        const Json& t2 = layerOf(got, 2);
        EXPECT_EQ(t2["style"]["face"]["bevel"]["useGlobalLight"], false) << "a v4 bevel keeps its own light";
        EXPECT_EQ(t2["style"]["face"]["satin"]["useGlobalLight"], false);
        EXPECT_EQ(t2["effects"][0]["params"]["useGlobalLight"], false) << "a v4 shadow keeps its angle";
        EXPECT_EQ(t2["source"]["tateChuYoko"], false);
        EXPECT_EQ(t2["clip"], false);
        EXPECT_TRUE(t2["partEffects"].is_object());
        EXPECT_EQ(t2["style"]["rims"][0]["plate"]["kind"], "contour");
        EXPECT_EQ(t2["style"]["erode"]["enabled"], false);
        EXPECT_EQ(t2["weight"], 0);
        EXPECT_EQ(got["comp"]["dpi"], 72);
        EXPECT_EQ(got["comp"]["globalLight"]["angle"], 120);
        EXPECT_EQ(layerOf(got, 3)["source"]["trace"]["threshold"], 128);
        // the 3D title's v4 style converted to the letter and its bands
        const Json& s3 = layerOf(got, 1)["style3d"];
        EXPECT_FALSE(s3.contains("backplates"));
        EXPECT_EQ(s3["bevelPosition"], "inside") << "files from before bevelPosition cut their bands inward";
        EXPECT_EQ(s3["face"]["gradientAxis"], "xy");
        EXPECT_EQ(s3["face"]["surface"]["kind"], "none");
        // migrating twice equals once
        EXPECT_TRUE(same(migrated(got), got));
    }
}

TEST(PachislopMigrationTest, KeepsPerCharacterOffsetsAndScalesAsStored) {
    const Json got = migrated(v4Project(4));
    const Json& c = layerOf(got, 2)["source"]["chars"]["2"];
    EXPECT_EQ(c["dx"], 5);
    EXPECT_EQ(c["dy"], -3);
    EXPECT_EQ(c["scale"], 0.8);
    EXPECT_EQ(c["rotate"], 10);
}

TEST(PachislopMigrationTest, AV6FileFromBeforeImageTracingAndFaceImagesGetsThem) {
    Json old = migrated(v4Project(4));
    old["version"] = 6;
    old["layers"][3]["source"].erase("trace");
    old["layers"][1]["style3d"].erase("faceImage");
    const Json got = migrated(old);
    EXPECT_TRUE(same(got["layers"][3]["source"]["trace"], Json{{"threshold", 128}, {"smooth", 0.5}, {"despeckle", 0.05}}));
    EXPECT_EQ(got["layers"][1]["style3d"]["faceImage"], 1);
}

TEST(PachislopMigrationTest, KeepsValuesThatArePresentInsteadOfOverwritingThem) {
    Json old = v4Project(4);
    old["comp"]["globalLight"] = Json{{"angle", 45}};
    old["comp"]["dpi"] = 300;
    old["layers"][2]["style"]["face"]["bevel"]["useGlobalLight"] = true;
    old["layers"][2]["source"]["tateChuYoko"] = true;
    old["layers"][2]["effects"][0]["params"]["useGlobalLight"] = true;
    old["layers"][1]["style3d"]["bevelPosition"] = "outside";
    old["layers"][1]["style3d"]["face"]["surface"] = Json{{"kind", "brushed"}};
    old["layers"][2]["clip"] = true;
    const Json got = migrated(old);
    EXPECT_TRUE(same(got["comp"]["globalLight"], Json{{"angle", 45}, {"altitude", 30}}));
    EXPECT_EQ(got["comp"]["dpi"], 300);
    EXPECT_EQ(layerOf(got, 2)["style"]["face"]["bevel"]["useGlobalLight"], true);
    EXPECT_EQ(layerOf(got, 2)["source"]["tateChuYoko"], true);
    EXPECT_EQ(layerOf(got, 2)["effects"][0]["params"]["useGlobalLight"], true);
    EXPECT_EQ(layerOf(got, 2)["clip"], true);
    EXPECT_EQ(layerOf(got, 1)["style3d"]["bevelPosition"], "outside");
    EXPECT_TRUE(same(layerOf(got, 1)["style3d"]["face"]["surface"], Json{{"kind", "brushed"}, {"amount", 0.5}, {"scale", 20}, {"seed", 1}}))
        << "a partial nested object keeps its fields and gets the rest";
}

TEST(PachislopMigrationTest, IsIdempotentCurrentDataPassesThroughUnchanged) {
    const Json once = migrated(v4Project(4));
    EXPECT_TRUE(same(migrated(once), once));
    const Json current = Json{{"version", 11}, {"name", "Untitled"}, {"comp", once["comp"]}, {"layers", Json::array()}};
    EXPECT_TRUE(same(migrated(current), current));
}

TEST(PachislopMigrationTest, MigratesPartEffectsOfV6Titles) {
    Json p = migrated(v4Project(4));
    p["layers"][2]["partEffects"] = Json{{"face", Json::array({fx("dropShadow", Json{{"angle", 30}})})}};
    p["version"] = 6;
    EXPECT_EQ(migrated(p)["layers"][2]["partEffects"]["face"][0]["params"]["useGlobalLight"], false);
}

TEST(PachislopMigrationTest, RefusesVersion1To3NamingTheVersionAndTheSupportedOnes) {
    for (int version : {1, 2, 3}) {
        Json old = v4Project(4);
        old["version"] = version;
        Json out;
        std::string error;
        EXPECT_FALSE(migrateProject(old, out, error));
        EXPECT_TRUE(std::regex_search(error, std::regex("version " + std::to_string(version) + " project.*version 4 to 11"))) << error;
    }
}

TEST(PachislopMigrationTest, RefusesOtherVersionsAsUnsupported) {
    for (const Json& version : {Json(12), Json(0), Json("7"), Json()}) {
        Json old = v4Project(4);
        old["version"] = version;
        Json out;
        std::string error;
        EXPECT_FALSE(migrateProject(old, out, error));
        EXPECT_TRUE(std::regex_search(error, std::regex(R"(Unsupported project version .* \(expected 4 to 11\))"))) << error;
    }
}

TEST(PachislopMigrationTest, RefusesNonProjects) {
    for (const Json& bad : {Json(), Json("x"), Json::array(), Json{{"version", 7}, {"comp", Json::object()}}, Json{{"version", 7}, {"layers", Json::array()}},
                            Json{{"version", 7}, {"layers", Json::object()}, {"comp", Json::object()}}}) {
        Json out;
        std::string error;
        EXPECT_FALSE(migrateProject(bad, out, error));
        EXPECT_EQ(error, "Not a Pachislop project");
    }
}

// ==================================================================== project.test.ts: v8 -> v9 tiltY

namespace {

    Json withChars(int version, Json chars) {
        Json p = stored(Json::object(), std::move(chars));
        p["version"] = version;
        p["layers"][0]["style3d"]["bands"] = Json::array();
        p["layers"][0]["style3d"].erase("backplates");
        p["layers"][0]["style3d"]["bevel"] = Json{{"width", 0}, {"depth", 0}, {"profile", "round"}, {"segments", 6}, {"curve", 0}};
        return p;
    }

}

TEST(PachislopMigrationTest, NegatesAV8GlyphsTiltYAndKeepsItsOther3DAdjustments) {
    const Json got = migrated(withChars(8, Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltX", 12}, {"tiltY", 20}, {"dz", 5}}}}));
    EXPECT_EQ(got["version"], 11);
    const Json& c = got["layers"][0]["source"]["chars"]["0"];
    EXPECT_EQ(c["tiltX"], 12);
    EXPECT_EQ(c["tiltY"], -20);
    EXPECT_EQ(c["dz"], 5);
}

TEST(PachislopMigrationTest, LeavesAGlyphWithoutTiltYWithoutIt) {
    const Json got = migrated(withChars(8, Json{{"1", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltX", 12}}}}));
    EXPECT_FALSE(got["layers"][0]["source"]["chars"]["1"].contains("tiltY"));
    EXPECT_EQ(got["layers"][0]["source"]["chars"]["1"]["tiltX"], 12);
}

TEST(PachislopMigrationTest, PassesV9DataUnchangedMigratingTwiceEqualsOnce) {
    const Json v9 = migrated(withChars(9, Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltY", 20}}}}));
    EXPECT_EQ(v9["layers"][0]["source"]["chars"]["0"]["tiltY"], 20);
    const Json once = migrated(withChars(8, Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltY", 20}}}}));
    EXPECT_TRUE(same(migrated(once), once));
}

// ==================================================================== project.test.ts: v10 -> v11 bands

TEST(PachislopMigrationTest, BackplatesBecomeBandsExactly) {
    const Json p1 = plate("p1");
    const Json p2 = plate("p2", Json{{"enabled", false}, {"grow", 2}, {"join", "miter"}, {"fillCounters", false}, {"zGap", -3},
                                     {"face", mat3("chrome", "#dddddd")}, {"side", mat3("redMetal", "#aa0000")}});
    const Json s = migrated(stored(Json{{"backplates", Json::array({p1, p2})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevel"]["width"], 0);
    EXPECT_EQ(s["depth"], 10);
    const auto asBand = [](const Json& p) {
        Json b = Json{{"id", p["id"]}, {"enabled", p["enabled"]}, {"grow", p["grow"]}, {"join", p["join"]}, {"fillCounters", p["fillCounters"]},
                      {"shape", p["plate"]}, {"depth", p["depth"]}, {"offset", p["zGap"]}, {"bevel", p["bevel"]}, {"material", p["face"]}};
        if (p.contains("side")) b["side"] = p["side"];
        return b;
    };
    ASSERT_EQ(s["bands"].size(), 2u);
    EXPECT_TRUE(same(s["bands"][0], asBand(p1))) << s["bands"][0].dump();
    EXPECT_TRUE(same(s["bands"][1], asBand(p2))) << s["bands"][1].dump();
}

TEST(PachislopMigrationTest, TheFirstRingInTheFacesMaterialFoldsIntoTheLetter) {
    Json goldLinked = mat3("gold", "#ffcc33");
    goldLinked["link"] = "g";
    const Json old = stored(Json{{"bands", Json::array({ring("r1", 1.6, 1.6, goldLinked, "chamfer"), ring("r2", 0.3, 0.6, DARK, "step"),
                                                        ring("r3", 1, 1.2, mat3("redMetal", "#aa0000"))})},
                                 {"backplates", Json::array({plate("p1")})}});
    const Json s = migrated(old)["layers"][0]["style3d"];
    EXPECT_TRUE(same(s["bevel"], Json{{"width", 1.6}, {"depth", 1.6}, {"profile", "chamfer"}, {"segments", 6}, {"curve", 0}}));
    EXPECT_EQ(s["bevelPosition"], "outside");
    EXPECT_NEAR(s["depth"].get<double>(), 10 + 0.6 + 1.2, 1e-9) << "the old back: 10 behind the rings' end";
    ASSERT_EQ(s["bands"].size(), 3u);
    EXPECT_EQ(s["bands"][0]["id"], "r2");
    EXPECT_EQ(s["bands"][1]["id"], "r3");
    EXPECT_EQ(s["bands"][2]["id"], "p1");
    const Json &r2 = s["bands"][0], &r3 = s["bands"][1], &p1 = s["bands"][2];
    EXPECT_EQ(r2["grow"], 0.3);
    EXPECT_EQ(r2["join"], "miter");
    EXPECT_EQ(r2["shape"]["kind"], "contour");
    EXPECT_EQ(r2["bevel"]["profile"], "step");
    EXPECT_TRUE(same(r2["material"], DARK));
    EXPECT_NEAR(r2["offset"].get<double>(), -s["depth"].get<double>(), 1e-9);
    EXPECT_NEAR(r2["depth"].get<double>(), 11.2, 1e-9);
    EXPECT_NEAR(r3["offset"].get<double>(), -r2["depth"].get<double>(), 1e-9);
    EXPECT_EQ(r3["depth"], 10);
    EXPECT_FALSE(r3.contains("side")) << "the old side wall showed the outermost ring, now that band's own material";
    EXPECT_FALSE(s.contains("side"));
    EXPECT_EQ(p1["grow"], 4) << "'outside' rings reach exactly as far as before";
    EXPECT_EQ(p1["offset"], 1.5);
}

TEST(PachislopMigrationTest, TheOldSideWallGoesOntoTheOutermostConvertedBandOrTheLetter) {
    const Json red = mat3("redMetal", "#aa0000");
    const Json banded = migrated(stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                             {"side", red}}))["layers"][0]["style3d"];
    EXPECT_TRUE(same(banded["bands"][0]["side"], red));
    EXPECT_FALSE(banded.contains("side"));
    const Json bare = migrated(stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}, {"side", red}}))["layers"][0]["style3d"];
    EXPECT_TRUE(same(bare["side"], red));
    EXPECT_TRUE(bare["bands"].empty());
}

TEST(PachislopMigrationTest, AFirstRingUnlikeTheFaceBecomesABandToo) {
    const Json s = migrated(stored(Json{{"bands", Json::array({ring("r1", 0.3, 0.4, mat3("whiteGloss", "#ffffff")),
                                                               ring("r2", 1.6, 1.6, mat3("gold", "#ffcc33"))})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevel"]["width"], 0);
    ASSERT_EQ(s["bands"].size(), 2u);
    EXPECT_EQ(s["bands"][0]["id"], "r1");
    EXPECT_EQ(s["bands"][0]["grow"], 0.3);
    EXPECT_EQ(s["bands"][1]["grow"], 1.6);
    EXPECT_NEAR(s["bands"][0]["offset"].get<double>(), -s["depth"].get<double>(), 1e-9);
}

TEST(PachislopMigrationTest, InsideRingsTakeTheClosestPositionAndPlatesKeepTheirOuterEdge) {
    const Json s = migrated(stored(Json{{"bevelPosition", "inside"},
                                        {"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                        {"backplates", Json::array({plate("p1", Json{{"grow", 2}})})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevelPosition"], "inside");
    EXPECT_NEAR(s["bands"][1]["grow"].get<double>(), 1.5, 1e-9);
}

TEST(PachislopMigrationTest, InInflateModeTheRingsPastTheBevelBecomeBandsSwitchedOff) {
    const Json s = migrated(stored(Json{{"mode", "inflate"}, {"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                        {"backplates", Json::array({plate("p1")})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevel"]["width"], 1);
    EXPECT_EQ(s["depth"], 10);
    ASSERT_EQ(s["bands"].size(), 2u);
    EXPECT_EQ(s["bands"][0]["id"], "r2");
    EXPECT_EQ(s["bands"][0]["enabled"], false);
    EXPECT_EQ(s["bands"][1]["enabled"], true);
    EXPECT_TRUE(same(s["side"], DARK));
    EXPECT_EQ(s["bands"][1]["grow"], 4);
}

TEST(PachislopMigrationTest, PartEffectsFollowTheirParts) {
    const auto glows = [](int n) { return Json::array({fx("outerGlow", Json{{"size", n}})}); };
    const Json old = stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                 {"backplates", Json::array({plate("p1")})}},
                            Json::object(), Json{{"face", glows(1)}, {"band:r1", glows(2)}, {"band:r2", glows(3)}, {"side", glows(4)}, {"plate:p1", glows(5)}});
    const Json got = migrated(old)["layers"][0]["partEffects"];
    std::vector<std::string> keys;
    for (auto it = got.begin(); it != got.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, std::vector<std::string>({"band:p1", "band:r2", "face"}));
    const auto sizes = [&](const char* k) {
        std::vector<int> out;
        for (const Json& e : got[k]) out.push_back(e["params"]["size"].get<int>());
        return out;
    };
    EXPECT_EQ(sizes("face"), std::vector<int>({1, 2}));
    EXPECT_EQ(sizes("band:r2"), std::vector<int>({3, 4}));
    EXPECT_EQ(sizes("band:p1"), std::vector<int>({5}));
}

TEST(PachislopMigrationTest, LettersKeepTheirRingOverridesOnTheConvertedBands) {
    const Json white = mat3("whiteGloss", "#ffffff"), red = mat3("redMetal", "#aa0000");
    const Json chars = Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"face3d", white}, {"bands3d", Json::array({white, white})}}},
                            {"1", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"side3d", red}}}};
    const Json rings = Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)});
    const Json got = migrated(stored(Json{{"bands", rings}, {"backplates", Json::array({plate("p1")})}}, chars));
    const Json& s = got["layers"][0]["style3d"];
    const Json& c = got["layers"][0]["source"]["chars"];
    EXPECT_EQ(s["bevel"]["width"], 1) << "0's first ring is its face: the ring folds";
    EXPECT_TRUE(same(c["0"]["bands3d"], Json::array({white})));
    EXPECT_FALSE(c["0"].contains("side3d"));
    EXPECT_FALSE(c["1"].contains("side3d")) << "1's own wall is inside band 0 now: it has nowhere to show";
    const Json bare = migrated(stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}}, chars));
    EXPECT_TRUE(same(bare["layers"][0]["source"]["chars"]["1"]["side3d"], red)) << "without converted rings a letter keeps its own wall";
    EXPECT_FALSE(bare["layers"][0]["style3d"].contains("side")) << "the side resolves to the gold face";
}

TEST(PachislopMigrationTest, ARingOverriddenUnlikeTheFaceOnSomeLetterDoesNotFold) {
    const Json chars = Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"bands3d", Json::array({mat3("redMetal", "#aa0000")})}}}};
    const Json got = migrated(stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}}, chars));
    EXPECT_EQ(got["layers"][0]["style3d"]["bevel"]["width"], 0);
    EXPECT_TRUE(same(got["layers"][0]["source"]["chars"]["0"]["bands3d"], Json::array({mat3("redMetal", "#aa0000")})));
}

TEST(PachislopMigrationTest, BandsMigratingTwiceEqualsOnce) {
    const Json once = migrated(stored(Json{{"bands", Json::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                           {"backplates", Json::array({plate("p1")})}, {"side", mat3("redMetal", "#aa0000")}},
                                      Json{{"0", Json{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"bands3d", Json::array({Json(), mat3("chrome", "#dddddd")})}}}}));
    EXPECT_TRUE(same(migrated(once), once));
}

// ==================================================================== project.test.ts: migrateEffects

TEST(PachislopMigrationTest, FillsAMissingUseGlobalLightOnlyOnEffectsThatHaveTheParam) {
    const Json shadow = fx("dropShadow", Json{{"angle", 60}});
    const Json glow = fx("outerGlow", Json{{"size", 10}});
    const Json out = migrateEffects(Json::array({shadow, glow}));
    EXPECT_TRUE(same(out[0]["params"], Json{{"angle", 60}, {"useGlobalLight", false}}));
    EXPECT_TRUE(same(out[1], glow));
    EXPECT_TRUE(same(shadow["params"], Json{{"angle", 60}}));
}

TEST(PachislopMigrationTest, TurnsAnOldTwoBlurDeepGlowIntoTheLightGlow) {
    const Json out = migrateEffects(Json::array({fx("deepGlow", Json{{"innerRadius", 10}, {"outerRadius", 120}, {"innerIntensity", 1}, {"outerIntensity", 0.5},
                                                                     {"tint", "#ff0000"}, {"tintAmount", 0.8}, {"threshold", 0.5}, {"exposure", 1},
                                                                     {"onFace", 0.3}})}))[0]["params"];
    for (const char* k : {"innerRadius", "outerRadius", "innerIntensity", "outerIntensity", "tint"}) EXPECT_FALSE(out.contains(k)) << k;
    EXPECT_EQ(out["radius"], 120);
    EXPECT_EQ(out["exposure"], 0);
    EXPECT_EQ(out["knee"], 0.1);
    EXPECT_EQ(out["colorMode"], "custom");
    EXPECT_EQ(out["color"], "#ff0000");
    EXPECT_EQ(out["tintAmount"], 0);
    EXPECT_EQ(out["onFace"], 0.3);
    EXPECT_NEAR(out["intensity"].get<double>(), 5.4, 1e-9) << "the same scattered energy: (1.2 + 3 x 0.5) x 2^1";
    EXPECT_NEAR(out["threshold"].get<double>(), 0.214, 1e-3) << "sRGB 0.5 is linear 0.214";
    EXPECT_GE(out["falloff"].get<double>(), 0);
    EXPECT_LE(out["falloff"].get<double>(), 100);
}

TEST(PachislopMigrationTest, AWeakTintTintsTheLayerColoursInsteadOfReplacingThem) {
    const Json out = migrateEffects(Json::array({fx("deepGlow", Json{{"tint", "#00ff00"}, {"tintAmount", 0.3}})}))[0]["params"];
    EXPECT_EQ(out["colorMode"], "layer");
    EXPECT_EQ(out["tintAmount"], 0.3);
    EXPECT_EQ(out["tintInner"], "#00ff00");
    EXPECT_EQ(out["tintOuter"], "#00ff00");
    EXPECT_EQ(out["radius"], 240);
    EXPECT_FALSE(out.contains("color"));
}

TEST(PachislopMigrationTest, MoreOuterLightMeansALongerHazeIntensityIsCappedAt8) {
    const auto falloff = [](double outer) {
        return migrateEffects(Json::array({fx("deepGlow", Json{{"outerIntensity", outer}})}))[0]["params"]["falloff"].get<double>();
    };
    EXPECT_GT(falloff(0.2), falloff(1.5));
    EXPECT_EQ(falloff(0), 100);
    EXPECT_EQ(migrateEffects(Json::array({fx("deepGlow", Json{{"innerIntensity", 5}, {"outerIntensity", 5}, {"exposure", 3}})}))[0]["params"]["intensity"], 8);
}

TEST(PachislopMigrationTest, LeavesACurrentDeepGlowAlone) {
    const Json current = fx("deepGlow", Json{{"exposure", 1}, {"radius", 200}, {"tintAmount", 0.4}, {"tintInner", "#ffffff"}});
    EXPECT_TRUE(same(migrateEffects(Json::array({current}))[0], current));
}

TEST(PachislopMigrationTest, MigrateEffectsIsIdempotent) {
    const Json effects = Json::array({fx("deepGlow", Json{{"innerRadius", 20}, {"outerRadius", 300}, {"tint", "#ffb43c"}, {"tintAmount", 0.6}, {"threshold", 0.02}}),
                                      fx("deepGlow", Json{{"outerIntensity", 0.1}, {"tintAmount", 0.2}}), fx("dropShadow", Json{{"angle", 60}}),
                                      fx("innerShadow", Json{{"angle", 60}, {"useGlobalLight", true}}), fx("bevel"), fx("outerGlow"),
                                      fx("noSuchEffect", Json{{"innerRadius", 3}})});
    const Json once = migrateEffects(effects);
    EXPECT_TRUE(same(migrateEffects(once), once));
}

// ==================================================================== project.test.ts: parseProjectFile

namespace {

    std::string fileOf(int version, int projectVersion) {
        Json p = v4Project(projectVersion);
        // [1, 2, 3] and [250, 0] in base64
        return Json{{"format", "pslot"}, {"version", version}, {"project", p},
                    {"fonts", Json::array({Json{{"id", "f1"}, {"name", "Font"}, {"data", "AQID"}}})},
                    {"assets", Json::array({Json{{"id", "a1"}, {"name", "A"}, {"data", "+gA="}}})}}
            .dump();
    }

}

TEST(PachislopMigrationTest, UpgradesAnOlderFileAndNotesItDecodesEmbeddedBlobs) {
    Bundle b;
    std::string code, error;
    ASSERT_TRUE(parseProjectFile(fileOf(4, 4), b, code, error)) << error;
    EXPECT_EQ(b.version, 4);
    EXPECT_EQ(b.project["version"], 11);
    EXPECT_EQ(b.notes.size(), 1u);
    EXPECT_EQ(b.fonts[0].data, std::string("\x01\x02\x03", 3));
    EXPECT_EQ(b.assets[0].id, "a1");
    EXPECT_EQ(b.assets[0].mime, "application/octet-stream");
    EXPECT_EQ(b.assets[0].data, std::string("\xfa\x00", 2));
}

TEST(PachislopMigrationTest, ACurrentFileHasNoNotes) {
    Bundle b;
    std::string code, error;
    ASSERT_TRUE(parseProjectFile(fileOf(11, 11), b, code, error)) << error;
    EXPECT_TRUE(b.notes.empty());
}

TEST(PachislopMigrationTest, RefusesAFileWhoseVersionDisagreesWithItsProjectAndNonJson) {
    Bundle b;
    std::string code, error;
    EXPECT_FALSE(parseProjectFile(fileOf(11, 4), b, code, error));
    EXPECT_NE(error.find("Unsupported project version 11"), std::string::npos) << error;
    EXPECT_FALSE(parseProjectFile("{", b, code, error));
    EXPECT_NE(error.find("JSON parse failed"), std::string::npos);
    EXPECT_FALSE(parseProjectFile(R"({"format":"other"})", b, code, error));
    EXPECT_EQ(error, "Not a Pachislop project file");
    EXPECT_FALSE(parseProjectFile(fileOf(12, 12), b, code, error));
    EXPECT_EQ(code, "pachislop_version_newer");
    EXPECT_FALSE(parseProjectFile(fileOf(3, 3), b, code, error));
    EXPECT_NE(error.find("This is a version 3 project"), std::string::npos) << error;
}

// ==================================================================== effectDedupe.test.ts

TEST(PachislopMigrationTest, DedupeLeavesStacksWithoutRemovedEffectsAsTheyAre) {
    IdMint ids;
    const Json stack = Json::array({fx("outerGlow"), fx("keyline")});
    EXPECT_TRUE(same(dedupeEffects(stack, 120, ids), stack));
}

TEST(PachislopMigrationTest, DedupeReplacesEveryRemovedTypeInPlaceAndIsIdempotent) {
    for (const char* type : {"gradientGlow", "stroke", "longShadow", "shine", "rainbow", "hueSaturation", "colorAdjust", "gradientMap"}) {
        SCOPED_TRACE(type);
        IdMint ids;
        const Json stack = Json::array({fx("dropShadow"), fx(type, Json::object(), false), fx("grain")});
        const Json out = dedupeEffects(stack, 120, ids);
        EXPECT_TRUE(same(out[0], stack[0]));
        EXPECT_TRUE(same(out[out.size() - 1], stack[2]));
        ASSERT_GT(out.size(), 2u);
        EXPECT_EQ(out[1]["id"], std::string(type) + "-id");
        for (size_t i = 1; i + 1 < out.size(); i++) EXPECT_EQ(out[i]["enabled"], false);
        EXPECT_TRUE(same(dedupeEffects(out, 120, ids), out));
    }
}

TEST(PachislopMigrationTest, GradientGlowBecomesAPreciseGlowColouredByDistance) {
    IdMint ids;
    const Json outer = dedupeEffects(Json::array({fx("gradientGlow", Json{{"width", 20}, {"softness", 0.6}, {"opacity", 0.8}})}), 120, ids)[0];
    EXPECT_EQ(outer["type"], "outerGlow");
    const Json& p = outer["params"];
    EXPECT_EQ(p["fill"], "distance");
    EXPECT_EQ(p["technique"], "precise");
    EXPECT_EQ(p["size"], 20);
    EXPECT_NEAR(p["spread"].get<double>(), 0.4, 1e-9);
    EXPECT_EQ(p["intensity"], 1);
    EXPECT_EQ(p["opacity"], 0.8);
    EXPECT_EQ(p["mode"], "behind");
    const Json inner = dedupeEffects(Json::array({fx("gradientGlow", Json{{"inner", true}})}), 120, ids)[0];
    EXPECT_EQ(inner["type"], "innerGlow");
    EXPECT_EQ(inner["params"]["size"], 34);
    EXPECT_NEAR(inner["params"]["choke"].get<double>(), 0.25, 1e-9);
    EXPECT_EQ(inner["params"]["mode"], "normal");
}

TEST(PachislopMigrationTest, MultiStrokeBecomesKeylines) {
    IdMint ids;
    const Json first = dedupeEffects(Json::array({fx("stroke", Json{{"width1", 5}, {"width2", 0}, {"width3", 4}})}), 120, ids);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0]["params"]["width"], 9);
    EXPECT_EQ(first[0]["params"]["color"], "#2a0000");
    EXPECT_EQ(first[0]["params"]["follow"], "layer");
    EXPECT_EQ(first[1]["params"]["width"], 5);
    EXPECT_EQ(first[1]["params"]["color"], "#ffffff");
    const Json after = dedupeEffects(Json::array({fx("outerGlow"), fx("stroke", Json{{"opacity", 0.6}})}), 120, ids);
    ASSERT_EQ(after.size(), 4u);
    const int widths[] = {5, 9, 4};
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(after[i + 1]["params"]["width"], widths[i]);
        EXPECT_EQ(after[i + 1]["params"]["follow"], "stack");
        EXPECT_NEAR(after[i + 1]["params"]["threshold"].get<double>(), 0.3, 1e-9);
    }
}

TEST(PachislopMigrationTest, LongShadowBecomesAnUnshadedExtrudeAtTheGlobalLightsAngle) {
    IdMint ids;
    const Json e = dedupeEffects(Json::array({fx("longShadow", Json{{"angle", 10}, {"useGlobalLight", true}, {"length", 300}, {"fade", 0.5}})}), 120, ids)[0];
    EXPECT_EQ(e["type"], "extrude2d");
    EXPECT_EQ(e["params"]["angle"], 60);
    EXPECT_EQ(e["params"]["length"], 300);
    EXPECT_EQ(e["params"]["fade"], 0.5);
    EXPECT_EQ(e["params"]["shading"], 0);
    EXPECT_EQ(e["params"]["opacity"], 0.55);
    EXPECT_EQ(dedupeEffects(Json::array({fx("longShadow", Json{{"angle", 10}})}), 120, ids)[0]["params"]["angle"], 10);
}

TEST(PachislopMigrationTest, ShineAndRainbowBecomeGradientOverlay) {
    IdMint ids;
    const Json shine = dedupeEffects(Json::array({fx("shine", Json{{"angle", -30}, {"width", 40}})}), 120, ids)[0];
    EXPECT_EQ(shine["type"], "gradientOverlay");
    EXPECT_EQ(shine["params"]["style"], "sweep");
    EXPECT_EQ(shine["params"]["angle"], 30);
    EXPECT_EQ(shine["params"]["length"], 200);
    EXPECT_EQ(shine["params"]["mode"], "light");
    EXPECT_NEAR(shine["params"]["amount"].get<double>(), 0.9, 1e-9);
    const Json rainbow = dedupeEffects(Json::array({fx("rainbow", Json{{"scale", 2}, {"mode", "overlay"}})}), 120, ids)[0];
    EXPECT_EQ(rainbow["params"]["style"], "linear");
    EXPECT_EQ(rainbow["params"]["scale"], 0.5);
    EXPECT_EQ(rainbow["params"]["offset"], 0.5);
    EXPECT_EQ(rainbow["params"]["wrap"], "repeat");
    EXPECT_EQ(rainbow["params"]["mode"], "overlay");
}

TEST(PachislopMigrationTest, ColorAdjustBecomesExposureLevelsLevels) {
    IdMint ids;
    const Json out = dedupeEffects(Json::array({fx("colorAdjust", Json{{"exposure", 1}, {"gamma", 0.9}, {"contrast", 0.12}})}), 120, ids);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0]["params"]["adjustment"]["type"], "exposure");
    EXPECT_EQ(out[1]["params"]["adjustment"]["type"], "levels");
    EXPECT_EQ(out[2]["params"]["adjustment"]["type"], "levels");
    const Json& gamma = out[1]["params"]["adjustment"]["channels"]["rgb"];
    EXPECT_EQ(gamma["inBlack"], 0);
    EXPECT_EQ(gamma["inWhite"], 255);
    EXPECT_EQ(gamma["gamma"], 0.9);
    const Json& line = out[2]["params"]["adjustment"]["channels"]["rgb"];
    EXPECT_NEAR(line["inBlack"].get<double>(), (0.06 / 1.12) * 255, 1e-6);
    EXPECT_NEAR(line["inWhite"].get<double>(), (1.06 / 1.12) * 255, 1e-6);
    EXPECT_EQ(line["outBlack"], 0);
    EXPECT_EQ(line["outWhite"], 255);
}

TEST(PachislopMigrationTest, DedupeRunsOverLayerAndPartEffectsOfAProject) {
    IdMint ids;
    const Json p = Json{{"comp", Json{{"globalLight", Json{{"angle", 120}}}}},
                        {"layers", Json::array({Json{{"id", "t"}, {"kind", "title2d"}, {"effects", Json::array({fx("hueSaturation")})},
                                                     {"partEffects", Json{{"face", Json::array({fx("gradientMap")})}}}}})}};
    const Json out = effectDedupe(p, ids)["layers"][0];
    EXPECT_EQ(out["effects"][0]["type"], "adjust");
    EXPECT_EQ(out["partEffects"]["face"][0]["type"], "adjust");
}

// ==================================================================== generatorParams.test.ts

namespace {

    Json generatorLayer(const std::string& type, Json params, double scale = 1, double opacity = 1) {
        return Json{{"id", "g-" + type}, {"name", type}, {"kind", "generator"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                    {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", scale}, {"opacity", opacity}}}, {"effects", Json::array()},
                    {"masks", Json::array()}, {"generator", Json{{"type", type}, {"params", std::move(params)}}}};
    }

    Json projectOf(std::initializer_list<Json> layers) {
        return Json{{"version", 11}, {"comp", Json::object()}, {"layers", Json(layers)}};
    }

}

TEST(PachislopMigrationTest, GeneratorParamsPassesCurrentLayersThroughUnchanged) {
    IdMint ids;
    const Json p = projectOf({generatorLayer("particles", Json{{"count", 100}}), generatorLayer("speedLines", Json{{"type", "beta"}}),
                              generatorLayer("patternFill", Json{{"kind", "zebra"}})});
    EXPECT_TRUE(same(generatorParams(p, ids), p));
    const Json once = generatorParams(projectOf({generatorLayer("shockwave", Json{{"glow", 20}})}), ids);
    EXPECT_TRUE(same(generatorParams(once, ids), once));
}

TEST(PachislopMigrationTest, TurnsAGlowIntoALightOuterGlowAtTheBottomOfTheStack) {
    IdMint ids;
    Json l = generatorLayer("particles", Json{{"glow", 24}});
    l["effects"] = Json::array({Json{{"id", "mine"}, {"type", "gaussianBlur"}, {"enabled", true}, {"params", Json{{"radius", 2}}}}});
    const Json out = generatorParams(projectOf({l}), ids)["layers"][0];
    EXPECT_FALSE(out["generator"]["params"].contains("glow"));
    ASSERT_EQ(out["effects"].size(), 2u);
    EXPECT_EQ(out["effects"][0]["type"], "outerGlow");
    EXPECT_EQ(out["effects"][1]["type"], "gaussianBlur");
    EXPECT_EQ(out["effects"][0]["fromPreset"], true);
    const Json& p = out["effects"][0]["params"];
    EXPECT_EQ(p["fill"], "layer");
    EXPECT_EQ(p["mode"], "add");
    EXPECT_EQ(p["onFace"], 1);
    EXPECT_EQ(p["spread"], 0);
    EXPECT_EQ(p["size"], 24);
    EXPECT_EQ(p["intensity"], 0.8);
    EXPECT_FALSE(out["effects"][1].contains("fromPreset"));
}

TEST(PachislopMigrationTest, ScalesARingGlowByTheLayerScale) {
    IdMint ids;
    const Json out = generatorParams(projectOf({generatorLayer("brushSwirl", Json{{"glow", 10}}, 2)}), ids)["layers"][0];
    EXPECT_EQ(out["effects"][0]["params"]["size"], 20);
    EXPECT_EQ(out["effects"][0]["params"]["intensity"], 0.6);
}

TEST(PachislopMigrationTest, MovesTheBetaBurstFillToASunburstLayerBelowAndItsHalftoneToEffects) {
    IdMint ids;
    const Json l = generatorLayer("speedLines", Json{{"type", "beta"}, {"opacity", 50}, {"fillBurst", true}, {"burstColor", "#ff0000"}, {"halftone", true}, {"dotSize", 6}},
                                  1, 0.8);
    const Json layers = generatorParams(projectOf({l}), ids)["layers"];
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0]["generator"]["type"], "sunburst");
    EXPECT_EQ(layers[1]["generator"]["type"], "speedLines");
    const Json &burst = layers[0], &lines = layers[1];
    EXPECT_NEAR(lines["transform"]["opacity"].get<double>(), 0.8, 1e-9);
    EXPECT_EQ(lines["generator"]["params"]["opacity"], 50);
    EXPECT_NEAR(burst["transform"]["opacity"].get<double>(), 0.4, 1e-9);
    EXPECT_EQ(burst["generator"]["params"]["colorMode"], "ab");
    EXPECT_EQ(burst["generator"]["params"]["colorA"], "#ff0000");
    EXPECT_EQ(burst["generator"]["params"]["colorB"], "#ff0000");
    EXPECT_EQ(burst["generator"]["params"]["glow"], 0);
    EXPECT_TRUE(burst["effects"].empty());
    ASSERT_EQ(lines["effects"].size(), 1u);
    EXPECT_EQ(lines["effects"][0]["type"], "halftone");
    EXPECT_EQ(lines["effects"][0]["params"]["source"], "alpha");
    EXPECT_EQ(lines["effects"][0]["params"]["size"], 6);
    EXPECT_EQ(lines["effects"][0]["params"]["fade"], 24);
    for (const char* k : {"fillBurst", "burstColor", "halftone", "dotSize", "edgeBlur", "glow"}) EXPECT_FALSE(lines["generator"]["params"].contains(k)) << k;
}

TEST(PachislopMigrationTest, TurnsAPatternFadeIntoALinkedGradientMask) {
    IdMint ids;
    const Json out = generatorParams(projectOf({generatorLayer("patternFill", Json{{"fade", "radial"}, {"fadeX", 10}, {"fadeY", -20}, {"fadeRadius", 300},
                                                                                   {"feather", 120}})}),
                                     ids)["layers"][0];
    const Json& m = out["masks"][0];
    EXPECT_EQ(m["kind"], "gradient");
    EXPECT_EQ(m["shape"], "radial");
    EXPECT_EQ(m["linked"], true);
    EXPECT_EQ(m["x"], 10);
    EXPECT_EQ(m["y"], -20);
    EXPECT_EQ(m["length"], 340);
    EXPECT_NEAR(m["inner"].get<double>() * m["length"].get<double>(), 260, 1e-9);
    for (auto it = out["generator"]["params"].begin(); it != out["generator"]["params"].end(); ++it) {
        EXPECT_TRUE(it.key().rfind("fade", 0) != 0 && it.key() != "feather" && it.key() != "opacity") << it.key();
    }
}

TEST(PachislopMigrationTest, TurnsASunburstVignetteIntoARadialShadingGradientOverlay) {
    IdMint ids;
    const Json sun = generatorParams(projectOf({generatorLayer("sunburst", Json{{"vignette", 45}})}), ids)["layers"][0];
    EXPECT_FALSE(sun["generator"]["params"].contains("vignette"));
    EXPECT_EQ(sun["effects"][0]["type"], "gradientOverlay");
    EXPECT_EQ(sun["effects"][0]["fromPreset"], true);
    EXPECT_EQ(sun["effects"][0]["params"]["style"], "radial");
    EXPECT_EQ(sun["effects"][0]["params"]["mode"], "shade");
}

// ==================================================================== materialEffects.test.ts

TEST(PachislopMigrationTest, MaterialLeavesStacksWithoutTheRemovedEffectsAsTheyAre) {
    const Json stack = Json::array({fx("outerGlow"), fx("material")});
    EXPECT_TRUE(same(migrateMaterialEffects(stack), stack));
}

TEST(PachislopMigrationTest, TurnsBevelChromeGlassAndMetalTextureIntoMaterialInPlace) {
    for (const char* type : {"bevel", "chrome", "glass", "metalTexture"}) {
        SCOPED_TRACE(type);
        const Json stack = Json::array({fx("dropShadow"), fx(type, Json::object(), false), fx("grain")});
        const Json out = migrateMaterialEffects(stack);
        EXPECT_TRUE(same(out[0], stack[0]));
        EXPECT_TRUE(same(out[2], stack[2]));
        EXPECT_EQ(out[1]["id"], std::string(type) + "-id");
        EXPECT_EQ(out[1]["type"], "material");
        EXPECT_EQ(out[1]["enabled"], false);
        EXPECT_EQ(out[1]["params"]["size"], 200);
        EXPECT_EQ(out[1]["params"]["colorMode"], std::string(type) == "chrome" ? "fill" : "layer");
        EXPECT_TRUE(same(migrateMaterialEffects(out), out));
    }
}

TEST(PachislopMigrationTest, BevelBecomesABevelOfTheSameWidthHeightSmoothnessAndLight) {
    const Json b = migrateMaterialEffects(Json::array({fx("bevel", Json{{"width", 30}, {"depth", 70}, {"smooth", 3}, {"angle", 60}, {"elevation", 25},
                                                                        {"profile", "chamfer"}, {"useGlobalLight", true}})}))[0]["params"]["material"]["bevel"];
    EXPECT_EQ(b["enabled"], true);
    EXPECT_EQ(b["profile"], "chamfer");
    EXPECT_EQ(b["sizeMode"], "fixed");
    EXPECT_EQ(b["size"], 15);
    EXPECT_EQ(b["soften"], 3);
    EXPECT_EQ(b["lightAngle"], 60);
    EXPECT_EQ(b["lightAltitude"], 25);
    EXPECT_EQ(b["useGlobalLight"], true);
    EXPECT_EQ(b["gloss"], "linear");
    EXPECT_NEAR(b["depth"].get<double>(), 0.7 / 0.35, 1e-9);
    const Json round = migrateMaterialEffects(Json::array({fx("bevel")}))[0]["params"]["material"]["bevel"];
    EXPECT_EQ(round["profile"], "custom");
    EXPECT_EQ(round["profileCurve"]["points"][0]["x"], 0);
    EXPECT_EQ(round["profileCurve"]["points"][0]["y"], 0);
    EXPECT_EQ(round["profileCurve"]["points"].back()["x"], 1);
    EXPECT_EQ(round["profileCurve"]["points"].back()["y"], 1);
    EXPECT_EQ(round["useGlobalLight"], false) << "a missing flag predates the global light";
    const auto shadow = [](double s) {
        return migrateMaterialEffects(Json::array({fx("bevel", Json{{"shadow", s}})}))[0]["params"]["material"]["bevel"]["shadowOpacity"].get<double>();
    };
    EXPECT_GT(shadow(90), shadow(30));
    EXPECT_EQ(shadow(0), 0);
}

TEST(PachislopMigrationTest, ChromeBecomesItsMapAsATopToBottomFill) {
    const Json m = migrateMaterialEffects(Json::array({fx("chrome", Json{{"map", "gold"}, {"horizon", 20}, {"repeat", 2}, {"tint", "#ff0000"}, {"tintAmount", 100},
                                                                         {"distortion", 70}})}))[0]["params"]["material"];
    EXPECT_EQ(m["fill"]["kind"], "linear");
    EXPECT_EQ(m["fill"]["angle"], 90);
    EXPECT_EQ(m["fill"]["span"], "line");
    EXPECT_NEAR(m["fill"]["scale"].get<double>(), 1 / 0.85, 1e-9);
    EXPECT_NEAR(m["fill"]["offset"].get<double>(), -0.085, 1e-9);
    for (const Json& s : m["fill"]["gradient"]["stops"]) EXPECT_TRUE(std::regex_match(s["color"].get<std::string>(), std::regex("^#[0-9a-f]{2}0000$"))) << s.dump();
    EXPECT_LT(m["refraction"].get<double>(), 0);
    const Json custom = Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#123456"}}, Json{{"pos", 1}, {"color", "#abcdef"}}})}};
    EXPECT_TRUE(same(migrateMaterialEffects(Json::array({fx("chrome", Json{{"map", "custom"}, {"gradient", custom}})}))[0]["params"]["material"]["fill"]["gradient"], custom));
}

TEST(PachislopMigrationTest, GlassBecomesGlass2DRefractionPxToPercentNoShadow) {
    const Json m = migrateMaterialEffects(Json::array({fx("glass", Json{{"color", "#ff00aa"}, {"opacity", 40}, {"tint", 80}, {"edge", 50}, {"caustic", 150},
                                                                        {"refraction", 20}})}))[0]["params"]["material"];
    EXPECT_TRUE(same(m["glass"], Json{{"enabled", true}, {"color", "#ff00aa"}, {"tint", 0.8}, {"opacity", 0.4}, {"edge", 0.5}, {"caustic", 1.5}}));
    EXPECT_EQ(m["refraction"], 10);
    EXPECT_EQ(m["bevel"]["shadowOpacity"], 0);
}

TEST(PachislopMigrationTest, MetalTextureBecomesTextureKindGrainDirectionStrengthAndSheen) {
    const auto one = [](Json params) { return migrateMaterialEffects(Json::array({fx("metalTexture", std::move(params))}))[0]["params"]["material"]; };
    const Json spun = one(Json{{"mode", "radial"}, {"scale", 4}, {"angle", 30}, {"strength", 50}})["texture"];
    EXPECT_EQ(spun["kind"], "spun");
    EXPECT_EQ(spun["scale"], 2);
    EXPECT_EQ(spun["angle"], 30);
    EXPECT_EQ(spun["amount"], 0.5);
    EXPECT_EQ(spun["blend"], "overlay");
    EXPECT_EQ(one(Json{{"mode", "hairline"}, {"scale", 4}})["texture"]["kind"], "hairline");
    EXPECT_NEAR(one(Json{{"mode", "hairline"}, {"scale", 4}})["texture"]["scale"].get<double>(), 0.9, 1e-9);
    EXPECT_EQ(one(Json{{"mode", "flake"}})["texture"]["kind"], "flakes");
    EXPECT_TRUE(same(one(Json{{"sheen", false}, {"sheenPosition", 40}, {"sheenAmount", 120}, {"sheenWidth", 50}, {"sheenColor", "#ffee00"}})["sheen"],
                     Json{{"enabled", false}, {"color", "#ffee00"}, {"amount", 1.2}, {"position", 0.4}, {"width", 0.5}}));
    EXPECT_EQ(one(Json::object())["bevel"]["enabled"], false);
}

TEST(PachislopMigrationTest, MaterialRunsOverEveryLayerAndPartStackOfAProject) {
    const Json p = Json{{"layers", Json::array({Json{{"id", "t"}, {"kind", "title2d"}, {"effects", Json::array({fx("glass")})},
                                                     {"partEffects", Json{{"face", Json::array({fx("bevel")})}}}}})}};
    const Json out = materialEffects(p)["layers"][0];
    EXPECT_EQ(out["effects"][0]["type"], "material");
    EXPECT_EQ(out["partEffects"]["face"][0]["type"], "material");
}

// ==================================================================== contentEffects.test.ts

namespace {

    Json title(const std::string& id, const std::string& name, Json effects) {
        return Json{{"id", id}, {"name", name}, {"kind", "title2d"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                    {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", std::move(effects)},
                    {"masks", Json::array()}, {"partEffects", Json::object()}};
    }

    std::vector<std::string> shape(const Json& p) {
        std::vector<std::string> out;
        for (const Json& l : p["layers"]) {
            const std::string kind = l["kind"];
            out.push_back(kind == "generator" ? l["generator"]["type"].get<std::string>() : kind == "group" ? "[" + l["name"].get<std::string>() + "]"
                                                                                                            : l["name"].get<std::string>());
        }
        return out;
    }

    const Json* generatorOf(const Json& p, const std::string& type) {
        for (const Json& l : p["layers"]) if (l["kind"] == "generator" && l["generator"]["type"] == type) return &l;
        return nullptr;
    }

}

TEST(PachislopMigrationTest, ContentE1SparklesE2BecomesAGroupCarryingE2AndAGlintsCompanion) {
    IdMint ids;
    const Json t = title("t", "確定", Json::array({fx("outerGlow"), fx("sparkles", Json{{"placement", "hero"}, {"size", 52}}), fx("dropShadow")}));
    const Json out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(shape(out), std::vector<std::string>({"確定", "glints", "[確定]"}));
    const Json &layer = out["layers"][0], &glints = out["layers"][1], &group = out["layers"][2];
    ASSERT_EQ(layer["effects"].size(), 1u);
    EXPECT_EQ(layer["effects"][0]["type"], "outerGlow");
    ASSERT_EQ(group["effects"].size(), 1u);
    EXPECT_EQ(group["effects"][0]["type"], "dropShadow");
    EXPECT_EQ(group["companionGroupOf"], "t");
    EXPECT_EQ(glints["blend"], "add");
    EXPECT_EQ(glints["companionOf"], "t");
    EXPECT_EQ(glints["parentId"], group["id"]);
    EXPECT_EQ(glints["generator"]["params"]["follow"], "t");
    EXPECT_EQ(glints["generator"]["params"]["placement"], "hero");
    EXPECT_EQ(glints["generator"]["params"]["size"], 52);
}

TEST(PachislopMigrationTest, ConsecutiveContentEffectsAreSiblingsTheNextRunFollowsTheGroup) {
    IdMint ids;
    const Json t = title("t", "激", Json::array({fx("fire"), fx("smoke"), fx("lightning"), fx("deepGlow"), fx("sparkles")}));
    const Json out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(shape(out), std::vector<std::string>({"smoke", "fire", "激", "lightning", "[激]", "glints", "[激]"}));
    const Json& inner = out["layers"][4];
    ASSERT_EQ(inner["effects"].size(), 1u);
    EXPECT_EQ(inner["effects"][0]["type"], "deepGlow");
    const Json* glints = generatorOf(out, "glints");
    ASSERT_NE(glints, nullptr);
    EXPECT_EQ((*glints)["companionOf"], "t");
    EXPECT_EQ((*glints)["generator"]["params"]["follow"], inner["id"]);
}

TEST(PachislopMigrationTest, KeepsAFreeLensFlareFreeHidesDisabledCompanionsBurstRaysBehindGoBelow) {
    IdMint ids;
    const Json t = title("t", "A", Json::array({fx("lensFlare", Json{{"x", 10}}), fx("starGlint", Json::object(), false), fx("burstRays", Json{{"placement", "behind"}})}));
    const Json out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(shape(out), std::vector<std::string>({"sunburst", "A", "lensFlare", "glints", "[A]"}));
    EXPECT_EQ((*generatorOf(out, "lensFlare"))["generator"]["params"]["follow"], "");
    EXPECT_EQ((*generatorOf(out, "glints"))["visible"], false);
    const Json& rays = (*generatorOf(out, "sunburst"))["generator"]["params"];
    EXPECT_EQ(rays["style"], "wedges");
    EXPECT_EQ(rays["onLayer"], "all");
    EXPECT_EQ(rays["centre"], "point");
    EXPECT_EQ(rays["follow"], "t");
}

TEST(PachislopMigrationTest, ParticleBurstInFrontAndBehindTwoParticlesLayers) {
    IdMint ids;
    const Json out = contentEffects(projectOf({title("t", "A", Json::array({fx("particleBurst", Json{{"glow", 0}})}))}), ids);
    EXPECT_EQ(shape(out), std::vector<std::string>({"particles", "A", "particles", "[A]"}));
    const Json &back = out["layers"][0], &front = out["layers"][2];
    EXPECT_EQ(back["generator"]["params"]["emitter"], "layer");
    EXPECT_EQ(back["generator"]["params"]["split"], "back");
    EXPECT_EQ(back["generator"]["params"]["travel"], 180);
    EXPECT_EQ(front["blend"], "add");
    EXPECT_EQ(front["generator"]["params"]["split"], "front");
    EXPECT_TRUE(front["effects"].empty());
}

TEST(PachislopMigrationTest, TakesOverTheLayersPlaceClippingBlendAndOpacityGoToTheGroup) {
    IdMint ids;
    const Json base = title("base", "base", Json::array());
    Json t = title("t", "A", Json::array({fx("sparkles")}));
    t["clip"] = true;
    t["blend"] = "screen";
    t["transform"]["opacity"] = 0.6;
    const Json g = Json{{"id", "g"}, {"name", "G"}, {"kind", "group"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                        {"transform", Json{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", Json::array({fx("fire")})},
                        {"masks", Json::array()}, {"collapsed", false}};
    Json member = title("m", "m", Json::array());
    member["parentId"] = "g";
    const Json out = contentEffects(projectOf({base, t, member, g}), ids);
    const Json* wrap = nullptr;
    for (const Json& l : out["layers"]) if (l["kind"] == "group" && l["name"] == "A") wrap = &l;
    ASSERT_NE(wrap, nullptr);
    EXPECT_EQ((*wrap)["clip"], true);
    EXPECT_EQ((*wrap)["blend"], "screen");
    EXPECT_EQ((*wrap)["transform"]["opacity"], 0.6);
    for (const Json& l : out["layers"]) {
        if (l["id"] == "t") {
            EXPECT_EQ(l["clip"], false);
            EXPECT_EQ(l["blend"], "normal");
            EXPECT_EQ(l["transform"]["opacity"], 1);
        }
        if (l["id"] == "g") EXPECT_TRUE(l["effects"].empty());
    }
    EXPECT_EQ((*generatorOf(out, "fire"))["generator"]["params"]["follow"], "g");
}

TEST(PachislopMigrationTest, ContentIsIdempotentAndLeavesProjectsWithoutContentEffectsAlone) {
    IdMint ids;
    const Json plain = projectOf({title("t", "A", Json::array({fx("outerGlow")}))});
    EXPECT_TRUE(same(contentEffects(plain, ids)["layers"], plain["layers"]));
    const Json once = contentEffects(projectOf({title("t", "A", Json::array({fx("sparkles")}))}), ids);
    EXPECT_TRUE(same(contentEffects(once, ids)["layers"], once["layers"]));
}

TEST(PachislopMigrationTest, TurnsAParticlesLayerAlphaEmitterIntoFollowing) {
    IdMint ids;
    const Json l = generatorLayer("particles", Json{{"emitter", "layer"}, {"layer", "L1"}});
    const Json out = contentEffects(projectOf({l}), ids)["layers"][0]["generator"]["params"];
    EXPECT_EQ(out["emitter"], "layer");
    EXPECT_EQ(out["follow"], "L1");
    EXPECT_EQ(out["edgeWidth"], 6);
}
