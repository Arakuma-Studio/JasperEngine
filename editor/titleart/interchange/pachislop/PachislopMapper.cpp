// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Maps Pachislop's project model (src/model/types.ts by SafEight, used with the author's
// permission) onto the `.titleart` document.

#include "PachislopMapper.h"

#include "PachislopMembers.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/fx/PachislopFxMap.h"
#include "titleart/fx/PachislopGeneratorMap.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "titleart/three_d/Title3DConvert.h"
#endif

#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

namespace {

    constexpr bool HAS_3D =
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
        true;
#else
        false;
#endif

    struct Mapper {
        const Json& project;
        const MapperAssets& assets;
        std::vector<TitleImportNote> notes;
        std::map<std::string, std::string> ids;     // Pachislop layer id -> new
        std::set<std::string> taken;
        MemberContext members;
        std::vector<MemberNote> memberNotes;
        std::string currentPath;

        Mapper(const Json& p, const MapperAssets& a) : project(p), assets(a) {
            members.has3D = HAS_3D;
            members.notes = &memberNotes;
            members.layer = [this](const std::string& id) {
                auto it = ids.find(id);
                return it == ids.end() ? std::string() : it->second;
            };
            members.asset = [this](const std::string& assetId, const std::string& field) {
                return assets.asset ? assets.asset(assetId, currentPath, field) : Json();
            };
            members.environment = [this](const std::string& mapName, const std::string& field) {
                return assets.environment ? assets.environment(mapName, currentPath, field) : Json();
            };
        }

        void note(const std::string& field, const std::string& what, const std::string& action = "dropped", const std::string& code = {}) {
            TitleImportNote n;
            n.layer = currentPath;
            n.field = field;
            n.what = what;
            n.action = action;
            n.code = code;
            notes.push_back(std::move(n));
        }

        void flushMemberNotes() {
            for (const MemberNote& m : memberNotes) note(m.field, m.what, m.action);
            memberNotes.clear();
        }

        // a new 8-hex id from a Pachislop one, unique in the document
        std::string mint(const std::string& old) {
            uint64_t h = titleart::fnv1a64(old);
            for (int n = 0;; n++) {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%08x", (unsigned)((h >> 16) ^ h) & 0xffffffffu);
                if (taken.insert(buffer).second) return buffer;
                h = titleart::fnv1a64(old + "#" + std::to_string(n + 1));
            }
        }

        // ---- effects ----

        Json effect(const Json& fx, const std::string& field) {
            const std::string type = str(fx, "type");
            Json params = fx.value("params", Json::object());
            if (type == "material" && params.contains("material") && params["material"].is_object()) {
                params["material"] = convertMembers(params["material"], "material", members, field + ".params.material");
            }
            if (type == "adjust" && params.contains("adjustment") && params["adjustment"].is_object()) {
                params["adjustment"] = adjustment(params["adjustment"], field + ".params.adjustment");
            }
            Json out = Json{{"id", mint(str(fx, "id", type) + "@" + field)}, {"enabled", fx.value("enabled", Json(true))}};
            PachislopFxConversion conv = convertPachislopEffect(type, params);
            // a key the map does not know is dropped (with a note) and the rest converted
            for (int guard = 0; !conv.ok && conv.error.find("is not a param") != std::string::npos && guard < 64; guard++) {
                const size_t a = conv.error.find('\''), b = conv.error.find('\'', a + 1);
                const std::string key = conv.error.substr(a + 1, b - a - 1);
                note(field + ".params." + key, "parameter '" + key + "' of " + type + " has no Title Art equivalent");
                params.erase(key);
                conv = convertPachislopEffect(type, params);
            }
            if (!conv.ok) {
                note(field, "effect type '" + type + "' has no Title Art effect: kept as it is (drawn as nothing)", "kept_unknown", "unknown_type");
                out["type"] = type;
                out["params"] = params;
            } else {
                out["type"] = conv.type;
                out["params"] = conv.params;
            }
            if (fx.value("fromPreset", false)) out["from_look"] = true;
            return out;
        }

        Json effects(const Json& list, const std::string& field) {
            Json out = Json::array();
            if (!list.is_array()) return out;
            for (size_t i = 0; i < list.size(); i++) {
                if (list[i].is_object()) out.push_back(effect(list[i], field + "[" + std::to_string(i) + "]"));
            }
            return out;
        }

        Json adjustment(const Json& adj, const std::string& field) {
            const std::string type = snakeCase(str(adj, "type", "hueSaturation"));
            Json body = adj;
            body.erase("type");
            Json out = convertMembers(body, "adjustment." + type, members, field);
            out["type"] = type;
            return out;
        }

        // ---- masks ----

        Json mask(const Json& m, const std::string& field) {
            const std::string kind = str(m, "kind", "shape");
            Json out = Json{{"id", mint(str(m, "id", "mask") + "@" + field)}, {"type", kind}, {"enabled", m.value("enabled", Json(true))},
                            {"invert", m.value("invert", Json(false))}, {"feather", m.value("feather", Json(0))},
                            {"density", m.value("density", Json(1))}, {"linked", m.value("linked", Json(true))}};
            if (kind == "shape") {
                Json source = m.value("source", Json::object());
                source.erase("kind");
                out["shape"] = convertMembers(source, "shape_geometry", members, field + ".source");
                out["position"] = Json::array({num(m, "x", 0), num(m, "y", 0)});
                out["rotation"] = m.value("rotation", Json(0));
            } else if (kind == "gradient") {
                out["shape"] = m.value("shape", Json("linear"));
                out["position"] = Json::array({num(m, "x", 0), num(m, "y", 0)});
                for (const char* k : {"angle", "length", "inner", "mid"}) if (m.contains(k)) out[k] = m[k];
            } else if (kind == "layer") {
                out["layer"] = members.layer(str(m, "layerId"));
                out["channel"] = m.value("channel", Json("alpha"));
                if (str(out, "layer").empty()) note(field, "the mask's layer is not in the project", "dropped", "mask_layer_missing");
            } else {
                note(field, "mask kind '" + kind + "' has no Title Art mask");
                return Json();
            }
            return out;
        }

        // ---- layers ----

        Json transform(const Json& t, bool threeD) {
            Json out = Json{{"position", Json::array({num(t, "x", 0), num(t, "y", 0)})}, {"rotation", num(t, "rotZ", 0)}, {"scale", num(t, "scale", 1)},
                            {"flip_x", boolean(t, "flipX", false)}, {"flip_y", boolean(t, "flipY", false)}};
            const double z = num(t, "z", 0), rx = num(t, "rotX", 0), ry = num(t, "rotY", 0);
            if (HAS_3D && threeD) {
                out["z"] = z;
                out["rotation_x"] = rx;
                out["rotation_y"] = ry;
            } else if (z != 0 || rx != 0 || ry != 0) {
                note("transform", "z / rotX / rotY of a 2D layer (only 3D layers turn in depth)");
            }
            return out;
        }

        void base(const Json& l, Json& out, const std::string& type, bool threeD) {
            out["id"] = ids[str(l, "id")];
            out["type"] = type;
            out["name"] = str(l, "name", type);
            out["visible"] = l.value("visible", true);
            out["locked"] = l.value("locked", false);
            out["guide"] = l.value("guide", false);
            out["blend"] = l.value("blend", Json("normal"));
            const Json t = l.value("transform", Json::object());
            out["opacity"] = num(t, "opacity", 1);
            out["clip"] = l.value("clip", false);
            out["transform"] = transform(t, threeD);
            if (l.value("solo", false)) note("solo", "solo is a view setting of Pachislop's layer list");
            Json masks = Json::array();
            const Json list = l.value("masks", Json::array());
            for (size_t i = 0; i < list.size(); i++) {
                Json m = mask(list[i], "masks[" + std::to_string(i) + "]");
                if (m.is_object()) masks.push_back(m);
            }
            out["masks"] = masks;
            out["effects"] = effects(l.value("effects", Json::array()), "effects");
        }

        Json partKey(const std::string& key, const std::map<std::string, std::string>& rims, const std::map<std::string, std::string>& bands) {
            if (key == "face" || key == "side") return key;
            if (key.rfind("rim:", 0) == 0) {
                auto it = rims.find(key.substr(4));
                return it == rims.end() ? Json() : Json("rim:" + it->second);
            }
            if (key.rfind("band:", 0) == 0) {
                auto it = bands.find(key.substr(5));
                return it == bands.end() ? Json() : Json("band:" + it->second);
            }
            return Json();
        }

        void title(const Json& l, Json& out) {
            const bool threeD = str(l, "kind") == "title3d";
            const Json source = l.value("source", Json::object());
            const std::string kind = str(source, "kind", "text");
            const std::string type = kind == "text" ? "text" : kind == "shape" ? "shape" : kind == "svg" ? "vector" : "image";
            base(l, out, type, threeD);
            if (kind == "text") {
                out["text"] = source.value("text", Json(""));
                out["font"] = assets.font ? assets.font(str(source, "fontId"), currentPath) : Json();
                for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
                         {"size", "size"}, {"tracking", "tracking"}, {"lineHeight", "line_height"}, {"align", "align"}, {"vertical", "vertical"},
                         {"shear", "shear"}, {"hScale", "h_scale"}, {"vScale", "v_scale"}, {"tsume", "proportional"},
                         {"tateChuYoko", "tate_chu_yoko"}}) {
                    if (source.contains(from)) out[to] = source[from];
                }
                Json chars = Json::object();
                if (source.contains("chars") && source["chars"].is_object()) {
                    for (auto it = source["chars"].begin(); it != source["chars"].end(); ++it) {
                        chars[it.key()] = convertMembers(*it, "char_adjust", members, "source.chars." + it.key());
                    }
                }
                out["chars"] = chars;
                Json lines = Json::object();
                if (source.contains("lines") && source["lines"].is_object()) {
                    for (auto it = source["lines"].begin(); it != source["lines"].end(); ++it) {
                        lines[it.key()] = convertMembers(*it, "line_adjust", members, "source.lines." + it.key());
                    }
                }
                out["lines"] = lines;
            } else if (kind == "shape") {
                Json shape = source;
                shape.erase("kind");
                Json geometry = convertMembers(shape, "shape_geometry", members, "source");
                for (auto it = geometry.begin(); it != geometry.end(); ++it) out[it.key()] = it.value();
            } else if (kind == "svg") {
                out["svg"] = members.asset(str(source, "assetId"), "source.assetId");
                out["size"] = source.value("size", Json(200));
                out["mode"] = "shape";
            } else if (kind == "image") {
                out["image"] = members.asset(str(source, "assetId"), "source.assetId");
                out["size"] = source.value("size", Json(200));
                if (source.contains("trace")) out["trace"] = convertMembers(source["trace"], "trace", members, "source.trace");
            } else {
                note("source", "a " + kind + " source (a 3D model) has no Title Art layer: an empty Image layer stands in", "dropped", "source_unsupported");
            }
            if (type != "image") {
                out["warp"] = convertMembers(l.value("warp", Json::object()), "warp", members, "warp");
                out["distort"] = convertMembers(l.value("distort", Json::object()), "distort", members, "distort");
                out["weight"] = l.value("weight", Json(0));
            }
            out["dimension"] = threeD ? "3d" : "2d";
            std::map<std::string, std::string> rims, bands;
            if (!threeD) {
                Json style = convertMembers(l.value("style", Json::object()), "style2d", members, "style");
                if (style.contains("rims")) {
                    const Json old = l.value("style", Json::object()).value("rims", Json::array());
                    for (size_t i = 0; i < style["rims"].size() && i < old.size(); i++) {
                        const std::string oldId = str(old[i], "id");
                        const std::string id = mint(oldId.empty() ? currentPath + "/rim" + std::to_string(i) : oldId);
                        style["rims"][i]["id"] = id;
                        if (!oldId.empty()) rims[oldId] = id;
                    }
                }
                out["style"] = style;
            } else {
                const Json style3d = l.value("style3d", Json::object());
                if (HAS_3D) {
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
                    Json converted = titleart::title3DFromPachislop(style3d);
                    if (converted.contains("bands") && converted["bands"].is_array()) {
                        const Json old = style3d.value("bands", Json::array());
                        for (size_t i = 0; i < converted["bands"].size() && i < old.size(); i++) {
                            const std::string oldId = str(old[i], "id");
                            const std::string id = mint(oldId.empty() ? currentPath + "/band" + std::to_string(i) : oldId);
                            converted["bands"][i]["id"] = id;
                            if (!oldId.empty()) bands[oldId] = id;
                        }
                    }
                    out["style_3d"] = converted;
#endif
                } else {
                    // R30: drawn 2D with the 3D face's base colour until the 3D module re-maps it
                    const std::string color = str(style3d.value("face", Json::object()), "color", "#ffffff");
                    out["style"] = Json{{"face", Json{{"fill", Json{{"kind", "solid"}, {"color", color}}}}}};
                    out["pachislop_style3d"] = style3d;
                    note("style3d", "3D style kept verbatim (pachislop_style3d) until the 3D module re-maps it (Reimport 3D)", "kept_unknown",
                         "dimension_unavailable");
                }
            }
            Json parts = Json::object();
            if (l.contains("partEffects") && l["partEffects"].is_object()) {
                for (auto it = l["partEffects"].begin(); it != l["partEffects"].end(); ++it) {
                    const Json key = partKey(it.key(), rims, bands);
                    if (!key.is_string()) {
                        note("partEffects." + it.key(), "effects of a part that does not exist in Title Art");
                        continue;
                    }
                    parts[key.get<std::string>()] = effects(*it, "partEffects." + it.key());
                }
            }
            out["part_effects"] = parts;
        }

        void background(const Json& l, Json& out) {
            base(l, out, "generator", false);
            const Json fill = l.value("fill", Json::object());
            const std::string kind = snakeCase(str(fill, "kind", "solid"));
            Json params = Json::object();
            if (kind == "image") {
                note("fill", "an image background has no fill generator: a solid of its colour stands in", "approximated");
                params["kind"] = "solid";
            } else {
                params["kind"] = kind;
            }
            if (fill.contains("color")) params["color"] = fill["color"];
            if (fill.contains("gradient")) {
                Json gradient = fill["gradient"];
                for (Json& s : gradient["stops"]) {
                    if (!s.contains("alpha")) s["alpha"] = 1;
                    if (!s.contains("mid")) s["mid"] = 0.5;
                }
                if (!gradient.contains("interp")) gradient["interp"] = "rgb";
                params["gradient"] = gradient;
            }
            for (const char* k : {"angle", "scale", "offset"}) if (fill.contains(k)) params[k] = fill[k];
            if (str(fill, "span", "layer") != "layer") note("fill.span", "a background gradient spans the canvas", "approximated");
            out["generator"] = Json{{"type", "fill"}, {"params", params}};
        }

        void generator(const Json& l, Json& out) {
            base(l, out, "generator", false);
            std::vector<std::string> conversionNotes;
            const TitlePachislopGenerator g = convertPachislopGeneratorLayer(l, &conversionNotes);
            for (const std::string& n : conversionNotes) note("generator", n, n.find("companion") != std::string::npos ? "dropped" : "approximated");
            const Json pachislopGenerator = l.value("generator", Json::object());
            if (g.type.empty()) {
                note("generator", "generator type '" + str(pachislopGenerator, "type") + "' has no Title Art generator: kept as it is", "kept_unknown",
                     "unknown_type");
                out["generator"] = Json{{"type", str(pachislopGenerator, "type")}, {"params", pachislopGenerator.value("params", Json::object())}};
                return;
            }
            out["generator"] = Json{{"type", g.type}, {"params", g.params}};
            if (!g.follow.empty()) {
                const std::string follow = members.layer(g.follow);
                if (follow.empty()) note("generator.follow", "the followed layer is not in the project", "dropped", "follow_missing");
                out["follow"] = follow;
            }
        }

        Json layer(const Json& l, const std::string& path, const std::vector<Json>& all) {
            currentPath = path;
            Json out = Json::object();
            const std::string kind = str(l, "kind");
            if (kind == "title2d" || kind == "title3d") {
                title(l, out);
            } else if (kind == "background") {
                background(l, out);
            } else if (kind == "group") {
                base(l, out, "group", false);
                out["collapsed"] = l.value("collapsed", false);
            } else if (kind == "adjustment") {
                base(l, out, "adjustment", false);
                out["adjustment"] = adjustment(l.value("adjustment", Json::object()), "adjustment");
            } else if (kind == "generator") {
                generator(l, out);
            } else {
                base(l, out, "group", false);
                note("kind", "layer kind '" + kind + "' has no Title Art layer: an empty group stands in", "dropped", "unknown_type");
            }
            flushMemberNotes();
            if (kind == "group") {
                out["children"] = children(str(l, "id"), path + "/", all);
                currentPath = path;
            }
            return out;
        }

        // the layers whose parent is `parent` ("" = the top level), bottom first, mapped
        Json children(const std::string& parent, const std::string& prefix, const std::vector<Json>& all) {
            Json out = Json::array();
            std::map<std::string, int> names;
            for (const Json& l : all) {
                const std::string pid = l.contains("parentId") && l["parentId"].is_string() ? l["parentId"].get<std::string>() : std::string();
                const bool top = pid.empty() || !ids.count(pid);
                if (parent.empty() ? !top : pid != parent) continue;
                const std::string name = str(l, "name", "Layer");
                const int n = names[name]++;
                out.push_back(layer(l, prefix + name + (n ? "#" + std::to_string(n) : ""), all));
            }
            return out;
        }
    };

}

bool pachislop::mapperHas3D() {
    return HAS_3D;
}

bool pachislop::remapStyle3D(Json& layer) {
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    auto it = layer.find("pachislop_style3d");
    if (it == layer.end() || !it->is_object()) return false;
    layer["style_3d"] = titleart::title3DFromPachislop(*it);
    layer["dimension"] = "3d";
    layer.erase("pachislop_style3d");
    return true;
#else
    (void)layer;
    return false;
#endif
}

MapResult pachislop::mapProject(const Json& project, const MapperAssets& assets) {
    Mapper m(project, assets);
    std::vector<Json> all;
    for (const Json& l : project.value("layers", Json::array())) if (l.is_object()) all.push_back(l);
    for (const Json& l : all) m.ids[str(l, "id")] = m.mint(str(l, "id"));

    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    Json& root = doc.root;
    const Json comp = project.value("comp", Json::object());
    root["name"] = project.value("name", Json(""));
    root["canvas"]["width"] = comp.value("width", Json(1920));
    root["canvas"]["height"] = comp.value("height", Json(1080));
    const Json background = comp.value("background", Json::object());
    root["canvas"]["background"] = Json{{"transparent", background.value("transparent", true)}, {"color", str(background, "color", "#000000")}};
    root["global_light"] = Json{{"angle", num(comp.value("globalLight", Json::object()), "angle", 120)},
                                {"altitude", num(comp.value("globalLight", Json::object()), "altitude", 30)}};
    root["dpi"] = comp.value("dpi", Json(72));
    root["exposed"] = Json::array();
    root["layers"] = m.children("", "", all);
    m.currentPath.clear();
    bool any3D = false;
    for (const Json& l : all) any3D = any3D || str(l, "kind") == "title3d";
    // the comp's 3D scene (camera, environment, lights, tone mapping, exposure, supersampling):
    // Title Art's 3D scene keeps its defaults, which the 3D layers then light with
    if (any3D) {
        m.note("comp.camera", "the comp's camera, environment, lights, tone mapping, exposure and supersampling keep Title Art's default 3D scene",
               "approximated");
    }
    if (project.contains("materialLinks") && project["materialLinks"].is_object()) {
        Json links = Json::object();
        for (auto it = project["materialLinks"].begin(); it != project["materialLinks"].end(); ++it) {
            links[it.key()] = Json{{"name", str(*it, "name")}};
        }
        root["material_links"] = links;
    }
    // S80 validates and normalizes as it does an op batch
    titleart::TitleIdGenerator generator(1);
    generator.reserveFrom(root);
    TitleOpsResult invariants;
    TitleArtOps::applyCommitInvariants(nullptr, doc, generator, invariants);
    for (const TitleOpProblem& p : invariants.problems) {
        TitleImportNote n;
        n.field = p.path;
        n.what = p.message;
        n.action = "approximated";
        m.notes.push_back(std::move(n));
    }
    MapResult result;
    Json lines = Json::array();
    for (const TitleImportNote& n : m.notes) lines.push_back(n.toLine());
    doc.root["import_notes"] = lines;
    result.doc = std::move(doc);
    result.notes = std::move(m.notes);
    return result;
}
