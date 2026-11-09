// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The unified Add palette (docs/specs/S82-title-designer.md R17, D6): one searchable popover for
// layers, generators and effects with a category rail, hover/arrow preview and a header naming
// the target. Pachislop's AddEffectMenu (src/ui/properties/AddEffectMenu.tsx) by SafEight, used
// with the author's permission, is the model; its separate three-level generator flyout is not.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/Profile.h"
#include "titleart/designer/TitleSearchFold.h"
#include "titleart/designer/TitleSearchTerms.h"
#include "search/SearchScorer.h"
#include "titleart/TitleArtSchema.h"
#include "util/FileDialogs.h"

#include "imgui.h"

#include <algorithm>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    struct AddEntry {
        std::string section;        // "Layers", "Generators", "Effects"
        std::string category;       // the rail
        std::string id;             // unique
        std::string label;
        std::string description;
        std::vector<std::string> terms;
        ta::Json op;                    // what Enter adds (null: a picker action)
        std::string stepName;
        PreviewItem::Kind kind = PreviewItem::Kind::Layer;
        bool previewable = true;
    };

    // English and Japanese both, folded (R26/R27's matching)
    bool matches(const AddEntry& entry, const std::string& query) {
        if (query.empty()) return true;
        const std::string q = foldForSearch(query);
        if (q.empty()) return true;
        auto has = [&](const std::string& text) {
            const std::string f = foldForSearch(text);
            return f.find(q) != std::string::npos || search::fuzzyScore(search::normalize(q), search::normalize(f)) >= 800;
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

}

void TitleDesignerWindow::drawAddPalette() {
    JE_PROFILE_SCOPE("TD drawAddPalette");
    if (addOpenRequested) {
        ImGui::OpenPopup("Add###addpalette");
        addOpenRequested = false;
        addOpen = true;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Add###addpalette")) {
        if (addOpen) {
            // Esc / a click outside closes and restores
            addOpen = false;
            if (doc.previewActive() && doc.preview() && doc.preview()->kind != PreviewItem::Kind::Font) doc.clearPreview();
            addHovered.clear();
        }
        return;
    }
    const ta::TitleArtDoc& shown = doc.shown();
    const std::vector<std::string> layers = doc.selectedLayers();
    const NodeRef primary = doc.primary();
    const bool partsSelected = primary.kind == NodeKind::Part;
    const ta::Json* targetLayer = layers.empty() ? nullptr : layerById(shown, layers.back());
    const std::string targetName = targetLayer ? str(*targetLayer, "name") : std::string();

    // the header names the target, and offers parts or the whole layer
    if (targetLayer) {
        ImGui::Text("Adds to \"%s\"", targetName.c_str());
        if (partsSelected) {
            ImGui::SameLine();
            if (ImGui::RadioButton("Selected parts", addToParts)) addToParts = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Whole layer", !addToParts)) addToParts = false;
        }
    } else {
        ImGui::TextDisabled("Effects need a selected layer");
    }

    // the entries
    std::vector<AddEntry> entries;
    if (addFilter.empty() || addFilter == "layers") {
        auto layer = [&](const std::string& category, const std::string& id, const std::string& label, ta::Json op, const std::string& description) {
            AddEntry e;
            e.section = "Layers";
            e.category = category;
            e.id = "layer:" + id;
            e.label = label;
            e.description = description;
            e.op = std::move(op);
            e.stepName = "Add Layer " + label;
            e.kind = PreviewItem::Kind::Layer;
            if (targetLayer && e.op.is_object()) e.op["after"] = "id:" + layers.back();
            entries.push_back(e);
        };
        layer("Layers", "text", "Text", ta::Json{{"op", "add_layer"}, {"type", "text"}}, "Type words on the canvas");
        for (const char* shape : {"rect", "parallelogram", "ellipse", "orb", "polygon", "star", "burst", "shield", "speech", "shout", "ribbon"}) {
            layer("Shapes", std::string("shape:") + shape, "Shape " + enumLabel(shape), ta::Json{{"op", "add_layer"}, {"type", "shape"}, {"values", {{"shape", shape}}}},
                  "A " + enumLabel(shape) + " shape with rims and materials like text");
        }
        layer("Layers", "image", "Image...", ta::Json{{"op", "add_layer"}, {"type", "image"}}, "A picture from the project (drop a Texture on the tree)");
        // R34: a picker action, the chosen file goes through dropFiles
        layer("Layers", "vector", "Vector (SVG)...", ta::Json(), "An SVG file, split into layers if you ask (import_svg)");
        entries.back().previewable = false;
        layer("Layers", "group", "Group", ta::Json{{"op", "add_layer"}, {"type", "group"}}, "A folder of layers");
        // the adjustment types of S80's schema
        std::vector<std::string> adjustments;
        if (const ta::SchemaKind* kind = ta::TitleArtSchema::get().kind("adjustment")) {
            if (const ta::SchemaMember* type = ta::TitleArtSchema::get().member(*kind, "type")) adjustments = type->options;
        }
        for (const std::string& adjustment : adjustments) {
            layer("Adjustments", "adj:" + adjustment, "Adjustment " + enumLabel(adjustment),
                  ta::Json{{"op", "add_layer"}, {"type", "adjustment"}, {"adjustment", adjustment}}, "Adjusts the colours of the layers under it");
        }
    }
    if (addFilter.empty() || addFilter == "layers" || addFilter == "generators" || addFilter == "effects") {
        for (const ta::GeneratorDef* def : ta::TitleEffectRegistry::get().generators()) {
            AddEntry e;
            e.section = "Generators";
            e.category = def->category;
            e.id = std::string("gen:") + def->type;
            e.label = def->label;
            e.description = def->description;
            for (const char* alias : def->aliases) e.terms.push_back(alias);
            const std::string_view ja = SearchTerms::builtin().japaneseName(def->type);
            if (!ja.empty()) e.terms.push_back(std::string(ja));
            e.op = ta::Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", def->type}};
            if (targetLayer) e.op["after"] = "id:" + layers.back();
            // generators that follow a layer follow the selected one
            if (def->follow != ta::FollowMode::None && targetLayer) e.op["values"] = ta::Json{{"follow", layers.back()}};
            e.stepName = std::string("Add Layer ") + def->label;
            e.kind = PreviewItem::Kind::Generator;
            entries.push_back(e);
        }
    }
    if ((addFilter.empty() || addFilter == "effects") && targetLayer) {
        for (const ta::EffectDef* def : ta::TitleEffectRegistry::get().effects()) {
            const bool toParts = partsSelected && addToParts;
            if (toParts && !def->onParts) continue;
            AddEntry e;
            e.section = "Effects";
            e.category = def->category;
            e.id = std::string("fx:") + def->type;
            e.label = def->label;
            e.description = def->description;
            for (const char* alias : def->aliases) e.terms.push_back(alias);
            const std::string_view ja = SearchTerms::builtin().japaneseName(def->type);
            if (!ja.empty()) e.terms.push_back(std::string(ja));
            ta::Json ops = ta::Json::array();
            if (toParts) {
                for (const NodeRef& node : doc.selection()) {
                    if (node.kind == NodeKind::Part) ops.push_back({{"op", "add_effect"}, {"layer", "id:" + node.layer}, {"part", node.part}, {"type", def->type}});
                }
            } else {
                for (const std::string& id : layers) ops.push_back({{"op", "add_effect"}, {"layer", "id:" + id}, {"type", def->type}});
            }
            e.op = ops;
            e.stepName = std::string("Add Effect ") + def->label + " to " + (layers.size() > 1 ? std::to_string(layers.size()) + " Layers" : targetName);
            e.kind = PreviewItem::Kind::Effect;
            entries.push_back(e);
        }
    }

    // search (English and Japanese) and the category rail with match counts
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", addQuery.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##addsearch", ICON_FA_MAGNIFYING_GLASS " Search (English / 日本語)", buffer, sizeof(buffer))) {
        addQuery = buffer;
        addHighlight = 0;
    }
    std::vector<const AddEntry*> visible;
    std::map<std::string, int> counts;
    std::vector<std::string> categoryOrder;
    for (const AddEntry& e : entries) {
        if (!matches(e, addQuery)) continue;
        const std::string cat = e.section + " \xE2\x80\xBA " + e.category;
        if (!counts.count(cat)) categoryOrder.push_back(cat);
        counts[cat]++;
        if (addCategory.empty() || addCategory == cat) visible.push_back(&e);
    }
    ImGui::BeginChild("##rail", ImVec2(170, -ImGui::GetTextLineHeightWithSpacing() * 2.5f), true);
    if (ImGui::Selectable(("All (" + std::to_string(counts.empty() ? 0 : (int)visible.size()) + ")").c_str(), addCategory.empty())) addCategory.clear();
    for (const std::string& cat : categoryOrder) {
        if (ImGui::Selectable((cat + " (" + std::to_string(counts[cat]) + ")").c_str(), addCategory == cat)) addCategory = cat;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##addlist", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 2.5f), true);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) addHighlight = std::min(addHighlight + 1, (int)visible.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) addHighlight = std::max(0, addHighlight - 1);
    bool arrowed = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    std::string section;
    const AddEntry* hoveredEntry = nullptr;
    const AddEntry* chosen = nullptr;
    for (size_t i = 0; i < visible.size(); i++) {
        const AddEntry& e = *visible[i];
        if (e.section != section) {
            section = e.section;
            ImGui::SeparatorText(section.c_str());
        }
        ImGui::PushID(e.id.c_str());
        if (ImGui::Selectable(e.label.c_str(), (int)i == addHighlight)) chosen = &e;
        if (ImGui::IsItemHovered()) {
            hoveredEntry = &e;
            if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) addHighlight = (int)i;
        }
        if ((int)i == addHighlight && arrowed) ImGui::SetScrollHereY();
        ImGui::SameLine(260);
        ImGui::TextDisabled("%s", e.description.substr(0, 48).c_str());
        ImGui::PopID();
    }
    if (visible.empty()) ImGui::TextDisabled("Nothing matches \"%s\"", addQuery.c_str());
    ImGui::EndChild();
    const AddEntry* highlighted = addHighlight >= 0 && addHighlight < (int)visible.size() ? visible[(size_t)addHighlight] : nullptr;
    if (highlighted) ImGui::TextWrapped("%s", highlighted->description.c_str());
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && highlighted) chosen = highlighted;

    // hovering with intent (150 ms after real movement, R42) or arrowing previews it on the canvas
    const AddEntry* previewing = arrowed ? highlighted : hoveredEntry;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (previewing && ((previewing->previewable && previewing->op.is_object()) || previewing->op.is_array())) {
        if (addHovered != previewing->id) {
            addHovered = previewing->id;
            addHoverSince = arrowed ? now() - 1.0 : now();
            addHoverMouse = mouse;
        }
        const bool moved = arrowed || mouse.x != addHoverMouse.x || mouse.y != addHoverMouse.y || now() - addHoverSince > 0.15;
        if (moved && now() - addHoverSince >= 0.15 && doc.editable() && (!doc.preview() || doc.preview()->id != previewing->id)) {
            PreviewItem item;
            item.kind = previewing->kind;
            item.id = previewing->id;
            item.label = previewing->label;
            item.ops = previewing->op.is_array() ? previewing->op : ta::Json::array({previewing->op});
            item.stepName = previewing->stepName;
            std::string error;
            doc.setPreview(item, &error);
        }
    }
    if (chosen && chosen->id == "layer:vector") {
        if (doc.editable()) {
            doc.clearPreview();
            const std::string file = FileDialogs::openFileDialogFiltered("", "SVG", "svg");
            std::string error;
            if (!file.empty() && !dropFiles({file}, std::string(), &error)) reportError(error);
        }
        addHovered.clear();
        addOpen = false;
        ImGui::CloseCurrentPopup();
    } else if (chosen && doc.editable()) {
        doc.clearPreview();
        const ta::Json ops = chosen->op.is_array() ? chosen->op : ta::Json::array({chosen->op});
        std::string error;
        if (doc.commit(ops, chosen->stepName, &error)) {
            // a new layer is selected; a new text starts editing (R36)
            const std::string layerId = doc.newId(0, "layer");
            if (chosen->section != "Effects" && !layerId.empty()) {
                doc.selectOne({NodeKind::Layer, layerId, {}, {}});
                if (chosen->id == "layer:text") beginTextEdit(layerId, true);
            } else if (chosen->section == "Effects") {
                const std::string effectId = doc.newId(0, "effect");
                if (!effectId.empty() && !layers.empty()) doc.selectOne({NodeKind::Effect, layers.back(), partsSelected && addToParts ? primary.part : std::string(), effectId});
            }
        } else {
            reportError(error);
        }
        addHovered.clear();
        addOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
