// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_sprite's transform family (docs/specs/S47-agent-sprite-drawing.md R14, R21-R23): shift, flip,
// rotate, scale, mirror, copy and select. A transform takes the pixels of its target (the rect,
// else the selection's bounds, else the whole canvas) that the selection allows, clears them, and
// places what it made of them; an unselected pixel stays unless something lands on it.

#include "SpriteEditInternal.h"

#include "DrawPrimitives.h"
#include "SpriteRotate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::editor::pixelart::draw::detail {

    namespace {

        IntRect canvasRect(const EditState& s) { return {0, 0, s.doc.width, s.doc.height}; }

        bool selectionApplies(const EditState& s) { return s.hasSelection && !s.ignoreSelection; }

        IntRect targetRect(EditState& s, const Json& op) {
            if (std::optional<IntRect> r = rectField(op, "rect")) return r->intersect(canvasRect(s));
            if (selectionApplies(s)) return s.selectionBounds();
            return canvasRect(s);
        }

        uint64_t countDifferences(const PixelImage& a, const PixelImage& b) {
            uint64_t count = 0;
            const bool indices = a.hasIndices() && b.hasIndices();
            for (int y = 0; y < a.height(); y++) {
                for (int x = 0; x < a.width(); x++) {
                    if (indices ? a.indexAt(x, y) != b.indexAt(x, y) : std::memcmp(a.row(y) + x * 4, b.row(y) + x * 4, 4) != 0) count++;
                }
            }
            return count;
        }

        // One target of a transform on one frame: what it may take, as a crop
        struct Taken {
            IntRect rect;
            PixelImage pixels;              // the crop of the canvas
            std::vector<uint8_t> mask;      // 1: taken
            std::vector<uint8_t> rgba;      // RGBA with what was not taken cleared (what the rotations see)
        };

        Taken takeTarget(EditState& s, const Canvas& c, const IntRect& rect) {
            Taken t;
            t.rect = rect;
            t.pixels = c.current.crop(rect);
            t.mask.assign(static_cast<size_t>(rect.w) * rect.h, 1);
            const std::span<const uint8_t> src = t.pixels.pixels();
            t.rgba.assign(src.begin(), src.end());
            if (selectionApplies(s)) {
                for (int y = 0; y < rect.h; y++) {
                    for (int x = 0; x < rect.w; x++) {
                        if (s.selected(rect.x + x, rect.y + y)) continue;
                        const size_t i = static_cast<size_t>(y) * rect.w + x;
                        t.mask[i] = 0;
                        std::memset(t.rgba.data() + i * 4, 0, 4);
                    }
                }
            }
            return t;
        }

        // Clears what was taken and writes the output map (`outW` x `outH` at (outX, outY) on the
        // canvas; entries index the crop, -1 for nothing). A clear source pixel writes nothing.
        void placeMap(EditState& s, Canvas& c, const Taken& t, const std::vector<int32_t>& map, int outX, int outY, int outW, int outH) {
            const uint64_t changesBefore = s.opChanges;
            const PixelImage snapshot = c.current;
            const Ink clear = s.clearInk(c.layer);
            for (int y = 0; y < t.rect.h; y++) {
                for (int x = 0; x < t.rect.w; x++) {
                    if (t.mask[static_cast<size_t>(y) * t.rect.w + x]) s.putRaw(c, t.rect.x + x, t.rect.y + y, clear);
                }
            }
            for (int y = 0; y < outH; y++) {
                for (int x = 0; x < outW; x++) {
                    const int32_t source = map[static_cast<size_t>(y) * outW + x];
                    if (source < 0 || !t.mask[static_cast<size_t>(source)]) continue;
                    const int sx = source % t.rect.w, sy = source / t.rect.w;
                    if (s.clearAt(t.pixels, sx, sy)) continue;
                    s.putRaw(c, outX + x, outY + y, s.inkAt(t.pixels, sx, sy));
                }
            }
            // a pixel cleared and written back is no change: nothing_drawn looks at the result
            s.opChanges = changesBefore + countDifferences(snapshot, c.current);
        }

        void shiftOp(EditState& s, const Json& op) {
            if (!op.contains("by")) fail("bad_op_field", "by", "shift needs by: [dx, dy]", "");
            const IntPoint by = pointValue(op["by"], "by");
            const bool wrap = boolField(op, "wrap", false);
            const IntRect rect = targetRect(s, op);
            if (rect.empty()) return;
            const int layer = s.pixelLayer(op);
            std::vector<int32_t> map(static_cast<size_t>(rect.w) * rect.h);
            for (int y = 0; y < rect.h; y++) {
                for (int x = 0; x < rect.w; x++) {
                    int sx = x, sy = y;
                    if (wrap) {
                        sx = ((x - by.x) % rect.w + rect.w) % rect.w;
                        sy = ((y - by.y) % rect.h + rect.h) % rect.h;
                    }
                    map[static_cast<size_t>(y) * rect.w + x] = sy * rect.w + sx;
                }
            }
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const Taken t = takeTarget(s, c, rect);
                if (wrap) placeMap(s, c, t, map, rect.x, rect.y, rect.w, rect.h);
                else placeMap(s, c, t, map, rect.x + by.x, rect.y + by.y, rect.w, rect.h);
            }
        }

        void flipOp(EditState& s, const Json& op) {
            const std::string axis = enumField(op, "axis", "", {"x", "y"});
            if (axis.empty()) fail("bad_op_field", "axis", "flip needs axis: \"x\" or \"y\"", "axis x mirrors left and right");
            const IntRect rect = targetRect(s, op);
            if (rect.empty()) return;
            const int layer = s.pixelLayer(op);
            std::vector<int32_t> map(static_cast<size_t>(rect.w) * rect.h);
            for (int y = 0; y < rect.h; y++) {
                for (int x = 0; x < rect.w; x++) {
                    const int sx = axis == "x" ? rect.w - 1 - x : x, sy = axis == "y" ? rect.h - 1 - y : y;
                    map[static_cast<size_t>(y) * rect.w + x] = sy * rect.w + sx;
                }
            }
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                placeMap(s, c, takeTarget(s, c, rect), map, rect.x, rect.y, rect.w, rect.h);
            }
        }

        void rotateOp(EditState& s, const Json& op) {
            if (!op.contains("angle")) fail("bad_op_field", "angle", "rotate needs angle (degrees, clockwise, -360 to 360)", "");
            const double angle = numberField(op, "angle", 0, -360, 360);
            const std::string name = enumField(op, "method", "auto", {"auto", "cleanedge", "rotxel", "omniscale", "nearest"});
            const RotateMethod method = name == "rotxel" ? RotateMethod::Rotxel : name == "omniscale" ? RotateMethod::OmniScale :
                                        name == "nearest" ? RotateMethod::Nearest : RotateMethod::CleanEdge;
            const IntRect rect = targetRect(s, op);
            if (rect.empty()) return;
            double px = (rect.w - 1) / 2.0, py = (rect.h - 1) / 2.0;
            if (op.contains("pivot")) {
                const Json& p = op["pivot"];
                if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
                    fail("bad_op_field", "pivot", "pivot must be [x, y] (pixels, halves allowed)", "");
                }
                px = p[0].get<double>() - rect.x;
                py = p[1].get<double>() - rect.y;
            }
            const int layer = s.pixelLayer(op);
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const Taken t = takeTarget(s, c, rect);
                const std::vector<int32_t> map = rotateMap(rect.w, rect.h, t.rgba, angle, px, py, method);
                placeMap(s, c, t, map, rect.x, rect.y, rect.w, rect.h);
            }
        }

        void scaleOp(EditState& s, const Json& op) {
            if (!op.contains("factor")) fail("bad_op_field", "factor", "scale needs factor: 2, 3 or 4", "");
            const int factor = intField(op, "factor", 2, 2, 4);
            const int tolerance = intField(op, "tolerance", 0, 0, 255);
            const std::string method = enumField(op, "method", factor == 3 ? "scale3x" : "scale2x", {"nearest", "scale2x", "scale3x"});
            if ((method == "scale2x" && factor == 3) || (method == "scale3x" && factor != 3)) {
                fail("bad_op_field", "method", method + " does not make factor " + std::to_string(factor),
                     "scale2x is factor 2 (4 runs it twice), scale3x is factor 3, nearest takes 2-4");
            }
            const IntRect rect = targetRect(s, op);
            if (rect.empty()) return;
            const int layer = s.pixelLayer(op);
            const int outW = rect.w * factor, outH = rect.h * factor;
            // centred on the target (the lower-right pixel wins half a pixel), clipped by the canvas
            const int outX = rect.x + static_cast<int>(std::floor((rect.w - outW) / 2.0));
            const int outY = rect.y + static_cast<int>(std::floor((rect.h - outH) / 2.0));
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const Taken t = takeTarget(s, c, rect);
                std::vector<int32_t> map;
                if (method == "nearest") {
                    map.resize(static_cast<size_t>(outW) * outH);
                    for (int y = 0; y < outH; y++) {
                        for (int x = 0; x < outW; x++) map[static_cast<size_t>(y) * outW + x] = (y / factor) * rect.w + x / factor;
                    }
                } else {
                    const bool useIndex = s.indexed() && tolerance == 0 && t.pixels.hasIndices();
                    map = scaleMap(rect.w, rect.h, factor, [&](int32_t a, int32_t b) {
                        const size_t ia = static_cast<size_t>(a), ib = static_cast<size_t>(b);
                        if (!t.mask[ia] || !t.mask[ib]) return t.mask[ia] == t.mask[ib];
                        if (useIndex) return t.pixels.indices()[ia] == t.pixels.indices()[ib];
                        for (int k = 0; k < 4; k++) {
                            if (std::abs(int(t.rgba[ia * 4 + k]) - int(t.rgba[ib * 4 + k])) > tolerance) return false;
                        }
                        return true;
                    });
                }
                placeMap(s, c, t, map, outX, outY, outW, outH);
            }
        }

        void mirrorOp(EditState& s, const Json& op) {
            const std::string axis = enumField(op, "axis", "", {"x", "y"});
            if (axis.empty()) fail("bad_op_field", "axis", "mirror needs axis: \"x\" or \"y\"", "");
            const std::string side = enumField(op, "from_side", axis == "x" ? "left" : "top", {"left", "right", "top", "bottom"});
            if ((axis == "x") != (side == "left" || side == "right")) {
                fail("bad_op_field", "from_side", "axis " + axis + " copies from " + (axis == "x" ? "left or right" : "top or bottom"), "");
            }
            const IntRect rect = targetRect(s, op);
            if (rect.empty()) return;
            const double defaultAxis = axis == "x" ? rect.x + (rect.w - 1) / 2.0 : rect.y + (rect.h - 1) / 2.0;
            const int twice = static_cast<int>(std::floor(numberField(op, "at", defaultAxis, -1, 8192) * 2 + 0.5));
            const int layer = s.pixelLayer(op);
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const PixelImage before = c.current;
                for (int y = rect.y; y < rect.y + rect.h; y++) {
                    for (int x = rect.x; x < rect.x + rect.w; x++) {
                        const int along = axis == "x" ? x : y;
                        const bool fromLow = side == "left" || side == "top";
                        if (fromLow ? along * 2 <= twice : along * 2 >= twice) continue;   // the side copied from, and the axis
                        const int mx = axis == "x" ? twice - x : x, my = axis == "y" ? twice - y : y;
                        if (!rect.contains(mx, my)) continue;
                        s.put(c, x, y, s.inkAt(before, mx, my));
                    }
                }
            }
        }

        void copyOp(EditState& s, const Json& op) {
            static const Json kNoSource = Json::object();
            const Json& source = op.contains("source") ? op["source"] : kNoSource;
            for (auto it = source.begin(); it != source.end(); ++it) {
                if (it.key() != "frame" && it.key() != "layer" && it.key() != "rect") {
                    fail("bad_op_field", "source." + it.key(), "source has no field \"" + it.key() + "\"", "source takes: frame, layer, rect");
                }
            }
            const std::vector<int> opFrames = s.frames(op);
            const int fromFrame = source.contains("frame") ? s.parseFrame(source["frame"], "source.frame") : opFrames[0];
            const int fromLayer = source.contains("layer") ? s.parseLayer(source["layer"], "source.layer", true) : s.pixelLayer(op);
            const bool useSelection = !source.contains("rect") && selectionApplies(s);
            const IntRect rect = source.contains("rect") ? rectValue(source["rect"], "source.rect").intersect(canvasRect(s))
                                                         : useSelection ? s.selectionBounds() : canvasRect(s);
            if (rect.empty()) return;
            const IntPoint at = op.contains("at") ? pointValue(op["at"], "at") : IntPoint{rect.x, rect.y};
            const int toLayer = op.contains("to_layer") ? s.parseLayer(op["to_layer"], "to_layer", true) : s.pixelLayer(op);
            const std::vector<int> toFrames = op.contains("to_frame") ? s.parseFrames(op["to_frame"], "to_frame") : opFrames;
            const bool over = enumField(op, "blend", "replace", {"replace", "over"}) == "over";
            const bool cut = boolField(op, "cut", false);
            Canvas& from = s.canvas(fromLayer, fromFrame);
            const PixelImage taken = from.current.crop(rect);
            auto isTaken = [&](int x, int y) { return !useSelection || s.selected(rect.x + x, rect.y + y); };
            if (cut) {
                const Ink clear = s.clearInk(fromLayer);
                for (int y = 0; y < rect.h; y++) {
                    for (int x = 0; x < rect.w; x++) {
                        if (isTaken(x, y)) s.putRaw(from, rect.x + x, rect.y + y, clear);
                    }
                }
            }
            for (int frame : toFrames) {
                Canvas& to = s.canvas(toLayer, frame);
                for (int y = 0; y < rect.h; y++) {
                    for (int x = 0; x < rect.w; x++) {
                        if (!isTaken(x, y)) continue;
                        if (over && s.clearAt(taken, x, y)) continue;
                        // the selection chose what was taken: the destination is the op's own
                        s.putRaw(to, at.x + x, at.y + y, s.inkAt(taken, x, y));
                    }
                }
            }
        }

        void selectOp(EditState& s, const Json& op) {
            const std::string shape = enumField(op, "shape", "", {"rect", "ellipse", "polygon", "color", "all", "none", "invert"});
            if (shape.empty()) fail("bad_op_field", "shape", "select needs shape", "shape is one of rect, ellipse, polygon, color, all, none, invert");
            const std::string mode = enumField(op, "select_mode", "replace", {"replace", "add", "subtract", "intersect"});
            const int w = s.doc.width, h = s.doc.height;
            const size_t count = static_cast<size_t>(w) * h;
            if (shape == "none") {
                s.hasSelection = false;
                s.selection.clear();
                return;
            }
            std::vector<uint8_t> mask(count, 0);
            auto mark = [&](const std::vector<IntPoint>& points) {
                for (const IntPoint& p : points) {
                    if (p.x >= 0 && p.y >= 0 && p.x < w && p.y < h) mask[static_cast<size_t>(p.y) * w + p.x] = 1;
                }
            };
            if (shape == "rect" || shape == "ellipse") {
                if (!op.contains("rect")) fail("bad_op_field", "rect", "select " + shape + " needs rect: [x, y, w, h]", "");
                const IntRect r = rectValue(op["rect"], "rect");
                if (shape == "ellipse") {
                    mark(ellipseFilled(r));
                } else {
                    const IntRect in = r.intersect(canvasRect(s));
                    for (int y = in.y; y < in.y + in.h; y++) {
                        for (int x = in.x; x < in.x + in.w; x++) mask[static_cast<size_t>(y) * w + x] = 1;
                    }
                }
            } else if (shape == "polygon") {
                mark(polygonFilled(pointsField(op, "points", 3), canvasRect(s)));
            } else if (shape == "color") {
                if (!op.contains("at")) fail("bad_op_field", "at", "select color needs at: [x, y] (the seed)", "");
                const IntPoint at = pointValue(op["at"], "at");
                const int tolerance = intField(op, "tolerance", 0, 0, 255);
                const bool contiguous = boolField(op, "contiguous", true);
                if (at.x >= 0 && at.y >= 0 && at.x < w && at.y < h) {
                    const int layer = s.pixelLayer(op);
                    const PixelImage image = s.canvas(layer, s.frames(op)[0]).current;
                    const Ink target = s.inkAt(image, at.x, at.y);
                    auto similar = [&](int x, int y) { return s.similar(s.inkAt(image, x, y), target, tolerance); };
                    if (contiguous) {
                        mask = floodFill(w, h, at, false, similar);
                    } else {
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) mask[static_cast<size_t>(y) * w + x] = similar(x, y) ? 1 : 0;
                        }
                    }
                }
            } else if (shape == "all") {
                std::fill(mask.begin(), mask.end(), uint8_t(1));
            } else {   // invert: no selection means everything, so inverting it leaves nothing
                if (s.hasSelection) {
                    for (size_t i = 0; i < count; i++) mask[i] = s.selection[i] ? 0 : 1;
                }
                s.selection = mask;
                s.hasSelection = true;
                return;
            }
            if (mode != "replace" && !s.hasSelection) {
                // no selection is "everything" for subtract and intersect, "nothing" for add
                s.selection.assign(count, mode == "add" ? 0 : 1);
            }
            if (mode == "replace") {
                s.selection = mask;
            } else {
                for (size_t i = 0; i < count; i++) {
                    if (mode == "add") s.selection[i] = s.selection[i] || mask[i];
                    else if (mode == "subtract") s.selection[i] = s.selection[i] && !mask[i];
                    else s.selection[i] = s.selection[i] && mask[i];
                }
            }
            s.hasSelection = true;
            if (s.selectionBounds().empty()) s.warn("nothing_selected", "ops[" + std::to_string(s.opIndex) + "] (select) selected no pixel: the pixel ops after it draw nothing");
        }

    }

    void runTransformOp(EditState& state, const Json& op, const std::string& name) {
        if (name == "shift") shiftOp(state, op);
        else if (name == "flip") flipOp(state, op);
        else if (name == "rotate") rotateOp(state, op);
        else if (name == "scale") scaleOp(state, op);
        else if (name == "mirror") mirrorOp(state, op);
        else if (name == "copy") copyOp(state, op);
        else if (name == "select") selectOp(state, op);
    }

}
