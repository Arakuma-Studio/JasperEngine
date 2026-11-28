// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_EXPRESSION_H
#define DIALOGUE_EXPRESSION_H

// Contract (docs/specs/dialogue-graphs.md R4, D4)
// - The dialogue expression language, closed: literals (numbers, "strings", true, false),
//   variables `$name`, message arguments `{name}`, `not`, `and`, `or`, == != < <= > >=, + - * /,
//   unary minus, parentheses; built-ins visited("n12"), visits("n12"), random(), chance(p); handler
//   calls `prefix.verb(arg, ...)`, `prefix.verb:arg` (one string argument, the same call) and
//   `prefix.verb` (no arguments).
// - Two modes: a Condition is one expression; Statements are `;`-separated assignments
//   (`$x = e`, `+=`, `-=`) and handler calls (a call as a statement is an action, any other call
//   a condition). An assignment in a condition is a parse error.
// - Compiled once to a postfix program in a pool shared by a dialogue's expressions; a parse error
//   names its 1-based column (code points) and compiles nothing.
// - Evaluation: dynamic types; comparing different types is false (!= true); `and` / `or` short
//   circuit on truthiness; arithmetic on a non-number, or a division by zero, is an error that
//   stops the expression. Never throws, never reads past the source; the stack is the caller's, so
//   a warm evaluation allocates nothing for numbers and bools.
// - Thread-safety: compiling is pure; evaluation runs where its environment does (the main thread).

#include "Export.h"
#include "dialogue/DialogueValue.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::dialogue {

    enum class ExprMode { Condition, Statements };

    enum class Op : uint8_t {
        Const,          // push constants[operand]
        Var,            // push the variable names[operand]
        Arg,            // push the message argument names[operand]
        Not, Neg,
        Eq, Ne, Lt, Le, Gt, Ge,
        Add, Sub, Mul, Div,
        JumpIfFalse,    // short circuit: keeps the top when it jumps to operand, else pops it
        JumpIfTrue,
        Truthy,         // the top as a bool
        Visited,        // visits of the node names[operand] > 0
        Visits,
        Random,         // push a number in [0, 1)
        Chance,         // pop p, push random < p
        CallCondition,  // pop argc arguments, push the condition handler's value (calls[operand])
        CallAction,     // pop argc arguments, run the action handler (calls[operand])
        Assign,         // pop the value into the variable names[operand]
        AssignAdd,
        AssignSub,
    };

    struct Instr {
        Op op = Op::Const;
        uint8_t argc = 0;
        uint32_t operand = 0;
    };

    struct CallSite {
        std::string name;       // "quest.done"
        std::string prefix;     // "quest"
        std::string verb;       // "done"
    };

    // A compiled expression: a range of its pool's code
    struct Expr {
        uint32_t first = 0;
        uint32_t count = 0;
        bool empty() const { return count == 0; }
    };

    struct ExprPool {
        std::vector<Instr> code;
        std::vector<Value> constants;
        std::vector<std::string> names;     // variables, arguments, visited() nodes
        std::vector<CallSite> calls;

        uint32_t name(std::string_view text);
        uint32_t constant(const Value& value);
        uint32_t call(std::string_view prefix, std::string_view verb);
        size_t memoryBytes() const;
    };

    struct ParseError {
        int column = 0;         // 1-based, in code points; 0 = no error
        std::string message;
    };

    // What one compiled expression reads and writes
    struct ExprUse {
        std::vector<std::string> variables;     // read or written
        std::vector<std::string> assigned;
        std::vector<std::string> arguments;
        std::vector<std::string> visitedNodes;
        std::vector<std::string> conditionCalls;    // "quest.done"
        std::vector<std::string> actionCalls;
    };

    // Compiles `source` into `pool` (nothing is added on an error). An empty source compiles to an
    // empty Expr: a condition that is true, statements that do nothing.
    JASPERENGINE_API bool compileExpr(std::string_view source, ExprMode mode, ExprPool& pool, Expr& out, ParseError* error = nullptr);
    JASPERENGINE_API ExprUse exprUse(const ExprPool& pool, const Expr& expr);

    // What an expression reads, writes and calls while it runs
    class JASPERENGINE_API ExprEnv {
    public:
        virtual ~ExprEnv() = default;
        virtual Value variable(uint32_t nameIndex) = 0;
        virtual Value argument(uint32_t nameIndex) = 0;
        virtual void assign(uint32_t nameIndex, const Value& value) = 0;
        virtual uint32_t visits(uint32_t nameIndex) = 0;
        virtual double random01() = 0;
        virtual Value callCondition(const CallSite& call, std::span<const Value> args) = 0;
        virtual void callAction(const CallSite& call, std::span<const Value> args) = 0;
    };

    // Runs `expr`; `out` gets a condition's value (true for an empty one). False with `error` set
    // when it stopped on an evaluation error (the statements before it have run).
    JASPERENGINE_API bool evaluate(const ExprPool& pool, const Expr& expr, ExprEnv& env, std::vector<Value>& stack, Value& out,
                                   std::string* error = nullptr);

}

#endif //DIALOGUE_EXPRESSION_H
