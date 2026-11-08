// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProcMatExpander.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace jasperengine::editor::procmat;

namespace {

    bool isWordLetter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '?';
    }

    bool isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    std::string stripped(const std::string& text) {
        size_t begin = 0, end = text.size();
        while (begin < end && isSpace(text[begin])) begin++;
        while (end > begin && isSpace(text[end - 1])) end--;
        return text.substr(begin, end - begin);
    }

    size_t matchingClose(const std::string& text, size_t start) {
        int depth = 0;
        for (size_t i = start; i < text.size(); i++) {
            if (text[i] == '(') {
                depth++;
            } else if (text[i] == ')') {
                if (--depth == 0) return i;
            }
        }
        throw ExpansionError("unbalanced text: " + text.substr(start, 60));
    }

    std::vector<std::string> splitArguments(const std::string& text) {
        std::vector<std::string> args;
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i];
            if (c == '(' || c == '[') {
                depth++;
            } else if (c == ')' || c == ']') {
                depth--;
            } else if (c == ',' && depth == 0) {
                args.push_back(stripped(text.substr(start, i - start)));
                start = i + 1;
            }
        }
        args.push_back(stripped(text.substr(start)));
        return args;
    }

    // Material Maker substitutes coordinates as text, so `$uv.x` with `a+b` binds to b alone. It
    // goes unnoticed when the types still agree, and fails to compile when they do not.
    std::string parenthesized(const std::string& uv) {
        const bool simple = !uv.empty() && std::all_of(uv.begin(), uv.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '[' || c == ']';
        });
        if (simple) return uv;
        if (!uv.empty() && uv[0] == '(' && matchingClose(uv, 0) == uv.size() - 1) return uv;
        return "(" + uv + ")";
    }

    std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
        size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos) {
            text.replace(position, from.size(), to);
            position += to.size();
        }
        return text;
    }

    std::string joined(const Json& lines) {
        std::string text;
        bool first = true;
        if (lines.is_array()) {
            for (const Json& line : lines) {
                if (!first) text += "\n";
                text += line.get<std::string>();
                first = false;
            }
        }
        return text;
    }

    bool truthy(const Json& value) {
        if (value.is_null()) return false;
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_number()) return value.get<double>() != 0.0;
        if (value.is_string()) return !value.get<std::string>().empty();
        return !value.empty();
    }

    double numberOf(const Json& value) {
        if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
        if (value.is_number()) return value.get<double>();
        throw ExpansionError("a number was expected, got " + value.dump());
    }

    const Json* member(const Json& object, const std::string& name) {
        if (!object.is_object()) return nullptr;
        auto it = object.find(name);
        return it == object.end() ? nullptr : &*it;
    }

    double numberOr(const Json& object, const char* name, double fallback) {
        const Json* found = member(object, name);
        return found ? numberOf(*found) : fallback;
    }

    std::string textOf(const Json& value) {
        // as Python's str() for what an input's default holds
        return value.is_string() ? value.get<std::string>() : (value.is_null() ? std::string() : value.dump());
    }

    std::string formatted(const char* format, double value) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), format, value);
        return buffer;
    }

    std::string slotKey(const std::string& prefix, const std::string& part) {
        return prefix + "\x1f" + part;
    }

    // A ramp's keys in position order, as ShaderNode::gradient writes them, and the index each had in the
    // value; a ramp with no value is the default one
    struct RampKeys {
        std::vector<Json> points;
        std::vector<size_t> order;
        int interpolation = 1;
        bool given = false;
    };

    RampKeys rampKeys(const Json& given) {
        static const Json fallback = Json::parse(R"({"points": [{"pos": 0, "r": 0, "g": 0, "b": 0, "a": 1}, {"pos": 1, "r": 1, "g": 1, "b": 1, "a": 1}], "interpolation": 1})");
        RampKeys keys;
        keys.given = truthy(given);
        const Json& value = keys.given ? given : fallback;
        std::vector<Json> points;
        for (const Json& point : value.is_object() ? value.at("points") : value) points.push_back(point);
        keys.order.resize(points.size());
        for (size_t i = 0; i < points.size(); i++) keys.order[i] = i;
        std::stable_sort(keys.order.begin(), keys.order.end(), [&](size_t a, size_t b) { return numberOf(points[a].at("pos")) < numberOf(points[b].at("pos")); });
        for (size_t i : keys.order) keys.points.push_back(points[i]);
        keys.interpolation = value.is_object() ? (int)numberOr(value, "interpolation", 1.0) : 1;
        return keys;
    }

    std::vector<double> colourOf(const Json& value) {
        return {numberOf(value.at("r")), numberOf(value.at("g")), numberOf(value.at("b")), numberOr(value, "a", 1.0)};
    }

    bool hasGenericMark(const Json& item, const char* field) {
        const Json* value = member(item, field);
        return value && value->is_string() && value->get<std::string>().find('#') != std::string::npos;
    }

    // A generic node repeats the run of items that hold the mark
    std::vector<Json> expandGenericList(const std::vector<Json>& items, const char* field, int genericSize) {
        int first = -1, last = -1;
        for (int i = 0; i < (int)items.size(); i++) {
            const bool mark = hasGenericMark(items[i], field);
            if (mark && first == -1) {
                first = i;
            } else if (!mark && first != -1 && last == -1) {
                last = i;
            }
        }
        if (first == -1) return items;
        if (last == -1) last = (int)items.size();
        std::vector<Json> result(items.begin(), items.begin() + first);
        for (int g = 0; g < genericSize; g++) {
            for (int i = first; i < last; i++) {
                Json copy = items[i];
                copy[field] = replaceAll(copy[field].get<std::string>(), "#", std::to_string(g + 1));
                result.push_back(std::move(copy));
            }
        }
        result.insert(result.end(), items.begin() + last, items.end());
        return result;
    }

    std::string expandGenericCode(std::string code, int genericSize) {
        std::string result;
        code = replaceAll(std::move(code), "#count", std::to_string(genericSize));
        while (!code.empty()) {
            const size_t position = code.find("#for");
            if (position == std::string::npos) {
                result += code;
                break;
            }
            result += code.substr(0, position);
            code = code.substr(position + 4);
            const size_t end = code.find("#end");
            std::string generic;
            if (end == std::string::npos) {
                generic = code;
                code.clear();
            } else {
                generic = code.substr(0, end);
                code = code.substr(end + 4);
            }
            for (int i = 0; i < genericSize; i++) result += replaceAll(generic, "#", std::to_string(i + 1));
        }
        return result;
    }

    std::vector<Json> listOf(const Json& descriptor, const char* field) {
        std::vector<Json> items;
        if (const Json* list = member(descriptor, field); list && list->is_array()) {
            for (const Json& item : *list) items.push_back(item);
        }
        return items;
    }

}

bool jasperengine::editor::procmat::isGenericDescriptor(const Json& descriptor) {
    for (const char* field : {"parameters", "inputs"}) {
        for (const Json& item : listOf(descriptor, field)) {
            if (hasGenericMark(item, "name")) return true;
        }
    }
    return false;
}

std::vector<Json> jasperengine::editor::procmat::expandedInputs(const Json& descriptor, int genericSize) {
    return expandGenericList(listOf(descriptor, "inputs"), "name", genericSize);
}

std::vector<Json> jasperengine::editor::procmat::expandedOutputs(const Json& descriptor, int genericSize) {
    return expandGenericList(listOf(descriptor, "outputs"), "expr", genericSize);
}

std::vector<Json> jasperengine::editor::procmat::expandedParameters(const Json& descriptor, int genericSize) {
    return expandGenericList(listOf(descriptor, "parameters"), "name", genericSize);
}

std::vector<std::string> jasperengine::editor::procmat::outputPortIds(const std::vector<Json>& outputs) {
    std::vector<std::string> ids;
    std::map<std::string, int> seen;
    for (const Json& output : outputs) {
        std::string base = output.value("name", std::string());
        if (base.empty()) base = output.value("label", std::string());
        std::string id;
        for (char c : base) {
            const bool letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (c >= 'A' && c <= 'Z') id += (char)(c - 'A' + 'a');
            else if (letter) id += c;
            else if (!id.empty() && id.back() != '_') id += '_';
        }
        while (!id.empty() && id.back() == '_') id.pop_back();
        if (id.empty()) id = "out";
        const int count = ++seen[id];
        ids.push_back(count == 1 ? id : id + "_" + std::to_string(count));
    }
    return ids;
}

// ------------------------------------------------------------------ contexts and passes

bool ExpansionContext::hasVariant(const Generator* generator) const {
    return variants.count(generator) != 0 || (parent != nullptr && parent->hasVariant(generator));
}

void ExpansionContext::touch(const Generator* generator) {
    variants[generator];
    if (parent) parent->touch(generator);
}

int ExpansionContext::getVariant(const Generator* generator, const std::string& variant) {
    int result = -1;
    auto it = variants.find(generator);
    if (it != variants.end()) {
        auto found = std::find(it->second.begin(), it->second.end(), variant);
        if (found != it->second.end()) {
            result = (int)(found - it->second.begin());
        } else {
            it->second.push_back(variant);
        }
    } else {
        variants[generator] = {variant};
    }
    touch(generator);
    return result;
}

void ExpandedCode::merge(const ExpandedCode& other) {
    functions.insert(other.functions.begin(), other.functions.end());
    defs += other.defs;
}

std::string PassBuilder::floatSlot(const std::string& key, double value) {
    auto it = slots.find(key);
    if (it != slots.end()) return it->second;
    const size_t index = values.size();
    values.push_back(value);
    indices[key] = index;
    const char component[] = {"xyzw"[index % 4], 0};
    std::string slot = "pm.v[" + std::to_string(index / 4) + "]." + component;
    slots[key] = slot;
    return slot;
}

std::string PassBuilder::vec4Slot(const std::string& key, const double value[4]) {
    auto it = slots.find(key);
    if (it != slots.end()) return it->second;
    while (values.size() % 4) values.push_back(0.0);
    const size_t index = values.size();
    values.insert(values.end(), value, value + 4);
    indices[key] = index;
    std::string slot = "pm.v[" + std::to_string(index / 4) + "]";
    slots[key] = slot;
    return slot;
}

void PassBuilder::noteValue(const std::string& node, const std::string& param, int part, const std::string& key) {
    const size_t index = indices.at(key);
    const bool known = std::any_of(valueSlots.begin(), valueSlots.end(), [&](const ValueSlot& slot) { return slot.index == index; });
    if (!known) valueSlots.push_back({node, param, part, index});
}

std::string PassBuilder::texture(const std::string& name, TextureSource* source, int port) {
    const bool known = std::any_of(textures.begin(), textures.end(), [&](const TextureUse& use) { return use.name == name; });
    if (!known) textures.push_back({name, source, port});
    return "sampler2D(t_" + name + ", s_" + name + ")";
}

// ------------------------------------------------------------------ generators

Generator::Generator(Expander& expander, std::string name)
    : graphNode(expander.ownerNode), expander(expander), nodeIndex(expander.nextIndex()), genname("o" + std::to_string(nodeIndex)), label(std::move(name)) {}

std::optional<SourceRef> Generator::getSource(int index) const {
    auto it = sources.find(index);
    if (it == sources.end()) return std::nullopt;
    return it->second;
}

void Generator::setSource(int index, const SourceRef& source) {
    sources[index] = source;
}

void Generator::clearSource(int index) {
    sources.erase(index);
}

bool Generator::getOutputAttribute(int, const std::string&, std::string&) {
    return false;
}

ExpandedCode Generator::shaderCode(const std::string& uv, int port, ExpansionContext& context) {
    ExpandedCode result = generate(uv, port, context);
    const Json* type = expander.nodes().portType(result.outputType);
    if (const Json* convert = type ? member(*type, "convert") : nullptr; convert && convert->is_array()) {
        for (const Json& conversion : *convert) {
            const std::string to = conversion.at("type").get<std::string>();
            if (!result.outputValues.count(to)) {
                result.outputValues[to] = replaceAll(conversion.at("expr").get<std::string>(), "$(value)", result.outputValues.at(result.outputType));
            }
        }
    }
    return result;
}

TextureSource::TextureSource(Expander& expander, std::string name, Json parameters, Kind kind)
    : Generator(expander, std::move(name)), parameters(std::move(parameters)), passKind(kind) {
    const Json* filterValue = member(this->parameters, "filter");
    filter = filterValue ? truthy(*filterValue) : true;
    int size = (int)std::pow(2.0, numberOr(this->parameters, "size", 10.0));
    if (expander.maxBufferSize > 0) size = std::min(size, expander.maxBufferSize);
    bufferSize = size;
}

bool TextureSource::isGreyscale() const {
    const std::optional<SourceRef> source = getSource(0);
    return source && source->generator && source->generator->outputType(source->port) == "f";
}

std::string TextureSource::outputType(int) const {
    return isGreyscale() ? "f" : "rgba";
}

int TextureSource::iterations() const {
    return passKind == Kind::Iterate ? std::max(1, (int)numberOr(parameters, "iterations", 1.0)) : 0;
}

bool TextureSource::shrink() const {
    const Json* value = member(parameters, "shrink");
    return value && truthy(*value);
}

std::string TextureSource::textureName(int port) const {
    return genName() + (passKind == Kind::Iterate && port == 1 ? "_loop" : "_tex");
}

std::string TextureSource::sizeSlot(PassBuilder& pass) {
    const std::string key = genName() + "_size";
    const std::string slot = pass.floatSlot(key, (double)bufferSize);
    pass.sizeSlots[this] = pass.floatIndex(key);
    return slot;
}

bool TextureSource::getOutputAttribute(int port, const std::string& attribute, std::string& value) {
    // both are read in the same order as the reference: the texture is used by either attribute
    PassBuilder& current = expander.currentPass();
    const std::string texture = current.texture(textureName(port), this, port);
    const std::string size = sizeSlot(current);
    if (attribute == "texture") {
        value = texture;
    } else if (attribute == "texture_size") {
        value = size;
    } else {
        return false;
    }
    return true;
}

ExpandedCode TextureSource::generate(const std::string& uv, int port, ExpansionContext& context) {
    PassBuilder& current = expander.currentPass();
    ExpandedCode result;
    const bool grey = isGreyscale();
    result.outputType = grey ? "f" : "rgba";
    const std::string sampler = current.texture(textureName(port), this, port);
    int variant = context.getVariant(this, uv);
    if (variant == -1) {
        variant = context.getVariant(this, uv);
        const std::string size = sizeSlot(current);
        // nearest texel. A sample exactly on a texel edge (another grid's coordinates, the GPU's interpolated
        // ones) would round either way per device: the edge moves by 1/64 texel, between any grid's points
        const std::string adjusted = filter ? "fract(" + uv + ")" : "((floor(fract(" + uv + ") * " + size + " + 0.015625)+vec2(0.5))/" + size + ")";
        result.code = std::string(grey ? "float" : "vec4") + " " + genName() + "_" + std::to_string(variant) + " = textureLod(" + sampler + ", " + adjusted + ", 0.0)" + (grey ? ".r" : "") + ";\n";
    }
    result.outputValues[result.outputType] = genName() + "_" + std::to_string(variant);
    return result;
}

SwitchNode::SwitchNode(Expander& expander, std::string name, Json parameters)
    : Generator(expander, std::move(name)), parameters(std::move(parameters)) {}

int SwitchNode::outputCount() const {
    return (int)numberOr(parameters, "outputs", 2.0);
}

int SwitchNode::inputCount() const {
    return outputCount() * (int)numberOr(parameters, "choices", 2.0);
}

std::optional<SourceRef> SwitchNode::forward(int port) const {
    return getSource(port + (int)numberOr(parameters, "source", 0.0) * outputCount());
}

std::string SwitchNode::outputType(int port) const {
    const std::optional<SourceRef> source = forward(port);
    if (!source) return "rgba";
    if (!source->generator) throw ExpansionError(name() + ": a switch forwards a graph's own input");
    return source->generator->outputType(source->port);
}

ExpandedCode SwitchNode::generate(const std::string& uv, int port, ExpansionContext& context) {
    const std::optional<SourceRef> source = forward(port);
    if (source) {
        if (!source->generator) throw ExpansionError(name() + ": a switch forwards a graph's own input");
        return source->generator->generate(uv, source->port, context);
    }
    ExpandedCode result;
    result.outputType = "rgba";
    result.outputValues = {{"f", "0.0"}, {"rgb", "vec3(0.0)"}, {"rgba", "vec4(0.0, 0.0, 0.0, 0.0)"}};
    return result;
}

ShaderNode::ShaderNode(Expander& expander, std::string name, const Json& body, const Json& parameters, double seed, int genericSize)
    : Generator(expander, std::move(name)), genericSize(genericSize), seed(seed + expander.seedOffset) {
    for (const Json& function : listOf(body, "functions")) functions.push_back(function.get<std::string>());
    if (const Json* lines = member(body, "instance")) instance = joined(*lines);
    if (const Json* lines = member(body, "code")) code = joined(*lines);
    inputs = listOf(body, "inputs");
    outputs = listOf(body, "outputs");
    parameterDefs = listOf(body, "parameters");
    if (isGenericDescriptor(body)) {
        inputs = expandGenericList(inputs, "name", genericSize);
        parameterDefs = expandGenericList(parameterDefs, "name", genericSize);
        outputs = expandGenericList(outputs, "expr", genericSize);
        code = expandGenericCode(code, genericSize);
        instance = expandGenericCode(instance, genericSize);
    }
    this->parameters = Json::object();
    for (const Json& definition : parameterDefs) {
        const std::string parameter = definition.at("name").get<std::string>();
        if (const Json* given = member(parameters, parameter)) {
            this->parameters[parameter] = *given;
        } else if (const Json* fallback = member(definition, "default")) {
            this->parameters[parameter] = *fallback;
        } else {
            this->parameters[parameter] = nullptr;
        }
    }
}

int ShaderNode::inputIndex(const std::string& inputName) const {
    for (int i = 0; i < (int)inputs.size(); i++) {
        if (inputs[i].at("name").get<std::string>() == inputName) return i;
    }
    return -1;
}

std::string ShaderNode::outputType(int port) const {
    if (port >= (int)outputs.size()) throw ExpansionError(name() + ": no output " + std::to_string(port));
    return outputs[port].at("type").get<std::string>();
}

void ShaderNode::processParameters(ExpandedCode& rv, Variables& variables, bool declare) {
    PassBuilder& current = expander.currentPass();
    const auto text = [](std::string value) {
        Variable variable;
        variable.text = std::move(value);
        return variable;
    };
    for (const Json& definition : parameterDefs) {
        const std::string parameter = definition.at("name").get<std::string>();
        const std::string kind = definition.at("type").get<std::string>();
        const Json& value = parameters.at(parameter);
        const std::string prefix = genName() + "_" + parameter;
        if (kind == "float") {
            if (value.is_string()) {
                variables[parameter] = text("(" + randomMacros(value.get<std::string>()) + ")");
            } else {
                variables[parameter] = text(current.floatSlot(prefix, value.is_null() ? 0.0 : numberOf(value)));
                if (!paramsNode.empty() && value.is_number()) current.noteValue(paramsNode, parameter, 0, prefix);
            }
        } else if (kind == "color") {
            double color[4] = {1.0, 1.0, 1.0, 1.0};
            if (truthy(value)) {
                color[0] = numberOf(value.at("r"));
                color[1] = numberOf(value.at("g"));
                color[2] = numberOf(value.at("b"));
                color[3] = numberOr(value, "a", 1.0);
            }
            variables[parameter] = text(current.vec4Slot(prefix, color));
            if (!paramsNode.empty() && value.is_object() && truthy(value)) current.noteValue(paramsNode, parameter, 0, prefix);
        } else if (kind == "enum") {
            const Json* values = member(definition, "values");
            const size_t count = values ? values->size() : 0;
            size_t index = 0;
            if (value.is_number() || value.is_boolean()) {
                const double number = numberOf(value);
                if (number >= 0.0 && number < (double)count) index = (size_t)number;
            }
            variables[parameter] = text(count ? (*values)[index].at("value").get<std::string>() : std::string());
        } else if (kind == "boolean") {
            variables[parameter] = text(truthy(value) ? "true" : "false");
        } else if (kind == "size") {
            variables[parameter] = text(formatted("%.1f", std::pow(2.0, numberOf(value))));
        } else if (kind == "gradient") {
            variables[parameter] = text(gradient(prefix, parameter, value, rv, declare));
        } else if (kind == "curve") {
            variables[parameter] = text(curve(prefix, value, rv, declare));
        } else if (kind == "polygon" || kind == "polyline" || kind == "lattice") {
            const Json& points = value.at("points");
            std::string slots;
            int i = 0;
            for (const Json& point : points) {
                const double xy[4] = {numberOf(point.at("x")), numberOf(point.at("y")), 0.0, 0.0};
                if (i) slots += ", ";
                slots += current.vec4Slot(slotKey(prefix, std::to_string(i)), xy) + ".xy";
                i++;
            }
            variables[parameter] = text("vec2[" + std::to_string(points.size()) + "](" + slots + ")");
            if (kind == "lattice") {
                const Json& size = value.at("size");
                variables[parameter + "_size"] = text("ivec2(" + std::to_string((int)numberOf(size.at("x"))) + ", " + std::to_string((int)numberOf(size.at("y"))) + ")");
            }
        } else if (kind == "pixels") {
            std::string function, size;
            pixels(prefix, value, rv, declare, function, size);
            variables[parameter] = text(function);
            variables[parameter + "_size"] = text(size);
        } else {
            throw ExpansionError(name() + ": unsupported parameter type " + kind);
        }
    }
}

std::string ShaderNode::randomMacros(std::string text) {
    for (const auto& [keyword, function] : {std::pair<const char*, const char*>{"rnd", "param_rnd"}, {"rndi", "param_rndi"}}) {
        const std::string needle = "$" + std::string(keyword) + "(";
        for (;;) {
            const size_t position = text.find(needle);
            if (position == std::string::npos) break;
            const size_t open = position + needle.size() - 1;
            const size_t end = matchingClose(text, open);
            const std::string inner = text.substr(open + 1, end - open - 1);
            expander.noteFunction(this, function);
            text = text.substr(0, position) + function + "(" + inner + ", $seed+" + formatted("%f", std::sin((double)position) * 0.5 + 0.5) + ")" + text.substr(end + 1);
        }
    }
    return text;
}

std::string ShaderNode::gradient(const std::string& prefix, const std::string& parameter, const Json& given, ExpandedCode& rv, bool declare) {
    PassBuilder& current = expander.currentPass();
    const RampKeys keys = rampKeys(given);
    const std::vector<Json>& points = keys.points;
    const int interpolation = keys.interpolation;
    std::vector<std::string> pos, col;
    for (size_t i = 0; i < points.size(); i++) {
        pos.push_back(current.floatSlot(slotKey(prefix, "p" + std::to_string(i)), numberOf(points[i].at("pos"))));
    }
    for (size_t i = 0; i < points.size(); i++) {
        const std::vector<double> colour = colourOf(points[i]);
        col.push_back(current.vec4Slot(slotKey(prefix, "c" + std::to_string(i)), colour.data()));
    }
    if (!paramsNode.empty() && keys.given) {
        for (size_t i = 0; i < points.size(); i++) {
            current.noteValue(paramsNode, parameter, (int)(2 * i), slotKey(prefix, "p" + std::to_string(i)));
            current.noteValue(paramsNode, parameter, (int)(2 * i + 1), slotKey(prefix, "c" + std::to_string(i)));
        }
    }
    const std::string function = prefix + "_gradient_fct";
    if (declare) {
        const int count = (int)points.size();
        std::string body = "vec4 " + function + "(float x) {\n";
        if (interpolation == 0) {
            body += "  if (x < " + (count > 1 ? pos[1] : std::string("0.0")) + ") return " + col[0] + ";\n";
            for (int i = 1; i < count - 1; i++) body += "  if (x < " + pos[i + 1] + ") return " + col[i] + ";\n";
            body += "  return " + col.back() + ";\n";
        } else {
            const std::string blend = interpolation == 1 ? "(" : "0.5-0.5*cos(3.14159265359*";
            body += "  if (x < " + pos[0] + ") return " + col[0] + ";\n";
            for (int i = 0; i < count - 1; i++) {
                const std::string t = "(x-" + pos[i] + ")/(" + pos[i + 1] + "-" + pos[i] + ")";
                if (interpolation == 3) {
                    const std::string b = "mix(" + col[i] + ", " + col[i + 1] + ", " + t + ")";
                    if (i > 0 && i < count - 2) {
                        const std::string a = "mix(" + col[i - 1] + ", " + col[i] + ", (x-" + pos[i - 1] + ")/(" + pos[i] + "-" + pos[i - 1] + "))";
                        const std::string c = "mix(" + col[i + 1] + ", " + col[i + 2] + ", (x-" + pos[i + 1] + ")/(" + pos[i + 2] + "-" + pos[i + 1] + "))";
                        body += "  if (x < " + pos[i + 1] + ") return 0.5*(" + b + " + mix(" + a + ", " + c + ", 0.5-0.5*cos(3.14159265359*" + t + ")));\n";
                    } else if (i > 0) {
                        const std::string a = "mix(" + col[i - 1] + ", " + col[i] + ", (x-" + pos[i - 1] + ")/(" + pos[i] + "-" + pos[i - 1] + "))";
                        body += "  if (x < " + pos[i + 1] + ") return mix(" + a + ", " + b + ", 0.5+0.5*" + t + ");\n";
                    } else if (i < count - 2) {
                        const std::string c = "mix(" + col[i + 1] + ", " + col[i + 2] + ", (x-" + pos[i + 1] + ")/(" + pos[i + 2] + "-" + pos[i + 1] + "))";
                        body += "  if (x < " + pos[i + 1] + ") return mix(" + c + ", " + b + ", 1.0-0.5*" + t + ");\n";
                    } else {
                        body += "  if (x < " + pos[i + 1] + ") return " + b + ";\n";
                    }
                } else {
                    body += "  if (x < " + pos[i + 1] + ") return mix(" + col[i] + ", " + col[i + 1] + ", " + blend + t + "));\n";
                }
            }
            body += "  return " + col.back() + ";\n";
        }
        rv.defs += body + "}\n";
    }
    return function;
}

std::string ShaderNode::curve(const std::string& prefix, const Json& given, ExpandedCode& rv, bool declare) {
    PassBuilder& current = expander.currentPass();
    static const Json fallback = Json::parse(R"({"points": [{"x": 0, "y": 0, "ls": 0, "rs": 1}, {"x": 1, "y": 1, "ls": 1, "rs": 0}]})");
    const Json& points = (truthy(given) ? given : fallback).at("points");
    const std::string function = prefix + "_curve_fct";
    std::vector<std::string> slots;
    int index = 0;
    for (const Json& point : points) {
        const double values[4] = {numberOf(point.at("x")), numberOf(point.at("y")), numberOf(point.at("ls")), numberOf(point.at("rs"))};
        slots.push_back(current.vec4Slot(slotKey(prefix, std::to_string(index++)), values));
    }
    if (declare) {
        std::string body = "float " + function + "(float x) {\n";
        const int count = (int)slots.size();
        for (int i = 0; i < count - 1; i++) {
            if (i < count - 2) body += "if (x <= " + slots[i + 1] + ".x) ";
            body += "{\n";
            body += "float dx = x - " + slots[i] + ".x;\nfloat d = " + slots[i + 1] + ".x - " + slots[i] + ".x;\nfloat t = dx/d;\n";
            body += "float omt = (1.0 - t);\nfloat omt2 = omt * omt;\nfloat omt3 = omt2 * omt;\nfloat t2 = t * t;\nfloat t3 = t2 * t;\nd /= 3.0;\n";
            body += "float y1 = " + slots[i] + ".y;\nfloat yac = " + slots[i] + ".y + d*" + slots[i] + ".w;\nfloat ybc = " + slots[i + 1] + ".y - d*" + slots[i + 1] + ".z;\nfloat y2 = " + slots[i + 1] + ".y;\n";
            body += "return y1*omt3 + yac*omt2*t*3.0 + ybc*omt*t2*3.0 + y2*t3;\n}\n";
        }
        rv.defs += body + "}\n";
    }
    return function;
}

void ShaderNode::pixels(const std::string& prefix, const Json& value, ExpandedCode& rv, bool declare, std::string& function, std::string& size) {
    PassBuilder& current = expander.currentPass();
    const int width = (int)numberOf(value.at("w")), height = (int)numberOf(value.at("h")), bpp = (int)numberOf(value.at("bpp"));
    std::vector<std::string> palette;
    int index = 0;
    for (const Json& color : value.at("pl")) {
        const double values[4] = {numberOf(color.at("r")), numberOf(color.at("g")), numberOf(color.at("b")), numberOr(color, "a", 1.0)};
        palette.push_back(current.vec4Slot(slotKey(prefix, std::to_string(index++)), values));
    }
    function = prefix + "_pixels_fct";
    size = prefix + "_size";
    if (declare) {
        std::string words;
        for (const Json& word : value.at("px")) {
            // stored as an unsigned 32-bit pattern, read in GLSL as a signed int
            const int32_t bits = (int32_t)(uint32_t)(int64_t)numberOf(word);
            if (!words.empty()) words += ", ";
            words += std::to_string(bits);
        }
        std::string colors;
        for (size_t i = 0; i < palette.size(); i++) colors += (i ? ", " : "") + palette[i];
        const std::string w = std::to_string(width), h = std::to_string(height), count = std::to_string(value.at("px").size());
        const std::string perWord = std::to_string(32 / bpp), mask = std::to_string((1 << bpp) - 1), bits = std::to_string(bpp);
        rv.defs += "const int " + prefix + "_pixels[" + count + "] = int[" + count + "](" + words + ");\n" +
                   "const ivec2 " + prefix + "_size = ivec2(" + w + ", " + h + ");\n" +
                   "vec4 " + function + "(ivec2 v) {\n\tvec4 palette[" + std::to_string(palette.size()) + "] = vec4[" + std::to_string(palette.size()) + "](" + colors + ");\n" +
                   "\tif (v.x < 0 || v.x >= " + w + " || v.y < 0 || v.y >= " + h + ") {\n\t\treturn palette[0];\n\t} else {\n" +
                   "\t\tint i = v.x+" + w + "*v.y;\n\t\tint c = " + mask + " & (" + prefix + "_pixels[i/" + perWord + "] >> (" + bits + "*(i%" + perWord + ")));\n" +
                   "\t\treturn palette[c];\n\t}\n}\n";
    }
}

void ShaderNode::generateInputFunction(int index, const Json& definition, ExpandedCode& rv, ExpansionContext& context) {
    const std::optional<SourceRef> source = getSource(index);
    if (!source) return;
    if (!source->generator) throw ExpansionError(name() + ": an input is wired to a graph's own input");
    PassBuilder& current = expander.currentPass();
    const size_t floatsBefore = current.values.size(), texturesBefore = current.textures.size();
    ExpansionContext local(&context);
    const std::string inputName = definition.at("name").get<std::string>();
    const std::string type = definition.at("type").get<std::string>();
    const Json& portType = expander.portType(type);
    ExpandedCode sourceRv = source->generator->shaderCode(portType.at("params").get<std::string>(), source->port, local);
    rv.merge(sourceRv);
    rv.defs += portType.at("type").get<std::string>() + " " + genName() + "_input_" + inputName + "(" + portType.at("paramdefs").get<std::string>() + ", " +
               "float _seed_variation_, vec4 _controlled_variation_) {\n";
    rv.defs += sourceRv.code + "\nreturn " + sourceRv.outputValues.at(type) + ";\n}\n";
    current.edges.push_back({this, index, sourceRv.defs.size() + sourceRv.code.size(), current.values.size() - floatsBefore, current.textures.size() - texturesBefore});
    // a function called from a loop runs its whole source per iteration, which a Buffer turns into one sample
    if ((instance.find("for (") != std::string::npos || instance.find("for(") != std::string::npos) && instance.find("$" + inputName) != std::string::npos) {
        current.loopedInputs.insert({this, index});
    }
}

std::string ShaderNode::replaceInput(int index, const std::string& suffix, const std::string& arguments, Variables& variables, ExpandedCode& rv, ExpansionContext& context) {
    const Json& definition = inputs[index];
    const std::optional<SourceRef> source = getSource(index);
    const std::string inputName = definition.at("name").get<std::string>();
    if (!source) {
        if (suffix == "texture" || suffix == "size") throw ExpansionError(name() + ": input " + inputName + " has to be connected: it is read as a texture");
        // .variation() has more arguments than coordinates, and only those reach a default
        const std::string coordinates = suffix == "variation" ? splitArguments(arguments)[0] : arguments;
        const Variable oldUv = variables["uv"];
        variables["uv"].text = parenthesized(coordinates);
        const Json* fallback = member(definition, "default");
        std::string replaced = replaceVariables(fallback ? textOf(*fallback) : std::string(), variables, rv, context);
        variables["uv"] = oldUv;
        return replaced;
    }
    if (!source->generator) throw ExpansionError(name() + ": an input is wired to a graph's own input");
    if (!suffix.empty()) {
        std::string value;
        if (source->generator->getOutputAttribute(source->port, suffix == "size" ? "texture_size" : suffix, value)) {
            if (suffix == "texture") {
                if (auto* texture = dynamic_cast<TextureSource*>(source->generator)) texture->setNeedsMips();
            }
            return value;
        }
        if (suffix == "texture" || suffix == "size") {
            throw NeedsBufferError(name() + ": input " + inputName + " needs a texture for ." + suffix, this, index);
        }
    }
    const Json* function = member(definition, "function");
    if (function && truthy(*function)) {
        const std::string called = genName() + "_input_" + inputName;
        if (suffix == "variation") {
            const std::vector<std::string> args = splitArguments(arguments);
            std::string tail = ")";
            if (args.size() == 1) {
                tail = ", _seed_variation_, _controlled_variation_)";
            } else if (args.size() == 2) {
                tail = ", _controlled_variation_)";
            }
            std::string list;
            for (size_t i = 0; i < args.size(); i++) list += (i ? ", " : "") + args[i];
            return called + "(" + list + tail;
        }
        return called + "(" + arguments + ", _seed_variation_, _controlled_variation_)";
    }
    PassBuilder& current = expander.currentPass();
    const size_t floatsBefore = current.values.size(), texturesBefore = current.textures.size();
    auto sourceRv = std::make_shared<ExpandedCode>(source->generator->shaderCode(arguments, source->port, context));
    rv.merge(*sourceRv);
    rv.code += sourceRv->code;
    current.edges.push_back({this, index, sourceRv->defs.size() + sourceRv->code.size(), current.values.size() - floatsBefore, current.textures.size() - texturesBefore});
    rv.alias = sourceRv;
    const auto value = sourceRv->outputValues.find(definition.at("type").get<std::string>());
    if (value == sourceRv->outputValues.end()) {
        throw ExpansionError(name() + ": a " + sourceRv->outputType + " output cannot feed input " + inputName);
    }
    return value->second;
}

std::string ShaderNode::replaceVariables(std::string string, Variables& variables, ExpandedCode& rv, ExpansionContext& context) {
    std::string stringEnd;
    for (;;) {
        const size_t dollar = string.rfind('$');
        if (dollar == std::string::npos) break;
        size_t variableEnd;
        std::string variable;
        if (dollar + 1 < string.size() && string[dollar + 1] == '(') {
            variableEnd = matchingClose(string, dollar + 1) + 1;
            variable = string.substr(dollar + 2, variableEnd - 1 - (dollar + 2));
        } else {
            variableEnd = dollar + 1;
            while (variableEnd < string.size() && isWordLetter(string[variableEnd])) variableEnd++;
            variable = string.substr(dollar + 1, variableEnd - dollar - 1);
        }
        stringEnd = string.substr(variableEnd) + stringEnd;
        string = string.substr(0, dollar);
        auto found = variables.find(variable);
        if (found == variables.end()) throw ExpansionError(name() + ": " + variable + " not found");
        if (!found->second.isInput) {
            string += found->second.text;
            continue;
        }
        const int input = found->second.index;
        bool replaceWhole = dollar == 0;
        stringEnd = stripped(stringEnd);
        std::string arguments, suffix;
        if (!stringEnd.empty() && stringEnd[0] == '.') {
            stringEnd = stripped(stringEnd.substr(1));
            size_t end = 0;
            while (end < stringEnd.size() && isWordLetter(stringEnd[end])) end++;
            suffix = stringEnd.substr(0, end);
            stringEnd = stripped(stringEnd.substr(end));
        }
        if (!stringEnd.empty() && stringEnd[0] == '(') {
            const size_t close = matchingClose(stringEnd, 0);
            if (close < stringEnd.size() - 1) replaceWhole = false;
            arguments = stringEnd.substr(0, close + 1);
            if (arguments.at(1) == '(' && matchingClose(arguments, 1) == arguments.size() - 2) {
                arguments = arguments.substr(1, arguments.size() - 2);
            }
            stringEnd = stringEnd.substr(close + 1);
            if (arguments.at(0) == '(' && matchingClose(arguments, 0) == arguments.size() - 1) {
                arguments = arguments.substr(1, arguments.size() - 2);
            }
        } else {
            replaceWhole = false;
        }
        string += replaceInput(input, suffix, arguments, variables, rv, context);
        if (!replaceWhole) rv.alias.reset();
    }
    return string + stringEnd;
}

ExpandedCode ShaderNode::generate(const std::string& uv, int port, ExpansionContext& context) {
    ExpandedCode rv;
    PassBuilder& current = expander.currentPass();
    if (!graphNode.empty()) current.nodes.insert(graphNode);
    const bool declare = !context.hasVariant(this);
    if (port >= (int)outputs.size()) throw ExpansionError(name() + ": no output " + std::to_string(port));
    const Json& output = outputs[port];
    const std::string expr = output.at("expr").get<std::string>();
    Variables variables;
    const auto setText = [&](const std::string& key, std::string value) {
        Variable variable;
        variable.text = std::move(value);
        variables[key] = std::move(variable);
    };
    setText("uv", parenthesized(uv));
    setText("name", genName());
    setText("?1", "_controlled_variation_.x");
    setText("?2", "_controlled_variation_.y");
    setText("?3", "_controlled_variation_.z");
    setText("?4", "_controlled_variation_.w");
    setText("node_id", std::to_string(index()));
    setText("seed", "(" + current.floatSlot(genName() + "_seed", seed) + "+fract(_seed_variation_))");
    for (const auto& [parameter, slot] : namedParameters) setText(parameter, current.floatSlot(slot.first, slot.second));
    if ((instance + code + expr).find("$time") != std::string::npos) setText("time", current.floatSlot("elapsed_time", 0.0));
    processParameters(rv, variables, declare);
    for (int i = 0; i < (int)inputs.size(); i++) {
        const Json& definition = inputs[i];
        const Json* function = member(definition, "function");
        if (declare && function && truthy(*function)) generateInputFunction(i, definition, rv, context);
        Variable variable;
        variable.isInput = true;
        variable.index = i;
        variables[definition.at("name").get<std::string>()] = std::move(variable);
    }
    const std::set<std::string>* extra = expander.extraFunctions(this);
    if (declare) {
        rv.functions.insert(functions.begin(), functions.end());
        if (extra) rv.functions.insert(extra->begin(), extra->end());
        if (!stripped(instance).empty()) rv.defs += replaceVariables(instance, variables, rv, context) + "\n\n";
    }
    if (!stripped(code).empty() && expr.find("@NOCODE") == std::string::npos) {
        const int variantIndex = context.getVariant(this, uv);
        setText("name_uv", genName() + "_" + std::to_string(context.getVariant(this, uv)));
        if (variantIndex == -1) {
            const std::string replaced = replaceVariables(code, variables, rv, context);
            if (!replaced.empty()) rv.code += replaced + "\n";
        }
    } else if (!variables.count("name_uv")) {
        setText("name_uv", genName() + "_nouv");
    }
    const std::string variantString = uv + "," + std::to_string(port);
    int variantIndex = context.getVariant(this, variantString);
    bool assign = false;
    if (variantIndex == -1) {
        variantIndex = context.getVariant(this, variantString);
        assign = true;
        current.samples[{this, port}]++;
    }
    const std::string kind = output.at("type").get<std::string>();
    const bool keepType = expr.find("@KEEPTYPE") != std::string::npos;
    const std::string text = stripped(replaceAll(replaceAll(expr, "@NOCODE", ""), "@KEEPTYPE", ""));
    const std::string expression = replaceVariables(text, variables, rv, context);
    extra = expander.extraFunctions(this);
    if (extra) rv.functions.insert(extra->begin(), extra->end());
    if (keepType && rv.alias) {
        const std::shared_ptr<ExpandedCode> alias = rv.alias;
        rv.outputValues = alias->outputValues;
        rv.outputType = alias->outputType;
        return rv;
    }
    const std::string variable = genName() + "_" + std::to_string(port) + "_" + std::to_string(variantIndex) + "_" + kind;
    if (assign) rv.code += expander.portType(kind).at("type").get<std::string>() + " " + variable + " = " + expression + ";\n";
    rv.outputValues[kind] = variable;
    rv.outputType = kind;
    return rv;
}

// ------------------------------------------------------------------ the expander

Expander::Expander(std::shared_ptr<const NodeLibrary> library) : library(std::move(library)) {}

void Expander::reset() {
    generators.clear();
    extra.clear();
    counter = 0;
    pass = nullptr;
}

void Expander::noteFunction(const Generator* generator, const std::string& function) {
    extra[generator].insert(function);
}

const std::set<std::string>* Expander::extraFunctions(const Generator* generator) const {
    auto it = extra.find(generator);
    return it == extra.end() ? nullptr : &it->second;
}

const Json& Expander::portType(const std::string& name) const {
    const Json* type = library->portType(name);
    if (!type) throw ExpansionError("unknown port type " + name);
    return *type;
}

PassBuilder& Expander::currentPass() const {
    if (!pass) throw ExpansionError("no pass is being generated");
    return *pass;
}

NodeInstance Expander::childEntry(Generator* generator) {
    NodeInstance instance;
    instance.generator = generator;
    int outputs = 2, inputs = 2;
    if (auto* switchNode = dynamic_cast<SwitchNode*>(generator)) {
        outputs = switchNode->outputCount();
        inputs = switchNode->inputCount();
    } else if (auto* shader = dynamic_cast<ShaderNode*>(generator)) {
        outputs = shader->outputCount();
        inputs = shader->inputCount();
    }
    for (int i = 0; i < outputs; i++) instance.outputs.push_back(SourceRef{generator, i, false});
    for (int i = 0; i < inputs; i++) {
        InputTarget target;
        target.generator = generator;
        target.port = i;
        instance.inputs.push_back(std::move(target));
    }
    return instance;
}

NodeInstance Expander::instantiate(const std::string& descriptorId, const Json& parameters, double seed, int genericSize) {
    const Json* descriptor = library->descriptor(descriptorId);
    if (!descriptor) throw ExpansionError("unknown node type " + descriptorId);
    if (descriptor->value("kind", std::string()) == "shader") {
        return childEntry(make<ShaderNode>(descriptorId, *descriptor, parameters, seed, genericSize));
    }
    return instantiateGraph(descriptorId, *descriptor, parameters);
}

void Expander::connect(const InputTarget& target, const SourceRef& source) {
    if (target.isGroup) {
        for (const InputTarget& inner : target.group) connect(inner, source);
    } else {
        target.generator->setSource(target.port, source);
    }
}

NodeInstance Expander::instantiateGraph(const std::string& owner, const Json& graph, const Json& parameters) {
    std::map<std::string, Json> values;
    std::vector<std::pair<std::string, std::pair<std::string, double>>> named;
    const std::vector<Json> definitions = listOf(graph, "parameters");
    const auto parameterValue = [&](const Json& definition) -> Json {
        const std::string parameter = definition.at("name").get<std::string>();
        if (const Json* given = member(parameters, parameter)) return *given;
        if (const Json* fallback = member(definition, "default")) return *fallback;
        return nullptr;
    };
    for (const Json& definition : definitions) {
        const Json value = parameterValue(definition);
        for (const Json& link : listOf(definition, "links")) {
            values[link.at("node").get<std::string>()][link.at("param").get<std::string>()] = value;
        }
        if (const Json* configurations = member(definition, "configurations"); configurations && truthy(*configurations)) {
            std::vector<std::string> keys;
            for (auto it = configurations->begin(); it != configurations->end(); ++it) keys.push_back(it.key());
            std::sort(keys.begin(), keys.end());
            const int index = value.is_boolean() ? (value.get<bool>() ? 1 : 0) : (truthy(value) ? (int)numberOf(value) : 0);
            for (const Json& link : configurations->at(keys[(size_t)std::min(index, (int)keys.size() - 1)])) {
                values[link.at("node").get<std::string>()][link.at("param").get<std::string>()] = link.at("value");
            }
        }
    }
    for (const Json& definition : definitions) {
        const Json* isNamed = member(definition, "named");
        if (!isNamed || !truthy(*isNamed)) continue;
        const std::string parameter = definition.at("name").get<std::string>();
        const Json* given = member(parameters, parameter);
        const Json* fallback = member(definition, "default");
        const double value = given ? numberOf(*given) : (fallback ? numberOf(*fallback) : 0.0);
        named.push_back({parameter, {owner + "_" + parameter, value}});
    }

    std::map<std::string, NodeInstance> children;
    for (const Json& node : listOf(graph, "nodes")) {
        const std::string name = node.at("id").get<std::string>();
        Json merged = Json::object();
        if (const Json* own = member(node, "parameters"); own && own->is_object()) merged = *own;
        if (auto overrides = values.find(name); overrides != values.end()) {
            for (auto it = overrides->second.begin(); it != overrides->second.end(); ++it) merged[it.key()] = *it;
        }
        const double seed = numberOr(node, "seed", 0.0);
        const int generic = (int)numberOr(node, "generic_size", 1.0);
        const std::string type = node.value("type", std::string());
        NodeInstance child;
        if (const Json* shader = member(node, "shader")) {
            child = childEntry(make<ShaderNode>(owner + "/" + name, *shader, merged, seed, generic));
        } else if (const Json* inner = member(node, "graph")) {
            child = instantiateGraph(owner + "/" + name, *inner, merged);
        } else if (type == "buffer" || type == "iterate_buffer") {
            child = childEntry(make<TextureSource>(owner + "/" + name, merged, type == "iterate_buffer" ? TextureSource::Kind::Iterate : TextureSource::Kind::Buffer));
        } else if (type == "switch") {
            child = childEntry(make<SwitchNode>(owner + "/" + name, merged));
        } else if (type == "ios") {
            throw ExpansionError(owner + ": a free-standing ios node " + name);
        } else {
            child = instantiate(type, merged, seed, generic);
        }
        if (auto* shaderNode = dynamic_cast<ShaderNode*>(child.generator)) shaderNode->namedParameters = named;
        children[name] = std::move(child);
    }

    // the interface of the graph: ports wired below
    NodeInstance result;
    result.outputs.resize(listOf(graph, "outputs").size());
    std::map<int, std::vector<InputTarget>> inputTargets;
    const std::vector<Json> connections = listOf(graph, "connections");
    for (const Json& connection : connections) {
        const std::string sourceName = connection.at("from").get<std::string>();
        const std::string targetName = connection.at("to").get<std::string>();
        const int fromPort = (int)numberOf(connection.at("from_port"));
        const int toPort = (int)numberOf(connection.at("to_port"));
        auto sourceChild = children.find(sourceName);
        if (targetName == "gen_outputs") {
            if (sourceName == "gen_inputs") continue;
            if (sourceChild != children.end() && fromPort < (int)sourceChild->second.outputs.size()) {
                result.outputs.at((size_t)toPort) = sourceChild->second.outputs[(size_t)fromPort];
            }
            continue;
        }
        auto targetChild = children.find(targetName);
        if (targetChild == children.end()) throw ExpansionError(owner + ": connection to unknown node " + targetName);
        const std::vector<InputTarget>& targets = targetChild->second.inputs;
        if (sourceName == "gen_inputs") {
            if (toPort < (int)targets.size()) inputTargets[fromPort].push_back(targets[(size_t)toPort]);
            continue;
        }
        if (sourceChild == children.end()) throw ExpansionError(owner + ": connection from unknown node " + sourceName);
        if (toPort < (int)targets.size() && fromPort < (int)sourceChild->second.outputs.size()) {
            const std::optional<SourceRef>& source = sourceChild->second.outputs[(size_t)fromPort];
            if (source) {
                connect(targets[(size_t)toPort], *source);
            } else {
                // an unwired output of an inner graph: its input stays unconnected
                InputTarget target = targets[(size_t)toPort];
                std::function<void(const InputTarget&)> clear = [&](const InputTarget& item) {
                    if (item.isGroup) {
                        for (const InputTarget& inner : item.group) clear(inner);
                    } else {
                        item.generator->clearSource(item.port);
                    }
                };
                clear(target);
            }
        }
    }
    for (const Json& connection : connections) {
        if (connection.at("from").get<std::string>() == "gen_inputs" && connection.at("to").get<std::string>() == "gen_outputs") {
            result.outputs.at((size_t)numberOf(connection.at("to_port"))) = SourceRef{nullptr, (int)numberOf(connection.at("from_port")), true};
        }
    }
    const int inputCount = (int)listOf(graph, "inputs").size();
    for (int index = 0; index < inputCount; index++) {
        InputTarget group;
        group.isGroup = true;
        if (auto targets = inputTargets.find(index); targets != inputTargets.end()) group.group = targets->second;
        result.inputs.push_back(std::move(group));
    }
    return result;
}

std::vector<std::string> Expander::functionClosure(const std::set<std::string>& ids) const {
    std::set<std::string> emitted;
    std::vector<std::string> order;
    const std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (!emitted.insert(id).second) return;
        const Json* function = library->function(id);
        if (!function) throw ExpansionError("unknown function " + id);
        std::vector<std::string> deps;
        for (const Json& dep : function->at("deps")) deps.push_back(dep.get<std::string>());
        std::sort(deps.begin(), deps.end());
        for (const std::string& dep : deps) visit(dep);
        order.push_back(id);
    };
    for (const std::string& id : ids) visit(id);
    return order;
}

std::string Expander::shaderText(const SourceRef& root, PassBuilder& current) {
    pass = &current;
    if (!root.generator) throw ExpansionError("a pass has nothing to draw");
    const std::string kind = root.generator->outputType(root.port);
    // a 2D view of a 3D output: the coordinate the port's functions take, at z = 0
    const Json& type = portType(kind);
    const std::string coordinate = type.at("params").get<std::string>();
    const std::string paramdefs = type.at("paramdefs").get<std::string>();
    const std::string coordinateType = paramdefs.substr(0, paramdefs.find(' '));
    const std::string setup = coordinate == "uv" ? std::string()
        : "\t" + coordinateType + " " + coordinate + " = " + coordinateType + "(uv, " + (coordinateType == "vec3" ? "0.0" : "0.0, 0.0") + ");";
    ExpansionContext context;
    ExpandedCode rv = root.generator->shaderCode(coordinate, root.port, context);
    static const std::map<std::string, std::string> colors = {
        {"f", "vec4(vec3(@), 1.0)"}, {"rgb", "vec4(@, 1.0)"}, {"rgba", "@"}, {"sdf2d", "vec4(vec3(@), 1.0)"},
        {"fill", "@"}, {"sdf3d", "vec4(vec3(@), 1.0)"}, {"sdf3dc", "vec4(vec3(@.x), 1.0)"},
        {"tex3d", "vec4(@, 1.0)"}, {"tex3d_gs", "vec4(vec3(@), 1.0)"},
    };
    auto colorTemplate = colors.find(rv.outputType);
    if (colorTemplate == colors.end()) throw ExpansionError("a pass cannot show a " + rv.outputType + " output");
    const std::string color = replaceAll(colorTemplate->second, "@", rv.outputValues.at(rv.outputType));
    std::set<std::string> needed = rv.functions;
    needed.insert("jp_finite");
    std::string text = "#version 450\nin vec2 v_uv;\nout vec4 g_finalColor;\n";
    for (const PassBuilder::TextureUse& use : current.textures) {
        text += "uniform texture2D t_" + use.name + ";\nuniform sampler s_" + use.name + ";\n";
    }
    if (!current.values.empty()) {
        text += "uniform u_fs_procmat {\n\tvec4 v[" + std::to_string((current.values.size() + 3) / 4) + "];\n} pm;\n";
    }
    bool firstFunction = true;
    for (const std::string& id : functionClosure(needed)) {
        if (!firstFunction) text += "\n";
        text += joined(library->function(id)->at("code"));
        firstFunction = false;
    }
    text += "\n" + rv.defs + "\nvoid main() {\n\tfloat _seed_variation_ = 0.0;\n\tvec4 _controlled_variation_ = vec4(0.0);\n\tvec2 uv = v_uv;\n" +
            setup + "\n" + rv.code + "\n\tg_finalColor = jp_finite(" + color + ");\n}\n";
    pass = nullptr;
    return text;
}

bool jasperengine::editor::procmat::isValueEdit(const std::string& kind, const Json& before, const Json& after) {
    try {
        if (kind == "float") return before.is_number() && after.is_number();
        if (kind == "color") {
            if (!before.is_object() || !after.is_object() || !truthy(before) || !truthy(after)) return false;
            colourOf(before);
            colourOf(after);
            return true;
        }
        if (kind == "gradient") {
            if (!truthy(before) || !truthy(after) || before.is_object() != after.is_object()) return false;
            const RampKeys a = rampKeys(before), b = rampKeys(after);
            if (a.order != b.order || a.interpolation != b.interpolation) return false;
            for (const Json& point : b.points) colourOf(point);
            return true;
        }
    } catch (const std::exception&) {
        // a value the expander would refuse: planning says why
    }
    return false;
}

std::vector<double> jasperengine::editor::procmat::valueSlotFloats(const std::string& kind, const Json& value, int part) {
    if (kind == "float") return {numberOf(value)};
    if (kind == "color") return colourOf(value);
    const RampKeys keys = rampKeys(value);
    const Json& point = keys.points.at((size_t)(part / 2));
    return part % 2 == 0 ? std::vector<double>{numberOf(point.at("pos"))} : colourOf(point);
}
