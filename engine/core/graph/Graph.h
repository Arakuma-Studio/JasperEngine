// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_GRAPH_H
#define JASPERENGINE_GRAPH_H

// The generic graph data model for dataflow tools (shader, VFX, dialogue, tooling graphs):
// nodes with typed ports, edges, variables and groups, stored as JSON. What a graph *means*
// belongs to its kind (GraphRegistry.h); this file is the data, its JSON form, validation and
// the undoable edit operations. Unknown keys at every level are written back verbatim, an
// unknown node type is kept as it is, and the output is deterministic. No canvas here: the
// editor's graph canvas draws this model.

#include "Export.h"
#include "json.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::graph {

    using Json = nlohmann::ordered_json;

    inline constexpr int FORMAT_VERSION = 1;

    enum class PortDirection { In, Out };

    struct Vec2 {
        double x = 0.0;
        double y = 0.0;
        bool operator==(const Vec2&) const = default;
    };

    struct Rect {
        double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
        bool operator==(const Rect&) const = default;
    };

    // A port, as a node type declares it; stored in a node only for dynamic-port types
    struct Port {
        std::string id;
        PortDirection direction = PortDirection::In;
        std::string type;
        std::string name;
        bool multi = false;      // an input taking several edges / an output allowed to fan out (outputs always may)
        bool optional = false;   // an input that may stay unconnected
        Json passthrough = Json::object();
    };

    struct PortRef {
        std::string node;
        std::string port;
        Json passthrough = Json::object();
    };

    struct Node {
        std::string id;                 // "n12", never reused within a file
        std::string type;               // a node type id of the graph's kind
        Vec2 position;
        // false: the file gives no position (an agent wrote the node without one). Saved without
        // one too, so editors keep laying it out (layoutPositions) until it is moved (S90 R30)
        bool placed = true;
        std::optional<Vec2> size;
        Json params = Json::object();   // name -> value (asset references: {guid, path})
        std::optional<std::vector<Port>> dynamicPorts;
        Json passthrough = Json::object();
    };

    struct Edge {
        std::string id;                 // "e7"
        PortRef from;                   // an output
        PortRef to;                     // an input
        Json passthrough = Json::object();
    };

    struct Variable {
        std::string name;
        std::string type;
        Json defaultValue;
        bool exposed = false;
        std::string group;
        Json passthrough = Json::object();
    };

    struct Group {
        std::string id;                 // "g3"
        std::string title;
        Rect rect;
        std::vector<std::string> nodeIds;
        Json passthrough = Json::object();
    };

    struct Viewport {
        Vec2 offset;
        double zoom = 1.0;
        Json passthrough = Json::object();
    };

    struct Graph {
        int formatVersion = FORMAT_VERSION;
        std::string kind;
        int kindVersion = 1;
        uint64_t nextId = 1;            // the next "n<id>" / "e<id>" / "g<id>": ids are never reused
        std::vector<Node> nodes;        // in id order
        std::vector<Edge> edges;        // in id order
        std::vector<Variable> variables;
        std::vector<Group> groups;
        Viewport viewport;
        Json metadata = Json::object();
        Json passthrough = Json::object();

        Node* findNode(std::string_view id);
        const Node* findNode(std::string_view id) const;
        Edge* findEdge(std::string_view id);
        const Edge* findEdge(std::string_view id) const;
        Variable* findVariable(std::string_view name);
        const Variable* findVariable(std::string_view name) const;
        Group* findGroup(std::string_view id);
        // a fresh id with that prefix ("n", "e", "g")
        std::string allocateId(const char* prefix);
    };

    // Ids in file order: the prefix, then the number ("n2" before "n10"), then the text
    JASPERENGINE_API bool idLess(std::string_view a, std::string_view b);

    struct LoadReport {
        bool ok = true;
        std::string error;                       // not ok: why (not JSON, not a graph, a newer format)
        std::vector<std::string> warnings;
        std::vector<std::string> unknownNodeTypes;   // kept as opaque nodes
        bool unknownKind = false;
        int migratedFrom = 0;                    // the kindVersion migrated from, 0 when none ran
        std::vector<std::string> malformed;      // known keys of the wrong type: their values are not read, a save loses them
    };

    // Reads a graph file. The kind's migrations (GraphRegistry) bring an older kindVersion up to
    // date; unknown keys, node types and kinds are kept; an edge without an id gets one.
    JASPERENGINE_API Graph load(std::string_view json, LoadReport& report);
    JASPERENGINE_API Graph fromJson(const Json& json, LoadReport& report);
    // Why this read of the file at `path` is not a file of that path's kind: not a graph, a key
    // of the wrong type, or another kind than the one its extension belongs to (the kind decides
    // what is checked, so a file must not choose it); "" when it is
    JASPERENGINE_API std::string fileProblem(std::string_view path, const Graph& graph, const LoadReport& report);
    // The load's warnings a caller should pass on: validate() reports the unknown node types itself
    JASPERENGINE_API std::vector<std::string> fileWarnings(const LoadReport& report);
    // Deterministic: known keys in a fixed order, then the unknown ones as read; nodes and edges
    // in id order; two-space indent, LF line endings, a final newline
    JASPERENGINE_API std::string save(const Graph& graph);
    JASPERENGINE_API Json toJson(const Graph& graph);
    JASPERENGINE_API Json toJson(const Node& node);
    JASPERENGINE_API Json toJson(const Edge& edge);
    JASPERENGINE_API Json toJson(const Variable& variable);
    JASPERENGINE_API Json toJson(const Group& group);
    JASPERENGINE_API Node nodeFromJson(const Json& json);
    JASPERENGINE_API Edge edgeFromJson(const Json& json);
    JASPERENGINE_API Variable variableFromJson(const Json& json);
    JASPERENGINE_API Group groupFromJson(const Json& json);

    // ---- validation

    enum class Severity { Info, Warning, Error };

    struct Issue {
        Severity severity = Severity::Error;
        std::string code;       // dangling-edge, duplicate-id, direction, type-mismatch, multiplicity, cycle,
                                // unknown-node-type, missing-required-input, param-range, unknown-param
                                // (kinds with strictParams only), or a kind's own
        std::string nodeId;
        std::string portId;
        std::string edgeId;
        std::string message;
        std::string suggestion; // what to do instead ("did you mean 'value'?"), "" when nothing is known
    };

    JASPERENGINE_API std::vector<Issue> validate(const Graph& graph);
    JASPERENGINE_API const char* severityName(Severity severity);
    JASPERENGINE_API Json toJson(const Issue& issue);

    // The ports a node has: its type's (and dynamic ports when the type has them); empty for
    // an unknown type
    JASPERENGINE_API std::vector<Port> portsOf(const Graph& graph, const Node& node);

    // ---- edit operations

    // One edit: {op, args}. apply() returns its inverse, so apply(apply(g, op)) restores g byte
    // for byte. Ops and their args:
    //   addNode {node (id optional: allocated), edges?, restore?: true (an undo's: the node as given,
    //                 no defaults)}                          removeNode {id} (and its edges)
    //   moveNodes {ids, delta: [x, y]} or {positions: {id: [x, y] or null (no position)}}
    //   layout {ids?} (the nodes without a position, or `ids`: placed by layoutPositions)
    //   setParam {node, name, value} (value null removes)    connect {from: {node, port}, to: {...}, id?}
    //   disconnect {id} or {from, to}                         addVariable {variable}
    //   removeVariable {name}                                 renameVariable {from, to}
    //   setVariable {name, type?, default?, exposed?, group?} group {title, nodes, rect?, id?}
    //   ungroup {id}                                          setMetadata {key, value} (value null removes)
    // A bad op (unknown node, missing args) throws GraphOpError and changes nothing.
    struct GraphOp {
        std::string op;
        Json args = Json::object();
    };

    class JASPERENGINE_API GraphOpError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    JASPERENGINE_API GraphOp apply(Graph& graph, const GraphOp& op);

    // Where an automatic layout puts `ids` (S90 R30): left to right by their longest path from a
    // source among them, a column per step, each column in the order of the rows feeding it.
    // Beside the other placed nodes, to their right, so nothing lands on them. Deterministic. A
    // node takes LAYOUT_ROW plus LAYOUT_PORT_ROW per port row of its column: room for an editor's
    // header, port rows and a 112-unit image (the Graph Editor's node is 150 + 20 per row tall).
    inline constexpr double LAYOUT_COLUMN = 280.0;
    inline constexpr double LAYOUT_ROW = 180.0;
    inline constexpr double LAYOUT_PORT_ROW = 20.0;
    JASPERENGINE_API std::vector<std::pair<std::string, Vec2>> layoutPositions(const Graph& graph, const std::vector<std::string>& ids);
    // The ids of the nodes without a position, in file order
    JASPERENGINE_API std::vector<std::string> unplacedNodes(const Graph& graph);
    JASPERENGINE_API GraphOp opFromJson(const Json& json);
    JASPERENGINE_API Json toJson(const GraphOp& op);
    // "Connect Speed → Blend", "Move 3 Nodes": the undo name of an op
    JASPERENGINE_API std::string describe(const Graph& graph, const GraphOp& op);

}

#endif // JASPERENGINE_GRAPH_H
