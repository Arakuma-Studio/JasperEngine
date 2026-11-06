// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_LIBRARYVARIANTS_H
#define PROCMAT_LIBRARYVARIANTS_H

// The sweep of the whole library as tools/procmat/expand_nodes.py walks it: one entry per node
// variant (default, wired, each enum value and boolean, generic2) with the same small sources wired
// into inputs. Shared by the expander's agreement test and the node image test (editor/tests and
// the GPU suite in engine/tests/core/procmat).

#include "../../procmat/ProcMatExpander.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat::test {

    // A small source of a given port type: what a wired variant connects to an input
    class ProbeSource : public Generator {
    public:
        ProbeSource(Expander& expander, const std::string& kind) : Generator(expander, "probe_" + kind), kind(kind) {}

        std::string outputType(int) const override { return kind; }
        bool isTrivial() const override { return true; }

        ExpandedCode generate(const std::string& uv, int, ExpansionContext&) override {
            static const std::map<std::string, std::string> expressions = {
                {"f", "fract($uv.x*7.0)"}, {"rgb", "vec3($uv, 0.5)"}, {"rgba", "vec4($uv, 0.5, 1.0)"},
                {"sdf2d", "(length($uv-vec2(0.5))-0.25)"}, {"fill", "vec4(0.25, 0.25, 0.5, 0.5)"},
                {"sdf3d", "(length($uv)-0.5)"}, {"sdf3dc", "vec2(length($uv)-0.5, 0.0)"},
                {"tex3d", "vec3(fract(($uv).xyz))"}, {"tex3d_gs", "fract(($uv).x)"},
            };
            std::string text = expressions.at(kind);
            for (size_t at = text.find("$uv"); at != std::string::npos; at = text.find("$uv", at + uv.size())) text.replace(at, 3, uv);
            ExpandedCode result;
            result.outputType = kind;
            result.outputValues[kind] = text;
            return result;
        }

    private:
        std::string kind;
    };

    struct NodeVariant {
        std::string name;
        Json parameters = Json::object();
        int genericSize = 1;
        bool wire = false;
    };

    inline std::vector<NodeVariant> variantsOf(const Json& descriptor) {
        std::vector<NodeVariant> variants;
        variants.push_back({"default", Json::object(), 1, false});
        const bool hasInputs = descriptor.contains("inputs") && !descriptor["inputs"].empty();
        if (hasInputs) variants.push_back({"wired", Json::object(), 1, true});
        bool generic = false;
        if (descriptor.contains("inputs")) {
            for (const Json& port : descriptor["inputs"]) generic = generic || port.value("name", std::string()).find('#') != std::string::npos;
        }
        for (const Json& parameter : descriptor.value("parameters", Json::array())) {
            const std::string name = parameter.at("name").get<std::string>();
            const std::string kind = parameter.value("type", std::string());
            generic = generic || name.find('#') != std::string::npos;
            if (kind == "enum") {
                const int fallback = parameter.contains("default") && parameter["default"].is_number() ? (int)parameter["default"].get<double>() : 0;
                const int count = parameter.contains("values") ? (int)parameter["values"].size() : 0;
                for (int i = 0; i < count; i++) {
                    if (i != fallback) variants.push_back({name + "=" + std::to_string(i), Json{{name, i}}, 1, hasInputs});
                }
            } else if (kind == "boolean") {
                // as Python's truth of whatever the descriptor holds (a flag can be 0 or 1)
                const Json& stored = parameter.contains("default") ? parameter["default"] : Json();
                const bool fallback = stored.is_boolean() ? stored.get<bool>() : (stored.is_number() ? stored.get<double>() != 0.0 : false);
                variants.push_back({name + "=" + (fallback ? "false" : "true"), Json{{name, !fallback}}, 1, hasInputs});
            }
        }
        if (generic) variants.push_back({"generic2", Json::object(), 2, hasInputs});
        return variants;
    }

    // inputs read as .texture or .size: only a Buffer has those
    inline std::vector<bool> usesTextureAttribute(const Json& descriptor, size_t inputs) {
        Json parts = Json::object();
        for (const char* field : {"instance", "code", "outputs"}) {
            if (descriptor.contains(field)) parts[field] = descriptor[field];
        }
        const std::string text = parts.dump();
        std::vector<bool> result(inputs, false);
        const std::vector<Json> raw = descriptor.value("inputs", std::vector<Json>());
        for (size_t i = 0; i < raw.size() && i < inputs; i++) {
            const std::string name = raw[i].at("name").get<std::string>();
            result[i] = text.find("$" + name + ".texture") != std::string::npos || text.find("$" + name + ".size") != std::string::npos;
        }
        return result;
    }

    inline std::string portTypeOf(const Json& descriptor, size_t index, int genericSize) {
        const std::vector<Json> raw = descriptor.value("inputs", std::vector<Json>());
        std::vector<const Json*> groups, expanded;
        for (const Json& port : raw) {
            if (port.value("name", std::string()).find('#') != std::string::npos) groups.push_back(&port);
        }
        for (const Json& port : raw) {
            if (port.value("name", std::string()).find('#') != std::string::npos) {
                if (&port == groups[0]) {
                    for (int g = 0; g < genericSize; g++) expanded.insert(expanded.end(), groups.begin(), groups.end());
                }
            } else {
                expanded.push_back(&port);
            }
        }
        return index < expanded.size() ? expanded[index]->at("type").get<std::string>() : std::string();
    }

    // One node's generators with the variant's sources wired to its inputs, as the reference does
    inline NodeInstance instantiateVariant(Expander& expander, const std::string& nodeId, const Json& descriptor, const NodeVariant& variant) {
        NodeInstance instance = expander.instantiate(nodeId, variant.parameters, 0.0, variant.genericSize);
        const std::vector<bool> needsTexture = usesTextureAttribute(descriptor, instance.inputs.size());
        for (size_t index = 0; index < instance.inputs.size(); index++) {
            std::string kind = portTypeOf(descriptor, index, variant.genericSize);
            const bool texture = index < needsTexture.size() && needsTexture[index];
            if (variant.wire || kind.empty() || texture) {
                if (kind.empty()) kind = "rgba";
                if (texture) {
                    auto* source = expander.make<TextureSource>(nodeId + "/probe" + std::to_string(index), Json{{"size", 10}});
                    source->setSource(0, SourceRef{expander.make<ProbeSource>(std::string("rgba")), 0, false});
                    expander.connect(instance.inputs[index], SourceRef{source, 0, false});
                } else {
                    expander.connect(instance.inputs[index], SourceRef{expander.make<ProbeSource>(kind), 0, false});
                }
            }
        }
        return instance;
    }

    inline uint64_t fnv1a(const void* data, size_t size) {
        uint64_t hash = 14695981039346656037ull;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

}

#endif
