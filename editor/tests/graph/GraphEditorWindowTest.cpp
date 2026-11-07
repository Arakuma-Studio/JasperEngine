// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Graph Editor window and its document (S90 R26, R30, D7), on a kind of the test's own, driven
// through the functions its canvas, menus, search and Inspector call (AnimatorWindowTest's way):
// edits as undo steps that reach the document on undo through GraphEditCmd's listener (BF23 F12);
// outside edits reloaded; a commit over a changed file refused; nodes without a position laid out
// on open without a write, and Arrange as one step; the search filtered by a dropped wire's port,
// ranked with aliases and enum values, and the picked node wired with its port under the cursor; a
// second wire into a one-wire input replacing the first; Copy and Paste as JSON text; a Problems
// row's Fix; every panel and the Inspector's rows drawn in a frame; and the ramp values' JSON (32
// keys for a graph, the particles' 8 kept).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "graph/GraphDocument.h"
#include "graph/GraphParamValues.h"
#include "graph/GraphRegistry.h"
#include "widget/GradientEditorOps.h"
#include "window/GraphEditorWindow.h"

#include "imgui.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
// (the editor has its own Json, nlohmann::json; graph files are ordered)
using GraphJson = graph::Json;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-graph-window-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: graphproject\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    bool started = false;
    Project* project() {
        started = true;
        return headless().project.get();
    }
    fs::path assets() { return project()->getAssetsPath(); }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    struct Cleanup : public ::testing::Environment {
        void TearDown() override {
            if (!started) return;
            std::error_code ec;
            fs::remove_all(headless().dir.parent_path(), ec);
        }
    };
    [[maybe_unused]] ::testing::Environment* const cleanup = ::testing::AddGlobalTestEnvironment(new Cleanup);

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    graph::Port port(const char* id, graph::PortDirection direction, const char* type, const char* name, bool optional = false) {
        graph::Port p;
        p.id = id;
        p.direction = direction;
        p.type = type;
        p.name = name;
        p.optional = optional;
        return p;
    }

    // the kind every test here edits: Speed -> Blend (float), a Vector, a Tint with ramp parameters
    void registerKind() {
        graph::GraphKindDesc kind;
        kind.id = "window_test";
        kind.displayName = "Window Test Graph";
        kind.fileExtension = ".wtgraph";
        kind.acyclic = true;
        kind.portTypes = {{"float", 0xff8800ffu, {"vector3"}}, {"vector3", 0xffff00ffu, {}}};
        graph::NodeTypeDesc speed;
        speed.id = "speed";
        speed.displayName = "Speed";
        speed.category = "Input";
        speed.ports = {port("value", graph::PortDirection::Out, "float", "Speed")};
        speed.params = {{"value", "float", 1.0, 0.0, 100.0, {}}};
        graph::NodeTypeDesc blend;
        blend.id = "blend";
        blend.displayName = "Blend";
        blend.category = "Math";
        blend.aliases = {"mix", "lerp"};
        blend.ports = {port("a", graph::PortDirection::In, "float", "A"), port("b", graph::PortDirection::In, "float", "B", true),
                       port("out", graph::PortDirection::Out, "float", "Out")};
        blend.params = {{"weight", "float", 0.5, 0.0, 1.0, {}}, {"mode", "enum", "linear", std::nullopt, std::nullopt, {"linear", "multiply", "screen"}}};
        graph::NodeTypeDesc vec;
        vec.id = "vec";
        vec.displayName = "Vector";
        vec.category = "Input";
        vec.ports = {port("v", graph::PortDirection::Out, "vector3", "V")};
        graph::NodeTypeDesc tint;
        tint.id = "tint";
        tint.displayName = "Tint";
        tint.category = "Color";
        tint.ports = {port("in", graph::PortDirection::In, "vector3", "In", true), port("out", graph::PortDirection::Out, "vector3", "Out")};
        tint.params = {{"color", "color", GraphJson{{"r", 1.0}, {"g", 0.5}, {"b", 0.25}, {"a", 1.0}}, std::nullopt, std::nullopt, {}},
                       {"ramp", "gradient", GraphJson{{"points", {{{"pos", 0.0}, {"r", 0.0}, {"g", 0.0}, {"b", 0.0}, {"a", 1.0}},
                                                            {{"pos", 1.0}, {"r", 1.0}, {"g", 1.0}, {"b", 1.0}, {"a", 1.0}}}}, {"interpolation", 1}},
                        std::nullopt, std::nullopt, {}},
                       {"shape", "curve", GraphJson{{"points", {{{"x", 0.0}, {"y", 0.0}, {"ls", 0.0}, {"rs", 1.0}}, {{"x", 1.0}, {"y", 1.0}, {"ls", 1.0}, {"rs", 0.0}}}}},
                        std::nullopt, std::nullopt, {}},
                       {"label", "string", "", std::nullopt, std::nullopt, {}},
                       {"on", "bool", true, std::nullopt, std::nullopt, {}}};
        kind.nodeTypes = {speed, blend, vec, tint};
        graph::GraphRegistry::get().registerKind(kind);
    }

    class GraphEditorWindowTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready);
            registerKind();
        }
        void TearDown() override {
            GraphDocument::closeAll();
            graph::GraphRegistry::get().unregisterKind("window_test");
        }

        // a graph file under assets/graphs: Speed (n1) at 0,0 and Blend (n2) at 300,0, unwired
        static std::string writeGraph(const std::string& name, const std::string& nodes = std::string()) {
            const std::string body = nodes.empty() ? R"([{"id": "n1", "type": "speed", "position": [0, 0], "params": {"value": 1}},
                {"id": "n2", "type": "blend", "position": [300, 0], "params": {"weight": 0.5, "mode": "linear"}}])" : nodes;
            writeAll(assets() / "graphs" / (name + ".wtgraph"),
                     R"({"formatVersion": 1, "kind": "window_test", "kindVersion": 1, "nextId": 3, "nodes": )" + body + R"(, "edges": []})");
            return "assets/graphs/" + name + ".wtgraph";
        }
    };

    // an ImGui frame loop of its own (the window's show(), the Inspector)
    struct Frames {
        ImGuiContext* previous = nullptr;
        ImGuiContext* context = nullptr;
        Frames() {
            previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1400, 900);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~Frames() {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }
        bool frame(GraphEditorWindow& window) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            window.show();
            ImGui::Begin("Inspector");
            const bool drew = window.drawSelectionInspector();
            ImGui::End();
            ImGui::EndFrame();
            return drew;
        }
    };

    const graph::Edge* edgeInto(const graph::Graph& g, const std::string& node, const std::string& port) {
        for (const graph::Edge& edge : g.edges) {
            if (edge.to.node == node && edge.to.port == port) return &edge;
        }
        return nullptr;
    }

}

// BF23 F12: the window's steps, their undo and redo reach the document through GraphEditCmd's
// change listener, so the canvas never shows a graph the file no longer holds
TEST_F(GraphEditorWindowTest, AnEditIsOneUndoStepAndItsUndoReachesTheDocument) {
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph(writeGraph("undo"))) << window.getStatus();
    std::shared_ptr<GraphDocument> doc = window.getDocument();
    const std::string before = readAll(doc->absolutePath());
    const size_t start = history()->position();

    const std::string added = window.addNode("speed", ImVec2(0, 200));
    ASSERT_FALSE(added.empty()) << window.getStatus();
    EXPECT_EQ(history()->position(), start + 1) << "one step";
    EXPECT_NE(doc->graph().findNode(added), nullptr);
    EXPECT_EQ(window.selectedNodes(), std::vector<std::string>{added});

    ASSERT_TRUE(window.connect("n1", "value", "n2", "a"));
    EXPECT_EQ(history()->position(), start + 2);

    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(doc->graph().edges.size(), 0u) << "the undo reloaded the document";
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(doc->graph().findNode(added), nullptr);
    EXPECT_EQ(readAll(doc->absolutePath()), before);
    EXPECT_TRUE(window.selectedNodes().empty()) << "a selection whose node is gone is dropped";
    ASSERT_TRUE(history()->redo());
    EXPECT_NE(doc->graph().findNode(added), nullptr);
    ASSERT_TRUE(history()->undo());
}

TEST_F(GraphEditorWindowTest, OneDocumentPerPathAndOutsideChangesReload) {
    const std::string path = writeGraph("outside");
    std::string error;
    std::shared_ptr<GraphDocument> a = GraphDocument::open(project(), path, &error);
    std::shared_ptr<GraphDocument> b = GraphDocument::open(project(), "graphs/outside.wtgraph", &error);
    ASSERT_TRUE(a) << error;
    EXPECT_EQ(a, b) << "the project-relative and the asset-relative path are one document";
    int heard = 0;
    a->addListener([&]() { heard++; });

    // another program's write: the hot reload calls filesChanged, a missed one checkDiskChanges
    writeGraph("outside", R"([{"id": "n1", "type": "speed", "position": [0, 0], "params": {"value": 7}}])");
    GraphDocument::checkDiskChanges();
    EXPECT_EQ(heard, 1);
    EXPECT_EQ(a->graph().nodes.size(), 1u);

    // a file that stops parsing keeps the last good graph, with the error
    writeAll(a->absolutePath(), "{ not json");
    GraphDocument::filesChanged({a->absolutePath()});
    EXPECT_FALSE(a->loadError().empty());
    EXPECT_EQ(a->graph().nodes.size(), 1u);
    EXPECT_FALSE(a->editable());
}

TEST_F(GraphEditorWindowTest, AnEditOverAFileChangedOnDiskIsRefused) {
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph(writeGraph("race")));
    const size_t start = history()->position();
    // written behind the document's back, not reloaded yet
    writeGraph("race", R"([{"id": "n1", "type": "speed", "position": [50, 50], "params": {"value": 3}}])");
    EXPECT_FALSE(window.setParam("n1", "value", 9.0));
    EXPECT_NE(window.getStatus().find("changed on disk"), std::string::npos) << window.getStatus();
    EXPECT_EQ(history()->position(), start) << "no step";
    EXPECT_EQ(window.getDocument()->graph().nodes.size(), 1u) << "and the document reloaded";
}

// R30: an agent's nodes without a position open laid out, and opening writes nothing
TEST_F(GraphEditorWindowTest, NodesWithoutAPositionOpenLaidOutAndArrangeIsOneStep) {
    const std::string path = writeGraph("pile", R"([{"id": "n1", "type": "speed", "params": {}},
        {"id": "n2", "type": "speed", "params": {}}, {"id": "n3", "type": "blend", "params": {}}])");
    const std::string text = readAll(project()->getProjectPath() / path);
    GraphEditorWindow window(project());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(path));
    std::shared_ptr<GraphDocument> doc = window.getDocument();
    const graph::Vec2 a = doc->displayPosition(*doc->graph().findNode("n1"));
    const graph::Vec2 b = doc->displayPosition(*doc->graph().findNode("n2"));
    EXPECT_FALSE(a == b) << "not a pile";
    Frames frames;
    frames.frame(window);
    EXPECT_EQ(readAll(doc->absolutePath()), text) << "opening and drawing write nothing";

    const size_t start = history()->position();
    ASSERT_TRUE(window.arrange());
    EXPECT_EQ(history()->position(), start + 1);
    EXPECT_TRUE(graph::unplacedNodes(doc->graph()).empty());
    ASSERT_NE(history()->top(), nullptr);
    EXPECT_EQ(history()->top()->name(), "Arrange Nodes");
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(readAll(doc->absolutePath()), text);
}

TEST_F(GraphEditorWindowTest, AWireDroppedOnTheCanvasSearchesWhatFitsAndPlacesItsPortUnderTheCursor) {
    GraphEditorWindow window(project());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeGraph("drop")));
    Frames frames;
    frames.frame(window);
    // Speed's float output: only nodes with an input a float feeds (Blend; Tint by the implicit vector3)
    const GraphCanvas::PortRef source{1, "value", true};
    std::vector<std::string> types;
    for (const auto& entry : window.searchNodes("", &source)) types.push_back(entry.type);
    EXPECT_EQ(types, (std::vector<std::string>{"tint", "blend"})) << "by category: Color, then Math";
    // Vector's output feeds no Blend input (vector3 does not become float)
    ASSERT_EQ(window.addNode("vec", ImVec2(0, 300)), "n3");
    frames.frame(window);
    const GraphCanvas::PortRef vectorOut{3, "v", true};
    for (const auto& entry : window.searchNodes("blend", &vectorOut)) EXPECT_NE(entry.type, "blend");

    const ImVec2 drop(500, 120);
    window.openSearch(drop, source);
    ASSERT_TRUE(window.isSearchOpen());
    const size_t start = history()->position();
    const auto entries = window.searchNodes("blend", &source);
    ASSERT_FALSE(entries.empty());
    ASSERT_EQ(entries[0].type, "blend");
    EXPECT_EQ(entries[0].port, "a");
    const std::string added = window.pickSearchEntry(entries[0]);
    ASSERT_FALSE(added.empty()) << window.getStatus();
    EXPECT_FALSE(window.isSearchOpen());
    EXPECT_EQ(history()->position(), start + 1) << "the node and its wire: one step";
    const graph::Graph& g = window.getDocument()->graph();
    const graph::Edge* wire = edgeInto(g, added, "a");
    ASSERT_NE(wire, nullptr);
    EXPECT_EQ(wire->from.node, "n1");
    // the joined port lands where the wire was dropped
    const GraphCanvas::Node shape = window.canvasNode(*g.findNode(added));
    const ImVec2 at = GraphCanvas::portPosition(shape, "a", false);
    EXPECT_NEAR(at.x, drop.x, 1.0f);
    EXPECT_NEAR(at.y, drop.y, 1.0f);
}

TEST_F(GraphEditorWindowTest, TheSearchRanksNamesAliasesAndEnumValues) {
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph(writeGraph("search")));
    auto first = [&](const std::string& query) {
        const auto entries = window.searchNodes(query);
        return entries.empty() ? std::string() : entries[0].label;
    };
    EXPECT_EQ(first("blend"), "Blend");
    EXPECT_EQ(first("mix"), "Blend") << "an alias";
    EXPECT_EQ(first("spd"), "Speed") << "fuzzy";
    EXPECT_EQ(first("multiply"), "Blend: Multiply") << "an enum value as an entry";
    for (const auto& entry : window.searchNodes("")) EXPECT_TRUE(entry.param.empty()) << "enum entries only once the user types";

    window.openSearch(ImVec2(0, 400), std::nullopt);
    const auto entries = window.searchNodes("multiply");
    const std::string added = window.pickSearchEntry(entries[0]);
    ASSERT_FALSE(added.empty());
    EXPECT_EQ(window.getDocument()->graph().findNode(added)->params.value("mode", std::string()), "multiply");
    EXPECT_EQ(window.getDocument()->graph().findNode(added)->params.value("weight", 0.0), 0.5) << "the other defaults too";
}

TEST_F(GraphEditorWindowTest, AWireIntoAOneWireInputReplacesItsWireAndTypesAreChecked) {
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph(writeGraph("wires")));
    const std::string other = window.addNode("speed", ImVec2(0, 200));
    const std::string vec = window.addNode("vec", ImVec2(0, 400));
    ASSERT_TRUE(window.connect("n1", "value", "n2", "a"));
    ASSERT_TRUE(window.connect(other, "value", "n2", "a"));
    const graph::Graph& g = window.getDocument()->graph();
    ASSERT_EQ(g.edges.size(), 1u);
    EXPECT_EQ(g.edges[0].from.node, other);
    EXPECT_FALSE(window.connect(vec, "v", "n2", "b")) << "a vector3 does not feed a float";
    EXPECT_FALSE(window.connect("n2", "out", "n2", "b")) << "a node to itself";
    EXPECT_FALSE(window.connect("n2", "out", "n1", "value")) << "an output to an output";
    // acyclic: Blend -> Blend2 -> Blend would loop
    const std::string blend2 = window.addNode("blend", ImVec2(600, 0));
    ASSERT_TRUE(window.connect("n2", "out", blend2, "a"));
    EXPECT_FALSE(window.canConnect(blend2, "out", "n2", "b"));
}

TEST_F(GraphEditorWindowTest, CopyAndPasteCarryNodesAndTheirWiresAsJsonText) {
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph(writeGraph("clip")));
    ASSERT_TRUE(window.connect("n1", "value", "n2", "a"));
    window.selectNodes({"n1", "n2"});
    const std::string text = window.copyText();
    const GraphJson clip = GraphJson::parse(text);
    EXPECT_EQ(clip.value("graph_clipboard", std::string()), "window_test");
    EXPECT_EQ(clip["nodes"].size(), 2u);
    EXPECT_EQ(clip["edges"].size(), 1u);

    const size_t start = history()->position();
    ASSERT_TRUE(window.pasteText(text, ImVec2(1000, 1000)));
    EXPECT_EQ(history()->position(), start + 1);
    const graph::Graph& g = window.getDocument()->graph();
    ASSERT_EQ(g.nodes.size(), 4u);
    ASSERT_EQ(g.edges.size(), 2u);
    const std::vector<std::string> pasted = window.selectedNodes();
    ASSERT_EQ(pasted.size(), 2u);
    EXPECT_EQ(std::count(pasted.begin(), pasted.end(), "n1"), 0) << "new ids";
    const graph::Node* first = g.findNode(pasted[0]);
    EXPECT_EQ(first->position.x, 1000.0) << "the fragment's top-left at the paste point";
    EXPECT_EQ(edgeInto(g, pasted[1], "a")->from.node, pasted[0]) << "the wire between the copies";

    // another kind's clipboard, and text that is not a graph's, are refused
    EXPECT_FALSE(window.pasteText(R"({"graph_clipboard": "other", "nodes": [{"id": "n1", "type": "x"}]})", ImVec2(0, 0)));
    EXPECT_FALSE(window.pasteText("hello", ImVec2(0, 0)));
    // a value as JSON text
    EXPECT_EQ(window.copyValueText("n2", "weight"), "0.5");
    ASSERT_TRUE(window.pasteValueText("n2", "weight", "0.25"));
    EXPECT_EQ(g.findNode("n2")->params.value("weight", 0.0), 0.25);
}

TEST_F(GraphEditorWindowTest, AProblemSelectsItsNodeAndFixAppliesItsOneEdit) {
    // a wire to a node that is gone
    writeAll(assets() / "graphs/dangling.wtgraph", R"({"formatVersion": 1, "kind": "window_test", "kindVersion": 1, "nextId": 5,
        "nodes": [{"id": "n1", "type": "speed", "position": [0, 0], "params": {}},
                  {"id": "n2", "type": "blend", "position": [300, 0], "params": {}}],
        "edges": [{"id": "e4", "from": {"node": "n9", "port": "value"}, "to": {"node": "n2", "port": "a"}}]})");
    GraphEditorWindow window(project());
    ASSERT_TRUE(window.openGraph("assets/graphs/dangling.wtgraph"));
    std::vector<GraphEditorWindow::Problem> rows = window.problems();
    auto fixable = std::find_if(rows.begin(), rows.end(), [](const auto& p) { return !p.fix.empty(); });
    ASSERT_NE(fixable, rows.end());
    EXPECT_EQ(fixable->code, "dangling-edge");
    auto onNode = std::find_if(rows.begin(), rows.end(), [](const auto& p) { return p.nodeId == "n2"; });
    ASSERT_NE(onNode, rows.end()) << "Blend's input a is required and unwired";
    EXPECT_TRUE(window.inspectProblem(*onNode));
    EXPECT_EQ(window.selectedNodes(), std::vector<std::string>{"n2"});
    ASSERT_TRUE(window.fixProblem(*fixable));
    EXPECT_TRUE(window.getDocument()->graph().edges.empty());
}

// every panel drawn, the Inspector's rows for each parameter kind, an unknown kind read-only
TEST_F(GraphEditorWindowTest, EveryPanelAndTheInspectorRowsDraw) {
    GraphEditorWindow window(project());
    window.setOpen(true);
    ASSERT_TRUE(window.openGraph(writeGraph("panels")));
    const std::string tint = window.addNode("tint", ImVec2(0, 300));
    ASSERT_TRUE(window.connect("n1", "value", "n2", "a"));
    Frames frames;
    frames.frame(window);
    window.selectNodes({tint});
    EXPECT_TRUE(frames.frame(window)) << "the Inspector shows the node";
    window.openSearch(ImVec2(100, 100), std::nullopt);
    frames.frame(window);
    frames.frame(window);
    EXPECT_TRUE(window.isSearchOpen());
    window.closeSearch();
    ImVec2 min, max;
    ASSERT_TRUE(window.nodeRect(tint, min, max));
    EXPECT_GT(max.y - min.y, GraphCanvas::HEADER_HEIGHT) << "ports and their rows";

    // a graph of a kind this build does not know: shown, never written
    writeAll(assets() / "graphs/foreign.wtgraph", R"({"formatVersion": 1, "kind": "window_test", "kindVersion": 1, "nextId": 2,
        "nodes": [{"id": "n1", "type": "future_node", "position": [0, 0], "params": {"x": 1}}], "edges": []})");
    ASSERT_TRUE(window.openGraph("assets/graphs/foreign.wtgraph"));
    frames.frame(window);
    window.selectNodes({"n1"});
    EXPECT_TRUE(frames.frame(window)) << "an unknown type's values show as JSON";
}

TEST(GraphParamValues, ColoursGradientsAndCurvesRoundTripInTheirShape) {
    Vector4 color;
    ASSERT_TRUE(graphvalues::colorFrom(GraphJson{{"r", 1.0}, {"g", 0.5}, {"b", 0.25}}, color));
    EXPECT_EQ(color.w, 1.0f);
    EXPECT_TRUE(graphvalues::colorJson(color, GraphJson::array({0, 0, 0})).is_array()) << "an array stays an array";
    EXPECT_EQ(graphvalues::colorJson(color, GraphJson::array({0, 0, 0})).size(), 3u);
    EXPECT_EQ(graphvalues::colorJson(color, GraphJson::object()).value("g", 0.0), 0.5);
    EXPECT_FALSE(graphvalues::colorFrom(GraphJson("red"), color));

    // a ramp of 20 keys: more than the particles' 8, within a graph's 32
    GraphJson points = GraphJson::array();
    for (int i = 0; i < 20; i++) points.push_back({{"pos", i / 19.0}, {"r", i / 19.0}, {"g", 0.0}, {"b", 0.0}, {"a", 1.0}});
    Gradient gradient;
    ASSERT_TRUE(graphvalues::gradientFrom(GraphJson{{"points", points}, {"interpolation", 2}}, gradient));
    EXPECT_EQ(gradient.colorKeys.size(), 20u);
    GraphJson out;
    std::string error;
    ASSERT_TRUE(graphvalues::gradientJson(gradient, GraphJson{{"points", points}, {"interpolation", 2}}, out, error)) << error;
    EXPECT_EQ(out["points"].size(), 20u);
    EXPECT_EQ(out.value("interpolation", 0), 2) << "a smooth ramp stays smooth";
    gradient.mode = GradientMode::Fixed;
    ASSERT_TRUE(graphvalues::gradientJson(gradient, out, out, error));
    EXPECT_EQ(out.value("interpolation", 1), 0);
    for (int i = 0; i < 13; i++) gradient.alphaKeys.push_back({0.01f + 0.07f * (float)i, 1.0f});
    EXPECT_FALSE(graphvalues::gradientJson(gradient, out, out, error)) << "33 key places";
    EXPECT_FALSE(error.empty());

    // the editor's gradient keys: a graph's field allows 32, the particles' default stays 8
    Gradient keys = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    while (gradientops::addColorKey(keys, 0.5f, graphvalues::RAMP_KEYS_MAX) >= 0) {}
    EXPECT_EQ(keys.colorKeys.size(), graphvalues::RAMP_KEYS_MAX);
    Gradient particle = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    while (gradientops::addColorKey(particle, 0.5f) >= 0) {}
    EXPECT_EQ(particle.colorKeys.size(), Gradient::kMaxKeys);

    Curve curve;
    ASSERT_TRUE(graphvalues::curveFrom(GraphJson{{"points", {{{"x", 0.0}, {"y", 0.0}, {"ls", 0.0}, {"rs", 2.0}}, {{"x", 1.0}, {"y", 1.0}, {"ls", 0.5}, {"rs", 0.0}}}}}, curve));
    EXPECT_EQ(curve.keys[0].outTangent, 2.0f);
    EXPECT_EQ(curve.keys[1].inTangent, 0.5f);
    ASSERT_TRUE(graphvalues::curveJson(curve, GraphJson::object(), out, error));
    EXPECT_EQ(out["points"][0].value("rs", 0.0), 2.0);
}
