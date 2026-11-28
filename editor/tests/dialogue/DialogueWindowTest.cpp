// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Window > Dialogue (docs/specs/dialogue-graphs.md R19-R21, AC10), driven through the functions its
// canvas, Inspector, preview and Problems strip call (GraphEditorWindowTest's way).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "TempDir.h"

#include "Engine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "dialogue/DialogueEditor.h"
#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueVariables.h"
#include "dialogue/DialogueWindow.h"
#include "graph/GraphDocument.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace DialogueWindowTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using WindowJson = graph::Json;

namespace {

    class DialogueWindowTest : public ::testing::Test {
    protected:
        jasperengine::test::TempDir dir;
        Project project;
        std::unique_ptr<DialogueWindow> window;
        fs::path file;

        void SetUp() override {
            editor::test::ensureEngine();
            std::string error;
            ASSERT_TRUE(project.createProjectAt(dir.path() / "DialogueWindow", "DialogueWindow", &error)) << error;
            Engine::setAsyncLoading(false);
            dialogues::install(&project);
            window = std::make_unique<DialogueWindow>(&project);
            file = project.getAssetsPath() / "shop.dialogue";
            fs::create_directories(file.parent_path());
            // entry -> line n2 with two options: o1 "Buy" (if shop.canAfford(5)) -> n3 end "bought", o2 "Leave" -> n4 end "left"
            graph::Graph g;
            g.kind = "dialogue";
            dialogues::prepareNew(&project, file, g, false);
            auto add = [&](const WindowJson& node) { graph::apply(g, graph::opFromJson({{"op", "addNode"}, {"node", node}})); };
            add({{"id", "n1"}, {"type", "entry"}, {"position", {0, 0}}, {"params", {{"name", "start"}}}});
            add({{"id", "n2"}, {"type", "line"}, {"position", {200, 0}}, {"params", {{"speaker", "smith"}, {"text", "What will it be?"},
                 {"options", {{{"id", "o1"}, {"text", "Buy"}, {"condition", "shop.canAfford(5)"}, {"action", "shop.pay(5); $bought = true"}},
                              {{"id", "o2"}, {"text", "Leave"}}}}, {"next_sub_id", 3}}}});
            add({{"id", "n3"}, {"type", "end"}, {"position", {400, 0}}, {"params", {{"result", "bought"}}}});
            add({{"id", "n4"}, {"type", "end"}, {"position", {400, 200}}, {"params", {{"result", "left"}}}});
            auto wire = [&](const char* from, const char* port, const char* to) {
                graph::apply(g, graph::opFromJson({{"op", "connect"}, {"from", {{"node", from}, {"port", port}}}, {"to", {{"node", to}, {"port", "in"}}}}));
            };
            wire("n1", "next", "n2");
            wire("n2", "o1", "n3");
            wire("n2", "o2", "n4");
            graph::apply(g, graph::opFromJson({{"op", "addVariable"}, {"variable", {{"name", "bought"}, {"type", "bool"}, {"default", false}}}}));
            std::ofstream(file, std::ios::binary) << graph::save(g);
            ASSERT_TRUE(window->openGraph(file.string())) << window->getStatus();
        }
        void TearDown() override {
            window.reset();
            GraphDocument::closeAll();
            dialogues::uninstall();
            dialogue::Handlers::clear();
            project.releaseScenes();
        }
        const graph::Graph& graph() { return window->getDocument()->graph(); }
        CommandHistory* history() { return project.getProjectCommandHistory(); }
    };

}

TEST_F(DialogueWindowTest, OpensDialogueFilesInItsWindow) {
    // (the editor App another test made has its own Dialogue window: any Dialogue window, never the Graph Editor)
    GraphEditorWindow* opens = GraphEditorWindow::windowFor(file.string());
    ASSERT_NE(opens, nullptr);
    EXPECT_NE(dynamic_cast<DialogueWindow*>(opens), nullptr) << "a .dialogue opens in Window > Dialogue";
    EXPECT_EQ(opens->kindId(), "dialogue");
    EXPECT_EQ(window->windowName(), std::string(DialogueWindow::WINDOW_NAME));
    EXPECT_EQ(DialogueWindow::instance(), window.get());
    window->setOpen(false);
    ASSERT_TRUE(window->reveal(file.string(), "n2"));
    EXPECT_TRUE(window->isOpen());
    EXPECT_EQ(window->selectedNodes(), std::vector<std::string>{"n2"});
    // a card's text follows the window's language
    EXPECT_EQ(window->displayText(*graph().findNode("n2")), "What will it be?");
}

TEST_F(DialogueWindowTest, InspectorFieldEditIsOneUndoStep) {
    const size_t before = history()->position();
    ASSERT_TRUE(window->setParam("n2", "speaker", WindowJson("blacksmith")));
    EXPECT_EQ(history()->position(), before + 1);
    EXPECT_EQ(graph().findNode("n2")->params.value("speaker", std::string()), "blacksmith");
    ASSERT_TRUE(window->setOptionField("n2", "o2", "style", WindowJson("exit")));
    EXPECT_EQ(history()->position(), before + 2);
    ASSERT_TRUE(window->addOption("n2", "Haggle"));
    EXPECT_EQ(history()->position(), before + 3) << "the option and next_sub_id in one step";
    EXPECT_EQ(graph().findNode("n2")->params["options"][2].value("id", std::string()), "o3");
    EXPECT_EQ(graph().findNode("n2")->params.value("next_sub_id", 0), 4);
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(graph().findNode("n2")->params["options"].size(), 2u);
    EXPECT_EQ(graph().findNode("n2")->params.value("next_sub_id", 0), 3);
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(graph().findNode("n2")->params["options"][1].contains("style"));
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(graph().findNode("n2")->params.value("speaker", std::string()), "smith");
}

TEST_F(DialogueWindowTest, OptionReorderKeepsIdsAndWires) {
    ASSERT_TRUE(window->moveOption("n2", 0, 1));
    const WindowJson& options = graph().findNode("n2")->params["options"];
    EXPECT_EQ(options[0].value("id", std::string()), "o2");
    EXPECT_EQ(options[1].value("id", std::string()), "o1");
    // the wires stay on their option ids, the ports follow the new order, the graph has no new error
    const graph::Edge* buy = nullptr;
    for (const graph::Edge& edge : graph().edges) {
        if (edge.from.node == "n2" && edge.from.port == "o1") buy = &edge;
    }
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(buy->to.node, "n3");
    std::vector<std::string> ports;
    for (const graph::Port& port : graph::portsOf(graph(), *graph().findNode("n2"))) ports.push_back(port.id);
    EXPECT_EQ(ports, (std::vector<std::string>{"in", "next", "o2", "o1"}));
    for (const graph::Issue& issue : graph::validate(graph())) { EXPECT_NE(issue.severity, graph::Severity::Error) << issue.message; }
    // removing an option takes its wire in the same step
    ASSERT_TRUE(window->removeOption("n2", "o1"));
    for (const graph::Edge& edge : graph().edges) { EXPECT_FALSE(edge.from.node == "n2" && edge.from.port == "o1"); }
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(graph().edges.size(), 3u);
}

TEST_F(DialogueWindowTest, PreviewWalksWithOverrides) {
    int called = 0;
    dialogue::Handlers::onCondition("shop", [&](dialogue::Runner&, std::string_view, std::span<const dialogue::Value>) {
        called++;
        return dialogue::Value(true);
    });
    dialogue::Handlers::onAction("shop", [&](dialogue::Runner&, std::string_view, std::span<const dialogue::Value>) {
        called++;
        return false;
    });
    window->previewStart();
    ASSERT_TRUE(window->previewRunning());
    const dialogues::WalkResult& shown = window->previewResult();
    ASSERT_EQ(shown.state, "line");
    ASSERT_EQ(shown.transcript.back().options.size(), 1u) << "shop.* asks: the Buy option's condition is false";
    EXPECT_EQ(shown.unknownCalls, std::vector<std::string>{"shop.canAfford(5)"});
    // the asked call taken true: Buy shows; choosing it lists its action, never runs it
    window->answerCondition("shop.canAfford(5)", true);
    ASSERT_EQ(window->previewResult().transcript.back().options.size(), 2u);
    ASSERT_TRUE(window->previewChoose(1));
    EXPECT_EQ(window->previewResult().state, "ended");
    EXPECT_EQ(window->previewResult().result, "bought");
    EXPECT_EQ(window->previewActions(), std::vector<std::string>{"shop.pay(5)"});
    EXPECT_EQ(called, 0) << "the preview never calls a handler";
    // Back takes the choice back
    ASSERT_TRUE(window->previewBack());
    EXPECT_EQ(window->previewResult().state, "line");
    // a prefix set false, a start value edited
    window->setConditionMode("shop", "false");
    EXPECT_TRUE(window->previewResult().unknownCalls.empty());
    window->setPreviewVariable("bought", dialogue::Value(true));
    bool listed = false;
    for (const auto& [name, value] : window->previewResult().variables) listed = listed || (name == "bought" && value.same(dialogue::Value(true)));
    EXPECT_TRUE(listed);
    EXPECT_TRUE(dialogue::Variables::get("bought").isNil()) << "the game's store is left as it was";
}

TEST_F(DialogueWindowTest, ProblemsStripFixDeclaresVariable) {
    ASSERT_TRUE(window->setParam("n2", "action", WindowJson("$met = true")));
    const GraphEditorWindow::Problem* declare = nullptr;
    const std::vector<GraphEditorWindow::Problem> problems = window->problems();
    for (const GraphEditorWindow::Problem& problem : problems) {
        if (problem.code == "unknown-variable") declare = &problem;
    }
    ASSERT_NE(declare, nullptr);
    EXPECT_EQ(declare->fixLabel, "Declare Variable $met");
    ASSERT_TRUE(window->fixProblem(*declare));
    ASSERT_NE(graph().findVariable("met"), nullptr);
    for (const GraphEditorWindow::Problem& problem : window->problems()) { EXPECT_NE(problem.code, "unknown-variable") << problem.message; }
    // an undeclared prefix, with prefixes declared: Fix declares it in Project Settings, one undo step
    DialogueSettings settings;
    settings.actionPrefixes.push_back({"quest", "", ""});
    project.getDialogueSettings() = settings;
    const GraphEditorWindow::Problem* prefix = nullptr;
    const std::vector<GraphEditorWindow::Problem> withPrefixes = window->problems();
    for (const GraphEditorWindow::Problem& problem : withPrefixes) {
        if (problem.code == "unknown-action-prefix") prefix = &problem;
    }
    ASSERT_NE(prefix, nullptr);
    ASSERT_TRUE(window->fixProblem(*prefix));
    ASSERT_EQ(project.getDialogueSettings().actionPrefixes.size(), 2u);
    EXPECT_EQ(project.getDialogueSettings().actionPrefixes[1].name, "shop");
    ASSERT_TRUE(history()->undo());
    EXPECT_EQ(project.getDialogueSettings().actionPrefixes.size(), 1u);
}

}  // namespace DialogueWindowTest_cpp
