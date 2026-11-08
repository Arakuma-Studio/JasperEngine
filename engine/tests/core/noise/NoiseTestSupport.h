// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_TESTSUPPORT_H
#define NOISE_TESTSUPPORT_H

#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace noisetest {

    using namespace jasperengine;
    using graph::Json;

    // A graph, built node by node
    class GraphBuilder {
    public:
        GraphBuilder() {
            graphData.kind = "noise";
            graphData.kindVersion = 1;
        }

        std::string node(const std::string& type, const Json& params = Json::object()) {
            graph::Node added;
            added.id = graphData.allocateId("n");
            added.type = type;
            if (!params.is_null()) added.params = params;
            graphData.nodes.push_back(added);
            return added.id;
        }

        void edge(const std::string& from, const std::string& to, const std::string& toPort = "in", const std::string& fromPort = "out") {
            graph::Edge added;
            added.id = graphData.allocateId("e");
            added.from.node = from;
            added.from.port = fromPort;
            added.to.node = to;
            added.to.port = toPort;
            graphData.edges.push_back(added);
        }

        // node -> a Value Output named name
        std::string output(const std::string& from, const std::string& name = "value", const std::string& fromPort = "out") {
            const std::string out = node("output_value", {{"name", name}});
            edge(from, out, "in", fromPort);
            return out;
        }

        void variable(const std::string& name, double value, bool exposed = true) {
            graph::Variable added;
            added.name = name;
            added.type = "float";
            added.defaultValue = value;
            added.exposed = exposed;
            graphData.variables.push_back(added);
        }

        graph::Graph& graph() { return graphData; }

    private:
        graph::Graph graphData;
    };

    inline noise::CompileResult compiled(const graph::Graph& graph, const noise::CompileOptions& options = {}) {
        return noise::compile(graph, options);
    }

    inline bool hasIssue(const noise::CompileResult& result, const std::string& code, const std::string& node = {}) {
        for (const graph::Issue& issue : result.issues) {
            if (issue.code == code && (node.empty() || issue.nodeId == node)) return true;
        }
        return false;
    }

    inline uint32_t bitsOf(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    inline std::vector<float> gridOf(const noise::CompiledGraph& compiledGraph, const noise::GridDomain& domain, noise::EvalContext context = {}, int output = 0) {
        std::vector<float> values(static_cast<size_t>(domain.width) * static_cast<size_t>(domain.depth));
        compiledGraph.evaluateGrid(domain, context, output, values.data());
        return values;
    }

    // A pool of that many workers for one test; the next test gets a default one back
    class WorkerScope {
    public:
        explicit WorkerScope(int workers) {
            jobs::shutdown();
            jobs::Config config;
            config.workers = workers;
            jobs::initialize(config);
        }
        ~WorkerScope() {
            jobs::shutdown();
            jobs::initialize(jobs::Config{});
        }
    };

}

#endif // NOISE_TESTSUPPORT_H
