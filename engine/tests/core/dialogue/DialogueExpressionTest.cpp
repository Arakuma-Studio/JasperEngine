// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The dialogue expression language (docs/specs/dialogue-graphs.md R4, AC1).

#include "dialogue/DialogueExpression.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace DialogueExpressionTest_cpp {

using namespace jasperengine::dialogue;

namespace {

    class TestEnv : public ExprEnv {
    public:
        ExprPool* pool = nullptr;
        std::map<std::string, Value> variables;
        std::map<std::string, Value> arguments;
        std::map<std::string, uint32_t> visitCounts;
        std::vector<std::string> calls;
        Value conditionResult = Value(true);
        double nextRandom = 0.5;

        Value variable(uint32_t index) override {
            auto it = variables.find(pool->names[index]);
            return it == variables.end() ? Value() : it->second;
        }
        Value argument(uint32_t index) override {
            auto it = arguments.find(pool->names[index]);
            return it == arguments.end() ? Value() : it->second;
        }
        void assign(uint32_t index, const Value& value) override { variables[pool->names[index]] = value; }
        uint32_t visits(uint32_t index) override {
            auto it = visitCounts.find(pool->names[index]);
            return it == visitCounts.end() ? 0 : it->second;
        }
        double random01() override { return nextRandom; }
        Value callCondition(const CallSite& call, std::span<const Value> args) override {
            calls.push_back("cond " + call.name + record(args));
            return conditionResult;
        }
        void callAction(const CallSite& call, std::span<const Value> args) override { calls.push_back("act " + call.name + record(args)); }

    private:
        static std::string record(std::span<const Value> args) {
            std::string out = "(";
            for (size_t i = 0; i < args.size(); i++) out += (i ? "," : "") + args[i].describe();
            return out + ")";
        }
    };

    struct Eval {
        ExprPool pool;
        TestEnv env;
        std::vector<Value> stack;
        Eval() { env.pool = &pool; }

        Value run(const std::string& source, ExprMode mode = ExprMode::Condition, std::string* error = nullptr) {
            Expr expr;
            ParseError parse;
            EXPECT_TRUE(compileExpr(source, mode, pool, expr, &parse)) << source << ": " << parse.column << " " << parse.message;
            Value out;
            const bool ok = evaluate(pool, expr, env, stack, out, error);
            if (!error) {
                EXPECT_TRUE(ok) << source;
            }
            return ok ? out : Value();
        }
    };

    ParseError parseError(const std::string& source, ExprMode mode = ExprMode::Condition) {
        ExprPool pool;
        Expr expr;
        ParseError error;
        EXPECT_FALSE(compileExpr(source, mode, pool, expr, &error)) << source;
        EXPECT_TRUE(pool.code.empty() && pool.names.empty() && pool.constants.empty()) << "a failed compile adds nothing: " << source;
        return error;
    }

}

TEST(DialogueExpression, EvaluatesPrecedenceAndTypes) {
    Eval e;
    e.env.variables["gold"] = Value(12.0);
    e.env.variables["name"] = Value("Ana");
    e.env.variables["met"] = Value(true);
    EXPECT_TRUE(e.run("1 + 2 * 3 == 7").same(Value(true)));
    EXPECT_TRUE(e.run("(1 + 2) * 3").same(Value(9.0)));
    EXPECT_TRUE(e.run("-2 - -3").same(Value(1.0)));
    EXPECT_TRUE(e.run("10 / 4").same(Value(2.5)));
    EXPECT_TRUE(e.run("$gold >= 10 and $met").same(Value(true)));
    EXPECT_TRUE(e.run("not $met or $gold < 5").same(Value(false)));
    EXPECT_TRUE(e.run("not $met and $gold > 1 or $gold == 12").same(Value(true))) << "and binds tighter than or";
    EXPECT_TRUE(e.run("$name == \"Ana\"").same(Value(true)));
    EXPECT_TRUE(e.run("\"b\" > \"a\"").same(Value(true)));
    // different types never compare equal; ordering them is false
    EXPECT_TRUE(e.run("$gold == \"12\"").same(Value(false)));
    EXPECT_TRUE(e.run("$gold != \"12\"").same(Value(true)));
    EXPECT_TRUE(e.run("$name < 3").same(Value(false)));
    EXPECT_TRUE(e.run("$unknown == $alsoUnknown").same(Value(true))) << "nothing equals nothing";
    EXPECT_TRUE(e.run("true").same(Value(true)));
    EXPECT_TRUE(e.run("2.25 * 4").same(Value(9.0)));
    // and / or short-circuit: the right side's call never runs
    e.env.calls.clear();
    EXPECT_TRUE(e.run("false and quest.done(\"a\")").same(Value(false)));
    EXPECT_TRUE(e.run("true or quest.done(\"a\")").same(Value(true)));
    EXPECT_TRUE(e.env.calls.empty());
    // arithmetic on a non-number is an error
    std::string error;
    e.run("$name + 1", ExprMode::Condition, &error);
    EXPECT_NE(error.find("arithmetic on a string"), std::string::npos) << error;
    error.clear();
    e.run("1 / 0", ExprMode::Condition, &error);
    EXPECT_NE(error.find("division by zero"), std::string::npos) << error;
    // built-ins
    e.env.visitCounts["n12"] = 3;
    EXPECT_TRUE(e.run("visited(\"n12\") and visits(\"n12\") == 3 and not visited(\"n13\")").same(Value(true)));
    e.env.nextRandom = 0.2;
    EXPECT_TRUE(e.run("chance(0.25)").same(Value(true)));
    EXPECT_TRUE(e.run("chance(0.1)").same(Value(false)));
    EXPECT_TRUE(e.run("random() < 0.3").same(Value(true)));
    // message arguments
    e.env.arguments["bribe"] = Value(5.0);
    EXPECT_TRUE(e.run("{bribe} * 2 == 10").same(Value(true)));
    // an empty condition is true
    EXPECT_TRUE(e.run("").same(Value(true)));
}

TEST(DialogueExpression, ShorthandAndCallFormsAreEqual) {
    ExprPool pool;
    Expr a, b;
    ASSERT_TRUE(compileExpr("quest.done:intro", ExprMode::Condition, pool, a));
    ASSERT_TRUE(compileExpr("quest.done(\"intro\")", ExprMode::Condition, pool, b));
    ASSERT_EQ(a.count, b.count);
    for (uint32_t i = 0; i < a.count; i++) {
        EXPECT_EQ(pool.code[a.first + i].op, pool.code[b.first + i].op);
        EXPECT_EQ(pool.code[a.first + i].operand, pool.code[b.first + i].operand);
        EXPECT_EQ(pool.code[a.first + i].argc, pool.code[b.first + i].argc);
    }
    EXPECT_EQ(pool.calls.size(), 1u);
    EXPECT_EQ(pool.calls[0].prefix, "quest");
    EXPECT_EQ(pool.calls[0].verb, "done");

    Eval e;
    e.run("job.pay:tavern_2; shop.open(\"weapons\", 3)", ExprMode::Statements);
    EXPECT_EQ(e.env.calls, (std::vector<std::string>{"act job.pay(\"tavern_2\")", "act shop.open(\"weapons\",3)"}));
    e.env.calls.clear();
    e.run("quest.count:wolves > 2 or inventory.has", ExprMode::Condition);
    EXPECT_EQ(e.env.calls, (std::vector<std::string>{"cond quest.count(\"wolves\")", "cond inventory.has()"}));
}

TEST(DialogueExpression, ParseErrorsNameTheColumn) {
    EXPECT_EQ(parseError("$gold > ").column, 9);
    EXPECT_EQ(parseError("(1 + 2").column, 1);
    ParseError bang = parseError("$a && $b");
    EXPECT_EQ(bang.column, 4);
    EXPECT_NE(bang.message.find("and"), std::string::npos) << bang.message;
    EXPECT_EQ(parseError("\"unclosed").column, 1);
    EXPECT_EQ(parseError("foo(1)").column, 1);
    EXPECT_EQ(parseError("1 < 2 < 3").column, 7);
    EXPECT_EQ(parseError("visited(n12)").column, 9);
    // columns count code points, not bytes
    EXPECT_EQ(parseError("\"ñandú\" == ").column, 12);
}

TEST(DialogueExpression, AssignmentsOnlyInSet) {
    // a condition never assigns
    EXPECT_NE(parseError("$gold = 5").message.find("assignments"), std::string::npos);
    EXPECT_EQ(parseError("$gold += 5").column, 7);
    EXPECT_EQ(parseError("$a == 1; $b == 2").column, 8);
    // statements assign, add and subtract; a bare value is not a statement
    Eval e;
    e.env.variables["gold"] = Value(10.0);
    e.run("$gold -= 3; $met = true; $name = \"Ana\"; $gold += $gold * 2", ExprMode::Statements);
    EXPECT_TRUE(e.env.variables["gold"].same(Value(21.0)));
    EXPECT_TRUE(e.env.variables["met"].same(Value(true)));
    EXPECT_TRUE(e.env.variables["name"].same(Value("Ana")));
    EXPECT_NE(parseError("$gold + 1", ExprMode::Statements).message.find("statement"), std::string::npos);
    EXPECT_NE(parseError("1 + 2", ExprMode::Statements).message.find("statement"), std::string::npos);
    // += on a string is an evaluation error
    std::string error;
    e.run("$name += 1", ExprMode::Statements, &error);
    EXPECT_NE(error.find("'+='"), std::string::npos) << error;
}

TEST(DialogueExpression, MalformedInputNeverCrashes) {
    // 1,000 mutations of valid expressions from a fixed seed: each compiles or names a column inside
    // the text, and what compiles evaluates without reading past anything (ASan in the sanitizer lane)
    const std::vector<std::string> seeds = {
        "$gold >= 10 and not visited(\"n3\")", "quest.done:intro or chance(0.5)", "$a = ($b + 2) * {c}; shop.open(\"x\", 1, true)",
        "random() < 0.25 and $name == \"Ana\"", "-(-3) / 2 != $x", "$x += visits(\"n1\"); $y -= 1"};
    const std::string alphabet = "$(){}\";:.,=!<>+-*/ abcnot01andor9\xc3\xb1";
    uint64_t state = 0x1234567u;
    auto next = [&]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    };
    int compiled = 0;
    for (int i = 0; i < 1000; i++) {
        std::string text = seeds[next() % seeds.size()];
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int k = 0; k < edits && !text.empty(); k++) {
            const size_t at = next() % (text.size() + 1);
            switch (next() % 3) {
                case 0: text.insert(at, 1, alphabet[next() % alphabet.size()]); break;
                case 1: if (at < text.size()) text.erase(at, 1); break;
                default: if (at < text.size()) text[at] = alphabet[next() % alphabet.size()]; break;
            }
        }
        for (ExprMode mode : {ExprMode::Condition, ExprMode::Statements}) {
            ExprPool pool;
            Expr expr;
            ParseError error;
            if (compileExpr(text, mode, pool, expr, &error)) {
                compiled++;
                TestEnv env;
                env.pool = &pool;
                std::vector<Value> stack;
                Value out;
                evaluate(pool, expr, env, stack, out);
            } else {
                EXPECT_GE(error.column, 1) << text;
                EXPECT_LE(error.column, static_cast<int>(text.size()) + 1) << text;
                EXPECT_FALSE(error.message.empty()) << text;
            }
        }
    }
    EXPECT_GT(compiled, 0);
}

}  // namespace DialogueExpressionTest_cpp
