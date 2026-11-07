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
  "metadata": {"author": "test", "seed": 7},
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
        {{"op", "setMetadata"}, {"key", "seed"}, {"value", 12}},
        {{"op", "setMetadata"}, {"key", "presets"}, {"value", Json::array({{{"name", "Wet"}}})}},
        // removing the first key: its undo puts it back first
        {{"op", "setMetadata"}, {"key", "author"}, {"value", nullptr}},
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

// S90 R30: a node an agent wrote with no position stays without one through a load and a save,
// so an editor keeps laying it out; the layout op places those, and its undo takes them back
TEST_F(GraphTest, NodesWithoutAPositionAreLaidOutByTheLayoutOp) {
    const std::string text = R"({"formatVersion": 1, "kind": "test", "kindVersion": 2, "nextId": 1, "nodes": [
        {"id": "n1", "type": "const", "params": {"value": 1}},
        {"id": "n2", "type": "const", "params": {"value": 2}},
        {"id": "n3", "type": "add", "params": {"mode": "linear"}},
        {"id": "n4", "type": "sink", "position": [-500.0, 40.0], "params": {}}],
      "edges": [{"id": "e1", "from": {"node": "n1", "port": "value"}, "to": {"node": "n3", "port": "a"}},
                {"id": "e2", "from": {"node": "n2", "port": "value"}, "to": {"node": "n3", "port": "b"}}]})";
    Graph graph = loadOk(text);
    EXPECT_EQ(unplacedNodes(graph), (std::vector<std::string>{"n1", "n2", "n3"}));
    EXPECT_EQ(save(graph).find("\"position\""), save(graph).rfind("\"position\"")) << "only n4 has a position after a save";

    const std::string before = save(graph);
    const GraphOp inverse = apply(graph, opFromJson({{"op", "layout"}}));
    EXPECT_TRUE(unplacedNodes(graph).empty());
    const Node* a = graph.findNode("n1");
    const Node* b = graph.findNode("n2");
    const Node* blend = graph.findNode("n3");
    EXPECT_EQ(a->position.x, b->position.x) << "both sources in the first column";
    EXPECT_NE(a->position.y, b->position.y);
    EXPECT_EQ(blend->position.x, a->position.x + LAYOUT_COLUMN) << "what they feed one column right";
    EXPECT_GT(a->position.x, graph.findNode("n4")->position.x) << "to the right of the placed node, not on it";
    EXPECT_EQ(graph.findNode("n4")->position.x, -500.0) << "the placed node stays";
    EXPECT_EQ(describe(graph, opFromJson({{"op", "layout"}})), "Arrange Nodes");

    apply(graph, inverse);
    EXPECT_EQ(save(graph), before) << "the undo takes the positions away again";
    EXPECT_EQ(unplacedNodes(graph).size(), 3u);

    // a move places a node; the move's undo takes its position away
    const GraphOp unmove = apply(graph, opFromJson({{"op", "moveNodes"}, {"positions", {{"n1", {7, 8}}}}}));
    EXPECT_TRUE(graph.findNode("n1")->placed);
    apply(graph, unmove);
    EXPECT_EQ(save(graph), before);

    // with ids: those nodes, whatever they had
    apply(graph, opFromJson({{"op", "layout"}, {"ids", {"n4"}}}));
    EXPECT_EQ(graph.findNode("n4")->position.x, 0.0) << "alone among unplaced nodes: at the origin";
    EXPECT_THROW(apply(graph, opFromJson({{"op", "layout"}, {"ids", {"n404"}}})), GraphOpError);
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

// ---- BF23 F5, F9: a kind that opts in to strict parameters

namespace {

    void registerStrictKind() {
        GraphKindDesc kind;
        kind.id = "strict";
        kind.fileExtension = ".strictgraph";
        kind.strictParams = true;
        kind.portTypes = {{"float", 0xff8800ffu, {}}};
        NodeTypeDesc speed;
        speed.id = "speed";
        speed.ports = {port("value", PortDirection::Out, "float")};
        speed.params = {{"value", "float", 1.0, 0.0, 100.0, {}},
                        {"mode", "enum", "linear", std::nullopt, std::nullopt, {"linear", "smooth"}}};
        kind.nodeTypes = {speed};
        GraphRegistry::get().registerKind(kind);
    }

    class StrictGraphTest : public ::testing::Test {
    protected:
        void SetUp() override { registerStrictKind(); }
        void TearDown() override { GraphRegistry::get().unregisterKind("strict"); }

        static Issue only(const Graph& graph, const char* code) {
            std::vector<Issue> found;
            for (const Issue& issue : validate(graph)) {
                if (issue.code == code) found.push_back(issue);
            }
            EXPECT_EQ(found.size(), 1u) << code;
            return found.empty() ? Issue() : found.front();
        }
    };

    Graph strictGraph(const char* node) {
        LoadReport report;
        Graph graph = load(std::string(R"({"formatVersion": 1, "kind": "strict", "nodes": [)") + node + "]}", report);
        EXPECT_TRUE(report.ok) << "load never refuses: " << report.error;
        return graph;
    }

}

TEST_F(StrictGraphTest, MisspelledParameterIsAnError) {
    const Issue issue = only(strictGraph(R"({"id": "n1", "type": "speed", "params": {"valeu": 3}})"), "unknown-param");
    EXPECT_EQ(issue.severity, Severity::Error);
    EXPECT_EQ(issue.nodeId, "n1");
    EXPECT_EQ(issue.suggestion, "did you mean 'value'?");
    EXPECT_EQ(toJson(issue).value("suggestion", std::string()), "did you mean 'value'?");
}

TEST_F(StrictGraphTest, NothingCloseListsTheParameters) {
    const Issue issue = only(strictGraph(R"({"id": "n1", "type": "speed", "params": {"elevation": 3}})"), "unknown-param");
    EXPECT_EQ(issue.suggestion, "its parameters: value, mode");
}

TEST_F(StrictGraphTest, UnknownNodeTypeIsAnError) {
    const Issue issue = only(strictGraph(R"({"id": "n1", "type": "sped", "params": {}})"), "unknown-node-type");
    EXPECT_EQ(issue.severity, Severity::Error);
    EXPECT_EQ(issue.suggestion, "did you mean 'speed'?");
}

TEST_F(StrictGraphTest, OutOfRangeIsAnError) {
    EXPECT_EQ(only(strictGraph(R"({"id": "n1", "type": "speed", "params": {"value": 500}})"), "param-range").severity, Severity::Error);
    const Issue value = only(strictGraph(R"({"id": "n1", "type": "speed", "params": {"mode": "smoth"}})"), "param-range");
    EXPECT_EQ(value.severity, Severity::Error);
    EXPECT_EQ(value.suggestion, "did you mean 'smooth'?");
}

TEST_F(StrictGraphTest, LoadKeepsWhatValidateRefuses) {
    const char* text = R"({"id": "n1", "type": "future", "position": [0.0, 0.0], "params": {"valeu": 3}})";
    Graph graph = strictGraph(text);
    ASSERT_EQ(graph.nodes.size(), 1u);
    EXPECT_EQ(graph.nodes[0].type, "future");
    EXPECT_EQ(graph.nodes[0].params.value("valeu", 0), 3);
    LoadReport again;
    load(save(graph), again);
    EXPECT_TRUE(again.ok);
}

TEST_F(GraphTest, AKindThatDidNotOptInKeepsItsWarnings) {
    const Graph graph = loadOk(R"({"formatVersion": 1, "kind": "test", "kindVersion": 2, "nodes": [
        {"id": "n1", "type": "const", "params": {"valeu": 3, "value": 99}},
        {"id": "n2", "type": "cosnt", "params": {}}]})");
    std::set<std::string> found;
    for (const Issue& issue : validate(graph)) {
        found.insert(issue.code);
        EXPECT_NE(issue.severity, Severity::Error) << issue.code << ": " << issue.message;
    }
    EXPECT_FALSE(found.count("unknown-param")) << "S23's forward compatibility";
    EXPECT_TRUE(found.count("param-range"));
    EXPECT_TRUE(found.count("unknown-node-type"));
}

// ---- BF23 F7: a supplied id does not skip the type's defaults, an undo stays exact

TEST_F(GraphTest, AddNodeWithAnIdGetsDefaultsAndDynamicPorts) {
    Graph graph = loadOk(SAMPLE);
    apply(graph, opFromJson({{"op", "addNode"}, {"node", {{"id", "bricks"}, {"type", "const"}}}}));
    const Node* constant = graph.findNode("bricks");
    ASSERT_NE(constant, nullptr);
    EXPECT_EQ(constant->params.value("value", 0.0), 1.0);

    apply(graph, opFromJson({{"op", "addNode"}, {"node", {{"id", "total"}, {"type", "sum"}}}}));
    const Node* sum = graph.findNode("total");
    ASSERT_NE(sum, nullptr);
    EXPECT_EQ(sum->params.value("count", 0), 2);
    ASSERT_TRUE(sum->dynamicPorts.has_value());
    EXPECT_EQ(sum->dynamicPorts->size(), 3u);

    // given values win, and the ports follow them
    apply(graph, opFromJson({{"op", "addNode"}, {"node", {{"id", "wide"}, {"type", "sum"}, {"params", {{"count", 4}}}}}}));
    EXPECT_EQ(graph.findNode("wide")->params.value("count", 0), 4);
    EXPECT_EQ(graph.findNode("wide")->dynamicPorts->size(), 5u);
}

TEST_F(GraphTest, UndoOfARemoveRestoresTheNodeExactly) {
    Graph graph = loadOk(SAMPLE);
    // a node without its default: a removal and its undo must not bring the default back
    apply(graph, opFromJson({{"op", "setParam"}, {"node", "n1"}, {"name", "value"}, {"value", nullptr}}));
    const std::string before = save(graph);
    const GraphOp inverse = apply(graph, opFromJson({{"op", "removeNode"}, {"id", "n1"}}));
    ASSERT_EQ(inverse.op, "addNode");
    EXPECT_EQ(inverse.args.value("restore", false), true) << "the undo path says so explicitly";
    const GraphOp redo = apply(graph, opFromJson(toJson(inverse)));
    EXPECT_EQ(save(graph), before);
    apply(graph, redo);
    apply(graph, inverse);
    EXPECT_EQ(save(graph), before);
}

// ---- BF23 F8: hand-written edges without ids

TEST_F(GraphTest, LoadGivesEdgesWithoutIdsTheirOwn) {
    const char* text = R"({"formatVersion": 1, "kind": "test", "kindVersion": 2, "nextId": 2,
      "nodes": [{"id": "n1", "type": "const", "params": {}}, {"id": "n2", "type": "add", "params": {}},
                {"id": "n20", "type": "sink", "params": {}}],
      "edges": [{"from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "a"}},
                {"id": "e30", "from": {"node": "n1", "port": "value"}, "to": {"node": "n2", "port": "b"}},
                {"id": "", "from": {"node": "n2", "port": "sum"}, "to": {"node": "n20", "port": "x"}}],
      "groups": [{"id": "g40", "title": "", "rect": [0, 0, 1, 1], "nodes": []}]})";
    const Graph graph = loadOk(text);
    EXPECT_FALSE(codes(graph).count("duplicate-id"));
    ASSERT_EQ(graph.edges.size(), 3u);
    // past every id of the file (nextId says 2, the group holds 40), in file order, deterministic
    EXPECT_EQ(graph.edges[0].id, "e30");
    EXPECT_EQ(graph.edges[1].id, "e41");
    EXPECT_EQ(graph.edges[2].id, "e42");
    EXPECT_EQ(graph.edges[1].from.port, "value");
    EXPECT_EQ(graph.edges[2].from.port, "sum");
    EXPECT_EQ(graph.nextId, 43u);
    EXPECT_EQ(save(loadOk(text)), save(graph));
    // once saved, they are the file's
    EXPECT_EQ(save(loadOk(save(graph))), save(graph));
}

// ---- BF23 F11: a kind's extension names its files, whatever kind it is

TEST_F(GraphTest, KindForFileMatchesAnyRegisteredExtension) {
    GraphKindDesc dotted;
    dotted.id = "dotted";
    dotted.fileExtension = ".proc.mat";
    GraphRegistry::get().registerKind(dotted);
    const auto idOf = [](std::string_view path) {
        const auto kind = GraphRegistry::get().kindForFile(path);
        return kind ? kind->id : std::string();
    };
    EXPECT_EQ(idOf("graphs/a.testgraph"), "test");
    EXPECT_EQ(idOf("graphs/A.TESTGRAPH"), "test");
    EXPECT_EQ(idOf("a.proc.mat"), "dotted");
    EXPECT_EQ(idOf("a.mat"), "");
    EXPECT_EQ(idOf("graphs/atestgraph"), "");
    EXPECT_EQ(idOf(".testgraph"), "") << "a hidden file with no name";
    GraphRegistry::get().unregisterKind("dotted");
    EXPECT_EQ(idOf("a.proc.mat"), "");
}
