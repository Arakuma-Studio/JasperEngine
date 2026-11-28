// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The console's command registry (docs/specs/dev-console-for-agents.md, AC1-AC11): typed
// arguments and the messages that name the fix, untyped words, aliases, the result's lines, log and
// value, shadowed built-ins, help, completion, the cheat flag, the gate, nesting, and no storage
// until the first use. Nothing here needs the HTML UI module.

#include "debug/ConsoleCommands.h"
#include "debug/DebugOverlay.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/Transform.h"
#include "util/FunctionSubscribe.h"
#include "../ecs/EcsTestUtil.h"

#include "AllocationCounter.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace ConsoleCommandsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::console;
using jasperengine::test::LogCapture;

namespace {

    class ConsoleCommands : public ::testing::Test {
    protected:
        void SetUp() override{
            console::reset();
            console::setCommandsEnabled(true);
        }
        void TearDown() override{
            Engine::setScene(nullptr);
            Events::onCheatUsed.remove("console-test");
            console::reset();
        }
    };

    ArgSpec consoleArg(const char* name, ArgType type, bool optional = false){
        ArgSpec spec;
        spec.name = name;
        spec.type = type;
        spec.optional = optional;
        return spec;
    }

    // a typed command that keeps its last call's arguments
    struct Recorder {
        std::vector<ArgValue> args;
        std::vector<std::string> words;
        int runs = 0;
    };

    bool addRecorded(const std::string& name, std::vector<ArgSpec> args, Recorder& recorder, bool cheat = false,
                     std::vector<std::string> aliases = {}, std::string* error = nullptr){
        CommandSpec spec;
        spec.name = name;
        spec.args = std::move(args);
        spec.cheat = cheat;
        spec.aliases = std::move(aliases);
        spec.run = [&recorder](Call& call){
            recorder.args = call.args();
            recorder.words = call.words();
            recorder.runs++;
        };
        return console::add(std::move(spec), error);
    }

    void addGive(Recorder& recorder){
        ArgSpec item = consoleArg("item", ArgType::Enum);
        item.choices = {"gold", "gem", "key"};
        ArgSpec count = consoleArg("count", ArgType::Int, true);
        count.defaultValue = "1";
        count.min = 1;
        count.max = 999;
        CommandSpec spec;
        spec.name = "give";
        spec.description = "adds an item";
        spec.aliases = {"g"};
        spec.cheat = true;
        spec.args = {item, count};
        spec.run = [&recorder](Call& call){
            recorder.args = call.args();
            recorder.runs++;
        };
        ASSERT_TRUE(console::add(std::move(spec)));
    }

    std::string usageError(const Result& result){
        return result.error + " Usage: " + result.usage;
    }

}

// AC1: every type, ranges, hex, enum prefixes, vector3, #id and path GameObjects, rest, escaped quotes
TEST_F(ConsoleCommands, TypedArgumentsParseOrNameTheFix){
    Scene scene;
    const Entity hero = test::makeTransformEntity(scene);
    scene.setEntityName(hero, "Hero");
    const Entity parent = test::makeTransformEntity(scene);
    scene.setEntityName(parent, "Parent");
    const Entity child = test::makeTransformEntity(scene, parent);
    scene.setEntityName(child, "Child");
    Engine::setScene(&scene);

    Recorder give;
    addGive(give);
    Result r = console::execute("give gold 10");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command, "give");
    EXPECT_EQ(give.args[0].text, "gold");
    EXPECT_EQ(give.args[1].i, 10);
    r = console::execute("G GOL");
    ASSERT_TRUE(r.ok()) << r.error << " (alias, case, a unique prefix)";
    EXPECT_EQ(r.command, "give");
    EXPECT_EQ(give.args[0].text, "gold");
    EXPECT_EQ(give.args[1].i, 1) << "the default";
    EXPECT_TRUE(give.args[1].given);
    EXPECT_TRUE(console::execute("give key 0x10").ok());
    EXPECT_EQ(give.args[1].i, 16);

    LogCapture log;
    r = console::execute("give gold lots");
    EXPECT_EQ(r.status, Status::Usage);
    EXPECT_EQ(usageError(r), "give: count must be a whole number from 1 to 999 (got \"lots\"). Usage: give <item> [count=1]");
    EXPECT_EQ(log.count(D_LOG_WARN, usageError(r)), 1u) << "the overlay shows it in the warning colour";
    EXPECT_EQ(console::execute("give gold 1000").error, "give: count must be a whole number from 1 to 999 (got \"1000\").");
    EXPECT_EQ(console::execute("give ruby").error, "give: item must be one of gold, gem, key (got \"ruby\").");
    EXPECT_EQ(console::execute("give g").error, "give: item must be one of gold, gem, key (got \"g\").") << "gold or gem";
    EXPECT_EQ(console::execute("give").error, "give: item is missing.");
    EXPECT_EQ(console::execute("give gold 1 2").error, "give: takes at most 2 words (got 3).");
    EXPECT_EQ(give.runs, 3) << "no usage error runs the command";

    Recorder num;
    ASSERT_TRUE(addRecorded("num", {consoleArg("n", ArgType::Int)}, num));
    EXPECT_TRUE(console::execute("num -5").ok());
    EXPECT_EQ(num.args[0].i, -5);
    EXPECT_TRUE(console::execute("num 9223372036854775807").ok());
    EXPECT_EQ(num.args[0].i, INT64_MAX);
    EXPECT_EQ(console::execute("num 9223372036854775808").error, "num: n must be a whole number (got \"9223372036854775808\").");
    EXPECT_EQ(console::execute("num 1.5").status, Status::Usage);

    Recorder flt;
    ArgSpec x = consoleArg("x", ArgType::Float);
    x.min = 0;
    x.max = 100;
    ASSERT_TRUE(addRecorded("flt", {x}, flt));
    EXPECT_TRUE(console::execute("flt 0.5").ok());
    EXPECT_DOUBLE_EQ(flt.args[0].f, 0.5);
    EXPECT_EQ(console::execute("flt inf").error, "flt: x must be a number from 0 to 100 (got \"inf\").");
    EXPECT_EQ(console::execute("flt 101").status, Status::Usage);

    Recorder flag;
    ASSERT_TRUE(addRecorded("flag", {consoleArg("on", ArgType::Bool)}, flag));
    EXPECT_TRUE(console::execute("flag YES").ok());
    EXPECT_TRUE(flag.args[0].b);
    EXPECT_TRUE(console::execute("flag 0").ok());
    EXPECT_FALSE(flag.args[0].b);
    EXPECT_EQ(console::execute("flag maybe").error, "flag: on must be true or false (on/off, yes/no, 1/0) (got \"maybe\").");

    Recorder pos;
    ASSERT_TRUE(addRecorded("pos", {consoleArg("p", ArgType::Vector3)}, pos));
    EXPECT_TRUE(console::execute("pos 1,2.5,-3").ok());
    EXPECT_EQ(pos.args[0].v, Vector3(1.0f, 2.5f, -3.0f));
    EXPECT_EQ(console::execute("pos 1,2").error, "pos: p must be x,y,z (three numbers, as 1,0,-2.5) (got \"1,2\").");

    Recorder tp;
    ASSERT_TRUE(addRecorded("tp", {consoleArg("target", ArgType::GameObject)}, tp));
    EXPECT_TRUE(console::execute("tp Hero").ok());
    EXPECT_EQ(tp.args[0].entity, hero);
    EXPECT_EQ(tp.args[0].scene, &scene);
    EXPECT_TRUE(console::execute("tp #" + std::to_string(child)).ok());
    EXPECT_EQ(tp.args[0].entity, child);
    EXPECT_TRUE(console::execute("tp Parent/Child").ok());
    EXPECT_EQ(tp.args[0].entity, child);
    EXPECT_TRUE(console::execute("tp /Parent/Child").ok());
    EXPECT_EQ(console::execute("tp /Child").status, Status::Usage) << "not a root";
    r = console::execute("tp Hero2");
    EXPECT_EQ(usageError(r), "tp: no GameObject \"Hero2\" (did you mean \"Hero\"?). Usage: tp <target>");
    EXPECT_EQ(console::execute("tp #999999").error, "tp: no GameObject #999999 in the running scenes.");
    scene.setEntityActive(hero, false);
    EXPECT_EQ(console::execute("tp Hero").status, Status::Usage) << "active GameObjects only";

    Recorder say;
    ASSERT_TRUE(addRecorded("say", {consoleArg("who", ArgType::String), consoleArg("text", ArgType::Rest)}, say));
    EXPECT_TRUE(console::execute("say bob \"hello \\\"there\\\"\"  again  ").ok());
    EXPECT_EQ(say.args[0].text, "bob");
    EXPECT_EQ(say.args[1].text, "\"hello \\\"there\\\"\"  again") << "the rest of the line verbatim";
    EXPECT_TRUE(console::execute("say \"a \\\"b\\\" \\\\ c\" x").ok());
    EXPECT_EQ(say.args[0].text, "a \"b\" \\ c") << "escaped quotes and backslashes inside quotes";
    EXPECT_EQ(console::execute("say bob").error, "say: text is missing.");
    EXPECT_EQ(console::execute("say bob x").command, "say");
}

// AC2: an untyped command gets its words as they were; minArgs/maxArgs give usage results
TEST_F(ConsoleCommands, UntypedCountsAndLegacyWords){
    std::vector<std::string> got;
    ASSERT_TRUE(DebugOverlay::addCommand("god", "god mode", [&got](const std::vector<std::string>& words){ got = words; }));
    EXPECT_TRUE(DebugOverlay::executeCommand("god 1 two").ok());
    EXPECT_EQ(got, (std::vector<std::string>{"1", "two"}));
    DebugOverlay::executeCommand("god \"a b\" 'c d'");
    EXPECT_EQ(got, (std::vector<std::string>{"a b", "c d"}));
    EXPECT_FALSE(DebugOverlay::addCommand("Bad Name", "", [](const std::vector<std::string>&){}));

    CommandSpec pair;
    pair.name = "pair";
    pair.minArgs = 1;
    pair.maxArgs = 2;
    pair.run = [](Call&){};
    ASSERT_TRUE(console::add(pair));
    Result r = console::execute("pair");
    EXPECT_EQ(r.status, Status::Usage);
    EXPECT_EQ(usageError(r), "pair: takes at least 1 word (got 0). Usage: pair");
    EXPECT_EQ(console::execute("pair a b c").error, "pair: takes at most 2 words (got 3).");
    EXPECT_TRUE(console::execute("pair a b").ok());
}

// AC3: a colliding alias registers nothing and names the holder; replacing replaces the aliases
TEST_F(ConsoleCommands, AliasesRegisterAllOrNothing){
    Recorder grab, give;
    ASSERT_TRUE(addRecorded("grab", {}, grab, false, {"g", "gr"}));
    std::string error;
    EXPECT_FALSE(addRecorded("give", {}, give, false, {"gv", "g"}, &error));
    EXPECT_EQ(error, "alias 'g' is taken by 'grab'");
    EXPECT_EQ(console::find("give"), nullptr) << "nothing of it registered";
    EXPECT_EQ(console::find("gv"), nullptr);
    EXPECT_FALSE(addRecorded("g", {}, give, false, {}, &error));
    EXPECT_EQ(error, "'g' is taken by 'grab' (an alias)");
    EXPECT_FALSE(addRecorded("give", {}, give, false, {"grab"}, &error));
    EXPECT_EQ(error, "alias 'grab' is taken by 'grab'");
    EXPECT_FALSE(addRecorded("give", {}, give, false, {"clear"}, &error)) << "a built-in's name";

    ASSERT_TRUE(addRecorded("grab", {}, grab, false, {"take"}));
    EXPECT_EQ(console::find("g"), nullptr) << "the replaced command's aliases went with it";
    ASSERT_NE(console::find("take"), nullptr);
    EXPECT_EQ(console::find("take")->name, "grab");
    EXPECT_TRUE(addRecorded("give", {}, give, false, {"g"})) << "free again";
    EXPECT_EQ(console::execute("g").command, "give");
}

// AC4: print, an Info line, a warning and an error during the call; the echo is not a line
TEST_F(ConsoleCommands, ResultCarriesLinesLogAndValue){
    CommandSpec spec;
    spec.name = "rep";
    spec.run = [](Call& call){
        Log::print("captured");
        call.print("printed");
        Log::warn("careful");
        Log::error("broken");
        call.setValue(nlohmann::json{{"n", 3}});
    };
    ASSERT_TRUE(console::add(spec));
    LogCapture log;
    const Result r = console::execute("rep");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.lines, (std::vector<std::string>{"printed", "captured"})) << "print first, then the captured lines";
    ASSERT_EQ(r.log.size(), 2u);
    EXPECT_EQ(r.log[0].level, LogLevel::Warn);
    EXPECT_EQ(r.log[0].message, "careful");
    EXPECT_EQ(r.log[1].level, LogLevel::Error);
    EXPECT_EQ(r.log[1].message, "broken");
    EXPECT_EQ(r.value, (nlohmann::json{{"n", 3}}));
    EXPECT_TRUE(log.contains("> rep")) << "the echo is logged";
    EXPECT_TRUE(log.contains("[rep] printed")) << "a printed line is logged with its prefix";

    // a failure: its message, logged as an error
    CommandSpec failing;
    failing.name = "nope2";
    failing.run = [](Call& call){ call.fail("not enough gold"); };
    ASSERT_TRUE(console::add(failing));
    const Result failed = console::execute("nope2");
    EXPECT_EQ(failed.status, Status::Failed);
    EXPECT_EQ(failed.error, "not enough gold");
    EXPECT_EQ(log.count(D_LOG_ERROR, "nope2: not enough gold"), 1u);

    // the empty line and an unknown name
    EXPECT_EQ(console::execute("   ").status, Status::Unknown);
    const Result unknown = console::execute("rpe");
    EXPECT_EQ(unknown.status, Status::Unknown);
    EXPECT_EQ(unknown.error, "unknown command 'rpe' (did you mean 'rep'? type help)");
    EXPECT_EQ(console::execute("xyzzy").error, "unknown command 'xyzzy' (type help)");
}

// AC5: a game command takes a built-in's name (one warning) and gives it back when it goes; R7
// replaces the refusal of a built-in's name
TEST_F(ConsoleCommands, GameCommandShadowsBuiltInAndUnshadows){
    LogCapture log;
    Recorder scene;
    ASSERT_TRUE(addRecorded("scene", {consoleArg("level", ArgType::String)}, scene));
    ASSERT_TRUE(addRecorded("scene", {consoleArg("level", ArgType::String)}, scene)) << "replaced";
    EXPECT_EQ(log.count(D_LOG_WARN, "console: 'scene' now runs the game's command; the built-in returns when it is removed"), 1u);
    EXPECT_TRUE(console::execute("scene two").ok());
    EXPECT_EQ(scene.runs, 1);
    EXPECT_EQ(scene.args[0].text, "two");
    ASSERT_NE(console::find("scene"), nullptr);
    EXPECT_FALSE(console::find("scene")->builtIn);
    size_t listed = 0;
    for (const CommandSpec* spec : console::list()) listed += spec->name == "scene" ? 1 : 0;
    EXPECT_EQ(listed, 1u) << "the shadowed built-in is not listed";

    console::remove("scene");
    ASSERT_NE(console::find("scene"), nullptr);
    EXPECT_TRUE(console::find("scene")->builtIn) << "the built-in is back";
    const Result builtIn = console::execute("scene");
    EXPECT_EQ(builtIn.status, Status::Usage);
    EXPECT_EQ(builtIn.usage, "scene <name...>");

    std::string error;
    EXPECT_FALSE(addRecorded("help", {}, scene, false, {}, &error)) << "help is never shadowed";
    EXPECT_NE(error.find("help"), std::string::npos);
    EXPECT_TRUE(console::find("help")->builtIn);
}

// AC6: help lists, describes one command, filters by a prefix
TEST_F(ConsoleCommands, HelpListsAndDescribes){
    Recorder give, alpha, apple;
    addGive(give);
    ASSERT_TRUE(addRecorded("alpha", {}, alpha));
    ASSERT_TRUE(addRecorded("apple", {}, apple));

    Result all = console::execute("help");
    ASSERT_TRUE(all.ok());
    EXPECT_NE(std::find(all.lines.begin(), all.lines.end(), "give - adds an item [g] (cheat)"), all.lines.end());
    ASSERT_TRUE(all.value.is_array());
    EXPECT_EQ(all.value.size(), all.lines.size());
    bool found = false;
    for (const nlohmann::json& entry : all.value){
        if (entry["name"] != "give") continue;
        found = true;
        EXPECT_EQ(entry["usage"], "give <item> [count=1]");
        EXPECT_EQ(entry["aliases"], (nlohmann::json{"g"}));
        EXPECT_EQ(entry["cheat"], true);
        EXPECT_EQ(entry["builtIn"], false);
        EXPECT_EQ(entry["args"][1], (nlohmann::json{{"name", "count"}, {"type", "int"}, {"optional", true}, {"default", "1"}, {"min", 1.0}, {"max", 999.0}}));
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(all.value[0]["name"], "alpha") << "sorted by name";

    const Result one = console::execute("help g");
    ASSERT_TRUE(one.ok());
    EXPECT_EQ(one.lines, (std::vector<std::string>{"give - adds an item [g] (cheat)", "Usage: give <item> [count=1]",
                                                  "  item: enum (gold, gem, key)", "  count: int 1..999, optional, default 1"}));
    EXPECT_EQ(one.value["name"], "give");

    const Result filtered = console::execute("help a*");
    EXPECT_EQ(filtered.lines, (std::vector<std::string>{"alpha", "apple"}));
    EXPECT_EQ(filtered.value.size(), 2u);

    const Result missing = console::execute("help gvie");
    EXPECT_EQ(missing.status, Status::Failed);
    EXPECT_EQ(missing.error, "no command 'gvie' (did you mean 'give'?)");
}

// AC7: names, enum, bool, GameObjects (20, sorted), a custom function, the space, the cap of 50
TEST_F(ConsoleCommands, CompletionOfNamesArgsAndCustom){
    Recorder give, flag, tp, many;
    addGive(give);
    ASSERT_TRUE(addRecorded("flag", {consoleArg("on", ArgType::Bool)}, flag));
    ASSERT_TRUE(addRecorded("tp", {consoleArg("target", ArgType::GameObject)}, tp));

    Completion c = console::complete("tim", 3);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"timescale"}));
    EXPECT_EQ(c.replacement, "timescale ") << "one candidate: a space after it";
    EXPECT_EQ(c.start, 0u);

    c = console::complete("give g", 6);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"gem", "gold"}));
    EXPECT_EQ(c.replacement, "g");
    EXPECT_EQ(c.start, 5u);
    c = console::complete("give g extra", 6);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"gem", "gold"})) << "the word at the cursor";
    c = console::complete("give ", 5);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"gem", "gold", "key"}));
    EXPECT_EQ(c.replacement, "");

    c = console::complete("flag o", 6);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"off", "on"}));
    EXPECT_EQ(c.replacement, "o");
    EXPECT_TRUE(console::complete("give gold ", 10).candidates.empty()) << "an int completes nothing";

    Scene scene;
    for (int i = 29; i >= 0; i--){
        const Entity enemy = test::makeTransformEntity(scene);
        scene.setEntityName(enemy, (i < 10 ? "Enemy0" : "Enemy") + std::to_string(i));
    }
    const Entity hero = test::makeTransformEntity(scene);
    scene.setEntityName(hero, "Hero");
    const Entity a = test::makeTransformEntity(scene);
    scene.setEntityName(a, "A");
    const Entity b = test::makeTransformEntity(scene);
    scene.setEntityName(b, "B");
    scene.setEntityName(test::makeTransformEntity(scene, a), "Twin");
    scene.setEntityName(test::makeTransformEntity(scene, b), "Twin");
    Engine::setScene(&scene);
    c = console::complete("tp E", 4);
    ASSERT_EQ(c.candidates.size(), 20u);
    EXPECT_EQ(c.candidates.front(), "Enemy00");
    EXPECT_EQ(c.candidates.back(), "Enemy19");
    EXPECT_EQ(c.replacement, "Enemy");
    EXPECT_EQ(console::complete("tp h", 4).replacement, "Hero ");
    EXPECT_EQ(console::complete("tp T", 4).candidates, (std::vector<std::string>{"/A/Twin", "/B/Twin"})) << "a shared name by its paths";

    CommandSpec pick;
    pick.name = "pick";
    pick.args = {consoleArg("first", ArgType::String), consoleArg("fruit", ArgType::String)};
    int askedIndex = -1;
    std::vector<std::string> askedWords;
    pick.complete = [&askedIndex, &askedWords](int argIndex, const std::string&, const std::vector<std::string>& words){
        askedIndex = argIndex;
        askedWords = words;
        return std::vector<std::string>{"banana", "apricot", "apple"};
    };
    pick.run = [](Call&){};
    ASSERT_TRUE(console::add(pick));
    c = console::complete("pick x ap", 9);
    EXPECT_EQ(c.candidates, (std::vector<std::string>{"apple", "apricot"}));
    EXPECT_EQ(c.replacement, "ap");
    EXPECT_EQ(askedIndex, 1);
    EXPECT_EQ(askedWords, (std::vector<std::string>{"x"}));

    for (int i = 0; i < 60; i++) {
        ASSERT_TRUE(addRecorded("zz" + std::to_string(100 + i), {}, many));
    }
    c = console::complete("zz", 2);
    EXPECT_EQ(c.candidates.size(), 50u);
    EXPECT_EQ(c.candidates.front(), "zz100");
    EXPECT_EQ(c.replacement, "zz1");
}

// AC8: a failing cheat sets nothing; the first ok one sets the flag and fires once
TEST_F(ConsoleCommands, CheatFlagAndEvent){
    std::vector<std::string> fired;
    Events::onCheatUsed.add("console-test", std::function<void(const std::string&)>([&fired](const std::string& name){ fired.push_back(name); }));
    CommandSpec boost;
    boost.name = "boost";
    boost.cheat = true;
    boost.maxArgs = 1;
    boost.run = [](Call& call){
        if (!call.words().empty() && call.words()[0] == "bad")
            call.fail("bad boost");
    };
    ASSERT_TRUE(console::add(boost));
    CommandSpec plain;
    plain.name = "plain";
    plain.run = [](Call&){};
    ASSERT_TRUE(console::add(plain));

    EXPECT_FALSE(console::execute("boost bad").ok());
    EXPECT_FALSE(console::execute("boost a b").ok()) << "a usage error";
    EXPECT_FALSE(console::cheatsUsed());
    EXPECT_TRUE(fired.empty());
    EXPECT_TRUE(console::execute("plain").ok());
    EXPECT_FALSE(console::cheatsUsed()) << "not a cheat";

    const Result r = console::execute("boost");
    EXPECT_TRUE(r.cheat);
    EXPECT_TRUE(console::cheatsUsed());
    console::execute("boost");
    EXPECT_EQ(fired, (std::vector<std::string>{"boost"})) << "once";
    console::resetCheatsUsed();
    EXPECT_FALSE(console::cheatsUsed());
    console::execute("plain");
    EXPECT_FALSE(console::cheatsUsed());
    console::execute("boost");
    EXPECT_EQ(fired.size(), 2u) << "a new game fires again";
    console::endSession();
    EXPECT_FALSE(console::cheatsUsed()) << "Play stop clears it";
}

// AC9: with commands off, the overlay, agents and --command get `disabled`; the game's code runs
TEST_F(ConsoleCommands, GateBlocksSourcesNotGameCode){
    int runs = 0;
    CommandSpec spec;
    spec.name = "plain";
    spec.run = [&runs](Call&){ runs++; };
    ASSERT_TRUE(console::add(spec));
    console::setCommandsEnabled(false);
    for (Source source : {Source::Overlay, Source::Agent, Source::LaunchOption}){
        const Result r = console::execute("plain", source);
        EXPECT_EQ(r.status, Status::Disabled);
        EXPECT_EQ(r.error, "Console commands are off in this build (Project Settings > Debug > Console Commands).");
    }
    EXPECT_EQ(runs, 0);
    EXPECT_TRUE(console::execute("plain", Source::Game).ok());
    EXPECT_TRUE(DebugOverlay::executeCommand("plain").ok()) << "the game's scripts and C++";
    EXPECT_EQ(runs, 2);
    console::setCommandsEnabled(true);
    EXPECT_TRUE(console::execute("plain", Source::Agent).ok());

    // DebugOverlay::configure sets the gate (Project Settings > Debug > Console Commands)
    DebugOverlaySettings settings;
    settings.commands = false;
    DebugOverlay::configure(settings);
    EXPECT_FALSE(console::commandsEnabled());
    settings.commands = true;
    DebugOverlay::updateSettings(settings);
    EXPECT_TRUE(console::commandsEnabled());
    DebugOverlay::shutdown();
}

// AC10: depth 5 fails; a command that removes itself finishes; main thread only
TEST_F(ConsoleCommands, NestingLimitAndSelfRemoval){
    int runs = 0;
    Result innermost;
    CommandSpec nest;
    nest.name = "nest";
    nest.run = [&runs, &innermost](Call&){
        runs++;
        const Result inner = console::execute("nest");
        if (inner.status != Status::Ok)
            innermost = inner;
    };
    ASSERT_TRUE(console::add(nest));
    EXPECT_TRUE(console::execute("nest").ok());
    EXPECT_EQ(runs, 4);
    EXPECT_EQ(innermost.status, Status::Failed);
    EXPECT_NE(innermost.error.find("nested too deep"), std::string::npos) << innermost.error;
    EXPECT_TRUE(console::execute("nest").ok()) << "the depth went back to 0";
    EXPECT_EQ(runs, 8);

    CommandSpec once;
    once.name = "once";
    once.run = [](Call& call){
        console::remove("once");
        call.print("still here");
    };
    ASSERT_TRUE(console::add(once));
    const Result r = console::execute("once");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.lines, (std::vector<std::string>{"still here"}));
    EXPECT_EQ(console::find("once"), nullptr);

    Result elsewhere;
    std::thread([&elsewhere](){ elsewhere = console::execute("help"); }).join();
    EXPECT_EQ(elsewhere.status, Status::Error);
    EXPECT_EQ(elsewhere.error, "main thread only");
}

// AC11 (R22): nothing is allocated by what runs every frame or at start while no command exists
TEST_F(ConsoleCommands, NothingAllocatedUntilFirstUse){
    ASSERT_FALSE(console::isAllocated());
    // (the overlay's own state is made by its first use, whatever ran before this test)
    DebugOverlay::isEnabled();
    const DebugOverlaySettings settings;
    const std::string absent = "absent";
    const testsupport::ThreadAllocationCount allocations;
    // the start: the generated init's configure sets the gate; then frames with the overlay off
    // (the per-frame hooks), and the session's end (Play stop)
    console::setCommandsEnabled(settings.commands);
    bool anything = console::commandsEnabled() || console::cheatsUsed();
    for (int frame = 0; frame < 100; frame++){
        if (DebugOverlay::needsUpdate())
            DebugOverlay::update(1.0 / 60.0);
        DebugOverlay::handleKey(0, 0, true, false);
        anything = anything || console::cheatsUsed();
    }
    console::removeByTag(absent);
    console::remove(absent);
    console::removeOwned();
    console::endSession();
    const uint64_t counted = allocations.count();
    EXPECT_FALSE(anything);
    EXPECT_FALSE(console::isAllocated()) << "no registry before the first registration or execution";
    if (testsupport::countsAllocations()){
        EXPECT_EQ(counted, 0u);
    }
    console::list();
    EXPECT_TRUE(console::isAllocated()) << "the first use builds it (the built-ins)";
}

}  // namespace ConsoleCommandsTest_cpp
