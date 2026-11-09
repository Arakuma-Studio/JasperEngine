// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_sprite's paint family (docs/specs/S47-agent-sprite-drawing.md R16-R20): shapes, paste,
// fills, dither, clear, outline/inline and shade.

#include "SpriteEditInternal.h"

#include "DrawPrimitives.h"
#include "GridCodec.h"
#include "ShadeColor.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor::pixelart::draw::detail {

    namespace {

        const Json kNull;

        // the colour an op paints with (required)
        Ink opColor(EditState& s, const Json& op, const char* field = "color") {
            if (!op.contains(field)) {
                fail("bad_op_field", field, op["op"].get<std::string>() + " needs " + field,
                     std::string(field) + " is a palette index, \"#rrggbb\", \"#rrggbbaa\" or \"transparent\"");
            }
            return s.color(op[field], field);
        }

        // the area an op without points works in: its rect, else the selection's bounds, else the canvas
        IntRect opArea(EditState& s, const Json& op) {
            const IntRect canvas{0, 0, s.doc.width, s.doc.height};
            if (std::optional<IntRect> r = rectField(op, "rect")) return r->intersect(canvas);
            if (s.hasSelection && !s.ignoreSelection) return s.selectionBounds();
            return canvas;
        }

        void paintPixels(EditState& s, const Json& op, const std::vector<IntPoint>& pixels, const Ink& ink) {
            const std::vector<IntPoint> all = s.withMirrors(pixels);
            const int layer = s.pixelLayer(op);
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                for (const IntPoint& p : all) s.put(c, p.x, p.y, ink);
            }
        }

        void shapeOp(EditState& s, const Json& op, const std::string& name) {
            const Ink ink = opColor(s, op);
            std::vector<IntPoint> pixels;
            const int thickness = intField(op, "thickness", 1, 1, 16);
            const bool fill = boolField(op, "fill", false);
            if (name == "pixel") {
                pixels = pointsField(op, "points", 1);
            } else if (name == "line") {
                const std::vector<IntPoint> points = pointsField(op, "points", 2);
                const bool perfect = boolField(op, "pixel_perfect", true) && thickness == 1;
                pixels = stamp(polyline(points, perfect), thickness);
            } else if (name == "rect") {
                if (!op.contains("rect")) fail("bad_op_field", "rect", "rect needs rect: [x, y, w, h]", "");
                const IntRect r = rectValue(op["rect"], "rect");
                const int radius = intField(op, "radius", 0, 0, 64);
                pixels = fill ? roundedRectFilled(r, radius) : roundedRectOutline(r, radius, thickness);
            } else if (name == "ellipse") {
                if (!op.contains("rect")) fail("bad_op_field", "rect", "ellipse needs rect: [x, y, w, h] (the box it fits)", "");
                const IntRect r = rectValue(op["rect"], "rect");
                pixels = fill ? ellipseFilled(r) : ellipseRing(r, thickness);
            } else {
                const std::vector<IntPoint> points = pointsField(op, "points", 3);
                pixels = fill ? polygonFilled(points, {0, 0, s.doc.width, s.doc.height}) : polygonEdges(points);
            }
            paintPixels(s, op, pixels, ink);
        }

        void pasteOp(EditState& s, const Json& op) {
            if (!op.contains("rows")) fail("bad_op_field", "rows", "paste needs rows: [\"...\", ...] (top first)", "");
            const std::vector<std::string> rows = op["rows"].get<std::vector<std::string>>();
            GridEncoding encoding = GridEncoding::Auto;
            parseGridEncoding(op.value("encoding", std::string("auto")), encoding);
            PixelPatch patch;
            PixelProblem problem;
            if (!decodeRows(rows, encoding, op.contains("legend") ? op["legend"] : kNull, s.indexed(), patch, problem)) throw OpError{problem};
            const IntPoint at = op.contains("at") ? pointValue(op["at"], "at") : IntPoint{0, 0};
            const bool over = enumField(op, "blend", "replace", {"replace", "over"}) == "over";
            const int layer = s.pixelLayer(op);
            // the patch's inks, resolved once
            std::vector<Ink> inks(patch.keep.size());
            std::vector<uint8_t> skip(patch.keep.size(), 0);
            for (size_t i = 0; i < patch.keep.size(); i++) {
                if (patch.keep[i]) {
                    skip[i] = 1;
                    continue;
                }
                const std::string field = "rows[" + std::to_string(i / patch.w) + "]";
                if (patch.indexed) {
                    const int index = patch.index[i];
                    if (index >= static_cast<int>(s.doc.palette.size()) && !(index == 0)) {
                        fail("bad_color", field, field + " column " + std::to_string(i % patch.w) + " is index " + std::to_string(index) + ", not in the palette",
                             "palette has indices 0-" + std::to_string(static_cast<int>(s.doc.palette.size()) - 1));
                    }
                    if (over && index == 0) skip[i] = 1;
                    if (s.indexed()) inks[i] = s.inkOfIndex(index);
                    else inks[i] = index == 0 ? Ink{} : s.inkOfRgba(s.doc.palette[static_cast<size_t>(index)].color, field);
                } else {
                    if (over && patch.rgba[i][3] == 0) skip[i] = 1;
                    inks[i] = s.inkOfRgba(patch.rgba[i], field);
                }
            }
            std::vector<std::pair<IntPoint, size_t>> cells;
            for (int y = 0; y < patch.h; y++) {
                for (int x = 0; x < patch.w; x++) {
                    const size_t i = static_cast<size_t>(y) * patch.w + x;
                    if (!skip[i]) cells.push_back({{at.x + x, at.y + y}, i});
                }
            }
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                for (const auto& [p, i] : cells) {
                    s.put(c, p.x, p.y, inks[i]);
                    if (s.symmetric()) {
                        for (const IntPoint& m : s.mirrorsOf(p)) s.put(c, m.x, m.y, inks[i]);
                    }
                }
            }
        }

        void fillOp(EditState& s, const Json& op) {
            if (!op.contains("at")) fail("bad_op_field", "at", "fill needs at: [x, y] (the seed)", "");
            const IntPoint seed = pointValue(op["at"], "at");
            const Ink ink = opColor(s, op);
            const int tolerance = intField(op, "tolerance", 0, 0, 255);
            const bool contiguous = boolField(op, "contiguous", true);
            const int connectivity = intField(op, "connectivity", 4, 4, 8);
            const bool composite = enumField(op, "sample", "layer", {"layer", "composite"}) == "composite";
            const int layer = s.pixelLayer(op);
            std::vector<IntPoint> seeds{seed};
            for (const IntPoint& m : s.mirrorsOf(seed)) seeds.push_back(m);
            dedupe(seeds);
            const int w = s.doc.width, h = s.doc.height;
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                for (const IntPoint& from : seeds) {
                    if (from.x < 0 || from.y < 0 || from.x >= w || from.y >= h) continue;
                    // decided on the layer, or on what the frame shows (Pixelorama's _fill_merged_area)
                    const PixelImage sample = composite ? s.composite(frame, s.indexed()) : c.current;
                    const bool sampleIndexed = sample.hasIndices();
                    const Ink target = s.inkAt(sample, from.x, from.y);
                    auto similar = [&](int x, int y) {
                        const Ink here = s.inkAt(sample, x, y);
                        if (!sampleIndexed && tolerance == 0) return here.rgba == target.rgba || (here.rgba[3] == 0 && target.rgba[3] == 0);
                        return s.similar(here, target, tolerance);
                    };
                    std::vector<uint8_t> area;
                    if (contiguous) {
                        area = floodFill(w, h, from, connectivity == 8, [&](int x, int y) { return similar(x, y) && s.writable(x, y); });
                    } else {
                        // Pixelorama's COLORS mode: every similar pixel
                        area.assign(static_cast<size_t>(w) * h, 0);
                        for (int y = 0; y < h; y++) {
                            for (int x = 0; x < w; x++) area[static_cast<size_t>(y) * w + x] = similar(x, y) ? 1 : 0;
                        }
                    }
                    Canvas& target2 = s.canvas(layer, frame);
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            if (area[static_cast<size_t>(y) * w + x]) s.put(target2, x, y, ink);
                        }
                    }
                }
            }
        }

        void replaceOp(EditState& s, const Json& op) {
            if (!op.contains("from") || !op.contains("to")) fail("bad_op_field", op.contains("from") ? "to" : "from", "replace needs from and to colours", "");
            const Ink from = s.color(op["from"], "from");
            const Ink to = s.color(op["to"], "to");
            const int tolerance = intField(op, "tolerance", 0, 0, 255);
            const IntRect area = opArea(s, op);
            const int layer = s.pixelLayer(op);
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const PixelImage before = c.current;
                for (int y = area.y; y < area.y + area.h; y++) {
                    for (int x = area.x; x < area.x + area.w; x++) {
                        if (s.similar(s.inkAt(before, x, y), from, tolerance)) s.put(c, x, y, to);
                    }
                }
            }
        }

        void ditherOp(EditState& s, const Json& op) {
            if (!op.contains("colors") || !op["colors"].is_array() || op["colors"].size() != 2) {
                fail("bad_op_field", "colors", "dither needs colors: [a, b] (two colours)", "colors is [a, b]: b shows where the level is above the matrix");
            }
            const Ink a = s.color(op["colors"][0], "colors[0]");
            const Ink b = s.color(op["colors"][1], "colors[1]");
            const std::string pattern = enumField(op, "pattern", "bayer4", {"bayer2", "bayer4", "bayer8", "bayer16", "checker"});
            if (op.contains("level") == op.contains("gradient")) {
                fail("bad_op_field", "level", "dither needs one of level (0-1) or gradient {from, to, kind}", "give level: 0.5, or gradient: {\"from\": [0, 0], \"to\": [15, 0]}");
            }
            const double level = numberField(op, "level", 0.5, 0.0, 1.0);
            bool radial = false;
            IntPoint g0, g1;
            if (op.contains("gradient")) {
                const Json& g = op["gradient"];
                for (auto it = g.begin(); it != g.end(); ++it) {
                    if (it.key() != "from" && it.key() != "to" && it.key() != "kind") {
                        fail("bad_op_field", "gradient." + it.key(), "gradient has no field \"" + it.key() + "\"", "gradient takes: from, to, kind");
                    }
                }
                if (!g.contains("from") || !g.contains("to")) fail("bad_op_field", "gradient", "gradient needs from and to: [x, y]", "");
                g0 = pointValue(g["from"], "gradient.from");
                g1 = pointValue(g["to"], "gradient.to");
                radial = enumField(g, "kind", "linear", {"linear", "radial"}) == "radial";
            }
            int n = 2;
            std::vector<int> matrix;
            if (pattern == "checker") {
                matrix = {0, 2, 2, 0};
            } else {
                n = std::stoi(pattern.substr(5));
                matrix = bayerMatrix(n);
            }
            const IntRect area = opArea(s, op);
            const int layer = s.pixelLayer(op);
            const double dx = g1.x - g0.x, dy = g1.y - g0.y, len2 = dx * dx + dy * dy;
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                for (int y = area.y; y < area.y + area.h; y++) {
                    for (int x = area.x; x < area.x + area.w; x++) {
                        double l = level;
                        if (op.contains("gradient")) {
                            if (len2 <= 0) l = 0;
                            else if (radial) l = std::sqrt(((x - g0.x) * (x - g0.x) + (y - g0.y) * (y - g0.y)) / len2);
                            else l = ((x - g0.x) * dx + (y - g0.y) * dy) / len2;
                            l = std::clamp(l, 0.0, 1.0);
                        }
                        // canvas coordinates, so the pattern lines up across ops and frames
                        const int m = matrix[static_cast<size_t>(x % n) * n + (y % n)];
                        const double threshold = (m + 0.5) / (n * n);
                        s.put(c, x, y, l > threshold ? b : a);
                    }
                }
            }
        }

        void clearOp(EditState& s, const Json& op) {
            const IntRect area = opArea(s, op);
            const int layer = s.pixelLayer(op);
            const Ink clear = s.clearInk(layer);
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                for (int y = area.y; y < area.y + area.h; y++) {
                    for (int x = area.x; x < area.x + area.w; x++) s.put(c, x, y, clear);
                }
            }
        }

        void outlineOp(EditState& s, const Json& op, bool inside) {
            const Ink ink = opColor(s, op);
            const int width = intField(op, "width", 1, 1, 10);
            const std::string shape = enumField(op, "shape", "diamond", {"diamond", "circle", "square"});
            const std::vector<IntPoint> around = neighbourhood(width, shape == "circle" ? NeighbourShape::Circle :
                                                                         shape == "square" ? NeighbourShape::Square : NeighbourShape::Diamond);
            const int layer = s.pixelLayer(op);
            const int w = s.doc.width, h = s.doc.height;
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const PixelImage before = c.current;
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const bool clear = s.clearAt(before, x, y);
                        // outline: a clear pixel near the shape; inline: a shape pixel near clear (or the edge)
                        if (clear == inside) continue;
                        bool hit = false;
                        for (const IntPoint& d : around) {
                            const int nx = x + d.x, ny = y + d.y;
                            const bool outside = nx < 0 || ny < 0 || nx >= w || ny >= h;
                            const bool neighbourClear = outside || s.clearAt(before, nx, ny);
                            if (inside ? neighbourClear : !neighbourClear) {
                                hit = true;
                                break;
                            }
                        }
                        if (hit) s.put(c, x, y, ink);
                    }
                }
            }
        }

        void shadeOp(EditState& s, const Json& op) {
            const std::string mode = enumField(op, "mode", "simple", {"simple", "hue_shift", "ramp"});
            const bool lighten = enumField(op, "direction", "lighten", {"lighten", "darken"}) == "lighten";
            const bool add = enumField(op, "palette_mode", "snap", {"snap", "add"}) == "add";
            ShadeParams params;
            params.hueShift = mode == "hue_shift";
            params.lighten = lighten;
            params.amount = numberField(op, "amount", 10, 0, 100);
            params.hue = numberField(op, "hue", 10, -180, 180);
            params.saturation = numberField(op, "saturation", 10, -100, 100);
            params.value = numberField(op, "value", 10, -100, 100);
            // the ramp: palette indices, or an S43 ramp's colours
            std::vector<Ink> ramp;
            if (mode == "ramp") {
                if (op.contains("ramp") == op.contains("ramp_name")) {
                    fail("bad_op_field", "ramp", "shade mode ramp needs one of ramp ([indices], dark to light) or ramp_name", "give ramp: [2, 3, 4]");
                }
                if (op.contains("ramp")) {
                    for (size_t i = 0; i < op["ramp"].size(); i++) ramp.push_back(s.color(op["ramp"][i], "ramp[" + std::to_string(i) + "]"));
                } else {
                    const std::string name = op["ramp_name"].get<std::string>();
                    std::string known;
                    const std::vector<PixelRgba> colors = s.env.findRamp ? s.env.findRamp(name, known) : std::vector<PixelRgba>();
                    if (colors.empty()) {
                        fail("bad_op_field", "ramp_name", "no palette ramp named \"" + name + "\"",
                             known.empty() ? std::string("the project's palettes have no ramps; give ramp: [indices]") : "ramps: " + known);
                    }
                    for (const PixelRgba& c : colors) ramp.push_back(s.inkOfRgba(c, "ramp_name"));
                }
            } else if (op.contains("ramp") || op.contains("ramp_name")) {
                fail("bad_op_field", "ramp", "ramp and ramp_name go with mode \"ramp\"", "add mode: \"ramp\"");
            }
            const int layer = s.pixelLayer(op);
            // the pixels: points (mirrored), a rect, a colour, else the area (rect/selection/canvas)
            std::vector<IntPoint> points;
            if (op.contains("points")) points = s.withMirrors(pointsField(op, "points", 1));
            std::optional<Ink> match;
            if (op.contains("match")) match = s.color(op["match"], "match");
            const IntRect area = opArea(s, op);
            if (!op.contains("points")) {
                for (int y = area.y; y < area.y + area.h; y++) {
                    for (int x = area.x; x < area.x + area.w; x++) points.push_back({x, y});
                }
            }
            // indexed sprites snap the new colours to the palette (or add them)
            std::vector<jasperengine::pixelart::Rgba8> colors;
            for (const ase::PaletteEntry& e : s.doc.palette) colors.push_back({e.color[0], e.color[1], e.color[2], e.color[3]});
            if (!colors.empty() && s.indexed()) colors[std::min<size_t>(s.doc.transparentIndex, colors.size() - 1)].a = 0;
            std::optional<jasperengine::pixelart::PaletteMatcher> matcher;
            auto resolve = [&](const PixelRgba& rgba) -> Ink {
                if (!s.indexed()) return s.inkOfRgba(rgba, "shade");
                for (size_t i = 0; i < s.doc.palette.size(); i++) {
                    if (i != s.doc.transparentIndex && s.doc.palette[i].color == rgba) return s.inkOfIndex(static_cast<int>(i));
                }
                if (add) {
                    if (s.doc.palette.size() >= 256) {
                        fail("palette_full", "palette_mode", "the palette has 256 colours: shade can't add " + hexColor(rgba),
                             "use palette_mode \"snap\", or free palette entries first");
                    }
                    s.doc.palette.push_back({rgba, std::nullopt});
                    colors.push_back({rgba[0], rgba[1], rgba[2], rgba[3]});
                    matcher.reset();
                    s.markStructure();
                    return s.inkOfIndex(static_cast<int>(s.doc.palette.size()) - 1);
                }
                if (!matcher) matcher.emplace(colors, jasperengine::pixelart::ColorMetric::OkLab);
                return s.inkOfIndex(matcher->nearest({rgba[0], rgba[1], rgba[2], rgba[3]}));
            };
            for (int frame : s.frames(op)) {
                Canvas& c = s.canvas(layer, frame);
                const PixelImage before = c.current;
                for (const IntPoint& p : points) {
                    if (p.x < 0 || p.y < 0 || p.x >= s.doc.width || p.y >= s.doc.height) continue;
                    if (s.clearAt(before, p.x, p.y)) continue;   // transparent pixels never change
                    const Ink here = s.inkAt(before, p.x, p.y);
                    if (match && !s.sameInk(here, *match)) continue;
                    if (mode == "ramp") {
                        for (size_t k = 0; k < ramp.size(); k++) {
                            if (!s.sameInk(here, ramp[k])) continue;
                            if (lighten && k + 1 < ramp.size()) s.put(c, p.x, p.y, ramp[k + 1]);
                            if (!lighten && k > 0) s.put(c, p.x, p.y, ramp[k - 1]);
                            break;
                        }
                        continue;
                    }
                    const PixelRgba shaded = shadeColor(here.rgba, params);
                    if (shaded == here.rgba) continue;
                    s.put(c, p.x, p.y, resolve(shaded));
                }
            }
        }

    }

    void runPaintOp(EditState& state, const Json& op, const std::string& name) {
        if (name == "pixel" || name == "line" || name == "rect" || name == "ellipse" || name == "polygon") shapeOp(state, op, name);
        else if (name == "paste") pasteOp(state, op);
        else if (name == "fill") fillOp(state, op);
        else if (name == "replace") replaceOp(state, op);
        else if (name == "dither") ditherOp(state, op);
        else if (name == "clear") clearOp(state, op);
        else if (name == "outline") outlineOp(state, op, false);
        else if (name == "inline") outlineOp(state, op, true);
        else if (name == "shade") shadeOp(state, op);
    }

}
