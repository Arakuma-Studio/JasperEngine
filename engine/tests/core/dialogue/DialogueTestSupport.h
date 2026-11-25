// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Dialogue graphs written in a test: nodes and wires by id, saved as a .dialogue text; and a
// transcript of what a runner shows.

#ifndef DIALOGUE_TEST_SUPPORT_H
#define DIALOGUE_TEST_SUPPORT_H

#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueProgram.h"
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueVariables.h"
#include "graph/Graph.h"

#include <memory>
#include <string>
#include <vector>

namespace dialoguetest {

    using jasperengine::graph::Json;

    struct GraphText {
        Json nodes = Json::array();
        Json edges = Json::array();
        Json variables = Json::array();
        std::string ns = "t";
        int nextEdge = 500;

        GraphText& node(const std::string& id, const std::string& type, Json params = Json::object()) {
            Json node = {{"id", id}, {"type", type}, {"position", {0.0, 0.0}}, {"params", params}};
            if (type == "line") {
                Json ports = Json::array();
                for (const jasperengine::graph::Port& port : jasperengine::dialogue::linePorts(params)) {
                    Json p = {{"id", port.id}, {"direction", port.direction == jasperengine::graph::PortDirection::In ? "in" : "out"}, {"type", port.type}};
                    if (!port.name.empty()) p["name"] = port.name;
                    if (port.multi) p["multi"] = true;
                    if (port.optional) p["optional"] = true;
                    ports.push_back(p);
                }
                node["ports"] = ports;
            }
            nodes.push_back(node);
            return *this;
        }
        GraphText& wire(const std::string& from, const std::string& port, const std::string& to) {
            edges.push_back({{"id", "e" + std::to_string(nextEdge++)}, {"from", {{"node", from}, {"port", port}}}, {"to", {{"node", to}, {"port", "in"}}}});
            return *this;
        }
        GraphText& variable(const std::string& name, Json value) {
            variables.push_back({{"name", name}, {"type", value.is_boolean() ? "bool" : value.is_string() ? "string" : "number"}, {"default", value}});
            return *this;
        }
        std::string text(const std::string& guid = "") const {
            Json graph = {{"formatVersion", 1}, {"kind", "dialogue"}, {"kindVersion", 1}, {"nextId", 1000}, {"nodes", nodes}, {"edges", edges},
                          {"variables", variables}, {"metadata", {{"namespace", ns}}}};
            if (!guid.empty()) graph["guid"] = guid;
            return graph.dump(2);
        }
        jasperengine::graph::Graph graph(const std::string& guid = "") const {
            jasperengine::graph::LoadReport report;
            return jasperengine::graph::load(text(guid), report);
        }
        std::shared_ptr<const jasperengine::dialogue::Program> program(const std::string& path = "test.dialogue") const {
            return jasperengine::dialogue::Programs::fromText(path, text());
        }
    };

    inline std::string lineText(const jasperengine::dialogue::Line& line) {
        std::string out = "line " + line.node + " " + line.speaker + ": " + line.text;
        for (const jasperengine::dialogue::Option& option : line.options) {
            out += " [" + std::to_string(option.index) + " " + option.text + (option.enabled ? "" : " (disabled)") + "]";
        }
        return out;
    }

    // what a runner shows, a line per event
    struct Transcript {
        std::vector<std::string> lines;

        void attach(jasperengine::dialogue::Runner& runner) {
            runner.onStart = [this](jasperengine::dialogue::Runner&) { lines.push_back("start"); };
            runner.onLine = [this](jasperengine::dialogue::Runner&, const jasperengine::dialogue::Line& line) { lines.push_back(lineText(line)); };
            runner.onEnd = [this](jasperengine::dialogue::Runner&, const std::string& result) { lines.push_back("end " + result); };
        }
        std::string text() const {
            std::string out;
            for (const std::string& line : lines) out += line + "\n";
            return out;
        }
    };

}

#endif //DIALOGUE_TEST_SUPPORT_H
