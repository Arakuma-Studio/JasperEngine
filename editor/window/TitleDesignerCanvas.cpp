// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Title Designer's canvas (docs/specs/S82-title-designer.md R28-R39, R64, R68, R71). Auto
// select within the selected layer first, snapping within 8 screen px and the full-frame
// generator pick at alpha >= 0.05 follow Pachislop's Viewport / drag.ts / pick.ts
// (src/ui/viewport/) by SafEight, used with the author's permission; part picks use S26's
// root-then-drill clicks instead of Pachislop's Alt-click (Alt is Unity's orbit).

#include "TitleDesignerWindow.h"

#include "Backend.h"
#include "Project.h"
#include "external/IconsFontAwesome6.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtHash.h"
#include "titleart/designer/TitleCanvasView.h"
#include "titleart/designer/TitleGizmo2D.h"
#include "titleart/designer/TitleGizmo3D.h"
#include "titleart/designer/TitleHandles.h"
#include "titleart/designer/TitleSnap.h"
#include "titleart/designer/TitleTextEdit.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "widget/ColorField.h"
#include "window/DeviceProfiles.h"
#include "window/GameResolutions.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    constexpr float RULER = 18.0f;
    constexpr double SETTLE_SECONDS = 0.15;      // R29: the visible window re-renders 150 ms after the view settles
    constexpr int MAX_RENDER_SIDE = 8192;

    ScreenOrientation orientationOf(const std::string& name) {
        if (name == "portrait") return ScreenOrientation::Portrait;
        if (name == "portrait_upside_down") return ScreenOrientation::PortraitUpsideDown;
        if (name == "landscape_right") return ScreenOrientation::LandscapeRight;
        return ScreenOrientation::LandscapeLeft;
    }

    bool rectContains(const TitleRect& r, TitleVec2 p) {
        return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
    }

    bool rectsTouch(const TitleRect& a, const TitleRect& b) {
        return a.x <= b.x + b.width && b.x <= a.x + a.width && a.y <= b.y + b.height && b.y <= a.y + a.height;
    }

}

// ---- geometry of layers ----

TitleRect TitleDesignerWindow::layerBounds(const std::string& layerId) const {
    if (!geometry || !doc.isOpen()) return {};
    const ta::Json* layer = layerById(doc.shown(), layerId);
    if (!layer) return {};
    return geometry->contentBounds(*layer, doc.shown());
}

TitleRect TitleDesignerWindow::stageRect() const {
    const ta::TitleArtDoc& shown = doc.shown();
    const DocumentView* v = viewOrNull();
    if (v && !v->device.profile.empty()) {
        int w = 0, h = 0;
        if (v->device.profile.rfind("res:", 0) == 0) {
            GameResolution r;
            std::string error;
            if (findGameResolution(v->device.profile.substr(4), {}, r, error)) {
                w = r.width;
                h = r.height;
            }
        } else if (const DeviceProfile* p = findDeviceProfile(v->device.profile)) {
            profileSize(*p, orientationOf(v->device.orientation), w, h);
        }
        if (w > 0 && h > 0) return {-w * 0.5, -h * 0.5, (double)w, (double)h};
    }
    return canvasRect(shown.canvasWidth(), shown.canvasHeight());
}

void TitleDesignerWindow::zoomAbout(float factor, ImVec2 screen) {
    DocumentView& v = view();
    const TitleVec2 before = xform.toDoc(screen);
    const float current = xform.zoom;
    const float next = std::clamp(current * factor, 1.0f / 16.0f, 32.0f);
    v.fit = false;
    v.zoom = next;
    // keep the point under the cursor in place
    const float zoomPx = next;
    const ImVec2 centre((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f);
    v.panX = before.x - (screen.x - centre.x) / zoomPx;
    v.panY = before.y + (screen.y - centre.y) / zoomPx;
    lastViewChange = now();
}

void TitleDesignerWindow::updateView(float width, float height) {
    DocumentView& v = view();
    const TitleRect stage = stageRect();
    // ImGui pixels per document unit (the canvas framebuffer is ImGui pixels x display scale)
    float zoom = v.zoom;
    if (v.fit) {
        const float margin = 0.92f;
        zoom = std::min(width / (float)stage.width, height / (float)stage.height) * margin;
        v.panX = stage.x + stage.width * 0.5;
        v.panY = stage.y + stage.height * 0.5;
    }
    zoom = std::clamp(zoom, 1.0f / 16.0f, 32.0f);
    const ImVec2 centre((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f);
    CanvasXform next;
    next.zoom = zoom;
    next.origin = ImVec2(centre.x - (float)v.panX * zoom, centre.y + (float)v.panY * zoom);
    if (next.zoom != xform.zoom || next.origin.x != xform.origin.x || next.origin.y != xform.origin.y) lastViewChange = now();
    xform = next;
}

// the title's placement on a device stage (Anchor, Display Height: R31)
static void devicePlacement(const DocumentView& v, const TitleRect& stage, const ta::TitleArtDoc& doc, double& scale, double& offsetX, double& offsetY) {
    scale = 1.0;
    offsetX = offsetY = 0.0;
    if (v.device.profile.empty()) return;
    const double titleHeight = v.device.displayHeightPx ? v.device.displayHeight : stage.height * v.device.displayHeight / 100.0;
    scale = titleHeight / std::max(1, doc.canvasHeight());
    const double w = doc.canvasWidth() * scale, h = doc.canvasHeight() * scale;
    const int col = v.device.anchor % 3, row = v.device.anchor / 3;
    const double cx = stage.x + (col == 0 ? w * 0.5 : col == 1 ? stage.width * 0.5 : stage.width - w * 0.5);
    const double cy = stage.y + stage.height - (row == 0 ? h * 0.5 : row == 1 ? stage.height * 0.5 : stage.height - h * 0.5);
    offsetX = cx;
    offsetY = cy;
}

void TitleDesignerWindow::requestCanvasRender() {
    if (!canvas || !doc.isOpen()) return;
    const DocumentView& v = view();
    const bool baked = v.previewAs == "baked";
    bool registered = false;
    for (const PreviewModeDef& mode : previewModes()) registered = registered || mode.id == v.previewAs;
    if (baked || registered) return;
    const ta::TitleArtDoc& shown = doc.shown();
    const double t = now();
    const bool settled = t - lastViewChange >= SETTLE_SECONDS;
    const bool gesture = doc.gestureActive() || drag != Drag::None || panning;
    // Draft while interacting (Auto), Full when settled (R68)
    const bool draft = settings.quality == Quality::Draft || (settings.quality == Quality::Auto && gesture);
    double stageScale = 1.0, ox = 0.0, oy = 0.0;
    devicePlacement(v, stageRect(), shown, stageScale, ox, oy);
    // display resolution of the visible window (at most 8,192 px a side)
    float pxScale = (float)(xform.zoom * displayScale * stageScale);
    ta::TitleRectI region;
    const float canvasW = (float)shown.canvasWidth(), canvasH = (float)shown.canvasHeight();
    // during pan and zoom the last image scales; no new render until the view settles
    if (!settled && canvas->info().hasImage && !doc.gestureActive() && !doc.previewActive()) return;
    {
        // the visible part of the canvas in canvas px (y down from its top-left)
        const TitleVec2 a = xform.toDoc(canvasMin), b = xform.toDoc(canvasMax);
        const double x0 = (std::min(a.x, b.x) - ox) / stageScale + canvasW * 0.5, x1 = (std::max(a.x, b.x) - ox) / stageScale + canvasW * 0.5;
        const double y0 = canvasH * 0.5 - (std::max(a.y, b.y) - oy) / stageScale, y1 = canvasH * 0.5 - (std::min(a.y, b.y) - oy) / stageScale;
        const int rx0 = std::max(0, (int)std::floor(x0)), ry0 = std::max(0, (int)std::floor(y0));
        const int rx1 = std::min((int)canvasW, (int)std::ceil(x1)), ry1 = std::min((int)canvasH, (int)std::ceil(y1));
        if (rx1 > rx0 && ry1 > ry0 && (rx1 - rx0 < (int)canvasW || ry1 - ry0 < (int)canvasH)) region = {rx0, ry0, rx1 - rx0, ry1 - ry0};
    }
    const int side = region.empty() ? std::max(shown.canvasWidth(), shown.canvasHeight()) : std::max(region.width, region.height);
    pxScale = std::min(pxScale, (float)MAX_RENDER_SIDE / std::max(1, side));
    if (draft) pxScale = std::max(pxScale * 0.5f, std::min(pxScale, 0.25f * displayScale));
    CanvasRenderRequest request;
    request.doc = doc.shownSnapshot();
    request.docChange = doc.changeCount() * 1000003ull ^ transient.revision();
    request.pxScale = std::max(0.02f, std::round(pxScale * 1000.0f) / 1000.0f);
    request.region = region;
    request.draft = draft;
    request.includeGuides = v.showGuides;
    request.sliceBudgetMs = draft ? 8.0 : 12.0;
    request.assets = &transient;
    if (const TimeSource& time = timeSource()) {
        // S85's playhead reaches the canvas as overrides it samples; the time feeds the key
        request.docChange ^= (uint64_t)std::llround(time() * 1000.0) << 20;
    }
    canvas->requestRender(request);
}

// ---- input ----

std::string TitleDesignerWindow::pickLayer(ImVec2 screen, bool drill) {
    const ta::TitleArtDoc& shown = doc.shown();
    const TitleVec2 p = titleXform.toDoc(screen);
    const DocumentView& v = view();
    std::string hit;
    // topmost visible, unlocked layer under the cursor: walk front to back
    std::function<bool(const ta::Json&, bool)> walk = [&](const ta::Json& list, bool parentVisible) -> bool {
        for (size_t i = list.size(); i-- > 0;) {
            const ta::Json& layer = list[i];
            const bool visible = parentVisible && layer.value("visible", true);
            if (!visible || layer.value("locked", false)) continue;
            if (layer.value("guide", false) && !v.showGuides) continue;
            const ta::LayerType type = ta::layerTypeOf(layer);
            if (type == ta::LayerType::Group) {
                if (walk(ta::layerChildren(layer), visible)) return true;
                continue;
            }
            const std::string id = str(layer, "id");
            if (type == ta::LayerType::Generator || type == ta::LayerType::Adjustment) {
                // a full-frame sparkle does not take every click: its alpha at 1/4 scale (R29)
                const ta::TitleArtHashes hashes = ta::hashTitleArt(shown);
                auto h = hashes.layers.find(id);
                const uint64_t key = h != hashes.layers.end() ? h->second.node : 0;
                auto cached = pickCache.find(id);
                if (cached == pickCache.end() || cached->second.first != key) {
                    ta::RenderRequest request;
                    request.doc = doc.shownSnapshot();
                    request.isolateLayer = id;
                    request.pxScale = 0.25f;
                    request.assets = &transient;
                    TitleRenderedImage image;
                    std::string error;
                    std::vector<uint8_t> alpha;
                    if (TitleArtRenderHost::editor().render(project, request, image, error)) {
                        alpha.resize((size_t)image.image.width * image.image.height);
                        for (int y = 0; y < image.image.height; y++) {
                            for (int x = 0; x < image.image.width; x++) alpha[(size_t)y * image.image.width + x] = image.image.pixel(x, y)[3];
                        }
                        pickFrames[id] = {image.frame.x, image.frame.y, image.image.width, image.image.height};
                    }
                    pickCache[id] = {key, alpha};
                    cached = pickCache.find(id);
                }
                const ta::TitleRectI frame = pickFrames[id];
                const std::vector<uint8_t>& alpha = cached->second.second;
                if (alpha.empty() || frame.width <= 0) continue;
                // within 4 screen px
                const double reach = 4.0 / std::max(0.01f, titleXform.zoom);
                bool inside = false;
                for (double dy = -reach; dy <= reach && !inside; dy += reach) {
                    for (double dx = -reach; dx <= reach && !inside; dx += reach) {
                        const double cx = (p.x + dx + shown.canvasWidth() * 0.5) * 0.25 - frame.x;
                        const double cy = (shown.canvasHeight() * 0.5 - (p.y + dy)) * 0.25 - frame.y;
                        const int ix = (int)cx, iy = (int)cy;
                        if (ix >= 0 && iy >= 0 && ix < frame.width && iy < frame.height && alpha[(size_t)iy * frame.width + ix] >= 13) inside = true;
                    }
                }
                if (inside) {
                    hit = id;
                    return true;
                }
                continue;
            }
            if (rectContains(layerBounds(id), p)) {
                hit = id;
                return true;
            }
        }
        return false;
    };
    // the selected layer wins while it is under the cursor (it can be dragged out from under others)
    for (const std::string& id : doc.selectedLayers()) {
        const ta::Json* layer = layerById(shown, id);
        if (layer && !layer->value("locked", false) && ta::layerTypeOf(*layer) != ta::LayerType::Group && rectContains(layerBounds(id), p)) {
            if (!drill) return id;
        }
    }
    walk(shown.layers(), true);
    if (hit.empty()) return {};
    // root-then-drill (S26): the first click takes the top-level group, the next its child
    std::vector<std::string> chain;
    for (std::string id = hit; !id.empty(); id = parentLayerOf(shown, id)) chain.insert(chain.begin(), id);
    const float spot = std::abs(screen.x - clickSpot.x) + std::abs(screen.y - clickSpot.y);
    if (drill && spot < 4.0f && clickLayer == hit) clickDepth++;
    else if (drill) clickDepth = 0;
    clickLayer = hit;
    clickSpot = screen;
    const size_t depth = std::min((size_t)clickDepth, chain.size() - 1);
    return chain[depth];
}

bool TitleDesignerWindow::textEditing() const {
    return textEdit && textEdit->active();
}

void TitleDesignerWindow::beginTextEdit(const std::string& layerId, bool selectAll) {
    if (!textEdit || !doc.editable()) return;
    const ta::Json* layer = layerById(doc.committed(), layerId);
    if (!layer || ta::layerTypeOf(*layer) != ta::LayerType::Text) return;
    if (textEdit->active()) endTextEdit(true);
    doc.selectOne({NodeKind::Layer, layerId, {}, {}});
    textEdit->begin(layerId, str(*layer, "text"), selectAll);
    doc.beginGesture("Edit Text");
}

void TitleDesignerWindow::endTextEdit(bool commit) {
    if (!textEdit || !textEdit->active()) return;
    const std::string layerId = textEdit->layerId();
    const std::string original = textEdit->originalText();
    const std::string text = textEdit->text();
    textEdit->end();
    doc.cancelGesture();
    if (!commit || text == original) return;
    // one step "Edit Text"; char adjustments past the new length are dropped by S80 (R39)
    const ta::Json* layer = layerById(doc.committed(), layerId);
    const size_t removed = layer ? lettersRemoved(layer->value("chars", ta::Json::object()), codepointCount(text)) : 0;
    std::string name = "Edit Text";
    if (removed > 0) name += " (" + std::to_string(removed) + " Letter Adjustment" + (removed == 1 ? "" : "s") + " Removed)";
    std::string error;
    if (!doc.commit(ta::Json::array({ta::Json{{"op", "set_text"}, {"layer", "id:" + layerId}, {"text", text}}}), name, &error)) {
        reportError(error);
        return;
    }
    // Pachislop's prompt: the document is named after the replaced text
    if (doc.committed().root.value("name", std::string()) == original && !text.empty()) {
        renamePromptOld = original;
        renamePromptNew = text;
        renamePromptUntil = now() + 12.0;
    }
}

void TitleDesignerWindow::stageLetters(const ta::Json& values) {
    if (!textEditing() || values.empty()) return;
    // the edit's text stays staged with the characters' new values
    ta::Json ops = ta::Json::array();
    const std::string text = textEdit->previewText();
    if (text != textEdit->originalText()) ops.push_back({{"op", "set_text"}, {"layer", "id:" + textEdit->layerId()}, {"text", text}});
    ops.push_back({{"op", "set"}, {"layer", "id:" + textEdit->layerId()}, {"values", values}});
    std::string error;
    if (!doc.stage(ops, &error)) reportError(error);
}

void TitleDesignerWindow::commitLetters(const ta::Json& values, const std::string& stepName) {
    if (!textEditing() || values.empty()) return;
    // one step, with the text typed so far (the indexes are of that text); the edit goes on
    // from it, so Esc no longer reverts what this step kept
    ta::Json ops = ta::Json::array();
    const std::string text = textEdit->text();
    if (text != textEdit->originalText()) ops.push_back({{"op", "set_text"}, {"layer", "id:" + textEdit->layerId()}, {"text", text}});
    ops.push_back({{"op", "set"}, {"layer", "id:" + textEdit->layerId()}, {"values", values}});
    std::string error;
    if (!doc.commit(ops, stepName, &error)) reportError(error);
    textEdit->rebase();
    doc.beginGesture("Edit Text");
}

void TitleDesignerWindow::handleCanvasInput() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool hovered = ImGui::IsItemHovered();
    cursorInCanvas = hovered;
    if (hovered) {
        const TitleVec2 p = titleXform.toDoc(mouse);
        cursorDoc = ImVec2((float)p.x, (float)p.y);
    }
    const GizmoModifiers mods{io.KeyShift, io.KeyCtrl || io.KeySuper, io.KeyAlt};
    DocumentView& v = view();

    // zoom about the cursor (wheel), pan with the middle or right button, or the Hand tool
    if (hovered && io.MouseWheel != 0.0f) zoomAbout(io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f, mouse);
    const bool panButton = ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) ||
                           (currentTool == Tool::Hand && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && drag == Drag::Pan);
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) panning = true;
    if (panning || panButton) {
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            v.fit = false;
            v.zoom = xform.zoom;
            v.panX -= io.MouseDelta.x / xform.zoom;
            v.panY += io.MouseDelta.y / xform.zoom;
            lastViewChange = now();
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) panning = false;

    const bool editable = doc.editable();
    SnapContext snap;
    snap.settings.enabled = v.snap;
    snap.settings.pixelGrid = v.pixelGrid;
    snap.settings.moveIncrement = settings.snapMove;
    snap.settings.rotateIncrement = settings.snapRotate;
    snap.settings.scaleIncrement = settings.snapScale;
    snap.zoom = titleXform.zoom;
    snap.canvas = canvasRect(doc.shown().canvasWidth(), doc.shown().canvasHeight());
    for (const Guide& g : v.guides) (g.vertical ? snap.guidesX : snap.guidesY).push_back(g.position);
    const std::vector<std::string> selected = doc.selectedLayers();
    ta::forEachLayer(doc.shown().root, [&](const ta::Json& l, const ta::Json*, int) {
        const std::string id = str(l, "id");
        if (std::find(selected.begin(), selected.end(), id) == selected.end() && l.value("visible", true) &&
            ta::layerTypeOf(l) != ta::LayerType::Group && ta::layerTypeOf(l) != ta::LayerType::Adjustment) {
            const TitleRect b = layerBounds(id);
            if (!b.empty() && b.width < snap.canvas.width * 1.5) snap.others.push_back(b);
        }
        return true;
    });

    // text editing: clicks inside the edited layer place the caret, outside commit (R36, R39); a
    // selected character's gizmo moves, turns or scales the selected characters (R38)
    if (textEditing()) {
        if (drag == Drag::Letters) {
            const bool moved = std::fabs(mouse.x - dragStart.x) + std::fabs(mouse.y - dragStart.y) >= 1.0f;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (moved) stageLetters(letterGizmo.values(mouse, io.KeyShift));
            } else {
                if (moved) commitLetters(letterGizmo.values(mouse, io.KeyShift), letterGizmo.stepName());
                letterGizmo.end();
                drag = Drag::None;
            }
            return;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const TitleLetterGizmo::Hit hit = editable && io.MouseClickedCount[0] == 1 ? letterGizmo.hitTest(mouse) : TitleLetterGizmo::Hit();
            if (hit.part != TitleLetterGizmo::Part::None) {
                letterGizmo.begin(hit, mouse);
                dragStart = mouse;
                drag = Drag::Letters;
            } else if (rectContains(layerBounds(textEdit->layerId()), titleXform.toDoc(mouse))) {
                textEdit->clickAt(mouse, io.KeyShift, io.MouseClickedCount[0] == 2);
                drag = Drag::TextSelect;
            } else {
                endTextEdit(true);
            }
        } else if (drag == Drag::TextSelect && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            textEdit->clickAt(mouse, true, false);
        }
        if (drag == Drag::TextSelect && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = Drag::None;
        return;
    }

    // gizmo and handles first
    if (drag == Drag::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && editable && currentTool != Tool::Hand) {
        const int handle = handles ? handles->hitTest(mouse) : -1;
        const Gizmo3DHandle h3 = gizmo3d && gizmo3d->visible() ? gizmo3d->hitTest(mouse) : Gizmo3DHandle::None;
        const Gizmo2DHandle h2 = gizmo2d ? gizmo2d->hitTest(mouse) : Gizmo2DHandle::None;
        if (handle >= 0 && handles->begin(handle, mouse, mods)) {
            drag = Drag::Handle;
            doc.beginGesture(handles->gestureName());
        } else if (h3 != Gizmo3DHandle::None && gizmo3d->begin(h3, mouse, mods)) {
            drag = Drag::Gizmo3D;
            doc.beginGesture(gizmo3d->actionName());
        } else if (h2 != Gizmo2DHandle::None && h2 != Gizmo2DHandle::Inside && gizmo2d->begin(h2, mouse, mods)) {
            drag = Drag::Gizmo;
            doc.beginGesture("Move Layer");
        }
    }
    if (drag == Drag::Gizmo || drag == Drag::Gizmo3D || drag == Drag::Handle) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Esc mid-drag restores every value and adds no step
            if (drag == Drag::Gizmo) gizmo2d->end();
            if (drag == Drag::Gizmo3D) gizmo3d->end();
            if (drag == Drag::Handle) handles->end();
            doc.cancelGesture();
            drag = Drag::None;
            return;
        }
        std::vector<FieldEdit> edits;
        if (drag == Drag::Gizmo) {
            edits = gizmo2d->update(mouse, mods, &snap);
            doc.setGestureName(gizmo2d->gestureName());
        } else if (drag == Drag::Gizmo3D) {
            edits = gizmo3d->update(mouse, mods);
        } else {
            edits = handles->update(mouse, mods, &snap);
        }
        if (!edits.empty()) {
            std::string error;
            if (!doc.stage(editsToOps(edits), &error)) reportError(error);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            std::string error;
            if (drag == Drag::Gizmo) {
                if (gizmo2d->pivotMoved()) {
                    // the pivot is the gizmo's (view state per layer, D16): no step
                    for (const std::string& id : selected) pivotOverride[id] = gizmo2d->pivot();
                    doc.cancelGesture();
                } else if (!doc.endGesture(&error)) {
                    reportError(error);
                }
                gizmo2d->end();
            } else {
                if (!doc.endGesture(&error)) reportError(error);
                if (drag == Drag::Gizmo3D) gizmo3d->end();
                else handles->end();
            }
            drag = Drag::None;
        }
        return;
    }

    // guides: drag from a ruler adds one; drag one moves it, back onto the ruler deletes it (R32)
    if (drag == Drag::NewGuide || drag == Drag::Guide) {
        const TitleVec2 p = titleXform.toDoc(mouse);
        if (guideIndex >= 0 && guideIndex < (int)v.guides.size()) v.guides[(size_t)guideIndex].position = guideVertical ? std::round(p.x) : std::round(p.y);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const bool onRuler = guideVertical ? mouse.y < canvasMin.y : mouse.x < canvasMin.x;
            if (onRuler && guideIndex >= 0 && guideIndex < (int)v.guides.size()) v.guides.erase(v.guides.begin() + guideIndex);
            guideIndex = -1;
            drag = Drag::None;
        }
        return;
    }
    if (drag == Drag::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && v.showGuides) {
        for (size_t i = 0; i < v.guides.size(); i++) {
            const Guide& g = v.guides[i];
            const ImVec2 s = titleXform.toScreen({g.vertical ? g.position : 0.0, g.vertical ? 0.0 : g.position});
            if ((g.vertical && std::abs(mouse.x - s.x) < 4.0f) || (!g.vertical && std::abs(mouse.y - s.y) < 4.0f)) {
                drag = Drag::Guide;
                guideIndex = (int)i;
                guideVertical = g.vertical;
                return;
            }
        }
    }

    // Hand tool: drag pans
    if (currentTool == Tool::Hand && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = Drag::Pan;
    if (drag == Drag::Pan) {
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            v.fit = false;
            v.zoom = xform.zoom;
            v.panX -= io.MouseDelta.x / xform.zoom;
            v.panY += io.MouseDelta.y / xform.zoom;
            lastViewChange = now();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag = Drag::None;
        return;
    }

    // click picks; a drag from empty canvas is a marquee; a drag on a layer moves it
    if (drag == Drag::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragStart = mouse;
        if (io.MouseClickedCount[0] == 2) {
            // double-click a text layer edits it on the canvas (R36)
            const std::string id = pickLayer(mouse, false);
            const ta::Json* layer = layerById(doc.committed(), id);
            if (layer && ta::layerTypeOf(*layer) == ta::LayerType::Text) {
                beginTextEdit(id, false);
                textEdit->clickAt(mouse, false, true);
                return;
            }
        }
        const std::string id = pickLayer(mouse, true);
        if (id.empty()) {
            if (!io.KeyShift && !(io.KeyCtrl || io.KeySuper)) doc.select({});
            drag = Drag::Marquee;
        } else {
            const NodeRef node{NodeKind::Layer, id, {}, {}};
            if (io.KeyShift) doc.select([&] {
                std::vector<NodeRef> s = doc.selection();
                s.push_back(node);
                return s;
            }());
            else if (io.KeyCtrl || io.KeySuper) doc.toggle(node);
            else if (!doc.isSelected(node)) doc.selectOne(node);
            // a press on the layer then a drag moves it (the gizmo's free move)
            if (editable && gizmo2d && currentTool != Tool::Hand) {
                std::vector<Gizmo2DTarget> targets;
                for (const std::string& layerId : doc.selectedLayers()) {
                    const ta::Json* layer = layerById(doc.shown(), layerId);
                    if (!layer || layer->value("locked", false)) continue;
                    Gizmo2DTarget t;
                    t.layerId = layerId;
                    t.type = ta::layerTypeOf(*layer);
                    t.transform = ta::decodeTransform(layer->value("transform", ta::Json::object()));
                    targets.push_back(t);
                }
                (void)targets;
                if (gizmo2d->begin(Gizmo2DHandle::Inside, mouse, mods)) {
                    drag = Drag::Gizmo;
                    doc.beginGesture("Move Layer");
                }
            }
        }
    }
    if (drag == Drag::Marquee) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(dragStart, mouse, IM_COL32(90, 160, 255, 40));
        dl->AddRect(dragStart, mouse, IM_COL32(90, 160, 255, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const TitleVec2 a = titleXform.toDoc(dragStart), b = titleXform.toDoc(mouse);
            const TitleRect box{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
            if (box.width > 1.0 / titleXform.zoom || box.height > 1.0 / titleXform.zoom) {
                std::vector<NodeRef> picked = (io.KeyShift || io.KeyCtrl) ? doc.selection() : std::vector<NodeRef>{};
                for (const ta::Json& layer : doc.shown().layers()) {
                    if (!layer.value("visible", true) || layer.value("locked", false)) continue;
                    if (rectsTouch(layerBounds(str(layer, "id")), box)) picked.push_back({NodeKind::Layer, str(layer, "id"), {}, {}});
                }
                doc.select(picked);
            }
            drag = Drag::None;
        }
    }
}

// ---- drawing ----

void TitleDesignerWindow::drawRulers(ImDrawList* dl) {
    const DocumentView& v = view();
    if (!v.rulers) return;
    const ImVec2 top0(canvasMin.x, canvasMin.y - RULER), left0(canvasMin.x - RULER, canvasMin.y);
    const ImU32 bg = IM_COL32(40, 40, 44, 255), tick = IM_COL32(150, 150, 155, 255);
    dl->AddRectFilled(top0, ImVec2(canvasMax.x, canvasMin.y), bg);
    dl->AddRectFilled(left0, ImVec2(canvasMin.x, canvasMax.y), bg);
    // a step of 1, 2 or 5 x 10^n canvas px that leaves at least 60 screen px between labels
    double step = 1.0;
    while (step * xform.zoom < 60.0) {
        if (step * 2.0 * xform.zoom >= 60.0) {
            step *= 2.0;
            break;
        }
        if (step * 5.0 * xform.zoom >= 60.0) {
            step *= 5.0;
            break;
        }
        step *= 10.0;
    }
    const TitleVec2 a = xform.toDoc(canvasMin), b = xform.toDoc(canvasMax);
    char label[32];
    for (double x = std::floor(a.x / step) * step; x <= b.x; x += step) {
        const float sx = xform.toScreen({x, 0}).x;
        dl->AddLine(ImVec2(sx, canvasMin.y - 6.0f), ImVec2(sx, canvasMin.y), tick);
        std::snprintf(label, sizeof(label), "%.0f", x);
        dl->AddText(ImVec2(sx + 2.0f, top0.y + 1.0f), tick, label);
    }
    for (double y = std::floor(b.y / step) * step; y <= a.y; y += step) {
        const float sy = xform.toScreen({0, y}).y;
        dl->AddLine(ImVec2(canvasMin.x - 6.0f, sy), ImVec2(canvasMin.x, sy), tick);
        std::snprintf(label, sizeof(label), "%.0f", y);
        dl->AddText(ImVec2(left0.x + 1.0f, sy + 1.0f), tick, label);
    }
    // the cursor on the rulers
    if (cursorInCanvas) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        dl->AddLine(ImVec2(m.x, top0.y), ImVec2(m.x, canvasMin.y), IM_COL32(90, 160, 255, 255));
        dl->AddLine(ImVec2(left0.x, m.y), ImVec2(canvasMin.x, m.y), IM_COL32(90, 160, 255, 255));
    }
}

void TitleDesignerWindow::drawEmptyState(ImDrawList* dl) {
    // R5: the card, with the template cards when the Library exists
    const ImVec2 centre((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f);
    TitleLibraryProvider* library = libraryProvider();
    const float scale = ImGui::GetFontSize() / 16.0f;
    const ImVec2 size(std::min(560.0f * scale, canvasMax.x - canvasMin.x - 40.0f), (library ? 300.0f : 96.0f) * scale);
    const ImVec2 min(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f);
    dl->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), IM_COL32(30, 30, 34, 235), 10.0f);
    dl->AddRect(min, ImVec2(min.x + size.x, min.y + size.y), IM_COL32(90, 90, 100, 255), 10.0f);
    ImGui::SetCursorScreenPos(ImVec2(min.x + 20.0f, min.y + 16.0f));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("Start your title");
    ImGui::PopFont();
    const bool editable = doc.editable();
    if (!editable) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
    if (ImGui::Button(ICON_FA_FONT " Add Text###emptyaddtext", ImVec2(140.0f * scale, 0.0f))) addLayer("text", ta::Json::object());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a Text layer and type (Enter)");
    // the buttons wrap inside the card on a narrow canvas
    const float cardRight = min.x + size.x - 16.0f;
    auto next = [cardRight](const char* label) {
        ImGui::SameLine();
        const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetCursorScreenPos().x + w > cardRight) ImGui::NewLine();
    };
    next("Shape " ICON_FA_CARET_DOWN);
    if (ImGui::Button("Shape " ICON_FA_CARET_DOWN "###emptyshape")) ImGui::OpenPopup("##emptyshapes");
    if (ImGui::BeginPopup("##emptyshapes")) {
        for (const char* shape : {"rect", "parallelogram", "ellipse", "orb", "polygon", "star", "burst", "shield", "speech", "shout", "ribbon"}) {
            if (ImGui::MenuItem(enumLabel(shape).c_str())) addLayer("shape", ta::Json{{"shape", shape}});
        }
        ImGui::EndPopup();
    }
    next("Image...");
    if (ImGui::Button("Image...###emptyimage")) openAddPalette("layers");
    next("Vector (SVG)...");
    if (ImGui::Button("Vector (SVG)...###emptysvg")) openAddPalette("layers");
    if (!editable) ImGui::EndDisabled();
    if (library) {
        ImGui::Spacing();
        ImGui::TextDisabled("Templates");
        std::vector<LibraryItem> featured;
        for (const LibraryItem& item : library->items(LibraryTab::Templates)) {
            if (item.featured && featured.size() < 6) featured.push_back(item);
        }
        const float card = (size.x - 40.0f - 5.0f * ImGui::GetStyle().ItemSpacing.x) / 6.0f;
        for (size_t i = 0; i < featured.size(); i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID((int)i);
            const LibraryTile tile = library->tile(LibraryTab::Templates, featured[i], std::string(), card, card * 0.5625f, true);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##template", ImVec2(card, card * 0.5625f)) && editable) applyLibraryItem(LibraryTab::Templates, featured[i], false);
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + card, p.y + card * 0.5625f), IM_COL32(50, 50, 56, 255), 4.0f);
            if (tile.texture) ImGui::GetWindowDrawList()->AddImage(tile.texture, p, ImVec2(p.x + card, p.y + card * 0.5625f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", featured[i].name.c_str());
            ImGui::PopID();
        }
        if (ImGui::Button("All Templates...###alltemplates")) setLibrary("templates", {}, {});
        ImGui::TextDisabled("Or pick a Look below: it adds a text in that look.");
    }
    ImGui::EndGroup();
}

void TitleDesignerWindow::drawCanvasOverlays(ImDrawList* dl) {
    const DocumentView& v = view();
    const ta::TitleArtDoc& shown = doc.shown();
    const TitleRect stage = stageRect();
    const ImVec2 stageMin = xform.toScreen({stage.x, stage.y + stage.height}), stageMax = xform.toScreen({stage.x + stage.width, stage.y});

    // device frame and safe areas (R31)
    if (!v.device.profile.empty() && v.safeAreas) {
        if (const DeviceProfile* profile = findDeviceProfile(v.device.profile)) {
            const ScreenOrientation orientation = orientationOf(v.device.orientation);
            drawDeviceOverlay(dl, stageMin, stageMax, *profile, orientation, true);
            // content outside the safe rectangle hatched red
            const Rect safe = profileSafeRect(*profile, orientation);
            int w = 0, h = 0;
            profileSize(*profile, orientation, w, h);
            const float sx = (stageMax.x - stageMin.x) / std::max(1, w), sy = (stageMax.y - stageMin.y) / std::max(1, h);
            const ImVec2 safeMin(stageMin.x + safe.getX() * sx, stageMin.y + safe.getY() * sy);
            const ImVec2 safeMax(safeMin.x + safe.getWidth() * sx, safeMin.y + safe.getHeight() * sy);
            double scale = 1.0, ox = 0.0, oy = 0.0;
            devicePlacement(v, stage, shown, scale, ox, oy);
            for (const ta::Json& layer : shown.layers()) {
                if (!layer.value("visible", true)) continue;
                const TitleRect b = layerBounds(str(layer, "id"));
                if (b.empty()) continue;
                const ImVec2 lmin = xform.toScreen({ox + b.x * scale, oy + (b.y + b.height) * scale});
                const ImVec2 lmax = xform.toScreen({ox + (b.x + b.width) * scale, oy + b.y * scale});
                const ImVec2 cmin(std::max(lmin.x, stageMin.x), std::max(lmin.y, stageMin.y)), cmax(std::min(lmax.x, stageMax.x), std::min(lmax.y, stageMax.y));
                if (cmin.x >= cmax.x || cmin.y >= cmax.y) continue;
                // hatch the parts outside the safe rectangle
                dl->PushClipRect(cmin, cmax, true);
                for (float d = -(cmax.y - cmin.y); d < cmax.x - cmin.x; d += 8.0f) {
                    const ImVec2 a(cmin.x + d, cmax.y), b2(cmin.x + d + (cmax.y - cmin.y), cmin.y);
                    dl->PushClipRect(cmin, cmax, true);
                    if (!(lmin.x >= safeMin.x && lmax.x <= safeMax.x && lmin.y >= safeMin.y && lmax.y <= safeMax.y)) {
                        dl->AddLine(a, b2, IM_COL32(230, 60, 60, 140), 1.0f);
                    }
                    dl->PopClipRect();
                }
                dl->PopClipRect();
            }
        } else if (v.device.profile.rfind("res:", 0) == 0) {
            // TV resolutions: title-safe 90 % and action-safe 93 % (SMPTE ST 2046-1)
            const float w = stageMax.x - stageMin.x, h = stageMax.y - stageMin.y;
            if (stage.width >= 1280 && std::abs(stage.width / stage.height - 16.0 / 9.0) < 0.02) {
                for (float share : {0.93f, 0.90f}) {
                    const float mx = w * (1.0f - share) * 0.5f, my = h * (1.0f - share) * 0.5f;
                    dl->AddRect(ImVec2(stageMin.x + mx, stageMin.y + my), ImVec2(stageMax.x - mx, stageMax.y - my),
                                share > 0.91f ? IM_COL32(90, 200, 120, 200) : IM_COL32(240, 200, 80, 200), 0.0f, 0, 1.0f);
                }
            }
        }
    }
    // pixel grid at 8x and above
    if (v.pixelGrid && titleXform.zoom >= 8.0f) {
        const TitleVec2 a = titleXform.toDoc(canvasMin), b = titleXform.toDoc(canvasMax);
        for (double x = std::floor(a.x); x <= b.x; x += 1.0) dl->AddLine(titleXform.toScreen({x, a.y}), titleXform.toScreen({x, b.y}), IM_COL32(255, 255, 255, 25));
        for (double y = std::floor(b.y); y <= a.y; y += 1.0) dl->AddLine(titleXform.toScreen({a.x, y}), titleXform.toScreen({b.x, y}), IM_COL32(255, 255, 255, 25));
    }
    // guides
    if (v.showGuides) {
        for (const Guide& g : v.guides) {
            if (g.vertical) {
                const float x = titleXform.toScreen({g.position, 0}).x;
                dl->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), IM_COL32(0, 220, 255, 200));
            } else {
                const float y = titleXform.toScreen({0, g.position}).y;
                dl->AddLine(ImVec2(canvasMin.x, y), ImVec2(canvasMax.x, y), IM_COL32(0, 220, 255, 200));
            }
        }
    }
    // effect reach (Guides > Effect Reach)
    if (v.effectReach && geometry) {
        for (const std::string& id : doc.selectedLayers()) {
            const ta::Json* layer = layerById(shown, id);
            if (!layer) continue;
            const TitleRect r = geometry->reachBounds(*layer, shown, 1.0);
            dl->AddRect(titleXform.toScreen({r.x, r.y + r.height}), titleXform.toScreen({r.x + r.width, r.y}), IM_COL32(255, 255, 255, 70), 0.0f, 0, 1.0f);
        }
    }
    // empty texts: a dashed box with "Empty text" (R24)
    ta::forEachLayer(shown.root, [&](const ta::Json& layer, const ta::Json*, int) {
        if (ta::layerTypeOf(layer) != ta::LayerType::Text || !str(layer, "text").empty() || !layer.value("visible", true)) return true;
        const ta::TitleTransform t = ta::decodeTransform(layer.value("transform", ta::Json::object()));
        const double size = layer.value("size", 200.0);
        const ImVec2 a = titleXform.toScreen({t.position.x - size, t.position.y + size * 0.5}), b = titleXform.toScreen({t.position.x + size, t.position.y - size * 0.5});
        for (float x = a.x; x < b.x; x += 10.0f) {
            dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + 5.0f, b.x), a.y), IM_COL32(220, 220, 220, 200));
            dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + 5.0f, b.x), b.y), IM_COL32(220, 220, 220, 200));
        }
        for (float y = a.y; y < b.y; y += 10.0f) {
            dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + 5.0f, b.y)), IM_COL32(220, 220, 220, 200));
            dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + 5.0f, b.y)), IM_COL32(220, 220, 220, 200));
        }
        const ImVec2 textSize = ImGui::CalcTextSize("Empty text");
        dl->AddText(ImVec2((a.x + b.x - textSize.x) * 0.5f, (a.y + b.y - textSize.y) * 0.5f), IM_COL32(220, 220, 220, 220), "Empty text");
        return true;
    });
}

void TitleDesignerWindow::drawCanvas(float width, float height) {
    ImGui::BeginChild("##canvas", ImVec2(width, height), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    regionFocus(RegionCanvas);
    if (newerVersion()) {
        ImGui::TextDisabled("%s", doc.readOnlyReason().c_str());
        ImGui::EndChild();
        return;
    }
    if (!doc.isOpen()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 20.0f, p.y + height * 0.4f));
        ImGui::TextDisabled("Open a Title Art: double-click a .titleart in the Project window, or Assets > Create > Title Art");
        ImGui::EndChild();
        return;
    }
    if (!geometry) geometry = std::make_unique<ta::geometry::TitleGeometry>(&transient);
    const DocumentView& v = view();
    const float ruler = v.rulers ? RULER : 0.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    canvasMin = ImVec2(origin.x + ruler, origin.y + ruler);
    canvasMax = ImVec2(origin.x + avail.x, origin.y + avail.y);
    updateView(canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y);
    {
        double s = 1.0, ox = 0.0, oy = 0.0;
        devicePlacement(view(), stageRect(), doc.shown(), s, ox, oy);
        titleXform.zoom = (float)(xform.zoom * s);
        titleXform.origin = ImVec2(xform.origin.x + (float)ox * xform.zoom, xform.origin.y - (float)oy * xform.zoom);
    }
    requestCanvasRender();

    // the composited view (backdrop + title) from the private scene
    const ta::TitleArtDoc& shown = doc.shown();
    const TitleRect stage = stageRect();
    CanvasViewSetup setup;
    setup.width = std::max(16, (int)std::round((canvasMax.x - canvasMin.x) * displayScale));
    setup.height = std::max(16, (int)std::round((canvasMax.y - canvasMin.y) * displayScale));
    setup.zoom = xform.zoom * displayScale;
    const TitleVec2 centre = xform.toDoc(ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
    setup.centerX = centre.x;
    setup.centerY = centre.y;
    setup.stage = stage;
    devicePlacement(v, stage, shown, setup.stageScale, setup.stageOffsetX, setup.stageOffsetY);
    setup.checkerPixels = 8.0f * displayScale;
    const bool baked = v.previewAs == "baked";
    bool modeRegistered = false;
    for (const PreviewModeDef& mode : previewModes()) modeRegistered = modeRegistered || mode.id == v.previewAs;
    setup.dim = (baked || modeRegistered) ? 0.0f : textEditing() ? 1.0f : 1.0f;
    CanvasBackdrop backdrop;
    const std::string& kind = v.backdrop.kind;
    if (kind == "black") backdrop.kind = CanvasBackdrop::Kind::Black;
    else if (kind == "white") backdrop.kind = CanvasBackdrop::Kind::White;
    else if (kind == "gray") backdrop.kind = CanvasBackdrop::Kind::Gray;
    else if (kind == "color") {
        backdrop.kind = CanvasBackdrop::Kind::Color;
        ta::TitleColor c;
        ta::parseTitleColor(ta::Json(v.backdrop.color), c);
        backdrop.color = Vector4(c.r, c.g, c.b, 1.0f);
    } else if ((kind == "in_context" && inContext && !inContext->snapshotRgba.empty())) {
        backdrop.kind = CanvasBackdrop::Kind::Image;
        backdrop.imageKey = "incontext:" + inContext->document.generic_string();
        backdrop.imageRgba = inContext->snapshotRgba;
        backdrop.imageWidth = inContext->screenWidth;
        backdrop.imageHeight = inContext->screenHeight;
    } else if ((kind == "image" || kind == "game_scene") && !snapshotRgba.empty() && snapshotKey == (kind == "image" ? v.backdrop.image : v.backdrop.scene)) {
        backdrop.kind = CanvasBackdrop::Kind::Image;
        backdrop.imageKey = snapshotKey;
        backdrop.imageRgba = snapshotRgba;
        backdrop.imageWidth = snapshotWidth;
        backdrop.imageHeight = snapshotHeight;
    }
    if (kind == "game_scene" && snapshotKey != v.backdrop.scene && !v.backdrop.scene.empty()) {
        // the snapshot cached on disk; it never refreshes by itself
        const fs::path cached = project->getProjectInternalPath() / "titleart" / "snapshots" /
                                (std::to_string(std::hash<std::string>{}(v.backdrop.scene)) + ".png");
        texsynth::Image image;
        std::string error;
        snapshotRgba.clear();
        if (texsynth::loadImage(cached, image, error) && image.channels == 4 && image.bitDepth == 8) {
            snapshotRgba = image.data;
            snapshotWidth = image.width;
            snapshotHeight = image.height;
        }
        snapshotKey = v.backdrop.scene;
    }
    if (kind == "image" && snapshotKey != v.backdrop.image && !v.backdrop.image.empty()) {
        // a Texture asset, decoded once
        texsynth::Image image;
        std::string error;
        if (texsynth::loadImage(project->getAssetsPath() / FileUtils::pathFromUtf8(v.backdrop.image), image, error) && image.channels == 4 && image.bitDepth == 8) {
            snapshotRgba = image.data;
            snapshotWidth = image.width;
            snapshotHeight = image.height;
        }
        snapshotKey = v.backdrop.image;
    }
    canvas->requestDraw(setup, backdrop);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(canvasMin);
    ImGui::InvisibleButton("##canvasarea", ImVec2(canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    if (TextureRender* texture = canvas->texture()) {
        dl->AddImage(Backend::getImGuiTexture(texture), canvasMin, canvasMax);
        // the eyedropper samples the canvas exactly
        ui::registerViewImage(ImGui::GetWindowViewport()->ID, canvasMin, canvasMax, canvas->framebuffer());
    } else {
        dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(34, 34, 36, 255));
    }
    // Drops from the Project window and the OS onto the canvas (R14)
    if (ImGui::BeginDragDropTarget()) {
        for (const char* type : {"resource_files", "external_files"}) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
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
                if (!dropFiles(files, std::string(), &error)) reportError(error);
            }
        }
        ImGui::EndDragDropTarget();
    }
    handleCanvasInput();
    canvasFocused = ImGui::IsWindowFocused();
    dl->PushClipRect(canvasMin, canvasMax, true);

    // Preview As Baked: the main output's PNG over the backdrop (stale badge when the recipe differs)
    if (baked) {
        const fs::path png = titleArtOutputPath(doc.document()->absolutePath(), shown.root.value("outputs", ta::Json::array()).empty() ? ta::Json{{"id", "main"}}
                                                                                                                             : shown.root["outputs"][0]);
        std::error_code ec;
        const std::string key = png.generic_string() + ":" + std::to_string(fs::exists(png, ec) ? fs::last_write_time(png, ec).time_since_epoch().count() : 0);
        if (key != bakedKey) {
            bakedKey = key;
            bakedTexture.destroy();
            texsynth::Image image;
            std::string error;
            if (texsynth::loadImage(png, image, error) && image.channels == 4 && image.bitDepth == 8) {
                bakedPixels = image.data;
                bakedTexture.setData("editor:titledesigner-baked:" + key,
                                     TextureData(image.width, image.height, (unsigned int)bakedPixels.size(), ColorFormat::RGBA, 4, bakedPixels.data()));
                bakedTexture.load();
                TitleBakeInfo info;
                std::string text;
                if (ProjectChecks::readFile(titleArtSidecarPath(doc.document()->absolutePath()), text) && TitleBakeInfo::parse(text, info, error)) bakedFrame = info.frame;
                else bakedFrame = {0, 0, image.width, image.height};
            }
            const TitleBakeStatus status = titleArtStatusOf(project, doc.document()->absolutePath(), doc.committed());
            bakedStale = !status.outputs.empty() && status.outputs[0].status != TitleOutputStatus::Fresh;
        }
        if (bakedTexture.getRender()) {
            const double left = bakedFrame.x - shown.canvasWidth() * 0.5, top = shown.canvasHeight() * 0.5 - bakedFrame.y;
            dl->AddImage(Backend::getImGuiTexture(bakedTexture.getRender()), titleXform.toScreen({left, top}),
                         titleXform.toScreen({left + bakedFrame.width, top - bakedFrame.height}));
        } else {
            dl->AddText(ImVec2(canvasMin.x + 12.0f, canvasMin.y + 12.0f), IM_COL32(240, 200, 80, 255), "Not baked: press Bake");
        }
        if (bakedStale) dl->AddText(ImVec2(canvasMin.x + 12.0f, canvasMax.y - 24.0f), IM_COL32(240, 170, 50, 255), ICON_FA_TRIANGLE_EXCLAMATION " Stale: the bake differs from the document");
    }
    for (const PreviewModeDef& mode : previewModes()) {
        if (mode.id != v.previewAs || !mode.draw) continue;
        if (!mode.draw(doc, dl, canvasMin, canvasMax, titleXform)) {
            dl->AddText(ImVec2(canvasMin.x + 12.0f, canvasMin.y + 12.0f), IM_COL32(200, 200, 200, 255), "Preparing preview...");
        }
        if (!mode.note.empty()) dl->AddText(ImVec2(canvasMin.x + 12.0f, canvasMax.y - 24.0f), IM_COL32(200, 200, 200, 220), mode.note.c_str());
    }

    drawCanvasOverlays(dl);

    // selection boxes, gizmo and handles (R33-R35)
    std::vector<Gizmo2DTarget> targets;
    for (const std::string& id : doc.selectedLayers()) {
        const ta::Json* layer = layerById(shown, id);
        if (!layer || layer->value("locked", false)) continue;     // locked layers have no gizmo
        if (layer->value("dimension", std::string("2d")) == "3d") continue;
        Gizmo2DTarget t;
        t.layerId = id;
        t.type = ta::layerTypeOf(*layer);
        t.transform = ta::decodeTransform(layer->value("transform", ta::Json::object()));
        Affine2 parentToDoc;
        std::vector<std::string> chain;
        for (std::string p = parentLayerOf(shown, id); !p.empty(); p = parentLayerOf(shown, p)) chain.push_back(p);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            parentToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layerById(shown, *it)->value("transform", ta::Json::object())));
        }
        t.parentToDoc = parentToDoc;
        // the content box in the layer's own space (before effects)
        const Affine2 layerToDoc = parentToDoc * layerTransformAffine(t.transform);
        if (ta::layerTypeHasStyle(t.type) && geometry) {
            const ta::geometry::LayerElements els = geometry->elements(*layer, shown);
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            // the content box: faces and rims (S80's content bounds of a 2D title include its rims)
            for (const ta::geometry::TitleElement& e : els.elements) {
                const TitleRect b = ta::geometry::elementBounds(e);
                x0 = std::min(x0, b.x);
                y0 = std::min(y0, b.y);
                x1 = std::max(x1, b.x + b.width);
                y1 = std::max(y1, b.y + b.height);
            }
            if (x1 > x0) t.localBox = {x0, y0, x1 - x0, y1 - y0};
        }
        if (t.localBox.empty()) {
            // groups, generators, adjustments: their document box in local space
            const TitleRect b = layerBounds(id);
            const Affine2 inv = layerToDoc.inverse();
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (TitleVec2 c : {TitleVec2{b.x, b.y}, TitleVec2{b.x + b.width, b.y}, TitleVec2{b.x, b.y + b.height}, TitleVec2{b.x + b.width, b.y + b.height}}) {
                const TitleVec2 l = inv.apply(c);
                x0 = std::min(x0, l.x);
                y0 = std::min(y0, l.y);
                x1 = std::max(x1, l.x);
                y1 = std::max(y1, l.y);
            }
            if (x1 > x0) t.localBox = {x0, y0, x1 - x0, y1 - y0};
        }
        t.hScale = layer->value("h_scale", 1.0);
        t.vScale = layer->value("v_scale", 1.0);
        t.size = layer->value("size", 200.0);
        t.aspect = layer->value("aspect", 1.0);
        auto pivot = pivotOverride.find(id);
        if (pivot != pivotOverride.end()) {
            t.hasPivot = true;
            t.pivot = pivot->second;
        }
        if (v.effectReach && geometry) {
            const TitleRect reach = geometry->reachBounds(*layer, shown, 1.0), content = layerBounds(id);
            t.effectReach = std::max(0.0, std::max(content.x - reach.x, reach.x + reach.width - content.x - content.width));
        }
        targets.push_back(t);
    }
    const bool showTools = !textEditing() && doc.editable() && currentTool != Tool::Hand && v.previewAs == "realtime";
    if (gizmo2d) {
        Gizmo2DSettings gs = gizmo2d->getSettings();
        gs.cjkLabels = ImGui::GetFont()->IsGlyphInFont((ImWchar)0x9577);       // 長
        gizmo2d->setSettings(gs);
        if (drag != Drag::Gizmo) gizmo2d->setTargets(targets, currentTool, pivotMode ? PivotMode::Pivot : PivotMode::Center, titleXform);
        if (showTools && !targets.empty()) {
            const Gizmo2DHandle hovered = drag == Drag::None && cursorInCanvas ? gizmo2d->hitTest(ImGui::GetIO().MousePos) : Gizmo2DHandle::None;
            if (hovered != Gizmo2DHandle::None) ImGui::SetMouseCursor(imguiCursor(gizmo2d->cursor(hovered)));
            gizmo2d->draw(dl, hovered, gizmo2d->activeHandle());
            if (gizmo2d->active()) {
                drawSnap(dl, titleXform, gizmo2d->lastSnap());
                const std::string readout = gizmo2d->readout();
                ImVec2 corners[4];
                if (!readout.empty() && gizmo2d->screenCorners(corners)) {
                    const float y = std::max(std::max(corners[0].y, corners[1].y), std::max(corners[2].y, corners[3].y)) + 12.0f;
                    const float x = (corners[0].x + corners[2].x) * 0.5f;
                    overlay::label(dl, ImVec2(x, y), readout, overlay::WHITE, true);
                }
            }
        }
    }
    // the fixed-schematic 3D gizmo for S86's Dimension 3D layers (R35)
    if (gizmo3d && showTools) {
        const std::vector<std::string> layers = doc.selectedLayers();
        const Gizmo3DProvider& provider = gizmo3DProvider();
        bool shownGizmo = false;
        if (provider && layers.size() == 1 && drag != Drag::Gizmo3D) {
            Gizmo3DTarget target;
            if (provider(doc, layers[0], target)) shownGizmo = gizmo3d->layout(target, Gizmo3DCamera::frontOn(titleXform, displayScale));
        } else if (drag == Drag::Gizmo3D) {
            shownGizmo = true;
        }
        if (shownGizmo && gizmo3d->visible()) {
            const Gizmo3DHandle hovered = drag == Drag::None && cursorInCanvas ? gizmo3d->hitTest(ImGui::GetIO().MousePos) : Gizmo3DHandle::None;
            gizmo3d->draw(dl, hovered, drag == Drag::Gizmo3D ? hovered : Gizmo3DHandle::None);
        }
    }
    if (handles) {
        handles->setView(titleXform);
        if (drag != Drag::Handle) {
            handles->clear();
            const NodeRef primary = doc.primary();
            const ta::Json* layer = layerById(shown, primary.layer);
            if (layer && primary.kind != NodeKind::Document && showTools) {
                HandleFrame frame;
                frame.layerId = primary.layer;
                Affine2 parentToDoc;
                std::vector<std::string> chain;
                for (std::string p = parentLayerOf(shown, primary.layer); !p.empty(); p = parentLayerOf(shown, p)) chain.push_back(p);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    parentToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layerById(shown, *it)->value("transform", ta::Json::object())));
                }
                frame.layerToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layer->value("transform", ta::Json::object())));
                for (const Gizmo2DTarget& t : targets) {
                    if (t.layerId == primary.layer) frame.localBox = t.localBox;
                }
                frame.sourceSize = ta::layerSourceSize(*layer);
                // handles show only while their node is selected (effects, parts, masks; the layer's own)
                handles->addNode(shown.root, primary, frame);
            }
        }
        if (handles->count() > 0 && showTools) {
            const int hovered = drag == Drag::None && cursorInCanvas ? handles->hitTest(ImGui::GetIO().MousePos) : -1;
            handles->draw(dl, hovered, drag == Drag::Handle ? hovered : -1);
        }
    }

    // text editing overlay: caret, selection, preedit (R36, R37)
    if (textEditing()) {
        const ta::Json* layer = layerById(doc.committed(), textEdit->layerId());
        if (!layer) {
            textEdit->end();
            doc.cancelGesture();
            letterGizmo.setGlyphs({}, Affine2(), titleXform, 1.0);
        } else {
            if (!textLayout) textLayout = std::make_unique<ta::TitleTextLayout>(&transient);
            // the working layer: a Letters drag in progress moves its cells too
            const ta::Json* working = layerById(doc.shown(), textEdit->layerId());
            ta::Json staged = working ? *working : *layer;
            staged["text"] = textEdit->previewText();
            const ta::LaidOutText& laid = textLayout->layout(staged);
            Affine2 parentToDoc;
            std::vector<std::string> chain;
            for (std::string p = parentLayerOf(shown, textEdit->layerId()); !p.empty(); p = parentLayerOf(shown, p)) chain.push_back(p);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                parentToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layerById(shown, *it)->value("transform", ta::Json::object())));
            }
            TextEditFrame frame;
            frame.layout = &laid;
            frame.layerToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(layer->value("transform", ta::Json::object())));
            frame.xform = titleXform;
            frame.fieldPos = ImVec2(canvasMin.x + 2.0f, canvasMin.y + 2.0f);
            // the art outside the edited layer dims to 60 %
            const TitleRect editBox = layerBounds(textEdit->layerId());
            const ImVec2 emin = titleXform.toScreen({editBox.x, editBox.y + editBox.height}), emax = titleXform.toScreen({editBox.x + editBox.width, editBox.y});
            const ImU32 dim = IM_COL32(20, 20, 22, 102);
            dl->AddRectFilled(canvasMin, ImVec2(canvasMax.x, emin.y), dim);
            dl->AddRectFilled(ImVec2(canvasMin.x, emax.y), canvasMax, dim);
            dl->AddRectFilled(ImVec2(canvasMin.x, emin.y), ImVec2(emin.x, emax.y), dim);
            dl->AddRectFilled(ImVec2(emax.x, emin.y), ImVec2(canvasMax.x, emax.y), dim);
            const TitleTextEdit::Result result = textEdit->frame(frame, dl);
            // the selected characters' gizmos (R38)
            {
                int first = 0, end = 0;
                textEdit->selection(first, end);
                std::vector<LetterGlyph> glyphs;
                const ta::Json chars = staged.value("chars", ta::Json::object());
                for (const ta::LaidOutCluster& cluster : laid.clusters) {
                    if ((int)cluster.index < first || (int)cluster.index >= end || cluster.emCell.width <= 0.0 || cluster.emCell.height <= 0.0) continue;
                    LetterGlyph g;
                    g.index = (int)cluster.index;
                    g.centre = {cluster.emCell.x + cluster.emCell.width * 0.5, cluster.emCell.y + cluster.emCell.height * 0.5};
                    g.width = cluster.emCell.width;
                    g.height = cluster.emCell.height;
                    const ta::Json adjust = chars.value(std::to_string(cluster.index), ta::Json::object());
                    g.rotation = adjust.value("rotation", 0.0);
                    g.scale = adjust.value("scale", 1.0);
                    if (adjust.contains("offset")) g.offset = ta::decodeVec2(adjust["offset"]);
                    glyphs.push_back(g);
                }
                if (!doc.editable()) glyphs.clear();
                letterGizmo.setGlyphs(std::move(glyphs), frame.layerToDoc, titleXform, staged.value("size", 100.0));
                const TitleLetterGizmo::Hit hot = drag == Drag::None && cursorInCanvas ? letterGizmo.hitTest(ImGui::GetIO().MousePos) : TitleLetterGizmo::Hit();
                letterGizmo.draw(dl, hot);
            }
            if (result == TitleTextEdit::Result::Changed) {
                std::string error;
                doc.stage(ta::Json::array({ta::Json{{"op", "set_text"}, {"layer", "id:" + textEdit->layerId()}, {"text", textEdit->previewText()}}}), &error);
            } else if (result == TitleTextEdit::Result::Commit) {
                endTextEdit(true);
            } else if (result == TitleTextEdit::Result::Cancel) {
                endTextEdit(false);
            }
        }
    }

    // progress: a 2 px bar and "Rendering 3/7 passes" after 250 ms of a Full render (R71)
    const CanvasRenderInfo& info = canvas->info();
    if (info.busy && info.runningMs > 250.0 && !info.draft) {
        dl->AddRectFilled(canvasMin, ImVec2(canvasMin.x + (canvasMax.x - canvasMin.x) * info.progress, canvasMin.y + 2.0f), IM_COL32(90, 160, 255, 255));
    }
    if (info.rebuilding) {
        const ImVec2 size = ImGui::CalcTextSize("Rebuilding preview...");
        dl->AddRectFilled(ImVec2(canvasMin.x + 8.0f, canvasMin.y + 8.0f), ImVec2(canvasMin.x + 20.0f + size.x, canvasMin.y + 16.0f + size.y), IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(ImVec2(canvasMin.x + 14.0f, canvasMin.y + 12.0f), IM_COL32(255, 255, 255, 255), "Rebuilding preview...");
    } else if (!info.hasImage && v.previewAs == "realtime") {
        // a newly opened document shows the checker and a spinner until its first image
        const ImVec2 c((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f);
        const float t = (float)now() * 6.0f;
        for (int i = 0; i < 8; i++) {
            const float a = t + i * 0.785f;
            dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * 14.0f, c.y + std::sin(a) * 14.0f), 2.5f, IM_COL32(255, 255, 255, 40 + i * 25));
        }
    }
    dl->PopClipRect();
    drawRulers(dl);
    // a ruler press starts a new guide (R32)
    if (v.rulers && doc.editable() && drag == Drag::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const bool onTop = m.y >= canvasMin.y - RULER && m.y < canvasMin.y && m.x > canvasMin.x;
        const bool onLeft = m.x >= canvasMin.x - RULER && m.x < canvasMin.x && m.y > canvasMin.y;
        if (onTop || onLeft) {
            DocumentView& mv = view();
            const TitleVec2 p = titleXform.toDoc(m);
            mv.guides.push_back({onLeft, onLeft ? std::round(p.x) : std::round(p.y)});
            guideIndex = (int)mv.guides.size() - 1;
            guideVertical = onLeft;
            drag = Drag::NewGuide;
        }
    }
    if (contentEmpty() && !textEditing()) drawEmptyState(dl);
    // the tiny layout's Panels button (R3)
    if (width < 480.0f * ImGui::GetFontSize() / 16.0f) {
        ImGui::SetCursorScreenPos(ImVec2(canvasMax.x - 80.0f, canvasMin.y + 6.0f));
        if (ImGui::SmallButton("Panels###panels")) narrowPanelsOpen = true;
    }
    ImGui::EndChild();
}
