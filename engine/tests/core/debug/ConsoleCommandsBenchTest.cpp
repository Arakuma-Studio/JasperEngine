// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The console's budgets (docs/specs/dev-console-for-agents.md R19, R20, AC11): a 5-argument line's
// dispatch, a first-word completion among 500 commands, a GameObject completion in a scene of
// 10,000. Each prints a [bench] line; the budgets are enforced only with JASPER_TEST_PERF_SLACK
// (perfSlack(), docs/testing.md).

#include "debug/ConsoleCommands.h"

#include "Engine.h"
#include "Scene.h"
#include "../ecs/EcsTestUtil.h"

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace ConsoleCommandsBenchTest_cpp {

using namespace jasperengine;
using namespace jasperengine::console;

namespace {

    using Clock = std::chrono::steady_clock;

    void expectWithin(double measured, double budget, const char* what){
        const double slack = testsupport::perfSlack();
        std::printf("[bench] %s: %.3f (budget %.3f)\n", what, measured, budget);
        if (slack > 0.0){
            EXPECT_LE(measured, budget * slack) << what;
        }
    }

    ArgSpec arg(const char* name, ArgType type){
        ArgSpec spec;
        spec.name = name;
        spec.type = type;
        return spec;
    }

    struct Fresh {
        Fresh(){ console::reset(); }
        ~Fresh(){ Engine::setScene(nullptr); console::reset(); }
    };

}

// R19: split, lookup, alias resolution and typed parsing of a 5-argument line, the body empty
// (the echo line is logged as for any call)
TEST(ConsoleCommandsBench, Dispatch5Args){
    Fresh fresh;
    ArgSpec mode = arg("mode", ArgType::Enum);
    mode.choices = {"slow", "normal", "fast"};
    CommandSpec spec;
    spec.name = "five";
    spec.aliases = {"5"};
    spec.args = {arg("count", ArgType::Int), arg("speed", ArgType::Float), arg("on", ArgType::Bool), mode, arg("at", ArgType::Vector3)};
    int runs = 0;
    spec.run = [&runs](Call&){ runs++; };
    ASSERT_TRUE(console::add(spec));
    constexpr int calls = 2000;
    const auto start = Clock::now();
    for (int i = 0; i < calls; i++){
        if (!console::execute("5 42 1.5 yes fa 1,2,3").ok())
            FAIL() << "the line did not run";
    }
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / calls;
    EXPECT_EQ(runs, calls);
    expectWithin(us, 20.0, "ConsoleCommands dispatch of a 5-argument line (us)");
}

// R20: a first-word completion among 500 commands; a GameObject completion among 10,000
TEST(ConsoleCommandsBench, Complete){
    Fresh fresh;
    for (int i = 0; i < 500; i++){
        CommandSpec spec;
        spec.name = "cmd" + std::to_string(1000 + i);
        spec.run = [](Call&){};
        ASSERT_TRUE(console::add(spec));
    }
    CommandSpec tp;
    tp.name = "tp";
    tp.args = {arg("target", ArgType::GameObject)};
    tp.run = [](Call&){};
    ASSERT_TRUE(console::add(tp));

    constexpr int names = 200;
    auto start = Clock::now();
    size_t found = 0;
    for (int i = 0; i < names; i++)
        found += console::complete("cmd12", 5).candidates.size();
    const double nameMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / names;
    EXPECT_EQ(found, static_cast<size_t>(names) * 50u) << "capped at 50";
    expectWithin(nameMs, 0.2, "ConsoleCommands first-word completion, 500 commands (ms)");

    Scene scene;
    for (int i = 0; i < 10000; i++)
        scene.setEntityName(test::makeTransformEntity(scene), "Object" + std::to_string(i));
    Engine::setScene(&scene);
    constexpr int objects = 20;
    start = Clock::now();
    found = 0;
    for (int i = 0; i < objects; i++)
        found += console::complete("tp Object99", 11).candidates.size();
    const double objectMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / objects;
    EXPECT_EQ(found, static_cast<size_t>(objects) * 20u) << "111 match Object99: the first 20";
    expectWithin(objectMs, 1.0, "ConsoleCommands gameobject completion, 10,000 GameObjects (ms)");
}

}  // namespace ConsoleCommandsBenchTest_cpp
