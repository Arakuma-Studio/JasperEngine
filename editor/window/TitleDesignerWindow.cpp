// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerWindow.h"
#include "titleart/anim/TitleAnimationPanel.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "external/IconsFontAwesome6.h"
#include "io/AtomicFile.h"
#include "profiler/Profile.h"
#include "render/SceneRender2D.h"
#include "render/SystemRender.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtSettings.h"
#include "titleart/TitlePreviewQueue.h"
#include "titleart/designer/TitleCanvasView.h"
#include "titleart/designer/TitleGizmo2D.h"
#include "titleart/designer/TitleGizmo3D.h"
#include "titleart/designer/TitleHandles.h"
#include "titleart/designer/TitleLayerThumbnails.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitleTextEdit.h"
#include "titleart/fonts/FontCatalog.h"
#include "titleart/fonts/FontPreviewRaster.h"
#include "util/FileUtils.h"
#include "util/ImePreedit.h"
#include "util/SafeWrite.h"
#include "util/SystemFonts.h"
#include "util/UIProbe.h"
#include "window/DeviceProfiles.h"
#include "window/InspectorWindowTitleArt.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    // the window's preview owner (TitlePreviewQueue::cancelAll on close)
    constexpr TitlePreviewOwner OWNER = 0x5449544c45ull;      // "TITLE"

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    bool isSoftwareRenderer(const std::string& device) {
        std::string lower = device;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return lower.find("llvmpipe") != std::string::npos || lower.find("swiftshader") != std::string::npos ||
               lower.find("basic render") != std::string::npos || lower.find("softpipe") != std::string::npos;
    }

}

TitleDesignerWindow::TitleDesignerWindow(Project* project) : project(project), doc(project) {
    settings = TitleDesignerSettings::load();
    // S81's, S83's and S86's rows, modes and gizmo target (§6.8)
    registerTitleDesignerExtensions();
    titledesigner::registerTitleAnimationPanel(&settings);
    // reopened as it was: its live parts are made by the first show() (R64, R65)
    windowOpen = settings.windowOpen;
}

TitleDesignerWindow::~TitleDesignerWindow() {
    destroyLive();
}

double TitleDesignerWindow::now() const {
    return clockOverride ? clockOverride() : ImGui::GetTime();
}

// ---- lifecycle (R65) ----

void TitleDesignerWindow::createLive() {
    if (canvas) return;
    canvas = std::make_unique<TitleCanvasView>(project);
    thumbnails = std::make_unique<TitleLayerThumbnails>(project, OWNER);
    libraryTiles = std::make_unique<TitleLayerThumbnails>(project, OWNER);
    // the Library's live tiles (S84's provider): the item's ops on the user's layer, through the one preview queue
    if (TitleLibraryProvider* provider = libraryProvider()) {
        provider->attachTileSource([this](LibraryTab tab, const LibraryItem& item, float width, float height, bool visible) {
            return libraryTile(tab, item, width, height, visible);
        });
    }
    textEdit = std::make_unique<TitleTextEdit>();
    gizmo2d = std::make_unique<TitleGizmo2D>();
    gizmo3d = std::make_unique<TitleGizmo3D>();
    handles = std::make_unique<TitleHandles>();
    transient.setBase(TitleArtRenderHost::editor().resolver(project));
    doc.setEditRouter(editRouter());
    // R70: a software renderer starts at Draft with an 8 ms budget, said once
    softwareRenderer = isSoftwareRenderer(SystemRender::getDeviceName());
    if (softwareRenderer) {
        if (!settings.qualitySet) settings.quality = Quality::Draft;
        if (!settings.budgetSet) settings.previewBudgetMs = 8.0f;
        if (!settings.softwareNoticeShown) {
            notice = "Software renderer: previews at Draft quality";
            noticeUntil = now() + 8.0;
            settings.softwareNoticeShown = true;
            settings.save();
        }
    }
}

void TitleDesignerWindow::destroyLive() {
    if (textEdit && textEdit->active()) textEdit->end();
    if (doc.gestureActive()) doc.cancelGesture();
    doc.clearPreview();
    TitlePreviewQueue::editor().cancelAll(OWNER);
    if (TitleLibraryProvider* provider = library()) provider->cancelAll();
    if (gpuTimingHeld) {
        SystemRender::releaseGpuTiming();
        gpuTimingHeld = false;
    }
    for (const PreviewModeDef& mode : previewModes()) {
        if (mode.close) mode.close();
    }
    livePreviews.clear();
    liveStart = -1.0;
    canvas.reset();
    thumbnails.reset();
    if (TitleLibraryProvider* provider = libraryProvider()) {
        provider->cancelAll();
        provider->attachTileSource(nullptr);
    }
    libraryTiles.reset();
    textEdit.reset();
    gizmo2d.reset();
    gizmo3d.reset();
    handles.reset();
    gizmo3dTarget.reset();
    fontCatalog.reset();
    fontStrips.reset();
    transient.clear();
    transient.setBase(nullptr);
    fontPickerOpen = false;
    addOpen = false;
}

void TitleDesignerWindow::setOpen(bool open) {
    if (open == windowOpen && (open == (canvas != nullptr))) return;
    windowOpen = open;
    if (settings.windowOpen != open) {
        settings.windowOpen = open;
        settings.save();
    }
    if (open) {
        createLive();
        openFirstTime();
    } else {
        saveView();
        destroyLive();
    }
}

void TitleDesignerWindow::restoreFromSettings() {
    settings = TitleDesignerSettings::load();
    if (settings.windowOpen && !windowOpen) {
        windowOpen = true;
        focusRequested = true;
    }
}

void TitleDesignerWindow::resetForProjectSwitch() {
    saveView();
    destroyLive();
    doc.close();
    workspace.clear();
    workspaceProject.clear();
    inContext.reset();
    if (windowOpen) createLive();
}

void TitleDesignerWindow::loadWorkspace() {
    if (!project) return;
    const fs::path path = project->getProjectPath();
    if (path == workspaceProject) return;
    workspace.load(path);
    workspaceProject = path;
}

void TitleDesignerWindow::openFirstTime() {
    loadWorkspace();
    if (doc.isOpen()) return;
    // the last document, else the empty "Open a Title Art" state
    for (const std::string& recent : workspace.recent()) {
        std::string error;
        if (open(project->getAssetsPath() / FileUtils::pathFromUtf8(recent), &error)) return;
    }
}

DocumentView& TitleDesignerWindow::view() {
    static DocumentView fallback;
    if (!doc.isOpen()) {
        fallback = DocumentView();
        return fallback;
    }
    return workspace.view(doc.committed().guid());
}

const DocumentView* TitleDesignerWindow::viewOrNull() const {
    if (!doc.isOpen()) return nullptr;
    return &const_cast<TitleDesignerWorkspace&>(workspace).view(doc.committed().guid());
}

void TitleDesignerWindow::saveView() {
    if (!doc.isOpen()) return;
    rememberSelection();
    workspace.save();
}

void TitleDesignerWindow::rememberSelection() {
    if (!doc.isOpen()) return;
    std::vector<std::string> paths;
    for (const NodeRef& node : doc.selection()) paths.push_back(nodePath(doc.committed(), node));
    view().selection = paths;
}

void TitleDesignerWindow::restoreSelection() {
    std::vector<NodeRef> nodes;
    for (const std::string& path : view().selection) {
        NodeRef node;
        if (nodeFromPath(doc.committed(), path, node, nullptr) && node.kind != NodeKind::Document) nodes.push_back(node);
    }
    doc.select(nodes);
}

bool TitleDesignerWindow::open(const fs::path& file, std::string* error) {
    loadWorkspace();
    if (doc.isOpen()) saveView();
    if (textEdit && textEdit->active()) endTextEdit(true);
    std::string why;
    if (!doc.open(file, &why)) {
        if (error) *error = why;
        return false;
    }
    if (inContext && fs::path(inContext->document).lexically_normal() != doc.document()->absolutePath().lexically_normal()) inContext.reset();
    workspace.touch(doc.document()->assetPath());
    restoreSelection();
    if (canvas) canvas->clear();
    if (thumbnails) thumbnails->cancelAll();
    if (libraryTiles) libraryTiles->cancelAll();
    indexChange = ~0ull;
    lintChange = ~0ull;
    searchText.clear();
    findings.clear();
    livePreviews.clear();
    liveStart = -1.0;
    realtimeCheckedAt = -1.0;
    workspace.save();
    if (!windowOpen) setOpen(true);
    focusRequested = true;
    return true;
}

bool TitleDesignerWindow::openInContext(const TitleInContext& context, std::string* error) {
    if (!open(context.document, error)) return false;
    inContext = context;
    BackdropChoice backdrop = view().backdrop;
    backdrop.kind = "in_context";
    view().backdrop = backdrop;
    return true;
}

fs::path TitleDesignerWindow::documentPath() const {
    return doc.isOpen() ? doc.document()->absolutePath() : fs::path();
}

bool TitleDesignerWindow::windowRect(ImVec2& min, ImVec2& max) const {
    if (!rectValid) return false;
    min = rectMin;
    max = rectMax;
    return true;
}

// ---- scripted API ----

bool TitleDesignerWindow::select(const std::vector<std::string>& paths, std::string* error) {
    if (!doc.isOpen()) {
        if (error) *error = "No Title Art is open";
        return false;
    }
    std::vector<NodeRef> nodes;
    for (const std::string& path : paths) {
        NodeRef node;
        std::string why;
        if (!nodeFromPath(doc.shown(), path, node, &why)) {
            if (error) *error = "select: " + why;
            return false;
        }
        if (node.kind != NodeKind::Document) nodes.push_back(node);
    }
    if (textEdit && textEdit->active()) endTextEdit(true);
    doc.select(nodes);
    return true;
}

bool TitleDesignerWindow::runOps(const ta::Json& ops, const std::string& stepName, std::string* error) {
    return doc.commit(ops, stepName, error);
}

void TitleDesignerWindow::commitNamed(const ta::Json& ops, const std::string& name) {
    std::string error;
    if (!doc.commit(ops, name, &error)) reportError(error);
}

bool TitleDesignerWindow::needsFrames() const {
    if (!windowOpen || !canvas) return false;
    const CanvasRenderInfo& info = canvas->info();
    return info.busy || info.rebuilding || now() - lastViewChange < 0.5 || doc.gestureActive() || (thumbnails && thumbnails->pendingCount() > 0) ||
           (libraryTiles && libraryTiles->pendingCount() > 0) ||
           textEditing() || drag != Drag::None || !livePreviews.empty() || engineFramesWanted();
}

bool TitleDesignerWindow::typing() const {
    return ImGui::GetIO().WantTextInput || textEditing();
}

void TitleDesignerWindow::setTool(Tool tool) {
    currentTool = tool;
}

const PreviewModeDef* TitleDesignerWindow::activePreviewMode() const {
    const DocumentView* v = viewOrNull();
    if (!v) return nullptr;
    for (const PreviewModeDef& mode : previewModes()) {
        if (mode.id == v->previewAs) return &mode;
    }
    return nullptr;
}

bool TitleDesignerWindow::setPreviewAs(const std::string& mode, std::string* error) {
    // the mode left frees what it made (S81's Glyph Atlas title)
    auto leave = [&] {
        const PreviewModeDef* previous = activePreviewMode();
        if (previous && previous->id != mode && previous->close) previous->close();
    };
    if (mode == "realtime" || mode == "baked") {
        leave();
        view().previewAs = mode;
        return true;
    }
    for (const PreviewModeDef& def : previewModes()) {
        if (def.id == mode) {
            leave();
            view().previewAs = mode;
            return true;
        }
    }
    if (error) {
        std::string valid = "realtime, baked";
        for (const PreviewModeDef& def : previewModes()) valid += ", " + def.id;
        *error = "preview_as '" + mode + "' is not available (valid: " + valid + ")";
    }
    return false;
}

bool TitleDesignerWindow::setBackground(const BackdropChoice& backdrop, std::string* error) {
    static const char* const kinds[] = {"checker", "black", "white", "gray", "color", "image", "game_scene", "in_context"};
    bool known = false;
    for (const char* k : kinds) known = known || backdrop.kind == k;
    if (!known) {
        if (error) *error = "background '" + backdrop.kind + "' is not one of checker, black, white, gray, color, image, game_scene, in_context";
        return false;
    }
    if (backdrop.kind == "in_context" && !inContext) {
        if (error) *error = "background in_context needs the window opened from a Title Art Renderer";
        return false;
    }
    view().backdrop = backdrop;
    return true;
}

bool TitleDesignerWindow::setDevice(const DeviceChoice& device, std::string* error) {
    if (!device.profile.empty() && device.profile.rfind("res:", 0) != 0 && !findDeviceProfile(device.profile)) {
        if (error) {
            std::string valid;
            for (const DeviceProfile& p : deviceProfiles()) valid += (valid.empty() ? "" : ", ") + std::string(p.id);
            *error = "device profile '" + device.profile + "' is unknown (valid: " + valid + ", or res:<W>x<H>)";
        }
        return false;
    }
    view().device = device;
    return true;
}

void TitleDesignerWindow::setZoom(float zoom) {
    DocumentView& v = view();
    v.fit = false;
    v.zoom = std::clamp(zoom, 1.0f / 16.0f, 32.0f);
    lastViewChange = now();
}

void TitleDesignerWindow::fit() {
    view().fit = true;
    lastViewChange = now();
}

void TitleDesignerWindow::setSearch(const std::string& query) {
    searchText = query;
    highlightedHit = 0;
    stableOrder.clear();
}

bool TitleDesignerWindow::setPanel(const std::string& panel, std::string* error) {
    if (panel == "layers" || panel == "history") {
        sideTab = panel;
        settings.layersCollapsed = false;
        narrowTabRequest = panel;
    } else if (panel == "properties") {
        settings.propertiesCollapsed = false;
        narrowTabRequest = panel;
    } else if (panel == "library") {
        // shown, also on a narrow window where it folds to its header (R3)
        settings.libraryCollapsed = false;
        libraryNarrowOpen = true;
    } else {
        if (error) *error = "panel '" + panel + "' is not one of layers, properties, library, history";
        return false;
    }
    panelFocus = panel;
    return true;
}

bool TitleDesignerWindow::setLibrary(const std::string& tab, const std::string& family, const std::string& query, std::string* error) {
    if (!tab.empty()) {
        LibraryTab parsed;
        if (!libraryTabFromName(tab, parsed)) {
            if (error) *error = "library tab '" + tab + "' is not one of looks, color_schemes, templates";
            return false;
        }
        view().libraryTab = tab;
    }
    view().libraryFamily = family;
    view().libraryQuery = query;
    settings.libraryCollapsed = false;
    return true;
}

bool TitleDesignerWindow::previewItem(const PreviewItem& item, std::string* error) {
    return doc.setPreview(item, error);
}

void TitleDesignerWindow::clearPreview() {
    doc.clearPreview();
}

bool TitleDesignerWindow::commitPreview(std::string* error) {
    return doc.commitPreview(error);
}

void TitleDesignerWindow::bake() {
    if (!doc.isOpen() || !doc.editable()) return;
    TitleArtAutoBake::get().requestBake(project, doc.document()->absolutePath(), {}, true);
}

bool TitleDesignerWindow::snapshotScene(std::string* error) {
    if (!canvas || !doc.isOpen()) {
        if (error) *error = "No Title Art is open";
        return false;
    }
    const std::string scenePath = view().backdrop.scene;
    SceneProject* scene = nullptr;
    if (!scenePath.empty()) {
        const uint32_t id = project->findSceneByPath(FileUtils::pathFromUtf8(scenePath));
        scene = project->getScene(id);
    }
    if (!scene) scene = project->getSelectedScene();
    if (!scene || !scene->scene) {
        if (error) *error = "Open the scene to snapshot first";
        return false;
    }
    // the Device's resolution, else the canvas size
    const TitleRect stage = stageRect();
    const int width = (int)std::round(stage.width), height = (int)std::round(stage.height);
    const std::string key = scenePath.empty() ? scene->filepath.generic_string() : scenePath;
    const fs::path cached = project->getProjectInternalPath() / "titleart" / "snapshots" / (std::to_string(std::hash<std::string>{}(key)) + ".png");
    canvas->requestSceneSnapshot(scene->scene, scene->scene->getCamera(), width, height,
                                 [this, cached, key](std::vector<uint8_t> rgba, int w, int h, std::string why) {
                                     if (rgba.empty()) {
                                         reportError("Snapshot failed: " + why);
                                         return;
                                     }
                                     snapshotRgba = std::move(rgba);
                                     snapshotWidth = w;
                                     snapshotHeight = h;
                                     snapshotKey = key;
                                     texsynth::Image image = texsynth::Image::make(w, h, 4, 8);
                                     image.data = snapshotRgba;
                                     std::vector<uint8_t> png;
                                     if (texsynth::encodePng(image, png)) {
                                         safewrite::EditorInternal token(cached.parent_path());
                                         io::AtomicWriteOptions options;
                                         options.durability = io::Durability::Cache;
                                         io::writeFileAtomic(cached, std::string_view((const char*)png.data(), png.size()), options);
                                     }
                                 });
    BackdropChoice b = view().backdrop;
    b.kind = "game_scene";
    b.scene = key;
    view().backdrop = b;
    return true;
}

ta::Json TitleDesignerWindow::state() const {
    ta::Json out = ta::Json::object();
    out["open"] = windowOpen;
    if (!doc.isOpen()) {
        out["path"] = nullptr;
        return out;
    }
    out["path"] = doc.document()->projectPath();
    ta::Json selection = ta::Json::array();
    for (const NodeRef& node : doc.selection()) selection.push_back(nodePath(doc.shown(), node));
    out["selection"] = selection;
    out["panel"] = panelFocus.empty() ? sideTab : panelFocus;
    const DocumentView* v = viewOrNull();
    out["zoom"] = v && v->fit ? ta::Json("fit") : ta::Json(v ? v->zoom : 1.0f);
    out["zoom_value"] = xform.zoom;
    out["preview_as"] = v ? v->previewAs : std::string("realtime");
    ta::Json openPanels = ta::Json::object();
    for (const PanelDef& panel : panels()) openPanels[panel.id] = panelOpen(panel);
    out["panels"] = openPanels;
    out["animation_panel"] = titledesigner::titleAnimationPanelState();
    out["canvas_origin"] = ta::Json::array({xform.origin.x, xform.origin.y});
    out["background"] = v ? v->backdrop.kind : std::string("checker");
    out["device"] = v && !v->device.profile.empty() ? ta::Json{{"profile", v->device.profile}, {"orientation", v->device.orientation},
                                                           {"display_height", v->device.displayHeight}}
                                                     : ta::Json();
    out["tool"] = toolName(currentTool);
    out["busy"] = canvas ? canvas->info().busy : false;
    out["staged"] = doc.gestureActive() && doc.hasStaged();
    out["preview_active"] = doc.previewActive();
    out["last_render_ms"] = canvas ? canvas->info().lastRenderMs : 0.0;
    if (canvas) {
        // what the canvas shows: an agent tells an empty render from a missing one
        const CanvasRenderInfo& info = canvas->info();
        ta::Json problems = ta::Json::array();
        for (const ta::TitleProblem& p : info.problems) problems.push_back(p.code);
        out["canvas"] = {{"has_image", info.hasImage}, {"title_drawn", info.titleDrawn}, {"draft", info.draft}, {"px_scale", info.shownPxScale},
                         {"image", ta::Json::array({info.imageWidth, info.imageHeight})},
                         {"frame", ta::Json::array({info.shownFrame.x, info.shownFrame.y, info.shownFrame.width, info.shownFrame.height})},
                         {"error", info.error}, {"problems", problems}};
    }
    out["editable"] = doc.editable();
    if (!doc.editable()) out["read_only"] = doc.readOnlyReason();
    out["search"] = searchText;
    out["text_editing"] = textEditing();
    out["keyboard_focus"] = focusedRegion >= 0 ? ta::Json(regionName(focusedRegion)) : ta::Json();
    out["timings"] = {{"frame_ms", frameMs}, {"frame_ms_avg", frameSamples.empty() ? 0.0 : frameMsSum / (double)frameSamples.size()},
                      {"frame_ms_max", frameMsMax}, {"frames", frameSamples.size()}, {"index_ms", indexMs}, {"query_ms", queryMs},
                      {"index_entries", index.size()}, {"document_passes", documentPasses},
                      {"index_rows_ms", lastCollectTimes().rows}, {"index_lookup_ms", lastCollectTimes().lookup},
                      {"index_values_ms", lastCollectTimes().values}, {"index_add_ms", lastCollectTimes().index},
                      {"index_rows", lastCollectTimes().rowCount}, {"index_building", indexBuild.active},
                      {"index_frames", indexFrames}, {"index_frame_ms_max", indexFrameMsMax}, {"index_unit_ms_max", indexUnitMsMax},
                      {"index_units", [&] {
                          ta::Json units = ta::Json::array();
                          for (const auto& [name, ms] : indexUnitCosts) units.push_back(ta::Json::array({name, ms}));
                          return units;
                      }()}, {"index_units_frame", indexUnitsLastFrame}};
    out["library"] = {{"provider", libraryProvider() ? "registered" : "none"}, {"shown", libraryShown}};
    if (addOpen) {
        // the Add palette as drawn last: its rail with counts and the listed entries
        ta::Json rail = ta::Json::array();
        for (const auto& [category, count] : addRail) rail.push_back({{"category", category}, {"count", count}});
        out["add"] = {{"query", addQuery}, {"filter", addFilter}, {"rail", rail}, {"entries", addVisible}};
    }
    if (inContext) {
        // In Context (R30): the instance's screen rectangle once found, on its screen
        out["in_context"] = {{"located", inContext->located}, {"snapshot", !inContext->snapshotRgba.empty()},
                             {"rect", ta::Json::array({inContext->x, inContext->y, inContext->width, inContext->height})},
                             {"screen", ta::Json::array({inContext->screenWidth, inContext->screenHeight})}};
    }
    if (gizmo3d && gizmo3d->visible()) {
        // S82 R35's schematic where it is drawn (window coords, as `rect`): its centre and the
        // free-rotate ring's radius
        out["gizmo_3d"] = {{"centre", ta::Json::array({gizmo3d->centre().x, gizmo3d->centre().y})}, {"ring", rotation3d::RING_PX}};
    }
    ta::Json live = ta::Json::array();
    for (const auto& [layer, effect] : livePreviews) live.push_back(nodePath(doc.shown(), {NodeKind::Effect, layer, {}, effect}));
    out["live_previews"] = live;
    ta::Json notices = ta::Json::array();
    if (cjkNoticeShown) notices.push_back("cjk_font_missing");
    out["notices"] = notices;
    if (rectValid) out["rect"] = ta::Json::array({rectMin.x, rectMin.y, rectMax.x - rectMin.x, rectMax.y - rectMin.y});
    return out;
}

// ---- PropertyHost ----

bool TitleDesignerWindow::sectionOpen(const std::string& key, bool defaultOpen) {
    auto it = settings.sections.find(key);
    return it == settings.sections.end() ? defaultOpen : it->second;
}

void TitleDesignerWindow::setSectionOpen(const std::string& key, bool open) {
    settings.sections[key] = open;
    settings.save();
}

std::string TitleDesignerWindow::flashingRow() const {
    return now() < flashUntil ? flashRow : std::string();
}

void TitleDesignerWindow::openAddPalette(const std::string& filter) {
    addFilter = filter;
    addQuery.clear();
    addCategory.clear();
    addHighlight = 0;
    addOpenRequested = true;
}

void TitleDesignerWindow::openAddPalette(const std::string& filter, const std::string& query) {
    openAddPalette(filter);
    addQuery = query;
    // focus_window focuses the window one frame after its keys run, which closes popups: the
    // palette opens two frames later, over it
    addOpenRequested = false;
    addOpenDelay = 3;
}

void TitleDesignerWindow::closeAddPalette() {
    addOpenRequested = false;
    addCloseRequested = addOpen;
}

void TitleDesignerWindow::exposeField(const FieldRef& target, const std::string& label) {
    exposeTarget = target;
    exposeLabel = label;
    exposeName.clear();
    for (char c : label) {
        if (std::isalnum((unsigned char)c)) exposeName += (char)std::tolower((unsigned char)c);
        else if (!exposeName.empty() && exposeName.back() != '_') exposeName += '_';
    }
    while (!exposeName.empty() && exposeName.back() == '_') exposeName.pop_back();
    exposeOpen = true;
}

void TitleDesignerWindow::selectNode(const NodeRef& node) {
    if (textEdit && textEdit->active()) endTextEdit(true);
    doc.selectOne(node);
}

const ta::AssetResolver* TitleDesignerWindow::assets() {
    return &transient;
}

bool TitleDesignerWindow::selectedCharacters(std::string& layer, std::vector<int>& indexes) {
    if (!textEditing()) return false;
    int first = 0, end = 0;
    textEdit->selection(first, end);
    layer = textEdit->layerId();
    indexes.clear();
    for (int i = first; i < end; i++) indexes.push_back(i);
    return true;
}

void TitleDesignerWindow::reportError(const std::string& message) {
    if (message.empty()) return;
    errorText = message;
    errorUntil = now() + 6.0;
}

// ---- actions (§6.3) ----

bool TitleDesignerWindow::addLayer(const std::string& type, const ta::Json& values, const std::string& extra) {
    if (!doc.editable()) return false;
    ta::Json op = {{"op", "add_layer"}, {"type", type}};
    if (!values.empty()) op["values"] = values;
    if (type == "generator") op["generator"] = extra.empty() ? std::string("fill") : extra;
    if (type == "adjustment") op["adjustment"] = extra.empty() ? std::string("hue_saturation") : extra;
    // above the primary layer, inside its group
    const std::vector<std::string> layers = doc.selectedLayers();
    if (!layers.empty()) op["after"] = "id:" + layers.back();
    std::string label = ta::layerTypeLabel(ta::layerTypeFromName(type));
    if (type == "generator") {
        const ta::GeneratorDef* def = ta::findGenerator(extra);
        label = def ? def->label : extra;
    }
    std::string error;
    if (!doc.commit(ta::Json::array({op}), "Add Layer " + label, &error)) {
        reportError(error);
        return false;
    }
    const std::string newId = doc.newId(0, "layer");
    if (!newId.empty()) {
        doc.selectOne({NodeKind::Layer, newId, {}, {}});
        // a new text starts editing with its placeholder selected (R36)
        if (type == "text") beginTextEdit(newId, true);
    }
    return true;
}

bool TitleDesignerWindow::deleteSelection() {
    const std::vector<NodeRef> selection = doc.selection();
    if (selection.empty() || !doc.editable()) return false;
    ta::Json ops = ta::Json::array();
    size_t layers = 0;
    std::string name;
    for (const NodeRef& node : selection) {
        const ta::Json* layer = layerById(doc.committed(), node.layer);
        if (!layer) continue;
        if (node.kind == NodeKind::Layer) {
            ops.push_back({{"op", "remove_layer"}, {"layer", "id:" + node.layer}});
            layers++;
        } else if (node.kind == NodeKind::Effect) {
            const std::string path = nodePath(doc.committed(), node);
            const size_t slash = path.rfind('/');
            const std::string effect = path.substr(slash + 1);
            ta::Json op = {{"op", "remove_effect"}, {"layer", "id:" + node.layer}, {"effect", node.part.empty() ? effect : nodePath(doc.committed(), node).substr(layerPathOf(doc.committed(), node.layer).size() + 1)}};
            ops.push_back(op);
            name = "Remove Effect";
        } else if (node.kind == NodeKind::Mask) {
            ops.push_back({{"op", "remove_mask"}, {"layer", "id:" + node.layer}, {"mask", "mask#" + std::to_string(maskIndexOf(*layer, node.item))}});
            name = "Remove Mask";
        }
        // parts are never deleted from the keyboard (Pachislop's rule): rims go from their row's menu
    }
    if (ops.empty()) return false;
    if (layers > 0) name = layers == 1 ? "Delete Layer" : "Delete " + std::to_string(layers) + " Layers";
    std::string error;
    if (!doc.commit(ops, name, &error)) {
        reportError(error);
        return false;
    }
    doc.select({});
    return true;
}

bool TitleDesignerWindow::duplicateSelection() {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return false;
    ta::Json ops = ta::Json::array();
    for (const std::string& id : layers) ops.push_back({{"op", "duplicate_layer"}, {"layer", "id:" + id}});
    std::string error;
    if (!doc.commit(ops, layers.size() == 1 ? "Duplicate Layer" : "Duplicate " + std::to_string(layers.size()) + " Layers", &error)) {
        reportError(error);
        return false;
    }
    std::vector<NodeRef> made;
    for (const std::string& id : doc.newIds("layer")) made.push_back({NodeKind::Layer, id, {}, {}});
    if (!made.empty()) doc.select(made);
    return true;
}

bool TitleDesignerWindow::groupSelection() {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return false;
    ta::Json list = ta::Json::array();
    for (const std::string& id : layers) list.push_back("id:" + id);
    std::string error;
    if (!doc.commit(ta::Json::array({ta::Json{{"op", "group"}, {"layers", list}, {"name", "Group"}}}), "Group Layers", &error)) {
        reportError(error);
        return false;
    }
    const std::string group = doc.newId(0, "layer");
    if (!group.empty()) doc.selectOne({NodeKind::Layer, group, {}, {}});
    return true;
}

bool TitleDesignerWindow::ungroupSelection() {
    ta::Json ops = ta::Json::array();
    for (const std::string& id : doc.selectedLayers()) {
        const ta::Json* layer = layerById(doc.committed(), id);
        if (layer && ta::layerTypeOf(*layer) == ta::LayerType::Group) ops.push_back({{"op", "ungroup"}, {"layer", "id:" + id}});
    }
    if (ops.empty() || !doc.editable()) return false;
    std::string error;
    if (!doc.commit(ops, "Ungroup", &error)) {
        reportError(error);
        return false;
    }
    doc.select({});
    return true;
}

bool TitleDesignerWindow::toggleClip() {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return false;
    const ta::Json* first = layerById(doc.committed(), layers[0]);
    const bool clip = !(first && first->value("clip", false));
    std::vector<FieldEdit> edits;
    for (const std::string& id : layers) edits.push_back({id, "clip", clip});
    std::string error;
    if (!doc.commit(editsToOps(edits), clip ? "Create Clipping Mask" : "Release Clipping Mask", &error)) {
        reportError(error);
        return false;
    }
    return true;
}

bool TitleDesignerWindow::arrange(const std::string& how) {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return false;
    ta::Json ops = ta::Json::array();
    const ta::TitleArtDoc& current = doc.committed();
    for (const std::string& id : layers) {
        const std::string parent = parentLayerOf(current, id);
        const ta::Json& siblings = parent.empty() ? current.layers() : ta::layerChildren(*layerById(current, parent));
        size_t index = 0;
        for (size_t i = 0; i < siblings.size(); i++) {
            if (str(siblings[i], "id") == id) index = i;
        }
        ta::Json op = {{"op", "move_layer"}, {"layer", "id:" + id}};
        // S80 stores bottom first: forward is a higher index
        if (how == "forward") {
            if (index + 1 >= siblings.size()) continue;
            op["after"] = "id:" + str(siblings[index + 1], "id");
        } else if (how == "backward") {
            if (index == 0) continue;
            op["before"] = "id:" + str(siblings[index - 1], "id");
        } else if (how == "front") {
            if (index + 1 >= siblings.size()) continue;
            op["after"] = "id:" + str(siblings[siblings.size() - 1], "id");
        } else if (how == "back") {
            if (index == 0) continue;
            op["before"] = "id:" + str(siblings[0], "id");
        }
        ops.push_back(op);
    }
    if (ops.empty()) return false;
    const char* names[][2] = {{"forward", "Bring Forward"}, {"backward", "Send Backward"}, {"front", "Bring to Front"}, {"back", "Send to Back"}};
    std::string name = "Move Layer";
    for (const auto& n : names) {
        if (how == n[0]) name = n[1];
    }
    std::string error;
    if (!doc.commit(ops, name, &error)) {
        reportError(error);
        return false;
    }
    return true;
}

bool TitleDesignerWindow::toggleFlag(const char* member) {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return false;
    const ta::Json* first = layerById(doc.committed(), layers[0]);
    const bool defaultValue = std::string(member) == "visible";
    const bool value = !(first && first->value(member, defaultValue));
    std::vector<FieldEdit> edits;
    for (const std::string& id : layers) edits.push_back({id, member, value});
    std::string name;
    const std::string m = member;
    const std::string count = layers.size() == 1 ? "Layer" : std::to_string(layers.size()) + " Layers";
    if (m == "visible") name = (value ? "Show " : "Hide ") + count;
    else if (m == "locked") name = (value ? "Lock " : "Unlock ") + count;
    else name = (value ? "Make Guide " : "Unmake Guide ") + count;
    std::string error;
    if (!doc.commit(editsToOps(edits), name, &error)) {
        reportError(error);
        return false;
    }
    return true;
}

bool TitleDesignerWindow::moveLayers(const std::vector<std::string>& layers, const std::string& target, const std::string& where) {
    if (layers.empty() || !doc.editable()) return false;
    const ta::TitleArtDoc& current = doc.committed();
    // a group cannot go into itself or its descendants
    for (const std::string& id : layers) {
        std::string walk = target;
        while (!walk.empty()) {
            if (walk == id && (where == "into" || walk != target)) return false;
            walk = parentLayerOf(current, walk);
        }
        if (id == target) return false;
    }
    ta::Json ops = ta::Json::array();
    bool clipChange = false;
    const ta::Json* targetLayer = target.empty() ? nullptr : layerById(current, target);
    for (const std::string& id : layers) {
        ta::Json op = {{"op", "move_layer"}, {"layer", "id:" + id}};
        if (where == "into") op["parent"] = "id:" + target;
        else if (where == "above") op["after"] = "id:" + target;
        else if (where == "below") op["before"] = "id:" + target;
        ops.push_back(op);
        // between clipped rows a layer clips; dragged out it is released (R14)
        const ta::Json* layer = layerById(current, id);
        const bool clipped = layer && layer->value("clip", false);
        const bool intoClip = targetLayer && where == "above" && targetLayer->value("clip", false);
        if (intoClip != clipped && where != "into") {
            ops.push_back({{"op", "set"}, {"layer", "id:" + id}, {"values", {{"clip", intoClip}}}});
            clipChange = true;
        }
    }
    std::string name = layers.size() == 1 ? "Move Layer" : "Move " + std::to_string(layers.size()) + " Layers";
    if (clipChange) name += " (Clipped)";
    std::string error;
    if (!doc.commit(ops, name, &error)) {
        reportError(error);
        return false;
    }
    return true;
}

bool TitleDesignerWindow::copySelection(bool cut) {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty()) return false;
    ta::Json clip = ta::Json::array();
    for (const std::string& id : layers) {
        const ta::Json* layer = layerById(doc.committed(), id);
        if (layer) clip.push_back(*layer);
    }
    const std::string text = ta::Json{{"jasperengine.titleart.layers", clip}}.dump();
    ImGui::SetClipboardText(text.c_str());
    if (cut) {
        ta::Json ops = ta::Json::array();
        for (const std::string& id : layers) ops.push_back({{"op", "remove_layer"}, {"layer", "id:" + id}});
        std::string error;
        if (!doc.commit(ops, layers.size() == 1 ? "Cut Layer" : "Cut " + std::to_string(layers.size()) + " Layers", &error)) reportError(error);
    }
    return true;
}

bool TitleDesignerWindow::paste() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !doc.editable()) return false;
    ta::Json parsed = ta::Json::parse(clip, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("jasperengine.titleart.layers")) return false;
    ta::Json ops = ta::Json::array();
    for (const ta::Json& layer : parsed["jasperengine.titleart.layers"]) ops.push_back({{"op", "add_layer"}, {"from", layer}});
    if (ops.empty()) return false;
    std::string error;
    if (!doc.commit(ops, ops.size() == 1 ? "Paste Layer" : "Paste " + std::to_string(ops.size()) + " Layers", &error)) {
        reportError(error);
        return false;
    }
    return true;
}

void TitleDesignerWindow::nudge(double dx, double dy) {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty() || !doc.editable()) return;
    // a burst of presses under 800 ms on the same selection is one step (R34)
    std::string key;
    for (const std::string& id : layers) key += id + ",";
    const double t = now();
    const bool continuing = nudgeAt >= 0.0 && t - nudgeAt < 0.8 && key == nudgeKey;
    nudgeAt = t;
    nudgeKey = key;
    static uint64_t burst = 0;
    if (!continuing) burst++;
    std::vector<FieldEdit> edits;
    const ta::TitleArtDoc& current = doc.committed();
    for (const std::string& id : layers) {
        const ta::Json* layer = layerById(current, id);
        if (!layer || layer->value("locked", false)) continue;
        // the nudge is in document space: into the parent's space
        Affine2 parentToDoc;
        std::string parent = parentLayerOf(current, id);
        std::vector<std::string> chain;
        while (!parent.empty()) {
            chain.push_back(parent);
            parent = parentLayerOf(current, parent);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            parentToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layerById(current, *it)->value("transform", ta::Json::object())));
        }
        const TitleVec2 local = parentToDoc.inverse().applyLinear({dx, dy});
        const ta::TitleTransform t0 = ta::decodeTransform(layer->value("transform", ta::Json::object()));
        edits.push_back({id, "transform.position", ta::Json::array({t0.position.x + local.x, t0.position.y + local.y})});
    }
    if (edits.empty()) return;
    std::string error;
    if (!doc.commit(editsToOps(edits), "Nudge", &error, "titleart:" + current.guid() + ":nudge:" + key + ":" + std::to_string(burst))) reportError(error);
}

// ---- commands (§6.5) ----

bool TitleDesignerWindow::runCommand(const std::string& id) {
    if (!doc.isOpen() && id != "titleDesigner.fitCanvas") return false;
    const std::string c = id.rfind("titleDesigner.", 0) == 0 ? id.substr(14) : id;
    if (c == "toolHand") setTool(Tool::Hand);
    else if (c == "toolMove") setTool(Tool::Move);
    else if (c == "toolRotate") setTool(Tool::Rotate);
    else if (c == "toolScale") setTool(Tool::Scale);
    else if (c == "toolRect") setTool(Tool::Rect);
    else if (c == "togglePivot") pivotMode = !pivotMode;
    else if (c == "frameSelected") frameSelection();
    else if (c == "fitCanvas") fit();
    else if (c == "zoomActual") setZoom(1.0f);
    else if (c == "zoomIn") zoomAbout(1.25f, ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
    else if (c == "zoomOut") zoomAbout(0.8f, ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
    else if (c == "editText") {
        const NodeRef primary = doc.primary();
        const ta::Json* layer = layerById(doc.committed(), primary.layer);
        if (!layer || ta::layerTypeOf(*layer) != ta::LayerType::Text) {
            if (contentEmpty()) return addLayer("text", ta::Json::object());
            return false;
        }
        beginTextEdit(primary.layer, false);
    } else if (c == "find") searchFocus = true;
    else if (c == "add") openAddPalette({});
    else if (c == "duplicate") return duplicateSelection();
    else if (c == "delete") return deleteSelection();
    else if (c == "rename") {
        const std::vector<std::string> layers = doc.selectedLayers();
        if (layers.empty()) return false;
        renaming = layers.back();
        const ta::Json* layer = layerById(doc.committed(), renaming);
        renameBuffer = layer ? str(*layer, "name") : std::string();
        renameFocus = true;
    } else if (c == "selectAll") {
        std::vector<NodeRef> all;
        ta::forEachLayer(doc.shown().root, [&](const ta::Json& l, const ta::Json* parent, int) {
            if (!parent) all.push_back({NodeKind::Layer, str(l, "id"), {}, {}});
            return true;
        });
        doc.select(all);
    } else if (c == "copy") return copySelection(false);
    else if (c == "cut") return copySelection(true);
    else if (c == "paste") return paste();
    else if (c == "group") return groupSelection();
    else if (c == "ungroup") return ungroupSelection();
    else if (c == "clip") return toggleClip();
    else if (c == "forward") return arrange("forward");
    else if (c == "backward") return arrange("backward");
    else if (c == "toFront") return arrange("front");
    else if (c == "toBack") return arrange("back");
    else if (c == "toggleVisible") return toggleFlag("visible");
    else if (c == "toggleLock") return toggleFlag("locked");
    else if (c == "nudgeLeft") nudge(-1, 0);
    else if (c == "nudgeRight") nudge(1, 0);
    else if (c == "nudgeUp") nudge(0, 1);
    else if (c == "nudgeDown") nudge(0, -1);
    else if (c == "nudgeLeftBig") nudge(-10, 0);
    else if (c == "nudgeRightBig") nudge(10, 0);
    else if (c == "nudgeUpBig") nudge(0, 10);
    else if (c == "nudgeDownBig") nudge(0, -10);
    else if (c == "up") {
        if (doc.previewActive()) doc.clearPreview();
        else if (doc.gestureActive()) doc.cancelGesture();
        else doc.selectParent();
    } else if (c == "bake") bake();
    else return false;
    return true;
}

// ---- per frame ----

void TitleDesignerWindow::handleExternalChange() {
    std::optional<ExternalChange> change = doc.takeExternalChange();
    if (!change) return;
    if (change->cancelledGesture) {
        notice = "The title changed on disk: your drag was cancelled";
        noticeUntil = now() + 6.0;
    }
    if (textEdit && textEdit->active() && change->origin != TitleArtChangeOrigin::Person) {
        textEdit->end();
    }
    if (change->origin == TitleArtChangeOrigin::Agent || change->origin == TitleArtChangeOrigin::Disk) {
        // the chip: who and what, Undo while that step is the newest (R74)
        CommandHistory* history = project ? project->getProjectCommandHistory() : nullptr;
        const UndoEntry* top = history ? history->top() : nullptr;
        std::string who = change->origin == TitleArtChangeOrigin::Agent ? "an agent" : "another program";
        if (top && top->info.origin.kind == UndoOriginKind::Agent && !top->info.origin.client.empty()) who = top->info.origin.client;
        chipText = "Edited by " + who + (top && change->origin == TitleArtChangeOrigin::Agent ? ": " + top->name() : std::string());
        chipEntry = top && change->origin == TitleArtChangeOrigin::Agent ? top->info.id : 0;
        chipUntil = now() + 8.0;
    }
    indexDirtyAt = now();
    lintDirtyAt = now();
}

void TitleDesignerWindow::tick() {
    JE_PROFILE_SCOPE("TD tick");
    handleExternalChange();
    if (thumbnails) thumbnails->process();
    if (libraryTiles) libraryTiles->process();
    // the stats acquire GPU timing only while shown (R72)
    if (settings.stats && !gpuTimingHeld) {
        SystemRender::acquireGpuTiming();
        gpuTimingHeld = true;
    } else if (!settings.stats && gpuTimingHeld) {
        SystemRender::releaseGpuTiming();
        gpuTimingHeld = false;
    }
    // lint: document checks 300 ms after the last change, while Properties shows (R53)
    if (doc.isOpen() && !newerVersion() && lintChange != doc.changeCount()) {
        if (lintDirtyAt < 0.0) lintDirtyAt = now();
        if (now() - lintDirtyAt >= 0.3 && !doc.gestureActive()) {
            refreshLint(false);
            lintChange = doc.changeCount();
            lintDirtyAt = -1.0;
        }
    }
}

void TitleDesignerWindow::show() {
    if (!windowOpen) return;
    // the window's own CPU work per frame (the canvas renders and draws elsewhere: processFrame)
    constexpr size_t FRAME_SAMPLES = 120;
    const auto start = std::chrono::steady_clock::now();
    showContents();
    frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (frameSamples.size() < FRAME_SAMPLES) {
        frameSamples.push_back(frameMs);
    } else {
        frameSamples[frameSampleNext] = frameMs;
        frameSampleNext = (frameSampleNext + 1) % FRAME_SAMPLES;
    }
    frameMsSum = 0.0;
    frameMsMax = 0.0;
    for (double ms : frameSamples) {
        frameMsSum += ms;
        frameMsMax = std::max(frameMsMax, ms);
    }
}

void TitleDesignerWindow::showContents() {
    JE_PROFILE_SCOPE("TD showContents");
    if (!canvas) {
        createLive();
        openFirstTime();
    }
    // the project's resolver is made again when the project (or its paths) changes: a restored
    // window starts before the project has loaded, so its previews' base is looked up each frame
    const jasperengine::titleart::AssetResolver* base = TitleArtRenderHost::editor().resolver(project);
    if (base != transient.base()) {
        transient.setBase(base);
        // what was laid out or found missing through the old resolver is stale
        geometry.reset();
        textLayout.reset();
        pickCache.clear();
        boundsKey = DerivedKey();
        hashesKey = DerivedKey();
        if (canvas) canvas->clear();
    }
    loadWorkspace();
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool stillOpen = true;
    ImGui::SetNextWindowSize(ImVec2(1280, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &stillOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        focused = false;
        rectValid = false;
        ImGui::End();
        if (!stillOpen) setOpen(false);
        return;
    }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    rectMin = ImGui::GetWindowPos();
    rectMax = ImVec2(rectMin.x + ImGui::GetWindowWidth(), rectMin.y + ImGui::GetWindowHeight());
    rectValid = true;
    displayScale = ImGui::GetIO().DisplayFramebufferScale.x > 0.0f ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
    ImePreedit::setInlineWanted(textEditing() && focused);
    tick();

    libraryShown = false;
    // R76: Tab (Shift+Tab back) moves between the regions while no field takes the text; the key
    // is the window's, so ImGui's own tabbing between widgets does not run too
    if (focused && !ImGui::GetIO().WantTextInput && !textEditing()) {
        const ImGuiID owner = ImGui::GetCurrentWindow()->ID;
        ImGui::SetKeyOwner(ImGuiKey_Tab, owner);
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_None, owner) && !ImGui::GetIO().KeyCtrl) cycleRegion(ImGui::GetIO().KeyShift ? -1 : 1);
    }
    regionFocus(RegionToolbar);
    drawToolbar();
    drawReadOnlyBanner();
    drawCjkFontNotice();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = avail.x;
    const float statusHeight = ImGui::GetFrameHeight() + 2.0f;
    const bool hasLibrary = library() != nullptr && doc.isOpen() && !newerVersion();
    const float scale = ImGui::GetFontSize() / 16.0f;
    // R3: narrow windows
    const bool narrow = width < 960.0f * std::max(1.0f, scale);
    const bool tiny = width < 480.0f * std::max(1.0f, scale);
    // R3: narrow, the Library is its header bar until a click opens it
    const bool libraryFolded = settings.libraryCollapsed || (narrow && !libraryNarrowOpen);
    const float libraryHeight = !hasLibrary ? 0.0f : libraryFolded ? ImGui::GetFrameHeightWithSpacing() * 1.2f : settings.libraryHeight * scale;
    // an open Library adds its splitter and one more item gap above it
    const float librarySplitter = hasLibrary && !libraryFolded ? 2.0f * ImGui::GetStyle().ItemSpacing.y : 0.0f;
    // registered bottom panels (S85's Animation strip): an open one reserves its height, a closed
    // one its header
    const float panelHeader = ImGui::GetFrameHeightWithSpacing();
    float panelsHeight = 0.0f;
    for (const PanelDef& panel : panels()) {
        if (panel.region != PanelDef::Region::Bottom) continue;
        panelsHeight += panelOpen(panel) ? std::max(180.0f * scale, avail.y * 0.45f) : panelHeader;
    }
    if (panelsHeight > 0.0f) panelsHeight = std::min(panelsHeight, avail.y * 0.55f) + ImGui::GetStyle().ItemSpacing.y;
    const float mainHeight = std::max(120.0f, avail.y - libraryHeight - statusHeight - librarySplitter - panelsHeight);
    regionAvailable[RegionLayers] = !tiny && (narrow || !settings.layersCollapsed);
    regionAvailable[RegionProperties] = !tiny && (narrow || !settings.propertiesCollapsed);
    regionAvailable[RegionCanvas] = true;
    regionAvailable[RegionLibrary] = hasLibrary && !tiny;

    if (tiny) {
        if (narrowPanelsOpen) {
            drawSideColumn(width, mainHeight);
        } else {
            drawCanvas(width, mainHeight);
        }
    } else if (narrow) {
        const float side = std::min(settings.propertiesWidth * scale, width * 0.45f);
        drawCanvas(width - side - ImGui::GetStyle().ItemSpacing.x, mainHeight);
        ImGui::SameLine();
        drawSideColumn(side, mainHeight);
    } else {
        const float left = settings.layersCollapsed ? ImGui::GetFrameHeight() : settings.layersWidth * scale;
        const float right = settings.propertiesCollapsed ? ImGui::GetFrameHeight() : settings.propertiesWidth * scale;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        // Layers | History
        ImGui::BeginChild("##left", ImVec2(left, mainHeight), true);
        regionFocus(RegionLayers);
        if (settings.layersCollapsed) {
            if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##expandleft")) {
                settings.layersCollapsed = false;
                settings.save();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show Layers and History");
        } else {
            if (ImGui::BeginTabBar("##sidetabs")) {
                if (ImGui::BeginTabItem("Layers###tab_layers", nullptr, sideTab == "layers" && panelFocus == "layers" ? ImGuiTabItemFlags_SetSelected : 0)) {
                    if (sideTab != "layers" && !ImGui::IsItemActivated()) sideTab = "layers";
                    sideTab = "layers";
                    drawLayers();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("History###tab_history", nullptr, sideTab == "history" && panelFocus == "history" ? ImGuiTabItemFlags_SetSelected : 0)) {
                    sideTab = "history";
                    drawHistory();
                    ImGui::EndTabItem();
                }
                if (ImGui::TabItemButton(ICON_FA_CHEVRON_LEFT "##collapseleft", ImGuiTabItemFlags_Trailing)) {
                    settings.layersCollapsed = true;
                    settings.save();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
        // splitter
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##splitleft", ImVec2(spacing, mainHeight));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive() && !settings.layersCollapsed) {
            settings.layersWidth = std::clamp(settings.layersWidth + ImGui::GetIO().MouseDelta.x / scale, 140.0f, 900.0f);
        }
        if (ImGui::IsItemDeactivated()) settings.save();
        ImGui::SameLine(0.0f, 0.0f);
        drawCanvas(std::max(120.0f, width - left - right - 2.0f * spacing), mainHeight);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##splitright", ImVec2(spacing, mainHeight));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive() && !settings.propertiesCollapsed) {
            settings.propertiesWidth = std::clamp(settings.propertiesWidth - ImGui::GetIO().MouseDelta.x / scale, 200.0f, 1000.0f);
        }
        if (ImGui::IsItemDeactivated()) settings.save();
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##right", ImVec2(right, mainHeight), true);
        regionFocus(RegionProperties);
        if (settings.propertiesCollapsed) {
            if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##expandright")) {
                settings.propertiesCollapsed = false;
                settings.save();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show Properties");
        } else {
            drawProperties();
        }
        ImGui::EndChild();
    }

    if (hasLibrary) {
        if (!libraryFolded) {
            ImGui::InvisibleButton("##splitlib", ImVec2(width, ImGui::GetStyle().ItemSpacing.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) settings.libraryHeight = std::clamp(settings.libraryHeight - ImGui::GetIO().MouseDelta.y / scale, 80.0f, 900.0f);
            if (ImGui::IsItemDeactivated()) settings.save();
        }
        drawLibrary(width, libraryHeight);
    }
    // the region with the keyboard focus this frame (the window itself: the toolbar)
    focusedRegion = focusedRegionNow >= 0 ? focusedRegionNow : (focused ? (int)RegionToolbar : -1);
    focusedRegionNow = -1;
    if (keyboardRegion >= 0 && focusedRegion != keyboardRegion) keyboardRegion = -1;
    drawPanels(panelsHeight);
    drawStatusBar();
    drawAddPalette();
    drawFontPicker();
    drawExposeDialog();
    drawNotices();
    ImGui::End();
    if (!stillOpen) setOpen(false);
    firstShow = false;
}

bool TitleDesignerWindow::panelOpen(const PanelDef& panel) const {
    const auto found = panelOpenStates.find(panel.id);
    if (found != panelOpenStates.end()) return found->second;
    return panel.visibleByDefault ? panel.visibleByDefault(doc) : true;
}

void TitleDesignerWindow::drawPanels(float height) {
    JE_PROFILE_SCOPE("TD drawPanels");
    if (height <= 0.0f) return;
    ImGui::BeginChild("##titledesignerpanels", ImVec2(0.0f, height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    for (const PanelDef& panel : panels()) {
        if (panel.region != PanelDef::Region::Bottom || !panel.draw) continue;
        const bool wasOpen = panelOpen(panel);
        ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
        const bool open = ImGui::CollapsingHeader((panel.title + "###panel_" + panel.id).c_str());
        // only a click pins it: until then it follows visibleByDefault (a document's first animation opens it)
        if (open != wasOpen) panelOpenStates[panel.id] = open;
        if (open) panel.draw(doc);
    }
    ImGui::EndChild();
}

void TitleDesignerWindow::drawSideColumn(float width, float height) {
    JE_PROFILE_SCOPE("TD drawSideColumn");
    // R3: Layers, History and Properties as tabs of one column (Properties first with a selection)
    ImGui::BeginChild("##sidecolumn", ImVec2(width, height), true);
    // Layers and Properties share the column: the one Tab asked for (its tab is selected)
    regionFocus(pendingRegionFocus == RegionProperties || (pendingRegionFocus < 0 && keyboardRegion == RegionProperties) ? RegionProperties : RegionLayers);
    if (ImGui::BeginTabBar("##narrowtabs")) {
        const bool hasSelection = !doc.selection().empty();
        // a tab asked for (setPanel, Tab's keyboard regions) is selected once
        auto tab = [&](const char* label, const char* id, auto draw) {
            const bool asked = narrowTabRequest == id;
            if (ImGui::BeginTabItem(label, nullptr, asked ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw();
                ImGui::EndTabItem();
            }
        };
        if (hasSelection) tab("Properties###n_props", "properties", [&] { drawProperties(); });
        tab("Layers###n_layers", "layers", [&] { drawLayers(); });
        tab("History###n_history", "history", [&] { drawHistory(); });
        if (!hasSelection) tab("Properties###n_props", "properties", [&] { drawProperties(); });
        if (narrowPanelsOpen && ImGui::TabItemButton(ICON_FA_XMARK "##closepanels", ImGuiTabItemFlags_Trailing)) narrowPanelsOpen = false;
        ImGui::EndTabBar();
        narrowTabRequest.clear();
    }
    ImGui::EndChild();
}

void TitleDesignerWindow::drawReadOnlyBanner() {
    JE_PROFILE_SCOPE("TD drawReadOnlyBanner");
    if (!doc.isOpen() || doc.editable()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.45f, 0.30f, 0.05f, 0.6f));
    ImGui::BeginChild("##readonly", ImVec2(0, ImGui::GetFrameHeightWithSpacing()), false);
    ImGui::Text(ICON_FA_LOCK " Read-only: %s", doc.readOnlyReason().c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

namespace {

    // Han, kana, Hangul, CJK punctuation and full-width forms: what the editor's own fonts lack
    bool isCjkCodepoint(unsigned int c) {
        return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3000 && c <= 0x30FF) || (c >= 0x3130 && c <= 0x9FFF) ||
               (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3134F);
    }

    bool hasCjk(const std::string& text) {
        const char* p = text.c_str();
        const char* end = p + text.size();
        while (p < end) {
            unsigned int c = 0;
            const int n = ImTextCharFromUtf8(&c, p, end);
            if (n <= 0) break;
            if (isCjkCodepoint(c)) return true;
            p += n;
        }
        return false;
    }

    // dismissed: not said again this session
    bool cjkFontNoticeDismissed = false;

    const char* const CJK_FONT_DOCS = "https://github.com/JasperRaccoon/JasperEngine/blob/main/docs/editor/text-and-languages.md#fonts-in-the-editor";

}

// A title in Chinese, Japanese or Korean on a system the editor found no CJK font on: the
// editor's fields (Text, layer names, search) would show boxes. Said once, until dismissed.
void TitleDesignerWindow::drawCjkFontNotice() {
    JE_PROFILE_SCOPE("TD drawCjkFontNotice");
    cjkNoticeShown = false;
    if (cjkFontNoticeDismissed || !doc.isOpen()) return;
    if (cjkScanDocument != doc.document().get() || cjkScanChange != doc.changeCount()) {
        cjkScanDocument = doc.document().get();
        cjkScanChange = doc.changeCount();
        cjkInDocument = false;
        documentPasses++;
        ta::forEachLayer(doc.committed().root, [&](const ta::Json& l, const ta::Json*, int) {
            if (hasCjk(str(l, "name")) || (ta::layerTypeOf(l) == ta::LayerType::Text && hasCjk(str(l, "text")))) cjkInDocument = true;
            return !cjkInDocument;
        });
    }
    if (!cjkInDocument) return;
    int found = -1;
    if (systemCjkOverride) {
        found = systemCjkOverride();
    } else if (SystemFonts::state() == SystemFonts::State::None) {
        found = 0;
    } else if (SystemFonts::state() == SystemFonts::State::Ready) {
        found = 0;
        for (const SystemFonts::Font& font : SystemFonts::fonts()) {
            if (font.covers != "emoji") found = 1;
        }
    }
    if (found != 0) return;
    cjkNoticeShown = true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
    ImGui::TextUnformatted(ICON_FA_LANGUAGE);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(200.0f, ImGui::GetContentRegionAvail().x - 120.0f * ImGui::GetFontSize() / 16.0f));
    ImGui::TextDisabled("No system CJK font found: this title's Chinese, Japanese or Korean characters show as boxes in the editor's "
                        "fields (the title renders with its own font). Install one, e.g. Noto Sans CJK, and restart; "
                        "Preferences > CJK Font Language (cjk_font_language) picks its glyph forms.");
    ImGui::PopTextWrapPos();
    ImGui::SameLine();
    if (ImGui::SmallButton("Docs###cjkdocs")) {
        auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
        if (!openInShell || !openInShell(ImGui::GetCurrentContext(), CJK_FONT_DOCS)) reportError(std::string("Could not open your browser: ") + CJK_FONT_DOCS);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", CJK_FONT_DOCS);
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_XMARK "###cjkdismiss")) cjkFontNoticeDismissed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Do not say this again this session");
}

const char* TitleDesignerWindow::regionName(int region) {
    switch (region) {
        case RegionToolbar: return "toolbar";
        case RegionLayers: return "layers";
        case RegionCanvas: return "canvas";
        case RegionProperties: return "properties";
        case RegionLibrary: return "library";
        default: return "";
    }
}

void TitleDesignerWindow::cycleRegion(int direction) {
    const int from = keyboardRegion >= 0 ? keyboardRegion : (focusedRegion >= 0 ? focusedRegion : (int)RegionToolbar);
    int next = from;
    for (int i = 0; i < RegionCount; i++) {
        next = (next + direction + RegionCount) % RegionCount;
        if (regionAvailable[next]) break;
    }
    if (next == RegionLayers) setPanel(sideTab == "history" ? "history" : "layers");
    else if (next == RegionProperties) setPanel("properties");
    pendingRegionFocus = next;
    keyboardRegion = next;
}

void TitleDesignerWindow::regionFocus(int region) {
    if (pendingRegionFocus == region) {
        ImGui::SetWindowFocus();
        // the toolbar: its first control has the keyboard
        if (region == RegionToolbar) ImGui::SetKeyboardFocusHere();
        pendingRegionFocus = -1;
    }
    if (region != RegionToolbar && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) focusedRegionNow = region;
    // the region Tab reached is outlined while it keeps the focus
    if (keyboardRegion == region && region != RegionToolbar && focusedRegion == region) {
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 max(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
        ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f, 0, 2.0f);
    }
}

void TitleDesignerWindow::drawNotices() {
    JE_PROFILE_SCOPE("TD drawNotices");
    const double t = now();
    if (!toasts.empty() && t > toastUntil) toasts.clear();
    if (t > errorUntil) errorText.clear();
    if (t > noticeUntil) notice.clear();
    if (t > chipUntil) chipText.clear();
    if (t > renamePromptUntil) renamePromptOld.clear();
}

void TitleDesignerWindow::frameSelection() {
    const std::vector<std::string> layers = doc.selectedLayers();
    if (layers.empty()) {
        fit();
        return;
    }
    TitleRect box;
    bool any = false;
    for (const std::string& id : layers) {
        const TitleRect b = layerBounds(id);
        if (b.empty()) continue;
        if (!any) box = b;
        else {
            const double x0 = std::min(box.x, b.x), y0 = std::min(box.y, b.y);
            const double x1 = std::max(box.x + box.width, b.x + b.width), y1 = std::max(box.y + box.height, b.y + b.height);
            box = {x0, y0, x1 - x0, y1 - y0};
        }
        any = true;
    }
    if (!any) return;
    DocumentView& v = view();
    const float w = std::max(1.0f, canvasMax.x - canvasMin.x), h = std::max(1.0f, canvasMax.y - canvasMin.y);
    const double zoom = std::min(w / (box.width * 1.25), h / (box.height * 1.25));
    v.fit = false;
    v.zoom = (float)std::clamp(zoom, 1.0 / 16.0, 32.0);
    v.panX = box.x + box.width * 0.5;
    v.panY = box.y + box.height * 0.5;
    lastViewChange = now();
}

bool TitleDesignerWindow::contentEmpty() const {
    JE_PROFILE_SCOPE("TD contentEmpty");
    // R5: no visible, non-guide content layer (hidden ancestors count); once per change
    if (!doc.isOpen()) return true;
    const DerivedKey key = derivedKey();
    if (key == contentEmptyKey) return contentEmptyCache;
    contentEmptyKey = key;
    documentPasses++;
    bool any = false;
    std::function<void(const ta::Json&, bool)> walk = [&](const ta::Json& list, bool parentVisible) {
        if (!list.is_array()) return;
        for (const ta::Json& layer : list) {
            if (any) return;
            const bool visible = parentVisible && layer.value("visible", true);
            const ta::LayerType type = ta::layerTypeOf(layer);
            if (type == ta::LayerType::Group) walk(ta::layerChildren(layer), visible);
            else if (visible && !layer.value("guide", false) && type != ta::LayerType::Adjustment) any = true;
        }
    };
    walk(doc.shown().layers(), true);
    contentEmptyCache = !any;
    return contentEmptyCache;
}

std::vector<std::string> TitleDesignerWindow::fontTextOfSelection() const {
    std::vector<std::string> out;
    for (const std::string& id : doc.selectedLayers()) {
        const ta::Json* layer = layerById(doc.shown(), id);
        if (layer && ta::layerTypeOf(*layer) == ta::LayerType::Text) out.push_back(str(*layer, "text"));
    }
    return out;
}

bool TitleDesignerWindow::dropFiles(const std::vector<std::string>& files, const std::string& underLayer, std::string* error) {
    if (!doc.editable()) {
        if (error) *error = doc.readOnlyReason();
        return false;
    }
    ta::Json ops = ta::Json::array();
    std::string name;
    for (const std::string& file : files) {
        std::string ext = fs::path(file).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        fs::path absolute = FileUtils::pathFromUtf8(file);
        if (!absolute.is_absolute()) absolute = project->getProjectPath() / absolute;
        // R34: an SVG comes in through import_svg (a file outside the assets is copied beside the document)
        if (ext == ".svg") {
            ops.push_back({{"op", "import_svg"}, {"file", absolute.generic_string()}});
            name = "Add Layer Vector (SVG)";
            continue;
        }
        std::error_code ec;
        const fs::path relative = fs::relative(absolute, project->getAssetsPath(), ec);
        const std::string assetPath = ec ? std::string() : relative.generic_string();
        if (assetPath.empty() || assetPath.rfind("..", 0) == 0) {
            if (error) *error = "'" + file + "' is outside the project's assets: drop it into the Project window first";
            return false;
        }
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".tga" || ext == ".bmp") {
            ops.push_back({{"op", "add_layer"}, {"type", "image"}, {"name", fs::path(file).stem().string()}, {"values", {{"image", {{"path", assetPath}, {"guid", ""}}}}}});
            name = "Add Layer Image";
        } else if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
            std::string target = underLayer;
            if (target.empty()) {
                for (const std::string& id : doc.selectedLayers()) {
                    const ta::Json* layer = layerById(doc.committed(), id);
                    if (layer && ta::layerTypeOf(*layer) == ta::LayerType::Text) target = id;
                }
            }
            if (target.empty()) {
                if (error) *error = "Select a text layer to set its font";
                return false;
            }
            ops.push_back({{"op", "set"}, {"layer", "id:" + target}, {"values", {{"font", {{"path", assetPath}, {"guid", ""}}}}}});
            name = "Change Font to " + fs::path(file).stem().string();
        } else {
            if (error) *error = "'" + file + "' is not an image, an SVG or a font";
            return false;
        }
    }
    if (ops.empty()) return false;
    return doc.commit(ops, name, error);
}
