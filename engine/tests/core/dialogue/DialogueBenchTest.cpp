// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Dialogue load and step costs (docs/specs/dialogue-graphs.md R16-R17, AC13), printed as [bench]
// lines; the wall-clock budgets apply through perfSlack() (the benchmark server), the memory and
// allocation counts always. A game loads the cooked file an export ships (R16, D-15).

#include "DialogueTestSupport.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

namespace DialogueBenchTest_cpp {

using namespace jasperengine;
using namespace jasperengine::dialogue;
using dialoguetest::GraphText;
using dialoguetest::Json;

namespace {

    using Clock = std::chrono::steady_clock;

    double msSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // 500 nodes: an entry and 499 lines of 3 options each (1,497 option texts, a few variants
    // making 1,500 texts), every output wired: about 2,000 edges
    std::string largeDialogue() {
        GraphText g;
        g.node("n1", "entry", {{"name", "start"}});
        const int lines = 499;
        for (int i = 0; i < lines; i++) {
            const std::string id = "n" + std::to_string(i + 2);
            Json options = Json::array();
            for (int k = 1; k <= 3; k++) {
                Json option = {{"id", "o" + std::to_string(k)}, {"text", "Option " + std::to_string(k) + " of line " + std::to_string(i) + ", a reply."}};
                if (k == 2) option["condition"] = "$trust > " + std::to_string(i % 7);
                if (k == 3) option["action"] = "$trust += 1";
                options.push_back(option);
            }
            Json params = {{"speaker", "npc"}, {"text", "Line " + std::to_string(i) + ": what the character says here, at some length."},
                           {"options", options}, {"next_sub_id", 4}};
            if (i < 3) params["variants"] = {{{"id", "v1"}, {"text", "Another way to say line " + std::to_string(i) + "."}}};
            g.node(id, "line", params);
        }
        g.wire("n1", "next", "n2");
        for (int i = 0; i < lines; i++) {
            const std::string id = "n" + std::to_string(i + 2);
            auto target = [&](int offset) { return "n" + std::to_string(2 + (i + offset) % lines); };
            g.wire(id, "next", target(1)).wire(id, "o1", target(2)).wire(id, "o2", target(3)).wire(id, "o3", target(5));
        }
        g.variable("trust", 0);
        return g.text("bench-guid");
    }

}

TEST(DialogueBench, Load500) {
    registerDialogueKind();
    const std::string text = largeDialogue();
    // the editor's (and the export's) load: the JSON parsed into the graph model, then compiled
    std::shared_ptr<const Program> compiled;
    double bestParse = 1e9, bestCompile = 1e9;
    for (int round = 0; round < 5; round++) {
        const Clock::time_point start = Clock::now();
        graph::LoadReport report;
        const graph::Graph parsed = graph::load(text, report);
        bestParse = std::min(bestParse, msSince(start));
        const Clock::time_point compiling = Clock::now();
        compiled = Program::compile(parsed, "bench.dialogue");
        bestCompile = std::min(bestCompile, msSince(compiling));
    }
    ASSERT_TRUE(compiled);
    // a game's load (R16): the file the export cooked, read by path as Programs::load reads it
    const std::string cooked = compiled->cook();
    std::shared_ptr<const Program> program;
    double best = 1e9;
    for (int round = 0; round < 5; round++) {
        const Clock::time_point start = Clock::now();
        std::string error;
        program = Programs::fromText("bench.dialogue", cooked, &error);
        best = std::min(best, msSince(start));
        ASSERT_TRUE(program) << error;
    }
    EXPECT_EQ(program->cook(), cooked) << "the cooked program is the compiled one";
    ASSERT_EQ(program->nodes.size(), 500u);
    const size_t texts = program->options.size() + program->variants.size();
    EXPECT_EQ(texts, 1500u);
    const size_t memory = program->memoryBytes();
    const size_t sourceTexts = program->textBytes();
    const double perNode = static_cast<double>(memory - sourceTexts) / static_cast<double>(program->nodes.size());
    // what a node's record holds, and what each option or variant adds to its node's share
    const double perNodeRecord = static_cast<double>(memory - sourceTexts - program->options.size() * sizeof(ProgramOption) -
                                                     program->variants.size() * sizeof(ProgramVariant)) / static_cast<double>(program->nodes.size());
    std::printf("[bench] dialogue load 500 nodes, 1996 edges, 1500 texts: %.3f ms cooked (budget 3 ms desktop; %zu bytes); the editor's JSON "
                "parse %.2f ms and compile %.2f; %zu bytes, %zu of source texts; %.0f bytes per node (%.0f without its options' %zu-byte records)\n",
                best, cooked.size(), bestParse, bestCompile, memory, sourceTexts, perNode, perNodeRecord, sizeof(ProgramOption));
    EXPECT_LE(perNodeRecord, 160.0) << "R16: a node's record and its share of the expressions and strings";
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(best, 3.0 * testsupport::perfSlack());
    }
}

namespace {

    // a line, 16 nodes that present nothing, then a line with 4 options (2 conditions asking a handler)
    struct StepFixture {
        std::shared_ptr<const Program> program;
        Runner runner;

        StepFixture() {
            registerDialogueKind();
            Variables::reset();
            Handlers::clear();
            GraphText g;
            g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"speaker", "npc"}, {"text", "Go on."}});
            g.wire("n1", "next", "n2");
            std::string previous = "n2";
            for (int i = 0; i < 16; i++) {
                const std::string id = "n" + std::to_string(10 + i);
                if (i % 2 == 0) g.node(id, "set", {{"assign", "$x = 1; $y = $x * 2"}});
                else g.node(id, "branch", {{"condition", "$x > 0 and $y == 2"}});
                g.wire(previous, "next", id);
                previous = id;
            }
            // the branches lead on through their true output
            for (Json& edge : g.edges) {
                const std::string from = edge["from"]["node"];
                if (from.size() > 1 && std::stoi(from.substr(1)) >= 10 && (std::stoi(from.substr(1)) - 10) % 2 == 1) edge["from"]["port"] = "true";
            }
            Json options = Json::array();
            for (int k = 1; k <= 4; k++) {
                Json option = {{"id", "o" + std::to_string(k)}, {"text", "Choice number " + std::to_string(k)}};
                if (k <= 2) option["condition"] = "test.ok(" + std::to_string(k) + ")";
                options.push_back(option);
            }
            g.node("n40", "line", {{"speaker", "npc"}, {"text", "Pick one, any one of them."}, {"options", options}});
            g.wire(previous, "true", "n40").wire("n40", "o1", "n2");
            g.variable("x", 0).variable("y", 0);
            program = Programs::fromText("step.dialogue", g.text("step-guid"));
            Handlers::onCondition("test", [](Runner&, std::string_view, std::span<const Value> args) { return Value(args[0].asNumber() > 0); });
            runner.setDialogue(program);
            runner.start();
        }
        ~StepFixture() { Handlers::clear(); Variables::reset(); }

        // from the first line through the 16 nodes to the menu, then back
        bool step() {
            const bool advanced = runner.advance();
            const bool menu = runner.getCurrentLine() && runner.getCurrentLine()->options.size() == 4;
            runner.choose(1);
            return advanced && menu;
        }
    };

}

TEST(DialogueBench, Step) {
    StepFixture fixture;
    ASSERT_EQ(fixture.runner.getCurrentNode(), "n2");
    ASSERT_TRUE(fixture.step());
    const int rounds = 2000;
    const Clock::time_point start = Clock::now();
    double advanceMs = 0.0;
    for (int i = 0; i < rounds; i++) {
        const Clock::time_point one = Clock::now();
        fixture.runner.advance();
        advanceMs += msSince(one);
        fixture.runner.choose(1);
    }
    const double perAdvance = advanceMs / rounds;
    std::printf("[bench] dialogue advance through 16 nodes to a 4-option line: %.4f ms (budget 0.04 ms desktop); %d rounds in %.1f ms\n",
                perAdvance, rounds, msSince(start));
    EXPECT_EQ(fixture.runner.getState(), RunnerState::Line);
    if (testsupport::perfSlack() > 0.0) {
        EXPECT_LE(perAdvance, 0.04 * testsupport::perfSlack());
    }
}

TEST(DialogueBench, StepAllocations) {
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this binary cannot count allocations";
    StepFixture fixture;
    // warm: every buffer has its size
    for (int i = 0; i < 8; i++) { ASSERT_TRUE(fixture.step()); }
    uint64_t allocations = 0;
    for (int i = 0; i < 100; i++) {
        const testsupport::ThreadAllocationCount count;
        fixture.runner.advance();
        allocations += count.count();
        fixture.runner.choose(1);
    }
    std::printf("[bench] dialogue advance allocations: %llu in 100 warm steps\n", static_cast<unsigned long long>(allocations));
    EXPECT_EQ(allocations, 0u) << "R17: a warm step reuses the payload's strings and the runner's buffers";
}

}  // namespace DialogueBenchTest_cpp
