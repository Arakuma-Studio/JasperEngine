// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The noise budgets (docs/specs/T2-noise-graphs.md R46-R49, AC36), docs/perf/noise.md:
//   je-bench-noise [--json <file>]
// Kernels on one thread, in Mpts/s on Native and Scalar; whole maps and erosion on the default workers.
// check_bench.py gates them with tools/perf/bench-thresholds.json on an idle machine.

#include "BenchUtil.h"

#include "JobsTestSystem.h"

#include "Engine.h"
#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/NoiseKernels.h"
#include "terrain/HeightfieldErosion.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::noise;

namespace {

    double kernelMpts(Basis basis, int dims, Backend backend) {
        const size_t count = 4096;
        std::vector<float> x(count), y(count), z(count), out(count);
        for (size_t i = 0; i < count; i++) {
            x[i] = static_cast<float>(i) * 0.137f;
            y[i] = static_cast<float>(i) * 0.071f;
            z[i] = static_cast<float>(i) * -0.093f;
        }
        BasisParams params;
        params.basis = basis;
        params.dimensions = dims;
        params.frequency = 0.37f;
        std::vector<double> rates;
        for (int run = 0; run < 7; run++) {
            const auto start = bench::Clock::now();
            size_t points = 0;
            while (bench::secondsSince(start) < 0.05) {
                basisBatch(params, x.data(), y.data(), z.data(), count, out.data(), backend);
                points += count;
            }
            rates.push_back(static_cast<double>(points) / bench::secondsSince(start) / 1e6);
        }
        return bench::percentile(rates, 0.5);
    }

    double singlePointNs() {
        float sum = 0.0f;
        const auto start = bench::Clock::now();
        const int calls = 4000000;
        for (int i = 0; i < calls; i++) sum += perlin2(static_cast<float>(i) * 0.013f, static_cast<float>(i) * 0.029f, 3u);
        const double seconds = bench::secondsSince(start);
        volatile float keep = sum;
        (void)keep;
        return seconds * 1e9 / calls;
    }

    graph::Graph loadPreset(const char* id) {
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR) + "/editor/resources/noisegraphs/" + id + ".noisegraph", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        graph::LoadReport report;
        return graph::load(text, report);
    }

    double wholeMapSeconds(const graph::Graph& source, int size, bool parallel, const char* output = "height") {
        CompileOptions options;
        options.resolution = size;
        options.maxHeight = 800.0f;
        const CompileResult compiled = compile(source, options);
        if (!compiled.graph) return 1e9;
        GridDomain domain;
        domain.width = domain.depth = size;
        domain.unitsPerTexel = 1.0 / static_cast<double>(size);
        EvalContext context;
        context.maxHeight = 800.0f;
        context.metresPerTexel = 2000.0 / static_cast<double>(size);
        context.parallel = parallel;
        std::vector<float> values(static_cast<size_t>(size) * static_cast<size_t>(size));
        std::vector<double> times;
        for (int run = 0; run < 3; run++) {
            const auto start = bench::Clock::now();
            compiled.graph->evaluateGrid(domain, context, compiled.graph->outputIndex(output), values.data());
            times.push_back(bench::secondsSince(start));
        }
        return bench::percentile(times, 0.5);
    }

    // A chain with fan-in, of the shape a person's graph has, n nodes long
    graph::Graph graphOfNodes(int nodes) {
        graph::Graph out;
        out.kind = "noise";
        std::string last;
        for (int i = 0; i < nodes - 1; i++) {
            graph::Node node;
            node.id = out.allocateId("n");
            if (i == 0) {
                node.type = "perlin";
            } else {
                node.type = "scale_offset";
                node.params = {{"scale", 0.99}, {"offset", 0.001}};
                graph::Edge edge;
                edge.id = out.allocateId("e");
                edge.from.node = last;
                edge.from.port = "out";
                edge.to.node = node.id;
                edge.to.port = "in";
                out.edges.push_back(edge);
            }
            last = node.id;
            out.nodes.push_back(node);
        }
        graph::Node output;
        output.id = out.allocateId("n");
        output.type = "output_value";
        output.params = {{"name", "value"}};
        graph::Edge edge;
        edge.id = out.allocateId("e");
        edge.from.node = last;
        edge.from.port = "out";
        edge.to.node = output.id;
        edge.to.port = "in";
        out.edges.push_back(edge);
        out.nodes.push_back(output);
        return out;
    }

    double compileMs(int nodes) {
        const graph::Graph source = graphOfNodes(nodes);
        std::vector<double> times;
        for (int run = 0; run < 15; run++) {
            const auto start = bench::Clock::now();
            const CompileResult result = compile(source);
            times.push_back(bench::secondsSince(start) * 1000.0);
            if (!result.graph) return 1e9;
        }
        return bench::percentile(times, 0.5);
    }

    std::vector<float> erosionGround(int size) {
        FractalParams params;
        params.basis.frequency = 4.0f;
        params.octaves = 5;
        std::vector<float> h(static_cast<size_t>(size) * size);
        std::vector<float> x(h.size()), z(h.size());
        for (int j = 0; j < size; j++) {
            for (int i = 0; i < size; i++) {
                x[static_cast<size_t>(j) * size + i] = (static_cast<float>(i) + 0.5f) / static_cast<float>(size);
                z[static_cast<size_t>(j) * size + i] = (static_cast<float>(j) + 0.5f) / static_cast<float>(size);
            }
        }
        fbmBatch(params, x.data(), nullptr, z.data(), h.size(), h.data());
        for (float& value : h) value = (value + 1.0f) * 0.5f;
        return h;
    }

}

int main(int argc, char** argv) {
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    registerNoiseKind();
    bench::Results results("je-bench-noise");

    const struct { Basis basis; const char* name; } bases[] = {
        {Basis::Value, "value"}, {Basis::Perlin, "perlin"}, {Basis::Simplex, "simplex"}, {Basis::Cellular, "cellular"}};
    for (const auto& basis : bases) {
        for (int dims : {2, 3}) {
            const std::string name = std::string(basis.name) + std::to_string(dims) + "d";
            results.add("kernel_native_" + name + "_mpts", kernelMpts(basis.basis, dims, Backend::Native), "Mpts/s");
            results.add("kernel_scalar_" + name + "_mpts", kernelMpts(basis.basis, dims, Backend::Scalar), "Mpts/s");
        }
    }
    results.add("single_point_perlin2_ns", singlePointNs(), "ns");

    const graph::Graph mountains = loadPreset("mountains");
    results.add("mountains_1024_one_thread_s", wholeMapSeconds(mountains, 1024, false), "s");
    results.add("mountains_1024_workers_s", wholeMapSeconds(mountains, 1024, true), "s");
    results.add("mountains_4096_workers_s", wholeMapSeconds(mountains, 4096, true), "s");
    results.add("alpine_1024_workers_s", wholeMapSeconds(loadPreset("alpine"), 1024, true), "s");

    // R48, one thread
    {
        const int size = 513;
        const std::vector<float> start = erosionGround(size);
        terrain::ErosionRegion region;
        region.width = region.depth = size;
        region.maxHeight = 400.0f;
        region.metresPerTexel = 4.0;
        terrain::ErosionControl serial;
        serial.parallel = false;
        std::vector<float> h = start;
        terrain::HydraulicParams hydraulic;
        uint64_t steps = 0;
        auto begin = bench::Clock::now();
        terrain::HeightfieldErosion::hydraulic(hydraulic, region, h.data(), nullptr, nullptr, nullptr, serial, &steps);
        results.add("hydraulic_mega_steps_per_s", static_cast<double>(steps) / bench::secondsSince(begin) / 1e6, "M/s");
        h = start;
        terrain::ThermalParams thermal;
        thermal.iterations = 16;
        begin = bench::Clock::now();
        terrain::HeightfieldErosion::thermal(thermal, region, h.data(), serial);
        results.add("thermal_mega_texel_iterations_per_s", static_cast<double>(size) * size * thermal.iterations / bench::secondsSince(begin) / 1e6, "M/s");
    }

    results.add("compile_64_nodes_ms", compileMs(64), "ms");
    results.add("compile_1000_nodes_ms", compileMs(1000), "ms");

    // R49: every node's preview of a 32-node graph at 64 texels, on one worker
    {
        CompileOptions options;
        options.previewNodes = true;
        const CompileResult compiled = compile(graphOfNodes(32), options);
        if (compiled.graph) {
            GridDomain domain;
            domain.width = domain.depth = 64;
            domain.unitsPerTexel = 1.0 / 64.0;
            EvalContext context;
            context.parallel = false;
            const size_t outputs = compiled.graph->outputs().size();
            std::vector<std::vector<float>> values(outputs, std::vector<float>(64u * 64u));
            std::vector<int> indices;
            std::vector<float*> targets;
            for (size_t i = 0; i < outputs; i++) {
                indices.push_back(static_cast<int>(i));
                targets.push_back(values[i].data());
            }
            std::vector<double> times;
            for (int run = 0; run < 9; run++) {
                const auto begin = bench::Clock::now();
                compiled.graph->evaluateGridOutputs(domain, context, indices, targets);
                times.push_back(bench::secondsSince(begin) * 1000.0);
            }
            results.add("preview_32_nodes_64px_ms", bench::percentile(times, 0.5), "ms");
        }
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-noise.json");
    results.write(path);
    return 0;
}
