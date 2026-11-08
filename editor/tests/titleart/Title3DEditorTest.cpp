// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 AC25, AC30 (unit level) and R59, R61: the 3D ops through TitleArtOps (set_dimension's R2
// derivation in one step, the band ops and their errors, apply_material, fit_to_tier), the 3d_*
// checks, and the GLB writer's round trip through tinygltf (TitleGlbWriterTest).

#include <gtest/gtest.h>

#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/three_d/Title3DEditor.h"
#include "titleart/three_d/TitleGlbWriter.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/TitleMeshFile.h"
#include "titleart/three_d/TitleEnvironmentCook.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "texture/Ktx2.h"
#include "texture/TextureEncoders.h"

#include "tiny_gltf.h"

#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    // a shape layer "Plate" (no font needed) with two rims and a red face
    TitleArtDoc sample() {
        Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", titleart::TITLE_ART_VERSION}};
        root["layers"] = Json::array(
            {{{"type", "shape"}, {"name", "Plate"}, {"shape", "rect"}, {"size", 200}, {"aspect", 2},
              {"style", {{"face", {{"fill", {{"kind", "solid"}, {"color", "#ff2020"}}}}},
                         {"rims", Json::array({Json{{"width", 4}, {"material", {{"fill", {{"color", "#ffd000"}}}}}}, Json{{"width", 2}}})}}}}});
        return titleart::parseTitleArtJson(root).doc;
    }

    TitleOpsResult applyOps(const TitleArtDoc& doc, const Json& ops) {
        registerTitle3DEditor();
        return TitleArtOps::apply(doc, ops, TitleOpsOptions{7});
    }

}

TEST(Title3DEditorTest, SetDimensionDerivesTheStyleInOneStep) {
    const TitleArtDoc doc = sample();
    const TitleOpsResult r = applyOps(doc, Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(r.undoName, "Set Dimension 3D");
    const Json& layer = r.doc.layers()[0];
    EXPECT_EQ(layer["dimension"], "3d");
    ASSERT_TRUE(layer["style_3d"].is_object());
    ASSERT_EQ(layer["style_3d"]["bands"].size(), 2u);
    EXPECT_DOUBLE_EQ(layer["style_3d"]["bands"][0]["grow"].get<double>(), 4);
    EXPECT_EQ(layer["style_3d"]["bands"][0]["material"]["color"], "#ffd000");
    EXPECT_EQ(layer["style_3d"]["face"]["color"], "#ff2020");
    EXPECT_TRUE(r.doc.root["scene_3d"].is_object());
    // back to 2D keeps the 2D style exactly
    const TitleOpsResult back = applyOps(r.doc, Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "2d"}});
    ASSERT_TRUE(back.ok) << back.error();
    EXPECT_EQ(back.doc.layers()[0]["style"], doc.layers()[0]["style"]);
    EXPECT_EQ(back.undoName, "Set Dimension 2D");
}

TEST(Title3DEditorTest, BandOpsAndErrors) {
    const TitleArtDoc doc = applyOps(sample(), Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}}).doc;
    TitleOpsResult added = applyOps(doc, Json{{"op", "add_band"}, {"layer", "Plate"}, {"preset", "plate"}, {"values", {{"depth", 9}}}});
    ASSERT_TRUE(added.ok) << added.error();
    EXPECT_EQ(added.undoName, "Add Band");
    const Json& bands = added.doc.layers()[0]["style_3d"]["bands"];
    ASSERT_EQ(bands.size(), 3u);
    EXPECT_DOUBLE_EQ(bands[2]["depth"].get<double>(), 9);
    EXPECT_EQ(bands[2]["name"], "Plate");
    const TitleOpsResult moved = applyOps(added.doc, Json{{"op", "move_band"}, {"layer", "Plate"}, {"band", "band#2"}, {"index", 0}});
    ASSERT_TRUE(moved.ok) << moved.error();
    EXPECT_EQ(moved.undoName, "Move Band");
    EXPECT_EQ(moved.doc.layers()[0]["style_3d"]["bands"][0]["name"], "Plate");
    const TitleOpsResult removed = applyOps(moved.doc, Json{{"op", "remove_band"}, {"layer", "Plate"}, {"band", "band#0"}});
    ASSERT_TRUE(removed.ok) << removed.error();
    EXPECT_EQ(removed.doc.layers()[0]["style_3d"]["bands"].size(), 2u);
    // errors list the valid values
    const TitleOpsResult badBand = applyOps(doc, Json{{"op", "remove_band"}, {"layer", "Plate"}, {"band", "band#9"}});
    ASSERT_FALSE(badBand.ok);
    ASSERT_FALSE(badBand.problems.empty());
    EXPECT_FALSE(badBand.problems[0].validHere.empty());
    const TitleOpsResult badPreset = applyOps(doc, Json{{"op", "add_band"}, {"layer", "Plate"}, {"preset", "neon_halo"}});
    ASSERT_FALSE(badPreset.ok);
    EXPECT_EQ(badPreset.problems[0].validHere, title3DBandPresetNames());
}

TEST(Title3DEditorTest, ApplyMaterialAndFitToTier) {
    const TitleArtDoc doc = applyOps(sample(), Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}}).doc;
    const TitleOpsResult gold = applyOps(doc, Json{{"op", "apply_material"}, {"layer", "Plate"}, {"part", "face"}, {"preset", "gold"}});
    ASSERT_TRUE(gold.ok) << gold.error();
    EXPECT_EQ(gold.undoName, "Apply Material Gold");
    EXPECT_EQ(gold.doc.layers()[0]["style_3d"]["face"]["preset"], "gold");
    const TitleOpsResult band = applyOps(doc, Json{{"op", "apply_material"}, {"layer", "Plate"}, {"part", "band#1"}, {"preset", "chrome"}});
    ASSERT_TRUE(band.ok) << band.error();
    EXPECT_EQ(band.doc.layers()[0]["style_3d"]["bands"][1]["material"]["preset"], "chrome");
    const TitleOpsResult bad = applyOps(doc, Json{{"op", "apply_material"}, {"layer", "Plate"}, {"part", "rim"}, {"preset", "gold"}});
    ASSERT_FALSE(bad.ok);
    EXPECT_NE(std::find(bad.problems[0].validHere.begin(), bad.problems[0].validHere.end(), "band#0"), bad.problems[0].validHere.end());
    // fit_to_tier reduces the curve segments and band bevels for Low
    const TitleOpsResult fit = applyOps(doc, Json{{"op", "fit_to_tier"}, {"layer", "Plate"}, {"tier", "low"}});
    ASSERT_TRUE(fit.ok) << fit.error();
    EXPECT_EQ(fit.undoName, "Fit To Tier Low");
    EXPECT_EQ(fit.doc.layers()[0]["style_3d"]["curve_segments"], 4);
}

TEST(Title3DEditorTest, ChecksReportZFightingAndBudgets) {
    registerTitle3DEditor();
    TitleArtDoc doc = applyOps(sample(), Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}}).doc;
    doc = applyOps(doc, Json{{"op", "set"}, {"layer", "Plate"}, {"values", {{"band#0.offset", -0.1}}}}).doc;
    std::vector<const CheckDef*> checks;
    std::string error;
    std::vector<std::string> valid;
    ASSERT_TRUE(TitleArtChecks::select({"3d"}, checks, error, valid)) << error;
    TitleCheckContext context(doc, "plate.titleart");
    const TitleCheckReport report = TitleArtChecks::run(checks, context);
    bool zFighting = false, lowFits = false;
    for (const TitleCheckResult& r : report.checks) {
        if (r.id == "3d_z_fighting") zFighting = r.status == TitleCheckStatus::Warn && r.fix.is_object();
        if (r.id == "3d_tier_budget.low") lowFits = r.status == TitleCheckStatus::Pass;
    }
    EXPECT_TRUE(zFighting);
    EXPECT_TRUE(lowFits);
}

TEST(TitleGlbWriterTest, RoundTripThroughTinyGltf) {
    TitleArtDoc doc = applyOps(sample(), Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}}).doc;
    doc = applyOps(doc, Json::array({Json{{"op", "apply_material"}, {"layer", "Plate"}, {"part", "face"}, {"preset", "gold"}},
                                  Json{{"op", "apply_material"}, {"layer", "Plate"}, {"part", "band#0"}, {"preset", "sapphire"}},
                                  Json{{"op", "set"}, {"layer", "Plate"}, {"values", {{"3d.face.clearcoat", 0.5}, {"3d.face.iridescence", 0.4}}}}}))
              .doc;
    TitleGlbOptions options;
    const TitleGlbResult glb = writeTitleGlb(doc, nullptr, options);
    ASSERT_TRUE(glb.ok) << glb.error;
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    ASSERT_TRUE(loader.LoadBinaryFromMemory(&model, &err, &warn, glb.bytes.data(), (unsigned)glb.bytes.size())) << err;
    // the triangle count survives
    size_t triangles = 0;
    for (const tinygltf::Mesh& m : model.meshes) {
        for (const tinygltf::Primitive& p : m.primitives) triangles += model.accessors[(size_t)p.indices].count / 3;
    }
    EXPECT_EQ(triangles, glb.triangles);
    // the extensions round trip
    bool clearcoat = false, iridescence = false, transmission = false, volume = false, ior = false, dispersion = false, ramp = false;
    for (const tinygltf::Material& m : model.materials) {
        if (m.extensions.count("KHR_materials_clearcoat")) clearcoat = std::abs(m.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").GetNumberAsDouble() - 0.5) < 1e-6;
        if (m.extensions.count("KHR_materials_iridescence")) iridescence = true;
        if (m.extensions.count("KHR_materials_transmission")) transmission = true;
        if (m.extensions.count("KHR_materials_volume")) volume = true;
        if (m.extensions.count("KHR_materials_ior")) ior = std::abs(m.extensions.at("KHR_materials_ior").Get("ior").GetNumberAsDouble() - 1.77) < 1e-6;
        if (m.extensions.count("KHR_materials_dispersion")) dispersion = true;
        if (m.pbrMetallicRoughness.baseColorTexture.index >= 0) ramp = true;
        EXPECT_TRUE(m.extras.Has("jasperengine_title"));
    }
    EXPECT_TRUE(clearcoat);
    EXPECT_TRUE(iridescence);
    EXPECT_TRUE(transmission);
    EXPECT_TRUE(volume);
    EXPECT_TRUE(ior);
    EXPECT_TRUE(dispersion);
    EXPECT_TRUE(ramp);
    // one node per 3D layer, positions in metres (a 400 x 200 px plate is 4 x 2 m)
    ASSERT_EQ(model.scenes[0].nodes.size(), 1u);
    double maxX = -1e9;
    for (const tinygltf::Mesh& m : model.meshes) {
        for (const tinygltf::Primitive& p : m.primitives) maxX = std::max(maxX, model.accessors[(size_t)p.attributes.at("POSITION")].maxValues[0]);
    }
    EXPECT_GT(maxX, 2.0 - 1e-3);
    EXPECT_LT(maxX, 2.3);
}

TEST(Title3DEditorTest, CookedMeshesReadBackAtTheirTier) {
    // R38: a 3D layer's mesh cooked at Low reads back at Low with the triangles stats_3d counts
    const TitleArtDoc doc = applyOps(sample(), Json{{"op", "set_dimension"}, {"layer", "Plate"}, {"dimension", "3d"}}).doc;
    std::map<std::string, std::vector<uint8_t>> cooked;
    std::string error;
    ASSERT_TRUE(cookTitleMeshes(doc, nullptr, titleart::TitleTier::Low, cooked, error)) << error;
    ASSERT_EQ(cooked.size(), 1u);
    const std::shared_ptr<titleart::TitleMesh> mesh = titleart::readTitleMesh(cooked.begin()->second, &error);
    ASSERT_TRUE(mesh) << error;
    EXPECT_EQ(mesh->tier, titleart::TitleTier::Low);
    const Json stats = title3DStats(doc, nullptr, titleart::TitleTier::Low);
    ASSERT_EQ(stats.size(), 1u);
    EXPECT_EQ(mesh->stats.triangles, stats[0]["triangles"].get<uint32_t>());
}

TEST(TitleEnvironmentCookTest, StudioCooksForEveryFamilyAndCaches) {
    // AC14: studio at Low (256 px faces) for desktop (BC6H), mobile and android_etc2 (RGB9E5) and
    // web (UASTC HDR): KTX2 cubes with every level and the key-values; the decoded top level is the
    // CPU twin's within the format's error; a second cook is a cache hit (no encode)
    registerEditorTextureCodecs();
    const int face = titleart::titleEnvironmentFace(titleart::Environment3D{}, 1.0, titleart::TitleTier::Low);
    const titleart::TitleEnvCube twin = titleart::buildTitleEnvironmentCpu(*titleart::titleEnvironmentPreset("studio"), nullptr, 0, face);
    for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}) {
        SCOPED_TRACE((int)family);
        std::vector<uint8_t> file;
        std::string error;
        ASSERT_TRUE(cookTitleEnvironment("studio", titleart::TitleTier::Low, family, file, error)) << error;
        ASSERT_TRUE(Ktx2::isKtx2(file));
        Ktx2Info info;
        ASSERT_TRUE(Ktx2::parseInfo(file, file.size(), info, error)) << error;
        EXPECT_EQ(info.faces, 6u);
        EXPECT_EQ((int)info.width, face);
        EXPECT_EQ((int)info.levelCount, titleart::titleEnvironmentLevels(face));
        EXPECT_EQ(info.keyValues["JE.titleart.environment"], "studio");
        EXPECT_EQ(info.keyValues["JE.logical"], "title_environment");
        // the decoded top level against the twin: the same mean radiance (3 %)
        GpuTextureFormat format;
        int width = 0, height = 0, faces = 0;
        std::vector<uint8_t> pixels;
        ASSERT_TRUE(decodeKtx2ForPreview(file, 0, format, width, height, faces, pixels, error)) << error;
        ASSERT_EQ(format, GpuTextureFormat::RGBA16F);
        double mean = 0;
        const size_t texels = pixels.size() / 8;
        for (size_t i = 0; i < texels; i++) {
            uint16_t h[4];
            std::memcpy(h, &pixels[i * 8], 8);
            mean += (halfToFloat(h[0]) + halfToFloat(h[1]) + halfToFloat(h[2])) / 3.0;
        }
        mean /= (double)texels;
        // the twin over the faces the decode gave
        const size_t twinTexels = std::min(twin.levels[0].size(), (size_t)std::max(1, faces) * face * face);
        double twinMean = 0;
        for (size_t i = 0; i < twinTexels; i++) twinMean += (twin.levels[0][i][0] + twin.levels[0][i][1] + twin.levels[0][i][2]) / 3.0;
        twinMean /= (double)twinTexels;
        EXPECT_NEAR(mean, twinMean, 0.03 * twinMean);
    }
    const TitleEnvironmentCookStats before = titleEnvironmentCookStats();
    std::vector<uint8_t> again;
    std::string error;
    ASSERT_TRUE(cookTitleEnvironment("studio", titleart::TitleTier::Low, TextureFamily::Desktop, again, error)) << error;
    const TitleEnvironmentCookStats after = titleEnvironmentCookStats();
    EXPECT_EQ(after.encodes, before.encodes);
    EXPECT_EQ(after.hits, before.hits + 1);
}

