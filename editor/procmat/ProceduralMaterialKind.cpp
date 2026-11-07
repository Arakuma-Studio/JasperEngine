// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralMaterialKind.h"

#include "ProcMatExpander.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    std::string slugged(const std::string& text) {
        std::string id;
        for (char c : text) {
            if (c >= 'A' && c <= 'Z') id += (char)(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') id += c;
            else if (!id.empty() && id.back() != '_') id += '_';
        }
        while (!id.empty() && id.back() == '_') id.pop_back();
        return id;
    }

    bool isFiniteNumber(const Json& value) {
        return value.is_number() && std::isfinite(value.get<double>());
    }

    bool isWholeNumber(const Json& value) {
        return isFiniteNumber(value) && std::floor(value.get<double>()) == value.get<double>();
    }

    std::string withoutMark(std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '#'), text.end());
        return text;
    }

    uint32_t parseColor(const std::string& hex) {
        // "#RRGGBB", as the library's port types give it; opaque for the canvas
        if (hex.size() != 7 || hex[0] != '#') return 0xffffffffu;
        return ((uint32_t)std::strtoul(hex.c_str() + 1, nullptr, 16) << 8) | 0xffu;
    }

    bool hasFields(const Json& object, std::initializer_list<const char*> fields) {
        if (!object.is_object()) return false;
        for (const char* field : fields) {
            auto it = object.find(field);
            if (it == object.end() || !isFiniteNumber(*it)) return false;
        }
        return true;
    }

    bool optionalNumber(const Json& object, const char* field) {
        auto it = object.find(field);
        return it == object.end() || isFiniteNumber(*it);
    }

    // why a stored value of this parameter type cannot reach the generator; empty when it can
    std::string valueProblem(const Json& definition, const Json& value) {
        const std::string kind = definition.value("type", std::string());
        if (kind == "float") {
            return isFiniteNumber(value) ? std::string() : "is not a number";
        }
        if (kind == "boolean") {
            // the library's own data writes some flags as 0 and 1
            const bool flag = value.is_boolean() || (value.is_number() && (value.get<double>() == 0.0 || value.get<double>() == 1.0));
            return flag ? std::string() : "is not true or false";
        }
        if (kind == "enum") {
            const std::vector<std::string> ids = enumValueIds(definition);
            if (value.is_string()) {
                return std::find(ids.begin(), ids.end(), value.get<std::string>()) != ids.end() ? std::string() : "is not one of its values";
            }
            if (isWholeNumber(value) && value.get<double>() >= 0.0 && value.get<double>() < (double)ids.size()) return {};
            return "is not one of its values";
        }
        if (kind == "size") {
            return isWholeNumber(value) ? std::string() : "is not a whole number";
        }
        if (kind == "color") {
            return hasFields(value, {"r", "g", "b"}) && optionalNumber(value, "a") ? std::string() : "is not a colour {r, g, b, a}";
        }
        if (kind == "gradient") {
            if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return "is not a gradient {points: [{pos, r, g, b, a}]}";
            if (value["points"].size() > RAMP_KEYS_MAX) return "has more than 32 keys";
            for (const Json& point : value["points"]) {
                if (!hasFields(point, {"pos", "r", "g", "b"}) || !optionalNumber(point, "a")) return "has a key that is not {pos, r, g, b, a}";
            }
            if (value.contains("interpolation") && !(isWholeNumber(value["interpolation"]) && value["interpolation"].get<double>() >= 0.0 && value["interpolation"].get<double>() <= 3.0)) {
                return "has an interpolation other than 0 to 3";
            }
            return {};
        }
        if (kind == "curve") {
            if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].size() < 2) return "is not a curve {points: [{x, y, ls, rs}]}";
            if (value["points"].size() > RAMP_KEYS_MAX) return "has more than 32 keys";
            for (const Json& point : value["points"]) {
                if (!hasFields(point, {"x", "y", "ls", "rs"})) return "has a key that is not {x, y, ls, rs}";
            }
            return {};
        }
        if (kind == "polygon" || kind == "polyline" || kind == "lattice") {
            if (!value.is_object() || !value.contains("points") || !value["points"].is_array() || value["points"].empty()) return "is not a shape {points: [{x, y}]}";
            // every point is a vec4 of the pass's uniforms (GL 4.1 guarantees 256)
            if (value["points"].size() > SHAPE_POINTS_MAX) return "has more than " + std::to_string(SHAPE_POINTS_MAX) + " points";
            for (const Json& point : value["points"]) {
                if (!hasFields(point, {"x", "y"})) return "has a point that is not {x, y}";
            }
            if (kind == "lattice") {
                const Json* size = value.contains("size") ? &value["size"] : nullptr;
                const auto cells = [&](const char* axis) { return size && isWholeNumber((*size)[axis]) && (*size)[axis].get<double>() >= 1.0 && (*size)[axis].get<double>() <= 8.0; };
                if (!size || !cells("x") || !cells("y")) return "has no size {x, y} of 1 to 8 cells";
                const double points = ((*size)["x"].get<double>() + 1.0) * ((*size)["y"].get<double>() + 1.0);
                if ((double)value["points"].size() != points) return "has not (x + 1) * (y + 1) points";
            }
            return {};
        }
        if (kind == "pixels") {
            if (!hasFields(value, {"w", "h", "bpp"}) || !value.contains("pl") || !value.contains("px") || !value["pl"].is_array() || !value["px"].is_array()) {
                return "is not an image {w, h, bpp, pl, px}";
            }
            const double bpp = value["bpp"].get<double>();
            if (bpp != 1.0 && bpp != 2.0 && bpp != 4.0 && bpp != 8.0) return "has a depth other than 1, 2, 4 or 8 bits";
            const auto side = [&](const char* field) { return isWholeNumber(value[field]) && value[field].get<double>() >= 1.0 && value[field].get<double>() <= (double)PIXELS_SIDE_MAX; };
            if (!side("w") || !side("h")) return "is not 1 to " + std::to_string(PIXELS_SIDE_MAX) + " pixels on a side";
            const int depth = (int)bpp, perWord = 32 / depth;
            const size_t pixels = (size_t)value["w"].get<double>() * (size_t)value["h"].get<double>();
            if (value["px"].size() != (pixels + perWord - 1) / perWord) return "has not the words its size needs";
            const size_t colors = value["pl"].size();
            if (colors < 1 || colors > (size_t)1 << depth) return "has a palette of 0 or more than 2^bpp colours";
            for (const Json& color : value["pl"]) {
                if (!hasFields(color, {"r", "g", "b"}) || !optionalNumber(color, "a")) return "has a palette entry that is not {r, g, b, a}";
            }
            // the shader reads each word as a signed int and indexes the palette with it: no index past it
            size_t pixel = 0;
            for (const Json& word : value["px"]) {
                if (!isWholeNumber(word) || word.get<double>() < -2147483648.0 || word.get<double>() > 4294967295.0) return "has a word that is not a 32-bit whole number";
                const uint32_t bits = (uint32_t)(int64_t)word.get<double>();
                for (int k = 0; k < perWord && pixel < pixels; k++, pixel++) {
                    if (((bits >> (depth * k)) & ((1u << depth) - 1u)) >= colors) return "has a pixel past its palette";
                }
            }
            return {};
        }
        return "has a type this build does not know (" + kind + ")";
    }

    graph::ParamDesc paramOf(const Json& definition) {
        graph::ParamDesc param;
        param.name = definition.at("name").get<std::string>();
        const std::string kind = definition.value("type", std::string());
        static const std::map<std::string, std::string> kinds = {{"float", "float"}, {"boolean", "bool"}, {"enum", "enum"}, {"size", "int"}, {"color", "color"}};
        auto mapped = kinds.find(kind);
        param.kind = mapped != kinds.end() ? mapped->second : "any";
        if (auto fallback = definition.find("default"); fallback != definition.end()) param.defaultValue = *fallback;
        if (kind == "float") {
            if (auto low = definition.find("min"); low != definition.end() && low->is_number()) param.min = low->get<double>();
            if (auto high = definition.find("max"); high != definition.end() && high->is_number()) param.max = high->get<double>();
        } else if (kind == "size") {
            if (auto low = definition.find("first"); low != definition.end() && low->is_number()) param.min = low->get<double>();
            if (auto high = definition.find("last"); high != definition.end() && high->is_number()) param.max = high->get<double>();
        } else if (kind == "enum") {
            param.enumValues = enumValueIds(definition);
            // a default is stored as its index in the descriptor, and a file names values
            if (param.defaultValue.is_number()) {
                const size_t index = (size_t)param.defaultValue.get<double>();
                if (index < param.enumValues.size()) param.defaultValue = param.enumValues[index];
            }
        }
        return param;
    }

    graph::Port makePort(const std::string& id, graph::PortDirection direction, const std::string& type, const std::string& name, bool optional) {
        graph::Port port;
        port.id = id;
        port.direction = direction;
        port.type = type;
        port.name = name;
        port.optional = optional;
        return port;
    }

    // inputs read through .texture or .size have nothing to read when unconnected
    bool readsAsTexture(const Json& descriptor, const std::string& input) {
        const std::string needle = "$" + input;
        for (const char* field : {"instance", "code", "outputs"}) {
            auto it = descriptor.find(field);
            if (it == descriptor.end()) continue;
            const std::string text = it->dump();
            if (text.find(needle + ".texture") != std::string::npos || text.find(needle + ".size") != std::string::npos) return true;
        }
        return false;
    }

    graph::NodeTypeDesc nodeTypeOf(const NodeLibrary& library, const std::string& id, const Json& descriptor) {
        graph::NodeTypeDesc type;
        type.id = id;
        type.displayName = descriptor.value("name", id);
        if (const Json* entry = library.catalogEntry(id)) {
            type.displayName = entry->value("display_name", type.displayName);
            const std::string item = entry->value("tree_item", std::string());
            type.category = item.substr(0, item.find('/'));
        }
        type.ports = nodePorts(descriptor, 1);
        for (const Json& definition : expandedParameters(descriptor, GENERIC_SIZE_MAX)) type.params.push_back(paramOf(definition));
        // a node's own seed (R27's reroll is the graph's): a whole number, so a file stays diffable
        graph::ParamDesc seed;
        seed.name = "seed";
        seed.kind = "int";
        seed.defaultValue = 0;
        type.params.push_back(std::move(seed));
        if (isGenericDescriptor(descriptor)) {
            graph::ParamDesc size;
            size.name = "generic_size";
            size.kind = "int";
            size.defaultValue = 1;
            size.min = 1.0;
            size.max = (double)GENERIC_SIZE_MAX;
            type.params.push_back(std::move(size));
            // captured by value: the kind holds these functions for as long as it is registered
            type.dynamicPorts = [descriptor](const graph::Json& params) {
                int genericSize = 1;
                // clamped as a double: a file's 1e300 is no int (S23's ops run this before any range check)
                if (auto it = params.find("generic_size"); it != params.end() && it->is_number()) {
                    genericSize = (int)std::clamp(it->get<double>(), 1.0, (double)GENERIC_SIZE_MAX);
                }
                return nodePorts(descriptor, genericSize);
            };
        }
        return type;
    }

    graph::NodeTypeDesc materialOutputType() {
        graph::NodeTypeDesc type;
        type.id = MATERIAL_OUTPUT_NODE;
        type.displayName = "Material Output";
        type.category = "Output";
        const auto in = graph::PortDirection::In;
        type.ports = {makePort("base_color", in, "rgba", "Base Color", true), makePort("normal", in, "rgb", "Normal", true),
                      makePort("occlusion", in, "f", "Occlusion", true), makePort("roughness", in, "f", "Roughness", true),
                      makePort("metallic", in, "f", "Metallic", true), makePort("emission", in, "rgb", "Emission", true),
                      makePort("height", in, "f", "Height", true)};
        // the bake makes the Normal from the float Height when Normal is not connected (R14)
        graph::ParamDesc strength;
        strength.name = MATERIAL_NORMAL_STRENGTH;
        strength.kind = "float";
        strength.defaultValue = 1.0;
        strength.min = 0.0;
        strength.max = 4.0;
        type.params = {std::move(strength)};
        return type;
    }

    graph::NodeTypeDesc bufferType() {
        graph::NodeTypeDesc type;
        type.id = BUFFER_NODE;
        type.displayName = "Buffer";
        type.category = "Simple";
        type.ports = {makePort("in", graph::PortDirection::In, "rgba", "In", false), makePort("out", graph::PortDirection::Out, "rgba", "Out", false)};
        graph::ParamDesc size;
        size.name = "size";
        size.kind = "int";
        size.defaultValue = 10;
        size.min = 4.0;
        size.max = 13.0;
        graph::ParamDesc filter;
        filter.name = "filter";
        filter.kind = "bool";
        filter.defaultValue = true;
        type.params = {std::move(size), std::move(filter)};
        return type;
    }

    void addIssue(std::vector<graph::Issue>& issues, const char* code, const std::string& node, const std::string& message) {
        graph::Issue issue;
        issue.severity = graph::Severity::Error;
        issue.code = code;
        issue.nodeId = node;
        issue.message = message;
        issues.push_back(std::move(issue));
    }

    void validateGraph(const NodeLibrary& library, const graph::Graph& file, std::vector<graph::Issue>& issues) {
        int outputs = 0;
        for (const graph::Node& node : file.nodes) {
            if (node.type == MATERIAL_OUTPUT_NODE) {
                if (++outputs == 2) addIssue(issues, "duplicate-output", node.id, "a Procedural Material has one Material Output");
                continue;
            }
            const Json* descriptor = node.type == BUFFER_NODE ? &jasperengine::editor::procmat::hostBufferDescriptor() : library.descriptor(node.type);
            if (!descriptor) continue; // reported as unknown-node-type by S23's own validation
            for (const std::string& problem : checkNodeParameters(*descriptor, node, false)) {
                addIssue(issues, "param-type", node.id, problem);
            }
        }
    }

}

const Json& jasperengine::editor::procmat::hostBufferDescriptor() {
    static const Json descriptor = Json::parse(R"({"id": "buffer", "parameters": [
        {"name": "size", "type": "size", "first": 4, "last": 13, "default": 10},
        {"name": "filter", "type": "boolean", "default": true}]})");
    return descriptor;
}

std::vector<std::string> jasperengine::editor::procmat::enumValueIds(const Json& parameter) {
    std::vector<std::string> ids;
    auto values = parameter.find("values");
    if (values == parameter.end() || !values->is_array()) return ids;
    int index = 0;
    for (const Json& value : *values) {
        std::string id = slugged(value.value("name", std::string()));
        // a label can repeat or hold nothing a name can use: the index stays unique
        if (id.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) id = "value_" + std::to_string(index);
        ids.push_back(std::move(id));
        index++;
    }
    return ids;
}

std::vector<graph::Port> jasperengine::editor::procmat::nodePorts(const Json& descriptor, int genericSize) {
    std::vector<graph::Port> ports;
    for (const Json& input : expandedInputs(descriptor, genericSize)) {
        const std::string id = input.at("name").get<std::string>();
        const bool hasDefault = input.contains("default");
        ports.push_back(makePort(id, graph::PortDirection::In, input.value("type", std::string("rgba")), withoutMark(input.value("label", id)),
                                 hasDefault && !readsAsTexture(descriptor, id)));
    }
    const std::vector<Json> outputs = expandedOutputs(descriptor, genericSize);
    const std::vector<std::string> ids = outputPortIds(outputs);
    for (size_t i = 0; i < outputs.size(); i++) {
        ports.push_back(makePort(ids[i], graph::PortDirection::Out, outputs[i].value("type", std::string("rgba")), withoutMark(outputs[i].value("label", ids[i])), false));
    }
    return ports;
}

int jasperengine::editor::procmat::genericSizeOf(const graph::Node& node) {
    auto it = node.params.find("generic_size");
    return it != node.params.end() && it->is_number() ? std::clamp((int)it->get<double>(), 1, GENERIC_SIZE_MAX) : 1;
}

std::vector<std::string> jasperengine::editor::procmat::checkNodeParameters(const Json& descriptor, const graph::Node& node, bool checkRange) {
    std::vector<std::string> problems;
    // R2: a node's stored version names the behaviour it was authored against, and a graph never changes
    // look silently: a version with no descriptor here is refused instead of drawn with another's code
    if (auto version = node.passthrough.find("typeVersion"); version != node.passthrough.end()) {
        if (!version->is_number() || version->get<double>() != descriptor.value("type_version", 1.0)) {
            problems.push_back("node " + node.id + " was made with version " + version->dump() + " of '" + node.type + "', which this build does not have");
        }
    }
    if (auto seed = node.params.find("seed"); seed != node.params.end() && !isWholeNumber(*seed)) {
        problems.push_back("'seed' of " + node.id + " is not a whole number");
    }
    if (auto size = node.params.find("generic_size"); size != node.params.end()) {
        if (!isWholeNumber(*size) || size->get<double>() < 1.0 || size->get<double>() > (double)GENERIC_SIZE_MAX) {
            problems.push_back("'generic_size' of " + node.id + " is not a whole number from 1 to " + std::to_string(GENERIC_SIZE_MAX));
        }
    }
    for (const Json& definition : expandedParameters(descriptor, GENERIC_SIZE_MAX)) {
        const std::string name = definition.at("name").get<std::string>();
        auto value = node.params.find(name);
        if (value == node.params.end()) continue;
        // an enum named wrong is S23's strict param-range error, with its did-you-mean: not twice
        if (!checkRange && definition.value("type", std::string()) == "enum" && value->is_string()) continue;
        std::string problem = valueProblem(definition, *value);
        if (problem.empty() && checkRange) {
            const std::string kind = definition.value("type", std::string());
            const char* low = kind == "size" ? "first" : "min";
            const char* high = kind == "size" ? "last" : "max";
            if (kind == "float" || kind == "size") {
                const double number = value->get<double>();
                if ((definition.contains(low) && definition[low].is_number() && number < definition[low].get<double>()) ||
                    (definition.contains(high) && definition[high].is_number() && number > definition[high].get<double>())) {
                    problem = "is outside its range";
                }
            }
        }
        if (!problem.empty()) problems.push_back("'" + name + "' of " + node.id + " " + problem);
    }
    return problems;
}

Json jasperengine::editor::procmat::generatorParameters(const Json& descriptor, const graph::Node& node) {
    Json parameters = Json::object();
    for (const Json& definition : expandedParameters(descriptor, genericSizeOf(node))) {
        const std::string name = definition.at("name").get<std::string>();
        auto value = node.params.find(name);
        if (value == node.params.end()) continue;
        if (definition.value("type", std::string()) == "enum" && value->is_string()) {
            const std::vector<std::string> ids = enumValueIds(definition);
            auto found = std::find(ids.begin(), ids.end(), value->get<std::string>());
            parameters[name] = (int)(found - ids.begin());
        } else {
            parameters[name] = *value;
        }
    }
    return parameters;
}

graph::GraphKindDesc jasperengine::editor::procmat::makeProceduralMaterialKind(const std::shared_ptr<const NodeLibrary>& library) {
    graph::GraphKindDesc kind;
    kind.id = PROCEDURAL_MATERIAL_KIND;
    kind.displayName = "Procedural Material";
    kind.fileExtension = PROCEDURAL_MATERIAL_EXTENSION;
    kind.kindVersion = 1;
    kind.acyclic = true;
    // R28: a misspelled parameter or an out-of-range value is an error edit_graph refuses
    kind.strictParams = true;
    for (const auto& [name, type] : library->portTypes()) {
        graph::PortTypeDesc port;
        port.id = name;
        port.color = parseColor(type.value("color", std::string()));
        if (auto convert = type.find("convert"); convert != type.end() && convert->is_array()) {
            for (const Json& conversion : *convert) port.implicitTo.push_back(conversion.value("type", std::string()));
        }
        kind.portTypes.push_back(std::move(port));
    }
    kind.nodeTypes.push_back(materialOutputType());
    kind.nodeTypes.push_back(bufferType());
    for (const auto& [id, descriptor] : library->descriptors()) {
        if (NodeLibrary::leftOutReason(id)) continue;
        kind.nodeTypes.push_back(nodeTypeOf(*library, id, descriptor));
    }
    kind.validate = [library](const graph::Graph& file, std::vector<graph::Issue>& issues) { validateGraph(*library, file, issues); };
    return kind;
}
