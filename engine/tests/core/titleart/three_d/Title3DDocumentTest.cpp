// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 R1-R4, R17, R37, R38, R45, R64 and AC20, AC22, AC26, AC27: the 3D members of the document
// (defaults, unknown ids, R2's derivation, lighting inheritance, presets, rigs), the tiers'
// features and budgets (TitleTierBudgetTest), `.titlemesh` (TitleMeshFileTest) and that a
// material or rig edit never rebuilds geometry (TitleRigTest).

#include "gtest/gtest.h"

#include "titleart/three_d/Title3DTestSupport.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/three_d/Title3DSchema.h"
#include "titleart/three_d/TitleMeshFile.h"
#include "titleart/three_d/TitleTierFeatures.h"

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;
using namespace jasperengine::titleart::test;

namespace {

    TitleArtDoc docOf(Json layer, Json scene = nullptr) {
        Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"layers", Json::array({layer})}};
        if (!scene.is_null()) root["scene_3d"] = scene;
        return parseTitleArtJson(root).doc;
    }

    TitleMeshRequest requestOf(const TitleArtDoc& doc, const TestAssetResolver& assets) {
        TitleMeshRequest r;
        r.source = sourceOf(doc, 0, assets);
        r.style = decodeStyle3D(doc.layers()[0]);
        return r;
    }

}

TEST(Title3DDocumentTest, A2DDocumentWritesNoStyle) {
    const TitleArtDoc doc = makeNewTitleArt();
    const std::string text = writeTitleArt(doc);
    EXPECT_NE(text.find("\"style_3d\": null"), std::string::npos);
    EXPECT_NE(text.find("\"scene_3d\": null"), std::string::npos);
    // re-saving is byte-identical
    EXPECT_EQ(writeTitleArt(parseTitleArt(text).doc), text);
}

TEST(Title3DDocumentTest, DefaultsAndUnknownIds) {
    const TitleArtDoc doc = docOf({{"type", "text"},
                                   {"dimension", "3d"},
                                   {"style_3d", {{"face", {{"preset", "unobtainium"}}}, {"bevel", {{"profile", "wavy"}}},
                                                 {"lighting", {{"environment", {{"preset", "moon"}}}}}}}});
    const Json& s = doc.layers()[0]["style_3d"];
    EXPECT_EQ(s["face"]["preset"], "custom");
    EXPECT_EQ(s["bevel"]["profile"], "round");
    EXPECT_EQ(s["lighting"]["environment"]["preset"], "studio");
    EXPECT_EQ(s["depth"], 14);
    EXPECT_EQ(s["curve_segments"], 8);
    EXPECT_EQ(s["glint"]["tint"], "#fff4dc");
    // a face given in part takes the material's own defaults for the rest
    EXPECT_EQ(s["face"]["color"], "#ffffff");
    // without one, the plain face
    const TitleArtDoc plain = docOf({{"type", "text"}, {"dimension", "3d"}, {"style_3d", Json::object()}});
    EXPECT_EQ(plain.layers()[0]["style_3d"]["face"]["color"], "#c0c0c0");
    EXPECT_DOUBLE_EQ(plain.layers()[0]["style_3d"]["face"]["env_multiplier"].get<double>(), 0.5);
    // the lighting override is sparse: only what was set
    EXPECT_FALSE(s["lighting"].contains("tone_mapping"));
    EXPECT_EQ(s["lighting"]["environment"].size(), 1u);
    // a warning names each unknown id
    Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION},
                     {"layers", Json::array({{{"type", "text"}, {"style_3d", {{"face", {{"preset", "unobtainium"}}}}}}})}};
    const TitleArtParseResult parsed = parseTitleArtJson(root);
    bool named = false;
    for (const TitleWarning& w : parsed.warnings) named = named || w.message.find("unobtainium") != std::string::npos;
    EXPECT_TRUE(named);
}

TEST(Title3DDocumentTest, FirstSwitchDerivesFromThe2DStyle) {
    Json layer = {{"type", "text"},
                  {"style", {{"face", {{"fill", {{"kind", "solid"}, {"color", "#ff0000"}}}}},
                             {"rims", Json::array({{{"width", 4}, {"join", "bevel"}, {"material", {{"fill", {{"color", "#00ff00"}}}}}},
                                                   {{"width", 2}, {"fill_counters", true}}})}}}};
    const TitleArtDoc doc = docOf(layer);
    const Json derived = deriveStyle3D(doc.layers()[0]);
    EXPECT_EQ(derived["face"]["color"], "#ff0000");
    EXPECT_DOUBLE_EQ(derived["face"]["roughness"].get<double>(), 0.35);
    EXPECT_DOUBLE_EQ(derived["face"]["clearcoat"].get<double>(), 0.5);
    ASSERT_EQ(derived["bands"].size(), 2u);
    EXPECT_DOUBLE_EQ(derived["bands"][0]["grow"].get<double>(), 4);
    EXPECT_EQ(derived["bands"][0]["join"], "miter");     // S80's bevel join maps to miter
    EXPECT_EQ(derived["bands"][0]["material"]["color"], "#00ff00");
    EXPECT_TRUE(derived["bands"][1]["fill_counters"].get<bool>());
    EXPECT_EQ(derived["side"]["color"], "#800000");
    // a default 2D style gets the plain 3D style
    const TitleArtDoc plain = docOf({{"type", "text"}});
    EXPECT_EQ(deriveStyle3D(plain.layers()[0]), plainStyle3D());
    // a 3D layer without style_3d decodes as the derivation
    Json threeD = layer;
    threeD["dimension"] = "3d";
    const Title3D decoded = decodeStyle3D(docOf(threeD).layers()[0]);
    EXPECT_TRUE(decoded.derived);
    EXPECT_EQ(decoded.bands.size(), 2u);
}

TEST(Title3DDocumentTest, LightingInheritsValueByValue) {
    const TitleArtDoc doc = docOf({{"type", "text"},
                                   {"dimension", "3d"},
                                   {"style_3d", {{"lighting", {{"environment", {{"rotation", 30}}}, {"tone_mapping", "punch"}}},
                                                 {"face", {{"environment", {{"preset", "city"}}}}}}}},
                                  {{"environment", {{"preset", "strips"}, {"intensity", 2}}}, {"exposure", 1.5}});
    const Scene3D scene = decodeScene3D(doc.root);
    const Title3D style = decodeStyle3D(doc.layers()[0]);
    const Lighting3D layer = resolveLighting(scene, style);
    EXPECT_EQ(layer.environment.preset, "strips");
    EXPECT_DOUBLE_EQ(layer.environment.intensity, 2);
    EXPECT_DOUBLE_EQ(layer.environment.rotation, 30);
    EXPECT_EQ(layer.toneMapping, ToneMapping3D::Punch);
    EXPECT_DOUBLE_EQ(layer.exposure, 1.5);
    const Lighting3D part = resolveLighting(scene, style, &style.face);
    EXPECT_EQ(part.environment.preset, "city");
    for (const auto& [key, level] : part.sources) {
        if (key == "preset") { EXPECT_EQ(level, LightingLevel::Part); }
        if (key == "rotation") { EXPECT_EQ(level, LightingLevel::Layer); }
        if (key == "intensity") { EXPECT_EQ(level, LightingLevel::Document); }
    }
    // the default rig: one directional light and an ambient one
    EXPECT_EQ(scene.lights.size(), 2u);
    EXPECT_EQ(countNonAmbientLights(scene.lights), 1);
}

TEST(Title3DDocumentTest, PresetsSetTheirMembersAndKeepTheRest) {
    Json m = {{"roughness", 0.9}, {"opacity", 0.5}};
    ASSERT_TRUE(applyMaterialPreset(m, "gold"));
    EXPECT_EQ(m["preset"], "gold");
    EXPECT_DOUBLE_EQ(m["roughness"].get<double>(), 0.06);
    EXPECT_DOUBLE_EQ(m["opacity"].get<double>(), 0.5);
    EXPECT_EQ(m["gradient"]["stops"].size(), 13u);
    EXPECT_FALSE(applyMaterialPreset(m, "unobtainium"));
    EXPECT_EQ(title3DMaterialPresetNames().size(), 23u);
    for (const std::string& p : title3DMaterialPresetNames()) {
        const Json full = materialPreset(p);
        EXPECT_EQ(full["preset"], p);
    }
}

// ---- tiers (AC22) ----

TEST(TitleTierBudgetTest, FeaturesStrippedPerTier) {
    TitleMaterial3D gem;
    gem.transmission = 1;
    gem.dispersion = 0.5;
    gem.iridescence = 0.3;
    gem.surface = SurfaceKind3D::Crumple;
    Title3D style;
    style.ao.enabled = true;
    style.glint.enabled = true;
    namespace B = TitleLitBits;
    const TitleTierFeatures bake = titleTierFeatures(gem, style, TitleTier::Bake);
    EXPECT_TRUE(bake.litBits & B::DISPERSION);
    EXPECT_TRUE(bake.behind);
    EXPECT_TRUE(bake.ao);
    EXPECT_EQ(bake.glint, GlintMode::Full);
    const TitleTierFeatures medium = titleTierFeatures(gem, style, TitleTier::Medium);
    EXPECT_FALSE(medium.litBits & B::DISPERSION);
    EXPECT_FALSE(medium.behind);
    EXPECT_FALSE(medium.ao);
    EXPECT_TRUE(medium.litBits & B::SURFACE_LITE);
    EXPECT_TRUE(medium.litBits & B::GEM);
    EXPECT_EQ(medium.glint, GlintMode::Mobile);
    const TitleTierFeatures low = titleTierFeatures(gem, style, TitleTier::Low);
    EXPECT_FALSE(low.litBits & B::IRIDESCENCE);
    EXPECT_FALSE(low.litBits & B::GEM);
    EXPECT_TRUE(low.litBits & B::GLASS_DEPTH);
    EXPECT_FALSE(low.partRenders);
    EXPECT_EQ(low.glint, GlintMode::Sprites);
}

TEST(TitleTierBudgetTest, TiersMapOntoDeviceTiersAndBackends) {
    EXPECT_EQ(titleTierForDevice(DeviceTier::Ultra), TitleTier::High);
    EXPECT_EQ(titleTierForDevice(DeviceTier::Low), TitleTier::Low);
    EXPECT_EQ(defaultTitleTier(1), TitleTier::Medium);   // iOS
    EXPECT_EQ(defaultTitleTier(3), TitleTier::Low);      // Android
    EXPECT_EQ(defaultTitleTier(5), TitleTier::High);     // Windows
    EXPECT_EQ(clampTitleTierForBackend(TitleTier::High, true), TitleTier::Medium);
    EXPECT_EQ(resolveTitleTier("low", TitleTier::High, true, DeviceTier::High, false), TitleTier::Low);
    EXPECT_EQ(resolveTitleTier("auto", TitleTier::High, true, DeviceTier::Medium, false), TitleTier::Medium);
    EXPECT_EQ(resolveTitleTier("auto", TitleTier::High, false, DeviceTier::Low, true), TitleTier::Medium);
}

TEST(TitleTierBudgetTest, ReductionsAndFit) {
    EXPECT_EQ(titleTierCurveSegments(10, TitleTier::Medium, false), 8);
    EXPECT_EQ(titleTierCurveSegments(10, TitleTier::Low, false), 5);
    EXPECT_EQ(titleTierCurveSegments(6, TitleTier::Low, false), 4);
    EXPECT_EQ(titleTierCurveSegments(10, TitleTier::Bake, true), 5);
    TitleMeshStats stats;
    stats.triangles = 45210;
    stats.solids = 2;
    const TitleTierFit low = titleTierFit(stats, 1000, TitleTier::Low);
    EXPECT_FALSE(low.fits);
    ASSERT_EQ(low.over.size(), 1u);
    EXPECT_EQ(low.over[0], "triangles 45210 > 30000");
    EXPECT_TRUE(titleTierFit(stats, 1000, TitleTier::High).fits);
}

TEST(TitleTierBudgetTest, BigBonusMeshesFitTheirTiers) {
    TestAssetResolver assets;
    Json bands = Json::array({{{"grow", 2}, {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "chamfer"}}}},
                              {{"grow", 2}, {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "step"}}}},
                              {{"grow", 4}}});
    const TitleArtDoc doc = docOf(textLayer3D("BIG BONUS", DELA_GOTHIC, 200,
                                              {{"face", {{"preset", "gold"}}}, {"bevel", {{"width", 2.5}, {"depth", 2.5}}}, {"bands", bands}}));
    for (TitleTier tier : {TitleTier::High, TitleTier::Medium, TitleTier::Low}) {
        TitleMesher mesher;
        TitleMeshRequest r = requestOf(doc, assets);
        r.tier = tier;
        const auto mesh = mesher.build(r);
        const TitleTierLimits l = titleTierLimits(tier);
        EXPECT_LE(mesh->stats.triangles, l.maxTriangles) << titleTierName(tier);
        EXPECT_LE(mesh->bytes(), l.meshBytes) << titleTierName(tier);
    }
}

// ---- .titlemesh (AC26) ----

TEST(TitleMeshFileTest, RoundTripIsByteIdenticalAndNewerIsRefused) {
    TestAssetResolver assets;
    const TitleArtDoc doc = docOf(textLayer3D("Ab", DELA_GOTHIC, 200, {{"bevel", {{"width", 2}, {"depth", 2}}}}));
    TitleMesher mesher;
    const auto mesh = mesher.build(requestOf(doc, assets));
    const std::vector<uint8_t> bytes = writeTitleMesh(*mesh);
    std::string error;
    const auto back = readTitleMesh(bytes, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(writeTitleMesh(*back), bytes);
    EXPECT_EQ(back->stats.triangles, mesh->stats.triangles);
    std::vector<uint8_t> newer = bytes;
    newer[4] = 99;
    EXPECT_FALSE(readTitleMesh(newer, &error));
    EXPECT_EQ(error.rfind("unknown_version", 0), 0u);
}

// ---- rigs and edits that never remesh (AC20) ----

TEST(TitleRigTest, TiltsAndMaterialsNeverRebuildGeometry) {
    TestAssetResolver assets;
    Json layer = textLayer3D("AB", DELA_GOTHIC, 200, {{"bands", Json::array({{{"grow", 2}}})}});
    layer["chars"] = {{"1", {{"rig", true}}}};
    const TitleArtDoc doc = docOf(layer);
    const TitleMeshRequest base = requestOf(doc, assets);
    const uint64_t key = titleGeometryKey(base);
    // a tilt of the rigged letter, a material and a lighting edit: the same geometry key
    Json tilted = doc.layers()[0];
    tilted["chars"]["1"]["tilt_x"] = 25;
    tilted["chars"]["1"]["dz"] = 10;
    tilted["style_3d"]["face"]["roughness"] = 0.9;
    tilted["style_3d"]["lighting"]["environment"]["rotation"] = 45;
    const TitleArtDoc edited = docOf(tilted);
    EXPECT_EQ(titleGeometryKey(requestOf(edited, assets)), key);
    // the rigged letter is a part of its own whose band stack carries its rig
    TitleMesher mesher;
    const auto mesh = mesher.build(base);
    ASSERT_EQ(mesh->rigs.size(), 1u);
    EXPECT_EQ(mesh->rigs[0].charIndex, 1u);
    bool rigBand = false;
    for (const TitleMeshSlot& slot : mesh->slots) {
        if (slot.role != TitleSlotRole::BandFront) continue;
        for (uint32_t i = 0; i < slot.indexCount; i++) {
            const TitleMeshVertex& v = mesh->vertices[mesh->index(slot.firstIndex + i)];
            rigBand = rigBand || mesh->parts[v.ids[2]].rig == 1;
        }
    }
    EXPECT_TRUE(rigBand);
    // the cache serves the edited request without a build
    mesher.build(requestOf(edited, assets));
    EXPECT_EQ(mesher.meshBuilds(), 1u);
    // a geometry edit rebuilds
    Json deeper = doc.layers()[0];
    deeper["style_3d"]["depth"] = 30;
    mesher.build(requestOf(docOf(deeper), assets));
    EXPECT_EQ(mesher.meshBuilds(), 2u);
}
