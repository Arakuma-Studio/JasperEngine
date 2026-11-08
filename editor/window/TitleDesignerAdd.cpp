// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The unified Add palette (docs/specs/S82-title-designer.md R17, D6; S83 §6.10, R41, R42): one
// searchable popover for layers, generators and effects with a category rail, hover/arrow preview,
// cost badges and a header naming the target. Its entries are TitleAddCatalog's. Pachislop's
// AddEffectMenu (src/ui/properties/AddEffectMenu.tsx) by SafEight, used with the author's
// permission, is the model; its separate three-level generator flyout is not.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/Profile.h"
#include "titleart/designer/TitleAddCatalog.h"

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

}

AddTarget TitleDesignerWindow::addTarget() const {
    AddTarget target;
    const ta::TitleArtDoc& shown = doc.shown();
    target.layers = doc.selectedLayers();
    const ta::Json* layer = target.layers.empty() ? nullptr : layerById(shown, target.layers.back());
    if (layer) {
        target.name = str(*layer, "name");
        target.hasStyle = ta::layerTypeHasStyle(ta::layerTypeOf(*layer));
        target.is3D = layer->value("dimension", std::string("2d")) == "3d";
    }
    if (doc.primary().kind == NodeKind::Part && addToParts) {
        for (const NodeRef& node : doc.selection()) {
            if (node.kind == NodeKind::Part) target.parts.push_back(node);
        }
    }
    return target;
}

void TitleDesignerWindow::drawAddPalette() {
    JE_PROFILE_SCOPE("TD drawAddPalette");
    if (addOpenRequested) {
        ImGui::OpenPopup("Add###addpalette");
        addOpenRequested = false;
        addOpen = true;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Add###addpalette")) {
        if (addOpen) {
            // Esc / a click outside closes and restores
            addOpen = false;
            if (doc.previewActive() && doc.preview() && doc.preview()->kind != PreviewItem::Kind::Font) doc.clearPreview();
            addHovered.clear();
        }
        return;
    }
    if (addCloseRequested) {
        addCloseRequested = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const NodeRef primary = doc.primary();
    const bool partsSelected = primary.kind == NodeKind::Part;
    const AddTarget target = addTarget();

    // the header names the target, and offers parts or the whole layer
    if (!target.layers.empty()) {
        ImGui::Text("Adds to \"%s\"", target.layers.size() > 1 ? (std::to_string(target.layers.size()) + " layers").c_str() : target.name.c_str());
        if (partsSelected) {
            ImGui::SameLine();
            if (ImGui::RadioButton("Selected parts", addToParts)) addToParts = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Whole layer", !addToParts)) addToParts = false;
        }
    } else {
        ImGui::TextDisabled("Effects need a selected layer");
    }

    const std::vector<AddEntry> entries = buildAddEntries(addFilter, target);

    // search (English and Japanese) and the category rail with match counts, in §6.10's order
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", addQuery.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##addsearch", ICON_FA_MAGNIFYING_GLASS " Search (English / \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E)", buffer, sizeof(buffer))) {
        addQuery = buffer;
        addHighlight = 0;
    }
    std::vector<const AddEntry*> visible;
    std::map<std::string, int> counts;
    std::vector<std::string> categoryOrder;
    int matched = 0;
    for (const AddEntry& e : entries) {
        if (!addEntryMatches(e, addQuery)) continue;
        matched++;
        const std::string cat = std::string(addSectionName(e.section)) + " \xE2\x80\xBA " + e.category;
        if (!counts.count(cat)) categoryOrder.push_back(cat);
        counts[cat]++;
        if (addCategory.empty() || addCategory == cat) visible.push_back(&e);
    }
    addRail.clear();
    for (const std::string& cat : categoryOrder) addRail.push_back({cat, counts[cat]});
    addVisible.clear();
    for (const AddEntry* e : visible) addVisible.push_back(e->id);
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.5f;
    ImGui::BeginChild("##rail", ImVec2(200, -footer), true);
    if (ImGui::Selectable(("All (" + std::to_string(matched) + ")###railall").c_str(), addCategory.empty())) addCategory.clear();
    for (const std::string& cat : categoryOrder) {
        if (ImGui::Selectable((cat + " (" + std::to_string(counts[cat]) + ")###rail" + cat).c_str(), addCategory == cat)) addCategory = cat;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##addlist", ImVec2(0, -footer), true);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) addHighlight = std::min(addHighlight + 1, (int)visible.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) addHighlight = std::max(0, addHighlight - 1);
    const bool arrowed = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    std::string heading;
    const AddEntry* hoveredEntry = nullptr;
    const AddEntry* chosen = nullptr;
    for (size_t i = 0; i < visible.size(); i++) {
        const AddEntry& e = *visible[i];
        const std::string h = std::string(addSectionName(e.section)) + (e.category == FOLLOW_CATEGORY ? std::string(" \xC2\xB7 ") + FOLLOW_CATEGORY : std::string());
        if (h != heading) {
            heading = h;
            ImGui::SeparatorText(heading.c_str());
        }
        ImGui::PushID(e.id.c_str());
        if (e.depth > 0) ImGui::Indent(14.0f);
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable((e.label + "###entry").c_str(), (int)i == addHighlight)) chosen = &e;
        if (ImGui::IsItemHovered()) {
            hoveredEntry = &e;
            if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) addHighlight = (int)i;
        }
        if ((int)i == addHighlight && arrowed) ImGui::SetScrollHereY();
        // the note (or the description) and the cost badge on the row (S83 R41)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = rowStart.y;
        const std::string side = !e.note.empty() ? e.note : e.description.substr(0, 46);
        dl->AddText(ImVec2(rowStart.x + 210.0f - (e.depth > 0 ? 14.0f : 0.0f), y), ImGui::GetColorU32(ImGuiCol_TextDisabled), side.c_str());
        if (!e.cost.empty()) {
            CostBadge badge;
            badge.cls = e.cost;
            const float bx = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("C0").x - 6.0f;
            dl->AddText(ImVec2(bx, y), ImGui::ColorConvertFloat4ToU32(costBadgeColor(badge)), e.cost.c_str());
        }
        if (e.depth > 0) ImGui::Unindent(14.0f);
        ImGui::PopID();
    }
    if (visible.empty()) ImGui::TextDisabled("Nothing matches \"%s\"", addQuery.c_str());
    ImGui::EndChild();
    const AddEntry* highlighted = addHighlight >= 0 && addHighlight < (int)visible.size() ? visible[(size_t)addHighlight] : nullptr;
    if (highlighted) {
        std::string footerText = highlighted->description;
        if (!highlighted->cost.empty()) footerText += "  (cost " + highlighted->cost + ")";
        ImGui::TextWrapped("%s", footerText.c_str());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && highlighted) chosen = highlighted;

    // hovering with intent (150 ms after real movement, R42) or arrowing previews it on the canvas
    const AddEntry* previewing = arrowed ? highlighted : hoveredEntry;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (previewing && !previewing->ops.empty()) {
        if (addHovered != previewing->id) {
            addHovered = previewing->id;
            addHoverSince = arrowed ? now() - 1.0 : now();
            addHoverMouse = mouse;
        }
        const bool moved = arrowed || mouse.x != addHoverMouse.x || mouse.y != addHoverMouse.y || now() - addHoverSince > 0.15;
        if (moved && now() - addHoverSince >= 0.15 && doc.editable() && (!doc.preview() || doc.preview()->id != previewing->id)) {
            previewAddEntry(*previewing);
        }
    }
    if (chosen && doc.editable()) {
        std::string error;
        if (!addEntry(*chosen, &error)) reportError(error);
        addHovered.clear();
        addOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool TitleDesignerWindow::previewAddEntry(const AddEntry& entry) {
    PreviewItem item;
    item.kind = entry.kind;
    item.id = entry.id;
    item.label = entry.label;
    item.ops = entry.ops;
    item.stepName = entry.stepName;
    std::string error;
    return doc.setPreview(item, &error);
}

bool TitleDesignerWindow::addEntry(const AddEntry& entry, std::string* error) {
    const std::vector<std::string> layers = doc.selectedLayers();
    const NodeRef primary = doc.primary();
    doc.clearPreview();
    if (!doc.commit(entry.ops, entry.stepName, error)) return false;
    // a new layer is selected (a new text starts editing, R36); an effect is; a style setting
    // selects its layer and opens its section
    if (entry.addsLayer) {
        const std::vector<std::string> added = doc.newIds("layer");
        if (!added.empty()) {
            doc.selectOne({NodeKind::Layer, added.back(), {}, {}});
            if (entry.startsTextEdit) beginTextEdit(added.back(), true);
        }
    } else if (!entry.openSections.empty()) {
        if (!layers.empty()) doc.selectOne({NodeKind::Layer, layers.back(), {}, {}});
        forceOpenSections = entry.openSections;
    } else if (entry.section == AddEntry::Section::Effects) {
        const std::string effectId = doc.newId(0, "effect");
        if (!effectId.empty() && !layers.empty()) {
            const bool toParts = primary.kind == NodeKind::Part && addToParts;
            doc.selectOne({NodeKind::Effect, toParts ? primary.layer : layers.back(), toParts ? primary.part : std::string(), effectId});
        }
    }
    return true;
}
