// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R7, R12, AC3: the GPU tiers' evaluator on the headless device (Mesa's llvmpipe or lavapipe here,
// so tier 2's code path; tier 1 runs the same code). It lives in this tree because only this one has a
// device for a test; the editor's sources it uses need nothing of the editor.

#include "../../../../editor/procmat/NodeLibrary.h"
#include "../../../../editor/procmat/PassPlanner.h"
#include "../../../../editor/procmat/ProceduralGpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralMaterialKind.h"
#include "../../../../editor/procmat/ProceduralSession.h"
#include "../../../../editor/procmat/ProceduralCpuEvaluator.h"
#include "../../../../editor/tests/procmat/ProcMatImageDifference.h"

#include "render/SystemRender.h"
#include "render/TextureRender.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;
using namespace jasperengine::editor::procmat::test;
using jasperengine::test::GpuEnvironment;

namespace {

    std::shared_ptr<const NodeLibrary> repositoryLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    std::shared_ptr<const NodeLibrary> tinyLibrary() {
        const auto node = [](const char* id, const char* type, const char* expr, const char* inputs) {
            return std::make_pair(std::string(id) + ".json", std::string(R"({"id": ")") + id + R"(", "kind": "shader", "functions": [], "instance": [], "code": [], "parameters": [],
                "inputs": [)" + inputs + R"(], "outputs": [{"type": ")" + type + R"(", "expr": ")" + expr + R"("}]})");
        };
        const std::vector<std::pair<std::string, std::string>> files = {
            {"_types.json", R"json({"types": [
                {"name": "f", "type": "float", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "rgba", "expr": "vec4(vec3($(value)), 1.0)"}]},
                {"name": "rgba", "type": "vec4", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "f", "expr": "($(value)).r"}]}]})json"},
            {"_functions.json", R"json({"functions": {"jp_finite": {"code": [
                "vec4 jp_finite(vec4 c) {", "return vec4(isnan(c.x) ? 0.0 : c.x, isnan(c.y) ? 0.0 : c.y, isnan(c.z) ? 0.0 : c.z, isnan(c.w) ? 0.0 : c.w);", "}"], "deps": []}}})json"},
            node("rows", "f", "$uv.y", ""),
            node("big", "f", "(4.0+$uv.x*0.0)", ""),
            node("negative", "f", "(-1.0+$uv.x*0.0)", ""),
            node("nan", "f", "log(-1.0-$uv.x)", ""),
            node("eighth", "f", "($in($uv)/8.0)", R"({"name": "in", "type": "f", "default": "0.0"})"),
        };
        std::string error;
        auto library = NodeLibrary::fromFiles(files, error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    // Frames until the evaluation ends, as the editor's frames would
    bool evaluate(ProceduralEvaluator& evaluator, EvaluationResult& result, double budgetMs = 8.0) {
        const auto start = std::chrono::steady_clock::now();
        while (!evaluator.advance(budgetMs, result)) {
            GpuEnvironment::drawFrames(1);
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) return false;
        }
        return true;
    }

    bool runSession(ProceduralSession& session, double now, double budgetMs = 8.0) {
        const auto start = std::chrono::steady_clock::now();
        session.update(now, budgetMs);
        while (session.isStale() && session.error().empty()) {
            GpuEnvironment::drawFrames(1);
            session.update(now, budgetMs);
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) return false;
        }
        return true;
    }

    // Graph::findNode looks nodes up in id order, as load() and the edit ops keep them
    void sortById(graph::Graph& file) {
        std::sort(file.nodes.begin(), file.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
        std::sort(file.edges.begin(), file.edges.end(), [](const graph::Edge& a, const graph::Edge& b) { return graph::idLess(a.id, b.id); });
    }

    graph::Graph singleNodeGraph(const char* type, const char* port = "out") {
        graph::Graph file;
        file.kind = PROCEDURAL_MATERIAL_KIND;
        graph::Node node;
        node.id = "n";
        node.type = type;
        graph::Node output;
        output.id = "o";
        output.type = MATERIAL_OUTPUT_NODE;
        file.nodes = {node, output};
        graph::Edge edge;
        edge.id = "e1";
        edge.from = {"n", port};
        edge.to = {"o", "height"};
        file.edges = {edge};
        return file;
    }

    PlanOptions optionsOf(int size) {
        PlanOptions options;
        options.size = size;
        options.maxBufferSize = size;
        return options;
    }

}

TEST(ProceduralEvaluatorGpu, ReportsItsDevice) {
    JASPER_REQUIRE_GPU();
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Software);
    EXPECT_STREQ(evaluatorTierName(evaluator.tier()), "software");
    EXPECT_FALSE(evaluator.deviceName().empty());
}

TEST(ProceduralEvaluatorGpu, ReadsBackTheRowAtVZeroFirst) {
    JASPER_REQUIRE_GPU();
    const auto library = tinyLibrary();
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(singleNodeGraph("rows"), library, {}, optionsOf(64), plan, error)) << error;
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error << "\n" << result.log;
    ASSERT_EQ(result.outputs.size(), 1u);
    const OutputImage& image = result.outputs[0];
    EXPECT_EQ(image.name, "height");
    ASSERT_EQ(image.width, 64);
    ASSERT_EQ(image.rgba.size(), 64u * 64u * 4u);
    for (int y : {0, 31, 63}) {
        const int expected = (int)std::floor(((float)y + 0.5f) / 64.0f * 255.0f + 0.5f);
        EXPECT_NEAR(image.rgba[(size_t)y * 64 * 4], expected, 1) << "row " << y;
    }
    EXPECT_EQ(image.rgba[3], 255);
}

TEST(ProceduralEvaluatorGpu, BuffersHoldValuesOutsideZeroToOne) {
    JASPER_REQUIRE_GPU();
    // R12: 4.0 stored in a Buffer and divided by 8 afterwards is 0.5 only if the Buffer is float
    const auto library = tinyLibrary();
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node big, buffer, eighth, output;
    big.id = "a";
    big.type = "big";
    buffer.id = "b";
    buffer.type = BUFFER_NODE;
    buffer.params = {{"size", 6}};
    eighth.id = "c";
    eighth.type = "eighth";
    output.id = "o";
    output.type = MATERIAL_OUTPUT_NODE;
    file.nodes = {big, buffer, eighth, output};
    graph::Edge one, two, three;
    one.id = "e1";
    one.from = {"a", "out"};
    one.to = {"b", "in"};
    two.id = "e2";
    two.from = {"b", "out"};
    two.to = {"c", "in"};
    three.id = "e3";
    three.from = {"c", "out"};
    three.to = {"o", "height"};
    file.edges = {one, two, three};

    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(file, library, {}, optionsOf(64), plan, error)) << error;
    ASSERT_EQ(plan.passes.size(), 2u);
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error << "\n" << result.log;
    EXPECT_NEAR(result.outputs[0].rgba[0], 128, 1);
}

TEST(ProceduralEvaluatorGpu, TheQuantizePassClampsAndAPassWritesNoNaN) {
    JASPER_REQUIRE_GPU();
    const auto library = tinyLibrary();
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    const auto firstValue = [&](const char* type) -> int {
        PassPlan plan;
        std::string error;
        EXPECT_TRUE(planGraph(singleNodeGraph(type), library, {}, optionsOf(16), plan, error)) << error;
        EXPECT_TRUE(evaluator.prepare(plan, error)) << error;
        EvaluationResult result;
        EXPECT_TRUE(evaluate(evaluator, result));
        EXPECT_TRUE(result.ok) << result.error;
        return result.outputs.empty() ? -1 : result.outputs[0].rgba[0];
    };
    EXPECT_EQ(firstValue("big"), 255);
    EXPECT_EQ(firstValue("negative"), 0);
    EXPECT_EQ(firstValue("nan"), 0);
}

TEST(ProceduralEvaluatorGpu, AFloatEditChangesTheImageWithNoCompile) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    graph::Graph file = singleNodeGraph("perlin", "output");
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ProceduralSession session(evaluator, library, optionsOf(64));
    session.setGraph(file, 0.0);
    ASSERT_TRUE(runSession(session, 0.0));
    ASSERT_TRUE(session.hasImage()) << session.error();
    const std::vector<uint8_t> before = session.latest().outputs[0].rgba;
    const uint64_t compiles = evaluator.compileCount();
    EXPECT_GE(compiles, 1u);

    file.findNode("n")->params["persistence"] = 0.95;
    session.setGraph(file, 1.0);
    EXPECT_TRUE(session.isStale());
    EXPECT_FALSE(session.isRebuildPending());
    ASSERT_TRUE(runSession(session, 1.0));
    EXPECT_EQ(evaluator.compileCount(), compiles) << "R7: no compile for a uniform";
    EXPECT_NE(session.latest().outputs[0].rgba, before);
}

TEST(ProceduralEvaluatorGpu, AnEnumEditRebuildsOnceAfterTheDebounceAndKeepsThePreviousImage) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node a, b, blend, output;
    a.id = "a";
    a.type = "perlin";
    b.id = "b";
    b.type = "perlin";
    b.params = {{"scale_x", 16}, {"scale_y", 16}};
    blend.id = "m";
    blend.type = "blend";
    output.id = "o";
    output.type = MATERIAL_OUTPUT_NODE;
    file.nodes = {a, b, blend, output};
    graph::Edge one, two, three;
    one.id = "e1";
    one.from = {"a", "output"};
    one.to = {"m", "s1"};
    two.id = "e2";
    two.from = {"b", "output"};
    two.to = {"m", "s2"};
    three.id = "e3";
    three.from = {"m", "output"};
    three.to = {"o", "base_color"};
    file.edges = {one, two, three};

    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ProceduralSession session(evaluator, library, optionsOf(64));
    session.setGraph(file, 0.0);
    ASSERT_TRUE(runSession(session, 0.0));
    ASSERT_TRUE(session.hasImage()) << session.error();
    const std::vector<uint8_t> before = session.latest().outputs[0].rgba;
    const uint64_t compiles = evaluator.compileCount();

    file.findNode("m")->params["blend_type"] = "multiply";
    session.setGraph(file, 1.0);
    session.update(1.1, 8.0);
    EXPECT_EQ(evaluator.compileCount(), compiles) << "still within the debounce";
    EXPECT_TRUE(session.isStale());
    EXPECT_EQ(session.latest().outputs[0].rgba, before) << "the previous image stays";

    ASSERT_TRUE(runSession(session, 1.0 + ProceduralSession::REBUILD_DEBOUNCE_SECONDS));
    EXPECT_FALSE(session.isStale()) << session.error();
    EXPECT_EQ(evaluator.compileCount(), compiles + 1) << "one pass, compiled once";
    EXPECT_NE(session.latest().outputs[0].rgba, before);
}

TEST(ProceduralEvaluatorGpu, ARebuildKeepsThePassesThatDidNotChange) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node noise, buffer, blend, output;
    noise.id = "n";
    noise.type = "perlin";
    buffer.id = "b";
    buffer.type = BUFFER_NODE;
    buffer.params = {{"size", 6}};
    blend.id = "m";
    blend.type = "blend";
    output.id = "o";
    output.type = MATERIAL_OUTPUT_NODE;
    file.nodes = {noise, buffer, blend, output};
    sortById(file);
    graph::Edge one, two, three;
    one.id = "e1";
    one.from = {"n", "output"};
    one.to = {"b", "in"};
    two.id = "e2";
    two.from = {"b", "out"};
    two.to = {"m", "s1"};
    three.id = "e3";
    three.from = {"m", "output"};
    three.to = {"o", "base_color"};
    file.edges = {one, two, three};

    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ProceduralSession session(evaluator, library, optionsOf(64));
    session.setGraph(file, 0.0);
    ASSERT_TRUE(runSession(session, 0.0));
    ASSERT_TRUE(session.hasImage()) << session.error();
    const uint64_t compiles = evaluator.compileCount();
    file.findNode("m")->params["blend_type"] = "screen";
    session.setGraph(file, 1.0);
    ASSERT_TRUE(runSession(session, 2.0));
    EXPECT_EQ(evaluator.compileCount(), compiles + 1) << "only the pass the edit changed";
}

TEST(ProceduralEvaluatorGpu, APassThatDoesNotCompileIsNamedWithItsLog) {
    JASPER_REQUIRE_GPU();
    PassPlan plan;
    plan.size = 16;
    PlannedPass pass;
    pass.label = "broken node";
    pass.fragment = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\nvoid main() { g_finalColor = notAFunction(v_uv); }\n";
    plan.passes.push_back(pass);
    plan.buffers.push_back({"output", 16, false});
    plan.steps.push_back({0, 0, 16});
    plan.outputs.push_back({"out", 0});
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    std::string error;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << "a compile failure is reported by advance";
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failedPass, 0);
    EXPECT_NE(result.error.find("broken node"), std::string::npos) << result.error;
    EXPECT_NE(result.log.find("notAFunction"), std::string::npos) << result.log;
    EXPECT_TRUE(result.outputs.empty()) << "R9: no stale pixels";
}

TEST(ProceduralEvaluatorGpu, ACancelledEvaluationDrawsNothingMore) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(singleNodeGraph("perlin", "output"), library, {}, optionsOf(16), plan, error)) << error;
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    evaluator.cancel();
    EvaluationResult result;
    EXPECT_TRUE(evaluator.advance(8.0, result));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.outputs.empty());
    EXPECT_EQ(evaluator.compileCount(), 0u);

    // the plan stays: a value edit restarts it, compiling what the cancel skipped
    evaluator.setUniforms(0, plan.passes[0].uniforms);
    ASSERT_TRUE(evaluate(evaluator, result));
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(ProceduralEvaluatorGpu, APlanThatDoesNotReadBackLeavesTheImageOnTheDevice) {
    JASPER_REQUIRE_GPU();
    const auto library = tinyLibrary();
    PlanOptions options = optionsOf(32);
    options.readBack = false;
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(singleNodeGraph("rows"), library, {}, options, plan, error)) << error;
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    EXPECT_EQ(evaluator.outputTexture("height"), nullptr);
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.outputs.empty());
    TextureRender* texture = evaluator.outputTexture("height");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->getWidth(), 32);
}

TEST(ProceduralEvaluatorGpu, TheSameInputsGiveTheSameBytes) {
    JASPER_REQUIRE_GPU();
    // R13 on one device: the second evaluator reuses nothing of the first
    const auto library = repositoryLibrary();
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(singleNodeGraph("fbm4", "output"), library, {}, optionsOf(64), plan, error)) << error;
    std::vector<uint8_t> first;
    for (int run = 0; run < 2; run++) {
        ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
        ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
        EvaluationResult result;
        ASSERT_TRUE(evaluate(evaluator, result));
        ASSERT_TRUE(result.ok) << result.error;
        if (run == 0) first = result.outputs[0].rgba;
        else EXPECT_EQ(result.outputs[0].rgba, first);
    }
}

TEST(ProceduralEvaluatorGpu, APassMatchesTheInterpreter) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    PassPlan plan;
    std::string error;
    ASSERT_TRUE(planGraph(singleNodeGraph("perlin", "output"), library, {}, optionsOf(128), plan, error)) << error;
    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error << "\n" << result.log;
    EvaluationResult reference;
    ASSERT_TRUE(evaluateOnCpu(plan, reference)) << reference.error;
    const ImageDifference difference = differenceOf(result.outputs[0].rgba, reference.outputs[0].rgba);
    std::printf("[gpu] perlin on %s against the interpreter: %.3f mean, %d at p99\n", jasperengine::test::HeadlessContext::getRendererName(), difference.mean, difference.p99);
    EXPECT_TRUE(difference.withinAc8()) << difference.mean << " mean, " << difference.p99 << " at p99";
}

TEST(ProceduralEvaluatorGpu, AValueEditInsideAnIterationStartsItOver) {
    JASPER_REQUIRE_GPU();
    // X iterates twice; its loop reads Y, a Buffer the planner cut, whose pass adds a uniform to X
    const auto pass = [](const char* label, const char* reads, const char* body, bool block) {
        PlannedPass planned;
        planned.label = label;
        planned.fragment = std::string("#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\n") +
            (reads[0] ? std::string("uniform texture2D t_") + reads + ";\nuniform sampler s_" + reads + ";\n" : std::string()) +
            (block ? "uniform u_fs_procmat {\n\tvec4 v[1];\n} pm;\n" : "") + "void main() {\n\tg_finalColor = " + body + ";\n}\n";
        return planned;
    };
    PassPlan plan;
    plan.size = 16;
    plan.passes.push_back(pass("init", "", "vec4(0.0, 0.0, 0.0, 1.0)", false));
    plan.passes.push_back(pass("add", "x", "textureLod(sampler2D(t_x, s_x), v_uv, 0.0) + vec4(pm.v[0].x, 0.0, 0.0, 0.0)", true));
    plan.passes.push_back(pass("loop", "y", "textureLod(sampler2D(t_y, s_y), v_uv, 0.0)", false));
    plan.passes.push_back(pass("out", "x", "textureLod(sampler2D(t_x, s_x), v_uv, 0.0)", false));
    plan.passes[1].uniforms.values = {0.1f, 0.0f, 0.0f, 0.0f};
    plan.passes[1].textures = {{"x", 0}};
    plan.passes[2].textures = {{"y", 1}};
    plan.passes[3].textures = {{"x", 0}};
    plan.buffers = {{"x", 16, false}, {"y", 16, false}, {"out", 16, false}};
    plan.steps = {{0, 0, 16}, {1, 1, 16}, {2, 0, 16}, {1, 1, 16}, {2, 0, 16}, {3, 2, 16}};
    plan.outputs = {{"out", 2}};

    ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
    std::string error;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(evaluate(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error << "\n" << result.log;
    EXPECT_NEAR(result.outputs[0].rgba[0], 51, 1) << "two iterations of + 0.1";

    evaluator.setUniforms(1, UniformBlock{{0.2f, 0.0f, 0.0f, 0.0f}});
    ASSERT_TRUE(evaluate(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NEAR(result.outputs[0].rgba[0], 102, 1) << "two iterations of + 0.2 from the start, not on top of the last result";
}
