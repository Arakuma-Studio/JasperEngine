// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pinned here too: this unit's own arithmetic is part of what the table holds, and the test project
// builds it with its target's default flags (R38). GCC takes -ffp-contract=off from its build.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "NoiseGoldenCases.h"

#include "graph/Graph.h"
#include "noise/NoiseCompat.h"
#include "noise/NoiseDetMath.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/NoiseKernels.h"
#include "terrain/HeightfieldErosion.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace jasperengine { namespace noise { namespace golden {

    namespace {

        using Cases = std::vector<std::pair<std::string, uint64_t>>;
        using graph::Json;

        constexpr int kGridSize = 64;

        // Deterministic points, from integers: rand() and friends differ per library
        std::vector<float> scatteredPoints(uint32_t seed, size_t count, float span){
            std::vector<float> values(count);
            uint32_t state = seed;
            for (size_t i = 0; i < count; i++){
                state = state * 1664525u + 1013904223u;
                const uint32_t bits = (state >> 8) & 0xFFFFFFu;
                values[i] = (static_cast<float>(static_cast<int32_t>(bits) - 0x800000) / 8388608.0f) * span;
            }
            return values;
        }

        std::vector<float> extremeCoords(){
            const float inf = std::numeric_limits<float>::infinity();
            const float nan = std::numeric_limits<float>::quiet_NaN();
            return {0.0f, -0.0f, 1e-40f, -1e-40f, 1e-30f, -1e-30f, 1e-5f, -1e-5f, 0.5f, -0.5f, 1.0f, -1.0f,
                    255.5f, 256.0f, 65535.5f, 65535.996f, 65536.0f, 65536.5f, -65536.0f, -65536.5f, 131072.25f, 8388608.0f, 16777216.0f,
                    1e10f, -1e10f, 1e30f, -1e30f, 3e38f, -3e38f, inf, -inf, nan};
        }

        void addBasisCases(Cases& out, Backend backend){
            const std::vector<float> xs = scatteredPoints(11u, 1027, 300.0f);
            const std::vector<float> ys = scatteredPoints(23u, 1027, 300.0f);
            const std::vector<float> zs = scatteredPoints(37u, 1027, 300.0f);
            const std::vector<float> extreme = extremeCoords();
            std::vector<float> xe, ye, ze;
            for (float a : extreme){
                for (float b : extreme){
                    xe.push_back(a);
                    ye.push_back(b);
                    ze.push_back(b);
                }
            }
            std::vector<float> result;
            const auto hashBatch = [&](const BasisParams& params, const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z){
                result.resize(x.size());
                basisBatch(params, x.data(), y.data(), z.data(), x.size(), result.data(), backend);
                return hashFloats(result.data(), result.size());
            };
            const struct { Basis basis; const char* name; } bases[] = {
                {Basis::Perlin, "perlin"}, {Basis::Simplex, "simplex"}, {Basis::Value, "value"}, {Basis::Cellular, "cellular"}};
            for (const auto& basis : bases){
                for (int dims : {2, 3}){
                    BasisParams params;
                    params.basis = basis.basis;
                    params.dimensions = dims;
                    params.frequency = 0.37f;
                    params.seed = 1234u;
                    const std::string base = std::string("kernel/") + basis.name + (dims == 2 ? "2" : "3");
                    out.emplace_back(base, hashBatch(params, xs, ys, zs));
                    params.frequency = 1.0f;
                    out.emplace_back(base + "/extreme", hashBatch(params, xe, ye, ze));
                    params.seed = 0xFFFFFFFFu;
                    out.emplace_back(base + "/seed_max", hashBatch(params, xs, ys, zs));
                }
            }
            for (int dims : {2, 3}){
                const struct { CellularMetric metric; const char* name; } metrics[] = {
                    {CellularMetric::Euclidean, "euclidean"}, {CellularMetric::Manhattan, "manhattan"}, {CellularMetric::Chebyshev, "chebyshev"}};
                const struct { CellularReturn value; const char* name; } returns[] = {
                    {CellularReturn::F1, "f1"}, {CellularReturn::F2, "f2"}, {CellularReturn::F2MinusF1, "f2_minus_f1"}, {CellularReturn::CellValue, "cell_value"}};
                for (const auto& metric : metrics){
                    for (const auto& ret : returns){
                        BasisParams params;
                        params.basis = Basis::Cellular;
                        params.dimensions = dims;
                        params.frequency = 0.37f;
                        params.seed = 77u;
                        params.metric = metric.metric;
                        params.cellReturn = ret.value;
                        params.jitter = 0.75f;
                        out.emplace_back(std::string("kernel/cellular") + (dims == 2 ? "2/" : "3/") + metric.name + "/" + ret.name, hashBatch(params, xs, ys, zs));
                    }
                }
            }
            for (const auto& basis : bases){
                FractalParams fractal;
                fractal.basis.basis = basis.basis;
                fractal.basis.frequency = 0.11f;
                fractal.basis.seed = 9u;
                fractal.octaves = 7;
                fractal.lacunarity = 2.13f;
                fractal.gain = 0.47f;
                for (int dims : {2, 3}){
                    fractal.basis.dimensions = dims;
                    result.resize(xs.size());
                    fbmBatch(fractal, xs.data(), ys.data(), zs.data(), xs.size(), result.data(), backend);
                    out.emplace_back(std::string("fractal/fbm/") + basis.name + (dims == 2 ? "2" : "3"), hashFloats(result.data(), result.size()));
                    ridgedBatch(fractal, xs.data(), ys.data(), zs.data(), xs.size(), result.data(), backend);
                    out.emplace_back(std::string("fractal/ridged/") + basis.name + (dims == 2 ? "2" : "3"), hashFloats(result.data(), result.size()));
                }
            }
        }

        void addDetMathCases(Cases& out){
            typedef detmath::DetMath<Lane1> M;
            std::vector<float> values;
            for (int i = -4096; i <= 4096; i++) values.push_back(static_cast<float>(i) * 0.0123f);
            for (float e : extremeCoords()) values.push_back(e);
            std::vector<float> result(values.size());
            const auto run = [&](const char* name, float (*fn)(float)){
                for (size_t i = 0; i < values.size(); i++) result[i] = fn(values[i]);
                out.emplace_back(std::string("detmath/") + name, hashFloats(result.data(), result.size()));
            };
            run("sin", [](float v){ return M::sin(v); });
            run("cos", [](float v){ return M::cos(v); });
            run("tan", [](float v){ return M::tan(v); });
            run("exp2", [](float v){ return M::exp2(v); });
            run("log2", [](float v){ return M::log2(v); });
            run("pow", [](float v){ return M::pow(v, 1.7f); });
        }

        void addCompatCases(Cases& out){
            std::vector<float> result;
            for (int z = 0; z < kGridSize; z++){
                for (int x = 0; x < kGridSize; x++){
                    result.push_back(compat::value(static_cast<float>(x) * 0.37f, static_cast<float>(z) * 0.37f, 7u));
                    result.push_back(compat::fractal(static_cast<float>(x) * 0.11f, static_cast<float>(z) * 0.11f, 7u, 6));
                    result.push_back(compat::hash01(x, z, 7u));
                }
            }
            out.emplace_back("compat/value_fractal_hash", hashFloats(result.data(), result.size()));
        }

        uint64_t hashGraph(const graph::Graph& graph, Backend backend, const char* outputName, float maxHeight = 200.0f, const float* snapshot = nullptr){
            const CompileResult compiled = compile(graph);
            if (!compiled.graph) return 0;
            int output = 0;
            if (outputName) output = compiled.graph->outputIndex(outputName);
            std::vector<float> values(static_cast<size_t>(kGridSize) * static_cast<size_t>(kGridSize));
            GridDomain domain;
            domain.width = kGridSize;
            domain.depth = kGridSize;
            domain.unitsPerTexel = 1.0 / static_cast<double>(kGridSize);
            EvalContext context;
            context.seed = 1234u;
            context.maxHeight = maxHeight;
            context.metresPerTexel = 1000.0 / static_cast<double>(kGridSize);
            context.backend = backend;
            context.parallel = false;
            if (snapshot){
                context.terrainHeight = snapshot;
                context.terrainHeightWidth = kGridSize;
                context.terrainHeightDepth = kGridSize;
            }
            if (!compiled.graph->evaluateGrid(domain, context, output, values.data())) return 0;
            return hashFloats(values.data(), values.size());
        }

        std::vector<float> rampSnapshot(){
            std::vector<float> snapshot(static_cast<size_t>(kGridSize) * static_cast<size_t>(kGridSize));
            for (int z = 0; z < kGridSize; z++){
                for (int x = 0; x < kGridSize; x++){
                    snapshot[static_cast<size_t>(z) * kGridSize + static_cast<size_t>(x)] = (perlin2(static_cast<float>(x) * 0.1f, static_cast<float>(z) * 0.1f, 21u) + 1.0f) * 0.5f;
                }
            }
            return snapshot;
        }

        // A graph of sources a, b and m (in 0..1), the node under test, and one Value Output on it
        graph::Graph nodeGraph(const std::string& type, const Json& params, std::vector<std::pair<std::string, std::string>> inputs, const std::string& fromPort = "out", bool toCoordNoise = false, const char* coordNoise = "perlin"){
            Json nodes = Json::array();
            Json edges = Json::array();
            int edge = 100;
            const auto addEdge = [&](const std::string& from, const std::string& port, const std::string& to, const std::string& toPort){
                edges.push_back({{"id", "e" + std::to_string(edge++)}, {"from", {{"node", from}, {"port", port}}}, {"to", {{"node", to}, {"port", toPort}}}});
            };
            const auto addNode = [&](const std::string& id, const std::string& nodeType, const Json& nodeParams){
                nodes.push_back({{"id", id}, {"type", nodeType}, {"position", {0.0, 0.0}}, {"params", nodeParams}});
            };
            addNode("n1", "perlin", {{"frequency", 3.0}});
            addNode("n2", "simplex", {{"frequency", 5.0}, {"seed_offset", 3}});
            addNode("n3", "value", {{"frequency", 2.0}, {"seed_offset", 5}});
            addNode("n4", "remap", {{"from_min", -1.0}, {"from_max", 1.0}, {"to_min", 0.0}, {"to_max", 1.0}, {"clamp", true}});
            addEdge("n3", "out", "n4", "in");
            addNode("n5", type, params);
            for (const auto& [port, source] : inputs) {
                addEdge(source, "out", "n5", port);
            }
            if (toCoordNoise) {
                addNode("n6", coordNoise, {{"frequency", 2.0}});
                addEdge("n5", "out", "n6", "coord");
                addNode("n7", "output_value", {{"name", "result"}});
                addEdge("n6", "out", "n7", "in");
            } else if (type.rfind("output_", 0) != 0) {
                addNode("n7", "output_value", {{"name", "result"}});
                addEdge("n5", fromPort, "n7", "in");
            }
            Json root = {{"formatVersion", 1}, {"kind", "noise"}, {"kindVersion", 1}, {"nextId", 200}, {"nodes", nodes}, {"edges", edges}};
            graph::LoadReport report;
            return graph::fromJson(root, report);
        }

        void addNodeCases(Cases& out, Backend backend){
            const auto add = [&](const std::string& name, const graph::Graph& graph, const char* output = "result", const float* snapshot = nullptr){
                out.emplace_back(nodeCaseName(name), hashGraph(graph, backend, output, 200.0f, snapshot));
            };
            add("position", nodeGraph("position", Json::object(), {}, "out", true));
            add("terrain_uv", nodeGraph("terrain_uv", Json::object(), {}, "u"));
            add("constant", nodeGraph("constant", {{"value", 0.625}}, {}));
            {
                graph::Graph graph = nodeGraph("variable", {{"name", "v"}}, {});
                graph::Variable variable;
                variable.name = "v";
                variable.type = "float";
                variable.defaultValue = 0.375;
                variable.exposed = true;
                graph.variables.push_back(variable);
                add("variable", graph);
            }
            const std::vector<float> snapshot = rampSnapshot();
            add("terrain_height", nodeGraph("terrain_height", Json::object(), {}), "result", snapshot.data());
            add("perlin", nodeGraph("perlin", {{"frequency", 7.0}, {"seed_offset", 11}}, {}));
            add("simplex", nodeGraph("simplex", {{"dimensions", "3d"}, {"frequency", 7.0}}, {}));
            add("value", nodeGraph("value", {{"frequency", 7.0}}, {}));
            add("cellular", nodeGraph("cellular", {{"frequency", 7.0}, {"return", "f2_minus_f1"}, {"jitter", 0.5}}, {}));
            add("fbm", nodeGraph("fbm", {{"octaves", 6}, {"frequency", 4.0}}, {}));
            add("ridged", nodeGraph("ridged", {{"octaves", 6}, {"frequency", 4.0}, {"basis", "simplex"}}, {}));
            add("domain_warp", nodeGraph("domain_warp", {{"octaves", 3}, {"amplitude", 0.2}}, {{"strength", "n4"}}, "out", true));
            add("domain_transform", nodeGraph("domain_transform", {{"scale", {2.0, 2.0, 3.0}}, {"offset", {0.5, 0.0, -0.25}}, {"rotation_y", 33.0}}, {}, "out", true));
            add("scale_offset", nodeGraph("scale_offset", {{"scale", 0.75}, {"offset", 0.125}}, {{"in", "n1"}}));
            const char* mathOps[] = {"add", "subtract", "multiply", "divide", "min", "max", "power", "modulo", "abs", "negate", "one_minus", "sqrt", "floor", "frac", "sin", "cos"};
            add("math", nodeGraph("math", {{"op", "add"}}, {{"a", "n1"}, {"b", "n2"}}));
            for (const char* op : mathOps) {
                out.emplace_back(std::string("math/") + op, hashGraph(nodeGraph("math", {{"op", op}}, {{"a", "n1"}, {"b", "n2"}}), backend, "result"));
            }
            add("clamp", nodeGraph("clamp", {{"min", -0.25}, {"max", 0.5}}, {{"in", "n1"}}));
            add("remap", nodeGraph("remap", {{"from_min", -0.5}, {"from_max", 0.5}, {"to_min", 2.0}, {"to_max", 5.0}, {"clamp", false}}, {{"in", "n1"}}));
            add("curve", nodeGraph("curve", {{"keys", {{-1.0, 0.0, 0.0, 2.0}, {0.0, 0.75, 0.5, 0.5}, {1.0, 0.25, -1.0, 0.0}}}}, {{"in", "n1"}}));
            add("terrace", nodeGraph("terrace", {{"steps", 9}, {"smoothness", 0.3}}, {{"in", "n4"}}));
            add("select", nodeGraph("select", {{"threshold", 0.5}, {"falloff", 0.1}}, {{"a", "n1"}, {"b", "n2"}, {"mask", "n4"}}));
            add("blend", nodeGraph("blend", Json::object(), {{"a", "n1"}, {"b", "n2"}, {"weight", "n4"}}));
            add("height_mask", nodeGraph("height_mask", {{"min", 0.25}, {"max", 0.75}, {"falloff", 0.1}}, {{"in", "n4"}}));
            add("slope_mask", nodeGraph("slope_mask", {{"min_angle", 5.0}, {"max_angle", 25.0}, {"falloff", 3.0}}, {{"in", "n4"}}));
            add("hydraulic_erosion", nodeGraph("hydraulic_erosion", {{"droplets_per_texel", 0.5}, {"lifetime", 16}}, {{"height", "n4"}}));
            {
                graph::Graph flow = nodeGraph("hydraulic_erosion", {{"droplets_per_texel", 0.5}, {"lifetime", 16}}, {{"height", "n4"}, {"hardness", "n4"}}, "flow");
                out.emplace_back("erosion/hydraulic/flow", hashGraph(flow, backend, "result"));
                graph::Graph sediment = nodeGraph("hydraulic_erosion", {{"droplets_per_texel", 0.5}, {"lifetime", 16}}, {{"height", "n4"}}, "sediment");
                out.emplace_back("erosion/hydraulic/sediment", hashGraph(sediment, backend, "result"));
            }
            add("thermal_erosion", nodeGraph("thermal_erosion", {{"iterations", 4}, {"talus_angle", 20.0}}, {{"height", "n4"}}));
            add("output_height", nodeGraph("output_height", {{"unit", "metres"}}, {{"in", "n1"}}), nullptr);
            add("output_layer", nodeGraph("output_layer", {{"layer", 2}}, {{"weight", "n4"}}), nullptr);
            add("output_density", nodeGraph("output_density", {{"foliage_layer", 1}}, {{"density", "n4"}}), nullptr);
            add("output_value", nodeGraph("output_value", {{"name", "custom"}}, {{"in", "n2"}}), nullptr);
        }

        void addNumericCases(Cases& out, Backend backend){
            const auto runMath = [&](const char* name, const char* op, double a, double b, const char* output = "result"){
                // Constants fold, so feed the rule through a run-time zero-span variable path too
                graph::Graph graph = nodeGraph("math", {{"op", op}, {"b", b}}, {{"a", "n8"}});
                Json node = {{"id", "n8"}, {"type", "scale_offset"}, {"position", {0.0, 0.0}}, {"params", {{"scale", 0.0}, {"offset", a}}}};
                graph.nodes.push_back(graph::nodeFromJson(node));
                Json edge = {{"id", "e180"}, {"from", {{"node", "n1"}, {"port", "out"}}}, {"to", {{"node", "n8"}, {"port", "in"}}}};
                graph.edges.push_back(graph::edgeFromJson(edge));
                out.emplace_back(std::string("numeric/") + name, hashGraph(graph, backend, output));
            };
            runMath("divide_by_zero", "divide", 3.0, 0.0);
            runMath("sqrt_negative", "sqrt", -2.0, 0.0);
            runMath("pow_negative_base", "power", -2.0, 0.5);
            runMath("modulo_by_zero", "modulo", 3.0, 0.0);
            // A huge domain scale reaches the wrap, and non-finite coordinates are coordinate 0 (R6)
            const graph::Graph huge = nodeGraph("domain_transform", {{"scale", {1e30, 1.0, 1e30}}}, {}, "out", true, "value");
            out.emplace_back("numeric/huge_scale_coordinates", hashGraph(huge, backend, "result"));
            const graph::Graph inf = nodeGraph("domain_transform", {{"scale", {1e38, 1.0, 1e38}}, {"offset", {1e38, 0.0, 1e38}}}, {}, "out", true, "value");
            out.emplace_back("numeric/overflowing_coordinates", hashGraph(inf, backend, "result"));
        }

        void addErosionCases(Cases& out){
            std::vector<float> height(static_cast<size_t>(129) * 129);
            for (int z = 0; z < 129; z++){
                for (int x = 0; x < 129; x++){
                    height[static_cast<size_t>(z) * 129 + static_cast<size_t>(x)] = (perlin2(static_cast<float>(x) * 0.05f, static_cast<float>(z) * 0.05f, 3u) + 1.0f) * 0.5f;
                }
            }
            terraincore::ErosionRegion region;
            region.startX = 17;
            region.startZ = -9;
            region.width = 129;
            region.depth = 129;
            region.maxHeight = 200.0f;
            region.metresPerTexel = 4.0;
            terraincore::ErosionControl control;
            control.parallel = false;
            std::vector<float> hydraulic = height, flow(height.size()), sediment(height.size());
            terraincore::HydraulicParams params;
            params.seed = 5u;
            terraincore::HeightfieldErosion::hydraulic(params, region, hydraulic.data(), nullptr, flow.data(), sediment.data(), control);
            out.emplace_back("erosion/direct/hydraulic", hashFloats(hydraulic.data(), hydraulic.size()));
            out.emplace_back("erosion/direct/flow", hashFloats(flow.data(), flow.size()));
            out.emplace_back("erosion/direct/sediment", hashFloats(sediment.data(), sediment.size()));
            std::vector<float> thermal = height;
            terraincore::ThermalParams thermalParams;
            terraincore::HeightfieldErosion::thermal(thermalParams, region, thermal.data(), control);
            out.emplace_back("erosion/direct/thermal", hashFloats(thermal.data(), thermal.size()));
        }

        void addPresetCases(Cases& out, Backend backend, const std::vector<Preset>& presets){
            const std::vector<float> snapshot = rampSnapshot();
            for (const Preset& preset : presets){
                graph::LoadReport report;
                const graph::Graph graph = graph::load(preset.json, report);
                const CompileResult compiled = compile(graph);
                const std::string base = "preset/" + preset.id;
                if (!compiled.graph){
                    out.emplace_back(base, 0);
                    continue;
                }
                GridDomain domain;
                domain.width = kGridSize;
                domain.depth = kGridSize;
                domain.unitsPerTexel = 1.0 / static_cast<double>(kGridSize);
                EvalContext context;
                context.seed = 7u;
                context.maxHeight = 400.0f;
                context.metresPerTexel = 1000.0 / static_cast<double>(kGridSize);
                context.backend = backend;
                context.parallel = false;
                context.terrainHeight = snapshot.data();
                context.terrainHeightWidth = kGridSize;
                context.terrainHeightDepth = kGridSize;
                const std::vector<OutputInfo>& outputs = compiled.graph->outputs();
                std::vector<int> indices;
                std::vector<std::vector<float>> values(outputs.size(), std::vector<float>(static_cast<size_t>(kGridSize) * kGridSize));
                std::vector<float*> targets;
                for (size_t i = 0; i < outputs.size(); i++){
                    indices.push_back(static_cast<int>(i));
                    targets.push_back(values[i].data());
                }
                compiled.graph->evaluateGridOutputs(domain, context, indices, targets);
                for (size_t i = 0; i < outputs.size(); i++){
                    out.emplace_back(base + "/" + outputs[i].name, hashFloats(values[i].data(), values[i].size()));
                }
            }
        }

        constexpr Expected kTable[] = {
#include "NoiseGoldenTable.inc"
            {nullptr, 0}
        };

    }

    std::string nodeCaseName(const std::string& type){
        return "node/" + type;
    }

    std::vector<std::pair<std::string, uint64_t>> computeCases(Backend backend, const std::vector<Preset>& presets){
        Cases out;
        addBasisCases(out, backend);
        addDetMathCases(out);
        addCompatCases(out);
        addNodeCases(out, backend);
        addNumericCases(out, backend);
        addErosionCases(out);
        addPresetCases(out, backend, presets);
        return out;
    }

    const Expected* expectedTable(size_t& count){
        count = sizeof(kTable) / sizeof(kTable[0]) - 1;
        return kTable;
    }

    std::string reportLine(const char* target, const char* backend, size_t cases, bool pass){
        char hash[32];
        std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(tableHash()));
        return std::string("noise-golden ") + target + " " + backend + " " + std::to_string(cases) + " " + (pass ? "pass" : "FAIL") + " table=" + hash;
    }

    uint64_t tableHash(){
        uint64_t h = 14695981039346656037ull;
        size_t count = 0;
        const Expected* table = expectedTable(count);
        const auto mix = [&h](unsigned char byte){
            h ^= byte;
            h *= 1099511628211ull;
        };
        for (size_t i = 0; i < count; i++){
            for (const char* c = table[i].name; *c; c++) mix(static_cast<unsigned char>(*c));
            for (int b = 0; b < 8; b++) mix(static_cast<unsigned char>(table[i].hash >> (b * 8)));
        }
        return h;
    }

}}}
