// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R8, R9, R11, R12, R13 and AC6 on the CPU tier, which needs no device: this binary has none, so it
// is also the path a bake takes with no window (R39). The GPU tiers against it are
// engine/tests/core/procmat (ProceduralTiersGpu, ProcMatNodeImageGpuSlow).

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/PassPlanner.h"
#include "../../procmat/ProceduralCpuEvaluator.h"
#include "../../procmat/ProceduralDevice.h"
#include "../../procmat/ProceduralMaterialKind.h"
#include "../../procmat/ProceduralSession.h"
#include "../../procmat/ProceduralTieredEvaluator.h"
#include "../../procmat/SpirvInterpreter.h"

#include "jobs/JobSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    std::shared_ptr<const NodeLibrary> cpuTierLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    // Two nodes of our own: `kill` discards, which the interpreter does not run, and `eighth` inlines it
    std::shared_ptr<const NodeLibrary> refusingLibrary() {
        const auto node = [](const char* id, const char* instance, const char* expr, const char* inputs) {
            return std::make_pair(std::string(id) + ".json", std::string(R"({"id": ")") + id + R"(", "kind": "shader", "functions": [], "instance": [)" + instance +
                R"(], "code": [], "parameters": [], "inputs": [)" + inputs + R"(], "outputs": [{"type": "f", "expr": ")" + expr + R"("}]})");
        };
        const std::vector<std::pair<std::string, std::string>> files = {
            {"_types.json", R"json({"types": [
                {"name": "f", "type": "float", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "rgba", "expr": "vec4(vec3($(value)), 1.0)"}]},
                {"name": "rgba", "type": "vec4", "params": "uv", "paramdefs": "vec2 uv", "convert": [{"type": "f", "expr": "($(value)).r"}]}]})json"},
            {"_functions.json", R"json({"functions": {"jp_finite": {"code": [
                "vec4 jp_finite(vec4 c) {", "return vec4(isnan(c.x) ? 0.0 : c.x, isnan(c.y) ? 0.0 : c.y, isnan(c.z) ? 0.0 : c.z, isnan(c.w) ? 0.0 : c.w);", "}"], "deps": []}}})json"},
            node("kill", R"("float kill_below(vec2 p) { if (p.x > 7.0) discard; return p.x; }")", "kill_below($uv)", ""),
            node("eighth", "", "($in($uv)/8.0)", R"({"name": "in", "type": "f", "default": "0.0"})"),
        };
        std::string error;
        auto library = NodeLibrary::fromFiles(files, error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    std::string firstPort(const std::shared_ptr<const NodeLibrary>& library, const std::string& type, graph::PortDirection direction) {
        const Json* descriptor = library->descriptor(type);
        EXPECT_NE(descriptor, nullptr) << type;
        if (!descriptor) return {};
        for (const graph::Port& port : nodePorts(*descriptor, 1)) {
            if (port.direction == direction) return port.id;
        }
        return {};
    }

    // Each node's first output into the next one's first input, the last into the height output
    graph::Graph chainGraph(const std::shared_ptr<const NodeLibrary>& library, const std::vector<std::pair<std::string, std::string>>& nodes) {
        graph::Graph file;
        file.kind = PROCEDURAL_MATERIAL_KIND;
        int edge = 0;
        for (size_t i = 0; i < nodes.size(); i++) {
            graph::Node node;
            node.id = nodes[i].first;
            node.type = nodes[i].second;
            file.nodes.push_back(node);
            graph::Edge link;
            link.id = "e" + std::to_string(++edge);
            link.from = {nodes[i].first, firstPort(library, nodes[i].second, graph::PortDirection::Out)};
            link.to = i + 1 < nodes.size() ? graph::PortRef{nodes[i + 1].first, firstPort(library, nodes[i + 1].second, graph::PortDirection::In)}
                                           : graph::PortRef{"zz", "height"};
            file.edges.push_back(link);
        }
        graph::Node output;
        output.id = "zz";
        output.type = MATERIAL_OUTPUT_NODE;
        file.nodes.push_back(output);
        std::sort(file.nodes.begin(), file.nodes.end(), [](const graph::Node& a, const graph::Node& b) { return graph::idLess(a.id, b.id); });
        std::sort(file.edges.begin(), file.edges.end(), [](const graph::Edge& a, const graph::Edge& b) { return graph::idLess(a.id, b.id); });
        return file;
    }

    PassPlan libraryPlan(const std::shared_ptr<const NodeLibrary>& library, const graph::Graph& file, int size) {
        PlanOptions options;
        options.size = size;
        options.maxBufferSize = size;
        PassPlan plan;
        std::string error;
        EXPECT_TRUE(planGraph(file, library, {}, options, plan, error)) << error;
        return plan;
    }

    bool finish(ProceduralEvaluator& evaluator, EvaluationResult& result, double budgetMs = 2.0) {
        const auto start = std::chrono::steady_clock::now();
        while (!evaluator.advance(budgetMs, result)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) return false;
        }
        return true;
    }

    PlannedPass handPass(const char* label, const char* reads, const char* body, bool block) {
        PlannedPass planned;
        planned.label = label;
        planned.nodes = {label};
        planned.fragment = std::string("#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\n") +
            (reads[0] ? std::string("uniform texture2D t_") + reads + ";\nuniform sampler s_" + reads + ";\n" : std::string()) +
            (block ? "uniform u_fs_procmat {\n\tvec4 v[1];\n} pm;\n" : "") + "void main() {\n\tg_finalColor = " + body + ";\n}\n";
        return planned;
    }

    // X iterates twice; its loop reads Y, a Buffer the planner cut, whose pass adds a uniform to X
    PassPlan iteratePlan(float add) {
        PassPlan plan;
        plan.size = 16;
        plan.passes.push_back(handPass("init", "", "vec4(0.0, 0.0, 0.0, 1.0)", false));
        plan.passes.push_back(handPass("add", "x", "textureLod(sampler2D(t_x, s_x), v_uv, 0.0) + vec4(pm.v[0].x, 0.0, 0.0, 0.0)", true));
        plan.passes.push_back(handPass("loop", "y", "textureLod(sampler2D(t_y, s_y), v_uv, 0.0)", false));
        plan.passes.push_back(handPass("out", "x", "textureLod(sampler2D(t_x, s_x), v_uv, 0.0)", false));
        plan.passes[1].uniforms.values = {add, 0.0f, 0.0f, 0.0f};
        plan.passes[1].textures = {{"x", 0}};
        plan.passes[2].textures = {{"y", 1}};
        plan.passes[3].textures = {{"x", 0}};
        plan.buffers = {{"x", 16, false}, {"y", 16, false}, {"out", 16, false}};
        plan.steps = {{0, 0, 16}, {1, 1, 16}, {2, 0, 16}, {1, 1, 16}, {2, 0, 16}, {3, 2, 16}};
        plan.outputs = {{"out", 2}};
        return plan;
    }

    // A device tier that fails what it is told to: R9's logic without a device
    class FailingDevice : public ProceduralEvaluator {
    public:
        EvaluatorTier tier() const override { return EvaluatorTier::Gpu; }
        std::string deviceName() const override { return "Test GPU 9000"; }
        bool prepare(const PassPlan& plan, std::string&) override {
            prepares++;
            pending = true;
            passes = plan.passes.size();
            return true;
        }
        void setUniforms(uint32_t, const UniformBlock&) override { pending = true; }
        bool advance(double, EvaluationResult& result) override {
            result = EvaluationResult();
            if (!pending) return true;
            pending = false;
            if (failPass >= 0) {
                result.error = "Error compiling to SPIRV";
                result.failedPass = failPass;
                result.log = "ERROR: 0:12: 'dFdx' : no such function";
                return true;
            }
            result.ok = true;
            // an image no CPU evaluation draws
            result.outputs.push_back({"height", 1, 1, {7, 7, 7, 7}});
            return true;
        }
        void cancel() override { pending = false; }

        int failPass = -1;
        int prepares = 0;
        bool pending = false;
        size_t passes = 0;
    };

}

TEST(ProceduralDevice, ATierComesFromTheDeviceTypeOrAKnownSoftwareName) {
    // R8, AC4: Mesa, SwiftShader and WARP are software; hardware names, and names we do not know, are gpu
    EXPECT_EQ(tierOfContext("llvmpipe (LLVM 15.0.7, 256 bits)", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("softpipe", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("Mesa Offscreen swrast", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("SwiftShader Device (Subzero)", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("Microsoft Basic Render Driver", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("Apple Software Renderer", false), EvaluatorTier::Software);
    EXPECT_EQ(tierOfContext("NVIDIA GeForce RTX 4090/PCIe/SSE2", false), EvaluatorTier::Gpu);
    EXPECT_EQ(tierOfContext("AMD Radeon RX 7900 XTX (radeonsi, navi31, LLVM 17.0.6, DRM 3.57)", false), EvaluatorTier::Gpu);
    EXPECT_EQ(tierOfContext("Mesa Intel(R) UHD Graphics 630 (CFL GT2)", false), EvaluatorTier::Gpu);
    EXPECT_EQ(tierOfContext("Apple M2 Pro", false), EvaluatorTier::Gpu);
    EXPECT_EQ(tierOfContext("Some Future Accelerator", false), EvaluatorTier::Gpu);
    // a Vulkan CPU device is software whatever it is called
    EXPECT_EQ(tierOfContext("Some Future Accelerator", true), EvaluatorTier::Software);
}

TEST(ProceduralDevice, WithNoContextTheEditorIsOnTheCpuTier) {
    // this binary starts no graphics device, as a bake with no window does not (R39)
    const EvaluatorDevice device = editorDevice();
    EXPECT_EQ(device.tier, EvaluatorTier::Cpu);
    EXPECT_EQ(device.name, cpuDeviceName());
    std::unique_ptr<ProceduralEvaluator> evaluator = makeEditorEvaluator();
    EXPECT_EQ(evaluator->tier(), EvaluatorTier::Cpu);
    EXPECT_STREQ(evaluatorTierName(evaluator->tier()), "cpu");

    const auto library = cpuTierLibrary();
    const PassPlan plan = libraryPlan(library, chainGraph(library, {{"n", "perlin"}}), 32);
    std::string error;
    ASSERT_TRUE(evaluator->prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(finish(*evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.outputs.size(), 1u);
    EXPECT_EQ(result.outputs[0].rgba.size(), 32u * 32u * 4u);
    EXPECT_TRUE(result.warnings.empty()) << "no context is not a failure";
}

TEST(ProceduralCpuEvaluator, TheSameGraphGivesTheSameBytesOnEveryRun) {
    // AC6 on tier 3: a single pass, and a graph whose Buffer feeds the next pass, on job systems of 0 (every
    // step run by the slices themselves), 1 and 5 workers, at two slicings, and through the blocking entry
    const auto library = cpuTierLibrary();
    for (const graph::Graph& file : {chainGraph(library, {{"n", "perlin"}}), chainGraph(library, {{"a", "voronoi2"}, {"b", "fast_blur"}})}) {
        const PassPlan plan = libraryPlan(library, file, 64);
        ASSERT_FALSE(plan.passes.empty());
        std::vector<std::vector<uint8_t>> runs;
        for (int workers : {0, 1, 5}) {
            jobs::Config config;
            config.workers = workers;
            config.reserveForFrame = 0;
            jobs::JobSystem pool(config);
            for (double budget : {0.0, 50.0}) {
                ProceduralCpuEvaluator evaluator(&pool);
                std::string error;
                ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
                EvaluationResult result;
                ASSERT_TRUE(finish(evaluator, result, budget));
                ASSERT_TRUE(result.ok) << result.error;
                ASSERT_EQ(result.outputs.size(), 1u);
                runs.push_back(result.outputs[0].rgba);
            }
        }
        EvaluationResult blocking;
        ASSERT_TRUE(evaluateOnCpu(plan, blocking)) << blocking.error;
        runs.push_back(blocking.outputs[0].rgba);
        for (size_t run = 1; run < runs.size(); run++) EXPECT_EQ(runs[0], runs[run]) << "run " << run;
        EXPECT_GT(std::count_if(runs[0].begin(), runs[0].end(), [&](uint8_t v) { return v != runs[0][0]; }), 0) << "an image, not a flat colour";
    }
}

TEST(ProceduralCpuEvaluator, ABufferIsStoredAtHalfPrecision) {
    // R12: 1.0004 is 1.0 in a half, so the pass reading it finds nothing left over; in a float it would
    // find 0.0004, which * 1000 is 102 in 8 bits
    PassPlan plan;
    plan.size = 8;
    plan.passes = {handPass("write", "", "vec4(1.0004 + v_uv.x * 0.0)", false),
                   handPass("read", "b", "(textureLod(sampler2D(t_b, s_b), v_uv, 0.0) - 1.0) * 1000.0", false)};
    plan.passes[1].textures = {{"b", 0}};
    plan.buffers = {{"b", 8, false}, {"out", 8, false}};
    plan.steps = {{0, 0, 8}, {1, 1, 8}};
    plan.outputs = {{"out", 1}};
    EvaluationResult result;
    ASSERT_TRUE(evaluateOnCpu(plan, result)) << result.error;
    EXPECT_EQ(result.outputs[0].rgba[0], 0);
    EXPECT_EQ(roundedToHalf(1.0004f), 1.0f);
    EXPECT_EQ(roundedToHalf(65519.0f), 65504.0f);
    EXPECT_TRUE(std::isinf(roundedToHalf(65520.0f)));
    // to nearest, not toward zero: 1 + 0.75/1024 lies nearer 1 + 1/1024 than 1
    EXPECT_EQ(roundedToHalf(1.0f + 0.75f / 1024.0f), 1.0f + 1.0f / 1024.0f);
    EXPECT_EQ(roundedToHalf(-(1.0f + 0.75f / 1024.0f)), -(1.0f + 1.0f / 1024.0f));
}

TEST(ProceduralCpuEvaluator, AMipLevelIsStoredAtHalfPrecisionToo) {
    // R12: the device's box passes store each level as RGBA16F; the average 1 + 3/4096 of these four texels is
    // 1 + 1/1024 in a half
    const float a = 1.0f, b = 1.0f + 1.0f / 1024.0f;
    std::vector<float> rgba;
    for (float value : {a, b, b, b}) rgba.insert(rgba.end(), {value, value, value, 1.0f});
    const SpirvImage image(2, 2, rgba, true, true);
    ASSERT_EQ(image.mipCount(), 2);
    float texel[4];
    image.sampleBilinear(0.5f, 0.5f, 1, texel);
    EXPECT_EQ(texel[0], 1.0f + 1.0f / 1024.0f);
    const SpirvImage single(2, 2, rgba, false, true);
    EXPECT_EQ(single.mipCount(), 1) << "a Buffer without mips has one level, as on the device";
}

TEST(ProceduralCpuEvaluator, AValueEditRedrawsFromTheStepItAffects) {
    // R7 on tier 3: an edit inside an Iterate Buffer's loop starts the loop over from its initial step
    ProceduralCpuEvaluator evaluator;
    std::string error;
    ASSERT_TRUE(evaluator.prepare(iteratePlan(0.1f), error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(finish(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NEAR(result.outputs[0].rgba[0], 51, 1) << "two iterations of + 0.1";
    evaluator.setUniforms(1, UniformBlock{{0.2f, 0.0f, 0.0f, 0.0f}});
    ASSERT_TRUE(finish(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NEAR(result.outputs[0].rgba[0], 102, 1) << "two iterations of + 0.2 from the start, not on top of the last result";
}

namespace {

    bool waitForNoStep() {
        const auto start = std::chrono::steady_clock::now();
        while (ProceduralCpuEvaluator::activity().runningSteps > 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

}

TEST(ProceduralCpuEvaluator, CancelStopsARunningStepAndAValueEditStartsAgain) {
    // R11: a cancel stops the step that is shading, at its next row, not at the end of the image
    const auto library = cpuTierLibrary();
    const PassPlan plan = libraryPlan(library, chainGraph(library, {{"n", "perlin"}}), 1024);
    ASSERT_EQ(plan.steps.size(), 1u);
    const int rows = plan.steps[0].size;
    ProceduralCpuEvaluator evaluator;
    std::string error;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    ASSERT_TRUE(waitForNoStep());
    const uint64_t before = ProceduralCpuEvaluator::activity().shadedRows;
    EvaluationResult result;
    // the compile, then the step's job: slices return while the rows run on the job system
    const auto start = std::chrono::steady_clock::now();
    while (ProceduralCpuEvaluator::activity().shadedRows == before) {
        ASSERT_FALSE(evaluator.advance(1.0, result)) << "the image was done before a row was seen";
        ASSERT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(60));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    evaluator.cancel();
    ASSERT_TRUE(waitForNoStep());
    EXPECT_LT(ProceduralCpuEvaluator::activity().shadedRows - before, (uint64_t)rows) << "the stopped step shaded every row";
    ASSERT_TRUE(evaluator.advance(0.0, result));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.empty()) << "a cancel is no failure";
    EXPECT_TRUE(result.outputs.empty());
    evaluator.setUniforms(0, plan.passes[0].uniforms);
    ASSERT_TRUE(finish(evaluator, result));
    EXPECT_TRUE(result.ok) << result.error;

    // the blocking entry stops its running step the same way
    const uint64_t blockingBefore = ProceduralCpuEvaluator::activity().shadedRows;
    EXPECT_FALSE(evaluateOnCpu(plan, result, [&]() { return ProceduralCpuEvaluator::activity().shadedRows > blockingBefore; }));
    EXPECT_EQ(result.error, "Cancelled.");
    EXPECT_TRUE(result.outputs.empty());
    ASSERT_TRUE(waitForNoStep());
    EXPECT_LT(ProceduralCpuEvaluator::activity().shadedRows - blockingBefore, (uint64_t)rows);
}

TEST(ProceduralCpuEvaluator, AnOutputWhoseBufferNoStepDrawsIsAnError) {
    PassPlan plan;
    plan.size = 8;
    plan.passes = {handPass("write", "", "vec4(1.0)", false)};
    plan.buffers = {{"drawn", 8, false}, {"never", 8, false}};
    plan.steps = {{0, 0, 8}};
    plan.outputs = {{"height", 1}};
    EvaluationResult result;
    EXPECT_FALSE(evaluateOnCpu(plan, result));
    EXPECT_EQ(result.error, "output height has no buffer");
}

TEST(ProceduralCpuEvaluator, AnUnsupportedInstructionIsRefusedAtLoadNamingTheNode) {
    // R11: found when the pass loads, before any pixel, and named by its node
    PassPlan plan;
    plan.size = 8;
    PlannedPass pass = handPass("kill", "", "vec4(1.0)", false);
    pass.nodes = {"n7"};
    pass.fragment = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\nvoid main() {\n\tif (v_uv.x > 7.0) discard;\n\tg_finalColor = vec4(1.0);\n}\n";
    plan.passes = {pass};
    plan.buffers = {{"kill", 8, false}};
    plan.steps = {{0, 0, 8}};
    plan.outputs = {{"height", 0}};
    ProceduralCpuEvaluator evaluator;
    std::string error;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(finish(evaluator, result));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.unsupported);
    EXPECT_EQ(result.failedPass, 0);
    EXPECT_EQ(result.failedNode, "n7");
    EXPECT_NE(result.error.find("node n7"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("unsupported SPIR-V instruction"), std::string::npos) << result.error;
}

TEST(ProceduralCpuEvaluator, TheSessionNamesTheRefusedNodeOfAPassOfMany) {
    // two nodes inlined in one pass: the graph says which of them the CPU tier cannot run
    const auto library = refusingLibrary();
    ProceduralCpuEvaluator evaluator;
    PlanOptions options;
    options.size = 8;
    ProceduralSession session(evaluator, library, options);
    session.setGraph(chainGraph(library, {{"k1", "kill"}, {"k2", "eighth"}}), 0.0);
    const auto start = std::chrono::steady_clock::now();
    session.update(0.0, 2.0);
    while (session.error().empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        session.update(0.0, 2.0);
    }
    ASSERT_NE(session.activePlan(), nullptr) << session.error();
    ASSERT_EQ(session.activePlan()->passes.size(), 1u);
    EXPECT_EQ(session.activePlan()->passes[0].nodes.size(), 2u);
    EXPECT_NE(session.error().find("one of nodes k1, k2"), std::string::npos) << session.error();
    EXPECT_NE(session.error().find("The node is k1."), std::string::npos) << session.error();
}

TEST(ProceduralTieredEvaluator, ADeviceFailureContinuesOnTheCpuWithAWarning) {
    // R9 without a device: the pass, the device and the log are named, and the image is the CPU's, never
    // the device's earlier one
    auto* device = new FailingDevice();
    ProceduralTieredEvaluator evaluator{std::unique_ptr<ProceduralEvaluator>(device)};
    const auto library = cpuTierLibrary();
    const PassPlan plan = libraryPlan(library, chainGraph(library, {{"n", "perlin"}}), 32);
    std::string error;
    EvaluationResult result;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    ASSERT_TRUE(finish(evaluator, result));
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.outputs[0].rgba[0], 7) << "the device drew it";
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Gpu);

    device->failPass = 0;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    ASSERT_TRUE(finish(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Cpu);
    EXPECT_EQ(evaluator.deviceName(), cpuDeviceName());
    ASSERT_EQ(result.warnings.size(), 1u);
    const std::string& warning = result.warnings[0];
    EXPECT_NE(warning.find("Pass 0 (" + plan.passes[0].label + ") failed on Test GPU 9000"), std::string::npos) << warning;
    EXPECT_NE(warning.find("'dFdx' : no such function"), std::string::npos) << warning;
    EXPECT_NE(warning.find("CPU tier"), std::string::npos) << warning;
    EvaluationResult cpu;
    ASSERT_TRUE(evaluateOnCpu(plan, cpu)) << cpu.error;
    EXPECT_EQ(result.outputs[0].rgba, cpu.outputs[0].rgba);

    // D4: it stays there, and says why on every later image
    device->failPass = -1;
    const int prepares = device->prepares;
    ASSERT_TRUE(evaluator.prepare(plan, error)) << error;
    ASSERT_TRUE(finish(evaluator, result));
    EXPECT_EQ(device->prepares, prepares);
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Cpu);
    EXPECT_EQ(result.warnings.size(), 1u);
}

TEST(ProceduralTieredEvaluator, WhenTheCpuTierFailsTooTheSessionSaysWhatTheDeviceDid) {
    // R9: the device failed, then the CPU tier refused the graph; the problem names both
    auto* device = new FailingDevice();
    device->failPass = 0;
    ProceduralTieredEvaluator evaluator{std::unique_ptr<ProceduralEvaluator>(device)};
    const auto library = refusingLibrary();
    PlanOptions options;
    options.size = 8;
    ProceduralSession session(evaluator, library, options);
    session.setGraph(chainGraph(library, {{"k1", "kill"}}), 0.0);
    const auto start = std::chrono::steady_clock::now();
    session.update(0.0, 2.0);
    while (session.error().empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        session.update(0.0, 2.0);
    }
    EXPECT_NE(session.error().find("failed on Test GPU 9000"), std::string::npos) << session.error();
    EXPECT_NE(session.error().find("the CPU tier cannot run node k1"), std::string::npos) << session.error();
}
