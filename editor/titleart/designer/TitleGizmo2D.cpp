// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGizmo2D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine::editor::titledesigner;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    // values below this count as unchanged (readouts)
    constexpr double SAME = 1e-9;
    // the schema's transform.scale range (S80 §6.1)
    constexpr double SCALE_MIN = 0.01, SCALE_MAX = 100.0;

    double wrap180(double v) {
        double r = std::fmod(v + 180.0, 360.0);
        if (r < 0.0) r += 360.0;
        return r - 180.0;
    }

    TitleVec2 sub(TitleVec2 a, TitleVec2 b) { return {a.x - b.x, a.y - b.y}; }
    TitleVec2 add(TitleVec2 a, TitleVec2 b) { return {a.x + b.x, a.y + b.y}; }
    double dot(TitleVec2 a, TitleVec2 b) { return a.x * b.x + a.y * b.y; }
    float screenDistance(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

    float segmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float len2 = vx * vx + vy * vy;
        float t = len2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return screenDistance(p, ImVec2(a.x + vx * t, a.y + vy * t));
    }

    Json vec2Json(TitleVec2 p) { return Json::array({p.x, p.y}); }

    bool isCorner(Gizmo2DHandle h) { return h >= Gizmo2DHandle::CornerBL && h <= Gizmo2DHandle::CornerTL; }
    bool isEdge(Gizmo2DHandle h) { return h >= Gizmo2DHandle::EdgeL && h <= Gizmo2DHandle::EdgeT; }
    bool isRotateZone(Gizmo2DHandle h) { return h >= Gizmo2DHandle::RotateBL && h <= Gizmo2DHandle::RotateTL; }
    int cornerIndex(Gizmo2DHandle h) { return (int)h - (int)Gizmo2DHandle::CornerBL; }
    int edgeIndex(Gizmo2DHandle h) { return (int)h - (int)Gizmo2DHandle::EdgeL; }
    Gizmo2DHandle cornerHandle(int i) { return (Gizmo2DHandle)((int)Gizmo2DHandle::CornerBL + i); }
    Gizmo2DHandle edgeHandle(int i) { return (Gizmo2DHandle)((int)Gizmo2DHandle::EdgeL + i); }
    Gizmo2DHandle rotateHandle(int i) { return (Gizmo2DHandle)((int)Gizmo2DHandle::RotateBL + i); }

    // framePoint indices: corners 0..3 (BL, BR, TR, TL), edge midpoints 4..7 (L, R, B, T)
    TitleVec2 framePointOf(const TitleRect& frame, int index) {
        const double x0 = frame.x, x1 = frame.x + frame.width, y0 = frame.y, y1 = frame.y + frame.height;
        const double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
        switch (index) {
            case 0: return {x0, y0};
            case 1: return {x1, y0};
            case 2: return {x1, y1};
            case 3: return {x0, y1};
            case 4: return {x0, cy};
            case 5: return {x1, cy};
            case 6: return {cx, y0};
            default: return {cx, y1};
        }
    }
    constexpr int OPPOSITE_CORNER[4] = {2, 3, 0, 1};
    constexpr int OPPOSITE_EDGE[4] = {5, 4, 7, 6};

    // the document-space bounds of a layer box (or of its position when it has none)
    void growBounds(const Gizmo2DTarget& t, double& x0, double& y0, double& x1, double& y1, bool& any) {
        const Affine2 m = t.parentToDoc * layerTransformAffine(t.transform);
        const TitleRect& b = t.localBox;
        const auto include = [&](TitleVec2 p) {
            if (!any) {
                x0 = x1 = p.x;
                y0 = y1 = p.y;
                any = true;
                return;
            }
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        };
        if (b.empty()) {
            include(m.apply({0.0, 0.0}));
            return;
        }
        include(m.apply({b.x, b.y}));
        include(m.apply({b.x + b.width, b.y}));
        include(m.apply({b.x + b.width, b.y + b.height}));
        include(m.apply({b.x, b.y + b.height}));
    }

    std::string layersWord(size_t n) {
        return n == 1 ? std::string("Layer") : std::to_string(n) + " Layers";
    }

    constexpr const char* DOT = " \xC2\xB7 ";      // " · "
    constexpr const char* DEGREE = "\xC2\xB0";     // "°"

}

ImGuiMouseCursor jasperengine::editor::titledesigner::imguiCursor(Gizmo2DCursor cursor) {
    switch (cursor) {
        case Gizmo2DCursor::Arrow: return ImGuiMouseCursor_Arrow;
        case Gizmo2DCursor::Move: return ImGuiMouseCursor_ResizeAll;
        case Gizmo2DCursor::Rotate: return ImGuiMouseCursor_None;
        case Gizmo2DCursor::ResizeNS: return ImGuiMouseCursor_ResizeNS;
        case Gizmo2DCursor::ResizeEW: return ImGuiMouseCursor_ResizeEW;
        case Gizmo2DCursor::ResizeNESW: return ImGuiMouseCursor_ResizeNESW;
        case Gizmo2DCursor::ResizeNWSE: return ImGuiMouseCursor_ResizeNWSE;
    }
    return ImGuiMouseCursor_Arrow;
}

std::string jasperengine::editor::titledesigner::formatReadout(double value, int decimals) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", std::max(0, decimals), value);
    std::string s = text;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

TitleVec2 jasperengine::editor::titledesigner::nudgeDelta(int dx, int dy, bool shift) {
    const double step = shift ? 10.0 : 1.0;
    return {std::clamp(dx, -1, 1) * step, std::clamp(dy, -1, 1) * step};
}

std::vector<FieldEdit> jasperengine::editor::titledesigner::nudgeEdits(const std::vector<Gizmo2DTarget>& targets, TitleVec2 docDelta) {
    std::vector<FieldEdit> edits;
    for (const Gizmo2DTarget& t : targets) {
        const TitleVec2 d = t.parentToDoc.inverse().applyLinear(docDelta);
        edits.push_back({t.layerId, "transform.position", vec2Json(add(t.transform.position, d))});
    }
    return edits;
}

// ---- TitleGizmo2D ----

void TitleGizmo2D::setTargets(std::vector<Gizmo2DTarget> value, Tool toolValue, PivotMode modeValue, const CanvasXform& viewValue) {
    targets = std::move(value);
    tool = toolValue;
    mode = modeValue;
    view = viewValue;
    layout();
}

void TitleGizmo2D::layout() {
    hasFrame = false;
    frameToDoc = Affine2();
    frame = TitleRect();
    if (targets.empty()) return;
    if (targets.size() == 1) {
        const Gizmo2DTarget& t = targets[0];
        frameToDoc = t.parentToDoc * layerTransformAffine(t.transform);
        frame = t.localBox;
        hasFrame = !frame.empty();
    } else {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        bool any = false;
        for (const Gizmo2DTarget& t : targets) growBounds(t, x0, y0, x1, y1, any);
        frame = {x0, y0, x1 - x0, y1 - y0};
        hasFrame = !frame.empty();
    }
    const Gizmo2DTarget& first = targets[0];
    if (first.hasPivot) {
        pivotDoc = first.pivot;
    } else if (mode == PivotMode::Center && hasFrame) {
        pivotDoc = frameToDoc.apply({frame.x + frame.width * 0.5, frame.y + frame.height * 0.5});
    } else {
        pivotDoc = first.parentToDoc.apply(first.transform.position);
    }
    // the gesture's pivot stays put on screen while it runs (a Center pivot of several layers
    // would wander as their union turns)
    if (drag.gesture == Gesture::Pivot && drag.pivotMoved) pivotDoc = drag.pivotNow;
    if (drag.gesture == Gesture::Rotate) pivotDoc = drag.pivot;
}

bool TitleGizmo2D::hasRect() const {
    return tool == Tool::Rect && hasFrame;
}

TitleVec2 TitleGizmo2D::framePoint(int index) const {
    return framePointOf(frame, index);
}

ImVec2 TitleGizmo2D::screenOf(int index) const {
    return view.toScreen(frameToDoc.apply(framePoint(index)));
}

bool TitleGizmo2D::insideFrame(ImVec2 screen) const {
    if (!hasFrame) return false;
    const TitleVec2 p = frameToDoc.inverse().apply(view.toDoc(screen));
    return p.x >= frame.x && p.x <= frame.x + frame.width && p.y >= frame.y && p.y <= frame.y + frame.height;
}

bool TitleGizmo2D::screenCorners(ImVec2 out[4]) const {
    if (!hasFrame) return false;
    for (int i = 0; i < 4; i++) out[i] = screenOf(i);
    return true;
}

Gizmo2DHandle TitleGizmo2D::hitTest(ImVec2 s) const {
    if (targets.empty() || tool == Tool::Hand) return Gizmo2DHandle::None;
    const float half = settings.handleHit * 0.5f;
    const auto inSquare = [&](ImVec2 c) { return std::abs(s.x - c.x) <= half && std::abs(s.y - c.y) <= half; };
    const ImVec2 pv = view.toScreen(pivotDoc);
    if ((tool == Tool::Rect || tool == Tool::Rotate) && screenDistance(s, pv) <= half) return Gizmo2DHandle::Pivot;
    if (hasRect()) {
        for (int i = 0; i < 4; i++) {
            if (inSquare(screenOf(i))) return cornerHandle(i);
        }
        for (int i = 0; i < 4; i++) {
            if (inSquare(screenOf(4 + i))) return edgeHandle(i);
        }
        if (!insideFrame(s)) {
            int best = -1;
            float bestDistance = half + settings.rotateZone;
            for (int i = 0; i < 4; i++) {
                const float d = screenDistance(s, screenOf(i));
                if (d <= bestDistance) {
                    best = i;
                    bestDistance = d;
                }
            }
            if (best >= 0) return rotateHandle(best);
        }
    }
    if (tool == Tool::Move) {
        if (inSquare(pv)) return Gizmo2DHandle::MoveXY;
        const float arm = settings.armLength;
        if (segmentDistance(s, ImVec2(pv.x + half, pv.y), ImVec2(pv.x + arm, pv.y)) <= 8.0f) return Gizmo2DHandle::MoveX;
        if (segmentDistance(s, ImVec2(pv.x, pv.y - half), ImVec2(pv.x, pv.y - arm)) <= 8.0f) return Gizmo2DHandle::MoveY;
    }
    if (tool == Tool::Rotate && std::abs(screenDistance(s, pv) - settings.ringRadius) <= 10.0f) return Gizmo2DHandle::Ring;
    if (tool == Tool::Scale && inSquare(pv)) return Gizmo2DHandle::ScaleBox;
    if (insideFrame(s)) return Gizmo2DHandle::Inside;
    return Gizmo2DHandle::None;
}

Gizmo2DCursor TitleGizmo2D::cursor(Gizmo2DHandle handle) const {
    if (isRotateZone(handle) || handle == Gizmo2DHandle::Ring) return Gizmo2DCursor::Rotate;
    if (isCorner(handle) || isEdge(handle)) {
        const int index = isCorner(handle) ? cornerIndex(handle) : 4 + edgeIndex(handle);
        const ImVec2 c = view.toScreen(frameToDoc.apply({frame.x + frame.width * 0.5, frame.y + frame.height * 0.5}));
        const ImVec2 h = screenOf(index);
        // the handle's direction from the centre on screen, folded to 0..180 (y down)
        double a = std::atan2(h.y - c.y, h.x - c.x) / DEG;
        a = std::fmod(a + 360.0, 180.0);
        if (a < 22.5 || a >= 157.5) return Gizmo2DCursor::ResizeEW;
        if (a < 67.5) return Gizmo2DCursor::ResizeNWSE;
        if (a < 112.5) return Gizmo2DCursor::ResizeNS;
        return Gizmo2DCursor::ResizeNESW;
    }
    if (handle == Gizmo2DHandle::ScaleBox) return Gizmo2DCursor::ResizeNWSE;
    if (handle == Gizmo2DHandle::None) return Gizmo2DCursor::Arrow;
    return Gizmo2DCursor::Move;
}

bool TitleGizmo2D::begin(Gizmo2DHandle handle, ImVec2 screen, const GizmoModifiers&) {
    if (handle == Gizmo2DHandle::None || targets.empty()) return false;
    const bool rectHandle = isCorner(handle) || isEdge(handle) || isRotateZone(handle);
    if (rectHandle && !hasRect()) return false;
    if ((handle == Gizmo2DHandle::MoveX || handle == Gizmo2DHandle::MoveY || handle == Gizmo2DHandle::MoveXY) && tool != Tool::Move) return false;
    if (handle == Gizmo2DHandle::Ring && tool != Tool::Rotate) return false;
    if (handle == Gizmo2DHandle::ScaleBox && tool != Tool::Scale) return false;
    if (handle == Gizmo2DHandle::Pivot && tool != Tool::Rect && tool != Tool::Rotate) return false;
    if (handle == Gizmo2DHandle::Inside && !hasFrame) return false;

    drag = Drag();
    drag.handle = handle;
    drag.start = targets;
    drag.frameToDoc = frameToDoc;
    drag.frame = frame;
    drag.pivot = pivotDoc;
    drag.pivotNow = pivotDoc;
    drag.press = view.toDoc(screen);
    drag.pressScreen = screen;
    drag.first = targets[0].transform;
    const TitleVec2 r = sub(drag.press, drag.pivot);
    drag.lastAngle = std::atan2(r.y, r.x) / DEG;

    const bool single = targets.size() == 1;
    if (handle == Gizmo2DHandle::Inside || handle == Gizmo2DHandle::MoveX || handle == Gizmo2DHandle::MoveY || handle == Gizmo2DHandle::MoveXY) {
        drag.gesture = Gesture::Move;
    } else if (isRotateZone(handle) || handle == Gizmo2DHandle::Ring) {
        drag.gesture = Gesture::Rotate;
    } else if (isEdge(handle) && single && targets[0].type == LayerType::Text) {
        drag.gesture = Gesture::EdgeText;
    } else if (isEdge(handle) && single && targets[0].type == LayerType::Shape) {
        drag.gesture = Gesture::EdgeShape;
    } else if (isCorner(handle) || isEdge(handle) || handle == Gizmo2DHandle::ScaleBox) {
        drag.gesture = Gesture::Scale;
    } else {
        drag.gesture = Gesture::Pivot;
    }
    if (drag.gesture == Gesture::EdgeText) {
        const bool horizontal = edgeIndex(handle) < 2;
        drag.field = horizontal ? "h_scale" : "v_scale";
        drag.fieldStart = drag.fieldValue = horizontal ? targets[0].hScale : targets[0].vScale;
    } else if (drag.gesture == Gesture::EdgeShape) {
        const bool horizontal = edgeIndex(handle) < 2;
        drag.field = horizontal ? "aspect" : "size";
        drag.fieldStart = drag.fieldValue = horizontal ? targets[0].aspect : targets[0].size;
        drag.aspectStart = drag.aspectValue = targets[0].aspect;
    }
    return true;
}

void TitleGizmo2D::end() {
    drag = Drag();
}

std::vector<FieldEdit> TitleGizmo2D::applySimilarity(const Affine2& g, double turnCw, double factor, bool rotates, bool scales) {
    std::vector<FieldEdit> edits;
    bool changed = false;
    for (size_t i = 0; i < drag.start.size(); i++) {
        const Gizmo2DTarget& t = drag.start[i];
        const Affine2& p = t.parentToDoc;
        // a turn seen through a mirrored parent runs the other way in the layer's parent space
        const double sigma = p.determinant() < 0.0 ? -1.0 : 1.0;
        TitleTransform next = t.transform;
        next.position = p.inverse().apply(g.apply(p.apply(t.transform.position)));
        edits.push_back({t.layerId, "transform.position", vec2Json(next.position)});
        if (rotates) {
            next.rotation = t.transform.rotation + sigma * turnCw;
            edits.push_back({t.layerId, "transform.rotation", next.rotation});
        }
        if (scales) {
            next.scale = t.transform.scale * factor;
            edits.push_back({t.layerId, "transform.scale", next.scale});
        }
        changed = changed || std::abs(next.position.x - t.transform.position.x) > SAME || std::abs(next.position.y - t.transform.position.y) > SAME ||
                  std::abs(next.rotation - t.transform.rotation) > SAME || std::abs(next.scale - t.transform.scale) > SAME;
        if (i == 0) drag.first = next;
    }
    drag.turn = turnCw;
    drag.factor = factor;
    drag.changed = changed;
    return edits;
}

std::vector<FieldEdit> TitleGizmo2D::updateEdge(TitleVec2 mouse, const GizmoModifiers& modifiers, const SnapSettings& increments) {
    const Gizmo2DTarget& t = drag.start[0];
    const int e = edgeIndex(drag.handle);
    const bool horizontal = e < 2;
    const TitleRect& b = drag.frame;
    const double x0 = b.x, x1 = b.x + b.width, y0 = b.y, y1 = b.y + b.height;
    const double fixed = e == 0 ? x1 : e == 1 ? x0 : e == 2 ? y1 : y0;
    // the content scales about the layer's origin (text frames and shapes are centred on it);
    // the position then moves so the opposite edge stays, or with Alt the origin itself
    const double anchor = modifiers.alt ? 0.0 : fixed;
    const Affine2 inv = drag.frameToDoc.inverse();
    const TitleVec2 lm = inv.apply(mouse), lp = inv.apply(drag.press);
    const double pressed = (horizontal ? lp.x : lp.y) - anchor;
    double r = std::abs(pressed) > 1e-9 ? ((horizontal ? lm.x : lm.y) - anchor) / pressed : 1.0;
    r = std::max(r, 1e-3);

    std::vector<FieldEdit> edits;
    double effective = 1.0;
    if (drag.gesture == Gesture::EdgeText) {
        double v = std::clamp(drag.fieldStart * r, 0.1, 10.0);
        if (modifiers.ctrl) v = std::clamp(incrementScale(v, increments), 0.1, 10.0);
        effective = v / drag.fieldStart;
        drag.fieldValue = v;
        edits.push_back({t.layerId, drag.field, v});
    } else if (horizontal) {
        double aspect = std::clamp(drag.aspectStart * r, 0.05, 20.0);
        if (modifiers.ctrl) aspect = std::clamp(incrementScale(aspect, increments), 0.05, 20.0);
        effective = aspect / drag.aspectStart;
        drag.fieldValue = drag.aspectValue = aspect;
        edits.push_back({t.layerId, "aspect", aspect});
    } else {
        // the height is `size`; the width (size x aspect) stays
        double size = std::clamp(drag.fieldStart * r, 1.0, 16384.0);
        if (modifiers.ctrl) size = std::clamp(snapToIncrement(size, increments.moveIncrement), 1.0, 16384.0);
        effective = size / drag.fieldStart;
        const double aspect = std::clamp(drag.aspectStart / effective, 0.05, 20.0);
        drag.fieldValue = size;
        drag.aspectValue = aspect;
        edits.push_back({t.layerId, "size", size});
        edits.push_back({t.layerId, "aspect", aspect});
    }
    const double shift = anchor - effective * anchor;
    const TitleVec2 local = horizontal ? TitleVec2{shift, 0.0} : TitleVec2{0.0, shift};
    TitleTransform next = t.transform;
    next.position = add(t.transform.position, layerTransformAffine(t.transform).applyLinear(local));
    edits.push_back({t.layerId, "transform.position", vec2Json(next.position)});
    drag.first = next;
    drag.changed = std::abs(drag.fieldValue - drag.fieldStart) > SAME || std::abs(drag.aspectValue - drag.aspectStart) > SAME;
    return edits;
}

std::vector<FieldEdit> TitleGizmo2D::update(ImVec2 screen, const GizmoModifiers& modifiers, const SnapContext* snap) {
    if (!active()) return {};
    const SnapSettings increments = snap ? snap->settings : SnapSettings();
    const bool snapping = snap && snap->settings.enabled && !modifiers.ctrl;
    drag.snap = SnapResult();
    const TitleVec2 m = view.toDoc(screen);

    switch (drag.gesture) {
        case Gesture::Move: {
            TitleVec2 d = sub(m, drag.press);
            bool lockX = drag.handle == Gizmo2DHandle::MoveY, lockY = drag.handle == Gizmo2DHandle::MoveX;
            if (modifiers.shift && (drag.handle == Gizmo2DHandle::Inside || drag.handle == Gizmo2DHandle::MoveXY)) {
                if (std::abs(d.x) >= std::abs(d.y)) lockY = true;
                else lockX = true;
            }
            if (lockX) d.x = 0.0;
            if (lockY) d.y = 0.0;
            if (modifiers.ctrl) {
                d = incrementMove(d, increments);
            } else if (snapping) {
                double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
                bool any = false;
                for (const Gizmo2DTarget& t : drag.start) growBounds(t, x0, y0, x1, y1, any);
                drag.snap = snapBox(*snap, {x0, y0, x1 - x0, y1 - y0}, d);
                d = drag.snap.delta;
                // an axis the drag is locked to never snaps off it
                auto& lines = drag.snap.lines;
                lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const SnapLine& l) { return (l.vertical && lockX) || (!l.vertical && lockY); }), lines.end());
            }
            if (lockX) d.x = 0.0;
            if (lockY) d.y = 0.0;
            drag.moved = d;
            return applySimilarity(Affine2::translate(d.x, d.y), 0.0, 1.0, false, false);
        }
        case Gesture::Rotate: {
            const TitleVec2 r = sub(m, drag.pivot);
            if (std::abs(r.x) + std::abs(r.y) > 1e-12) {
                const double a = std::atan2(r.y, r.x) / DEG;
                drag.turned += wrap180(a - drag.lastAngle);
                drag.lastAngle = a;
            }
            // y up: a counter-clockwise pointer turn is a negative (clockwise-positive) rotation
            double turn = -drag.turned;
            if (modifiers.shift) turn = snapToIncrement(turn, 15.0);
            else if (modifiers.ctrl) turn = incrementAngle(turn, increments);
            const TitleVec2 p = drag.pivot;
            const Affine2 g = Affine2::translate(p.x, p.y) * Affine2::rotateClockwise(turn) * Affine2::translate(-p.x, -p.y);
            return applySimilarity(g, turn, 1.0, true, false);
        }
        case Gesture::Scale: {
            TitleVec2 o = drag.pivot;
            double f = 1.0;
            TitleVec2 mm = m;
            if (snapping && drag.handle != Gizmo2DHandle::ScaleBox) {
                drag.snap = snapPoint(*snap, m, {0.0, 0.0});
                mm = add(m, drag.snap.delta);
            }
            if (drag.handle == Gizmo2DHandle::ScaleBox) {
                // right / up grows (Unity's centre cube)
                const float gx = screen.x - drag.pressScreen.x, gy = screen.y - drag.pressScreen.y;
                f = 1.0 + (gx - gy) / 100.0;
            } else if (isCorner(drag.handle)) {
                if (!modifiers.alt) o = drag.frameToDoc.apply(framePointOf(drag.frame, OPPOSITE_CORNER[cornerIndex(drag.handle)]));
                const TitleVec2 v0 = sub(drag.press, o);
                const double len2 = dot(v0, v0);
                f = len2 > 1e-12 ? dot(sub(mm, o), v0) / len2 : 1.0;
            } else {
                // an edge of an Image or Vector layer, or of several layers: uniform about the opposite edge
                const int e = edgeIndex(drag.handle);
                const bool horizontal = e < 2;
                const Affine2 inv = drag.frameToDoc.inverse();
                const TitleVec2 fixedPoint = framePointOf(drag.frame, OPPOSITE_EDGE[e]);
                const TitleVec2 anchorLocal = modifiers.alt ? inv.apply(drag.pivot) : fixedPoint;
                const double anchor = horizontal ? anchorLocal.x : anchorLocal.y;
                const TitleVec2 lm = inv.apply(mm), lp = inv.apply(drag.press);
                const double pressed = (horizontal ? lp.x : lp.y) - anchor;
                f = std::abs(pressed) > 1e-9 ? ((horizontal ? lm.x : lm.y) - anchor) / pressed : 1.0;
                if (!modifiers.alt) o = drag.frameToDoc.apply(fixedPoint);
            }
            double fMin = 0.0, fMax = 1e300;
            for (const Gizmo2DTarget& t : drag.start) {
                const double s0 = std::max(t.transform.scale, 1e-9);
                fMin = std::max(fMin, SCALE_MIN / s0);
                fMax = std::min(fMax, SCALE_MAX / s0);
            }
            f = std::clamp(f, fMin, std::max(fMin, fMax));
            if (modifiers.ctrl) {
                if (drag.start.size() == 1) {
                    const double s0 = std::max(drag.start[0].transform.scale, 1e-9);
                    f = incrementScale(s0 * f, increments) / s0;
                } else {
                    f = incrementScale(f, increments);
                }
                f = std::clamp(f, fMin, std::max(fMin, fMax));
            }
            const Affine2 g = Affine2::translate(o.x, o.y) * Affine2::scale(f, f) * Affine2::translate(-o.x, -o.y);
            return applySimilarity(g, 0.0, f, false, true);
        }
        case Gesture::EdgeText:
        case Gesture::EdgeShape: {
            TitleVec2 mm = m;
            if (snapping) {
                drag.snap = snapPoint(*snap, m, {0.0, 0.0});
                mm = add(m, drag.snap.delta);
            }
            return updateEdge(mm, modifiers, increments);
        }
        case Gesture::Pivot: {
            TitleVec2 d = sub(m, drag.press);
            if (snapping) {
                drag.snap = snapPoint(*snap, drag.pivot, d);
                d = drag.snap.delta;
            }
            drag.pivotNow = add(drag.pivot, d);
            drag.pivotMoved = true;
            pivotDoc = drag.pivotNow;
            return {};
        }
        case Gesture::None: break;
    }
    return {};
}

std::string TitleGizmo2D::readout() const {
    if (!active() || !drag.changed) return "";
    const size_t n = drag.start.size();
    const TitleTransform& t0 = drag.start[0].transform;
    const TitleTransform& t1 = drag.first;
    // the first layer's position, only the axes that moved
    const auto position = [&]() {
        std::string s;
        if (std::abs(t1.position.x - t0.position.x) > SAME) s += std::string(DOT) + "X " + formatReadout(t1.position.x);
        if (std::abs(t1.position.y - t0.position.y) > SAME) s += std::string(DOT) + "Y " + formatReadout(t1.position.y);
        return s;
    };
    switch (drag.gesture) {
        case Gesture::Move: {
            if (n == 1) return "Move" + position();
            std::string s = "Move " + layersWord(n);
            if (std::abs(drag.moved.x) > SAME) s += std::string(DOT) + "X " + formatReadout(drag.moved.x);
            if (std::abs(drag.moved.y) > SAME) s += std::string(DOT) + "Y " + formatReadout(drag.moved.y);
            return s;
        }
        case Gesture::Rotate:
            if (n == 1) return "Rotate" + std::string(DOT) + formatReadout(t1.rotation) + DEGREE + position();
            return "Rotate " + layersWord(n) + DOT + formatReadout(drag.turn) + DEGREE;
        case Gesture::Scale:
            if (n == 1) return "Scale" + std::string(DOT) + formatReadout(t1.scale, 2) + position();
            return "Scale " + layersWord(n) + DOT + "\xC3\x97" + formatReadout(drag.factor, 2);
        case Gesture::EdgeText: {
            const bool horizontal = drag.field == "h_scale";
            std::string s = std::string(horizontal ? "Horizontal Scale" : "Vertical Scale") + DOT + formatReadout(drag.fieldValue * 100.0) + " %";
            if (settings.cjkLabels) s += horizontal ? " (\xE9\x95\xB7\xE4\xBD\x93)" : " (\xE5\xB9\xB3\xE4\xBD\x93)";    // 長体 / 平体
            return s + position();
        }
        case Gesture::EdgeShape:
            if (drag.field == "aspect") return "Aspect" + std::string(DOT) + formatReadout(drag.fieldValue, 2) + position();
            return "Size" + std::string(DOT) + formatReadout(drag.fieldValue) + " px" + DOT + "Aspect " + formatReadout(drag.aspectValue, 2) + position();
        case Gesture::Pivot:
        case Gesture::None: break;
    }
    return "";
}

std::string TitleGizmo2D::gestureName() const {
    const size_t n = drag.start.size();
    switch (drag.gesture) {
        case Gesture::Move: return "Move " + layersWord(n);
        case Gesture::Rotate: return "Rotate " + layersWord(n);
        case Gesture::Scale: return "Scale " + layersWord(n);
        case Gesture::EdgeText: return drag.field == "h_scale" ? "Change Horizontal Scale" : "Change Vertical Scale";
        case Gesture::EdgeShape: return drag.field == "aspect" ? "Change Aspect" : "Change Size";
        case Gesture::Pivot:
        case Gesture::None: break;
    }
    return "";
}

void TitleGizmo2D::draw(ImDrawList* dl, Gizmo2DHandle hovered, Gizmo2DHandle activeHandle) const {
    if (!dl || targets.empty() || tool == Tool::Hand) return;
    const auto lit = [&](Gizmo2DHandle h) { return h == hovered || h == activeHandle; };
    const ImVec2 pv = view.toScreen(pivotDoc);
    float lowest = pv.y;

    if (hasFrame) {
        ImVec2 c[4];
        for (int i = 0; i < 4; i++) {
            c[i] = screenOf(i);
            lowest = std::max(lowest, c[i].y);
        }
        // the effects' reach: a faint box around the content (Guides > Effect Reach)
        if (targets.size() == 1 && targets[0].effectReach > 0.0) {
            const double scale = std::sqrt(std::abs(frameToDoc.determinant()));
            const double g = scale > 1e-12 ? targets[0].effectReach / scale : 0.0;
            const TitleRect r{frame.x - g, frame.y - g, frame.width + 2 * g, frame.height + 2 * g};
            ImVec2 q[4] = {view.toScreen(frameToDoc.apply({r.x, r.y})), view.toScreen(frameToDoc.apply({r.x + r.width, r.y})),
                           view.toScreen(frameToDoc.apply({r.x + r.width, r.y + r.height})), view.toScreen(frameToDoc.apply({r.x, r.y + r.height}))};
            dl->AddPolyline(q, 4, IM_COL32(143, 198, 255, 90), 1.0f, ImDrawFlags_Closed);
        }
        if (tool == Tool::Rect) {
            overlay::polyline(dl, c, 4, overlay::ACCENT, lit(Gizmo2DHandle::Inside), true);
            const float side = settings.handleSize;
            for (int i = 0; i < 4; i++) {
                const bool on = lit(edgeHandle(i));
                overlay::square(dl, screenOf(4 + i), on ? side + 2.0f : side, on ? overlay::ACCENT_LIT : overlay::WHITE, on);
            }
            for (int i = 0; i < 4; i++) {
                const bool on = lit(cornerHandle(i)) || lit(rotateHandle(i));
                overlay::square(dl, c[i], on ? side + 2.0f : side, on ? overlay::ACCENT_LIT : overlay::WHITE, on);
            }
        } else {
            // the other tools keep a thin outline of the selection
            dl->AddPolyline(c, 4, overlay::HALO_COLOR, 3.0f, ImDrawFlags_Closed);
            dl->AddPolyline(c, 4, overlay::ACCENT, 1.0f, ImDrawFlags_Closed);
        }
    }

    if (tool == Tool::Move) {
        const float arm = settings.armLength;
        const float head = 14.0f, headHalf = 7.0f;
        for (int axis = 0; axis < 2; axis++) {
            const Gizmo2DHandle h = axis == 0 ? Gizmo2DHandle::MoveX : Gizmo2DHandle::MoveY;
            const bool on = lit(h);
            const ImVec2 u = axis == 0 ? ImVec2(1.0f, 0.0f) : ImVec2(0.0f, -1.0f);
            const ImVec2 tip(pv.x + u.x * arm, pv.y + u.y * arm);
            const ImVec2 base(tip.x - u.x * head, tip.y - u.y * head);
            const ImU32 color = on ? overlay::AXIS_LIT[axis] : overlay::AXIS[axis];
            overlay::line(dl, ImVec2(pv.x + u.x * 10.0f, pv.y + u.y * 10.0f), base, color, on);
            overlay::triangle(dl, tip, ImVec2(base.x - u.y * headHalf, base.y + u.x * headHalf), ImVec2(base.x + u.y * headHalf, base.y - u.x * headHalf), color);
        }
        const bool on = lit(Gizmo2DHandle::MoveXY);
        overlay::square(dl, pv, on ? 14.0f : 12.0f, on ? overlay::ACCENT_LIT : overlay::WHITE, on);
        lowest = std::max(lowest, pv.y + 12.0f);
    } else if (tool == Tool::Rotate) {
        overlay::circle(dl, pv, settings.ringRadius, lit(Gizmo2DHandle::Ring) ? overlay::ACCENT_LIT : overlay::ACCENT, lit(Gizmo2DHandle::Ring));
        lowest = std::max(lowest, pv.y + settings.ringRadius);
    } else if (tool == Tool::Scale) {
        const bool on = lit(Gizmo2DHandle::ScaleBox);
        overlay::square(dl, pv, on ? 14.0f : 12.0f, on ? overlay::ACCENT_LIT : overlay::WHITE, on);
    }
    if (tool == Tool::Rect || tool == Tool::Rotate) {
        const bool on = lit(Gizmo2DHandle::Pivot);
        overlay::circle(dl, pv, on ? 7.0f : 6.0f, on ? overlay::ACCENT_LIT : overlay::WHITE, false);
        dl->AddCircleFilled(pv, 1.5f, overlay::WHITE);
    }

    const std::string text = readout();
    if (!text.empty()) overlay::label(dl, ImVec2(pv.x, lowest + 22.0f), text, overlay::WHITE, true);
}

// ---- overlay ----

void jasperengine::editor::titledesigner::overlay::line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, bool hovered) {
    dl->AddLine(a, b, HALO_COLOR, hovered ? HOVER_HALO_WIDTH : HALO_WIDTH);
    dl->AddLine(a, b, color, hovered ? HOVER_WIDTH : STROKE_WIDTH);
}

void jasperengine::editor::titledesigner::overlay::polyline(ImDrawList* dl, const ImVec2* points, int count, ImU32 color, bool hovered, bool closed) {
    if (count < 2) return;
    const ImDrawFlags flags = closed ? ImDrawFlags_Closed : ImDrawFlags_None;
    dl->AddPolyline(points, count, HALO_COLOR, hovered ? HOVER_HALO_WIDTH : HALO_WIDTH, flags);
    dl->AddPolyline(points, count, color, hovered ? HOVER_WIDTH : STROKE_WIDTH, flags);
}

void jasperengine::editor::titledesigner::overlay::circle(ImDrawList* dl, ImVec2 centre, float radius, ImU32 color, bool hovered) {
    dl->AddCircle(centre, radius, HALO_COLOR, 0, hovered ? HOVER_HALO_WIDTH : HALO_WIDTH);
    dl->AddCircle(centre, radius, color, 0, hovered ? HOVER_WIDTH : STROKE_WIDTH);
}

void jasperengine::editor::titledesigner::overlay::triangle(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImU32 color) {
    dl->AddTriangle(a, b, c, HALO_COLOR, 4.0f);
    dl->AddTriangleFilled(a, b, c, color);
}

void jasperengine::editor::titledesigner::overlay::square(ImDrawList* dl, ImVec2 centre, float side, ImU32 color, bool hovered) {
    const float h = side * 0.5f, rim = hovered ? 3.0f : 2.0f;
    dl->AddRectFilled(ImVec2(centre.x - h - rim, centre.y - h - rim), ImVec2(centre.x + h + rim, centre.y + h + rim), HALO_COLOR);
    dl->AddRectFilled(ImVec2(centre.x - h, centre.y - h), ImVec2(centre.x + h, centre.y + h), color);
}

void jasperengine::editor::titledesigner::overlay::diamond(ImDrawList* dl, ImVec2 centre, float side, ImU32 color, bool hovered) {
    const float h = side * 0.70710678f, rim = hovered ? 3.0f : 2.0f;
    const float o = h + rim * 1.41421356f;
    dl->AddQuadFilled(ImVec2(centre.x, centre.y - o), ImVec2(centre.x + o, centre.y), ImVec2(centre.x, centre.y + o), ImVec2(centre.x - o, centre.y), HALO_COLOR);
    dl->AddQuadFilled(ImVec2(centre.x, centre.y - h), ImVec2(centre.x + h, centre.y), ImVec2(centre.x, centre.y + h), ImVec2(centre.x - h, centre.y), color);
}

void jasperengine::editor::titledesigner::overlay::disc(ImDrawList* dl, ImVec2 centre, float radius, ImU32 color, bool hovered) {
    dl->AddCircleFilled(centre, radius + (hovered ? 3.0f : 2.0f), HALO_COLOR);
    dl->AddCircleFilled(centre, radius, color);
}

void jasperengine::editor::titledesigner::overlay::label(ImDrawList* dl, ImVec2 at, const std::string& text, ImU32 color, bool centred) {
    if (text.empty()) return;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos = centred ? ImVec2(std::floor(at.x - size.x * 0.5f), std::floor(at.y - size.y * 0.5f)) : ImVec2(std::floor(at.x), std::floor(at.y - size.y * 0.5f));
    dl->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 2.0f), ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 2.0f), IM_COL32(13, 13, 16, 220), 3.0f);
    dl->AddText(pos, color, text.c_str());
}

void jasperengine::editor::titledesigner::overlay::rotateCursor(ImDrawList* dl, ImVec2 at) {
    // three quarters of a circle and an arrowhead at its end
    constexpr int STEPS = 16;
    const float r = 7.0f;
    ImVec2 pts[STEPS + 1];
    for (int i = 0; i <= STEPS; i++) {
        const float a = (float)(-0.25 * PI + 1.5 * PI * i / STEPS);
        pts[i] = ImVec2(at.x + std::cos(a) * r, at.y + std::sin(a) * r);
    }
    dl->AddPolyline(pts, STEPS + 1, IM_COL32(0, 0, 0, 255), 4.0f, ImDrawFlags_None);
    dl->AddPolyline(pts, STEPS + 1, IM_COL32(255, 255, 255, 255), 2.0f, ImDrawFlags_None);
    const ImVec2 e = pts[STEPS];
    const float a = (float)(1.25 * PI);
    const ImVec2 t(-std::sin(a), std::cos(a));
    const ImVec2 n(std::cos(a), std::sin(a));
    const ImVec2 p0(e.x + t.x * 5.0f, e.y + t.y * 5.0f), p1(e.x + n.x * 4.0f, e.y + n.y * 4.0f), p2(e.x - n.x * 4.0f, e.y - n.y * 4.0f);
    dl->AddTriangleFilled(p0, p1, p2, IM_COL32(255, 255, 255, 255));
    dl->AddTriangle(p0, p1, p2, IM_COL32(0, 0, 0, 255), 1.0f);
}
