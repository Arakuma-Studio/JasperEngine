// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLetterGizmo.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;

namespace {

    constexpr float KNOB_DISTANCE = 16.0f;  // screen px above the cell's top edge
    constexpr float KNOB_RADIUS = 4.5f;
    constexpr float SQUARE = 4.0f;          // half side of the scale square
    constexpr float DOT = 3.5f;
    constexpr double PI = 3.14159265358979323846;

    float distance(ImVec2 a, ImVec2 b) {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

    bool insideQuad(const ImVec2 q[4], ImVec2 p) {
        // the four edge cross products share a sign inside a convex quad
        int sign = 0;
        for (int i = 0; i < 4; i++) {
            const ImVec2 a = q[i], b = q[(i + 1) % 4];
            const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            const int s = cross > 0.0f ? 1 : cross < 0.0f ? -1 : 0;
            if (s == 0) continue;
            if (sign == 0) sign = s;
            else if (s != sign) return false;
        }
        return true;
    }

    std::string key(int index, const char* member) {
        return "chars." + std::to_string(index) + "." + member;
    }

}

void TitleLetterGizmo::setGlyphs(std::vector<LetterGlyph> next, const Affine2& layerToDoc, const CanvasXform& canvas, double size) {
    glyphs = std::move(next);
    toDoc = layerToDoc;
    xform = canvas;
    textSize = size > 0.0 ? size : 1.0;
}

void TitleLetterGizmo::corners(size_t i, ImVec2 out[4]) const {
    const LetterGlyph& g = glyphs[i];
    const Affine2 turn = Affine2::rotateClockwise(g.rotation);
    const double hw = g.width * 0.5, hh = g.height * 0.5;
    const TitleVec2 local[4] = {{-hw, hh}, {hw, hh}, {hw, -hh}, {-hw, -hh}};
    for (int k = 0; k < 4; k++) {
        const TitleVec2 v = turn.applyLinear(local[k]);
        out[k] = xform.toScreen(toDoc.apply({g.centre.x + v.x, g.centre.y + v.y}));
    }
}

ImVec2 TitleLetterGizmo::centreOf(size_t i) const {
    return xform.toScreen(toDoc.apply(glyphs[i].centre));
}

ImVec2 TitleLetterGizmo::knobOf(size_t i) const {
    ImVec2 q[4];
    corners(i, q);
    const ImVec2 top((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f);
    const ImVec2 c = centreOf(i);
    float dx = top.x - c.x, dy = top.y - c.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) {
        dx = 0.0f;
        dy = -1.0f;
    } else {
        dx /= len;
        dy /= len;
    }
    return ImVec2(top.x + dx * KNOB_DISTANCE, top.y + dy * KNOB_DISTANCE);
}

ImVec2 TitleLetterGizmo::scaleHandleOf(size_t i) const {
    ImVec2 q[4];
    corners(i, q);
    return q[2];
}

TitleLetterGizmo::Hit TitleLetterGizmo::hitTest(ImVec2 p) const {
    // knobs and squares first (they sit on or outside the cells), then the cells
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (distance(p, knobOf(i)) <= KNOB_RADIUS + 3.0f) return {Part::Rotate, (int)i};
        const ImVec2 s = scaleHandleOf(i);
        if (std::fabs(p.x - s.x) <= SQUARE + 3.0f && std::fabs(p.y - s.y) <= SQUARE + 3.0f) return {Part::Scale, (int)i};
    }
    for (size_t i = 0; i < glyphs.size(); i++) {
        ImVec2 q[4];
        corners(i, q);
        if (insideQuad(q, p)) return {Part::Move, (int)i};
    }
    return {};
}

void TitleLetterGizmo::begin(const Hit& hit, ImVec2 screen) {
    if (hit.part == Part::None || hit.glyph < 0 || hit.glyph >= (int)glyphs.size()) {
        active = Hit();
        return;
    }
    active = hit;
    pressed = screen;
    startGlyphs = glyphs;
    startCentre = centreOf((size_t)hit.glyph);
}

Json TitleLetterGizmo::values(ImVec2 screen, bool snap) const {
    Json out = Json::object();
    if (active.part == Part::None) return out;
    if (active.part == Part::Move) {
        // screen -> document -> the layer's own axes, as % of the text size
        const TitleVec2 d = {(screen.x - pressed.x) / std::max(1e-6f, xform.zoom), -(screen.y - pressed.y) / std::max(1e-6f, xform.zoom)};
        const TitleVec2 local = toDoc.inverse().applyLinear(d);
        const double px = local.x / textSize * 100.0, py = local.y / textSize * 100.0;
        for (const LetterGlyph& g : startGlyphs) {
            double x = g.offset.x + px, y = g.offset.y + py;
            if (snap) {
                x = std::round(x);
                y = std::round(y);
            }
            out[key(g.index, "offset")] = Json::array({x, y});
        }
    } else if (active.part == Part::Rotate) {
        // the pointer's angle about the grabbed cell's centre; screen y points down, so the angle
        // grows clockwise; a mirrored layer turns the other way in its own space
        const double a0 = std::atan2(pressed.y - startCentre.y, pressed.x - startCentre.x);
        const double a1 = std::atan2(screen.y - startCentre.y, screen.x - startCentre.x);
        double delta = (a1 - a0) * 180.0 / PI;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        if (toDoc.determinant() < 0.0) delta = -delta;
        for (const LetterGlyph& g : startGlyphs) {
            double r = g.rotation + delta;
            if (snap) r = std::round(r / 15.0) * 15.0;
            out[key(g.index, "rotation")] = r;
        }
    } else if (active.part == Part::Scale) {
        const double from = std::max(1.0f, distance(pressed, startCentre));
        const double factor = distance(screen, startCentre) / from;
        for (const LetterGlyph& g : startGlyphs) {
            double s = g.scale * factor;
            if (snap) s = std::round(s / 0.05) * 0.05;
            out[key(g.index, "scale")] = std::clamp(s, 0.01, 100.0);
        }
    }
    return out;
}

std::string TitleLetterGizmo::stepName() const {
    switch (active.part) {
        case Part::Move: return "Move Letters";
        case Part::Rotate: return "Rotate Letters";
        case Part::Scale: return "Scale Letters";
        default: return "Edit Letters";
    }
}

void TitleLetterGizmo::draw(ImDrawList* dl, const Hit& hovered) const {
    if (!dl) return;
    const ImU32 line = IM_COL32(255, 196, 64, 230);
    const ImU32 hot = IM_COL32(255, 255, 255, 255);
    for (size_t i = 0; i < glyphs.size(); i++) {
        ImVec2 q[4];
        corners(i, q);
        const bool thisOne = hovered.glyph == (int)i || active.glyph == (int)i;
        const Part part = active.part != Part::None ? active.part : hovered.part;
        dl->AddPolyline(q, 4, line, ImDrawFlags_Closed, 1.0f);
        const ImVec2 c = centreOf(i);
        dl->AddCircleFilled(c, DOT, thisOne && part == Part::Move ? hot : line);
        const ImVec2 top((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f);
        const ImVec2 k = knobOf(i);
        dl->AddLine(top, k, line, 1.0f);
        dl->AddCircleFilled(k, KNOB_RADIUS, thisOne && part == Part::Rotate ? hot : line);
        const ImVec2 s = scaleHandleOf(i);
        dl->AddRectFilled(ImVec2(s.x - SQUARE, s.y - SQUARE), ImVec2(s.x + SQUARE, s.y + SQUARE), thisOne && part == Part::Scale ? hot : line);
    }
}
