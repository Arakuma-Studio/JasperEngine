// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R14-R16, AC10: what a bake draws, what makes it stale, and what it writes into a .material and a
// .meta. The writing itself (one undo step, the files, export) is tools/mcp/tests/test_procedural_material.py's.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/ProceduralBake.h"
#include "../../procmat/ProceduralMaterialKind.h"
#include "../../procmat/ProceduralRun.h"
#include "../../procmat/ProceduralChecks.h"
#include "PlanInterpreter.h"

#include "asset/AssetMeta.h"
#include "texsynth/SynthImage.h"
#include "texsynth/SynthesisRecipe.h"
#include "texture/Material.h"

#include "TempDir.h"

#include "yaml-cpp/yaml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::procmat;
using namespace jasperengine::test;

namespace {

    const std::string GRAPH_GUID = "0123456789abcdef0123456789abcdef";

    graph::Graph graphOf(std::vector<std::pair<std::string, std::string>> nodes, std::vector<std::array<std::string, 4>> edges) {
        graph::Graph file;
        file.kind = PROCEDURAL_MATERIAL_KIND;
        for (const auto& [id, type] : nodes) {
            graph::Node node;
            node.id = id;
            node.type = type;
            file.nodes.push_back(std::move(node));
        }
        int next = 1;
        for (const auto& edge : edges) {
            graph::Edge item;
            item.id = "e" + std::to_string(next++);
            item.from = {edge[0], edge[1]};
            item.to = {edge[2], edge[3]};
            file.edges.push_back(std::move(item));
        }
        std::sort(file.nodes.begin(), file.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
        file.passthrough["guid"] = GRAPH_GUID;
        return file;
    }

    // perlin into Height and Roughness
    graph::Graph stoneGraph() {
        return graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}}, {{"n1", "output", "out", "height"}, {"n1", "output", "out", "roughness"}});
    }

    std::string hashOf(const graph::Graph& file, const std::shared_ptr<const NodeLibrary>& library, BakeOptions options = BakeOptions()) {
        options.size = 64;
        PassPlan plan;
        std::vector<std::string> outputs;
        std::string error;
        EXPECT_TRUE(planBake(file, library, options, plan, outputs, error)) << error;
        return bakeInputHash(plan);
    }

    // a library whose node `wave` draws `expression`: the source a hash must follow
    std::shared_ptr<const NodeLibrary> waveLibrary(const char* expression) {
        const std::vector<std::pair<std::string, std::string>> files = {
            {"_types.json", R"json({"types": [
                {"name": "f", "type": "float", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "rgba", "expr": "vec4(vec3($(value)), 1.0)"}]},
                {"name": "rgb", "type": "vec3", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "rgba", "expr": "vec4($(value), 1.0)"}]},
                {"name": "rgba", "type": "vec4", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "f", "expr": "($(value)).r"}]}]})json"},
            {"_functions.json", R"json({"functions": {"jp_finite": {"code": ["vec4 jp_finite(vec4 c) { return c; }"], "deps": []}}})json"},
            {"wave.json", std::string(R"({"id": "wave", "kind": "shader", "functions": [], "instance": [], "code": [], "inputs": [], "parameters": [],
                "outputs": [{"type": "f", "expr": ")") + expression + R"("}]})"}};
        std::string error;
        auto library = NodeLibrary::fromFiles(files, error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    // A project folder holding the graph, and the recipe of a bake of it as it is now
    struct BakedProject {
        TempDir dir;
        BakeRecipe recipe;

        explicit BakedProject(const graph::Graph& file) {
            dir.write("project.yaml", "name: bake\n");
            write(file);
            recipe.graphGuid = GRAPH_GUID;
            recipe.graphPath = "assets/stone.procmat";
            recipe.inputHash = hashOf(file, NodeLibrary::shipped());
            recipe.size = 64;
            recipe.seed = 0;
            recipe.output = "height";
            recipe.outputs = {"normal", "orm", "height"};
            recipe.graphOutputs = recipe.outputs;
            recipe.seedFromGraph = true;
            recipe.tier = "gpu";
            recipe.device = "a GPU";
        }
        void write(const graph::Graph& file) {
            std::filesystem::create_directories(dir.path() / "assets");
            dir.write("assets/stone.procmat", graph::save(file));
        }
    };

}

TEST(ProceduralBake, AHeightAloneBakesANormalMadeFromIt) {
    PassPlan plan;
    std::vector<std::string> outputs;
    std::string error;
    BakeOptions options;
    options.size = 64;
    ASSERT_TRUE(planBake(stoneGraph(), NodeLibrary::shipped(), options, plan, outputs, error)) << error;
    EXPECT_EQ(outputs, (std::vector<std::string>{"normal", "orm", "height"})) << "R14's order, the normal from the height";
    const auto normal = std::find_if(plan.passes.begin(), plan.passes.end(), [](const PlannedPass& pass) { return pass.fragment.find("jp_height_normal_") != std::string::npos; });
    EXPECT_NE(normal, plan.passes.end()) << "the normal is drawn from the float height in a pass, not from the 8-bit image";
}

TEST(ProceduralBake, AConnectedNormalIsTurnedIntoTheEnginesConvention) {
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"n2", "normal_map2"}, {"out", MATERIAL_OUTPUT_NODE}},
                                      {{"n1", "output", "n2", "Bump"}, {"n2", "normal", "out", "normal"}});
    PassPlan plan;
    std::vector<std::string> outputs;
    std::string error;
    BakeOptions options;
    options.size = 64;
    ASSERT_TRUE(planBake(file, NodeLibrary::shipped(), options, plan, outputs, error)) << error;
    EXPECT_EQ(outputs, (std::vector<std::string>{"normal"}));
    const bool converted = std::any_of(plan.passes.begin(), plan.passes.end(), [](const PlannedPass& pass) {
        return pass.fragment.find("vec3(-1.0, 1.0, -1.0)") != std::string::npos;
    });
    EXPECT_TRUE(converted) << "Material Maker's normals face -z";
}

TEST(ProceduralBake, OutputsAreChosenByNameAndAnUnknownOneIsRefused) {
    PassPlan plan;
    std::vector<std::string> outputs;
    std::string error;
    BakeOptions options;
    options.size = 64;
    options.outputs = {"height", "orm"};
    ASSERT_TRUE(planBake(stoneGraph(), NodeLibrary::shipped(), options, plan, outputs, error)) << error;
    EXPECT_EQ(outputs, (std::vector<std::string>{"orm", "height"}));
    options.outputs = {"emission"};
    EXPECT_FALSE(planBake(stoneGraph(), NodeLibrary::shipped(), options, plan, outputs, error));
    EXPECT_NE(error.find("no emission output"), std::string::npos) << error;
    options.outputs.clear();
    options.size = 100;
    EXPECT_FALSE(planBake(stoneGraph(), NodeLibrary::shipped(), options, plan, outputs, error)) << "a size that is no power of two";
}

TEST(ProceduralBake, TheHashFollowsInputsAndNothingElse) {
    const auto library = NodeLibrary::shipped();
    const graph::Graph file = stoneGraph();
    const std::string base = hashOf(file, library);
    EXPECT_EQ(base.size(), 64u);
    EXPECT_EQ(hashOf(file, library), base) << "the same inputs, the same hash";

    graph::Graph edited = file;
    edited.findNode("n1")->params["scale_x"] = 9.0;
    EXPECT_NE(hashOf(edited, library), base) << "a value edit is an input";

    graph::Graph moved = file;
    moved.findNode("n1")->position = graph::Vec2{300.0, 40.0};
    EXPECT_EQ(hashOf(moved, library), base) << "where a node sits is not";

    graph::Graph unconnected = file;
    graph::Node spare;
    spare.id = "n9";
    spare.type = "perlin";
    spare.params["scale_x"] = 20.0;
    unconnected.nodes.push_back(spare);
    EXPECT_EQ(hashOf(unconnected, library), base) << "nor a node no output reads";

    BakeOptions seeded;
    seeded.seed = 7;
    EXPECT_NE(hashOf(file, library, seeded), base) << "the seed is";

    // a node's source, resolved: a library update that changes what a node draws makes the bake stale
    const graph::Graph waves = graphOf({{"n1", "wave"}, {"out", MATERIAL_OUTPUT_NODE}}, {{"n1", "out", "out", "roughness"}});
    EXPECT_NE(hashOf(waves, waveLibrary("$uv.x*2.0")), hashOf(waves, waveLibrary("$uv.x*3.0")));
    EXPECT_EQ(hashOf(waves, waveLibrary("$uv.x*2.0")), hashOf(waves, waveLibrary("$uv.x*2.0")));
}

TEST(ProceduralBake, AnEditMakesABakeStaleAndATierChangeAloneDoesNot) {
    BakedProject project(stoneGraph());
    EXPECT_EQ(bakeStatus(project.dir.path(), project.recipe).state, BakeState::Fresh);

    BakeRecipe onTheCpu = project.recipe;
    onTheCpu.tier = "cpu";
    onTheCpu.device = "SpirvInterpreter";
    EXPECT_EQ(bakeStatus(project.dir.path(), onTheCpu).state, BakeState::Fresh) << "R16: a CPU bake is not stale against a GPU bake";

    graph::Graph edited = stoneGraph();
    edited.findNode("n1")->params["persistence"] = 0.8;
    project.write(edited);
    const BakeStatus status = bakeStatus(project.dir.path(), project.recipe);
    EXPECT_EQ(status.state, BakeState::Stale);
    EXPECT_NE(status.reason.find("assets/stone.procmat changed"), std::string::npos) << status.reason;
}

TEST(ProceduralBake, AGraphIsFoundByItsGuidWhereverItMoved) {
    BakedProject project(stoneGraph());
    std::filesystem::create_directories(project.dir.path() / "assets/rocks");
    std::filesystem::rename(project.dir.path() / "assets/stone.procmat", project.dir.path() / "assets/rocks/granite.procmat");
    const BakeStatus moved = bakeStatus(project.dir.path(), project.recipe);
    EXPECT_EQ(moved.state, BakeState::Fresh) << moved.reason;
    EXPECT_EQ(moved.graphPath, "assets/rocks/granite.procmat");
    std::filesystem::remove(project.dir.path() / "assets/rocks/granite.procmat");
    EXPECT_EQ(bakeStatus(project.dir.path(), project.recipe).state, BakeState::GraphMissing);
}

TEST(ProceduralBake, ARecipeRoundTripsAndAGarbledOneIsRefused) {
    BakeRecipe recipe;
    recipe.graphGuid = GRAPH_GUID;
    recipe.graphPath = "assets/stone.procmat";
    recipe.inputHash = std::string(64, 'a');
    recipe.seed = 7;
    recipe.size = 1024;
    recipe.output = "orm";
    recipe.outputs = {"base_color", "orm"};
    recipe.tier = "software";
    recipe.device = "llvmpipe";
    BakeRecipe back;
    std::string error;
    ASSERT_TRUE(recipeFromJson(recipeToJson(recipe), back, error)) << error;
    EXPECT_EQ(back.graphGuid, recipe.graphGuid);
    EXPECT_EQ(back.seed, 7u);
    EXPECT_EQ(back.outputs, recipe.outputs);
    EXPECT_EQ(back.tier, "software");
    nlohmann::json garbled = recipeToJson(recipe);
    garbled["outputs"] = {"orm", "lightmap"};
    EXPECT_FALSE(recipeFromJson(garbled, back, error));
    garbled = recipeToJson(recipe);
    garbled["graph"] = "assets/stone.procmat";
    EXPECT_FALSE(recipeFromJson(garbled, back, error));
}

TEST(ProceduralBake, ARebakeKeepsTheMaterialsOtherFieldsAndATunedFactor) {
    const BakedSlot base = {"base_color", "stone/stone_base_color.png", "11111111111111111111111111111111"};
    const BakedSlot orm = {"orm", "stone/stone_orm.png", "22222222222222222222222222222222"};
    const BakedSlot emission = {"emission", "stone/stone_emission.png", "33333333333333333333333333333333"};

    Material material;
    material.alphaCutoff = 0.25f;
    material.baseColorFactor = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
    material.metallicFactor = 0.0f;
    material.stochasticTiling = true;
    applyBakeToMaterial(material, {base, orm, emission}, {});
    EXPECT_EQ(material.baseColorTexture.getPath(), base.assetPath);
    EXPECT_EQ(material.baseColorTexture.getPathGuid().str(), base.guid);
    EXPECT_EQ(material.metallicRoughnessTexture.getPath(), orm.assetPath) << "the ORM is both slots";
    EXPECT_EQ(material.occlusionTexture.getPath(), orm.assetPath);
    EXPECT_EQ(material.baseColorFactor, Vector4(1.0f, 1.0f, 1.0f, 1.0f)) << "a slot newly baked shows its PNG as it is";
    EXPECT_EQ(material.metallicFactor, 1.0f);
    EXPECT_EQ(material.emissiveFactor, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_EQ(material.alphaCutoff, 0.25f) << "a field the bake does not own";
    EXPECT_TRUE(material.stochasticTiling);

    // someone tints the baked colour and drops the emission from the graph: a re-bake keeps the tint
    material.baseColorFactor = Vector4(0.8f, 0.6f, 0.6f, 1.0f);
    material.alphaCutoff = 0.4f;
    applyBakeToMaterial(material, {base, orm}, {emission});
    EXPECT_EQ(material.baseColorFactor, Vector4(0.8f, 0.6f, 0.6f, 1.0f));
    EXPECT_EQ(material.alphaCutoff, 0.4f);
    EXPECT_TRUE(material.emissiveTexture.empty()) << "a slot holding an output this bake no longer has";
    EXPECT_EQ(material.emissiveFactor, Vector3(0.0f, 0.0f, 0.0f)) << "without its texture a factor of 1 makes the whole surface glow";
}

TEST(ProceduralBake, AMetaSaysWhatItIsAndKeepsWhatItHad) {
    const std::string guid = "44444444444444444444444444444444";
    const std::string fresh = bakedMetaText("", guid, "orm");
    AssetMeta meta;
    ASSERT_TRUE(AssetMeta::parse(fresh, meta)) << fresh;
    EXPECT_EQ(meta.guid.str(), guid);
    const YAML::Node orm = YAML::Load(fresh)["import"]["textureImporter"];
    EXPECT_EQ(orm["type"].as<std::string>(), "default") << "explicit: an ORM in the occlusion slot would be inferred single channel";
    EXPECT_FALSE(orm["srgb"].as<bool>());
    EXPECT_TRUE(YAML::Load(bakedMetaText("", guid, "base_color"))["import"]["textureImporter"]["srgb"].as<bool>());
    EXPECT_EQ(YAML::Load(bakedMetaText("", guid, "normal"))["import"]["textureImporter"]["type"].as<std::string>(), "normal");
    EXPECT_EQ(YAML::Load(bakedMetaText("", guid, "height"))["import"]["textureImporter"]["type"].as<std::string>(), "single_channel");

    const std::string existing = "metaVersion: 1\nguid: 55555555555555555555555555555555\nimport:\n  textureImporter:\n    version: 1\n"
                                 "    type: normal\n    inferredFrom: \"an occlusion slot\"\n    maxSize: 512\nlabels: [rock]\n";
    const std::string rebaked = bakedMetaText(existing, guid, "orm");
    const YAML::Node root = YAML::Load(rebaked);
    EXPECT_EQ(root["guid"].as<std::string>(), "55555555555555555555555555555555") << "a GUID it has stays";
    EXPECT_EQ(root["labels"][0].as<std::string>(), "rock");
    EXPECT_EQ(root["import"]["textureImporter"]["maxSize"].as<int>(), 512) << "an import setting the bake does not own";
    EXPECT_EQ(root["import"]["textureImporter"]["type"].as<std::string>(), "default");
    EXPECT_FALSE(root["import"]["textureImporter"]["inferredFrom"]) << "written, not inferred";
    EXPECT_EQ(metaGuid(rebaked), "55555555555555555555555555555555");
}

TEST(ProceduralBake, CheckReportsAGraphProblemAndAStaleBake) {
    BakedProject project(stoneGraph());
    graph::Graph broken = stoneGraph();
    broken.findNode("n1")->params["scale_z"] = 2.0;
    project.dir.write("assets/broken.procmat", graph::save(broken));
    MaterialsCheck check = checkMaterials(project.dir.path(), {});
    EXPECT_EQ(check.graphs, 2u);
    const auto typo = std::find_if(check.findings.begin(), check.findings.end(), [](const ProjectChecks::Finding& f) { return f.code == "unknown-param"; });
    ASSERT_NE(typo, check.findings.end());
    EXPECT_EQ(typo->path, "assets/broken.procmat");
    EXPECT_EQ(typo->severity, "error");
    EXPECT_NE(typo->suggestion.find("scale_x"), std::string::npos) << typo->suggestion;
    EXPECT_FALSE(std::any_of(check.findings.begin(), check.findings.end(), [](const ProjectChecks::Finding& f) { return f.path == "assets/stone.procmat"; }));

    // a PNG baked from stone.procmat before its last edit
    texsynth::Image image = texsynth::Image::make(4, 4, 1, 8);
    std::vector<uint8_t> png, baked;
    std::string error;
    ASSERT_TRUE(texsynth::encodePng(image, png));
    ASSERT_TRUE(texsynth::insertRecipeChunk(png, recipeToJson(project.recipe).dump(), baked, error, BAKE_RECIPE_KEYWORD)) << error;
    project.dir.write("assets/stone/stone_height.png", std::string(baked.begin(), baked.end()));
    check = checkMaterials(project.dir.path(), {});
    EXPECT_EQ(check.bakes, 1u);
    EXPECT_FALSE(std::any_of(check.findings.begin(), check.findings.end(), [](const ProjectChecks::Finding& f) { return f.code == "procmat-stale"; }))
        << "fresh";
    graph::Graph edited = stoneGraph();
    edited.findNode("n1")->params["scale_y"] = 12.0;
    project.write(edited);
    check = checkMaterials(project.dir.path(), {project.dir.file("assets/stone.procmat")});
    const auto stale = std::find_if(check.findings.begin(), check.findings.end(), [](const ProjectChecks::Finding& f) { return f.code == "procmat-stale"; });
    ASSERT_NE(stale, check.findings.end()) << "a .procmat given checks the PNGs baked from it";
    EXPECT_EQ(stale->path, "assets/stone/stone_height.png");
    EXPECT_EQ(stale->severity, "warning");
}

TEST(ProceduralRun, TheTierComesFromTheDeviceName) {
    EXPECT_EQ(ProceduralRun::editorTier("llvmpipe (LLVM 15.0.7, 256 bits)"), EvaluatorTier::Software);
    EXPECT_EQ(ProceduralRun::editorTier("Llvmpipe (LLVM 17)"), EvaluatorTier::Software);
    EXPECT_EQ(ProceduralRun::editorTier("lavapipe"), EvaluatorTier::Software);
    EXPECT_EQ(ProceduralRun::editorTier("NVIDIA GeForce RTX 4070/PCIe/SSE2"), EvaluatorTier::Gpu);
    EXPECT_EQ(ProceduralRun::editorTier("a device nobody knows"), EvaluatorTier::Gpu) << "R8: an unrecognised name counts as gpu";
}

namespace {

    // How a normal image's red and green follow the height's slope: +1 each in the engine's convention
    // (red against the slope across, green with the slope down the rows)
    std::pair<double, double> slopeCorrelations(const OutputImage& normal, const OutputImage& height) {
        const int w = normal.width, h = normal.height;
        double rx = 0, xx = 0, uu = 0, gy = 0, yy = 0, vv = 0;
        const auto at = [&](int x, int y) { return (double)height.rgba[((size_t)((y + h) % h) * w + (size_t)((x + w) % w)) * 4]; };
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint8_t* p = &normal.rgba[((size_t)y * w + x) * 4];
                const double nx = p[0] / 127.5 - 1.0, ny = p[1] / 127.5 - 1.0;
                const double du = at(x + 1, y) - at(x - 1, y), dv = at(x, y + 1) - at(x, y - 1);
                rx += nx * -du;
                xx += nx * nx;
                uu += du * du;
                gy += ny * dv;
                yy += ny * ny;
                vv += dv * dv;
            }
        }
        return {rx / std::sqrt(xx * uu + 1e-12), gy / std::sqrt(yy * vv + 1e-12)};
    }

    std::map<std::string, OutputImage> interpreted(const graph::Graph& file) {
        PassPlan plan;
        std::vector<std::string> outputs;
        std::string error;
        BakeOptions options;
        options.size = 64;
        EXPECT_TRUE(planBake(file, NodeLibrary::shipped(), options, plan, outputs, error)) << error;
        std::vector<OutputImage> images;
        EXPECT_TRUE(jasperengine::editor::procmat::test::runPlanOnInterpreter(plan, 4, images, error)) << error;
        std::map<std::string, OutputImage> out;
        for (OutputImage& image : images) out[image.name] = std::move(image);
        return out;
    }

}

TEST(ProceduralBake, TheNormalMadeFromTheHeightIsInTheEnginesConvention) {
    std::map<std::string, OutputImage> images = interpreted(stoneGraph());
    ASSERT_TRUE(images.count("normal") && images.count("height"));
    const auto [red, green] = slopeCorrelations(images["normal"], images["height"]);
    EXPECT_GT(red, 0.8) << "faces away from the rise across";
    EXPECT_GT(green, 0.8) << "faces up where the height rises down the rows (OpenGL, Y up)";
    MaterialImages material;
    material.outputs = images;
    const qa::Json report = ProceduralChecks::report(material, CheckRequest());
    for (const qa::Json& row : report["checks"]) {
        if (row["id"] == "normal") {
            EXPECT_EQ(row["status"], "PASS") << row.dump();
        }
    }
}

TEST(ProceduralBake, ALibraryNormalIsTurnedIntoTheEnginesConvention) {
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"n2", "normal_map2"}, {"out", MATERIAL_OUTPUT_NODE}},
                                      {{"n1", "output", "n2", "Bump"}, {"n2", "normal", "out", "normal"}, {"n1", "output", "out", "height"}});
    std::map<std::string, OutputImage> images = interpreted(file);
    ASSERT_TRUE(images.count("normal") && images.count("height"));
    const auto [red, green] = slopeCorrelations(images["normal"], images["height"]);
    EXPECT_GT(red, 0.5) << "Material Maker's x is mirrored";
    EXPECT_GT(green, 0.5);
    size_t away = 0;
    for (size_t i = 0; i < images["normal"].rgba.size(); i += 4) away += images["normal"].rgba[i + 2] < 128;
    EXPECT_EQ(away, 0u) << "Material Maker's normals face -z; the engine's +z";
}

TEST(ProceduralBake, TheSeedIsTheGraphsOwnUnlessToldAnother) {
    graph::Graph file = stoneGraph();
    EXPECT_EQ(graphSeed(file), 0u) << "none kept";
    file.metadata["seed"] = 7;
    EXPECT_EQ(graphSeed(file), 7u);
    file.metadata["seed"] = -3;
    EXPECT_EQ(graphSeed(file), 0u) << "no seed a bake could record";
    file.metadata["seed"] = "seven";
    EXPECT_EQ(graphSeed(file), 0u);
}

TEST(ProceduralBake, ARerollOfTheGraphsSeedMakesItsBakeStaleAndAGivenSeedDoesNot) {
    BakedProject project(stoneGraph());
    graph::Graph rerolled = stoneGraph();
    rerolled.metadata["seed"] = 1234;
    project.write(rerolled);
    const BakeStatus status = bakeStatus(project.dir.path(), project.recipe);
    EXPECT_EQ(status.state, BakeState::Stale) << "the bake took the graph's seed, which changed";
    EXPECT_NE(status.reason.find("seed changed"), std::string::npos) << status.reason;

    BakeRecipe given = project.recipe;
    given.seedFromGraph = false;
    EXPECT_EQ(bakeStatus(project.dir.path(), given).state, BakeState::Fresh) << "a bake at a seed the call gave does not follow the graph's";
}

TEST(ProceduralBake, ConnectingOrDisconnectingAnOutputMakesItsBakeStale) {
    BakedProject project(stoneGraph());
    graph::Graph more = graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}},
                                {{"n1", "output", "out", "height"}, {"n1", "output", "out", "roughness"}, {"n1", "output", "out", "emission"}});
    more.passthrough["guid"] = GRAPH_GUID;
    project.write(more);
    BakeStatus status = bakeStatus(project.dir.path(), project.recipe);
    EXPECT_EQ(status.state, BakeState::Stale) << "the .material lacks the emission now connected";
    EXPECT_NE(status.reason.find("emission is connected"), std::string::npos) << status.reason;

    graph::Graph fewer = graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}}, {{"n1", "output", "out", "roughness"}});
    fewer.passthrough["guid"] = GRAPH_GUID;
    project.write(fewer);
    status = bakeStatus(project.dir.path(), project.recipe);
    EXPECT_EQ(status.state, BakeState::Stale) << "out of date, not a broken graph";
    EXPECT_NE(status.reason.find("height is no longer connected"), std::string::npos) << status.reason;

    // a bake of some outputs is not stale for the others
    project.write(stoneGraph());
    BakeRecipe some = project.recipe;
    BakeOptions orm;
    orm.size = 64;
    orm.outputs = {"orm"};
    PassPlan plan;
    std::vector<std::string> outputs;
    std::string error;
    ASSERT_TRUE(planBake(stoneGraph(), NodeLibrary::shipped(), orm, plan, outputs, error)) << error;
    some.outputs = {"orm"};
    some.inputHash = bakeInputHash(plan);
    EXPECT_EQ(bakeStatus(project.dir.path(), some).state, BakeState::Fresh);
}

TEST(ProceduralBake, ATileabilityFailNamesTheScaleAndItsSingleEdit) {
    graph::Graph file = stoneGraph();
    file.findNode("n1")->params["scale_x"] = 4.5;
    qa::Json report = {{"checks", qa::Json::array({{{"id", "tileability"}, {"status", "FAIL"}, {"threshold", 1.5},
                                                    {"measured", {{"orm", 2.4}, {"height", 1.1}}}, {"limited_by", "seam 2.4 on orm."}}})},
                       {"fixes", qa::Json::array()}};
    attachGraphFixes(report, file, NodeLibrary::shipped());
    ASSERT_EQ(report["fixes"].size(), 1u) << report.dump();
    const qa::Json& fix = report["fixes"][0];
    EXPECT_EQ(fix["check"], "tileability");
    EXPECT_EQ(fix["node"], "n1");
    EXPECT_EQ(fix["op"], (qa::Json{{"op", "setParam"}, {"node", "n1"}, {"name", "scale_x"}, {"value", 5.0}}));
    const std::string limited = report["checks"][0]["limited_by"];
    EXPECT_NE(limited.find("node n1 (perlin) has scale_x 4.5"), std::string::npos) << limited;
    EXPECT_NE(report["checks"][0].value("fix_hint", std::string()).find("Set scale_x of node n1 to 5"), std::string::npos);

    // a seam no scale explains has no edit to offer
    file.findNode("n1")->params["scale_x"] = 4.0;
    report["fixes"] = qa::Json::array();
    attachGraphFixes(report, file, NodeLibrary::shipped());
    EXPECT_TRUE(report["fixes"].empty());
}

TEST(ProceduralBake, ANormalRowOffersToLetTheBakeMakeTheNormal) {
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"n2", "normal_map2"}, {"out", MATERIAL_OUTPUT_NODE}},
                                      {{"n1", "output", "n2", "Bump"}, {"n2", "normal", "out", "normal"}, {"n1", "output", "out", "height"}});
    qa::Json report = {{"checks", qa::Json::array({{{"id", "normal"}, {"status", "WARN"}, {"limited_by", "a DirectX normal map."}}})},
                       {"fixes", qa::Json::array()}};
    attachGraphFixes(report, file, NodeLibrary::shipped());
    ASSERT_EQ(report["fixes"].size(), 1u);
    EXPECT_EQ(report["fixes"][0]["op"], (qa::Json{{"op", "disconnect"}, {"id", "e2"}}));
    EXPECT_NE(report["checks"][0]["limited_by"].get<std::string>().find("Node n2 feeds Normal"), std::string::npos);
}
