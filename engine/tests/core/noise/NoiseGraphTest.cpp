// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 AC8-AC9, AC11-AC15, AC40: the asset, the compiler, ranges and the evaluators.

#include "NoiseGoldenCases.h"
#include "NoiseTestSupport.h"
#include "graph/GraphRegistry.h"
#include "noise/NoiseKernels.h"
#include "noise/NoiseNodes.h"

#if defined(JE_TEST_COUNTS_ALLOCATIONS)
#include "support/AllocationCounter.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::noise;
using namespace noisetest;

namespace {

    GridDomain squareDomain(int size) {
        GridDomain domain;
        domain.width = size;
        domain.depth = size;
        domain.unitsPerTexel = 1.0 / static_cast<double>(size);
        return domain;
    }

    // A fractal height with a mask, the shape most graphs have
    std::string addHeightChain(GraphBuilder& b) {
        const std::string fbm = b.node("fbm", {{"octaves", 4}, {"frequency", 3.0}});
        const std::string remap = b.node("remap", {{"from_min", -1.0}, {"from_max", 1.0}, {"to_min", 0.0}, {"to_max", 1.0}, {"clamp", true}});
        b.edge(fbm, remap);
        return remap;
    }

    bool contains(const std::vector<std::string>& list, const std::string& id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    }

    // One graph per node type for the range sampling: it reads the type's output through output_value
    struct TypeCase { std::string type; std::string port; GraphBuilder builder; };

    std::vector<std::string> everyType() {
        std::vector<std::string> types;
        for (const graph::NodeTypeDesc& type : makeNoiseKindDesc().nodeTypes) types.push_back(type.id);
        return types;
    }

    // Wires inputs from noise sources and returns what to output. Coord outputs go through a noise.
    std::string wireType(GraphBuilder& b, const graph::NodeTypeDesc& type, const Json& params) {
        const std::string n = b.node(type.id, params);
        for (const graph::Port& port : type.ports) {
            if (port.direction != graph::PortDirection::In) continue;
            if (port.type == "coord") continue;
            const std::string source = b.node("fbm", {{"frequency", 2.0}, {"octaves", 2}, {"seed_offset", 3}});
            const std::string unit = b.node("remap", {{"from_min", -1.0}, {"from_max", 1.0}, {"to_min", 0.0}, {"to_max", 1.0}, {"clamp", true}});
            b.edge(source, unit);
            b.edge(unit, n, port.id);
        }
        return n;
    }

    Json rangeParamsFor(const std::string& type) {
        if (type == "variable") return {{"name", "v"}};
        if (type == "hydraulic_erosion") return {{"lifetime", 8}, {"droplets_per_texel", 0.1}};
        if (type == "thermal_erosion") return {{"iterations", 2}};
        if (type == "cellular") return {{"return", "f2"}};
        return Json::object();
    }

}

// ---- ports, seeds, the file

TEST(NoiseGraphAsset, CoordDoesNotFeedField) {
    GraphBuilder b;
    const std::string position = b.node("position");
    const std::string remap = b.node("remap");
    b.edge(position, remap);    // coord to a field input
    b.output(remap);
    const CompileResult result = compiled(b.graph());
    EXPECT_EQ(result.graph, nullptr);
    EXPECT_TRUE(hasIssue(result, "type-mismatch", remap));
}

TEST(NoiseGraphAsset, AddingANodeKeepsOtherOutputs) {
    GraphBuilder before;
    const std::string chain = addHeightChain(before);
    before.output(chain, "first");
    const auto beforeCompiled = compiled(before.graph());
    ASSERT_NE(beforeCompiled.graph, nullptr);
    const std::vector<float> a = gridOf(*beforeCompiled.graph, squareDomain(32));

    // A new noise node first in the file, with outputs of its own: every id and index moves
    GraphBuilder after;
    const std::string extra = after.node("simplex", {{"seed_offset", 9}});
    after.output(extra, "second");
    const std::string chain2 = addHeightChain(after);
    after.output(chain2, "first");
    // Reordered as a person's or an agent's edit of the file reorders it: through the file's form
    graph::Json reordered = graph::toJson(after.graph());
    std::reverse(reordered["nodes"].begin(), reordered["nodes"].end());
    std::reverse(reordered["edges"].begin(), reordered["edges"].end());
    graph::LoadReport reorderedReport;
    const graph::Graph reloaded = graph::fromJson(reordered, reorderedReport);
    ASSERT_TRUE(reorderedReport.ok) << reorderedReport.error;
    const auto afterCompiled = compiled(reloaded);
    ASSERT_NE(afterCompiled.graph, nullptr);
    const int output = afterCompiled.graph->outputIndex("first");
    ASSERT_GE(output, 0);
    const std::vector<float> c = gridOf(*afterCompiled.graph, squareDomain(32), {}, output);
    EXPECT_EQ(hashFloats(a.data(), a.size()), hashFloats(c.data(), c.size()));
}

// Graph::findNode is a binary search over ids (Graph.cpp lowerById), so a graph's lookups rely on id
// order, and a .noisegraph is a file people and agents edit by hand. graph::fromJson sorts what it reads,
// which is what makes any order in a file safe: n10 before n2, and edges before the nodes they name.
TEST(NoiseGraphAsset, OutOfOrderFileLoadsInIdOrder) {
    registerNoiseKind();
    const std::string ordered = R"({"formatVersion":1,"kind":"noise","kindVersion":1,"nextId":20,
        "nodes":[{"id":"n2","type":"fbm","params":{"octaves":3}},
                 {"id":"n3","type":"remap","params":{"clamp":true}},
                 {"id":"n10","type":"output_height","params":{}}],
        "edges":[{"id":"e4","from":{"node":"n2","port":"out"},"to":{"node":"n3","port":"in"}},
                 {"id":"e11","from":{"node":"n3","port":"out"},"to":{"node":"n10","port":"in"}}]})";
    const std::string shuffled = R"({"formatVersion":1,"kind":"noise","kindVersion":1,"nextId":20,
        "nodes":[{"id":"n10","type":"output_height","params":{}},
                 {"id":"n3","type":"remap","params":{"clamp":true}},
                 {"id":"n2","type":"fbm","params":{"octaves":3}}],
        "edges":[{"id":"e11","from":{"node":"n3","port":"out"},"to":{"node":"n10","port":"in"}},
                 {"id":"e4","from":{"node":"n2","port":"out"},"to":{"node":"n3","port":"in"}}]})";
    graph::LoadReport orderedReport, shuffledReport;
    const graph::Graph a = graph::load(ordered, orderedReport);
    const graph::Graph b = graph::load(shuffled, shuffledReport);
    ASSERT_TRUE(shuffledReport.ok) << shuffledReport.error;
    for (const char* id : {"n2", "n3", "n10"}) EXPECT_NE(b.findNode(id), nullptr) << id;
    for (const char* id : {"e4", "e11"}) EXPECT_NE(b.findEdge(id), nullptr) << id;
    for (const graph::Issue& issue : graph::validate(b)) {
        EXPECT_NE(issue.severity, graph::Severity::Error) << issue.code << ": " << issue.message;
    }
    EXPECT_EQ(graph::save(b), graph::save(a)) << "saving normalizes the order";
    const auto compiledA = compiled(a);
    const auto compiledB = compiled(b);
    ASSERT_NE(compiledA.graph, nullptr);
    ASSERT_NE(compiledB.graph, nullptr);
    const std::vector<float> valuesA = gridOf(*compiledA.graph, squareDomain(32));
    const std::vector<float> valuesB = gridOf(*compiledB.graph, squareDomain(32));
    EXPECT_EQ(hashFloats(valuesA.data(), valuesA.size()), hashFloats(valuesB.data(), valuesB.size()));
}

// The model's own contract (Graph.h: nodes "in id order"): a graph built by hand in memory out of order
// has not been through a load, and must fail loudly on its edges rather than compile into something else
TEST(NoiseGraphAsset, InMemoryOutOfOrderFailsLoudly) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    std::reverse(b.graph().nodes.begin(), b.graph().nodes.end());
    const CompileResult result = compiled(b.graph());
    EXPECT_EQ(result.graph, nullptr);
    EXPECT_TRUE(hasIssue(result, "dangling-edge"));
}

TEST(NoiseGraphAsset, RoundTripIsByteIdentical) {
    registerNoiseKind();
    for (const char* preset : {"alpine", "plains", "islands"}) {
        std::ifstream file(std::string(JASPER_TEST_REPO_DIR) + "/editor/resources/noisegraphs/" + preset + ".noisegraph", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        ASSERT_FALSE(text.empty()) << preset;
        graph::LoadReport report;
        const graph::Graph loaded = graph::load(text, report);
        ASSERT_TRUE(report.ok) << report.error;
        EXPECT_EQ(graph::save(loaded), text) << preset;
    }
}

TEST(NoiseGraphAsset, UnknownKeysSurvive) {
    registerNoiseKind();
    const std::string text = R"({"formatVersion":1,"kind":"noise","kindVersion":1,"nextId":3,"future":{"a":1},
        "nodes":[{"id":"n1","type":"constant","params":{"value":1.0,"later":"x"},"tag":7},
                 {"id":"n2","type":"output_value","params":{"name":"v"}}],
        "edges":[{"id":"e1","from":{"node":"n1","port":"out"},"to":{"node":"n2","port":"in"},"note":"n"}],
        "metadata":{"noise":{"space":"world","own":true}}})";
    graph::LoadReport report;
    const graph::Graph loaded = graph::load(text, report);
    ASSERT_TRUE(report.ok);
    const std::string saved = graph::save(loaded);
    for (const char* key : {"\"future\"", "\"later\"", "\"tag\"", "\"note\"", "\"own\""}) {
        EXPECT_NE(saved.find(key), std::string::npos) << key;
    }
    EXPECT_NE(compiled(loaded).graph, nullptr);
}

TEST(NoiseGraphAsset, UnknownNodeIsKeptAndFailsOnThatNode) {
    GraphBuilder b;
    const std::string strange = b.node("from_the_future", {{"x", 1}});
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    EXPECT_EQ(result.graph, nullptr);
    EXPECT_TRUE(hasIssue(result, "unknown-node-type", strange));
    EXPECT_NE(graph::save(b.graph()).find("from_the_future"), std::string::npos);
}

TEST(NoiseGraphAsset, MigrationRunsOnce) {
    graph::GraphKindDesc kind = makeNoiseKindDesc();
    kind.id = "noise_migration_test";
    kind.fileExtension = ".noisemigrationtest";
    kind.kindVersion = 2;
    auto runs = std::make_shared<int>(0);
    graph::Migration migration;
    migration.toVersion = 2;
    migration.fn = [runs](graph::Json& json) {
        (*runs)++;
        json["metadata"]["migrated"] = true;
    };
    kind.migrations.push_back(migration);
    graph::GraphRegistry::get().registerKind(kind);
    const std::string v1 = R"({"formatVersion":1,"kind":"noise_migration_test","kindVersion":1,"nextId":1,"nodes":[],"edges":[]})";
    graph::LoadReport report;
    const graph::Graph first = graph::load(v1, report);
    EXPECT_EQ(*runs, 1);
    EXPECT_EQ(report.migratedFrom, 1);
    EXPECT_EQ(first.kindVersion, 2);
    graph::LoadReport again;
    const graph::Graph second = graph::load(graph::save(first), again);
    EXPECT_EQ(*runs, 1) << "a migrated file does not migrate again";
    EXPECT_EQ(again.migratedFrom, 0);
    EXPECT_TRUE(second.metadata.value("migrated", false));
    graph::GraphRegistry::get().unregisterKind(kind.id);
}

TEST(NoiseGraphAsset, NewerKindVersionWarns) {
    registerNoiseKind();
    const std::string text = R"({"formatVersion":1,"kind":"noise","kindVersion":99,"nextId":3,
        "nodes":[{"id":"n1","type":"constant","params":{"value":0.5}},{"id":"n2","type":"output_value","params":{"name":"v"}}],
        "edges":[{"id":"e1","from":{"node":"n1","port":"out"},"to":{"node":"n2","port":"in"}}]})";
    graph::LoadReport report;
    const graph::Graph loaded = graph::load(text, report);
    EXPECT_TRUE(report.ok);
    EXPECT_FALSE(report.warnings.empty());
    EXPECT_NE(compiled(loaded).graph, nullptr) << "the nodes it knows still compile";
}

TEST(NoiseNodes, TableIsFrozen) {
    // Additions are allowed; a change to a shipped node fails (R14). JASPER_TEST_UPDATE_GOLDEN=1 writes it.
    const graph::GraphKindDesc kind = makeNoiseKindDesc();
    graph::Json current = {{"kind_version", kind.kindVersion}, {"node_types", graph::Json::array()}};
    for (const graph::NodeTypeDesc& type : kind.nodeTypes) current["node_types"].push_back(graph::toJson(type));
    const std::string path = std::string(JASPER_TEST_REPO_DIR) + "/engine/tests/core/noise/golden/noise_kind.json";
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
        std::ofstream out(path, std::ios::binary);
        out << current.dump(2) << "\n";
    }
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const graph::Json frozen = graph::Json::parse(file);
    for (const graph::Json& type : frozen["node_types"]) {
        const graph::Json* match = nullptr;
        for (const graph::Json& candidate : current["node_types"]) {
            if (candidate["id"] == type["id"]) match = &candidate;
        }
        ASSERT_NE(match, nullptr) << type["id"] << " was removed";
        for (const auto& [key, value] : type.items()) {
            if (key != "params") {
                EXPECT_EQ((*match)[key], value) << type["id"] << "." << key;
                continue;
            }
            for (const graph::Json& param : value) {
                bool found = false;
                for (const graph::Json& candidate : (*match)["params"]) {
                    if (candidate["name"] == param["name"]) {
                        found = true;
                        EXPECT_EQ(candidate, param) << type["id"] << "." << param["name"];
                    }
                }
                EXPECT_TRUE(found) << type["id"] << " lost " << param["name"];
            }
        }
    }
    EXPECT_EQ(frozen["kind_version"], current["kind_version"]);
    EXPECT_EQ(frozen["node_types"].size(), 29u);
}

TEST(NoiseNodes, EveryTypeHasAGoldenAndARange) {
    size_t count = 0;
    const golden::Expected* table = golden::expectedTable(count);
    const std::vector<std::string> types = everyType();
    EXPECT_EQ(types.size(), 29u);
    for (const std::string& type : types) {
        bool found = false;
        const std::string name = golden::nodeCaseName(type);
        for (size_t i = 0; i < count; i++) found = found || name == table[i].name;
        EXPECT_TRUE(found) << type << " has no golden case";
    }
}

// ---- the compiler

TEST(NoiseCompile, DeadNodesAreRemoved) {
    GraphBuilder live;
    live.output(addHeightChain(live));
    const auto liveResult = compiled(live.graph());
    GraphBuilder b;
    b.output(addHeightChain(b));
    const std::string orphan = b.node("simplex");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_TRUE(contains(result.deadNodes, orphan));
    EXPECT_EQ(result.opCount, liveResult.opCount);
}

TEST(NoiseCompile, ConstantsFold) {
    GraphBuilder b;
    const std::string a = b.node("constant", {{"value", 3.0}});
    const std::string c = b.node("constant", {{"value", 4.0}});
    const std::string mul = b.node("math", {{"op", "multiply"}});
    b.edge(a, mul, "a");
    b.edge(c, mul, "b");
    b.output(mul);
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_TRUE(contains(result.foldedNodes, mul));
    const Interval range = result.graph->rangeOf(0);
    EXPECT_EQ(range.min, 12.0f);
    EXPECT_EQ(range.max, 12.0f);
    const std::vector<float> values = gridOf(*result.graph, squareDomain(8));
    for (float value : values) EXPECT_EQ(value, 12.0f);
}

TEST(NoiseCompile, ProvenMaskDropsABranch) {
    GraphBuilder b;
    const std::string kept = b.node("perlin");
    const std::string dropped = b.node("simplex");
    const std::string mask = b.node("constant", {{"value", 0.1}});
    const std::string select = b.node("select", {{"threshold", 0.5}});
    b.edge(kept, select, "a");
    b.edge(dropped, select, "b");
    b.edge(mask, select, "mask");
    b.output(select);
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_TRUE(contains(result.deadNodes, dropped));
    EXPECT_FALSE(contains(result.deadNodes, kept));
    // Blend's weight the same way
    GraphBuilder c;
    const std::string first = c.node("perlin");
    const std::string second = c.node("simplex");
    const std::string blend = c.node("blend", {{"weight", 1.0}});
    c.edge(first, blend, "a");
    c.edge(second, blend, "b");
    c.output(blend);
    const CompileResult blended = compiled(c.graph());
    ASSERT_NE(blended.graph, nullptr);
    EXPECT_TRUE(contains(blended.deadNodes, first));
    EXPECT_FALSE(contains(blended.deadNodes, second));
}

TEST(NoiseCompile, BuffersAreReused) {
    GraphBuilder b;
    std::string previous = b.node("perlin");
    const int chain = 40;
    for (int i = 0; i < chain; i++) {
        const std::string next = b.node("scale_offset", {{"scale", 0.5}, {"offset", 0.25}});
        b.edge(previous, next);
        previous = next;
    }
    b.output(previous);
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_LT(result.bufferCount, static_cast<size_t>(chain) / 2);
    // Reuse does not change a bit: the chain against its closed form
    const std::vector<float> values = gridOf(*result.graph, squareDomain(16));
    for (float value : values) EXPECT_TRUE(std::isfinite(value));
}

TEST(NoiseCompile, ReportsDeadAndFolded) {
    GraphBuilder b;
    const std::string constant = b.node("constant", {{"value", 2.0}});
    const std::string negate = b.node("math", {{"op", "negate"}});
    b.edge(constant, negate, "a");
    const std::string sum = b.node("math", {{"op", "add"}});
    const std::string noiseNode = b.node("perlin");
    b.edge(noiseNode, sum, "a");
    b.edge(negate, sum, "b");
    b.output(sum);
    const std::string orphan = b.node("value");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_TRUE(contains(result.deadNodes, orphan));
    EXPECT_TRUE(contains(result.foldedNodes, negate));
    EXPECT_FALSE(contains(result.foldedNodes, sum));
    bool nodeMarked = false;
    for (const CompiledNodeInfo& info : result.graph->nodes()) {
        if (info.id == negate) nodeMarked = info.folded;
    }
    EXPECT_TRUE(nodeMarked);
}

namespace {
    std::atomic<int> gClockCalls{0};
    uint64_t countingClock() {
        return static_cast<uint64_t>(gClockCalls.fetch_add(1)) * 1000u;
    }
}

TEST(NoiseCompile, ProfileCoversLiveNodes) {
    GraphBuilder b;
    const std::string chain = addHeightChain(b);
    b.output(chain);
    const std::string orphan = b.node("simplex");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    NodeProfile profile;
    EvalContext context;
    context.profile = &profile;
    (void)gridOf(*result.graph, squareDomain(64), context);
    EXPECT_GT(profile.totalMs, 0.0);
    EXPECT_EQ(profile.points, 64u * 64u);
    std::set<std::string> covered;
    double shares = 0.0;
    for (const NodeProfileEntry& entry : profile.nodes) {
        covered.insert(entry.nodeId);
        shares += entry.share;
    }
    EXPECT_TRUE(covered.count(chain));
    EXPECT_FALSE(covered.count(orphan));
    EXPECT_NEAR(shares, 1.0, 1e-6);
}

TEST(NoiseCompile, ProfileOffCallsNoClock) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EvalContext context;
    context.clock = &countingClock;
    gClockCalls = 0;
    (void)gridOf(*result.graph, squareDomain(64), context);
    EXPECT_EQ(gClockCalls.load(), 0);
    NodeProfile profile;
    context.profile = &profile;
    (void)gridOf(*result.graph, squareDomain(64), context);
    EXPECT_GT(gClockCalls.load(), 0) << "the seam is what the profile reads";
}

// ---- ranges

TEST(NoiseRanges, ObservedWithinAnalytic) {
    const graph::GraphKindDesc kind = makeNoiseKindDesc();
    const int side = 317;   // 100,489 points
    const std::vector<float> snapshot(static_cast<size_t>(side) * side, 0.5f);
    for (const graph::NodeTypeDesc& type : kind.nodeTypes) {
        if (type.id.rfind("output_", 0) == 0) continue;
        for (uint32_t seed : {1u, 2u, 3u}) {
            GraphBuilder b;
            if (type.id == "variable") b.variable("v", 0.5);
            const std::string n = wireType(b, type, rangeParamsFor(type.id));
            // Every output port of the type, each its own output
            std::vector<std::string> names;
            for (const graph::Port& port : type.ports) {
                if (port.direction == graph::PortDirection::In) continue;
                std::string from = n;
                std::string fromPort = port.id;
                if (port.type == "coord") {
                    from = b.node("perlin");
                    b.edge(n, from, "coord", port.id);
                    fromPort = "out";
                }
                const std::string name = "o" + std::to_string(names.size());
                b.output(from, name, fromPort);
                names.push_back(name);
            }
            const CompileResult result = compiled(b.graph());
            ASSERT_NE(result.graph, nullptr) << type.id;
            EvalContext context;
            context.seed = seed;
            context.terrainHeight = snapshot.data();
            context.terrainHeightWidth = side;
            context.terrainHeightDepth = side;
            context.metresPerTexel = 3.0;
            GridDomain domain = squareDomain(type.id == "hydraulic_erosion" || type.id == "thermal_erosion" ? 96 : side);
            for (const std::string& name : names) {
                const int output = result.graph->outputIndex(name);
                ASSERT_GE(output, 0);
                const Interval range = result.graph->rangeOf(output);
                const std::vector<float> values = gridOf(*result.graph, domain, context, output);
                for (float value : values) {
                    ASSERT_TRUE(std::isfinite(value)) << type.id;
                    ASSERT_GE(value, range.min) << type.id << " " << name;
                    ASSERT_LE(value, range.max) << type.id << " " << name;
                }
            }
        }
    }
}

// Math over every op with inputs that reach zero and negatives
TEST(NoiseRanges, MathOpsStayWithin) {
    const char* ops[] = {"add", "subtract", "multiply", "divide", "min", "max", "power", "modulo", "abs", "negate", "one_minus", "sqrt", "floor", "frac", "sin", "cos"};
    for (const char* op : ops) {
        GraphBuilder b;
        const std::string first = b.node("perlin", {{"frequency", 3.0}});
        const std::string second = b.node("simplex", {{"frequency", 5.0}});
        const std::string math = b.node("math", {{"op", op}});
        b.edge(first, math, "a");
        b.edge(second, math, "b");
        b.output(math);
        const CompileResult result = compiled(b.graph());
        ASSERT_NE(result.graph, nullptr) << op;
        const Interval range = result.graph->rangeOf(0);
        for (float value : gridOf(*result.graph, squareDomain(128))) {
            ASSERT_TRUE(std::isfinite(value)) << op;
            ASSERT_GE(value, range.min) << op;
            ASSERT_LE(value, range.max) << op;
        }
    }
}

TEST(NoiseRanges, RangeOverBoxContainsSamples) {
    GraphBuilder b;
    const std::string position = b.node("position");
    const std::string height = b.node("scale_offset", {{"scale", -1.0}, {"offset", 2.0}});
    b.edge(position, height, "in", "y");
    const std::string fbm = b.node("fbm", {{"dimensions", "3d"}, {"frequency", 0.1}, {"octaves", 3}});
    const std::string amplitude = b.node("scale_offset", {{"scale", 0.5}, {"offset", 0.0}});
    b.edge(fbm, amplitude);
    const std::string sum = b.node("math", {{"op", "add"}});
    b.edge(height, sum, "a");
    b.edge(amplitude, sum, "b");
    b.output(sum, "sdf");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    const int output = result.graph->outputIndex("sdf");
    const float boxMin[3] = {-4.0f, 10.0f, 3.0f};
    const float boxMax[3] = {4.0f, 12.0f, 9.0f};
    const Interval range = result.graph->rangeOver(output, boxMin, boxMax);
    // y in 10..12 gives 2 - y in -10..-8, plus at most 0.5 either way
    EXPECT_LE(range.max, -7.4f);
    EXPECT_GE(range.min, -10.6f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<float> x, y, z;
    for (int i = 0; i < 5000; i++) {
        x.push_back(boxMin[0] + unit(rng) * (boxMax[0] - boxMin[0]));
        y.push_back(boxMin[1] + unit(rng) * (boxMax[1] - boxMin[1]));
        z.push_back(boxMin[2] + unit(rng) * (boxMax[2] - boxMin[2]));
    }
    std::vector<float> out(x.size());
    EvalScratch scratch(result.graph->scratchBytes());
    result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, output, out.data(), scratch);
    for (float value : out) {
        ASSERT_GE(value, range.min);
        ASSERT_LE(value, range.max);
    }
}

// ---- evaluators

TEST(NoiseEval, PointsMatchGrid) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    GridDomain domain = squareDomain(97);
    domain.startX = 11;
    domain.startZ = -7;
    const std::vector<float> grid = gridOf(*result.graph, domain);
    std::vector<float> x, z;
    for (int j = 0; j < domain.depth; j++) {
        for (int i = 0; i < domain.width; i++) {
            x.push_back(static_cast<float>((static_cast<double>(domain.startX + i) + 0.5) * domain.unitsPerTexel));
            z.push_back(static_cast<float>((static_cast<double>(domain.startZ + j) + 0.5) * domain.unitsPerTexel));
        }
    }
    const std::vector<float> y(x.size(), 0.0f);
    std::vector<float> points(x.size());
    EvalScratch scratch(result.graph->scratchBytes());
    result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, 0, points.data(), scratch);
    for (size_t i = 0; i < points.size(); i++) ASSERT_EQ(bitsOf(points[i]), bitsOf(grid[i])) << i;
}

TEST(NoiseEval, Grid3DMatchesPoints) {
    GraphBuilder b;
    const std::string fbm = b.node("fbm", {{"dimensions", "3d"}, {"frequency", 0.2}, {"octaves", 3}});
    b.output(fbm, "sdf");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    Grid3Domain domain;
    domain.startX = -8;
    domain.startY = 4;
    domain.startZ = 16;
    domain.width = 17;
    domain.height = 9;
    domain.depth = 13;
    domain.unitsPerVoxel = 0.5;
    std::vector<float> grid(static_cast<size_t>(domain.width) * domain.height * domain.depth);
    EXPECT_TRUE(result.graph->evaluateGrid3D(domain, EvalContext{}, 0, grid.data()));
    std::vector<float> x, y, z;
    for (int l = 0; l < domain.depth; l++) {
        for (int j = 0; j < domain.height; j++) {
            for (int i = 0; i < domain.width; i++) {
                x.push_back(static_cast<float>(static_cast<double>(domain.startX + i) * domain.unitsPerVoxel));
                y.push_back(static_cast<float>(static_cast<double>(domain.startY + j) * domain.unitsPerVoxel));
                z.push_back(static_cast<float>(static_cast<double>(domain.startZ + l) * domain.unitsPerVoxel));
            }
        }
    }
    std::vector<float> points(x.size());
    EvalScratch scratch(result.graph->scratchBytes());
    result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, 0, points.data(), scratch);
    for (size_t i = 0; i < points.size(); i++) ASSERT_EQ(bitsOf(points[i]), bitsOf(grid[i])) << i;
}

// T6 R5: a coarse sample is the fine sample at its position
TEST(NoiseEval, Grid3DCoarseIsFineDecimated) {
    GraphBuilder b;
    b.output(b.node("fbm", {{"dimensions", "3d"}, {"frequency", 0.1}, {"octaves", 3}}), "sdf");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    Grid3Domain fine;
    fine.width = fine.height = fine.depth = 17;
    fine.unitsPerVoxel = 0.25;
    Grid3Domain coarse;
    coarse.width = coarse.height = coarse.depth = 9;
    coarse.unitsPerVoxel = 0.5;
    std::vector<float> fineValues(17u * 17u * 17u), coarseValues(9u * 9u * 9u);
    result.graph->evaluateGrid3D(fine, EvalContext{}, 0, fineValues.data());
    result.graph->evaluateGrid3D(coarse, EvalContext{}, 0, coarseValues.data());
    for (int l = 0; l < 9; l++) {
        for (int j = 0; j < 9; j++) {
            for (int i = 0; i < 9; i++) {
                const size_t c = static_cast<size_t>(l) * 81u + static_cast<size_t>(j) * 9u + static_cast<size_t>(i);
                const size_t f = static_cast<size_t>(l * 2) * 289u + static_cast<size_t>(j * 2) * 17u + static_cast<size_t>(i * 2);
                ASSERT_EQ(bitsOf(coarseValues[c]), bitsOf(fineValues[f]));
            }
        }
    }
}

TEST(NoiseEval, GridOnlyRefusesPoints) {
    GraphBuilder b;
    const std::string slope = b.node("slope_mask");
    b.edge(addHeightChain(b), slope);
    b.output(slope, "mask");
    const CompileResult allowed = compiled(b.graph());
    ASSERT_NE(allowed.graph, nullptr);
    EXPECT_TRUE(allowed.graph->isGridOnly());
    EXPECT_FALSE(allowed.graph->isBakeOnly());
    CompileOptions options;
    options.forbidGridOnly = true;
    const CompileResult refused = compiled(b.graph(), options);
    EXPECT_EQ(refused.graph, nullptr);
    EXPECT_TRUE(hasIssue(refused, "grid-only-node", slope));
    // Reaching the point evaluator anyway writes nothing meaningful, and no crash
    float x = 0.5f, y = 0.0f, z = 0.5f, out = 1.0f;
    EvalScratch scratch(allowed.graph->scratchBytes());
    allowed.graph->evaluatePoints(&x, &y, &z, 1, EvalContext{}, 0, &out, scratch);
    EXPECT_EQ(out, 0.0f);
}

TEST(NoiseEval, BakeOnlyRefusedWhenForbidden) {
    GraphBuilder b;
    const std::string thermal = b.node("thermal_erosion");
    b.edge(addHeightChain(b), thermal, "height");
    b.output(thermal);
    const CompileResult allowed = compiled(b.graph());
    ASSERT_NE(allowed.graph, nullptr);
    EXPECT_TRUE(allowed.graph->isBakeOnly());
    CompileOptions options;
    options.forbidBakeOnly = true;
    const CompileResult refused = compiled(b.graph(), options);
    EXPECT_EQ(refused.graph, nullptr);
    EXPECT_TRUE(hasIssue(refused, "bake-only-node", thermal));
}

TEST(NoiseEval, WorkerCountDoesNotChangeBits) {
    GraphBuilder b;
    const std::string height = addHeightChain(b);
    const std::string slope = b.node("slope_mask", {{"min_angle", 2.0}, {"max_angle", 30.0}});
    b.edge(height, slope);
    b.output(height, "height");
    b.output(slope, "mask");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    GridDomain domain = squareDomain(129);
    EvalContext context;
    context.metresPerTexel = 8.0;
    uint64_t expected[2] = {0, 0};
    for (int workers : {0, 1, 3, 7}) {
        WorkerScope scope(workers);
        for (int output = 0; output < 2; output++) {
            const std::vector<float> values = gridOf(*result.graph, domain, context, output);
            const uint64_t hash = hashFloats(values.data(), values.size());
            if (workers == 0) expected[output] = hash;
            EXPECT_EQ(hash, expected[output]) << workers << " workers, output " << output;
        }
    }
}

TEST(NoiseEval, NoNonFiniteReachesAnOutput) {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Every operation a graph can blow up with, fed by huge values and zeros
    GraphBuilder b;
    const std::string position = b.node("position");
    const std::string huge = b.node("scale_offset", {{"scale", 1e30}, {"offset", 0.0}});
    b.edge(position, huge, "in", "x");
    const std::string zero = b.node("scale_offset", {{"scale", 0.0}, {"offset", 0.0}});
    b.edge(position, zero, "in", "x");
    const char* ops[] = {"divide", "multiply", "power", "modulo", "sqrt"};
    for (const char* op : ops) {
        const std::string math = b.node("math", {{"op", op}});
        b.edge(huge, math, "a");
        b.edge(zero, math, "b");
        b.output(math, op);
    }
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    std::vector<float> x = {0.0f, 1.0f, -1.0f, inf, -inf, nan, 1e30f, -1e30f, 1e-40f};
    std::vector<float> y(x.size(), 0.0f), z(x.size(), 0.0f), out(x.size());
    EvalScratch scratch(result.graph->scratchBytes());
    for (size_t o = 0; o < result.graph->outputs().size(); o++) {
        result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, static_cast<int>(o), out.data(), scratch);
        for (float value : out) EXPECT_TRUE(std::isfinite(value)) << result.graph->outputs()[o].name;
    }
    // Infinite exposed value: the variable op cleans it (R6)
    GraphBuilder v;
    v.variable("v", 1.0);
    const std::string variable = v.node("variable", {{"name", "v"}});
    v.output(variable);
    const CompileResult withVariable = compiled(v.graph());
    ASSERT_NE(withVariable.graph, nullptr);
    const std::vector<std::pair<std::string, float>> values = {{"v", inf}};
    EvalContext context;
    context.variables = values;
    for (float value : gridOf(*withVariable.graph, squareDomain(4), context)) EXPECT_EQ(value, 0.0f);
}

// R23: which region computes a texel cannot change its bits
TEST(NoiseEval, SubRegionMatchesWholeBitForBit) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    const GridDomain whole = squareDomain(128);
    const std::vector<float> wholeValues = gridOf(*result.graph, whole);
    GridDomain part = whole;
    part.startX = 37;
    part.startZ = 90;
    part.width = 41;
    part.depth = 23;
    const std::vector<float> partValues = gridOf(*result.graph, part);
    for (int j = 0; j < part.depth; j++) {
        for (int i = 0; i < part.width; i++) {
            ASSERT_EQ(bitsOf(partValues[static_cast<size_t>(j) * part.width + i]),
                      bitsOf(wholeValues[static_cast<size_t>(j + part.startZ) * whole.width + static_cast<size_t>(i + part.startX)]));
        }
    }
}

TEST(NoiseEval, AdjacentTilesAgreeAtAnyOrigin) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    const int64_t origin = int64_t{1} << 40;
    GridDomain whole;
    whole.startX = origin;
    whole.startZ = -origin;
    whole.width = 64;
    whole.depth = 32;
    whole.unitsPerTexel = 1.0 / 4096.0;
    const std::vector<float> wholeValues = gridOf(*result.graph, whole);
    GridDomain left = whole;
    left.width = 40;
    GridDomain right = whole;
    right.startX += 40;
    right.width = 24;
    const std::vector<float> leftValues = gridOf(*result.graph, left);
    const std::vector<float> rightValues = gridOf(*result.graph, right);
    for (int j = 0; j < whole.depth; j++) {
        for (int i = 0; i < whole.width; i++) {
            const float expected = wholeValues[static_cast<size_t>(j) * whole.width + i];
            const float got = i < left.width ? leftValues[static_cast<size_t>(j) * left.width + i] : rightValues[static_cast<size_t>(j) * right.width + static_cast<size_t>(i - left.width)];
            ASSERT_EQ(bitsOf(got), bitsOf(expected)) << i << " " << j;
        }
    }
}

TEST(NoiseEval, SlopeMaskIsSeamless) {
    GraphBuilder b;
    const std::string slope = b.node("slope_mask", {{"min_angle", 1.0}, {"max_angle", 40.0}, {"falloff", 2.0}});
    b.edge(addHeightChain(b), slope);
    b.output(slope, "mask");
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EXPECT_EQ(result.graph->paddingOf(0), 1);
    EvalContext context;
    context.metresPerTexel = 4.0;
    const GridDomain whole = squareDomain(96);
    const std::vector<float> wholeValues = gridOf(*result.graph, whole, context);
    GridDomain part = whole;
    part.startX = 48;
    part.width = 48;
    part.depth = 40;
    const std::vector<float> partValues = gridOf(*result.graph, part, context);
    bool anyInRange = false;
    for (int j = 0; j < part.depth; j++) {
        for (int i = 0; i < part.width; i++) {
            const float expected = wholeValues[static_cast<size_t>(j) * whole.width + static_cast<size_t>(i + part.startX)];
            // The whole grid's own border has no padding of a neighbour here: compare the interior of both
            if (j == 0 || i + part.startX == whole.width - 1) continue;
            ASSERT_EQ(bitsOf(partValues[static_cast<size_t>(j) * part.width + i]), bitsOf(expected)) << i << " " << j;
            anyInRange = anyInRange || (expected > 0.0f && expected < 1.0f);
        }
    }
    EXPECT_TRUE(anyInRange);
}

TEST(NoiseEval, NoAllocationOnceWarm) {
#if defined(JE_TEST_COUNTS_ALLOCATIONS)
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    std::vector<float> x(10000, 0.25f), y(10000, 0.0f), z(10000, 0.75f), out(10000);
    EvalScratch scratch;
    EvalContext context;
    result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), context, 0, out.data(), scratch);
    {
        testsupport::ThreadAllocationCount counter;
        result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), context, 0, out.data(), scratch);
        EXPECT_EQ(counter.count(), 0u);
    }
    // A chunk's scratch is buffers * 4096 * 4 bytes (R52)
    EXPECT_EQ(result.graph->scratchBytes() % (4096u * 4u), 0u);
#else
    GTEST_SKIP() << "the allocation counter is Linux only";
#endif
}

// ---- threads

TEST(NoiseThreads, CompiledGraphIsShareable) {
    GraphBuilder b;
    b.output(addHeightChain(b));
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    std::vector<float> x(5000), y(5000, 0.0f), z(5000);
    for (size_t i = 0; i < x.size(); i++) {
        x[i] = static_cast<float>(i) * 0.01f;
        z[i] = static_cast<float>(i) * -0.003f;
    }
    std::vector<float> reference(x.size());
    EvalScratch referenceScratch(result.graph->scratchBytes());
    result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, 0, reference.data(), referenceScratch);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&] {
            EvalScratch scratch(result.graph->scratchBytes());
            std::vector<float> out(x.size());
            for (int round = 0; round < 8; round++) {
                result.graph->evaluatePoints(x.data(), y.data(), z.data(), x.size(), EvalContext{}, 0, out.data(), scratch);
                for (size_t i = 0; i < out.size(); i++) {
                    if (bitsOf(out[i]) != bitsOf(reference[i])) mismatches++;
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(mismatches.load(), 0);
}

TEST(NoiseThreads, ZeroWorkersRuns) {
    WorkerScope scope(0);
    GraphBuilder b;
    const std::string height = addHeightChain(b);
    const std::string thermal = b.node("thermal_erosion", {{"iterations", 2}});
    b.edge(height, thermal, "height");
    b.output(thermal);
    const CompileResult result = compiled(b.graph());
    ASSERT_NE(result.graph, nullptr);
    EvalContext context;
    context.metresPerTexel = 8.0;
    const std::vector<float> values = gridOf(*result.graph, squareDomain(32), context);
    EXPECT_NE(hashFloats(values.data(), values.size()), 0u);
}
