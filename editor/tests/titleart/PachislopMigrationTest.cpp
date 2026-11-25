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
// its own name: Title Art's, Pachislop's and the editor's Json aliases may all be in sight in a unity unit
using PachislopMigrationJson = jasperengine::titleart::Json;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

namespace {

    PachislopMigrationJson fx(const std::string& type, PachislopMigrationJson params = PachislopMigrationJson::object(), bool enabled = true) {
        return PachislopMigrationJson{{"id", type + "-id"}, {"type", type}, {"enabled", enabled}, {"params", std::move(params)}};
    }

    // nlohmann's comparison ignores member order: Pachislop's toEqual
    bool same(const PachislopMigrationJson& a, const PachislopMigrationJson& b) {
        return nlohmann::json(a) == nlohmann::json(b);
    }

    // a current Material3D: every field the migration fills already present
    PachislopMigrationJson mat3(const std::string& preset, const std::string& color) {
        return PachislopMigrationJson{{"preset", preset}, {"color", color}, {"metalness", 1}, {"roughness", 0.3}, {"gradientAxis", "xy"}, {"gradientBend", 0},
                    {"glassDepth", 0}, {"environment", PachislopMigrationJson::object()}, {"surface", PachislopMigrationJson{{"kind", "none"}, {"amount", 0.5}, {"scale", 20}, {"seed", 1}}}};
    }

    // ---- a v4-shaped project (the fields v4 lacked left out) ----

    PachislopMigrationJson mat3V4(const std::string& preset, const std::string& color) {
        return PachislopMigrationJson{{"preset", preset}, {"color", color}, {"metalness", 1}, {"roughness", 0.3}};
    }

    PachislopMigrationJson material2DV4(const std::string& color) {
        return PachislopMigrationJson{{"fill", PachislopMigrationJson{{"kind", "solid"}, {"color", color}}},
                    {"bevel", PachislopMigrationJson{{"enabled", true}, {"profile", "smooth"}, {"size", 3}}},
                    {"satin", PachislopMigrationJson{{"enabled", false}}},
                    {"texture", PachislopMigrationJson{{"kind", "none"}}},
                    {"gloss", PachislopMigrationJson{{"enabled", false}}}};
    }

    PachislopMigrationJson legacyStyle3D() {
        return PachislopMigrationJson{{"depth", 14}, {"face", mat3V4("gold", "#ffcc33")}, {"side", mat3V4("gold", "#ffcc33")},
                    {"bands", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "ring"}, {"width", 2.5}, {"depth", 2.5}, {"profile", "round"}, {"segments", 6}, {"curve", 0},
                                                {"material", mat3V4("gold", "#ffcc33")}}})},
                    {"backplates", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "b1"}, {"enabled", true}, {"grow", 4}, {"join", "miter"}, {"fillCounters", true},
                                                     {"depth", 10}, {"zGap", 0}, {"bevel", PachislopMigrationJson{{"width", 1.5}, {"depth", 1.5}, {"profile", "round"},
                                                                                                {"segments", 6}, {"curve", 0}}},
                                                     {"face", mat3V4("chrome", "#dddddd")}, {"side", mat3V4("chrome", "#dddddd")}}})}};
    }

    PachislopMigrationJson v4Project(int version) {
        PachislopMigrationJson t2 = PachislopMigrationJson{{"id", "t2"}, {"name", "BIG BONUS"}, {"kind", "title2d"}, {"visible", true}, {"blend", "normal"},
                       {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}},
                       {"effects", PachislopMigrationJson::array({fx("dropShadow", PachislopMigrationJson{{"angle", 60}, {"distance", 10}, {"size", 14}})})},
                       {"source", PachislopMigrationJson{{"kind", "text"}, {"text", "BIG BONUS"}, {"fontId", "dela-gothic-one"}, {"size", 200},
                                       {"chars", PachislopMigrationJson{{"2", PachislopMigrationJson{{"dx", 5}, {"dy", -3}, {"scale", 0.8}, {"rotate", 10},
                                                                 {"face", material2DV4("#ff0000")}}}}}}},
                       {"style", PachislopMigrationJson{{"face", material2DV4("#ffd700")},
                                      {"rims", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "r1"}, {"enabled", true}, {"width", 4}, {"join", "round"},
                                                                 {"material", material2DV4("#000000")}}})}}}};
        PachislopMigrationJson t3 = PachislopMigrationJson{{"id", "t3"}, {"name", "確定"}, {"kind", "title3d"}, {"visible", true},
                       {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", PachislopMigrationJson::array()},
                       {"source", PachislopMigrationJson{{"kind", "text"}, {"text", "確定"}, {"fontId", "dela-gothic-one"}, {"size", 200},
                                       {"chars", PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 1}, {"dy", 0}, {"scale", 1.2}, {"rotate", 0}, {"face3d", mat3V4("gold", "#ffcc33")}}}}}}},
                       {"style3d", legacyStyle3D()}};
        PachislopMigrationJson img = PachislopMigrationJson{{"id", "img"}, {"name", "img"}, {"kind", "title2d"}, {"visible", true},
                        {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", PachislopMigrationJson::array()},
                        {"source", PachislopMigrationJson{{"kind", "image"}, {"assetId", "asset-1"}, {"size", 300}}}, {"style", PachislopMigrationJson{{"face", material2DV4("#ffffff")}, {"rims", PachislopMigrationJson::array()}}}};
        PachislopMigrationJson bg = PachislopMigrationJson{{"id", "bg"}, {"name", "Background"}, {"kind", "background"}, {"visible", true},
                       {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", PachislopMigrationJson::array()},
                       {"fill", PachislopMigrationJson{{"kind", "solid"}, {"color", "#000000"}}}};
        return PachislopMigrationJson{{"version", version}, {"name", "T"},
                    {"comp", PachislopMigrationJson{{"width", 1920}, {"height", 1080}, {"environment", PachislopMigrationJson{{"preset", "studio"}, {"intensity", 1}}}}},
                    {"layers", PachislopMigrationJson::array({bg, t3, t2, img})}};
    }

    PachislopMigrationJson pachislopMigrationMigrated(const PachislopMigrationJson& project) {
        PachislopMigrationJson out;
        std::string error;
        EXPECT_TRUE(migrateProject(project, out, error)) << error;
        return out;
    }

    const PachislopMigrationJson& pachislopMigrationLayerOf(const PachislopMigrationJson& p, size_t i) { return p["layers"][i]; }

    // a v10 project with one 3D text title in the v4-v10 shape
    PachislopMigrationJson pachislopMigrationStored(PachislopMigrationJson style, PachislopMigrationJson chars = PachislopMigrationJson::object(), PachislopMigrationJson partEffects = PachislopMigrationJson::object()) {
        PachislopMigrationJson s = PachislopMigrationJson{{"face", mat3("gold", "#ffcc33")}, {"depth", 10}, {"bevelPosition", "outside"}, {"bands", PachislopMigrationJson::array()}, {"backplates", PachislopMigrationJson::array()},
                      {"mode", "extrude"}};
        for (auto it = style.begin(); it != style.end(); ++it) s[it.key()] = it.value();
        PachislopMigrationJson t3 = PachislopMigrationJson{{"id", "t3"}, {"name", "確定"}, {"kind", "title3d"}, {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"scale", 1}, {"opacity", 1}}},
                       {"effects", PachislopMigrationJson::array()}, {"source", PachislopMigrationJson{{"kind", "text"}, {"text", "確定"}, {"chars", chars}}}, {"style3d", s},
                       {"partEffects", partEffects}};
        return PachislopMigrationJson{{"version", 10}, {"name", "T"}, {"comp", PachislopMigrationJson{{"width", 1920}, {"height", 1080}, {"environment", PachislopMigrationJson::object()}}},
                    {"layers", PachislopMigrationJson::array({t3})}};
    }

    PachislopMigrationJson ring(const std::string& id, double width, double depth, const PachislopMigrationJson& material, const std::string& profile = "round") {
        return PachislopMigrationJson{{"id", id}, {"width", width}, {"depth", depth}, {"profile", profile}, {"segments", 6}, {"curve", 0}, {"material", material}};
    }

    PachislopMigrationJson plate(const std::string& id, PachislopMigrationJson o = PachislopMigrationJson::object()) {
        PachislopMigrationJson p = PachislopMigrationJson{{"id", id}, {"enabled", true}, {"grow", 4}, {"join", "round"}, {"fillCounters", true},
                      {"plate", PachislopMigrationJson{{"kind", "box"}, {"close", 0}, {"corner", 2}, {"slant", 8}}}, {"depth", 7}, {"zGap", 1.5},
                      {"bevel", PachislopMigrationJson{{"width", 0.8}, {"depth", 1.2}, {"profile", "chamfer"}, {"segments", 3}, {"curve", 0.2}}},
                      {"face", mat3("blackGloss", "#101010")}};
        for (auto it = o.begin(); it != o.end(); ++it) p[it.key()] = it.value();
        return p;
    }

    const PachislopMigrationJson DARK = mat3("custom", "#3a1000");

}

// ==================================================================== project.test.ts: migrateProject

TEST(PachislopMigrationTest, AV4OrV5ProjectGainsEveryCurrentFieldWithTheOldBehaviour) {
    for (int version : {4, 5}) {
        SCOPED_TRACE(version);
        const PachislopMigrationJson old = v4Project(version);
        const std::string text = old.dump();
        for (const char* newer : {"useGlobalLight", "tateChuYoko", "bevelPosition", "gradientAxis", "\"surface\"", "\"clip\"", "partEffects", "\"trace\"",
                                  "\"dpi\"", "\"erode\"", "\"plate\"", "\"weight\""}) {
            EXPECT_EQ(text.find(newer), std::string::npos) << "the sample has none of the newer fields: " << newer;
        }
        const PachislopMigrationJson got = pachislopMigrationMigrated(old);
        EXPECT_EQ(got["version"], 11);
        EXPECT_EQ(old.dump(), text) << "the input is not modified";
        const PachislopMigrationJson& t2 = pachislopMigrationLayerOf(got, 2);
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
        EXPECT_EQ(pachislopMigrationLayerOf(got, 3)["source"]["trace"]["threshold"], 128);
        // the 3D title's v4 style converted to the letter and its bands
        const PachislopMigrationJson& s3 = pachislopMigrationLayerOf(got, 1)["style3d"];
        EXPECT_FALSE(s3.contains("backplates"));
        EXPECT_EQ(s3["bevelPosition"], "inside") << "files from before bevelPosition cut their bands inward";
        EXPECT_EQ(s3["face"]["gradientAxis"], "xy");
        EXPECT_EQ(s3["face"]["surface"]["kind"], "none");
        // migrating twice equals once
        EXPECT_TRUE(same(pachislopMigrationMigrated(got), got));
    }
}

TEST(PachislopMigrationTest, KeepsPerCharacterOffsetsAndScalesAsStored) {
    const PachislopMigrationJson got = pachislopMigrationMigrated(v4Project(4));
    const PachislopMigrationJson& c = pachislopMigrationLayerOf(got, 2)["source"]["chars"]["2"];
    EXPECT_EQ(c["dx"], 5);
    EXPECT_EQ(c["dy"], -3);
    EXPECT_EQ(c["scale"], 0.8);
    EXPECT_EQ(c["rotate"], 10);
}

TEST(PachislopMigrationTest, AV6FileFromBeforeImageTracingAndFaceImagesGetsThem) {
    PachislopMigrationJson old = pachislopMigrationMigrated(v4Project(4));
    old["version"] = 6;
    old["layers"][3]["source"].erase("trace");
    old["layers"][1]["style3d"].erase("faceImage");
    const PachislopMigrationJson got = pachislopMigrationMigrated(old);
    EXPECT_TRUE(same(got["layers"][3]["source"]["trace"], PachislopMigrationJson{{"threshold", 128}, {"smooth", 0.5}, {"despeckle", 0.05}}));
    EXPECT_EQ(got["layers"][1]["style3d"]["faceImage"], 1);
}

TEST(PachislopMigrationTest, KeepsValuesThatArePresentInsteadOfOverwritingThem) {
    PachislopMigrationJson old = v4Project(4);
    old["comp"]["globalLight"] = PachislopMigrationJson{{"angle", 45}};
    old["comp"]["dpi"] = 300;
    old["layers"][2]["style"]["face"]["bevel"]["useGlobalLight"] = true;
    old["layers"][2]["source"]["tateChuYoko"] = true;
    old["layers"][2]["effects"][0]["params"]["useGlobalLight"] = true;
    old["layers"][1]["style3d"]["bevelPosition"] = "outside";
    old["layers"][1]["style3d"]["face"]["surface"] = PachislopMigrationJson{{"kind", "brushed"}};
    old["layers"][2]["clip"] = true;
    const PachislopMigrationJson got = pachislopMigrationMigrated(old);
    EXPECT_TRUE(same(got["comp"]["globalLight"], PachislopMigrationJson{{"angle", 45}, {"altitude", 30}}));
    EXPECT_EQ(got["comp"]["dpi"], 300);
    EXPECT_EQ(pachislopMigrationLayerOf(got, 2)["style"]["face"]["bevel"]["useGlobalLight"], true);
    EXPECT_EQ(pachislopMigrationLayerOf(got, 2)["source"]["tateChuYoko"], true);
    EXPECT_EQ(pachislopMigrationLayerOf(got, 2)["effects"][0]["params"]["useGlobalLight"], true);
    EXPECT_EQ(pachislopMigrationLayerOf(got, 2)["clip"], true);
    EXPECT_EQ(pachislopMigrationLayerOf(got, 1)["style3d"]["bevelPosition"], "outside");
    EXPECT_TRUE(same(pachislopMigrationLayerOf(got, 1)["style3d"]["face"]["surface"], PachislopMigrationJson{{"kind", "brushed"}, {"amount", 0.5}, {"scale", 20}, {"seed", 1}}))
        << "a partial nested object keeps its fields and gets the rest";
}

TEST(PachislopMigrationTest, IsIdempotentCurrentDataPassesThroughUnchanged) {
    const PachislopMigrationJson once = pachislopMigrationMigrated(v4Project(4));
    EXPECT_TRUE(same(pachislopMigrationMigrated(once), once));
    const PachislopMigrationJson current = PachislopMigrationJson{{"version", 11}, {"name", "Untitled"}, {"comp", once["comp"]}, {"layers", PachislopMigrationJson::array()}};
    EXPECT_TRUE(same(pachislopMigrationMigrated(current), current));
}

TEST(PachislopMigrationTest, MigratesPartEffectsOfV6Titles) {
    PachislopMigrationJson p = pachislopMigrationMigrated(v4Project(4));
    p["layers"][2]["partEffects"] = PachislopMigrationJson{{"face", PachislopMigrationJson::array({fx("dropShadow", PachislopMigrationJson{{"angle", 30}})})}};
    p["version"] = 6;
    EXPECT_EQ(pachislopMigrationMigrated(p)["layers"][2]["partEffects"]["face"][0]["params"]["useGlobalLight"], false);
}

TEST(PachislopMigrationTest, RefusesVersion1To3NamingTheVersionAndTheSupportedOnes) {
    for (int version : {1, 2, 3}) {
        PachislopMigrationJson old = v4Project(4);
        old["version"] = version;
        PachislopMigrationJson out;
        std::string error;
        EXPECT_FALSE(migrateProject(old, out, error));
        EXPECT_TRUE(std::regex_search(error, std::regex("version " + std::to_string(version) + " project.*version 4 to 11"))) << error;
    }
}

TEST(PachislopMigrationTest, RefusesOtherVersionsAsUnsupported) {
    for (const PachislopMigrationJson& version : {PachislopMigrationJson(12), PachislopMigrationJson(0), PachislopMigrationJson("7"), PachislopMigrationJson()}) {
        PachislopMigrationJson old = v4Project(4);
        old["version"] = version;
        PachislopMigrationJson out;
        std::string error;
        EXPECT_FALSE(migrateProject(old, out, error));
        EXPECT_TRUE(std::regex_search(error, std::regex(R"(Unsupported project version .* \(expected 4 to 11\))"))) << error;
    }
}

TEST(PachislopMigrationTest, RefusesNonProjects) {
    for (const PachislopMigrationJson& bad : {PachislopMigrationJson(), PachislopMigrationJson("x"), PachislopMigrationJson::array(), PachislopMigrationJson{{"version", 7}, {"comp", PachislopMigrationJson::object()}}, PachislopMigrationJson{{"version", 7}, {"layers", PachislopMigrationJson::array()}},
                            PachislopMigrationJson{{"version", 7}, {"layers", PachislopMigrationJson::object()}, {"comp", PachislopMigrationJson::object()}}}) {
        PachislopMigrationJson out;
        std::string error;
        EXPECT_FALSE(migrateProject(bad, out, error));
        EXPECT_EQ(error, "Not a Pachislop project");
    }
}

// ==================================================================== project.test.ts: v8 -> v9 tiltY

namespace {

    PachislopMigrationJson withChars(int version, PachislopMigrationJson chars) {
        PachislopMigrationJson p = pachislopMigrationStored(PachislopMigrationJson::object(), std::move(chars));
        p["version"] = version;
        p["layers"][0]["style3d"]["bands"] = PachislopMigrationJson::array();
        p["layers"][0]["style3d"].erase("backplates");
        p["layers"][0]["style3d"]["bevel"] = PachislopMigrationJson{{"width", 0}, {"depth", 0}, {"profile", "round"}, {"segments", 6}, {"curve", 0}};
        return p;
    }

}

TEST(PachislopMigrationTest, NegatesAV8GlyphsTiltYAndKeepsItsOther3DAdjustments) {
    const PachislopMigrationJson got = pachislopMigrationMigrated(withChars(8, PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltX", 12}, {"tiltY", 20}, {"dz", 5}}}}));
    EXPECT_EQ(got["version"], 11);
    const PachislopMigrationJson& c = got["layers"][0]["source"]["chars"]["0"];
    EXPECT_EQ(c["tiltX"], 12);
    EXPECT_EQ(c["tiltY"], -20);
    EXPECT_EQ(c["dz"], 5);
}

TEST(PachislopMigrationTest, LeavesAGlyphWithoutTiltYWithoutIt) {
    const PachislopMigrationJson got = pachislopMigrationMigrated(withChars(8, PachislopMigrationJson{{"1", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltX", 12}}}}));
    EXPECT_FALSE(got["layers"][0]["source"]["chars"]["1"].contains("tiltY"));
    EXPECT_EQ(got["layers"][0]["source"]["chars"]["1"]["tiltX"], 12);
}

TEST(PachislopMigrationTest, PassesV9DataUnchangedMigratingTwiceEqualsOnce) {
    const PachislopMigrationJson v9 = pachislopMigrationMigrated(withChars(9, PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltY", 20}}}}));
    EXPECT_EQ(v9["layers"][0]["source"]["chars"]["0"]["tiltY"], 20);
    const PachislopMigrationJson once = pachislopMigrationMigrated(withChars(8, PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"tiltY", 20}}}}));
    EXPECT_TRUE(same(pachislopMigrationMigrated(once), once));
}

// ==================================================================== project.test.ts: v10 -> v11 bands

TEST(PachislopMigrationTest, BackplatesBecomeBandsExactly) {
    const PachislopMigrationJson p1 = plate("p1");
    const PachislopMigrationJson p2 = plate("p2", PachislopMigrationJson{{"enabled", false}, {"grow", 2}, {"join", "miter"}, {"fillCounters", false}, {"zGap", -3},
                                     {"face", mat3("chrome", "#dddddd")}, {"side", mat3("redMetal", "#aa0000")}});
    const PachislopMigrationJson s = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"backplates", PachislopMigrationJson::array({p1, p2})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevel"]["width"], 0);
    EXPECT_EQ(s["depth"], 10);
    const auto asBand = [](const PachislopMigrationJson& p) {
        PachislopMigrationJson b = PachislopMigrationJson{{"id", p["id"]}, {"enabled", p["enabled"]}, {"grow", p["grow"]}, {"join", p["join"]}, {"fillCounters", p["fillCounters"]},
                      {"shape", p["plate"]}, {"depth", p["depth"]}, {"offset", p["zGap"]}, {"bevel", p["bevel"]}, {"material", p["face"]}};
        if (p.contains("side")) b["side"] = p["side"];
        return b;
    };
    ASSERT_EQ(s["bands"].size(), 2u);
    EXPECT_TRUE(same(s["bands"][0], asBand(p1))) << s["bands"][0].dump();
    EXPECT_TRUE(same(s["bands"][1], asBand(p2))) << s["bands"][1].dump();
}

TEST(PachislopMigrationTest, TheFirstRingInTheFacesMaterialFoldsIntoTheLetter) {
    PachislopMigrationJson goldLinked = mat3("gold", "#ffcc33");
    goldLinked["link"] = "g";
    const PachislopMigrationJson old = pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1.6, 1.6, goldLinked, "chamfer"), ring("r2", 0.3, 0.6, DARK, "step"),
                                                        ring("r3", 1, 1.2, mat3("redMetal", "#aa0000"))})},
                                 {"backplates", PachislopMigrationJson::array({plate("p1")})}});
    const PachislopMigrationJson s = pachislopMigrationMigrated(old)["layers"][0]["style3d"];
    EXPECT_TRUE(same(s["bevel"], PachislopMigrationJson{{"width", 1.6}, {"depth", 1.6}, {"profile", "chamfer"}, {"segments", 6}, {"curve", 0}}));
    EXPECT_EQ(s["bevelPosition"], "outside");
    EXPECT_NEAR(s["depth"].get<double>(), 10 + 0.6 + 1.2, 1e-9) << "the old back: 10 behind the rings' end";
    ASSERT_EQ(s["bands"].size(), 3u);
    EXPECT_EQ(s["bands"][0]["id"], "r2");
    EXPECT_EQ(s["bands"][1]["id"], "r3");
    EXPECT_EQ(s["bands"][2]["id"], "p1");
    const PachislopMigrationJson &r2 = s["bands"][0], &r3 = s["bands"][1], &p1 = s["bands"][2];
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
    const PachislopMigrationJson red = mat3("redMetal", "#aa0000");
    const PachislopMigrationJson banded = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                             {"side", red}}))["layers"][0]["style3d"];
    EXPECT_TRUE(same(banded["bands"][0]["side"], red));
    EXPECT_FALSE(banded.contains("side"));
    const PachislopMigrationJson bare = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}, {"side", red}}))["layers"][0]["style3d"];
    EXPECT_TRUE(same(bare["side"], red));
    EXPECT_TRUE(bare["bands"].empty());
}

TEST(PachislopMigrationTest, AFirstRingUnlikeTheFaceBecomesABandToo) {
    const PachislopMigrationJson s = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 0.3, 0.4, mat3("whiteGloss", "#ffffff")),
                                                               ring("r2", 1.6, 1.6, mat3("gold", "#ffcc33"))})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevel"]["width"], 0);
    ASSERT_EQ(s["bands"].size(), 2u);
    EXPECT_EQ(s["bands"][0]["id"], "r1");
    EXPECT_EQ(s["bands"][0]["grow"], 0.3);
    EXPECT_EQ(s["bands"][1]["grow"], 1.6);
    EXPECT_NEAR(s["bands"][0]["offset"].get<double>(), -s["depth"].get<double>(), 1e-9);
}

TEST(PachislopMigrationTest, InsideRingsTakeTheClosestPositionAndPlatesKeepTheirOuterEdge) {
    const PachislopMigrationJson s = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bevelPosition", "inside"},
                                        {"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                        {"backplates", PachislopMigrationJson::array({plate("p1", PachislopMigrationJson{{"grow", 2}})})}}))["layers"][0]["style3d"];
    EXPECT_EQ(s["bevelPosition"], "inside");
    EXPECT_NEAR(s["bands"][1]["grow"].get<double>(), 1.5, 1e-9);
}

TEST(PachislopMigrationTest, InInflateModeTheRingsPastTheBevelBecomeBandsSwitchedOff) {
    const PachislopMigrationJson s = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"mode", "inflate"}, {"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                        {"backplates", PachislopMigrationJson::array({plate("p1")})}}))["layers"][0]["style3d"];
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
    const auto glows = [](int n) { return PachislopMigrationJson::array({fx("outerGlow", PachislopMigrationJson{{"size", n}})}); };
    const PachislopMigrationJson old = pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                 {"backplates", PachislopMigrationJson::array({plate("p1")})}},
                            PachislopMigrationJson::object(), PachislopMigrationJson{{"face", glows(1)}, {"band:r1", glows(2)}, {"band:r2", glows(3)}, {"side", glows(4)}, {"plate:p1", glows(5)}});
    const PachislopMigrationJson got = pachislopMigrationMigrated(old)["layers"][0]["partEffects"];
    std::vector<std::string> keys;
    for (auto it = got.begin(); it != got.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, std::vector<std::string>({"band:p1", "band:r2", "face"}));
    const auto sizes = [&](const char* k) {
        std::vector<int> out;
        for (const PachislopMigrationJson& e : got[k]) out.push_back(e["params"]["size"].get<int>());
        return out;
    };
    EXPECT_EQ(sizes("face"), std::vector<int>({1, 2}));
    EXPECT_EQ(sizes("band:r2"), std::vector<int>({3, 4}));
    EXPECT_EQ(sizes("band:p1"), std::vector<int>({5}));
}

TEST(PachislopMigrationTest, LettersKeepTheirRingOverridesOnTheConvertedBands) {
    const PachislopMigrationJson white = mat3("whiteGloss", "#ffffff"), red = mat3("redMetal", "#aa0000");
    const PachislopMigrationJson chars = PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"face3d", white}, {"bands3d", PachislopMigrationJson::array({white, white})}}},
                            {"1", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"side3d", red}}}};
    const PachislopMigrationJson rings = PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)});
    const PachislopMigrationJson got = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", rings}, {"backplates", PachislopMigrationJson::array({plate("p1")})}}, chars));
    const PachislopMigrationJson& s = got["layers"][0]["style3d"];
    const PachislopMigrationJson& c = got["layers"][0]["source"]["chars"];
    EXPECT_EQ(s["bevel"]["width"], 1) << "0's first ring is its face: the ring folds";
    EXPECT_TRUE(same(c["0"]["bands3d"], PachislopMigrationJson::array({white})));
    EXPECT_FALSE(c["0"].contains("side3d"));
    EXPECT_FALSE(c["1"].contains("side3d")) << "1's own wall is inside band 0 now: it has nowhere to show";
    const PachislopMigrationJson bare = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}}, chars));
    EXPECT_TRUE(same(bare["layers"][0]["source"]["chars"]["1"]["side3d"], red)) << "without converted rings a letter keeps its own wall";
    EXPECT_FALSE(bare["layers"][0]["style3d"].contains("side")) << "the side resolves to the gold face";
}

TEST(PachislopMigrationTest, ARingOverriddenUnlikeTheFaceOnSomeLetterDoesNotFold) {
    const PachislopMigrationJson chars = PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"bands3d", PachislopMigrationJson::array({mat3("redMetal", "#aa0000")})}}}};
    const PachislopMigrationJson got = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33"))})}}, chars));
    EXPECT_EQ(got["layers"][0]["style3d"]["bevel"]["width"], 0);
    EXPECT_TRUE(same(got["layers"][0]["source"]["chars"]["0"]["bands3d"], PachislopMigrationJson::array({mat3("redMetal", "#aa0000")})));
}

TEST(PachislopMigrationTest, BandsMigratingTwiceEqualsOnce) {
    const PachislopMigrationJson once = pachislopMigrationMigrated(pachislopMigrationStored(PachislopMigrationJson{{"bands", PachislopMigrationJson::array({ring("r1", 1, 1, mat3("gold", "#ffcc33")), ring("r2", 0.5, 0.5, DARK)})},
                                           {"backplates", PachislopMigrationJson::array({plate("p1")})}, {"side", mat3("redMetal", "#aa0000")}},
                                      PachislopMigrationJson{{"0", PachislopMigrationJson{{"dx", 0}, {"dy", 0}, {"scale", 1}, {"rotate", 0}, {"bands3d", PachislopMigrationJson::array({PachislopMigrationJson(), mat3("chrome", "#dddddd")})}}}}));
    EXPECT_TRUE(same(pachislopMigrationMigrated(once), once));
}

// ==================================================================== project.test.ts: migrateEffects

TEST(PachislopMigrationTest, FillsAMissingUseGlobalLightOnlyOnEffectsThatHaveTheParam) {
    const PachislopMigrationJson shadow = fx("dropShadow", PachislopMigrationJson{{"angle", 60}});
    const PachislopMigrationJson glow = fx("outerGlow", PachislopMigrationJson{{"size", 10}});
    const PachislopMigrationJson out = migrateEffects(PachislopMigrationJson::array({shadow, glow}));
    EXPECT_TRUE(same(out[0]["params"], PachislopMigrationJson{{"angle", 60}, {"useGlobalLight", false}}));
    EXPECT_TRUE(same(out[1], glow));
    EXPECT_TRUE(same(shadow["params"], PachislopMigrationJson{{"angle", 60}}));
}

TEST(PachislopMigrationTest, TurnsAnOldTwoBlurDeepGlowIntoTheLightGlow) {
    const PachislopMigrationJson out = migrateEffects(PachislopMigrationJson::array({fx("deepGlow", PachislopMigrationJson{{"innerRadius", 10}, {"outerRadius", 120}, {"innerIntensity", 1}, {"outerIntensity", 0.5},
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
    const PachislopMigrationJson out = migrateEffects(PachislopMigrationJson::array({fx("deepGlow", PachislopMigrationJson{{"tint", "#00ff00"}, {"tintAmount", 0.3}})}))[0]["params"];
    EXPECT_EQ(out["colorMode"], "layer");
    EXPECT_EQ(out["tintAmount"], 0.3);
    EXPECT_EQ(out["tintInner"], "#00ff00");
    EXPECT_EQ(out["tintOuter"], "#00ff00");
    EXPECT_EQ(out["radius"], 240);
    EXPECT_FALSE(out.contains("color"));
}

TEST(PachislopMigrationTest, MoreOuterLightMeansALongerHazeIntensityIsCappedAt8) {
    const auto falloff = [](double outer) {
        return migrateEffects(PachislopMigrationJson::array({fx("deepGlow", PachislopMigrationJson{{"outerIntensity", outer}})}))[0]["params"]["falloff"].get<double>();
    };
    EXPECT_GT(falloff(0.2), falloff(1.5));
    EXPECT_EQ(falloff(0), 100);
    EXPECT_EQ(migrateEffects(PachislopMigrationJson::array({fx("deepGlow", PachislopMigrationJson{{"innerIntensity", 5}, {"outerIntensity", 5}, {"exposure", 3}})}))[0]["params"]["intensity"], 8);
}

TEST(PachislopMigrationTest, LeavesACurrentDeepGlowAlone) {
    const PachislopMigrationJson current = fx("deepGlow", PachislopMigrationJson{{"exposure", 1}, {"radius", 200}, {"tintAmount", 0.4}, {"tintInner", "#ffffff"}});
    EXPECT_TRUE(same(migrateEffects(PachislopMigrationJson::array({current}))[0], current));
}

TEST(PachislopMigrationTest, MigrateEffectsIsIdempotent) {
    const PachislopMigrationJson effects = PachislopMigrationJson::array({fx("deepGlow", PachislopMigrationJson{{"innerRadius", 20}, {"outerRadius", 300}, {"tint", "#ffb43c"}, {"tintAmount", 0.6}, {"threshold", 0.02}}),
                                      fx("deepGlow", PachislopMigrationJson{{"outerIntensity", 0.1}, {"tintAmount", 0.2}}), fx("dropShadow", PachislopMigrationJson{{"angle", 60}}),
                                      fx("innerShadow", PachislopMigrationJson{{"angle", 60}, {"useGlobalLight", true}}), fx("bevel"), fx("outerGlow"),
                                      fx("noSuchEffect", PachislopMigrationJson{{"innerRadius", 3}})});
    const PachislopMigrationJson once = migrateEffects(effects);
    EXPECT_TRUE(same(migrateEffects(once), once));
}

// ==================================================================== project.test.ts: parseProjectFile

namespace {

    std::string fileOf(int version, int projectVersion) {
        PachislopMigrationJson p = v4Project(projectVersion);
        // [1, 2, 3] and [250, 0] in base64
        return PachislopMigrationJson{{"format", "pslot"}, {"version", version}, {"project", p},
                    {"fonts", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "f1"}, {"name", "Font"}, {"data", "AQID"}}})},
                    {"assets", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "a1"}, {"name", "A"}, {"data", "+gA="}}})}}
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
    const PachislopMigrationJson stack = PachislopMigrationJson::array({fx("outerGlow"), fx("keyline")});
    EXPECT_TRUE(same(dedupeEffects(stack, 120, ids), stack));
}

TEST(PachislopMigrationTest, DedupeReplacesEveryRemovedTypeInPlaceAndIsIdempotent) {
    for (const char* type : {"gradientGlow", "stroke", "longShadow", "shine", "rainbow", "hueSaturation", "colorAdjust", "gradientMap"}) {
        SCOPED_TRACE(type);
        IdMint ids;
        const PachislopMigrationJson stack = PachislopMigrationJson::array({fx("dropShadow"), fx(type, PachislopMigrationJson::object(), false), fx("grain")});
        const PachislopMigrationJson out = dedupeEffects(stack, 120, ids);
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
    const PachislopMigrationJson outer = dedupeEffects(PachislopMigrationJson::array({fx("gradientGlow", PachislopMigrationJson{{"width", 20}, {"softness", 0.6}, {"opacity", 0.8}})}), 120, ids)[0];
    EXPECT_EQ(outer["type"], "outerGlow");
    const PachislopMigrationJson& p = outer["params"];
    EXPECT_EQ(p["fill"], "distance");
    EXPECT_EQ(p["technique"], "precise");
    EXPECT_EQ(p["size"], 20);
    EXPECT_NEAR(p["spread"].get<double>(), 0.4, 1e-9);
    EXPECT_EQ(p["intensity"], 1);
    EXPECT_EQ(p["opacity"], 0.8);
    EXPECT_EQ(p["mode"], "behind");
    const PachislopMigrationJson inner = dedupeEffects(PachislopMigrationJson::array({fx("gradientGlow", PachislopMigrationJson{{"inner", true}})}), 120, ids)[0];
    EXPECT_EQ(inner["type"], "innerGlow");
    EXPECT_EQ(inner["params"]["size"], 34);
    EXPECT_NEAR(inner["params"]["choke"].get<double>(), 0.25, 1e-9);
    EXPECT_EQ(inner["params"]["mode"], "normal");
}

TEST(PachislopMigrationTest, MultiStrokeBecomesKeylines) {
    IdMint ids;
    const PachislopMigrationJson first = dedupeEffects(PachislopMigrationJson::array({fx("stroke", PachislopMigrationJson{{"width1", 5}, {"width2", 0}, {"width3", 4}})}), 120, ids);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0]["params"]["width"], 9);
    EXPECT_EQ(first[0]["params"]["color"], "#2a0000");
    EXPECT_EQ(first[0]["params"]["follow"], "layer");
    EXPECT_EQ(first[1]["params"]["width"], 5);
    EXPECT_EQ(first[1]["params"]["color"], "#ffffff");
    const PachislopMigrationJson after = dedupeEffects(PachislopMigrationJson::array({fx("outerGlow"), fx("stroke", PachislopMigrationJson{{"opacity", 0.6}})}), 120, ids);
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
    const PachislopMigrationJson e = dedupeEffects(PachislopMigrationJson::array({fx("longShadow", PachislopMigrationJson{{"angle", 10}, {"useGlobalLight", true}, {"length", 300}, {"fade", 0.5}})}), 120, ids)[0];
    EXPECT_EQ(e["type"], "extrude2d");
    EXPECT_EQ(e["params"]["angle"], 60);
    EXPECT_EQ(e["params"]["length"], 300);
    EXPECT_EQ(e["params"]["fade"], 0.5);
    EXPECT_EQ(e["params"]["shading"], 0);
    EXPECT_EQ(e["params"]["opacity"], 0.55);
    EXPECT_EQ(dedupeEffects(PachislopMigrationJson::array({fx("longShadow", PachislopMigrationJson{{"angle", 10}})}), 120, ids)[0]["params"]["angle"], 10);
}

TEST(PachislopMigrationTest, ShineAndRainbowBecomeGradientOverlay) {
    IdMint ids;
    const PachislopMigrationJson shine = dedupeEffects(PachislopMigrationJson::array({fx("shine", PachislopMigrationJson{{"angle", -30}, {"width", 40}})}), 120, ids)[0];
    EXPECT_EQ(shine["type"], "gradientOverlay");
    EXPECT_EQ(shine["params"]["style"], "sweep");
    EXPECT_EQ(shine["params"]["angle"], 30);
    EXPECT_EQ(shine["params"]["length"], 200);
    EXPECT_EQ(shine["params"]["mode"], "light");
    EXPECT_NEAR(shine["params"]["amount"].get<double>(), 0.9, 1e-9);
    const PachislopMigrationJson rainbow = dedupeEffects(PachislopMigrationJson::array({fx("rainbow", PachislopMigrationJson{{"scale", 2}, {"mode", "overlay"}})}), 120, ids)[0];
    EXPECT_EQ(rainbow["params"]["style"], "linear");
    EXPECT_EQ(rainbow["params"]["scale"], 0.5);
    EXPECT_EQ(rainbow["params"]["offset"], 0.5);
    EXPECT_EQ(rainbow["params"]["wrap"], "repeat");
    EXPECT_EQ(rainbow["params"]["mode"], "overlay");
}

TEST(PachislopMigrationTest, ColorAdjustBecomesExposureLevelsLevels) {
    IdMint ids;
    const PachislopMigrationJson out = dedupeEffects(PachislopMigrationJson::array({fx("colorAdjust", PachislopMigrationJson{{"exposure", 1}, {"gamma", 0.9}, {"contrast", 0.12}})}), 120, ids);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0]["params"]["adjustment"]["type"], "exposure");
    EXPECT_EQ(out[1]["params"]["adjustment"]["type"], "levels");
    EXPECT_EQ(out[2]["params"]["adjustment"]["type"], "levels");
    const PachislopMigrationJson& gamma = out[1]["params"]["adjustment"]["channels"]["rgb"];
    EXPECT_EQ(gamma["inBlack"], 0);
    EXPECT_EQ(gamma["inWhite"], 255);
    EXPECT_EQ(gamma["gamma"], 0.9);
    const PachislopMigrationJson& line = out[2]["params"]["adjustment"]["channels"]["rgb"];
    EXPECT_NEAR(line["inBlack"].get<double>(), (0.06 / 1.12) * 255, 1e-6);
    EXPECT_NEAR(line["inWhite"].get<double>(), (1.06 / 1.12) * 255, 1e-6);
    EXPECT_EQ(line["outBlack"], 0);
    EXPECT_EQ(line["outWhite"], 255);
}

TEST(PachislopMigrationTest, DedupeRunsOverLayerAndPartEffectsOfAProject) {
    IdMint ids;
    const PachislopMigrationJson p = PachislopMigrationJson{{"comp", PachislopMigrationJson{{"globalLight", PachislopMigrationJson{{"angle", 120}}}}},
                        {"layers", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "t"}, {"kind", "title2d"}, {"effects", PachislopMigrationJson::array({fx("hueSaturation")})},
                                                     {"partEffects", PachislopMigrationJson{{"face", PachislopMigrationJson::array({fx("gradientMap")})}}}}})}};
    const PachislopMigrationJson out = effectDedupe(p, ids)["layers"][0];
    EXPECT_EQ(out["effects"][0]["type"], "adjust");
    EXPECT_EQ(out["partEffects"]["face"][0]["type"], "adjust");
}

// ==================================================================== generatorParams.test.ts

namespace {

    PachislopMigrationJson generatorLayer(const std::string& type, PachislopMigrationJson params, double scale = 1, double opacity = 1) {
        return PachislopMigrationJson{{"id", "g-" + type}, {"name", type}, {"kind", "generator"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                    {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", scale}, {"opacity", opacity}}}, {"effects", PachislopMigrationJson::array()},
                    {"masks", PachislopMigrationJson::array()}, {"generator", PachislopMigrationJson{{"type", type}, {"params", std::move(params)}}}};
    }

    PachislopMigrationJson projectOf(std::initializer_list<PachislopMigrationJson> layers) {
        return PachislopMigrationJson{{"version", 11}, {"comp", PachislopMigrationJson::object()}, {"layers", PachislopMigrationJson(layers)}};
    }

}

TEST(PachislopMigrationTest, GeneratorParamsPassesCurrentLayersThroughUnchanged) {
    IdMint ids;
    const PachislopMigrationJson p = projectOf({generatorLayer("particles", PachislopMigrationJson{{"count", 100}}), generatorLayer("speedLines", PachislopMigrationJson{{"type", "beta"}}),
                              generatorLayer("patternFill", PachislopMigrationJson{{"kind", "zebra"}})});
    EXPECT_TRUE(same(generatorParams(p, ids), p));
    const PachislopMigrationJson once = generatorParams(projectOf({generatorLayer("shockwave", PachislopMigrationJson{{"glow", 20}})}), ids);
    EXPECT_TRUE(same(generatorParams(once, ids), once));
}

TEST(PachislopMigrationTest, TurnsAGlowIntoALightOuterGlowAtTheBottomOfTheStack) {
    IdMint ids;
    PachislopMigrationJson l = generatorLayer("particles", PachislopMigrationJson{{"glow", 24}});
    l["effects"] = PachislopMigrationJson::array({PachislopMigrationJson{{"id", "mine"}, {"type", "gaussianBlur"}, {"enabled", true}, {"params", PachislopMigrationJson{{"radius", 2}}}}});
    const PachislopMigrationJson out = generatorParams(projectOf({l}), ids)["layers"][0];
    EXPECT_FALSE(out["generator"]["params"].contains("glow"));
    ASSERT_EQ(out["effects"].size(), 2u);
    EXPECT_EQ(out["effects"][0]["type"], "outerGlow");
    EXPECT_EQ(out["effects"][1]["type"], "gaussianBlur");
    EXPECT_EQ(out["effects"][0]["fromPreset"], true);
    const PachislopMigrationJson& p = out["effects"][0]["params"];
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
    const PachislopMigrationJson out = generatorParams(projectOf({generatorLayer("brushSwirl", PachislopMigrationJson{{"glow", 10}}, 2)}), ids)["layers"][0];
    EXPECT_EQ(out["effects"][0]["params"]["size"], 20);
    EXPECT_EQ(out["effects"][0]["params"]["intensity"], 0.6);
}

TEST(PachislopMigrationTest, MovesTheBetaBurstFillToASunburstLayerBelowAndItsHalftoneToEffects) {
    IdMint ids;
    const PachislopMigrationJson l = generatorLayer("speedLines", PachislopMigrationJson{{"type", "beta"}, {"opacity", 50}, {"fillBurst", true}, {"burstColor", "#ff0000"}, {"halftone", true}, {"dotSize", 6}},
                                  1, 0.8);
    const PachislopMigrationJson layers = generatorParams(projectOf({l}), ids)["layers"];
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0]["generator"]["type"], "sunburst");
    EXPECT_EQ(layers[1]["generator"]["type"], "speedLines");
    const PachislopMigrationJson &burst = layers[0], &lines = layers[1];
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
    const PachislopMigrationJson out = generatorParams(projectOf({generatorLayer("patternFill", PachislopMigrationJson{{"fade", "radial"}, {"fadeX", 10}, {"fadeY", -20}, {"fadeRadius", 300},
                                                                                   {"feather", 120}})}),
                                     ids)["layers"][0];
    const PachislopMigrationJson& m = out["masks"][0];
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
    const PachislopMigrationJson sun = generatorParams(projectOf({generatorLayer("sunburst", PachislopMigrationJson{{"vignette", 45}})}), ids)["layers"][0];
    EXPECT_FALSE(sun["generator"]["params"].contains("vignette"));
    EXPECT_EQ(sun["effects"][0]["type"], "gradientOverlay");
    EXPECT_EQ(sun["effects"][0]["fromPreset"], true);
    EXPECT_EQ(sun["effects"][0]["params"]["style"], "radial");
    EXPECT_EQ(sun["effects"][0]["params"]["mode"], "shade");
}

// ==================================================================== materialEffects.test.ts

TEST(PachislopMigrationTest, MaterialLeavesStacksWithoutTheRemovedEffectsAsTheyAre) {
    const PachislopMigrationJson stack = PachislopMigrationJson::array({fx("outerGlow"), fx("material")});
    EXPECT_TRUE(same(migrateMaterialEffects(stack), stack));
}

TEST(PachislopMigrationTest, TurnsBevelChromeGlassAndMetalTextureIntoMaterialInPlace) {
    for (const char* type : {"bevel", "chrome", "glass", "metalTexture"}) {
        SCOPED_TRACE(type);
        const PachislopMigrationJson stack = PachislopMigrationJson::array({fx("dropShadow"), fx(type, PachislopMigrationJson::object(), false), fx("grain")});
        const PachislopMigrationJson out = migrateMaterialEffects(stack);
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
    const PachislopMigrationJson b = migrateMaterialEffects(PachislopMigrationJson::array({fx("bevel", PachislopMigrationJson{{"width", 30}, {"depth", 70}, {"smooth", 3}, {"angle", 60}, {"elevation", 25},
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
    const PachislopMigrationJson round = migrateMaterialEffects(PachislopMigrationJson::array({fx("bevel")}))[0]["params"]["material"]["bevel"];
    EXPECT_EQ(round["profile"], "custom");
    EXPECT_EQ(round["profileCurve"]["points"][0]["x"], 0);
    EXPECT_EQ(round["profileCurve"]["points"][0]["y"], 0);
    EXPECT_EQ(round["profileCurve"]["points"].back()["x"], 1);
    EXPECT_EQ(round["profileCurve"]["points"].back()["y"], 1);
    EXPECT_EQ(round["useGlobalLight"], false) << "a missing flag predates the global light";
    const auto shadow = [](double s) {
        return migrateMaterialEffects(PachislopMigrationJson::array({fx("bevel", PachislopMigrationJson{{"shadow", s}})}))[0]["params"]["material"]["bevel"]["shadowOpacity"].get<double>();
    };
    EXPECT_GT(shadow(90), shadow(30));
    EXPECT_EQ(shadow(0), 0);
}

TEST(PachislopMigrationTest, ChromeBecomesItsMapAsATopToBottomFill) {
    const PachislopMigrationJson m = migrateMaterialEffects(PachislopMigrationJson::array({fx("chrome", PachislopMigrationJson{{"map", "gold"}, {"horizon", 20}, {"repeat", 2}, {"tint", "#ff0000"}, {"tintAmount", 100},
                                                                         {"distortion", 70}})}))[0]["params"]["material"];
    EXPECT_EQ(m["fill"]["kind"], "linear");
    EXPECT_EQ(m["fill"]["angle"], 90);
    EXPECT_EQ(m["fill"]["span"], "line");
    EXPECT_NEAR(m["fill"]["scale"].get<double>(), 1 / 0.85, 1e-9);
    EXPECT_NEAR(m["fill"]["offset"].get<double>(), -0.085, 1e-9);
    for (const PachislopMigrationJson& s : m["fill"]["gradient"]["stops"]) EXPECT_TRUE(std::regex_match(s["color"].get<std::string>(), std::regex("^#[0-9a-f]{2}0000$"))) << s.dump();
    EXPECT_LT(m["refraction"].get<double>(), 0);
    const PachislopMigrationJson custom = PachislopMigrationJson{{"stops", PachislopMigrationJson::array({PachislopMigrationJson{{"pos", 0}, {"color", "#123456"}}, PachislopMigrationJson{{"pos", 1}, {"color", "#abcdef"}}})}};
    EXPECT_TRUE(same(migrateMaterialEffects(PachislopMigrationJson::array({fx("chrome", PachislopMigrationJson{{"map", "custom"}, {"gradient", custom}})}))[0]["params"]["material"]["fill"]["gradient"], custom));
}

TEST(PachislopMigrationTest, GlassBecomesGlass2DRefractionPxToPercentNoShadow) {
    const PachislopMigrationJson m = migrateMaterialEffects(PachislopMigrationJson::array({fx("glass", PachislopMigrationJson{{"color", "#ff00aa"}, {"opacity", 40}, {"tint", 80}, {"edge", 50}, {"caustic", 150},
                                                                        {"refraction", 20}})}))[0]["params"]["material"];
    EXPECT_TRUE(same(m["glass"], PachislopMigrationJson{{"enabled", true}, {"color", "#ff00aa"}, {"tint", 0.8}, {"opacity", 0.4}, {"edge", 0.5}, {"caustic", 1.5}}));
    EXPECT_EQ(m["refraction"], 10);
    EXPECT_EQ(m["bevel"]["shadowOpacity"], 0);
}

TEST(PachislopMigrationTest, MetalTextureBecomesTextureKindGrainDirectionStrengthAndSheen) {
    const auto one = [](PachislopMigrationJson params) { return migrateMaterialEffects(PachislopMigrationJson::array({fx("metalTexture", std::move(params))}))[0]["params"]["material"]; };
    const PachislopMigrationJson spun = one(PachislopMigrationJson{{"mode", "radial"}, {"scale", 4}, {"angle", 30}, {"strength", 50}})["texture"];
    EXPECT_EQ(spun["kind"], "spun");
    EXPECT_EQ(spun["scale"], 2);
    EXPECT_EQ(spun["angle"], 30);
    EXPECT_EQ(spun["amount"], 0.5);
    EXPECT_EQ(spun["blend"], "overlay");
    EXPECT_EQ(one(PachislopMigrationJson{{"mode", "hairline"}, {"scale", 4}})["texture"]["kind"], "hairline");
    EXPECT_NEAR(one(PachislopMigrationJson{{"mode", "hairline"}, {"scale", 4}})["texture"]["scale"].get<double>(), 0.9, 1e-9);
    EXPECT_EQ(one(PachislopMigrationJson{{"mode", "flake"}})["texture"]["kind"], "flakes");
    EXPECT_TRUE(same(one(PachislopMigrationJson{{"sheen", false}, {"sheenPosition", 40}, {"sheenAmount", 120}, {"sheenWidth", 50}, {"sheenColor", "#ffee00"}})["sheen"],
                     PachislopMigrationJson{{"enabled", false}, {"color", "#ffee00"}, {"amount", 1.2}, {"position", 0.4}, {"width", 0.5}}));
    EXPECT_EQ(one(PachislopMigrationJson::object())["bevel"]["enabled"], false);
}

TEST(PachislopMigrationTest, MaterialRunsOverEveryLayerAndPartStackOfAProject) {
    const PachislopMigrationJson p = PachislopMigrationJson{{"layers", PachislopMigrationJson::array({PachislopMigrationJson{{"id", "t"}, {"kind", "title2d"}, {"effects", PachislopMigrationJson::array({fx("glass")})},
                                                     {"partEffects", PachislopMigrationJson{{"face", PachislopMigrationJson::array({fx("bevel")})}}}}})}};
    const PachislopMigrationJson out = materialEffects(p)["layers"][0];
    EXPECT_EQ(out["effects"][0]["type"], "material");
    EXPECT_EQ(out["partEffects"]["face"][0]["type"], "material");
}

// ==================================================================== contentEffects.test.ts

namespace {

    PachislopMigrationJson pachislopMigrationTitle(const std::string& id, const std::string& name, PachislopMigrationJson effects) {
        return PachislopMigrationJson{{"id", id}, {"name", name}, {"kind", "title2d"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                    {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", std::move(effects)},
                    {"masks", PachislopMigrationJson::array()}, {"partEffects", PachislopMigrationJson::object()}};
    }

    std::vector<std::string> pachislopMigrationShape(const PachislopMigrationJson& p) {
        std::vector<std::string> out;
        for (const PachislopMigrationJson& l : p["layers"]) {
            const std::string kind = l["kind"];
            out.push_back(kind == "generator" ? l["generator"]["type"].get<std::string>() : kind == "group" ? "[" + l["name"].get<std::string>() + "]"
                                                                                                            : l["name"].get<std::string>());
        }
        return out;
    }

    const PachislopMigrationJson* generatorOf(const PachislopMigrationJson& p, const std::string& type) {
        for (const PachislopMigrationJson& l : p["layers"]) if (l["kind"] == "generator" && l["generator"]["type"] == type) return &l;
        return nullptr;
    }

}

TEST(PachislopMigrationTest, ContentE1SparklesE2BecomesAGroupCarryingE2AndAGlintsCompanion) {
    IdMint ids;
    const PachislopMigrationJson t = pachislopMigrationTitle("t", "確定", PachislopMigrationJson::array({fx("outerGlow"), fx("sparkles", PachislopMigrationJson{{"placement", "hero"}, {"size", 52}}), fx("dropShadow")}));
    const PachislopMigrationJson out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(pachislopMigrationShape(out), std::vector<std::string>({"確定", "glints", "[確定]"}));
    const PachislopMigrationJson &layer = out["layers"][0], &glints = out["layers"][1], &group = out["layers"][2];
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
    const PachislopMigrationJson t = pachislopMigrationTitle("t", "激", PachislopMigrationJson::array({fx("fire"), fx("smoke"), fx("lightning"), fx("deepGlow"), fx("sparkles")}));
    const PachislopMigrationJson out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(pachislopMigrationShape(out), std::vector<std::string>({"smoke", "fire", "激", "lightning", "[激]", "glints", "[激]"}));
    const PachislopMigrationJson& inner = out["layers"][4];
    ASSERT_EQ(inner["effects"].size(), 1u);
    EXPECT_EQ(inner["effects"][0]["type"], "deepGlow");
    const PachislopMigrationJson* glints = generatorOf(out, "glints");
    ASSERT_NE(glints, nullptr);
    EXPECT_EQ((*glints)["companionOf"], "t");
    EXPECT_EQ((*glints)["generator"]["params"]["follow"], inner["id"]);
}

TEST(PachislopMigrationTest, KeepsAFreeLensFlareFreeHidesDisabledCompanionsBurstRaysBehindGoBelow) {
    IdMint ids;
    const PachislopMigrationJson t = pachislopMigrationTitle("t", "A", PachislopMigrationJson::array({fx("lensFlare", PachislopMigrationJson{{"x", 10}}), fx("starGlint", PachislopMigrationJson::object(), false), fx("burstRays", PachislopMigrationJson{{"placement", "behind"}})}));
    const PachislopMigrationJson out = contentEffects(projectOf({t}), ids);
    EXPECT_EQ(pachislopMigrationShape(out), std::vector<std::string>({"sunburst", "A", "lensFlare", "glints", "[A]"}));
    EXPECT_EQ((*generatorOf(out, "lensFlare"))["generator"]["params"]["follow"], "");
    EXPECT_EQ((*generatorOf(out, "glints"))["visible"], false);
    const PachislopMigrationJson& rays = (*generatorOf(out, "sunburst"))["generator"]["params"];
    EXPECT_EQ(rays["style"], "wedges");
    EXPECT_EQ(rays["onLayer"], "all");
    EXPECT_EQ(rays["centre"], "point");
    EXPECT_EQ(rays["follow"], "t");
}

TEST(PachislopMigrationTest, ParticleBurstInFrontAndBehindTwoParticlesLayers) {
    IdMint ids;
    const PachislopMigrationJson out = contentEffects(projectOf({pachislopMigrationTitle("t", "A", PachislopMigrationJson::array({fx("particleBurst", PachislopMigrationJson{{"glow", 0}})}))}), ids);
    EXPECT_EQ(pachislopMigrationShape(out), std::vector<std::string>({"particles", "A", "particles", "[A]"}));
    const PachislopMigrationJson &back = out["layers"][0], &front = out["layers"][2];
    EXPECT_EQ(back["generator"]["params"]["emitter"], "layer");
    EXPECT_EQ(back["generator"]["params"]["split"], "back");
    EXPECT_EQ(back["generator"]["params"]["travel"], 180);
    EXPECT_EQ(front["blend"], "add");
    EXPECT_EQ(front["generator"]["params"]["split"], "front");
    EXPECT_TRUE(front["effects"].empty());
}

TEST(PachislopMigrationTest, TakesOverTheLayersPlaceClippingBlendAndOpacityGoToTheGroup) {
    IdMint ids;
    const PachislopMigrationJson base = pachislopMigrationTitle("base", "base", PachislopMigrationJson::array());
    PachislopMigrationJson t = pachislopMigrationTitle("t", "A", PachislopMigrationJson::array({fx("sparkles")}));
    t["clip"] = true;
    t["blend"] = "screen";
    t["transform"]["opacity"] = 0.6;
    const PachislopMigrationJson g = PachislopMigrationJson{{"id", "g"}, {"name", "G"}, {"kind", "group"}, {"visible", true}, {"guide", false}, {"blend", "normal"}, {"clip", false},
                        {"transform", PachislopMigrationJson{{"x", 0}, {"y", 0}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", PachislopMigrationJson::array({fx("fire")})},
                        {"masks", PachislopMigrationJson::array()}, {"collapsed", false}};
    PachislopMigrationJson member = pachislopMigrationTitle("m", "m", PachislopMigrationJson::array());
    member["parentId"] = "g";
    const PachislopMigrationJson out = contentEffects(projectOf({base, t, member, g}), ids);
    const PachislopMigrationJson* wrap = nullptr;
    for (const PachislopMigrationJson& l : out["layers"]) if (l["kind"] == "group" && l["name"] == "A") wrap = &l;
    ASSERT_NE(wrap, nullptr);
    EXPECT_EQ((*wrap)["clip"], true);
    EXPECT_EQ((*wrap)["blend"], "screen");
    EXPECT_EQ((*wrap)["transform"]["opacity"], 0.6);
    for (const PachislopMigrationJson& l : out["layers"]) {
        if (l["id"] == "t") {
            EXPECT_EQ(l["clip"], false);
            EXPECT_EQ(l["blend"], "normal");
            EXPECT_EQ(l["transform"]["opacity"], 1);
        }
        if (l["id"] == "g") {
            EXPECT_TRUE(l["effects"].empty());
        }
    }
    EXPECT_EQ((*generatorOf(out, "fire"))["generator"]["params"]["follow"], "g");
}

TEST(PachislopMigrationTest, ContentIsIdempotentAndLeavesProjectsWithoutContentEffectsAlone) {
    IdMint ids;
    const PachislopMigrationJson plain = projectOf({pachislopMigrationTitle("t", "A", PachislopMigrationJson::array({fx("outerGlow")}))});
    EXPECT_TRUE(same(contentEffects(plain, ids)["layers"], plain["layers"]));
    const PachislopMigrationJson once = contentEffects(projectOf({pachislopMigrationTitle("t", "A", PachislopMigrationJson::array({fx("sparkles")}))}), ids);
    EXPECT_TRUE(same(contentEffects(once, ids)["layers"], once["layers"]));
}

TEST(PachislopMigrationTest, TurnsAParticlesLayerAlphaEmitterIntoFollowing) {
    IdMint ids;
    const PachislopMigrationJson l = generatorLayer("particles", PachislopMigrationJson{{"emitter", "layer"}, {"layer", "L1"}});
    const PachislopMigrationJson out = contentEffects(projectOf({l}), ids)["layers"][0]["generator"]["params"];
    EXPECT_EQ(out["emitter"], "layer");
    EXPECT_EQ(out["follow"], "L1");
    EXPECT_EQ(out["edgeWidth"], 6);
}
