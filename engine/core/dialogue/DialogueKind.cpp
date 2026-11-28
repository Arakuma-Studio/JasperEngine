// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueKind.h"

#include "dialogue/DialogueExpression.h"
#include "dialogue/DialogueProgram.h"
#include "io/Data.h"
#include "localization/Localization.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableRuntime.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

using namespace jasperengine;

namespace jasperengine::dialogue {

namespace {

    using graph::Json;
    using graph::Port;
    using graph::PortDirection;

    Port flowPort(const char* id, PortDirection direction, const std::string& name) {
        Port port;
        port.id = id;
        port.direction = direction;
        port.type = "flow";
        port.name = name;
        port.optional = true;
        port.multi = direction == PortDirection::In;
        return port;
    }

    graph::ParamDesc param(const char* name, const char* kind, Json value, std::vector<std::string> values = {}) {
        graph::ParamDesc desc;
        desc.name = name;
        desc.kind = kind;
        desc.defaultValue = std::move(value);
        desc.enumValues = std::move(values);
        return desc;
    }

    graph::NodeTypeDesc nodeType(const char* id, const char* name, const char* category, std::vector<Port> ports,
                                 std::vector<graph::ParamDesc> params, std::vector<std::string> aliases = {}) {
        graph::NodeTypeDesc type;
        type.id = id;
        type.displayName = name;
        type.category = category;
        type.ports = std::move(ports);
        type.params = std::move(params);
        type.aliases = std::move(aliases);
        return type;
    }

    std::shared_ptr<const LintContext>& contextSlot() {
        static std::shared_ptr<const LintContext> context;
        return context;
    }

    std::function<std::shared_ptr<const LintContext>()>& contextProvider() {
        static std::function<std::shared_ptr<const LintContext>()> provider;
        return provider;
    }

    std::mutex& contextMutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::string stringParam(const graph::Node& node, const char* name) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    const Json& arrayParam(const graph::Node& node, const char* name) {
        static const Json empty = Json::array();
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_array() ? *it : empty;
    }

    std::string assetPathOf(const graph::Node& node, const char* name) {
        auto it = node.params.find(name);
        if (it == node.params.end()) return {};
        if (it->is_string()) return it->get<std::string>();
        if (it->is_object()) return it->value("path", std::string());
        return {};
    }

    std::string truncated(const std::string& text, size_t limit) {
        if (text.size() <= limit) return text;
        size_t cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
        return text.substr(0, cut) + "...";
    }

    std::vector<std::string> splitNames(const std::string& text) {
        std::vector<std::string> out;
        std::string current;
        for (char c : text + ",") {
            if (c == ',' || c == ' ' || c == ';') {
                if (!current.empty()) out.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        return out;
    }

    std::shared_ptr<const l10n::StringTable> readDialogueTable(const std::string& path, const std::string& language, std::string* error) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
            if (error) *error = "cannot read " + path;
            return nullptr;
        }
        graph::LoadReport report;
        const graph::Graph graph = graph::load(data.readString(), report);
        if (!report.ok || graph.kind != KIND_ID) {
            if (error) *error = path + " is not a dialogue" + (report.error.empty() ? std::string() : ": " + report.error);
            return nullptr;
        }
        std::vector<l10n::StringTable::Entry> entries;
        for (const SourceEntry& entry : sourceEntries(graph)) entries.push_back({entry.key, entry.text, false});
        return l10n::StringTable::fromEntries(language, entries);
    }

    // one field's expression, as the runner compiles it
    struct ExprField {
        std::string field;
        std::string source;
        ExprMode mode;
    };

    std::vector<ExprField> exprFields(const graph::Node& node) {
        std::vector<ExprField> out;
        if (node.type == "branch") out.push_back({"condition", stringParam(node, "condition"), ExprMode::Condition});
        else if (node.type == "set") out.push_back({"assign", stringParam(node, "assign"), ExprMode::Statements});
        else if (node.type == "action") out.push_back({"action", stringParam(node, "action"), ExprMode::Statements});
        else if (node.type == "line") {
            out.push_back({"action", stringParam(node, "action"), ExprMode::Statements});
            for (const Json& option : arrayParam(node, "options")) {
                if (!option.is_object()) continue;
                const std::string id = option.value("id", std::string());
                out.push_back({"option " + id + " condition", option.value("condition", std::string()), ExprMode::Condition});
                out.push_back({"option " + id + " action", option.value("action", std::string()), ExprMode::Statements});
            }
        }
        return out;
    }

    // the texts of a line: (where, text)
    std::vector<std::pair<std::string, std::string>> lineTexts(const graph::Node& node) {
        std::vector<std::pair<std::string, std::string>> out;
        out.emplace_back("text", stringParam(node, "text"));
        for (const Json& variant : arrayParam(node, "variants")) {
            if (variant.is_object()) out.emplace_back("variant " + variant.value("id", std::string()), variant.value("text", std::string()));
        }
        for (const Json& option : arrayParam(node, "options")) {
            if (option.is_object()) out.emplace_back("option " + option.value("id", std::string()), option.value("text", std::string()));
        }
        return out;
    }

}

std::vector<Port> linePorts(const Json& params) {
    std::vector<Port> ports = {flowPort("in", PortDirection::In, "In"), flowPort("next", PortDirection::Out, "Next")};
    auto options = params.find("options");
    if (options == params.end() || !options->is_array()) return ports;
    for (const Json& option : *options) {
        if (!option.is_object()) continue;
        const std::string id = option.value("id", std::string());
        if (id.empty()) continue;
        const std::string text = option.value("text", std::string());
        Port port = flowPort("", PortDirection::Out, text.empty() ? "Option " + id : truncated(text, 24));
        port.id = id;
        ports.push_back(std::move(port));
    }
    return ports;
}

graph::GraphKindDesc makeDialogueKind() {
    graph::GraphKindDesc kind;
    kind.id = KIND_ID;
    kind.displayName = "Dialogue";
    kind.fileExtension = FILE_EXTENSION;
    kind.kindVersion = 1;
    kind.acyclic = false;
    kind.strictParams = true;
    kind.portTypes = {{"flow", 0xd8d8d8ffu, {}}};
    const Port in = flowPort("in", PortDirection::In, "In");
    const Port next = flowPort("next", PortDirection::Out, "Next");

    graph::NodeTypeDesc line = nodeType("line", "Line", "Dialogue", {in, next},
        {param("speaker", "string", ""), param("text", "string", ""), param("variants", "any", Json::array()),
         param("variant_mode", "enum", "random", {"random", "sequence", "once"}), param("mood", "string", ""),
         param("tags", "any", Json::array()), param("voice", "asset", Json()), param("options", "any", Json::array()),
         param("action", "string", ""), param("max_chars", "int", 0), param("next_sub_id", "int", 1)},
        {"say", "text", "choice", "menu", "option"});
    line.params[9].min = 0.0;
    line.params[10].min = 1.0;
    line.dynamicPorts = [](const Json& params) { return linePorts(params); };

    kind.nodeTypes = {
        nodeType("entry", "Entry", "Flow", {next}, {param("name", "string", "start"), param("arguments", "string", "")}, {"start"}),
        line,
        nodeType("branch", "Branch", "Logic", {in, flowPort("true", PortDirection::Out, "True"), flowPort("false", PortDirection::Out, "False")},
                 {param("condition", "string", "")}, {"if", "condition"}),
        nodeType("set", "Set", "Logic", {in, next}, {param("assign", "string", "")}, {"variable", "assign"}),
        nodeType("action", "Action", "Logic", {in, next}, {param("action", "string", ""), param("wait", "bool", false)}, {"command", "call"}),
        nodeType("jump", "Jump", "Flow", {in}, {param("dialogue", "asset", Json()), param("entry", "string", "start")}, {"goto"}),
        nodeType("end", "End", "Flow", {in}, {param("result", "string", "")}, {"exit", "finish"}),
    };
    kind.validate = [](const graph::Graph& graph, std::vector<graph::Issue>& issues) { lint(graph, issues); };
    return kind;
}

void registerDialogueKind() {
    static std::once_flag once;
    std::call_once(once, [] { graph::GraphRegistry::get().registerKind(makeDialogueKind()); });
    // every time: Localization::resetForTesting drops the readers
    Localization::registerReader(FILE_EXTENSION, readDialogueTable);
}

void setLintContext(std::shared_ptr<const LintContext> context) {
    std::lock_guard<std::mutex> lock(contextMutex());
    contextSlot() = std::move(context);
}

void setLintContextProvider(std::function<std::shared_ptr<const LintContext>()> provider) {
    std::lock_guard<std::mutex> lock(contextMutex());
    contextProvider() = std::move(provider);
}

std::shared_ptr<const LintContext> lintContext() {
    std::function<std::shared_ptr<const LintContext>()> provider;
    {
        std::lock_guard<std::mutex> lock(contextMutex());
        if (contextSlot() || !contextProvider()) return contextSlot();
        provider = contextProvider();
    }
    return provider();
}

namespace {

    // a small reader of ICU message syntax: the argument names, sub-messages of plural and select included
    struct MessageScan {
        const std::string& text;
        size_t i = 0;
        std::vector<std::string> names;
        int depth = 0;

        void add(const std::string& name) {
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        void spaces() {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')) i++;
        }
        std::string word() {
            const size_t begin = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) i++;
            return text.substr(begin, i - begin);
        }
        // a message up to its closing '}' (consumed) or the end
        void message() {
            if (++depth > 8) return;
            while (i < text.size()) {
                const char c = text[i];
                if (c == '\'') {
                    // '' is an apostrophe; a quote before a syntax character starts a literal run to the next quote
                    if (i + 1 < text.size() && text[i + 1] == '\'') { i += 2; continue; }
                    if (i + 1 < text.size() && (text[i + 1] == '{' || text[i + 1] == '}' || text[i + 1] == '#')) {
                        const size_t close = text.find('\'', i + 1);
                        i = close == std::string::npos ? text.size() : close + 1;
                        continue;
                    }
                    i++;
                    continue;
                }
                if (c == '{') { i++; argument(); continue; }
                if (c == '}') { i++; depth--; return; }
                i++;
            }
            depth--;
        }
        void argument() {
            spaces();
            const std::string name = word();
            spaces();
            if (i >= text.size()) return;
            if (text[i] == '}') { i++; add(name); return; }
            if (text[i] != ',') { skipTo(); return; }
            i++;
            add(name);
            spaces();
            const std::string type = word();
            spaces();
            if (i < text.size() && text[i] == '}') { i++; return; }
            if (i >= text.size() || text[i] != ',' || (type != "plural" && type != "select" && type != "selectordinal")) { skipTo(); return; }
            i++;
            // selector {message} ... }
            while (i < text.size()) {
                spaces();
                if (i >= text.size()) return;
                if (text[i] == '}') { i++; return; }
                while (i < text.size() && text[i] != '{' && text[i] != '}' && text[i] != ' ') i++;
                spaces();
                if (i < text.size() && text[i] == '{') { i++; message(); }
            }
        }
        // past the '}' that closes this argument
        void skipTo() {
            int level = 1;
            while (i < text.size() && level > 0) {
                if (text[i] == '{') level++;
                else if (text[i] == '}') level--;
                i++;
            }
        }
    };

}

std::vector<std::string> messageArguments(const std::string& message) {
    MessageScan scan{message};
    while (scan.i < message.size()) scan.message();
    return scan.names;
}

void lint(const graph::Graph& graph, std::vector<graph::Issue>& issues) {
    const std::shared_ptr<const LintContext> context = lintContext();
    auto add = [&](graph::Severity severity, const char* code, const std::string& node, const std::string& message, const std::string& port = {},
                   const std::string& edge = {}, const std::string& suggestion = {}) {
        graph::Issue issue;
        issue.severity = severity;
        issue.code = code;
        issue.nodeId = node;
        issue.portId = port;
        issue.edgeId = edge;
        issue.message = message;
        issue.suggestion = suggestion;
        issues.push_back(std::move(issue));
    };
    using graph::Severity;

    std::map<std::string, const graph::Node*> nodes;
    for (const graph::Node& node : graph.nodes) nodes[node.id] = &node;
    // outputs: (node, port) -> the edges leaving it, in id order
    std::map<std::pair<std::string, std::string>, std::vector<const graph::Edge*>> outgoing;
    for (const graph::Edge& edge : graph.edges) outgoing[{edge.from.node, edge.from.port}].push_back(&edge);

    // entries
    std::map<std::string, std::string> entryNames;
    std::set<std::string> entryArguments;
    bool anyEntry = false;
    for (const graph::Node& node : graph.nodes) {
        if (node.type != "entry") continue;
        anyEntry = true;
        const std::string name = stringParam(node, "name");
        auto [it, inserted] = entryNames.emplace(name, node.id);
        if (!inserted) add(Severity::Error, "duplicate-entry", node.id, "entry '" + name + "' is also " + it->second + ": entry names are unique in a file");
        for (const std::string& argument : splitNames(stringParam(node, "arguments"))) entryArguments.insert(argument);
    }
    if (!anyEntry) add(Severity::Error, "no-entry", "", "the dialogue has no entry node: add one (start(name) begins there)");

    // fan-out
    for (const auto& [port, edges] : outgoing) {
        if (edges.size() < 2 || !nodes.count(port.first)) continue;
        add(Severity::Error, "fan-out", port.first, "output '" + port.second + "' of " + port.first + " has " + std::to_string(edges.size()) +
            " wires: a flow output leads to one node", port.second, edges[1]->id, "remove the extra wire");
    }

    // expressions, prefixes, variables
    std::set<std::string> declared;
    for (const graph::Variable& variable : graph.variables) declared.insert(variable.name);
    if (context) declared.insert(context->variables.begin(), context->variables.end());
    for (const graph::Node& node : graph.nodes) {
        std::set<std::string> warnedVariables;
        for (const ExprField& field : exprFields(node)) {
            if (field.source.empty()) continue;
            ExprPool pool;
            Expr expr;
            ParseError error;
            if (!compileExpr(field.source, field.mode, pool, expr, &error)) {
                add(Severity::Error, "bad-expression", node.id, field.field + " of " + node.id + ", column " + std::to_string(error.column) + ": " + error.message);
                continue;
            }
            const ExprUse use = exprUse(pool, expr);
            if (context && !context->actionPrefixes.empty()) {
                for (const std::string& call : use.actionCalls) {
                    const std::string prefix = call.substr(0, call.find('.'));
                    if (!context->actionPrefixes.count(prefix)) {
                        add(Severity::Warning, "unknown-action-prefix", node.id, field.field + " of " + node.id + " calls '" + call +
                            "': its prefix '" + prefix + "' is not declared in Project Settings > Dialogue > Action Prefixes", {}, {}, "declare prefix " + prefix);
                    }
                }
            }
            if (context && !context->conditionPrefixes.empty()) {
                for (const std::string& call : use.conditionCalls) {
                    const std::string prefix = call.substr(0, call.find('.'));
                    if (!context->conditionPrefixes.count(prefix)) {
                        add(Severity::Warning, "unknown-condition-prefix", node.id, field.field + " of " + node.id + " asks '" + call +
                            "': its prefix '" + prefix + "' is not declared in Project Settings > Dialogue > Condition Prefixes", {}, {}, "declare prefix " + prefix);
                    }
                }
            }
            for (const std::string& variable : use.variables) {
                if (declared.count(variable) || !warnedVariables.insert(variable).second) continue;
                add(Severity::Warning, "unknown-variable", node.id, "$" + variable + " (" + field.field + " of " + node.id +
                    ") is not declared in the dialogue's variables or Project Settings > Dialogue", {}, {}, "declare variable " + variable);
            }
            for (const std::string& target : use.visitedNodes) {
                if (!nodes.count(target)) add(Severity::Error, "bad-expression", node.id, field.field + " of " + node.id + " asks visits of '" + target + "', which is no node of this dialogue");
            }
        }
    }

    // texts
    for (const graph::Node& node : graph.nodes) {
        if (node.type != "line") continue;
        bool anyText = false;
        std::map<std::string, std::string> optionTexts;
        for (const auto& [where, text] : lineTexts(node)) {
            if (text.empty()) continue;
            anyText = true;
            l10n::MessageError error;
            if (!l10n::MessageFormat::validate(text, &error)) {
                add(Severity::Error, "bad-message", node.id, where + " of " + node.id + ", column " + std::to_string(l10n::MessageFormat::column(text, error.offset)) +
                    ": " + error.message);
                continue;
            }
            for (const std::string& argument : messageArguments(text)) {
                if (declared.count(argument) || entryArguments.count(argument)) continue;
                add(Severity::Warning, "unknown-argument", node.id, "{" + argument + "} in the " + where + " of " + node.id +
                    " names no declared variable or entry argument (start(entry, {" + argument + " = ...}) passes one)", {}, {}, "declare variable " + argument);
            }
            if (where.rfind("option ", 0) == 0) {
                auto [it, inserted] = optionTexts.emplace(text, where);
                if (!inserted) add(Severity::Info, "duplicate-option-text", node.id, where + " of " + node.id + " reads the same as " + it->second + ": \"" + truncated(text, 40) + "\"");
            }
        }
        if (!anyText) add(Severity::Warning, "empty-line", node.id, node.id + " has no text and no options: it shows nothing");
    }

    // jumps
    std::set<std::string> roots;
    std::map<std::string, std::vector<std::string>> successors;
    for (const graph::Edge& edge : graph.edges) {
        if (nodes.count(edge.from.node) && nodes.count(edge.to.node)) successors[edge.from.node].push_back(edge.to.node);
    }
    auto localTarget = [&](const std::string& target) -> std::string {
        auto entry = entryNames.find(target.empty() ? "start" : target);
        if (entry != entryNames.end()) return entry->second;
        if (target.empty() && !entryNames.empty()) {
            std::string lowest;
            for (const auto& name : entryNames) {
                if (lowest.empty() || graph::idLess(name.second, lowest)) lowest = name.second;
            }
            return lowest;
        }
        return nodes.count(target) ? target : std::string();
    };
    for (const graph::Node& node : graph.nodes) {
        if (node.type == "entry") roots.insert(node.id);
        if (node.type != "jump") continue;
        const std::string path = assetPathOf(node, "dialogue");
        const std::string target = stringParam(node, "entry");
        if (path.empty()) {
            const std::string local = localTarget(target);
            if (local.empty()) add(Severity::Error, "jump-target", node.id, node.id + " jumps to '" + target + "', which is no entry or node of this dialogue");
            else successors[node.id].push_back(local);
        } else if (context && context->jumpTarget) {
            const std::string why = context->jumpTarget(path, target);
            if (!why.empty()) add(Severity::Error, "jump-target", node.id, node.id + " jumps to " + path + " '" + target + "': " + why);
        }
    }
    if (context && context->jumpedInto) {
        auto guid = graph.passthrough.find("guid");
        if (guid != graph.passthrough.end() && guid->is_string()) {
            for (const std::string& target : context->jumpedInto(guid->get<std::string>())) {
                const std::string local = localTarget(target);
                if (!local.empty()) roots.insert(local);
            }
        }
    }

    // unreachable
    std::set<std::string> reached;
    std::vector<std::string> work(roots.begin(), roots.end());
    while (!work.empty()) {
        const std::string id = work.back();
        work.pop_back();
        if (!reached.insert(id).second) continue;
        for (const std::string& next : successors[id]) work.push_back(next);
    }
    for (const graph::Node& node : graph.nodes) {
        if (!reached.count(node.id) && node.type != "entry") {
            add(Severity::Warning, "unreachable", node.id, node.id + " cannot be reached from any entry (nor from a jump of another dialogue)");
        }
    }

    // no-exit: what can reach an end, a jump elsewhere or an unconnected output
    auto exits = [&](const graph::Node& node) {
        if (node.type == "end" || node.type == "jump") return true;
        if (node.type == "entry" || node.type == "set" || node.type == "action") return !outgoing.count({node.id, "next"});
        if (node.type == "branch") return !outgoing.count({node.id, "true"}) || !outgoing.count({node.id, "false"});
        if (node.type == "line") {
            bool allConditional = true;
            bool anyOption = false;
            for (const Json& option : arrayParam(node, "options")) {
                if (!option.is_object() || option.value("id", std::string()).empty()) continue;
                anyOption = true;
                if (!outgoing.count({node.id, option.value("id", std::string())})) return true;
                // an option may be hidden: a condition that hides it, or once
                const bool mayHide = option.value("once", false) ||
                                     (!option.value("condition", std::string()).empty() && option.value("when_false", std::string("hide")) != "disable");
                if (!mayHide) allConditional = false;
            }
            // `next` is taken when there are no options, or when conditions may hide all of them
            return !outgoing.count({node.id, "next"}) && (!anyOption || allConditional);
        }
        return true;
    };
    std::map<std::string, std::vector<std::string>> predecessors;
    for (const auto& [from, list] : successors) {
        for (const std::string& to : list) predecessors[to].push_back(from);
    }
    std::set<std::string> escapes;
    for (const graph::Node& node : graph.nodes) {
        if (exits(node)) work.push_back(node.id);
    }
    while (!work.empty()) {
        const std::string id = work.back();
        work.pop_back();
        if (!escapes.insert(id).second) continue;
        for (const std::string& previous : predecessors[id]) work.push_back(previous);
    }
    std::set<std::string> trapped;
    // (an entry that leads into such a loop is where it starts, not part of it)
    for (const std::string& id : reached) {
        if (!escapes.count(id) && nodes.at(id)->type != "entry") trapped.insert(id);
    }
    std::set<std::string> grouped;
    for (const graph::Node& node : graph.nodes) {
        if (!trapped.count(node.id) || grouped.count(node.id)) continue;
        std::vector<std::string> members;
        work = {node.id};
        while (!work.empty()) {
            const std::string id = work.back();
            work.pop_back();
            if (!trapped.count(id) || !grouped.insert(id).second) continue;
            members.push_back(id);
            for (const std::string& next : successors[id]) work.push_back(next);
            for (const std::string& previous : predecessors[id]) work.push_back(previous);
        }
        std::sort(members.begin(), members.end(), [](const std::string& a, const std::string& b) { return graph::idLess(a, b); });
        std::string list;
        for (const std::string& id : members) list += (list.empty() ? "" : ", ") + id;
        add(Severity::Warning, "no-exit", members.front(), "these nodes loop with no way out (no end, no unconnected output, no option leading out): " + list);
    }
}

}  // namespace jasperengine::dialogue
