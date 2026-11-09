// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The category rail, counts and the generator section inside Add Effect follow Pachislop's
// AddEffectMenu (src/ui/properties/AddEffectMenu.tsx) by SafEight, used with the author's
// permission; its separate generator flyout is not ported (S82 R17).

#include "TitleAddCatalog.h"

#include "TitleRows.h"
#include "TitleSearchFold.h"
#include "TitleSearchTerms.h"
#include "search/SearchScorer.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/fx/FxPresets.h"
#include "titleart/fx/FxRecipes.h"

#include <algorithm>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

const char* const jasperengine::editor::titledesigner::FOLLOW_CATEGORY = "Generators that follow this layer";

namespace {

    // S83 §6.10's order inside each Add Effect category ("fx:" a type, "recipe:" a recipe,
    // "style:" a style setting); entries the table does not name follow in registry order
    const std::map<std::string, std::vector<std::string>>& effectOrder() {
        static const std::map<std::string, std::vector<std::string>> order = {
            {"Glow", {"fx:outer_glow", "fx:inner_glow", "fx:light_glow", "fx:contour_lines", "fx:neon_tube", "style:glint_3d"}},
            {"Shadow & Depth", {"fx:drop_shadow", "fx:inner_shadow", "recipe:long_shadow", "fx:extrude", "fx:perspective_shadow", "fx:floor_reflection"}},
            {"Stroke & Edge", {"fx:outline", "fx:brush_stroke", "fx:stitch"}},
            {"Color", {"fx:color_overlay", "fx:gradient_overlay", "recipe:shine", "recipe:rainbow", "recipe:vignette", "fx:adjust", "fx:holographic", "fx:grain"}},
            {"Texture & Material",
             {"fx:pattern_overlay", "fx:material", "recipe:emboss", "recipe:deboss", "fx:distress", "style:dry_brush", "fx:drips", "recipe:icicles", "fx:frost",
              "fx:chalk"}},
            {"Stylize", {"fx:halftone", "recipe:comic_dots", "recipe:ben_day", "fx:pixelate", "fx:scanlines", "recipe:crt", "fx:glitch", "fx:chromatic_aberration"}},
            {"Blur", {"fx:gaussian_blur", "fx:motion_blur", "fx:radial_blur", "fx:lens_blur", "fx:tilt_shift"}},
            {"Distort", {"fx:wave_warp", "fx:motion_trail", "fx:shatter"}},
        };
        return order;
    }

    const std::map<std::string, std::vector<std::string>>& generatorOrder() {
        static const std::map<std::string, std::vector<std::string>> order = {
            {"Particles", {"particles", "ink_splatter"}},
            {"Lines", {"speed_lines", "sunburst", "light_streaks"}},
            {"Light", {"glints", "lens_flare", "backlight"}},
            {"Rings", {"shockwave", "magic_circle", "brush_swirl"}},
            {"Energy", {"fire", "smoke", "aura", "lightning"}},
            {"Fill & Pattern", {"fill", "pattern_fill", "fractal_noise"}},
        };
        return order;
    }

    // the position of `key` in its category's order (unnamed ones after, in their given order)
    size_t rankIn(const std::vector<std::string>& order, const std::string& key, size_t fallback) {
        auto it = std::find(order.begin(), order.end(), key);
        return it != order.end() ? (size_t)(it - order.begin()) : order.size() + fallback;
    }

    std::string withTarget(const std::string& verb, const AddTarget& target) {
        if (target.layers.size() > 1) return verb + " to " + std::to_string(target.layers.size()) + " Layers";
        return verb + " to " + target.name;
    }

    void addTerms(AddEntry& e, const std::vector<const char*>& aliases, const std::string& type) {
        for (const char* alias : aliases) e.terms.push_back(alias);
        const std::string_view ja = SearchTerms::builtin().japaneseName(type);
        if (!ja.empty()) e.terms.push_back(std::string(ja));
    }

    std::string placeOf(const TitleFxPreset* preset) {
        if (preset && preset->layer.is_object()) return preset->layer.value("place", std::string("above"));
        return "above";
    }

    // a generator's add_layer: beside the followed layer when it follows (S80's placement), else
    // above the selection
    Json generatorOp(const ta::GeneratorDef& def, const TitleFxPreset* preset, const AddTarget& target) {
        Json op = Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", def.type}};
        if (preset) op["preset"] = preset->id;
        const std::string primary = target.layers.empty() ? std::string() : target.layers.back();
        if (def.follow != ta::FollowMode::None && !primary.empty() && target.hasStyle) {
            op["follow"] = "id:" + primary;
        } else if (!primary.empty()) {
            op["after"] = "id:" + primary;
        }
        return op;
    }

    std::string costOf(ta::CostClass base, const TitleFxPreset* preset) {
        if (preset && !preset->cost.empty()) return preset->cost;
        return ta::costClassName(base);
    }

}

const char* jasperengine::editor::titledesigner::addSectionName(AddEntry::Section section) {
    switch (section) {
        case AddEntry::Section::Layers: return "Layers";
        case AddEntry::Section::Generators: return "Generators";
        case AddEntry::Section::Effects: return "Effects";
    }
    return "";
}

const std::vector<std::string>& jasperengine::editor::titledesigner::addEffectCategories() {
    static const std::vector<std::string> list = {"Glow", "Shadow & Depth", "Stroke & Edge", "Color", "Texture & Material", "Stylize", "Blur", "Distort"};
    return list;
}

const std::vector<std::string>& jasperengine::editor::titledesigner::addGeneratorCategories() {
    static const std::vector<std::string> list = {"Particles", "Lines", "Light", "Rings", "Energy", "Fill & Pattern"};
    return list;
}

std::vector<AddEntry> jasperengine::editor::titledesigner::buildAddEntries(const std::string& filter, const AddTarget& target) {
    std::vector<AddEntry> out;
    const std::string primary = target.layers.empty() ? std::string() : target.layers.back();
    const ta::TitleEffectRegistry& registry = ta::TitleEffectRegistry::get();

    // ---- Layers ----
    if (filter.empty() || filter == "layers") {
        auto layer = [&](const std::string& category, const std::string& id, const std::string& label, Json op, const std::string& description) {
            AddEntry e;
            e.section = AddEntry::Section::Layers;
            e.category = category;
            e.id = "layer:" + id;
            e.label = label;
            e.description = description;
            if (!primary.empty()) op["after"] = "id:" + primary;
            e.ops = Json::array({op});
            e.stepName = "Add Layer " + label;
            e.kind = PreviewItem::Kind::Layer;
            e.addsLayer = true;
            out.push_back(std::move(e));
        };
        layer("Layers", "text", "Text", Json{{"op", "add_layer"}, {"type", "text"}}, "Type words on the canvas");
        out.back().startsTextEdit = true;
        for (const char* shape : {"rect", "parallelogram", "ellipse", "orb", "polygon", "star", "burst", "shield", "speech", "shout", "ribbon"}) {
            layer("Shapes", std::string("shape:") + shape, "Shape " + enumLabel(shape), Json{{"op", "add_layer"}, {"type", "shape"}, {"values", {{"shape", shape}}}},
                  "A " + enumLabel(shape) + " shape with rims and materials like text");
        }
        layer("Layers", "image", "Image...", Json{{"op", "add_layer"}, {"type", "image"}}, "A picture from the project (drop a Texture on the tree)");
        // S87 R34: a picker action (no preview): the chosen file goes through dropFiles
        layer("Layers", "vector", "Vector (SVG)...", Json{{"op", "add_layer"}, {"type", "vector"}}, "An SVG file, split into layers if you ask (import_svg)");
        out.back().ops = Json::array();
        layer("Layers", "group", "Group", Json{{"op", "add_layer"}, {"type", "group"}}, "A folder of layers");
        std::vector<std::string> adjustments;
        if (const ta::SchemaKind* kind = ta::TitleArtSchema::get().kind("adjustment")) {
            if (const ta::SchemaMember* type = ta::TitleArtSchema::get().member(*kind, "type")) adjustments = type->options;
        }
        for (const std::string& adjustment : adjustments) {
            layer("Adjustments", "adj:" + adjustment, "Adjustment " + enumLabel(adjustment),
                  Json{{"op", "add_layer"}, {"type", "adjustment"}, {"adjustment", adjustment}}, "Adjusts the colours of the layers under it");
        }
    }

    // the generators in §6.10's category order, each with its presets
    std::vector<const ta::GeneratorDef*> generators;
    for (const ta::GeneratorDef* def : registry.generators()) generators.push_back(def);
    auto categoryRank = [](const std::vector<std::string>& categories, const std::string& category) {
        auto it = std::find(categories.begin(), categories.end(), category);
        return it != categories.end() ? (size_t)(it - categories.begin()) : categories.size();
    };
    std::stable_sort(generators.begin(), generators.end(), [&](const ta::GeneratorDef* a, const ta::GeneratorDef* b) {
        const size_t ca = categoryRank(addGeneratorCategories(), a->category), cb = categoryRank(addGeneratorCategories(), b->category);
        if (ca != cb) return ca < cb;
        static const std::vector<std::string> none;
        auto order = generatorOrder().find(a->category);
        const std::vector<std::string>& list = order != generatorOrder().end() ? order->second : none;
        return rankIn(list, a->type, 0) < rankIn(list, b->type, 0);
    });

    // ---- Generators ----
    if (filter.empty() || filter == "layers" || filter == "generators") {
        for (const ta::GeneratorDef* def : generators) {
            auto generator = [&](const TitleFxPreset* preset) {
                AddEntry e;
                e.section = AddEntry::Section::Generators;
                e.category = def->category;
                e.id = std::string("gen:") + def->type + (preset ? ":" + preset->id : std::string());
                e.label = preset ? preset->name : std::string(def->label);
                e.description = def->description;
                addTerms(e, def->aliases, def->type);
                if (preset) {
                    e.terms.push_back(def->label);
                    for (const std::string& alias : preset->aliases) e.terms.push_back(alias);
                } else {
                    for (const TitleFxPreset& p : TitleFxPresets::forType(def->type)) e.terms.push_back(p.name);
                }
                e.ops = Json::array({generatorOp(*def, preset, target)});
                e.stepName = "Add Layer " + e.label;
                e.kind = PreviewItem::Kind::Generator;
                e.cost = costOf(def->costClass, preset);
                e.depth = preset ? 1 : 0;
                e.addsLayer = true;
                if (def->follow != ta::FollowMode::None && !primary.empty() && target.hasStyle) e.note = "Follows \"" + target.name + "\" \xC2\xB7 " + placeOf(preset);
                else if (def->follow == ta::FollowMode::Required) e.note = "Select a layer for it to follow";
                out.push_back(std::move(e));
            };
            generator(nullptr);
            for (const TitleFxPreset& preset : TitleFxPresets::forType(def->type)) generator(&preset);
        }
    }

    // ---- Effects (a selected layer) ----
    if ((filter.empty() || filter == "effects") && !primary.empty()) {
        const bool toParts = !target.parts.empty();
        std::vector<AddEntry> effects;
        std::map<std::string, size_t> registryOrder;
        size_t serial = 0;
        auto effectOps = [&](const std::function<Json(const std::string& layer, const std::string& part)>& make) {
            Json ops = Json::array();
            if (toParts) {
                for (const NodeRef& node : target.parts) ops.push_back(make(node.layer, node.part));
            } else {
                for (const std::string& id : target.layers) ops.push_back(make(id, std::string()));
            }
            return ops;
        };
        for (const ta::EffectDef* def : registry.effects()) {
            if (toParts && !def->onParts) continue;
            AddEntry e;
            e.section = AddEntry::Section::Effects;
            e.category = def->category;
            e.id = std::string("fx:") + def->type;
            e.label = def->label;
            e.description = def->description;
            addTerms(e, def->aliases, def->type);
            for (const TitleFxPreset& p : TitleFxPresets::forType(def->type)) {
                e.terms.push_back(p.name);
                for (const std::string& alias : p.aliases) e.terms.push_back(alias);
            }
            const std::string type = def->type;
            e.ops = effectOps([&](const std::string& layer, const std::string& part) {
                Json op = Json{{"op", "add_effect"}, {"layer", "id:" + layer}, {"type", type}};
                if (!part.empty()) op["part"] = part;
                return op;
            });
            e.stepName = withTarget(std::string("Add Effect ") + def->label, target);
            e.kind = PreviewItem::Kind::Effect;
            e.cost = ta::costClassName(def->costClass);
            registryOrder[e.id] = serial++;
            effects.push_back(std::move(e));
        }
        for (const TitleFxRecipe& recipe : titleFxRecipes()) {
            const ta::EffectDef* def = ta::findEffect(recipe.type);
            if (!def || (toParts && !def->onParts)) continue;
            AddEntry e;
            e.section = AddEntry::Section::Effects;
            e.category = recipe.category;
            e.id = std::string("recipe:") + recipe.id;
            e.label = recipe.name;
            e.description = std::string(def->description) + " (" + recipe.name + " is a recipe of " + def->label + ")";
            for (const char* alias : recipe.aliases) e.terms.push_back(alias);
            e.terms.push_back(def->label);
            const std::string id = recipe.id;
            e.ops = effectOps([&](const std::string& layer, const std::string& part) {
                Json op = Json{{"op", "apply_recipe"}, {"layer", "id:" + layer}, {"recipe", id}};
                if (!part.empty()) op["part"] = part;
                return op;
            });
            e.stepName = withTarget(std::string("Add Effect ") + recipe.name, target);
            e.kind = PreviewItem::Kind::Effect;
            const TitleFxPreset* preset = recipe.preset ? TitleFxPresets::find(recipe.type, recipe.preset) : nullptr;
            e.cost = costOf(def->costClass, preset);
            e.note = std::string("Recipe of ") + def->label;
            registryOrder[e.id] = serial++;
            effects.push_back(std::move(e));
        }
        if (!toParts && target.hasStyle) {
            for (const TitleFxStyleSetting& setting : titleFxStyleSettings()) {
                const std::string member = setting.member;
                const bool threeD = member.rfind("style_3d.", 0) == 0;
                // Dry Brush is the 2D style's erode; Glint (3D highlights) is a 3D layer's
                if (threeD != target.is3D) continue;
                AddEntry e;
                e.section = AddEntry::Section::Effects;
                e.category = setting.category;
                e.id = std::string("style:") + setting.id;
                e.label = setting.name;
                e.description = std::string("Switches on the layer's ") + setting.name + " (a style setting of " + setting.layers + ")";
                for (const char* alias : setting.aliases) e.terms.push_back(alias);
                Json ops = Json::array();
                for (const std::string& id : target.layers) ops.push_back(Json{{"op", "set"}, {"layer", "id:" + id}, {"values", {{member, true}}}});
                e.ops = ops;
                e.stepName = std::string("Turn On ") + setting.name;
                e.kind = PreviewItem::Kind::Effect;
                e.note = "Style setting";
                e.openSections = threeD ? std::vector<std::string>{"Dimension", "Glint"} : std::vector<std::string>{"Appearance", "Erode"};
                registryOrder[e.id] = serial++;
                effects.push_back(std::move(e));
            }
        }
        std::stable_sort(effects.begin(), effects.end(), [&](const AddEntry& a, const AddEntry& b) {
            const size_t ca = categoryRank(addEffectCategories(), a.category), cb = categoryRank(addEffectCategories(), b.category);
            if (ca != cb) return ca < cb;
            static const std::vector<std::string> none;
            auto order = effectOrder().find(a.category);
            const std::vector<std::string>& list = order != effectOrder().end() ? order->second : none;
            return rankIn(list, a.id, registryOrder[a.id]) < rankIn(list, b.id, registryOrder[b.id]);
        });
        for (AddEntry& e : effects) out.push_back(std::move(e));

        // generators that follow this layer: every preset of every following generator (§6.10)
        if (!toParts && target.hasStyle) {
            for (const ta::GeneratorDef* def : generators) {
                if (def->follow == ta::FollowMode::None) continue;
                const std::vector<TitleFxPreset>& presets = TitleFxPresets::forType(def->type);
                auto follow = [&](const TitleFxPreset* preset) {
                    AddEntry e;
                    e.section = AddEntry::Section::Effects;
                    e.category = FOLLOW_CATEGORY;
                    e.id = std::string("follow:") + def->type + (preset ? ":" + preset->id : std::string());
                    e.label = preset ? preset->name : std::string(def->label);
                    e.description = std::string(def->category) + " \xC2\xB7 " + def->description;
                    addTerms(e, def->aliases, def->type);
                    e.terms.push_back(def->label);
                    if (preset) {
                        for (const std::string& alias : preset->aliases) e.terms.push_back(alias);
                    }
                    e.ops = Json::array({generatorOp(*def, preset, target)});
                    e.stepName = "Add Layer " + e.label;
                    e.kind = PreviewItem::Kind::Generator;
                    e.cost = costOf(def->costClass, preset);
                    e.note = "Adds a Generator layer \xC2\xB7 " + placeOf(preset);
                    e.addsLayer = true;
                    out.push_back(std::move(e));
                };
                if (presets.empty()) follow(nullptr);
                for (const TitleFxPreset& preset : presets) follow(&preset);
            }
        }
    }
    return out;
}

bool jasperengine::editor::titledesigner::addEntryMatches(const AddEntry& entry, const std::string& query) {
    if (query.empty()) return true;
    const std::string q = foldForSearch(query);
    if (q.empty()) return true;
    const std::string nq = search::normalize(q);
    auto has = [&](const std::string& text) {
        const std::string f = foldForSearch(text);
        return f.find(q) != std::string::npos || search::fuzzyScore(nq, search::normalize(f)) >= 800;
    };
    if (has(entry.label) || has(entry.category)) return true;
    for (const std::string& t : entry.terms) {
        if (has(t)) return true;
    }
    for (const std::string& s : SearchTerms::builtin().synonymsOf(q)) {
        if (foldForSearch(entry.label).find(s) != std::string::npos) return true;
        for (const std::string& t : entry.terms) {
            if (foldForSearch(t).find(s) != std::string::npos) return true;
        }
    }
    return false;
}

std::string jasperengine::editor::titledesigner::fitAddEntryText(const std::string& text, float maxWidth,
                                                                 const std::function<float(const std::string&)>& measure) {
    if (measure(text) <= maxWidth) return text;
    static const std::string dots = "...";
    if (measure(dots) > maxWidth) return std::string();
    // the byte offsets where a character ends, then the longest prefix that fits with the dots
    std::vector<size_t> ends{0};
    for (size_t i = 0; i < text.size(); i++) {
        if (i + 1 == text.size() || ((unsigned char)text[i + 1] & 0xC0) != 0x80) ends.push_back(i + 1);
    }
    size_t low = 0, high = ends.size() - 1;
    while (low < high) {
        const size_t middle = (low + high + 1) / 2;
        if (measure(text.substr(0, ends[middle]) + dots) <= maxWidth) low = middle;
        else high = middle - 1;
    }
    return text.substr(0, ends[low]) + dots;
}
