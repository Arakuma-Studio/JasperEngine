// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Library panel (docs/specs/S82-title-designer.md R40-R49) over S84's data (the provider of
// TitleDesignerHooks.h). Tiles on the user's own content, hover intent (150 ms after real pointer
// movement), the preview as a transient state, and the second click of a double-click ignored,
// follow Pachislop's LibraryPanel (src/ui/library/) and its previewOverride (src/store/store.ts)
// by SafEight, used with the author's permission; its tile work ran without a frame budget and
// outlived its dialog (report p7), the provider's tiles here go through the preview queue.

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/TitleArtHash.h"
#include "titleart/designer/TitleSearchFold.h"

#include "imgui.h"

#include <algorithm>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    bool itemMatches(const LibraryItem& item, const std::string& foldedQuery) {
        if (foldedQuery.empty()) return true;
        if (foldForSearch(item.name).find(foldedQuery) != std::string::npos) return true;
        if (foldForSearch(item.family).find(foldedQuery) != std::string::npos) return true;
        for (const std::string& t : item.terms) {
            if (foldForSearch(t).find(foldedQuery) != std::string::npos) return true;
        }
        return false;
    }

}

LibraryApplyOptions TitleDesignerWindow::libraryOptions(LibraryTab tab) const {
    LibraryApplyOptions options;
    const DocumentView* v = viewOrNull();
    options.applyFont = v && v->applyFont;
    options.keepColorScheme = !v || v->keepColorScheme;
    const std::vector<std::string> layers = doc.selectedLayers();
    if (tab == LibraryTab::ColorSchemes) {
        // Whole Title when the selection is empty or a group, Selected Layers otherwise (R45)
        bool groupOnly = !layers.empty();
        for (const std::string& id : layers) {
            const ta::Json* layer = layerById(doc.committed(), id);
            groupOnly = groupOnly && layer && ta::layerTypeOf(*layer) == ta::LayerType::Group;
        }
        const std::string scope = v ? v->colorSchemeScope : std::string();
        options.wholeTitle = scope == "whole" || (scope.empty() && (layers.empty() || groupOnly));
    }
    if (!options.wholeTitle) options.layers = layers;
    if (options.layers.empty() && tab == LibraryTab::Looks) {
        // nothing selected: the topmost text layer; with none, the Look adds a text (R41)
        std::string top;
        ta::forEachLayer(doc.committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            if (ta::layerTypeOf(l) == ta::LayerType::Text) top = str(l, "id");
            return true;
        });
        if (!top.empty()) options.layers.push_back(top);
    }
    return options;
}

std::string TitleDesignerWindow::libraryTargetKey() const {
    // the target layer's content key: moving it or editing another layer invalidates no tile (R46)
    const LibraryApplyOptions options = libraryOptions(LibraryTab::Looks);
    if (options.layers.empty()) return "sample";
    const ta::TitleArtHashes hashes = ta::hashTitleArt(doc.shown());
    auto it = hashes.layers.find(options.layers.back());
    return it == hashes.layers.end() ? std::string("sample") : std::to_string(it->second.content);
}

bool TitleDesignerWindow::applyLibraryItem(LibraryTab tab, const LibraryItem& item, bool preview) {
    TitleLibraryProvider* library = libraryProvider();
    if (!library || !doc.editable()) return false;
    if (!item.disabledReason.empty()) {
        reportError(item.disabledReason);
        return false;
    }
    const LibraryApplyOptions options = libraryOptions(tab);
    PreviewItem p;
    p.kind = tab == LibraryTab::Looks ? PreviewItem::Kind::Look : tab == LibraryTab::ColorSchemes ? PreviewItem::Kind::ColorScheme : PreviewItem::Kind::Template;
    p.id = item.id;
    p.label = item.name;
    // the same S84 function, the same arguments, the same document (R44)
    p.ops = library->applyOps(tab, item, options);
    p.stepName = library->stepName(tab, item, options);
    std::string error;
    if (preview) return doc.setPreview(p, &error);
    // templates on a document with content first ask (R43)
    if (tab == LibraryTab::Templates && !contentEmpty() && pendingTemplate != item.id) {
        pendingTemplate = item.id;
        return false;
    }
    pendingTemplate.clear();
    const ta::TitleArtDoc before = doc.committed();
    bool ok = false;
    if (doc.preview() && doc.preview()->id == item.id && doc.preview()->ops == p.ops) ok = doc.commitPreview(&error);
    else ok = doc.commit(p.ops, p.stepName, &error);
    if (!ok) {
        reportError(error);
        return false;
    }
    library->recordApply(tab, item.id);
    // the structure a Look adds is named in a toast; the applied layer stays selected (R75)
    const std::string note = library->structureNote(before, doc.committed());
    if (!note.empty()) {
        toasts.push_back(p.stepName + " " + note);
        toastUntil = now() + 6.0;
    }
    if (tab == LibraryTab::Templates) {
        // the layer of the template's `title` placeholder, so the first Look click restyles it (R5)
        std::string title = library->titleLayer(doc.committed());
        if (title.empty()) {
            ta::forEachLayer(doc.committed().root, [&](const ta::Json& l, const ta::Json*, int) {
                if (ta::layerTypeOf(l) == ta::LayerType::Text) title = str(l, "id");
                return true;
            });
        }
        if (!title.empty()) doc.selectOne({NodeKind::Layer, title, {}, {}});
    } else if (!options.layers.empty()) {
        std::vector<NodeRef> kept;
        for (const std::string& id : options.layers) {
            if (layerById(doc.committed(), id)) kept.push_back({NodeKind::Layer, id, {}, {}});
        }
        doc.select(kept);
    } else {
        // a Look with nothing to apply to added a text: select it
        const ta::Json& ids = doc.lastResult().newIds;
        for (auto it = ids.begin(); it != ids.end(); ++it) {
            if (it->is_string() && layerById(doc.committed(), it->get<std::string>())) {
                doc.selectOne({NodeKind::Layer, it->get<std::string>(), {}, {}});
                break;
            }
        }
    }
    return true;
}

void TitleDesignerWindow::drawLibrary(float width, float height) {
    TitleLibraryProvider* library = libraryProvider();
    if (!library || !doc.isOpen()) return;
    DocumentView& v = view();
    LibraryTab tab = LibraryTab::Looks;
    libraryTabFromName(v.libraryTab, tab);
    ImGui::BeginChild("##library", ImVec2(width, height), true, ImGuiWindowFlags_NoScrollbar);
    // the header names what a click does (R40)
    const LibraryApplyOptions options = libraryOptions(tab);
    std::string applies;
    if (tab == LibraryTab::Templates) applies = "Replaces the whole title";
    else if (options.wholeTitle) applies = "Applies to the whole title";
    else if (options.layers.empty()) applies = "Adds a text in this look";
    else if (options.layers.size() == 1) {
        const ta::Json* layer = layerById(doc.shown(), options.layers[0]);
        const ta::LayerType type = layer ? ta::layerTypeOf(*layer) : ta::LayerType::Unknown;
        const std::string name = layer ? (type == ta::LayerType::Text ? str(*layer, "text") : str(*layer, "name")) : std::string();
        applies = "Applies to \"" + name + "\" (" + ta::layerTypeLabel(type) + ")";
    } else {
        applies = "Applies to " + std::to_string(options.layers.size()) + " layers";
    }
    const bool collapsed = settings.libraryCollapsed || width < 960.0f * ImGui::GetFontSize() / 16.0f;
    if (ImGui::ArrowButton("##libcollapse", collapsed ? ImGuiDir_Up : ImGuiDir_Down)) {
        settings.libraryCollapsed = !settings.libraryCollapsed;
        settings.save();
    }
    ImGui::SameLine();
    for (LibraryTab t : {LibraryTab::Looks, LibraryTab::ColorSchemes, LibraryTab::Templates}) {
        const char* label = t == LibraryTab::Looks ? "Looks" : t == LibraryTab::ColorSchemes ? "Color Schemes" : "Templates";
        if (t != LibraryTab::Looks) ImGui::SameLine(0, 2);
        if (ImGui::Selectable(label, tab == t, 0, ImVec2(ImGui::CalcTextSize(label).x, 0))) {
            v.libraryTab = libraryTabName(t);
            tab = t;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", applies.c_str());
    if (collapsed) {
        ImGui::EndChild();
        return;
    }
    // search, families, dimension, tile size, Apply Font, Keep Color Scheme, Random Look
    ImGui::SameLine();
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", v.libraryQuery.c_str());
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputTextWithHint("##libsearch", ICON_FA_MAGNIFYING_GLASS " Search", buffer, sizeof(buffer))) v.libraryQuery = buffer;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    const char* dims[] = {"All", "2D", "3D"};
    int dim = v.libraryDimension == "2d" ? 1 : v.libraryDimension == "3d" ? 2 : 0;
    if (ImGui::Combo("##libdim", &dim, dims, 3)) v.libraryDimension = dim == 1 ? "2d" : dim == 2 ? "3d" : "all";
    ImGui::SameLine();
    for (const char* size : {"s", "m", "l"}) {
        if (ImGui::RadioButton(size == std::string("s") ? "S" : size == std::string("m") ? "M" : "L", v.tileSize == size)) v.tileSize = size;
        ImGui::SameLine();
    }
    if (tab == LibraryTab::Looks) {
        ImGui::Checkbox("Apply Font", &v.applyFont);
        ImGui::SameLine();
        ImGui::Checkbox("Keep Color Scheme", &v.keepColorScheme);
        ImGui::SameLine();
    }
    if (tab == LibraryTab::ColorSchemes) {
        const bool whole = options.wholeTitle;
        if (ImGui::RadioButton("Selected Layers", !whole)) v.colorSchemeScope = "selected";
        ImGui::SameLine();
        if (ImGui::RadioButton("Whole Title", whole)) v.colorSchemeScope = "whole";
        ImGui::SameLine();
    }
    std::vector<LibraryItem> items = library->items(tab);
    // family chips with match counts
    const std::string folded = foldForSearch(v.libraryQuery);
    std::vector<LibraryItem> filtered;
    for (const LibraryItem& item : items) {
        if (!itemMatches(item, folded)) continue;
        if (v.libraryDimension != "all" && !item.dimension.empty() && item.dimension != v.libraryDimension) continue;
        filtered.push_back(item);
    }
    if (tab == LibraryTab::Looks && ImGui::Button(ICON_FA_DICE " Random Look") && !filtered.empty() && doc.editable()) {
        static std::mt19937 random(std::random_device{}());
        const LibraryItem& pick = filtered[random() % filtered.size()];
        applyLibraryItem(tab, pick, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ELLIPSIS "###libmenu")) ImGui::OpenPopup("##libmenu");
    if (ImGui::BeginPopup("##libmenu")) {
        if (ImGui::MenuItem("Save As Look...")) {
            saveAsTab = LibraryTab::Looks;
            saveAsOpen = true;
        }
        if (ImGui::MenuItem("Save As Color Scheme...")) {
            saveAsTab = LibraryTab::ColorSchemes;
            saveAsOpen = true;
        }
        if (ImGui::MenuItem("Save As Template...")) {
            saveAsTab = LibraryTab::Templates;
            saveAsOpen = true;
        }
        ImGui::EndPopup();
    }
    if (saveAsOpen && library->drawSaveAs(saveAsTab, doc)) saveAsOpen = false;
    std::map<std::string, int> familyCounts;
    for (const LibraryItem& item : filtered) familyCounts[item.family]++;
    if (ImGui::SmallButton(("All##fam (" + std::to_string(filtered.size()) + ")").c_str())) v.libraryFamily.clear();
    for (const std::string& family : library->families()) {
        if (!familyCounts.count(family)) continue;
        ImGui::SameLine(0, 2);
        const bool on = v.libraryFamily == family;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton((family + " (" + std::to_string(familyCounts[family]) + ")").c_str())) v.libraryFamily = on ? std::string() : family;
        if (on) ImGui::PopStyleColor();
    }
    if (!v.libraryFamily.empty()) {
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(), [&](const LibraryItem& i) { return i.family != v.libraryFamily; }), filtered.end());
    }
    // Favorites and Recent first when no filter is on (R40)
    if (folded.empty() && v.libraryFamily.empty()) {
        std::vector<LibraryItem> first, rest;
        const std::vector<std::string> recents = library->recents(tab);
        for (const LibraryItem& item : filtered) {
            const bool pinned = library->isFavorite(tab, item.id) || std::find(recents.begin(), recents.end(), item.id) != recents.end();
            (pinned ? first : rest).push_back(item);
        }
        first.insert(first.end(), rest.begin(), rest.end());
        filtered = first;
    }

    // the target's key: requests wait until it has been stable 300 ms (R46)
    const std::string key = libraryTargetKey();
    if (key != targetKey) {
        targetKey = key;
        targetKeySince = now();
    }
    const bool keyStable = now() - targetKeySince >= 0.3;
    const float scale = ImGui::GetFontSize() / 16.0f;
    const ImVec2 tile = v.tileSize == "s" ? ImVec2(120, 68) : v.tileSize == "l" ? ImVec2(240, 135) : ImVec2(160, 90);
    const ImVec2 tileSize(tile.x * scale, tile.y * scale);
    ImGui::BeginChild("##tiles", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (filtered.empty()) {
        const char* noun = tab == LibraryTab::Looks ? "Looks" : tab == LibraryTab::ColorSchemes ? "Color Schemes" : "Templates";
        ImGui::TextDisabled("No %s match \"%s\"", noun, v.libraryQuery.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            v.libraryQuery.clear();
            v.libraryFamily.clear();
        }
    }
    const float visibleRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool mouseMoved = mouse.x != libraryHoverMouse.x || mouse.y != libraryHoverMouse.y;
    std::string hoveredNow;
    for (size_t i = 0; i < filtered.size(); i++) {
        const LibraryItem& item = filtered[i];
        if (i) ImGui::SameLine();
        ImGui::PushID(item.id.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        // visible tiles and the next screen only
        const bool visibleTile = p.x < visibleRight && p.x + tileSize.x > ImGui::GetWindowPos().x;
        const bool nextScreen = p.x < visibleRight + ImGui::GetWindowWidth();
        const bool clicked = ImGui::InvisibleButton("##tile", tileSize);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool highlighted = (int)i == libraryHighlight;
        dl->AddRectFilled(p, ImVec2(p.x + tileSize.x, p.y + tileSize.y), IM_COL32(44, 44, 50, 255), 4.0f);
        if (keyStable && (visibleTile || nextScreen)) {
            const LibraryTile t = library->tile(tab, item, targetKey, tileSize.x, tileSize.y, visibleTile);
            if (t.texture) {
                const float fit = std::min(tileSize.x / std::max(1.0f, t.width), tileSize.y / std::max(1.0f, t.height));
                const ImVec2 size(t.width * fit, t.height * fit);
                const ImVec2 o(p.x + (tileSize.x - size.x) * 0.5f, p.y + (tileSize.y - size.y) * 0.5f);
                // the baked first paint shows at 50 % with a spinner, never as the user's text (R47)
                dl->AddImage(t.texture, o, ImVec2(o.x + size.x, o.y + size.y), ImVec2(0, 0), ImVec2(1, 1), t.live ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 128));
            }
            if (t.pending || !t.live) {
                const float a = (float)now() * 6.0f;
                dl->AddCircle(ImVec2(p.x + tileSize.x - 10.0f, p.y + 10.0f), 5.0f, IM_COL32(255, 255, 255, 120), 12, 2.0f);
                dl->AddLine(ImVec2(p.x + tileSize.x - 10.0f, p.y + 10.0f), ImVec2(p.x + tileSize.x - 10.0f + std::cos(a) * 5.0f, p.y + 10.0f + std::sin(a) * 5.0f),
                            IM_COL32(255, 255, 255, 220), 2.0f);
            }
            if (!t.failed.empty()) {
                dl->AddText(ImVec2(p.x + 6.0f, p.y + tileSize.y * 0.4f), IM_COL32(240, 120, 100, 255), "Render failed");
                if (hovered) ImGui::SetTooltip("%s", t.failed.c_str());
            }
        }
        dl->AddText(ImVec2(p.x + 4.0f, p.y + tileSize.y - ImGui::GetFontSize() - 2.0f), IM_COL32(230, 230, 230, 230), item.name.c_str());
        if (item.user) dl->AddText(ImVec2(p.x + 4.0f, p.y + 2.0f), IM_COL32(120, 200, 255, 255), "user");
        if (!item.convertsTo.empty()) {
            const std::string badge = item.convertsTo == "3d" ? "\xE2\x86\x92" "3D" : "\xE2\x86\x92" "2D";
            dl->AddText(ImVec2(p.x + tileSize.x - 28.0f, p.y + tileSize.y - ImGui::GetFontSize() - 2.0f), IM_COL32(255, 210, 100, 255), badge.c_str());
        }
        const bool favorite = library->isFavorite(tab, item.id);
        const ImVec2 star(p.x + tileSize.x - 18.0f, p.y + 2.0f);
        dl->AddText(star, favorite ? IM_COL32(255, 210, 60, 255) : IM_COL32(200, 200, 200, hovered ? 200 : 0), favorite ? ICON_FA_STAR : ICON_FA_STAR);
        if (highlighted || hovered) dl->AddRect(p, ImVec2(p.x + tileSize.x, p.y + tileSize.y), IM_COL32(90, 160, 255, 255), 4.0f, 0, 2.0f);
        if (!item.disabledReason.empty()) {
            dl->AddRectFilled(p, ImVec2(p.x + tileSize.x, p.y + tileSize.y), IM_COL32(0, 0, 0, 120), 4.0f);
            if (hovered) ImGui::SetTooltip("%s", item.disabledReason.c_str());
        } else if (hovered && !item.convertsTo.empty()) {
            ImGui::SetTooltip("Converts the layer to %s (undoable)", item.convertsTo == "3d" ? "3D" : "2D");
        }
        if (hovered) {
            hoveredNow = item.id;
            if (clicked && mouse.x >= star.x && mouse.y <= star.y + ImGui::GetFontSize()) {
                library->setFavorite(tab, item.id, !favorite);
            } else if (clicked) {
                // the second click of a double-click is ignored (R43)
                const bool second = lastLibraryClickId == item.id && now() - lastLibraryClick < ImGui::GetIO().MouseDoubleClickTime;
                lastLibraryClick = now();
                lastLibraryClickId = item.id;
                if (!second) applyLibraryItem(tab, item, false);
            }
            if (item.user && ImGui::BeginPopupContextItem("##usermenu")) {
                std::string error;
                if (ImGui::MenuItem("Delete")) library->deleteItem(tab, item.id, &error);
                ImGui::EndPopup();
                if (!error.empty()) reportError(error);
            }
        }
        ImGui::PopID();
    }
    if (pendingTemplate.size()) {
        ImGui::OpenPopup("Replace the whole title?###replacetitle");
    }
    if (ImGui::BeginPopupModal("Replace the whole title?###replacetitle", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Replace the whole title? Undo brings it back.");
        if (ImGui::Button("Replace")) {
            for (const LibraryItem& item : items) {
                if (item.id == pendingTemplate) {
                    applyLibraryItem(LibraryTab::Templates, item, false);
                    break;
                }
            }
            pendingTemplate.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingTemplate.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // keys (R49): arrows move the highlight and preview at once, Enter applies, Esc restores
    const bool libraryFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
    bool arrowed = false;
    if (libraryFocused && !filtered.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            libraryHighlight = std::min((int)filtered.size() - 1, libraryHighlight + 1);
            arrowed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            libraryHighlight = std::max(0, libraryHighlight - 1);
            arrowed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            libraryHighlight = 0;
            arrowed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) {
            libraryHighlight = (int)filtered.size() - 1;
            arrowed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && libraryHighlight >= 0) applyLibraryItem(tab, filtered[(size_t)libraryHighlight], false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            doc.clearPreview();
            libraryHighlight = -1;
            ImGui::SetWindowFocus(nullptr);
        }
    }
    // hover with intent: 150 ms after the pointer moved at least 1 px over the tile; a list
    // scrolling under a still pointer or a zero-movement enter never previews (R42)
    if (hoveredNow != libraryHover) {
        libraryHover = hoveredNow;
        libraryHoverSince = now();
        libraryHoverMoved = false;
        libraryHoverMouse = mouse;
        if (hoveredNow.empty() && !arrowed && doc.preview() &&
            (doc.preview()->kind == PreviewItem::Kind::Look || doc.preview()->kind == PreviewItem::Kind::ColorScheme ||
             doc.preview()->kind == PreviewItem::Kind::Template)) {
            doc.clearPreview();
        }
    } else if (!hoveredNow.empty() && mouseMoved) {
        if (!libraryHoverMoved) libraryHoverSince = now();
        libraryHoverMoved = true;
        libraryHoverMouse = mouse;
    }
    if (!libraryHover.empty() && libraryHoverMoved && now() - libraryHoverSince >= 0.15) {
        for (const LibraryItem& item : filtered) {
            if (item.id == libraryHover && (!doc.preview() || doc.preview()->id != item.id)) applyLibraryItem(tab, item, true);
        }
    }
    if (arrowed && libraryHighlight >= 0) applyLibraryItem(tab, filtered[(size_t)libraryHighlight], true);
    ImGui::EndChild();
    ImGui::EndChild();
}
