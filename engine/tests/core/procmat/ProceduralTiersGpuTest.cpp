// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R8, R9, R12, D4 and AC4 on the headless device: which tier the context is, and what a device
// failure does. The device fails on demand through JASPER_TEST_PROCMAT_DEVICE_FAIL, read by
// ProceduralGpuEvaluator at prepare(). Mesa's reference renderers are `software` and any other device is
// `gpu` (GpuEnvironment::isReferenceRenderer), so the suite holds on the RTX lane and a desktop as here.

#include "../../../../editor/procmat/NodeLibrary.h"
#include "../../../../editor/procmat/PassPlanner.h"
#include "../../../../editor/procmat/ProceduralCpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralDevice.h"
#include "../../../../editor/procmat/ProceduralGpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralMaterialKind.h"
#include "../../../../editor/procmat/ProceduralTieredEvaluator.h"

#include "render/SystemRender.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;
using jasperengine::test::GpuEnvironment;

namespace {

    void injectDeviceFailure(const char* what) {
#if defined(_WIN32)
        _putenv_s("JASPER_TEST_PROCMAT_DEVICE_FAIL", what ? what : "");
#else
        if (what) ::setenv("JASPER_TEST_PROCMAT_DEVICE_FAIL", what, 1);
        else ::unsetenv("JASPER_TEST_PROCMAT_DEVICE_FAIL");
#endif
    }

    struct DeviceFailure {
        explicit DeviceFailure(const char* what) { injectDeviceFailure(what); }
        ~DeviceFailure() { injectDeviceFailure(nullptr); }
    };

    std::shared_ptr<const NodeLibrary> tiersLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    PassPlan tiersPlan(const std::shared_ptr<const NodeLibrary>& library, const char* type, const char* port, int size) {
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
        PlanOptions options;
        options.size = size;
        options.maxBufferSize = size;
        PassPlan plan;
        std::string error;
        EXPECT_TRUE(planGraph(file, library, {}, options, plan, error)) << error;
        return plan;
    }

    bool runTiers(ProceduralEvaluator& evaluator, EvaluationResult& result) {
        const auto start = std::chrono::steady_clock::now();
        while (!evaluator.advance(8.0, result)) {
            GpuEnvironment::drawFrames(1);
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120)) return false;
        }
        return true;
    }

    // Two outputs of the same plan as small hand-made passes: one value per pass, so a stale image shows
    PassPlan constantPlan(float value) {
        PassPlan plan;
        plan.size = 16;
        PlannedPass pass;
        pass.label = "constant";
        pass.nodes = {"n"};
        pass.fragment = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\nuniform u_fs_procmat {\n\tvec4 v[1];\n} pm;\n"
                        "void main() {\n\tg_finalColor = vec4(pm.v[0].x, v_uv.x, 0.0, 1.0);\n}\n";
        pass.uniforms.values = {value, 0.0f, 0.0f, 0.0f};
        plan.passes = {pass};
        plan.buffers = {{"constant", 16, false}};
        plan.steps = {{0, 0, 16}};
        plan.outputs = {{"height", 0}};
        return plan;
    }

}

TEST(ProceduralTiersGpu, TheDeviceTierFollowsItsRenderer) {
    JASPER_REQUIRE_GPU();
    // AC4: llvmpipe on GL and lavapipe on Vulkan are R8's tier 2; a hardware device is tier 1
    const bool software = GpuEnvironment::isReferenceRenderer();
    const EvaluatorTier expected = software ? EvaluatorTier::Software : EvaluatorTier::Gpu;
    const EvaluatorDevice device = editorDevice();
    EXPECT_EQ(device.tier, expected) << device.name;
    EXPECT_EQ(device.name, SystemRender::getDeviceName());
    if (std::strcmp(jasperengine::test::HeadlessContext::getBackendName(), "vulkan") == 0) {
        EXPECT_EQ(SystemRender::isCpuDevice(), software) << "lavapipe's Vulkan device type is the CPU, a GPU's is not";
    } else {
        EXPECT_FALSE(SystemRender::isCpuDevice()) << "GL and Metal have no device type";
    }
    std::unique_ptr<ProceduralEvaluator> evaluator = makeEditorEvaluator();
    EXPECT_EQ(evaluator->tier(), expected);
    std::printf("[gpu] procedural material device on %s: %s \"%s\"\n", jasperengine::test::HeadlessContext::getBackendName(),
                evaluatorTierName(device.tier), device.name.c_str());
}

TEST(ProceduralTiersGpu, AFailedCompileContinuesOnTheCpuWithAWarning) {
    JASPER_REQUIRE_GPU();
    const auto library = tiersLibrary();
    const PassPlan plan = tiersPlan(library, "perlin", "output", 64);
    ASSERT_FALSE(plan.passes.empty());
    DeviceFailure failure(("compile:" + plan.passes[0].label).c_str());

    std::unique_ptr<ProceduralEvaluator> evaluator = makeEditorEvaluator();
    const std::string device = evaluator->deviceName();
    std::string error;
    ASSERT_TRUE(evaluator->prepare(plan, error)) << error;
    EvaluationResult result;
    ASSERT_TRUE(runTiers(*evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(evaluator->tier(), EvaluatorTier::Cpu);
    ASSERT_EQ(result.warnings.size(), 1u);
    const std::string& warning = result.warnings[0];
    EXPECT_NE(warning.find("Pass 0 (" + plan.passes[0].label + ")"), std::string::npos) << warning;
    EXPECT_NE(warning.find(device), std::string::npos) << warning;
    EXPECT_NE(warning.find("injected by JASPER_TEST_PROCMAT_DEVICE_FAIL"), std::string::npos) << "the compiler log: " << warning;

    EvaluationResult cpu;
    ASSERT_TRUE(evaluateOnCpu(plan, cpu)) << cpu.error;
    ASSERT_EQ(result.outputs.size(), 1u);
    EXPECT_EQ(result.outputs[0].rgba, cpu.outputs[0].rgba) << "the CPU tier's image, all of it";
}

TEST(ProceduralTiersGpu, AFailureNeverLeavesTheDevicesOlderImage) {
    JASPER_REQUIRE_GPU();
    std::unique_ptr<ProceduralEvaluator> evaluator = makeEditorEvaluator();
    std::string error;
    EvaluationResult result;
    ASSERT_TRUE(evaluator->prepare(constantPlan(0.25f), error)) << error;
    ASSERT_TRUE(runTiers(*evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(evaluator->tier(), editorDevice().tier) << "the device drew it";
    EXPECT_NE(evaluator->tier(), EvaluatorTier::Cpu);
    EXPECT_NEAR(result.outputs[0].rgba[0], 64, 1);

    // the next image fails to read back: what comes is the new plan's, from the CPU tier, never 0.25's
    DeviceFailure failure("readback");
    ASSERT_TRUE(evaluator->prepare(constantPlan(0.75f), error)) << error;
    ASSERT_TRUE(runTiers(*evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(evaluator->tier(), EvaluatorTier::Cpu);
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_NE(result.warnings[0].find("cannot read height back"), std::string::npos) << result.warnings[0];
    EXPECT_NEAR(result.outputs[0].rgba[0], 191, 1);
}

TEST(ProceduralTiersGpu, ALiveEditorStaysOnTheCpuAfterAFailure) {
    JASPER_REQUIRE_GPU();
    // D4: the process cannot change its renderer, so the next graph does not go back to the device, nor
    // to a software tier
    auto* gpu = new ProceduralGpuEvaluator(EvaluatorTier::Gpu);
    ProceduralTieredEvaluator evaluator{std::unique_ptr<ProceduralEvaluator>(gpu)};
    std::string error;
    EvaluationResult result;
    {
        DeviceFailure failure("compile:constant");
        ASSERT_TRUE(evaluator.prepare(constantPlan(0.5f), error)) << error;
        ASSERT_TRUE(runTiers(evaluator, result));
        ASSERT_TRUE(result.ok) << result.error;
    }
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Cpu);
    EXPECT_EQ(evaluator.deviceEvaluator(), nullptr);
    const uint64_t compiles = gpu->compileCount();
    ASSERT_TRUE(evaluator.prepare(constantPlan(0.1f), error)) << error;
    ASSERT_TRUE(runTiers(evaluator, result));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(evaluator.tier(), EvaluatorTier::Cpu);
    EXPECT_EQ(gpu->compileCount(), compiles) << "the device is not tried again";
    EXPECT_FALSE(result.warnings.empty()) << "a later image still says why it is on the CPU";
    EXPECT_NEAR(result.outputs[0].rgba[0], 26, 1);
}

TEST(ProceduralTiersGpu, BuffersRoundToHalfOnBothTiers) {
    JASPER_REQUIRE_GPU();
    // R12: 1.0004 is 1.0 in a half, so a pass reading it back finds nothing left over; in a float it
    // would find 0.0004, which * 1000 is 102 in 8 bits
    PassPlan plan;
    plan.size = 8;
    PlannedPass write;
    write.label = "write";
    write.fragment = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\nvoid main() {\n\tg_finalColor = vec4(1.0004 + v_uv.x * 0.0);\n}\n";
    PlannedPass read;
    read.label = "read";
    read.fragment = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\nuniform texture2D t_b;\nuniform sampler s_b;\n"
                    "void main() {\n\tg_finalColor = (textureLod(sampler2D(t_b, s_b), v_uv, 0.0) - 1.0) * 1000.0;\n}\n";
    read.textures = {{"b", 0}};
    plan.passes = {write, read};
    plan.buffers = {{"b", 8, false}, {"out", 8, false}};
    plan.steps = {{0, 0, 8}, {1, 1, 8}};
    plan.outputs = {{"out", 1}};

    ProceduralGpuEvaluator device(EvaluatorTier::Software);
    std::string error;
    ASSERT_TRUE(device.prepare(plan, error)) << error;
    EvaluationResult onDevice;
    ASSERT_TRUE(runTiers(device, onDevice));
    ASSERT_TRUE(onDevice.ok) << onDevice.error << "\n" << onDevice.log;
    EvaluationResult onCpu;
    ASSERT_TRUE(evaluateOnCpu(plan, onCpu)) << onCpu.error;
    EXPECT_EQ(onDevice.outputs[0].rgba[0], 0);
    EXPECT_EQ(onCpu.outputs[0].rgba, onDevice.outputs[0].rgba);
}
