// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "noise/NoiseNodes.h"
#include "terrain/TerrainLayers.h"

#include <cstdint>
#include <set>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::graph;

namespace {

    Port inPort(const char* id, const char* type, const char* name, bool optional = false) {
        Port port;
        port.id = id;
        port.direction = PortDirection::In;
        port.type = type;
        port.name = name;
        port.optional = optional;
        return port;
    }

    Port outPort(const char* id, const char* type, const char* name) {
        Port port;
        port.id = id;
        port.direction = PortDirection::Out;
        port.type = type;
        port.name = name;
        port.multi = true;
        return port;
    }

    ParamDesc floatParam(const char* name, double value, std::optional<double> min = std::nullopt, std::optional<double> max = std::nullopt) {
        ParamDesc param;
        param.name = name;
        param.kind = "float";
        param.defaultValue = value;
        param.min = min;
        param.max = max;
        return param;
    }

    ParamDesc intParam(const char* name, int value, std::optional<double> min = std::nullopt, std::optional<double> max = std::nullopt) {
        ParamDesc param;
        param.name = name;
        param.kind = "int";
        param.defaultValue = value;
        param.min = min;
        param.max = max;
        return param;
    }

    ParamDesc boolParam(const char* name, bool value) {
        ParamDesc param;
        param.name = name;
        param.kind = "bool";
        param.defaultValue = value;
        return param;
    }

    ParamDesc stringParam(const char* name, const char* value) {
        ParamDesc param;
        param.name = name;
        param.kind = "string";
        param.defaultValue = value;
        return param;
    }

    ParamDesc enumParam(const char* name, const char* value, std::vector<std::string> values) {
        ParamDesc param;
        param.name = name;
        param.kind = "enum";
        param.defaultValue = value;
        param.enumValues = std::move(values);
        return param;
    }

    ParamDesc vec3Param(const char* name, double x, double y, double z) {
        ParamDesc param;
        param.name = name;
        param.kind = "vector3";
        param.defaultValue = Json::array({x, y, z});
        return param;
    }

    ParamDesc dimensionsParam() { return enumParam("dimensions", "2d", {"2d", "3d"}); }
    ParamDesc basisParam() { return enumParam("basis", "perlin", {"perlin", "simplex", "value", "cellular"}); }

    NodeTypeDesc nodeType(const char* id, const char* category, const char* name, std::vector<Port> ports, std::vector<ParamDesc> params) {
        NodeTypeDesc type;
        type.id = id;
        type.category = category;
        type.displayName = name;
        type.ports = std::move(ports);
        type.params = std::move(params);
        return type;
    }

    NodeTypeDesc basisType(const char* id, const char* name) {
        return nodeType(id, "Noise", name, {inPort("coord", "coord", "Coord", true), outPort("out", "field", "Out")},
                        {dimensionsParam(), floatParam("frequency", 4.0), intParam("seed_offset", 0)});
    }

    NodeTypeDesc fractalType(const char* id, const char* name) {
        return nodeType(id, "Fractal", name, {inPort("coord", "coord", "Coord", true), outPort("out", "field", "Out")},
                        {basisParam(), dimensionsParam(), floatParam("frequency", 4.0), intParam("octaves", 6, 1.0, 12.0),
                         floatParam("lacunarity", 2.0), floatParam("gain", 0.5), intParam("seed_offset", 0)});
    }

    bool isOutputType(const std::string& type) {
        return type == "output_height" || type == "output_layer" || type == "output_density" || type == "output_value";
    }

    void addIssue(std::vector<Issue>& issues, Severity severity, const char* code, std::string message, std::string node = {}, std::string port = {}) {
        Issue issue;
        issue.severity = severity;
        issue.code = code;
        issue.nodeId = std::move(node);
        issue.portId = std::move(port);
        issue.message = std::move(message);
        issues.push_back(std::move(issue));
    }

    // S20's curve: [[t, v, in, out], ...], ascending t; a null tangent is a step
    bool curveKeysValid(const Json& keys, std::string& why) {
        if (!keys.is_array() || keys.empty()) {
            why = "'keys' must be a non-empty array of [t, v, in, out]";
            return false;
        }
        double previous = 0.0;
        for (size_t i = 0; i < keys.size(); i++) {
            const Json& key = keys[i];
            if (!key.is_array() || key.size() < 2 || !key[0].is_number() || !key[1].is_number()) {
                why = "key " + std::to_string(i) + " must be [t, v, in, out]";
                return false;
            }
            const double t = key[0].get<double>();
            if (i > 0 && t < previous) {
                why = "key " + std::to_string(i) + "'s t goes backward";
                return false;
            }
            previous = t;
        }
        return true;
    }

}

namespace jasperengine { namespace noise {

    double paramNumber(const graph::Node& node, const char* name, double fallback) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_number() ? it->get<double>() : fallback;
    }

    std::string paramString(const graph::Node& node, const char* name, const char* fallback) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
    }

    bool paramBool(const graph::Node& node, const char* name, bool fallback) {
        auto it = node.params.find(name);
        return it != node.params.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    bool isGridOnlyType(const std::string& typeId) {
        // terrain_height reads a snapshot by texel, so it has no meaning at an arbitrary point
        return typeId == "slope_mask" || typeId == "hydraulic_erosion" || typeId == "thermal_erosion" || typeId == "terrain_height";
    }

    bool isBakeOnlyType(const std::string& typeId) {
        return typeId == "hydraulic_erosion" || typeId == "thermal_erosion" || typeId == "terrain_height";
    }

    void appendErosionBudgetIssues(const graph::Graph& graph, int resolution, std::vector<graph::Issue>& issues) {
        int side = resolution;
        if (side <= 0) {
            side = 1024;
            auto meta = graph.metadata.find("noise");
            if (meta != graph.metadata.end() && meta->is_object()) {
                auto it = meta->find("resolution");
                if (it != meta->end() && it->is_number_integer()) side = it->get<int>();
            }
        }
        const double texels = static_cast<double>(side) * static_cast<double>(side);
        for (const graph::Node& node : graph.nodes) {
            if (node.type == "hydraulic_erosion") {
                const double droplets = paramNumber(node, "droplets_per_texel", 0.25) * texels;
                const double lifetime = paramNumber(node, "lifetime", 48.0);
                if (droplets * lifetime > 2147483648.0) {
                    addIssue(issues, Severity::Error, "erosion-budget",
                             "droplets * lifetime is " + std::to_string(static_cast<long long>(droplets * lifetime)) + " at " + std::to_string(side) +
                             " texels, over the 2^31 budget: lower droplets_per_texel or lifetime", node.id);
                }
            } else if (node.type == "thermal_erosion") {
                const double iterations = paramNumber(node, "iterations", 8.0);
                if (texels * iterations > 17179869184.0) {
                    addIssue(issues, Severity::Error, "erosion-budget",
                             "texels * iterations is " + std::to_string(static_cast<long long>(texels * iterations)) + " at " + std::to_string(side) +
                             " texels, over the 2^34 budget: lower iterations", node.id);
                }
            }
        }
    }

    void appendKindIssues(const graph::Graph& graph, int resolution, std::vector<graph::Issue>& issues) {
        bool hasOutput = false;
        std::set<std::string> outputs;
        std::string usage = "bake";
        auto meta = graph.metadata.find("noise");
        if (meta != graph.metadata.end() && meta->is_object()) {
            auto it = meta->find("usage");
            if (it != meta->end() && it->is_string()) usage = it->get<std::string>();
        }
        for (const graph::Node& node : graph.nodes) {
            if (isOutputType(node.type)) {
                hasOutput = true;
                std::string key;
                if (node.type == "output_height") key = "height";
                else if (node.type == "output_layer") key = "layer:" + std::to_string(static_cast<int>(paramNumber(node, "layer", 0.0)));
                else if (node.type == "output_density") key = "density:" + std::to_string(static_cast<int>(paramNumber(node, "foliage_layer", 0.0)));
                else key = "value:" + paramString(node, "name", "value");
                if (!outputs.insert(key).second) {
                    addIssue(issues, Severity::Error, "duplicate-output", "a second output for '" + key + "': remove one of them", node.id);
                }
            }
            if (node.type == "fbm" || node.type == "ridged" || node.type == "domain_warp") {
                const double octaves = paramNumber(node, "octaves", node.type == "domain_warp" ? 3.0 : 6.0);
                if (octaves < 1.0 || octaves > 12.0) {
                    addIssue(issues, Severity::Error, "octave-limit", "octaves must be 1..12, not " + std::to_string(static_cast<int>(octaves)), node.id);
                }
            }
            if (node.type == "variable") {
                const std::string name = paramString(node, "name", "");
                if (!graph.findVariable(name)) {
                    addIssue(issues, Severity::Error, "unknown-variable", "no graph variable is named '" + name + "': add it with addVariable or pick another", node.id);
                }
            }
            if (node.type == "curve") {
                auto keys = node.params.find("keys");
                std::string why;
                if (keys != node.params.end() && !curveKeysValid(*keys, why)) {
                    addIssue(issues, Severity::Error, "curve-keys", why, node.id);
                }
            }
            // A graph can state where it will run: "runtime" is a game, "points" is scripts' sample() and T6's volumes
            if (usage != "bake" && isBakeOnlyType(node.type)) {
                addIssue(issues, Severity::Error, "bake-only-node", "'" + node.type + "' only runs in the editor's bake: a game cannot load it", node.id);
            }
            if (usage == "points" && isGridOnlyType(node.type)) {
                addIssue(issues, Severity::Error, "grid-only-node", "'" + node.type + "' needs a whole grid: it cannot run at single points", node.id);
            }
        }
        if (!hasOutput) {
            addIssue(issues, Severity::Error, "no-output", "the graph has no output node: add output_height, output_layer, output_density or output_value");
        }
        appendErosionBudgetIssues(graph, resolution, issues);
    }

    graph::GraphKindDesc makeNoiseKindDesc() {
        GraphKindDesc kind;
        kind.id = kNoiseKindId;
        kind.displayName = "Noise Graph";
        kind.fileExtension = ".noisegraph";
        kind.kindVersion = kNoiseKindVersion;
        kind.acyclic = true;

        PortTypeDesc field;
        field.id = "field";
        field.color = 0x7fd1ffffu;
        PortTypeDesc coord;
        coord.id = "coord";
        coord.color = 0xffc861ffu;
        kind.portTypes = {field, coord};

        auto& t = kind.nodeTypes;
        const char* metric = "metric";
        t.push_back(nodeType("position", "Input", "Position",
                             {outPort("out", "coord", "Position"), outPort("x", "field", "X"), outPort("y", "field", "Y"), outPort("z", "field", "Z")}, {}));
        t.push_back(nodeType("terrain_uv", "Input", "Terrain UV", {outPort("u", "field", "U"), outPort("v", "field", "V")}, {}));
        t.push_back(nodeType("constant", "Input", "Constant", {outPort("out", "field", "Out")}, {floatParam("value", 0.0)}));
        t.push_back(nodeType("variable", "Input", "Variable", {outPort("out", "field", "Out")}, {stringParam("name", "")}));
        t.push_back(nodeType("terrain_height", "Input", "Terrain Height", {outPort("out", "field", "Height")}, {}));

        t.push_back(basisType("perlin", "Perlin"));
        t.push_back(basisType("simplex", "Simplex"));
        t.push_back(basisType("value", "Value"));
        NodeTypeDesc cellular = basisType("cellular", "Cellular");
        cellular.params.push_back(enumParam(metric, "euclidean", {"euclidean", "manhattan", "chebyshev"}));
        cellular.params.push_back(enumParam("return", "f1", {"f1", "f2", "f2_minus_f1", "cell_value"}));
        cellular.params.push_back(floatParam("jitter", 1.0, 0.0, 1.0));
        t.push_back(std::move(cellular));

        t.push_back(fractalType("fbm", "FBm"));
        t.push_back(fractalType("ridged", "Ridged"));

        t.push_back(nodeType("domain_warp", "Domain", "Domain Warp",
                             {inPort("coord", "coord", "Coord", true), inPort("strength", "field", "Strength", true), outPort("out", "coord", "Out")},
                             {basisParam(), dimensionsParam(), floatParam("frequency", 4.0), floatParam("amplitude", 0.1),
                              intParam("octaves", 3, 1.0, 12.0), intParam("seed_offset", 0)}));
        t.push_back(nodeType("domain_transform", "Domain", "Domain Scale/Offset",
                             {inPort("coord", "coord", "Coord", true), outPort("out", "coord", "Out")},
                             {vec3Param("scale", 1.0, 1.0, 1.0), vec3Param("offset", 0.0, 0.0, 0.0), floatParam("rotation_y", 0.0)}));

        t.push_back(nodeType("scale_offset", "Math", "Scale/Offset", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {floatParam("scale", 1.0), floatParam("offset", 0.0)}));
        t.push_back(nodeType("math", "Math", "Math", {inPort("a", "field", "A"), inPort("b", "field", "B", true), outPort("out", "field", "Out")},
                             {enumParam("op", "add", {"add", "subtract", "multiply", "divide", "min", "max", "power", "modulo", "abs", "negate",
                                                      "one_minus", "sqrt", "floor", "frac", "sin", "cos"}),
                              floatParam("b", 0.0)}));
        t.push_back(nodeType("clamp", "Math", "Clamp", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {floatParam("min", 0.0), floatParam("max", 1.0)}));
        t.push_back(nodeType("remap", "Math", "Remap", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {floatParam("from_min", -1.0), floatParam("from_max", 1.0), floatParam("to_min", 0.0), floatParam("to_max", 1.0), boolParam("clamp", false)}));
        ParamDesc keys;
        keys.name = "keys";
        keys.kind = "any";
        keys.defaultValue = Json::array({Json::array({0.0, 0.0, 1.0, 1.0}), Json::array({1.0, 1.0, 1.0, 1.0})});
        t.push_back(nodeType("curve", "Math", "Curve", {inPort("in", "field", "In"), outPort("out", "field", "Out")}, {keys}));
        t.push_back(nodeType("terrace", "Math", "Terrace", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {intParam("steps", 8, 2.0, 64.0), floatParam("smoothness", 0.2, 0.0, 1.0)}));
        t.push_back(nodeType("select", "Math", "Select",
                             {inPort("a", "field", "A"), inPort("b", "field", "B"), inPort("mask", "field", "Mask"), outPort("out", "field", "Out")},
                             {floatParam("threshold", 0.5), floatParam("falloff", 0.0, 0.0)}));
        t.push_back(nodeType("blend", "Math", "Blend",
                             {inPort("a", "field", "A"), inPort("b", "field", "B"), inPort("weight", "field", "Weight", true), outPort("out", "field", "Out")},
                             {floatParam("weight", 0.5, 0.0, 1.0)}));

        t.push_back(nodeType("height_mask", "Mask", "Height Mask", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {floatParam("min", 0.0), floatParam("max", 1.0), floatParam("falloff", 0.0, 0.0)}));
        t.push_back(nodeType("slope_mask", "Mask", "Slope Mask", {inPort("in", "field", "In"), outPort("out", "field", "Out")},
                             {floatParam("min_angle", 0.0, 0.0, 90.0), floatParam("max_angle", 45.0, 0.0, 90.0), floatParam("falloff", 5.0, 0.0, 90.0)}));

        t.push_back(nodeType("hydraulic_erosion", "Stage", "Hydraulic Erosion",
                             {inPort("height", "field", "Height"), inPort("hardness", "field", "Hardness", true),
                              outPort("out", "field", "Height"), outPort("flow", "field", "Flow"), outPort("sediment", "field", "Sediment")},
                             {floatParam("droplets_per_texel", 0.25, 0.01, 4.0), intParam("lifetime", 48, 8.0, 128.0), floatParam("inertia", 0.05, 0.0, 1.0),
                              floatParam("capacity", 4.0, 0.0), floatParam("min_slope", 0.01, 0.0), floatParam("erode_rate", 0.3, 0.0, 1.0),
                              floatParam("deposit_rate", 0.3, 0.0, 1.0), floatParam("evaporation", 0.02, 0.0, 1.0), floatParam("gravity", 4.0, 0.0),
                              intParam("radius", 2, 1.0, 4.0), intParam("seed_offset", 0)}));
        t.push_back(nodeType("thermal_erosion", "Stage", "Thermal Erosion", {inPort("height", "field", "Height"), outPort("out", "field", "Height")},
                             {intParam("iterations", 8, 1.0, 64.0), floatParam("talus_angle", 35.0, 0.0, 89.0), floatParam("rate", 0.5, 0.0, 1.0)}));

        t.push_back(nodeType("output_height", "Output", "Height Output", {inPort("in", "field", "In")}, {enumParam("unit", "normalized", {"normalized", "metres"})}));
        t.push_back(nodeType("output_layer", "Output", "Layer Output", {inPort("weight", "field", "Weight")},
                             {intParam("layer", 0, 0.0, static_cast<double>(terraincore::TERRAIN_MAX_LAYERS - 1)), boolParam("optional", false)}));
        t.push_back(nodeType("output_density", "Output", "Foliage Density Output", {inPort("density", "field", "Density")},
                             {intParam("foliage_layer", 0, 0.0), boolParam("optional", false)}));
        t.push_back(nodeType("output_value", "Output", "Value Output", {inPort("in", "field", "In")}, {stringParam("name", "value")}));

        kind.validate = [](const graph::Graph& graph, std::vector<graph::Issue>& issues) {
            appendKindIssues(graph, 0, issues);
        };
        return kind;
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
