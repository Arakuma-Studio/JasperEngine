// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Houdini VAT 3 and OpenVAT importers (vertex animation spec 4.6, 4.6.1). The fixtures are
// written here, at test time, from KNOWN ground truth (positions, normals, tangents, piece motion
// per frame) with the exporters' documented encodings: ASCII FBX meshes (every UV set, normals,
// tangents, binormals), glTF meshes (tinygltf), EXR textures (FloatImage's writeExr), 8-bit PNGs
// (stb), Unity .mat YAML, the legacy data JSON and OpenVAT's remap_info.json. Each import is
// decoded back from our payload with vat::vatTexelIndex and compared with the ground truth
// (positions within 1e-4 relative to the motion's size, normals within 1e-3).
// The SideFX tutorial exports (VAT3_Houdini/VAT_Example_Files, read only, never copied) are
// imported when present (JASPER_VAT_SIDEFX_EXAMPLES or the default path) and checked against the
// invariants of spec 4.6.1; those tests skip with a message when the folder is absent.

#include <gtest/gtest.h>
#include "stream/UpstreamParticles.h"

#include "vat/VatImportFixtures.h"

#include "vat/import/VatImport.h"

#include "util/FloatImage.h"

#include "json.hpp"
#include "stb_image_write.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;
namespace ev = jasperengine::editor::vat;
using Json = nlohmann::json;

namespace {

    using namespace vatimportfixtures;


    // every mesh vertex of a soft import against the truth (matched by rest position)
    void expectSoft(const ev::VatImportResult& r, const SoftTruth& truth, double posTol, double nrmTol, const std::string& what) {
        ASSERT_TRUE(r.ok()) << what << ": " << issues(r);
        ASSERT_EQ(r.data.mode, vat::VatMode::Vertices) << what;
        ASSERT_EQ(r.data.layout.vertices, (int)truth.rest.size()) << what;
        ASSERT_EQ(r.data.layout.samples, (int)truth.pos.size()) << what;
        double scale = 0.0;
        for (const auto& frame : truth.pos)
            for (size_t i = 0; i < frame.size(); i++) scale = std::max(scale, len3(frame[i] - truth.rest[i]));
        scale = std::max(scale, 1.0);
        const ev::VatMeshData& m = r.mesh;
        for (size_t v = 0; v < m.vertexCount(); v++) {
            int point = -1;
            for (size_t i = 0; i < truth.rest.size(); i++)
                if (len3(meshPosition(m, v) - truth.rest[i]) < 1e-5) point = (int)i;
            ASSERT_GE(point, 0) << what << ": mesh vertex " << v << " is not a grid point";
            for (int f = 0; f < (int)truth.pos.size(); f++) {
                const V3 p = animated(r, v, f);
                EXPECT_LE(len3(p - truth.pos[f][point]), posTol * scale) << what << ": point " << point << " frame " << f;
                if (r.data.layout.normals && nrmTol > 0) {
                    const Q4 q = texelQuat(r, f, (int)m.vatIndex[v], 1);
                    EXPECT_LE(len3(rot(q, V3(0, 0, 1)) - truth.nrm[f][point]), nrmTol) << what << ": normal of point " << point << " frame " << f;
                }
            }
        }
    }

    ev::VatImportSpec specFor(const fs::path& path) {
        ev::VatImportSpec spec;
        spec.path = path.string();
        spec.precision = "float";
        return spec;
    }

    // ---- the SideFX tutorial exports ----

    fs::path sidefxExamples() {
        const char* env = std::getenv("JASPER_VAT_SIDEFX_EXAMPLES");
        return env && *env ? fs::path(env) : fs::path("/tmp/vat-sidefx-zip/pkg/Assets/VAT3_Houdini/VAT_Example_Files");
    }
    bool haveSidefx() {
        std::error_code ec;
        return fs::is_directory(sidefxExamples() / "Soft_body", ec);
    }
    // every vertex at every exported frame, mirrored back to Unity's axes, inside the bounds the
    // export wrote into its material (they bound the animation's frames, rounded out to 0.1;
    // upsampled samples between frames may swing past them)
    void expectInsideExportBounds(const ev::VatImportResult& r, V3 mn, V3 mx, const std::string& what, double tolerance = 0.05) {
        double worst = 0.0;
        for (int f = 0; f < r.data.layout.samples; f += r.report.upsample) {
            for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
                const V3 p = animated(r, v, f);
                const V3 u(-p.x, p.y, p.z);
                worst = std::max({worst, mn.x - u.x, u.x - mx.x, mn.y - u.y, u.y - mx.y, mn.z - u.z, u.z - mx.z});
            }
        }
        EXPECT_LT(worst, tolerance) << what << ": a vertex leaves the export's bounds by " << worst;
    }

// The SideFX exports are not in the repository (the VAT 3 tutorial project's files), so these
// tests are opt-in: DISABLED_, as the unit gate allows no skip (docs/testing.md); run them with
// JASPER_VAT_SIDEFX_EXAMPLES=... --gtest_also_run_disabled_tests --gtest_filter='VatImportSideFx.*'
#define REQUIRE_SIDEFX()                                                                                                              \
    if (!haveSidefx()) GTEST_SKIP() << "the SideFX VAT 3 tutorial exports are not at " << sidefxExamples().string()                    \
                                    << " (set JASPER_VAT_SIDEFX_EXAMPLES to VAT3_Houdini/VAT_Example_Files of the tutorial project)"

}

// ===================================================================== Houdini: soft body

TEST(VatImport, SoftFromMaterialUnity) {
    const fs::path root = freshDir("soft-mat");
    SoftOptions opt;
    const SoftTruth truth = writeSoft(root, "Wave", opt);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectSoft(r, truth, 1e-4, 1e-3, "soft from .mat");
    EXPECT_EQ(r.report.houdiniMode, ev::HoudiniMode::Soft);
    EXPECT_EQ(r.report.target, "unity");
    EXPECT_EQ(r.report.sourcePoints, 12);      // 9 grid points + 3 guard corners
    ASSERT_EQ(r.data.clips.size(), 1u);
    EXPECT_EQ(r.data.clips[0].name, "Wave");
    EXPECT_EQ(r.data.clips[0].frames, 6);
    EXPECT_FLOAT_EQ(r.data.clips[0].fps, 24.0f);
    EXPECT_EQ(r.data.clips[0].wrap, ClipWrapMode::Loop);
    EXPECT_TRUE(r.data.clips[0].interpolate);
    EXPECT_FALSE(r.data.clips[0].restNone);
    EXPECT_TRUE(r.data.layout.colors);
    // colours, as stored
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const int e = (int)r.mesh.vatIndex[v];
        const auto c = texel(r, 2, e, 2);
        int point = -1;
        for (size_t i = 0; i < truth.rest.size(); i++)
            if (len3(meshPosition(r.mesh, v) - truth.rest[i]) < 1e-5) point = (int)i;
        for (int k = 0; k < 4; k++) EXPECT_NEAR(c[k], truth.col[2][point][k], 1e-6);
    }
    // the guard triangle is gone, the cloth stays double-sided, material UVs flipped to glTF's V
    size_t triangles = 0;
    for (const auto& s : r.mesh.submeshes) triangles += s.indices.size() / 3;
    EXPECT_EQ(triangles, 8u);
    ASSERT_FALSE(r.mesh.materials.empty());
    EXPECT_TRUE(r.mesh.materials[0].doubleSided);
    ASSERT_EQ(r.mesh.uv0.size(), r.mesh.vertexCount() * 2);
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const V3 p = meshPosition(r.mesh, v);
        const int i = (int)std::lround((p.x + 0.5) / 0.5) + 3 * (int)std::lround((p.z + 0.5) / 0.5);
        EXPECT_NEAR(r.mesh.uv0[v * 2 + 1], 1.0 - (i / 3) / 2.0, 1e-6);
    }
    // recipe: the kind, the clip, a hash of every file read, no options
    ASSERT_EQ(r.data.recipe.sources.size(), 1u);
    EXPECT_EQ(r.data.recipe.sources[0].kind, "houdini_vat3");
    EXPECT_EQ(r.data.recipe.sources[0].clips, std::vector<std::string>{"Wave"});
    EXPECT_EQ(r.data.recipe.sources[0].hashes.size(), 5u);   // mesh, pos, rot, col, .mat
    for (const auto& h : r.data.recipe.sources[0].hashes) EXPECT_EQ(h.second.rfind("sha1:", 0), 0u) << h.first;
    EXPECT_EQ(r.data.recipe.importOptions, "");
    EXPECT_EQ(r.data.guid.size(), 32u);
}

TEST(VatImport, SoftWritesFilesThatReadBack) {
    const fs::path root = freshDir("soft-encode");
    writeSoft(root, "Wave", SoftOptions());
    ev::VatImportSpec spec = specFor(root / "geo" / "Wave_mesh.fbx");
    spec.precision = "auto";
    ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.data.layout.precision, vat::VatPrecision::Half);   // small offsets: half is within the tolerance
    ev::VatEncodedFiles files;
    std::string error;
    ASSERT_TRUE(ev::encodeVatImport(r, (root / "Wave.vat").string(), files, &error)) << error;
    const vat::VatParseResult parsed = vat::parseVat(files.vatText);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.data->recipe.sources[0].kind, "houdini_vat3");
    vat::VatGlbContents glb;
    ASSERT_TRUE(vat::readVatGlb(files.glbBytes, glb, &error)) << error;
    EXPECT_EQ(glb.payload.bytes, r.payload.bytes);
    EXPECT_EQ(glb.layoutHash, parsed.data->layout.layoutHash);
    // the report's JSON form
    const Json report = Json::parse(ev::vatImportReportJson(r.report));
    EXPECT_EQ(report["kind"], "houdini_vat3");
    EXPECT_EQ(report["houdini_mode"], "soft");
    EXPECT_EQ(report["mode"], "vertices");
    EXPECT_EQ(report["texture"]["format"], "rgba16f");
    EXPECT_EQ(report["clips"][0]["frames"], 6);
}

TEST(VatImport, SoftFromDataJsonUnrealTarget) {
    const fs::path root = freshDir("soft-json-unreal");
    SoftOptions opt;
    opt.metadata = "json";
    opt.target = "unreal";
    const SoftTruth truth = writeSoft(root, "Cloth", opt);
    const ev::VatImportResult r = ev::importVat(specFor(root / "data" / "Cloth_data.json"));
    expectSoft(r, truth, 1e-4, 1e-3, "soft from data json, Unreal target");
    EXPECT_EQ(r.report.target, "unreal");
    EXPECT_EQ(r.data.clips[0].frames, 6);
}

TEST(VatImport, SoftFromMeshBoundsOnly) {
    const fs::path root = freshDir("soft-bounds");
    SoftOptions opt;
    opt.metadata = "bounds";
    opt.vr = 0.9;       // inactive rows below the frames
    opt.ur = 0.8;       // inactive columns on the right
    const SoftTruth truth = writeSoft(root, "Drape", opt);
    // without the target and the frame rate the import stops with the documented fixes
    ev::VatImportResult r = ev::importVat(specFor(root));
    EXPECT_TRUE(hasError(r, "unknown_target")) << issues(r);
    EXPECT_TRUE(hasError(r, "fps_unknown")) << issues(r);
    bool fixNamed = false;
    for (const auto& e : r.errors)
        if (e.kind == "unknown_target") fixNamed = e.fix.find("re-export with Target Engine = Unity, or pass axis") != std::string::npos;
    EXPECT_TRUE(fixNamed);
    // the frame count comes from the active height / the UV rows; fps and axis from the options
    ev::VatImportSpec spec = specFor(root);
    spec.axis = "unity";
    spec.fps = 30.0f;
    r = ev::importVat(spec);
    expectSoft(r, truth, 1e-4, 1e-3, "soft from the mesh bounds");
    EXPECT_FLOAT_EQ(r.data.clips[0].fps, 30.0f);
    EXPECT_EQ(r.data.recipe.importOptions, R"({"axis":"unity","fps":30.0})");
    // Rebake: the recipe gives back the same import
    ev::VatImportSpec again;
    std::string error;
    ASSERT_TRUE(ev::importSpecFromRecipe(r.data.recipe, "", again, error)) << error;
    EXPECT_EQ(again.axis, "unity");
    EXPECT_FLOAT_EQ(again.fps, 30.0f);
    again.precision = "float";
    const ev::VatImportResult r2 = ev::importVat(again);
    ASSERT_TRUE(r2.ok()) << issues(r2);
    EXPECT_EQ(r2.payload.bytes, r.payload.bytes);
}

TEST(VatImport, SoftFloat32BoundsTrap) {
    // a bound whose float32 tenths read 81.0000038: without snapping its fraction reads ~1 (active
    // height ratio ~0) instead of 0
    for (const char* metadata : {"material", "bounds"}) {
        const fs::path root = freshDir(std::string("soft-trap-") + metadata);
        SoftOptions opt;
        opt.metadata = metadata;
        opt.float32Trap = true;
        const SoftTruth truth = writeSoft(root, "Trap", opt);
        ev::VatImportSpec spec = specFor(root);
        if (opt.metadata == "bounds") {
            spec.axis = "unity";
            spec.fps = 24;
        }
        const ev::VatImportResult r = ev::importVat(spec);
        expectSoft(r, truth, 1e-4, 1e-3, std::string("float32 trap, ") + metadata);
    }
}

TEST(VatImport, SoftLdrSplitPositions) {
    const fs::path root = freshDir("soft-ldr-split");
    SoftOptions opt;
    opt.hdr = false;
    opt.twoPositions = true;
    const SoftTruth truth = writeSoft(root, "Ldr", opt);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectSoft(r, truth, 1e-4, 1e-3, "LDR, two position textures");
}

TEST(VatImport, SoftHdrSplitPositions) {
    const fs::path root = freshDir("soft-hdr-split");
    SoftOptions opt;
    opt.twoPositions = true;
    const SoftTruth truth = writeSoft(root, "Split", opt);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectSoft(r, truth, 1e-4, 1e-3, "HDR, two position textures");
}

TEST(VatImport, SoftNodeTransformBaked) {
    // the VAT data is in the mesh's own space: the node's transform moves both into the .vat.glb's
    const fs::path root = freshDir("soft-node");
    SoftOptions opt;
    opt.nodeTransform = true;
    SoftTruth truth = writeSoft(root, "Moved", opt);
    const Q4 turn = axisAngle(V3(0, 1, 0), PI / 2);
    auto place = [&](const V3& p) { return rot(turn, p * 2.0) + V3(1, 2, 3); };
    for (V3& p : truth.rest) p = place(p);
    for (auto& frame : truth.pos)
        for (V3& p : frame) p = place(p);
    for (auto& frame : truth.nrm)
        for (V3& n : frame) n = rot(turn, n);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectSoft(r, truth, 1e-4, 1e-3, "node transform");
    EXPECT_TRUE(hasWarning(r, "node_transform_baked"));
}

TEST(VatImport, SoftCompressedNormals) {
    const fs::path root = freshDir("soft-compressed");
    SoftOptions opt;
    opt.compressed = true;
    const SoftTruth truth = writeSoft(root, "Packed", opt);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectSoft(r, truth, 1e-4, 1e-3, "compressed normals");
    // the tangent: the mesh's, made perpendicular to the decoded normal
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const Q4 q = texelQuat(r, 3, (int)r.mesh.vatIndex[v], 1);
        EXPECT_NEAR(dot3(rot(q, V3(1, 0, 0)), rot(q, V3(0, 0, 1))), 0.0, 1e-5);
    }
}

TEST(VatImport, SoftMissingRotation) {
    const fs::path root = freshDir("soft-missing-rot");
    writeSoft(root, "Wave", SoftOptions());
    fs::remove(root / "tex" / "Wave_rot.exr");
    const ev::VatImportDetection d = ev::detectVatImport(root.string());
    bool reported = false;
    for (const auto& i : d.issues) reported = reported || (i.kind == "missing_file" && i.message.find("rotation texture not found") != std::string::npos);
    EXPECT_TRUE(reported) << ev::vatImportDetectionJson(d);
    ev::VatImportResult r = ev::importVat(specFor(root));
    ASSERT_TRUE(hasError(r, "missing_file")) << issues(r);
    EXPECT_NE(r.errors[0].fix.find("pass rotation"), std::string::npos);
    ev::VatImportSpec spec = specFor(root);
    spec.normals = false;
    r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_FALSE(r.data.layout.normals);
}

TEST(VatImport, Detection) {
    const fs::path root = freshDir("detect");
    writeSoft(root, "Wave", SoftOptions());
    for (const fs::path& p : {root, root / "geo", root / "geo" / "Wave_mesh.fbx", root / "unity" / "Wave_mat.mat", root / "tex" / "Wave_pos.exr"}) {
        const ev::VatImportDetection d = ev::detectVatImport(p.string());
        ASSERT_TRUE(d.found()) << p;
        EXPECT_EQ(d.kind, ev::VatImportKind::HoudiniVat3) << p;
        EXPECT_EQ(d.mode, ev::HoudiniMode::Soft) << p;
        EXPECT_EQ(d.modeSource, "material shader") << p;
        EXPECT_EQ(d.assetName, "Wave") << p;
        EXPECT_EQ(d.frames, 6) << p;
        EXPECT_FLOAT_EQ(d.fps, 24.0f) << p;
        EXPECT_EQ(d.target, "unity") << p;
        EXPECT_EQ(fs::path(d.files.rotation).filename(), "Wave_rot.exr");
        for (const auto& i : d.issues) EXPECT_FALSE(i.error) << i.kind << ": " << i.message;
        const Json j = Json::parse(ev::vatImportDetectionJson(d));
        EXPECT_EQ(j["kind"], "houdini_vat3");
    }
    EXPECT_FALSE(ev::detectVatImport((root / "nothing").string()).found());
    // without the material: the mode from the textures and UV sets
    fs::remove(root / "unity" / "Wave_mat.mat");
    const ev::VatImportDetection d = ev::detectVatImport(root.string());
    EXPECT_EQ(d.mode, ev::HoudiniMode::Soft);
    EXPECT_EQ(d.modeSource, "textures and UV sets");
    EXPECT_EQ(d.metadataSource, "mesh bounds");
}

TEST(VatImport, InvalidSettings) {
    const fs::path root = freshDir("invalid");
    writeSoft(root, "Wave", SoftOptions());
    ev::VatImportSpec spec = specFor(root);
    spec.preset = "phone";
    spec.precision = "double";
    spec.upsample = -1;
    const ev::VatImportResult r = ev::importVat(spec);
    EXPECT_EQ(r.errors.size(), 3u) << issues(r);
    for (const auto& e : r.errors) EXPECT_EQ(e.kind, "invalid_settings");
    ev::VatImportSpec missing = specFor(root / "absent");
    EXPECT_TRUE(hasError(ev::importVat(missing), "not_found"));
}

// ===================================================================== Houdini: rigid body

namespace {

    // every payload sample of the rigid import against the exact motion (frames 0 .. N-2 and
    // their sub-frames; the last frame's own sample)
    void expectRigid(const ev::VatImportResult& r, const RigidTruth& truth, double tol, const std::string& what, int fromFrame = 0) {
        ASSERT_TRUE(r.ok()) << what << ": " << issues(r);
        const int k = r.report.upsample;
        ASSERT_EQ(r.data.layout.samples, truth.frames * k) << what;
        ASSERT_EQ(r.data.clips.size(), 1u);
        EXPECT_EQ(r.data.clips[0].frames, truth.frames * k);
        EXPECT_NEAR(r.data.clips[0].fps, truth.fps * k, 1e-4);
        const ev::VatMeshData& m = r.mesh;
        double scale = 0;
        for (int f = 0; f < truth.frames; f++)
            for (size_t i = 0; i < truth.rest.size(); i++) scale = std::max(scale, len3(truth.point((int)i, f) - truth.rest[i]));
        for (size_t v = 0; v < m.vertexCount(); v++) {
            int point = -1;
            for (size_t i = 0; i < truth.rest.size(); i++)
                if (len3(meshPosition(m, v) - truth.rest[i]) < 1e-5) point = (int)i;
            ASSERT_GE(point, 0) << what;
            for (int g = fromFrame * k; g < truth.frames * k; g++) {
                const int f = g / k;
                if (f == truth.frames - 1 && g % k != 0) continue;   // the wrap segment holds (Houdini's last angle is 0)
                const double frame = (double)g / k;
                EXPECT_LE(len3(animated(r, v, g) - truth.point(point, frame)), tol * scale) << what << ": point " << point << " sample " << g;
            }
        }
    }

}

TEST(VatImport, RigidBonesWithUpsampling) {
    const fs::path root = freshDir("rigid");
    const RigidTruth truth = writeRigid(root, "Shatter", false);
    ev::VatImportSpec spec = specFor(root);
    const ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.data.mode, vat::VatMode::Bones);
    EXPECT_EQ(r.data.layout.bones, 3);
    EXPECT_EQ(r.report.pieces, 3);
    EXPECT_EQ(r.data.layout.precision, vat::VatPrecision::Float);
    // 140 degrees per frame needs at least 3 sub-steps of <= 60 degrees
    EXPECT_GE(r.report.upsample, 3);
    EXPECT_NE(r.report.modeReason.find("upsampled"), std::string::npos);
    expectRigid(r, truth, 1e-4, "rigid bones");
    // every vertex weight 1 on its piece
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        EXPECT_FLOAT_EQ(r.mesh.weights[v * 4], 1.0f);
        EXPECT_FLOAT_EQ(r.mesh.weights[v * 4 + 1], 0.0f);
    }
    // frame 1: pieces at their pivots, unrotated: identity bones, the half-scale piece scaled about its pivot
    for (int b = 0; b < 3; b++) {
        const auto q = texel(r, 0, b, 0);
        const auto t = texel(r, 0, b, 1);
        const RigidPiece& piece = truth.pieces[b];
        const V3 expected = piece.pivot * (1.0 - piece.scale);
        EXPECT_NEAR(std::fabs(q[3]), 1.0, 1e-6);
        EXPECT_NEAR(t[0], expected.x, 1e-5);
        EXPECT_NEAR(t[1], expected.y, 1e-5);
        EXPECT_NEAR(t[2], expected.z, 1e-5);
        EXPECT_NEAR(t[3], piece.scale, 1e-6);
    }
    // the half-scale piece keeps its scale
    EXPECT_NEAR(texel(r, 2, 2, 1)[3], 0.5, 1e-6);
    // static colours from the colour texture (constant per piece)
    ASSERT_EQ(r.mesh.colors.size(), r.mesh.vertexCount() * 4);
    EXPECT_FALSE(hasWarning(r, "color_animation_ignored"));
    // consecutive rotations are in one hemisphere
    for (int b = 0; b < 3; b++)
        for (int g = 1; g < r.data.layout.samples; g++) {
            const auto a = texel(r, g - 1, b, 0), c = texel(r, g, b, 0);
            EXPECT_GE(a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3], 0.0);
        }
}

TEST(VatImport, RigidSmallestThreeIndices) {
    // the fixture's rotations use every dropped-component index
    std::set<int> indices;
    const RigidTruth truth = writeRigid(freshDir("rigid-indices"), "Idx", false);
    const Target target{"unity"};
    for (int b = 0; b < 3; b++)
        for (int f = 0; f < truth.frames; f++) {
            double rgb[3];
            int index = 0;
            encodeRotation(target.quat(truth.rotation(b, f)), rgb, index);
            indices.insert(index);
        }
    EXPECT_EQ(indices, (std::set<int>{0, 1, 2, 3}));
}

TEST(VatImport, RigidFixedUpsampleAndVertices) {
    const fs::path root = freshDir("rigid-options");
    const RigidTruth truth = writeRigid(root, "Shatter", false);
    ev::VatImportSpec spec = specFor(root);
    spec.upsample = 4;
    ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.report.upsample, 4);
    EXPECT_EQ(r.data.layout.samples, 24);
    // the samples themselves are exact (between them the GPU's lerp/nlerp approximates)
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        int point = -1;
        for (size_t i = 0; i < truth.rest.size(); i++)
            if (len3(meshPosition(r.mesh, v) - truth.rest[i]) < 1e-5) point = (int)i;
        for (int g = 0; g < 20; g++) EXPECT_LE(len3(animated(r, v, g) - truth.point(point, g / 4.0)), 1e-4) << g;
    }
    // Vertices: per-vertex positions and frames rotated with the piece
    spec.mode = "vertices";
    r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.data.mode, vat::VatMode::Vertices);
    EXPECT_TRUE(r.mesh.joints.empty());
    expectRigid(r, truth, 1e-4, "rigid as vertices");
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const V3 n(r.mesh.normals[v * 3], r.mesh.normals[v * 3 + 1], r.mesh.normals[v * 3 + 2]);
        int piece = -1;
        for (size_t i = 0; i < truth.rest.size(); i++)
            if (len3(meshPosition(r.mesh, v) - truth.rest[i]) < 1e-5) piece = truth.pointPiece[i];
        const Q4 q = texelQuat(r, 8, (int)r.mesh.vatIndex[v], 1);
        EXPECT_LE(len3(rot(q, V3(0, 0, 1)) - rot(truth.rotation(piece, 2.0), n)), 1e-3);
    }
    // Bones forced: fine with 3 pieces (over VAT_MAX_BONES, 65535, it would be bone_limit)
    spec.mode = "bones";
    EXPECT_TRUE(ev::importVat(spec).ok());
}

TEST(VatImport, RigidSmoothTrajectories) {
    const fs::path root = freshDir("rigid-smooth");
    const RigidTruth truth = writeRigid(root, "Toss", true, 8);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_GE(r.report.upsample, 2);   // the parabola between frames needs sub-steps
    // Houdini's interframe position is exact for a parabola from the second frame on (frame 1 lerps)
    expectRigid(r, truth, 1e-4, "smooth trajectories", 1);
    EXPECT_TRUE(r.mesh.colors.empty());     // the colour texture held accelerations
}

TEST(VatImport, RigidStretchByVelocityWarns) {
    const fs::path root = freshDir("rigid-stretch");
    writeRigid(root, "Shatter", false);
    std::string mat;
    {
        std::ifstream in(root / "unity" / "Shatter_mat.mat");
        std::stringstream s;
        s << in.rdbuf();
        mat = s.str();
    }
    mat.replace(mat.find("_B_stretchByVel: 0"), 18, "_B_stretchByVel: 1");
    writeText(root / "unity" / "Shatter_mat.mat", mat);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_TRUE(hasWarning(r, "stretch_by_velocity_ignored"));
}

// ===================================================================== Houdini: fluid

namespace {

    struct FluidTruth {
        std::vector<std::vector<V3>> pos, nrm;     // [frame][corner]
    };

    // 4 triangles whose corners move freely; each frame packs the corners in another order
    FluidTruth writeFluid(const fs::path& root, const std::string& asset, bool lookup16, int WP = 4) {
        FluidTruth truth;
        const Target target{"unity"};
        const int C = 12, N = 3, HP = 9, WL = 4;
        truth.pos.assign(N, {});
        truth.nrm.assign(N, {});
        Image pos(WP, HP), rotTex(WP, HP), col(WP, HP), lookup(WL, 3 * N);
        V3 lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
        for (int f = 0; f < N; f++) {
            for (int c = 0; c < C; c++) {
                const V3 p(0.3 * c - 1.0 + 0.1 * f, 0.5 * std::sin(c + f), 0.2 * (c % 3) + 0.05 * f * f);
                truth.pos[f].push_back(p);
                const Q4 r = axisAngle(V3(0.2, 1, 0.4), 0.3 * c + 0.5 * f);
                truth.nrm[f].push_back(rot(r, V3(0, 1, 0)));
                const V3 t = target.point(p);
                lo = V3(std::min(lo.x, t.x), std::min(lo.y, t.y), std::min(lo.z, t.z));
                hi = V3(std::max(hi.x, t.x), std::max(hi.y, t.y), std::max(hi.z, t.z));
                // this frame's texel of the corner
                const int slot = (c * 5 + f * 3) % C;
                const int pc = slot % WP, pr = f * 3 + slot / WP;
                pos.set(pc, pr, t.x, t.y, t.z, 1.0);
                const V3 tan = rot(r, V3(1, 0, 0));
                const Q4 qt = target.quat(fromColumns(tan, truth.nrm[f][c], cross3(tan, truth.nrm[f][c])));
                rotTex.set(pc, pr, qt.x, qt.y, qt.z, qt.w);
                col.set(pc, pr, 0.1, 0.2, 0.3, 1.0);
                const double u = (pc + 0.5) / WP, vTop = (pr + 0.5) / HP;
                if (lookup16) {
                    lookup.set(c % WL, f * 3 + c / WL, u, 0.0, vTop, 0.0);
                } else {
                    const double ur = std::floor(u * 255.0), ug = std::round((u - ur / 255.0) * 65025.0);
                    const double vr = std::floor(vTop * 255.0), vg = std::round((vTop - vr / 255.0) * 65025.0);
                    lookup.set(c % WL, f * 3 + c / WL, ur, std::min(ug, 255.0), vr, std::min(vg, 255.0));
                }
            }
        }
        // the bounds' min-X digit: >= 0.5 for the 16-bit (/ 2048) lookup, else 8-bit (/ 255)
        const HBounds bounds = flaggedBounds(lo, hi, true, 1.0, 1.0, 1.0, lookup16 ? 0.6 : 0.3);
        writeExrImage(root / "tex" / (asset + "_pos.exr"), pos, 3);
        writeExrImage(root / "tex" / (asset + "_rot.exr"), rotTex);
        writeExrImage(root / "tex" / (asset + "_col.exr"), col);
        if (lookup16) writeExrImage(root / "tex" / (asset + "_lookup.exr"), lookup);
        else writePng8(root / "tex" / (asset + "_lookup.png"), lookup, true);

        FbxMesh mesh;
        mesh.uvNames = {"uv", "uv2"};
        mesh.uvs.assign(2, {});
        for (int c = 0; c < C; c++) {
            mesh.points.push_back(truth.pos[0][c]);
            mesh.corners.push_back(c);
            mesh.uvs[0].push_back({(c % WL + 0.5) / WL, 1.0 - (c / WL + 0.5) / (3.0 * N)});
            mesh.uvs[1].push_back({0, 0});
            mesh.normals.push_back(truth.nrm[0][c]);
        }
        writeFbx(root / "geo" / (asset + "_mesh.fbx"), mesh);
        writeText(root / "unity" / (asset + "_mat.mat"),
                  unityMaterial(FLUID_GUID, bounds, N, 24, {{"_B_LOAD_COL_TEX", 1}, {"_B_uvFromRG", 0}}, "_B_LOAD_COL_TEX"));
        return truth;
    }

    void expectFluid(const ev::VatImportResult& r, const FluidTruth& truth, const std::string& what) {
        ASSERT_TRUE(r.ok()) << what << ": " << issues(r);
        EXPECT_EQ(r.data.mode, vat::VatMode::Vertices);
        ASSERT_EQ(r.data.clips.size(), 1u);
        EXPECT_TRUE(r.data.clips[0].restNone);
        EXPECT_FALSE(r.data.clips[0].interpolate);
        EXPECT_TRUE(hasWarning(r, "topology_changes"));
        EXPECT_EQ(r.data.layout.vertices, 12);
        for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
            int corner = -1;
            for (size_t c = 0; c < truth.pos[0].size(); c++)
                if (len3(meshPosition(r.mesh, v) - truth.pos[0][c]) < 1e-5) corner = (int)c;
            ASSERT_GE(corner, 0) << what;
            for (int f = 0; f < (int)truth.pos.size(); f++) {
                EXPECT_LE(len3(animated(r, v, f) - truth.pos[f][corner]), 1e-5) << what << " corner " << corner << " frame " << f;
                const Q4 q = texelQuat(r, f, (int)r.mesh.vatIndex[v], 1);
                EXPECT_LE(len3(rot(q, V3(0, 0, 1)) - truth.nrm[f][corner]), 1e-3) << what;
            }
        }
    }

}

TEST(VatImport, FluidLookup8Bit) {
    const fs::path root = freshDir("fluid8");
    const FluidTruth truth = writeFluid(root, "Splash", false);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    expectFluid(r, truth, "fluid, 8-bit lookup");
    EXPECT_TRUE(r.data.layout.colors);
}

TEST(VatImport, FluidLookup16Bit) {
    const fs::path root = freshDir("fluid16");
    const FluidTruth truth = writeFluid(root, "Splash", true);
    expectFluid(ev::importVat(specFor(root)), truth, "fluid, HDR lookup (/ 2048)");
}

TEST(VatImport, FluidLookupDepthMismatch) {
    // an 8-bit lookup read as / 2048 lands between pixels of a wide position texture: refused
    // with the numbers
    const fs::path root = freshDir("fluid-mismatch");
    writeFluid(root, "Splash", false, 256);
    ASSERT_TRUE(ev::importVat(specFor(root)).ok());
    std::string mat;
    {
        std::ifstream in(root / "unity" / "Splash_mat.mat");
        std::stringstream s;
        s << in.rdbuf();
        mat = s.str();
    }
    const size_t at = mat.find("_boundMinX: ");
    const size_t end = mat.find('\n', at);
    const double minX = std::strtod(mat.substr(at + 12, end - at - 12).c_str(), nullptr);
    mat.replace(at, end - at, "_boundMinX: " + unityFloat(minX - 0.03));   // digit .3 -> .6
    writeText(root / "unity" / "Splash_mat.mat", mat);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    EXPECT_TRUE(hasError(r, "metadata_mismatch")) << issues(r);
}

TEST(VatImport, SpritesAreRefused) {
    const fs::path root = freshDir("sprites");
    writeSoft(root, "Sparks", SoftOptions());
    std::string mat;
    {
        std::ifstream in(root / "unity" / "Sparks_mat.mat");
        std::stringstream s;
        s << in.rdbuf();
        mat = s.str();
    }
    mat.replace(mat.find(SOFT_GUID), 32, SPRITE_GUID);
    writeText(root / "unity" / "Sparks_mat.mat", mat);
    const ev::VatImportResult r = ev::importVat(specFor(root));
    ASSERT_TRUE(hasError(r, "unsupported")) << issues(r);
    EXPECT_NE(r.errors[0].message.find("particle sprites are not supported yet"), std::string::npos);
}

TEST(VatImport, TooLargeNamesTheDesktopPreset) {
    const fs::path root = freshDir("too-large");
    SoftOptions opt;
    opt.frames = 6;
    writeSoft(root, "Wave", opt);
    ev::VatImportSpec spec = specFor(root);
    spec.preset = "mobile_web";
    const ev::VatImportResult ok = ev::importVat(spec);
    ASSERT_TRUE(ok.ok()) << issues(ok);
    // a rigid import with a forced huge upsampling overflows the preset
    const fs::path rigid = freshDir("too-large-rigid");
    writeRigid(rigid, "Shatter", false, 40);
    ev::VatImportSpec big = specFor(rigid);
    big.upsample = 1024;
    big.mode = "vertices";
    const ev::VatImportResult r = ev::importVat(big);
    ASSERT_TRUE(hasError(r, "too_large")) << issues(r);
    EXPECT_NE(r.errors[0].fix.find("desktop"), std::string::npos);
}

// ===================================================================== OpenVAT

namespace {

    struct OpenVatOptions {
        std::string normals = "packed";     // "packed" | "separate" | "none"
        bool singleRow = true;              // else 4-wide blocks padded to powers of two
        bool fbx = false;                   // FBX mesh with a VAT_UV set (else glTF, VAT_UV the last TEXCOORD)
        std::string animations = "dict";    // "dict" | "list" | "none"
        bool png = false;                   // 8-bit PNG textures (else EXR)
        int frames = 4;
        bool unlitMaterial = false;         // glTF: a material with KHR_materials_unlit and foliage transmission 0.3
    };

    struct OpenVatTruth {
        std::vector<V3> rest;                       // our space (Y-up for glTF; FBX: raw Blender)
        std::vector<std::vector<V3>> pos, nrm;      // [frame][vertex], our space
    };

    // 6 vertices (two quads) bending; offsets and normals in Blender space
    OpenVatTruth writeOpenVat(const fs::path& dir, const std::string& name, const OpenVatOptions& opt) {
        OpenVatTruth truth;
        const int V = 6, F = opt.frames;
        auto toOurs = [&](const V3& b) { return opt.fbx ? b : V3(b.x, b.z, -b.y); };
        std::vector<V3> restBlender;
        for (int i = 0; i < V; i++) restBlender.push_back(V3((i % 3) * 0.5, (i / 3) * 0.7, 0.1 * i));
        std::vector<std::vector<V3>> off(F), nrmB(F);
        V3 mn(1e9, 1e9, 1e9), mx(-1e9, -1e9, -1e9);
        for (int f = 0; f < F; f++) {
            for (int i = 0; i < V; i++) {
                const V3 o = f == 0 ? V3() : V3(0.1 * std::sin(f + i), 0.05 * f * i, 0.3 * std::cos(f * 0.5 + i) - 0.3 * std::cos(i));
                off[f].push_back(o);
                nrmB[f].push_back(rot(axisAngle(V3(1, 0.3, 0), 0.25 * f + 0.2 * i), V3(0, 0, 1)));
                mn = V3(std::min(mn.x, o.x), std::min(mn.y, o.y), std::min(mn.z, o.z));
                mx = V3(std::max(mx.x, o.x), std::max(mx.y, o.y), std::max(mx.z, o.z));
            }
        }
        mx = mx + V3(0.01, 0.01, 0.01);
        mn = mn - V3(0.01, 0.01, 0.01);
        int W = 0, rows = 0;
        if (opt.singleRow) {
            W = 8;
            rows = F;
        } else {
            W = 4;
            rows = ((V + W - 1) / W) * F;
            int p = 1;
            while (p < rows) p *= 2;
            rows = p;
        }
        const int H = opt.normals == "packed" ? rows * 2 : rows;
        Image vat(W, H), vnrm(W, H);
        truth.pos.assign(F, {});
        truth.nrm.assign(F, {});
        for (int i = 0; i < V; i++) truth.rest.push_back(toOurs(restBlender[i]));
        for (int f = 0; f < F; f++) {
            for (int i = 0; i < V; i++) {
                const int col = i % W, row = (i / W) * F + f;
                double c[3] = {(off[f][i].x - mn.x) / (mx.x - mn.x), (off[f][i].y - mn.y) / (mx.y - mn.y), (off[f][i].z - mn.z) / (mx.z - mn.z)};
                double n[3] = {(nrmB[f][i].x + 1) / 2, (nrmB[f][i].y + 1) / 2, (nrmB[f][i].z + 1) / 2};
                if (opt.png) {
                    for (double& v : c) v = q8(v);
                    for (double& v : n) v = q8(v);
                }
                vat.set(col, row, c[0], c[1], c[2], 1.0);
                if (opt.normals == "packed") vat.set(col, row + H / 2, n[0], n[1], n[2], 1.0);
                if (opt.normals == "separate") vnrm.set(col, row, n[0], n[1], n[2], 1.0);
                const V3 o(mn.x + c[0] * (mx.x - mn.x), mn.y + c[1] * (mx.y - mn.y), mn.z + c[2] * (mx.z - mn.z));
                truth.pos[f].push_back(truth.rest[i] + toOurs(o));
                truth.nrm[f].push_back(unit(toOurs(V3(n[0] * 2 - 1, n[1] * 2 - 1, n[2] * 2 - 1))));
            }
        }
        const std::string ext = opt.png ? ".png" : ".exr";
        if (opt.png) writePng8(dir / (name + "_vat" + ext), vat);
        else writeExrImage(dir / (name + "_vat" + ext), vat);
        if (opt.normals == "separate") {
            if (opt.png) writePng8(dir / (name + "_vnrm" + ext), vnrm);
            else writeExrImage(dir / (name + "_vnrm" + ext), vnrm);
        }
        Json remap = {{"os-remap", {{"Min", {mn.x, mn.y, mn.z}}, {"Max", {mx.x, mx.y, mx.z}}, {"Frames", F}}}};
        if (opt.animations == "dict") {
            remap["animations"] = {{"Walk", {{"startFrame", 1}, {"endFrame", 2}, {"framerate", 24}, {"looping", true}}},
                                   {"Jump", {{"startFrame", 3}, {"endFrame", F}, {"framerate", 30}, {"looping", false}}}};
        } else if (opt.animations == "list") {
            remap["animations"] = Json::array({{{"name", "Walk"}, {"startFrame", 1}, {"endFrame", 2}, {"framerate", 24}, {"looping", true}},
                                               {{"name", "Jump"}, {"startFrame", 3}, {"endFrame", F}, {"framerate", 30}, {"looping", false}}});
        }
        writeText(dir / (name + "-remap_info.json"), remap.dump(2));

        const int tris[4][3] = {{0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}};
        if (opt.fbx) {
            FbxMesh mesh;
            mesh.points = restBlender;
            mesh.uvNames = {"UVMap", "VAT_UV"};
            mesh.uvs.assign(2, {});
            for (const auto& t : tris)
                for (int k = 0; k < 3; k++) {
                    const int i = t[k];
                    mesh.corners.push_back(i);
                    mesh.uvs[0].push_back({(i % 3) / 2.0, (i / 3) * 1.0});
                    mesh.uvs[1].push_back({(i % W + 0.5) / W, 1.0 - ((i / W) * F + 0.5) / H});
                    mesh.normals.push_back(nrmB[0][i]);
                }
            writeFbx(dir / (name + ".fbx"), mesh);
        } else {
            tinygltf::Model model;
            model.asset.version = "2.0";
            std::vector<float> positions, uv0, uv1, normals;
            std::vector<uint32_t> indices;
            for (int i = 0; i < V; i++) {
                const V3 p = truth.rest[i];
                positions.insert(positions.end(), {(float)p.x, (float)p.y, (float)p.z});
                uv0.insert(uv0.end(), {(float)((i % 3) / 2.0), (float)(i / 3)});
                uv1.insert(uv1.end(), {(float)((i % W + 0.5) / W), (float)(((i / W) * F + 0.5) / H)});
                const V3 n = truth.nrm[0][i];
                normals.insert(normals.end(), {(float)n.x, (float)n.y, (float)n.z});
            }
            for (const auto& t : tris) indices.insert(indices.end(), {(uint32_t)t[0], (uint32_t)t[1], (uint32_t)t[2]});
            tinygltf::Buffer buffer;
            auto view = [&](const void* data, size_t bytes, int target) {
                while (buffer.data.size() % 4) buffer.data.push_back(0);
                tinygltf::BufferView v;
                v.buffer = 0;
                v.byteOffset = buffer.data.size();
                v.byteLength = bytes;
                v.target = target;
                const unsigned char* b = (const unsigned char*)data;
                buffer.data.insert(buffer.data.end(), b, b + bytes);
                model.bufferViews.push_back(v);
                return (int)model.bufferViews.size() - 1;
            };
            auto accessor = [&](int bufferView, int componentType, int type, size_t count, bool minmax, const std::vector<float>* values) {
                tinygltf::Accessor a;
                a.bufferView = bufferView;
                a.componentType = componentType;
                a.type = type;
                a.count = count;
                if (minmax && values) {
                    a.minValues = {1e9, 1e9, 1e9};
                    a.maxValues = {-1e9, -1e9, -1e9};
                    for (size_t i = 0; i < values->size(); i++) {
                        a.minValues[i % 3] = std::min(a.minValues[i % 3], (double)(*values)[i]);
                        a.maxValues[i % 3] = std::max(a.maxValues[i % 3], (double)(*values)[i]);
                    }
                }
                model.accessors.push_back(a);
                return (int)model.accessors.size() - 1;
            };
            tinygltf::Primitive prim;
            prim.mode = TINYGLTF_MODE_TRIANGLES;
            prim.attributes["POSITION"] = accessor(view(positions.data(), positions.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                                   TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, V, true, &positions);
            prim.attributes["NORMAL"] = accessor(view(normals.data(), normals.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                                 TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, V, false, nullptr);
            prim.attributes["TEXCOORD_0"] = accessor(view(uv0.data(), uv0.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                                     TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, V, false, nullptr);
            prim.attributes["TEXCOORD_1"] = accessor(view(uv1.data(), uv1.size() * 4, TINYGLTF_TARGET_ARRAY_BUFFER),
                                                     TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, V, false, nullptr);
            prim.indices = accessor(view(indices.data(), indices.size() * 4, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER),
                                    TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, indices.size(), false, nullptr);
            if (opt.unlitMaterial) {
                tinygltf::Material material;
                material.name = "Glow";
                material.pbrMetallicRoughness.baseColorFactor = {0.2, 0.6, 0.9, 1.0};
                material.extensions["KHR_materials_unlit"] = tinygltf::Value(tinygltf::Value::Object());
                tinygltf::Value::Object extras;
                extras["jasperengine_foliage_transmission"] = tinygltf::Value(0.3);
                material.extras = tinygltf::Value(std::move(extras));
                model.materials.push_back(material);
                model.extensionsUsed.push_back("KHR_materials_unlit");
                prim.material = 0;
            }
            tinygltf::Mesh mesh;
            mesh.primitives.push_back(prim);
            model.meshes.push_back(mesh);
            model.buffers.push_back(buffer);
            tinygltf::Node node;
            node.mesh = 0;
            model.nodes.push_back(node);
            tinygltf::Scene scene;
            scene.nodes = {0};
            model.scenes.push_back(scene);
            model.defaultScene = 0;
            tinygltf::TinyGLTF writer;
            EXPECT_TRUE(writer.WriteGltfSceneToFile(&model, (dir / (name + ".glb")).string(), true, true, false, true));
        }
        return truth;
    }

    void expectOpenVat(const ev::VatImportResult& r, const OpenVatTruth& truth, double posTol, bool normals, const std::string& what) {
        ASSERT_TRUE(r.ok()) << what << ": " << issues(r);
        EXPECT_EQ(r.report.kind, ev::VatImportKind::OpenVat);
        ASSERT_EQ(r.data.layout.vertices, (int)truth.rest.size()) << what;
        EXPECT_EQ(r.data.layout.normals, normals) << what;
        for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
            int point = -1;
            for (size_t i = 0; i < truth.rest.size(); i++)
                if (len3(meshPosition(r.mesh, v) - truth.rest[i]) < 1e-5) point = (int)i;
            ASSERT_GE(point, 0) << what;
            for (int f = 0; f < (int)truth.pos.size(); f++) {
                EXPECT_LE(len3(animated(r, v, f) - truth.pos[f][point]), posTol) << what << " vertex " << point << " frame " << f;
                if (normals) {
                    const Q4 q = texelQuat(r, f, (int)r.mesh.vatIndex[v], 1);
                    EXPECT_LE(len3(rot(q, V3(0, 0, 1)) - truth.nrm[f][point]), 1e-3) << what << " normal " << point << " frame " << f;
                }
            }
        }
    }

}

TEST(VatImport, OpenVatPackedSingleRowGltf) {
    const fs::path dir = freshDir("openvat-packed");
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", OpenVatOptions());
    const ev::VatImportResult r = ev::importVat(specFor(dir / "Flag-remap_info.json"));
    expectOpenVat(r, truth, 1e-5, true, "packed, single row, glTF");
    // clips from the dict: 1-based scene frames made relative
    ASSERT_EQ(r.data.clips.size(), 2u);
    const vat::VatClip* walk = &r.data.clips[0];
    const vat::VatClip* jump = &r.data.clips[1];
    if (walk->name != "Walk") std::swap(walk, jump);
    EXPECT_EQ(walk->name, "Walk");
    EXPECT_EQ(walk->start, 0);
    EXPECT_EQ(walk->frames, 2);
    EXPECT_FLOAT_EQ(walk->fps, 24.0f);
    EXPECT_EQ(walk->wrap, ClipWrapMode::Loop);
    EXPECT_EQ(jump->name, "Jump");
    EXPECT_EQ(jump->start, 2);
    EXPECT_EQ(jump->frames, 2);
    EXPECT_FLOAT_EQ(jump->fps, 30.0f);
    EXPECT_EQ(jump->wrap, ClipWrapMode::Once);
    EXPECT_EQ(r.data.recipe.sources[0].kind, "openvat");
    // material UVs: TEXCOORD_0 kept, VAT_UV gone
    EXPECT_EQ(r.mesh.uv0.size(), r.mesh.vertexCount() * 2);
    EXPECT_TRUE(r.mesh.uv1.empty());
    ev::VatEncodedFiles files;
    std::string error;
    ev::VatImportResult copy = r;
    ASSERT_TRUE(ev::encodeVatImport(copy, (dir / "Flag.vat").string(), files, &error)) << error;
    // detection from every entry point
    for (const fs::path& p : {dir, dir / "Flag-remap_info.json", dir / "Flag_vat.exr", dir / "Flag.glb"}) {
        const ev::VatImportDetection d = ev::detectVatImport(p.string());
        EXPECT_EQ(d.kind, ev::VatImportKind::OpenVat) << p;
        EXPECT_EQ(d.assetName, "Flag") << p;
        EXPECT_EQ(d.frames, 4) << p;
        EXPECT_EQ(d.clips.size(), 2u) << p;
    }
}

// the source glTF's unlit (KHR_materials_unlit) and foliage transmission (the extra) reach the
// .vat.glb's material as MeshSystem reads them
TEST(VatImport, OpenVatGltfKeepsUnlitAndFoliageTransmission) {
    const fs::path dir = freshDir("openvat-unlit");
    OpenVatOptions opt;
    opt.unlitMaterial = true;
    const OpenVatTruth truth = writeOpenVat(dir, "Glow", opt);
    const ev::VatImportResult r = ev::importVat(specFor(dir / "Glow-remap_info.json"));
    expectOpenVat(r, truth, 1e-5, true, "unlit material");
    ASSERT_EQ(r.mesh.materials.size(), 1u);
    EXPECT_EQ(r.mesh.materials[0].name, "Glow");
    EXPECT_TRUE(r.mesh.materials[0].unlit);
    EXPECT_FLOAT_EQ(r.mesh.materials[0].foliageTransmission, 0.3f);
    EXPECT_FLOAT_EQ(r.mesh.materials[0].baseColor[2], 0.9f);
}

TEST(VatImport, OpenVatSeparateNormalsPowerOfTwo) {
    const fs::path dir = freshDir("openvat-separate");
    OpenVatOptions opt;
    opt.normals = "separate";
    opt.singleRow = false;
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", opt);
    expectOpenVat(ev::importVat(specFor(dir)), truth, 1e-5, true, "separate normals, power-of-two blocks");
}

TEST(VatImport, OpenVatPackedPowerOfTwo) {
    const fs::path dir = freshDir("openvat-packed-pot");
    OpenVatOptions opt;
    opt.singleRow = false;
    opt.frames = 3;
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", opt);
    expectOpenVat(ev::importVat(specFor(dir)), truth, 1e-5, true, "packed normals, power-of-two blocks");
}

TEST(VatImport, OpenVatNoNormalsListAnimations) {
    const fs::path dir = freshDir("openvat-none");
    OpenVatOptions opt;
    opt.normals = "none";
    opt.animations = "list";
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", opt);
    const ev::VatImportResult r = ev::importVat(specFor(dir));
    expectOpenVat(r, truth, 1e-5, false, "no normals, list animations");
    EXPECT_TRUE(hasWarning(r, "animations_list_form"));
    ASSERT_EQ(r.data.clips.size(), 2u);
    EXPECT_EQ(r.data.clips[0].name, "Walk");
    EXPECT_EQ(r.data.clips[1].name, "Jump");
}

TEST(VatImport, OpenVatNoAnimationsFbxPng) {
    const fs::path dir = freshDir("openvat-fbx");
    OpenVatOptions opt;
    opt.fbx = true;
    opt.png = true;
    opt.animations = "none";
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", opt);
    ev::VatImportResult r = ev::importVat(specFor(dir));
    EXPECT_TRUE(hasError(r, "fps_unknown")) << issues(r);
    ev::VatImportSpec spec = specFor(dir);
    spec.fps = 25;
    r = ev::importVat(spec);
    // 8-bit offsets: exact to the stored codes
    expectOpenVat(r, truth, 1e-5, true, "FBX VAT_UV, PNG, no animations");
    ASSERT_EQ(r.data.clips.size(), 1u);
    EXPECT_EQ(r.data.clips[0].name, "Flag");
    EXPECT_EQ(r.data.clips[0].frames, 4);
    EXPECT_FLOAT_EQ(r.data.clips[0].fps, 25.0f);
}

// ===================================================================== the SideFX tutorial exports

TEST(VatImportSideFx, DISABLED_SoftBodyCloth) {
    REQUIRE_SIDEFX();
    ev::VatImportSpec spec;
    spec.path = (sidefxExamples() / "Soft_body").string();
    spec.precision = "float";
    const ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.report.houdiniMode, ev::HoudiniMode::Soft);
    EXPECT_EQ(r.report.target, "unity");
    EXPECT_EQ(r.report.sourcePoints, 5627);         // 5,621 cloth points + the 6 guard corners
    EXPECT_EQ(r.data.layout.vertices, 5621);
    EXPECT_EQ(r.data.layout.samples, 100);
    EXPECT_FLOAT_EQ(r.data.clips[0].fps, 24.0f);
    EXPECT_TRUE(r.data.layout.colors);
    ASSERT_FALSE(r.mesh.materials.empty());
    EXPECT_TRUE(r.mesh.materials[0].doubleSided);
    // frame 1: offsets ~0, the rotation texture's frame is the mesh's normal and tangent
    double maxOffset = 0, worstNormal = 1, worstTangent = 1;
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const int e = (int)r.mesh.vatIndex[v];
        const auto o = texel(r, 0, e, 0);
        maxOffset = std::max(maxOffset, std::max({std::fabs(o[0]), std::fabs(o[1]), std::fabs(o[2])}));
        const Q4 q = texelQuat(r, 0, e, 1);
        const V3 n(r.mesh.normals[v * 3], r.mesh.normals[v * 3 + 1], r.mesh.normals[v * 3 + 2]);
        const V3 t(r.mesh.tangents[v * 4], r.mesh.tangents[v * 4 + 1], r.mesh.tangents[v * 4 + 2]);
        worstNormal = std::min(worstNormal, dot3(rot(q, V3(0, 0, 1)), unit(n)));
        worstTangent = std::min(worstTangent, dot3(rot(q, V3(1, 0, 0)), unit(t)));
    }
    EXPECT_LT(maxOffset, 1e-6);
    EXPECT_GT(worstNormal, 0.999);
    EXPECT_GT(worstTangent, 0.999);
    // the cloth falls: its lowest point at the last frame is well below frame 1's
    double y0 = 1e9, yN = 1e9;
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        y0 = std::min(y0, animated(r, v, 0).y);
        yN = std::min(yN, animated(r, v, 99).y);
    }
    EXPECT_LT(yN, y0 - 1.0);
    expectInsideExportBounds(r, V3(-8.1, -12.706, -5.4), V3(8.1, 0, 5.17), "cloth");
}

TEST(VatImportSideFx, DISABLED_RigidBodyDestruction) {
    REQUIRE_SIDEFX();
    // Auto, default preset (mobile_web): one bone per piece, two texels per bone per sample
    ev::VatImportSpec spec;
    spec.path = (sidefxExamples() / "RBD" / "geo" / "RBD_Simulation_mesh.fbx").string();
    const ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.report.houdiniMode, ev::HoudiniMode::Rigid);
    EXPECT_EQ(r.report.pieces, 519);
    EXPECT_EQ(r.report.sourceFrames, 100);
    ASSERT_EQ(r.data.mode, vat::VatMode::Bones);
    EXPECT_EQ(r.data.layout.bones, 519);
    EXPECT_FALSE(hasWarning(r, "bone_limit"));
    EXPECT_EQ(r.data.layout.precision, vat::VatPrecision::Float);
    // the payload: 519 bones x 2 texels x (100 frames x the upsampling), rows of 2048
    const int k = r.report.upsample;
    EXPECT_GE(k, 4);   // pieces turn up to 216 degrees between frames: at least 4 steps of <= 60
    EXPECT_EQ(r.data.layout.samples, 100 * k);
    const uint64_t texels = 519ull * 2ull * 100ull * (uint64_t)k;
    EXPECT_LE(texels, 2048ull * 2048ull);
    EXPECT_EQ(r.data.layout.width, 2048);
    EXPECT_EQ((uint64_t)r.data.layout.height, (texels + 2047) / 2048);
    EXPECT_EQ(r.payload.bytes.size(), (size_t)r.data.layout.width * (size_t)r.data.layout.height * 16);
    // every vertex weight 1 on its piece, joints past 255 (uint16)
    int maxJoint = 0;
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        EXPECT_FLOAT_EQ(r.mesh.weights[v * 4], 1.0f);
        maxJoint = std::max(maxJoint, (int)r.mesh.joints[v * 4]);
    }
    EXPECT_EQ(maxJoint, 518);
    // frame 1: every piece at its pivot, unrotated: identity bones
    for (int b = 0; b < 519; b++) {
        const auto q = texel(r, 0, b, 0);
        const auto t = texel(r, 0, b, 1);
        EXPECT_NEAR(std::fabs(q[3]), 1.0, 1e-6) << b;
        EXPECT_LT(std::max({std::fabs(t[0]), std::fabs(t[1]), std::fabs(t[2])}), 1e-5) << b;
        EXPECT_NEAR(t[3], 1.0, 1e-6) << b;
    }
    // the decoded rotations (each piece's bone applied to the rest mesh) keep every piece inside
    // the export's bounds: SideFX's own shader math puts a few vertices of piece 240 at frame 11
    // 0.057 under the floor (the bounds come from the simulation, which lets pieces sink a
    // little), a wrong quaternion convention throws thousands of vertices out by up to 0.25
    expectInsideExportBounds(r, V3(-3.3, -0.201, -6.1), V3(3.7, 2.3, 7.77), "destruction (bones)", 0.1);
    // the files encode (joints as uint16 in the .vat.glb)
    ev::VatImportResult copy = r;
    ev::VatEncodedFiles files;
    std::string error;
    const fs::path out = freshDir("sidefx-rbd") / "RBD.vat";
    ASSERT_TRUE(ev::encodeVatImport(copy, out.string(), files, &error)) << error;
}

TEST(VatImportSideFx, DISABLED_RigidBodyDestructionAsVertices) {
    REQUIRE_SIDEFX();
    ev::VatImportSpec spec;
    spec.path = (sidefxExamples() / "RBD" / "geo" / "RBD_Simulation_mesh.fbx").string();
    spec.mode = "vertices";
    // ~80k vertices x 100 frames x 2 texels: the desktop preset, without colours
    spec.preset = "desktop";
    spec.colors = false;
    spec.precision = "float";
    const ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.data.mode, vat::VatMode::Vertices);
    EXPECT_TRUE(r.mesh.joints.empty());
    // frame 1: every piece at its pivot, unrotated: the vertices at rest
    double maxOffset = 0;
    for (size_t v = 0; v < r.mesh.vertexCount(); v++) {
        const auto o = texel(r, 0, (int)r.mesh.vatIndex[v], 0);
        maxOffset = std::max(maxOffset, std::max({std::fabs(o[0]), std::fabs(o[1]), std::fabs(o[2])}));
    }
    EXPECT_LT(maxOffset, 1e-5);
    expectInsideExportBounds(r, V3(-3.3, -0.201, -6.1), V3(3.7, 2.3, 7.77), "destruction (vertices)", 0.1);
}

TEST(VatImportSideFx, DISABLED_FluidSplash) {
    REQUIRE_SIDEFX();
    ev::VatImportSpec spec;
    spec.path = (sidefxExamples() / "Fluid" / "unity" / "Fluid_Simulation_mat.mat").string();
    // 57,216 corners x 100 frames x 3 texels: the desktop preset only
    ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(hasError(r, "too_large")) << issues(r);
    spec.preset = "desktop";
    r = ev::importVat(spec);
    // the lookup texels must decode to pixel centres (the importer refuses otherwise)
    ASSERT_TRUE(r.ok()) << issues(r);
    EXPECT_EQ(r.report.houdiniMode, ev::HoudiniMode::Fluid);
    EXPECT_EQ(r.report.sourcePoints, 57216);
    EXPECT_EQ(r.data.layout.samples, 100);
    EXPECT_TRUE(r.data.clips[0].restNone);
    EXPECT_FALSE(r.data.clips[0].interpolate);
    expectInsideExportBounds(r, V3(-6.03, -6.928, -6.3), V3(6.1, 3.3, 5.57), "fluid");
}

TEST(VatImportSideFx, DISABLED_SpritesAreRefused) {
    REQUIRE_SIDEFX();
    ev::VatImportSpec spec;
    spec.path = (sidefxExamples() / "Sprites").string();
    const ev::VatImportResult r = ev::importVat(spec);
    ASSERT_TRUE(hasError(r, "unsupported")) << issues(r);
    EXPECT_NE(issues(r).find("particle sprites are not supported yet"), std::string::npos);
}

// ---- malformed inputs (crafted files must be refused with an error, never read out of bounds) ----

namespace {

    // A 3-vertex OpenVAT export whose mesh is a .gltf (JSON + Bad.bin) the test edits before the
    // import: POSITION (accessor 0), TEXCOORD_0 = VAT_UV (accessor 1), and when `extras` NORMAL (2),
    // TANGENT (3) and COLOR_0 (4), every one with 3 elements. A 2 x 2 PNG, 1 frame.
    struct BadGltf {
        fs::path dir;
        Json gltf;
        void write() const { writeText(dir / "Bad.gltf", gltf.dump()); }
    };

    BadGltf badGltf(const std::string& name, bool extras = false) {
        BadGltf out;
        out.dir = freshDir(name);
        std::vector<float> data = {0, 0, 0, 1, 0, 0, 0, 1, 0,                    // POSITION
                                   0.25f, 0.25f, 0.75f, 0.25f, 0.25f, 0.75f};   // TEXCOORD_0
        if (extras) {
            for (int i = 0; i < 3; i++) data.insert(data.end(), {0, 0, 1});         // NORMAL
            for (int i = 0; i < 3; i++) data.insert(data.end(), {1, 0, 0, 1});      // TANGENT
            for (int i = 0; i < 3; i++) data.insert(data.end(), {1, 1, 1, 1});      // COLOR_0
        }
        std::string bytes(data.size() * 4, '\0');
        std::memcpy(bytes.data(), data.data(), bytes.size());
        writeText(out.dir / "Bad.bin", bytes);
        Json views = Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}}, {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 24}}});
        Json accessors = Json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                                      {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}}});
        Json attributes = {{"POSITION", 0}, {"TEXCOORD_0", 1}};
        if (extras) {
            views.push_back({{"buffer", 0}, {"byteOffset", 60}, {"byteLength", 36}});
            views.push_back({{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 48}});
            views.push_back({{"buffer", 0}, {"byteOffset", 144}, {"byteLength", 48}});
            accessors.push_back({{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}});
            accessors.push_back({{"bufferView", 3}, {"componentType", 5126}, {"count", 3}, {"type", "VEC4"}});
            accessors.push_back({{"bufferView", 4}, {"componentType", 5126}, {"count", 3}, {"type", "VEC4"}});
            attributes["NORMAL"] = 2;
            attributes["TANGENT"] = 3;
            attributes["COLOR_0"] = 4;
        }
        out.gltf = {{"asset", {{"version", "2.0"}}},
                    {"buffers", Json::array({{{"byteLength", bytes.size()}, {"uri", "Bad.bin"}}})},
                    {"bufferViews", views},
                    {"accessors", accessors},
                    {"meshes", Json::array({{{"primitives", Json::array({{{"attributes", attributes}}})}}})},
                    {"nodes", Json::array({{{"mesh", 0}}})},
                    {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
                    {"scene", 0}};
        out.write();
        writeText(out.dir / "Bad-remap_info.json",
                  Json({{"os-remap", {{"Min", {0, 0, 0}}, {"Max", {1, 1, 1}}, {"Frames", 1}}},
                        {"animations", {{"A", {{"startFrame", 1}, {"endFrame", 1}, {"framerate", 24}}}}}}).dump());
        Image vat(2, 2);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 2; c++) vat.set(c, r, 0.5, 0.5, 0.5, 1.0);
        writePng8(out.dir / "Bad_vat.png", vat);
        return out;
    }

    // the import of the edited export fails, its errors naming `expected`
    void expectRefused(const BadGltf& bad, const std::string& expected, const std::string& what) {
        bad.write();
        const ev::VatImportResult r = ev::importVat(specFor(bad.dir / "Bad-remap_info.json"));
        ASSERT_FALSE(r.ok()) << what;
        EXPECT_NE(issues(r).find(expected), std::string::npos) << what << ": " << issues(r);
    }

}

TEST(VatImport, MalformedGltfBaseExportImports) {
    // the unedited fixtures are valid: every refusal below comes from the edit
    for (bool extras : {false, true}) {
        const BadGltf bad = badGltf(extras ? "bad-base-extras" : "bad-base", extras);
        const ev::VatImportResult r = ev::importVat(specFor(bad.dir / "Bad-remap_info.json"));
        ASSERT_TRUE(r.ok()) << issues(r);
        EXPECT_EQ(r.data.layout.vertices, 3);
    }
}

TEST(VatImport, MalformedGltfAccessorSizesOverflow) {
    // counts whose byte size or count * components wrap around 64 bits (heap corruption before)
    for (uint64_t count : {(uint64_t)1 << 62, ((uint64_t)1 << 63) + 1, (uint64_t)0x5555555555555556ull, (uint64_t)0xFFFFFFFFFFFFFFFFull}) {
        BadGltf bad = badGltf("bad-count");
        bad.gltf["accessors"][0]["count"] = count;
        expectRefused(bad, "", "POSITION count " + std::to_string(count));
    }
    BadGltf bad = badGltf("bad-offset");
    bad.gltf["accessors"][0]["byteOffset"] = (uint64_t)0xFFFFFFFFFFFFFFF0ull;
    expectRefused(bad, "", "accessor byteOffset near 2^64");
    bad = badGltf("bad-view-offset");
    bad.gltf["bufferViews"][0]["byteOffset"] = (uint64_t)0xFFFFFFFFFFFFFFF0ull;
    expectRefused(bad, "", "bufferView byteOffset near 2^64");
    bad = badGltf("bad-stride");
    bad.gltf["bufferViews"][0]["byteStride"] = 252;
    expectRefused(bad, "", "a stride far past the buffer");
    bad = badGltf("bad-zeros");
    bad.gltf["accessors"][1].erase("bufferView");
    bad.gltf["accessors"][1]["count"] = (uint64_t)1 << 60;
    expectRefused(bad, "", "a data-less accessor claiming 2^60 elements");
}

TEST(VatImport, MalformedGltfIndicesOutOfRange) {
    // bufferView / buffer / accessor indices past their arrays (SIGSEGV before)
    BadGltf bad = badGltf("bad-view");
    bad.gltf["accessors"][0]["bufferView"] = 50;
    expectRefused(bad, "", "POSITION bufferView 50");
    bad = badGltf("bad-view-uv");
    bad.gltf["accessors"][1]["bufferView"] = 50;
    expectRefused(bad, "", "TEXCOORD_0 bufferView 50");
    bad = badGltf("bad-buffer");
    bad.gltf["bufferViews"][1]["buffer"] = 7;
    expectRefused(bad, "", "bufferView buffer 7");
    bad = badGltf("bad-accessor");
    bad.gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"] = 9;
    expectRefused(bad, "", "TEXCOORD_0 accessor 9");
    bad = badGltf("bad-uvset");
    bad.gltf["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_99999999"] = 1;
    expectRefused(bad, "UV sets", "TEXCOORD_99999999");
}

TEST(VatImport, MalformedGltfShortAttributes) {
    // NORMAL / TANGENT / COLOR_0 / TEXCOORD with fewer elements than POSITION (read past the end before)
    for (const char* name : {"NORMAL", "TANGENT", "COLOR_0", "TEXCOORD_0"}) {
        BadGltf bad = badGltf(std::string("bad-short-") + name, true);
        const int accessor = bad.gltf["meshes"][0]["primitives"][0]["attributes"][name].get<int>();
        bad.gltf["accessors"][(size_t)accessor]["count"] = 1;
        expectRefused(bad, std::string(name) + " has 1 elements but POSITION has 3", name);
    }
}

TEST(VatImport, MalformedOpenVatFrameCounts) {
    // frame counts / numbers that would overflow the int frame math
    BadGltf bad = badGltf("bad-frames");
    writeText(bad.dir / "Bad-remap_info.json",
              Json({{"os-remap", {{"Min", {0, 0, 0}}, {"Max", {1, 1, 1}}, {"Frames", 1e300}}}}).dump());
    expectRefused(bad, "not a frame count", "Frames 1e300");
    writeText(bad.dir / "Bad-remap_info.json",
              Json({{"os-remap", {{"Min", {0, 0, 0}}, {"Max", {1, 1, 1}}, {"Frames", 2147483647}}}}).dump());
    expectRefused(bad, "", "Frames INT_MAX");
    writeText(bad.dir / "Bad-remap_info.json",
              Json({{"os-remap", {{"Min", {0, 0, 0}}, {"Max", {1, 1, 1}}, {"Frames", 1}}},
                    {"animations", {{"A", {{"startFrame", -1e300}, {"endFrame", 1e300}, {"framerate", 24}}}}}}).dump());
    const ev::VatImportResult r = ev::importVat(specFor(bad.dir / "Bad-remap_info.json"));
    EXPECT_TRUE(r.ok()) << issues(r);   // the absurd frame numbers are ignored (the clip covers the texture)
}

TEST(VatImport, OpenVatOverlappingClipsBoundsCoverEveryFrame) {
    const fs::path dir = freshDir("openvat-overlap");
    const OpenVatTruth truth = writeOpenVat(dir, "Flag", OpenVatOptions());
    // Walk plays every frame, Jump the last two: frames 3-4 belong to both
    std::ifstream in(dir / "Flag-remap_info.json");
    Json remap = Json::parse(in);
    in.close();
    remap["animations"] = {{"Walk", {{"startFrame", 1}, {"endFrame", 4}, {"framerate", 24}, {"looping", true}}},
                           {"Jump", {{"startFrame", 3}, {"endFrame", 4}, {"framerate", 30}, {"looping", false}}}};
    writeText(dir / "Flag-remap_info.json", remap.dump());
    const ev::VatImportResult r = ev::importVat(specFor(dir));
    ASSERT_TRUE(r.ok()) << issues(r);
    ASSERT_EQ(r.data.clips.size(), 2u);
    for (const vat::VatClip& clip : r.data.clips) {
        ASSERT_TRUE(clip.bounds.valid) << clip.name;
        for (int f = clip.start; f < clip.start + clip.frames; f++)
            for (const V3& p : truth.pos[(size_t)f]) {
                EXPECT_GE(p.x, clip.bounds.min.x - 1e-5) << clip.name << " frame " << f;
                EXPECT_GE(p.y, clip.bounds.min.y - 1e-5) << clip.name << " frame " << f;
                EXPECT_GE(p.z, clip.bounds.min.z - 1e-5) << clip.name << " frame " << f;
                EXPECT_LE(p.x, clip.bounds.max.x + 1e-5) << clip.name << " frame " << f;
                EXPECT_LE(p.y, clip.bounds.max.y + 1e-5) << clip.name << " frame " << f;
                EXPECT_LE(p.z, clip.bounds.max.z + 1e-5) << clip.name << " frame " << f;
            }
    }
}

TEST(VatImport, NonFiniteDecodedDataIsRefused) {
    // a NaN texel in the VAT texture: vertex 2 (column 2 of the single-row layout), frame 1
    const fs::path dir = freshDir("openvat-nan");
    writeOpenVat(dir, "Flag", OpenVatOptions());
    jasperengine::editor::FloatImage image;
    std::string error;
    ASSERT_TRUE(jasperengine::editor::readFloatImage((dir / "Flag_vat.exr").string(), image, &error)) << error;
    image.rgba[((size_t)1 * (size_t)image.width + 2) * 4 + 1] = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(jasperengine::editor::writeExr((dir / "Flag_vat.exr").string(), image, false, &error)) << error;
    ev::VatImportResult r = ev::importVat(specFor(dir));
    ASSERT_TRUE(hasError(r, "invalid_data")) << issues(r);
    EXPECT_NE(issues(r).find("nan at frame 1, vertex "), std::string::npos) << issues(r);
    EXPECT_NE(issues(r).find("payload texel"), std::string::npos) << issues(r);

    // bounds whose remap overflows to infinity
    const fs::path inf = freshDir("openvat-inf");
    writeOpenVat(inf, "Flag", OpenVatOptions());
    std::ifstream in(inf / "Flag-remap_info.json");
    Json remap = Json::parse(in);
    in.close();
    remap["os-remap"]["Min"] = {1e308, 0, 0};
    remap["os-remap"]["Max"] = {-1e308, 1, 1};
    writeText(inf / "Flag-remap_info.json", remap.dump());
    r = ev::importVat(specFor(inf));
    ASSERT_TRUE(hasError(r, "invalid_data")) << issues(r);
    EXPECT_NE(issues(r).find("inf at frame "), std::string::npos) << issues(r);
}
