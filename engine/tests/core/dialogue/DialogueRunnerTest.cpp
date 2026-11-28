// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The dialogue runner (docs/specs/dialogue-graphs.md R6-R10, AC2-AC4, AC7): flow, handlers by
// prefix, seeded variants, options, localised lines.

#include "DialogueTestSupport.h"

#include "Engine.h"
#include "Log.h"
#include "localization/Localization.h"
#include "object/DialogueRunner.h"
#include "util/TitleArtLanguage.h"

#include "TestPaths.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace DialogueRunnerTest_cpp {

using namespace jasperengine;
using namespace jasperengine::dialogue;
using dialoguetest::GraphText;
using dialoguetest::Json;
using dialoguetest::Transcript;

namespace {

    class DialogueRunnerTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerDialogueKind();
            Variables::reset();
            Handlers::clear();
            Programs::clear();
        }
        void TearDown() override {
            Variables::reset();
            Handlers::clear();
            Programs::clear();
        }
    };

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    Json option(const std::string& id, const std::string& text, const std::string& condition = "", const std::string& action = "") {
        Json out = {{"id", id}, {"text", text}};
        if (!condition.empty()) out["condition"] = condition;
        if (!action.empty()) out["action"] = action;
        return out;
    }

}

TEST_F(DialogueRunnerTest, WalksEntryLinesBranchesSetsJumpsEnd) {
    std::string error;
    std::shared_ptr<const Program> program = Programs::load("dialogue/walk.dialogue", &error);
    ASSERT_TRUE(program) << error;
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    Handlers::onAction("quest", [&](Runner&, std::string_view verb, std::span<const Value> args) {
        transcript.lines.push_back("action " + describeCall("quest." + std::string(verb), args));
        return false;
    });
    runner.setDialogue(program);
    ASSERT_TRUE(runner.start());
    EXPECT_EQ(runner.getState(), RunnerState::Line);
    EXPECT_EQ(runner.getCurrentNode(), "n3");
    EXPECT_FALSE(runner.advance()) << "a line with choices waits for one";
    transcript.lines.push_back("choose 1");
    ASSERT_TRUE(runner.choose(1));
    transcript.lines.push_back("advance");
    ASSERT_TRUE(runner.advance());
    EXPECT_EQ(runner.getState(), RunnerState::Ended);
    EXPECT_EQ(runner.getResult(), "paid");
    EXPECT_FALSE(runner.isRunning());
    EXPECT_EQ(transcript.text(), readFile(std::filesystem::path(test::testPaths().assetDir) / "dialogue" / "walk.expected.txt"));
    // the store kept the conversation's variables and visits
    EXPECT_TRUE(Variables::get("gold").same(Value(5.0)));
    EXPECT_EQ(Variables::visits(program->identity(), "n3"), 1u);
    EXPECT_EQ(Variables::visits(program->identity(), "n3.o1"), 1u);
}

TEST_F(DialogueRunnerTest, UnconnectedOutputEnds) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"text", "Hello"}, {"options", {option("o1", "Bye")}}});
    g.wire("n1", "next", "n2");
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    runner.setDialogue(g.program());
    ASSERT_TRUE(runner.start());
    ASSERT_TRUE(runner.choose(1));
    EXPECT_EQ(transcript.text(), "start\nline n2 : Hello [1 Bye]\nend \n");
    EXPECT_EQ(runner.getState(), RunnerState::Ended);
    // an entry with nothing after it ends at once
    GraphText empty;
    empty.node("n1", "entry", {{"name", "start"}});
    Runner other;
    Transcript ended;
    ended.attach(other);
    other.setDialogue(empty.program());
    ASSERT_TRUE(other.start());
    EXPECT_EQ(ended.text(), "start\nend \n");
}

TEST_F(DialogueRunnerTest, FallbackNextWhenAllOptionsHidden) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "line", {{"text", "Shop"}, {"options", {option("o1", "Buy", "$gold >= 10"), option("o2", "Steal", "$thief")}}})
     .node("n3", "end", {{"result", "fallback"}})
     .node("n4", "end", {{"result", "bought"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3").wire("n2", "o1", "n4");
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    runner.setDialogue(g.program());
    ASSERT_TRUE(runner.start());
    ASSERT_NE(runner.getCurrentLine(), nullptr);
    EXPECT_TRUE(runner.getCurrentLine()->options.empty());
    EXPECT_FALSE(runner.choose(1));
    ASSERT_TRUE(runner.advance());
    EXPECT_EQ(runner.getResult(), "fallback");
    // with gold the option shows and leads on
    Variables::set("gold", Value(20.0));
    ASSERT_TRUE(runner.start());
    ASSERT_EQ(runner.getCurrentLine()->options.size(), 1u);
    EXPECT_EQ(runner.getCurrentLine()->options[0].id, "o1");
    ASSERT_TRUE(runner.choose(1));
    EXPECT_EQ(runner.getResult(), "bought");
}

TEST_F(DialogueRunnerTest, LoopGuardStopsAt256) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "set", {{"assign", "$n += 1"}}).node("n3", "branch", {{"condition", "$n < 100000"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3").wire("n3", "true", "n2");
    g.variable("n", 0);
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    runner.setDialogue(g.program());
    runner.start();
    EXPECT_EQ(runner.getState(), RunnerState::Ended);
    EXPECT_NE(runner.getError().find("dialogue-loop"), std::string::npos) << runner.getError();
    EXPECT_EQ(transcript.text(), "start\nend \n");
    // 256 nodes ran: the entry, then set and branch in turn
    EXPECT_TRUE(Variables::get("n").same(Value(128.0))) << Variables::get("n").describe();
}

TEST_F(DialogueRunnerTest, JumpAcrossFiles) {
    std::string error;
    std::shared_ptr<const Program> other = Programs::load("dialogue/other.dialogue", &error);
    ASSERT_TRUE(other) << error;
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    runner.setDialogue(other);
    Variables::set("gold", Value(7.0));
    ASSERT_TRUE(runner.start());
    EXPECT_EQ(transcript.text(), "start\nline n8 guard: Welcome in. 7 left.\n");
    ASSERT_NE(runner.getDialogue(), nullptr);
    EXPECT_EQ(runner.getDialogue()->path, "dialogue/walk.dialogue");
    ASSERT_TRUE(runner.advance());
    EXPECT_EQ(runner.getResult(), "paid");
    // a jump to a missing entry stops with the error
    GraphText broken;
    broken.node("n1", "entry", {{"name", "start"}}).node("n2", "jump", {{"dialogue", {{"path", "dialogue/walk.dialogue"}}}, {"entry", "nowhere"}});
    broken.wire("n1", "next", "n2");
    Runner lost;
    lost.setDialogue(broken.program());
    EXPECT_FALSE(lost.start());
    EXPECT_NE(lost.getError().find("jump-target"), std::string::npos);
}

TEST_F(DialogueRunnerTest, ActionsAndConditionsDispatchByPrefix) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "action", {{"action", "quest.start:intro; shop.open(\"weapons\", 2)"}})
     .node("n3", "branch", {{"condition", "quest.done(\"intro\") and shop.canAfford(5)"}})
     .node("n4", "end", {{"result", "yes"}})
     .node("n5", "end", {{"result", "no"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3").wire("n3", "true", "n4").wire("n3", "false", "n5");
    std::vector<std::string> order;
    Runner* seen = nullptr;
    Handlers::onAction("quest", [&](Runner& runner, std::string_view verb, std::span<const Value> args) {
        seen = &runner;
        order.push_back("quest " + std::string(verb) + " " + args[0].toText());
        return false;
    });
    Handlers::onAction("shop", [&](Runner&, std::string_view verb, std::span<const Value> args) {
        order.push_back("shop " + std::string(verb) + " " + args[0].toText() + " " + args[1].toText());
        return false;
    });
    Handlers::onCondition("quest", [&](Runner&, std::string_view verb, std::span<const Value> args) {
        order.push_back("quest? " + std::string(verb) + " " + args[0].toText());
        return Value(true);
    });
    Handlers::onCondition("shop", [&](Runner&, std::string_view verb, std::span<const Value> args) {
        order.push_back("shop? " + std::string(verb) + " " + args[0].toText());
        return Value(args[0].asNumber() <= 5);
    });
    Runner runner;
    runner.setDialogue(g.program());
    runner.start();
    EXPECT_EQ(seen, &runner) << "a handler gets the runner";
    EXPECT_EQ(order, (std::vector<std::string>{"quest start intro", "shop open weapons 2", "quest? done intro", "shop? canAfford 5"}));
    EXPECT_EQ(runner.getResult(), "yes");
    // a second registration replaces the first
    Handlers::onCondition("shop", [](Runner&, std::string_view, std::span<const Value>) { return Value(false); });
    runner.start();
    EXPECT_EQ(runner.getResult(), "no");
}

TEST_F(DialogueRunnerTest, WaitPausesUntilResume) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "action", {{"action", "shop.open:x"}})
     .node("n3", "line", {{"text", "Thanks"}})
     .node("n4", "action", {{"action", ""}, {"wait", true}})
     .node("n5", "end", {{"result", "done"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3").wire("n3", "next", "n4").wire("n4", "next", "n5");
    Handlers::onAction("shop", [](Runner&, std::string_view, std::span<const Value>) { return true; });
    Runner runner;
    runner.setDialogue(g.program());
    ASSERT_TRUE(runner.start());
    EXPECT_EQ(runner.getState(), RunnerState::Waiting);
    EXPECT_EQ(runner.getCurrentNode(), "n2");
    EXPECT_TRUE(runner.isRunning());
    EXPECT_FALSE(runner.advance());
    ASSERT_TRUE(runner.resume());
    EXPECT_EQ(runner.getState(), RunnerState::Line);
    EXPECT_EQ(runner.getCurrentNode(), "n3");
    ASSERT_TRUE(runner.advance());
    // an action node with `wait` pauses with no handler asking
    EXPECT_EQ(runner.getState(), RunnerState::Waiting);
    ASSERT_TRUE(runner.resume());
    EXPECT_EQ(runner.getResult(), "done");
    EXPECT_FALSE(runner.resume());
}

TEST_F(DialogueRunnerTest, UnknownPrefixWarnsOnceAndIsFalse) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "action", {{"action", "ghost.boo:1; ghost.boo:2"}})
     .node("n3", "branch", {{"condition", "ghost.here"}})
     .node("n4", "end", {{"result", "true"}})
     .node("n5", "end", {{"result", "false"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3").wire("n3", "true", "n4").wire("n3", "false", "n5");
    std::vector<std::string> warnings;
    const LogListenerId listener = Log::addListener([&](const LogEntry& entry) {
        if (entry.level == LogLevel::Warn && entry.message.find("'ghost'") != std::string::npos) warnings.push_back(entry.message);
    });
    Runner runner;
    runner.setDialogue(g.program());
    runner.start();
    EXPECT_EQ(runner.getResult(), "false");
    EXPECT_EQ(runner.unknownCalls(), (std::vector<std::string>{"ghost.boo(\"1\")", "ghost.boo(\"2\")", "ghost.here()"}));
    // one warning for the action prefix and one for the condition prefix, in this run
    EXPECT_EQ(warnings.size(), 2u);
    runner.start();
    EXPECT_EQ(warnings.size(), 4u) << "a new run warns again";
    Log::removeListener(listener);
}

TEST_F(DialogueRunnerTest, HandlersClearedAtStop) {
    Handlers::onAction("quest", [](Runner&, std::string_view, std::span<const Value>) { return false; });
    Handlers::onCondition("quest", [](Runner&, std::string_view, std::span<const Value>) { return Value(true); });
    ASSERT_NE(Handlers::action("quest"), nullptr);
    Engine::clearAllSubscriptions(false);
    EXPECT_EQ(Handlers::action("quest"), nullptr);
    EXPECT_EQ(Handlers::condition("quest"), nullptr);
    EXPECT_FALSE(Handlers::registered()) << "nothing allocated after the clear";
}

TEST_F(DialogueRunnerTest, RandomVariantsAreSeededAndNeverRepeat) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "line", {{"text", "a"}, {"variants", {{{"id", "v1"}, {"text", "b"}}, {{"id", "v2"}, {"text", "c"}}, {{"id", "v3"}, {"text", "d"}}}}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n2");
    const std::shared_ptr<const Program> program = g.program();
    auto walk = [&](uint64_t seed) {
        Runner runner;
        runner.setSeed(seed);
        runner.setDialogue(program);
        std::string out;
        runner.start();
        for (int i = 0; i < 10000; i++) {
            out += runner.getCurrentLine()->text;
            runner.advance();
        }
        return out;
    };
    const std::string first = walk(7);
    EXPECT_EQ(first, walk(7)) << "the same seed gives the same transcript";
    const std::string second = walk(8);
    EXPECT_NE(first, second);
    for (const std::string& run : {first, second}) {
        for (size_t i = 1; i < run.size(); i++) { ASSERT_NE(run[i], run[i - 1]) << "the same variant twice in a row at " << i; }
        for (char c : std::string("abcd")) { EXPECT_GT(std::count(run.begin(), run.end(), c), 2000) << c; }
    }
    // the stream is integer-only: these are the same on every platform
    EXPECT_EQ(first.substr(0, 12), walk(7).substr(0, 12));
}

TEST_F(DialogueRunnerTest, SequenceAndOnceModes) {
    auto run = [](const std::string& mode, int times) {
        Variables::resetVisits();
        GraphText g;
        g.node("n1", "entry", {{"name", "start"}})
         .node("n2", "line", {{"text", "a"}, {"variant_mode", mode}, {"variants", {{{"id", "v1"}, {"text", "b"}}, {{"id", "v2"}, {"text", "c"}}}}});
        g.wire("n1", "next", "n2");
        Runner runner;
        runner.setDialogue(g.program());
        std::string out;
        for (int i = 0; i < times; i++) {
            runner.start();
            out += runner.getCurrentLine()->text;
            if (!runner.getCurrentLine()->variant.empty()) {
                EXPECT_EQ(runner.getCurrentLine()->key, "t.n2." + runner.getCurrentLine()->variant);
            }
        }
        return out;
    };
    EXPECT_EQ(run("sequence", 7), "abcabca");
    EXPECT_EQ(run("once", 6), "abcccc");
}

TEST_F(DialogueRunnerTest, OnceOptionsDisappear) {
    GraphText g;
    Json ask = option("o1", "Ask about the gate");
    ask["once"] = true;
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"text", "Yes?"}, {"options", {ask, option("o2", "Bye")}}});
    g.wire("n1", "next", "n2").wire("n2", "o1", "n2");
    Runner runner;
    runner.setDialogue(g.program());
    ASSERT_TRUE(runner.start());
    ASSERT_EQ(runner.getCurrentLine()->options.size(), 2u);
    ASSERT_TRUE(runner.choose(1));
    ASSERT_EQ(runner.getCurrentLine()->options.size(), 1u);
    EXPECT_EQ(runner.getCurrentLine()->options[0].id, "o2");
    EXPECT_EQ(runner.getCurrentLine()->options[0].index, 1);
    // gone in the next conversation too: the store remembers the choice
    runner.start();
    EXPECT_EQ(runner.getCurrentLine()->options.size(), 1u);
}

TEST_F(DialogueRunnerTest, WhenFalseDisableKeepsTheOptionDisabled) {
    GraphText g;
    Json pay = option("o1", "Pay {price}", "$gold >= 10");
    pay["when_false"] = "disable";
    pay["style"] = "accept";
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"text", "Ten coins."}, {"options", {pay, option("o2", "No")}}});
    g.wire("n1", "next", "n2");
    Runner runner;
    runner.setDialogue(g.program());
    ASSERT_TRUE(runner.start("", {{"price", int64_t(10)}}));
    const Line* line = runner.getCurrentLine();
    ASSERT_EQ(line->options.size(), 2u);
    EXPECT_FALSE(line->options[0].enabled);
    EXPECT_EQ(line->options[0].text, "Pay 10");
    EXPECT_EQ(line->options[0].style, "accept");
    EXPECT_TRUE(line->options[1].enabled);
    EXPECT_FALSE(runner.choose(1)) << "a disabled option cannot be chosen";
    EXPECT_EQ(runner.getState(), RunnerState::Line);
}

TEST_F(DialogueRunnerTest, LinesUseTranslationsAndArguments) {
    TitleArtLanguage::resetForTesting();
    Localization::resetForTesting();
    registerDialogueKind();
    const std::filesystem::path dir = std::filesystem::path(test::testPaths().binaryDir) / "dialogue-l10n";
    std::filesystem::create_directories(dir);
    GraphText g;
    g.ns = "guard";
    g.node("n1", "entry", {{"name", "start"}, {"arguments", "player"}})
     .node("n2", "line", {{"speaker", "guard"}, {"text", "Halt, {player}! {gold, plural, one {# coin} other {# coins}}?"},
                          {"options", {option("o1", "Pay")}}});
    g.wire("n1", "next", "n2");
    const std::string dialoguePath = (dir / "guard.dialogue").string();
    std::ofstream(dialoguePath) << g.text();
    const std::string translationPath = (dir / "guard.de.translation").string();
    std::ofstream(translationPath) << R"({"format": "jasperengine.translation", "version": 1, "language": "de",
        "owner": {"guid": "", "path": "guard.dialogue"},
        "entries": {"n2": {"text": "Halt, {player}! {gold, plural, one {# Münze} other {# Münzen}}?", "status": "translated"},
                    "n2.o1": {"text": "Zahlen", "status": "translated"}}})";
    LocalizationManifest manifest;
    manifest.defaultLanguage = "en";
    manifest.rememberLanguage = false;
    manifest.languages = {"en", "de"};
    manifest.namespaces.push_back({"guard", "en", {{"en", dialoguePath}, {"de", translationPath}}});
    Localization::setManifest(manifest);
    ASSERT_TRUE(Localization::setLanguage("de"));

    Runner runner;
    std::string error;
    runner.setDialogue(Programs::load(dialoguePath, &error));
    ASSERT_TRUE(runner.getDialogue()) << error;
    Variables::set("gold", Value(1.0));
    ASSERT_TRUE(runner.start("", {{"player", std::string("Ana")}}));
    EXPECT_EQ(runner.getCurrentLine()->key, "guard.n2");
    EXPECT_EQ(runner.getCurrentLine()->text, "Halt, Ana! 1 Münze?");
    EXPECT_EQ(runner.getCurrentLine()->options[0].text, "Zahlen");
    EXPECT_EQ(runner.getCurrentLine()->speakerName, "guard") << "no speakers table: the speaker as written";
    // a start argument wins over a variable of the same name; plurals follow the variable
    Variables::set("player", Value("Bo"));
    Variables::set("gold", Value(3.0));
    runner.start("", {{"player", std::string("Ana")}});
    EXPECT_EQ(runner.getCurrentLine()->text, "Halt, Ana! 3 Münzen?");
    // a language change sends the line again in the new language
    int sent = 0;
    runner.onLine = [&](Runner&, const Line&) { sent++; };
    ASSERT_TRUE(Localization::setLanguage("en"));
    EXPECT_EQ(sent, 1);
    EXPECT_EQ(runner.getCurrentLine()->text, "Halt, Ana! 3 coins?");
    Localization::resetForTesting();
    TitleArtLanguage::resetForTesting();
}

// R13: the component's runner seeds from Seed (0: the entity id), loads its Dialogue and mirrors its state
TEST_F(DialogueRunnerTest, ComponentRunnerMirrorsItsState) {
    Scene scene;
    const Entity entity = scene.createEntity();
    DialogueRunner object(&scene, entity);
    object.setDialogue("dialogue/walk.dialogue");
    std::vector<std::string> events;
    object.getOnLine().add("test", std::function<void(DialogueRunner, const Line&)>([&](DialogueRunner, const Line& line) { events.push_back(line.node); }));
    object.getOnEnd().add("test", std::function<void(DialogueRunner, const std::string&)>([&](DialogueRunner, const std::string& result) { events.push_back("end " + result); }));
    ASSERT_TRUE(object.start());
    const DialogueRunnerComponent& component = scene.getComponent<DialogueRunnerComponent>(entity);
    EXPECT_EQ(component.state, "line");
    EXPECT_EQ(component.currentNode, "n3");
    EXPECT_TRUE(component.isRunning);
    EXPECT_EQ(object.getRunner()->getSeed(), static_cast<uint64_t>(entity));
    ASSERT_TRUE(object.choose(2));
    EXPECT_EQ(component.state, "ended");
    EXPECT_FALSE(component.isRunning);
    EXPECT_EQ(events, (std::vector<std::string>{"n3", "end left"}));
}

// R16: what an export ships, the compiled program's bytes at the dialogue's path, loads by that path
// and plays as the source does
TEST_F(DialogueRunnerTest, CookedFilePlaysAsItsSource) {
    const std::filesystem::path assets = std::filesystem::path(test::testPaths().assetDir) / "dialogue";
    std::string error;
    const std::shared_ptr<const Program> source = Programs::fromText("dialogue/walk.dialogue", readFile(assets / "walk.dialogue"), &error);
    ASSERT_TRUE(source) << error;
    const std::string cooked = source->cook();
    ASSERT_TRUE(Program::isCooked(cooked));
    EXPECT_EQ(source->cook(), cooked) << "equal programs give equal bytes";
    const std::filesystem::path dir = std::filesystem::path(test::testPaths().binaryDir) / "dialogue-cooked";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "walk.dialogue").string();
    std::ofstream(path, std::ios::binary) << cooked;

    const std::shared_ptr<const Program> program = Programs::load(path, &error);
    ASSERT_TRUE(program) << error;
    EXPECT_EQ(program->cook(), cooked) << "every field read back";
    EXPECT_EQ(program->path, path);
    EXPECT_EQ(program->identity(), source->identity());
    EXPECT_EQ(program->ns, source->ns);
    EXPECT_EQ(program->variablesUsed, source->variablesUsed);
    Runner runner;
    Transcript transcript;
    transcript.attach(runner);
    Handlers::onAction("quest", [&](Runner&, std::string_view verb, std::span<const Value> args) {
        transcript.lines.push_back("action " + describeCall("quest." + std::string(verb), args));
        return false;
    });
    runner.setDialogue(program);
    ASSERT_TRUE(runner.start());
    transcript.lines.push_back("choose 1");
    ASSERT_TRUE(runner.choose(1));
    transcript.lines.push_back("advance");
    ASSERT_TRUE(runner.advance());
    EXPECT_EQ(runner.getResult(), "paid");
    EXPECT_EQ(transcript.text(), readFile(assets / "walk.expected.txt"));

    // the fields walk.dialogue leaves out: tags, voice, mood, variant modes, once, when_false, styles,
    // a waiting action, a jump into another file, variables of each type
    GraphText g;
    Json first = option("o1", "Ask", "visited(\"n3\") or chance(0.5)", "$asked = true; shop.open(1, \"two\")");
    first["once"] = true;
    first["tags"] = Json::array({"lore", "gate"});
    Json second = option("o2", "Leave");
    second["when_false"] = "disable";
    second["style"] = "exit";
    g.node("n1", "entry", {{"name", "start"}})
     .node("n2", "line", {{"speaker", "guard"}, {"mood", "angry"}, {"text", "Who {name}?"}, {"voice", "audio/halt.ogg"}, {"tags", "a, b"},
                          {"variant_mode", "once"}, {"variants", {{{"id", "v1"}, {"text", "Again?"}}}}, {"options", {first, second}}})
     .node("n3", "action", {{"action", "quest.start:intro"}, {"wait", true}})
     .node("n4", "jump", {{"entry", "inside"}, {"dialogue", {{"guid", "g-2"}, {"path", "dialogue/other.dialogue"}}}});
    g.wire("n1", "next", "n2").wire("n2", "o1", "n3").wire("n3", "next", "n4");
    g.variable("asked", false).variable("name", "stranger").variable("gold", 2.5);
    const std::shared_ptr<const Program> rich = g.program("rich.dialogue");
    ASSERT_TRUE(rich);
    const std::shared_ptr<const Program> back = Program::fromCooked(rich->cook(), "rich.dialogue", &error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(back->cook(), rich->cook());
    ASSERT_TRUE(back->defaultOf("name"));
    EXPECT_TRUE(back->defaultOf("name")->same(Value("stranger")));
    EXPECT_EQ(back->exprs.calls.size(), rich->exprs.calls.size());
    EXPECT_EQ(back->exprs.calls[0].name, rich->exprs.calls[0].name);
}

// R16: a damaged cooked file is refused with a reason, never run past its arrays or its stack
TEST_F(DialogueRunnerTest, CookedFileRefusesDamage) {
    const std::filesystem::path assets = std::filesystem::path(test::testPaths().assetDir) / "dialogue";
    std::string error;
    const std::shared_ptr<const Program> source = Programs::fromText("walk.dialogue", readFile(assets / "walk.dialogue"), &error);
    ASSERT_TRUE(source) << error;
    const std::string cooked = source->cook();
    for (size_t size = 0; size < cooked.size(); size++) {
        error.clear();
        EXPECT_FALSE(Program::fromCooked(std::string_view(cooked).substr(0, size), "walk.dialogue", &error)) << size << " bytes";
        EXPECT_FALSE(error.empty()) << size;
    }
    EXPECT_FALSE(Program::fromCooked(cooked + '\0', "walk.dialogue", &error));
    EXPECT_NE(error.find("after its end"), std::string::npos) << error;
    std::string newer = cooked;
    newer[4] = static_cast<char>(Program::COOKED_VERSION + 1);
    EXPECT_FALSE(Programs::fromText("walk.dialogue", newer, &error));
    EXPECT_NE(error.find("export the game again"), std::string::npos) << error;

    // what only the checks can catch: indexes and stacks a well-formed file could still carry
    auto refused = [&](const std::function<void(Program&)>& damage) {
        Program copy = *source;
        damage(copy);
        return !Program::fromCooked(copy.cook(), "walk.dialogue", &error);
    };
    ASSERT_FALSE(refused([](Program&) {}));
    EXPECT_TRUE(refused([](Program& p) { p.nodes[0].next = static_cast<int32_t>(p.nodes.size()); }));
    EXPECT_TRUE(refused([](Program& p) { p.nodes[2].text.offset = static_cast<uint32_t>(p.strings.size()); }));
    EXPECT_TRUE(refused([](Program& p) { p.nodes[2].optionsCount = static_cast<uint16_t>(p.options.size() + 1); }));
    EXPECT_TRUE(refused([](Program& p) { p.options[0].condition.count = static_cast<uint32_t>(p.exprs.code.size() + 1); }));
    EXPECT_TRUE(refused([](Program& p) { p.exprs.code[0].operand = static_cast<uint32_t>(p.exprs.names.size() + p.exprs.constants.size()); }));
    EXPECT_TRUE(refused([](Program& p) { p.exprs.code[p.nodes[1].expr.first].op = Op::Add; })) << "an operator with nothing to pop";
    EXPECT_TRUE(refused([](Program& p) { p.exprs.code[0].op = static_cast<Op>(200); }));
    EXPECT_TRUE(refused([](Program& p) { p.entries[0].second = -1; }));

    // any byte changed: refused, or a program the runner can play
    for (size_t at = sizeof(Program::COOKED_MAGIC); at < cooked.size(); at++) {
        std::string damaged = cooked;
        damaged[at] = static_cast<char>(damaged[at] ^ 0x5A);
        const std::shared_ptr<const Program> program = Program::fromCooked(damaged, "walk.dialogue", &error);
        if (!program) continue;
        Runner runner;
        runner.setDialogue(program);
        runner.start();
        for (int i = 0; i < 8 && runner.isRunning(); i++) {
            if (!runner.choose(1)) runner.advance();
        }
    }
}

}  // namespace DialogueRunnerTest_cpp
