// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Designer's toolbar and status bar (docs/specs/S82-title-designer.md R4, R9, R71, R72)

#include "TitleDesignerWindow.h"

#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/Profile.h"
#include "render/RenderUtil.h"
#include "render/SystemRender.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtSettings.h"
#include "titleart/TitleArtSettingsCmd.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "titleart/TitlePreviewQueue.h"
#include "titleart/designer/TitleCanvasView.h"
#include "util/FileUtils.h"
#include "window/DeviceProfiles.h"
#include "window/GameResolutions.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    void tooltip(const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
    }

    bool toolButton(const char* icon, const char* id, bool active, const char* tip) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked = ImGui::Button((std::string(icon) + "###" + id).c_str());
        if (active) ImGui::PopStyleColor();
        tooltip(tip);
        return clicked;
    }

    // the toolbar wraps onto a second row when the next item does not fit (R4)
    void nextItem(float width) {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
    }

}

const TitleBakeStatus& TitleDesignerWindow::bakeStatus() {
    JE_PROFILE_SCOPE("TD bakeStatus");
    const fs::path document = doc.document()->absolutePath();
    std::error_code ec;
    const auto sidecarTime = fs::last_write_time(titleArtSidecarPath(document), ec);
    const std::string key = document.string() + "|" + std::to_string(doc.changeCount()) + "|" + std::to_string(transient.revision()) + "|" +
                            (TitleArtAutoBake::get().baking(document) ? "1" : "0") + "|" +
                            std::to_string(ec ? 0 : (long long)sidecarTime.time_since_epoch().count());
    if (key != bakeStatusKey) {
        JE_PROFILE_SCOPE("TD bakeStatus compute");
        bakeStatusKey = key;
        bakeStatusCache = titleArtStatusOf(project, document, doc.committed());
        documentPasses++;
    }
    return bakeStatusCache;
}

void TitleDesignerWindow::drawToolbar() {
    JE_PROFILE_SCOPE("TD drawToolbar");
    const bool open = doc.isOpen();
    const bool editable = doc.editable();
    // document chip: name, recent list, Show in Project, Duplicate As...; dragging it into a scene
    // drops the shown file (R61)
    const std::string chip = open ? fs::path(doc.document()->assetPath()).filename().string() : std::string("Open a Title Art");
    // the menu opens on the release of a click: opened on the press it took the drag (R61)
    const bool chipClicked = ImGui::Button((ICON_FA_FILE_IMAGE " " + chip + " " ICON_FA_CARET_DOWN "###docchip").c_str());
    if (open && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        // the Project window's payload: the project-relative path
        const std::string payload = doc.document()->projectPath();
        ImGui::SetDragDropPayload("resource_files", payload.c_str(), payload.size() + 1);
        ImGui::Text(ICON_FA_FILE_IMAGE " %s", chip.c_str());
        ImGui::EndDragDropSource();
    } else if (chipClicked) {
        ImGui::OpenPopup("##docmenu");
    }
    tooltip(open ? "The shown Title Art: drag it into the Hierarchy or the Scene view to place it" : "Recent Title Art documents");
    if (ImGui::BeginPopup("##docmenu")) {
        if (workspace.recent().empty()) ImGui::TextDisabled("No recent Title Art");
        for (const std::string& recent : workspace.recent()) {
            if (ImGui::MenuItem(recent.c_str(), nullptr, open && recent == doc.document()->assetPath())) {
                std::string error;
                if (!this->open(project->getAssetsPath() / FileUtils::pathFromUtf8(recent), &error)) {
                    reportError(error);
                    workspace.forget(recent);
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Project", nullptr, false, open)) {
            project->setInspectedAsset(doc.document()->absolutePath());
        }
        if (ImGui::MenuItem("Duplicate As...", nullptr, false, open)) {
            const fs::path source = doc.document()->absolutePath();
            fs::path target = source.parent_path() / (source.stem().string() + " Copy" + ta::TITLE_ART_EXTENSION);
            for (int n = 2; fs::exists(target); n++) target = source.parent_path() / (source.stem().string() + " Copy " + std::to_string(n) + ta::TITLE_ART_EXTENSION);
            ta::TitleArtDoc copy = doc.committed();
            copy.root["guid"] = "";
            std::string error;
            const fs::path relative = fs::relative(target, project->getProjectPath());
            if (TitleArtDocument::create(project, relative.generic_string(), &copy, "Duplicate Title Art", &error)) this->open(target, &error);
            if (!error.empty()) reportError(error);
        }
        ImGui::EndPopup();
    }

    // tools (Unity's Q W E R T) and Pivot/Center (Z)
    nextItem(220.0f);
    if (toolButton(ICON_FA_HAND, "toolHand", currentTool == Tool::Hand, "Hand Tool (Q): drag to pan")) setTool(Tool::Hand);
    ImGui::SameLine(0, 2);
    if (toolButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "toolMove", currentTool == Tool::Move, "Move Tool (W)")) setTool(Tool::Move);
    ImGui::SameLine(0, 2);
    if (toolButton(ICON_FA_ROTATE, "toolRotate", currentTool == Tool::Rotate, "Rotate Tool (E)")) setTool(Tool::Rotate);
    ImGui::SameLine(0, 2);
    if (toolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "toolScale", currentTool == Tool::Scale, "Scale Tool (R)")) setTool(Tool::Scale);
    ImGui::SameLine(0, 2);
    if (toolButton(ICON_FA_VECTOR_SQUARE, "toolRect", currentTool == Tool::Rect, "Rect Tool (T)")) setTool(Tool::Rect);
    ImGui::SameLine();
    if (ImGui::Button(pivotMode ? "Pivot###pivot" : "Center###pivot")) pivotMode = !pivotMode;
    tooltip("Toggle Pivot/Center (Z): rotate and scale about the layer's position or its centre");

    // Add
    nextItem(70.0f);
    if (!editable) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_FA_PLUS " Add " ICON_FA_CARET_DOWN "###add")) openAddPalette({});
    if (!editable) ImGui::EndDisabled();
    tooltip(editable ? "Add a layer, generator or effect (Ctrl+Shift+N)" : doc.readOnlyReason().c_str());

    if (!open || newerVersion()) return;
    DocumentView& v = view();

    // Preview As (S81's Render Modes; S86 adds its tiers through the hooks)
    nextItem(170.0f);
    std::string previewLabel = v.previewAs == "baked" ? "Baked" : "Realtime";
    for (const PreviewModeDef& mode : previewModes()) {
        if (mode.id == v.previewAs) previewLabel = mode.label;
    }
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##previewas", ("Preview: " + previewLabel).c_str())) {
        if (ImGui::Selectable("Realtime", v.previewAs == "realtime")) setPreviewAs("realtime");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The renderer on the working document (what a Realtime title shows)");
        if (ImGui::Selectable("Baked", v.previewAs == "baked")) setPreviewAs("baked");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The baked PNG the game ships (stale when the recipe differs)");
        for (const PreviewModeDef& mode : previewModes()) {
            if (ImGui::Selectable(mode.label.c_str(), v.previewAs == mode.id)) setPreviewAs(mode.id);
            if (ImGui::IsItemHovered() && !mode.note.empty()) ImGui::SetTooltip("%s", mode.note.c_str());
        }
        ImGui::EndCombo();
    }
    // Quality
    nextItem(110.0f);
    ImGui::SetNextItemWidth(100.0f);
    const char* qualities[] = {"Auto", "Draft", "Full"};
    int q = (int)settings.quality;
    if (ImGui::Combo("##quality", &q, qualities, 3)) {
        settings.quality = (Quality)q;
        settings.qualitySet = true;
        settings.save();
    }
    tooltip("Quality: Auto renders Draft while you drag and Full when it settles");

    // Background
    nextItem(120.0f);
    static const char* const kinds[][2] = {{"checker", "Checker"}, {"black", "Black"},       {"white", "White"},           {"gray", "50% Gray"},
                                           {"color", "Color..."},  {"image", "Image..."},    {"game_scene", "Game Scene"}, {"in_context", "In Context"}};
    std::string bgLabel = v.backdrop.kind;
    for (const auto& k : kinds) {
        if (v.backdrop.kind == k[0]) bgLabel = k[1];
    }
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##background", ("BG: " + bgLabel).c_str())) {
        for (const auto& k : kinds) {
            const bool needsContext = std::string(k[0]) == "in_context";
            if (needsContext && !inContext) {
                ImGui::BeginDisabled();
                ImGui::Selectable(k[1], false);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Open the title from a Title Art Renderer's Edit button");
                continue;
            }
            if (ImGui::Selectable(k[1], v.backdrop.kind == k[0])) {
                BackdropChoice b = v.backdrop;
                b.kind = k[0];
                if (b.kind == "game_scene" && b.scene.empty() && project) {
                    if (const SceneProject* scene = project->getSelectedScene()) b.scene = scene->filepath.generic_string();
                }
                setBackground(b);
            }
        }
        if (v.backdrop.kind == "game_scene") {
            ImGui::Separator();
            if (ImGui::Selectable(snapshotRgba.empty() ? "Snapshot" : "Refresh")) {
                std::string error;
                if (!snapshotScene(&error)) reportError(error);
            }
        }
        ImGui::EndCombo();
    }
    tooltip("The backdrop behind the title (view only: the document's own background is on the Document page)");
    // Device
    nextItem(130.0f);
    std::string deviceLabel = "None";
    if (!v.device.profile.empty()) {
        if (const DeviceProfile* p = findDeviceProfile(v.device.profile)) deviceLabel = p->name;
        else deviceLabel = v.device.profile.substr(v.device.profile.rfind(':') + 1);
    }
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##device", ("Device: " + deviceLabel).c_str())) {
        if (ImGui::Selectable("None", v.device.profile.empty())) {
            DeviceChoice d = v.device;
            d.profile.clear();
            setDevice(d);
        }
        ImGui::SeparatorText("Resolutions");
        for (const GameResolution& r : builtInGameResolutions()) {
            const std::string id = "res:" + r.name;
            if (ImGui::Selectable(gameResolutionLabel(r).c_str(), v.device.profile == id)) {
                DeviceChoice d = v.device;
                d.profile = id;
                setDevice(d);
            }
        }
        ImGui::SeparatorText("Devices");
        for (const DeviceProfile& p : deviceProfiles()) {
            for (const char* orientation : {"portrait", "landscape_left"}) {
                const std::string label = std::string(p.name) + (std::string(orientation) == "portrait" ? " (Portrait)" : " (Landscape)");
                if (ImGui::Selectable(label.c_str(), v.device.profile == p.id && v.device.orientation == orientation)) {
                    DeviceChoice d = v.device;
                    d.profile = p.id;
                    d.orientation = orientation;
                    setDevice(d);
                }
            }
        }
        if (!v.device.profile.empty()) {
            ImGui::SeparatorText("Placement");
            float height = (float)v.device.displayHeight;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat("Display Height", &height, 0.5f, 1.0f, v.device.displayHeightPx ? 8192.0f : 100.0f,
                                 v.device.displayHeightPx ? "%.0f px" : "%.0f %%")) {
                v.device.displayHeight = height;
            }
            ImGui::Checkbox("In pixels", &v.device.displayHeightPx);
            ImGui::TextUnformatted("Anchor");
            for (int i = 0; i < 9; i++) {
                if (i % 3) ImGui::SameLine(0, 2);
                ImGui::PushID(i);
                if (ImGui::RadioButton("##a", v.device.anchor == i)) v.device.anchor = i;
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    // Guides
    nextItem(90.0f);
    if (ImGui::Button("Guides " ICON_FA_CARET_DOWN "###guides")) ImGui::OpenPopup("##guidesmenu");
    if (ImGui::BeginPopup("##guidesmenu")) {
        ImGui::MenuItem("Rulers", nullptr, &v.rulers);
        ImGui::MenuItem("Guides", nullptr, &v.showGuides);
        ImGui::MenuItem("Safe Areas", nullptr, &v.safeAreas);
        ImGui::MenuItem("Pixel Grid", nullptr, &v.pixelGrid);
        ImGui::MenuItem("Effect Reach", nullptr, &v.effectReach);
        ImGui::Separator();
        if (ImGui::MenuItem("Clear Guides", nullptr, false, !v.guides.empty())) v.guides.clear();
        ImGui::SeparatorText("Snap Increments (Ctrl)");
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::DragFloat("Move", &settings.snapMove, 0.5f, 0.1f, 1000.0f, "%.1f px")) settings.save();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::DragFloat("Rotate", &settings.snapRotate, 0.5f, 0.1f, 180.0f, "%.1f\xC2\xB0")) settings.save();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::DragFloat("Scale", &settings.snapScale, 0.01f, 0.001f, 10.0f, "%.2f")) settings.save();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap###snap", &v.snap);
    tooltip("Snap to edges, centres, other layers and guides within 8 px (hold Ctrl for increments)");

    // zoom
    nextItem(90.0f);
    char zoomText[32];
    if (v.fit) std::snprintf(zoomText, sizeof(zoomText), "Fit");
    else std::snprintf(zoomText, sizeof(zoomText), "%.0f%%", xform.zoom * 100.0f);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::BeginCombo("##zoomcombo", zoomText)) {
        if (ImGui::Selectable("Fit (Home)", v.fit)) fit();
        for (float z : {0.5f, 1.0f, 2.0f, 4.0f}) {
            char label[16];
            std::snprintf(label, sizeof(label), "%.0f%%", z * 100.0f);
            if (ImGui::Selectable(label, !v.fit && std::abs(v.zoom - z) < 1e-3f)) setZoom(z);
        }
        ImGui::EndCombo();
    }

    // Bake (R9)
    nextItem(220.0f);
    const TitleBakeStatus& status = bakeStatus();
    const bool baking = TitleArtAutoBake::get().baking(doc.document()->absolutePath());
    std::string bakeLabel = baking ? "Baking..." : status.allFresh() ? "Up to date" : status.anyBaked() ? "Out of date" : "Not baked";
    if (!editable) ImGui::BeginDisabled();
    if (ImGui::Button((ICON_FA_FIRE " Bake (" + bakeLabel + ")###bake").c_str())) bake();
    if (!editable) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string text = "Bake every output now (Ctrl+B)";
        for (const TitleOutputState& output : status.outputs) {
            text += "\n" + output.id + ": " + titleOutputStatusLabel(output.status) + (output.reason.empty() ? std::string() : " (" + output.reason + ")");
        }
        ImGui::SetTooltip("%s", text.c_str());
    }
    ImGui::SameLine();
    bool autoBake = project->getTitleArtSettings().autoBake;
    if (ImGui::Checkbox("Auto###autobake", &autoBake)) {
        TitleArtProjectSettings next = project->getTitleArtSettings();
        next.autoBake = autoBake;
        project->getProjectCommandHistory()->addCommand(new TitleArtSettingsCmd(project, project->getTitleArtSettings(), next));
    }
    tooltip("Auto Bake: rebake the outputs 500 ms after the last edit (Project Settings > Title Art)");

    // lint badge (R53)
    nextItem(100.0f);
    const size_t issues = findings.size();
    char lintText[64];
    if (issues == 0) std::snprintf(lintText, sizeof(lintText), ICON_FA_CHECK " %zu checks###lint", checksRun);
    else std::snprintf(lintText, sizeof(lintText), ICON_FA_TRIANGLE_EXCLAMATION " %zu###lint", issues);
    if (ImGui::Button(lintText)) {
        lintPopup = true;
        ImGui::OpenPopup("##lintpopup");
    }
    tooltip(issues == 0 ? "No issues found by the checks (measurements, not approval)" : "Style lint: findings with fixes");
    drawLintPopup();

    // Stats (R72)
    nextItem(70.0f);
    if (ImGui::Checkbox("Stats###stats", &settings.stats)) settings.save();
    tooltip("Per-pass GPU times and preview budget use (acquires GPU timing while on)");

    for (const ToolbarItemDef& item : toolbarItems()) {
        if (item.overflowOnly || !item.draw) continue;
        nextItem(80.0f);
        item.draw(doc);
    }
    if (project && project->isPlaySessionActive()) {
        nextItem(260.0f);
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), ICON_FA_PLAY " Playing: changes are saved to the asset");
    }
}

void TitleDesignerWindow::drawStatusBar() {
    JE_PROFILE_SCOPE("TD drawStatusBar");
    ImGui::Separator();
    if (!doc.isOpen() || newerVersion()) {
        ImGui::TextDisabled("Open a Title Art: double-click one in the Project window, or Assets > Create > Title Art");
        return;
    }
    const ta::TitleArtDoc& shown = doc.shown();
    ImGui::Text("%d \xC3\x97 %d", shown.canvasWidth(), shown.canvasHeight());
    if (cursorInCanvas) {
        ImGui::SameLine();
        ImGui::TextDisabled("x %.0f  y %.0f", cursorDoc.x, cursorDoc.y);
    }
    const std::vector<std::string> layers = doc.selectedLayers();
    if (!layers.empty()) {
        TitleRect box = layerBounds(layers.back());
        ImGui::SameLine();
        ImGui::TextDisabled("%s %.0f \xC3\x97 %.0f", layers.size() > 1 ? (std::to_string(layers.size()) + " layers").c_str() : "selection", box.width, box.height);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", xform.zoom * 100.0f);
    if (canvas) {
        const CanvasRenderInfo& info = canvas->info();
        ImGui::SameLine();
        if (info.busy && info.runningMs > 250.0) {
            ImGui::Text("Rendering %u/%u passes", info.passesDone, info.passesTotal);
        } else if (info.hasImage) {
            ImGui::TextDisabled("%s \xC2\xB7 %.1f ms \xC2\xB7 %d \xC3\x97 %d \xC2\xB7 cached %zu of %zu layers", info.draft ? "Draft" : "Full",
                                info.lastRenderMs, info.imageWidth, info.imageHeight, info.cachedLayers, info.totalLayers);
        } else if (!info.error.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Render failed: %s", info.error.c_str());
        }
    }
    if (TitleArtAutoBake::get().baking(doc.document()->absolutePath())) {
        ImGui::SameLine();
        ImGui::Text(ICON_FA_FIRE " Baking...");
    }
    if (textEditing()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Enter: new line \xC2\xB7 Ctrl+Enter: done \xC2\xB7 Esc: cancel");
    }
    if (!notice.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", notice.c_str());
    }
    if (!errorText.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), ICON_FA_CIRCLE_EXCLAMATION " %s", errorText.c_str());
    }
    if (!toasts.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", toasts.back().c_str());
    }
    if (!chipText.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.75f, 0.7f, 1.0f, 1.0f), ICON_FA_ROBOT " %s", chipText.c_str());
        // Undo while the agent's step is the timeline's newest (it undoes that step only)
        const UndoTimeline::EntryView next = UndoTimeline::nextUndo(project->getSelectedSceneId());
        if (chipEntry && next && next.entry->info.id == chipEntry) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Undo###chipundo")) {
                std::string why;
                if (!UndoTimeline::undo(project->getSelectedSceneId(), &why)) reportError(why);
                chipText.clear();
            }
        }
    }
    if (!renamePromptOld.empty()) {
        ImGui::SameLine();
        ImGui::Text("Rename title to %s?", renamePromptNew.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename###renametitle")) {
            commitNamed(editsToOps({{std::string(), "name", renamePromptNew}}), "Rename Title");
            renamePromptOld.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep###keeptitle")) renamePromptOld.clear();
    }
    if (settings.stats) drawStats();
}

void TitleDesignerWindow::drawStats() {
    if (!canvas) return;
    if (!ImGui::BeginTable("##stats", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Layer / Pass");
    ImGui::TableSetupColumn("Passes");
    ImGui::TableSetupColumn("ms");
    ImGui::TableHeadersRow();
    for (const ta::NodeCost& cost : canvas->info().nodeCosts) {
        const ta::Json* layer = layerById(doc.shown(), cost.layerId);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(layer ? layer->value("name", std::string("?")).c_str() : cost.layerId.empty() ? "Document" : cost.layerId.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", cost.passes);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", cost.measuredUs / 1000.0);
    }
    const PassList passes = SystemRender::getPassList(TITLE_DESIGNER_VIEW_ID, true);
    for (const PassRecord& pass : passes.passes) {
        if (pass.hidden) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("  %s", pass.label);
        ImGui::TableNextColumn();
        ImGui::Text("%u draws", pass.draws);
        ImGui::TableNextColumn();
        if (pass.gpuValid) ImGui::Text("%.3f", pass.gpuMs);
        else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
    const TitlePreviewStats& queue = TitlePreviewQueue::editor().stats();
    const ta::RendererStats* renderer = TitleArtRenderHost::editor().stats();
    ImGui::TextDisabled("Preview queue %zu \xC2\xB7 last frame %.2f / %.1f ms \xC2\xB7 targets %.1f MB", queue.queued, queue.lastFrameMs,
                        settings.previewBudgetMs, renderer ? renderer->bytesLive / (1024.0 * 1024.0) : 0.0);
}
