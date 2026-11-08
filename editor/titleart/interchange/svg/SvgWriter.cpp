// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SvgWriter.h"

#include "titleart/geometry/Shapes.h"
#include "titleart/geometry/TitleGeometry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::titleart;
using geometry::Affine;
using geometry::Path;
using geometry::PathVerb;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    std::string str(const Json& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    double num(const Json& object, const char* key, double fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool on(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_boolean() && it->get<bool>();
    }

    const Json& member(const Json& object, const char* key) {
        static const Json empty = Json::object();
        auto it = object.find(key);
        return it != object.end() ? *it : empty;
    }

    // a number in the file: at most 3 decimals, no trailing zeros, never "-0"
    std::string fmt(double v) {
        if (!std::isfinite(v)) v = 0.0;
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.3f", v);
        std::string s = buffer;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        if (s == "-0" || s.empty()) s = "0";
        return s;
    }

    std::string escape(const std::string& text) {
        std::string out;
        for (char c : text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    std::string matrixOf(const Affine& m) {
        return "matrix(" + fmt(m.a) + " " + fmt(m.b) + " " + fmt(m.c) + " " + fmt(m.d) + " " + fmt(m.e) + " " + fmt(m.f) + ")";
    }

    std::string pathData(const Path& path, const Affine& m) {
        std::string d;
        const std::vector<TitleVec2>& pts = path.pointList();
        size_t p = 0;
        auto point = [&](const TitleVec2& q) {
            const TitleVec2 t = m.apply(q);
            return fmt(t.x) + " " + fmt(t.y);
        };
        for (PathVerb verb : path.verbList()) {
            if (!d.empty()) d += ' ';
            switch (verb) {
                case PathVerb::Move: d += "M" + point(pts[p]); p += 1; break;
                case PathVerb::Line: d += "L" + point(pts[p]); p += 1; break;
                case PathVerb::Quad: d += "Q" + point(pts[p]) + " " + point(pts[p + 1]); p += 2; break;
                case PathVerb::Cubic: d += "C" + point(pts[p]) + " " + point(pts[p + 1]) + " " + point(pts[p + 2]); p += 3; break;
                case PathVerb::Close: d += "Z"; break;
            }
        }
        return d;
    }

    // the contours of a path, each its own Path
    std::vector<Path> contoursOf(const Path& path) {
        std::vector<Path> out;
        const std::vector<TitleVec2>& pts = path.pointList();
        size_t p = 0;
        for (PathVerb verb : path.verbList()) {
            switch (verb) {
                case PathVerb::Move:
                    out.emplace_back();
                    out.back().evenOdd = path.evenOdd;
                    out.back().moveTo(pts[p].x, pts[p].y);
                    p += 1;
                    break;
                case PathVerb::Line: if (!out.empty()) out.back().lineTo(pts[p].x, pts[p].y); p += 1; break;
                case PathVerb::Quad: if (!out.empty()) out.back().quadTo(pts[p].x, pts[p].y, pts[p + 1].x, pts[p + 1].y); p += 2; break;
                case PathVerb::Cubic:
                    if (!out.empty()) out.back().cubicTo(pts[p].x, pts[p].y, pts[p + 1].x, pts[p + 1].y, pts[p + 2].x, pts[p + 2].y);
                    p += 3;
                    break;
                case PathVerb::Close: if (!out.empty()) out.back().close(); break;
            }
        }
        return out;
    }

    // fill_counters keeps the outer contours: those not inside another contour of the outline
    Path outerContours(const Path& path) {
        const std::vector<Path> contours = contoursOf(path);
        std::vector<std::vector<TitleVec2>> polys;
        for (const Path& c : contours) {
            const auto flat = c.flatten(0.5);
            polys.push_back(flat.empty() ? std::vector<TitleVec2>() : flat.front());
        }
        const auto insidePoly = [](const std::vector<TitleVec2>& poly, const TitleVec2& q) {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                if (((poly[i].y > q.y) != (poly[j].y > q.y)) &&
                    q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) {
                    in = !in;
                }
            }
            return in;
        };
        Path out;
        for (size_t i = 0; i < contours.size(); i++) {
            if (polys[i].empty()) continue;
            bool nested = false;
            for (size_t j = 0; j < contours.size() && !nested; j++) {
                if (i != j && polys[j].size() > 2 && insidePoly(polys[j], polys[i].front())) nested = true;
            }
            if (!nested) out.append(contours[i]);
        }
        return out;
    }

    // ---- paint ----

    struct Stop {
        double at;
        TitleColor color;
        double alpha;
    };

    TitleColor mix(const TitleColor& a, const TitleColor& b, double t) {
        return {(float)(a.r + (b.r - a.r) * t), (float)(a.g + (b.g - a.g) * t), (float)(a.b + (b.b - a.b) * t)};
    }

    // the stops of a gradient sampled at raw t (the renderer reads its ramp at t - offset),
    // midpoints as extra stops, cut to [0, 1]
    std::vector<Stop> rampStops(const TitleGradient& g, double offset) {
        std::vector<Stop> raw;
        for (size_t i = 0; i < g.stops.size(); i++) {
            const TitleGradientStop& s = g.stops[i];
            raw.push_back({s.pos + offset, s.color, s.alpha});
            if (i + 1 < g.stops.size() && std::abs(s.mid - 0.5) > 1e-6) {
                const TitleGradientStop& n = g.stops[i + 1];
                raw.push_back({s.pos + s.mid * (n.pos - s.pos) + offset, mix(s.color, n.color, 0.5), (s.alpha + n.alpha) / 2});
            }
        }
        if (raw.empty()) return {{0, TitleColor{1, 1, 1}, 1}};
        const auto at = [&](double t) {
            if (t <= raw.front().at) return Stop{t, raw.front().color, raw.front().alpha};
            for (size_t i = 0; i + 1 < raw.size(); i++) {
                if (t > raw[i + 1].at) continue;
                const double span = raw[i + 1].at - raw[i].at;
                const double k = span > 0 ? (t - raw[i].at) / span : 0;
                return Stop{t, mix(raw[i].color, raw[i + 1].color, k), raw[i].alpha + (raw[i + 1].alpha - raw[i].alpha) * k};
            }
            return Stop{t, raw.back().color, raw.back().alpha};
        };
        std::vector<Stop> out;
        if (raw.front().at > 0 || raw.back().at < 0) out.push_back(at(0));
        for (const Stop& s : raw) if (s.at >= 0 && s.at <= 1) out.push_back(s);
        if (raw.front().at < 0 && (out.empty() || out.front().at > 0)) out.insert(out.begin(), at(0));
        if (raw.back().at > 1 || (out.empty() || out.back().at < 1)) {
            if (out.empty() || out.back().at < 1) out.push_back(at(1));
        }
        for (Stop& s : out) s.at = std::clamp(s.at, 0.0, 1.0);
        return out;
    }

    std::string stopsXml(const std::vector<Stop>& stops) {
        std::string out;
        for (const Stop& s : stops) {
            out += "<stop offset=\"" + fmt(s.at) + "\" stop-color=\"" + formatTitleColor(s.color) + "\"";
            if (s.alpha < 1.0) out += " stop-opacity=\"" + fmt(std::clamp(s.alpha, 0.0, 1.0)) + "\"";
            out += "/>";
        }
        return out;
    }

    struct Writer {
        const TitleArtDoc& doc;
        const TitleSvgOptions& options;
        const AssetResolver* assets;
        const TitleSvgRasterFn& raster;
        TitleRectI frame;
        geometry::TitleGeometry geo;
        Affine toCanvas;                 // document -> canvas pixels (y down)
        std::string defs;
        int nextDef = 0;
        std::set<std::string> ids;
        TitleSvgResult& result;

        Writer(const TitleArtDoc& d, const TitleSvgOptions& o, const AssetResolver* a, const TitleSvgRasterFn& r, const TitleRectI& f,
               TitleSvgResult& out)
            : doc(d), options(o), assets(a), raster(r), frame(f), geo(a), result(out) {
            toCanvas.a = 1;
            toCanvas.d = -1;
            toCanvas.e = doc.canvasWidth() * 0.5;
            toCanvas.f = doc.canvasHeight() * 0.5;
        }

        std::string defId(const char* kind) { return std::string(kind) + "-" + std::to_string(++nextDef); }

        void warn(const std::string& layerPath, const std::string& what, bool leftOut) {
            const std::string line = (layerPath.empty() ? std::string() : layerPath + ": ") + what;
            if (std::find(result.warnings.begin(), result.warnings.end(), line) == result.warnings.end()) result.warnings.push_back(line);
            if (leftOut && std::find(result.leftOut.begin(), result.leftOut.end(), line) == result.leftOut.end()) result.leftOut.push_back(line);
        }

        std::string slugId(const std::string& name) {
            std::string s;
            bool dash = false;
            for (unsigned char c : name) {
                if (c < 128 && std::isalnum(c)) {
                    if (dash && !s.empty()) s += '-';
                    s += (char)std::tolower(c);
                    dash = false;
                } else {
                    dash = true;
                }
            }
            if (s.empty() || std::isdigit((unsigned char)s[0])) s = "layer-" + s;
            if (s.back() == '-') s.pop_back();
            std::string unique = s;
            for (int n = 2; !ids.insert(unique).second; n++) unique = s + "-" + std::to_string(n);
            return unique;
        }

        static Affine localMatrix(const Json& layer) {
            const TitleTransform t = decodeTransform(member(layer, "transform"));
            return Affine::translate(t.position.x, t.position.y)
                .after(Affine::rotateDegrees(-t.rotation))
                .after(Affine::scale(t.scale * (t.flipX ? -1 : 1), t.scale * (t.flipY ? -1 : 1)));
        }

        // the paint of a material over an element box (layer space); `layerToCanvas` maps the
        // gradient's layer coordinates to the file's
        std::string paint(const Json& material, const TitleRect& box, const Affine& layerToCanvas, const std::string& layerPath) {
            const Json& fill = member(material, "fill");
            const std::string kind = str(fill, "kind", "solid");
            TitleColor solid;
            if (!parseTitleColor(fill.value("color", Json("#ffffff")), solid)) solid = {1, 1, 1};
            if (kind == "solid") return formatTitleColor(solid);
            const TitleGradient g = decodeGradient(member(fill, "gradient"));
            if (kind != "linear" && kind != "radial" && kind != "reflected") {
                if (kind == "image" || kind == "source") {
                    warn(layerPath, "the " + kind + " fill is drawn in its colour", false);
                    return formatTitleColor(solid);
                }
                warn(layerPath, "the " + kind + " gradient is drawn as its first colour", false);
                return formatTitleColor(g.stops.empty() ? solid : g.stops.front().color);
            }
            if (g.vivid) warn(layerPath, "the vivid gradient blend is drawn in RGB", false);
            if (str(fill, "span", "layer") != "layer") warn(layerPath, "the gradient spans the layer's box, not each " + str(fill, "span"), false);
            const double s = std::max(num(fill, "scale", 1.0), 1e-3);
            const double offset = num(fill, "offset", 0.0);
            const double angle = num(fill, "angle", 90.0) * DEG;
            const TitleVec2 dir{std::cos(angle), -std::sin(angle)};
            const TitleVec2 c{box.x + box.width / 2, box.y + box.height / 2};
            const TitleVec2 hs{std::max(box.width / 2, 0.5), std::max(box.height / 2, 0.5)};
            const std::string id = defId("gradient");
            std::vector<Stop> stops = rampStops(g, offset);
            if (kind == "radial") {
                const double r = std::max(std::hypot(hs.x, hs.y), 1e-3) * s;
                defs += "<radialGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" cx=\"" + fmt(c.x) + "\" cy=\"" + fmt(c.y) +
                        "\" r=\"" + fmt(r) + "\" gradientTransform=\"" + matrixOf(layerToCanvas) + "\">" + stopsXml(stops) + "</radialGradient>";
            } else {
                const double ext = std::max(std::abs(dir.x) * hs.x + std::abs(dir.y) * hs.y, 1e-3) * s;
                TitleVec2 a{c.x - dir.x * ext, c.y - dir.y * ext}, b{c.x + dir.x * ext, c.y + dir.y * ext};
                if (kind == "reflected") {
                    // t = |v.dir| / ext: the ramp mirrored about the centre (u = 0.5 +- t / 2)
                    std::vector<Stop> mirrored;
                    for (auto it = stops.rbegin(); it != stops.rend(); ++it) mirrored.push_back({0.5 - it->at / 2, it->color, it->alpha});
                    for (const Stop& st : stops) mirrored.push_back({0.5 + st.at / 2, st.color, st.alpha});
                    stops = std::move(mirrored);
                }
                defs += "<linearGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" x1=\"" + fmt(a.x) + "\" y1=\"" + fmt(a.y) +
                        "\" x2=\"" + fmt(b.x) + "\" y2=\"" + fmt(b.y) + "\" gradientTransform=\"" + matrixOf(layerToCanvas) + "\">" +
                        stopsXml(stops) + "</linearGradient>";
            }
            return "url(#" + id + ")";
        }

        void materialWarnings(const Json& material, const std::string& layerPath, const std::string& part) {
            static const std::pair<const char*, const char*> layers[] = {
                {"bevel", "bevel"}, {"reflection", "reflection"}, {"finish", "finish"}, {"satin", "satin"}, {"inner_glow", "inner glow"},
                {"gloss", "gloss"}, {"glass", "glass"}, {"sheen", "sheen"}};
            for (const auto& [key, label] : layers) {
                if (on(member(material, key), "enabled")) warn(layerPath, part + " " + label + " left out", true);
            }
            if (str(member(material, "pattern"), "kind", "none") != "none") warn(layerPath, part + " pattern left out", true);
            if (str(member(material, "texture"), "kind", "none") != "none") warn(layerPath, part + " texture left out", true);
            if (std::abs(num(material, "refraction", 0)) > 0) warn(layerPath, part + " refraction left out", true);
        }

        static const char* joinName(geometry::StrokeJoin j) {
            return j == geometry::StrokeJoin::Round ? "round" : j == geometry::StrokeJoin::Bevel ? "bevel" : "miter";
        }
        static const char* capName(geometry::StrokeCap c) {
            return c == geometry::StrokeCap::Round ? "round" : c == geometry::StrokeCap::Square ? "square" : "butt";
        }

        // the elements of a vector layer as SVG (rims outermost first, then the faces); `white`
        // draws every element in opaque white (a mask of the layer's shape)
        std::string elementsXml(const Json& layer, const geometry::LayerElements& els, const Affine& world, const std::string& layerPath,
                                bool white) {
            const Affine toFile = toCanvas.after(world);
            const double k = std::sqrt(std::abs(world.a * world.d - world.b * world.c));
            std::string out;
            std::vector<const geometry::TitleElement*> order;
            for (auto it = els.elements.rbegin(); it != els.elements.rend(); ++it) {
                if (it->kind == geometry::ElementKind::Rim) order.push_back(&*it);
            }
            for (const geometry::TitleElement& e : els.elements) {
                if (e.kind == geometry::ElementKind::Face) order.push_back(&e);
            }
            for (const geometry::TitleElement* e : order) {
                const bool rim = e->kind == geometry::ElementKind::Rim;
                const Affine m = toFile.after(Affine::translate(e->offset.x, e->offset.y));
                const Json material = e->material ? *e->material : Json::object();
                const TitleRect box{els.baseBounds.x - e->grow + e->offset.x, els.baseBounds.y - e->grow + e->offset.y,
                                    els.baseBounds.width + 2 * e->grow, els.baseBounds.height + 2 * e->grow};
                std::string fill = "#ffffff";
                if (!white) {
                    materialWarnings(material, layerPath, rim ? "rim " + std::to_string(e->rimIndex + 1) : std::string("face"));
                    // a gradient's coordinates are the element's own (layer space, moved by the rim offset)
                    fill = paint(material, box, toFile, layerPath);
                }
                const double opacity = white ? 1.0 : std::clamp(num(material, "opacity", 1.0), 0.0, 1.0);
                std::string group = "<g";
                if (opacity < 1.0) group += " opacity=\"" + fmt(opacity) + "\"";
                group += ">";
                std::string body;
                for (const Path& outline : e->outlines) {
                    const Path drawn = rim && e->fillCounters ? outerContours(outline) : outline;
                    if (drawn.empty()) continue;
                    body += "<path d=\"" + pathData(drawn, m) + "\" fill=\"" + fill + "\"";
                    if (drawn.evenOdd) body += " fill-rule=\"evenodd\"";
                    if (rim && e->grow > 0) {
                        body += " stroke=\"" + fill + "\" stroke-width=\"" + fmt(2 * e->grow * k) + "\" stroke-linejoin=\"" + joinName(e->join) +
                                "\" stroke-miterlimit=\"4\"";
                    }
                    body += "/>";
                }
                for (const geometry::StrokedPath& s : e->strokes) {
                    const bool grown = rim && e->grow > 0;
                    body += "<path d=\"" + pathData(s.path, m) + "\" fill=\"none\" stroke=\"" + fill + "\" stroke-width=\"" +
                            fmt((s.width + (grown ? 2 * e->grow : 0)) * k) + "\" stroke-linejoin=\"" + joinName(grown ? e->join : s.join) +
                            "\" stroke-linecap=\"" + capName(s.cap) + "\" stroke-miterlimit=\"" + fmt(grown ? 4.0 : s.miterLimit) + "\"/>";
                }
                if (!body.empty()) out += group + body + "</g>";
            }
            (void)layer;
            return out;
        }

        static bool vectorKind(const Json& layer) {
            const LayerType type = layerTypeOf(layer);
            if (type == LayerType::Text || type == LayerType::Shape) return str(layer, "dimension", "2d") != "3d";
            if (type == LayerType::Vector) return str(layer, "dimension", "2d") != "3d" && str(layer, "mode", "shape") == "shape";
            return false;
        }

        static bool draws(const Json& layer) {
            return layer.value("visible", true) && !layer.value("guide", false) && num(layer, "opacity", 1) > 0;
        }

        static const char* cssBlend(const std::string& blend) {
            static const std::map<std::string, const char*> table = {
                {"add", "plus-lighter"}, {"screen", "screen"}, {"multiply", "multiply"}, {"overlay", "overlay"},
                {"soft_light", "soft-light"}, {"hard_light", "hard-light"}, {"darken", "darken"}, {"lighten", "lighten"},
                {"color_dodge", "color-dodge"}, {"color_burn", "color-burn"}, {"difference", "difference"},
                {"exclusion", "exclusion"}, {"hue", "hue"}, {"saturation", "saturation"}, {"color", "color"},
                {"luminosity", "luminosity"}};
            auto it = table.find(blend);
            return it == table.end() ? nullptr : it->second;
        }

        std::string frameRect(const char* fill) const {
            return "<rect x=\"" + fmt(frame.x) + "\" y=\"" + fmt(frame.y) + "\" width=\"" + fmt(frame.width) + "\" height=\"" +
                   fmt(frame.height) + "\" fill=\"" + fill + "\"";
        }

        // shape and gradient masks as <mask> ids (outermost applied first); others warned
        std::vector<std::string> masksOf(const Json& layer, const Affine& world, const std::string& layerPath) {
            std::vector<std::string> out;
            const Json& masks = member(layer, "masks");
            if (!masks.is_array()) return out;
            for (const Json& m : masks) {
                if (!m.value("enabled", true)) continue;
                const std::string type = str(m, "type");
                if (type != "shape" && type != "gradient") {
                    warn(layerPath, type + " mask left out", true);
                    continue;
                }
                if (num(m, "feather", 0) > 0) warn(layerPath, type + " mask feather drawn hard", false);
                const double density = std::clamp(num(m, "density", 1.0), 0.0, 1.0);
                const bool invert = m.value("invert", false);
                // the value v becomes 1 - density x (1 - v), after the inversion
                const auto value = [&](double v) { return 1.0 - density * (1.0 - (invert ? 1.0 - v : v)); };
                const Affine maskToDoc = m.value("linked", true) ? world : Affine();
                const TitleVec2 position = decodeVec2(m.value("position", Json::array({0, 0})));
                const std::string id = defId("mask");
                std::string body;
                const auto alphaFill = [&](double v) { return std::string("#ffffff\" fill-opacity=\"") + fmt(std::clamp(v, 0.0, 1.0)); };
                if (type == "shape") {
                    Path shape = geometry::shapePath(geometry::decodeShape(member(m, "shape")));
                    const Affine place = Affine::translate(position.x, position.y).after(Affine::rotateDegrees(-num(m, "rotation", 0)));
                    body += frameRect(alphaFill(value(0)).c_str()) + "/>";
                    body += "<path d=\"" + pathData(shape, toCanvas.after(maskToDoc).after(place)) + "\" fill=\"" + alphaFill(value(1)) + "\"/>";
                } else {
                    const double angle = num(m, "angle", 0) * DEG;
                    const double length = std::max(1e-3, num(m, "length", 200));
                    const double inner = std::clamp(num(m, "inner", 0.0), 0.0, 1.0);
                    const double gamma = std::log(0.5) / std::log(std::clamp(num(m, "mid", 0.5), 0.01, 0.99));
                    const bool radial = str(m, "shape", "linear") == "radial";
                    const std::string gid = defId("gradient");
                    std::string stops;
                    for (int i = 0; i <= 16; i++) {
                        const double t = i / 16.0;
                        const double u = radial ? inner + t * (1.0 - inner) : t;
                        stops += "<stop offset=\"" + fmt(u) + "\" stop-color=\"#ffffff\" stop-opacity=\"" + fmt(value(1.0 - std::pow(t, gamma))) + "\"/>";
                    }
                    if (radial && inner > 0) {
                        stops = "<stop offset=\"0\" stop-color=\"#ffffff\" stop-opacity=\"" + fmt(value(1.0)) + "\"/>" + stops;
                    }
                    const Affine toFile = toCanvas.after(maskToDoc);
                    if (radial) {
                        defs += "<radialGradient id=\"" + gid + "\" gradientUnits=\"userSpaceOnUse\" cx=\"" + fmt(position.x) + "\" cy=\"" +
                                fmt(position.y) + "\" r=\"" + fmt(length) + "\" gradientTransform=\"" + matrixOf(toFile) + "\">" + stops +
                                "</radialGradient>";
                    } else {
                        const TitleVec2 end{position.x + std::cos(angle) * length, position.y - std::sin(angle) * length};
                        defs += "<linearGradient id=\"" + gid + "\" gradientUnits=\"userSpaceOnUse\" x1=\"" + fmt(position.x) + "\" y1=\"" +
                                fmt(position.y) + "\" x2=\"" + fmt(end.x) + "\" y2=\"" + fmt(end.y) + "\" gradientTransform=\"" +
                                matrixOf(toFile) + "\">" + stops + "</linearGradient>";
                    }
                    body += frameRect(("url(#" + gid + ")").c_str()) + "/>";
                }
                defs += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"" + fmt(frame.x) + "\" y=\"" + fmt(frame.y) + "\" width=\"" +
                        fmt(frame.width) + "\" height=\"" + fmt(frame.height) + "\">" + body + "</mask>";
                out.push_back(id);
            }
            return out;
        }

        void effectWarnings(const Json& layer, const std::string& layerPath) {
            for (const Json& e : member(layer, "effects").is_array() ? member(layer, "effects") : Json::array()) {
                if (e.value("enabled", true)) warn(layerPath, str(e, "type") + " effect left out", true);
            }
            const Json& parts = member(layer, "part_effects");
            for (auto it = parts.begin(); parts.is_object() && it != parts.end(); ++it) {
                for (const Json& e : *it) {
                    if (e.value("enabled", true)) warn(layerPath, it.key() + " " + str(e, "type") + " effect left out", true);
                }
            }
            const Json& style = member(layer, "style");
            if (on(member(style, "erode"), "enabled")) warn(layerPath, "erode left out", true);
            if (on(style, "knockout")) warn(layerPath, "knockout left out", true);
        }

        // the clip of `layer` to the base below it: a clipPath (faces only) or a mask (rims, strokes)
        std::string clipOf(const Json& base, const Affine& baseWorld, const std::string& layerPath, bool& asMask) {
            asMask = false;
            if (!vectorKind(base)) {
                warn(layerPath, "clipping to a non-vector layer left out", true);
                return {};
            }
            const geometry::LayerElements els = geo.elements(base, doc);
            bool facesOnly = true;
            for (const geometry::TitleElement& e : els.elements) {
                if (e.kind == geometry::ElementKind::Rim || !e.strokes.empty()) facesOnly = false;
            }
            const std::string id = defId(facesOnly ? "clip" : "mask");
            if (facesOnly) {
                std::string paths;
                const Affine m = toCanvas.after(baseWorld);
                for (const geometry::TitleElement& e : els.elements) {
                    for (const Path& p : e.outlines) {
                        paths += "<path d=\"" + pathData(p, m) + "\"" + (p.evenOdd ? " clip-rule=\"evenodd\"" : "") + "/>";
                    }
                }
                defs += "<clipPath id=\"" + id + "\" clipPathUnits=\"userSpaceOnUse\">" + paths + "</clipPath>";
                return id;
            }
            asMask = true;
            defs += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"" + fmt(frame.x) + "\" y=\"" + fmt(frame.y) + "\" width=\"" +
                    fmt(frame.width) + "\" height=\"" + fmt(frame.height) + "\">" + elementsXml(base, els, baseWorld, layerPath, true) + "</mask>";
            return id;
        }

        static std::string base64(const std::vector<uint8_t>& bytes) {
            static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            for (size_t i = 0; i < bytes.size(); i += 3) {
                const uint32_t n = (uint32_t)bytes[i] << 16 | (i + 1 < bytes.size() ? (uint32_t)bytes[i + 1] << 8 : 0) |
                                   (i + 2 < bytes.size() ? bytes[i + 2] : 0);
                out += table[(n >> 18) & 63];
                out += table[(n >> 12) & 63];
                out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
                out += i + 2 < bytes.size() ? table[n & 63] : '=';
            }
            return out;
        }

        std::string list(const Json& layers, const Affine& parentWorld, const std::string& prefix) {
            std::string out;
            if (!layers.is_array()) return out;
            std::map<std::string, int> counts;
            for (size_t i = 0; i < layers.size(); i++) {
                const Json& layer = layers[i];
                const std::string name = str(layer, "name", "Layer");
                const int n = counts[name]++;
                const std::string path = prefix + name + (n ? "#" + std::to_string(n) : "");
                if (!draws(layer)) continue;
                const Affine world = parentWorld.after(localMatrix(layer));
                const LayerType type = layerTypeOf(layer);
                std::string content;
                if (type == LayerType::Group) {
                    content = list(member(layer, "children"), world, path + "/");
                    effectWarnings(layer, path);
                } else if (vectorKind(layer)) {
                    const geometry::LayerElements els = geo.elements(layer, doc);
                    for (const TitleProblem& p : els.problems) warn(path, p.code + ": " + p.message, false);
                    content = elementsXml(layer, els, world, path, false);
                    effectWarnings(layer, path);
                } else {
                    const std::string what = type == LayerType::Image ? "image layer"
                                             : type == LayerType::Vector ? (str(layer, "dimension", "2d") == "3d" ? "3D layer" : "artwork vector layer")
                                             : type == LayerType::Generator ? "generator layer"
                                             : type == LayerType::Adjustment ? "adjustment layer"
                                             : type == LayerType::Unknown ? "layer of an unknown type" : "3D layer";
                    TitleSvgRaster image;
                    if (options.embedRaster && raster && type != LayerType::Adjustment && raster(layer, image) && !image.png.empty() &&
                        !image.frame.empty()) {
                        out += "<g id=\"" + slugId(name) + "\"><image x=\"" + fmt(image.frame.x) + "\" y=\"" + fmt(image.frame.y) +
                               "\" width=\"" + fmt(image.frame.width) + "\" height=\"" + fmt(image.frame.height) +
                               "\" preserveAspectRatio=\"none\" href=\"data:image/png;base64," + base64(image.png) + "\"/></g>";
                        result.layers++;
                        warn(path, what + " embedded as a PNG", false);
                    } else {
                        warn(path, what + " left out", true);
                    }
                    continue;
                }
                if (content.empty()) continue;
                // masks, then the clip to the base below
                for (const std::string& mask : masksOf(layer, world, path)) content = "<g mask=\"url(#" + mask + ")\">" + content + "</g>";
                if (layer.value("clip", false)) {
                    size_t b = i;
                    while (b > 0 && layers[b - 1].value("clip", false)) b--;
                    if (b > 0) {
                        bool asMask = false;
                        const Json& base = layers[b - 1];
                        const std::string clip = clipOf(base, parentWorld.after(localMatrix(base)), path, asMask);
                        if (!clip.empty()) content = std::string("<g ") + (asMask ? "mask" : "clip-path") + "=\"url(#" + clip + ")\">" + content + "</g>";
                    }
                }
                std::string attrs = " id=\"" + slugId(name) + "\"";
                const double opacity = std::clamp(num(layer, "opacity", 1.0), 0.0, 1.0);
                if (opacity < 1.0) attrs += " opacity=\"" + fmt(opacity) + "\"";
                const std::string blend = str(layer, "blend", "normal");
                if (blend != "normal") {
                    if (const char* css = cssBlend(blend)) attrs += std::string(" style=\"mix-blend-mode:") + css + "\"";
                    else warn(path, "blend " + blend + " has no CSS mode: drawn normal", false);
                }
                out += "<g" + attrs + ">" + content + "</g>";
                result.layers++;
            }
            return out;
        }
    };

}

std::vector<std::string> editor::titleSvgRasterLayers(const TitleArtDoc& doc) {
    std::vector<std::string> out;
    std::function<void(const Json&)> walk = [&](const Json& layers) {
        if (!layers.is_array()) return;
        for (const Json& layer : layers) {
            if (!Writer::draws(layer)) continue;
            const LayerType type = layerTypeOf(layer);
            if (type == LayerType::Group) walk(member(layer, "children"));
            else if (type != LayerType::Adjustment && !Writer::vectorKind(layer)) out.push_back(str(layer, "id"));
        }
    };
    walk(doc.layers());
    return out;
}

TitleSvgResult editor::writeTitleSvg(const TitleArtDoc& doc, const TitleRectI& frame, const TitleSvgOptions& options, const AssetResolver* assets,
                                     const TitleSvgRasterFn& raster) {
    TitleSvgResult result;
    if (frame.empty()) {
        result.code = "bad_frame";
        result.error = "the export frame is empty";
        return result;
    }
    Writer w(doc, options, assets, raster, frame, result);
    const std::string body = w.list(doc.layers(), Affine(), "");
    if (options.strict && !result.leftOut.empty()) {
        result.code = "svg_unrepresentable";
        result.error = "strict: " + std::to_string(result.leftOut.size()) + " item(s) have no SVG form:";
        for (const std::string& l : result.leftOut) result.error += " " + l + ";";
        result.error.pop_back();
        return result;
    }
    const double scale = std::max(options.scale, 1e-3);
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    text += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"" + fmt(frame.width * scale) + "\" height=\"" +
            fmt(frame.height * scale) + "\" viewBox=\"" + fmt(frame.x) + " " + fmt(frame.y) + " " + fmt(frame.width) + " " +
            fmt(frame.height) + "\">\n";
    const std::string name = str(doc.root, "name");
    if (!name.empty()) text += "<title>" + escape(name) + "</title>\n";
    if (!w.defs.empty()) text += "<defs>" + w.defs + "</defs>\n";
    text += body + "\n</svg>\n";
    result.text = std::move(text);
    result.ok = true;
    return result;
}
