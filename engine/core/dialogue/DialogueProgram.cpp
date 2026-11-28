// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueProgram.h"

#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>

using namespace jasperengine;

namespace jasperengine::dialogue {

namespace {

    using Json = graph::Json;

    std::string stringParam(const graph::Node& node, const char* name) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    bool boolParam(const graph::Node& node, const char* name, bool fallback = false) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    const Json& arrayParam(const graph::Node& node, const char* name) {
        static const Json empty = Json::array();
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_array() ? *it : empty;
    }

    // an asset parameter's path: {guid, path} or a plain path
    std::string assetPath(const Json& value) {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_object()) return value.value("path", std::string());
        return {};
    }

    std::string assetParam(const graph::Node& node, const char* name) {
        auto it = node.params.find(name);
        return it != node.params.end() ? assetPath(*it) : std::string();
    }

    // tags: ["a", "b"] or "a, b"
    std::vector<std::string> tagList(const Json& value) {
        std::vector<std::string> out;
        if (value.is_array()) {
            for (const Json& item : value) {
                if (item.is_string() && !item.get<std::string>().empty()) out.push_back(item.get<std::string>());
            }
        } else if (value.is_string()) {
            std::string current;
            for (char c : value.get<std::string>() + ",") {
                if (c == ',') {
                    while (!current.empty() && current.back() == ' ') current.pop_back();
                    if (!current.empty()) out.push_back(current);
                    current.clear();
                } else if (c != ' ' || !current.empty()) {
                    current += c;
                }
            }
        }
        return out;
    }

    NodeKind kindOf(const std::string& type) {
        if (type == "entry") return NodeKind::Entry;
        if (type == "line") return NodeKind::Line;
        if (type == "branch") return NodeKind::Branch;
        if (type == "set") return NodeKind::Set;
        if (type == "action") return NodeKind::Action;
        if (type == "jump") return NodeKind::Jump;
        if (type == "end") return NodeKind::End;
        return NodeKind::Unknown;
    }

    Value valueOf(const Json& json) {
        if (json.is_boolean()) return Value(json.get<bool>());
        if (json.is_number()) return Value(json.get<double>());
        if (json.is_string()) return Value(json.get<std::string>());
        return Value();
    }

    struct Builder {
        Program& program;

        StrRef add(std::string_view text) {
            StrRef ref;
            ref.offset = static_cast<uint32_t>(program.strings.size());
            ref.length = static_cast<uint32_t>(text.size());
            program.strings.append(text.data(), text.size());
            return ref;
        }

        void addTags(const Json& value, uint32_t& first, uint16_t& count) {
            const std::vector<std::string> list = tagList(value);
            first = static_cast<uint32_t>(program.tags.size());
            count = static_cast<uint16_t>(std::min<size_t>(list.size(), 0xffff));
            for (size_t i = 0; i < count; i++) program.tags.push_back(add(list[i]));
        }
    };

    struct CacheEntry {
        std::string path;
        std::string systemPath;
        std::shared_ptr<const Program> program;
    };

    struct Cache {
        std::vector<CacheEntry> entries;
        std::map<int, Programs::Listener> listeners;
        int nextListener = 1;
    };

    Cache& cache() {
        static Cache instance;
        return instance;
    }

    std::string normalized(const std::string& path) {
        return std::filesystem::path(FileData::getSystemPath(path)).lexically_normal().generic_string();
    }

    bool readText(const std::string& path, std::string& out) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) return false;
        out = data.readString();
        return true;
    }

    // ---- the cooked form: little-endian fixed-width fields, strings as a u32 length and bytes

    struct DialogueCookWriter {
        std::string out;

        void u8(uint8_t v) { out.push_back(static_cast<char>(v)); }
        void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(static_cast<uint8_t>(v >> (8 * i))); }
        void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
        void u64(uint64_t v) { for (int i = 0; i < 8; i++) u8(static_cast<uint8_t>(v >> (8 * i))); }
        void str(std::string_view text) { u32(static_cast<uint32_t>(text.size())); out.append(text.data(), text.size()); }
        void ref(StrRef r) { u32(r.offset); u32(r.length); }
        void expr(Expr e) { u32(e.first); u32(e.count); }
        void value(const Value& v) {
            u8(static_cast<uint8_t>(v.index()));
            if (v.isBool()) u8(v.asBool() ? 1 : 0);
            else if (v.isNumber()) { uint64_t bits; const double d = v.asNumber(); std::memcpy(&bits, &d, sizeof(bits)); u64(bits); }
            else if (v.isString()) str(v.asString());
        }
    };

    struct DialogueCookReader {
        const unsigned char* at;
        const unsigned char* end;
        bool ok = true;

        size_t left() const { return static_cast<size_t>(end - at); }
        bool take(size_t n) {
            if (!ok || left() < n) return ok = false;
            return true;
        }
        uint8_t u8() { if (!take(1)) return 0; return *at++; }
        uint16_t u16() { if (!take(2)) return 0; const uint16_t v = static_cast<uint16_t>(at[0] | at[1] << 8); at += 2; return v; }
        uint32_t u32() {
            if (!take(4)) return 0;
            const uint32_t v = static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 | static_cast<uint32_t>(at[3]) << 24;
            at += 4;
            return v;
        }
        int32_t i32() { return static_cast<int32_t>(u32()); }
        uint64_t u64() { const uint64_t low = u32(); return low | static_cast<uint64_t>(u32()) << 32; }
        void str(std::string& out) {
            const uint32_t n = u32();
            if (!take(n)) return;
            out.assign(reinterpret_cast<const char*>(at), n);
            at += n;
        }
        StrRef ref() { StrRef r; r.offset = u32(); r.length = u32(); return r; }
        Expr expr() { Expr e; e.first = u32(); e.count = u32(); return e; }
        Value value() {
            switch (u8()) {
                case 0: return Value();
                case 1: return Value(u8() != 0);
                case 2: { const uint64_t bits = u64(); double d; std::memcpy(&d, &bits, sizeof(d)); return Value(d); }
                case 3: { std::string text; str(text); return Value(std::move(text)); }
                default: ok = false; return Value();
            }
        }
        // a count of records of at least `minBytes` each: never more than the bytes left could hold
        uint32_t count(size_t minBytes) {
            const uint32_t n = u32();
            if (ok && static_cast<uint64_t>(n) * minBytes > left()) ok = false;
            return ok ? n : 0;
        }
    };

    // An expression the evaluator can run without reading outside its pool or its stack: operands
    // in range, jumps forward inside it, and every instruction finding the values it pops (a jump's
    // target sees the depth the fall-through brings)
    bool cookedExprValid(const ExprPool& pool, Expr expr) {
        if (static_cast<uint64_t>(expr.first) + expr.count > pool.code.size()) return false;
        std::vector<int64_t> arriving(static_cast<size_t>(expr.count) + 1, -1);
        int64_t depth = 0;
        for (uint32_t pc = 0; pc < expr.count; pc++) {
            if (arriving[pc] >= 0 && arriving[pc] != depth) return false;
            const Instr& instr = pool.code[expr.first + pc];
            auto needs = [&](int64_t n) { return depth >= n; };
            switch (instr.op) {
                case Op::Const:
                    if (instr.operand >= pool.constants.size()) return false;
                    depth++;
                    break;
                case Op::Var: case Op::Arg: case Op::Visited: case Op::Visits:
                    if (instr.operand >= pool.names.size()) return false;
                    depth++;
                    break;
                case Op::Random:
                    depth++;
                    break;
                case Op::Not: case Op::Neg: case Op::Truthy: case Op::Chance:
                    if (!needs(1)) return false;
                    break;
                case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
                    if (!needs(2)) return false;
                    depth--;
                    break;
                case Op::JumpIfFalse: case Op::JumpIfTrue:
                    if (!needs(1) || instr.operand <= pc || instr.operand > expr.count) return false;
                    if (arriving[instr.operand] >= 0 && arriving[instr.operand] != depth) return false;
                    arriving[instr.operand] = depth;
                    depth--;
                    break;
                case Op::CallCondition: case Op::CallAction:
                    if (instr.operand >= pool.calls.size() || !needs(instr.argc)) return false;
                    depth -= instr.argc;
                    if (instr.op == Op::CallCondition) depth++;
                    break;
                case Op::Assign: case Op::AssignAdd: case Op::AssignSub:
                    if (instr.operand >= pool.names.size() || !needs(1)) return false;
                    depth--;
                    break;
                default:
                    return false;
            }
        }
        return arriving[expr.count] < 0 || arriving[expr.count] == depth;
    }

}

std::shared_ptr<const Program> Program::compile(const graph::Graph& graph, const std::string& path, std::vector<graph::Issue>* issues) {
    auto program = std::make_shared<Program>();
    Builder builder{*program};
    program->path = path;
    if (auto guid = graph.passthrough.find("guid"); guid != graph.passthrough.end() && guid->is_string()) program->guid = guid->get<std::string>();
    program->ns = namespaceOf(graph, path);
    if (auto language = graph.metadata.find("source_language"); language != graph.metadata.end() && language->is_string()) {
        program->sourceLanguage = language->get<std::string>();
    }
    for (const graph::Variable& variable : graph.variables) program->defaults.emplace_back(variable.name, valueOf(variable.defaultValue));

    // an output port -> the node its first edge reaches (more than one is the lint's fan-out)
    std::map<std::pair<std::string, std::string>, std::string> targets;
    for (const graph::Edge& edge : graph.edges) targets.emplace(std::make_pair(edge.from.node, edge.from.port), edge.to.node);

    program->nodes.reserve(graph.nodes.size());
    for (const graph::Node& source : graph.nodes) {
        ProgramNode node;
        node.kind = kindOf(source.type);
        node.id = builder.add(source.id);
        program->nodes.push_back(node);
    }
    auto target = [&](const graph::Node& source, const std::string& port) -> int32_t {
        auto it = targets.find({source.id, port});
        return it == targets.end() ? -1 : program->findNode(it->second);
    };
    auto compileField = [&](const graph::Node& source, const std::string& text, ExprMode mode, Expr& out, const char* field) {
        ParseError error;
        if (compileExpr(text, mode, program->exprs, out, &error)) return;
        if (issues) {
            graph::Issue issue;
            issue.code = "bad-expression";
            issue.nodeId = source.id;
            issue.message = std::string(field) + " of " + source.id + ", column " + std::to_string(error.column) + ": " + error.message;
            issues->push_back(std::move(issue));
        }
    };

    for (size_t i = 0; i < graph.nodes.size(); i++) {
        const graph::Node& source = graph.nodes[i];
        ProgramNode& node = program->nodes[i];
        switch (node.kind) {
            case NodeKind::Entry:
                node.a = builder.add(stringParam(source, "name"));
                node.next = target(source, "next");
                program->entries.emplace_back(node.a, static_cast<int32_t>(i));
                break;
            case NodeKind::Line: {
                node.a = builder.add(stringParam(source, "speaker"));
                node.b = builder.add(stringParam(source, "mood"));
                node.text = builder.add(stringParam(source, "text"));
                node.voice = builder.add(assetParam(source, "voice"));
                const std::string mode = stringParam(source, "variant_mode");
                node.variantMode = mode == "sequence" ? VariantMode::Sequence : mode == "once" ? VariantMode::Once : VariantMode::Random;
                auto tags = source.params.find("tags");
                builder.addTags(tags != source.params.end() ? *tags : Json(), node.tagsFirst, node.tagsCount);
                compileField(source, stringParam(source, "action"), ExprMode::Statements, node.expr, "action");
                node.next = target(source, "next");
                node.variantsFirst = static_cast<uint32_t>(program->variants.size());
                for (const Json& item : arrayParam(source, "variants")) {
                    if (!item.is_object()) continue;
                    ProgramVariant variant;
                    variant.id = builder.add(item.value("id", std::string()));
                    variant.text = builder.add(item.value("text", std::string()));
                    program->variants.push_back(variant);
                    node.variantsCount++;
                }
                node.optionsFirst = static_cast<uint32_t>(program->options.size());
                for (const Json& item : arrayParam(source, "options")) {
                    if (!item.is_object()) continue;
                    ProgramOption option;
                    const std::string id = item.value("id", std::string());
                    option.id = builder.add(id);
                    option.text = builder.add(item.value("text", std::string()));
                    option.style = builder.add(item.value("style", std::string()));
                    option.once = item.value("once", false);
                    option.disable = item.value("when_false", std::string("hide")) == "disable";
                    compileField(source, item.value("condition", std::string()), ExprMode::Condition, option.condition, ("option " + id + " condition").c_str());
                    compileField(source, item.value("action", std::string()), ExprMode::Statements, option.action, ("option " + id + " action").c_str());
                    option.target = target(source, id);
                    auto optionTags = item.find("tags");
                    builder.addTags(optionTags != item.end() ? *optionTags : Json(), option.tagsFirst, option.tagsCount);
                    program->options.push_back(option);
                    node.optionsCount++;
                }
                break;
            }
            case NodeKind::Branch:
                compileField(source, stringParam(source, "condition"), ExprMode::Condition, node.expr, "condition");
                node.next = target(source, "true");
                node.alt = target(source, "false");
                break;
            case NodeKind::Set:
                compileField(source, stringParam(source, "assign"), ExprMode::Statements, node.expr, "assign");
                node.next = target(source, "next");
                break;
            case NodeKind::Action:
                compileField(source, stringParam(source, "action"), ExprMode::Statements, node.expr, "action");
                node.wait = boolParam(source, "wait");
                node.next = target(source, "next");
                break;
            case NodeKind::Jump: {
                node.a = builder.add(stringParam(source, "entry"));
                auto dialogue = source.params.find("dialogue");
                node.b = builder.add(dialogue != source.params.end() ? assetPath(*dialogue) : std::string());
                break;
            }
            case NodeKind::End:
                node.a = builder.add(stringParam(source, "result"));
                break;
            case NodeKind::Unknown:
                break;
        }
    }

    std::vector<std::string> used;
    for (const std::string& name : exprUse(program->exprs, Expr{0, static_cast<uint32_t>(program->exprs.code.size())}).variables) used.push_back(name);
    // message arguments name variables too ({gold} reads $gold)
    for (const SourceEntry& entry : sourceEntries(graph)) {
        size_t open = entry.text.find('{');
        while (open != std::string::npos) {
            size_t end = open + 1;
            while (end < entry.text.size() && (std::isalnum(static_cast<unsigned char>(entry.text[end])) || entry.text[end] == '_')) end++;
            if (end > open + 1) used.push_back(entry.text.substr(open + 1, end - open - 1));
            open = entry.text.find('{', end);
        }
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    program->variablesUsed = std::move(used);
    // what the program keeps for as long as it plays: no growth slack
    program->strings.shrink_to_fit();
    program->nodes.shrink_to_fit();
    program->options.shrink_to_fit();
    program->variants.shrink_to_fit();
    program->tags.shrink_to_fit();
    program->entries.shrink_to_fit();
    program->exprs.code.shrink_to_fit();
    program->exprs.constants.shrink_to_fit();
    program->exprs.names.shrink_to_fit();
    program->exprs.calls.shrink_to_fit();
    return program;
}

int Program::findNode(std::string_view id) const {
    // nodes are in file order: by prefix, then number (graph::idLess)
    auto it = std::lower_bound(nodes.begin(), nodes.end(), id, [this](const ProgramNode& node, std::string_view wanted) {
        return graph::idLess(str(node.id), wanted);
    });
    if (it == nodes.end() || str(it->id) != id) return -1;
    return static_cast<int>(it - nodes.begin());
}

int Program::findEntry(std::string_view name) const {
    for (const auto& [entryName, index] : entries) {
        if (str(entryName) == name) return index;
    }
    return -1;
}

int Program::defaultEntry() const {
    const int start = findEntry("start");
    if (start >= 0) return start;
    return entries.empty() ? -1 : entries.front().second;
}

int Program::findTarget(std::string_view entryOrNode) const {
    if (entryOrNode.empty()) return defaultEntry();
    const int entry = findEntry(entryOrNode);
    return entry >= 0 ? entry : findNode(entryOrNode);
}

const Value* Program::defaultOf(std::string_view name) const {
    for (const auto& [variable, value] : defaults) {
        if (variable == name) return &value;
    }
    return nullptr;
}

bool Program::usesVariable(std::string_view name) const {
    return std::binary_search(variablesUsed.begin(), variablesUsed.end(), name, [](const auto& a, const auto& b) {
        return std::string_view(a) < std::string_view(b);
    });
}

size_t Program::memoryBytes() const {
    return sizeof(Program) + strings.capacity() + nodes.capacity() * sizeof(ProgramNode) + options.capacity() * sizeof(ProgramOption) +
           variants.capacity() * sizeof(ProgramVariant) + tags.capacity() * sizeof(StrRef) + entries.capacity() * sizeof(entries[0]) +
           exprs.memoryBytes() + path.capacity() + guid.capacity() + ns.capacity();
}

size_t Program::textBytes() const {
    size_t bytes = 0;
    for (const ProgramNode& node : nodes) bytes += node.text.length;
    for (const ProgramVariant& variant : variants) bytes += variant.text.length;
    for (const ProgramOption& option : options) bytes += option.text.length;
    return bytes;
}

// ------------------------------------------------------------------ cooked

std::string Program::cook() const {
    DialogueCookWriter w;
    w.out.reserve(64 + strings.size() + nodes.size() * 64 + options.size() * 52 + exprs.code.size() * 6);
    w.out.append(COOKED_MAGIC, sizeof(COOKED_MAGIC));
    w.u32(COOKED_VERSION);
    w.str(guid);
    w.str(ns);
    w.str(sourceLanguage);
    w.str(strings);
    w.u32(static_cast<uint32_t>(nodes.size()));
    for (const ProgramNode& node : nodes) {
        w.u8(static_cast<uint8_t>(node.kind));
        w.u8(static_cast<uint8_t>(node.variantMode));
        w.u8(node.wait ? 1 : 0);
        w.i32(node.next);
        w.i32(node.alt);
        w.ref(node.id);
        w.ref(node.a);
        w.ref(node.b);
        w.ref(node.text);
        w.ref(node.voice);
        w.expr(node.expr);
        w.u32(node.variantsFirst);
        w.u16(node.variantsCount);
        w.u32(node.optionsFirst);
        w.u16(node.optionsCount);
        w.u32(node.tagsFirst);
        w.u16(node.tagsCount);
    }
    w.u32(static_cast<uint32_t>(options.size()));
    for (const ProgramOption& option : options) {
        w.ref(option.id);
        w.ref(option.text);
        w.ref(option.style);
        w.expr(option.condition);
        w.expr(option.action);
        w.i32(option.target);
        w.u32(option.tagsFirst);
        w.u16(option.tagsCount);
        w.u8(option.once ? 1 : 0);
        w.u8(option.disable ? 1 : 0);
    }
    w.u32(static_cast<uint32_t>(variants.size()));
    for (const ProgramVariant& variant : variants) {
        w.ref(variant.id);
        w.ref(variant.text);
    }
    w.u32(static_cast<uint32_t>(tags.size()));
    for (const StrRef& tag : tags) w.ref(tag);
    w.u32(static_cast<uint32_t>(entries.size()));
    for (const auto& [name, index] : entries) {
        w.ref(name);
        w.i32(index);
    }
    w.u32(static_cast<uint32_t>(defaults.size()));
    for (const auto& [name, value] : defaults) {
        w.str(name);
        w.value(value);
    }
    w.u32(static_cast<uint32_t>(variablesUsed.size()));
    for (const std::string& name : variablesUsed) w.str(name);
    w.u32(static_cast<uint32_t>(exprs.code.size()));
    for (const Instr& instr : exprs.code) {
        w.u8(static_cast<uint8_t>(instr.op));
        w.u8(instr.argc);
        w.u32(instr.operand);
    }
    w.u32(static_cast<uint32_t>(exprs.constants.size()));
    for (const Value& value : exprs.constants) w.value(value);
    w.u32(static_cast<uint32_t>(exprs.names.size()));
    for (const std::string& name : exprs.names) w.str(name);
    w.u32(static_cast<uint32_t>(exprs.calls.size()));
    for (const CallSite& call : exprs.calls) {
        w.str(call.prefix);
        w.str(call.verb);
    }
    return std::move(w.out);
}

bool Program::isCooked(std::string_view bytes) {
    return bytes.size() >= sizeof(COOKED_MAGIC) && std::memcmp(bytes.data(), COOKED_MAGIC, sizeof(COOKED_MAGIC)) == 0;
}

std::shared_ptr<const Program> Program::fromCooked(std::string_view bytes, const std::string& path, std::string* error) {
    auto refuse = [&](const std::string& why) -> std::shared_ptr<const Program> {
        if (error) *error = path + " is not a cooked dialogue: " + why;
        return nullptr;
    };
    if (!isCooked(bytes)) return refuse("no cooked header");
    DialogueCookReader r{reinterpret_cast<const unsigned char*>(bytes.data()) + sizeof(COOKED_MAGIC),
                         reinterpret_cast<const unsigned char*>(bytes.data()) + bytes.size()};
    const uint32_t version = r.u32();
    if (r.ok && version != COOKED_VERSION) {
        return refuse("version " + std::to_string(version) + ", this game reads " + std::to_string(COOKED_VERSION) + " (export the game again)");
    }
    auto program = std::make_shared<Program>();
    program->path = path;
    r.str(program->guid);
    r.str(program->ns);
    r.str(program->sourceLanguage);
    r.str(program->strings);

    program->nodes.resize(r.count(77));
    for (ProgramNode& node : program->nodes) {
        node.kind = static_cast<NodeKind>(r.u8());
        node.variantMode = static_cast<VariantMode>(r.u8());
        node.wait = r.u8() != 0;
        node.next = r.i32();
        node.alt = r.i32();
        node.id = r.ref();
        node.a = r.ref();
        node.b = r.ref();
        node.text = r.ref();
        node.voice = r.ref();
        node.expr = r.expr();
        node.variantsFirst = r.u32();
        node.variantsCount = r.u16();
        node.optionsFirst = r.u32();
        node.optionsCount = r.u16();
        node.tagsFirst = r.u32();
        node.tagsCount = r.u16();
    }
    program->options.resize(r.count(52));
    for (ProgramOption& option : program->options) {
        option.id = r.ref();
        option.text = r.ref();
        option.style = r.ref();
        option.condition = r.expr();
        option.action = r.expr();
        option.target = r.i32();
        option.tagsFirst = r.u32();
        option.tagsCount = r.u16();
        option.once = r.u8() != 0;
        option.disable = r.u8() != 0;
    }
    program->variants.resize(r.count(16));
    for (ProgramVariant& variant : program->variants) {
        variant.id = r.ref();
        variant.text = r.ref();
    }
    program->tags.resize(r.count(8));
    for (StrRef& tag : program->tags) tag = r.ref();
    program->entries.resize(r.count(12));
    for (auto& [name, index] : program->entries) {
        name = r.ref();
        index = r.i32();
    }
    program->defaults.resize(r.count(5));
    for (auto& [name, value] : program->defaults) {
        r.str(name);
        value = r.value();
    }
    program->variablesUsed.resize(r.count(4));
    for (std::string& name : program->variablesUsed) r.str(name);
    ExprPool& pool = program->exprs;
    pool.code.resize(r.count(6));
    for (Instr& instr : pool.code) {
        instr.op = static_cast<Op>(r.u8());
        instr.argc = r.u8();
        instr.operand = r.u32();
    }
    pool.constants.resize(r.count(1));
    for (Value& value : pool.constants) value = r.value();
    pool.names.resize(r.count(4));
    for (std::string& name : pool.names) r.str(name);
    pool.calls.resize(r.count(8));
    for (CallSite& call : pool.calls) {
        r.str(call.prefix);
        r.str(call.verb);
        call.name = call.prefix + "." + call.verb;
    }
    if (!r.ok) return refuse("it ends early or a count is too large");
    if (r.left() != 0) return refuse(std::to_string(r.left()) + " bytes after its end");

    // everything the runner indexes with, checked once here so it never reads outside the program
    const size_t stringBytes = program->strings.size();
    auto refOk = [stringBytes](StrRef ref) { return static_cast<uint64_t>(ref.offset) + ref.length <= stringBytes; };
    const int64_t nodeCount = static_cast<int64_t>(program->nodes.size());
    auto targetOk = [nodeCount](int32_t target) { return target >= -1 && target < nodeCount; };
    auto rangeOk = [](uint32_t first, uint32_t count, size_t size) { return static_cast<uint64_t>(first) + count <= size; };
    for (const ProgramNode& node : program->nodes) {
        if (node.kind > NodeKind::Unknown || node.variantMode > VariantMode::Once || !targetOk(node.next) || !targetOk(node.alt) ||
            !refOk(node.id) || !refOk(node.a) || !refOk(node.b) || !refOk(node.text) || !refOk(node.voice) || !cookedExprValid(pool, node.expr) ||
            !rangeOk(node.variantsFirst, node.variantsCount, program->variants.size()) ||
            !rangeOk(node.optionsFirst, node.optionsCount, program->options.size()) || !rangeOk(node.tagsFirst, node.tagsCount, program->tags.size())) {
            return refuse("a node is inconsistent");
        }
    }
    for (const ProgramOption& option : program->options) {
        if (!refOk(option.id) || !refOk(option.text) || !refOk(option.style) || !targetOk(option.target) || !cookedExprValid(pool, option.condition) ||
            !cookedExprValid(pool, option.action) || !rangeOk(option.tagsFirst, option.tagsCount, program->tags.size())) {
            return refuse("an option is inconsistent");
        }
    }
    for (const ProgramVariant& variant : program->variants) {
        if (!refOk(variant.id) || !refOk(variant.text)) return refuse("a variant is inconsistent");
    }
    for (const StrRef& tag : program->tags) {
        if (!refOk(tag)) return refuse("a tag is inconsistent");
    }
    for (const auto& [name, index] : program->entries) {
        if (!refOk(name) || index < 0 || index >= nodeCount) return refuse("an entry is inconsistent");
    }
    return program;
}

// ------------------------------------------------------------------ keys

std::vector<SourceEntry> sourceEntries(const graph::Graph& graph) {
    std::vector<SourceEntry> out;
    for (const graph::Node& node : graph.nodes) {
        if (node.type != "line") continue;
        const std::string text = stringParam(node, "text");
        if (!text.empty()) out.push_back({node.id, text, node.id});
        for (const Json& item : arrayParam(node, "variants")) {
            if (!item.is_object() || item.value("id", std::string()).empty() || item.value("text", std::string()).empty()) continue;
            out.push_back({node.id + "." + item.value("id", std::string()), item.value("text", std::string()), node.id});
        }
        for (const Json& item : arrayParam(node, "options")) {
            if (!item.is_object() || item.value("id", std::string()).empty() || item.value("text", std::string()).empty()) continue;
            out.push_back({node.id + "." + item.value("id", std::string()), item.value("text", std::string()), node.id});
        }
    }
    return out;
}

std::string namespaceFromStem(std::string_view stem) {
    std::string out;
    for (char c : stem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 0x80) out += static_cast<char>(std::tolower(u));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 48) out.resize(48);
    return out.empty() ? "dialogue" : out;
}

std::string namespaceOf(const graph::Graph& graph, std::string_view path) {
    if (auto ns = graph.metadata.find("namespace"); ns != graph.metadata.end() && ns->is_string() && !ns->get<std::string>().empty()) {
        return ns->get<std::string>();
    }
    return namespaceFromStem(std::filesystem::path(std::string(path)).stem().string());
}

// ------------------------------------------------------------------ cache

std::shared_ptr<const Program> Programs::fromText(const std::string& path, std::string_view text, std::string* error) {
    // an exported game's file (no JSON starts with the magic)
    if (Program::isCooked(text)) return Program::fromCooked(text, path, error);
    graph::LoadReport report;
    const graph::Graph graph = graph::load(text, report);
    if (!report.ok) {
        if (error) *error = path + " is not a dialogue: " + report.error;
        return nullptr;
    }
    if (graph.kind != KIND_ID) {
        if (error) *error = path + " is a '" + graph.kind + "' graph, not a dialogue";
        return nullptr;
    }
    return Program::compile(graph, path);
}

std::shared_ptr<const Program> Programs::load(const std::string& path, std::string* error) {
    Cache& c = cache();
    for (const CacheEntry& entry : c.entries) {
        if (entry.path == path) return entry.program;
    }
    std::string text;
    if (!readText(path, text)) {
        if (error) *error = "cannot read " + path;
        return nullptr;
    }
    std::shared_ptr<const Program> program = fromText(path, text, error);
    if (!program) return nullptr;
    c.entries.push_back({path, normalized(path), program});
    return program;
}

bool Programs::fileChanged(const std::string& path) {
    Cache& c = cache();
    const std::string changed = normalized(path);
    bool any = false;
    for (CacheEntry& entry : c.entries) {
        if (entry.systemPath != changed) continue;
        any = true;
        std::string text, error;
        std::shared_ptr<const Program> program = readText(entry.path, text) ? fromText(entry.path, text, &error) : nullptr;
        if (!program) {
            Log::warn("Dialogue: %s did not reload (%s); the running conversations keep the previous version", entry.path.c_str(),
                      error.empty() ? "it cannot be read" : error.c_str());
            continue;
        }
        entry.program = program;
        // a listener may add or remove listeners
        std::vector<Listener> calls;
        for (const auto& listener : c.listeners) calls.push_back(listener.second);
        for (const Listener& call : calls) call(program);
    }
    return any;
}

int Programs::addListener(Listener listener) {
    Cache& c = cache();
    const int id = c.nextListener++;
    c.listeners[id] = std::move(listener);
    return id;
}

void Programs::removeListener(int id) {
    cache().listeners.erase(id);
}

void Programs::clear() {
    cache().entries.clear();
}

}  // namespace jasperengine::dialogue
