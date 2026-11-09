// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R1-R3, AC1: the .procmat kind over S23's graph model.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/PassPlanner.h"
#include "../../procmat/ProceduralMaterialKind.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    std::shared_ptr<const NodeLibrary> repositoryLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    const char* SAMPLE = R"({
  "formatVersion": 1,
  "kind": "procedural_material",
  "kindVersion": 1,
  "nextId": 5,
  "nodes": [
    {
      "id": "n1",
      "type": "perlin",
      "position": [0, 0],
      "params": {
        "scale_x": 8,
        "seed": 3
      }
    },
    {
      "id": "n2",
      "type": "blend",
      "position": [200, 0],
      "params": {
        "blend_type": "multiply"
      },
      "anUnknownNodeKey": true
    },
    {
      "id": "n3",
      "type": "material_output",
      "position": [400, 0],
      "params": {}
    }
  ],
  "edges": [
    {
      "id": "e4",
      "from": {
        "node": "n1",
        "port": "output"
      },
      "to": {
        "node": "n3",
        "port": "roughness"
      }
    }
  ],
  "variables": [],
  "groups": [],
  "viewport": {
    "offset": [0, 0],
    "zoom": 1.0
  },
  "metadata": {},
  "guid": "0123456789abcdef0123456789abcdef",
  "anUnknownFileKey": {"kept": [1, 2, 3]}
}
)";

    class GraphProcMatTest : public ::testing::Test {
    protected:
        void SetUp() override {
            library = repositoryLibrary();
            ASSERT_TRUE(library);
            graph::GraphRegistry::get().registerKind(makeProceduralMaterialKind(library));
        }
        void TearDown() override { graph::GraphRegistry::get().unregisterKind(PROCEDURAL_MATERIAL_KIND); }

        std::vector<std::string> paramIssues(const char* type, const graph::Json& params) {
            graph::Graph file;
            file.kind = PROCEDURAL_MATERIAL_KIND;
            graph::Node node;
            node.id = "n1";
            node.type = type;
            node.params = params;
            file.nodes.push_back(std::move(node));
            std::vector<std::string> codes;
            for (const graph::Issue& issue : graph::validate(file)) {
                if (issue.severity == graph::Severity::Error) codes.push_back(issue.code);
            }
            return codes;
        }

        std::shared_ptr<const NodeLibrary> library;
    };

}

TEST_F(GraphProcMatTest, RoundTripsByteIdentically) {
    graph::LoadReport report;
    const graph::Graph first = graph::load(SAMPLE, report);
    ASSERT_TRUE(report.ok) << report.error;
    EXPECT_FALSE(report.unknownKind);
    EXPECT_EQ(report.migratedFrom, 0);
    const std::string written = graph::save(first);
    graph::LoadReport again;
    const graph::Graph second = graph::load(written, again);
    ASSERT_TRUE(again.ok);
    EXPECT_EQ(graph::save(second), written);
    EXPECT_NE(written.find("\"guid\": \"0123456789abcdef0123456789abcdef\""), std::string::npos) << "R1: the GUID is in the file";
    EXPECT_NE(written.find("anUnknownFileKey"), std::string::npos) << "R40: unknown keys stay at file level";
    EXPECT_NE(written.find("anUnknownNodeKey"), std::string::npos);
}

TEST_F(GraphProcMatTest, MigratesFromAnOlderKindVersion) {
    // the kind as a later build would have it: version 2, where perlin's scale_x was renamed
    graph::GraphKindDesc kind = makeProceduralMaterialKind(library);
    kind.kindVersion = 2;
    kind.migrations.push_back({2, [](graph::Json& file) {
        for (graph::Json& node : file["nodes"]) {
            if (node.value("type", std::string()) != "perlin") continue;
            node["params"]["scale_u"] = node["params"]["scale_x"];
            node["params"].erase("scale_x");
        }
    }});
    graph::GraphRegistry::get().registerKind(std::move(kind));

    graph::LoadReport report;
    const graph::Graph file = graph::load(SAMPLE, report);
    ASSERT_TRUE(report.ok) << report.error;
    EXPECT_EQ(report.migratedFrom, 1);
    EXPECT_EQ(file.kindVersion, 2);
    const graph::Node* perlin = file.findNode("n1");
    ASSERT_TRUE(perlin);
    EXPECT_EQ(perlin->params.value("scale_u", 0.0), 8.0);
    EXPECT_FALSE(perlin->params.contains("scale_x"));
}

TEST_F(GraphProcMatTest, TheCatalogComesFromTheDescriptors) {
    auto kind = graph::GraphRegistry::get().kindForExtension(".procmat");
    ASSERT_TRUE(kind);
    EXPECT_EQ(kind->id, "procedural_material");
    EXPECT_EQ(kind->kindVersion, 1);
    EXPECT_TRUE(kind->acyclic);
    EXPECT_EQ(kind->nodeTypes.size(), library->descriptors().size() + 2 - NodeLibrary::leftOutNodes().size()) << "the descriptors, material_output and buffer";

    const graph::NodeTypeDesc* perlin = graph::GraphRegistry::nodeType(*kind, "perlin");
    ASSERT_TRUE(perlin);
    EXPECT_EQ(perlin->displayName, "Value") << "the catalog's name wins over the descriptor's";
    EXPECT_EQ(perlin->category, "Noise");
    ASSERT_EQ(perlin->ports.size(), 1u);
    EXPECT_EQ(perlin->ports[0].id, "output");
    EXPECT_EQ(perlin->ports[0].type, "f");
    const auto scale = std::find_if(perlin->params.begin(), perlin->params.end(), [](const graph::ParamDesc& param) { return param.name == "scale_x"; });
    ASSERT_NE(scale, perlin->params.end());
    EXPECT_EQ(scale->kind, "float");
    EXPECT_EQ(scale->min, 1.0);
    EXPECT_EQ(scale->max, 32.0);

    const graph::NodeTypeDesc* blend = graph::GraphRegistry::nodeType(*kind, "blend");
    ASSERT_TRUE(blend);
    const auto mode = std::find_if(blend->params.begin(), blend->params.end(), [](const graph::ParamDesc& param) { return param.name == "blend_type"; });
    ASSERT_NE(mode, blend->params.end());
    // a file names enum values; the descriptor stores its default as an index
    const Json* descriptor = library->descriptor("blend");
    ASSERT_NE(descriptor, nullptr);
    const Json& definition = *std::find_if((*descriptor)["parameters"].begin(), (*descriptor)["parameters"].end(), [](const Json& p) { return p["name"] == "blend_type"; });
    EXPECT_EQ(mode->defaultValue, enumValueIds(definition).at(definition["default"].get<size_t>()));
    EXPECT_NE(std::find(mode->enumValues.begin(), mode->enumValues.end(), "hard_light"), mode->enumValues.end());

    EXPECT_TRUE(graph::GraphRegistry::canConnect(kind.get(), "f", "rgba")) << "the library's own conversion";
    EXPECT_FALSE(graph::GraphRegistry::canConnect(kind.get(), "sdf2d", "tex3d"));
}

TEST_F(GraphProcMatTest, AGenericNodesPortsFollowItsSize) {
    auto kind = graph::GraphRegistry::get().kind(PROCEDURAL_MATERIAL_KIND);
    const graph::NodeTypeDesc* blend = graph::GraphRegistry::nodeType(*kind, "blend2");
    ASSERT_TRUE(blend);
    ASSERT_TRUE(blend->dynamicPorts);
    const auto inputs = [&](int size) {
        std::vector<std::string> ids;
        for (const graph::Port& port : blend->dynamicPorts(graph::Json{{"generic_size", size}})) {
            if (port.direction == graph::PortDirection::In) ids.push_back(port.id);
        }
        return ids;
    };
    EXPECT_NE(std::find(inputs(2).begin(), inputs(2).end(), "l2"), inputs(2).end());
    const std::vector<std::string> one = inputs(1);
    EXPECT_EQ(std::find(one.begin(), one.end(), "l2"), one.end());
}

TEST_F(GraphProcMatTest, AValueOfTheWrongTypeIsAnError) {
    const std::vector<std::string> paramType = {"param-type"};
    EXPECT_TRUE(paramIssues("perlin", {{"scale_x", 8}}).empty());
    // R3: text is what could reach GLSL, and a float field takes numbers only
    EXPECT_EQ(paramIssues("perlin", {{"scale_x", "1.0); return vec4(0.0); //"}}), paramType);
    EXPECT_EQ(paramIssues("perlin", {{"scale_x", "$(expr:1+1)"}}), paramType);
    EXPECT_EQ(paramIssues("perlin", {{"scale_x", true}}), paramType);
    EXPECT_EQ(paramIssues("perlin", {{"seed", 1.5}}), paramType);
    EXPECT_EQ(paramIssues("blend", {{"blend_type", "not_a_mode"}}), std::vector<std::string>{"param-range"}) << "S23's strict check, once";
    EXPECT_EQ(paramIssues("blend", {{"blend_type", 99}}), paramType);
    EXPECT_TRUE(paramIssues("blend", {{"blend_type", "multiply"}}).empty());
    EXPECT_TRUE(paramIssues("blend", {{"blend_type", 2}}).empty());
    EXPECT_EQ(paramIssues("colorize", {{"gradient", "red"}}), paramType);
    graph::Json keys = graph::Json::array();
    for (int i = 0; i < 33; i++) keys.push_back({{"pos", i / 33.0}, {"r", 0}, {"g", 0}, {"b", 0}, {"a", 1}});
    EXPECT_EQ(paramIssues("colorize", {{"gradient", {{"points", keys}}}}), paramType) << "R26: at most 32 keys";
    keys.erase(keys.size() - 1);
    EXPECT_TRUE(paramIssues("colorize", {{"gradient", {{"points", keys}}}}).empty());
}

TEST_F(GraphProcMatTest, APlanRefusesWhatTheValidationOnlyWarnsAbout) {
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node node;
    node.id = "n1";
    node.type = "perlin";
    node.params = {{"scale_x", 1000}};
    file.nodes.push_back(node);
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    EXPECT_FALSE(planGraph(file, library, {{"out", "n1", "output"}}, options, plan, error));
    EXPECT_NE(error.find("outside its range"), std::string::npos) << error;
    file.nodes[0].params = {{"scale_x", 8}};
    error.clear();
    EXPECT_TRUE(planGraph(file, library, {{"out", "n1", "output"}}, options, plan, error)) << error;
}

TEST_F(GraphProcMatTest, AStoredVersionThisBuildLacksIsRefused) {
    // R2: drawing it with today's code would change its look silently
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node node;
    node.id = "n1";
    node.type = "perlin";
    node.passthrough = {{"typeVersion", 2}};
    file.nodes.push_back(node);
    bool found = false;
    for (const graph::Issue& issue : graph::validate(file)) found = found || (issue.code == "param-type" && issue.message.find("version") != std::string::npos);
    EXPECT_TRUE(found);
    PassPlan plan;
    std::string error;
    PlanOptions options;
    options.size = 64;
    EXPECT_FALSE(planGraph(file, library, {{"out", "n1", "output"}}, options, plan, error));
    file.nodes[0].passthrough = {{"typeVersion", 1}};
    EXPECT_TRUE(planGraph(file, library, {{"out", "n1", "output"}}, options, plan, error)) << error;
}

TEST_F(GraphProcMatTest, EveryCatalogPresetIsAValidNode) {
    // what Assets > Create and the search would place: an entry's values pass the check they will meet
    int checked = 0;
    for (const auto& [id, descriptor] : library->descriptors()) {
        const Json* entry = library->catalogEntry(id);
        if (!entry || !entry->contains("parameters")) continue;
        graph::Node node;
        node.id = "n1";
        node.type = id;
        for (auto it = (*entry)["parameters"].begin(); it != (*entry)["parameters"].end(); ++it) {
            // a preset's enum is an index, and only the graph's own values are ranges to hold
            node.params[it.key()] = *it;
        }
        for (const std::string& problem : checkNodeParameters(descriptor, node, false)) ADD_FAILURE() << id << ": " << problem;
        checked++;
    }
    EXPECT_GT(checked, 150) << "the catalog has presets for most nodes";
}

TEST_F(GraphProcMatTest, AMisspelledParameterIsAnErrorWithASuggestion) {
    // R28: the kind is strict, so edit_graph refuses what validate reports as an error
    auto kind = graph::GraphRegistry::get().kind(PROCEDURAL_MATERIAL_KIND);
    ASSERT_TRUE(kind);
    EXPECT_TRUE(kind->strictParams);
    graph::Graph file;
    file.kind = PROCEDURAL_MATERIAL_KIND;
    graph::Node node;
    node.id = "n1";
    node.type = "perlin";
    node.params = {{"scale_z", 8}};
    file.nodes.push_back(node);
    const std::vector<graph::Issue> issues = graph::validate(file);
    const auto unknown = std::find_if(issues.begin(), issues.end(), [](const graph::Issue& issue) { return issue.code == "unknown-param"; });
    ASSERT_NE(unknown, issues.end());
    EXPECT_EQ(unknown->severity, graph::Severity::Error);
    EXPECT_NE(unknown->suggestion.find("scale_x"), std::string::npos) << unknown->suggestion;
    file.nodes[0].params = {{"scale_x", 1000}};
    bool range = false;
    for (const graph::Issue& issue : graph::validate(file)) range = range || (issue.code == "param-range" && issue.severity == graph::Severity::Error);
    EXPECT_TRUE(range) << "out of range is an error too";
}

TEST_F(GraphProcMatTest, ShapesAndImagesHaveBounds) {
    // R3: every number that becomes a cast, a table or a loop in GLSL has a range a file cannot leave
    const std::vector<std::string> paramType = {"param-type"};
    const auto image = [](double w, double h, double bpp, graph::Json px, size_t colors) {
        graph::Json palette = graph::Json::array();
        for (size_t i = 0; i < colors; i++) palette.push_back({{"r", 0}, {"g", 0}, {"b", 0}, {"a", 1}});
        return graph::Json{{"w", w}, {"h", h}, {"bpp", bpp}, {"pl", palette}, {"px", px}};
    };
    EXPECT_TRUE(paramIssues("pixels", {{"pixels", image(8, 8, 1, {42, 42}, 2)}}).empty()) << "the library's own default";
    EXPECT_EQ(paramIssues("pixels", {{"pixels", image(1e300, 8, 1, {42, 42}, 2)}}), paramType);
    EXPECT_EQ(paramIssues("pixels", {{"pixels", image(8, 8, 1, {42}, 2)}}), paramType) << "too few words";
    EXPECT_EQ(paramIssues("pixels", {{"pixels", image(8, 8, 1, {1e300, 42}, 2)}}), paramType);
    EXPECT_EQ(paramIssues("pixels", {{"pixels", image(4, 4, 2, {3}, 2)}}), paramType) << "pixel 0 is colour 3 of 2";
    EXPECT_EQ(paramIssues("pixels", {{"pixels", image(4, 4, 1, {0}, 3)}}), paramType) << "3 colours in 1 bit";
    graph::Json points = graph::Json::array();
    for (int i = 0; i < 65; i++) points.push_back({{"x", 0}, {"y", 0}});
    EXPECT_EQ(paramIssues("sdpolygon", {{"polygon", {{"points", points}}}}), paramType);
    const graph::Json corners = graph::Json::array({{{"x", 0}, {"y", 0}}, {{"x", 1}, {"y", 0}}, {{"x", 0}, {"y", 1}}, {{"x", 1}, {"y", 1}}});
    EXPECT_TRUE(paramIssues("distort", {{"lattice", {{"points", corners}, {"size", {{"x", 1}, {"y", 1}}}}}}).empty());
    EXPECT_EQ(paramIssues("distort", {{"lattice", {{"points", corners}, {"size", {{"x", 1e300}, {"y", 1}}}}}}), paramType);
    EXPECT_EQ(paramIssues("distort", {{"lattice", {{"points", corners}, {"size", {{"x", 2}, {"y", 1}}}}}}), paramType) << "6 points for 2 x 1";
    // a generic node's ports from a huge size: clamped as a number, never cast first
    const graph::NodeTypeDesc* blend = graph::GraphRegistry::nodeType(*graph::GraphRegistry::get().kind(PROCEDURAL_MATERIAL_KIND), "blend2");
    ASSERT_TRUE(blend && blend->dynamicPorts);
    EXPECT_FALSE(blend->dynamicPorts(graph::Json{{"generic_size", 1e300}}).empty());
}
