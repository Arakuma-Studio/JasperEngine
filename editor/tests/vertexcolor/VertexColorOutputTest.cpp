// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R7, R9, R61, R62, R72, R73, R92, R100, D11, Table 2: what leaves the
// editor's vertex colors: the model cook's painted order in every family, the exported file without
// its editor section and the export's refusal of a stale target, Export as Model's COLOR_0 with
// split hard edges and its Replace in Scene step, Bake to Texture's pixels, the material warnings
// and channel uses, and the Inspector's rows.

#include "VertexColorTestScene.h"

#include "Catalog.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ModelComponent.h"
#include "model/GltfDocument.h"
#include "model/ModelCookCache.h"
#include "model/ModelCooker.h"
#include "model/ModelUsage.h"
#include "object/Model.h"
#include "stb_image.h"
#include "subsystem/MeshSystem.h"
#include "support/ModelFixtures.h"
#include "support/TextureCorpus.h"
#include "texture/MaterialPbr.h"
#include "vertexcolor/VertexColorEditorInternal.h"
#include "vertexcolor/VertexColorFile.h"
#include "vertexcolor/VertexColorInspector.h"

#include <fstream>
#include <map>
#include <set>

namespace VertexColorOutputTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_output_test {
    using namespace jasperengine::editor::vertexcolortest;

    // two static mesh nodes, A (grid primitives of 2 and 3 quads a side) and B (4 quads, moved):
    // what Merge Static Meshes merges, each primitive with its own vertex count
    std::vector<uint8_t> staticTwoNodeModel() {
        editor::test::GltfBuild build;
        editor::test::GridOptions grid;
        grid.welded = false;
        grid.quads = 2;
        const auto first = editor::test::addGridPrimitive(build, grid);
        grid.quads = 3;
        grid.seed = 5;
        const auto second = editor::test::addGridPrimitive(build, grid);
        grid.quads = 4;
        grid.seed = 9;
        const auto third = editor::test::addGridPrimitive(build, grid);
        build.json["meshes"].push_back({{"name", "A"}, {"primitives", {first, second}}});
        build.json["meshes"].push_back({{"name", "B"}, {"primitives", {third}}});
        build.json["nodes"] = nlohmann::ordered_json::array({{{"name", "A"}, {"mesh", 0}}, {{"name", "B"}, {"mesh", 1}, {"translation", {3.0, 0.0, 0.0}}}});
        build.json["scenes"] = nlohmann::ordered_json::array({{{"nodes", {0, 1}}}});
        build.json["scene"] = 0;
        return build.glb();
    }

    class VertexColorOutput : public VertexColorSceneTest {
    protected:
        // a model in the assets (the cook tests' by default), on a Model of the scene
        Entity addModel(const std::string& file, const std::vector<uint8_t>& bytes = editor::test::makeTestModel(8, false), bool merged = false) {
            const fs::path path = project->getAssetsPath() / file;
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            project->onAssetFilesChanged(project->getAssetDatabase().importAssets(path));
            const Entity entity = track(std::make_unique<Model>(scene()), "Statue");
            ModelComponent& model = scene()->getComponent<ModelComponent>(entity);
            model.filename.set(file, project->getAssetDatabase().getAssetGuid(path));
            model.mergeStaticMeshes = merged;
            EXPECT_TRUE(scene()->getSystem<MeshSystem>()->loadGLTF(entity, path.string()));
            return entity;
        }

        // the COLOR_0 of a primitive of an exported file, and its triangle list
        static void colorsIn(const GltfDocument& doc, const GltfDocument::Json& primitive, std::vector<float>& colors, std::vector<uint32_t>& indices) {
            std::string error;
            int components = 0;
            ASSERT_TRUE(primitive["attributes"].contains("COLOR_0"));
            ASSERT_TRUE(doc.readAccessorFloats(primitive["attributes"]["COLOR_0"].get<size_t>(), colors, components, error)) << error;
            EXPECT_EQ(components, 4);
            ASSERT_TRUE(doc.readIndices(primitive["indices"].get<size_t>(), indices, error)) << error;
        }
    };

    class VertexColorCook : public ::testing::Test {};

    const Vector4 kRed(1.0f, 0.0f, 0.0f, 1.0f);
    const Vector4 kGreen(0.0f, 1.0f, 0.0f, 1.0f);

    std::vector<uint32_t> indicesOf(const GltfDocument& doc, size_t mesh, size_t primitive) {
        std::vector<uint32_t> out;
        std::string error;
        const GltfDocument::Json& json = doc.json()["meshes"][mesh]["primitives"][primitive];
        EXPECT_TRUE(doc.readIndices(json["indices"].get<size_t>(), out, error)) << error;
        return out;
    }

    std::vector<float> positionsOf(const GltfDocument& doc, size_t mesh, size_t primitive) {
        std::vector<float> out;
        int components = 0;
        std::string error;
        const size_t accessor = doc.json()["meshes"][mesh]["primitives"][primitive]["attributes"]["POSITION"].get<size_t>();
        EXPECT_TRUE(doc.readAccessorFloats(accessor, out, components, error)) << error;
        return out;
    }

    ResolvedModelSettings everything(MeshCompression compression) {
        ResolvedModelSettings settings;
        settings.optimizeMesh = OptimizeMesh::Everything;
        settings.meshCompression = compression;
        return settings;
    }

// ---- the cook (R9, D11)

TEST_F(VertexColorCook, PaintedPrimitivesKeepTheSourceOrderInEveryFamily) {
    editor::test::ScratchDir scratch("vertex-colors-cook");
    const fs::path source = scratch.path / "model.glb";
    const std::vector<uint8_t> bytes = editor::test::makeTestModel(16, false);
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    GltfDocument original;
    std::string error;
    ASSERT_TRUE(original.load(source, error)) << error;

    for (const MeshCompression compression : {MeshCompression::Lossless, MeshCompression::Medium}) {
        for (const ModelFamily family : {ModelFamily::Desktop, ModelFamily::Mobile, ModelFamily::Web}) {
            ModelCookRequest request;
            request.source = source;
            request.assetPath = "model.glb";
            request.settings = everything(compression);
            request.family = family;
            request.painted = {{-1, -1}};
            std::vector<uint8_t> glb;
            ModelCookRecord record;
            ASSERT_TRUE(ModelCooker::cookToMemory(request, glb, record, error)) << error;
            GltfDocument cooked;
            ASSERT_TRUE(cooked.loadMemory(glb, scratch.path, "cooked.glb", error)) << error;
            // vertex count, triangle order and every corner where the source had it: the paint's keys
            EXPECT_EQ(indicesOf(cooked, 0, 0), indicesOf(original, 0, 0)) << meshCompressionName(compression);
            EXPECT_EQ(positionsOf(cooked, 0, 0).size(), positionsOf(original, 0, 0).size()) << meshCompressionName(compression);
            EXPECT_EQ(indicesOf(cooked, 1, 0), indicesOf(original, 1, 0)) << meshCompressionName(compression);
        }
    }

    // red control: the same cook unpainted welds and reorders
    ModelCookRequest unpainted;
    unpainted.source = source;
    unpainted.assetPath = "model.glb";
    unpainted.settings = everything(MeshCompression::Lossless);
    std::vector<uint8_t> glb;
    ModelCookRecord record;
    ASSERT_TRUE(ModelCooker::cookToMemory(unpainted, glb, record, error)) << error;
    GltfDocument cooked;
    ASSERT_TRUE(cooked.loadMemory(glb, scratch.path, "cooked.glb", error)) << error;
    EXPECT_NE(indicesOf(cooked, 0, 0), indicesOf(original, 0, 0));
}

TEST_F(VertexColorCook, PaintedSetIsInTheCacheKeyOnlyWhenThere) {
    const ResolvedModelSettings settings = everything(MeshCompression::Lossless);
    const std::string plain = ModelCookCache::key("abc", settings, ModelFamily::Desktop, {});
    EXPECT_EQ(ModelCookCache::key("abc", settings, ModelFamily::Desktop, {}, {}), plain) << "an unpainted model's cooks stay valid";
    EXPECT_NE(ModelCookCache::key("abc", settings, ModelFamily::Desktop, {}, {{-1, -1}}), plain);

    ModelUsage usage;
    EXPECT_TRUE(usage.painted().empty());
    ModelDocumentUse use;
    use.painted = true;
    usage.documents.push_back(use);
    EXPECT_EQ(usage.painted(), (std::set<std::pair<int, int>>{{-1, -1}}));
}

// ---- the export (R92)

TEST_F(VertexColorOutput, ExportShipsNoSnapshotAndRefusesStaleTargets) {
    const Entity box = addBox();
    ASSERT_TRUE(edit(box, {fillAll(kRed)}).ok);
    ASSERT_TRUE(project->saveSceneNow(sceneId));
    const fs::path file = project->getAssetsPath() / colorsOf(box)->data.path;
    EXPECT_TRUE(vertexColorExportProblems(*project).empty());

    const fs::path shipped = dir.path() / "shipped.vcolors";
    std::string error;
    ASSERT_TRUE(cookVertexColorsFile(file, shipped, error)) << error;
    vc::Data data;
    ASSERT_TRUE(vc::readFile(shipped, data, error)) << error;
    ASSERT_EQ(data.targets.size(), 1u);
    EXPECT_TRUE(data.targets[0].snapshotPositions.empty()) << "the editor section stays home";
    EXPECT_FALSE(data.targets[0].base.empty());

    // the file on disk painted on another mesh: the export stops, naming the GameObject and the fix
    vc::Data stale;
    ASSERT_TRUE(vc::readFile(file, stale, error)) << error;
    stale.targets[0].fingerprint.topologyHash ^= 0x5a5a;
    ASSERT_TRUE(vc::writeFile(file, stale, true, error)) << error;
    const std::vector<std::string> problems = vertexColorExportProblems(*project);
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("'Box'"), std::string::npos) << problems[0];
    EXPECT_NE(problems[0].find("save it"), std::string::npos) << problems[0];
}

// ---- Export as Model (R61)

TEST_F(VertexColorOutput, ExportAsModelWritesSplitColorsAndReplacesInOneStep) {
    const Entity statue = addModel("statue.glb");
    const std::vector<vce::TargetInfo> targets = vce::listTargets(project.get(), sceneId, statue);
    ASSERT_FALSE(targets.empty());
    const vc::TargetKey key = targets.front().key;
    // faces 0 and 1 green, the rest red: hard edges, so split vertices
    vc::Op base = fillAll(kRed);
    vc::Op faces;
    faces.type = vc::OpType::Set;
    faces.target.kind = vc::TargetKind::Faces;
    faces.target.ids = {0, 1};
    faces.value = kGreen;
    vce::EditRequest request;
    request.targets = {{statue, key}};
    request.ops = {base, faces};
    const vce::EditResult painted = vce::applyEdit(project.get(), sceneId, request);
    ASSERT_TRUE(painted.ok) << painted.error;
    ASSERT_GT(painted.targets[0].splitVertices, 0u);

    vce::ExportModelOptions options;
    options.path = "statue painted.glb";
    options.replaceInScene = true;
    const size_t before = steps();
    std::string error;
    ASSERT_TRUE(vce::exportAsModel(project.get(), sceneId, statue, options, &error)) << error;
    GltfDocument doc;
    ASSERT_TRUE(doc.load(project->getAssetsPath() / options.path, error)) << error;
    const GltfDocument::Json& node = doc.json()["nodes"][(size_t)key.node];
    const GltfDocument::Json& primitive = doc.json()["meshes"][node["mesh"].get<size_t>()]["primitives"][(size_t)key.primitive];
    ASSERT_TRUE(primitive["attributes"].contains("COLOR_0"));
    std::vector<float> colors;
    int components = 0;
    ASSERT_TRUE(doc.readAccessorFloats(primitive["attributes"]["COLOR_0"].get<size_t>(), colors, components, error)) << error;
    EXPECT_EQ(components, 4);
    EXPECT_EQ(colors.size() / 4, (size_t)targets.front().vertexCount + painted.targets[0].splitVertices) << "hard edges split in the file";
    std::vector<uint32_t> indices;
    ASSERT_TRUE(doc.readIndices(primitive["indices"].get<size_t>(), indices, error)) << error;
    // face 0's corners are green, face 2's red
    for (int k = 0; k < 3; k++) {
        EXPECT_NEAR(colors[indices[(size_t)k] * 4 + 1], 1.0f, 1e-3f);
        EXPECT_NEAR(colors[indices[6 + (size_t)k] * 4], 1.0f, 1e-3f);
    }

    // Replace in Scene: the Model's file and the component, one step
    EXPECT_EQ(scene()->getComponent<ModelComponent>(statue).filename.path, options.path);
    EXPECT_EQ(colorsOf(statue), nullptr);
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(history->top()->name(), "Export as Model (Statue)");
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene()->getComponent<ModelComponent>(statue).filename.path, "statue.glb");
    EXPECT_NE(colorsOf(statue), nullptr);
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / options.path)) << "the file stays for a redo";

    // a second export to the same name is refused
    options.replaceInScene = false;
    EXPECT_FALSE(vce::exportAsModel(project.get(), sceneId, statue, options, &error));
    EXPECT_NE(error.find("exists"), std::string::npos) << error;
}

// R61: a plain mesh (a built-in primitive) is written from its geometry, and Replace in Scene makes the
// GameObject a Model of the new file in one step that an undo takes back to the mesh
TEST_F(VertexColorOutput, ExportAsModelWritesAPlainMeshAndMakesItAModel) {
    const Entity tile = addGrid("Tile", 2);
    const vc::TargetKey key = firstKey(tile);
    const uint32_t vertices = vce::listTargets(project.get(), sceneId, tile)[0].vertexCount;
    vc::Op face;
    face.type = vc::OpType::Set;
    face.target.kind = vc::TargetKind::Faces;
    face.target.ids = {0};
    face.value = kGreen;
    vce::EditRequest request;
    request.targets = {{tile, key}};
    request.ops = {fillAll(kRed), face};
    const vce::EditResult painted = vce::applyEdit(project.get(), sceneId, request);
    ASSERT_TRUE(painted.ok) << painted.error;
    ASSERT_GT(painted.targets[0].splitVertices, 0u);

    const uint64_t paintedHash = hashOf(tile);
    vce::ExportModelOptions options;
    options.path = "tile painted.glb";
    options.replaceInScene = true;
    const size_t before = steps();
    std::string error;
    ASSERT_TRUE(vce::exportAsModel(project.get(), sceneId, tile, options, &error)) << error;
    GltfDocument doc;
    ASSERT_TRUE(doc.load(project->getAssetsPath() / options.path, error)) << error;
    ASSERT_EQ(doc.json()["meshes"].size(), 1u);
    ASSERT_EQ(doc.json()["meshes"][0]["primitives"].size(), 1u);
    const GltfDocument::Json& primitive = doc.json()["meshes"][0]["primitives"][0];
    std::vector<float> colors;
    std::vector<uint32_t> indices;
    colorsIn(doc, primitive, colors, indices);
    EXPECT_EQ(colors.size() / 4, (size_t)vertices + painted.targets[0].splitVertices) << "hard edges split in the file";
    EXPECT_EQ(positionsOf(doc, 0, 0).size() / 3, colors.size() / 4);
    ASSERT_GE(indices.size(), 6u);
    for (int k = 0; k < 3; k++) {
        EXPECT_NEAR(colors[indices[(size_t)k] * 4 + 1], 1.0f, 1e-3f) << "face 0 green";
        EXPECT_NEAR(colors[indices[3 + (size_t)k] * 4], 1.0f, 1e-3f) << "face 1 red";
    }
    EXPECT_TRUE(primitive.contains("material"));

    // Replace in Scene: a Model of the file, its mesh the file's, no component, one step (the
    // editor loads models on a worker: the load is finished here)
    ModelLoadCmd::completePendingLoad(sceneId, tile);
    const ModelComponent* model = scene()->findComponent<ModelComponent>(tile);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->filename.path, options.path);
    EXPECT_EQ(colorsOf(tile), nullptr);
    EXPECT_EQ(vce::listTargets(project.get(), sceneId, tile).at(0).vertexCount, colors.size() / 4);
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(history->top()->name(), "Export as Model (Tile)");
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(scene()->findComponent<ModelComponent>(tile), nullptr);
    ASSERT_NE(colorsOf(tile), nullptr);
    EXPECT_EQ(vce::listTargets(project.get(), sceneId, tile).at(0).vertexCount, vertices) << "the mesh's own geometry back";
    EXPECT_EQ(hashOf(tile), paintedHash) << "and its paint";
    ASSERT_TRUE(history->redo());
    ModelLoadCmd::completePendingLoad(sceneId, tile);
    EXPECT_NE(scene()->findComponent<ModelComponent>(tile), nullptr);
    EXPECT_EQ(vce::listTargets(project.get(), sceneId, tile).at(0).vertexCount, colors.size() / 4);
    EXPECT_EQ(colorsOf(tile), nullptr);
}

// R2, R61: with Merge Static Meshes each merged submesh is written back into the file primitive it
// came from, and the Model, still merged, reads the same submeshes from the new file
TEST_F(VertexColorOutput, ExportAsModelWritesEachMergedSubmeshIntoItsPrimitive) {
    const Entity statue = addModel("pair.glb", staticTwoNodeModel(), true);
    const std::vector<vce::TargetInfo> targets = vce::listTargets(project.get(), sceneId, statue);
    ASSERT_EQ(targets.size(), 3u);
    const Vector4 shades[3] = {kRed, kGreen, Vector4(0.0f, 0.0f, 1.0f, 1.0f)};
    std::map<uint32_t, Vector4> byVertices;   // each primitive has its own vertex count
    vce::EditRequest request;
    for (size_t i = 0; i < targets.size(); i++) {
        ASSERT_GE(targets[i].key.submesh, 0) << "merged targets are submeshes";
        request.targets.push_back({statue, targets[i].key});
        request.perTargetOps.push_back({fillAll(shades[i])});
        byVertices[targets[i].vertexCount] = shades[i];
    }
    ASSERT_EQ(byVertices.size(), 3u);
    ASSERT_TRUE(vce::applyEdit(project.get(), sceneId, request).ok);

    vce::ExportModelOptions options;
    options.path = "pair painted.glb";
    options.replaceInScene = true;
    std::string error;
    ASSERT_TRUE(vce::exportAsModel(project.get(), sceneId, statue, options, &error)) << error;
    GltfDocument doc;
    ASSERT_TRUE(doc.load(project->getAssetsPath() / options.path, error)) << error;
    size_t checked = 0;
    for (size_t m = 0; m < doc.json()["meshes"].size(); m++) {
        for (size_t p = 0; p < doc.json()["meshes"][m]["primitives"].size(); p++) {
            std::vector<float> colors;
            std::vector<uint32_t> indices;
            colorsIn(doc, doc.json()["meshes"][m]["primitives"][p], colors, indices);
            const uint32_t count = (uint32_t)(colors.size() / 4);
            ASSERT_TRUE(byVertices.count(count)) << "mesh " << m << " primitive " << p << ": " << count << " colors";
            const Vector4 expected = byVertices[count];
            EXPECT_EQ(positionsOf(doc, m, p).size() / 3, count);
            for (uint32_t v = 0; v < count; v++) {
                EXPECT_NEAR(colors[v * 4], expected.x, 1e-3f);
                EXPECT_NEAR(colors[v * 4 + 1], expected.y, 1e-3f);
                EXPECT_NEAR(colors[v * 4 + 2], expected.z, 1e-3f);
            }
            checked++;
        }
    }
    EXPECT_EQ(checked, 3u);
    EXPECT_EQ(scene()->getComponent<ModelComponent>(statue).filename.path, options.path);
    EXPECT_TRUE(scene()->getComponent<ModelComponent>(statue).mergeStaticMeshes);
    EXPECT_EQ(colorsOf(statue), nullptr);
    const std::vector<vce::TargetInfo> reloaded = vce::listTargets(project.get(), sceneId, statue);
    ASSERT_EQ(reloaded.size(), 3u);
    for (size_t i = 0; i < 3; i++) EXPECT_EQ(reloaded[i].vertexCount, targets[i].vertexCount) << "the same merged submeshes";
}

// ---- Bake to Texture (R62)

TEST_F(VertexColorOutput, BakeToTextureRasterizesInUvSpace) {
    const Entity grid = addGrid("Floor", 4);
    const vc::TargetKey key = firstKey(grid);
    // the half x < 0 red, the other green; linear grey 0.5 through the middle row
    vc::Op left = fillAll(kRed);
    left.target.kind = vc::TargetKind::Box;
    left.target.center = Vector3(-1.0f, 0.0f, 0.0f);
    left.target.halfExtents = Vector3(0.9f, 1.0f, 2.0f);
    vc::Op right = left;
    right.value = kGreen;
    right.target.center = Vector3(1.0f, 0.0f, 0.0f);
    ASSERT_TRUE(edit(grid, {left, right}).ok);

    vce::BakeTextureOptions options;
    options.key = key;
    options.path = "Floor Colors.png";
    options.size = 64;
    options.padding = 2;
    std::string error;
    ASSERT_TRUE(vce::bakeToTexture(project.get(), sceneId, grid, options, &error)) << error;
    const fs::path file = project->getAssetsPath() / options.path;
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(width, 64);
    EXPECT_EQ(height, 64);
    const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, grid, key);
    ASSERT_NE(mesh, nullptr);
    // where a red vertex is in UV, the texel is red; where a green one is, green
    int checked = 0;
    for (uint32_t v = 0; v < mesh->vertexCount(); v++) {
        if (std::fabs(mesh->positions[v].x) < 0.6f) continue;
        const int x = std::min(63, (int)(mesh->uv0[v].x * 64.0f));
        const int y = std::min(63, (int)(mesh->uv0[v].y * 64.0f));
        const unsigned char* texel = pixels + ((size_t)y * 64 + (size_t)x) * 4;
        if (mesh->positions[v].x < 0.0f) EXPECT_GT(texel[0], 250) << v;
        else EXPECT_GT(texel[1], 250) << v;
        checked++;
    }
    EXPECT_GT(checked, 0);
    stbi_image_free(pixels);
    EXPECT_FALSE(project->getAssetDatabase().getAssetGuid(file).isNull()) << "imported";

    // sRGB off writes the linear values: grey 0.5 is 128, on it 188
    ASSERT_TRUE(edit(grid, {fillAll(Vector4(0.5f, 0.5f, 0.5f, 1.0f))}).ok);
    for (const bool srgb : {false, true}) {
        options.srgb = srgb;
        ASSERT_TRUE(vce::bakeToTexture(project.get(), sceneId, grid, options, &error)) << error;
        unsigned char* grey = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
        ASSERT_NE(grey, nullptr);
        // (the engine's sRGB curve is its deterministic series, not libm's: within a few levels of 188)
        EXPECT_NEAR((int)grey[(32 * 64 + 32) * 4], srgb ? 188 : 128, 3);
        stbi_image_free(grey);
    }
}

// ---- materials (R72, R73, Table 2)

TEST_F(VertexColorOutput, MaterialWarningsNameTheIgnoredChannelsAndTheFix) {
    const Entity box = addBox();
    Material& material = scene()->getComponent<MeshComponent>(box).submeshes[0].material;
    auto codes = [&](vc::ChannelMask painted) {
        std::string out;
        for (const vc::Warning& warning : vce::materialWarnings(project.get(), sceneId, box, painted)) out += warning.code + ": " + warning.message + "\n";
        return out;
    };
    // Lit Tint reads RGB and A
    EXPECT_EQ(codes(vc::CHANNELS_RGBA), "");
    // Tint without Alpha multiplies opacity ignores A
    MaterialPbrExtensions pbr = material.pbr;
    pbr.vertexColor.alphaOpacity = false;
    material.pbr = pbr;
    EXPECT_NE(codes(vc::CHANNEL_A).find("material_ignores_channels"), std::string::npos);
    EXPECT_NE(codes(vc::CHANNEL_A).find("channel A"), std::string::npos) << codes(vc::CHANNEL_A);
    EXPECT_EQ(codes(vc::CHANNELS_RGB), "");
    // Off ignores everything, and says how to turn it on
    pbr.vertexColor.use = VertexColorUse::OFF;
    material.pbr = pbr;
    EXPECT_NE(codes(vc::CHANNEL_R).find("Vertex Color Use"), std::string::npos) << codes(vc::CHANNEL_R);
    // Masks: R to roughness reads R; G unmapped is ignored
    pbr.vertexColor.use = VertexColorUse::MASKS;
    pbr.vertexColor.r = VertexColorTarget::ROUGHNESS_MULTIPLY;
    material.pbr = pbr;
    EXPECT_EQ(codes(vc::CHANNEL_R), "");
    EXPECT_NE(codes(vc::CHANNEL_G).find("channel G"), std::string::npos) << codes(vc::CHANNEL_G);

    // R73's fix: Off back to Tint with Alpha multiplies opacity, one step, undoable
    pbr.vertexColor.use = VertexColorUse::OFF;
    material.pbr = pbr;
    const size_t before = steps();
    std::string fixError;
    std::vector<vc::Warning> warnings = vce::materialWarnings(project.get(), sceneId, box, vc::CHANNELS_RGBA);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(vce::fixMaterialWarning(project.get(), sceneId, box, warnings[0], &fixError)) << fixError;
    EXPECT_EQ(codes(vc::CHANNELS_RGBA), "");
    EXPECT_EQ(steps(), before + 1);
    ASSERT_TRUE(history->undo());
    EXPECT_NE(codes(vc::CHANNEL_R), "");
    // Masks with A unmapped: the fix maps A to opacity
    pbr.vertexColor.use = VertexColorUse::MASKS;
    material.pbr = pbr;
    warnings = vce::materialWarnings(project.get(), sceneId, box, vc::CHANNEL_A);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(vce::fixMaterialWarning(project.get(), sceneId, box, warnings[0], &fixError)) << fixError;
    EXPECT_EQ(((const MaterialPbrExtensions&)material.pbr).vertexColor.a, VertexColorTarget::OPACITY);
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(vce::fixMaterialWarning(project.get(), sceneId, box, {"low_density", "x"}, &fixError));

    // R7, R72: what each channel feeds, after its name
    std::string error;
    ASSERT_TRUE(vce::ensureComponent(project.get(), sceneId, box, &error)) << error;
    history->addCommandNoMerge(new PropertyCmd<std::string>(project.get(), sceneId, box, ComponentType::VertexColorsComponent, "channelNames[0]", std::string("Moss")));
    std::array<std::string, 4> uses = vce::channelUses(project.get(), sceneId, box);
    EXPECT_EQ(uses[0], "Moss (Lit: " + std::string(MaterialPbr::getVertexColorTargetLabel(VertexColorTarget::ROUGHNESS_MULTIPLY)) + ")");
    EXPECT_EQ(uses[1], "") << "R is mapped: G and B feed nothing (RGB tints only when R, G and B are all unmapped)";

    // vertex_blend with two layers reads R only
    material.shader = "library:vertex_blend";
    EXPECT_EQ(codes(vc::CHANNEL_R), "");
    EXPECT_NE(codes(vc::CHANNEL_B).find("Layers"), std::string::npos) << codes(vc::CHANNEL_B);
    uses = vce::channelUses(project.get(), sceneId, box);
    EXPECT_EQ(uses[0], "Moss (vertex_blend: Layer R)");
    EXPECT_EQ(uses[2], "");
}

// ---- the Inspector (R100)

TEST_F(VertexColorOutput, InspectorRowsAreUnityLabelsOnRealFields) {
    const Entity box = addBox();
    std::string error;
    ASSERT_TRUE(vce::ensureComponent(project.get(), sceneId, box, &error)) << error;
    std::set<std::string> labels;
    for (const VertexColorInspectorRow& row : vertexColorInspectorRows()) {
        labels.insert(row.label);
        EXPECT_NE(Catalog::findProperty(scene(), box, ComponentType::VertexColorsComponent, row.property).ref, nullptr) << row.property;
        EXPECT_STRNE(row.help, "") << row.property;
        EXPECT_EQ(std::string(row.label).find("Element"), std::string::npos) << "the channel names are R, G, B and A rows";
    }
    for (const char* wanted : {"Data", "Format", "Initial Value", "R Channel", "G Channel", "B Channel", "A Channel"}) {
        EXPECT_TRUE(labels.count(wanted)) << wanted;
    }

    std::vector<VertexColorTargetRow> rows = vertexColorTargetRows(project.get(), sceneId, box);
    ASSERT_EQ(rows.size(), 1u);
    const uint32_t vertices = vce::listTargets(project.get(), sceneId, box)[0].vertexCount;
    EXPECT_EQ(rows[0].detail, vertexColorCount(vertices) + " vertices, not painted");

    // a face of its own color: the vertices its hard edges add
    vc::Op face;
    face.type = vc::OpType::Set;
    face.target.kind = vc::TargetKind::Faces;
    face.target.ids = {0};
    face.value = kRed;
    ASSERT_TRUE(edit(box, {face}).ok);
    rows = vertexColorTargetRows(project.get(), sceneId, box);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].painted);
    EXPECT_NE(rows[0].detail.find("vertices for hard edges"), std::string::npos) << rows[0].detail;
    EXPECT_EQ(vertexColorCount(1204), "1,204");

    // Format goes through the file: one step, values converted, undone together
    const size_t before = steps();
    ASSERT_TRUE(vce::setFormat(project.get(), sceneId, box, vc::Format::RGBA16F, &error)) << error;
    EXPECT_EQ(colorsOf(box)->format, vc::Format::RGBA16F);
    EXPECT_EQ(colorsOf(box)->loaded->format, vc::Format::RGBA16F);
    EXPECT_EQ(steps(), before + 1);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(colorsOf(box)->format, vc::Format::RGBA8);
    EXPECT_EQ(colorsOf(box)->loaded->format, vc::Format::RGBA8);
}

} // namespace vertex_color_output_test

}  // namespace VertexColorOutputTest_cpp
