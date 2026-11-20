// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueProgram.h"

#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

using namespace jasperengine;
using namespace jasperengine::dialogue;

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

// ------------------------------------------------------------------ keys

std::vector<SourceEntry> jasperengine::dialogue::sourceEntries(const graph::Graph& graph) {
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

std::string jasperengine::dialogue::namespaceFromStem(std::string_view stem) {
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

std::string jasperengine::dialogue::namespaceOf(const graph::Graph& graph, std::string_view path) {
    if (auto ns = graph.metadata.find("namespace"); ns != graph.metadata.end() && ns->is_string() && !ns->get<std::string>().empty()) {
        return ns->get<std::string>();
    }
    return namespaceFromStem(std::filesystem::path(std::string(path)).stem().string());
}

// ------------------------------------------------------------------ cache

std::shared_ptr<const Program> Programs::fromText(const std::string& path, std::string_view text, std::string* error) {
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
