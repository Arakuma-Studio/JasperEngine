// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Properties panel (docs/specs/S82-title-designer.md R18-R27, R53, D4). The live search (an
// index read from the real views, results that are the live controls, landing with a flash) is
// Pachislop's PropertySearch / searchIndex.ts / Findable (src/ui/properties/, src/ui/controls.tsx)
// by SafEight, used with the author's permission, ported to a collect pass over the same page
// functions; the search text is cleared after a jump (Pachislop's stale box, report p7).

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitleSearchTerms.h"
#include "window/EditorModal.h"

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

    std::string joinLocation(const std::vector<std::string>& location) {
        std::string out;
        for (const std::string& part : location) {
            if (!out.empty()) out += " \xE2\x80\xBA ";
            out += part;
        }
        return out;
    }

    // the type badge of a page
    const char* badgeOf(const ta::TitleArtDoc& doc, const NodeRef& node) {
        if (node.kind == NodeKind::Document) return "DOC";
        if (node.kind == NodeKind::Part) return "PART";
        if (node.kind == NodeKind::Effect) return "FX";
        if (node.kind == NodeKind::Mask) return "MASK";
        const ta::Json* layer = layerById(doc, node.layer);
        if (!layer) return "?";
        switch (ta::layerTypeOf(*layer)) {
            case ta::LayerType::Text: return "T";
            case ta::LayerType::Shape: return "SHP";
            case ta::LayerType::Image: return "IMG";
            case ta::LayerType::Vector: return "SVG";
            case ta::LayerType::Group: return "GRP";
            case ta::LayerType::Generator: return "GEN";
            case ta::LayerType::Adjustment: return "ADJ";
            default: return "?";
        }
    }

}

void TitleDesignerWindow::rebuildIndexIfNeeded() {
    std::string selectionKey = searchEveryLayer ? std::string("*") : std::string();
    for (const NodeRef& node : doc.selection()) selectionKey += node.layer + ";";
    const double t = now();
    const bool stale = indexChange != doc.changeCount() || indexSelectionKey != selectionKey;
    if (!stale) return;
    // while a result is edited the list keeps its order and re-indexes 250 ms after the last change
    if (!editingHit.empty() && t - editingHitAt < 0.25 && indexSelectionKey == selectionKey) return;
    collectIndex(doc, this, index, searchEveryLayer);
    indexChange = doc.changeCount();
    indexSelectionKey = selectionKey;
}

void TitleDesignerWindow::jumpToHit(size_t i) {
    if (i >= index.size()) return;
    const IndexEntry& entry = index.entry(i);
    selectNode(entry.node);
    // its sections open, the row scrolls to the centre and flashes for 1.6 s (R26)
    flashRow = entry.rowId;
    flashUntil = now() + 1.6;
    pendingScrollRow = entry.rowId;
    // every section of the page on the hit's location opens
    forceOpenSections = entry.location;
    // the search field is cleared after a jump
    searchText.clear();
    stableOrder.clear();
}

void TitleDesignerWindow::drawSearch() {
    if (searchFocus) {
        ImGui::SetKeyboardFocusHere();
        searchFocus = false;
    }
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", searchText.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint("##findsetting", ICON_FA_MAGNIFYING_GLASS " Find a setting (Ctrl+F)", buffer, sizeof(buffer));
    const bool active = ImGui::IsItemActive();
    if (changed) {
        searchText = buffer;
        highlightedHit = 0;
        stableOrder.clear();
        resultLimit = 30;
    }
    // IME composition keys never reach Enter or Esc (the backends hold them while composing)
    if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (!searchText.empty()) {
            searchText.clear();
            stableOrder.clear();
        } else {
            ImGui::SetWindowFocus(nullptr);
        }
    }
    if (active && !searchText.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) highlightedHit++;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) highlightedHit = std::max(0, highlightedHit - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            rebuildIndexIfNeeded();
            const std::vector<SearchHit> hits = index.query(searchText, resultLimit);
            if (!hits.empty()) jumpToHit(hits[(size_t)std::clamp(highlightedHit, 0, (int)hits.size() - 1)].entry);
        }
    }
}

void TitleDesignerWindow::drawSearchResults() {
    rebuildIndexIfNeeded();
    std::vector<SearchHit> hits = index.query(searchText, 0);
    // stable keys while a result is being edited (R26)
    if (!editingHit.empty() && !stableOrder.empty()) {
        std::vector<SearchHit> ordered;
        for (const std::string& rowId : stableOrder) {
            for (const SearchHit& hit : hits) {
                if (index.entry(hit.entry).rowId == rowId) ordered.push_back(hit);
            }
        }
        hits = ordered;
    } else {
        stableOrder.clear();
        for (const SearchHit& hit : hits) stableOrder.push_back(index.entry(hit.entry).rowId);
    }
    if (hits.empty()) ImGui::TextDisabled("No setting matches \"%s\"", searchText.c_str());
    highlightedHit = std::clamp(highlightedHit, 0, std::max(0, (int)hits.size() - 1));
    const size_t shown = std::min(hits.size(), resultLimit);
    std::string group;
    for (size_t i = 0; i < shown; i++) {
        const IndexEntry& entry = index.entry(hits[i].entry);
        if (searchEveryLayer && entry.nodeLabel != group) {
            group = entry.nodeLabel;
            ImGui::SeparatorText(group.c_str());
        }
        ImGui::PushID((int)i);
        const std::string location = joinLocation(entry.location);
        if ((int)i == highlightedHit) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetTextLineHeightWithSpacing()),
                                                      IM_COL32(80, 140, 230, 60), 3.0f);
        }
        if (ImGui::Selectable(((location.empty() ? entry.nodeLabel : location) + "##loc").c_str(), false)) jumpToHit(hits[i].entry);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to its page (Enter)");
        // the live control: the same row bound to the same targets
        RowContext ctx;
        ctx.host = this;
        ctx.state = &doc;
        ctx.node = entry.node;
        ctx.nodeLabel = entry.nodeLabel;
        ctx.readOnly = !doc.editable();
        ctx.readOnlyReason = doc.readOnlyReason();
        ctx.countNoun = entry.targets.size() > 1 && entry.fieldPath.find("rims[") != std::string::npos ? "Rims" : "Layers";
        // the step names are the page's: the location's object prefix + the label
        std::string prefix;
        for (const std::string& part : entry.location) {
            if (part.rfind("Rim", 0) == 0 || part == "Bevel" || part == "Satin" || part == "Pattern" || part == "Texture" || part == "Gloss" ||
                part == "Glass" || part == "Sheen" || part == "Finish" || part == "Reflection" || part == "Inner Glow" || part == "Plate" ||
                part == "Erode" || part == "Warp" || part == "Distort" || part == "Global Light")
                prefix += part + " ";
        }
        if (entry.node.kind == NodeKind::Effect) {
            const ta::Json* layer = layerById(doc.shown(), entry.node.layer);
            const ta::Json* effect = layer ? effectOf(*layer, entry.node) : nullptr;
            const ta::EffectDef* def = effect ? ta::findEffect(str(*effect, "type")) : nullptr;
            prefix = def ? std::string(def->label) + " " : std::string();
        }
        ctx.stepPrefix = prefix;
        const uint64_t before = doc.changeCount();
        if (entry.kind == "font") {
            std::vector<std::string> layers;
            for (const FieldRef& target : entry.targets) layers.push_back(target.layer);
            drawFontRow(layers, ctx.readOnly);
        } else if (entry.link) {
            ui::DocRowOptions options;
            options.label = entry.label.c_str();
            options.fieldPath = entry.fieldPath;
            if (ui::docRowLink(entry.rowId.c_str(), options, "Open on its page")) jumpToHit(hits[i].entry);
        } else {
            fieldRow(ctx, entry.targets, {.label = entry.label.c_str()});
        }
        if (doc.changeCount() != before || doc.gestureActive()) {
            editingHit = entry.rowId;
            editingHitAt = now();
        }
        ImGui::PopID();
    }
    if (hits.size() > shown && ImGui::Button(("Show more (" + std::to_string(hits.size() - shown) + ")").c_str())) resultLimit += 30;
    if (now() - editingHitAt > 0.25 && !doc.gestureActive()) editingHit.clear();

    // effects the layer does not have: + Add (R26)
    if (!doc.selectedLayers().empty() && doc.editable()) {
        const std::vector<OfferHit> offers = index.queryOffers(searchText, 6);
        for (const OfferHit& hit : offers) {
            const AddOffer& offer = index.offer(hit.offer);
            ImGui::PushID(("offer" + offer.type).c_str());
            const std::string label = offer.kind == "generator" ? "+ Add Generator Layer " + offer.label : "+ Add " + offer.label;
            const bool clicked = ImGui::Selectable(label.c_str());
            const bool hovered = ImGui::IsItemHovered();
            const std::string layer = doc.selectedLayers().back();
            ta::Json op = offer.kind == "generator" ? ta::Json{{"op", "add_layer"}, {"type", "generator"}, {"generator", offer.type}, {"after", "id:" + layer}}
                                                : ta::Json{{"op", "add_effect"}, {"layer", "id:" + layer}, {"type", offer.type}};
            const ta::Json* target = layerById(doc.committed(), layer);
            const std::string name = offer.kind == "generator" ? "Add Layer " + offer.label
                                                               : "Add Effect " + offer.label + " to " + (target ? str(*target, "name") : std::string());
            if (hovered) {
                // hover with intent previews it (R42's 150 ms)
                const ImVec2 mouse = ImGui::GetMousePos();
                if (addHovered != offer.type) {
                    addHovered = offer.type;
                    addHoverSince = now();
                    addHoverMouse = mouse;
                } else if (now() - addHoverSince >= 0.15 && (mouse.x != addHoverMouse.x || mouse.y != addHoverMouse.y || true)) {
                    PreviewItem item;
                    item.kind = offer.kind == "generator" ? PreviewItem::Kind::Generator : PreviewItem::Kind::Effect;
                    item.id = offer.type;
                    item.label = offer.label;
                    item.ops = ta::Json::array({op});
                    item.stepName = name;
                    std::string error;
                    doc.setPreview(item, &error);
                }
            } else if (addHovered == offer.type) {
                addHovered.clear();
                doc.clearPreview();
            }
            if (clicked) {
                doc.clearPreview();
                commitNamed(ta::Json::array({op}), name);
                addHovered.clear();
            }
            ImGui::PopID();
        }
    }
    if (!searchEveryLayer) {
        if (ImGui::SmallButton("Search every layer")) searchEveryLayer = true;
    } else if (ImGui::SmallButton("Search the selection only")) {
        searchEveryLayer = false;
    }
}

void TitleDesignerWindow::drawPropertiesHeader() {
    const NodeRef primary = doc.primary();
    const ta::TitleArtDoc& shown = doc.shown();
    // breadcrumb: Title > Logo > Rim 2 > Outer Glow, each crumb a button (R18)
    std::vector<std::pair<std::string, NodeRef>> crumbs;
    crumbs.push_back({shown.root.value("name", std::string()).empty() ? std::string("Document") : shown.root.value("name", std::string()), NodeRef{}});
    if (primary.kind != NodeKind::Document) {
        std::vector<std::string> chain;
        std::string id = primary.layer;
        while (!id.empty()) {
            chain.push_back(id);
            id = parentLayerOf(shown, id);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const ta::Json* layer = layerById(shown, *it);
            crumbs.push_back({layer ? str(*layer, "name") : *it, NodeRef{NodeKind::Layer, *it, {}, {}}});
        }
        const ta::Json* layer = layerById(shown, primary.layer);
        if (layer && !primary.part.empty()) crumbs.push_back({partLabel(*layer, primary.part), NodeRef{NodeKind::Part, primary.layer, primary.part, {}}});
        if (layer && primary.kind == NodeKind::Effect) {
            const ta::Json* effect = effectOf(*layer, primary);
            const ta::EffectDef* def = effect ? ta::findEffect(str(*effect, "type")) : nullptr;
            crumbs.push_back({def ? def->label : "Effect", primary});
        }
        if (layer && primary.kind == NodeKind::Mask) crumbs.push_back({"Mask", primary});
    }
    for (size_t i = 0; i < crumbs.size(); i++) {
        if (i) {
            ImGui::SameLine(0, 2);
            ImGui::TextDisabled("\xE2\x80\xBA");
            ImGui::SameLine(0, 2);
        }
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(crumbs[i].first.c_str())) {
            if (crumbs[i].second.kind == NodeKind::Document) doc.select({});
            else selectNode(crumbs[i].second);
        }
        ImGui::PopID();
    }
    // type badge, lint badge
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "[%s]", badgeOf(shown, primary));
    ImGui::SameLine();
    if (primary.kind == NodeKind::Layer || primary.kind == NodeKind::Document) {
        const ta::Json* layer = layerById(shown, primary.layer);
        ImGui::TextUnformatted(layer ? str(*layer, "name").c_str() : "Title Art");
    } else {
        ImGui::TextUnformatted(crumbs.back().first.c_str());
    }
    ImGui::SameLine();
    const size_t issues = findings.size();
    ImGui::PushStyleColor(ImGuiCol_Text, issues ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f));
    const std::string badge = issues ? std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(issues) : std::string(ICON_FA_CHECK " ") + std::to_string(checksRun) + " checks";
    if (ImGui::SmallButton((badge + "###proplint").c_str())) {
        lintPopup = true;
        ImGui::OpenPopup("##lintpopup");
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(issues ? "Style lint findings: click for the rules and fixes" : "No issues found by %zu checks", checksRun);
    drawLintPopup();
}

void TitleDesignerWindow::drawProperties() {
    if (newerVersion()) {
        ImGui::TextDisabled("Written by a newer JasperEngine: nothing to show here");
        return;
    }
    if (!doc.isOpen()) {
        ImGui::TextDisabled("No Title Art open");
        return;
    }
    drawPropertiesHeader();
    drawSearch();
    ImGui::Separator();
    ImGui::BeginChild("##propertiespage", ImVec2(0, 0), false);
    if (!searchText.empty()) {
        drawSearchResults();
    } else {
        RowContext ctx;
        ctx.host = this;
        ctx.state = &doc;
        ctx.readOnly = !doc.editable();
        ctx.readOnlyReason = doc.readOnlyReason();
        ctx.forceOpen = forceOpenSections;
        drawSelectionPage(ctx);
        forceOpenSections.clear();
    }
    ImGui::EndChild();
}

// ---- lint (R53) ----

void TitleDesignerWindow::refreshLint(bool pixels) {
    findings.clear();
    checksRun = 0;
    if (!doc.isOpen() || newerVersion()) return;
    registerBuiltinTitleChecks();
    TitleCheckContext context(doc.shown(), doc.document()->projectPath());
    context.file = doc.document()->absolutePath();
    context.assets = TitleArtRenderHost::editor().resolver(project);
    // the Device's Display Height is what legibility is measured at (R31)
    const DocumentView& v = view();
    if (!v.device.profile.empty()) {
        const double screenHeight = stageRect().height;
        context.options.displayHeightPx = (int)std::round(v.device.displayHeightPx ? v.device.displayHeight : screenHeight * v.device.displayHeight / 100.0);
    }
    if (pixels) {
        Project* p = project;
        context.render = [p](const ta::RenderRequest& request, TitleCheckImage& out, std::string& error) {
            TitleRenderedImage image;
            if (!TitleArtRenderHost::editor().render(p, request, image, error)) return false;
            out.width = image.image.width;
            out.height = image.image.height;
            out.rgba = image.image.data;
            out.frame = image.frame;
            return true;
        };
    }
    std::vector<const CheckDef*> checks;
    for (const CheckDef* def : TitleArtChecks::all()) {
        // document checks run by themselves; pixel checks only when asked (Check Rendered)
        if (def->kind == TitleCheckKind::Document || (pixels && def->kind != TitleCheckKind::Estimate)) checks.push_back(def);
    }
    const TitleCheckReport report = TitleArtChecks::run(checks, context);
    checksRun = report.checks.size();
    for (const TitleCheckResult& result : report.checks) {
        if (result.status != TitleCheckStatus::Warn && result.status != TitleCheckStatus::Fail) continue;
        CheckFinding f;
        f.id = result.id;
        f.status = titleCheckStatusName(result.status);
        f.message = result.message;
        f.rule = result.method;
        f.measured = result.measured.is_null() ? std::string() : result.measured.dump();
        f.threshold = result.threshold.is_null() ? std::string() : result.threshold.dump();
        if (!result.at.empty()) f.at = result.at[0];
        if (result.fix.is_object() && result.fix.contains("ops")) {
            f.fix = result.fix["ops"];
            std::string label = result.id;
            const size_t dot = label.find('.');
            if (dot != std::string::npos) label = label.substr(0, dot);
            f.fixName = "Style Fix: " + enumLabel(label);
        }
        findings.push_back(f);
    }
    // S84's style_lint and S85's checks register their sources
    const std::vector<CheckFinding> extra = runCheckSources(doc);
    findings.insert(findings.end(), extra.begin(), extra.end());
    checksRun += extra.size();
    pixelChecksRun = pixels;
}

void TitleDesignerWindow::drawLintPopup() {
    if (!ImGui::BeginPopup("##lintpopup")) return;
    if (findings.empty()) ImGui::Text(ICON_FA_CHECK " No issues found by %zu checks", checksRun);
    else ImGui::Text(ICON_FA_TRIANGLE_EXCLAMATION " %zu finding%s", findings.size(), findings.size() == 1 ? "" : "s");
    ImGui::Separator();
    ta::Json fixAll = ta::Json::array();
    for (size_t i = 0; i < findings.size(); i++) {
        const CheckFinding& f = findings[i];
        ImGui::PushID((int)i);
        ImGui::TextColored(f.status == "FAIL" ? ImVec4(0.95f, 0.45f, 0.35f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", f.status.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", f.message.c_str());
        if (!f.rule.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled(ICON_FA_CIRCLE_INFO);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.rule.c_str());
        }
        if (!f.measured.empty()) ImGui::TextDisabled("measured %s (threshold %s)", f.measured.c_str(), f.threshold.c_str());
        if (!f.at.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Look")) {
                const double cx = f.at.x + f.at.width * 0.5 - doc.shown().canvasWidth() * 0.5;
                const double cy = doc.shown().canvasHeight() * 0.5 - (f.at.y + f.at.height * 0.5);
                view().fit = false;
                view().panX = cx;
                view().panY = cy;
            }
        }
        if (!f.fix.is_null() && doc.editable()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix")) {
                commitNamed(f.fix, f.fixName.empty() ? "Style Fix" : f.fixName);
                lintDirtyAt = now() - 1.0;
            }
            for (const ta::Json& op : f.fix) fixAll.push_back(op);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!fixAll.empty() && doc.editable() && ImGui::Button("Fix All")) {
        commitNamed(fixAll, "Style Fix: All");
        lintDirtyAt = now() - 1.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Rendered")) refreshLint(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run the pixel checks too (contrast, legibility at the Device's height, clipping)");
    ImGui::TextDisabled("%s", TitleArtChecks::guidance());
    ImGui::EndPopup();
}

// ---- Expose as Parameter... ----

void TitleDesignerWindow::drawExposeDialog() {
    if (exposeOpen) {
        ImGui::OpenPopup("Expose as Parameter###exposedialog");
        exposeOpen = false;
    }
    if (!EditorModal::begin("Expose as Parameter###exposedialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string question = "Expose " + exposeLabel + " as a parameter instances can override";
    EditorModal::message(question);
    ImGui::TextUnformatted(question.c_str());
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", exposeName.c_str());
    if (EditorModal::inputText("Name", "name", buffer, sizeof(buffer))) exposeName = buffer;
    const ta::Json* value = fieldValue(doc.committed(), exposeTarget);
    std::string type = "number";
    if (value && value->is_string()) type = exposeTarget.path == "text" ? "text" : value->get<std::string>().rfind('#', 0) == 0 ? "color" : "text";
    else if (value && value->is_boolean()) type = "bool";
    else if (value && value->is_object()) type = exposeTarget.path == "font" ? "font" : "image";
    ImGui::TextDisabled("Type: %s \xC2\xB7 path %s", type.c_str(), exposeTarget.path.c_str());
    if (exposeName.empty()) EditorModal::error("The parameter needs a name");
    ImGui::BeginDisabled(exposeName.empty());
    const bool expose = EditorModal::button("Expose", "ok");
    ImGui::EndDisabled();
    if (expose && !exposeName.empty()) {
        ta::Json op = {{"op", "expose"}, {"name", exposeName}, {"type", type},
                   {"targets", ta::Json::array({ta::Json{{"layer", exposeTarget.layer.empty() ? std::string() : "id:" + exposeTarget.layer}, {"field", exposeTarget.path}}})}};
        commitNamed(ta::Json::array({op}), "Expose Parameter " + exposeName);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
