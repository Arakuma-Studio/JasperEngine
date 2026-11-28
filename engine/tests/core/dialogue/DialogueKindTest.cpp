// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The dialogue kind's lint and keys (docs/specs/dialogue-graphs.md R1-R3, R11, AC6): one fixture
// per code, which reports exactly that code at that node.

#include "DialogueTestSupport.h"

#include "graph/GraphRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace DialogueKindTest_cpp {

using namespace jasperengine;
using namespace jasperengine::dialogue;
using dialoguetest::GraphText;
using dialoguetest::Json;

namespace {

    class DialogueKindTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerDialogueKind();
            setLintContext(nullptr);
        }
        void TearDown() override { setLintContext(nullptr); }

        // the issues of the dialogue's own codes (the generic ones left out)
        static std::vector<graph::Issue> lintOf(const GraphText& g, const std::string& guid = "") {
            std::vector<graph::Issue> issues;
            lint(g.graph(guid), issues);
            return issues;
        }
        static std::vector<std::string> codesAt(const std::vector<graph::Issue>& issues, const std::string& code) {
            std::vector<std::string> nodes;
            for (const graph::Issue& issue : issues) {
                if (issue.code == code) nodes.push_back(issue.nodeId);
            }
            return nodes;
        }
        static std::vector<std::string> codes(const std::vector<graph::Issue>& issues) {
            std::vector<std::string> out;
            for (const graph::Issue& issue : issues) out.push_back(issue.code);
            return out;
        }
    };

    // entry -> line -> end: clean
    GraphText base() {
        GraphText g;
        g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"text", "Hello"}}).node("n3", "end");
        g.wire("n1", "next", "n2").wire("n2", "next", "n3");
        return g;
    }

}

TEST_F(DialogueKindTest, RegisteredWithItsNodeTypes) {
    auto kind = graph::GraphRegistry::get().kind("dialogue");
    ASSERT_TRUE(kind);
    EXPECT_EQ(kind->fileExtension, ".dialogue");
    EXPECT_EQ(kind->kindVersion, 1);
    EXPECT_FALSE(kind->acyclic);
    EXPECT_TRUE(kind->strictParams);
    std::vector<std::string> types;
    for (const graph::NodeTypeDesc& type : kind->nodeTypes) types.push_back(type.id);
    EXPECT_EQ(types, (std::vector<std::string>{"entry", "line", "branch", "set", "action", "jump", "end"}));
    // the base fixture is clean, generic checks and lint alike
    EXPECT_TRUE(graph::validate(base().graph()).empty());
}

TEST_F(DialogueKindTest, NoEntryIsAnError) {
    GraphText g;
    g.node("n2", "line", {{"text", "Hello"}});
    EXPECT_EQ(codes(lintOf(g)), (std::vector<std::string>{"no-entry", "unreachable"}));
}

TEST_F(DialogueKindTest, DuplicateEntryIsAnError) {
    GraphText g = base();
    g.node("n4", "entry", {{"name", "start"}}).wire("n4", "next", "n2");
    EXPECT_EQ(codes(lintOf(g)), std::vector<std::string>{"duplicate-entry"});
    EXPECT_EQ(codesAt(lintOf(g), "duplicate-entry"), std::vector<std::string>{"n4"});
}

TEST_F(DialogueKindTest, FanOutIsAnError) {
    GraphText g = base();
    g.node("n4", "end").wire("n2", "next", "n4");
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codes(issues), std::vector<std::string>{"fan-out"});
    EXPECT_EQ(issues[0].nodeId, "n2");
    EXPECT_EQ(issues[0].portId, "next");
    EXPECT_EQ(issues[0].severity, graph::Severity::Error);
}

TEST_F(DialogueKindTest, BadExpressionNamesTheColumn) {
    GraphText g = base();
    g.node("n4", "branch", {{"condition", "$gold >"}}).node("n5", "end");
    g.wire("n1", "next", "n4");
    g.edges.erase(g.edges.begin());   // n1 -> n2 replaced by n1 -> n4
    g.wire("n4", "true", "n2").wire("n4", "false", "n5");
    g.variable("gold", 0);
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codes(issues), std::vector<std::string>{"bad-expression"});
    EXPECT_EQ(issues[0].nodeId, "n4");
    EXPECT_NE(issues[0].message.find("column 8"), std::string::npos) << issues[0].message;
}

TEST_F(DialogueKindTest, JumpTargetMustExist) {
    GraphText g = base();
    g.node("n4", "jump", {{"entry", "nowhere"}});
    g.edges.erase(g.edges.end() - 1);
    g.wire("n2", "next", "n4").node("n5", "jump", {{"dialogue", {{"path", "assets/other.dialogue"}}}, {"entry", "x"}});
    g.node("n6", "entry", {{"name", "side"}}).wire("n6", "next", "n5");
    // n3 is now unreachable too
    std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codesAt(issues, "jump-target"), std::vector<std::string>{"n4"}) << "another file is only checked with a context";
    auto context = std::make_shared<LintContext>();
    context->jumpTarget = [](const std::string& path, const std::string& target) {
        return path == "assets/other.dialogue" && target == "x" ? std::string("assets/other.dialogue has no entry 'x'") : std::string();
    };
    setLintContext(context);
    issues = lintOf(g);
    EXPECT_EQ(codesAt(issues, "jump-target"), (std::vector<std::string>{"n4", "n5"}));
}

TEST_F(DialogueKindTest, UnreachableCountsJumpsFromOtherFiles) {
    GraphText g = base();
    g.node("n4", "line", {{"text", "Side door"}}).node("n5", "end").wire("n4", "next", "n5");
    EXPECT_EQ(codesAt(lintOf(g, "guid-x"), "unreachable"), (std::vector<std::string>{"n4", "n5"}));
    // another dialogue jumps to n4: reachable
    auto context = std::make_shared<LintContext>();
    context->jumpedInto = [](const std::string& guid) { return guid == "guid-x" ? std::set<std::string>{"n4"} : std::set<std::string>{}; };
    setLintContext(context);
    EXPECT_TRUE(codesAt(lintOf(g, "guid-x"), "unreachable").empty());
}

TEST_F(DialogueKindTest, NoExitLoopIsReported) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "line", {{"text", "Again?"}, {"options", {{{"id", "o1"}, {"text", "Yes"}}, {{"id", "o2"}, {"text", "Sure"}}}}})
     .node("n3", "set", {{"assign", "$n += 1"}});
    g.wire("n1", "next", "n2").wire("n2", "o1", "n3").wire("n2", "o2", "n3").wire("n3", "next", "n2");
    g.variable("n", 0);
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codes(issues), std::vector<std::string>{"no-exit"});
    EXPECT_EQ(issues[0].nodeId, "n2");
    EXPECT_NE(issues[0].message.find("n2, n3"), std::string::npos) << issues[0].message;
    // a conditional option may hide every option: the unconnected next is a way out
    GraphText open = g;
    open.nodes[1]["params"]["options"][0]["condition"] = "$n < 3";
    open.nodes[1]["params"]["options"][1]["once"] = true;
    EXPECT_TRUE(codesAt(lintOf(open), "no-exit").empty());
}

TEST_F(DialogueKindTest, UnknownPrefixOnlyWhenPrefixesDeclared) {
    GraphText g = base();
    g.nodes[1]["params"]["action"] = "quest.start:intro";
    g.nodes[1]["params"]["options"] = {{{"id", "o1"}, {"text", "Buy"}, {"condition", "shop.canAfford(3)"}}};
    EXPECT_TRUE(codesAt(lintOf(g), "unknown-action-prefix").empty()) << "nothing declared: no prefix check";
    auto context = std::make_shared<LintContext>();
    context->actionPrefixes = {"shop"};
    context->conditionPrefixes = {"quest"};
    setLintContext(context);
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codesAt(issues, "unknown-action-prefix"), std::vector<std::string>{"n2"});
    EXPECT_EQ(codesAt(issues, "unknown-condition-prefix"), std::vector<std::string>{"n2"});
}

TEST_F(DialogueKindTest, UnknownVariableUnlessDeclared) {
    GraphText g = base();
    g.nodes[1]["params"]["action"] = "$met = true; $gold += 1";
    g.variable("gold", 0);
    EXPECT_EQ(codesAt(lintOf(g), "unknown-variable"), std::vector<std::string>{"n2"});
    auto context = std::make_shared<LintContext>();
    context->variables = {"met"};
    setLintContext(context);
    EXPECT_TRUE(codesAt(lintOf(g), "unknown-variable").empty());
}

TEST_F(DialogueKindTest, UnknownArgumentInText) {
    GraphText g = base();
    g.nodes[1]["params"]["text"] = "Hi {player}, {gold, plural, one {# coin} other {# coins}} and {mystery}";
    g.nodes[0]["params"]["arguments"] = "player";
    g.variable("gold", 0);
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codesAt(issues, "unknown-argument"), std::vector<std::string>{"n2"});
    EXPECT_NE(issues[0].message.find("{mystery}"), std::string::npos) << issues[0].message;
}

TEST_F(DialogueKindTest, EmptyLineIsAWarning) {
    GraphText g = base();
    g.nodes[1]["params"]["text"] = "";
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codes(issues), std::vector<std::string>{"empty-line"});
    EXPECT_EQ(issues[0].severity, graph::Severity::Warning);
}

TEST_F(DialogueKindTest, BadMessageIsAnError) {
    GraphText g = base();
    g.nodes[1]["params"]["text"] = "{n, plural, one {# coin}}";
    g.variable("n", 0);
    EXPECT_EQ(codes(lintOf(g)), std::vector<std::string>{"bad-message"});
}

TEST_F(DialogueKindTest, DuplicateOptionTextIsInfo) {
    GraphText g = base();
    g.nodes[1]["params"]["options"] = {{{"id", "o1"}, {"text", "Bye"}}, {{"id", "o2"}, {"text", "Bye"}}};
    const std::vector<graph::Issue> issues = lintOf(g);
    EXPECT_EQ(codes(issues), std::vector<std::string>{"duplicate-option-text"});
    EXPECT_EQ(issues[0].severity, graph::Severity::Info);
}

TEST_F(DialogueKindTest, KeysAreStableAcrossEdits) {
    GraphText g = base();
    g.nodes[1]["params"]["variants"] = {{{"id", "v1"}, {"text", "Hi"}}, {{"id", "v2"}, {"text", "Hey"}}};
    g.nodes[1]["params"]["options"] = {{{"id", "o1"}, {"text", "Yes"}}, {{"id", "o2"}, {"text", "No"}}};
    auto keys = [](const graph::Graph& graph) {
        std::map<std::string, std::string> out;
        for (const SourceEntry& entry : sourceEntries(graph)) out[entry.key] = entry.text;
        return out;
    };
    const std::map<std::string, std::string> before = keys(g.graph());
    EXPECT_EQ(before, (std::map<std::string, std::string>{{"n2", "Hello"}, {"n2.v1", "Hi"}, {"n2.v2", "Hey"}, {"n2.o1", "Yes"}, {"n2.o2", "No"}}));
    // reorder the options, move the node, delete a variant: the other keys keep their texts
    graph::Graph edited = g.graph();
    graph::Node* line = edited.findNode("n2");
    graph::Json options = line->params["options"];
    std::reverse(options.begin(), options.end());
    graph::apply(edited, graph::opFromJson({{"op", "setParam"}, {"node", "n2"}, {"name", "options"}, {"value", options}}));
    graph::apply(edited, graph::opFromJson({{"op", "moveNodes"}, {"ids", {"n2"}}, {"delta", {300, 40}}}));
    Json oneVariant = Json::array();
    oneVariant.push_back({{"id", "v2"}, {"text", "Hey"}});
    graph::apply(edited, graph::opFromJson({{"op", "setParam"}, {"node", "n2"}, {"name", "variants"}, {"value", oneVariant}}));
    std::map<std::string, std::string> after = keys(edited);
    EXPECT_EQ(after.count("n2.v1"), 0u);
    for (const auto& [key, text] : after) { EXPECT_EQ(before.at(key), text) << key; }
    EXPECT_EQ(after.size(), 4u);
    // the option ports follow the reorder: the wires stay on their option ids
    std::vector<std::string> ports;
    for (const graph::Port& port : graph::portsOf(edited, *edited.findNode("n2"))) ports.push_back(port.id);
    EXPECT_EQ(ports, (std::vector<std::string>{"in", "next", "o2", "o1"}));
    // the runtime keys carry the namespace
    Runner runner;
    runner.setDialogue(g.program());
    runner.start();
    EXPECT_EQ(runner.getCurrentLine()->options[1].key, "t.n2.o2");
}

TEST_F(DialogueKindTest, MessageArgumentsSkipQuotedText) {
    EXPECT_EQ(messageArguments("{a} '{b}' {c, number} {d, plural, one {# {e}} other {x}}"), (std::vector<std::string>{"a", "c", "d", "e"}));
}

}  // namespace DialogueKindTest_cpp
