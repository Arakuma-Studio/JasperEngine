// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorOpsJson.h"

#include "vertexcolor/VertexColorInternal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace jasperengine::vertexcolor {

using Json = nlohmann::json;

namespace {

    // the fields each op takes besides "op" (R21: target, filter, channels, blend and strength where
    // they mean something for it)
    struct OpJsonFields {
        OpType type;
        bool blend;
        bool strength;
        std::vector<const char*> fields;
    };

    const std::vector<OpJsonFields>& opJsonFieldTable() {
        static const std::vector<OpJsonFields> table = {
            {OpType::Set, false, false, {"value"}},
            {OpType::Fill, true, true, {"value"}},
            {OpType::Stroke, true, true, {"points", "radius", "value", "falloff", "falloff_curve", "hardness", "spacing", "buildup", "mode",
                                          "pressure_strength", "pressure_size", "stencil", "stencil_scale", "stencil_rotation", "element"}},
            {OpType::Gradient, true, true, {"type", "from", "to", "axis", "stops", "interpolation", "hue_direction", "extend", "element"}},
            {OpType::Smooth, true, true, {"iterations"}},
            {OpType::Sharpen, true, true, {"iterations"}},
            {OpType::Noise, true, true, {"kind", "scale", "octaves", "seed", "space", "low", "high"}},
            {OpType::Adjust, true, true, {"levels", "brightness_contrast", "hsv", "invert", "clamp"}},
            {OpType::Channel, false, true, {"copy", "swap", "invert", "pack"}},
            {OpType::ReplaceColor, true, true, {"from", "to", "tolerance", "ignore_h", "ignore_s", "ignore_v", "ignore_alpha"}},
            {OpType::FromTexture, true, true, {"texture", "material_slot", "uv_set", "samples", "keep_seams", "seam_delta_e"}},
            {OpType::BakeAO, true, true, {"rays", "distance", "scope", "seed"}},
            {OpType::BakeThickness, true, true, {"rays", "distance", "scope", "seed"}},
            {OpType::BakeCurvature, true, true, {"radius_edges", "contrast"}},
            {OpType::ByDirection, true, true, {"value", "direction", "range"}},
            {OpType::Transfer, true, true, {"source_entity", "max_distance", "normal_angle", "unmatched"}},
            {OpType::Clear, false, true, {"to"}},
        };
        return table;
    }

    const OpJsonFields& opJsonFieldsOf(OpType type) {
        for (const OpJsonFields& entry : opJsonFieldTable()) {
            if (entry.type == type) return entry;
        }
        return opJsonFieldTable().front();
    }

    std::vector<std::string> opJsonFieldList(const OpJsonFields& entry) {
        std::vector<std::string> list = {"op", "target", "filter", "channels"};
        if (entry.blend) list.push_back("blend");
        if (entry.strength) list.push_back("strength");
        for (const char* field : entry.fields) list.push_back(field);
        return list;
    }

    std::string opJsonJoin(const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& name : names) {
            if (!out.empty()) out += ", ";
            out += name;
        }
        return out;
    }

    std::string opJsonOpNames() {
        std::vector<std::string> names;
        for (const OpJsonFields& entry : opJsonFieldTable()) names.push_back(opName(entry.type));
        return opJsonJoin(names);
    }

    // one op's parse: where errors point ("ops[2]") and how they end
    struct OpJsonContext {
        const OpJsonHooks& hooks;
        std::string where;
        std::string& error;

        bool fail(const std::string& field, const std::string& message) const {
            error = where + (field.empty() ? "" : "." + field) + ": " + message;
            return false;
        }
    };

    bool opJsonHas(const Json& object, const char* key) {
        return object.is_object() && object.contains(key) && !object[key].is_null();
    }

    // an enum spelled as a string: the names in order of the values
    template <class E> bool opJsonEnum(const OpJsonContext& ctx, const Json& value, const std::string& field, std::initializer_list<const char*> names, E& out) {
        std::string list;
        for (const char* name : names) list += (list.empty() ? "\"" : ", \"") + std::string(name) + "\"";
        if (!value.is_string()) return ctx.fail(field, "must be one of " + list + ".");
        const std::string text = value.get<std::string>();
        int index = 0;
        for (const char* name : names) {
            if (text == name) {
                out = (E)index;
                return true;
            }
            index++;
        }
        return ctx.fail(field, "\"" + text + "\" is not one of " + list + ".");
    }

    bool opJsonNumber(const OpJsonContext& ctx, const Json& object, const char* key, float& out, float lo, float hi, const char* fix) {
        if (!opJsonHas(object, key)) return true;
        const Json& value = object[key];
        if (!value.is_number()) return ctx.fail(key, std::string("must be a number; ") + fix + ".");
        const double v = value.get<double>();
        if (!std::isfinite(v) || v < lo || v > hi) {
            char buffer[160];
            if (hi == std::numeric_limits<float>::max()) std::snprintf(buffer, sizeof(buffer), "must be >= %g, got %g; %s.", (double)lo, v, fix);
            else std::snprintf(buffer, sizeof(buffer), "must be from %g to %g, got %g; %s.", (double)lo, (double)hi, v, fix);
            return ctx.fail(key, buffer);
        }
        out = (float)v;
        return true;
    }

    // a number that must be above (not at) lo
    bool opJsonPositive(const OpJsonContext& ctx, const Json& object, const char* key, float& out, const char* fix) {
        if (!opJsonHas(object, key)) return true;
        const Json& value = object[key];
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() <= 0.0) {
            char buffer[160];
            if (value.is_number()) std::snprintf(buffer, sizeof(buffer), "must be > 0, got %g; %s.", value.get<double>(), fix);
            else std::snprintf(buffer, sizeof(buffer), "must be a number > 0; %s.", fix);
            return ctx.fail(key, buffer);
        }
        out = value.get<float>();
        return true;
    }

    bool opJsonInt(const OpJsonContext& ctx, const Json& object, const char* key, int& out, int lo, int hi) {
        if (!opJsonHas(object, key)) return true;
        const Json& value = object[key];
        if (!value.is_number_integer() || value.get<int64_t>() < lo || value.get<int64_t>() > hi)
            return ctx.fail(key, "must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
        out = (int)value.get<int64_t>();
        return true;
    }

    bool opJsonBool(const OpJsonContext& ctx, const Json& object, const char* key, bool& out) {
        if (!opJsonHas(object, key)) return true;
        if (!object[key].is_boolean()) return ctx.fail(key, "must be true or false.");
        out = object[key].get<bool>();
        return true;
    }

    bool opJsonSeed(const OpJsonContext& ctx, const Json& object, uint32_t& out) {
        if (!opJsonHas(object, "seed")) return true;
        const Json& value = object["seed"];
        if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > (int64_t)UINT32_MAX)
            return ctx.fail("seed", "must be a whole number from 0 to 4294967295.");
        out = (uint32_t)value.get<int64_t>();
        return true;
    }

    bool opJsonVector(const OpJsonContext& ctx, const Json& value, const std::string& field, Vector3& out) {
        if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number())
            return ctx.fail(field, "must be [x, y, z].");
        out = Vector3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
        if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) return ctx.fail(field, "must be finite.");
        return true;
    }

    bool opJsonPair(const OpJsonContext& ctx, const Json& value, const std::string& field, float& a, float& b) {
        if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) return ctx.fail(field, "must be [low, high].");
        a = value[0].get<float>();
        b = value[1].get<float>();
        if (!std::isfinite(a) || !std::isfinite(b)) return ctx.fail(field, "must be finite.");
        if (a > b) return ctx.fail(field, "high must be >= low.");
        return true;
    }

    // [r, g, b(, a)] or "#hex": how many components it gave (3 or 4) for R21's default channels
    bool opJsonValue(const Json& value, Vector4& out, int& components, std::string& error) {
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (!parseHex(text, out)) {
                error = "\"" + text + "\" is not a color; give \"#RRGGBB\", \"#RRGGBBAA\" (sRGB) or [r, g, b(, a)] (linear 0..1).";
                return false;
            }
            components = (text.size() - (text[0] == '#' ? 1 : 0)) == 8 ? 4 : 3;
            return true;
        }
        if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
            float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (size_t i = 0; i < value.size(); i++) {
                if (!value[i].is_number() || !std::isfinite(value[i].get<double>())) {
                    error = "must be [r, g, b] or [r, g, b, a] numbers (linear 0..1), or \"#RRGGBB\".";
                    return false;
                }
                c[i] = value[i].get<float>();
            }
            out = Vector4(c[0], c[1], c[2], c[3]);
            components = (int)value.size();
            return true;
        }
        error = "must be [r, g, b] or [r, g, b, a] (linear 0..1), or \"#RRGGBB[AA]\" (sRGB).";
        return false;
    }

    bool opJsonValueField(const OpJsonContext& ctx, const Json& value, const std::string& field, Vector4& out, int& components) {
        std::string message;
        if (!opJsonValue(value, out, components, message)) return ctx.fail(field, message);
        return true;
    }

    int opJsonChannelIndex(const Json& value) {
        if (!value.is_string()) return -1;
        const std::string text = value.get<std::string>();
        if (text == "r") return 0;
        if (text == "g") return 1;
        if (text == "b") return 2;
        if (text == "a") return 3;
        return -1;
    }

    bool opJsonUnknown(const OpJsonContext& ctx, const Json& object, const std::vector<std::string>& valid, const std::string& prefix, const std::string& what) {
        for (auto it = object.begin(); it != object.end(); ++it) {
            bool known = false;
            for (const std::string& field : valid) known = known || field == it.key();
            if (!known) return ctx.fail(prefix + it.key(), "is not a field of " + what + ". Fields: " + opJsonJoin(valid) + " (" + ctx.hooks.fieldsDoc + ").");
        }
        return true;
    }

    bool opJsonTarget(const OpJsonContext& ctx, const Json& item, Target& target) {
        if (!item.is_object()) return ctx.fail("target", "must be an object such as {kind = \"all\"} or {kind = \"faces\", ids = [0, 1]}.");
        if (!opJsonHas(item, "kind")) return ctx.fail("target.kind", "is required: all, vertices, faces, corners, selection, sphere, box, capsule or color.");
        const Json& kindValue = item["kind"];
        static const char* const kinds[] = {"all", "vertices", "faces", "corners", "selection", "sphere", "box", "capsule", "color"};
        const std::string kind = kindValue.is_string() ? kindValue.get<std::string>() : "";
        int index = -1;
        for (int i = 0; i < 9; i++) {
            if (kind == kinds[i]) index = i;
        }
        if (index < 0) return ctx.fail("target.kind", (kindValue.is_string() ? "\"" + kind + "\" is not a target" : std::string("must be a string")) +
                                                          "; kinds: all, vertices, faces, corners, selection, sphere, box, capsule, color.");
        std::vector<std::string> fields = {"kind"};
        switch (index) {
            case 0: fields.insert(fields.end(), {"element"}); break;
            case 1: case 2: case 3: fields.insert(fields.end(), {"ids"}); break;
            case 4: break;
            case 5: fields.insert(fields.end(), {"center", "radius", "element"}); break;
            case 6: fields.insert(fields.end(), {"center", "half_extents", "rotation", "element"}); break;
            case 7: fields.insert(fields.end(), {"from", "to", "radius", "element"}); break;
            case 8: fields.insert(fields.end(), {"value", "tolerance", "ignore_h", "ignore_s", "ignore_v", "ignore_alpha", "coverage", "element"}); break;
            default: break;
        }
        if (!opJsonUnknown(ctx, item, fields, "target.", "a " + kind + " target")) return false;
        switch (index) {
            case 0: target.kind = TargetKind::All; break;
            case 1: target.kind = TargetKind::Vertices; break;
            case 2: target.kind = TargetKind::Faces; break;
            case 3: target.kind = TargetKind::Corners; break;
            case 4: {
                if (!ctx.hooks.selection) return ctx.fail("target.kind", "\"selection\" needs the editor's element selection; name the ids with a vertices, faces or corners target.");
                std::string message;
                if (!ctx.hooks.selection(target, message)) return ctx.fail("target.kind", message);
                return true;
            }
            case 5: target.kind = TargetKind::Sphere; break;
            case 6: target.kind = TargetKind::Box; break;
            case 7: target.kind = TargetKind::Capsule; break;
            case 8: target.kind = TargetKind::Color; break;
            default: break;
        }
        if (index >= 1 && index <= 3) {
            if (!opJsonHas(item, "ids") || !item["ids"].is_array()) return ctx.fail("target.ids", "is required: a list of ids such as [0, 4, 7].");
            for (const Json& id : item["ids"]) {
                if (!id.is_number_integer() || id.get<int64_t>() < 0 || id.get<int64_t>() > (int64_t)UINT32_MAX)
                    return ctx.fail("target.ids", "must be whole numbers from 0; read the ids with sample_mesh_colors elements.");
                target.ids.push_back((uint32_t)id.get<int64_t>());
            }
            return true;
        }
        if (opJsonHas(item, "element") && !opJsonEnum(ctx, item["element"], "target.element", {"vertex", "face"}, target.element)) return false;
        if (index == 5 || index == 6) {
            if (!opJsonHas(item, "center")) return ctx.fail("target.center", "is required: [x, y, z].");
            if (!opJsonVector(ctx, item["center"], "target.center", target.center)) return false;
        }
        if (index == 5 || index == 7) {
            if (!opJsonHas(item, "radius")) return ctx.fail("target.radius", "is required: a radius > 0.");
            const OpJsonContext inner{ctx.hooks, ctx.where + ".target", ctx.error};
            if (!opJsonPositive(inner, item, "radius", target.radius, "give the radius in units")) return false;
        }
        if (index == 6) {
            if (!opJsonHas(item, "half_extents")) return ctx.fail("target.half_extents", "is required: [x, y, z], half the box's size.");
            if (!opJsonVector(ctx, item["half_extents"], "target.half_extents", target.halfExtents)) return false;
            if (target.halfExtents.x < 0.0f || target.halfExtents.y < 0.0f || target.halfExtents.z < 0.0f) return ctx.fail("target.half_extents", "must be >= 0 on every axis.");
            if (opJsonHas(item, "rotation")) {
                Vector3 euler;
                if (!opJsonVector(ctx, item["rotation"], "target.rotation", euler)) return ctx.fail("target.rotation", "must be Euler angles in degrees [x, y, z].");
                target.rotation = detail::eulerDegrees(euler);
            }
        }
        if (index == 7) {
            if (!opJsonHas(item, "from") || !opJsonHas(item, "to")) return ctx.fail(opJsonHas(item, "from") ? "target.to" : "target.from", "is required: [x, y, z].");
            if (!opJsonVector(ctx, item["from"], "target.from", target.from) || !opJsonVector(ctx, item["to"], "target.to", target.to)) return false;
        }
        if (index == 8) {
            if (!opJsonHas(item, "value")) return ctx.fail("target.value", "is required: the color to match.");
            int components = 0;
            if (!opJsonValueField(ctx, item["value"], "target.value", target.color.value, components)) return false;
            if (components == 4) target.color.ignoreAlpha = false;
            OpJsonContext inner{ctx.hooks, ctx.where + ".target", ctx.error};
            if (!opJsonNumber(inner, item, "tolerance", target.color.tolerance, 0.0f, 1.0f, "0.02 matches about 5 steps of 255")) return false;
            if (!opJsonNumber(inner, item, "coverage", target.color.coverage, 0.0f, 1.0f, "1 needs all three corners of a face to match")) return false;
            if (!opJsonBool(inner, item, "ignore_h", target.color.ignoreH) || !opJsonBool(inner, item, "ignore_s", target.color.ignoreS) ||
                !opJsonBool(inner, item, "ignore_v", target.color.ignoreV) || !opJsonBool(inner, item, "ignore_alpha", target.color.ignoreAlpha))
                return false;
        }
        return true;
    }

    bool opJsonFilter(const OpJsonContext& ctx, const Json& item, const std::string& field, Filter& filter) {
        if (!item.is_object() || !opJsonHas(item, "kind")) return ctx.fail(field, "must be an object with kind: facing, slope, height, curvature, channel or occluded.");
        OpJsonContext inner{ctx.hooks, ctx.where + "." + field, ctx.error};
        if (!opJsonEnum(inner, item["kind"], "kind", {"facing", "slope", "height", "curvature", "channel", "occluded"}, filter.kind)) return false;
        std::vector<std::string> fields = {"kind", "invert"};
        switch (filter.kind) {
            case FilterKind::Facing: fields.insert(fields.end(), {"direction", "angle"}); break;
            case FilterKind::Channel: fields.insert(fields.end(), {"channel", "min", "max"}); break;
            case FilterKind::Occluded: break;
            default: fields.insert(fields.end(), {"min", "max"}); break;
        }
        if (!opJsonUnknown(ctx, item, fields, field + ".", "a " + item["kind"].get<std::string>() + " filter")) return false;
        if (!opJsonBool(inner, item, "invert", filter.invert)) return false;
        const float big = std::numeric_limits<float>::max();
        switch (filter.kind) {
            case FilterKind::Facing:
                if (opJsonHas(item, "direction") && !opJsonVector(inner, item["direction"], "direction", filter.direction)) return false;
                if (detail::length3(filter.direction) <= 0.0f) return inner.fail("direction", "must not be zero; [0, 1, 0] faces up.");
                return opJsonNumber(inner, item, "angle", filter.angle, 0.0f, 180.0f, "degrees from the direction");
            case FilterKind::Slope:
                filter.min = 0.0f;
                filter.max = 180.0f;
                if (!opJsonNumber(inner, item, "min", filter.min, 0.0f, 180.0f, "degrees from up") || !opJsonNumber(inner, item, "max", filter.max, 0.0f, 180.0f, "degrees from up")) return false;
                break;
            case FilterKind::Curvature:
                filter.min = -1.0f;
                filter.max = 1.0f;
                if (!opJsonNumber(inner, item, "min", filter.min, -1.0f, 1.0f, "-1 concave, 1 convex") || !opJsonNumber(inner, item, "max", filter.max, -1.0f, 1.0f, "-1 concave, 1 convex")) return false;
                break;
            case FilterKind::Channel: {
                if (!opJsonHas(item, "channel")) return inner.fail("channel", "is required: \"r\", \"g\", \"b\" or \"a\".");
                filter.channel = opJsonChannelIndex(item["channel"]);
                if (filter.channel < 0) return inner.fail("channel", "must be \"r\", \"g\", \"b\" or \"a\".");
                filter.min = -big;
                filter.max = big;
                if (!opJsonNumber(inner, item, "min", filter.min, -big, big, "the lowest value kept") || !opJsonNumber(inner, item, "max", filter.max, -big, big, "the highest value kept")) return false;
                break;
            }
            case FilterKind::Height:
                filter.min = -big;
                filter.max = big;
                if (!opJsonNumber(inner, item, "min", filter.min, -big, big, "the lowest Y kept") || !opJsonNumber(inner, item, "max", filter.max, -big, big, "the highest Y kept")) return false;
                break;
            case FilterKind::Occluded: return true;
        }
        if (filter.min > filter.max) return inner.fail("max", "must be >= min.");
        return true;
    }

}

OpJsonParser::OpJsonParser(OpJsonHooks hooks): hooks(std::move(hooks)) {}

bool parseValue(const Json& value, Vector4& out, std::string& error) {
    int components = 0;
    return opJsonValue(value, out, components, error);
}

bool parseChannels(const Json& value, ChannelMask& out, std::string& error) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        error = "must be a subset of \"rgba\" such as \"rgb\" or \"a\".";
        return false;
    }
    ChannelMask mask = 0;
    for (char c : value.get<std::string>()) {
        const char* names = "rgba";
        const char* found = std::strchr(names, c);
        if (c == '\0' || !found) {
            error = "\"" + value.get<std::string>() + "\" is not a subset of \"rgba\"; give letters of r, g, b, a such as \"rgb\".";
            return false;
        }
        const ChannelMask bit = (ChannelMask)(1u << (found - names));
        if (mask & bit) {
            error = "\"" + value.get<std::string>() + "\" names a channel twice.";
            return false;
        }
        mask = (ChannelMask)(mask | bit);
    }
    out = mask;
    return true;
}

std::string channelsName(ChannelMask channels) {
    std::string name;
    const char* letters = "rgba";
    for (int c = 0; c < 4; c++) {
        if (channels & (1u << c)) name += letters[c];
    }
    return name;
}

bool OpJsonParser::parseList(const Json& list, std::vector<Op>& ops) {
    if (!list.is_array() || list.empty()) {
        error = "ops must be a non-empty list of operations such as {op = \"fill\", target = {kind = \"all\"}, value = [1, 0, 0]}.";
        return false;
    }
    ops.assign(list.size(), Op());
    for (size_t i = 0; i < list.size(); i++) {
        if (!parse(list[i], i, ops[i])) return false;
    }
    return true;
}

bool OpJsonParser::parse(const Json& item, size_t index, Op& op) {
    op = Op();
    error.clear();
    const OpJsonContext ctx{hooks, "ops[" + std::to_string(index + hooks.firstIndex) + "]", error};
    if (!item.is_object()) return ctx.fail("", "must be an object with \"op\".");
    if (!opJsonHas(item, "op") || !item["op"].is_string()) return ctx.fail("op", "is required: one of " + opJsonOpNames() + ".");
    const std::string name = item["op"].get<std::string>();
    if (!opFromName(name, op.type)) return ctx.fail("op", "\"" + name + "\" is not an operation. Operations: " + opJsonOpNames() + ".");
    const OpJsonFields& table = opJsonFieldsOf(op.type);
    if (!opJsonUnknown(ctx, item, opJsonFieldList(table), "", name)) return false;

    // ---- common fields
    if (opJsonHas(item, "target") && !opJsonTarget(ctx, item["target"], op.target)) return false;
    if (opJsonHas(item, "filter")) {
        const Json& filter = item["filter"];
        if (filter.is_array()) {
            for (size_t i = 0; i < filter.size(); i++) {
                Filter f;
                if (!opJsonFilter(ctx, filter[i], "filter[" + std::to_string(i) + "]", f)) return false;
                op.filters.push_back(f);
            }
        } else {
            Filter f;
            if (!opJsonFilter(ctx, filter, "filter", f)) return false;
            op.filters.push_back(f);
        }
    }
    if (opJsonHas(item, "blend") &&
        !opJsonEnum(ctx, item["blend"], "blend", {"replace", "add", "subtract", "multiply", "screen", "overlay", "min", "max", "lerp"}, op.blend))
        return false;
    if (!opJsonNumber(ctx, item, "strength", op.strength, 0.0f, 1.0f, "1 is the full change")) return false;

    // R21: unlisted channels default from how the value was given (rgb for [r, g, b], rgba for four)
    int valueComponents = 0;
    auto readValue = [&](const char* key, Vector4& out, bool required) {
        if (!opJsonHas(item, key)) {
            if (required) return ctx.fail(key, "is required: [r, g, b(, a)] linear 0..1, or \"#RRGGBB[AA]\".");
            return true;
        }
        int components = 0;
        if (!opJsonValueField(ctx, item[key], key, out, components)) return false;
        valueComponents = std::max(valueComponents, components);
        return true;
    };
    ChannelMask defaultChannels = CHANNELS_RGBA;

    switch (op.type) {
        case OpType::Set:
        case OpType::Fill:
            if (!readValue("value", op.value, true)) return false;
            defaultChannels = valueComponents == 4 ? CHANNELS_RGBA : CHANNELS_RGB;
            break;
        case OpType::Stroke: {
            if (!opJsonHas(item, "points") || !item["points"].is_array() || item["points"].empty())
                return ctx.fail("points", "is required: a list such as [{p = [x, y, z], pressure = 1}, ...].");
            for (size_t i = 0; i < item["points"].size(); i++) {
                const Json& point = item["points"][i];
                const std::string field = "points[" + std::to_string(i) + "]";
                StrokePoint p;
                if (point.is_array()) {
                    if (!opJsonVector(ctx, point, field, p.position)) return false;
                } else if (point.is_object()) {
                    for (auto it = point.begin(); it != point.end(); ++it) {
                        if (it.key() != "p" && it.key() != "pressure") return ctx.fail(field + "." + it.key(), "is not a field of a point. Fields: p, pressure.");
                    }
                    if (!opJsonHas(point, "p")) return ctx.fail(field + ".p", "is required: [x, y, z].");
                    if (!opJsonVector(ctx, point["p"], field + ".p", p.position)) return false;
                    OpJsonContext inner{hooks, ctx.where + "." + field, error};
                    if (!opJsonNumber(inner, point, "pressure", p.pressure, 0.0f, 1.0f, "pen pressure")) return false;
                } else {
                    return ctx.fail(field, "must be {p = [x, y, z], pressure = 0..1} or [x, y, z].");
                }
                op.points.push_back(p);
            }
            if (!opJsonHas(item, "radius")) return ctx.fail("radius", "is required: the brush radius in units.");
            if (!opJsonPositive(ctx, item, "radius", op.radius, "give the brush radius in units")) return false;
            if (!opJsonHas(item, "mode") || item["mode"] == "paint") {
                if (!readValue("value", op.value, true)) return false;
            }
            if (opJsonHas(item, "mode") && !opJsonEnum(ctx, item["mode"], "mode", {"paint", "erase", "smooth", "sharpen"}, op.mode)) return false;
            if (op.mode != StrokeMode::Paint && opJsonHas(item, "value")) return ctx.fail("value", "is for mode \"paint\"; erase, smooth and sharpen take no value.");
            defaultChannels = valueComponents == 3 ? CHANNELS_RGB : CHANNELS_RGBA;
            if (opJsonHas(item, "falloff") && !opJsonEnum(ctx, item["falloff"], "falloff", {"smooth", "linear", "constant", "sphere", "custom"}, op.falloff)) return false;
            if (opJsonHas(item, "falloff_curve")) {
                const Json& curve = item["falloff_curve"];
                std::vector<std::pair<float, float>> points;
                bool good = curve.is_array() && !curve.empty();
                for (size_t i = 0; good && i < curve.size(); i++) {
                    good = curve[i].is_array() && curve[i].size() == 2 && curve[i][0].is_number() && curve[i][1].is_number();
                    if (good) points.emplace_back(curve[i][0].get<float>(), curve[i][1].get<float>());
                }
                if (!good) return ctx.fail("falloff_curve", "must be [[distance, weight], ...] with distance 0..1 from the center to the radius.");
                op.falloffCurve = Curve::polyline(points);
                if (!opJsonHas(item, "falloff")) op.falloff = FalloffPreset::Custom;
            }
            if (op.falloff == FalloffPreset::Custom && op.falloffCurve.empty()) return ctx.fail("falloff_curve", "is required with falloff \"custom\".");
            if (!opJsonNumber(ctx, item, "hardness", op.hardness, 0.0f, 1.0f, "the fraction of the radius at full strength")) return false;
            if (!opJsonNumber(ctx, item, "spacing", op.spacing, 0.01f, 10.0f, "0.25 stamps every quarter radius")) return false;
            if (!opJsonBool(ctx, item, "buildup", op.buildup)) return false;
            if (!opJsonBool(ctx, item, "pressure_strength", op.pressureStrength) || !opJsonBool(ctx, item, "pressure_size", op.pressureSize)) return false;
            if (opJsonHas(item, "stencil")) {
                if (!item["stencil"].is_string()) return ctx.fail("stencil", "must be the path of a grayscale texture.");
                if (!hooks.loadStencil) return ctx.fail("stencil", "stencil textures are read by the editor; this caller cannot load them.");
                std::string message;
                op.stencil = hooks.loadStencil(item["stencil"].get<std::string>(), message);
                if (!op.stencil) return ctx.fail("stencil", message.empty() ? "could not be read." : message);
            }
            if (!opJsonPositive(ctx, item, "stencil_scale", op.stencilScale, "the stencil's size in units")) return false;
            if (!opJsonNumber(ctx, item, "stencil_rotation", op.stencilRotation, -36000.0f, 36000.0f, "degrees")) return false;
            if (opJsonHas(item, "element") && !opJsonEnum(ctx, item["element"], "element", {"vertex", "face"}, op.target.element)) return false;
            break;
        }
        case OpType::Gradient: {
            if (opJsonHas(item, "type") && !opJsonEnum(ctx, item["type"], "type", {"linear", "radial", "height", "axis"}, op.gradientType)) return false;
            if (op.gradientType == GradientType::Linear || op.gradientType == GradientType::Radial) {
                if (!opJsonHas(item, "from") || !opJsonHas(item, "to")) return ctx.fail(opJsonHas(item, "from") ? "to" : "from", "is required for a linear or radial gradient: [x, y, z].");
                if (!opJsonVector(ctx, item["from"], "from", op.gradientFrom) || !opJsonVector(ctx, item["to"], "to", op.gradientTo)) return false;
                if (detail::length3(op.gradientTo - op.gradientFrom) <= 0.0f) return ctx.fail("to", "must differ from from.");
            } else if (opJsonHas(item, "from") || opJsonHas(item, "to")) {
                return ctx.fail(opJsonHas(item, "from") ? "from" : "to", "is for linear and radial gradients; height and axis span the target's bounds.");
            }
            if (opJsonHas(item, "axis")) {
                if (op.gradientType != GradientType::Axis) return ctx.fail("axis", "is for type \"axis\".");
                if (!opJsonVector(ctx, item["axis"], "axis", op.axis)) return false;
                if (detail::length3(op.axis) <= 0.0f) return ctx.fail("axis", "must not be zero; [0, 1, 0] runs bottom to top.");
            }
            if (!opJsonHas(item, "stops") || !item["stops"].is_array() || item["stops"].size() < 2)
                return ctx.fail("stops", "needs at least 2 stops such as [{t = 0, value = \"#000000\"}, {t = 1, value = \"#FFFFFF\"}].");
            for (size_t i = 0; i < item["stops"].size(); i++) {
                const Json& stop = item["stops"][i];
                const std::string field = "stops[" + std::to_string(i) + "]";
                if (!stop.is_object()) return ctx.fail(field, "must be {t = 0..1, value = ..., strength = 0..1}.");
                for (auto it = stop.begin(); it != stop.end(); ++it) {
                    if (it.key() != "t" && it.key() != "value" && it.key() != "strength") return ctx.fail(field + "." + it.key(), "is not a field of a stop. Fields: t, value, strength.");
                }
                GradientStop s;
                OpJsonContext inner{hooks, ctx.where + "." + field, error};
                if (!opJsonHas(stop, "t")) return inner.fail("t", "is required: the stop's position 0..1.");
                if (!opJsonNumber(inner, stop, "t", s.t, 0.0f, 1.0f, "the stop's position along the gradient")) return false;
                if (!opJsonHas(stop, "value")) return inner.fail("value", "is required.");
                int components = 0;
                if (!opJsonValueField(inner, stop["value"], "value", s.value, components)) return false;
                valueComponents = std::max(valueComponents, components);
                if (!opJsonNumber(inner, stop, "strength", s.strength, 0.0f, 1.0f, "how much of the stop's value is applied")) return false;
                op.stops.push_back(s);
            }
            defaultChannels = valueComponents == 4 ? CHANNELS_RGBA : CHANNELS_RGB;
            if (opJsonHas(item, "interpolation") && !opJsonEnum(ctx, item["interpolation"], "interpolation", {"linear", "oklab", "hsv"}, op.interpolation)) return false;
            if (opJsonHas(item, "hue_direction") && !opJsonEnum(ctx, item["hue_direction"], "hue_direction", {"near", "far", "cw", "ccw"}, op.hueDirection)) return false;
            if (opJsonHas(item, "extend") && !opJsonEnum(ctx, item["extend"], "extend", {"off", "forward", "backward", "both"}, op.extend)) return false;
            if (opJsonHas(item, "element") && !opJsonEnum(ctx, item["element"], "element", {"vertex", "face"}, op.target.element)) return false;
            break;
        }
        case OpType::Smooth:
        case OpType::Sharpen:
            if (!opJsonInt(ctx, item, "iterations", op.iterations, 1, 100)) return false;
            break;
        case OpType::Noise: {
            if (opJsonHas(item, "kind") && !opJsonEnum(ctx, item["kind"], "kind", {"value", "simplex", "cellular"}, op.noiseKind)) return false;
            if (!opJsonPositive(ctx, item, "scale", op.noiseScale, "the feature size in units")) return false;
            if (!opJsonInt(ctx, item, "octaves", op.octaves, 1, 12)) return false;
            if (!opJsonSeed(ctx, item, op.seed)) return false;
            if (opJsonHas(item, "space")) {
                int space = 0;
                if (!opJsonEnum(ctx, item["space"], "space", {"world", "local"}, space)) return false;
                op.noiseWorld = space == 0;
            }
            op.value = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
            op.value2 = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
            if (!readValue("low", op.value, false) || !readValue("high", op.value2, false)) return false;
            defaultChannels = valueComponents == 4 ? CHANNELS_RGBA : CHANNELS_RGB;
            break;
        }
        case OpType::Adjust: {
            defaultChannels = CHANNELS_RGB;
            const char* kinds[] = {"levels", "brightness_contrast", "hsv", "invert", "clamp"};
            int given = 0;
            for (const char* kind : kinds) given += opJsonHas(item, kind) ? 1 : 0;
            if (given != 1) return ctx.fail("", "adjust takes exactly one of levels, brightness_contrast, hsv, invert, clamp.");
            if (opJsonHas(item, "levels")) {
                op.adjustKind = AdjustKind::Levels;
                const Json& levels = item["levels"];
                if (!levels.is_object()) return ctx.fail("levels", "must be {in = [black, white], gamma = 1, out = [black, white]}.");
                if (!opJsonUnknown(ctx, levels, {"in", "gamma", "out"}, "levels.", "levels")) return false;
                OpJsonContext inner{hooks, ctx.where + ".levels", error};
                if (opJsonHas(levels, "in") && !opJsonPair(inner, levels["in"], "in", op.inBlack, op.inWhite)) return false;
                if (op.inWhite <= op.inBlack) return inner.fail("in", "white must be above black.");
                if (opJsonHas(levels, "out") && !opJsonPair(inner, levels["out"], "out", op.outBlack, op.outWhite)) return false;
                if (!opJsonPositive(inner, levels, "gamma", op.gamma, "1 leaves the midtones")) return false;
            } else if (opJsonHas(item, "brightness_contrast")) {
                op.adjustKind = AdjustKind::BrightnessContrast;
                const Json& bc = item["brightness_contrast"];
                if (!bc.is_object()) return ctx.fail("brightness_contrast", "must be {brightness = -1..1, contrast = -1..1}.");
                if (!opJsonUnknown(ctx, bc, {"brightness", "contrast"}, "brightness_contrast.", "brightness_contrast")) return false;
                OpJsonContext inner{hooks, ctx.where + ".brightness_contrast", error};
                if (!opJsonNumber(inner, bc, "brightness", op.brightness, -1.0f, 1.0f, "added to every channel") ||
                    !opJsonNumber(inner, bc, "contrast", op.contrast, -1.0f, 1.0f, "0 keeps the contrast"))
                    return false;
            } else if (opJsonHas(item, "hsv")) {
                op.adjustKind = AdjustKind::Hsv;
                const Json& hsv = item["hsv"];
                if (!hsv.is_object()) return ctx.fail("hsv", "must be {hue = degrees, saturation = -1..1, value = -1..1}.");
                if (!opJsonUnknown(ctx, hsv, {"hue", "saturation", "value"}, "hsv.", "hsv")) return false;
                OpJsonContext inner{hooks, ctx.where + ".hsv", error};
                if (!opJsonNumber(inner, hsv, "hue", op.hueShift, -360.0f, 360.0f, "degrees around the hue wheel") ||
                    !opJsonNumber(inner, hsv, "saturation", op.saturationShift, -1.0f, 1.0f, "added to the saturation") ||
                    !opJsonNumber(inner, hsv, "value", op.valueShift, -1.0f, 1.0f, "added to the value"))
                    return false;
            } else if (opJsonHas(item, "invert")) {
                op.adjustKind = AdjustKind::Invert;
                if (!item["invert"].is_boolean() || !item["invert"].get<bool>()) return ctx.fail("invert", "must be true.");
            } else {
                op.adjustKind = AdjustKind::Clamp;
                if (!opJsonPair(ctx, item["clamp"], "clamp", op.outBlack, op.outWhite)) return false;
            }
            break;
        }
        case OpType::Channel: {
            const char* kinds[] = {"copy", "swap", "invert", "pack"};
            int given = 0;
            for (const char* kind : kinds) given += opJsonHas(item, kind) ? 1 : 0;
            if (given != 1) return ctx.fail("", "channel takes exactly one of copy, swap, invert, pack.");
            if (opJsonHas(item, "copy")) {
                op.channelKind = ChannelOpKind::Copy;
                const Json& copy = item["copy"];
                if (!copy.is_object() || !opJsonHas(copy, "from") || !opJsonHas(copy, "to")) return ctx.fail("copy", "must be {from = \"r\", to = \"g\"}.");
                if (!opJsonUnknown(ctx, copy, {"from", "to"}, "copy.", "copy")) return false;
                op.sourceChannel = opJsonChannelIndex(copy["from"]);
                op.destChannel = opJsonChannelIndex(copy["to"]);
                if (op.sourceChannel < 0) return ctx.fail("copy.from", "must be \"r\", \"g\", \"b\" or \"a\".");
                if (op.destChannel < 0) return ctx.fail("copy.to", "must be \"r\", \"g\", \"b\" or \"a\".");
            } else if (opJsonHas(item, "swap")) {
                op.channelKind = ChannelOpKind::Swap;
                const Json& swap = item["swap"];
                std::string pair = swap.is_string() ? swap.get<std::string>() : "";
                if (swap.is_array() && swap.size() == 2 && swap[0].is_string() && swap[1].is_string()) pair = swap[0].get<std::string>() + swap[1].get<std::string>();
                const int a = pair.size() == 2 ? opJsonChannelIndex(Json(std::string(1, pair[0]))) : -1;
                const int b = pair.size() == 2 ? opJsonChannelIndex(Json(std::string(1, pair[1]))) : -1;
                if (a < 0 || b < 0 || a == b) return ctx.fail("swap", "must name two different channels such as [\"r\", \"g\"].");
                op.sourceChannel = a;
                op.destChannel = b;
            } else if (opJsonHas(item, "invert")) {
                op.channelKind = ChannelOpKind::Invert;
                std::string message;
                if (!parseChannels(item["invert"], op.channels, message)) return ctx.fail("invert", message);
                if (opJsonHas(item, "channels")) return ctx.fail("channels", "is set by invert for this op; drop one of them.");
            } else {
                op.channelKind = ChannelOpKind::Pack;
                const Json& pack = item["pack"];
                if (!pack.is_object() || pack.empty()) return ctx.fail("pack", "must be {r = \"g\" or 0.5, ...}: each listed channel takes another channel or a constant.");
                if (!opJsonUnknown(ctx, pack, {"r", "g", "b", "a"}, "pack.", "pack")) return false;
                const char* letters[] = {"r", "g", "b", "a"};
                ChannelMask written = 0;
                for (int c = 0; c < 4; c++) {
                    if (!opJsonHas(pack, letters[c])) continue;
                    const Json& source = pack[letters[c]];
                    written = (ChannelMask)(written | (1u << c));
                    if (source.is_number()) {
                        op.packSource[(size_t)c] = -1;
                        op.packConstant[(size_t)c] = source.get<float>();
                    } else {
                        const int s = opJsonChannelIndex(source);
                        if (s < 0) return ctx.fail(std::string("pack.") + letters[c], "must be \"r\", \"g\", \"b\", \"a\" or a number.");
                        op.packSource[(size_t)c] = (int8_t)s;
                    }
                }
                defaultChannels = written;
            }
            break;
        }
        case OpType::ReplaceColor: {
            if (!opJsonHas(item, "from")) return ctx.fail("from", "is required: the color to replace.");
            int components = 0;
            if (!opJsonValueField(ctx, item["from"], "from", op.target.color.value, components)) return false;
            op.target.color.ignoreAlpha = components != 4;
            if (!readValue("to", op.value, true)) return false;
            defaultChannels = valueComponents == 4 ? CHANNELS_RGBA : CHANNELS_RGB;
            if (!opJsonNumber(ctx, item, "tolerance", op.target.color.tolerance, 0.0f, 1.0f, "0.02 matches about 5 steps of 255")) return false;
            if (!opJsonBool(ctx, item, "ignore_h", op.target.color.ignoreH) || !opJsonBool(ctx, item, "ignore_s", op.target.color.ignoreS) ||
                !opJsonBool(ctx, item, "ignore_v", op.target.color.ignoreV) || !opJsonBool(ctx, item, "ignore_alpha", op.target.color.ignoreAlpha))
                return false;
            break;
        }
        case OpType::FromTexture: {
            const bool texture = opJsonHas(item, "texture");
            const bool slot = opJsonHas(item, "material_slot");
            if (texture && slot) return ctx.fail("material_slot", "and texture are two sources; give one.");
            if (!texture && !slot) return ctx.fail("texture", "is required: a texture path, or material_slot such as \"albedo\".");
            const char* field = texture ? "texture" : "material_slot";
            if (!item[field].is_string()) return ctx.fail(field, texture ? "must be a texture path." : "must be a slot name such as \"albedo\", \"emissive\" or \"occlusion\".");
            if (!hooks.loadImage) return ctx.fail(field, "textures are read by the editor; this caller cannot load them.");
            std::string where = field;
            std::string message;
            op.image = hooks.loadImage(item, where, message);
            if (!op.image) return ctx.fail(where.empty() ? field : where, message.empty() ? "could not be read." : message);
            if (!opJsonInt(ctx, item, "uv_set", op.uvSet, 0, 1)) return false;
            if (!opJsonInt(ctx, item, "samples", op.samples, 1, 256)) return false;
            if (!opJsonBool(ctx, item, "keep_seams", op.keepSeams)) return false;
            if (!opJsonPositive(ctx, item, "seam_delta_e", op.seamDeltaE, "2 is about a just visible difference")) return false;
            break;
        }
        case OpType::BakeAO:
        case OpType::BakeThickness: {
            defaultChannels = CHANNELS_RGB;
            if (!opJsonInt(ctx, item, "rays", op.rays, 1, 1024)) return false;
            if (!opJsonPositive(ctx, item, "distance", op.distance, "the longest ray in units")) return false;
            if (opJsonHas(item, "scope")) {
                int scope = 0;
                if (!opJsonEnum(ctx, item["scope"], "scope", {"self", "scene"}, scope)) return false;
                op.sceneScope = scope == 1;
            }
            if (!opJsonSeed(ctx, item, op.seed)) return false;
            break;
        }
        case OpType::BakeCurvature:
            defaultChannels = CHANNELS_RGB;
            if (!opJsonInt(ctx, item, "radius_edges", op.iterations, 0, 100)) return false;
            // the curvature bake reads its contrast from Op::contrast (adjust's brightness/contrast field)
            if (!opJsonNumber(ctx, item, "contrast", op.contrast, 0.0f, 100.0f, "1 keeps the curvature as measured")) return false;
            break;
        case OpType::ByDirection:
            if (!readValue("value", op.value, false)) return false;
            defaultChannels = valueComponents == 4 ? CHANNELS_RGBA : CHANNELS_RGB;
            if (opJsonHas(item, "direction")) {
                if (!opJsonVector(ctx, item["direction"], "direction", op.direction)) return false;
                if (detail::length3(op.direction) <= 0.0f) return ctx.fail("direction", "must not be zero; [0, 1, 0] is top-down.");
            }
            if (opJsonHas(item, "range")) {
                if (!opJsonPair(ctx, item["range"], "range", op.rangeMin, op.rangeMax)) return false;
                if (op.rangeMin < -1.0f || op.rangeMax > 1.0f) return ctx.fail("range", "must lie in -1..1: the cosine between the normal and the direction.");
            }
            break;
        case OpType::Transfer: {
            if (!opJsonHas(item, "source_entity")) return ctx.fail("source_entity", "is required: the entity to take colors from.");
            if (!hooks.transferSource) return ctx.fail("source_entity", "transfer reads another entity's colors; only the editor can.");
            std::string message;
            op.source = hooks.transferSource(item["source_entity"], message);
            if (!op.source) return ctx.fail("source_entity", message.empty() ? "has no vertex colors to transfer." : message);
            if (!opJsonPositive(ctx, item, "max_distance", op.maxDistance, "the farthest match in units")) return false;
            if (!opJsonNumber(ctx, item, "normal_angle", op.normalAngle, 0.0f, 180.0f, "degrees between the normals")) return false;
            if (opJsonHas(item, "unmatched") && !opJsonEnum(ctx, item["unmatched"], "unmatched", {"keep", "nearest"}, op.unmatched)) return false;
            break;
        }
        case OpType::Clear:
            if (opJsonHas(item, "to") && !opJsonEnum(ctx, item["to"], "to", {"initial", "asset", "white", "black"}, op.clearTo)) return false;
            break;
    }

    if (opJsonHas(item, "channels")) {
        std::string message;
        if (!parseChannels(item["channels"], op.channels, message)) return ctx.fail("channels", message);
    } else if (!(op.type == OpType::Channel && op.channelKind == ChannelOpKind::Invert)) {
        op.channels = defaultChannels;
    }
    return true;
}

namespace {

    // six decimals: enough for RGBA8 steps and RGBA16F data, without float noise in the text
    double opJsonRound(float v) {
        if (!std::isfinite(v)) return 0.0;
        return std::round((double)v * 1e6) / 1e6;
    }

    Json opJsonArray4(const std::array<float, 4>& values) {
        Json out = Json::array();
        for (float v : values) out.push_back(opJsonRound(v));
        return out;
    }

}

Json toJson(const ChannelStats& stats) {
    Json out = Json::object();
    out["count"] = stats.count;
    out["min"] = opJsonArray4(stats.min);
    out["mean"] = opJsonArray4(stats.mean);
    out["max"] = opJsonArray4(stats.max);
    out["p05"] = opJsonArray4(stats.p05);
    out["p50"] = opJsonArray4(stats.p50);
    out["p95"] = opJsonArray4(stats.p95);
    Json histogram = Json::array();
    for (const auto& channel : stats.histogram) {
        Json bins = Json::array();
        for (uint32_t bin : channel) bins.push_back(bin);
        histogram.push_back(bins);
    }
    out["histogram"] = histogram;
    return out;
}

Json toJson(const OpsResult& result) {
    Json out = Json::object();
    out["ok"] = result.ok;
    if (!result.ok) {
        out["error"] = result.error;
        out["failed_op"] = result.failedOp;
    }
    if (result.cancelled) out["cancelled"] = true;
    Json ops = Json::array();
    for (const OpResult& op : result.ops) {
        Json item = Json::object();
        item["changed_vertices"] = op.changedVertices;
        item["changed_corners"] = op.changedCorners;
        item["split_added"] = op.splitAdded;
        item["filtered_out"] = op.filteredOut;
        item["in_target"] = op.inTarget;
        item["mean_delta_e"] = opJsonRound(op.meanDeltaE);
        item["p95_delta_e"] = opJsonRound(op.p95DeltaE);
        item["under_sampled_faces"] = op.underSampledFaces;
        item["unmatched"] = op.unmatched;
        item["mean_distance"] = opJsonRound(op.meanDistance);
        ops.push_back(item);
    }
    out["ops"] = ops;
    out["stats"] = Json{{"before", toJson(result.before)}, {"after", toJson(result.after)}};
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)result.hash);
    out["hash"] = hash;
    Json warnings = Json::array();
    for (const Warning& warning : result.warnings) warnings.push_back(Json{{"code", warning.code}, {"message", warning.message}});
    out["warnings"] = warnings;
    return out;
}

}
