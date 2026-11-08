// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Layers panel (docs/specs/S82-title-designer.md R10-R16, D6, D17). The drop zones (row
// halves, the middle 40 % of a group, left of the indent steps out) follow Pachislop's
// LayersPanel / dnd.ts (src/ui/layers/) by SafEight, used with the author's permission; its
// folded companion rows and "generators shown as effects" are not ported (D6).

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/Profile.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/designer/TitleLayerThumbnails.h"
#include "titleart/designer/TitlePages.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    const char* typeIcon(ta::LayerType type) {
        switch (type) {
            case ta::LayerType::Text: return ICON_FA_FONT;
            case ta::LayerType::Shape: return ICON_FA_SHAPES;
            case ta::LayerType::Image: return ICON_FA_IMAGE;
            case ta::LayerType::Vector: return ICON_FA_BEZIER_CURVE;
            case ta::LayerType::Group: return ICON_FA_FOLDER;
            case ta::LayerType::Generator: return ICON_FA_WAND_MAGIC_SPARKLES;
            case ta::LayerType::Adjustment: return ICON_FA_SLIDERS;
            default: return ICON_FA_CIRCLE_QUESTION;
        }
    }

    // one row of the tree, flattened so the list clips (R69)
    struct Row {
        enum class Kind : uint8_t { Layer, PartsHeader, Part, EffectsHeader, Effect, Mask } kind = Kind::Layer;
        const ta::Json* layer = nullptr;
        std::string layerId;
        std::string parentId;
        int depth = 0;
        std::string part;               // Part, Effect (its stack)
        std::string item;               // Effect / Mask id
        const ta::Json* entry = nullptr;    // the effect or mask object
        bool open = false;
        bool hasChildren = false;
        bool clippedToBase = false;
    };

    // the drop zone of a pointer over a layer row (R14)
    enum class Zone : uint8_t { None, Above, Below, Into };

}

void TitleDesignerWindow::drawLayerHeader() {
    JE_PROFILE_SCOPE("TD drawLayerHeader");
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty()) {
        ImGui::TextDisabled("No layer selected");
        return;
    }
    RowContext ctx;
    ctx.host = this;
    ctx.state = &doc;
    ctx.node = {NodeKind::Layer, layers.back(), {}, {}};
    ctx.nodeKind = "layers-header";
    ctx.countNoun = "Layers";
    ctx.readOnly = !doc.editable();
    ctx.readOnlyReason = doc.readOnlyReason();
    // the same two rows head the Properties page: one op each, two places (R13)
    fieldRow(ctx, [&] {
        std::vector<FieldRef> out;
        for (const std::string& id : layers) out.push_back({id, "blend"});
        return out;
    }(), {.label = "Blend"});
    fieldRow(ctx, [&] {
        std::vector<FieldRef> out;
        for (const std::string& id : layers) out.push_back({id, "opacity"});
        return out;
    }(), {.label = "Opacity"});
    if (layers.size() > 1) ImGui::TextDisabled("%zu layers selected", layers.size());
}

void TitleDesignerWindow::drawLayerFooter() {
    const bool editable = doc.editable();
    if (!editable) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_FA_PLUS "###layeradd")) openAddPalette("layers");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Add a layer (Ctrl+Shift+N)");
    ImGui::SameLine();
    if (ImGui::Button("fx###layerfx")) openAddPalette("effects");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(doc.selectedLayers().empty() ? "Select a layer" : "Add an effect to the selection");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_PLUS "###layergroup")) groupSelection();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Group (Ctrl+G)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_TURN_DOWN "###layerclip")) toggleClip();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Create/Release Clipping Mask (Ctrl+Alt+G)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH "###layerdelete")) deleteSelection();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Delete (Delete)");
    if (!editable) ImGui::EndDisabled();
}

void TitleDesignerWindow::layerContextMenu(const std::string& layerId) {
    if (!ImGui::BeginPopupContextItem("##layermenu")) return;
    if (!doc.isSelected({NodeKind::Layer, layerId, {}, {}})) doc.selectOne({NodeKind::Layer, layerId, {}, {}});
    const bool editable = doc.editable();
    const ta::Json* layer = layerById(doc.committed(), layerId);
    const ta::LayerType type = layer ? ta::layerTypeOf(*layer) : ta::LayerType::Unknown;
    if (ImGui::MenuItem("Rename", "F2", false, editable)) runCommand("titleDesigner.rename");
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable)) duplicateSelection();
    if (ImGui::MenuItem("Delete", "Delete", false, editable)) deleteSelection();
    ImGui::Separator();
    if (ImGui::MenuItem("Group", "Ctrl+G", false, editable)) groupSelection();
    if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, editable && type == ta::LayerType::Group)) ungroupSelection();
    const bool clipped = layer && layer->value("clip", false);
    if (ImGui::MenuItem(clipped ? "Release Clipping Mask" : "Create Clipping Mask", "Ctrl+Alt+G", false, editable)) toggleClip();
    if (ta::layerTypeHasStyle(type)) {
        const std::string owner = TitleArtOps::pendingOwner("set_dimension");
        const bool is3d = layer && layer->value("dimension", std::string("2d")) == "3d";
        if (ImGui::MenuItem(is3d ? "Convert to 2D" : "Convert to 3D", nullptr, false, editable && owner.empty())) {
            commitNamed(ta::Json::array({ta::Json{{"op", "set_dimension"}, {"layer", "id:" + layerId}, {"dimension", is3d ? "2d" : "3d"}}}),
                        is3d ? "Set Dimension 2D" : "Set Dimension 3D");
        }
        if (!owner.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("3D titles arrive with %s", owner.c_str());
    }
    ImGui::Separator();
    // Looks (S84): copy / paste / save / revert through its ops
    const bool library = libraryProvider() != nullptr;
    if (ImGui::MenuItem("Copy Look", nullptr, false, layer != nullptr)) {
        ta::Json look = ta::Json::object();
        if (layer) {
            look["style"] = layer->value("style", ta::Json::object());
            look["effects"] = layer->value("effects", ta::Json::array());
            look["part_effects"] = layer->value("part_effects", ta::Json::object());
        }
        const std::string text = ta::Json{{"jasperengine.titleart.look", look}}.dump();
        ImGui::SetClipboardText(text.c_str());
    }
    if (ImGui::MenuItem("Paste Look", nullptr, false, editable)) {
        const char* clip = ImGui::GetClipboardText();
        ta::Json parsed = clip ? ta::Json::parse(clip, nullptr, false) : ta::Json();
        if (parsed.is_object() && parsed.contains("jasperengine.titleart.look")) {
            const ta::Json& look = parsed["jasperengine.titleart.look"];
            ta::Json values = ta::Json::object();
            for (const char* key : {"style", "effects", "part_effects"}) {
                if (look.contains(key)) values[key] = look[key];
            }
            std::vector<FieldEdit> edits;
            for (const std::string& id : doc.selectedLayers()) {
                for (auto it = values.begin(); it != values.end(); ++it) edits.push_back({id, it.key(), it.value()});
            }
            commitNamed(editsToOps(edits), "Paste Look");
        }
    }
    if (ImGui::MenuItem("Save As Look...", nullptr, false, library)) {
        saveAsTab = LibraryTab::Looks;
        saveAsOpen = true;
    }
    if (ImGui::MenuItem("Revert to Look", nullptr, false, library && editable && layer && !str(layer->value("look", ta::Json::object()), "id").empty())) {
        const std::string lookId = str(layer->value("look", ta::Json::object()), "id");
        commitNamed(ta::Json::array({ta::Json{{"op", "apply_look"}, {"layer", "id:" + layerId}, {"look", lookId}}}), "Revert to Look");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Expose Text as Parameter...", nullptr, false, editable && type == ta::LayerType::Text)) exposeField({layerId, "text"}, "Text");
    if (ImGui::MenuItem("Select Layers Using This Material Link", nullptr, false, layer && !str(layer->value("style", ta::Json::object()).value("face", ta::Json::object()), "link").empty())) {
        const std::string link = str(layer->value("style", ta::Json::object()).value("face", ta::Json::object()), "link");
        std::vector<NodeRef> using_;
        ta::forEachLayer(doc.committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            const ta::Json style = l.value("style", ta::Json::object());
            bool uses = str(style.value("face", ta::Json::object()), "link") == link;
            for (const ta::Json& rim : style.value("rims", ta::Json::array())) uses = uses || str(rim.value("material", ta::Json::object()), "link") == link;
            if (uses) using_.push_back({NodeKind::Layer, str(l, "id"), {}, {}});
            return true;
        });
        doc.select(using_);
    }
    if (ImGui::MenuItem("Frame on Canvas", "F")) frameSelection();
    ImGui::EndPopup();
}

void TitleDesignerWindow::drawLayers() {
    JE_PROFILE_SCOPE("TD drawLayers");
    if (newerVersion()) {
        ImGui::TextDisabled("Written by a newer JasperEngine: nothing to show here");
        return;
    }
    if (!doc.isOpen()) {
        ImGui::TextDisabled("No Title Art open");
        return;
    }
    drawLayerHeader();
    ImGui::Separator();
    const ta::TitleArtDoc& shown = doc.shown();
    DocumentView& v = view();
    const std::shared_ptr<const ta::TitleArtDoc> snapshot = doc.shownSnapshot();
    const ta::TitleArtHashes& hashes = shownHashes();

    // flatten the tree: top of the list = front (S80 stores bottom first: D17)
    std::vector<Row> rows;
    std::function<void(const ta::Json&, int, const std::string&)> walk = [&](const ta::Json& list, int depth, const std::string& parentId) {
        if (!list.is_array()) return;
        for (size_t i = list.size(); i-- > 0;) {
            const ta::Json& layer = list[i];
            const std::string id = str(layer, "id");
            const ta::LayerType type = ta::layerTypeOf(layer);
            Row row;
            row.layer = &layer;
            row.layerId = id;
            row.parentId = parentId;
            row.depth = depth;
            const bool group = type == ta::LayerType::Group;
            // groups open by default (their `collapsed` gives the first state), others closed (R11)
            auto twirl = v.twirls.find(id);
            row.open = twirl != v.twirls.end() ? twirl->second : group ? !layer.value("collapsed", false) : false;
            auto nonEmpty = [&](const char* key) {
                auto it = layer.find(key);
                return it != layer.end() && it->is_array() && !it->empty();
            };
            row.hasChildren = group ? !ta::layerChildren(layer).empty() : (ta::layerTypeHasStyle(type) || nonEmpty("effects") || nonEmpty("masks"));
            row.clippedToBase = layer.value("clip", false);
            rows.push_back(row);
            if (!row.open) continue;
            if (group) {
                walk(ta::layerChildren(layer), depth + 1, id);
                continue;
            }
            if (ta::layerTypeHasStyle(type)) {
                Row header;
                header.kind = Row::Kind::PartsHeader;
                header.layer = &layer;
                header.layerId = id;
                header.depth = depth + 1;
                rows.push_back(header);
                Row face = header;
                face.kind = Row::Kind::Part;
                face.part = "face";
                face.depth = depth + 2;
                rows.push_back(face);
                const ta::Json& rims = layer.contains("style") && layer["style"].contains("rims") ? layer["style"]["rims"] : ta::Json::array();
                for (size_t r = 0; r < rims.size(); r++) {
                    Row rim = face;
                    rim.part = "rim:" + str(rims[r], "id");
                    rows.push_back(rim);
                    if (layer.contains("part_effects") && layer["part_effects"].contains(rim.part)) {
                        for (const ta::Json& effect : layer["part_effects"][rim.part]) {
                            Row e = rim;
                            e.kind = Row::Kind::Effect;
                            e.item = str(effect, "id");
                            e.entry = &effect;
                            e.depth = depth + 3;
                            rows.push_back(e);
                        }
                    }
                }
                if (layer.contains("part_effects") && layer["part_effects"].contains("face")) {
                    for (const ta::Json& effect : layer["part_effects"]["face"]) {
                        Row e = face;
                        e.kind = Row::Kind::Effect;
                        e.item = str(effect, "id");
                        e.entry = &effect;
                        e.depth = depth + 3;
                        rows.push_back(e);
                    }
                }
            }
            if (layer.contains("effects") && !layer["effects"].empty()) {
                Row header;
                header.kind = Row::Kind::EffectsHeader;
                header.layer = &layer;
                header.layerId = id;
                header.depth = depth + 1;
                rows.push_back(header);
                for (const ta::Json& effect : layer["effects"]) {
                    Row e = header;
                    e.kind = Row::Kind::Effect;
                    e.item = str(effect, "id");
                    e.entry = &effect;
                    e.depth = depth + 2;
                    rows.push_back(e);
                }
            }
            if (layer.contains("masks")) {
                for (const ta::Json& mask : layer["masks"]) {
                    Row m;
                    m.kind = Row::Kind::Mask;
                    m.layer = &layer;
                    m.layerId = id;
                    m.item = str(mask, "id");
                    m.entry = &mask;
                    m.depth = depth + 1;
                    rows.push_back(m);
                }
            }
        }
    };
    {
        JE_PROFILE_SCOPE("TD drawLayers rows");
        walk(shown.layers(), 0, std::string());
    }
    JE_PROFILE_SCOPE("TD drawLayers tree");

    shownRows.clear();
    for (const Row& row : rows) {
        switch (row.kind) {
            case Row::Kind::Layer: shownRows.push_back({NodeKind::Layer, row.layerId, {}, {}}); break;
            case Row::Kind::Part: shownRows.push_back({NodeKind::Part, row.layerId, row.part, {}}); break;
            case Row::Kind::Effect: shownRows.push_back({NodeKind::Effect, row.layerId, row.part, row.item}); break;
            case Row::Kind::Mask: shownRows.push_back({NodeKind::Mask, row.layerId, {}, row.item}); break;
            default: shownRows.push_back({}); break;
        }
    }

    const float footer = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("##layertree", ImVec2(0, -footer), false);
    const float rowHeight = std::max(ImGui::GetFrameHeight(), 26.0f * ImGui::GetFontSize() / 16.0f);
    const float gutter = ImGui::GetFontSize() * 2.6f;
    const float indent = ImGui::GetFontSize() * 1.0f;
    const bool editable = doc.editable();
    const bool treeFocused = ImGui::IsWindowFocused();

    // selecting elsewhere opens the rows that show it and scrolls to it (R11)
    const NodeRef primary = doc.primary();

    ImGuiListClipper clipper;
    clipper.Begin((int)rows.size(), rowHeight);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            const Row& row = rows[(size_t)r];
            ImGui::PushID(r);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const NodeRef node = shownRows[(size_t)r];
            const bool selected = node.kind != NodeKind::Document && doc.isSelected(node);

            if (row.kind == Row::Kind::Layer) {
                const ta::Json& layer = *row.layer;
                const ta::LayerType type = ta::layerTypeOf(layer);
                // eye and lock gutter (left of the indent, HierarchyVisibilityGutter's place)
                bool visible = layer.value("visible", true);
                bool locked = layer.value("locked", false);
                ImGui::SetCursorScreenPos(start);
                if (!editable) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                if (ImGui::SmallButton(visible ? ICON_FA_EYE "##eye" : ICON_FA_EYE_SLASH "##eye")) {
                    commitNamed(editsToOps({{row.layerId, "visible", !visible}}), visible ? "Hide Layer" : "Show Layer");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(visible ? "Hide (H)" : "Show (H)");
                ImGui::SameLine(0, 1);
                if (ImGui::SmallButton(locked ? ICON_FA_LOCK "##lock" : ICON_FA_LOCK_OPEN "##lock")) {
                    commitNamed(editsToOps({{row.layerId, "locked", !locked}}), locked ? "Unlock Layer" : "Lock Layer");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(locked ? "Unlock (L)" : "Lock (L)");
                ImGui::PopStyleColor();
                if (!editable) ImGui::EndDisabled();

                // the row: twirl, thumbnail, name, badges
                const float x0 = start.x + gutter + row.depth * indent;
                ImGui::SetCursorScreenPos(ImVec2(x0, start.y));
                if (row.hasChildren) {
                    if (ImGui::ArrowButton("##twirl", row.open ? ImGuiDir_Down : ImGuiDir_Right)) {
                        const bool openNow = !row.open;
                        if (ImGui::GetIO().KeyAlt) {
                            // Alt-click opens or closes every sibling (R11)
                            for (const Row& other : rows) {
                                if (other.kind == Row::Kind::Layer && other.parentId == row.parentId) v.twirls[other.layerId] = openNow;
                            }
                        } else {
                            v.twirls[row.layerId] = openNow;
                        }
                    }
                } else {
                    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
                }
                ImGui::SameLine(0, 4);
                // thumbnail: the layer alone (R15)
                const float thumb = 24.0f * ImGui::GetFontSize() / 16.0f;
                const ImVec2 tp = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(tp, ImVec2(tp.x + thumb, tp.y + thumb), IM_COL32(60, 60, 64, 255), 3.0f);
                if (thumbnails && type != ta::LayerType::Unknown) {
                    auto hash = hashes.layers.find(row.layerId);
                    const TitleRect bounds = layerBounds(row.layerId);
                    const int pixels = (int)std::round(thumb * std::min(2.0f, displayScale));
                    ImTextureID texture = thumbnails->thumbnail(snapshot, row.layerId, bounds, hash != hashes.layers.end() ? hash->second.content : 0, pixels, true);
                    if (texture) dl->AddImage(texture, tp, ImVec2(tp.x + thumb, tp.y + thumb));
                }
                ImGui::Dummy(ImVec2(thumb, thumb));
                ImGui::SameLine(0, 6);
                if (row.clippedToBase) {
                    ImGui::TextDisabled(ICON_FA_ARROW_TURN_DOWN);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clipped to the layer below");
                    ImGui::SameLine(0, 4);
                }
                // name (or the rename field)
                const std::string name = str(layer, "name");
                const float nameX = ImGui::GetCursorScreenPos().x;
                if (renaming == row.layerId) {
                    ImGui::SetNextItemWidth(std::max(60.0f, start.x + width - nameX - 4.0f));
                    if (renameFocus) {
                        ImGui::SetKeyboardFocusHere();
                        renameFocus = false;
                    }
                    char buffer[256];
                    std::snprintf(buffer, sizeof(buffer), "%s", renameBuffer.c_str());
                    const bool enter = ImGui::InputText("##rename", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    renameBuffer = buffer;
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming.clear();
                    else if (enter || ImGui::IsItemDeactivated()) {
                        if (renameBuffer != name && !renameBuffer.empty()) commitNamed(editsToOps({{row.layerId, "name", renameBuffer}}), "Rename Layer");
                        renaming.clear();
                    }
                } else {
                    ImGui::SetCursorScreenPos(ImVec2(start.x + gutter, start.y));
                    const bool clicked = ImGui::Selectable(("##row" + row.layerId).c_str(), selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                                           ImVec2(width - gutter, rowHeight));
                    const bool hovered = ImGui::IsItemHovered();
                    if (clicked) {
                        ImGuiIO& io = ImGui::GetIO();
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            if (type == ta::LayerType::Text) beginTextEdit(row.layerId, false);
                            else runCommand("titleDesigner.rename");
                        } else if (io.KeyCtrl || io.KeySuper) {
                            doc.toggle(node);
                        } else if (io.KeyShift && !rangeAnchor.empty()) {
                            // the range of shown rows (R12)
                            int a = -1, b = r;
                            for (size_t k = 0; k < shownRows.size(); k++) {
                                if (shownRows[k].kind == NodeKind::Layer && shownRows[k].layer == rangeAnchor) a = (int)k;
                            }
                            if (a < 0) a = r;
                            std::vector<NodeRef> range;
                            for (int k = std::min(a, b); k <= std::max(a, b); k++) {
                                if (shownRows[(size_t)k].kind == NodeKind::Layer) range.push_back(shownRows[(size_t)k]);
                            }
                            doc.select(range);
                        } else {
                            selectNode(node);
                            rangeAnchor = row.layerId;
                        }
                    }
                    layerContextMenu(row.layerId);
                    // drag: a selected row carries the selection; an unselected one is selected first (R14)
                    if (editable && ImGui::BeginDragDropSource()) {
                        if (!selected) {
                            doc.selectOne(node);
                        }
                        dragLayer = row.layerId;
                        dragSet.clear();
                        for (const std::string& id : doc.selectedLayers()) dragSet.insert(id);
                        ImGui::SetDragDropPayload("titleart_layers", row.layerId.c_str(), row.layerId.size() + 1);
                        ImGui::Text("%s %s", typeIcon(type), dragSet.size() > 1 ? (std::to_string(dragSet.size()) + " layers").c_str() : name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const float t = (mouse.y - start.y) / rowHeight;
                        const bool group = type == ta::LayerType::Group;
                        Zone zone = t < 0.5f ? Zone::Above : Zone::Below;
                        if (group && t > 0.3f && t < 0.7f) zone = Zone::Into;
                        // left of the indent: step out of that many groups
                        const int stepsOut = row.depth > 0 ? std::clamp((int)((x0 - mouse.x) / indent), 0, row.depth) : 0;
                        bool refused = false;
                        for (const std::string& id : dragSet) {
                            std::string walkId = row.layerId;
                            while (!walkId.empty()) {
                                if (walkId == id) refused = true;
                                walkId = parentLayerOf(shown, walkId);
                            }
                        }
                        if (refused) {
                            ImGui::SetTooltip("A group cannot go into itself");
                        } else {
                            const float lineY = zone == Zone::Above ? start.y : start.y + rowHeight;
                            if (zone == Zone::Into) dl->AddRect(start, ImVec2(start.x + width, start.y + rowHeight), IM_COL32(90, 160, 255, 255), 3.0f, 0, 2.0f);
                            else dl->AddLine(ImVec2(x0 - stepsOut * indent, lineY), ImVec2(start.x + width, lineY), IM_COL32(90, 160, 255, 255), 2.0f);
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("titleart_layers")) {
                                (void)payload;
                                std::vector<std::string> moving(dragSet.begin(), dragSet.end());
                                // the list is front first: "above" is after in S80's order
                                std::string target = row.layerId;
                                for (int s = 0; s < stepsOut && zone != Zone::Into; s++) {
                                    const std::string parent = parentLayerOf(shown, target);
                                    if (parent.empty()) break;
                                    target = parent;
                                }
                                moveLayers(moving, target, zone == Zone::Into ? "into" : zone == Zone::Above ? "above" : "below");
                            }
                        }
                        // Project window drops onto a row: images, SVGs, fonts (R14)
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                            const std::string text((const char*)payload->Data, (size_t)payload->DataSize);
                            std::vector<std::string> files;
                            std::string current;
                            for (char c : text) {
                                if (c == '\n' || c == '\0') {
                                    if (!current.empty()) files.push_back(current);
                                    current.clear();
                                } else {
                                    current += c;
                                }
                            }
                            if (!current.empty()) files.push_back(current);
                            std::string error;
                            if (!dropFiles(files, type == ta::LayerType::Text ? row.layerId : std::string(), &error)) reportError(error);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    // name and badges over the selectable
                    ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    if (!visible) textColor.w *= 0.5f;
                    const float y = start.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
                    float x = nameX;
                    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(textColor), typeIcon(type));
                    x += ImGui::GetFontSize() * 1.4f;
                    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(textColor), name.c_str());
                    std::string badges;
                    if (layer.value("dimension", std::string("2d")) == "3d") badges += " 3D";
                    if (!layer.value("effects", ta::Json::array()).empty() || !layer.value("part_effects", ta::Json::object()).empty()) badges += " fx";
                    if (layer.value("guide", false)) badges += " G";
                    if (type == ta::LayerType::Generator) badges += " GEN";
                    bool exposed = false;
                    for (const ta::Json& e : shown.root.value("exposed", ta::Json::array())) {
                        for (const ta::Json& target : e.value("targets", ta::Json::array())) exposed = exposed || str(target, "layer") == row.layerId;
                    }
                    if (exposed) badges += " " ICON_FA_LINK;
                    const ImVec2 badgeSize = ImGui::CalcTextSize(badges.c_str());
                    dl->AddText(ImVec2(start.x + width - badgeSize.x - 6.0f, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), badges.c_str());
                    if (type == ta::LayerType::Unknown) {
                        dl->AddText(ImVec2(start.x + width - badgeSize.x - 24.0f, y), IM_COL32(240, 170, 50, 255), ICON_FA_TRIANGLE_EXCLAMATION);
                        if (hovered) ImGui::SetTooltip("Unknown layer type '%s': kept as it is, not drawn", str(layer, "type").c_str());
                    }
                    if (row.layerId == primary.layer && primary.kind == NodeKind::Layer && pendingScrollRow == "layer:" + row.layerId) {
                        ImGui::SetScrollHereY(0.5f);
                        pendingScrollRow.clear();
                    }
                }
                ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rowHeight));
            } else {
                // parts, effects, masks: child rows
                const float x0 = start.x + gutter + row.depth * indent;
                std::string label;
                if (row.kind == Row::Kind::PartsHeader) label = "Parts";
                else if (row.kind == Row::Kind::EffectsHeader) label = "Effects (applied top to bottom)";
                else if (row.kind == Row::Kind::Part) label = partLabel(*row.layer, row.part);
                else if (row.kind == Row::Kind::Effect) {
                    const ta::EffectDef* def = ta::findEffect(str(*row.entry, "type"));
                    label = std::string(ICON_FA_WAND_SPARKLES " ") + (def ? def->label : str(*row.entry, "type"));
                    if (!row.entry->value("enabled", true)) label += " (off)";
                } else {
                    label = std::string(ICON_FA_MASKS_THEATER " ") + str(*row.entry, "type") + " mask";
                }
                ImGui::SetCursorScreenPos(ImVec2(start.x + gutter, start.y));
                const bool header = row.kind == Row::Kind::PartsHeader || row.kind == Row::Kind::EffectsHeader;
                if (header) {
                    ImGui::SetCursorScreenPos(ImVec2(x0, start.y + (rowHeight - ImGui::GetFontSize()) * 0.5f));
                    ImGui::TextDisabled("%s", label.c_str());
                } else {
                    if (ImGui::Selectable("##child", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(width - gutter, rowHeight))) {
                        // parts multi-select inside one layer; effects one at a time (R12)
                        if (row.kind == Row::Kind::Part && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && doc.primary().layer == row.layerId &&
                            doc.primary().kind == NodeKind::Part) {
                            doc.toggle(node);
                        } else {
                            selectNode(node);
                        }
                    }
                    if (row.kind == Row::Kind::Effect && editable && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("titleart_effect", row.item.c_str(), row.item.size() + 1);
                        ImGui::Text("%s", label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if ((row.kind == Row::Kind::Effect || row.kind == Row::Kind::Part) && ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("titleart_effect")) {
                            // an effect dropped on another effect of the stack, or on a part / layer (R14)
                            const std::string effectId((const char*)payload->Data);
                            NodeRef source;
                            ta::forEachLayer(shown.root, [&](const ta::Json& l, const ta::Json*, int) {
                                NodeRef probe{NodeKind::Effect, str(l, "id"), {}, effectId};
                                if (effectOf(l, probe)) source = probe;
                                for (auto it = l.value("part_effects", ta::Json::object()).begin(); it != l.value("part_effects", ta::Json::object()).end(); ++it) {
                                    NodeRef p{NodeKind::Effect, str(l, "id"), it.key(), effectId};
                                    if (effectOf(l, p)) source = p;
                                }
                                return true;
                            });
                            const ta::Json* sourceLayer = layerById(shown, source.layer);
                            const ta::Json* effect = sourceLayer ? effectOf(*sourceLayer, source) : nullptr;
                            if (effect) {
                                const bool copy = ImGui::GetIO().KeyCtrl;
                                const std::string targetPart = row.kind == Row::Kind::Part ? row.part : row.part;
                                ta::Json ops = ta::Json::array();
                                const std::string sourcePath = nodePath(shown, source);
                                const std::string sourceEffect = sourcePath.substr(layerPathOf(shown, source.layer).size() + 1);
                                if (source.layer == row.layerId && source.part == targetPart && row.kind == Row::Kind::Effect && !copy) {
                                    // within the stack: to the target's index
                                    int index = 0;
                                    const ta::Json& list = targetPart.empty() ? (*row.layer)["effects"] : (*row.layer)["part_effects"][targetPart];
                                    for (size_t k = 0; k < list.size(); k++) {
                                        if (str(list[k], "id") == row.item) index = (int)k;
                                    }
                                    ops.push_back({{"op", "move_effect"}, {"layer", "id:" + row.layerId}, {"effect", sourceEffect}, {"index", index}});
                                } else {
                                    if (!copy) ops.push_back({{"op", "remove_effect"}, {"layer", "id:" + source.layer}, {"effect", sourceEffect}});
                                    ta::Json add = {{"op", "add_effect"}, {"layer", "id:" + row.layerId}, {"type", str(*effect, "type")}, {"params", effect->value("params", ta::Json::object())}};
                                    if (!targetPart.empty()) add["part"] = targetPart;
                                    ops.push_back(add);
                                }
                                commitNamed(ops, copy ? "Copy Effect" : "Move Effect");
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (row.kind == Row::Kind::Effect || row.kind == Row::Kind::Part) {
                        if (ImGui::BeginPopupContextItem("##childmenu")) {
                            if (row.kind == Row::Kind::Effect) {
                                if (ImGui::MenuItem("Remove Effect", "Delete", false, editable)) {
                                    selectNode(node);
                                    deleteSelection();
                                }
                            } else if (row.part.rfind("rim:", 0) == 0) {
                                if (ImGui::MenuItem("Remove Rim", nullptr, false, editable)) {
                                    const std::string rimPath = nodePath(shown, node);
                                    commitNamed(ta::Json::array({ta::Json{{"op", "remove_rim"}, {"layer", "id:" + row.layerId}, {"rim", rimPath.substr(rimPath.rfind('/') + 1)}}}),
                                                "Remove Rim");
                                }
                            }
                            if (ImGui::MenuItem("Add Effect...", nullptr, false, editable)) {
                                selectNode(node);
                                openAddPalette("effects");
                            }
                            ImGui::EndPopup();
                        }
                    }
                    // colour swatch of a part's fill, link icon
                    float x = x0;
                    const float y = start.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
                    if (row.kind == Row::Kind::Part) {
                        const ta::Json* material = nullptr;
                        if (row.part == "face") {
                            if (row.layer->contains("style") && (*row.layer)["style"].contains("face")) material = &(*row.layer)["style"]["face"];
                        } else {
                            const ta::Json& rims = (*row.layer)["style"]["rims"];
                            for (const ta::Json& rim : rims) {
                                if ("rim:" + str(rim, "id") == row.part && rim.contains("material")) material = &rim["material"];
                            }
                        }
                        ta::TitleColor c;
                        if (material) ta::parseTitleColor(material->value("fill", ta::Json::object()).value("color", ta::Json("#ffffff")), c);
                        const float s = ImGui::GetFontSize() * 0.8f;
                        dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + s, y + 1 + s), ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
                        x += s + 6.0f;
                        if (material && !str(*material, "link").empty()) {
                            dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_LINK);
                            x += ImGui::GetFontSize() * 1.2f;
                        }
                    }
                    if (row.kind == Row::Kind::Effect) {
                        // the enable eye
                        const bool enabled = row.entry->value("enabled", true);
                        ImGui::SetCursorScreenPos(ImVec2(start.x + 2.0f, start.y));
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        if (!editable) ImGui::BeginDisabled();
                        if (ImGui::SmallButton(enabled ? ICON_FA_EYE "##fxeye" : ICON_FA_EYE_SLASH "##fxeye")) {
                            const std::string path = effectPathOf(*row.layer, node);
                            commitNamed(editsToOps({{row.layerId, path + ".enabled", !enabled}}), enabled ? "Disable Effect" : "Enable Effect");
                        }
                        if (!editable) ImGui::EndDisabled();
                        ImGui::PopStyleColor();
                    }
                    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                }
                ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + rowHeight));
            }
            ImGui::PopID();
        }
    }
    clipper.End();
    // below the last row: the bottom at top level (R14)
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(rowHeight, ImGui::GetContentRegionAvail().y)));
    if (ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload("titleart_layers")) {
            const ta::Json& top = shown.layers();
            if (!top.empty()) moveLayers(std::vector<std::string>(dragSet.begin(), dragSet.end()), str(top[0], "id"), "below");
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) doc.select({});
    // panel keys (R16): Up/Down move, Left/Right close/open/parent
    if (treeFocused && renaming.empty() && !ImGui::GetIO().WantTextInput) {
        int at = -1;
        for (size_t k = 0; k < shownRows.size(); k++) {
            if (shownRows[k] == primary) at = (int)k;
        }
        auto selectRow = [&](int k) {
            while (k >= 0 && k < (int)shownRows.size() && shownRows[(size_t)k].kind == NodeKind::Document) k += (k > at ? 1 : -1);
            if (k >= 0 && k < (int)shownRows.size()) selectNode(shownRows[(size_t)k]);
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) selectRow(at + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) selectRow(std::max(0, at - 1));
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && primary.kind == NodeKind::Layer) v.twirls[primary.layer] = true;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            if (primary.kind == NodeKind::Layer && v.twirls[primary.layer]) v.twirls[primary.layer] = false;
            else doc.selectParent();
        }
    }
    ImGui::EndChild();
    drawLayerFooter();
}
