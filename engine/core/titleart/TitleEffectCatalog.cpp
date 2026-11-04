// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleEffectCatalog.h"
#include "TitleArtIO.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::string shortNumber(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", v);
        return text;
    }

    bool isSnakeCase(const char* text) {
        if (!text || !*text || !(*text >= 'a' && *text <= 'z')) return false;
        for (const char* c = text; *c; c++) {
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
        }
        return true;
    }

    bool empty(const char* text) {
        return !text || !*text;
    }

    bool isStepped(ParamKind kind) {
        return kind == ParamKind::Enum || kind == ParamKind::Bool || kind == ParamKind::Integer || kind == ParamKind::Asset ||
               kind == ParamKind::Layer || kind == ParamKind::String;
    }

    void checkParams(const std::string& owner, const std::vector<ParamDef>& params, std::vector<std::string>& problems) {
        std::set<std::string> keys;
        for (const ParamDef& p : params) {
            const std::string at = owner + "." + (p.key ? p.key : "");
            if (!isSnakeCase(p.key)) problems.push_back(at + ": the key is not snake_case");
            if (!keys.insert(p.key ? p.key : "").second) problems.push_back(at + ": the key is used twice");
            if (empty(p.label)) problems.push_back(at + ": no label");
            if (empty(p.description)) problems.push_back(at + ": no description");
            if ((p.kind == ParamKind::Number || p.kind == ParamKind::Integer) && !(p.max > p.min)) {
                problems.push_back(at + ": a number needs a range (min < max)");
            }
            if (p.kind == ParamKind::Enum) {
                std::set<std::string> values;
                if (p.options.empty()) problems.push_back(at + ": an enum without options");
                for (const EnumOption& o : p.options) {
                    if (!isSnakeCase(o.value)) problems.push_back(at + ": option '" + (o.value ? o.value : "") + "' is not snake_case");
                    if (empty(o.label)) problems.push_back(at + ": option '" + (o.value ? o.value : "") + "' has no label");
                    if (!values.insert(o.value ? o.value : "").second) problems.push_back(at + ": option '" + o.value + "' is listed twice");
                }
            }
            // the default is one the param's own check takes as it is
            Json value = p.defaultValue;
            std::vector<TitleWarning> warnings;
            NormalizeContext context;
            context.warnings = &warnings;
            if (!normalizeParamValue(p, value, at, context)) {
                problems.push_back(at + ": the default " + p.defaultValue.dump() + " is not a valid " + paramKindName(p.kind));
            } else if (!warnings.empty()) {
                problems.push_back(at + ": the default " + p.defaultValue.dump() + " is outside its range (" + warnings.front().message + ")");
            } else if ((p.kind == ParamKind::Number || p.kind == ParamKind::Integer || p.kind == ParamKind::Enum || p.kind == ParamKind::Bool) &&
                       value != p.defaultValue) {
                problems.push_back(at + ": the default changes when it is checked");
            }
            if (isStepped(p.kind) && (p.anim == FxAnim::Rerender || p.anim == FxAnim::Live) && p.interp != ParamInterp::Stepped) {
                problems.push_back(at + ": a stepped kind must interpolate Stepped");
            }
            if (p.kind == ParamKind::RowArray) checkParams(at, p.rows, problems);
        }
        for (const ParamDef& p : params) {
            if (!p.when.active()) continue;
            const std::string at = owner + "." + p.key;
            const ParamDef* other = findParam(params, p.when.param);
            if (!other || other == &p) {
                problems.push_back(at + ": `when` names no other param ('" + std::string(p.when.param) + "')");
                continue;
            }
            if (p.when.values.empty()) problems.push_back(at + ": `when` lists no value");
            for (const char* v : p.when.values) {
                bool known = false;
                if (other->kind == ParamKind::Enum) {
                    known = std::any_of(other->options.begin(), other->options.end(), [&](const EnumOption& o) { return std::string_view(o.value) == v; });
                } else if (other->kind == ParamKind::Bool) {
                    known = std::string_view(v) == "true" || std::string_view(v) == "false";
                }
                if (!known) problems.push_back(at + ": `when` value '" + v + "' is not one '" + other->key + "' takes");
            }
        }
    }

    template<class Def>
    void checkCommon(const Def& def, std::vector<std::string>& problems) {
        const std::string owner = def.type ? def.type : "";
        if (!isSnakeCase(def.type)) problems.push_back(owner + ": the type is not snake_case");
        if (empty(def.label)) problems.push_back(owner + ": no label");
        if (empty(def.category)) problems.push_back(owner + ": no category");
        if (empty(def.description)) problems.push_back(owner + ": no description");
        if (def.aliases.empty()) problems.push_back(owner + ": no aliases (the Add menus and list_title_library search them)");
        checkParams(owner, def.params, problems);
        for (const GizmoHint& g : def.gizmos) {
            if (!g.param || !findParam(def.params, g.param)) problems.push_back(owner + ": a gizmo names no param");
            if (g.param2 && !findParam(def.params, g.param2)) problems.push_back(owner + ": a gizmo's second param does not exist");
        }
        if (def.globalLight.angleKey) {
            if (!findParam(def.params, def.globalLight.angleKey)) problems.push_back(owner + ": the global light angle param does not exist");
            if (!findParam(def.params, "use_global_light")) problems.push_back(owner + ": no use_global_light param");
        }
        if (def.passVersion < 1) problems.push_back(owner + ": passVersion starts at 1");
    }

    const char* unitSuffix(ParamUnit unit) {
        switch (unit) {
            case ParamUnit::Px: return "px";
            case ParamUnit::Percent: return "%";
            case ParamUnit::Deg: return "deg";
            case ParamUnit::Factor: return "x";
            case ParamUnit::None: return "";
        }
        return "";
    }

    std::string defaultText(const ParamDef& p) {
        switch (p.kind) {
            case ParamKind::Number:
            case ParamKind::Integer:
                return p.defaultValue.is_number() ? shortNumber(p.defaultValue.get<double>()) : p.defaultValue.dump();
            case ParamKind::Enum:
            case ParamKind::Color:
            case ParamKind::String:
            case ParamKind::Layer:
                return p.defaultValue.is_string() ? p.defaultValue.get<std::string>() : p.defaultValue.dump();
            case ParamKind::Bool:
                return p.defaultValue.is_boolean() && p.defaultValue.get<bool>() ? "true" : "false";
            case ParamKind::Asset:
                return "none";
            case ParamKind::Gradient: {
                const TitleGradient g = decodeGradient(p.defaultValue);
                std::string text;
                for (const TitleGradientStop& s : g.stops) {
                    if (!text.empty()) text += " ";
                    text += formatTitleColor(s.color) + "@" + shortNumber(s.pos);
                }
                return text;
            }
            case ParamKind::Adjustment:
                return p.defaultValue.dump();
            default:
                return paramKindName(p.kind);
        }
    }

    // values at defaults, for the rows (reach, adds_light and fills_canvas are functions of params)
    template<class Def>
    Json defaultsOf(const Def& def) {
        return defaultParams(def.params);
    }

    Json aliasesOf(const std::vector<const char*>& aliases) {
        Json list = Json::array();
        for (const char* a : aliases) list.push_back(a);
        return list;
    }

    std::string mdCell(std::string text) {
        size_t at = 0;
        while ((at = text.find('|', at)) != std::string::npos) {
            text.replace(at, 1, "\\|");
            at += 2;
        }
        return text;
    }

    std::string whenText(const ParamDef& p) {
        if (!p.when.active()) return "";
        std::string text = std::string("`") + p.when.param + "` is ";
        for (size_t i = 0; i < p.when.values.size(); i++) text += (i ? " or " : "") + std::string("`") + p.when.values[i] + "`";
        return text;
    }

    void paramTable(std::string& out, const std::vector<ParamDef>& params) {
        out += "| Param | Type | Default | Range | Unit | Animation | Shown when | Description |\n";
        out += "|---|---|---|---|---|---|---|---|\n";
        for (const ParamDef& p : params) {
            std::string range;
            if ((p.kind == ParamKind::Number || p.kind == ParamKind::Integer) && p.max > p.min) range = shortNumber(p.min) + " .. " + shortNumber(p.max);
            if (p.kind == ParamKind::Enum) {
                for (size_t i = 0; i < p.options.size(); i++) range += (i ? ", " : "") + std::string("`") + p.options[i].value + "`";
            }
            out += "| `" + std::string(p.key) + "` " + p.label + " | " + paramKindName(p.kind) + " | " + mdCell(defaultText(p)) + " | " + mdCell(range) +
                   " | " + unitSuffix(p.unit) + " | " + fxAnimName(p.anim) + (p.light ? ", light" : "") + " | " + whenText(p) + " | " +
                   mdCell(p.description) + " |\n";
        }
    }

    std::vector<std::string> categoriesOf(const std::vector<const char*>& categories) {
        std::vector<std::string> order;
        for (const char* c : categories) {
            if (std::find(order.begin(), order.end(), c) == order.end()) order.push_back(c);
        }
        return order;
    }

}

std::vector<std::string> titleart::checkTitleEffectDef(const EffectDef& def) {
    std::vector<std::string> problems;
    checkCommon(def, problems);
    if (!def.apply) problems.push_back(std::string(def.type ? def.type : "") + ": no apply (pass code)");
    return problems;
}

std::vector<std::string> titleart::checkTitleGeneratorDef(const GeneratorDef& def) {
    std::vector<std::string> problems;
    checkCommon(def, problems);
    if (!def.render) problems.push_back(std::string(def.type ? def.type : "") + ": no render (pass code)");
    if (def.follow != FollowMode::None && !def.followReach) problems.push_back(std::string(def.type) + ": a following generator needs followReach");
    return problems;
}

std::vector<std::string> titleart::checkTitleEffectRegistry(const TitleEffectRegistry& registry) {
    std::vector<std::string> problems;
    std::set<std::string> ids, effectLabels, generatorLabels;
    for (const EffectDef* d : registry.effects()) {
        for (std::string& p : checkTitleEffectDef(*d)) problems.push_back(std::move(p));
        if (!ids.insert(d->type).second) problems.push_back(std::string(d->type) + ": the id is used twice");
        if (!effectLabels.insert(d->label).second) problems.push_back(std::string(d->type) + ": the name '" + d->label + "' is used twice");
    }
    for (const GeneratorDef* d : registry.generators()) {
        for (std::string& p : checkTitleGeneratorDef(*d)) problems.push_back(std::move(p));
        if (!ids.insert(d->type).second) problems.push_back(std::string(d->type) + ": the id is used twice");
        if (!generatorLabels.insert(d->label).second) problems.push_back(std::string(d->type) + ": the name '" + d->label + "' is used twice");
    }
    return problems;
}

Json titleart::titleParamSchema(const ParamDef& p) {
    Json s = Json::object();
    switch (p.kind) {
        case ParamKind::Number:
            s["type"] = "number";
            if (p.max > p.min) {
                s["minimum"] = p.min;
                s["maximum"] = p.max;
            }
            break;
        case ParamKind::Integer:
            s["type"] = "integer";
            if (p.max > p.min) {
                s["minimum"] = (int64_t)p.min;
                s["maximum"] = (int64_t)p.max;
            }
            break;
        case ParamKind::Bool:
            s["type"] = "boolean";
            break;
        case ParamKind::Enum: {
            s["type"] = "string";
            Json values = Json::array();
            for (const EnumOption& o : p.options) values.push_back(o.value);
            s["enum"] = values;
            break;
        }
        case ParamKind::Color:
            // "#rrggbb", "#rgb" or [r, g, b] in 0..1 (stored "#rrggbb")
            s["type"] = Json::array({"string", "array"});
            break;
        case ParamKind::Vec2:
            s["type"] = "array";
            s["items"] = Json{{"type", "number"}};
            s["minItems"] = 2;
            s["maxItems"] = 2;
            break;
        case ParamKind::Asset:
            s["type"] = Json::array({"string", "object", "null"});
            break;
        case ParamKind::Layer:
        case ParamKind::String:
            s["type"] = "string";
            break;
        case ParamKind::Gradient:
        case ParamKind::Curve:
        case ParamKind::Adjustment:
        case ParamKind::Material:
            // checked member by member by its schema kind (TitleArtSchema)
            s["type"] = "object";
            break;
        case ParamKind::RowArray:
            s["type"] = "array";
            s["items"] = titleParamsSchema(p.rows);
            if (p.maxRows) s["maxItems"] = p.maxRows;
            break;
    }
    s["default"] = p.defaultValue;
    s["description"] = p.description;
    return s;
}

Json titleart::titleParamsSchema(const std::vector<ParamDef>& params) {
    Json properties = Json::object();
    for (const ParamDef& p : params) properties[p.key] = titleParamSchema(p);
    return Json{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
}

std::string titleart::titleParamLine(const ParamDef& p) {
    std::string line = p.key;
    const char* unit = unitSuffix(p.unit);
    if (*unit) line += std::string(" ") + unit;
    line += " " + defaultText(p);
    if ((p.kind == ParamKind::Number || p.kind == ParamKind::Integer) && p.max > p.min) line += " [" + shortNumber(p.min) + ".." + shortNumber(p.max) + "]";
    if (p.kind == ParamKind::Enum) {
        line += " (";
        for (size_t i = 0; i < p.options.size(); i++) line += (i ? "|" : "") + std::string(p.options[i].value);
        line += ")";
    }
    if (p.kind != ParamKind::Number && p.kind != ParamKind::Integer && p.kind != ParamKind::Enum) line += std::string(" ") + paramKindName(p.kind);
    line += std::string(": ") + p.description;
    return line;
}

Json titleart::titleEffectRow(const EffectDef& def) {
    const Json values = defaultsOf(def);
    const ParamValues p(values);
    return Json{{"id", def.type},
                {"name", def.label},
                {"category", def.category},
                {"cost", costClassName(def.costClass)},
                {"adds_light", def.addsLight ? def.addsLight(p) : false},
                {"on_parts", def.onParts},
                {"outline", def.outline},
                {"fallback", fallbackClassName(def.fallback)},
                {"glyph_safety", glyphSafetyName(def.glyphSafety)},
                {"live", liveKindName(def.live.kind)},
                {"aliases", aliasesOf(def.aliases)}};
}

Json titleart::titleGeneratorRow(const GeneratorDef& def) {
    const Json values = defaultsOf(def);
    const ParamValues p(values);
    const bool fills = !def.bounds || def.bounds(p).empty();
    return Json{{"id", def.type},
                {"name", def.label},
                {"category", def.category},
                {"cost", costClassName(def.costClass)},
                {"follow", followModeName(def.follow)},
                {"fills_canvas", fills},
                {"fallback", fallbackClassName(def.fallback)},
                {"glyph_safety", glyphSafetyName(def.glyphSafety)},
                {"live", liveKindName(def.live.kind)},
                {"aliases", aliasesOf(def.aliases)}};
}

Json titleart::titleEffectDetail(const EffectDef& def) {
    Json detail = titleEffectRow(def);
    const Json values = defaultsOf(def);
    const ParamValues p(values);
    detail["description"] = def.description;
    detail["reach"] = def.reach ? def.reach(p) : 0.0f;
    detail["gather"] = def.gather ? def.gather(p) : -1.0f;
    Json lines = Json::array();
    for (const ParamDef& param : def.params) lines.push_back(titleParamLine(param));
    detail["params"] = lines;
    detail["schema"] = titleParamsSchema(def.params);
    detail["add"] = Json{{"op", "add_effect"}, {"layer", "<layer>"}, {"type", def.type}};
    return detail;
}

Json titleart::titleGeneratorDetail(const GeneratorDef& def) {
    Json detail = titleGeneratorRow(def);
    detail["description"] = def.description;
    Json lines = Json::array();
    for (const ParamDef& param : def.params) lines.push_back(titleParamLine(param));
    detail["params"] = lines;
    detail["schema"] = titleParamsSchema(def.params);
    detail["add"] = Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", def.type}};
    return detail;
}

std::vector<std::string> titleart::titleEffectFieldPaths(const EffectDef& def) {
    std::vector<std::string> paths;
    for (const ParamDef& p : def.params) paths.push_back(std::string("fx:") + def.type + "." + p.key);
    return paths;
}

std::vector<std::string> titleart::titleGeneratorFieldPaths(const GeneratorDef& def) {
    std::vector<std::string> paths;
    for (const ParamDef& p : def.params) paths.push_back(std::string("gen.") + p.key);
    return paths;
}

std::string titleart::titleEffectDocsMarkdown(const TitleEffectRegistry& registry) {
    std::string out;
    out += "# Title Art effects and generators\n\n";
    out += "Generated from the effect and generator registry (`engine/core/titleart/TitleEffectRegistry.h`) by\n";
    out += "`titleEffectDocsMarkdown` (`engine/core/titleart/TitleEffectCatalog.cpp`); `TitleEffectCatalogTest.DocsPageIsUpToDate`\n";
    out += "fails when it is stale (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites it). Do not edit by hand.\n\n";
    out += "Units: `px` are canvas pixels multiplied by the render's px_scale; `deg` are clockwise on screen from +x,\n";
    out += "except light angles (Photoshop's, counter-clockwise: where the light comes from). An effect is added with\n";
    out += "`edit_title {ops: [{op: \"add_effect\", layer, type, params?}]}` and its params are the field paths\n";
    out += "`fx:<type>.<param>` (`fx:outer_glow.size`), the names exposed parameters and Lua use. A generator is a layer:\n";
    out += "`{op: \"add_layer\", type: \"generator\", generator: \"<type>\"}`, its params `gen.<param>`.\n\n";
    out += "Classes: cost C0 (trivial) to C4 (bake only); 8-bit F0 to F2 (how closely the RGBA8 fallback agrees);\n";
    out += "glyph atlas safe, approximate or unsafe (S81's per-glyph rendering); animation `rerender` (smooth) or `step`;\n";
    out += "`light` marks a colour of light (colour schemes map it onto their light ramp).\n\n";

    std::vector<const char*> effectCategories, generatorCategories;
    for (const EffectDef* d : registry.effects()) effectCategories.push_back(d->category);
    for (const GeneratorDef* d : registry.generators()) generatorCategories.push_back(d->category);

    out += "## Effects\n\n";
    for (const std::string& category : categoriesOf(effectCategories)) {
        out += "### " + category + "\n\n";
        for (const EffectDef* d : registry.effects()) {
            if (category != d->category) continue;
            const Json row = titleEffectRow(*d);
            const Json values = defaultsOf(*d);
            const ParamValues p(values);
            out += "#### " + std::string(d->label) + " (`" + d->type + "`)\n\n";
            out += std::string(d->description) + ".\n\n";
            out += "Cost " + row["cost"].get<std::string>() + " · 8-bit " + row["fallback"].get<std::string>() + " · glyph atlas " +
                   row["glyph_safety"].get<std::string>() + " · live " + row["live"].get<std::string>() + " · reach at defaults " +
                   shortNumber(d->reach ? d->reach(p) : 0.0) + " px · adds light at defaults " + (row["adds_light"].get<bool>() ? "yes" : "no") +
                   (d->outline ? " · traces ink" : "") + (d->onParts ? "" : " · whole layers only") +
                   (d->globalLight.angleKey ? std::string(" · follows the global light (`") + d->globalLight.angleKey + "`)" : std::string()) +
                   "\n\n";
            std::string aliases;
            for (const char* a : d->aliases) aliases += (aliases.empty() ? "" : ", ") + std::string(a);
            out += "Also found as: " + aliases + ".\n\n";
            paramTable(out, d->params);
            out += "\n";
        }
    }
    out += "## Generators\n\n";
    for (const std::string& category : categoriesOf(generatorCategories)) {
        out += "### " + category + "\n\n";
        for (const GeneratorDef* d : registry.generators()) {
            if (category != d->category) continue;
            const Json row = titleGeneratorRow(*d);
            out += "#### " + std::string(d->label) + " (`" + d->type + "`)\n\n";
            out += std::string(d->description) + ".\n\n";
            out += "Cost " + row["cost"].get<std::string>() + " · 8-bit " + row["fallback"].get<std::string>() + " · glyph atlas " +
                   row["glyph_safety"].get<std::string>() + " · live " + row["live"].get<std::string>() + " · follow " +
                   row["follow"].get<std::string>() + (row["fills_canvas"].get<bool>() ? " · fills the canvas" : "") + "\n\n";
            std::string aliases;
            for (const char* a : d->aliases) aliases += (aliases.empty() ? "" : ", ") + std::string(a);
            out += "Also found as: " + aliases + ".\n\n";
            paramTable(out, d->params);
            out += "\n";
        }
    }
    while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    return out;
}

std::vector<std::string> titleart::titleGoldenCases(const TitleEffectRegistry& registry) {
    std::vector<std::string> cases;
    for (const EffectDef* d : registry.effects()) cases.push_back(std::string("effect/") + d->type);
    for (const GeneratorDef* d : registry.generators()) cases.push_back(std::string("generator/") + d->type);
    return cases;
}
