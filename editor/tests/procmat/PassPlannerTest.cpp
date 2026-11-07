// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R5, R6: what a graph becomes. Passes are cut at Buffers, and the planner inserts those a graph
// did not place.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/PassPlanner.h"
#include "../../procmat/ProceduralMaterialKind.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    std::shared_ptr<const NodeLibrary> repositoryLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    // A small library, for the rules no library node isolates: how many times an input is sampled
    std::shared_ptr<const NodeLibrary> tinyLibrary() {
        std::vector<std::pair<std::string, std::string>> files;
        const auto types = R"json({"types": [
            {"name": "f", "type": "float", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "rgba", "expr": "vec4(vec3($(value)), 1.0)"}]},
            {"name": "rgba", "type": "vec4", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "f", "expr": "(dot(($(value)).rgb, vec3(1.0))/3.0)"}]}]})json";
        const auto functions = R"json({"functions": {"jp_finite": {"code": ["vec4 jp_finite(vec4 c) { return c; }"], "deps": []}}})json";
        const auto noise = R"json({"id": "noise", "kind": "shader", "functions": [], "instance": [], "code": [], "inputs": [], "parameters": [],
            "outputs": [{"type": "f", "expr": "fract($uv.x*3.0)"}]})json";
        const auto tap3 = R"json({"id": "tap3", "kind": "shader", "functions": [], "instance": [], "code": [], "parameters": [],
            "inputs": [{"name": "in", "type": "f", "default": "0.0"}],
            "outputs": [{"type": "f", "expr": "($in($uv+vec2(0.1))+$in($uv+vec2(0.2))+$in($uv+vec2(0.3)))"}]})json";
        const auto tap4 = R"json({"id": "tap4", "kind": "shader", "functions": [], "instance": [], "code": [], "parameters": [],
            "inputs": [{"name": "in", "type": "f", "default": "0.0"}],
            "outputs": [{"type": "f", "expr": "($in($uv+vec2(0.1))+$in($uv+vec2(0.2))+$in($uv+vec2(0.3))+$in($uv+vec2(0.4)))"}]})json";
        const auto looped = R"json({"id": "looped", "kind": "shader", "functions": [], "code": [], "parameters": [],
            "instance": ["float $(name)_sum(vec2 uv) {", "float s = 0.0;", "for (int i = 0; i < 8; i++) { s += $in(uv+vec2(float(i))); }", "return s;", "}"],
            "inputs": [{"name": "in", "type": "f", "default": "0.0", "function": true}],
            "outputs": [{"type": "f", "expr": "$(name)_sum($uv)"}]})json";
        for (auto [name, text] : std::vector<std::pair<const char*, const char*>>{{"_types.json", types}, {"_functions.json", functions}, {"noise.json", noise},
                                                                                  {"tap3.json", tap3}, {"tap4.json", tap4}, {"looped.json", looped}}) {
            files.emplace_back(name, text);
        }
        std::string error;
        auto library = NodeLibrary::fromFiles(files, error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    // Graph::findNode looks nodes up in id order, as load() and the edit ops keep them
    void sortById(graph::Graph& file) {
        std::sort(file.nodes.begin(), file.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
        std::sort(file.edges.begin(), file.edges.end(), [](const graph::Edge& a, const graph::Edge& b) { return graph::idLess(a.id, b.id); });
    }

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
            item.from.node = edge[0];
            item.from.port = edge[1];
            item.to.node = edge[2];
            item.to.port = edge[3];
            file.edges.push_back(std::move(item));
        }
        sortById(file);
        return file;
    }

    // every buffer a step reads was drawn by an earlier step, or by the same run of an Iterate
    void expectDependenciesFirst(const PassPlan& plan) {
        std::set<int> drawn;
        for (const PlannedStep& step : plan.steps) {
            if (step.pass >= 0) {
                for (const PlannedTexture& texture : plan.passes[(size_t)step.pass].textures) {
                    EXPECT_TRUE(drawn.count(texture.buffer) != 0) << plan.passes[(size_t)step.pass].label << " reads buffer " << texture.buffer << " before it is drawn";
                }
            }
            drawn.insert(step.buffer);
        }
        for (const PlannedOutput& output : plan.outputs) EXPECT_TRUE(drawn.count(output.buffer) != 0);
    }

    // A chain of a noise blended over and over, in one graph file
    graph::Graph chainGraph(int blends) {
        std::vector<std::pair<std::string, std::string>> nodes = {{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}};
        std::vector<std::array<std::string, 4>> edges;
        std::string previous = "n1";
        const std::pair<const char*, const char*> noises[] = {{"fbm4", "output"}, {"voronoi2", "nodes"}, {"perlin", "output"}, {"bricks3", "bricks_pattern"}};
        for (int i = 0; i < blends; i++) {
            const std::string noise = "a" + std::to_string(i);
            const std::string blend = "b" + std::to_string(i);
            nodes.push_back({noise, noises[i % 4].first});
            nodes.push_back({blend, "blend2"});
            edges.push_back({previous, "output", blend, "b"});
            edges.push_back({noise, noises[i % 4].second, blend, "l1"});
            previous = blend;
        }
        edges.push_back({previous, "output", "out", "roughness"});
        return graphOf(nodes, edges);
    }

}

TEST(PassPlanner, ASmallGraphIsOnePass) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}}, {{"n1", "output", "out", "height"}});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 256;
    ASSERT_TRUE(planGraph(file, library, {}, options, plan, error)) << error;
    ASSERT_EQ(plan.passes.size(), 1u);
    EXPECT_EQ(plan.size, 256);
    ASSERT_EQ(plan.outputs.size(), 1u);
    EXPECT_EQ(plan.outputs[0].name, "height");
    EXPECT_NE(plan.passes[0].fragment.find("jp_finite"), std::string::npos) << "R12: every pass ends in jp_finite";
    EXPECT_EQ(plan.passes[0].nodes, std::vector<std::string>{"n1"});
}

TEST(PassPlanner, AGraphWithNothingConnectedIsAnError) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}}, {});
    PassPlan plan;
    std::string error;
    EXPECT_FALSE(planGraph(file, library, {}, PlanOptions(), plan, error));
    EXPECT_NE(error.find("Material Output"), std::string::npos) << error;
}

TEST(PassPlanner, TheMaterialOutputGivesOneImagePerSlotAndOneOrm) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const graph::Graph file = graphOf({{"n1", "perlin"}, {"out", MATERIAL_OUTPUT_NODE}},
                                      {{"n1", "output", "out", "base_color"}, {"n1", "output", "out", "roughness"}, {"n1", "output", "out", "metallic"}});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planGraph(file, library, {}, options, plan, error)) << error;
    std::vector<std::string> names;
    for (const PlannedOutput& output : plan.outputs) names.push_back(output.name);
    EXPECT_EQ(names, (std::vector<std::string>{"base_color", "orm"}));
    const auto orm = std::find_if(plan.passes.begin(), plan.passes.end(), [](const PlannedPass& pass) { return pass.label == "orm"; });
    ASSERT_NE(orm, plan.passes.end());
    EXPECT_NE(orm->fragment.find("1.0"), std::string::npos) << "occlusion takes its default";
}

TEST(PassPlanner, ALongChainIsCutWithinTheLimits) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planGraph(chainGraph(120), library, {}, options, plan, error)) << error;
    EXPECT_GT(plan.passes.size(), 1u) << "241 nodes do not fit one pass";
    for (const PlannedPass& pass : plan.passes) {
        EXPECT_LE(pass.fragment.size(), options.limits.glslBytes) << pass.label;
        EXPECT_LE((pass.uniforms.values.size() + 3) / 4, (size_t)options.limits.uniformVec4s) << pass.label;
        EXPECT_LE(pass.textures.size(), (size_t)options.limits.textures) << pass.label;
    }
    expectDependenciesFirst(plan);
}

TEST(PassPlanner, ThePlanOfAGraphIsDeterministic) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    PassPlan a, b;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planGraph(chainGraph(40), library, {}, options, a, error)) << error;
    ASSERT_TRUE(planGraph(chainGraph(40), library, {}, options, b, error)) << error;
    EXPECT_TRUE(a.sameStructure(b));
    for (size_t i = 0; i < a.passes.size(); i++) EXPECT_EQ(a.passes[i].uniforms, b.passes[i].uniforms);
}

TEST(PassPlanner, AnOutputSampledRepeatedlyGetsABuffer) {
    const auto library = tinyLibrary();
    ASSERT_TRUE(library);
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    const graph::Graph few = graphOf({{"n", "noise"}, {"t", "tap3"}}, {{"n", "out", "t", "in"}});
    ASSERT_TRUE(planGraph(few, library, {{"out", "t", "out"}}, options, plan, error)) << error;
    EXPECT_EQ(plan.passes.size(), 1u) << "three coordinates stay inline";

    const graph::Graph many = graphOf({{"n", "noise"}, {"t", "tap4"}}, {{"n", "out", "t", "in"}});
    ASSERT_TRUE(planGraph(many, library, {{"out", "t", "out"}}, options, plan, error)) << error;
    ASSERT_EQ(plan.passes.size(), 2u) << "four are worth a Buffer";
    EXPECT_EQ(plan.buffers.size(), 2u);
    expectDependenciesFirst(plan);
    const auto consumer = std::find_if(plan.passes.begin(), plan.passes.end(), [](const PlannedPass& pass) { return !pass.textures.empty(); });
    ASSERT_NE(consumer, plan.passes.end());
    EXPECT_EQ(consumer->textures.size(), 1u);
    EXPECT_EQ(plan.buffers[(size_t)consumer->textures[0].buffer].size, 64) << "an inserted Buffer takes the plan's size";
}

TEST(PassPlanner, AnInputCalledFromALoopGetsABuffer) {
    const auto library = tinyLibrary();
    ASSERT_TRUE(library);
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    const graph::Graph file = graphOf({{"n", "noise"}, {"l", "looped"}}, {{"n", "out", "l", "in"}});
    ASSERT_TRUE(planGraph(file, library, {{"out", "l", "out"}}, options, plan, error)) << error;
    EXPECT_EQ(plan.passes.size(), 2u);
    expectDependenciesFirst(plan);
}

TEST(PassPlanner, AnInputReadAsATextureGetsABufferWithLevels) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    // fast_blur_shader reads textureLod($in.texture, ...)
    const graph::Graph file = graphOf({{"n", "perlin"}, {"b", "fast_blur_shader"}}, {{"n", "output", "b", "in"}});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planGraph(file, library, {{"out", "b", "low_pass"}}, options, plan, error)) << error;
    ASSERT_EQ(plan.passes.size(), 2u);
    bool levels = false;
    for (const PlannedBuffer& buffer : plan.buffers) levels = levels || buffer.mips;
    EXPECT_TRUE(levels) << "a level of detail needs box-filtered mips (R11)";
    expectDependenciesFirst(plan);
}

TEST(PassPlanner, AnUnconnectedTextureInputIsAnError) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const graph::Graph file = graphOf({{"b", "fast_blur_shader"}}, {});
    PassPlan plan;
    std::string error;
    EXPECT_FALSE(planGraph(file, library, {{"out", "b", "low_pass"}}, PlanOptions(), plan, error));
    EXPECT_NE(error.find("has to be connected"), std::string::npos) << error;
}

TEST(PassPlanner, AnIterateBufferIsOneStepPerIteration) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    graph::Graph file = graphOf({{"n", "perlin"}, {"s", "symmetric_nearest_neighbor"}}, {{"n", "output", "s", "in"}});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    options.maxBufferSize = 64;
    ASSERT_TRUE(planGraph(file, library, {{"out", "s", "out"}}, options, plan, error)) << error;
    expectDependenciesFirst(plan);
    int onLoopBuffer = 0;
    for (const PlannedStep& step : plan.steps) {
        if (step.pass < 0) continue;
        for (const PlannedTexture& texture : plan.passes[(size_t)step.pass].textures) {
            if (texture.buffer == step.buffer && texture.name.find("_loop") != std::string::npos) onLoopBuffer++;
        }
    }
    EXPECT_GE(onLoopBuffer, 1) << "the loop reads what its own buffer holds";
}

TEST(PassPlanner, AShrinkingIterateChangesSizeAsItGoes) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    // auto_tones is the library's one shrinking Iterate Buffer
    Expander expander(library);
    NodeInstance tones = expander.instantiate("auto_tones", Json::object());
    NodeInstance noise = expander.instantiate("perlin", Json::object());
    expander.connect(tones.inputs.at(0), SourceRef{noise.generator, 0, false});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planPasses(expander, {{"out", *tones.outputs.at(0)}}, options, plan, error)) << error;
    expectDependenciesFirst(plan);
    std::set<int> sizes;
    for (const PlannedStep& step : plan.steps) sizes.insert(step.size);
    EXPECT_TRUE(sizes.count(2048) != 0) << "auto_tones's Buffer";
    EXPECT_TRUE(sizes.count(4) != 0) << "halved each iteration, down to 4";
}

TEST(PassPlanner, ACycleIsAnError) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const graph::Graph file = graphOf({{"a", "blend2"}, {"b", "blend2"}}, {{"a", "output", "b", "b"}, {"b", "output", "a", "b"}});
    PassPlan plan;
    std::string error;
    EXPECT_FALSE(planGraph(file, library, {{"out", "a", "out"}}, PlanOptions(), plan, error));
    EXPECT_NE(error.find("cycle"), std::string::npos) << error;
}

TEST(PassPlanner, EditsThatOnlyChangeValuesKeepTheStructure) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    graph::Graph file = graphOf({{"n", "perlin"}, {"b", "blend"}, {"out", MATERIAL_OUTPUT_NODE}},
                                {{"n", "output", "b", "s1"}, {"b", "output", "out", "base_color"}});
    PlanOptions options;
    options.size = 64;
    PassPlan base;
    std::string error;
    ASSERT_TRUE(planGraph(file, library, {}, options, base, error)) << error;

    PassPlan edited;
    file.findNode("n")->params["persistence"] = 0.25;
    ASSERT_TRUE(planGraph(file, library, {}, options, edited, error)) << error;
    EXPECT_TRUE(base.sameStructure(edited)) << "a float is a uniform";
    EXPECT_FALSE(base.passes[0].uniforms == edited.passes[0].uniforms);

    PlanOptions reseeded = options;
    reseeded.seed = 3.0;
    ASSERT_TRUE(planGraph(file, library, {}, reseeded, edited, error)) << error;
    EXPECT_TRUE(base.sameStructure(edited)) << "a seed is a uniform";

    file.findNode("b")->params["blend_type"] = "multiply";
    ASSERT_TRUE(planGraph(file, library, {}, options, edited, error)) << error;
    EXPECT_FALSE(base.sameStructure(edited)) << "an enum changes the text";

    PlanOptions larger = options;
    larger.size = 128;
    ASSERT_TRUE(planGraph(file, library, {}, larger, edited, error)) << error;
    EXPECT_FALSE(base.sameStructure(edited)) << "a size rebuilds";
}

TEST(PassPlanner, AValueEditInsideAnIterationRedrawsFromItsInitialStep) {
    // an Iterate Buffer X (initial pass 0, loop pass 2) whose loop reads a Buffer Y the planner cut (pass 1)
    PassPlan plan;
    plan.steps = {{0, 0, 16}, {1, 1, 16}, {2, 0, 16}, {1, 1, 16}, {2, 0, 16}, {3, 2, 16}};
    EXPECT_EQ(firstStepToRedraw(plan, 1), 0u) << "Y's pass reads X, which starts over from its initial step";
    EXPECT_EQ(firstStepToRedraw(plan, 2), 0u);
    EXPECT_EQ(firstStepToRedraw(plan, 0), 0u);
    EXPECT_EQ(firstStepToRedraw(plan, 3), 5u) << "a pass after the loop redraws alone";
    EXPECT_EQ(firstStepToRedraw(plan, 9), plan.steps.size()) << "a pass no step draws";
}

TEST(PassPlanner, APassKnowsWhereTheSizeOfEachBufferItReadsIs) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    // auto_tones' loop snaps to the nearest texel of a Buffer that halves each iteration (M4 of the review)
    Expander expander(library);
    NodeInstance tones = expander.instantiate("auto_tones", Json::object());
    NodeInstance noise = expander.instantiate("perlin", Json::object());
    expander.connect(tones.inputs.at(0), SourceRef{noise.generator, 0, false});
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    ASSERT_TRUE(planPasses(expander, {{"out", *tones.outputs.at(0)}}, options, plan, error)) << error;
    int sized = 0;
    for (const PlannedPass& pass : plan.passes) {
        for (const PlannedSize& size : pass.sizes) {
            ASSERT_LT(size.index, pass.uniforms.values.size());
            EXPECT_EQ(pass.uniforms.values[size.index], (float)plan.buffers[(size_t)size.buffer].size) << "generated with the Buffer's first size";
            sized++;
        }
    }
    EXPECT_GT(sized, 0);
}
