// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The dialogue variable store (docs/specs/dialogue-graphs.md R5, AC5) and its save slot namespace
// (savegame.md).

#include "DialogueTestSupport.h"

#include "jobs/JobSystem.h"
#include "save/SaveGameStore.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace DialogueVariablesTest_cpp {

using namespace jasperengine::dialogue;
using dialoguetest::GraphText;

namespace {

    class DialogueVariablesTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerDialogueKind();
            Variables::reset();
            Variables::setProjectDefaults({});
        }
        void TearDown() override {
            Variables::reset();
            Variables::setProjectDefaults({});
        }
    };

}

TEST_F(DialogueVariablesTest, DefaultsFillOnFirstRead) {
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}});
    g.variable("gold", 25).variable("name", "Ana");
    const std::shared_ptr<const Program> program = g.program();
    Variables::setProjectDefaults({{"gold", Value(1.0)}, {"reputation", Value(3.0)}});
    EXPECT_TRUE(Variables::get("gold").isNil()) << "get gives no defaults";
    EXPECT_TRUE(Variables::read("gold", program.get()).same(Value(25.0))) << "the dialogue's default wins over the project's";
    EXPECT_TRUE(Variables::has("gold")) << "the default was written";
    EXPECT_TRUE(Variables::read("reputation", program.get()).same(Value(3.0)));
    EXPECT_TRUE(Variables::read("missing", program.get()).isNil());
    EXPECT_FALSE(Variables::has("missing"));
    // a value set before is kept, defaults or not
    Variables::set("name", Value("Bo"));
    EXPECT_TRUE(Variables::read("name", program.get()).same(Value("Bo")));
}

TEST_F(DialogueVariablesTest, VisitCountsPerNode) {
    Variables::addVisit("guid-a", "n1");
    Variables::addVisit("guid-a", "n1");
    Variables::addVisit("guid-a", "n2");
    Variables::addVisit("guid-b", "n1");
    EXPECT_EQ(Variables::visits("guid-a", "n1"), 2u);
    EXPECT_EQ(Variables::visits("guid-a", "n2"), 1u);
    EXPECT_EQ(Variables::visits("guid-b", "n1"), 1u);
    EXPECT_EQ(Variables::visits("guid-b", "n2"), 0u);
    // a runner counts every node it enters, under its dialogue's GUID
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "set", {{"assign", "$x = visits(\"n2\")"}}).node("n3", "line", {{"text", "hi"}});
    g.wire("n1", "next", "n2").wire("n2", "next", "n3");
    const std::shared_ptr<const Program> program = Programs::fromText("v.dialogue", g.text("guid-c"));
    Runner runner;
    runner.setDialogue(program);
    runner.start();
    runner.start();
    EXPECT_EQ(Variables::visits("guid-c", "n2"), 2u);
    EXPECT_TRUE(Variables::get("x").same(Value(2.0))) << "visits counts the node being run";
    Variables::resetVisits();
    EXPECT_EQ(Variables::visits("guid-c", "n2"), 0u);
}

TEST_F(DialogueVariablesTest, ToTableFromTableRoundTrip) {
    Variables::set("gold", Value(12.0));
    Variables::set("met", Value(true));
    Variables::set("name", Value("Ana"));
    Variables::addVisit("guid-a", "n7");
    Variables::addVisit("guid-a", "n7.o1");
    const nlohmann::ordered_json saved = Variables::toJson();
    EXPECT_EQ(saved["visits"]["guid-a#n7"], 1);
    EXPECT_EQ(saved["visits"]["guid-a#n7.o1"], 1);
    const std::string captured = SaveHooks::capture();

    Variables::reset();
    EXPECT_TRUE(Variables::get("gold").isNil());
    ASSERT_TRUE(Variables::fromJson(saved));
    EXPECT_TRUE(Variables::get("gold").same(Value(12.0)));
    EXPECT_TRUE(Variables::get("met").same(Value(true)));
    EXPECT_TRUE(Variables::get("name").same(Value("Ana")));
    EXPECT_EQ(Variables::visits("guid-a", "n7.o1"), 1u);
    EXPECT_EQ(Variables::toJson(), saved);

    Variables::reset();
    ASSERT_TRUE(SaveHooks::restore(captured));
    EXPECT_EQ(Variables::toJson(), saved);
    // something that is not a captured state changes nothing
    std::string error;
    EXPECT_FALSE(SaveHooks::restore("{\"variables\": {\"x\": [1]}}", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Variables::toJson(), saved);
    EXPECT_FALSE(SaveHooks::restore("not json", &error));
}

TEST_F(DialogueVariablesTest, SaveSlotSaveChangeLoad) {
    jasperengine::test::TestTempDir dir{"dialogue-save"};
    jasperengine::savegame::SaveGameStore& store = jasperengine::savegame::SaveGameStore::instance();
    store.setFolder(dir.path() / "saves");
    Variables::set("gold", Value(12.0));
    Variables::set("ratio", Value(0.5));
    Variables::set("met", Value(true));
    Variables::set("name", Value("Ana"));
    Variables::set("$odd", Value("kept"));      // a key the slot's JSON view escapes
    Variables::addVisit("guid-a", "n7");
    Variables::addVisit("guid-a", "n7");
    Variables::addVisit("guid-a", "n7.o1");
    const nlohmann::ordered_json saved = Variables::toJson();
    // the slot's other namespaces stay
    std::string mainPayload;
    jasperengine::savegame::SaveError error;
    ASSERT_TRUE(jasperengine::savegame::fromJson(nlohmann::json{{"level", 3}}, mainPayload, error));
    ASSERT_TRUE(store.write("slot1", "main", mainPayload, jasperengine::savegame::WriteOptions(), nullptr).ok());

    error = SaveHooks::save("slot1");
    ASSERT_TRUE(error.ok()) << error.message;
    ASSERT_TRUE(store.flush("slot1").ok());

    Variables::set("gold", Value(99.0));
    Variables::set("extra", Value(1.0));
    Variables::addVisit("guid-a", "n8");
    std::vector<std::string> told;
    const int listener = Variables::addListener([&](const std::string& name, const Value&) { told.push_back(name); });
    // from the file, not the image in memory
    store.unload("slot1");
    error = SaveHooks::load("slot1");
    Variables::removeListener(listener);
    ASSERT_TRUE(error.ok()) << error.message;
    EXPECT_EQ(Variables::toJson(), saved);
    EXPECT_EQ(Variables::visits("guid-a", "n7"), 2u);
    EXPECT_EQ(Variables::visits("guid-a", "n8"), 0u);
    EXPECT_EQ(told, (std::vector<std::string>{"gold", "extra"})) << "a load is told as sets are";
    jasperengine::savegame::SlotInfo info;
    ASSERT_TRUE(store.info("slot1", info).ok());
    std::sort(info.namespaces.begin(), info.namespaces.end());
    EXPECT_EQ(info.namespaces, (std::vector<std::string>{"dialogue", "main"}));

    // another namespace, and the errors change nothing
    ASSERT_TRUE(SaveHooks::save("slot1", "talk").ok());
    EXPECT_EQ(SaveHooks::load("slot2").code, jasperengine::savegame::SaveErrorCode::NotFound);
    EXPECT_EQ(SaveHooks::load("slot1", "main").code, jasperengine::savegame::SaveErrorCode::Corrupt);
    EXPECT_EQ(Variables::toJson(), saved);
    Variables::reset();
    ASSERT_TRUE(SaveHooks::load("slot1", "talk").ok());
    EXPECT_EQ(Variables::toJson(), saved);

    store.flush();
    jasperengine::jobs::drainMainQueue();
    store.setFolder({});
}

TEST_F(DialogueVariablesTest, OnChangedFires) {
    std::vector<std::string> changes;
    const int listener = Variables::addListener([&](const std::string& name, const Value& value) { changes.push_back(name + "=" + value.toText()); });
    Variables::set("gold", Value(5.0));
    Variables::set("gold", Value(5.0));
    Variables::set("gold", Value(6.0));
    Variables::set("met", Value(true));
    EXPECT_EQ(changes, (std::vector<std::string>{"gold=5", "gold=6", "met=true"})) << "a set that changes nothing is silent";
    // a restore tells what it changed
    changes.clear();
    ASSERT_TRUE(Variables::fromJson(nlohmann::ordered_json{{"variables", {{"gold", 6}, {"name", "Ana"}}}}));
    EXPECT_EQ(changes, (std::vector<std::string>{"name=Ana", "met="}));
    Variables::removeListener(listener);
    // a runner's onVariableChanged hears only its dialogue's variables
    GraphText g;
    g.node("n1", "entry", {{"name", "start"}}).node("n2", "line", {{"text", "{gold} coins"}});
    g.wire("n1", "next", "n2");
    Runner runner;
    runner.setDialogue(g.program());
    std::vector<std::string> heard;
    runner.onVariableChanged = [&](Runner&, const std::string& name, const Value&) { heard.push_back(name); };
    Variables::set("gold", Value(1.0));
    Variables::set("unrelated", Value(1.0));
    EXPECT_EQ(heard, (std::vector<std::string>{"gold"}));
}

}  // namespace DialogueVariablesTest_cpp
