// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generic graph data model's tools (S23 R44): the kinds and node types registered, a
// graph file's contents and issues, and edits as ops, each batch one undo step.

#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/GraphEditCmd.h"
#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueVariables.h"
#include "dialogue/DialogueWalker.h"
#include "object/DialogueRunner.h"
#include "graph/Graph.h"
#include "graph/GraphRegistry.h"
#include "graph/GraphEditorHooks.h"
#include "noise/TerrainPresets.h"
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
        std::string haystack = lowered(type.id + " " + type.displayName + " " + type.category);
        for (const std::string& alias : type.aliases) haystack += " " + lowered(alias);
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

// T2 R16: create {kind, from_preset?} makes the graph a missing file starts as: a preset's copy, or empty
bool startCreatedGraph(const CallPtr& call, fs::path& file, Graph& out) {
    const Json& args = call->args();
    const Json& create = args["create"];
    const std::string kindId = create.value("kind", std::string());
    std::shared_ptr<const graph::GraphKindDesc> kind = graph::GraphRegistry::get().kind(kindId);
    if (!kind) {
        call->fail("create.kind: no graph kind '" + kindId + "' (list_graph_kinds).");
        return false;
    }
    std::string error;
    const std::string path = args.value("path", std::string());
    if (!McpRefs::resolveProjectPath(call->ctx().project(), path, file, error, false)) {
        call->fail(error.empty() ? "path is required." : error);
        return false;
    }
    std::error_code ec;
    if (fs::exists(file, ec)) {
        call->fail(path + " already exists: drop create to edit it.");
        return false;
    }
    if (file.extension().string() != kind->fileExtension) {
        call->fail("A '" + kindId + "' graph's file ends in " + kind->fileExtension + ".");
        return false;
    }
    graph::LoadReport report;
    if (create.contains("from_preset") && create["from_preset"].is_string()) {
        const std::string preset = create["from_preset"].get<std::string>();
        const std::string text = TerrainPresets::json(preset);
        if (text.empty()) {
            call->fail("create.from_preset: no preset '" + preset + "'.");
            return false;
        }
        out = graph::load(text, report);
    } else {
        out = Graph();
        out.kind = kindId;
        out.kindVersion = kind->kindVersion;
    }
    // what a new file of the kind starts with (a dialogue's namespace, source language and GUID)
    auto edits = graphEditorHooks().kindEdits.find(kindId);
    if (edits != graphEditorHooks().kindEdits.end() && edits->second.created) edits->second.created(call->ctx().project(), file, out);
    return true;
}

void editGraphTool(const CallPtr& call) {
    const Json& args = call->args();
    const bool creating = args.contains("create") && args["create"].is_object();
    if (!args.contains("ops") || !args["ops"].is_array() || (args["ops"].empty() && !creating)) {
        call->fail("ops is a non-empty list of {op, ...}: addNode, removeNode, moveNodes, setParam, connect, disconnect, "
                   "addVariable, removeVariable, renameVariable, setVariable, group, ungroup.");
        return;
    }
    fs::path file;
    std::string before;
    Graph g;
    graph::LoadReport report;
    if (creating) {
        if (!startCreatedGraph(call, file, g)) return;
    } else {
        if (!readGraph(call, file, before, g, report)) return;
        // rewriting it would lose what the file holds, or skip what its extension's kind checks
        const std::string problem = graph::fileProblem(args.value("path", std::string()), g, report);
        if (!problem.empty()) {
            call->fail(args.value("path", std::string()) + " is not a graph of its kind: " + problem + "; nothing was written. Fix the file first (read_file shows it).");
            return;
        }
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
    const std::string undoName = creating ? "Create " + file.filename().string()
        : (names.size() == 1 ? names.front() : "Edit Graph (" + std::to_string(names.size()) + " Changes)");
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
    if (after != before && !history->addCommandNoMerge(GraphEditCmd::makeStep(call->ctx().project(), file, g.kind, before, after, undoName, creating))) {
        const std::string busy = history->lastBusyRefusal();
        data["applied"] = 0;
        data.erase("added");
        call->fail("Could not write " + args.value("path", std::string()) + " (" + (busy.empty() ? "see read_console" : busy) + "); nothing was changed.", data);
        return;
    }
    call->ok(undoName + ": " + std::to_string(names.size()) + " op(s), " + std::to_string(issues.size()) + " issue(s).", data);
}

// ---- run_dialogue (dialogue-graphs.md R25)

bool dialogueValue(const Json& json, jasperengine::dialogue::Value& out) {
    if (json.is_boolean()) out = jasperengine::dialogue::Value(json.get<bool>());
    else if (json.is_number()) out = jasperengine::dialogue::Value(json.get<double>());
    else if (json.is_string()) out = jasperengine::dialogue::Value(json.get<std::string>());
    else return false;
    return true;
}

bool dialogueValues(const CallPtr& call, const char* key, std::vector<std::pair<std::string, jasperengine::dialogue::Value>>& out) {
    const Json& args = call->args();
    if (!args.contains(key)) return true;
    if (!args[key].is_object()) {
        call->fail(std::string(key) + " is an object of name -> bool, number or string.");
        return false;
    }
    for (auto it = args[key].begin(); it != args[key].end(); ++it) {
        jasperengine::dialogue::Value value;
        if (!dialogueValue(it.value(), value)) {
            call->fail(std::string(key) + "." + it.key() + " must be a bool, number or string.");
            return false;
        }
        std::string name = it.key();
        if (!name.empty() && name[0] == '$') name.erase(0, 1);
        out.emplace_back(name, value);
    }
    return true;
}

std::string choicesText(const std::vector<int>& choices) {
    std::string out;
    for (int choice : choices) out += (out.empty() ? "" : ", ") + std::to_string(choice);
    return "[" + out + "]";
}

std::string transcriptText(const dialogues::WalkResult& result) {
    std::string text;
    for (const dialogues::WalkStep& step : result.transcript) {
        text += "\n" + step.node + " " + (step.speaker.empty() ? std::string() : step.speaker + ": ") + step.text;
        for (const jasperengine::dialogue::Option& option : step.options) {
            text += "\n  " + std::to_string(option.index) + ". " + option.text + (option.enabled ? "" : " (disabled)") +
                    (step.chosen == option.index ? "  <- chosen" : "");
        }
    }
    return text;
}

// The live runner of a GameObject in Play: start, choices and advance with the real handlers
void runLiveDialogue(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error + " (entity drives a Dialogue Runner in Play: play first, or pass path to walk the file without Play).");
        return;
    }
    const Entity entity = w3::resolveLiveEntity(project, sp, args["entity"], error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    if (!sp->scene->findComponent<DialogueRunnerComponent>(entity)) {
        call->fail("That GameObject has no Dialogue Runner: add one (add_component dialoguerunner) or pass path.");
        return;
    }
    DialogueRunner object(sp->scene, entity);
    jasperengine::dialogue::Runner* runner = object.getRunner();
    if (!runner || !runner->getDialogue()) {
        call->fail("Its Dialogue Runner has no dialogue that loads (set its dialogue property).");
        return;
    }
    dialogues::WalkResult result;
    // what the runner shows during this call, on top of its own events
    auto previous = runner->onLine;
    runner->onLine = [&result, previous](jasperengine::dialogue::Runner& self, const jasperengine::dialogue::Line& line) {
        dialogues::WalkStep step;
        step.node = line.node;
        step.speaker = line.speakerName;
        step.text = line.text;
        step.key = line.key;
        step.options = line.options;
        step.actions = self.lastStep().actions;
        result.transcript.push_back(std::move(step));
        if (previous) previous(self, line);
    };
    runner->setRecording(true);
    if (args.contains("entry") || !runner->isRunning()) {
        if (!object.start(args.value("entry", std::string()))) error = runner->getError();
    }
    if (error.empty() && args.contains("choices") && args["choices"].is_array()) {
        for (const Json& choice : args["choices"]) {
            const int index = choice.is_number_integer() ? choice.get<int>() : 0;
            if (!result.transcript.empty()) result.transcript.back().chosen = index;
            if (!object.choose(index)) {
                error = "choice " + std::to_string(index) + " is not an enabled option of the current line (" + object.getCurrentNode() + ")";
                break;
            }
        }
    }
    for (int i = 0; error.empty() && i < args.value("advance", 0); i++) {
        if (!object.advance()) break;
    }
    runner->onLine = previous;
    runner->setRecording(false);
    result.state = jasperengine::dialogue::stateName(runner->getState());
    result.node = runner->getCurrentNode();
    result.result = runner->getResult();
    result.unknownCalls = runner->unknownCalls();
    result.variables = jasperengine::dialogue::Variables::all();
    result.error = error;
    Json data = dialogues::toJson(result);
    if (result.state == "line" && runner->getCurrentLine()) {
        Json options = Json::array();
        for (const jasperengine::dialogue::Option& option : runner->getCurrentLine()->options) options.push_back({{"index", option.index}, {"text", option.text}, {"enabled", option.enabled}});
        data["state"]["options"] = options;
        data["state"]["text"] = runner->getCurrentLine()->text;
    }
    if (!error.empty()) {
        call->fail(error + ".", data);
        return;
    }
    call->ok("Dialogue Runner: " + result.state + (result.node.empty() ? "" : " at " + result.node) + transcriptText(result), data);
}

void runDialogueTool(const CallPtr& call) {
    const Json& args = call->args();
    const bool hasPath = args.contains("path"), hasEntity = args.contains("entity");
    if (hasPath == hasEntity) {
        call->fail("Pass exactly one of path (walk a .dialogue headless, no Play needed) and entity (drive a Dialogue Runner in Play).");
        return;
    }
    if (hasEntity) {
        runLiveDialogue(call);
        return;
    }
    jasperengine::dialogue::registerDialogueKind();
    fs::path file;
    std::string text;
    Graph g;
    graph::LoadReport report;
    if (!readGraph(call, file, text, g, report)) return;
    if (g.kind != jasperengine::dialogue::KIND_ID) {
        call->fail(args.value("path", std::string()) + " is a '" + g.kind + "' graph: run_dialogue plays .dialogue files.");
        return;
    }
    // asset-relative, as a Dialogue Runner names it: the jumps it makes load the same way
    const std::string relative = call->ctx().project()->normalizeToAssetsRelative(file).generic_string();
    const std::shared_ptr<const jasperengine::dialogue::Program> program = jasperengine::dialogue::Program::compile(g, relative);
    dialogues::WalkOptions options;
    options.entry = args.value("entry", std::string());
    if (args.contains("choices") && args["choices"].is_array()) {
        for (const Json& choice : args["choices"]) {
            if (!choice.is_number_integer()) {
                call->fail("choices is a list of 1-based option numbers.");
                return;
            }
            options.choices.push_back(choice.get<int>());
        }
    }
    if (!dialogueValues(call, "variables", options.variables)) return;
    std::vector<std::pair<std::string, jasperengine::dialogue::Value>> conditions;
    if (!dialogueValues(call, "conditions", conditions)) return;
    for (const auto& [name, value] : conditions) options.conditions[name] = value;
    options.language = args.value("language", std::string());
    options.seed = args.value("seed", static_cast<uint64_t>(0));
    options.maxSteps = std::clamp(args.value("max_steps", 200), 1, 2000);
    if (!options.entry.empty() && program->findEntry(options.entry) < 0) {
        std::string entries;
        for (const auto& [name, index] : program->entries) entries += (entries.empty() ? "" : ", ") + std::string(program->str(name));
        call->fail("No entry '" + options.entry + "' in " + relative + " (entries: " + (entries.empty() ? "none" : entries) + ").");
        return;
    }
    if (args.value("explore", false)) {
        const dialogues::ExploreResult explored = dialogues::explore(program, options, std::clamp(args.value("max_paths", 64), 1, 512));
        Json data = dialogues::toJson(explored);
        std::string summary = "Explored " + std::to_string(explored.paths) + " path(s): " + std::to_string(explored.covered) + "/" + std::to_string(explored.nodes) +
                              " nodes covered, " + std::to_string(explored.endings.size()) + " ending(s), " + std::to_string(explored.loops.size()) + " no-exit loop(s).";
        for (const auto& ending : explored.endings) summary += "\nend \"" + ending.result + "\" at " + ending.node + " by choices " + choicesText(ending.choices);
        for (const auto& loop : explored.loops) {
            std::string nodes;
            for (const std::string& node : loop.nodes) nodes += (nodes.empty() ? "" : ", ") + node;
            summary += "\nno exit: " + nodes + " (reached by choices " + choicesText(loop.choices) + ")";
        }
        for (const std::string& line : explored.linesNeverShown) summary += "\nnever shown: " + line;
        if (explored.truncated) data["next"] = "max_paths was reached before every path was walked: raise max_paths (up to 512) for a full answer.";
        call->ok(summary, data);
        return;
    }
    const dialogues::WalkResult result = dialogues::walk(program, options);
    Json data = dialogues::toJson(result);
    std::vector<std::string> next;
    if (!result.unknownCalls.empty()) {
        std::string calls;
        for (const std::string& unknown : result.unknownCalls) calls += (calls.empty() ? "" : ", ") + unknown;
        next.push_back("conditions " + calls + " were assumed false; pass conditions (e.g. {\"" +
                       result.unknownCalls.front().substr(0, result.unknownCalls.front().find('(')) + "\": true}) to take the other branch");
    }
    if (result.state == "line" && !result.transcript.empty() && !result.transcript.back().options.empty()) {
        next.push_back("stopped at a menu: add a 1-based pick to choices; explore: true walks every path");
    }
    if (result.truncated) next.push_back("max_steps lines were shown: the walk may loop (explore: true reports no-exit loops)");
    if (!next.empty()) {
        std::string joined;
        for (const std::string& hint : next) joined += (joined.empty() ? "" : "; ") + hint;
        data["next"] = joined;
    }
    if (!result.error.empty()) {
        call->fail(result.error + ".", data);
        return;
    }
    call->ok(relative + ": " + result.state + (result.state == "ended" ? " (" + (result.result.empty() ? std::string("no result") : result.result) + ")" : "") +
             transcriptText(result), data);
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
        tool.inputSchema = object({{"kind", string("")}, {"category", string("")}, {"search", string("Words in the id, name or aliases.")},
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
            "dry_run writes nothing. create makes a missing file, undone by removing it.";
        tool.inputSchema = object({
            {"path", assetPath("A graph file.")},
            {"ops", array(anyObject("{op, ...args}"), "")},
            {"dry_run", withDefault(boolean(""), false)},
            {"allow_invalid", withDefault(boolean(""), false)},
            {"create", anyObject("{kind, from_preset?}: make the missing file")}
        }, {"path", "ops"});
        tool.handler = editGraphTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "run_dialogue";
        tool.title = "Run Dialogue";
        tool.group = "graphs";
        tool.description =
            "Play a .dialogue: path walks it headless (no Play; handler calls never run, unlisted conditions are false and reported), "
            "picking choices in order at each menu. entity drives a Dialogue Runner in Play with the real handlers.";
        tool.inputSchema = object({
            {"path", assetPath("A .dialogue.")},
            {"entity", entityRef()},
            {"entry", string("")},
            {"choices", array(integer("1-based option number.", 1, 64), "")},
            {"advance", integer("entity: advance() calls after the choices.", 0, 100)},
            {"variables", anyObject("Start values: {gold: 10}.")},
            {"conditions", anyObject("Handler condition values: {\"quest.done\": true} or {\"quest.*\": false}.")},
            {"language", string("")},
            {"seed", integer("", 0, 4294967295LL)},
            {"explore", withDefault(boolean(""), false)},
            {"max_steps", withDefault(integer("", 1, 2000), 200)},
            {"max_paths", withDefault(integer("", 1, 512), 64)},
        });
        tool.allowedWhilePlaying = true;
        tool.handler = runDialogueTool;
        registry.addTool(std::move(tool));
    }
}

}
