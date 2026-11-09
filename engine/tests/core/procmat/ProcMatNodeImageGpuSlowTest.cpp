// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R20 and AC8: a node ships when every output of every variant draws the same image on the device as
// on the CPU tier (ProceduralCpuEvaluator), within AC8. Each variant is planned and evaluated for real, so a Buffer feeds the
// pass reading it, an Iterate Buffer iterates and an RGBA16F target meets the interpreter's half rounding,
// which M0's sweep, with one probe image for every texture, could not show.
//
// JASPER_PROCMAT_IMAGE_NODES=a,b narrows it to some nodes, and JASPER_PROCMAT_IMAGE_REPORT=<file> writes
// one line per output. A node that fails goes in NodeLibrary::leftOutNodes and the spec's Deviations.

#include "../../../../editor/procmat/NodeLibrary.h"
#include "../../../../editor/procmat/PassPlanner.h"
#include "../../../../editor/procmat/ProceduralGpuEvaluator.h"
#include "../../../../editor/procmat/ProceduralCpuEvaluator.h"
#include "../../../../editor/tests/procmat/ProcMatImageDifference.h"
#include "../../../../editor/tests/procmat/ProcMatLibraryVariants.h"

#include "render/SystemRender.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;
using namespace jasperengine::editor::procmat::test;
using jasperengine::test::GpuEnvironment;

namespace {

    constexpr int IMAGE_SIZE = 128;
    // A library Buffer asks for up to 2048: a preview's cap keeps the interpreter's loops to minutes.
    // The two tiers run the same plan, so what is compared does not change with it.
    constexpr int BUFFER_CAP = 64;

    struct OutputRow {
        std::string node, variant, output, failure;
        ImageDifference difference;
    };

    std::set<std::string> nodeFilter() {
        std::set<std::string> nodes;
        if (const char* value = std::getenv("JASPER_PROCMAT_IMAGE_NODES")) {
            std::stringstream list(value);
            std::string id;
            while (std::getline(list, id, ',')) nodes.insert(id);
        }
        return nodes;
    }

    bool evaluateOnDevice(ProceduralEvaluator& evaluator, const PassPlan& plan, EvaluationResult& result, std::string& error) {
        if (!evaluator.prepare(plan, error)) return false;
        const auto start = std::chrono::steady_clock::now();
        while (!evaluator.advance(100.0, result)) {
            GpuEnvironment::drawFrames(1);
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(5)) {
                error = "the device took more than five minutes";
                return false;
            }
        }
        if (!result.ok) error = result.error + "\n" + result.log;
        return result.ok;
    }

    std::vector<OutputRow> compareNode(const std::shared_ptr<const NodeLibrary>& library, const std::string& id, const Json& descriptor) {
        std::vector<OutputRow> rows;
        ProceduralGpuEvaluator evaluator(EvaluatorTier::Software);
        for (const NodeVariant& variant : variantsOf(descriptor)) {
            Expander expander(library);
            expander.maxBufferSize = BUFFER_CAP;
            OutputRow base;
            base.node = id;
            base.variant = variant.name;
            std::vector<PlanSource> roots;
            try {
                const NodeInstance instance = instantiateVariant(expander, id, descriptor, variant);
                for (size_t port = 0; port < instance.outputs.size(); port++) {
                    const auto& output = instance.outputs[port];
                    if (output && !output->graphInput) roots.push_back({"o" + std::to_string(port), *output});
                }
            } catch (const std::exception& e) {
                base.failure = e.what();
                rows.push_back(base);
                continue;
            }
            if (roots.empty()) continue;
            PlanOptions options;
            options.size = IMAGE_SIZE;
            options.maxBufferSize = BUFFER_CAP;
            PassPlan plan;
            std::string error;
            if (!planPasses(expander, roots, options, plan, error)) {
                base.failure = "plan: " + error;
                rows.push_back(base);
                continue;
            }
            EvaluationResult device;
            if (!evaluateOnDevice(evaluator, plan, device, error)) {
                base.failure = "device: " + error;
                rows.push_back(base);
                continue;
            }
            EvaluationResult reference;
            if (!evaluateOnCpu(plan, reference)) {
                base.failure = "interpreter: " + reference.error;
                rows.push_back(base);
                continue;
            }
            for (size_t i = 0; i < reference.outputs.size(); i++) {
                OutputRow row = base;
                row.output = reference.outputs[i].name;
                row.difference = differenceOf(device.outputs[i].rgba, reference.outputs[i].rgba);
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }

    std::shared_ptr<const NodeLibrary> repositoryLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    bool rowsFail(const std::vector<OutputRow>& rows) {
        for (const OutputRow& row : rows) {
            if (!row.failure.empty() || !row.difference.withinAc8()) return true;
        }
        return false;
    }

}

TEST(ProcMatNodeImageGpuSlow, EveryShippedNodeDrawsWhatTheInterpreterDraws) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const std::set<std::string> filter = nodeFilter();
    std::ofstream report;
    if (const char* path = std::getenv("JASPER_PROCMAT_IMAGE_REPORT")) report.open(path);
    int nodes = 0, outputs = 0;
    double worstMean = 0.0;
    int worstP99 = 0;
    for (const auto& [id, descriptor] : library->descriptors()) {
        if (NodeLibrary::leftOutReason(id) || (!filter.empty() && filter.count(id) == 0)) continue;
        nodes++;
        for (const OutputRow& row : compareNode(library, id, descriptor)) {
            outputs++;
            if (report.is_open()) {
                report << row.node << " " << row.variant << " " << row.output << " " << row.difference.mean << " " << row.difference.p99 << " " << row.difference.maximum << " " << row.failure << "\n";
                report.flush();
            }
            worstMean = std::max(worstMean, row.difference.mean);
            worstP99 = std::max(worstP99, row.difference.p99);
            EXPECT_TRUE(row.failure.empty()) << row.node << " " << row.variant << ": " << row.failure;
            EXPECT_TRUE(row.difference.withinAc8()) << row.node << " " << row.variant << " " << row.output << ": " << row.difference.mean << " mean, " << row.difference.p99 << " at p99";
        }
    }
    std::printf("[gpu] node images on %s: %d nodes, %d outputs, worst %.3f mean, %d at p99\n", jasperengine::test::HeadlessContext::getRendererName(), nodes, outputs, worstMean, worstP99);
    EXPECT_GT(outputs, 0) << "the filter matched nothing";
}

TEST(ProcMatNodeImageGpuSlow, EveryLeftOutNodeStillFails) {
    JASPER_REQUIRE_GPU();
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    // a node that passes again belongs back in the library: nothing else would say so
    for (const LeftOutNode& node : NodeLibrary::leftOutNodes()) {
        const Json* descriptor = library->descriptor(node.id);
        ASSERT_NE(descriptor, nullptr) << node.id << " is not in the library any more";
        EXPECT_TRUE(rowsFail(compareNode(library, node.id, *descriptor))) << node.id << " passes now: " << node.reason;
    }
}

TEST(ProcMatNodeImageGpuSlow, TheOldFloatHashIsOverTheLimit) {
    JASPER_REQUIRE_GPU();
    // AC8's red control: Material Maker's own rand, a float hash, put back into the library. Its pattern
    // differs between tiers, which the limit must see, or the passing test above proves nothing (R10)
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes")) {
        if (entry.path().extension() != ".json" || entry.path().filename() == "_manifest.json") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        files.emplace_back(entry.path().filename().string(), std::string(std::istreambuf_iterator<char>(in), {}));
    }
    const std::vector<std::pair<const char*, std::vector<std::string>>> floatRand = {
        {"rand", {"float rand(vec2 x) {", " return fract(cos(mod(dot(x, vec2(13.9898, 8.141)), 3.14)) * 43758.5);", "}"}},
        {"rand2", {"vec2 rand2(vec2 x) {", " return fract(cos(mod(vec2(dot(x, vec2(13.9898, 8.141)),dot(x, vec2(3.4562, 17.398))), vec2(3.14, 3.14))) * 43758.5);", "}"}},
        {"rand3", {"vec3 rand3(vec2 x) {", " return fract(cos(mod(vec3(dot(x, vec2(13.9898, 8.141)),dot(x, vec2(3.4562, 17.398)),dot(x, vec2(13.254, 5.867))), vec3(3.14, 3.14, 3.14))) * 43758.5);", "}"}},
    };
    bool replaced = false;
    for (auto& [name, text] : files) {
        if (name != "_functions.json") continue;
        Json functions = Json::parse(text);
        for (const auto& [id, code] : floatRand) {
            ASSERT_TRUE(functions["functions"].contains(id)) << id;
            functions["functions"][id]["code"] = code;
            functions["functions"][id]["deps"] = Json::array();
        }
        text = functions.dump();
        replaced = true;
    }
    ASSERT_TRUE(replaced);
    std::string error;
    const auto library = NodeLibrary::fromFiles(files, error);
    ASSERT_TRUE(library) << error;
    int over = 0;
    for (const char* id : {"perlin", "noise_white"}) {
        const Json* descriptor = library->descriptor(id);
        ASSERT_NE(descriptor, nullptr) << id;
        for (const OutputRow& row : compareNode(library, id, *descriptor)) {
            ASSERT_TRUE(row.failure.empty()) << row.node << ": " << row.failure;
            if (!row.difference.withinAc8()) over++;
            std::printf("[gpu] float hash: %s %s %s %.3f mean, %d at p99\n", row.node.c_str(), row.variant.c_str(), row.output.c_str(), row.difference.mean, row.difference.p99);
        }
    }
    EXPECT_GT(over, 0) << "the float hash drew within AC8 on both tiers: the comparison cannot see a different pattern";
}
