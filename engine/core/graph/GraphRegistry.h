// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_GRAPH_REGISTRY_H
#define JASPERENGINE_GRAPH_REGISTRY_H

// What graphs of a kind mean: their node types (ports, parameters), port types and the
// conversions between them, the kind's version with its migrations, and extra validation.
// A tool registers its kind once (the editor at start, a game for the kinds it compiles);
// graph files name their kind by id.

#include "Graph.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::graph {

    struct ParamDesc {
        std::string name;
        // float, int, bool, string, enum, vector2, vector3, color ({r, g, b, a} or [r, g, b, a],
        // as shown: sRGB), asset ({guid, path}), gradient ({points: [{pos, r, g, b, a}],
        // interpolation 0 constant, 1 linear, 2 and 3 smooth}), curve ({points: [{x, y, ls, rs}]},
        // the slopes left and right of each point), any
        std::string kind;
        Json defaultValue;
        std::optional<double> min;
        std::optional<double> max;
        std::vector<std::string> enumValues;
    };

    struct NodeTypeDesc {
        std::string id;
        std::string category;
        std::string displayName;
        std::vector<std::string> aliases;    // other names a search finds it by
        std::vector<Port> ports;             // the static ports (passthrough unused)
        std::vector<ParamDesc> params;
        // set for dynamic-port types: the ports a node with these params has; such nodes store
        // their ports in the file
        std::function<std::vector<Port>(const Json& params)> dynamicPorts;
    };

    struct PortTypeDesc {
        std::string id;
        uint32_t color = 0xffffffffu;        // RGBA, for the canvas
        std::vector<std::string> implicitTo; // the input types an output of this type may feed
    };

    struct Migration {
        int toVersion = 0;
        std::function<void(Json& graph)> fn; // edits the raw file JSON
    };

    struct GraphKindDesc {
        std::string id;
        std::string displayName;
        std::string fileExtension;           // ".shadergraph"
        int kindVersion = 1;
        std::vector<Migration> migrations;   // ascending toVersion
        bool acyclic = false;
        // Unknown parameters and node types and out-of-range values are errors, so edit_graph
        // refuses them. Off, they stay S23's forward compatibility: what a newer build wrote is
        // kept and at most warned about. Either way load() never refuses a file for them.
        bool strictParams = false;
        std::vector<NodeTypeDesc> nodeTypes;
        std::vector<PortTypeDesc> portTypes;
        // kind-specific issues, after the generic ones
        std::function<void(const Graph& graph, std::vector<Issue>& issues)> validate;
    };

    // Thread-safe; descriptors are immutable once registered (looked up by shared pointer)
    class JASPERENGINE_API GraphRegistry {
    public:
        static GraphRegistry& get();

        // Replaces a kind of the same id
        void registerKind(GraphKindDesc kind);
        void unregisterKind(const std::string& id);

        std::shared_ptr<const GraphKindDesc> kind(const std::string& id) const;
        std::shared_ptr<const GraphKindDesc> kindForExtension(const std::string& extension) const;
        // The kind whose extension ends this path, whatever its letter case; null for none
        std::shared_ptr<const GraphKindDesc> kindForFile(std::string_view path) const;
        std::vector<std::string> kindIds() const;

        static const NodeTypeDesc* nodeType(const GraphKindDesc& kind, const std::string& typeId);
        static const PortTypeDesc* portType(const GraphKindDesc& kind, const std::string& typeId);
        // An output of type `out` may feed an input of type `in`: equal, a declared implicit
        // conversion, or either is "any"
        static bool canConnect(const GraphKindDesc* kind, const std::string& out, const std::string& in);

    private:
        mutable std::mutex mutex;
        std::map<std::string, std::shared_ptr<const GraphKindDesc>> kinds;
    };

    JASPERENGINE_API Json toJson(const GraphKindDesc& kind);
    JASPERENGINE_API Json toJson(const NodeTypeDesc& type);

}

#endif // JASPERENGINE_GRAPH_REGISTRY_H
