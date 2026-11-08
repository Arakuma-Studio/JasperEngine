// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtOps.h"
#include "fx/FxRecipes.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::LayerType;
using titleart::TitleArtDoc;

namespace {

    constexpr double PI = 3.14159265358979323846;

    // ---- the registries ----

    struct OpRegistry {
        std::map<std::string, TitleOpHandler> handlers;
        std::map<std::string, TitleCreateImporter> importers;
    };

    OpRegistry& registry() {
        static OpRegistry* r = new OpRegistry();
        return *r;
    }

    const std::vector<ReservedTitleOp>& reservedTable() {
        static const std::vector<ReservedTitleOp> table = {
            {"apply_recipe", "S83"},
            {"apply_look", "S84"}, {"apply_color_scheme", "S84"}, {"apply_template", "S84"}, {"save_look", "S84"},
            {"save_color_scheme", "S84"}, {"save_template", "S84"}, {"copy_library_item", "S84"}, {"delete_library_item", "S84"},
            {"add_animation", "S85"}, {"set_animation", "S85"}, {"remove_animation", "S85"}, {"duplicate_animation", "S85"},
            {"set_default_animation", "S85"}, {"set_track", "S85"}, {"set_key", "S85"}, {"remove_track", "S85"},
            {"remove_keys", "S85"}, {"retime", "S85"}, {"apply_animation_preset", "S85"}, {"set_marker", "S85"},
            {"remove_marker", "S85"},
            {"set_dimension", "S86"}, {"add_band", "S86"}, {"remove_band", "S86"}, {"move_band", "S86"},
            {"apply_material", "S86"}, {"fit_to_tier", "S86"},
            {"add_language", "S87"}, {"remove_language", "S87"}, {"set_source_language", "S87"}, {"set_translation", "S87"},
            {"revert_translation", "S87"}, {"mark_reviewed", "S87"}, {"import_strings", "S87"}, {"apply_ps_style", "S87"},
            {"import_ps_styles", "S87"}, {"import_svg", "S87"}, {"reimport_pachislop_3d", "S87"}};
        return table;
    }

    const std::vector<std::string>& s80Table() {
        static const std::vector<std::string> ops = {
            "set", "set_text", "add_layer", "remove_layer", "duplicate_layer", "move_layer", "group", "ungroup",
            "add_effect", "remove_effect", "move_effect", "add_rim", "remove_rim", "move_rim", "add_mask", "remove_mask",
            "expose", "unexpose", "link_material", "unlink_material"};
        return ops;
    }

    std::string lowerExtension(const std::string& path) {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return ext;
    }

    // ---- small helpers ----

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    Json& arrayAt(Json& object, const char* key) {
        Json& value = object[key];
        if (!value.is_array()) value = Json::array();
        return value;
    }

    std::string titleCase(std::string text) {
        bool upper = true;
        for (char& c : text) {
            if (c == '_') {
                c = ' ';
                upper = true;
            } else if (upper) {
                c = (char)std::toupper((unsigned char)c);
                upper = false;
            }
        }
        return text;
    }

    // the list holding a layer (the top level or a group's children) and its index there
    struct Slot {
        Json* list = nullptr;
        size_t index = 0;
        Json* parent = nullptr;     // the group, null at the top level
    };

    bool findSlot(Json& list, Json* parent, const std::string& id, Slot& out) {
        if (!list.is_array()) return false;
        for (size_t i = 0; i < list.size(); i++) {
            Json& layer = list[i];
            if (str(layer, "id") == id) {
                out = {&list, i, parent};
                return true;
            }
            if (titleart::layerTypeOf(layer) == LayerType::Group && layer.contains("children")) {
                if (findSlot(layer["children"], &layer, id, out)) return true;
            }
        }
        return false;
    }

    bool slotOf(TitleArtDoc& doc, const std::string& id, Slot& out) {
        return findSlot(doc.layers(), nullptr, id, out);
    }

    int depthOf(TitleArtDoc& doc, const std::string& id) {
        int found = -1;
        titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int depth) {
            if (str(layer, "id") == id) {
                found = depth;
                return false;
            }
            return true;
        });
        return found;
    }

    int subtreeHeight(const Json& layer) {
        int height = 0;
        for (const Json& child : titleart::layerChildren(layer)) height = std::max(height, 1 + subtreeHeight(child));
        return height;
    }

    bool contains(const Json& layer, const std::string& id) {
        if (str(layer, "id") == id) return true;
        for (const Json& child : titleart::layerChildren(layer)) {
            if (contains(child, id)) return true;
        }
        return false;
    }

    void subtreeIds(const Json& layer, std::set<std::string>& out) {
        out.insert(str(layer, "id"));
        for (const Json& child : titleart::layerChildren(layer)) subtreeIds(child, out);
    }

    // New ids for a layer and everything in it (layers, rims, masks, effects, every Id member);
    // references inside the copy follow, references outside it are kept (`dropOutside` false: a
    // duplicate in the same document) or cleared with a note (a paste from another document)
    void remapIds(Json& layer, titleart::TitleIdGenerator& ids, bool dropOutside, const std::set<std::string>& documentLayers,
                  std::vector<std::string>& notes) {
        const titleart::TitleArtSchema& schema = titleart::TitleArtSchema::get();
        const titleart::SchemaKind& layerKind = *schema.kind("layer");
        std::map<std::string, std::string> renamed;
        // ids first (a layer's own id is an Id member of the layer kind)
        std::function<void(Json&)> renew = [&](Json& l) {
            titleart::forEachMember(l, layerKind, [&](const titleart::SchemaMember* m, const titleart::ParamDef*, Json& value, const std::string&) {
                if (m && m->type == titleart::ValueType::Id) {
                    const std::string fresh = ids.next();
                    if (value.is_string() && !value.get<std::string>().empty()) renamed[value.get<std::string>()] = fresh;
                    value = fresh;
                }
                return true;
            });
            if (!titleart::isTitleArtId(str(l, "id"))) l["id"] = ids.next();
        };
        renew(layer);
        // then every reference: layer refs, part keys naming rims
        titleart::forEachMember(layer, layerKind, [&](const titleart::SchemaMember* m, const titleart::ParamDef* p, Json& value, const std::string& path) {
            const bool ref = (m && m->type == titleart::ValueType::LayerRef) || (p && p->kind == titleart::ParamKind::Layer);
            if (ref && value.is_string() && !value.get<std::string>().empty()) {
                const std::string old = value.get<std::string>();
                auto it = renamed.find(old);
                if (it != renamed.end()) {
                    value = it->second;
                } else if (dropOutside || !documentLayers.count(old)) {
                    notes.push_back(path + ": named layer '" + old + "' outside the copy: cleared");
                    value = "";
                }
            }
            if (m && m->type == titleart::ValueType::ObjectArrayMap && value.is_object()) {
                Json out = Json::object();
                for (auto it = value.begin(); it != value.end(); ++it) {
                    std::string key = it.key();
                    if (key.rfind("rim:", 0) == 0) {
                        auto r = renamed.find(key.substr(4));
                        if (r != renamed.end()) key = "rim:" + r->second;
                    }
                    out[key] = it.value();
                }
                value = std::move(out);
            }
            return true;
        });
    }

    // Folds a group's transform into a child's (ungroup): child' = group o child, flips about the
    // position before rotation; rotations are clockwise on screen in a y-up space
    void foldTransform(const Json& group, Json& child) {
        const titleart::TitleTransform g = titleart::decodeTransform(group.value("transform", Json::object()));
        const titleart::TitleTransform c = titleart::decodeTransform(child.value("transform", Json::object()));
        // the child's position through the group's flip, scale and (clockwise) rotation
        double x = c.position.x * (g.flipX ? -1 : 1) * g.scale;
        double y = c.position.y * (g.flipY ? -1 : 1) * g.scale;
        const double r = -g.rotation * PI / 180.0;
        const double rx = x * std::cos(r) - y * std::sin(r);
        const double ry = x * std::sin(r) + y * std::cos(r);
        Json& t = child["transform"];
        t["position"] = Json::array({g.position.x + rx, g.position.y + ry});
        // a single flip of the group mirrors the child's turn
        const bool mirrored = g.flipX != g.flipY;
        t["rotation"] = g.rotation + (mirrored ? -c.rotation : c.rotation);
        t["scale"] = std::clamp(g.scale * c.scale, 0.01, 100.0);
        t["flip_x"] = g.flipX != c.flipX;
        t["flip_y"] = g.flipY != c.flipY;
    }

    // a part key of an op (`face`, `rim#<n>`, `rim:<id>`) as stored
    bool partKeyOf(const Json& layer, const std::string& part, std::string& key, std::string& error, std::vector<std::string>& valid) {
        const Json& rims = layer.value("style", Json::object()).value("rims", Json::array());
        valid = {"face"};
        for (size_t i = 0; i < rims.size(); i++) {
            valid.push_back("rim#" + std::to_string(i));
            valid.push_back("rim:" + str(rims[i], "id"));
        }
        if (part == "face") {
            key = part;
            return true;
        }
        if (part.rfind("rim#", 0) == 0) {
            const std::string digits = part.substr(4);
            if (!digits.empty() && digits.size() < 6 && std::all_of(digits.begin(), digits.end(), ::isdigit)) {
                const size_t n = std::stoul(digits);
                if (n < rims.size()) {
                    key = "rim:" + str(rims[n], "id");
                    return true;
                }
            }
        } else if (part.rfind("rim:", 0) == 0) {
            for (const Json& r : rims) {
                if ("rim:" + str(r, "id") == part) {
                    key = part;
                    return true;
                }
            }
        } else if (part == "bevel" || part == "side" || part.rfind("band", 0) == 0) {
            error = "the 3D parts arrive with S86";
            return false;
        }
        error = "'" + part + "' is not a part of this layer";
        return false;
    }

    // every material of a document by (layer id, path), in layer then part order
    struct MaterialAt {
        std::string layer;
        std::string path;
        Json* value;
    };

    std::vector<MaterialAt> materialsOf(TitleArtDoc& doc) {
        std::vector<MaterialAt> out;
        const titleart::SchemaKind& layerKind = *titleart::TitleArtSchema::get().kind("layer");
        std::function<void(Json&)> walk = [&](Json& list) {
            if (!list.is_array()) return;
            for (Json& layer : list) {
                const std::string id = str(layer, "id");
                titleart::forEachMember(layer, layerKind, [&](const titleart::SchemaMember* m, const titleart::ParamDef* p, Json& value, const std::string& path) {
                    if (m && m->key == "children") return false;
                    const bool material = (m && m->kind == "material" && m->type == titleart::ValueType::Object) ||
                                          (p && p->kind == titleart::ParamKind::Material);
                    if (material && value.is_object()) out.push_back({id, path, &value});
                    return true;
                });
                if (titleart::layerTypeOf(layer) == LayerType::Group) walk(layer["children"]);
            }
        };
        walk(doc.layers());
        return out;
    }

    // the exposed type a field fits ("text" for strings, "color" for colours...)
    bool exposedTypeFits(const std::string& type, const titleart::SchemaMember* m, const titleart::ParamDef* p) {
        using VT = titleart::ValueType;
        using PK = titleart::ParamKind;
        if (type == "text") return (m && m->type == VT::String) || (p && p->kind == PK::String);
        if (type == "color") return (m && m->type == VT::Color) || (p && p->kind == PK::Color);
        if (type == "number") return (m && (m->type == VT::Number || m->type == VT::Integer)) || (p && (p->kind == PK::Number || p->kind == PK::Integer));
        if (type == "bool") return (m && m->type == VT::Bool) || (p && p->kind == PK::Bool);
        if (type == "font" || type == "image") return (m && m->type == VT::AssetRef) || (p && p->kind == PK::Asset);
        return false;
    }

    // ---- the ops ----

    class Batch {
    public:
        Batch(TitleOpContext& ctx) : ctx(ctx) {}

        bool run(const std::string& name, const Json& op) {
            if (name == "set") return set(op);
            if (name == "set_text") return setText(op);
            if (name == "add_layer") return addLayer(op);
            if (name == "remove_layer") return removeLayer(op);
            if (name == "duplicate_layer") return duplicateLayer(op);
            if (name == "move_layer") return moveLayer(op);
            if (name == "group") return group(op);
            if (name == "ungroup") return ungroup(op);
            if (name == "add_effect") return addEffect(op);
            if (name == "remove_effect") return removeEffect(op, false);
            if (name == "move_effect") return removeEffect(op, true);
            if (name == "add_rim") return addRim(op);
            if (name == "remove_rim") return removeRim(op, false);
            if (name == "move_rim") return removeRim(op, true);
            if (name == "add_mask") return addMask(op);
            if (name == "remove_mask") return removeMask(op);
            if (name == "expose") return expose(op);
            if (name == "unexpose") return unexpose(op);
            if (name == "link_material") return linkMaterial(op, true);
            if (name == "unlink_material") return linkMaterial(op, false);
            return ctx.fail("bad_op", "unknown op '" + name + "'");
        }

        std::string undoName;

    private:
        TitleOpContext& ctx;
        titleart::TitleArtDoc& doc() { return ctx.doc; }

        std::string layerName(const Json& layer) { return str(layer, "name"); }

        static const std::vector<std::string>& layerTypes() {
            const titleart::TitleArtSchema& schema = titleart::TitleArtSchema::get();
            return schema.member(*schema.kind("layer"), "type")->options;
        }

        // the layer an effect op works on: `layer`, else the only top-level layer
        Json* effectLayer(const Json& op) {
            if (op.contains("layer")) return ctx.layer(op);
            if (doc().layers().size() == 1) return &doc().layers()[0];
            std::vector<std::string> paths;
            titleart::forEachLayer(doc().root, [&](const Json& l, const Json*, int) {
                paths.push_back(ctx.layerPath(str(l, "id")));
                return true;
            });
            ctx.fail("bad_layer", "`layer` is needed: the document has " + std::to_string(paths.size()) + " layers", "layer", paths);
            return nullptr;
        }

        bool set(const Json& op) {
            auto values = op.find("values");
            if (values == op.end() || !values->is_object() || values->empty()) {
                return ctx.fail("bad_op", "`set` takes `values`: {\"<field path>\": value, ...}", "values");
            }
            Json* layer = op.contains("layer") ? ctx.layer(op) : nullptr;
            if (op.contains("layer") && !layer) return false;
            size_t before = ctx.result.changes.size();
            for (auto it = values->begin(); it != values->end(); ++it) {
                if (!ctx.set(layer, it.key(), it.value())) return false;
            }
            if (values->size() == 1) {
                undoName = "Set " + ctx.lastSetLabel;
            } else {
                undoName = "Edit Title Art (" + std::to_string(ctx.result.changes.size() - before) + " changes)";
            }
            return true;
        }

        bool setText(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            if (titleart::layerTypeOf(*layer) != LayerType::Text) return ctx.fail("bad_layer", "'" + layerName(*layer) + "' is not a Text layer", "layer");
            std::string text;
            if (!ctx.requireString(op, "text", text)) return false;
            if (!ctx.set(layer, "text", text)) return false;
            if (op.contains("font") && !ctx.set(layer, "font", op["font"])) return false;
            if (!op.value("keep_chars", false)) {
                size_t count = 0;
                for (unsigned char c : text) count += (c & 0xC0) != 0x80;
                Json& chars = (*layer)["chars"];
                std::vector<std::string> drop;
                for (auto it = chars.begin(); it != chars.end(); ++it) {
                    if (std::stoul(it.key()) >= count) drop.push_back(it.key());
                }
                for (const std::string& key : drop) {
                    TitleOpChange change;
                    change.layer = str(*layer, "id");
                    change.path = "chars." + key;
                    change.from = chars[key];
                    change.to = nullptr;
                    change.note = "past the new text: dropped";
                    chars.erase(key);
                    ctx.change(std::move(change));
                }
            }
            undoName = "Set Text";
            return true;
        }

        // where an op puts a layer: `parent` (a group), `before` / `after` (a sibling), else the
        // top of the parent
        bool placement(const Json& op, Json*& list, size_t& index, Json*& parentGroup, const std::string& moving) {
            parentGroup = nullptr;
            list = &doc().layers();
            if (op.contains("parent") && !op["parent"].is_null() && !(op["parent"].is_string() && op["parent"].get<std::string>().empty())) {
                Json* parent = ctx.layer(op, "parent");
                if (!parent) return false;
                if (titleart::layerTypeOf(*parent) != LayerType::Group) return ctx.fail("bad_layer", "the parent '" + layerName(*parent) + "' is not a Group", "parent");
                if (!moving.empty()) {
                    Json* self = titleart::findLayer(doc().root, moving);
                    if (self && contains(*self, str(*parent, "id"))) return ctx.fail("bad_layer", "a layer cannot move into itself", "parent");
                }
                parentGroup = parent;
                list = &arrayAt(*parent, "children");
            }
            index = list->size();
            for (const char* side : {"before", "after"}) {
                if (!op.contains(side)) continue;
                Json* sibling = ctx.layer(op, side);
                if (!sibling) return false;
                Slot slot;
                slotOf(doc(), str(*sibling, "id"), slot);
                if (op.contains("parent") && slot.list != list) {
                    return ctx.fail("bad_layer", std::string("`") + side + "` is not a child of `parent`", side);
                }
                list = slot.list;
                parentGroup = slot.parent;
                // `before` = below it in draw order (lists are bottom first)
                index = std::string(side) == "before" ? slot.index : slot.index + 1;
            }
            return true;
        }

        bool checkLimits(const Json& layer, Json* parentGroup) {
            size_t adding = 0;
            titleart::forEachLayer(Json{{"layers", Json::array({layer})}}, [&](const Json&, const Json*, int) {
                adding++;
                return true;
            });
            if (titleart::countLayers(doc().root) + adding > titleart::TITLE_MAX_LAYERS) {
                return ctx.fail("limit", "a document holds at most " + std::to_string(titleart::TITLE_MAX_LAYERS) + " layers");
            }
            const int depth = parentGroup ? depthOf(doc(), str(*parentGroup, "id")) + 1 : 0;
            if (depth + subtreeHeight(layer) >= titleart::TITLE_MAX_GROUP_DEPTH) {
                return ctx.fail("limit", "groups nest at most " + std::to_string(titleart::TITLE_MAX_GROUP_DEPTH) + " deep");
            }
            return true;
        }

        void normalizeLayer(Json& layer, const std::string& path) {
            std::vector<titleart::TitleWarning> warnings;
            titleart::NormalizeContext context;
            context.warnings = &warnings;
            context.ids = &ctx.ids;
            titleart::normalizeTitleArtObject(layer, "layer", path, context);
            for (const titleart::TitleWarning& w : warnings) ctx.note("normalized", w.message, w.path);
        }

        std::set<std::string> documentLayerIds() {
            std::set<std::string> ids;
            titleart::forEachLayer(doc().root, [&](const Json& l, const Json*, int) {
                ids.insert(str(l, "id"));
                return true;
            });
            return ids;
        }

        bool addLayer(const Json& op) {
            for (const char* s83 : {"preset", "place", "group"}) {
                if (op.contains(s83)) return ctx.fail("not_available", std::string("add_layer `") + s83 + "` arrives with S83", s83);
            }
            if (op.contains("look")) return ctx.fail("not_available", "add_layer `look` arrives with S84", "look");
            Json layer;
            if (op.contains("from")) {
                if (!op["from"].is_object()) return ctx.fail("bad_value", "`from` takes a layer object", "from");
                layer = op["from"];
                const std::string type = str(layer, "type");
                if (op.contains("type") && str(op, "type") != type) return ctx.fail("bad_value", "`type` differs from `from`'s type", "type");
                if (titleart::layerTypeFromName(type) == LayerType::Unknown) {
                    return ctx.fail("bad_value", "`from` has no layer type this build knows", "from.type", layerTypes());
                }
                std::vector<std::string> notes;
                remapIds(layer, ctx.ids, true, documentLayerIds(), notes);
                for (const std::string& n : notes) ctx.note("reference_dropped", n, "from");
            } else {
                std::string type;
                if (!ctx.requireString(op, "type", type)) return false;
                const std::vector<std::string>& types = layerTypes();
                if (titleart::layerTypeFromName(type) == LayerType::Unknown) {
                    return ctx.fail("bad_value", "'" + type + "' is not a layer type", "type", types, titleart::nearestName(type, types));
                }
                layer = Json{{"type", type}};
                if (type == "generator") {
                    std::string gen;
                    if (!ctx.requireString(op, "generator", gen)) return false;
                    if (!titleart::findGenerator(gen)) {
                        std::vector<std::string> valid;
                        for (const titleart::GeneratorDef* d : titleart::TitleEffectRegistry::get().generators()) valid.push_back(d->type);
                        return ctx.fail("bad_value", "'" + gen + "' is not a generator type", "generator", valid, titleart::nearestName(gen, valid));
                    }
                    layer["generator"] = Json{{"type", gen}};
                }
                if (type == "adjustment") {
                    const std::string adj = op.contains("adjustment") ? str(op, "adjustment") : std::string("hue_saturation");
                    const titleart::SchemaKind* kind = titleart::TitleArtSchema::get().kind("adjustment");
                    if (!kind->variants.count(adj)) {
                        std::vector<std::string> valid;
                        for (const auto& v : kind->variants) valid.push_back(v.first);
                        return ctx.fail("bad_value", "'" + adj + "' is not an adjustment", "adjustment", valid, titleart::nearestName(adj, valid));
                    }
                    layer["adjustment"] = Json{{"type", adj}};
                }
                layer["id"] = ctx.newId();
            }
            if (op.contains("name")) {
                std::string name;
                if (!ctx.requireString(op, "name", name)) return false;
                layer["name"] = name;
            }
            Json* list = nullptr;
            Json* parent = nullptr;
            size_t index = 0;
            if (!placement(op, list, index, parent, {})) return false;
            if (!checkLimits(layer, parent)) return false;
            normalizeLayer(layer, "layer");
            const std::string id = str(layer, "id");
            list->insert(list->begin() + (long)index, std::move(layer));
            ctx.recordNewId("layer", id);
            Json* added = titleart::findLayer(doc().root, id);
            TitleOpChange change;
            change.layer = id;
            change.note = "added " + std::string(titleart::layerTypeLabel(titleart::layerTypeOf(*added))) + " layer '" + layerName(*added) + "'";
            ctx.change(std::move(change));
            if (op.contains("values")) {
                if (!op["values"].is_object()) return ctx.fail("bad_value", "`values` takes {\"<field path>\": value}", "values");
                for (auto it = op["values"].begin(); it != op["values"].end(); ++it) {
                    if (!ctx.set(titleart::findLayer(doc().root, id), it.key(), it.value())) return false;
                }
            }
            undoName = "Add " + std::string(titleart::layerTypeLabel(titleart::layerTypeOf(*titleart::findLayer(doc().root, id)))) + " Layer";
            return true;
        }

        // masks, exposed targets and `follow` that name a removed layer
        void clearReferences(const std::set<std::string>& removed) {
            titleart::forEachLayer(doc().root, [&](const Json& constLayer, const Json*, int) {
                Json& layer = const_cast<Json&>(constLayer);
                const std::string id = str(layer, "id");
                Json& masks = layer["masks"];
                if (masks.is_array()) {
                    for (size_t i = masks.size(); i-- > 0;) {
                        if (str(masks[i], "type") == "layer" && removed.count(str(masks[i], "layer"))) {
                            TitleOpChange change;
                            change.layer = id;
                            change.path = "masks[" + std::to_string(i) + "]";
                            change.from = masks[i];
                            change.to = nullptr;
                            change.note = "the mask named a removed layer: removed";
                            masks.erase(masks.begin() + (long)i);
                            ctx.change(std::move(change));
                        }
                    }
                }
                if (layer.contains("follow") && removed.count(str(layer, "follow"))) {
                    TitleOpChange change;
                    change.layer = id;
                    change.path = "follow";
                    change.from = layer["follow"];
                    change.to = "";
                    change.note = "followed a removed layer: cleared";
                    layer["follow"] = "";
                    ctx.change(std::move(change));
                }
                return true;
            });
            Json& exposed = arrayAt(doc().root, "exposed");
            for (size_t i = 0; i < exposed.size(); i++) {
                Json& targets = arrayAt(exposed[i], "targets");
                for (size_t t = targets.size(); t-- > 0;) {
                    if (!removed.count(str(targets[t], "layer"))) continue;
                    TitleOpChange change;
                    change.path = "exposed[" + std::to_string(i) + "].targets[" + std::to_string(t) + "]";
                    change.from = targets[t];
                    change.to = nullptr;
                    change.note = "exposed parameter '" + str(exposed[i], "name") + "' targeted a removed layer: target removed";
                    targets.erase(targets.begin() + (long)t);
                    ctx.change(std::move(change));
                }
            }
        }

        bool removeLayer(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            const std::string name = layerName(*layer);
            std::set<std::string> removed;
            subtreeIds(*layer, removed);
            Slot slot;
            slotOf(doc(), str(*layer, "id"), slot);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.from = *layer;
            change.to = nullptr;
            change.note = "removed layer '" + name + "'";
            slot.list->erase(slot.list->begin() + (long)slot.index);
            ctx.change(std::move(change));
            clearReferences(removed);
            undoName = "Remove Layer " + name;
            return true;
        }

        bool duplicateLayer(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            Json copy = *layer;
            std::vector<std::string> notes;
            remapIds(copy, ctx.ids, false, documentLayerIds(), notes);
            if (op.contains("name")) {
                std::string name;
                if (!ctx.requireString(op, "name", name)) return false;
                copy["name"] = name;
            }
            Slot slot;
            slotOf(doc(), str(*layer, "id"), slot);
            if (!checkLimits(copy, slot.parent)) return false;
            const std::string name = layerName(*layer);
            const std::string id = str(copy, "id");
            slot.list->insert(slot.list->begin() + (long)slot.index + 1, std::move(copy));
            ctx.recordNewId("layer", id);
            TitleOpChange change;
            change.layer = id;
            change.note = "duplicated '" + name + "'";
            ctx.change(std::move(change));
            undoName = "Duplicate Layer " + name;
            return true;
        }

        bool moveLayer(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            const std::string id = str(*layer, "id");
            const std::string name = layerName(*layer);
            Json* list = nullptr;
            Json* parent = nullptr;
            size_t index = 0;
            // resolve the place first (references are stable until the layer moves)
            if (!placement(op, list, index, parent, id)) return false;
            for (const char* side : {"before", "after"}) {
                if (!op.contains(side)) continue;
                const Json* sibling = ctx.resolveLayer(str(op, side), side);
                if (sibling && str(*sibling, "id") == id) return ctx.fail("bad_layer", std::string("a layer cannot move ") + side + " itself", side);
            }
            Json moving = *layer;
            const int height = subtreeHeight(moving);
            const int depth = parent ? depthOf(doc(), str(*parent, "id")) + 1 : 0;
            if (depth + height >= titleart::TITLE_MAX_GROUP_DEPTH) {
                return ctx.fail("limit", "groups nest at most " + std::to_string(titleart::TITLE_MAX_GROUP_DEPTH) + " deep");
            }
            // remove, then insert where the target slot is now
            const std::string parentId = parent ? str(*parent, "id") : std::string();
            std::string anchor;
            bool anchorAfter = false;
            if (index < list->size()) anchor = str((*list)[index], "id");
            else if (!list->empty()) {
                anchor = str(list->back(), "id");
                anchorAfter = true;
            }
            Slot from;
            slotOf(doc(), id, from);
            from.list->erase(from.list->begin() + (long)from.index);
            Json* target = parentId.empty() ? &doc().layers() : &arrayAt(*titleart::findLayer(doc().root, parentId), "children");
            size_t at = target->size();
            if (!anchor.empty() && anchor != id) {
                for (size_t i = 0; i < target->size(); i++) {
                    if (str((*target)[i], "id") == anchor) at = anchorAfter ? i + 1 : i;
                }
            } else if (anchor == id) {
                at = std::min(from.index, target->size());
            }
            target->insert(target->begin() + (long)at, std::move(moving));
            TitleOpChange change;
            change.layer = id;
            change.note = "moved '" + name + "'";
            ctx.change(std::move(change));
            undoName = "Move Layer " + name;
            return true;
        }

        bool group(const Json& op) {
            auto refs = op.find("layers");
            if (refs == op.end() || !refs->is_array() || refs->empty()) return ctx.fail("bad_op", "`group` takes `layers`: [layer refs]", "layers");
            std::vector<std::string> ids;
            Json* list = nullptr;
            for (const Json& ref : *refs) {
                if (!ref.is_string()) return ctx.fail("bad_value", "`layers` takes layer refs", "layers");
                Json* layer = ctx.resolveLayer(ref.get<std::string>(), "layers");
                if (!layer) return false;
                Slot slot;
                slotOf(doc(), str(*layer, "id"), slot);
                if (list && slot.list != list) return ctx.fail("bad_layer", "`group` takes siblings only", "layers");
                list = slot.list;
                if (std::find(ids.begin(), ids.end(), str(*layer, "id")) == ids.end()) ids.push_back(str(*layer, "id"));
            }
            // members in their draw order; the group goes where the topmost was
            std::vector<size_t> indexes;
            for (size_t i = 0; i < list->size(); i++) {
                if (std::find(ids.begin(), ids.end(), str((*list)[i], "id")) != ids.end()) indexes.push_back(i);
            }
            Json groupLayer = Json{{"type", "group"}, {"id", ctx.newId()}, {"name", op.contains("name") ? str(op, "name") : std::string("Group")}};
            Json children = Json::array();
            for (size_t i : indexes) children.push_back((*list)[i]);
            groupLayer["children"] = children;
            const size_t top = indexes.back();
            Json* parent = nullptr;
            Slot slot;
            if (slotOf(doc(), ids.front(), slot)) parent = slot.parent;
            const int depth = parent ? depthOf(doc(), str(*parent, "id")) + 1 : 0;
            if (depth + subtreeHeight(groupLayer) >= titleart::TITLE_MAX_GROUP_DEPTH) {
                return ctx.fail("limit", "groups nest at most " + std::to_string(titleart::TITLE_MAX_GROUP_DEPTH) + " deep");
            }
            normalizeLayer(groupLayer, "group");
            const std::string gid = str(groupLayer, "id");
            (*list)[top] = std::move(groupLayer);
            for (size_t k = indexes.size() - 1; k-- > 0;) list->erase(list->begin() + (long)indexes[k]);
            ctx.recordNewId("layer", gid);
            TitleOpChange change;
            change.layer = gid;
            change.note = "grouped " + std::to_string(indexes.size()) + " layers";
            ctx.change(std::move(change));
            undoName = "Group Layers";
            return true;
        }

        bool ungroup(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            if (titleart::layerTypeOf(*layer) != LayerType::Group) return ctx.fail("bad_layer", "'" + layerName(*layer) + "' is not a Group", "layer");
            const Json groupLayer = *layer;
            const std::string name = layerName(groupLayer);
            Slot slot;
            slotOf(doc(), str(groupLayer, "id"), slot);
            Json children = groupLayer.value("children", Json::array());
            for (Json& child : children) foldTransform(groupLayer, child);
            slot.list->erase(slot.list->begin() + (long)slot.index);
            slot.list->insert(slot.list->begin() + (long)slot.index, children.begin(), children.end());
            TitleOpChange change;
            change.layer = str(groupLayer, "id");
            change.from = groupLayer;
            change.to = nullptr;
            change.note = "ungrouped '" + name + "': its transform folded into its children";
            const bool styled = !groupLayer.value("effects", Json::array()).empty() || !groupLayer.value("masks", Json::array()).empty() ||
                                groupLayer.value("opacity", 1.0) != 1.0 || str(groupLayer, "blend") != "normal";
            if (styled) change.note += "; its effects, masks, opacity and blend were dropped";
            ctx.change(std::move(change));
            std::set<std::string> removed = {str(groupLayer, "id")};
            clearReferences(removed);
            undoName = "Ungroup " + name;
            return true;
        }

        // the effect stack an op names: the layer's or one part's
        Json* effectStack(const Json& op, Json*& layer, std::string& partKey, const titleart::EffectDef* def) {
            layer = effectLayer(op);
            if (!layer) return nullptr;
            if (!op.contains("part")) return &arrayAt(*layer, "effects");
            if (!titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer))) {
                ctx.fail("bad_layer", "'" + layerName(*layer) + "' has no parts", "part");
                return nullptr;
            }
            std::string error;
            std::vector<std::string> valid;
            if (!partKeyOf(*layer, str(op, "part"), partKey, error, valid)) {
                ctx.fail(error.find("S86") != std::string::npos ? "not_available" : "bad_value", error, "part", valid,
                         titleart::nearestName(str(op, "part"), valid));
                return nullptr;
            }
            if (def && !def->onParts) {
                ctx.fail("bad_value", std::string(def->label) + " works on whole layers, not on parts", "part");
                return nullptr;
            }
            Json& parts = (*layer)["part_effects"];
            if (!parts.is_object()) parts = Json::object();
            return &arrayAt(parts, partKey.c_str());
        }

        bool addEffect(const Json& op) {
            std::string type;
            if (!ctx.requireString(op, "type", type)) return false;
            const titleart::EffectDef* def = titleart::findEffect(type);
            if (!def) {
                std::vector<std::string> valid;
                for (const titleart::EffectDef* d : titleart::TitleEffectRegistry::get().effects()) valid.push_back(d->type);
                return ctx.fail("bad_value", "'" + type + "' is not an effect type" + (valid.empty() ? std::string(": none are registered") : std::string()),
                                "type", valid, titleart::nearestName(type, valid));
            }
            Json* layer = nullptr;
            std::string part;
            Json* stack = effectStack(op, layer, part, def);
            if (!stack) return false;
            if (stack->size() >= titleart::TITLE_MAX_EFFECTS) return ctx.fail("limit", "a stack holds at most " + std::to_string(titleart::TITLE_MAX_EFFECTS) + " effects");
            Json params = titleart::defaultParams(def->params);
            // S83 R45: a preset's params over the defaults, the op's own over both
            if (op.contains("preset")) {
                std::string preset, error;
                std::vector<std::string> valid;
                if (!ctx.requireString(op, "preset", preset)) return false;
                if (!applyTitleFxPreset(type, preset, params, error, valid)) {
                    return ctx.fail("bad_value", error, "preset", valid, titleart::nearestName(preset, valid));
                }
            }
            if (op.contains("params")) {
                if (!op["params"].is_object()) return ctx.fail("bad_value", "`params` takes {key: value}", "params");
                for (auto it = op["params"].begin(); it != op["params"].end(); ++it) {
                    const titleart::ParamDef* p = titleart::findParam(def->params, it.key());
                    if (!p) {
                        std::vector<std::string> keys;
                        for (const titleart::ParamDef& d : def->params) keys.push_back(d.key);
                        return ctx.fail("bad_path", "'" + it.key() + "' is not a parameter of " + def->label, "params." + it.key(), keys,
                                        titleart::nearestName(it.key(), keys));
                    }
                    Json value = it.value();
                    std::vector<titleart::TitleWarning> warnings;
                    titleart::NormalizeContext context;
                    context.warnings = &warnings;
                    if (!titleart::normalizeParamValue(*p, value, "params." + it.key(), context)) {
                        return ctx.fail("bad_value", "'" + it.key() + "' takes a " + titleart::paramKindName(p->kind), "params." + it.key());
                    }
                    params[it.key()] = value;
                    for (const titleart::TitleWarning& w : warnings) {
                        TitleOpChange change;
                        change.layer = str(*layer, "id");
                        change.path = "params." + it.key();
                        change.from = it.value();
                        change.to = value;
                        change.clamped = true;
                        change.clampMessage = w.message;
                        ctx.change(std::move(change));
                    }
                }
            }
            size_t index = stack->size();
            if (op.contains("index")) {
                if (!op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 || (size_t)op["index"].get<int64_t>() > stack->size()) {
                    return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(stack->size()), "index");
                }
                index = (size_t)op["index"].get<int64_t>();
            }
            Json entry = Json{{"id", ctx.newId()}, {"type", type}, {"enabled", true}, {"params", params}};
            const std::string id = str(entry, "id");
            stack->insert(stack->begin() + (long)index, std::move(entry));
            ctx.recordNewId("effect", id);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = (part.empty() ? std::string("effects") : "part_effects." + part) + "[" + std::to_string(index) + "]";
            change.to = (*stack)[index];
            change.note = std::string("added ") + def->label;
            ctx.change(std::move(change));
            undoName = std::string("Add Effect ") + def->label;
            return true;
        }

        // remove_effect and move_effect
        bool removeEffect(const Json& op, bool move) {
            std::string ref;
            if (!ctx.requireString(op, "effect", ref)) return false;
            Json* layer = nullptr;
            std::string part;
            Json* stack = effectStack(op, layer, part, nullptr);
            if (!stack) return false;
            long index = -1;
            std::vector<std::string> valid;
            std::map<std::string, int> counts;
            for (size_t i = 0; i < stack->size(); i++) {
                const std::string type = str((*stack)[i], "type");
                const int n = counts[type]++;
                const std::string name = "fx:" + type + (n ? "#" + std::to_string(n) : "");
                valid.push_back(name);
                if (ref == name || ref == "id:" + str((*stack)[i], "id") || (n == 0 && ref == "fx:" + type + "#0")) index = (long)i;
            }
            if (index < 0) return ctx.fail("bad_path", "no effect '" + ref + "' here", "effect", valid, titleart::nearestName(ref, valid));
            const std::string type = str((*stack)[(size_t)index], "type");
            const titleart::EffectDef* def = titleart::findEffect(type);
            const std::string label = def ? def->label : titleCase(type);
            Json entry = (*stack)[(size_t)index];
            const std::string where = part.empty() ? std::string("effects") : "part_effects." + part;
            if (move) {
                if (!op.contains("index") || !op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 ||
                    (size_t)op["index"].get<int64_t>() >= stack->size()) {
                    return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(stack->size() - 1), "index");
                }
                const size_t to = (size_t)op["index"].get<int64_t>();
                stack->erase(stack->begin() + index);
                stack->insert(stack->begin() + (long)to, entry);
                TitleOpChange change;
                change.layer = str(*layer, "id");
                change.path = where + "[" + std::to_string(to) + "]";
                change.note = "moved " + label + " from " + std::to_string(index) + " to " + std::to_string(to);
                ctx.change(std::move(change));
                undoName = "Move Effect " + label;
                return true;
            }
            stack->erase(stack->begin() + index);
            if (!part.empty() && stack->empty()) (*layer)["part_effects"].erase(part);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = where + "[" + std::to_string(index) + "]";
            change.from = entry;
            change.to = nullptr;
            change.note = "removed " + label;
            ctx.change(std::move(change));
            undoName = "Remove Effect " + label;
            return true;
        }

        Json* styledLayer(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return nullptr;
            if (!titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer))) {
                ctx.fail("bad_layer", "'" + layerName(*layer) + "' has no rims (Text, Shape, Image and Vector layers do)", "layer");
                return nullptr;
            }
            return layer;
        }

        long rimIndex(Json& rims, const std::string& ref) {
            for (size_t i = 0; i < rims.size(); i++) {
                if (ref == "rim#" + std::to_string(i) || ref == "rim:" + str(rims[i], "id") || ref == "id:" + str(rims[i], "id")) return (long)i;
            }
            return -1;
        }

        bool addRim(const Json& op) {
            Json* layer = styledLayer(op);
            if (!layer) return false;
            Json& rims = arrayAt((*layer)["style"], "rims");
            if (rims.size() >= titleart::TITLE_MAX_RIMS) return ctx.fail("limit", "a layer has at most " + std::to_string(titleart::TITLE_MAX_RIMS) + " rims");
            size_t index = rims.size();
            if (op.contains("index")) {
                if (!op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 || (size_t)op["index"].get<int64_t>() > rims.size()) {
                    return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(rims.size()) + " (0 = innermost)", "index");
                }
                index = (size_t)op["index"].get<int64_t>();
            }
            Json rim = Json{{"id", ctx.newId()}};
            titleart::NormalizeContext context;
            titleart::normalizeTitleArtObject(rim, "rim", "rim", context);
            const std::string id = str(rim, "id");
            rims.insert(rims.begin() + (long)index, std::move(rim));
            ctx.recordNewId("rim", id);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = "style.rims[" + std::to_string(index) + "]";
            change.note = "added a rim";
            ctx.change(std::move(change));
            if (op.contains("values")) {
                if (!op["values"].is_object()) return ctx.fail("bad_value", "`values` takes {\"<rim field>\": value}", "values");
                for (auto it = op["values"].begin(); it != op["values"].end(); ++it) {
                    if (!ctx.set(layer, "style.rims[" + std::to_string(index) + "]." + it.key(), it.value())) return false;
                }
            }
            undoName = "Add Rim";
            return true;
        }

        bool removeRim(const Json& op, bool move) {
            Json* layer = styledLayer(op);
            if (!layer) return false;
            std::string ref;
            if (!ctx.requireString(op, "rim", ref)) return false;
            Json& rims = arrayAt((*layer)["style"], "rims");
            const long index = rimIndex(rims, ref);
            if (index < 0) {
                std::vector<std::string> valid;
                for (size_t i = 0; i < rims.size(); i++) {
                    valid.push_back("rim#" + std::to_string(i));
                    valid.push_back("rim:" + str(rims[i], "id"));
                }
                return ctx.fail("bad_path", "no rim '" + ref + "'", "rim", valid, titleart::nearestName(ref, valid));
            }
            Json rim = rims[(size_t)index];
            if (move) {
                if (!op.contains("index") || !op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 ||
                    (size_t)op["index"].get<int64_t>() >= rims.size()) {
                    return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(rims.size() - 1) + " (0 = innermost)", "index");
                }
                const size_t to = (size_t)op["index"].get<int64_t>();
                rims.erase(rims.begin() + index);
                rims.insert(rims.begin() + (long)to, rim);
                TitleOpChange change;
                change.layer = str(*layer, "id");
                change.path = "style.rims[" + std::to_string(to) + "]";
                change.note = "moved rim " + std::to_string(index) + " to " + std::to_string(to);
                ctx.change(std::move(change));
                undoName = "Move Rim";
                return true;
            }
            rims.erase(rims.begin() + index);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = "style.rims[" + std::to_string(index) + "]";
            change.from = rim;
            change.to = nullptr;
            change.note = "removed a rim";
            ctx.change(std::move(change));
            const std::string key = "rim:" + str(rim, "id");
            if (layer->contains("part_effects") && (*layer)["part_effects"].contains(key)) {
                TitleOpChange parts;
                parts.layer = str(*layer, "id");
                parts.path = "part_effects." + key;
                parts.from = (*layer)["part_effects"][key];
                parts.to = nullptr;
                parts.note = "the removed rim's part effects: removed";
                (*layer)["part_effects"].erase(key);
                ctx.change(std::move(parts));
            }
            undoName = "Remove Rim";
            return true;
        }

        bool addMask(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            std::string type;
            if (!ctx.requireString(op, "type", type)) return false;
            const titleart::SchemaKind* kind = titleart::TitleArtSchema::get().kind("mask");
            if (!kind->variants.count(type)) {
                std::vector<std::string> valid;
                for (const auto& v : kind->variants) valid.push_back(v.first);
                return ctx.fail("bad_value", "'" + type + "' is not a mask type", "type", valid, titleart::nearestName(type, valid));
            }
            Json& masks = arrayAt(*layer, "masks");
            if (masks.size() >= titleart::TITLE_MAX_MASKS) return ctx.fail("limit", "a layer has at most " + std::to_string(titleart::TITLE_MAX_MASKS) + " masks");
            Json mask = Json{{"id", ctx.newId()}, {"type", type}};
            titleart::NormalizeContext context;
            titleart::normalizeTitleArtObject(mask, "mask", "mask", context);
            const std::string id = str(mask, "id");
            masks.push_back(std::move(mask));
            const size_t index = masks.size() - 1;
            ctx.recordNewId("mask", id);
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = "masks[" + std::to_string(index) + "]";
            change.note = "added a " + type + " mask";
            ctx.change(std::move(change));
            if (op.contains("values")) {
                if (!op["values"].is_object()) return ctx.fail("bad_value", "`values` takes {\"<mask field>\": value}", "values");
                for (auto it = op["values"].begin(); it != op["values"].end(); ++it) {
                    Json value = it.value();
                    // a layer mask names its layer by ref
                    if (type == "layer" && it.key() == "layer" && value.is_string()) {
                        Json* source = ctx.resolveLayer(value.get<std::string>(), "values.layer");
                        if (!source) return false;
                        value = str(*source, "id");
                    }
                    if (!ctx.set(layer, "masks[" + std::to_string(index) + "]." + it.key(), value)) return false;
                }
            }
            undoName = "Add " + titleCase(type) + " Mask";
            return true;
        }

        bool removeMask(const Json& op) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            std::string ref;
            if (!ctx.requireString(op, "mask", ref)) return false;
            Json& masks = arrayAt(*layer, "masks");
            long index = -1;
            std::vector<std::string> valid;
            for (size_t i = 0; i < masks.size(); i++) {
                valid.push_back("mask#" + std::to_string(i));
                valid.push_back("mask:" + str(masks[i], "id"));
                if (ref == "mask#" + std::to_string(i) || ref == "mask:" + str(masks[i], "id") || ref == "id:" + str(masks[i], "id")) index = (long)i;
            }
            if (index < 0) return ctx.fail("bad_path", "no mask '" + ref + "'", "mask", valid, titleart::nearestName(ref, valid));
            TitleOpChange change;
            change.layer = str(*layer, "id");
            change.path = "masks[" + std::to_string(index) + "]";
            change.from = masks[(size_t)index];
            change.to = nullptr;
            change.note = "removed a mask";
            masks.erase(masks.begin() + index);
            ctx.change(std::move(change));
            undoName = "Remove Mask";
            return true;
        }

        bool expose(const Json& op) {
            std::string name, type;
            if (!ctx.requireString(op, "name", name) || !ctx.requireString(op, "type", type)) return false;
            const titleart::SchemaKind* kind = titleart::TitleArtSchema::get().kind("exposed");
            const titleart::SchemaMember* typeMember = titleart::TitleArtSchema::get().member(*kind, "type");
            if (std::find(typeMember->options.begin(), typeMember->options.end(), type) == typeMember->options.end()) {
                return ctx.fail("bad_value", "'" + type + "' is not an exposed parameter type", "type", typeMember->options,
                                titleart::nearestName(type, typeMember->options));
            }
            const titleart::SchemaMember* nameMember = titleart::TitleArtSchema::get().member(*kind, "name");
            if (!std::regex_match(name, std::regex(nameMember->pattern))) {
                return ctx.fail("bad_value", "'" + name + "' is not a parameter name: lowercase letters, digits and _, starting with a letter, at most 32", "name");
            }
            Json& exposed = arrayAt(doc().root, "exposed");
            for (const Json& e : exposed) {
                if (str(e, "name") == name) return ctx.fail("bad_value", "'" + name + "' is exposed already: unexpose it first", "name");
            }
            if (exposed.size() >= titleart::TITLE_MAX_EXPOSED) return ctx.fail("limit", "a document exposes at most " + std::to_string(titleart::TITLE_MAX_EXPOSED) + " parameters");
            Json targets = Json::array();
            if (type == "color_scheme") {
                if (op.contains("targets") && !op["targets"].empty()) {
                    return ctx.fail("bad_value", "a color_scheme parameter has no targets: it applies a Color Scheme to the document", "targets");
                }
            } else {
                auto list = op.find("targets");
                if (list == op.end() || !list->is_array() || list->empty()) return ctx.fail("bad_op", "`expose` takes `targets`: [{layer, field}]", "targets");
                for (size_t i = 0; i < list->size(); i++) {
                    const Json& t = (*list)[i];
                    const std::string at = "targets[" + std::to_string(i) + "]";
                    if (!t.is_object() || !t.contains("field") || !t["field"].is_string()) return ctx.fail("bad_value", "a target is {layer, field}", at);
                    Json* layer = t.contains("layer") ? ctx.resolveLayer(str(t, "layer"), at + ".layer") : nullptr;
                    if (t.contains("layer") && !layer) return false;
                    std::string longPath, error;
                    const std::string kindName = layer ? "layer" : "document";
                    Json probe = layer ? *layer : doc().root;
                    const Json* value = titleart::getTitleArtField(probe, kindName, str(t, "field"), &longPath, &error);
                    if (!value) return ctx.fail("bad_path", error, at + ".field");
                    // the member's value type decides which parameter types fit
                    titleart::FieldSetResult same = titleart::setTitleArtField(probe, kindName, longPath, *value, true);
                    if (!exposedTypeFits(type, same.member, same.param)) {
                        return ctx.fail("bad_value", "'" + longPath + "' is not a " + type + " field", at + ".field");
                    }
                    targets.push_back(Json{{"layer", layer ? str(*layer, "id") : std::string()}, {"field", longPath}});
                }
            }
            Json entry = Json{{"name", name}, {"type", type}, {"targets", targets}};
            if (op.contains("label")) entry["label"] = op["label"];
            if (op.contains("min")) entry["min"] = op["min"];
            if (op.contains("max")) entry["max"] = op["max"];
            if (op.contains("options")) entry["options"] = op["options"];
            titleart::NormalizeContext context;
            titleart::normalizeTitleArtObject(entry, "exposed", "exposed", context);
            exposed.push_back(entry);
            TitleOpChange change;
            change.path = "exposed[" + std::to_string(exposed.size() - 1) + "]";
            change.to = entry;
            change.note = "exposed '" + name + "'";
            ctx.change(std::move(change));
            undoName = "Expose " + name;
            return true;
        }

        bool unexpose(const Json& op) {
            std::string name;
            if (!ctx.requireString(op, "name", name)) return false;
            Json& exposed = arrayAt(doc().root, "exposed");
            std::vector<std::string> names;
            for (size_t i = 0; i < exposed.size(); i++) {
                names.push_back(str(exposed[i], "name"));
                if (names.back() != name) continue;
                TitleOpChange change;
                change.path = "exposed[" + std::to_string(i) + "]";
                change.from = exposed[i];
                change.to = nullptr;
                change.note = "unexposed '" + name + "'";
                exposed.erase(exposed.begin() + (long)i);
                ctx.change(std::move(change));
                undoName = "Unexpose " + name;
                return true;
            }
            return ctx.fail("bad_value", "no exposed parameter '" + name + "'", "name", names, titleart::nearestName(name, names));
        }

        bool linkMaterial(const Json& op, bool link) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            std::string path;
            if (!ctx.requireString(op, "material", path)) return false;
            std::string longPath, error;
            const Json* material = titleart::getTitleArtField(*layer, "layer", path, &longPath, &error);
            if (!material) return ctx.fail("bad_path", error, "material");
            Json probe = *layer;
            titleart::FieldSetResult same = titleart::setTitleArtField(probe, "layer", longPath, *material, true);
            const bool isMaterial = (same.member && same.member->kind == "material" && !same.pattern.empty() && same.pattern.back() != '*') ||
                                    (same.param && same.param->kind == titleart::ParamKind::Material);
            if (!isMaterial || !material->is_object()) return ctx.fail("bad_path", "'" + longPath + "' is not a Material", "material");
            Json& links = doc().root["material_links"];
            if (!links.is_object()) links = Json::object();
            if (!link) {
                if (str(*material, "link").empty()) return ctx.fail("bad_value", "'" + longPath + "' is not linked", "material");
                if (!ctx.set(layer, longPath + ".link", "")) return false;
                undoName = "Unlink Material";
                return true;
            }
            std::string key = op.contains("link") ? str(op, "link") : std::string();
            if (!key.empty()) {
                if (!links.contains(key)) {
                    std::vector<std::string> keys;
                    for (auto it = links.begin(); it != links.end(); ++it) keys.push_back(it.key());
                    return ctx.fail("bad_value", "no material link '" + key + "'", "link", keys, titleart::nearestName(key, keys));
                }
                // the new member takes the group's material (linking never restyles the others)
                for (const MaterialAt& m : materialsOf(doc())) {
                    if (str(*m.value, "link") == key) {
                        if (!ctx.set(layer, longPath, *m.value)) return false;
                        break;
                    }
                }
            } else {
                key = ctx.newId();
                const std::string name = op.contains("name") ? str(op, "name") : "Material " + std::to_string(links.size() + 1);
                links[key] = Json{{"name", name}};
                ctx.recordNewId("link", key);
            }
            if (!ctx.set(layer, longPath + ".link", key)) return false;
            undoName = "Link Material";
            return true;
        }
    };

}

// ---- TitleOpChange / TitleOpProblem / TitleOpsResult ----

TitleJson TitleOpChange::toJson() const {
    TitleJson out = TitleJson::object();
    if (!layer.empty()) out["layer"] = layer;
    if (!path.empty()) out["path"] = path;
    if (!from.is_null() || !to.is_null()) {
        out["from"] = from;
        out["to"] = to;
    }
    if (clamped) {
        out["clamped"] = true;
        if (!clampMessage.empty()) out["clamp"] = clampMessage;
    }
    if (px) out["px"] = *px;
    if (!note.empty()) out["note"] = note;
    return out;
}

TitleJson TitleOpProblem::toJson() const {
    TitleJson out = TitleJson::object();
    out["severity"] = severity;
    out["code"] = code;
    if (op >= 0) out["op"] = op;
    if (!path.empty()) out["path"] = path;
    out["message"] = message;
    if (!nearest.empty()) out["nearest"] = nearest;
    if (!validHere.empty()) out["valid_here"] = validHere;
    if (!fix.is_null()) out["fix"] = fix;
    return out;
}

std::string TitleOpsResult::error() const {
    if (ok) return {};
    for (const TitleOpProblem& p : problems) {
        if (p.severity != "error") continue;
        std::string text = "op " + std::to_string(p.op) + (p.path.empty() ? "" : " (" + p.path + ")") + ": " + p.message;
        if (!p.nearest.empty()) text += " (did you mean '" + p.nearest + "'?)";
        return text + ". Nothing was applied.";
    }
    return "Nothing was applied.";
}

TitleJson TitleOpsResult::toJson() const {
    TitleJson out = TitleJson::object();
    out["ok"] = ok;
    out["applied"] = applied;
    if (!ok) out["error"] = error();
    if (!undoName.empty()) out["undo_name"] = undoName;
    TitleJson list = TitleJson::array();
    for (const TitleOpChange& c : changes) list.push_back(c.toJson());
    out["changes"] = list;
    out["new_ids"] = newIds;
    TitleJson problemList = TitleJson::array();
    for (const TitleOpProblem& p : problems) problemList.push_back(p.toJson());
    out["problems"] = problemList;
    return out;
}

// ---- TitleOpContext ----

TitleOpContext::TitleOpContext(TitleArtDoc& doc, titleart::TitleIdGenerator& ids, TitleOpsResult& result)
    : doc(doc), ids(ids), result(result) {
}

bool TitleOpContext::fail(const std::string& code, const std::string& message, const std::string& path,
                          std::vector<std::string> validHere, const std::string& nearest, TitleJson fix) {
    TitleOpProblem p;
    p.code = code;
    p.op = opIndex;
    p.path = path;
    p.message = message;
    p.validHere = std::move(validHere);
    p.nearest = nearest;
    p.fix = std::move(fix);
    result.problems.push_back(std::move(p));
    return false;
}

void TitleOpContext::note(const std::string& code, const std::string& message, const std::string& path) {
    TitleOpProblem p;
    p.severity = "info";
    p.code = code;
    p.op = opIndex;
    p.path = path;
    p.message = message;
    result.problems.push_back(std::move(p));
}

bool TitleOpContext::requireString(const TitleJson& op, const char* member, std::string& out) {
    auto it = op.find(member);
    if (it == op.end() || !it->is_string()) return fail("bad_op", std::string("`") + member + "` is needed (a string)", member);
    out = it->get<std::string>();
    return true;
}

std::string TitleOpContext::newId() {
    return ids.next();
}

void TitleOpContext::recordNewId(const std::string& key, const std::string& id) {
    TitleJson& entry = result.newIds[std::to_string(opIndex)];
    if (!entry.is_object()) entry = TitleJson::object();
    entry[key] = id;
}

void TitleOpContext::change(TitleOpChange c) {
    result.changes.push_back(std::move(c));
}

std::string TitleOpContext::layerPath(const std::string& id) const {
    std::string found;
    std::function<bool(const TitleJson&, const std::string&)> walk = [&](const TitleJson& list, const std::string& prefix) {
        std::map<std::string, int> counts;
        for (const TitleJson& layer : list) {
            const std::string name = str(layer, "name");
            const int n = counts[name]++;
            const std::string path = prefix + name + (n ? "#" + std::to_string(n) : "");
            if (str(layer, "id") == id) {
                found = path;
                return true;
            }
            if (walk(titleart::layerChildren(layer), path + "/")) return true;
        }
        return false;
    };
    walk(doc.layers(), "");
    return found;
}

TitleJson* TitleOpContext::resolveLayer(const std::string& ref, const std::string& member) {
    std::vector<std::string> paths;
    titleart::forEachLayer(doc.root, [&](const TitleJson& l, const TitleJson*, int) {
        paths.push_back(layerPath(str(l, "id")));
        return true;
    });
    if (ref.rfind("id:", 0) == 0) {
        if (TitleJson* layer = titleart::findLayer(doc.root, ref.substr(3))) return layer;
        fail("bad_layer", "no layer with id '" + ref.substr(3) + "'", member, paths);
        return nullptr;
    }
    // the path from the top level
    TitleJson* list = &doc.layers();
    TitleJson* found = nullptr;
    size_t start = 0;
    while (list) {
        const size_t slash = ref.find('/', start);
        std::string segment = ref.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        int nth = 0;
        const size_t hash = segment.rfind('#');
        if (hash != std::string::npos && hash + 1 < segment.size() &&
            std::all_of(segment.begin() + (long)hash + 1, segment.end(), ::isdigit)) {
            nth = std::stoi(segment.substr(hash + 1));
            segment = segment.substr(0, hash);
        }
        TitleJson* match = nullptr;
        int seen = 0;
        for (TitleJson& layer : *list) {
            if (str(layer, "name") == segment && seen++ == nth) {
                match = &layer;
                break;
            }
        }
        if (!match) {
            found = nullptr;
            break;
        }
        found = match;
        if (slash == std::string::npos) break;
        start = slash + 1;
        list = titleart::layerTypeOf(*match) == LayerType::Group ? &(*match)["children"] : nullptr;
        if (!list) found = nullptr;
    }
    if (found) return found;
    // a bare name: the layer of that name anywhere
    if (ref.find('/') == std::string::npos) {
        std::vector<std::string> ids;
        titleart::forEachLayer(doc.root, [&](const TitleJson& l, const TitleJson*, int) {
            if (str(l, "name") == ref) ids.push_back(str(l, "id"));
            return true;
        });
        if (ids.size() == 1) return titleart::findLayer(doc.root, ids[0]);
        if (ids.size() > 1) {
            std::vector<std::string> matches;
            for (const std::string& id : ids) matches.push_back(layerPath(id));
            fail("ambiguous", "'" + ref + "' names " + std::to_string(ids.size()) + " layers: use a path or id:", member, matches);
            return nullptr;
        }
    }
    fail("bad_layer", "no layer '" + ref + "'", member, paths, titleart::nearestName(ref, paths));
    return nullptr;
}

TitleJson* TitleOpContext::layer(const TitleJson& op, const char* member, bool required) {
    auto it = op.find(member);
    if (it == op.end() || it->is_null()) {
        if (required) fail("bad_op", std::string("`") + member + "` is needed (a layer path or id:<id>)", member);
        return nullptr;
    }
    if (!it->is_string()) {
        fail("bad_op", std::string("`") + member + "` is a layer path or id:<id>", member);
        return nullptr;
    }
    return resolveLayer(it->get<std::string>(), member);
}

bool TitleOpContext::set(TitleJson* layer, const std::string& path, const TitleJson& value) {
    titleart::FieldSetResult r = titleart::setTitleArtField(layer ? *layer : doc.root, layer ? "layer" : "document", path, value);
    if (!r.ok) {
        std::string message = r.error;
        if (r.code == "not_available") message += " (Nothing in this build reads it yet)";
        return fail(r.code, message, path, r.validHere, r.nearest);
    }
    TitleOpChange c;
    c.layer = layer ? str(*layer, "id") : std::string();
    c.path = r.longPath;
    c.from = r.from;
    c.to = r.to;
    c.clamped = r.clamped;
    c.clampMessage = r.clampMessage;
    const bool percent = (r.member && r.member->unit == titleart::ParamUnit::Percent) || (r.param && r.param->unit == titleart::ParamUnit::Percent);
    if (percent && layer && r.to.is_number()) c.px = r.to.get<double>() * titleart::layerSourceSize(*layer) / 100.0;
    change(std::move(c));
    // S83 R45: a value set on a param its `when` hides still applies and is written, with a note
    if (r.param && r.param->when.active()) {
        const size_t dot = r.longPath.rfind('.');
        const TitleJson* params = dot == std::string::npos ? nullptr
                                                           : titleart::getTitleArtField(layer ? *layer : doc.root, layer ? "layer" : "document", r.longPath.substr(0, dot));
        const TitleJson* switchValue = params && params->is_object() && params->contains(r.param->when.param) ? &(*params)[r.param->when.param] : nullptr;
        bool shown = false;
        for (const char* v : r.param->when.values) {
            if (switchValue && ((switchValue->is_string() && *switchValue == v) || (switchValue->is_boolean() && switchValue->dump() == v))) shown = true;
        }
        if (!shown) {
            std::string values;
            for (const char* v : r.param->when.values) values += (values.empty() ? "" : " or ") + std::string(v);
            note("inactive", std::string("inactive: ") + r.param->key + " applies only when " + r.param->when.param + " is " + values, r.longPath);
        }
    }
    // the label a one-value `set` names its undo step with
    const std::string label = r.param ? std::string(r.param->label) : (r.member ? r.member->label : path);
    if (!r.ownerLabel.empty()) lastSetLabel = r.ownerLabel + " " + label;
    else lastSetLabel = label;
    return true;
}

// ---- TitleArtOps ----

const std::vector<std::string>& TitleArtOps::s80Ops() {
    return s80Table();
}

const std::vector<ReservedTitleOp>& TitleArtOps::reservedOps() {
    return reservedTable();
}

std::string TitleArtOps::pendingOwner(const std::string& name) {
    if (registry().handlers.count(name)) return {};
    for (const ReservedTitleOp& r : reservedTable()) {
        if (name == r.name) return r.owner;
    }
    return {};
}

bool TitleArtOps::runCoreOp(TitleOpContext& ctx, const TitleJson& op, std::string* undoName) {
    const std::string name = op.is_object() ? op.value("op", std::string()) : std::string();
    if (std::find(s80Table().begin(), s80Table().end(), name) == s80Table().end()) return ctx.fail("bad_op", "'" + name + "' is not one of S80's ops", "op");
    Batch batch(ctx);
    const bool ok = batch.run(name, op);
    if (undoName) *undoName = batch.undoName;
    return ok;
}

bool TitleArtOps::registerOp(const std::string& name, TitleOpHandler handler) {
    const auto& own = s80Table();
    if (name.empty() || !handler || std::find(own.begin(), own.end(), name) != own.end()) return false;
    return registry().handlers.emplace(name, std::move(handler)).second;
}

bool TitleArtOps::unregisterOp(const std::string& name) {
    return registry().handlers.erase(name) > 0;
}

bool TitleArtOps::registerCreateImporter(const std::string& extension, TitleCreateImporter importer) {
    if (extension.empty() || !importer) return false;
    return registry().importers.emplace(extension, std::move(importer)).second;
}

bool TitleArtOps::unregisterCreateImporter(const std::string& extension) {
    return registry().importers.erase(extension) > 0;
}

bool TitleArtOps::createFrom(const std::string& sourcePath, TitleArtDoc& out, std::string& code, std::string& error) {
    auto it = registry().importers.find(lowerExtension(sourcePath));
    if (it == registry().importers.end()) {
        code = "not_available";
        error = "creating a Title Art from '" + sourcePath + "' (a Pachislop, PSD or SVG import) arrives with S87";
        return false;
    }
    if (!it->second(sourcePath, out, error)) {
        code = "import_failed";
        return false;
    }
    return true;
}

void TitleArtOps::applyCommitInvariants(const TitleArtDoc* before, TitleArtDoc& after, titleart::TitleIdGenerator& ids, TitleOpsResult& result) {
    std::vector<titleart::TitleWarning> warnings;
    titleart::NormalizeContext context;
    context.warnings = &warnings;
    context.ids = &ids;
    titleart::normalizeTitleArt(after, context);
    for (const titleart::TitleWarning& w : warnings) {
        TitleOpProblem p;
        p.severity = "info";
        p.code = "normalized";
        p.path = w.path;
        p.message = w.message;
        result.problems.push_back(std::move(p));
    }

    // link sync
    Json& links = after.root["material_links"];
    std::vector<MaterialAt> materials = materialsOf(after);
    std::map<std::string, std::string> previous;     // (layer \n path) -> material text before
    if (before) {
        TitleArtDoc copy = *before;
        for (const MaterialAt& m : materialsOf(copy)) previous[m.layer + "\n" + m.path] = m.value->dump();
    }
    std::map<std::string, std::vector<MaterialAt*>> groups;
    for (MaterialAt& m : materials) {
        const std::string key = str(*m.value, "link");
        if (key.empty()) continue;
        if (!links.contains(key)) {
            TitleOpChange c;
            c.layer = m.layer;
            c.path = m.path + ".link";
            c.from = key;
            c.to = "";
            c.note = "the material link '" + key + "' does not exist: unlinked";
            (*m.value)["link"] = "";
            result.changes.push_back(std::move(c));
            continue;
        }
        groups[key].push_back(&m);
    }
    for (auto& [key, members] : groups) {
        if (members.size() < 2) continue;
        const MaterialAt* source = nullptr;
        for (const MaterialAt* m : members) {
            auto it = previous.find(m->layer + "\n" + m->path);
            if (it == previous.end() || it->second != m->value->dump()) {
                source = m;
                break;
            }
        }
        if (!source) continue;
        for (MaterialAt* m : members) {
            if (m == source || *m->value == *source->value) continue;
            TitleOpChange c;
            c.layer = m->layer;
            c.path = m->path;
            c.note = "linked material '" + links[key].value("name", key) + "': kept equal";
            *m->value = *source->value;
            result.changes.push_back(std::move(c));
        }
    }
    // links nothing uses any more
    std::vector<std::string> unused;
    for (auto it = links.begin(); it != links.end(); ++it) {
        if (!groups.count(it.key())) unused.push_back(it.key());
    }
    for (const std::string& key : unused) {
        TitleOpChange c;
        c.path = "material_links." + key;
        c.from = links[key];
        c.to = nullptr;
        c.note = "no material uses the link: removed";
        links.erase(key);
        result.changes.push_back(std::move(c));
    }

    // tree checks: a clip with no base below it is cleared
    std::function<void(Json&)> clips = [&](Json& list) {
        if (!list.is_array()) return;
        bool base = false;
        for (Json& layer : list) {
            if (layer.value("clip", false) && !base) {
                TitleOpChange c;
                c.layer = str(layer, "id");
                c.path = "clip";
                c.from = true;
                c.to = false;
                c.note = "no layer below to clip to: cleared";
                layer["clip"] = false;
                result.changes.push_back(std::move(c));
            }
            if (!layer.value("clip", false)) base = true;
            if (titleart::layerTypeOf(layer) == LayerType::Group) clips(layer["children"]);
        }
    };
    clips(after.layers());

    for (const titleart::TitleProblem& p : titleart::titleArtDocumentProblems(after)) {
        if (p.code != "mask_layer_missing" && p.code != "follow_missing" && p.code != "exposed_target_missing") continue;
        TitleOpProblem n;
        n.severity = "warning";
        n.code = p.code;
        n.path = p.path;
        n.message = p.message;
        result.problems.push_back(std::move(n));
    }
}

TitleOpsResult TitleArtOps::apply(const TitleArtDoc& doc, const TitleJson& ops, const TitleOpsOptions& options) {
    TitleOpsResult result;
    result.doc = doc;
    titleart::TitleIdGenerator ids = options.idSeed ? titleart::TitleIdGenerator(options.idSeed) : titleart::TitleIdGenerator();
    ids.reserveFrom(doc.root);
    TitleOpContext ctx(result.doc, ids, result);
    const TitleJson list = ops.is_array() ? ops : TitleJson::array({ops});
    if (list.empty()) {
        ctx.fail("bad_op", "no ops");
        return result;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < list.size(); i++) {
        ctx.opIndex = (int)i;
        const TitleJson& op = list[i];
        std::vector<std::string> valid = s80Table();
        for (const auto& h : registry().handlers) valid.push_back(h.first);
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) {
            ctx.fail("bad_op", "an op is an object with `op`", "op", valid);
            result.failedOp = (int)i;
            return result;
        }
        const std::string name = op["op"].get<std::string>();
        bool ok = false;
        auto handler = registry().handlers.find(name);
        if (handler != registry().handlers.end()) {
            ctx.undoName.clear();
            ok = handler->second(ctx, op);
            names.push_back(ctx.undoName.empty() ? titleCase(name) : ctx.undoName);
        } else if (std::find(s80Table().begin(), s80Table().end(), name) != s80Table().end()) {
            Batch batch(ctx);
            ok = batch.run(name, op);
            names.push_back(batch.undoName);
        } else if (!pendingOwner(name).empty()) {
            ok = ctx.fail("not_available", "'" + name + "' arrives with " + pendingOwner(name), "op");
        } else {
            for (const ReservedTitleOp& r : reservedTable()) valid.push_back(r.name);
            ok = ctx.fail("bad_op", "unknown op '" + name + "'", "op", valid, titleart::nearestName(name, valid));
        }
        if (!ok) {
            result.failedOp = (int)i;
            result.ok = false;
            result.doc = doc;
            result.changes.clear();
            result.newIds = TitleJson::object();
            return result;
        }
        result.applied++;
    }
    applyCommitInvariants(&doc, result.doc, ids, result);
    result.ok = true;
    result.undoName = names.size() == 1 && !names[0].empty() ? names[0]
                                                             : "Edit Title Art (" + std::to_string(std::max(result.changes.size(), names.size())) + " changes)";
    return result;
}
