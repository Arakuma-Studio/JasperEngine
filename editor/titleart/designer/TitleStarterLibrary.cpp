// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleStarterLibrary.h"

#include "TitleDesignerState.h"

#include "titleart/TitleArtDoc.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    struct RimDef {
        double width;
        const char* color;
    };

    struct LookDef {
        const char* id;
        const char* name;
        const char* family;
        std::vector<const char*> face;      // one colour: solid; more: a vertical gradient, top first
        bool bevel;
        std::vector<RimDef> rims;           // innermost first
        double glowSize;                    // 0: no Outer Glow
        const char* glowColor;
        double shadowDistance;              // 0: no Drop Shadow
        std::vector<const char*> terms;
    };

    struct SchemeDef {
        const char* id;
        const char* name;
        const char* family;
        std::vector<const char*> face;
        std::vector<const char*> rims;      // rim colours, innermost first (repeated past the end)
        std::vector<const char*> terms;
    };

    const std::vector<LookDef>& looks() {
        static const std::vector<LookDef> table = {
            {"starter_gold", "Casino Gold", "Casino", {"#fff6b0", "#ffc93a", "#b86a00"}, true, {{3, "#3a1a00"}, {5, "#ffffff"}, {4, "#d01818"}}, 50, "#ff7a00", 12,
             {"gold", "jackpot", "金", "ゴールド"}},
            {"starter_chrome", "Chrome", "Metal & Chrome", {"#ffffff", "#9aa4b0", "#3b4450", "#e8eef5"}, true, {{3, "#1a1f26"}, {4, "#cfd6de"}}, 0, "", 10,
             {"metal", "silver", "クローム", "メタル"}},
            {"starter_neon_pink", "Neon Pink", "Neon", {"#ffe6f4"}, false, {{4, "#ff2fa8"}}, 70, "#ff2fa8", 0, {"neon", "pink", "ネオン"}},
            {"starter_neon_cyan", "Neon Cyan", "Neon", {"#e6fdff"}, false, {{4, "#18d8ff"}}, 70, "#18d8ff", 0, {"neon", "blue", "ネオン"}},
            {"starter_fire", "Fire", "Elements", {"#fff3a0", "#ff9a1a", "#d42000"}, false, {{4, "#400800"}}, 60, "#ff4a00", 0, {"flame", "炎", "ファイア"}},
            {"starter_ice", "Ice", "Elements", {"#ffffff", "#a8e6ff", "#2a7fd0"}, false, {{3, "#0b2a50"}, {4, "#e8f8ff"}}, 40, "#7fd8ff", 0,
             {"frost", "氷", "アイス"}},
            {"starter_emerald", "Emerald", "Gems & Glass", {"#b8ffd8", "#18b86a", "#05542c"}, true, {{3, "#022414"}, {4, "#e8fff2"}}, 0, "", 10,
             {"gem", "green", "エメラルド"}},
            {"starter_wa_red", "Wa Red", "Japanese Wa", {"#c8102e"}, false, {{4, "#ffffff"}, {3, "#1a1a1a"}}, 0, "", 8, {"japanese", "red", "和", "朱"}},
            {"starter_arcade", "Arcade", "Retro Arcade", {"#ffe94a", "#ff8a00"}, false, {{5, "#2a0a5e"}, {4, "#00e5ff"}}, 0, "", 14,
             {"retro", "8bit", "レトロ"}},
            {"starter_minimal", "Minimal White", "Minimal", {"#ffffff"}, false, {}, 0, "", 6, {"plain", "simple", "シンプル"}},
        };
        return table;
    }

    const std::vector<SchemeDef>& schemes() {
        static const std::vector<SchemeDef> table = {
            {"starter_red_gold", "Red & Gold", "Casino", {"#ffe9a0", "#ffb800", "#a85a00"}, {"#5a0000", "#ffffff", "#d01818"}, {"red", "gold", "赤", "金"}},
            {"starter_blue_steel", "Blue Steel", "Metal & Chrome", {"#eaf4ff", "#6fa8dc", "#1d3f73"}, {"#0a1a33", "#ffffff", "#3d7bd9"}, {"blue", "青"}},
            {"starter_green", "Green", "Elements", {"#e8ffe0", "#5fd35f", "#1c6b1c"}, {"#0a2a0a", "#ffffff", "#22aa44"}, {"green", "緑"}},
            {"starter_purple", "Purple", "Neon", {"#f6e8ff", "#b26bff", "#4a1880"}, {"#1e0638", "#ffffff", "#9a3cff"}, {"violet", "紫"}},
            {"starter_mono", "Mono", "Minimal", {"#ffffff", "#bbbbbb", "#555555"}, {"#111111", "#ffffff", "#777777"}, {"grey", "gray", "モノクロ"}},
        };
        return table;
    }

    Json fillOf(const std::vector<const char*>& colors) {
        if (colors.size() == 1) return Json{{"kind", "solid"}, {"color", colors[0]}};
        Json stops = Json::array();
        for (size_t i = 0; i < colors.size(); i++) stops.push_back(Json{{"pos", (double)i / (double)(colors.size() - 1)}, {"color", colors[i]}});
        return Json{{"kind", "linear"}, {"angle", 90}, {"gradient", Json{{"stops", stops}}}};
    }

    // layers whose style a Look or a Color Scheme writes
    // (an Image keeps its own pixels: no face fill to recolour)
    bool styled(const Json& layer) {
        const ta::LayerType type = ta::layerTypeOf(layer);
        return ta::layerTypeHasStyle(type) && type != ta::LayerType::Image;
    }

    std::string idOf(const Json& object) {
        auto it = object.find("id");
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

}

TitleStarterLibrary::TitleStarterLibrary(const TitleDesignerState* state, TileSource tiles) : state(state), tiles(std::move(tiles)) {
}

std::vector<LibraryItem> TitleStarterLibrary::items(LibraryTab tab) {
    std::vector<LibraryItem> out;
    if (tab == LibraryTab::Looks) {
        for (const LookDef& d : looks()) {
            LibraryItem item{d.id, d.name, d.family};
            item.dimension = "2d";
            for (const char* t : d.terms) item.terms.push_back(t);
            out.push_back(std::move(item));
        }
    } else if (tab == LibraryTab::ColorSchemes) {
        for (const SchemeDef& d : schemes()) {
            LibraryItem item{d.id, d.name, d.family};
            for (const char* t : d.terms) item.terms.push_back(t);
            out.push_back(std::move(item));
        }
    }
    return out;
}

std::vector<std::string> TitleStarterLibrary::families() {
    return {"Casino", "Metal & Chrome", "Neon", "Elements", "Gems & Glass", "Japanese Wa", "Retro Arcade", "Minimal"};
}

Json TitleStarterLibrary::applyOps(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) {
    Json ops = Json::array();
    if (!state || !state->isOpen()) return ops;
    const ta::TitleArtDoc& doc = state->committed();
    if (tab == LibraryTab::Looks) {
        auto def = std::find_if(looks().begin(), looks().end(), [&](const LookDef& d) { return item.id == d.id; });
        if (def == looks().end()) return ops;
        Json rims = Json::array();
        for (const RimDef& r : def->rims) rims.push_back(Json{{"width", r.width}, {"material", Json{{"fill", Json{{"color", r.color}}}}}});
        Json effects = Json::array();
        if (def->glowSize > 0.0) effects.push_back(Json{{"type", "outer_glow"}, {"params", Json{{"size", def->glowSize}, {"color", def->glowColor}}}});
        if (def->shadowDistance > 0.0) effects.push_back(Json{{"type", "drop_shadow"}, {"params", Json{{"distance", def->shadowDistance}}}});
        bool any = false;
        for (const std::string& id : options.layers) {
            const Json* layer = layerById(doc, id);
            if (!layer || !styled(*layer)) continue;
            any = true;
            const std::string ref = "id:" + id;
            // the Look replaces what styled the layer: its effects and rims go first
            if (auto fx = layer->find("effects"); fx != layer->end() && fx->is_array()) {
                for (const Json& e : *fx) ops.push_back(Json{{"op", "remove_effect"}, {"layer", ref}, {"effect", "id:" + idOf(e)}});
            }
            const Json* style = layer->contains("style") ? &(*layer)["style"] : nullptr;
            if (style && style->contains("rims") && (*style)["rims"].is_array()) {
                for (const Json& r : (*style)["rims"]) ops.push_back(Json{{"op", "remove_rim"}, {"layer", ref}, {"rim", "id:" + idOf(r)}});
            }
            ops.push_back(Json{{"op", "set"}, {"layer", ref}, {"values", Json{{"style.face.fill", fillOf(def->face)}, {"style.face.bevel.enabled", def->bevel}}}});
            for (const RimDef& r : def->rims) {
                ops.push_back(Json{{"op", "add_rim"}, {"layer", ref}, {"values", Json{{"width", r.width}, {"material.fill.color", r.color}}}});
            }
            for (const Json& e : effects) {
                Json op = e;
                op["op"] = "add_effect";
                op["layer"] = ref;
                ops.push_back(std::move(op));
            }
        }
        if (!any && options.layers.empty()) {
            // nothing to restyle: a new text in this look (R40 "Adds a text in this look")
            Json layer = {{"type", "text"}, {"name", "Title"}, {"text", "TITLE"}, {"size", 160},
                          {"style", Json{{"face", Json{{"fill", fillOf(def->face)}, {"bevel", Json{{"enabled", def->bevel}}}}}, {"rims", rims}}}, {"effects", effects}};
            ops.push_back(Json{{"op", "add_layer"}, {"from", layer}});
        }
        return ops;
    }
    if (tab == LibraryTab::ColorSchemes) {
        auto def = std::find_if(schemes().begin(), schemes().end(), [&](const SchemeDef& d) { return item.id == d.id; });
        if (def == schemes().end()) return ops;
        std::vector<std::string> targets = options.layers;
        if (options.wholeTitle) {
            targets.clear();
            ta::forEachLayer(doc.root, [&](const Json& l, const Json*, int) {
                if (styled(l)) targets.push_back(idOf(l));
                return true;
            });
        }
        for (const std::string& id : targets) {
            const Json* layer = layerById(doc, id);
            if (!layer || !styled(*layer)) continue;
            Json values = {{"style.face.fill", fillOf(def->face)}};
            const Json* style = layer->contains("style") ? &(*layer)["style"] : nullptr;
            const size_t rimCount = style && style->contains("rims") && (*style)["rims"].is_array() ? (*style)["rims"].size() : 0;
            for (size_t i = 0; i < rimCount && !def->rims.empty(); i++) {
                values["style.rims[" + std::to_string(i) + "].material.fill.color"] = def->rims[i % def->rims.size()];
            }
            ops.push_back(Json{{"op", "set"}, {"layer", "id:" + id}, {"values", values}});
        }
    }
    return ops;
}

std::string TitleStarterLibrary::stepName(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) {
    if (tab == LibraryTab::ColorSchemes) return "Apply Color Scheme " + item.name + (options.wholeTitle ? " to Whole Title" : "");
    return "Apply Look " + item.name;
}

LibraryTile TitleStarterLibrary::tile(LibraryTab tab, const LibraryItem& item, const std::string& targetKey, float width, float height, bool visible) {
    (void)targetKey;
    if (!tiles) return {};
    return tiles(tab, item, width, height, visible);
}
