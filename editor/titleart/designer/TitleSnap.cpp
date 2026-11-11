// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleSnap.h"
#include "TitleGizmo2D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine::editor::titledesigner;

namespace {

    // a line a moving edge or centre can snap to, and the extent it is drawn over (along the
    // other axis)
    struct AxisTarget {
        double at = 0.0;
        double from = 0.0, to = 0.0;
    };

    // the targets along x (vertical lines) or y (horizontal lines)
    std::vector<AxisTarget> axisTargets(const SnapContext& context, bool alongX) {
        std::vector<AxisTarget> out;
        const auto addRect = [&](const TitleRect& r) {
            if (alongX) {
                for (double x : {r.x, r.x + r.width * 0.5, r.x + r.width}) out.push_back({x, r.y, r.y + r.height});
            } else {
                for (double y : {r.y, r.y + r.height * 0.5, r.y + r.height}) out.push_back({y, r.x, r.x + r.width});
            }
        };
        if (!context.canvas.empty()) addRect(context.canvas);
        for (const TitleRect& r : context.others) addRect(r);
        // a guide's line runs across the canvas (it is drawn there already; the match brightens it)
        const TitleRect& c = context.canvas;
        for (double g : alongX ? context.guidesX : context.guidesY) {
            out.push_back({g, alongX ? c.y : c.x, alongX ? c.y + c.height : c.x + c.width});
        }
        return out;
    }

    struct AxisSnap {
        double correction = 0.0;
        bool snapped = false;
    };

    // the smallest correction that puts one of `features` on a target within `threshold`; the
    // pixel grid only when no target is that close (a layer edge beats a whole pixel)
    AxisSnap snapAxis(const std::vector<double>& features, const std::vector<AxisTarget>& targets, bool grid, double threshold) {
        AxisSnap best;
        double bestDistance = threshold;
        for (double f : features) {
            for (const AxisTarget& t : targets) {
                const double d = t.at - f;
                if (std::abs(d) <= bestDistance && (!best.snapped || std::abs(d) < std::abs(best.correction))) {
                    best.correction = d;
                    best.snapped = true;
                    bestDistance = std::abs(d);
                }
            }
        }
        if (best.snapped || !grid) return best;
        for (double f : features) {
            const double d = std::round(f) - f;
            if (std::abs(d) <= bestDistance && (!best.snapped || std::abs(d) < std::abs(best.correction))) {
                best.correction = d;
                best.snapped = true;
                bestDistance = std::abs(d);
            }
        }
        return best;
    }

    // the lines the snapped features lie on, each spanning the moving extent and the target's
    void matchedLines(const std::vector<double>& features, double correction, const std::vector<AxisTarget>& targets, bool vertical,
                      double movingFrom, double movingTo, std::vector<SnapLine>& out) {
        constexpr double EPS = 1e-6;
        for (double f : features) {
            const double at = f + correction;
            for (const AxisTarget& t : targets) {
                if (std::abs(t.at - at) > EPS) continue;
                const double from = std::min(t.from, movingFrom), to = std::max(t.to, movingTo);
                auto same = std::find_if(out.begin(), out.end(), [&](const SnapLine& l) { return l.vertical == vertical && std::abs(l.at - t.at) <= EPS; });
                if (same != out.end()) {
                    same->from = std::min(same->from, from);
                    same->to = std::max(same->to, to);
                } else {
                    out.push_back({vertical, t.at, from, to});
                }
            }
        }
    }

    // the gaps from `box` to its nearest neighbour on each side that overlaps it across the gap
    void neighbourGaps(const TitleRect& box, const std::vector<TitleRect>& others, std::vector<SnapGap>& out) {
        const double bx0 = box.x, bx1 = box.x + box.width, by0 = box.y, by1 = box.y + box.height;
        const SnapGap none;
        SnapGap left = none, right = none, below = none, above = none;
        for (const TitleRect& o : others) {
            const double ox0 = o.x, ox1 = o.x + o.width, oy0 = o.y, oy1 = o.y + o.height;
            const double ylo = std::max(by0, oy0), yhi = std::min(by1, oy1);
            if (yhi > ylo) {
                const double y = (ylo + yhi) * 0.5;
                if (ox1 <= bx0 && (left.length == 0.0 || bx0 - ox1 < left.length)) left = {{ox1, y}, {bx0, y}, bx0 - ox1};
                if (ox0 >= bx1 && (right.length == 0.0 || ox0 - bx1 < right.length)) right = {{bx1, y}, {ox0, y}, ox0 - bx1};
            }
            const double xlo = std::max(bx0, ox0), xhi = std::min(bx1, ox1);
            if (xhi > xlo) {
                const double x = (xlo + xhi) * 0.5;
                if (oy1 <= by0 && (below.length == 0.0 || by0 - oy1 < below.length)) below = {{x, oy1}, {x, by0}, by0 - oy1};
                if (oy0 >= by1 && (above.length == 0.0 || oy0 - by1 < above.length)) above = {{x, by1}, {x, oy0}, oy0 - by1};
            }
        }
        for (const SnapGap& g : {left, right, below, above}) {
            if (g.length > 0.0) out.push_back(g);
        }
    }

    constexpr ImU32 SNAP_COLOR = IM_COL32(255, 61, 214, 255);

}

TitleRect jasperengine::editor::titledesigner::canvasRect(int width, int height) {
    return {-width * 0.5, -height * 0.5, (double)width, (double)height};
}

SnapResult jasperengine::editor::titledesigner::snapBox(const SnapContext& context, const TitleRect& box, TitleVec2 delta) {
    SnapResult result;
    result.delta = delta;
    if (!context.settings.enabled) return result;
    const double threshold = context.threshold();
    const TitleRect moved{box.x + delta.x, box.y + delta.y, box.width, box.height};
    const std::vector<double> fx = {moved.x, moved.x + moved.width * 0.5, moved.x + moved.width};
    const std::vector<double> fy = {moved.y, moved.y + moved.height * 0.5, moved.y + moved.height};
    const std::vector<AxisTarget> tx = axisTargets(context, true), ty = axisTargets(context, false);
    const AxisSnap sx = snapAxis(fx, tx, context.settings.pixelGrid, threshold);
    const AxisSnap sy = snapAxis(fy, ty, context.settings.pixelGrid, threshold);
    result.delta = {delta.x + sx.correction, delta.y + sy.correction};
    result.snappedX = sx.snapped;
    result.snappedY = sy.snapped;
    const TitleRect snapped{box.x + result.delta.x, box.y + result.delta.y, box.width, box.height};
    if (sx.snapped) matchedLines(fx, sx.correction, tx, true, snapped.y, snapped.y + snapped.height, result.lines);
    if (sy.snapped) matchedLines(fy, sy.correction, ty, false, snapped.x, snapped.x + snapped.width, result.lines);
    neighbourGaps(snapped, context.others, result.gaps);
    return result;
}

SnapResult jasperengine::editor::titledesigner::snapPoint(const SnapContext& context, TitleVec2 point, TitleVec2 delta) {
    SnapResult result;
    result.delta = delta;
    if (!context.settings.enabled) return result;
    const double threshold = context.threshold();
    const TitleVec2 moved{point.x + delta.x, point.y + delta.y};
    const std::vector<double> fx = {moved.x}, fy = {moved.y};
    const std::vector<AxisTarget> tx = axisTargets(context, true), ty = axisTargets(context, false);
    const AxisSnap sx = snapAxis(fx, tx, context.settings.pixelGrid, threshold);
    const AxisSnap sy = snapAxis(fy, ty, context.settings.pixelGrid, threshold);
    result.delta = {delta.x + sx.correction, delta.y + sy.correction};
    result.snappedX = sx.snapped;
    result.snappedY = sy.snapped;
    const TitleVec2 snapped{point.x + result.delta.x, point.y + result.delta.y};
    if (sx.snapped) matchedLines(fx, sx.correction, tx, true, snapped.y, snapped.y, result.lines);
    if (sy.snapped) matchedLines(fy, sy.correction, ty, false, snapped.x, snapped.x, result.lines);
    return result;
}

double jasperengine::editor::titledesigner::snapToIncrement(double value, double step) {
    return step > 0.0 ? std::round(value / step) * step : value;
}

TitleVec2 jasperengine::editor::titledesigner::incrementMove(TitleVec2 delta, const SnapSettings& settings) {
    return {snapToIncrement(delta.x, settings.moveIncrement), snapToIncrement(delta.y, settings.moveIncrement)};
}

double jasperengine::editor::titledesigner::incrementAngle(double degrees, const SnapSettings& settings) {
    return snapToIncrement(degrees, settings.rotateIncrement);
}

double jasperengine::editor::titledesigner::incrementScale(double scale, const SnapSettings& settings) {
    // never down to 0: one increment is the smallest scale a Ctrl drag gives
    const double v = snapToIncrement(scale, settings.scaleIncrement);
    return settings.scaleIncrement > 0.0 ? std::max(v, settings.scaleIncrement) : v;
}

std::string jasperengine::editor::titledesigner::formatGap(double length) {
    char text[32];
    if (std::abs(length - std::round(length)) < 0.05) {
        std::snprintf(text, sizeof(text), "%.0f px", std::round(length));
    } else {
        std::snprintf(text, sizeof(text), "%.1f px", length);
    }
    return text;
}

void jasperengine::editor::titledesigner::drawSnap(ImDrawList* drawList, const CanvasXform& view, const SnapResult& result) {
    if (!drawList) return;
    for (const SnapLine& l : result.lines) {
        const TitleVec2 a = l.vertical ? TitleVec2{l.at, l.from} : TitleVec2{l.from, l.at};
        const TitleVec2 b = l.vertical ? TitleVec2{l.at, l.to} : TitleVec2{l.to, l.at};
        ImVec2 sa = view.toScreen(a), sb = view.toScreen(b);
        // a point's match (zero extent) still shows: a short cross line
        if (std::abs(sa.x - sb.x) + std::abs(sa.y - sb.y) < 1.0f) {
            if (l.vertical) {
                sa.y -= 12.0f;
                sb.y += 12.0f;
            } else {
                sa.x -= 12.0f;
                sb.x += 12.0f;
            }
        }
        drawList->AddLine(sa, sb, overlay::HALO_COLOR, 3.0f);
        drawList->AddLine(sa, sb, SNAP_COLOR, 1.0f);
    }
    for (const SnapGap& g : result.gaps) {
        const ImVec2 a = view.toScreen(g.from), b = view.toScreen(g.to);
        drawList->AddLine(a, b, SNAP_COLOR, 1.0f);
        // end ticks across the gap
        const bool horizontal = std::abs(a.y - b.y) < std::abs(a.x - b.x);
        const ImVec2 t = horizontal ? ImVec2(0.0f, 4.0f) : ImVec2(4.0f, 0.0f);
        drawList->AddLine(ImVec2(a.x - t.x, a.y - t.y), ImVec2(a.x + t.x, a.y + t.y), SNAP_COLOR, 1.0f);
        drawList->AddLine(ImVec2(b.x - t.x, b.y - t.y), ImVec2(b.x + t.x, b.y + t.y), SNAP_COLOR, 1.0f);
        overlay::label(drawList, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), formatGap(g.length), SNAP_COLOR, true);
    }
}
