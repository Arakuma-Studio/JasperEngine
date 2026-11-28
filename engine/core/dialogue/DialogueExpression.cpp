// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueExpression.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

// ------------------------------------------------------------------ Value

namespace jasperengine::dialogue {

bool Value::truthy() const {
    switch (index()) {
        case 1: return asBool();
        case 2: return asNumber() != 0.0;
        case 3: return !asString().empty();
        default: return false;
    }
}

bool Value::same(const Value& other) const {
    if (index() != other.index()) return false;
    switch (index()) {
        case 1: return asBool() == other.asBool();
        case 2: return asNumber() == other.asNumber();
        case 3: return asString() == other.asString();
        default: return true;
    }
}

std::string Value::toText() const {
    switch (index()) {
        case 1: return asBool() ? "true" : "false";
        case 2: {
            const double number = asNumber();
            char text[48];
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15) {
                std::snprintf(text, sizeof(text), "%.0f", number);
            } else {
                std::snprintf(text, sizeof(text), "%.15g", number);
            }
            return text;
        }
        case 3: return asString();
        default: return {};
    }
}

std::string Value::describe() const {
    if (isString()) return "\"" + asString() + "\"";
    if (isNil()) return "nil";
    return toText();
}

const char* Value::typeName() const {
    switch (index()) {
        case 1: return "bool";
        case 2: return "number";
        case 3: return "string";
        default: return "nil";
    }
}

// ------------------------------------------------------------------ pool

uint32_t ExprPool::name(std::string_view text) {
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == text) return static_cast<uint32_t>(i);
    }
    names.emplace_back(text);
    return static_cast<uint32_t>(names.size() - 1);
}

uint32_t ExprPool::constant(const Value& value) {
    for (size_t i = 0; i < constants.size(); i++) {
        if (constants[i].same(value)) return static_cast<uint32_t>(i);
    }
    constants.push_back(value);
    return static_cast<uint32_t>(constants.size() - 1);
}

uint32_t ExprPool::call(std::string_view prefix, std::string_view verb) {
    for (size_t i = 0; i < calls.size(); i++) {
        if (calls[i].prefix == prefix && calls[i].verb == verb) return static_cast<uint32_t>(i);
    }
    CallSite site;
    site.prefix = std::string(prefix);
    site.verb = std::string(verb);
    site.name = site.prefix + "." + site.verb;
    calls.push_back(std::move(site));
    return static_cast<uint32_t>(calls.size() - 1);
}

size_t ExprPool::memoryBytes() const {
    size_t bytes = code.capacity() * sizeof(Instr) + constants.capacity() * sizeof(Value) + names.capacity() * sizeof(std::string) +
                   calls.capacity() * sizeof(CallSite);
    for (const Value& value : constants) {
        if (value.isString() && value.asString().size() > 15) bytes += value.asString().capacity();
    }
    for (const std::string& text : names) {
        if (text.size() > 15) bytes += text.capacity();
    }
    for (const CallSite& site : calls) {
        bytes += site.name.size() > 15 ? site.name.capacity() : 0;
    }
    return bytes;
}

// ------------------------------------------------------------------ parser

namespace {

    enum class Tok {
        End, Number, String, Var, Arg, Ident, Shorthand,
        LParen, RParen, Comma, Semi, Assign, PlusAssign, MinusAssign,
        Eq, Ne, Lt, Le, Gt, Ge, Plus, Minus, Star, Slash,
        True, False, Not, And, Or, Error,
    };

    struct Token {
        Tok type = Tok::End;
        size_t start = 0;
        std::string text;       // an identifier, a variable or argument name, a string's value
        double number = 0.0;
    };

    bool isIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    bool isIdentChar(char c) {
        return isIdentStart(c) || (c >= '0' && c <= '9');
    }

    bool isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    int columnOf(std::string_view source, size_t offset) {
        int column = 1;
        for (size_t i = 0; i < offset && i < source.size(); i++) {
            if ((static_cast<unsigned char>(source[i]) & 0xC0) != 0x80) column++;
        }
        return column;
    }

    class Parser {
    public:
        Parser(std::string_view source, ExprMode mode, ExprPool& pool): source(source), mode(mode), pool(pool) {}

        bool run(std::vector<Instr>& code, ParseError& error) {
            out = &code;
            next();
            if (mode == ExprMode::Condition) {
                if (token.type == Tok::End && !failed) return true;
                parseExpr();
                if (!failed && token.type == Tok::Semi) fail(token.start, "';' separates statements: only set and action fields have them");
                else if (!failed && token.type != Tok::End) fail(token.start, "unexpected " + describe(token));
            } else {
                while (!failed && token.type != Tok::End) {
                    if (token.type == Tok::Semi) {
                        next();
                        continue;
                    }
                    parseStatement();
                    if (failed) break;
                    if (token.type == Tok::Semi) next();
                    else if (token.type != Tok::End) fail(token.start, "unexpected " + describe(token) + " (statements are separated by ';')");
                }
            }
            if (failed) error = problem;
            return !failed;
        }

    private:
        std::string_view source;
        ExprMode mode;
        ExprPool& pool;
        std::vector<Instr>* out = nullptr;
        size_t pos = 0;
        Token token;
        bool failed = false;
        ParseError problem;

        void fail(size_t offset, const std::string& message) {
            if (failed) return;
            failed = true;
            problem.column = columnOf(source, offset);
            problem.message = message;
            token.type = Tok::End;
        }

        static std::string describe(const Token& t) {
            switch (t.type) {
                case Tok::End: return "end of text";
                case Tok::Number: return "number";
                case Tok::String: return "string";
                case Tok::Var: return "'$" + t.text + "'";
                case Tok::Arg: return "'{" + t.text + "}'";
                case Tok::Ident: case Tok::Shorthand: return "'" + t.text + "'";
                case Tok::LParen: return "'('";
                case Tok::RParen: return "')'";
                case Tok::Comma: return "','";
                case Tok::Semi: return "';'";
                case Tok::Assign: return "'='";
                case Tok::PlusAssign: return "'+='";
                case Tok::MinusAssign: return "'-='";
                case Tok::Eq: return "'=='";
                case Tok::Ne: return "'!='";
                case Tok::Lt: return "'<'";
                case Tok::Le: return "'<='";
                case Tok::Gt: return "'>'";
                case Tok::Ge: return "'>='";
                case Tok::Plus: return "'+'";
                case Tok::Minus: return "'-'";
                case Tok::Star: return "'*'";
                case Tok::Slash: return "'/'";
                case Tok::True: return "'true'";
                case Tok::False: return "'false'";
                case Tok::Not: return "'not'";
                case Tok::And: return "'and'";
                case Tok::Or: return "'or'";
                default: return "error";
            }
        }

        void emit(Op op, uint32_t operand = 0, uint8_t argc = 0) {
            Instr instr;
            instr.op = op;
            instr.operand = operand;
            instr.argc = argc;
            out->push_back(instr);
        }

        // the token after `token`
        void next() {
            if (failed) return;
            while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' || source[pos] == '\r')) pos++;
            token = Token();
            token.start = pos;
            if (pos >= source.size()) {
                token.type = Tok::End;
                return;
            }
            const char c = source[pos];
            auto two = [&](char second) { return pos + 1 < source.size() && source[pos + 1] == second; };
            auto single = [&](Tok type, size_t length = 1) {
                token.type = type;
                pos += length;
            };
            if (isDigit(c) || (c == '.' && pos + 1 < source.size() && isDigit(source[pos + 1]))) {
                // digit by digit: the same bits on every platform, whatever the C locale
                double value = 0.0;
                while (pos < source.size() && isDigit(source[pos])) value = value * 10.0 + (source[pos++] - '0');
                if (pos < source.size() && source[pos] == '.') {
                    pos++;
                    double scale = 0.1;
                    while (pos < source.size() && isDigit(source[pos])) {
                        value += (source[pos++] - '0') * scale;
                        scale *= 0.1;
                    }
                }
                if (pos < source.size() && isIdentStart(source[pos])) {
                    fail(pos, "a number cannot run into a name");
                    return;
                }
                token.type = Tok::Number;
                token.number = value;
                return;
            }
            if (c == '"') {
                pos++;
                std::string text;
                while (pos < source.size() && source[pos] != '"') {
                    if (source[pos] == '\\' && pos + 1 < source.size()) {
                        const char escaped = source[pos + 1];
                        text += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                        pos += 2;
                        continue;
                    }
                    text += source[pos++];
                }
                if (pos >= source.size()) {
                    fail(token.start, "the string is not closed (a '\"' is missing)");
                    return;
                }
                pos++;
                token.type = Tok::String;
                token.text = std::move(text);
                return;
            }
            if (c == '$') {
                pos++;
                const size_t begin = pos;
                if (pos >= source.size() || !isIdentStart(source[pos])) {
                    fail(token.start, "a variable is '$' and a name ($gold)");
                    return;
                }
                while (pos < source.size() && isIdentChar(source[pos])) pos++;
                token.type = Tok::Var;
                token.text = std::string(source.substr(begin, pos - begin));
                return;
            }
            if (c == '{') {
                pos++;
                while (pos < source.size() && source[pos] == ' ') pos++;
                const size_t begin = pos;
                while (pos < source.size() && isIdentChar(source[pos])) pos++;
                const size_t end = pos;
                while (pos < source.size() && source[pos] == ' ') pos++;
                if (end == begin || pos >= source.size() || source[pos] != '}') {
                    fail(token.start, "an argument is '{name}'");
                    return;
                }
                pos++;
                token.type = Tok::Arg;
                token.text = std::string(source.substr(begin, end - begin));
                return;
            }
            if (isIdentStart(c)) {
                const size_t begin = pos;
                while (pos < source.size() && isIdentChar(source[pos])) pos++;
                while (pos + 1 < source.size() && source[pos] == '.' && isIdentStart(source[pos + 1])) {
                    pos++;
                    while (pos < source.size() && isIdentChar(source[pos])) pos++;
                }
                token.text = std::string(source.substr(begin, pos - begin));
                if (token.text == "true") token.type = Tok::True;
                else if (token.text == "false") token.type = Tok::False;
                else if (token.text == "not") token.type = Tok::Not;
                else if (token.text == "and") token.type = Tok::And;
                else if (token.text == "or") token.type = Tok::Or;
                else token.type = Tok::Ident;
                return;
            }
            switch (c) {
                case '(': single(Tok::LParen); return;
                case ')': single(Tok::RParen); return;
                case ',': single(Tok::Comma); return;
                case ';': single(Tok::Semi); return;
                case '*': single(Tok::Star); return;
                case '/': single(Tok::Slash); return;
                case '+': if (two('=')) single(Tok::PlusAssign, 2); else single(Tok::Plus); return;
                case '-': if (two('=')) single(Tok::MinusAssign, 2); else single(Tok::Minus); return;
                case '=': if (two('=')) single(Tok::Eq, 2); else single(Tok::Assign); return;
                case '!':
                    if (two('=')) {
                        single(Tok::Ne, 2);
                        return;
                    }
                    fail(pos, "'!' is not an operator: use 'not'");
                    return;
                case '<': if (two('=')) single(Tok::Le, 2); else single(Tok::Lt); return;
                case '>': if (two('=')) single(Tok::Ge, 2); else single(Tok::Gt); return;
                case '&': fail(pos, "'&' is not an operator: use 'and'"); return;
                case '|': fail(pos, "'|' is not an operator: use 'or'"); return;
                case ':': fail(pos, "':' follows a handler name (quest.done:intro)"); return;
                default: break;
            }
            fail(pos, std::string("unexpected character '") + c + "'");
        }

        // ':' right after a dotted name: the rest up to a space, ';', ',' or ')' is one string argument
        bool shorthandArgument(std::string& out) {
            if (pos >= source.size() || source[pos] != ':') return false;
            const size_t begin = ++pos;
            while (pos < source.size() && source[pos] != ' ' && source[pos] != '\t' && source[pos] != ';' && source[pos] != ',' &&
                   source[pos] != ')' && source[pos] != '\n' && source[pos] != '\r') {
                pos++;
            }
            if (pos == begin) {
                fail(begin, "a ':' argument is missing (quest.done:intro)");
                return false;
            }
            out = std::string(source.substr(begin, pos - begin));
            return true;
        }

        void parseStatement() {
            if (token.type == Tok::Var) {
                const Token variable = token;
                next();
                if (token.type == Tok::Assign || token.type == Tok::PlusAssign || token.type == Tok::MinusAssign) {
                    const Op op = token.type == Tok::Assign ? Op::Assign : token.type == Tok::PlusAssign ? Op::AssignAdd : Op::AssignSub;
                    next();
                    if (token.type == Tok::End || token.type == Tok::Semi) {
                        fail(token.start, "the assignment has no value");
                        return;
                    }
                    parseExpr();
                    emit(op, pool.name(variable.text));
                    return;
                }
                fail(variable.start, "a statement is an assignment ($" + variable.text + " = ...) or a handler call");
                return;
            }
            if (token.type != Tok::Ident || token.text.find('.') == std::string::npos) {
                fail(token.start, "a statement is an assignment ($x = ...) or a handler call (prefix.verb(...))");
                return;
            }
            parseCall(true);
            if (!failed && token.type != Tok::Semi && token.type != Tok::End) {
                fail(token.start, "a statement is one assignment or one handler call (unexpected " + describe(token) + ")");
            }
        }

        void parseExpr() {
            parseOr();
        }

        void parseOr() {
            parseAnd();
            std::vector<size_t> jumps;
            while (!failed && token.type == Tok::Or) {
                jumps.push_back(out->size());
                emit(Op::JumpIfTrue);
                next();
                parseAnd();
            }
            if (!jumps.empty()) {
                for (size_t jump : jumps) (*out)[jump].operand = static_cast<uint32_t>(out->size());
                emit(Op::Truthy);
            }
        }

        void parseAnd() {
            parseNot();
            std::vector<size_t> jumps;
            while (!failed && token.type == Tok::And) {
                jumps.push_back(out->size());
                emit(Op::JumpIfFalse);
                next();
                parseNot();
            }
            if (!jumps.empty()) {
                for (size_t jump : jumps) (*out)[jump].operand = static_cast<uint32_t>(out->size());
                emit(Op::Truthy);
            }
        }

        void parseNot() {
            if (token.type == Tok::Not) {
                next();
                parseNot();
                emit(Op::Not);
                return;
            }
            parseComparison();
        }

        void parseComparison() {
            parseAdditive();
            Op op;
            switch (token.type) {
                case Tok::Eq: op = Op::Eq; break;
                case Tok::Ne: op = Op::Ne; break;
                case Tok::Lt: op = Op::Lt; break;
                case Tok::Le: op = Op::Le; break;
                case Tok::Gt: op = Op::Gt; break;
                case Tok::Ge: op = Op::Ge; break;
                case Tok::Assign:
                    fail(token.start, mode == ExprMode::Condition ? "assignments are only for set and action fields (compare with '==')"
                                                                  : "'=' assigns a variable only at the start of a statement (compare with '==')");
                    return;
                case Tok::PlusAssign:
                case Tok::MinusAssign:
                    fail(token.start, "assignments are only for set and action fields");
                    return;
                default: return;
            }
            next();
            parseAdditive();
            emit(op);
            if (!failed && (token.type == Tok::Eq || token.type == Tok::Ne || token.type == Tok::Lt || token.type == Tok::Le ||
                            token.type == Tok::Gt || token.type == Tok::Ge)) {
                fail(token.start, "comparisons do not chain: join them with 'and'");
            }
        }

        void parseAdditive() {
            parseMultiplicative();
            while (!failed && (token.type == Tok::Plus || token.type == Tok::Minus)) {
                const Op op = token.type == Tok::Plus ? Op::Add : Op::Sub;
                next();
                parseMultiplicative();
                emit(op);
            }
        }

        void parseMultiplicative() {
            parseUnary();
            while (!failed && (token.type == Tok::Star || token.type == Tok::Slash)) {
                const Op op = token.type == Tok::Star ? Op::Mul : Op::Div;
                next();
                parseUnary();
                emit(op);
            }
        }

        void parseUnary() {
            if (token.type == Tok::Minus) {
                next();
                parseUnary();
                emit(Op::Neg);
                return;
            }
            parsePrimary();
        }

        void parsePrimary() {
            if (failed) return;
            switch (token.type) {
                case Tok::Number:
                    emit(Op::Const, pool.constant(Value(token.number)));
                    next();
                    return;
                case Tok::String:
                    emit(Op::Const, pool.constant(Value(token.text)));
                    next();
                    return;
                case Tok::True:
                case Tok::False:
                    emit(Op::Const, pool.constant(Value(token.type == Tok::True)));
                    next();
                    return;
                case Tok::Var:
                    emit(Op::Var, pool.name(token.text));
                    next();
                    return;
                case Tok::Arg:
                    emit(Op::Arg, pool.name(token.text));
                    next();
                    return;
                case Tok::LParen: {
                    const size_t open = token.start;
                    next();
                    if (token.type == Tok::RParen) {
                        fail(token.start, "empty parentheses");
                        return;
                    }
                    parseExpr();
                    if (failed) return;
                    if (token.type != Tok::RParen) {
                        fail(token.type == Tok::End ? open : token.start, "a ')' is missing");
                        return;
                    }
                    next();
                    return;
                }
                case Tok::Ident:
                    parseCall(false);
                    return;
                case Tok::End:
                    fail(token.start, "a value is missing");
                    return;
                default:
                    fail(token.start, "unexpected " + describe(token));
                    return;
            }
        }

        // a built-in or a handler call; `statement`: a handler call that is a whole statement (an action)
        void parseCall(bool statement) {
            const Token name = token;
            const size_t dot = name.text.find('.');
            if (dot == std::string::npos) {
                parseBuiltin(name);
                return;
            }
            const std::string prefix = name.text.substr(0, dot);
            const std::string verb = name.text.substr(dot + 1);
            uint8_t argc = 0;
            std::string shorthand;
            if (shorthandArgument(shorthand)) {
                emit(Op::Const, pool.constant(Value(shorthand)));
                argc = 1;
                next();
            } else if (failed) {
                return;
            } else {
                next();
                if (token.type == Tok::LParen) {
                    const size_t open = token.start;
                    next();
                    if (token.type != Tok::RParen) {
                        while (!failed) {
                            parseExpr();
                            if (failed) return;
                            if (argc == 255) {
                                fail(token.start, "too many arguments");
                                return;
                            }
                            argc++;
                            if (token.type == Tok::Comma) {
                                next();
                                continue;
                            }
                            break;
                        }
                        if (failed) return;
                        if (token.type != Tok::RParen) {
                            fail(token.type == Tok::End ? open : token.start, "a ')' is missing after the arguments of " + name.text);
                            return;
                        }
                    }
                    next();
                }
            }
            emit(statement ? Op::CallAction : Op::CallCondition, pool.call(prefix, verb), argc);
        }

        void parseBuiltin(const Token& name) {
            const std::string& id = name.text;
            if (id != "visited" && id != "visits" && id != "random" && id != "chance") {
                fail(name.start, "unknown function '" + id + "' (built-ins: visited, visits, random, chance; handler calls are prefix.verb(...))");
                return;
            }
            next();
            if (token.type != Tok::LParen) {
                fail(token.start, id + " needs parentheses: " + id + (id == "random" ? "()" : id == "chance" ? "(0.25)" : "(\"n12\")"));
                return;
            }
            const size_t open = token.start;
            next();
            if (id == "random") {
                if (token.type != Tok::RParen) {
                    fail(token.start, "random() takes no argument");
                    return;
                }
                emit(Op::Random);
            } else if (id == "chance") {
                if (token.type == Tok::RParen) {
                    fail(token.start, "chance needs a probability: chance(0.25)");
                    return;
                }
                parseExpr();
                if (failed) return;
                emit(Op::Chance);
            } else {
                if (token.type != Tok::String) {
                    fail(token.start, id + " takes a node id string: " + id + "(\"n12\")");
                    return;
                }
                emit(id == "visited" ? Op::Visited : Op::Visits, pool.name(token.text));
                next();
            }
            if (token.type != Tok::RParen) {
                fail(token.type == Tok::End ? open : token.start, "a ')' is missing after " + id);
                return;
            }
            next();
        }
    };

}

bool compileExpr(std::string_view source, ExprMode mode, ExprPool& pool, Expr& out, ParseError* error) {
    out = Expr();
    const size_t names = pool.names.size();
    const size_t constants = pool.constants.size();
    const size_t calls = pool.calls.size();
    std::vector<Instr> code;
    ParseError problem;
    Parser parser(source, mode, pool);
    if (!parser.run(code, problem)) {
        pool.names.resize(names);
        pool.constants.resize(constants);
        pool.calls.resize(calls);
        if (error) *error = problem;
        return false;
    }
    if (error) *error = ParseError();
    out.first = static_cast<uint32_t>(pool.code.size());
    out.count = static_cast<uint32_t>(code.size());
    pool.code.insert(pool.code.end(), code.begin(), code.end());
    return true;
}

ExprUse exprUse(const ExprPool& pool, const Expr& expr) {
    ExprUse use;
    auto add = [](std::vector<std::string>& list, const std::string& text) {
        if (std::find(list.begin(), list.end(), text) == list.end()) list.push_back(text);
    };
    for (uint32_t i = 0; i < expr.count; i++) {
        const Instr& instr = pool.code[expr.first + i];
        switch (instr.op) {
            case Op::Var: add(use.variables, pool.names[instr.operand]); break;
            case Op::Assign: case Op::AssignAdd: case Op::AssignSub:
                add(use.variables, pool.names[instr.operand]);
                add(use.assigned, pool.names[instr.operand]);
                break;
            case Op::Arg: add(use.arguments, pool.names[instr.operand]); break;
            case Op::Visited: case Op::Visits: add(use.visitedNodes, pool.names[instr.operand]); break;
            case Op::CallCondition: add(use.conditionCalls, pool.calls[instr.operand].name); break;
            case Op::CallAction: add(use.actionCalls, pool.calls[instr.operand].name); break;
            default: break;
        }
    }
    return use;
}

// ------------------------------------------------------------------ evaluation

namespace {

    bool numbers(const std::vector<Value>& stack) {
        return stack[stack.size() - 2].isNumber() && stack.back().isNumber();
    }

    bool fail(std::string* error, std::string message) {
        if (error) *error = std::move(message);
        return false;
    }

}

bool evaluate(const ExprPool& pool, const Expr& expr, ExprEnv& env, std::vector<Value>& stack, Value& out,
                                      std::string* error) {
    stack.clear();
    const Instr* code = pool.code.data() + expr.first;
    uint32_t pc = 0;
    while (pc < expr.count) {
        const Instr& instr = code[pc++];
        switch (instr.op) {
            case Op::Const: stack.push_back(pool.constants[instr.operand]); break;
            case Op::Var: stack.push_back(env.variable(instr.operand)); break;
            case Op::Arg: stack.push_back(env.argument(instr.operand)); break;
            case Op::Not: stack.back() = Value(!stack.back().truthy()); break;
            case Op::Neg:
                if (!stack.back().isNumber()) return fail(error, std::string("'-' of a ") + stack.back().typeName());
                stack.back() = Value(-stack.back().asNumber());
                break;
            case Op::Eq: case Op::Ne: {
                const bool same = stack[stack.size() - 2].same(stack.back());
                stack.pop_back();
                stack.back() = Value(instr.op == Op::Eq ? same : !same);
                break;
            }
            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                const Value& a = stack[stack.size() - 2];
                const Value& b = stack.back();
                bool result = false;
                if (a.isNumber() && b.isNumber()) {
                    const double x = a.asNumber(), y = b.asNumber();
                    result = instr.op == Op::Lt ? x < y : instr.op == Op::Le ? x <= y : instr.op == Op::Gt ? x > y : x >= y;
                } else if (a.isString() && b.isString()) {
                    const int c = a.asString().compare(b.asString());
                    result = instr.op == Op::Lt ? c < 0 : instr.op == Op::Le ? c <= 0 : instr.op == Op::Gt ? c > 0 : c >= 0;
                }
                stack.pop_back();
                stack.back() = Value(result);
                break;
            }
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: {
                if (!numbers(stack)) {
                    const Value& bad = stack[stack.size() - 2].isNumber() ? stack.back() : stack[stack.size() - 2];
                    return fail(error, std::string("arithmetic on a ") + bad.typeName() + " (" + bad.describe() + ")");
                }
                const double y = stack.back().asNumber();
                stack.pop_back();
                const double x = stack.back().asNumber();
                if (instr.op == Op::Div && y == 0.0) return fail(error, "division by zero");
                stack.back() = Value(instr.op == Op::Add ? x + y : instr.op == Op::Sub ? x - y : instr.op == Op::Mul ? x * y : x / y);
                break;
            }
            case Op::JumpIfFalse:
                if (!stack.back().truthy()) pc = instr.operand;
                else stack.pop_back();
                break;
            case Op::JumpIfTrue:
                if (stack.back().truthy()) pc = instr.operand;
                else stack.pop_back();
                break;
            case Op::Truthy: stack.back() = Value(stack.back().truthy()); break;
            case Op::Visited: stack.push_back(Value(env.visits(instr.operand) > 0)); break;
            case Op::Visits: stack.push_back(Value(static_cast<double>(env.visits(instr.operand)))); break;
            case Op::Random: stack.push_back(Value(env.random01())); break;
            case Op::Chance:
                if (!stack.back().isNumber()) return fail(error, std::string("chance of a ") + stack.back().typeName());
                stack.back() = Value(env.random01() < stack.back().asNumber());
                break;
            case Op::CallCondition: {
                const size_t base = stack.size() - instr.argc;
                Value result = env.callCondition(pool.calls[instr.operand], std::span<const Value>(stack.data() + base, instr.argc));
                stack.resize(base);
                stack.push_back(std::move(result));
                break;
            }
            case Op::CallAction: {
                const size_t base = stack.size() - instr.argc;
                env.callAction(pool.calls[instr.operand], std::span<const Value>(stack.data() + base, instr.argc));
                stack.resize(base);
                break;
            }
            case Op::Assign:
                env.assign(instr.operand, stack.back());
                stack.pop_back();
                break;
            case Op::AssignAdd: case Op::AssignSub: {
                const Value current = env.variable(instr.operand);
                if (!current.isNumber() || !stack.back().isNumber()) {
                    const Value& bad = current.isNumber() ? stack.back() : current;
                    return fail(error, std::string(instr.op == Op::AssignAdd ? "'+='" : "'-='") + " on a " + bad.typeName() + " ($" +
                                           pool.names[instr.operand] + ")");
                }
                const double delta = stack.back().asNumber();
                stack.pop_back();
                env.assign(instr.operand, Value(instr.op == Op::AssignAdd ? current.asNumber() + delta : current.asNumber() - delta));
                break;
            }
        }
    }
    if (stack.empty()) out = Value(true);
    else out = std::move(stack.back());
    stack.clear();
    return true;
}

}  // namespace jasperengine::dialogue
