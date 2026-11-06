// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Graph.h"
#include "GraphRegistry.h"
#include "util/NearestName.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine::graph;

namespace {

    // ---- ids

    struct IdParts {
        std::string_view prefix;
        uint64_t number = 0;
        bool hasNumber = false;
    };

    IdParts splitId(std::string_view id){
        IdParts parts;
        size_t digits = id.size();
        while (digits > 0 && std::isdigit(static_cast<unsigned char>(id[digits - 1]))) digits--;
        parts.prefix = id.substr(0, digits);
        if (digits < id.size() && id.size() - digits <= 18) {
            parts.hasNumber = true;
            for (size_t i = digits; i < id.size(); i++) parts.number = parts.number * 10 + static_cast<uint64_t>(id[i] - '0');
        }
        return parts;
    }

    template<class T>
    auto lowerById(std::vector<T>& items, std::string_view id){
        return std::lower_bound(items.begin(), items.end(), id, [](const T& item, std::string_view value) { return idLess(item.id, value); });
    }

    template<class T>
    auto lowerById(const std::vector<T>& items, std::string_view id){
        return std::lower_bound(items.begin(), items.end(), id, [](const T& item, std::string_view value) { return idLess(item.id, value); });
    }

    template<class T>
    T* findById(std::vector<T>& items, std::string_view id){
        auto it = lowerById(items, id);
        return it != items.end() && it->id == id ? &*it : nullptr;
    }

    template<class T>
    const T* findById(const std::vector<T>& items, std::string_view id){
        auto it = lowerById(items, id);
        return it != items.end() && it->id == id ? &*it : nullptr;
    }

    template<class T>
    void insertSorted(std::vector<T>& items, T item){
        auto it = lowerById(items, item.id);
        items.insert(it, std::move(item));
    }

    // ---- JSON helpers

    Json vec2Json(const Vec2& v){
        return Json::array({v.x, v.y});
    }

    Vec2 vec2From(const Json& json){
        Vec2 v;
        if (json.is_array() && json.size() >= 2 && json[0].is_number() && json[1].is_number()) {
            v.x = json[0].get<double>();
            v.y = json[1].get<double>();
        } else if (json.is_object()) {
            v.x = json.value("x", 0.0);
            v.y = json.value("y", 0.0);
        }
        return v;
    }

    std::string stringOf(const Json& json, const char* key){
        auto it = json.find(key);
        return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    bool boolOf(const Json& json, const char* key){
        auto it = json.find(key);
        return it != json.end() && it->is_boolean() && it->get<bool>();
    }

    // the keys of `json` not in `known`, in their order
    Json passthroughOf(const Json& json, std::initializer_list<const char*> known){
        Json out = Json::object();
        if (!json.is_object()) return out;
        for (auto it = json.begin(); it != json.end(); ++it) {
            bool isKnown = false;
            for (const char* key : known) {
                if (it.key() == key) {
                    isKnown = true;
                    break;
                }
            }
            if (!isKnown) out[it.key()] = it.value();
        }
        return out;
    }

    void appendPassthrough(Json& out, const Json& passthrough){
        if (!passthrough.is_object()) return;
        for (auto it = passthrough.begin(); it != passthrough.end(); ++it) out[it.key()] = it.value();
    }

    const char* directionName(PortDirection direction){
        return direction == PortDirection::Out ? "out" : "in";
    }

    Json portJson(const Port& port){
        Json out = Json::object();
        out["id"] = port.id;
        out["direction"] = directionName(port.direction);
        out["type"] = port.type;
        if (!port.name.empty()) out["name"] = port.name;
        if (port.multi) out["multi"] = true;
        if (port.optional) out["optional"] = true;
        appendPassthrough(out, port.passthrough);
        return out;
    }

    Port portFrom(const Json& json){
        Port port;
        port.id = stringOf(json, "id");
        port.direction = stringOf(json, "direction") == "out" ? PortDirection::Out : PortDirection::In;
        port.type = stringOf(json, "type");
        port.name = stringOf(json, "name");
        port.multi = boolOf(json, "multi");
        port.optional = boolOf(json, "optional");
        port.passthrough = passthroughOf(json, {"id", "direction", "type", "name", "multi", "optional"});
        return port;
    }

    Json portRefJson(const PortRef& ref){
        Json out = Json::object();
        out["node"] = ref.node;
        out["port"] = ref.port;
        appendPassthrough(out, ref.passthrough);
        return out;
    }

    PortRef portRefFrom(const Json& json){
        PortRef ref;
        if (!json.is_object()) return ref;
        ref.node = stringOf(json, "node");
        ref.port = stringOf(json, "port");
        ref.passthrough = passthroughOf(json, {"node", "port"});
        return ref;
    }

    void noteId(Graph& graph, std::string_view id){
        const IdParts parts = splitId(id);
        if (parts.hasNumber && parts.number + 1 > graph.nextId) graph.nextId = parts.number + 1;
    }

    const Json& requireArg(const GraphOp& op, const char* key){
        auto it = op.args.find(key);
        if (it == op.args.end() || it->is_null()) throw GraphOpError(op.op + ": '" + key + "' is required");
        return *it;
    }

    std::string requireString(const GraphOp& op, const char* key){
        const Json& value = requireArg(op, key);
        if (!value.is_string() || value.get<std::string>().empty()) throw GraphOpError(op.op + ": '" + key + "' must be a non-empty string");
        return value.get<std::string>();
    }

    void restoreNextId(Graph& graph, const GraphOp& op){
        auto it = op.args.find("nextId");
        if (it != op.args.end() && it->is_number_unsigned()) graph.nextId = it->get<uint64_t>();
    }

    std::string nodeLabel(const Graph& graph, const std::string& nodeId){
        const Node* node = graph.findNode(nodeId);
        if (!node) return nodeId;
        if (auto kind = GraphRegistry::get().kind(graph.kind)) {
            if (const NodeTypeDesc* type = GraphRegistry::nodeType(*kind, node->type)) {
                if (!type->displayName.empty()) return type->displayName;
            }
        }
        return node->type.empty() ? nodeId : node->type;
    }

    std::string portLabel(const Graph& graph, const PortRef& ref){
        if (const Node* node = graph.findNode(ref.node)) {
            for (const Port& port : portsOf(graph, *node)) {
                if (port.id == ref.port && !port.name.empty()) return port.name;
            }
        }
        return ref.port;
    }

    std::string didYouMean(const std::string& name, const std::vector<std::string>& candidates){
        const std::string nearest = jasperengine::NearestName(name, candidates);
        return nearest.empty() ? std::string() : "did you mean '" + nearest + "'?";
    }

    std::string listOf(const std::vector<std::string>& names){
        std::string list;
        for (const std::string& name : names) list += (list.empty() ? "" : ", ") + name;
        return list;
    }

}

// ------------------------------------------------------------------ model

bool jasperengine::graph::idLess(std::string_view a, std::string_view b){
    const IdParts pa = splitId(a);
    const IdParts pb = splitId(b);
    if (pa.prefix != pb.prefix) return pa.prefix < pb.prefix;
    if (pa.hasNumber != pb.hasNumber) return !pa.hasNumber;
    if (pa.number != pb.number) return pa.number < pb.number;
    return a < b;
}

Node* Graph::findNode(std::string_view id){ return findById(nodes, id); }
const Node* Graph::findNode(std::string_view id) const { return findById(nodes, id); }
Edge* Graph::findEdge(std::string_view id){ return findById(edges, id); }
const Edge* Graph::findEdge(std::string_view id) const { return findById(edges, id); }
Group* Graph::findGroup(std::string_view id){
    for (Group& group : groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

Variable* Graph::findVariable(std::string_view name){
    for (Variable& variable : variables) {
        if (variable.name == name) return &variable;
    }
    return nullptr;
}

const Variable* Graph::findVariable(std::string_view name) const {
    for (const Variable& variable : variables) {
        if (variable.name == name) return &variable;
    }
    return nullptr;
}

std::string Graph::allocateId(const char* prefix){
    return std::string(prefix) + std::to_string(nextId++);
}

std::vector<Port> jasperengine::graph::portsOf(const Graph& graph, const Node& node){
    if (node.dynamicPorts) return *node.dynamicPorts;
    auto kind = GraphRegistry::get().kind(graph.kind);
    if (!kind) return {};
    const NodeTypeDesc* type = GraphRegistry::nodeType(*kind, node.type);
    if (!type) return {};
    if (type->dynamicPorts) return type->dynamicPorts(node.params);
    return type->ports;
}

// ------------------------------------------------------------------ JSON

Json jasperengine::graph::toJson(const Node& node){
    Json out = Json::object();
    out["id"] = node.id;
    out["type"] = node.type;
    out["position"] = vec2Json(node.position);
    if (node.size) out["size"] = vec2Json(*node.size);
    out["params"] = node.params.is_object() ? node.params : Json::object();
    if (node.dynamicPorts) {
        Json ports = Json::array();
        for (const Port& port : *node.dynamicPorts) ports.push_back(portJson(port));
        out["ports"] = std::move(ports);
    }
    appendPassthrough(out, node.passthrough);
    return out;
}

Node jasperengine::graph::nodeFromJson(const Json& json){
    Node node;
    if (!json.is_object()) return node;
    node.id = stringOf(json, "id");
    node.type = stringOf(json, "type");
    if (auto it = json.find("position"); it != json.end()) node.position = vec2From(*it);
    if (auto it = json.find("size"); it != json.end() && !it->is_null()) node.size = vec2From(*it);
    if (auto it = json.find("params"); it != json.end() && it->is_object()) node.params = *it;
    if (auto it = json.find("ports"); it != json.end() && it->is_array()) {
        std::vector<Port> ports;
        for (const Json& port : *it) ports.push_back(portFrom(port));
        node.dynamicPorts = std::move(ports);
    }
    node.passthrough = passthroughOf(json, {"id", "type", "position", "size", "params", "ports"});
    return node;
}

Json jasperengine::graph::toJson(const Edge& edge){
    Json out = Json::object();
    out["id"] = edge.id;
    out["from"] = portRefJson(edge.from);
    out["to"] = portRefJson(edge.to);
    appendPassthrough(out, edge.passthrough);
    return out;
}

Edge jasperengine::graph::edgeFromJson(const Json& json){
    Edge edge;
    if (!json.is_object()) return edge;
    edge.id = stringOf(json, "id");
    if (auto it = json.find("from"); it != json.end()) edge.from = portRefFrom(*it);
    if (auto it = json.find("to"); it != json.end()) edge.to = portRefFrom(*it);
    edge.passthrough = passthroughOf(json, {"id", "from", "to"});
    return edge;
}

Json jasperengine::graph::toJson(const Variable& variable){
    Json out = Json::object();
    out["name"] = variable.name;
    out["type"] = variable.type;
    out["default"] = variable.defaultValue;
    out["exposed"] = variable.exposed;
    if (!variable.group.empty()) out["group"] = variable.group;
    appendPassthrough(out, variable.passthrough);
    return out;
}

Variable jasperengine::graph::variableFromJson(const Json& json){
    Variable variable;
    if (!json.is_object()) return variable;
    variable.name = stringOf(json, "name");
    variable.type = stringOf(json, "type");
    if (auto it = json.find("default"); it != json.end()) variable.defaultValue = *it;
    variable.exposed = boolOf(json, "exposed");
    variable.group = stringOf(json, "group");
    variable.passthrough = passthroughOf(json, {"name", "type", "default", "exposed", "group"});
    return variable;
}

Json jasperengine::graph::toJson(const Group& group){
    Json out = Json::object();
    out["id"] = group.id;
    out["title"] = group.title;
    out["rect"] = Json::array({group.rect.x, group.rect.y, group.rect.width, group.rect.height});
    out["nodes"] = group.nodeIds;
    appendPassthrough(out, group.passthrough);
    return out;
}

Group jasperengine::graph::groupFromJson(const Json& json){
    Group group;
    if (!json.is_object()) return group;
    group.id = stringOf(json, "id");
    group.title = stringOf(json, "title");
    if (auto it = json.find("rect"); it != json.end() && it->is_array() && it->size() >= 4) {
        group.rect = {(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>(), (*it)[3].get<double>()};
    }
    if (auto it = json.find("nodes"); it != json.end() && it->is_array()) {
        for (const Json& id : *it) {
            if (id.is_string()) group.nodeIds.push_back(id.get<std::string>());
        }
    }
    group.passthrough = passthroughOf(json, {"id", "title", "rect", "nodes"});
    return group;
}

Json jasperengine::graph::toJson(const Graph& graph){
    Json out = Json::object();
    out["formatVersion"] = graph.formatVersion;
    out["kind"] = graph.kind;
    out["kindVersion"] = graph.kindVersion;
    out["nextId"] = graph.nextId;
    Json nodes = Json::array();
    for (const Node& node : graph.nodes) nodes.push_back(toJson(node));
    out["nodes"] = std::move(nodes);
    Json edges = Json::array();
    for (const Edge& edge : graph.edges) edges.push_back(toJson(edge));
    out["edges"] = std::move(edges);
    Json variables = Json::array();
    for (const Variable& variable : graph.variables) variables.push_back(toJson(variable));
    out["variables"] = std::move(variables);
    Json groups = Json::array();
    for (const Group& group : graph.groups) groups.push_back(toJson(group));
    out["groups"] = std::move(groups);
    Json viewport = Json::object();
    viewport["offset"] = vec2Json(graph.viewport.offset);
    viewport["zoom"] = graph.viewport.zoom;
    appendPassthrough(viewport, graph.viewport.passthrough);
    out["viewport"] = std::move(viewport);
    out["metadata"] = graph.metadata.is_object() ? graph.metadata : Json::object();
    appendPassthrough(out, graph.passthrough);
    return out;
}

Graph jasperengine::graph::fromJson(const Json& input, LoadReport& report){
    report = LoadReport();
    Graph graph;
    if (!input.is_object()) {
        report.ok = false;
        report.error = "a graph file is a JSON object";
        return graph;
    }
    Json json = input;
    graph.formatVersion = json.value("formatVersion", FORMAT_VERSION);
    if (graph.formatVersion > FORMAT_VERSION) {
        report.ok = false;
        report.error = "graph format " + std::to_string(graph.formatVersion) + " is newer than this build reads (" +
                       std::to_string(FORMAT_VERSION) + ")";
        return graph;
    }
    graph.kind = stringOf(json, "kind");
    graph.kindVersion = json.value("kindVersion", 1);

    std::shared_ptr<const GraphKindDesc> kind = GraphRegistry::get().kind(graph.kind);
    if (!kind) {
        report.unknownKind = true;
        report.warnings.push_back("unknown graph kind '" + graph.kind + "': kept as it is");
    } else if (graph.kindVersion < kind->kindVersion) {
        // each migration once, in order, on the raw file
        report.migratedFrom = graph.kindVersion;
        for (const Migration& migration : kind->migrations) {
            if (migration.toVersion <= graph.kindVersion || migration.toVersion > kind->kindVersion) continue;
            if (migration.fn) migration.fn(json);
            graph.kindVersion = migration.toVersion;
        }
        graph.kindVersion = kind->kindVersion;
    } else if (graph.kindVersion > kind->kindVersion) {
        report.warnings.push_back("'" + graph.kind + "' version " + std::to_string(graph.kindVersion) +
                                  " is newer than this build's " + std::to_string(kind->kindVersion) + ": kept as it is");
    }

    graph.nextId = json.value("nextId", static_cast<uint64_t>(1));
    for (const char* key : {"nodes", "edges", "variables", "groups"}) {
        if (auto it = json.find(key); it != json.end() && !it->is_array()) report.malformed.push_back(std::string("'") + key + "' is not a list");
    }
    for (const char* key : {"viewport", "metadata"}) {
        if (auto it = json.find(key); it != json.end() && !it->is_object()) report.malformed.push_back(std::string("'") + key + "' is not an object");
    }
    std::set<std::string> unknownTypes;
    if (auto it = json.find("nodes"); it != json.end() && it->is_array()) {
        for (const Json& item : *it) {
            Node node = nodeFromJson(item);
            noteId(graph, node.id);
            if (kind && !GraphRegistry::nodeType(*kind, node.type)) unknownTypes.insert(node.type);
            graph.nodes.push_back(std::move(node));
        }
    }
    if (auto it = json.find("edges"); it != json.end() && it->is_array()) {
        for (const Json& item : *it) {
            Edge edge = edgeFromJson(item);
            noteId(graph, edge.id);
            graph.edges.push_back(std::move(edge));
        }
    }
    if (auto it = json.find("variables"); it != json.end() && it->is_array()) {
        for (const Json& item : *it) graph.variables.push_back(variableFromJson(item));
    }
    if (auto it = json.find("groups"); it != json.end() && it->is_array()) {
        for (const Json& item : *it) {
            Group group = groupFromJson(item);
            noteId(graph, group.id);
            graph.groups.push_back(std::move(group));
        }
    }
    // a hand-written edge may leave its id out: past every id of the file, in file order, so two
    // loads of the file agree
    for (Edge& edge : graph.edges) {
        if (edge.id.empty()) edge.id = graph.allocateId("e");
    }
    if (auto it = json.find("viewport"); it != json.end() && it->is_object()) {
        if (auto offset = it->find("offset"); offset != it->end()) graph.viewport.offset = vec2From(*offset);
        graph.viewport.zoom = it->value("zoom", 1.0);
        graph.viewport.passthrough = passthroughOf(*it, {"offset", "zoom"});
    }
    if (auto it = json.find("metadata"); it != json.end() && it->is_object()) graph.metadata = *it;
    graph.passthrough = passthroughOf(json, {"formatVersion", "kind", "kindVersion", "nextId", "nodes", "edges",
                                             "variables", "groups", "viewport", "metadata"});

    // id order (stable: duplicates keep their file order, validate reports them)
    std::stable_sort(graph.nodes.begin(), graph.nodes.end(), [](const Node& a, const Node& b) { return idLess(a.id, b.id); });
    std::stable_sort(graph.edges.begin(), graph.edges.end(), [](const Edge& a, const Edge& b) { return idLess(a.id, b.id); });

    for (const std::string& type : unknownTypes) {
        report.unknownNodeTypes.push_back(type);
        report.warnings.push_back("unknown node type '" + type + "': its nodes are kept as they are");
    }
    return graph;
}

Graph jasperengine::graph::load(std::string_view text, LoadReport& report){
    Json json = Json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded()) {
        report = LoadReport();
        report.ok = false;
        report.error = "not valid JSON";
        return Graph();
    }
    return fromJson(json, report);
}

std::string jasperengine::graph::fileProblem(std::string_view path, const Graph& graph, const LoadReport& report){
    if (!report.ok) return report.error;
    if (!report.malformed.empty()) return report.malformed.front();
    const std::shared_ptr<const GraphKindDesc> owner = GraphRegistry::get().kindForFile(path);
    if (!owner || owner->id == graph.kind) return {};
    const std::string expected = "these files have kind '" + owner->id + "'";
    return graph.kind.empty() ? "it names no kind: " + expected : "its kind is '" + graph.kind + "': " + expected;
}

std::vector<std::string> jasperengine::graph::fileWarnings(const LoadReport& report){
    // (fromJson adds those about unknown node types last, one each)
    return {report.warnings.begin(), report.warnings.end() - static_cast<std::ptrdiff_t>(report.unknownNodeTypes.size())};
}

std::string jasperengine::graph::save(const Graph& graph){
    std::string text = toJson(graph).dump(2, ' ', false, Json::error_handler_t::replace);
    text += '\n';
    return text;
}

// ------------------------------------------------------------------ validation

const char* jasperengine::graph::severityName(Severity severity){
    switch (severity) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        default: return "error";
    }
}

Json jasperengine::graph::toJson(const Issue& issue){
    Json out = {{"severity", severityName(issue.severity)}, {"code", issue.code}, {"message", issue.message}};
    if (!issue.nodeId.empty()) out["node"] = issue.nodeId;
    if (!issue.portId.empty()) out["port"] = issue.portId;
    if (!issue.edgeId.empty()) out["edge"] = issue.edgeId;
    if (!issue.suggestion.empty()) out["suggestion"] = issue.suggestion;
    return out;
}

std::vector<Issue> jasperengine::graph::validate(const Graph& graph){
    std::vector<Issue> issues;
    auto add = [&](Severity severity, const char* code, std::string message, std::string node = {}, std::string port = {}, std::string edge = {},
                   std::string suggestion = {}) {
        issues.push_back({severity, code, std::move(node), std::move(port), std::move(edge), std::move(message), std::move(suggestion)});
    };
    std::shared_ptr<const GraphKindDesc> kind = GraphRegistry::get().kind(graph.kind);
    const Severity paramSeverity = kind && kind->strictParams ? Severity::Error : Severity::Warning;

    // ids
    std::unordered_set<std::string> seen;
    for (const Node& node : graph.nodes) {
        if (!seen.insert(node.id).second) add(Severity::Error, "duplicate-id", "node id '" + node.id + "' is used twice", node.id);
    }
    seen.clear();
    for (const Edge& edge : graph.edges) {
        if (!seen.insert(edge.id).second) add(Severity::Error, "duplicate-id", "edge id '" + edge.id + "' is used twice", {}, {}, edge.id);
    }
    seen.clear();
    for (const Group& group : graph.groups) {
        if (!seen.insert(group.id).second) add(Severity::Error, "duplicate-id", "group id '" + group.id + "' is used twice");
    }
    seen.clear();
    for (const Variable& variable : graph.variables) {
        if (!seen.insert(variable.name).second) add(Severity::Error, "duplicate-id", "variable '" + variable.name + "' is declared twice");
    }

    // nodes: type and params
    std::unordered_map<std::string, std::vector<Port>> ports;
    for (const Node& node : graph.nodes) {
        const NodeTypeDesc* type = kind ? GraphRegistry::nodeType(*kind, node.type) : nullptr;
        if (kind && !type) {
            std::vector<std::string> typeIds;
            for (const NodeTypeDesc& candidate : kind->nodeTypes) typeIds.push_back(candidate.id);
            add(paramSeverity, "unknown-node-type", "node type '" + node.type + "' is unknown to '" + graph.kind + "': the node is kept as it is", node.id, {}, {},
                didYouMean(node.type, typeIds));
        }
        ports[node.id] = portsOf(graph, node);
        if (!type) continue;
        for (const ParamDesc& param : type->params) {
            auto it = node.params.find(param.name);
            if (it == node.params.end()) continue;
            if (it->is_number()) {
                const double value = it->get<double>();
                if ((param.min && value < *param.min) || (param.max && value > *param.max)) {
                    std::string range = "[" + (param.min ? Json(*param.min).dump() : std::string("-inf")) + ", " +
                                        (param.max ? Json(*param.max).dump() : std::string("inf")) + "]";
                    add(paramSeverity, "param-range", "'" + param.name + "' = " + it->dump() + " is outside " + range, node.id);
                }
            } else if (param.kind == "enum" && it->is_string() && !param.enumValues.empty() &&
                       std::find(param.enumValues.begin(), param.enumValues.end(), it->get<std::string>()) == param.enumValues.end()) {
                std::string suggestion = didYouMean(it->get<std::string>(), param.enumValues);
                if (suggestion.empty()) suggestion = "one of " + listOf(param.enumValues);
                add(paramSeverity, "param-range", "'" + param.name + "' = " + it->dump() + " is not one of its values", node.id, {}, {}, std::move(suggestion));
            }
        }
        if (!kind->strictParams) continue;
        for (auto it = node.params.begin(); it != node.params.end(); ++it) {
            const std::string& name = it.key();
            const auto declared = [&](const ParamDesc& param) { return param.name == name; };
            if (std::any_of(type->params.begin(), type->params.end(), declared)) continue;
            std::vector<std::string> names;
            for (const ParamDesc& param : type->params) names.push_back(param.name);
            std::string suggestion = didYouMean(name, names);
            if (suggestion.empty()) suggestion = names.empty() ? "this node type has no parameters" : "its parameters: " + listOf(names);
            add(Severity::Error, "unknown-param", "'" + name + "' is not a parameter of '" + node.type + "'", node.id, {}, {}, std::move(suggestion));
        }
    }

    auto findPort = [&](const PortRef& ref) -> const Port* {
        auto it = ports.find(ref.node);
        if (it == ports.end()) return nullptr;
        for (const Port& port : it->second) {
            if (port.id == ref.port) return &port;
        }
        return nullptr;
    };
    auto knownType = [&](const std::string& nodeId) {
        const Node* node = graph.findNode(nodeId);
        return node && (node->dynamicPorts || (kind && GraphRegistry::nodeType(*kind, node->type)));
    };

    // edges
    std::map<std::pair<std::string, std::string>, int> inputCounts;
    for (const Edge& edge : graph.edges) {
        const bool fromNode = graph.findNode(edge.from.node) != nullptr;
        const bool toNode = graph.findNode(edge.to.node) != nullptr;
        if (!fromNode || !toNode) {
            add(Severity::Error, "dangling-edge", "edge " + edge.id + " points at a missing node '" + (!fromNode ? edge.from.node : edge.to.node) + "'",
                !fromNode ? edge.from.node : edge.to.node, {}, edge.id);
            continue;
        }
        const Port* from = findPort(edge.from);
        const Port* to = findPort(edge.to);
        // an opaque (unknown type) node's ports are not known: its edges are kept unchecked
        if ((!from && knownType(edge.from.node)) || (!to && knownType(edge.to.node))) {
            const PortRef& missing = !from && knownType(edge.from.node) ? edge.from : edge.to;
            add(Severity::Error, "dangling-edge", "edge " + edge.id + " points at a missing port '" + missing.port + "' of " + missing.node,
                missing.node, missing.port, edge.id);
            continue;
        }
        if (!from || !to) continue;
        if (from->direction != PortDirection::Out || to->direction != PortDirection::In) {
            add(Severity::Error, "direction", "edge " + edge.id + " must go from an output to an input", edge.to.node, edge.to.port, edge.id);
            continue;
        }
        if (!GraphRegistry::canConnect(kind.get(), from->type, to->type)) {
            add(Severity::Error, "type-mismatch", "edge " + edge.id + ": " + from->type + " output cannot feed a " + to->type + " input",
                edge.to.node, edge.to.port, edge.id);
        }
        const int count = ++inputCounts[{edge.to.node, edge.to.port}];
        if (count == 2 && !to->multi) {
            add(Severity::Error, "multiplicity", "input '" + edge.to.port + "' of " + edge.to.node + " takes one edge", edge.to.node, edge.to.port, edge.id);
        }
    }

    // required inputs
    for (const Node& node : graph.nodes) {
        for (const Port& port : ports[node.id]) {
            if (port.direction != PortDirection::In || port.optional) continue;
            if (inputCounts.count({node.id, port.id}) == 0) {
                add(Severity::Warning, "missing-required-input", "input '" + (port.name.empty() ? port.id : port.name) + "' of " + node.id + " is not connected",
                    node.id, port.id);
            }
        }
    }

    // cycles (Kahn's algorithm: what is left over sits on a cycle)
    if (kind && kind->acyclic) {
        std::unordered_map<std::string, int> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> out;
        for (const Node& node : graph.nodes) inDegree[node.id];
        for (const Edge& edge : graph.edges) {
            if (!inDegree.count(edge.from.node) || !inDegree.count(edge.to.node)) continue;
            out[edge.from.node].push_back(edge.to.node);
            inDegree[edge.to.node]++;
        }
        std::vector<std::string> ready;
        for (const auto& [id, degree] : inDegree) {
            if (degree == 0) ready.push_back(id);
        }
        size_t visited = 0;
        while (!ready.empty()) {
            const std::string id = ready.back();
            ready.pop_back();
            visited++;
            for (const std::string& next : out[id]) {
                if (--inDegree[next] == 0) ready.push_back(next);
            }
        }
        if (visited < inDegree.size()) {
            std::vector<std::string> onCycle;
            for (const auto& [id, degree] : inDegree) {
                if (degree > 0) onCycle.push_back(id);
            }
            std::sort(onCycle.begin(), onCycle.end(), [](const std::string& a, const std::string& b) { return idLess(a, b); });
            std::string list;
            for (const std::string& id : onCycle) list += (list.empty() ? "" : ", ") + id;
            add(Severity::Error, "cycle", "'" + graph.kind + "' graphs must not loop; these nodes are on a cycle: " + list, onCycle.front());
        }
    }

    if (kind && kind->validate) kind->validate(graph, issues);
    return issues;
}

// ------------------------------------------------------------------ edit operations

GraphOp jasperengine::graph::opFromJson(const Json& json){
    GraphOp op;
    if (!json.is_object()) throw GraphOpError("an op is an object {op, ...args}");
    op.op = stringOf(json, "op");
    if (op.op.empty()) throw GraphOpError("an op needs its name in 'op'");
    op.args = Json::object();
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() == "op") continue;
        if (it.key() == "args" && it->is_object()) {
            for (auto arg = it->begin(); arg != it->end(); ++arg) op.args[arg.key()] = arg.value();
        } else {
            op.args[it.key()] = it.value();
        }
    }
    return op;
}

Json jasperengine::graph::toJson(const GraphOp& op){
    Json out = {{"op", op.op}};
    for (auto it = op.args.begin(); it != op.args.end(); ++it) out[it.key()] = it.value();
    return out;
}

GraphOp jasperengine::graph::apply(Graph& graph, const GraphOp& op){
    const uint64_t nextIdBefore = graph.nextId;

    if (op.op == "addNode") {
        Node node = nodeFromJson(requireArg(op, "node"));
        if (node.type.empty()) throw GraphOpError("addNode: the node needs a 'type'");
        std::vector<Edge> edges;
        if (auto it = op.args.find("edges"); it != op.args.end() && it->is_array()) {
            for (const Json& item : *it) edges.push_back(edgeFromJson(item));
        }
        for (const Edge& edge : edges) {
            if (edge.id.empty() || graph.findEdge(edge.id)) throw GraphOpError("addNode: edge ids must be new");
        }
        if (node.id.empty()) {
            node.id = graph.allocateId("n");
        } else if (graph.findNode(node.id)) {
            throw GraphOpError("addNode: a node '" + node.id + "' exists");
        }
        // a new node gets its type's parameter defaults, and stores its ports when the type's
        // ports are dynamic; a node put back by an undo (restore) comes back exactly as it was
        if (!op.args.value("restore", false)) {
            if (auto kind = GraphRegistry::get().kind(graph.kind)) {
                if (const NodeTypeDesc* type = GraphRegistry::nodeType(*kind, node.type)) {
                    for (const ParamDesc& param : type->params) {
                        if (!node.params.contains(param.name) && !param.defaultValue.is_null()) node.params[param.name] = param.defaultValue;
                    }
                    if (type->dynamicPorts && !node.dynamicPorts) node.dynamicPorts = type->dynamicPorts(node.params);
                }
            }
        }
        const std::string id = node.id;
        noteId(graph, id);
        insertSorted(graph.nodes, std::move(node));
        for (Edge& edge : edges) {
            noteId(graph, edge.id);
            insertSorted(graph.edges, std::move(edge));
        }
        if (auto it = op.args.find("groups"); it != op.args.end() && it->is_array()) {
            for (const Json& membership : *it) {
                Group* group = graph.findGroup(membership.value("id", std::string()));
                if (!group) continue;
                const size_t index = std::min<size_t>(membership.value("index", group->nodeIds.size()), group->nodeIds.size());
                group->nodeIds.insert(group->nodeIds.begin() + static_cast<std::ptrdiff_t>(index), id);
            }
        }
        return {"removeNode", {{"id", id}, {"nextId", nextIdBefore}}};
    }

    if (op.op == "removeNode") {
        const std::string id = requireString(op, "id");
        auto it = lowerById(graph.nodes, id);
        if (it == graph.nodes.end() || it->id != id) throw GraphOpError("removeNode: no node '" + id + "'");
        Json inverse = {{"node", toJson(*it)}, {"restore", true}};
        Json edges = Json::array();
        graph.edges.erase(std::remove_if(graph.edges.begin(), graph.edges.end(), [&](const Edge& edge) {
            if (edge.from.node != id && edge.to.node != id) return false;
            edges.push_back(toJson(edge));
            return true;
        }), graph.edges.end());
        if (!edges.empty()) inverse["edges"] = std::move(edges);
        Json groups = Json::array();
        for (Group& group : graph.groups) {
            auto member = std::find(group.nodeIds.begin(), group.nodeIds.end(), id);
            if (member == group.nodeIds.end()) continue;
            groups.push_back({{"id", group.id}, {"index", static_cast<size_t>(member - group.nodeIds.begin())}});
            group.nodeIds.erase(member);
        }
        if (!groups.empty()) inverse["groups"] = std::move(groups);
        graph.nodes.erase(it);
        restoreNextId(graph, op);
        return {"addNode", std::move(inverse)};
    }

    if (op.op == "moveNodes") {
        std::vector<std::pair<Node*, Vec2>> moves;
        if (auto positions = op.args.find("positions"); positions != op.args.end() && positions->is_object()) {
            for (auto it = positions->begin(); it != positions->end(); ++it) {
                Node* node = graph.findNode(it.key());
                if (!node) throw GraphOpError("moveNodes: no node '" + it.key() + "'");
                moves.push_back({node, vec2From(it.value())});
            }
        } else {
            const Json& ids = requireArg(op, "ids");
            const Vec2 delta = vec2From(requireArg(op, "delta"));
            if (!ids.is_array()) throw GraphOpError("moveNodes: 'ids' is a list of node ids");
            for (const Json& id : ids) {
                Node* node = id.is_string() ? graph.findNode(id.get<std::string>()) : nullptr;
                if (!node) throw GraphOpError("moveNodes: no node " + id.dump());
                moves.push_back({node, {node->position.x + delta.x, node->position.y + delta.y}});
            }
        }
        Json old = Json::object();
        for (auto& [node, position] : moves) {
            old[node->id] = vec2Json(node->position);
            node->position = position;
        }
        return {"moveNodes", {{"positions", std::move(old)}}};
    }

    if (op.op == "setParam") {
        const std::string nodeId = requireString(op, "node");
        const std::string name = requireString(op, "name");
        Node* node = graph.findNode(nodeId);
        if (!node) throw GraphOpError("setParam: no node '" + nodeId + "'");
        auto value = op.args.find("value");
        Json previous;
        auto current = node->params.find(name);
        if (current != node->params.end()) previous = *current;
        if (value == op.args.end() || value->is_null()) {
            node->params.erase(name);
        } else {
            node->params[name] = *value;
        }
        return {"setParam", {{"node", nodeId}, {"name", name}, {"value", previous}}};
    }

    if (op.op == "connect") {
        Edge edge;
        if (auto full = op.args.find("edge"); full != op.args.end() && full->is_object()) {
            edge = edgeFromJson(*full);
        } else {
            edge.from = portRefFrom(requireArg(op, "from"));
            edge.to = portRefFrom(requireArg(op, "to"));
            edge.id = op.args.value("id", std::string());
        }
        if (edge.from.node.empty() || edge.from.port.empty() || edge.to.node.empty() || edge.to.port.empty()) {
            throw GraphOpError("connect: 'from' and 'to' are {node, port}");
        }
        if (!graph.findNode(edge.from.node) || !graph.findNode(edge.to.node)) {
            throw GraphOpError("connect: no node '" + (graph.findNode(edge.from.node) ? edge.to.node : edge.from.node) + "'");
        }
        if (edge.id.empty()) {
            edge.id = graph.allocateId("e");
        } else if (graph.findEdge(edge.id)) {
            throw GraphOpError("connect: an edge '" + edge.id + "' exists");
        }
        noteId(graph, edge.id);
        const std::string id = edge.id;
        insertSorted(graph.edges, std::move(edge));
        return {"disconnect", {{"id", id}, {"nextId", nextIdBefore}}};
    }

    if (op.op == "disconnect") {
        std::vector<Edge>::iterator it = graph.edges.end();
        if (auto id = op.args.find("id"); id != op.args.end() && id->is_string()) {
            it = lowerById(graph.edges, id->get<std::string>());
            if (it != graph.edges.end() && it->id != id->get<std::string>()) it = graph.edges.end();
        } else {
            const PortRef from = portRefFrom(requireArg(op, "from"));
            const PortRef to = portRefFrom(requireArg(op, "to"));
            it = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const Edge& edge) {
                return edge.from.node == from.node && edge.from.port == from.port && edge.to.node == to.node && edge.to.port == to.port;
            });
        }
        if (it == graph.edges.end()) throw GraphOpError("disconnect: no such edge");
        GraphOp inverse{"connect", {{"edge", toJson(*it)}}};
        graph.edges.erase(it);
        restoreNextId(graph, op);
        return inverse;
    }

    if (op.op == "addVariable") {
        Variable variable = variableFromJson(requireArg(op, "variable"));
        if (variable.name.empty()) throw GraphOpError("addVariable: the variable needs a 'name'");
        if (graph.findVariable(variable.name)) throw GraphOpError("addVariable: a variable '" + variable.name + "' exists");
        const size_t index = std::min<size_t>(op.args.value("index", graph.variables.size()), graph.variables.size());
        const std::string name = variable.name;
        graph.variables.insert(graph.variables.begin() + static_cast<std::ptrdiff_t>(index), std::move(variable));
        return {"removeVariable", {{"name", name}}};
    }

    if (op.op == "removeVariable") {
        const std::string name = requireString(op, "name");
        auto it = std::find_if(graph.variables.begin(), graph.variables.end(), [&](const Variable& v) { return v.name == name; });
        if (it == graph.variables.end()) throw GraphOpError("removeVariable: no variable '" + name + "'");
        GraphOp inverse{"addVariable", {{"variable", toJson(*it)}, {"index", static_cast<size_t>(it - graph.variables.begin())}}};
        graph.variables.erase(it);
        return inverse;
    }

    if (op.op == "renameVariable") {
        const std::string from = requireString(op, "from");
        const std::string to = requireString(op, "to");
        Variable* variable = graph.findVariable(from);
        if (!variable) throw GraphOpError("renameVariable: no variable '" + from + "'");
        if (from != to && graph.findVariable(to)) throw GraphOpError("renameVariable: a variable '" + to + "' exists");
        variable->name = to;
        return {"renameVariable", {{"from", to}, {"to", from}}};
    }

    if (op.op == "setVariable") {
        const std::string name = requireString(op, "name");
        Variable* variable = graph.findVariable(name);
        if (!variable) throw GraphOpError("setVariable: no variable '" + name + "'");
        Json inverse = {{"name", name}};
        if (auto it = op.args.find("type"); it != op.args.end() && it->is_string()) {
            inverse["type"] = variable->type;
            variable->type = it->get<std::string>();
        }
        if (auto it = op.args.find("default"); it != op.args.end()) {
            inverse["default"] = variable->defaultValue;
            variable->defaultValue = *it;
        }
        if (auto it = op.args.find("exposed"); it != op.args.end() && it->is_boolean()) {
            inverse["exposed"] = variable->exposed;
            variable->exposed = it->get<bool>();
        }
        if (auto it = op.args.find("group"); it != op.args.end() && it->is_string()) {
            inverse["group"] = variable->group;
            variable->group = it->get<std::string>();
        }
        return {"setVariable", std::move(inverse)};
    }

    if (op.op == "group") {
        Group group;
        size_t index = graph.groups.size();
        if (auto full = op.args.find("group"); full != op.args.end() && full->is_object()) {
            group = groupFromJson(*full);
            index = std::min<size_t>(op.args.value("index", graph.groups.size()), graph.groups.size());
        } else {
            group.title = op.args.value("title", std::string("Group"));
            const Json& nodes = requireArg(op, "nodes");
            if (!nodes.is_array()) throw GraphOpError("group: 'nodes' is a list of node ids");
            for (const Json& id : nodes) {
                if (!id.is_string() || !graph.findNode(id.get<std::string>())) throw GraphOpError("group: no node " + id.dump());
                group.nodeIds.push_back(id.get<std::string>());
            }
            if (auto rect = op.args.find("rect"); rect != op.args.end() && rect->is_array() && rect->size() >= 4) {
                group.rect = {(*rect)[0].get<double>(), (*rect)[1].get<double>(), (*rect)[2].get<double>(), (*rect)[3].get<double>()};
            }
            group.id = op.args.value("id", std::string());
        }
        if (group.id.empty()) {
            group.id = graph.allocateId("g");
        } else if (graph.findGroup(group.id)) {
            throw GraphOpError("group: a group '" + group.id + "' exists");
        }
        noteId(graph, group.id);
        const std::string id = group.id;
        graph.groups.insert(graph.groups.begin() + static_cast<std::ptrdiff_t>(index), std::move(group));
        return {"ungroup", {{"id", id}, {"nextId", nextIdBefore}}};
    }

    if (op.op == "ungroup") {
        const std::string id = requireString(op, "id");
        auto it = std::find_if(graph.groups.begin(), graph.groups.end(), [&](const Group& g) { return g.id == id; });
        if (it == graph.groups.end()) throw GraphOpError("ungroup: no group '" + id + "'");
        GraphOp inverse{"group", {{"group", toJson(*it)}, {"index", static_cast<size_t>(it - graph.groups.begin())}}};
        graph.groups.erase(it);
        restoreNextId(graph, op);
        return inverse;
    }

    throw GraphOpError("unknown op '" + op.op + "' (addNode, removeNode, moveNodes, setParam, connect, disconnect, addVariable, "
                       "removeVariable, renameVariable, setVariable, group, ungroup)");
}

std::string jasperengine::graph::describe(const Graph& graph, const GraphOp& op){
    if (op.op == "addNode") {
        const Json& node = op.args.contains("node") ? op.args["node"] : Json::object();
        std::string label = node.value("type", std::string("Node"));
        if (auto kind = GraphRegistry::get().kind(graph.kind)) {
            if (const NodeTypeDesc* type = GraphRegistry::nodeType(*kind, label); type && !type->displayName.empty()) label = type->displayName;
        }
        return "Add " + label;
    }
    if (op.op == "removeNode") return "Delete " + nodeLabel(graph, op.args.value("id", std::string()));
    if (op.op == "moveNodes") {
        size_t count = op.args.contains("positions") ? op.args["positions"].size() : op.args.value("ids", Json::array()).size();
        return count == 1 ? "Move Node" : "Move " + std::to_string(count) + " Nodes";
    }
    if (op.op == "setParam") return "Set " + op.args.value("name", std::string("Parameter"));
    if (op.op == "connect") {
        PortRef from, to;
        if (op.args.contains("edge")) {
            const Edge edge = edgeFromJson(op.args["edge"]);
            from = edge.from;
            to = edge.to;
        } else {
            from = portRefFrom(op.args.value("from", Json::object()));
            to = portRefFrom(op.args.value("to", Json::object()));
        }
        return "Connect " + portLabel(graph, from) + " \xE2\x86\x92 " + nodeLabel(graph, to.node);
    }
    if (op.op == "disconnect") return "Disconnect";
    if (op.op == "addVariable") return "Add Variable " + op.args.value("variable", Json::object()).value("name", std::string());
    if (op.op == "removeVariable") return "Delete Variable " + op.args.value("name", std::string());
    if (op.op == "renameVariable") return "Rename Variable " + op.args.value("from", std::string()) + " to " + op.args.value("to", std::string());
    if (op.op == "setVariable") return "Set Variable " + op.args.value("name", std::string());
    if (op.op == "group") return "Group Nodes";
    if (op.op == "ungroup") return "Ungroup";
    return "Edit Graph";
}
