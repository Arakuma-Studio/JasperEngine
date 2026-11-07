// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generic graph data model's tools (S23 R44): the kinds and node types registered, a
// graph file's contents and issues, and edits as ops, each batch one undo step.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/GraphEditCmd.h"
#include "graph/Graph.h"
#include "graph/GraphRegistry.h"
#include "procmat/ProceduralMaterialKind.h"
#include "util/ProjectChecks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using graph::Graph;
using graph::GraphOp;
using graph::GraphRegistry;

// the editor test build's kind (JASPER_TEST_GRAPH_KIND=1): MCP test_graph.py edits graphs of it
void registerTestKind() {
    graph::GraphKindDesc kind;
    kind.id = "test";
    kind.displayName = "Test Graph";
    kind.fileExtension = ".testgraph";
    kind.kindVersion = 1;
    kind.acyclic = true;
    kind.portTypes = {{"float", 0xff8800ffu, {"vector3"}}, {"vector3", 0xffff00ffu, {}}, {"string", 0xff00ffffu, {}}};
    auto port = [](const char* id, graph::PortDirection direction, const char* type, const char* name, bool optional = false) {
        graph::Port p;
        p.id = id;
        p.direction = direction;
        p.type = type;
        p.name = name;
        p.optional = optional;
        return p;
    };
    graph::NodeTypeDesc speed;
    speed.id = "speed";
    speed.displayName = "Speed";
    speed.category = "Input";
    speed.ports = {port("value", graph::PortDirection::Out, "float", "Speed")};
    speed.params = {{"value", "float", 1.0, 0.0, 100.0, {}}};
    graph::NodeTypeDesc label;
    label.id = "label";
    label.displayName = "Label";
    label.ports = {port("text", graph::PortDirection::Out, "string", "Text")};
    graph::NodeTypeDesc blend;
    blend.id = "blend";
    blend.displayName = "Blend";
    blend.category = "Math";
    blend.ports = {port("a", graph::PortDirection::In, "float", "A"), port("b", graph::PortDirection::In, "float", "B", true),
                   port("out", graph::PortDirection::Out, "float", "Out")};
    blend.params = {{"weight", "float", 0.5, 0.0, 1.0, {}}};
    kind.nodeTypes = {speed, label, blend};
    GraphRegistry::get().registerKind(kind);

    // the same graph as a kind that opted in to strict parameters
    kind.id = "test_strict";
    kind.displayName = "Strict Test Graph";
    kind.fileExtension = ".strictgraph";
    kind.strictParams = true;
    GraphRegistry::get().registerKind(kind);
}

// the graph model's ordered JSON as the MCP replies' JSON
Json toMcp(const graph::Json& value) {
    return Json::parse(value.dump());
}

bool readGraph(const CallPtr& call, fs::path& file, std::string& text, Graph& out, graph::LoadReport& report) {
    std::string error;
    if (!McpRefs::resolveProjectPath(call->ctx().project(), call->args().value("path", std::string()), file, error, true)) {
        call->fail(error.empty() ? "path is required." : error);
        return false;
    }
    if (!ProjectChecks::readFile(file, text)) {
        call->fail("Could not read " + call->args().value("path", std::string()) + ".");
        return false;
    }
    out = graph::load(text, report);
    if (!report.ok) {
        call->fail(call->args().value("path", std::string()) + " is not a graph file: " + report.error);
        return false;
    }
    return true;
}

Json issuesJson(const std::vector<graph::Issue>& issues) {
    Json out = Json::array();
    for (const graph::Issue& issue : issues) out.push_back(toMcp(graph::toJson(issue)));
    return out;
}

Json reportJson(const graph::LoadReport& report) {
    Json out = Json::object();
    if (!report.warnings.empty()) out["warnings"] = report.warnings;
    if (!report.unknownNodeTypes.empty()) out["unknown_node_types"] = report.unknownNodeTypes;
    if (report.unknownKind) out["unknown_kind"] = true;
    if (report.migratedFrom > 0) out["migrated_from"] = report.migratedFrom;
    return out;
}

// A strict kind's parameter errors by their message: a second misspelled name or another bad value
// on a node is new. Any other error stays the same one while its message changes: a cycle that
// shrinks lists fewer nodes, and the edit added nothing.
std::string errorKey(const graph::Issue& issue) {
    const std::string key = issue.code + "|" + issue.nodeId + "|" + issue.portId + "|" + issue.edgeId;
    return issue.code == "unknown-param" || issue.code == "param-range" ? key + "|" + issue.message : key;
}

std::string issueText(const graph::Issue& issue) {
    return issue.suggestion.empty() ? issue.message : issue.message + " (" + issue.suggestion + ")";
}

void listGraphKindsTool(const CallPtr& call) {
    Json kinds = Json::array();
    for (const std::string& id : GraphRegistry::get().kindIds()) {
        if (auto kind = GraphRegistry::get().kind(id)) kinds.push_back(toMcp(graph::toJson(*kind)));
    }
    call->ok(std::to_string(kinds.size()) + " graph kind(s).", Json{{"kinds", kinds}});
}

// a kind with more node types than this answers an unfiltered list with its categories (R33)
constexpr size_t CATALOG_LIST_LIMIT = 40;

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

// 8, .5, 0.25, 1e+03: as short as a number reads
std::string shortNumber(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", value);
    std::string out = text;
    if (out.rfind("0.", 0) == 0) out.erase(0, 1);
    else if (out.rfind("-0.", 0) == 0) out.erase(1, 1);
    return out;
}

// One node type as a line: stones(in:f; scale=8[1..64] mode=a{a|b} seed:int) -> height:f mask:f
std::string signatureLine(const graph::NodeTypeDesc& type) {
    std::string inputs, outputs, params;
    for (const graph::Port& port : type.ports) {
        std::string& list = port.direction == graph::PortDirection::In ? inputs : outputs;
        list += (list.empty() ? "" : " ") + port.id + ":" + port.type;
    }
    for (const graph::ParamDesc& param : type.params) {
        std::string text = param.name;
        const graph::Json& value = param.defaultValue;
        if (value.is_number()) text += "=" + shortNumber(value.get<double>());
        else if (value.is_boolean()) text += value.get<bool>() ? "=true" : "=false";
        else if (value.is_string()) text += "=" + value.get<std::string>();
        else text += ":" + param.kind;
        if (param.min && param.max) text += "[" + shortNumber(*param.min) + ".." + shortNumber(*param.max) + "]";
        if (!param.enumValues.empty()) {
            std::string values;
            for (const std::string& v : param.enumValues) values += (values.empty() ? "" : "|") + v;
            text += "{" + values + "}";
        }
        params += (params.empty() ? "" : " ") + text;
    }
    return type.id + "(" + inputs + (inputs.empty() || params.empty() ? "" : "; ") + params + ") -> " + (outputs.empty() ? "nothing" : outputs);
}

void listGraphNodeTypesTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string id = args.value("kind", std::string());
    auto kind = GraphRegistry::get().kind(id);
    if (!kind) {
        call->fail("No graph kind '" + id + "' (list_graph_kinds lists them).");
        return;
    }
    const std::string category = lowered(args.value("category", std::string()));
    std::vector<std::string> words;
    {
        std::istringstream search(lowered(args.value("search", std::string())));
        for (std::string word; search >> word;) words.push_back(word);
    }
    std::set<std::string> ids;
    if (args.contains("ids") && args["ids"].is_array()) {
        for (const Json& item : args["ids"]) {
            if (item.is_string()) ids.insert(item.get<std::string>());
        }
    }
    const bool detail = args.value("detail", false);
    const bool filtered = !category.empty() || !words.empty() || !ids.empty();
    std::map<std::string, size_t> categories;
    std::vector<const graph::NodeTypeDesc*> matched;
    for (const graph::NodeTypeDesc& type : kind->nodeTypes) {
        categories[type.category.empty() ? "Other" : type.category]++;
        if (!ids.empty() && !ids.count(type.id)) continue;
        if (!category.empty() && lowered(type.category) != category) continue;
        const std::string haystack = lowered(type.id + " " + type.displayName + " " + type.category);
        if (std::any_of(words.begin(), words.end(), [&](const std::string& word) { return haystack.find(word) == std::string::npos; })) continue;
        matched.push_back(&type);
    }
    if (!filtered && kind->nodeTypes.size() > CATALOG_LIST_LIMIT) {
        Json list = Json::array();
        std::string text;
        for (const auto& [name, count] : categories) {
            list.push_back({{"category", name}, {"count", count}});
            text += (text.empty() ? "" : ", ") + name + " " + std::to_string(count);
        }
        call->ok(std::to_string(kind->nodeTypes.size()) + " node types of " + id + " in " + std::to_string(categories.size()) +
                 " categories: " + text + ". Pass category, search or ids for their signatures.",
                 Json{{"kind", id}, {"total", kind->nodeTypes.size()}, {"categories", list}});
        return;
    }
    for (const std::string& wanted : ids) {
        if (std::none_of(matched.begin(), matched.end(), [&](const graph::NodeTypeDesc* type) { return type->id == wanted; })) {
            call->fail("No node type '" + wanted + "' in " + id + " (search finds types by name).");
            return;
        }
    }
    Json data = {{"kind", id}, {"total", kind->nodeTypes.size()}, {"matched", matched.size()}};
    std::string text = std::to_string(matched.size()) + " of " + std::to_string(kind->nodeTypes.size()) + " node type(s) of " + id + ":";
    if (detail) {
        Json types = Json::array();
        for (const graph::NodeTypeDesc* type : matched) types.push_back(toMcp(graph::toJson(*type)));
        data["node_types"] = types;
    }
    Json lines = Json::array();
    for (const graph::NodeTypeDesc* type : matched) {
        lines.push_back(signatureLine(*type));
        text += "\n" + signatureLine(*type);
    }
    data["signatures"] = lines;
    call->ok(text, data);
}

void getGraphTool(const CallPtr& call) {
    fs::path file;
    std::string text;
    Graph g;
    graph::LoadReport report;
    if (!readGraph(call, file, text, g, report)) return;
    Json data = {{"graph", toMcp(graph::toJson(g))}, {"issue_count", graph::validate(g).size()}};
    const Json loaded = reportJson(report);
    if (!loaded.empty()) data["load"] = loaded;
    call->ok(g.kind + " graph: " + std::to_string(g.nodes.size()) + " node(s), " + std::to_string(g.edges.size()) + " edge(s).", data);
}

void validateGraphTool(const CallPtr& call) {
    fs::path file;
    std::string text;
    Graph g;
    graph::LoadReport report;
    if (!readGraph(call, file, text, g, report)) return;
    const std::vector<graph::Issue> issues = graph::validate(g);
    std::string summary = std::to_string(issues.size()) + " issue(s).";
    for (const graph::Issue& issue : issues) summary += "\n" + std::string(graph::severityName(issue.severity)) + " " + issue.code + ": " + issueText(issue);
    Json data = {{"issues", issuesJson(issues)}};
    const Json loaded = reportJson(report);
    if (!loaded.empty()) data["load"] = loaded;
    call->ok(summary, data);
}

void editGraphTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("ops") || !args["ops"].is_array() || args["ops"].empty()) {
        call->fail("ops is a non-empty list of {op, ...}: addNode, removeNode, moveNodes, setParam, connect, disconnect, "
                   "addVariable, removeVariable, renameVariable, setVariable, group, ungroup.");
        return;
    }
    fs::path file;
    std::string before;
    Graph g;
    graph::LoadReport report;
    if (!readGraph(call, file, before, g, report)) return;
    // rewriting it would lose what the file holds, or skip what its extension's kind checks
    const std::string problem = graph::fileProblem(args.value("path", std::string()), g, report);
    if (!problem.empty()) {
        call->fail(args.value("path", std::string()) + " is not a graph of its kind: " + problem + "; nothing was written. Fix the file first (read_file shows it).");
        return;
    }
    const bool dryRun = args.value("dry_run", false);
    const bool allowInvalid = args.value("allow_invalid", false);

    std::set<std::string> errorsBefore;
    for (const graph::Issue& issue : graph::validate(g)) {
        if (issue.severity == graph::Severity::Error) errorsBefore.insert(errorKey(issue));
    }
    // the ops in order; a bad one stops the batch with nothing written
    std::vector<std::string> names;
    Json addedIds = Json::array();
    size_t index = 0;
    try {
        for (const Json& item : args["ops"]) {
            const GraphOp op = graph::opFromJson(item);
            names.push_back(graph::describe(g, op));
            // what undoes a new node, edge or group names its id, supplied or allocated
            const GraphOp inverse = graph::apply(g, op);
            if (inverse.op == "removeNode" || inverse.op == "disconnect" || inverse.op == "ungroup") {
                addedIds.push_back({{"op_index", index}, {"id", inverse.args.value("id", std::string())}});
            }
            index++;
        }
    } catch (const std::exception& e) {
        call->fail("ops[" + std::to_string(index) + "]: " + e.what() + " (nothing was changed).");
        return;
    }
    const std::vector<graph::Issue> issues = graph::validate(g);
    Json newErrors = Json::array();
    std::string firstNewError;
    for (const graph::Issue& issue : issues) {
        if (issue.severity != graph::Severity::Error || errorsBefore.count(errorKey(issue))) continue;
        if (newErrors.empty()) firstNewError = issueText(issue);
        newErrors.push_back(toMcp(graph::toJson(issue)));
    }
    const std::string undoName = names.size() == 1 ? names.front() : "Edit Graph (" + std::to_string(names.size()) + " Changes)";
    Json data = {{"issues", issuesJson(issues)}, {"applied", names.size()}, {"undo_name", undoName}, {"dry_run", dryRun}};
    if (!newErrors.empty() && !allowInvalid) {
        data["applied"] = 0;
        data["new_errors"] = newErrors;
        call->fail("The edit would add " + std::to_string(newErrors.size()) + " error(s) (first: " + firstNewError +
                   "); nothing was written. allow_invalid: true writes it anyway.", data);
        return;
    }
    data["added"] = addedIds;
    if (dryRun) {
        data["graph"] = toMcp(graph::toJson(g));
        call->ok("Dry run: " + std::to_string(names.size()) + " op(s) apply, " + std::to_string(issues.size()) + " issue(s); nothing was written.", data);
        return;
    }
    const std::string after = graph::save(g);
    // one step of the project history (file edits are project-wide)
    CommandHistory* history = call->ctx().project()->getProjectCommandHistory();
    if (after != before && !history->addCommandNoMerge(new GraphEditCmd(file, before, after, undoName))) {
        const std::string busy = history->lastBusyRefusal();
        data["applied"] = 0;
        data.erase("added");
        call->fail("Could not write " + args.value("path", std::string()) + " (" + (busy.empty() ? "see read_console" : busy) + "); nothing was changed.", data);
        return;
    }
    call->ok(undoName + ": " + std::to_string(names.size()) + " op(s), " + std::to_string(issues.size()) + " issue(s).", data);
}

}

void registerGraphTools(McpRegistry& registry) {
    using namespace schema;
    if (const char* test = std::getenv("JASPER_TEST_GRAPH_KIND"); test && std::strcmp(test, "1") == 0) registerTestKind();
    procmat::registerProceduralMaterialKind();

    {
        ToolDef tool;
        tool.name = "list_graph_kinds";
        tool.title = "List Graph Kinds";
        tool.description = "The graph kinds (dataflow graph file types) registered: id, file_extension, kind_version, acyclic, port_types.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.handler = listGraphKindsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_graph_node_types";
        tool.title = "List Graph Node Types";
        tool.description = "A kind's node types, one signature line each: id(inputs; param=default[min..max]) -> outputs. A large "
                           "kind lists its categories until category, search or ids narrows it; detail adds the full ports and params.";
        tool.inputSchema = object({{"kind", string("")}, {"category", string("")}, {"search", string("Words in the id or name.")},
                                   {"ids", array(string(""), "")}, {"detail", withDefault(boolean(""), false)}}, {"kind"});
        tool.annotations.readOnly = true;
        tool.handler = listGraphNodeTypesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_graph";
        tool.title = "Get Graph";
        tool.description = "A graph file as JSON (nodes, edges, variables, groups; unknown keys kept) and its issue count.";
        tool.inputSchema = object({{"path", assetPath("A graph file.")}}, {"path"});
        tool.annotations.readOnly = true;
        tool.handler = getGraphTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "validate_graph";
        tool.title = "Validate Graph";
        tool.description = "A graph file's issues: dangling-edge, duplicate-id, "
                           "direction, type-mismatch, multiplicity, cycle, unknown-node-type, missing-required-input, param-range, "
                           "unknown-param, and the kind's own.";
        tool.inputSchema = object({{"path", assetPath("A graph file.")}}, {"path"});
        tool.annotations.readOnly = true;
        tool.handler = validateGraphTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_graph";
        tool.title = "Edit Graph";
        tool.description =
            "Apply ops to a graph file as one undo step: [{op: addNode, node: {type, position, params}} | removeNode {id} | "
            "moveNodes {ids, delta} | setParam {node, name, value} | connect {from: {node, port}, to} | disconnect {id} | "
            "addVariable {variable} | removeVariable {name} | renameVariable {from, to} | setVariable {name, ...} | "
            "group {title, nodes} | ungroup {id}]. Refused when it adds error issues (allow_invalid: true writes anyway); "
            "dry_run writes nothing.";
        tool.inputSchema = object({
            {"path", assetPath("A graph file.")},
            {"ops", array(anyObject("{op, ...args}"), "")},
            {"dry_run", withDefault(boolean(""), false)},
            {"allow_invalid", withDefault(boolean(""), false)}
        }, {"path", "ops"});
        tool.handler = editGraphTool;
        registry.addTool(std::move(tool));
    }
}

}
