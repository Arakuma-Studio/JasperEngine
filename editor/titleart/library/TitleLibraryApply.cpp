// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The look transfer (px x k, handles relative to the layer, positional part keys, companions as
// following generator layers, group effects) is ported from Pachislop src/presets/apply.ts
// (applyPreset, placeLook, transferEffects, presetFromLayer) by SafEight, used with the author's
// permission.

#include "TitleLibraryApply.h"

#include "TitleLibraryImports.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/RemoveFilesCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "titleart/fx/FxPresets.h"
#include "titleart/ColorSchemeApply.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;
using titleart::TitleArtDoc;

namespace {

    bool dropMasksForTesting = false;

    // a preview in progress (R11): no import, the pack files read in place
    struct PreviewScope {
        std::map<std::string, std::string> transient;
    };
    PreviewScope* currentPreview = nullptr;
    // a batch of library ops alone (no document): project deletes run there
    bool libraryOnlyBatch = false;

    std::string str(const Json& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    bool readBytes(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    // ---- the layer tree ----

    struct Slot {
        Json* list = nullptr;
        size_t index = 0;
        Json* parent = nullptr;     // the group holding it (null at the top level)
    };

    bool findSlotIn(Json& list, Json* parent, const std::string& id, Slot& out) {
        if (!list.is_array()) return false;
        for (size_t i = 0; i < list.size(); i++) {
            if (str(list[i], "id") == id) {
                out = {&list, i, parent};
                return true;
            }
            if (list[i].contains("children") && findSlotIn(list[i]["children"], &list[i], id, out)) return true;
        }
        return false;
    }

    bool findSlot(TitleArtDoc& doc, const std::string& id, Slot& out) {
        return findSlotIn(doc.root["layers"], nullptr, id, out);
    }

    std::string ownerOf(const Json& layer) {
        return layer.contains("look") && layer["look"].is_object() ? str(layer["look"], "owner") : std::string();
    }

    // removes every generator layer whose look.owner is `owner`; the count removed
    int removeOwnedGenerators(Json& list, const std::string& owner) {
        if (!list.is_array()) return 0;
        int removed = 0;
        for (size_t i = list.size(); i-- > 0;) {
            Json& l = list[i];
            if (l.value("type", std::string()) == "generator" && ownerOf(l) == owner) {
                list.erase(list.begin() + (long)i);
                removed++;
                continue;
            }
            if (l.contains("children")) removed += removeOwnedGenerators(l["children"], owner);
        }
        return removed;
    }

    void ownedGeneratorIds(const Json& list, const std::string& owner, std::vector<std::string>& out) {
        if (!list.is_array()) return;
        for (const Json& l : list) {
            if (l.value("type", std::string()) == "generator" && ownerOf(l) == owner) out.push_back(str(l, "id"));
            if (l.contains("children")) ownedGeneratorIds(l["children"], owner, out);
        }
    }

    titleart::TitleVec2 positionOf(const Json& layer) {
        return titleart::decodeTransform(layer.contains("transform") ? layer["transform"] : Json::object()).position;
    }

    // ---- px params and handles (apply.ts transferEffects) ----

    bool isPointParam(const std::vector<titleart::GizmoHint>& gizmos, const std::string& key) {
        for (const titleart::GizmoHint& g : gizmos) {
            if (g.kind == titleart::GizmoKind::Point && g.param && key == g.param) return true;
        }
        return false;
    }

    void scaleParams(const std::vector<titleart::ParamDef>& defs, const std::vector<titleart::GizmoHint>& gizmos, Json& params, double k,
                     const titleart::TitleVec2* origin) {
        for (const titleart::ParamDef& p : defs) {
            auto it = params.find(p.key);
            if (it == params.end()) continue;
            Json& v = *it;
            if (origin && p.kind == titleart::ParamKind::Vec2 && isPointParam(gizmos, p.key) && v.is_array() && v.size() == 2) {
                v = Json::array({origin->x + k * v[0].get<double>(), origin->y + k * v[1].get<double>()});
                continue;
            }
            if (p.kind == titleart::ParamKind::RowArray && v.is_array()) {
                for (Json& row : v) if (row.is_object()) scaleParams(p.rows, {}, row, k, nullptr);
                continue;
            }
            if (p.unit != titleart::ParamUnit::Px) continue;
            if (p.kind == titleart::ParamKind::Number && v.is_number()) v = v.get<double>() * k;
            else if (p.kind == titleart::ParamKind::Integer && v.is_number()) v = (int64_t)std::llround(v.get<double>() * k);
            else if (p.kind == titleart::ParamKind::Vec2 && v.is_array() && v.size() == 2) v = Json::array({v[0].get<double>() * k, v[1].get<double>() * k});
        }
    }

    // a look's handle params made relative to the layer origin (the inverse, for capture)
    void relativeParams(const std::vector<titleart::GizmoHint>& gizmos, const std::vector<titleart::ParamDef>& defs, Json& params,
                        const titleart::TitleVec2& origin) {
        for (const titleart::ParamDef& p : defs) {
            auto it = params.find(p.key);
            if (it == params.end() || p.kind != titleart::ParamKind::Vec2 || !isPointParam(gizmos, p.key)) continue;
            if (it->is_array() && it->size() == 2) *it = Json::array({(*it)[0].get<double>() - origin.x, (*it)[1].get<double>() - origin.y});
        }
    }

    // a look's effect entries as the document's: fresh ids, defaults filled, px x k, handles at the layer
    Json transferEffects(const Json& list, double k, const titleart::TitleVec2& origin, TitleOpContext& ctx, bool fromLook) {
        Json out = Json::array();
        if (!list.is_array()) return out;
        for (const Json& entry : list) {
            if (!entry.is_object()) continue;
            Json e = Json::object();
            e["id"] = ctx.newId();
            e["type"] = str(entry, "type");
            e["enabled"] = entry.value("enabled", true);
            e["params"] = entry.contains("params") && entry["params"].is_object() ? entry["params"] : Json::object();
            if (fromLook) e["from_look"] = true;
            for (auto it = entry.begin(); it != entry.end(); ++it) {
                if (!e.contains(it.key())) e[it.key()] = it.value();
            }
            titleart::NormalizeContext context;
            context.ids = &ctx.ids;
            titleart::normalizeTitleArtObject(e, "effect", "", context);
            if (const titleart::EffectDef* def = titleart::findEffect(str(e, "type"))) scaleParams(def->params, def->gizmos, e["params"], k, &origin);
            out.push_back(std::move(e));
        }
        return out;
    }

    // positional part keys of the style in place: face, bevel, side, rim#n, band#n
    std::string partKeyAt(const Json& layer, const std::string& positional) {
        const size_t hash = positional.find('#');
        if (hash == std::string::npos) return positional;
        const std::string kind = positional.substr(0, hash);
        const size_t n = (size_t)std::strtoul(positional.c_str() + hash + 1, nullptr, 10);
        const Json* list = nullptr;
        if (kind == "rim" && layer.contains("style") && layer["style"].contains("rims")) list = &layer["style"]["rims"];
        if (kind == "band" && layer.contains("style_3d") && layer["style_3d"].is_object() && layer["style_3d"].contains("bands")) list = &layer["style_3d"]["bands"];
        if (!list || n >= list->size()) return {};
        return kind + ":" + str((*list)[n], "id");
    }

    std::string positionalOf(const Json& layer, const std::string& key) {
        const size_t colon = key.find(':');
        if (colon == std::string::npos) return key;
        const std::string kind = key.substr(0, colon), id = key.substr(colon + 1);
        const Json* list = nullptr;
        if (kind == "rim" && layer.contains("style") && layer["style"].contains("rims")) list = &layer["style"]["rims"];
        if (kind == "band" && layer.contains("style_3d") && layer["style_3d"].is_object() && layer["style_3d"].contains("bands")) list = &layer["style_3d"]["bands"];
        if (!list) return {};
        for (size_t i = 0; i < list->size(); i++) if (str((*list)[i], "id") == id) return kind + "#" + std::to_string(i);
        return {};
    }

    // every `link` name in a material tree -> a fresh link id of this application (§6.5.1 step 9)
    void freshLinks(Json& value, std::map<std::string, std::string>& names, TitleOpContext& ctx) {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key() == "link" && it.value().is_string() && !it.value().get<std::string>().empty()) {
                    const std::string name = it.value().get<std::string>();
                    auto found = names.find(name);
                    if (found == names.end()) found = names.emplace(name, ctx.newId()).first;
                    it.value() = found->second;
                } else {
                    freshLinks(it.value(), names, ctx);
                }
            }
        } else if (value.is_array()) {
            for (Json& v : value) freshLinks(v, names, ctx);
        }
    }

    // every assets-relative path under TitleArt/ that a value names (fonts, reflection maps)
    void titleArtPaths(const Json& value, std::set<std::string>& out) {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key() == "path" && it.value().is_string() && it.value().get<std::string>().rfind("TitleArt/", 0) == 0) out.insert(it.value().get<std::string>());
                else titleArtPaths(it.value(), out);
            }
        } else if (value.is_array()) {
            for (const Json& v : value) titleArtPaths(v, out);
        }
    }

    ImportPlan newPlan() {
        ImportPlan plan;
        TitleLibrary& lib = TitleLibrary::get();
        plan.projectDir = lib.projectDir();
        plan.assetsDir = lib.assetsDir();
        plan.preview = currentPreview != nullptr;
        return plan;
    }

    // the plan's files and transient paths joined to the batch's
    void finishPlan(ImportPlan& plan, TitleOpContext& ctx) {
        for (TitleOpFile& f : plan.files) {
            bool already = false;
            for (const TitleOpFile& g : ctx.result.files) already = already || g.path == f.path;
            if (!already) ctx.result.files.push_back(std::move(f));
        }
        if (currentPreview) currentPreview->transient.insert(plan.transient.begin(), plan.transient.end());
    }

    void addWarnings(Json& changes, const ImportPlan& plan) {
        for (const Json& w : plan.warnings) changes["warnings"].push_back(w);
        for (const std::string& p : plan.imported) changes["imported"].push_back(p);
    }

    bool failLibrary(TitleOpContext& ctx, const LibraryError& error, const char* member) {
        TitleJson fix = nullptr;
        return ctx.fail(error.code.empty() ? "library_item_not_found" : error.code, error.message, member, error.suggestions,
                        error.suggestions.empty() ? std::string() : error.suggestions.front(), fix);
    }

    bool optBool(const Json& op, const char* key, bool fallback) {
        auto it = op.find(key);
        return it != op.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    // the scheme a reference names: the library's, else the document's embedded copy (R12)
    std::shared_ptr<const titleart::ColorScheme> resolveScheme(const TitleArtDoc& doc, const std::string& ref, LibraryError* error) {
        LibraryError own;
        if (auto s = TitleLibrary::get().colorScheme(ref, &own)) return s;
        if (doc.root.contains("color_schemes") && doc.root["color_schemes"].is_object() && doc.root["color_schemes"].contains(ref)) {
            auto s = std::make_shared<titleart::ColorScheme>();
            if (titleart::parseColorScheme(doc.root["color_schemes"][ref], *s, nullptr)) {
                s->id = ref;
                return s;
            }
        }
        if (error) *error = own;
        return nullptr;
    }

    // recolours `layerId` and the generator layers it owns with `scheme`
    void recolorWithOwned(TitleArtDoc& doc, const titleart::ColorScheme& scheme, const std::string& layerId, std::vector<std::string>& changed) {
        auto add = [&](const std::vector<std::string>& paths) { changed.insert(changed.end(), paths.begin(), paths.end()); };
        add(titleart::applyColorScheme(doc, scheme, titleart::SchemeTarget{layerId}));
        std::vector<std::string> owned;
        ownedGeneratorIds(doc.root["layers"], layerId, owned);
        for (const std::string& id : owned) add(titleart::applyColorScheme(doc, scheme, titleart::SchemeTarget{id}));
    }

    // ---- apply_look (§6.5.1) ----

    Json lookGeneratorLayer(const Json& g, const Json& target, const std::string& follow, double k, TitleOpContext& ctx) {
        const std::string type = str(g, "type");
        Json params = Json::object();
        Json layer = Json{{"type", "generator"}, {"id", ctx.newId()}};
        std::string blend = str(g, "blend", "normal");
        double opacity = g.value("opacity", 1.0);
        Json effects = g.contains("effects") ? g["effects"] : Json::array();
        if (g.contains("preset") && g["preset"].is_string()) {
            if (const TitleFxPreset* preset = TitleFxPresets::find(type, g["preset"].get<std::string>())) {
                params = preset->params;
                if (preset->layer.is_object()) {
                    if (blend == "normal" && preset->layer.contains("blend")) blend = str(preset->layer, "blend", blend);
                    if (opacity == 1.0 && preset->layer.contains("opacity") && preset->layer["opacity"].is_number()) opacity = preset->layer["opacity"].get<double>();
                    if (effects.empty() && preset->layer.contains("effects") && preset->layer["effects"].is_array()) effects = preset->layer["effects"];
                }
            }
        }
        if (g.contains("params") && g["params"].is_object()) for (auto it = g["params"].begin(); it != g["params"].end(); ++it) params[it.key()] = it.value();
        const titleart::GeneratorDef* def = titleart::findGenerator(type);
        layer["name"] = g.contains("name") ? g["name"] : Json(def ? def->label : type.c_str());
        layer["blend"] = blend;
        layer["opacity"] = opacity;
        layer["generator"] = Json{{"type", type}, {"params", params}};
        const titleart::TitleVec2 origin = positionOf(target);
        layer["effects"] = transferEffects(effects, k, origin, ctx, false);
        layer["look"] = Json{{"id", ""}, {"color_scheme", ""}, {"owner", str(target, "id")}};
        if (def && def->follow != titleart::FollowMode::None) layer["follow"] = follow;
        else layer["transform"] = Json{{"position", Json::array({origin.x, origin.y})}};
        titleart::NormalizeContext context;
        context.ids = &ctx.ids;
        titleart::normalizeTitleArtObject(layer, "layer", "", context);
        if (def) scaleParams(def->params, {}, layer["generator"]["params"], k, nullptr);
        return layer;
    }

    // rims and bands get their ids here (a lone object's normalize does not mint them): the part keys
    // of the look's part effects name them
    void giveIds(Json& style, const char* list, TitleOpContext& ctx) {
        if (!style.contains(list) || !style[list].is_array()) return;
        for (Json& element : style[list]) {
            if (element.is_object() && !titleart::isTitleArtId(str(element, "id"))) element["id"] = ctx.newId();
        }
    }

    bool applyLook(TitleOpContext& ctx, const Json& op) {
        Json* found = ctx.layer(op);
        if (!found) return false;
        const std::string layerId = str(*found, "id");
        std::string ref;
        if (!ctx.requireString(op, "look", ref)) return false;
        LibraryError error;
        const LibraryItemInfo* info = TitleLibrary::get().info(LibraryKind::Look, ref, &error);
        if (!info) return failLibrary(ctx, error, "look");
        if (error.deprecated) ctx.note("deprecated", error.message, "look");
        if (!info->available) return ctx.fail("library_item_unavailable", "'" + info->ref + "' cannot be applied: " + info->unavailableReason, "look");
        std::shared_ptr<const TitleLook> look = TitleLibrary::get().look(info->ref, &error);
        if (!look) return failLibrary(ctx, error, "look");
        const bool applyFont = optBool(op, "apply_font", false);
        const bool keepScheme = optBool(op, "keep_color_scheme", true);
        const titleart::LayerType type = titleart::layerTypeOf(*found);
        const bool styleLook = look->applies() == "style";
        if (styleLook && !titleart::layerTypeHasStyle(type)) {
            return ctx.fail("look_not_applicable", "a Look restyles Text, Shape, Vector and Image layers, not a " + std::string(titleart::layerTypeLabel(type)) + " layer",
                            "layer");
        }
        if (!styleLook && type == titleart::LayerType::Adjustment) {
            return ctx.fail("look_not_applicable", "an effects Look applies to every layer but an Adjustment layer", "layer");
        }
        const bool look3D = look->dimension() == "3d";
        if (styleLook && look3D && !titleart::TitleArtSchema::get().kind("style_3d")) {
            return ctx.fail("dimension_unavailable", "this build has no 3D titles (S86): a 3D Look cannot be applied", "look");
        }

        Json changes = Json{{"replaced", Json::array()}, {"kept_color_scheme", false}, {"dimension_changed", nullptr}, {"generators_added", 0},
                            {"generators_removed", 0}, {"group", "none"}, {"imported", Json::array()}, {"warnings", Json::array()}};
        Json& layer = *found;
        const double size = titleart::layerSourceSize(layer);
        const double k = size > 0 ? size / look->refSize() : 1.0;
        const titleart::TitleVec2 origin = positionOf(layer);
        ImportPlan plan = newPlan();
        std::map<std::string, std::string> linkIds;

        if (styleLook) {
            const std::string before = str(layer, "dimension", "2d");
            const std::string after = look3D ? "3d" : "2d";
            if (before != after) {
                layer["dimension"] = after;
                changes["dimension_changed"] = before + "->" + after;
            }
            if (!look3D) {
                const Json oldFill = layer.contains("style") && layer["style"].contains("face") && layer["style"]["face"].contains("fill") ? layer["style"]["face"]["fill"] : Json();
                Json style = expandObject(look->member("style"), "style2d", &ctx.ids);
                giveIds(style, "rims", ctx);
                if (oldFill.is_object() && str(oldFill, "kind") == "source") style["face"]["fill"] = oldFill;
                freshLinks(style, linkIds, ctx);
                layer["style"] = std::move(style);
                changes["replaced"].push_back("style");
            } else {
                Json style = expandObject(look->member("style_3d"), "style_3d", &ctx.ids);
                giveIds(style, "bands", ctx);
                if (look->member("lighting").is_object()) style["lighting"] = expandObject(look->member("lighting"), "lighting_3d", &ctx.ids);
                freshLinks(style, linkIds, ctx);
                layer["style_3d"] = std::move(style);
                changes["replaced"].push_back("style_3d");
            }
            // per-character faces were the old look's colours (§6.4)
            if (layer.contains("chars") && layer["chars"].is_object()) {
                for (auto it = layer["chars"].begin(); it != layer["chars"].end(); ++it) {
                    if (it.value().is_object() && it.value().contains("face") && !it.value()["face"].is_null()) it.value()["face"] = nullptr;
                }
            }
            Json parts = Json::object();
            const Json& lookParts = look->member("part_effects");
            if (lookParts.is_object()) {
                for (auto it = lookParts.begin(); it != lookParts.end(); ++it) {
                    const std::string key = partKeyAt(layer, it.key());
                    if (key.empty()) {
                        changes["warnings"].push_back(Json{{"code", "part_missing"}, {"message", "the style has no part " + it.key() + ": its effects were dropped"}});
                        continue;
                    }
                    parts[key] = transferEffects(it.value(), k, origin, ctx, false);
                }
            }
            layer["part_effects"] = parts;
            changes["replaced"].push_back("part_effects");
            std::set<std::string> paths;
            titleArtPaths(look3D ? layer["style_3d"] : layer["style"], paths);
            for (const std::string& p : paths) plan.ensure(p);
        }
        layer["effects"] = transferEffects(look->member("effects"), k, origin, ctx, false);
        changes["replaced"].push_back("effects");
        if (dropMasksForTesting) layer["masks"] = Json::array();
        for (const auto& [name, id] : linkIds) {
            if (!ctx.doc.root.contains("material_links") || !ctx.doc.root["material_links"].is_object()) ctx.doc.root["material_links"] = Json::object();
            const Json& links = look->member("links");
            const std::string label = links.is_object() && links.contains(name) ? str(links[name], "name", name) : name;
            ctx.doc.root["material_links"][id] = Json{{"name", label}};
        }

        // font pair (R29)
        if (applyFont && type == titleart::LayerType::Text && look->member("font").is_object()) {
            const Json& pair = look->member("font");
            const std::string latin = str(pair, "latin"), ja = str(pair, "ja");
            const std::string latinPath = latin.empty() ? std::string() : plan.font(latin, str(pair, "latin_file"));
            if (!latinPath.empty()) {
                layer["font"] = Json{{"path", latinPath}, {"guid", ""}};
                layer["face_index"] = 0;
                Json fallbacks = Json::array();
                const PackFont* latinFont = findPackFont(latin);
                if (!ja.empty() && ja != latin && latinFont && !latinFont->coversJapanese()) {
                    const std::string jaPath = plan.font(ja);
                    if (!jaPath.empty()) fallbacks.push_back(Json{{"path", jaPath}, {"guid", ""}});
                }
                if (layer.contains("fallback_fonts") && layer["fallback_fonts"].is_array()) {
                    for (const Json& f : layer["fallback_fonts"]) {
                        if (std::find(fallbacks.begin(), fallbacks.end(), f) == fallbacks.end()) fallbacks.push_back(f);
                    }
                }
                layer["fallback_fonts"] = fallbacks;
                Json variations = latinFont ? latinFont->variations : Json::object();
                if (pair.contains("variations") && pair["variations"].is_object()) variations = pair["variations"];
                layer["variations"] = variations;
                changes["replaced"].push_back("font");
            }
        }

        // generators and the auto group (step 7)
        Json& root = ctx.doc.root;
        const int removed = removeOwnedGenerators(root["layers"], layerId);
        changes["generators_removed"] = removed;
        Slot slot;
        findSlot(ctx.doc, layerId, slot);
        Json* autoGroup = slot.parent && ownerOf(*slot.parent) == layerId ? slot.parent : nullptr;
        const Json& generators = look->member("generators");
        const Json& groupEffects = look->member("group_effects");
        bool scopeGroup = false, blendsBelow = false;
        const bool siblingsBelow = slot.index > 0;
        if (generators.is_array()) {
            for (const Json& g : generators) {
                if (str(g, "scope") == "group") scopeGroup = true;
                if (str(g, "blend", "normal") != "normal" && siblingsBelow) blendsBelow = true;
            }
        }
        const bool needGroup = (groupEffects.is_array() && !groupEffects.empty()) || scopeGroup || blendsBelow;
        if (autoGroup && !needGroup) {
            bool userEffects = false;
            if (autoGroup->contains("effects")) for (const Json& e : (*autoGroup)["effects"]) userEffects = userEffects || !e.value("from_look", false);
            bool onlyOwn = true;
            for (const Json& c : (*autoGroup)["children"]) onlyOwn = onlyOwn && (str(c, "id") == layerId || ownerOf(c) == layerId);
            if (!userEffects && onlyOwn) {
                std::string undo;
                if (!TitleArtOps::runCoreOp(ctx, Json{{"op", "ungroup"}, {"layer", "id:" + str(*autoGroup, "id")}}, &undo)) return false;
                changes["group"] = "dissolved";
                autoGroup = nullptr;
                findSlot(ctx.doc, layerId, slot);
            } else {
                // the user's own group effects keep the group: only the old look's go
                Json kept = Json::array();
                if (autoGroup->contains("effects")) for (const Json& e : (*autoGroup)["effects"]) if (!e.value("from_look", false)) kept.push_back(e);
                (*autoGroup)["effects"] = kept;
                changes["group"] = "kept";
            }
        }
        if (needGroup && !autoGroup) {
            std::string undo;
            const std::string name = str((*slot.list)[slot.index], "name", "Layer");
            if (!TitleArtOps::runCoreOp(ctx, Json{{"op", "group"}, {"layers", Json::array({"id:" + layerId})}, {"name", name + " FX"}}, &undo)) return false;
            findSlot(ctx.doc, layerId, slot);
            autoGroup = slot.parent;
            if (autoGroup) (*autoGroup)["look"] = Json{{"id", ""}, {"color_scheme", ""}, {"owner", layerId}};
            changes["group"] = "created";
        } else if (needGroup) {
            changes["group"] = "kept";
        }
        if (autoGroup) {
            Json effects = transferEffects(groupEffects, k, origin, ctx, true);
            if (autoGroup->contains("effects")) for (const Json& e : (*autoGroup)["effects"]) if (!e.value("from_look", false)) effects.push_back(e);
            (*autoGroup)["effects"] = effects;
        }
        int added = 0;
        if (generators.is_array() && !generators.empty()) {
            findSlot(ctx.doc, layerId, slot);
            const Json target = (*slot.list)[slot.index];
            std::vector<Json> below, above, outerBelow, outerAbove;
            for (const Json& g : generators) {
                const bool outer = str(g, "scope") == "group" && autoGroup;
                Json made = lookGeneratorLayer(g, target, outer ? str(*autoGroup, "id") : layerId, k, ctx);
                const bool isBelow = str(g, "place") == "below";
                (outer ? (isBelow ? outerBelow : outerAbove) : (isBelow ? below : above)).push_back(std::move(made));
                added++;
            }
            Json& list = *slot.list;
            // above the layer bottom first, then below it bottom first (draw order)
            for (size_t i = 0; i < above.size(); i++) list.insert(list.begin() + (long)(slot.index + 1 + i), above[i]);
            for (size_t i = 0; i < below.size(); i++) list.insert(list.begin() + (long)(slot.index + i), below[i]);
            if (autoGroup && (!outerBelow.empty() || !outerAbove.empty())) {
                Slot groupSlot;
                findSlot(ctx.doc, str(*autoGroup, "id"), groupSlot);
                Json& outerList = *groupSlot.list;
                for (size_t i = 0; i < outerAbove.size(); i++) outerList.insert(outerList.begin() + (long)(groupSlot.index + 1 + i), outerAbove[i]);
                for (size_t i = 0; i < outerBelow.size(); i++) outerList.insert(outerList.begin() + (long)(groupSlot.index + i), outerBelow[i]);
            }
        }
        changes["generators_added"] = added;

        // the record and Keep Color Scheme (step 12)
        Json* target = titleart::findLayer(ctx.doc.root, layerId);
        if (!target) return ctx.fail("bad_layer", "the layer went missing while its look applied");
        Json& lookMember = (*target)["look"];
        if (!lookMember.is_object()) lookMember = Json::object();
        const std::string scheme = str(lookMember, "color_scheme");
        lookMember["id"] = info->ref;
        if (!lookMember.contains("owner")) lookMember["owner"] = "";
        lookMember["color_scheme"] = "";
        if (keepScheme && !scheme.empty()) {
            LibraryError schemeError;
            if (auto s = resolveScheme(ctx.doc, scheme, &schemeError)) {
                std::vector<std::string> changed;
                recolorWithOwned(ctx.doc, *s, layerId, changed);
                changes["kept_color_scheme"] = true;
            } else {
                changes["warnings"].push_back(Json{{"code", "color_scheme_missing"}, {"message", "the layer's Color Scheme '" + scheme + "' no longer resolves: cleared"}});
            }
        }
        addWarnings(changes, plan);
        finishPlan(plan, ctx);
        ctx.result.details[std::to_string(ctx.opIndex)] = changes;
        TitleOpChange change;
        change.layer = layerId;
        change.path = "look.id";
        change.to = info->ref;
        change.note = "applied look '" + info->ref + "'";
        ctx.change(std::move(change));
        ctx.undoName = "Apply Look " + info->name.any();
        return true;
    }

    // ---- apply_color_scheme (§6.5.2: the engine's) ----

    bool applyColorSchemeOp(TitleOpContext& ctx, const Json& op) {
        std::string ref;
        if (!ctx.requireString(op, "color_scheme", ref)) return false;
        std::string layerId;
        if (op.contains("layer")) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            layerId = str(*layer, "id");
        }
        LibraryError error;
        auto scheme = resolveScheme(ctx.doc, ref, &error);
        if (!scheme) return failLibrary(ctx, error, "color_scheme");
        if (error.deprecated) ctx.note("deprecated", error.message, "color_scheme");
        std::vector<std::string> changed = titleart::applyColorScheme(ctx.doc, *scheme, titleart::SchemeTarget{layerId});
        Json paths = Json::array();
        for (size_t i = 0; i < changed.size() && i < 64; i++) paths.push_back(changed[i]);
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"color_scheme", scheme->id}, {"changed", changed.size()}, {"paths", paths}};
        TitleOpChange change;
        change.layer = layerId;
        change.path = "look.color_scheme";
        change.to = scheme->id;
        change.note = "recoloured " + std::to_string(changed.size()) + " fields";
        ctx.change(std::move(change));
        const LocalizedText name = decodeLocalized(scheme->name);
        ctx.undoName = "Apply Color Scheme " + (name.any().empty() ? scheme->id : name.any());
        return true;
    }

    // ---- apply_template (§6.5.3) ----

    // the value of an exposed text parameter: its first target's field
    std::string exposedText(const TitleArtDoc& doc, const Json& exposed) {
        if (!exposed.contains("targets") || exposed["targets"].empty()) return {};
        const Json& t = exposed["targets"][0];
        const Json* layer = titleart::findLayer(doc.root, str(t, "layer"));
        if (!layer) return {};
        const Json* v = titleart::getTitleArtField(*layer, "layer", str(t, "field"));
        return v && v->is_string() ? v->get<std::string>() : std::string();
    }

    void remapIds(Json& value, const std::map<std::string, std::string>& ids, const std::string& key = {}) {
        static const std::set<std::string> refKeys = {"id", "follow", "layer", "owner", "link", "glyph_layer"};
        if (value.is_object()) {
            Json out = Json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                std::string k = it.key();
                // part keys rim:<id> / band:<id>, link-group ids
                const size_t colon = k.find(':');
                if (colon != std::string::npos) {
                    auto f = ids.find(k.substr(colon + 1));
                    if (f != ids.end()) k = k.substr(0, colon + 1) + f->second;
                } else if (key == "material_links") {
                    auto f = ids.find(k);
                    if (f != ids.end()) k = f->second;
                }
                Json v = it.value();
                remapIds(v, ids, it.key());
                out[k] = std::move(v);
            }
            value = std::move(out);
        } else if (value.is_array()) {
            for (Json& v : value) remapIds(v, ids, key);
        } else if (value.is_string() && refKeys.count(key)) {
            auto f = ids.find(value.get<std::string>());
            if (f != ids.end()) value = f->second;
        }
    }

    void collectIds(const Json& value, std::vector<std::string>& out, const std::string& key = {}) {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key() == "id" && it.value().is_string() && titleart::isTitleArtId(it.value().get<std::string>())) out.push_back(it.value().get<std::string>());
                if (key == "material_links" && titleart::isTitleArtId(it.key())) out.push_back(it.key());
                collectIds(it.value(), out, it.key());
            }
        } else if (value.is_array()) {
            for (const Json& v : value) collectIds(v, out, key);
        }
    }

    bool isDefaultOutputs(const Json& outputs) {
        return outputs.is_array() && outputs.size() == 1 && str(outputs[0], "id") == "main" && str(outputs[0], "content", "all") == "all" &&
               str(outputs[0], "file").empty();
    }

    bool documentHasJapanese(const TitleArtDoc& doc) {
        bool ja = false;
        titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && hasJapaneseScript(str(layer, "text"))) ja = true;
            return !ja;
        });
        return ja;
    }

    bool applyTemplateDoc(TitleOpContext& ctx, const TitleTemplate& tmpl, const std::string& ref, const Json& op, Json& changes, ImportPlan& plan) {
        const bool keepText = optBool(op, "keep_text", true);
        const TitleArtDoc old = ctx.doc;
        std::string language = str(op, "language");
        if (language.empty()) language = documentHasJapanese(old) ? "ja" : "en";
        if (language != "en" && language != "ja") return ctx.fail("bad_value", "`language` is en or ja", "language", {"en", "ja"});
        // fresh ids for everything the template holds, references remapped (step 2)
        Json content = tmpl.doc.root;
        std::vector<std::string> oldIds;
        collectIds(content, oldIds);
        std::map<std::string, std::string> remap;
        for (const std::string& id : oldIds) if (!remap.count(id)) remap[id] = ctx.newId();
        remapIds(content, remap);
        Json root = Json::object();
        for (auto it = content.begin(); it != content.end(); ++it) {
            const std::string& key = it.key();
            if (key == "format" || key == "version" || key == "guid" || key == "name") root[key] = old.root[key];
            else if (key == "outputs" && !isDefaultOutputs(old.root["outputs"])) root[key] = old.root["outputs"];
            else root[key] = it.value();
        }
        for (const char* key : {"format", "version", "guid", "name"}) if (!root.contains(key)) root[key] = old.root[key];
        ctx.doc.root = std::move(root);
        // placeholders (step 3)
        std::map<std::string, std::string> oldTexts;
        if (old.root.contains("exposed")) {
            for (const Json& e : old.root["exposed"]) if (str(e, "type") == "text") oldTexts[str(e, "name")] = exposedText(old, e);
        }
        Json carried = Json::array();
        for (const Json& p : tmpl.placeholders()) {
            const std::string name = str(p, "name");
            std::string value = str(p["sample"], language.c_str());
            if (value.empty()) value = str(p["sample"], language == "en" ? "ja" : "en");
            const int maxChars = p.value("max_chars", 16);
            bool carriedText = false;
            if (keepText && oldTexts.count(name) && !oldTexts[name].empty()) {
                value = oldTexts[name];
                carriedText = true;
                if ((int)value.size() > maxChars * 2 * 3) {
                    changes["warnings"].push_back(Json{{"code", "placeholder_text_long"}, {"message", "'" + name + "' is longer than the template expects (" + std::to_string(maxChars) + " characters)"}});
                }
                carried.push_back(name);
            }
            for (const Json& e : ctx.doc.root["exposed"]) {
                if (str(e, "name") != name) continue;
                bool first = true;
                for (const Json& t : e["targets"]) {
                    Json* layer = titleart::findLayer(ctx.doc.root, str(t, "layer"));
                    if (!layer) continue;
                    if (!ctx.set(layer, str(t, "field"), value)) return false;
                    // per-language character adjustments of the sample (§6.10.1)
                    if (first && p.contains("chars") && p["chars"].is_object() && !carriedText && p["chars"].contains(language)) {
                        (*layer)["chars"] = p["chars"][language];
                    }
                    first = false;
                }
            }
        }
        changes["carried_text"] = carried;
        Json removedExposed = Json::array();
        for (const auto& [name, text] : oldTexts) {
            bool kept = false;
            for (const Json& p : tmpl.placeholders()) kept = kept || str(p, "name") == name;
            if (!kept) removedExposed.push_back(name);
        }
        if (old.root.contains("exposed")) {
            for (const Json& e : old.root["exposed"]) {
                if (str(e, "type") == "text") continue;
                bool kept = false;
                for (const Json& n : ctx.doc.root["exposed"]) kept = kept || str(n, "name") == str(e, "name");
                if (!kept) removedExposed.push_back(str(e, "name"));
            }
        }
        changes["exposed_removed"] = removedExposed;
        // imports (step 4)
        std::set<std::string> paths;
        titleArtPaths(ctx.doc.root["layers"], paths);
        for (const std::string& p : paths) plan.ensure(p);
        titleart::NormalizeContext context;
        context.ids = &ctx.ids;
        titleart::normalizeTitleArt(ctx.doc, context);
        changes["template"] = ref;
        return true;
    }

    bool applyTemplate(TitleOpContext& ctx, const Json& op) {
        std::string ref;
        if (!ctx.requireString(op, "template", ref)) return false;
        LibraryError error;
        const LibraryItemInfo* info = TitleLibrary::get().info(LibraryKind::Template, ref, &error);
        if (!info) return failLibrary(ctx, error, "template");
        if (error.deprecated) ctx.note("deprecated", error.message, "template");
        if (!info->available) return ctx.fail("library_item_unavailable", "'" + info->ref + "' cannot be applied: " + info->unavailableReason, "template");
        auto tmpl = TitleLibrary::get().templateDoc(info->ref, &error);
        if (!tmpl) return failLibrary(ctx, error, "template");
        Json changes = Json{{"imported", Json::array()}, {"warnings", Json::array()}};
        ImportPlan plan = newPlan();
        if (!applyTemplateDoc(ctx, *tmpl, info->ref, op, changes, plan)) return false;
        addWarnings(changes, plan);
        finishPlan(plan, ctx);
        ctx.result.details[std::to_string(ctx.opIndex)] = changes;
        TitleOpChange change;
        change.path = "layers";
        change.note = "replaced the document's content with template '" + info->ref + "'";
        ctx.change(std::move(change));
        ctx.undoName = "Apply Template " + info->name.any();
        return true;
    }

    // ---- saving (R13-R17) ----

    bool scopeOf(TitleOpContext& ctx, const Json& op, LibraryScope& scope) {
        const std::string s = str(op, "scope", "project");
        if (s == "project") scope = LibraryScope::Project;
        else if (s == "user") scope = LibraryScope::User;
        else return ctx.fail("bad_value", "`scope` is project or user", "scope", {"project", "user"});
        if (scope == LibraryScope::Project && TitleLibrary::get().projectDir().empty()) return ctx.fail("no_project", "no project is open: save with scope user", "scope");
        return true;
    }

    LocalizedText nameOf(const Json& op) {
        return decodeLocalized(op.contains("name") ? op["name"] : Json());
    }

    // the slug of an item: `id` (a slug, or a ref whose last part is one) or the slug of its name
    bool slugFor(TitleOpContext& ctx, const Json& op, const LocalizedText& name, std::string& slug) {
        std::string id = str(op, "id");
        const size_t slash = id.rfind('/');
        if (slash != std::string::npos) id = id.substr(slash + 1);
        slug = id.empty() ? slugOf(name.en.empty() ? name.any() : name.en) : id;
        if (slug.empty()) slug = "item-" + ctx.newId();
        if (!isLibrarySlug(slug)) return ctx.fail("bad_value", "'" + slug + "' is not an id: lowercase letters, digits and -, at most 48", "id");
        return true;
    }

    // the file of a saved item, or a failure when it exists without `overwrite`
    bool targetFile(TitleOpContext& ctx, const Json& op, LibraryKind kind, LibraryScope scope, const std::string& slug, fs::path& file) {
        file = TitleLibrary::get().scopeDir(scope, kind) / (slug + libraryExtension(kind));
        std::error_code ec;
        bool planned = false;
        for (const TitleOpFile& f : ctx.result.files) planned = planned || (f.path == file && !f.remove);
        if ((fs::exists(file, ec) || planned) && !optBool(op, "overwrite", false)) {
            Json fix = Json::array({op});
            fix[0]["overwrite"] = true;
            return ctx.fail("library_item_exists", std::string(libraryKindLabel(kind)) + " '" + libraryScopeName(scope) + "/" + slug + "' exists: pass overwrite: true to replace it",
                            "id", {}, {}, fix);
        }
        return true;
    }

    void writeItem(TitleOpContext& ctx, LibraryScope scope, const fs::path& file, const std::string& text) {
        ctx.result.files.push_back({file, text, scope == LibraryScope::Project, false});
    }

    bool saveLook(TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return false;
        LibraryScope scope;
        if (!scopeOf(ctx, op, scope)) return false;
        const LocalizedText name = nameOf(op);
        if (name.empty()) return ctx.fail("bad_op", "`save_look` takes a `name` (a string or {en, ja})", "name");
        std::string slug;
        if (!slugFor(ctx, op, name, slug)) return false;
        fs::path file;
        if (!targetFile(ctx, op, LibraryKind::Look, scope, slug, file)) return false;
        CaptureOptions options;
        options.name = name;
        options.id = std::string(libraryScopeName(scope)) + "/" + slug;
        options.family = str(op, "family", "custom");
        if (op.contains("tags") && op["tags"].is_array()) for (const Json& t : op["tags"]) if (t.is_string()) options.tags.push_back(t.get<std::string>());
        options.includeFont = optBool(op, "include_font", true);
        TitleLook look;
        std::vector<CapturedAsset> assets;
        std::string error;
        if (!captureLook(ctx.doc, str(*layer, "id"), options, look, &assets, &error)) return ctx.fail("bad_layer", error, "layer");
        // referenced files travel in <slug>.files/ (R13)
        const fs::path filesDir = file.parent_path() / (slug + ".files");
        for (const CapturedAsset& a : assets) {
            std::string bytes;
            if (!readBytes(TitleLibrary::get().assetsDir() / fs::path(a.assetPath), bytes)) continue;
            writeItem(ctx, scope, filesDir / a.file, bytes);
        }
        writeItem(ctx, scope, file, writeLook(look));
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"ref", options.id}, {"file", file.generic_string()}, {"assets", (int)assets.size()}};
        ctx.undoName = "Save Look " + name.any();
        return true;
    }

    bool saveColorScheme(TitleOpContext& ctx, const Json& op) {
        std::string layerId;
        if (op.contains("layer")) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            layerId = str(*layer, "id");
        }
        LibraryScope scope;
        if (!scopeOf(ctx, op, scope)) return false;
        const LocalizedText name = nameOf(op);
        if (name.empty()) return ctx.fail("bad_op", "`save_color_scheme` takes a `name`", "name");
        std::string slug;
        if (!slugFor(ctx, op, name, slug)) return false;
        fs::path file;
        if (!targetFile(ctx, op, LibraryKind::ColorScheme, scope, slug, file)) return false;
        titleart::ColorScheme scheme;
        std::string error;
        const std::string ref = std::string(libraryScopeName(scope)) + "/" + slug;
        if (!extractColorScheme(ctx.doc, layerId, name, ref, scheme, &error)) return ctx.fail("bad_layer", error, "layer");
        writeItem(ctx, scope, file, titleart::writeColorSchemeText(scheme));
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"ref", ref}, {"file", file.generic_string()}};
        ctx.undoName = "Save Color Scheme " + name.any();
        return true;
    }

    bool saveTemplate(TitleOpContext& ctx, const Json& op) {
        LibraryScope scope;
        if (!scopeOf(ctx, op, scope)) return false;
        const LocalizedText name = nameOf(op);
        if (name.empty()) return ctx.fail("bad_op", "`save_template` takes a `name`", "name");
        std::string slug;
        if (!slugFor(ctx, op, name, slug)) return false;
        fs::path file;
        if (!targetFile(ctx, op, LibraryKind::Template, scope, slug, file)) return false;
        std::vector<std::string> wanted;
        if (op.contains("placeholders") && op["placeholders"].is_array()) for (const Json& p : op["placeholders"]) if (p.is_string()) wanted.push_back(p.get<std::string>());
        Json placeholders = Json::array();
        const Json& exposed = ctx.doc.root["exposed"];
        for (const Json& e : exposed) {
            if (str(e, "type") != "text") continue;
            const std::string pname = str(e, "name");
            if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), pname) == wanted.end()) continue;
            const std::string text = exposedText(ctx.doc, e);
            Json sample = Json{{"en", ""}, {"ja", ""}};
            sample[hasJapaneseScript(text) ? "ja" : "en"] = text;
            const char* role = pname == "text" ? "title" : pname == "subtitle" ? "subtitle" : pname == "counter" ? "counter" : "label";
            placeholders.push_back(Json{{"name", pname}, {"role", role}, {"sample", sample}, {"max_chars", std::max<int>(8, (int)text.size())}});
        }
        for (const std::string& w : wanted) {
            bool found = false;
            for (const Json& p : placeholders) found = found || str(p, "name") == w;
            if (!found) return ctx.fail("bad_value", "'" + w + "' is not an exposed text parameter", "placeholders");
        }
        std::string firstText;
        titleart::forEachLayer(ctx.doc.root, [&](const Json& layer, const Json*, int) {
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Text) firstText = str(layer, "id");
            return firstText.empty();
        });
        if (placeholders.empty() && !firstText.empty()) {
            Json fix = Json::array({Json{{"op", "expose"}, {"name", "text"}, {"type", "text"},
                                         {"targets", Json::array({Json{{"layer", "id:" + firstText}, {"field", "text"}}})}}, op});
            return ctx.fail("template_without_placeholder", "a template over a Text layer needs an exposed text parameter as its placeholder: expose the title's text first",
                            "placeholders", {}, {}, fix);
        }
        TitleTemplate tmpl;
        tmpl.doc = ctx.doc;
        tmpl.doc.root["guid"] = titleart::generateTitleArtGuid();
        tmpl.meta = Json{{"version", TITLE_TEMPLATE_MEMBER_VERSION}, {"id", std::string(libraryScopeName(scope)) + "/" + slug}, {"revision", 1},
                         {"name", name.toJson()}, {"family", str(op, "family", "custom")}, {"tags", op.contains("tags") ? op["tags"] : Json::array()},
                         {"placeholders", placeholders}, {"provenance", Json{{"source", "user"}}}};
        writeItem(ctx, scope, file, writeTemplate(tmpl));
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"ref", tmpl.meta["id"]}, {"file", file.generic_string()}, {"placeholders", placeholders.size()}};
        ctx.undoName = "Save Template " + name.any();
        return true;
    }

    // the item a copy or delete names: any kind, or the `kind` given
    const LibraryItemInfo* itemOf(TitleOpContext& ctx, const Json& op, LibraryKind& kind) {
        std::string ref;
        if (!ctx.requireString(op, "ref", ref)) return nullptr;
        std::vector<LibraryKind> kinds(std::begin(LIBRARY_KINDS), std::end(LIBRARY_KINDS));
        if (op.contains("kind")) {
            LibraryKind k;
            if (!libraryKindFromName(str(op, "kind"), k)) {
                ctx.fail("bad_value", "`kind` is looks, color_schemes or templates", "kind", {"looks", "color_schemes", "templates"});
                return nullptr;
            }
            kinds = {k};
        }
        LibraryError first;
        for (LibraryKind k : kinds) {
            LibraryError error;
            if (const LibraryItemInfo* info = TitleLibrary::get().info(k, ref, &error)) {
                kind = k;
                return info;
            }
            if (first.code.empty()) first = error;
        }
        failLibrary(ctx, first, "ref");
        return nullptr;
    }

    // an item's text with its own id set to `ref`
    std::string withId(LibraryKind kind, const std::string& text, const std::string& ref) {
        if (kind == LibraryKind::Look) {
            LookParseResult parsed = parseLook(text);
            if (!parsed.ok) return {};
            parsed.look.root["id"] = ref;
            return writeLook(parsed.look);
        }
        if (kind == LibraryKind::ColorScheme) {
            titleart::ColorScheme s;
            if (!titleart::parseColorScheme(Json::parse(text, nullptr, false), s, nullptr)) return {};
            s.id = ref;
            return titleart::writeColorSchemeText(s);
        }
        TemplateParseResult parsed = parseTemplate(text);
        if (!parsed.ok) return {};
        parsed.tmpl.meta["id"] = ref;
        return writeTemplate(parsed.tmpl);
    }

    bool copyLibraryItem(TitleOpContext& ctx, const Json& op) {
        LibraryKind kind;
        const LibraryItemInfo* item = itemOf(ctx, op, kind);
        if (!item) return false;
        const std::string to = str(op, "to");
        LibraryScope scope;
        if (to == "project") scope = LibraryScope::Project;
        else if (to == "user") scope = LibraryScope::User;
        else return ctx.fail("bad_value", "`to` is project or user", "to", {"project", "user"});
        if (scope == LibraryScope::Project && TitleLibrary::get().projectDir().empty()) return ctx.fail("no_project", "no project is open", "to");
        std::string slug = str(op, "id");
        if (slug.empty()) slug = item->ref.substr(item->ref.rfind('/') + 1);
        if (slug.rfind('/') != std::string::npos) slug = slug.substr(slug.rfind('/') + 1);
        if (!isLibrarySlug(slug)) return ctx.fail("bad_value", "'" + slug + "' is not an id", "id");
        fs::path file;
        if (!targetFile(ctx, op, kind, scope, slug, file)) return false;
        const std::string ref = std::string(libraryScopeName(scope)) + "/" + slug;
        const std::string text = withId(kind, TitleLibrary::get().itemText(*item), ref);
        if (text.empty()) return ctx.fail("library_item_invalid", "'" + item->ref + "' does not load", "ref");
        writeItem(ctx, scope, file, text);
        if (!item->file.empty()) {
            const fs::path files = item->file.parent_path() / (item->file.stem().string() + ".files");
            std::error_code ec;
            if (fs::is_directory(files, ec)) {
                for (const auto& e : fs::directory_iterator(files, ec)) {
                    std::string bytes;
                    if (readBytes(e.path(), bytes)) writeItem(ctx, scope, file.parent_path() / (slug + ".files") / e.path().filename(), bytes);
                }
            }
        }
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"ref", ref}, {"file", file.generic_string()}};
        ctx.undoName = std::string("Copy ") + libraryKindLabel(kind) + " " + item->name.any();
        return true;
    }

    bool deleteLibraryItem(TitleOpContext& ctx, const Json& op) {
        LibraryKind kind;
        const LibraryItemInfo* item = itemOf(ctx, op, kind);
        if (!item) return false;
        if (item->scope == LibraryScope::BuiltIn) {
            return ctx.fail("library_item_builtin", "'" + item->ref + "' is built in: built-ins cannot be deleted (copy it to project or user to own one)", "ref");
        }
        std::vector<fs::path> paths = {item->file};
        const fs::path files = item->file.parent_path() / (item->file.stem().string() + ".files");
        std::error_code ec;
        if (fs::is_directory(files, ec)) for (const auto& e : fs::directory_iterator(files, ec)) paths.push_back(e.path());
        if (item->scope == LibraryScope::Project) {
            if (!libraryOnlyBatch) {
                return ctx.fail("bad_op", "deleting a project library item is its own undo step: send it in a batch of library ops only (no document `path`)", "op");
            }
            for (const fs::path& p : paths) ctx.result.files.push_back({p, {}, true, true});
        } else {
            // the personal library keeps deleted items 30 days in .trash/ (R16)
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            const fs::path trash = TitleLibrary::get().userDir() / ".trash" / std::to_string((long long)now);
            for (const fs::path& p : paths) {
                std::string bytes;
                if (readBytes(p, bytes)) {
                    const fs::path rel = p.lexically_relative(TitleLibrary::get().userDir());
                    ctx.result.files.push_back({trash / rel, bytes, false, false});
                }
                ctx.result.files.push_back({p, {}, false, true});
            }
        }
        ctx.result.details[std::to_string(ctx.opIndex)] = Json{{"ref", item->ref}, {"scope", libraryScopeName(item->scope)}};
        ctx.undoName = std::string("Delete ") + libraryKindLabel(kind) + " " + item->name.any();
        return true;
    }

    // R12: the data of every scheme an exposed color_scheme parameter offers, kept in the document
    void keepEmbeddedSchemes(TitleOpContext& ctx) {
        Json& root = ctx.doc.root;
        std::set<std::string> wanted;
        bool any = false, all = false;
        if (root.contains("exposed") && root["exposed"].is_array()) {
            for (const Json& e : root["exposed"]) {
                if (str(e, "type") != "color_scheme") continue;
                any = true;
                if (!e.contains("options") || e["options"].empty()) all = true;
                else for (const Json& o : e["options"]) if (o.is_string()) wanted.insert(o.get<std::string>());
            }
        }
        if (!any) {
            if (root.contains("color_schemes") && root["color_schemes"].is_object() && !root["color_schemes"].empty()) root["color_schemes"] = Json::object();
            return;
        }
        if (all) {
            for (const LibraryItemInfo& item : TitleLibrary::get().items(LibraryKind::ColorScheme)) {
                if (item.scope != LibraryScope::User) wanted.insert(item.ref);
            }
        }
        Json schemes = Json::object();
        const Json previous = root.contains("color_schemes") && root["color_schemes"].is_object() ? root["color_schemes"] : Json::object();
        for (const std::string& id : wanted) {
            if (auto s = TitleLibrary::get().colorScheme(id, nullptr)) {
                schemes[id] = titleart::writeColorScheme(*s);
            } else if (previous.contains(id)) {
                schemes[id] = previous[id];      // a scheme only this document still carries
            } else {
                ctx.note("color_scheme_missing", "the exposed Color Scheme '" + id + "' is not in the library");
            }
        }
        if (schemes != previous) root["color_schemes"] = schemes;
    }

}

// ---- public ----

void titlelibrary::setDropMasksForTesting(bool drop) {
    dropMasksForTesting = drop;
}

void titlelibrary::registerTitleLibraryOps() {
    static bool done = false;
    if (done) return;
    done = true;
    TitleArtOps::registerOp("apply_look", applyLook);
    TitleArtOps::registerOp("apply_color_scheme", applyColorSchemeOp);
    TitleArtOps::registerOp("apply_template", applyTemplate);
    TitleArtOps::registerOp("save_look", saveLook);
    TitleArtOps::registerOp("save_color_scheme", saveColorScheme);
    TitleArtOps::registerOp("save_template", saveTemplate);
    TitleArtOps::registerOp("copy_library_item", copyLibraryItem);
    TitleArtOps::registerOp("delete_library_item", deleteLibraryItem);
    TitleArtOps::registerBatchFinisher("s84_color_schemes", keepEmbeddedSchemes);
}

bool titlelibrary::preview(const TitleArtDoc& doc, const Json& ops, TitleArtDoc& out, std::map<std::string, std::string>* transient, std::string* error) {
    PreviewScope scope;
    PreviewScope* outer = currentPreview;
    currentPreview = &scope;
    TitleOpsResult result = TitleArtOps::apply(doc, ops);
    currentPreview = outer;
    if (!result.ok) {
        if (error) *error = result.error();
        return false;
    }
    out = std::move(result.doc);
    if (transient) transient->insert(scope.transient.begin(), scope.transient.end());
    return true;
}

namespace {
    Project* openProject = nullptr;
}

void titlelibrary::setCurrentProject(Project* project) {
    openProject = project;
}

Project* titlelibrary::currentProject() {
    return openProject;
}

bool titlelibrary::isLibraryOnlyBatch(const Json& ops) {
    static const std::set<std::string> libraryOps = {"copy_library_item", "delete_library_item", "import_ps_styles"};
    const Json list = ops.is_array() ? ops : Json::array({ops});
    if (list.empty()) return false;
    for (const Json& op : list) {
        if (!op.is_object() || !libraryOps.count(str(op, "op"))) return false;
    }
    return true;
}

TitleOpsResult titlelibrary::runLibraryBatch(Project* project, const Json& ops) {
    if (project) TitleLibrary::get().setProject(project->getProjectPath(), project->getAssetsPath());
    libraryOnlyBatch = true;
    TitleOpsOptions options;
    if (project) {
        // ops that read files of the project (import_ps_styles) resolve them from here
        options.projectPath = project->getProjectPath().string();
        options.assetsPath = project->getAssetsPath().string();
    }
    TitleOpsResult result = TitleArtOps::apply(titleart::makeNewTitleArt(), ops, options);
    libraryOnlyBatch = false;
    if (!result.ok) return result;
    std::vector<WriteFilesCmd::Item> writes;
    std::vector<fs::path> removes;
    for (const TitleOpFile& f : result.files) {
        if (!f.undoable) continue;
        if (f.remove) removes.push_back(f.path);
        else writes.push_back({f.path, f.content});
    }
    auto after = [](const std::vector<fs::path>&) { TitleLibrary::get().invalidate(); };
    if (project && !writes.empty()) {
        auto* command = new WriteFilesCmd(std::move(writes), after);
        command->setName(result.undoName);
        command->setNoMerge();
        if (!project->getProjectCommandHistory()->addCommand(command)) {
            result.ok = false;
            result.problems.push_back(TitleOpProblem{"error", "write_failed", -1, {}, "writing the library files failed. Nothing was applied."});
            return result;
        }
    }
    if (project && !removes.empty()) {
        auto* command = new RemoveFilesCmd(std::move(removes), after);
        if (!project->getProjectCommandHistory()->addCommand(command)) {
            result.ok = false;
            result.problems.push_back(TitleOpProblem{"error", "write_failed", -1, {}, "deleting the library files failed. Nothing was applied."});
            return result;
        }
    }
    for (const TitleOpFile& f : result.files) {
        if (f.undoable) continue;
        std::error_code ec;
        if (f.remove) {
            fs::remove(f.path, ec);
            continue;
        }
        fs::create_directories(f.path.parent_path(), ec);
        std::ofstream out(f.path, std::ios::binary | std::ios::trunc);
        out << f.content;
    }
    TitleLibrary::get().invalidate();
    return result;
}

Json titlelibrary::applyOpsFor(LibraryKind kind, const std::string& ref, const std::vector<std::string>& layers, bool applyFont, bool keepColorScheme) {
    Json ops = Json::array();
    if (kind == LibraryKind::Template) {
        ops.push_back(Json{{"op", "apply_template"}, {"template", ref}, {"keep_text", true}});
        return ops;
    }
    if (kind == LibraryKind::ColorScheme) {
        if (layers.empty()) ops.push_back(Json{{"op", "apply_color_scheme"}, {"color_scheme", ref}});
        for (const std::string& l : layers) ops.push_back(Json{{"op", "apply_color_scheme"}, {"layer", l}, {"color_scheme", ref}});
        return ops;
    }
    for (const std::string& l : layers) {
        ops.push_back(Json{{"op", "apply_look"}, {"layer", l}, {"look", ref}, {"apply_font", applyFont}, {"keep_color_scheme", keepColorScheme}});
    }
    return ops;
}

// ---- capture (§6.5.4) ----

bool titlelibrary::captureLook(const TitleArtDoc& docIn, const std::string& layerId, const CaptureOptions& options, TitleLook& out,
                               std::vector<CapturedAsset>* assets, std::string* error) {
    TitleArtDoc doc = docIn;
    Slot slot;
    if (!findSlot(doc, layerId, slot)) {
        if (error) *error = "no layer " + layerId;
        return false;
    }
    const Json layer = (*slot.list)[slot.index];
    const titleart::LayerType type = titleart::layerTypeOf(layer);
    if (!titleart::layerTypeHasStyle(type)) {
        if (error) *error = "only Text, Shape, Vector and Image layers have a look";
        return false;
    }
    const bool is3D = str(layer, "dimension", "2d") == "3d";
    const titleart::TitleVec2 origin = positionOf(layer);
    Json look = Json::object();
    look["format"] = TITLE_LOOK_FORMAT;
    look["version"] = TITLE_LOOK_VERSION;
    look["id"] = options.id;
    look["revision"] = 1;
    look["name"] = options.name.toJson();
    look["family"] = options.family;
    look["tags"] = options.tags;
    look["dimension"] = is3D ? "3d" : "2d";
    look["applies"] = "style";
    look["ref_size"] = titleart::layerSourceSize(layer) > 0 ? titleart::layerSourceSize(layer) : TITLE_LOOK_DEFAULT_REF_SIZE;
    // effects with handles relative to the layer
    auto relative = [&](const Json& list) {
        Json outList = Json::array();
        if (!list.is_array()) return outList;
        for (const Json& e : list) {
            Json copy = e;
            if (const titleart::EffectDef* def = titleart::findEffect(str(e, "type"))) {
                if (copy.contains("params")) relativeParams(def->gizmos, def->params, copy["params"], origin);
            }
            copy.erase("from_look");
            outList.push_back(copy);
        }
        return outList;
    };
    // links: the layer's link ids named by the document's material_links
    std::map<std::string, std::string> linkNames;
    std::function<void(Json&)> nameLinks = [&](Json& v) {
        if (v.is_object()) {
            for (auto it = v.begin(); it != v.end(); ++it) {
                if (it.key() == "link" && it.value().is_string() && !it.value().get<std::string>().empty()) {
                    const std::string id = it.value().get<std::string>();
                    std::string name = id;
                    if (doc.root.contains("material_links") && doc.root["material_links"].contains(id)) name = str(doc.root["material_links"][id], "name", id);
                    linkNames[name] = name;
                    it.value() = name;
                } else {
                    nameLinks(it.value());
                }
            }
        } else if (v.is_array()) {
            for (Json& x : v) nameLinks(x);
        }
    };
    Json style = is3D ? layer["style_3d"] : layer["style"];
    if (is3D && style.is_null()) style = Json::object();
    nameLinks(style);
    // referenced assets (pattern and image fills) travel with the look
    std::function<void(Json&)> collectAssets = [&](Json& v) {
        if (v.is_object()) {
            for (auto it = v.begin(); it != v.end(); ++it) {
                Json& value = it.value();
                if (value.is_object() && value.contains("path") && value["path"].is_string() && value.contains("guid") && value.size() == 2) {
                    const std::string path = value["path"].get<std::string>();
                    if (path.empty() || path.rfind("TitleArt/", 0) == 0) continue;     // pack files re-import from the pack
                    if (assets) {
                        const std::string key = "a" + std::to_string(assets->size() + 1);
                        assets->push_back({key, path, fs::path(path).filename().string()});
                        value = Json{{"path", "asset:" + key}, {"guid", ""}};
                    }
                } else {
                    collectAssets(value);
                }
            }
        } else if (v.is_array()) {
            for (Json& x : v) collectAssets(x);
        }
    };
    collectAssets(style);
    if (is3D) {
        // the effective lighting as a complete override (S86: the layer's over the document's)
        Json scene = doc.root.contains("scene_3d") && doc.root["scene_3d"].is_object() ? doc.root["scene_3d"] : titleart::TitleArtSchema::get().defaultObject("scene_3d");
        Json own = style.contains("lighting") && style["lighting"].is_object() ? style["lighting"] : Json::object();
        Json lighting = Json::object();
        Json env = scene.contains("environment") ? scene["environment"] : Json::object();
        if (own.contains("environment") && own["environment"].is_object()) for (auto it = own["environment"].begin(); it != own["environment"].end(); ++it) env[it.key()] = it.value();
        lighting["environment"] = env;
        lighting["tone_mapping"] = own.contains("tone_mapping") ? own["tone_mapping"] : scene.value("tone_mapping", Json("neutral"));
        lighting["exposure"] = own.contains("exposure") ? own["exposure"] : scene.value("exposure", Json(1));
        Json lights = own.contains("lights") ? own["lights"] : scene.value("lights", Json::array());
        for (Json& l : lights) l.erase("id");
        lighting["lights"] = lights;
        style.erase("lighting");
        look["style_3d"] = style;
        look["lighting"] = lighting;
    } else {
        look["style"] = style;
    }
    look["effects"] = relative(layer.contains("effects") ? layer["effects"] : Json());
    Json parts = Json::object();
    if (layer.contains("part_effects") && layer["part_effects"].is_object()) {
        for (auto it = layer["part_effects"].begin(); it != layer["part_effects"].end(); ++it) {
            const std::string pos = positionalOf(layer, it.key());
            if (!pos.empty() && !it.value().empty()) parts[pos] = relative(it.value());
        }
    }
    look["part_effects"] = parts;
    // generators: the ones following the layer inside its auto group, and the ones it owns
    Json* own = slot.parent && ownerOf(*slot.parent) == layerId ? slot.parent : nullptr;
    Json generators = Json::array();
    auto addGenerator = [&](const Json& g, const char* place, bool outer) {
        Json entry = Json::object();
        entry["type"] = g["generator"].value("type", std::string());
        entry["place"] = place;
        entry["scope"] = outer ? "group" : "layer";
        entry["name"] = str(g, "name");
        entry["blend"] = str(g, "blend", "normal");
        entry["opacity"] = g.value("opacity", 1.0);
        entry["params"] = g["generator"].contains("params") ? g["generator"]["params"] : Json::object();
        entry["effects"] = relative(g.contains("effects") ? g["effects"] : Json());
        generators.push_back(entry);
    };
    std::set<std::string> taken;
    auto visitList = [&](const Json& list, const std::string& pivot, bool outer) {
        bool beforePivot = true;
        for (const Json& l : list) {
            if (str(l, "id") == pivot) {
                beforePivot = false;
                continue;
            }
            if (l.value("type", std::string()) != "generator") continue;
            const std::string follow = str(l, "follow");
            const bool follows = follow == layerId || (own && follow == str(*own, "id"));
            if (!(follows || ownerOf(l) == layerId) || taken.count(str(l, "id"))) continue;
            taken.insert(str(l, "id"));
            addGenerator(l, beforePivot ? "below" : "above", outer || (own && follow == str(*own, "id")));
        }
    };
    visitList(*slot.list, layerId, false);
    if (own) {
        Slot groupSlot;
        if (findSlot(doc, str(*own, "id"), groupSlot)) visitList(*groupSlot.list, str(*own, "id"), true);
        look["group_effects"] = relative(own->contains("effects") ? (*own)["effects"] : Json());
    }
    look["generators"] = generators;
    Json links = Json::object();
    for (const auto& [name, label] : linkNames) links[name] = Json{{"name", label}};
    look["links"] = links;
    if (options.includeFont && type == titleart::LayerType::Text && layer.contains("font") && layer["font"].is_object()) {
        const std::string path = str(layer["font"], "path");
        const std::string fonts = std::string(TITLE_FONTS_ASSET_DIR) + "/";
        if (path.rfind(fonts, 0) == 0) {
            const std::string rest = path.substr(fonts.size());
            const std::string packId = rest.substr(0, rest.find('/'));
            Json pair = Json{{"latin", packId}, {"ja", packId}};
            if (layer.contains("fallback_fonts") && !layer["fallback_fonts"].empty()) {
                const std::string fb = str(layer["fallback_fonts"][0], "path");
                if (fb.rfind(fonts, 0) == 0) pair["ja"] = fb.substr(fonts.size(), fb.substr(fonts.size()).find('/'));
            }
            if (layer.contains("variations") && !layer["variations"].empty()) pair["variations"] = layer["variations"];
            look["font"] = pair;
        } else if (!path.empty() && assets) {
            const std::string key = "a" + std::to_string(assets->size() + 1);
            assets->push_back({key, path, fs::path(path).filename().string()});
            look["font"] = Json{{"file", fs::path(path).filename().string()}};
        }
    }
    if (assets && !assets->empty()) {
        Json list = Json::array();
        for (const CapturedAsset& a : *assets) list.push_back(Json{{"key", a.key}, {"file", a.file}});
        look["assets"] = list;
    }
    look["provenance"] = Json{{"source", "user"}};
    std::vector<LibraryWarning> warnings;
    out.root = canonicalLookJson(look, &warnings);
    out.readOnly = false;
    return true;
}

// ---- extract (R14: §6.5.2's roles in reverse) ----

namespace {

    double luma(const titleart::TitleColor& c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

    titleart::TitleColor colorOf(const Json& fill) {
        titleart::TitleColor c;
        if (fill.contains("color")) titleart::parseTitleColor(fill["color"], c);
        return c;
    }

    // a material's fill colour sampled where a solid would be: the gradient's middle or the colour
    titleart::TitleColor fillColor(const Json& material) {
        const Json& fill = material.contains("fill") ? material["fill"] : Json::object();
        if (str(fill, "kind", "solid") != "solid" && fill.contains("gradient")) {
            const titleart::TitleGradient g = titleart::decodeGradient(fill["gradient"]);
            if (!g.stops.empty()) return g.stops[g.stops.size() / 2].color;
        }
        return colorOf(fill);
    }

    titleart::ColorPaint paintOf(const Json& material) {
        titleart::ColorPaint p;
        p.fill = material.contains("fill") ? material["fill"] : Json();
        if (material.contains("finish") && material["finish"].value("enabled", false)) p.ramp = material["finish"]["gradient"];
        if (material.contains("bevel")) p.shade = str(material["bevel"], "shadow_color", "#000000");
        if (material.contains("pattern") && str(material["pattern"], "kind", "none") != "none" && material["pattern"].value("opacity", 1.0) > 0) p.pattern = material["pattern"];
        return p;
    }

}

bool titlelibrary::extractColorScheme(const TitleArtDoc& doc, const std::string& layerIdIn, const LocalizedText& name, const std::string& id,
                                      titleart::ColorScheme& out, std::string* error) {
    std::string layerId = layerIdIn;
    if (layerId.empty()) {
        titleart::forEachLayer(doc.root, [&](const Json& l, const Json*, int) {
            if (titleart::layerTypeHasStyle(titleart::layerTypeOf(l)) && !l.value("guide", false)) layerId = str(l, "id");
            return layerId.empty();
        });
    }
    const Json* layer = titleart::findLayer(doc.root, layerId);
    if (!layer || !titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer))) {
        if (error) *error = "no styled layer to take colours from";
        return false;
    }
    titleart::ColorScheme s;
    s.id = id;
    s.name = name.toJson();
    s.provenance = Json{{"source", "user"}};
    const bool is3D = str(*layer, "dimension", "2d") == "3d" && layer->contains("style_3d") && (*layer)["style_3d"].is_object();
    std::vector<titleart::TitleColor> lights;
    if (!is3D) {
        const Json& style = (*layer)["style"];
        s.face = paintOf(style["face"]);
        if (style["face"].contains("inner_glow")) s.faceGlow = str(style["face"]["inner_glow"], "color", "#ffffff");
        bool haveDark = false, haveBase = false;
        for (const Json& rim : style["rims"]) {
            if (!rim.value("enabled", true) || rim.value("width", 0.0) <= 0) continue;
            const Json& m = rim["material"];
            const titleart::TitleVec2 offset = titleart::decodeVec2(rim.contains("offset") ? rim["offset"] : Json::array({0, 0}));
            if (offset.x != 0 || offset.y != 0) {
                if (!haveBase) s.base = paintOf(m);
                haveBase = true;
            } else if (luma(fillColor(m)) < 0.18) {
                if (!haveDark) s.dark = paintOf(m);
                haveDark = true;
            } else {
                s.accents.push_back(paintOf(m));
            }
        }
        if (!haveDark) s.dark.fill = Json{{"kind", "solid"}, {"color", "#0a0604"}};
        if (!haveBase) s.base = s.dark;
        if (s.accents.empty()) s.accents.push_back(s.face);
    } else {
        const Json& style = (*layer)["style_3d"];
        s.face.material3d = style.contains("face") ? style["face"] : Json();
        s.face.fill = Json{{"kind", "solid"}, {"color", str(s.face.material3d, "color", "#ffffff")}};
        if (style.contains("side") && style["side"].is_object()) s.side3d = style["side"];
        if (style.contains("bands")) {
            for (const Json& b : style["bands"]) {
                if (!b.is_object()) continue;
                titleart::ColorPaint p;
                p.material3d = b["material"];
                p.fill = Json{{"kind", "solid"}, {"color", str(b["material"], "color", "#ffffff")}};
                s.accents.push_back(p);
            }
        }
        if (s.accents.empty()) s.accents.push_back(s.face);
        s.dark = s.accents.front();
        s.base = s.accents.back();
    }
    // light colours of the layer's effects
    bool glowSet = false, shadowSet = false;
    for (const Json& e : (*layer)["effects"]) {
        const std::string type = str(e, "type");
        const Json& p = e["params"];
        if (!glowSet && (type == "outer_glow" || type == "inner_glow" || type == "light_glow") && p.contains("color")) {
            s.glow = str(p, "color", s.glow);
            glowSet = true;
        }
        if (!shadowSet && (type == "drop_shadow" || type == "inner_shadow" || type == "perspective_shadow") && p.contains("color")) {
            s.shadow = str(p, "color", s.shadow);
            shadowSet = true;
        }
    }
    if (!glowSet) s.glow = s.faceGlow;
    s.glint = s.faceGlow;
    // the light ramp: the face's gradient stops, the glow and the shadow by luma (§6.9.1)
    std::vector<titleart::TitleColor> ramp;
    const Json& faceFill = s.face.fill.is_object() ? s.face.fill : Json::object();
    if (faceFill.contains("gradient")) for (const titleart::TitleGradientStop& st : titleart::decodeGradient(faceFill["gradient"]).stops) ramp.push_back(st.color);
    else ramp.push_back(colorOf(faceFill));
    titleart::TitleColor g, sh;
    titleart::parseTitleColor(Json(s.glow), g);
    titleart::parseTitleColor(Json(s.shadow), sh);
    ramp.push_back(g);
    ramp.push_back(sh);
    std::sort(ramp.begin(), ramp.end(), [](const auto& a, const auto& b) { return luma(a) < luma(b); });
    std::vector<titleart::TitleColor> unique;
    for (const auto& c : ramp) {
        bool close = false;
        for (const auto& u : unique) close = close || (std::abs(c.r - u.r) + std::abs(c.g - u.g) + std::abs(c.b - u.b) < 3.0 / 255 * 3);
        if (!close) unique.push_back(c);
    }
    while (unique.size() > 7) unique.erase(unique.begin() + (long)(unique.size() / 2));
    for (const auto& c : unique) s.lightRamp.push_back(titleart::formatTitleColor(c));
    while (s.lightRamp.size() < 4) s.lightRamp.insert(s.lightRamp.begin() + (long)(s.lightRamp.size() / 2), s.lightRamp[s.lightRamp.size() / 2]);
    Json swatch = Json::array();
    for (const auto& c : unique) swatch.push_back(titleart::formatTitleColor(c));
    s.swatch = swatch;
    out = std::move(s);
    return true;
}

bool titlelibrary::lookFromStyle(const StyleSource& source, double refSize, const CaptureOptions& options, TitleLook& out, std::string* error) {
    Json look = Json::object();
    look["format"] = TITLE_LOOK_FORMAT;
    look["version"] = TITLE_LOOK_VERSION;
    look["id"] = options.id;
    look["revision"] = 1;
    look["name"] = options.name.toJson();
    if (!source.description.empty()) look["description"] = source.description.toJson();
    look["family"] = options.family;
    look["tags"] = options.tags;
    look["dimension"] = "2d";
    look["applies"] = "style";
    look["ref_size"] = refSize > 0 ? refSize : TITLE_LOOK_DEFAULT_REF_SIZE;
    look["style"] = source.style;
    look["effects"] = source.effects;
    look["part_effects"] = Json::object();
    look["generators"] = Json::array();
    look["group_effects"] = Json::array();
    look["links"] = Json::object();
    if (!source.assets.empty()) look["assets"] = source.assets;
    if (!source.provenance.is_null()) look["provenance"] = source.provenance;
    LookParseResult parsed = parseLookJson(canonicalLookJson(look, nullptr));
    if (!parsed.ok) {
        if (error) *error = parsed.error;
        return false;
    }
    out = std::move(parsed.look);
    return true;
}
