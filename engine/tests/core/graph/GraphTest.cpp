// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generic graph data model (S23 R21-R24, R36): JSON round trip and preservation, kind
// migrations, validation codes, edit operations and their inverses, and the budgets.

#include "graph/Graph.h"
#include "graph/GraphRegistry.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <string>

using namespace jasperengine::graph;

namespace {

    int migrationRuns = 0;

    Port port(const char* id, PortDirection direction, const char* type, const char* name = "", bool multi = false, bool optional = false) {
        Port p;
        p.id = id;
        p.direction = direction;
        p.type = type;
        p.name = name;
        p.multi = multi;
        p.optional = optional;
        return p;
    }

    void registerTestKind() {
        GraphKindDesc kind;
        kind.id = "test";
        kind.displayName = "Test Graph";
        kind.fileExtension = ".testgraph";
        kind.kindVersion = 2;
        kind.acyclic = true;
        kind.portTypes = {{"float", 0xff8800ffu, {"vector3"}}, {"vector3", 0xffff00ffu, {}}, {"string", 0xff00ffffu, {}}};

        NodeTypeDesc constant;
        constant.id = "const";
        constant.displayName = "Constant";
        constant.category = "Input";
        constant.ports = {port("value", PortDirection::Out, "float", "Speed")};
        constant.params = {{"value", "float", 1.0, -10.0, 10.0, {}}};

        NodeTypeDesc add;
        add.id = "add";
        add.displayName = "Blend";
        add.category = "Math";
        add.ports = {port("a", PortDirection::In, "float", "A"), port("b", PortDirection::In, "float", "B", false, true),
                     port("sum", PortDirection::Out, "float", "Sum")};
        add.params = {{"mode", "enum", "linear", std::nullopt, std::nullopt, {"linear", "smooth"}}};

        NodeTypeDesc vec;
        vec.id = "vec";
        vec.displayName = "Vector";
        vec.ports = {port("v", PortDirection::Out, "vector3"), port("in", PortDirection::In, "vector3", "In", false, true)};

        NodeTypeDesc text;
        text.id = "text";
        text.ports = {port("s", PortDirection::Out, "string")};

        NodeTypeDesc sum;
        sum.id = "sum";
        sum.displayName = "Sum";
        sum.params = {{"count", "int", 2, 1.0, 8.0, {}}};
        sum.dynamicPorts = [](const Json& params) {
            std::vector<Port> ports;
            const int count = params.value("count", 2);
            for (int i = 0; i < count; i++) {
                ports.push_back(port(("in" + std::to_string(i)).c_str(), PortDirection::In, "float", "", false, true));
            }
            ports.push_back(port("out", PortDirection::Out, "float"));
            return ports;
        };

        NodeTypeDesc sink;
        sink.id = "sink";
        sink.displayName = "Output";
        sink.ports = {port("x", PortDirection::In, "float", "X")};

        kind.nodeTypes = {constant, add, vec, text, sum, sink};
        kind.migrations = {{2, [](Json& graph) {
            migrationRuns++;
            for (Json& node : graph["nodes"]) {
                if (node.value("type", "") == "const" && node["params"].contains("val")) {
                    node["params"]["value"] = node["params"]["val"];
                    node["params"].erase("val");
                }
            }
        }}};
        kind.validate = [](const Graph& graph, std::vector<Issue>& issues) {
            bool hasSink = false;
            for (const Node& node : graph.nodes) hasSink = hasSink || node.type == "sink";
            if (!hasSink && !graph.nodes.empty()) issues.push_back({Severity::Warning, "test-no-output", "", "", "", "no Output node"});
        };
        GraphRegistry::get().registerKind(kind);
    }

    class GraphTest : public ::testing::Test {
    protected:
        void SetUp() override { registerTestKind(); }
        void TearDown() override { GraphRegistry::get().unregisterKind("test"); }

        static Graph loadOk(const std::string& text) {
            LoadReport report;
            Graph graph = load(text, report);
            EXPECT_TRUE(report.ok) << report.error;
            return graph;
        }

        static std::set<std::string> codes(const Graph& graph) {
            std::set<std::string> out;
            for (const Issue& issue : validate(graph)) out.insert(issue.code);
            return out;
        }
    };

    const char* const SAMPLE = R"({
  "formatVersion": 1,
  "kind": "test",
  "kindVersion": 2,
  "nextId": 9,
  "nodes": [
    {"id": "n1", "type": "const", "position": [0.0, 0.0], "params": {"value": 2.5}, "color": "red"},
    {"id": "n2", "type": "add", "position": [200.0, 10.5], "size": [120.0, 60.0], "params": {"mode": "linear"}},
    {"id": "n10", "type": "sink", "position": [400.0, 0.0], "params": {}},
    {"id": "n3", "type": "sum", "position": [0.0, 100.0], "params": {"count": 2},
     "ports": [{"id": "in0", "direction": "in", "type": "float", "optional": true, "hint": "first"},
               {"id": "in1", "direction": "in", "type": "float", "optional": true},
               {"id": "out", "direction": "out", "type": "float"}]}
  ],
  "edges": [
    {"id": "e5", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "a"}, "weight": 3},
    {"id": "e6", "from": {"node": "n2", "port": "sum"}, "to": {"node": "n10", "port": "x", "socket": 1}}
  ],
  "variables": [{"name": "speed", "type": "float", "default": 1.5, "exposed": true, "tooltip": "how fast"}],
  "groups": [{"id": "g7", "title": "Inputs", "rect": [0.0, 0.0, 100.0, 50.0], "nodes": ["n1"], "collapsed": false}],
  "viewport": {"offset": [10.0, 20.0], "zoom": 1.25, "grid": true},
  "metadata": {"author": "test"},
  "futureKey": {"keep": [1, 2, 3]}
})";

}

TEST_F(GraphTest, RoundTripIsByteIdentical) {
    const std::string first = save(loadOk(SAMPLE));
    const std::string second = save(loadOk(first));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.find('\r'), std::string::npos) << "LF line endings";
    EXPECT_EQ(first.back(), '\n');
    // nodes in id order: n2 before n10
    EXPECT_LT(first.find("\"n3\""), first.find("\"n10\""));
}

TEST_F(GraphTest, UnknownKeysPreservedAtEveryLevel) {
    const std::string text = save(loadOk(SAMPLE));
    for (const char* key : {"\"futureKey\"", "\"color\"", "\"weight\"", "\"socket\"", "\"hint\"", "\"tooltip\"", "\"collapsed\"", "\"grid\""}) {
        EXPECT_NE(text.find(key), std::string::npos) << key;
    }
    const Graph graph = loadOk(text);
    EXPECT_EQ(graph.passthrough["futureKey"]["keep"][2], 3);
    EXPECT_EQ(graph.findNode("n1")->passthrough["color"], "red");
    EXPECT_EQ(graph.findEdge("e5")->passthrough["weight"], 3);
    EXPECT_EQ(graph.variables[0].passthrough["tooltip"], "how fast");
    EXPECT_EQ((*graph.findNode("n3")->dynamicPorts)[0].passthrough["hint"], "first");
}

TEST_F(GraphTest, UnknownNodeTypeKeptAndReported) {
    std::string text = SAMPLE;
    text.replace(text.find("\"type\": \"sink\""), 14, "\"type\": \"mystery\"");
    LoadReport report;
    const Graph graph = load(text, report);
    ASSERT_TRUE(report.ok);
    ASSERT_EQ(report.unknownNodeTypes, std::vector<std::string>({"mystery"}));
    ASSERT_NE(graph.findNode("n10"), nullptr);
    EXPECT_EQ(graph.findNode("n10")->type, "mystery");
    EXPECT_TRUE(codes(graph).count("unknown-node-type"));
    EXPECT_NE(save(graph).find("\"mystery\""), std::string::npos) << "never deleted";
    // its edges are kept, unchecked
    EXPECT_FALSE(codes(graph).count("dangling-edge"));
}

TEST_F(GraphTest, KindVersionMigratedOnce) {
    migrationRuns = 0;
    const char* old = R"({"formatVersion": 1, "kind": "test", "kindVersion": 1,
        "nodes": [{"id": "n1", "type": "const", "position": [0, 0], "params": {"val": 4}}], "edges": []})";
    LoadReport report;
    Graph graph = load(old, report);
    ASSERT_TRUE(report.ok);
    EXPECT_EQ(report.migratedFrom, 1);
    EXPECT_EQ(migrationRuns, 1);
    EXPECT_EQ(graph.kindVersion, 2);
    EXPECT_EQ(graph.findNode("n1")->params["value"], 4);
    EXPECT_FALSE(graph.findNode("n1")->params.contains("val"));

    LoadReport again;
    load(save(graph), again);
    EXPECT_EQ(again.migratedFrom, 0) << "written at the current version";
    EXPECT_EQ(migrationRuns, 1);
}

TEST_F(GraphTest, NewerFormatIsRefused) {
    LoadReport report;
    load(R"({"formatVersion": 2, "kind": "test"})", report);
    EXPECT_FALSE(report.ok);
    load("not json", report);
    EXPECT_FALSE(report.ok);
}

TEST_F(GraphTest, EveryValidationCode) {
    const char* broken = R"({"formatVersion": 1, "kind": "test", "kindVersion": 2,
      "nodes": [
        {"id": "n1", "type": "const", "position": [0, 0], "params": {"value": 99}},
        {"id": "n2", "type": "add", "position": [0, 0], "params": {"mode": "weird"}},
        {"id": "n3", "type": "add", "position": [0, 0], "params": {}},
        {"id": "n3", "type": "add", "position": [0, 0], "params": {}},
        {"id": "n4", "type": "text", "position": [0, 0], "params": {}},
        {"id": "n5", "type": "unknownThing", "position": [0, 0], "params": {}}
      ],
      "edges": [
        {"id": "e1", "from": {"node": "n1", "port": "value"}, "to": {"node": "n9", "port": "a"}},
        {"id": "e2", "from": {"node": "n2", "port": "a"}, "to": {"node": "n3", "port": "a"}},
        {"id": "e3", "from": {"node": "n4", "port": "s"}, "to": {"node": "n2", "port": "a"}},
        {"id": "e4", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "b"}},
        {"id": "e5", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "b"}},
        {"id": "e6", "from": {"node": "n2", "port": "sum"}, "to": {"node": "n3", "port": "b"}},
        {"id": "e7", "from": {"node": "n3", "port": "sum"}, "to": {"node": "n2", "port": "a"}},
        {"id": "e7", "from": {"node": "n1", "port": "nope"}, "to": {"node": "n3", "port": "a"}}
      ]})";
    const std::set<std::string> found = codes(loadOk(broken));
    for (const char* code : {"dangling-edge", "duplicate-id", "direction", "type-mismatch", "multiplicity", "cycle",
                             "unknown-node-type", "missing-required-input", "param-range", "test-no-output"}) {
        EXPECT_TRUE(found.count(code)) << code;
    }
    EXPECT_TRUE(validate(loadOk(SAMPLE)).empty()) << "the sample is valid";
}

TEST_F(GraphTest, CanConnect) {
    auto kind = GraphRegistry::get().kind("test");
    ASSERT_TRUE(kind);
    EXPECT_TRUE(GraphRegistry::canConnect(kind.get(), "float", "float"));
    EXPECT_TRUE(GraphRegistry::canConnect(kind.get(), "float", "vector3")) << "declared implicit conversion";
    EXPECT_FALSE(GraphRegistry::canConnect(kind.get(), "vector3", "float"));
    EXPECT_TRUE(GraphRegistry::canConnect(kind.get(), "string", "any"));
    EXPECT_TRUE(GraphRegistry::canConnect(kind.get(), "any", "float"));
}

TEST_F(GraphTest, ApplyAndInverseRestoreEveryOp) {
    const std::vector<Json> ops = {
        {{"op", "addNode"}, {"node", {{"type", "add"}, {"position", {5, 5}}}}},
        {{"op", "addNode"}, {"node", {{"type", "sum"}, {"position", {5, 5}}, {"params", {{"count", 3}}}}}},
        {{"op", "removeNode"}, {"id", "n1"}},
        {{"op", "removeNode"}, {"id", "n2"}},
        {{"op", "moveNodes"}, {"ids", {"n1", "n2"}}, {"delta", {10, -4}}},
        {{"op", "moveNodes"}, {"positions", {{"n3", {1, 2}}}}},
        {{"op", "setParam"}, {"node", "n1"}, {"name", "value"}, {"value", 7}},
        {{"op", "setParam"}, {"node", "n1"}, {"name", "fresh"}, {"value", "x"}},
        {{"op", "setParam"}, {"node", "n1"}, {"name", "value"}, {"value", nullptr}},
        {{"op", "connect"}, {"from", {{"node", "n3"}, {"port", "out"}}}, {"to", {{"node", "n2"}, {"port", "b"}}}},
        {{"op", "disconnect"}, {"id", "e5"}},
        {{"op", "disconnect"}, {"from", {{"node", "n2"}, {"port", "sum"}}}, {"to", {{"node", "n10"}, {"port", "x"}}}},
        {{"op", "addVariable"}, {"variable", {{"name", "gain"}, {"type", "float"}, {"default", 2}}}},
        {{"op", "removeVariable"}, {"name", "speed"}},
        {{"op", "renameVariable"}, {"from", "speed"}, {"to", "velocity"}},
        {{"op", "setVariable"}, {"name", "speed"}, {"default", 9}, {"exposed", false}, {"group", "Motion"}},
        {{"op", "group"}, {"title", "Math"}, {"nodes", {"n2", "n3"}}},
        {{"op", "ungroup"}, {"id", "g7"}},
    };
    const std::string before = save(loadOk(SAMPLE));
    for (const Json& json : ops) {
        Graph graph = loadOk(before);
        const GraphOp op = opFromJson(json);
        const GraphOp inverse = apply(graph, op);
        EXPECT_NE(save(graph), before) << "did nothing: " << json.dump();
        const GraphOp redo = apply(graph, inverse);
        EXPECT_EQ(save(graph), before) << "inverse of " << json.dump() << " is " << toJson(inverse).dump();
        apply(graph, redo);
        apply(graph, opFromJson(toJson(apply(graph, inverse))));   // redo + undo again through JSON
    }
}

TEST_F(GraphTest, BadOpChangesNothing) {
    Graph graph = loadOk(SAMPLE);
    const std::string before = save(graph);
    EXPECT_THROW(apply(graph, opFromJson({{"op", "connect"}, {"from", {{"node", "n404"}, {"port", "x"}}}, {"to", {{"node", "n1"}, {"port", "a"}}}})), GraphOpError);
    EXPECT_THROW(apply(graph, opFromJson({{"op", "setParam"}, {"node", "n1"}})), GraphOpError);
    EXPECT_THROW(apply(graph, opFromJson({{"op", "explode"}})), GraphOpError);
    EXPECT_EQ(save(graph), before);
}

TEST_F(GraphTest, IdsAreNeverReused) {
    Graph graph = loadOk(SAMPLE);
    // the file says nextId 9 but has n10: loading moves nextId past every id in the file
    ASSERT_EQ(graph.nextId, 11u);
    const GraphOp added = opFromJson({{"op", "addNode"}, {"node", {{"type", "const"}}}});
    apply(graph, added);
    ASSERT_NE(graph.findNode("n11"), nullptr);
    apply(graph, opFromJson({{"op", "removeNode"}, {"id", "n11"}}));
    apply(graph, added);
    EXPECT_EQ(graph.findNode("n11"), nullptr) << "a deleted node's id is not given again";
    EXPECT_NE(graph.findNode("n12"), nullptr);
}


TEST_F(GraphTest, Bench1000Nodes2000Edges) {
    Graph graph;
    graph.kind = "test";
    graph.kindVersion = 2;
    for (int i = 0; i < 1000; i++) {
        Node node;
        node.id = "n" + std::to_string(i + 1);
        node.type = i % 2 == 0 ? "const" : "add";
        node.position = {static_cast<double>(i), static_cast<double>(i % 37)};
        node.params = {{"value", 1.0}};
        graph.nodes.push_back(node);
    }
    std::sort(graph.nodes.begin(), graph.nodes.end(), [](const Node& a, const Node& b) { return idLess(a.id, b.id); });
    for (int i = 0; i < 2000; i++) {
        Edge edge;
        edge.id = "e" + std::to_string(i + 1);
        const int from = (i % 500) * 2 + 1;        // const nodes (odd ids are n1, n3, ...)
        const int to = ((i * 7) % 500) * 2 + 2;    // add nodes
        edge.from = {"n" + std::to_string(from), "value", Json::object()};
        edge.to = {"n" + std::to_string(to), i % 2 ? "a" : "b", Json::object()};
        graph.edges.push_back(edge);
    }
    std::sort(graph.edges.begin(), graph.edges.end(), [](const Edge& a, const Edge& b) { return idLess(a.id, b.id); });
    graph.nextId = 3001;
    const std::string text = save(graph);

    const auto start = std::chrono::steady_clock::now();
    LoadReport report;
    Graph loaded = load(text, report);
    const std::vector<Issue> issues = validate(loaded);
    const std::string written = save(loaded);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(report.ok);
    EXPECT_EQ(written, text);

    const int ops = 2000;
    const auto opStart = std::chrono::steady_clock::now();
    for (int i = 0; i < ops; i++) {
        apply(loaded, GraphOp{"setParam", {{"node", "n" + std::to_string((i % 1000) + 1)}, {"name", "value"}, {"value", i}}});
    }
    const double usPerOp = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - opStart).count() / ops;
    std::printf("[bench] graph 1000 nodes / 2000 edges load+validate+save %.2f ms (budget 25), apply %.2f us/op (budget 5)\n", ms, usPerOp);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(ms, 25.0 * testsupport::perfSlack());
        EXPECT_LE(usPerOp, 5.0 * testsupport::perfSlack());
    }
}
