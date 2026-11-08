// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "object/TerrainOpsJson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace jasperengine::terrain {

using Json = nlohmann::json;

namespace {

    bool opJsonHas(const Json& object, const char* key) {
        return object.is_object() && object.contains(key) && !object[key].is_null();
    }

    bool opJsonPair(const Json& value, float& a, float& b) {
        if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) return false;
        a = value[0].get<float>();
        b = value[1].get<float>();
        return std::isfinite(a) && std::isfinite(b);
    }

    std::string opJsonOpNames() {
        std::string names;
        for (int type = 0; type < static_cast<int>(OpType::Registered); type++) {
            if (static_cast<OpType>(type) == OpType::FillNoise) continue;
            if (!names.empty()) names += ", ";
            names += opTypeName(static_cast<OpType>(type));
        }
        return names;
    }

    OpParam opJsonParam(const Json& value) {
        OpParam param;
        if (value.is_boolean()) {
            param.kind = OpParam::Kind::Bool;
            param.boolean = value.get<bool>();
        } else if (value.is_number()) {
            param.kind = OpParam::Kind::Number;
            param.number = value.get<double>();
        } else if (value.is_string()) {
            param.kind = OpParam::Kind::String;
            param.text = value.get<std::string>();
        } else if (value.is_array()) {
            param.kind = OpParam::Kind::List;
            for (const Json& item : value) param.items.push_back(opJsonParam(item));
        } else if (value.is_object()) {
            param.kind = OpParam::Kind::Object;
            for (auto it = value.begin(); it != value.end(); ++it) param.fields.emplace_back(it.key(), opJsonParam(it.value()));
        }
        return param;
    }

    Json opJsonOfParam(const OpParam& param) {
        switch (param.kind) {
            case OpParam::Kind::Bool: return param.boolean;
            case OpParam::Kind::Number: return param.number;
            case OpParam::Kind::String: return param.text;
            case OpParam::Kind::List: {
                Json list = Json::array();
                for (const OpParam& item : param.items) list.push_back(opJsonOfParam(item));
                return list;
            }
            case OpParam::Kind::Object: {
                Json object = Json::object();
                for (const auto& [key, value] : param.fields) object[key] = opJsonOfParam(value);
                return object;
            }
            case OpParam::Kind::None: break;
        }
        return nullptr;
    }

}

OpSpace OpSpace::of(const Matrix4& model, const Vector3& worldPosition, const Vector3& worldScale, bool rotated, bool world) {
    OpSpace space;
    space.world = world;
    space.model = model;
    space.inverse = model.inverse();
    space.originY = worldPosition.y;
    space.scaleY = std::fabs(worldScale.y) > 1e-6f ? worldScale.y : 1.0f;
    space.scaleXZ = std::fabs(worldScale.x) > 1e-6f ? std::fabs(worldScale.x) : 1.0f;
    space.rotated = rotated;
    return space;
}

Vector2 OpSpace::point(float x, float z) const {
    if (!world) return Vector2(x, z);
    const Vector3 local = inverse * Vector3(x, originY, z);
    return Vector2(local.x, local.z);
}

Vector2 OpSpace::worldPoint(float x, float z) const {
    if (!world) return Vector2(x, z);
    const Vector3 w = model * Vector3(x, 0.0f, z);
    return Vector2(w.x, w.z);
}

OpJsonParser::OpJsonParser(const OpSpace& space, OpJsonHooks hooks): space(space), hooks(std::move(hooks)) {}

bool OpJsonParser::failWith(const std::string& field, const std::string& message) {
    error = where + (field.empty() ? "" : "." + field) + ": " + message;
    return false;
}

bool OpJsonParser::parseList(const Json& list, std::vector<Op>& ops) {
    if (!list.is_array() || list.empty()) {
        error = "ops must be a list of operations such as {op = \"raise\", delta = 2, shape = {type = \"circle\", center = {0, 0}, radius = 5}}.";
        return false;
    }
    ops.assign(list.size(), Op());
    for (size_t i = 0; i < list.size(); i++) {
        if (!parse(list[i], i, ops[i])) return false;
    }
    return true;
}

bool OpJsonParser::parse(const Json& item, size_t index, Op& op) {
    where = "ops[" + std::to_string(index + hooks.firstIndex) + "]";
    if (!item.is_object()) return failWith("", "must be an object with \"op\".");
    if (!opJsonHas(item, "op") || !item["op"].is_string()) return failWith("op", "is required.");
    const std::string name = item["op"].get<std::string>();
    if (!opTypeFromName(name, op.type) || op.type == OpType::FillNoise) {
        if (!registeredOpExists(name)) return failWith("op", "\"" + name + "\" is not an operation. Operations: " + opJsonOpNames() + ".");
        op.type = OpType::Registered;
        op.registeredName = name;
        return parseRegistered(item, op);
    }
    for (const auto& [type, reason] : hooks.refused) {
        if (type == op.type) return failWith("op", name + " " + reason);
    }

    static const std::set<std::string> fields = {
        "op", "shape", "filter", "height", "heights", "delta", "strength", "in_range", "out_range", "layer", "order", "normalize",
        "rules", "steps", "iterations", "seed", "noise_size", "amplitude", "octaves", "asset", "instanced", "spacing", "count",
        "scale", "rotation_jitter", "align_to_normal", "path", "format", "width", "byte_order", "flip_vertically", "range",
        "source_min", "source_max", "set_max_height", "resolution"};
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (!fields.count(it.key())) return failWith(it.key(), "is not a field of " + name + ". Fields: " + hooks.fieldsDoc + ".");
    }

    if (op.type == OpType::ImportHeightmap) {
        if (opJsonHas(item, "shape")) return failWith("shape", "import_heightmap replaces the whole map and takes no shape.");
        if (!hooks.importHeightmap) return failWith("op", "import_heightmap is for the editor only.");
        std::string field, message;
        if (!hooks.importHeightmap(item, op, field, message)) return failWith(field, message);
        return true;
    }

    if (opJsonHas(item, "shape") && !parseShape(item["shape"], op.shape)) return false;
    if (opJsonHas(item, "filter") && !parseFilter(item["filter"], op.filter)) return false;
    if (opJsonHas(item, "height")) {
        if (!item["height"].is_number()) return failWith("height", "must be a number.");
        op.height = space.height(item["height"].get<float>());
    }
    float a = 0.0f, b = 0.0f;
    if (opJsonHas(item, "heights")) {
        if (!opJsonPair(item["heights"], a, b)) return failWith("heights", "must be [start, end].");
        op.heights = FilterRange{space.height(a), space.height(b)};
    }
    if (opJsonHas(item, "delta")) {
        if (!item["delta"].is_number()) return failWith("delta", "must be a number.");
        op.delta = space.delta(item["delta"].get<float>());
    }
    if (opJsonHas(item, "strength")) {
        if (!item["strength"].is_number()) return failWith("strength", "must be a number from 0 to 1.");
        op.strength = item["strength"].get<float>();
    }
    if (opJsonHas(item, "in_range")) {
        if (!opJsonPair(item["in_range"], a, b)) return failWith("in_range", "must be [low, high].");
        op.inRange = FilterRange{space.height(a), space.height(b)};
    }
    if (opJsonHas(item, "out_range")) {
        if (!opJsonPair(item["out_range"], a, b)) return failWith("out_range", "must be [low, high].");
        op.outRange = FilterRange{space.height(a), space.height(b)};
    }
    if (opJsonHas(item, "layer")) {
        if (!item["layer"].is_number_integer()) return failWith("layer", "must be a layer index.");
        op.layer = item["layer"].get<int>();
    }
    if (opJsonHas(item, "order")) {
        if (!item["order"].is_array()) return failWith("order", "must list the new index of each layer.");
        for (const Json& value : item["order"]) {
            if (!value.is_number_integer()) return failWith("order", "must list the new index of each layer.");
            op.order.push_back(value.get<int>());
        }
    }
    if (opJsonHas(item, "normalize")) op.normalize = item["normalize"].is_boolean() && item["normalize"].get<bool>();
    if (opJsonHas(item, "steps")) op.steps = item["steps"].is_number_integer() ? item["steps"].get<int>() : 0;
    if (opJsonHas(item, "iterations")) op.iterations = item["iterations"].is_number_integer() ? item["iterations"].get<int>() : 0;
    if (opJsonHas(item, "seed")) {
        if (!item["seed"].is_number_integer() || item["seed"].get<int64_t>() < 0) return failWith("seed", "must be a whole number from 0.");
        op.seed = static_cast<uint32_t>(item["seed"].get<int64_t>());
    }
    if (opJsonHas(item, "noise_size")) op.noiseSize = space.length(item["noise_size"].is_number() ? item["noise_size"].get<float>() : 0.0f);
    if (opJsonHas(item, "amplitude")) op.amplitude = space.delta(item["amplitude"].is_number() ? item["amplitude"].get<float>() : 0.0f);
    if (opJsonHas(item, "octaves")) op.octaves = item["octaves"].is_number_integer() ? item["octaves"].get<int>() : 0;
    // R21: place_objects' plan; its asset, instanced and align_to_normal are the editor's (the commands it makes)
    if (opJsonHas(item, "spacing")) {
        if (!item["spacing"].is_number() || item["spacing"].get<float>() < 0.0f) return failWith("spacing", "must be metres from 0.");
        op.spacing = space.length(item["spacing"].get<float>());
    }
    if (opJsonHas(item, "count")) {
        if (!item["count"].is_number_integer()) return failWith("count", "must be a whole number from 1.");
        op.count = item["count"].get<int>();
    }
    if (opJsonHas(item, "scale")) {
        const Json& scale = item["scale"];
        if (scale.is_number()) {
            op.minScale = op.maxScale = scale.get<float>();
        } else if (scale.is_array() && scale.size() == 2 && scale[0].is_number() && scale[1].is_number()) {
            op.minScale = scale[0].get<float>();
            op.maxScale = scale[1].get<float>();
        } else {
            return failWith("scale", "must be a number or [min, max].");
        }
        if (op.minScale <= 0.0f || op.maxScale < op.minScale) return failWith("scale", "must be above 0, with min at most max.");
    }
    if (opJsonHas(item, "rotation_jitter")) {
        if (!item["rotation_jitter"].is_number()) return failWith("rotation_jitter", "must be a share of a turn, 0 to 1.");
        op.rotationJitter = std::clamp(item["rotation_jitter"].get<float>(), 0.0f, 1.0f);
    }
    if (opJsonHas(item, "rules")) {
        if (!item["rules"].is_array()) return failWith("rules", "must be a list of {layer, slope, height, invert, opacity, noise}.");
        for (size_t r = 0; r < item["rules"].size(); r++) {
            if (!parseRule(item["rules"][r], r, op)) return false;
        }
    }
    return true;
}

bool OpJsonParser::point(const Json& value, const std::string& field, Vector2& out, float* pressure) {
    if (!value.is_array() || value.size() < 2 || value.size() > (pressure ? 3u : 2u) || !value[0].is_number() || !value[1].is_number()) {
        return failWith(field, pressure ? "points are [x, z] or [x, z, pressure]." : "must be [x, z].");
    }
    out = space.point(value[0].get<float>(), value[1].get<float>());
    if (pressure) *pressure = value.size() == 3 && value[2].is_number() ? value[2].get<float>() : 1.0f;
    return true;
}

bool OpJsonParser::parseShape(const Json& value, Shape& shape) {
    if (!value.is_object()) return failWith("shape", "must be an object with type.");
    static const std::set<std::string> keys = {"type", "center", "radius", "min", "max", "points", "closed", "times", "path",
                                               "size", "rotation", "falloff", "feather", "spacing"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!keys.count(it.key())) return failWith("shape." + it.key(), "is not a shape field.");
    }
    const std::string type = opJsonHas(value, "type") && value["type"].is_string() ? value["type"].get<std::string>() : "all";
    if (type == "all") shape.type = ShapeType::All;
    else if (type == "circle") shape.type = ShapeType::Circle;
    else if (type == "rect") shape.type = ShapeType::Rect;
    else if (type == "polyline") shape.type = ShapeType::Polyline;
    else if (type == "mask") shape.type = ShapeType::Mask;
    else if (type == "stroke") shape.type = ShapeType::Stroke;
    else return failWith("shape.type", "must be all, circle, rect, polyline, mask or stroke.");

    if (opJsonHas(value, "center") && !point(value["center"], "shape.center", shape.center)) return false;
    if (opJsonHas(value, "radius")) shape.radius = space.length(value["radius"].is_number() ? value["radius"].get<float>() : -1.0f);
    if (opJsonHas(value, "rotation")) shape.rotation = value["rotation"].is_number() ? value["rotation"].get<float>() : 0.0f;
    if (opJsonHas(value, "closed")) shape.closed = value["closed"].is_boolean() && value["closed"].get<bool>();
    if (opJsonHas(value, "feather")) shape.feather = value["feather"].is_number() ? value["feather"].get<float>() : -1.0f;
    if (opJsonHas(value, "spacing")) shape.spacing = space.length(value["spacing"].is_number() ? value["spacing"].get<float>() : 0.0f);
    if (opJsonHas(value, "falloff")) {
        const std::string falloff = value["falloff"].is_string() ? value["falloff"].get<std::string>() : "";
        if (falloff == "smooth") shape.falloff = Falloff::Smooth;
        else if (falloff == "linear") shape.falloff = Falloff::Linear;
        else if (falloff == "constant") shape.falloff = Falloff::Constant;
        else return failWith("shape.falloff", "must be smooth, linear or constant.");
    }
    if (shape.type == ShapeType::Rect) {
        Vector2 a, b;
        if (!opJsonHas(value, "min") || !opJsonHas(value, "max")) return failWith("shape", "a rect needs min and max.");
        if (!point(value["min"], "shape.min", a) || !point(value["max"], "shape.max", b)) return false;
        shape.min = Vector2(std::min(a.x, b.x), std::min(a.y, b.y));
        shape.max = Vector2(std::max(a.x, b.x), std::max(a.y, b.y));
    }
    if (opJsonHas(value, "points")) {
        if (!value["points"].is_array()) return failWith("shape.points", "must be a list of points.");
        const bool stroke = shape.type == ShapeType::Stroke;
        for (const Json& item : value["points"]) {
            Vector2 p;
            float pressure = 1.0f;
            if (!point(item, "shape.points", p, stroke ? &pressure : nullptr)) return false;
            shape.points.push_back(p);
            if (stroke) shape.pressures.push_back(pressure);
        }
    }
    if (opJsonHas(value, "times")) {
        if (!value["times"].is_array()) return failWith("shape.times", "must list seconds, one per point.");
        for (const Json& item : value["times"]) shape.times.push_back(item.is_number() ? item.get<float>() : 0.0f);
    }
    if (shape.type == ShapeType::Mask) {
        if (!opJsonHas(value, "path") || !value["path"].is_string()) return failWith("shape.path", "a mask needs the image's path.");
        if (!hooks.loadMask) return failWith("shape.path", "mask shapes are for the editor only.");
        std::string message;
        if (!hooks.loadMask(value["path"].get<std::string>(), shape, message)) return failWith("shape.path", message);
        float w = 0.0f, h = 0.0f;
        if (!opJsonHas(value, "size") || !opJsonPair(value["size"], w, h)) return failWith("shape.size", "a mask needs its [width, depth].");
        shape.size = Vector2(space.length(w), space.length(h));
    }
    return true;
}

bool OpJsonParser::parseFilter(const Json& value, Filter& filter) {
    if (!value.is_object()) return failWith("filter", "must be an object.");
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        float a = 0.0f, b = 0.0f;
        if (key == "slope") {
            if (!opJsonPair(it.value(), a, b)) return failWith("filter.slope", "must be [min, max] degrees.");
            filter.slope = FilterRange{a, b};
        } else if (key == "height") {
            if (!opJsonPair(it.value(), a, b)) return failWith("filter.height", "must be [min, max].");
            filter.height = FilterRange{space.height(a), space.height(b)};
        } else if (key == "layer") {
            const Json& layer = it.value();
            if (!layer.is_object() || !opJsonHas(layer, "index") || !layer["index"].is_number_integer()) {
                return failWith("filter.layer", "must be {index, min, max}.");
            }
            LayerRange range;
            range.index = layer["index"].get<int>();
            range.min = opJsonHas(layer, "min") && layer["min"].is_number() ? layer["min"].get<float>() : 0.0f;
            range.max = opJsonHas(layer, "max") && layer["max"].is_number() ? layer["max"].get<float>() : 1.0f;
            filter.layer = range;
        } else if (key == "invert") {
            filter.invert = it.value().is_boolean() && it.value().get<bool>();
        } else if (key == "invert_slope") {
            filter.invertSlope = it.value().is_boolean() && it.value().get<bool>();
        } else if (key == "invert_height") {
            filter.invertHeight = it.value().is_boolean() && it.value().get<bool>();
        } else if (key == "softness") {
            filter.softness = it.value().is_number() ? it.value().get<float>() : -1.0f;
        } else {
            return failWith("filter." + key, "is not a filter field: slope, height, layer, invert, invert_slope, invert_height, softness.");
        }
    }
    return true;
}

bool OpJsonParser::parseRule(const Json& value, size_t index, Op& op) {
    const std::string field = "rules[" + std::to_string(index + hooks.firstIndex) + "]";
    if (!value.is_object() || !opJsonHas(value, "layer") || !value["layer"].is_number_integer()) return failWith(field, "needs layer (-1: the base).");
    PaintRule rule;
    rule.layer = value["layer"].get<int>();
    float a = 0.0f, b = 0.0f;
    if (opJsonHas(value, "slope")) {
        if (!opJsonPair(value["slope"], a, b)) return failWith(field + ".slope", "must be [min, max] degrees.");
        rule.slope = FilterRange{a, b};
    }
    if (opJsonHas(value, "height")) {
        if (!opJsonPair(value["height"], a, b)) return failWith(field + ".height", "must be [min, max].");
        rule.height = FilterRange{space.height(a), space.height(b)};
    }
    if (opJsonHas(value, "invert")) rule.invert = value["invert"].is_boolean() && value["invert"].get<bool>();
    if (opJsonHas(value, "opacity")) rule.opacity = value["opacity"].is_number() ? value["opacity"].get<float>() : -1.0f;
    if (opJsonHas(value, "noise")) rule.noise = value["noise"].is_number() ? value["noise"].get<float>() : -1.0f;
    op.rules.push_back(rule);
    return true;
}

// A registered op's own keys arrive in its op object (R89): everything but op, shape and filter
bool OpJsonParser::parseRegistered(const Json& item, Op& op) {
    if (opJsonHas(item, "shape") && !parseShape(item["shape"], op.shape)) return false;
    if (opJsonHas(item, "filter") && !parseFilter(item["filter"], op.filter)) return false;
    op.params.kind = OpParam::Kind::Object;
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (it.key() == "op" || it.key() == "shape" || it.key() == "filter") continue;
        op.params.fields.emplace_back(it.key(), opJsonParam(it.value()));
    }
    return true;
}

std::string mapIdName(MapId map) {
    switch (map.kind) {
        case MapKind::Height: return "height";
        case MapKind::Hole: return "hole";
        case MapKind::Blend: return "blend:" + std::to_string(map.index);
        case MapKind::Density: return "density:" + std::to_string(map.index);
    }
    return "height";
}

std::string hashHex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

Json opResultJson(const TerrainOpResult& result, const OpSpace& space, const std::vector<Op>& ops, size_t firstIndex) {
    Json changed = Json::array();
    for (const MapChangeReport& change : result.changed) {
        const Vector2 a = space.worldPoint(change.localMin.x, change.localMin.y);
        const Vector2 b = space.worldPoint(change.localMax.x, change.localMax.y);
        changed.push_back({{"map", mapIdName(change.map)}, {"rect", {change.rect.x, change.rect.y, change.rect.w, change.rect.h}},
                           {"local", {{"min", {std::min(a.x, b.x), std::min(a.y, b.y)}}, {"max", {std::max(a.x, b.x), std::max(a.y, b.y)}}}},
                           {"texels", change.texels}});
    }
    Json entries = Json::array();
    for (size_t i = 0; i < result.ops.size(); i++) {
        const OpEntry& entry = result.ops[i];
        Json item = {{"op", entry.op}, {"changed_texels", entry.changedTexels}, {"clipped_low", entry.clippedLow},
                     {"clipped_high", entry.clippedHigh}, {"filtered_out", entry.filteredOut}, {"outside", entry.outside}};
        if (entry.seed) item["seed"] = *entry.seed;
        if (i < ops.size() && (ops[i].type == OpType::CutHoles || ops[i].type == OpType::FillHoles)) {
            item["hole_texels"] = entry.holeTexels;
            item["hole_fraction"] = entry.holeFraction;
            item["map_created"] = entry.mapCreated;
            item["map_removed"] = entry.mapRemoved;
            item["collision_samples_open"] = entry.collisionSamplesOpen;
        }
        for (const auto& [key, value] : entry.fields) item[key] = opJsonOfParam(value);
        entries.push_back(item);
    }
    Json warnings = Json::array();
    for (const OpWarning& warning : result.warnings) {
        const int64_t op = warning.op >= 0 ? static_cast<int64_t>(warning.op) + static_cast<int64_t>(firstIndex) : warning.op;
        warnings.push_back({{"op", op}, {"code", warning.code}, {"message", warning.message}});
    }
    if (space.rotated) {
        warnings.push_back({{"op", -1}, {"code", "rotated_terrain"}, {"message", "The terrain is rotated: world heights follow its Y axis."}});
    }
    Json created = Json::array();
    for (const MapId& map : result.createdMaps) created.push_back(mapIdName(map));
    Json out = {{"changed", changed}, {"ops", entries}, {"warnings", warnings}, {"created_maps", created}};
    if (result.stats.valid) {
        out["stats"] = {{"height_min", space.worldHeight(result.stats.minAfter)}, {"height_max", space.worldHeight(result.stats.maxAfter)},
                        {"height_min_before", space.worldHeight(result.stats.minBefore)},
                        {"height_max_before", space.worldHeight(result.stats.maxBefore)},
                        {"mean_delta", space.worldDelta(result.stats.meanDelta)}, {"max_delta", space.worldDelta(result.stats.maxDelta)}};
    }
    return out;
}

}
