// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SvgPaths.h"

#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr int MAX_USE_DEPTH = 16;

    // ---------- numbers and lengths ----------

    bool isSeparator(char c) { return std::isspace((unsigned char)c) || c == ','; }

    struct Scanner {
        const char* p;
        const char* end;

        void skipSeparators() {
            while (p < end && isSeparator(*p)) p++;
        }
        bool atEnd() {
            skipSeparators();
            return p >= end;
        }
        // an SVG number: sign, digits, fraction, exponent ("1.5.5" is two numbers)
        bool number(double& out) {
            skipSeparators();
            const char* s = p;
            if (s < end && (*s == '+' || *s == '-')) s++;
            bool digits = false;
            while (s < end && std::isdigit((unsigned char)*s)) {
                s++;
                digits = true;
            }
            if (s < end && *s == '.') {
                s++;
                while (s < end && std::isdigit((unsigned char)*s)) {
                    s++;
                    digits = true;
                }
            }
            if (!digits) return false;
            if (s < end && (*s == 'e' || *s == 'E')) {
                const char* e = s + 1;
                if (e < end && (*e == '+' || *e == '-')) e++;
                if (e < end && std::isdigit((unsigned char)*e)) {
                    while (e < end && std::isdigit((unsigned char)*e)) e++;
                    s = e;
                }
            }
            out = std::strtod(std::string(p, s).c_str(), nullptr);
            p = s;
            return true;
        }
        // an arc flag: one 0 or 1, separators optional ("a1 1 0 104 4")
        bool flag(bool& out) {
            skipSeparators();
            if (p < end && (*p == '0' || *p == '1')) {
                out = *p == '1';
                p++;
                return true;
            }
            return false;
        }
    };

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // a length in user units; `unsupported` for relative units (%, em, ex)
    std::optional<double> parseLength(const std::string& text, bool* unsupported = nullptr) {
        const std::string t = trim(text);
        Scanner sc{t.c_str(), t.c_str() + t.size()};
        double v = 0;
        if (!sc.number(v)) return std::nullopt;
        const std::string unit = lower(trim(std::string(sc.p, sc.end)));
        if (unit.empty() || unit == "px") return v;
        if (unit == "pt") return v * 4.0 / 3.0;
        if (unit == "pc") return v * 16.0;
        if (unit == "in") return v * 96.0;
        if (unit == "cm") return v * 96.0 / 2.54;
        if (unit == "mm") return v * 96.0 / 25.4;
        if (unsupported) *unsupported = true;
        return std::nullopt;
    }

    // ---------- transforms ----------

    Affine parseTransform(const std::string& text, bool& ok) {
        Affine m;
        ok = true;
        const char* p = text.c_str();
        const char* end = p + text.size();
        while (p < end) {
            while (p < end && (std::isspace((unsigned char)*p) || *p == ',')) p++;
            if (p >= end) break;
            const char* nameStart = p;
            while (p < end && std::isalpha((unsigned char)*p)) p++;
            const std::string name(nameStart, p);
            while (p < end && std::isspace((unsigned char)*p)) p++;
            if (p >= end || *p != '(') {
                ok = false;
                return m;
            }
            const char* close = (const char*)std::memchr(p, ')', end - p);
            if (!close) {
                ok = false;
                return m;
            }
            Scanner sc{p + 1, close};
            std::vector<double> v;
            double x;
            while (sc.number(x)) v.push_back(x);
            p = close + 1;
            Affine t;
            if (name == "matrix" && v.size() == 6) {
                t = {v[0], v[1], v[2], v[3], v[4], v[5]};
            } else if (name == "translate" && !v.empty()) {
                t = Affine::translate(v[0], v.size() > 1 ? v[1] : 0);
            } else if (name == "scale" && !v.empty()) {
                t = Affine::scale(v[0], v.size() > 1 ? v[1] : v[0]);
            } else if (name == "rotate" && !v.empty()) {
                // SVG rotates clockwise on screen in its y-down space: the same matrix as a
                // counter-clockwise rotation in a y-up space
                t = Affine::rotateDegrees(v[0]);
                if (v.size() >= 3) t = Affine::translate(v[1], v[2]).after(t).after(Affine::translate(-v[1], -v[2]));
            } else if (name == "skewX" && !v.empty()) {
                t.c = std::tan(v[0] * PI / 180);
            } else if (name == "skewY" && !v.empty()) {
                t.b = std::tan(v[0] * PI / 180);
            } else {
                ok = false;
                return m;
            }
            m = m.after(t);
        }
        return m;
    }

    // ---------- path data ----------

    // SVG's endpoint arc as cubics (F.6.5 of the SVG spec: centre parameterization, then pieces of
    // at most 90 degrees)
    void arcTo(Path& out, const TitleVec2& from, double rx, double ry, double angleDeg, bool largeArc, bool sweep, const TitleVec2& to) {
        if (from.x == to.x && from.y == to.y) return;
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0 || ry == 0) {
            out.lineTo(to.x, to.y);
            return;
        }
        const double phi = angleDeg * PI / 180;
        const double cphi = std::cos(phi), sphi = std::sin(phi);
        const double dx = (from.x - to.x) / 2, dy = (from.y - to.y) / 2;
        const double x1 = cphi * dx + sphi * dy, y1 = -sphi * dx + cphi * dy;
        const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lambda > 1) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
        if (largeArc == sweep) coef = -coef;
        const double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
        const double cx = cphi * cx1 - sphi * cy1 + (from.x + to.x) / 2;
        const double cy = sphi * cx1 + cphi * cy1 + (from.y + to.y) / 2;
        const auto angle = [](double ux, double uy, double vx, double vy) {
            const double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return a;
        };
        const double theta1 = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        double dtheta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && dtheta > 0) dtheta -= 2 * PI;
        if (sweep && dtheta < 0) dtheta += 2 * PI;
        const int pieces = std::max(1, (int)std::ceil(std::abs(dtheta) / (PI / 2) - 1e-9));
        const double step = dtheta / pieces;
        const double k = 4.0 / 3.0 * std::tan(step / 4);
        const auto point = [&](double t) {
            const double x = rx * std::cos(t), y = ry * std::sin(t);
            return TitleVec2{cphi * x - sphi * y + cx, sphi * x + cphi * y + cy};
        };
        const auto derivative = [&](double t) {
            const double x = -rx * std::sin(t), y = ry * std::cos(t);
            return TitleVec2{cphi * x - sphi * y, sphi * x + cphi * y};
        };
        double t = theta1;
        for (int i = 0; i < pieces; i++) {
            const double t2 = t + step;
            const TitleVec2 p0 = point(t), p3 = i == pieces - 1 ? to : point(t2);
            const TitleVec2 d0 = derivative(t), d3 = derivative(t2);
            out.cubicTo(p0.x + k * d0.x, p0.y + k * d0.y, p3.x - k * d3.x, p3.y - k * d3.y, p3.x, p3.y);
            t = t2;
        }
    }

}

bool jasperengine::titleart::geometry::parseSvgPathData(const std::string& d, Path& out) {
    Scanner sc{d.c_str(), d.c_str() + d.size()};
    TitleVec2 current, start, lastControl;
    char lastCommand = 0;
    bool open = false;
    char command = 0;
    const auto ensureOpen = [&]() {
        if (!open) {
            out.moveTo(current.x, current.y);
            start = current;
            open = true;
        }
    };
    while (!sc.atEnd()) {
        if (std::isalpha((unsigned char)*sc.p)) {
            command = *sc.p++;
        } else if (command == 0) {
            return false;
        }
        const bool rel = std::islower((unsigned char)command) != 0;
        const char c = (char)std::toupper((unsigned char)command);
        const TitleVec2 base = rel ? current : TitleVec2{0, 0};
        double v[7];
        const auto read = [&](int n) {
            for (int i = 0; i < n; i++) {
                if (!sc.number(v[i])) return false;
            }
            return true;
        };
        switch (c) {
            case 'Z':
                if (open) {
                    out.close();
                    open = false;
                }
                current = start;
                lastCommand = 'Z';
                // Z takes no arguments: the next command letter must follow
                command = 0;
                continue;
            case 'M':
                if (!read(2)) return false;
                current = {base.x + v[0], base.y + v[1]};
                out.moveTo(current.x, current.y);
                start = current;
                open = true;
                // further pairs are implicit line-tos
                command = rel ? 'l' : 'L';
                lastCommand = 'M';
                continue;
            case 'L':
                if (!read(2)) return false;
                ensureOpen();
                current = {base.x + v[0], base.y + v[1]};
                out.lineTo(current.x, current.y);
                break;
            case 'H':
                if (!read(1)) return false;
                ensureOpen();
                current.x = (rel ? current.x : 0) + v[0];
                out.lineTo(current.x, current.y);
                break;
            case 'V':
                if (!read(1)) return false;
                ensureOpen();
                current.y = (rel ? current.y : 0) + v[0];
                out.lineTo(current.x, current.y);
                break;
            case 'C': {
                if (!read(6)) return false;
                ensureOpen();
                const TitleVec2 c1{base.x + v[0], base.y + v[1]}, c2{base.x + v[2], base.y + v[3]}, p{base.x + v[4], base.y + v[5]};
                out.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
                lastControl = c2;
                current = p;
                break;
            }
            case 'S': {
                if (!read(4)) return false;
                ensureOpen();
                const char last = (char)std::toupper((unsigned char)lastCommand);
                const TitleVec2 c1 = (last == 'C' || last == 'S') ? TitleVec2{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
                const TitleVec2 c2{base.x + v[0], base.y + v[1]}, p{base.x + v[2], base.y + v[3]};
                out.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
                lastControl = c2;
                current = p;
                break;
            }
            case 'Q': {
                if (!read(4)) return false;
                ensureOpen();
                const TitleVec2 q{base.x + v[0], base.y + v[1]}, p{base.x + v[2], base.y + v[3]};
                out.quadTo(q.x, q.y, p.x, p.y);
                lastControl = q;
                current = p;
                break;
            }
            case 'T': {
                if (!read(2)) return false;
                ensureOpen();
                const char last = (char)std::toupper((unsigned char)lastCommand);
                const TitleVec2 q = (last == 'Q' || last == 'T') ? TitleVec2{2 * current.x - lastControl.x, 2 * current.y - lastControl.y} : current;
                const TitleVec2 p{base.x + v[0], base.y + v[1]};
                out.quadTo(q.x, q.y, p.x, p.y);
                lastControl = q;
                current = p;
                break;
            }
            case 'A': {
                bool large = false, sweep = false;
                if (!sc.number(v[0]) || !sc.number(v[1]) || !sc.number(v[2]) || !sc.flag(large) || !sc.flag(sweep) ||
                    !sc.number(v[3]) || !sc.number(v[4])) {
                    return false;
                }
                ensureOpen();
                const TitleVec2 p{base.x + v[3], base.y + v[4]};
                arcTo(out, current, v[0], v[1], v[2], large, sweep, p);
                current = p;
                break;
            }
            default:
                return false;
        }
        lastCommand = command;
    }
    return true;
}

namespace {

    using Style = std::map<std::string, std::string>;

    // the properties this reader uses; others in a style are skipped silently (colours, opacity...)
    const std::set<std::string> KNOWN_PROPERTIES = {
        "fill", "fill-rule", "stroke", "stroke-width", "stroke-linejoin", "stroke-linecap", "stroke-miterlimit",
        "display", "visibility"};
    // properties that change what is drawn beyond outlines: reported when present
    const std::set<std::string> IGNORED_PROPERTIES = {"clip-path", "mask", "filter", "marker-start", "marker-mid", "marker-end", "marker"};
    const std::set<std::string> INHERITED = {
        "fill", "fill-rule", "stroke", "stroke-width", "stroke-linejoin", "stroke-linecap", "stroke-miterlimit", "visibility"};

    void parseDeclarations(const std::string& text, Style& out, std::set<std::string>& ignored) {
        size_t i = 0;
        while (i < text.size()) {
            const size_t semi = text.find(';', i);
            const std::string decl = text.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
            i = semi == std::string::npos ? text.size() : semi + 1;
            const size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(trim(decl.substr(0, colon)));
            std::string value = trim(decl.substr(colon + 1));
            const size_t important = lower(value).find("!important");
            if (important != std::string::npos) value = trim(value.substr(0, important));
            if (KNOWN_PROPERTIES.count(name)) out[name] = value;
            if (IGNORED_PROPERTIES.count(name) && value != "none") ignored.insert(name);
        }
    }

    struct Reader {
        SvgGeometry geometry;
        std::set<std::string> ignored;
        std::map<std::string, Style> classRules;
        std::map<std::string, const tinyxml2::XMLElement*> ids;

        static std::string localName(const char* name) {
            const char* colon = std::strrchr(name, ':');
            return colon ? colon + 1 : name;
        }

        void collectIds(const tinyxml2::XMLElement* e) {
            for (; e; e = e->NextSiblingElement()) {
                if (const char* id = e->Attribute("id")) ids.emplace(id, e);
                collectIds(e->FirstChildElement());
            }
        }

        void collectStyles(const tinyxml2::XMLElement* e) {
            for (; e; e = e->NextSiblingElement()) {
                if (localName(e->Name()) == "style") {
                    std::string css;
                    for (const tinyxml2::XMLNode* n = e->FirstChild(); n; n = n->NextSibling()) {
                        if (n->ToText()) css += n->Value();
                    }
                    parseCss(css);
                }
                collectStyles(e->FirstChildElement());
            }
        }

        void parseCss(std::string css) {
            // comments out
            for (size_t a = css.find("/*"); a != std::string::npos; a = css.find("/*", a)) {
                const size_t b = css.find("*/", a + 2);
                css.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
            }
            size_t i = 0;
            while (i < css.size()) {
                const size_t open = css.find('{', i);
                if (open == std::string::npos) break;
                const size_t close = css.find('}', open);
                const std::string selectors = css.substr(i, open - i);
                const std::string body = css.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
                i = close == std::string::npos ? css.size() : close + 1;
                size_t s = 0;
                while (s <= selectors.size()) {
                    const size_t comma = selectors.find(',', s);
                    const std::string selector = trim(selectors.substr(s, comma == std::string::npos ? std::string::npos : comma - s));
                    s = comma == std::string::npos ? selectors.size() + 1 : comma + 1;
                    if (selector.empty()) continue;
                    bool simpleClass = selector.size() > 1 && selector[0] == '.';
                    for (size_t k = 1; simpleClass && k < selector.size(); k++) {
                        const char ch = selector[k];
                        simpleClass = std::isalnum((unsigned char)ch) || ch == '-' || ch == '_';
                    }
                    if (simpleClass) {
                        parseDeclarations(body, classRules[selector.substr(1)], ignored);
                    } else {
                        ignored.insert("css selector '" + selector + "'");
                    }
                }
            }
        }

        // the element's own style: presentation attributes, then class rules, then `style`
        Style ownStyle(const tinyxml2::XMLElement* e) {
            Style s;
            for (const tinyxml2::XMLAttribute* a = e->FirstAttribute(); a; a = a->Next()) {
                const std::string name = lower(a->Name());
                if (KNOWN_PROPERTIES.count(name)) s[name] = trim(a->Value());
                if (IGNORED_PROPERTIES.count(name) && trim(a->Value()) != "none") ignored.insert(name);
            }
            if (const char* cls = e->Attribute("class")) {
                const std::string list = cls;
                size_t i = 0;
                while (i < list.size()) {
                    while (i < list.size() && std::isspace((unsigned char)list[i])) i++;
                    size_t j = i;
                    while (j < list.size() && !std::isspace((unsigned char)list[j])) j++;
                    if (j > i) {
                        const auto rule = classRules.find(list.substr(i, j - i));
                        if (rule != classRules.end()) {
                            for (const auto& [k, v] : rule->second) s[k] = v;
                        }
                    }
                    i = j;
                }
            }
            if (const char* style = e->Attribute("style")) parseDeclarations(style, s, ignored);
            return s;
        }

        static Style inherit(const Style& parent, const Style& own) {
            Style s;
            for (const auto& [k, v] : parent) {
                if (INHERITED.count(k)) s[k] = v;
            }
            for (const auto& [k, v] : own) {
                if (v != "inherit") s[k] = v;
            }
            return s;
        }

        double attr(const tinyxml2::XMLElement* e, const char* name, double fallback = 0) {
            const char* v = e->Attribute(name);
            if (!v) return fallback;
            bool unsupported = false;
            const std::optional<double> length = parseLength(v, &unsupported);
            if (unsupported) ignored.insert(std::string("relative length (") + name + ")");
            return length.value_or(fallback);
        }

        static double transformScale(const Affine& m) { return std::sqrt(std::abs(m.a * m.d - m.b * m.c)); }

        void emit(Path path, const Style& style, const Affine& ctm, bool closedShape) {
            if (path.empty()) return;
            const auto get = [&](const char* k, const char* fallback) {
                const auto it = style.find(k);
                return it == style.end() ? std::string(fallback) : it->second;
            };
            path.transform(ctm);
            const std::string fill = lower(get("fill", "black"));
            const std::string stroke = lower(get("stroke", "none"));
            // a line or an open polyline has no area to fill
            if (fill != "none" && fill != "transparent" && closedShape) {
                Path f = path;
                f.evenOdd = lower(get("fill-rule", "nonzero")) == "evenodd";
                geometry.fills.push_back(std::move(f));
            }
            if (stroke != "none" && stroke != "transparent") {
                bool unsupported = false;
                const double width = parseLength(get("stroke-width", "1"), &unsupported).value_or(1.0) * transformScale(ctm);
                if (unsupported) ignored.insert("relative length (stroke-width)");
                if (width > 0) {
                    SvgStroke s;
                    s.path = std::move(path);
                    s.width = width;
                    const std::string join = lower(get("stroke-linejoin", "miter"));
                    s.join = join == "round" ? OffsetJoin::Round : join == "bevel" ? OffsetJoin::Bevel : OffsetJoin::Miter;
                    const std::string cap = lower(get("stroke-linecap", "butt"));
                    s.cap = cap == "round" ? OffsetCap::Round : cap == "square" ? OffsetCap::Square : OffsetCap::Butt;
                    s.miterLimit = parseLength(get("stroke-miterlimit", "4")).value_or(4.0);
                    geometry.strokes.push_back(std::move(s));
                }
            }
        }

        void visit(const tinyxml2::XMLElement* e, const Style& parentStyle, const Affine& parentCtm, int useDepth) {
            const std::string name = localName(e->Name());
            const Style style = inherit(parentStyle, ownStyle(e));
            if (const auto d = style.find("display"); d != style.end() && lower(d->second) == "none") return;
            Affine ctm = parentCtm;
            if (const char* t = e->Attribute("transform")) {
                bool ok = true;
                const Affine m = parseTransform(t, ok);
                if (!ok) ignored.insert("transform");
                ctm = parentCtm.after(m);
            }
            const auto visible = [&]() {
                const auto v = style.find("visibility");
                return v == style.end() || (lower(v->second) != "hidden" && lower(v->second) != "collapse");
            };
            if (name == "svg" || name == "g" || name == "a" || name == "switch") {
                for (const tinyxml2::XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) visit(c, style, ctm, useDepth);
            } else if (name == "use") {
                const char* href = e->Attribute("href");
                if (!href) href = e->Attribute("xlink:href");
                if (!href || href[0] != '#') {
                    ignored.insert("use (external reference)");
                    return;
                }
                const auto target = ids.find(href + 1);
                if (target == ids.end() || useDepth >= MAX_USE_DEPTH) return;
                const Affine placed = ctm.after(Affine::translate(attr(e, "x"), attr(e, "y")));
                const std::string targetName = localName(target->second->Name());
                if (targetName == "symbol") {
                    const Style symbolStyle = inherit(style, ownStyle(target->second));
                    for (const tinyxml2::XMLElement* c = target->second->FirstChildElement(); c; c = c->NextSiblingElement()) {
                        visit(c, symbolStyle, placed, useDepth + 1);
                    }
                } else {
                    visit(target->second, style, placed, useDepth + 1);
                }
            } else if (name == "path") {
                if (!visible()) return;
                Path p;
                const char* d = e->Attribute("d");
                if (d && !parseSvgPathData(d, p)) ignored.insert("path data error");
                emit(std::move(p), style, ctm, true);
            } else if (name == "rect") {
                if (!visible()) return;
                const double x = attr(e, "x"), y = attr(e, "y"), w = attr(e, "width"), h = attr(e, "height");
                if (!(w > 0) || !(h > 0)) return;
                double rx = attr(e, "rx", -1), ry = attr(e, "ry", -1);
                if (rx < 0 && ry < 0) rx = ry = 0;
                if (rx < 0) rx = ry;
                if (ry < 0) ry = rx;
                rx = std::min(rx, w / 2);
                ry = std::min(ry, h / 2);
                Path p;
                if (rx > 0 && ry > 0) {
                    const double kx = 0.5522847498 * rx, ky = 0.5522847498 * ry;
                    p.moveTo(x + rx, y);
                    p.lineTo(x + w - rx, y);
                    p.cubicTo(x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry);
                    p.lineTo(x + w, y + h - ry);
                    p.cubicTo(x + w, y + h - ry + ky, x + w - rx + kx, y + h, x + w - rx, y + h);
                    p.lineTo(x + rx, y + h);
                    p.cubicTo(x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry);
                    p.lineTo(x, y + ry);
                    p.cubicTo(x, y + ry - ky, x + rx - kx, y, x + rx, y);
                } else {
                    p.moveTo(x, y);
                    p.lineTo(x + w, y);
                    p.lineTo(x + w, y + h);
                    p.lineTo(x, y + h);
                }
                p.close();
                emit(std::move(p), style, ctm, true);
            } else if (name == "circle" || name == "ellipse") {
                if (!visible()) return;
                const double cx = attr(e, "cx"), cy = attr(e, "cy");
                const double rx = name == "circle" ? attr(e, "r") : attr(e, "rx");
                const double ry = name == "circle" ? rx : attr(e, "ry");
                if (!(rx > 0) || !(ry > 0)) return;
                const double kx = 0.5522847498 * rx, ky = 0.5522847498 * ry;
                Path p;
                p.moveTo(cx + rx, cy);
                p.cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
                p.cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
                p.cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
                p.cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
                p.close();
                emit(std::move(p), style, ctm, true);
            } else if (name == "line") {
                if (!visible()) return;
                Path p;
                p.moveTo(attr(e, "x1"), attr(e, "y1"));
                p.lineTo(attr(e, "x2"), attr(e, "y2"));
                emit(std::move(p), style, ctm, false);
            } else if (name == "polyline" || name == "polygon") {
                if (!visible()) return;
                const char* points = e->Attribute("points");
                if (!points) return;
                Scanner sc{points, points + std::strlen(points)};
                Path p;
                double x, y;
                bool first = true;
                while (sc.number(x) && sc.number(y)) {
                    if (first) {
                        p.moveTo(x, y);
                    } else {
                        p.lineTo(x, y);
                    }
                    first = false;
                }
                if (name == "polygon") p.close();
                // an open polyline still fills (SVG fills it closed)
                emit(std::move(p), style, ctm, true);
            } else if (name == "defs" || name == "symbol" || name == "style" || name == "title" || name == "desc" ||
                       name == "metadata" || name == "linearGradient" || name == "radialGradient" || name == "stop") {
                // not drawn here (symbols through use, gradients' colours are ignored in shape mode)
            } else if (name == "clipPath" || name == "mask" || name == "filter" || name == "pattern" || name == "marker") {
                // definitions; their use is reported through the referring property
            } else {
                ignored.insert(name);
            }
        }
    };

}

SvgGeometry jasperengine::titleart::geometry::parseSvgGeometry(const std::string& text) {
    Reader reader;
    tinyxml2::XMLDocument doc;
    if (doc.Parse(text.c_str(), text.size()) != tinyxml2::XML_SUCCESS) {
        reader.geometry.error = doc.ErrorStr() ? doc.ErrorStr() : "not XML";
        return reader.geometry;
    }
    const tinyxml2::XMLElement* root = doc.RootElement();
    if (!root || Reader::localName(root->Name()) != "svg") {
        reader.geometry.error = "no <svg> root element";
        return reader.geometry;
    }
    reader.collectIds(root);
    reader.collectStyles(root);
    SvgGeometry& g = reader.geometry;
    if (const char* viewBox = root->Attribute("viewBox")) {
        Scanner sc{viewBox, viewBox + std::strlen(viewBox)};
        double v[4];
        if (sc.number(v[0]) && sc.number(v[1]) && sc.number(v[2]) && sc.number(v[3])) {
            g.width = v[2];
            g.height = v[3];
        }
    }
    if (g.width <= 0 || g.height <= 0) {
        // a root sized in % (of its embedding) has no viewport of its own: 0
        g.width = root->Attribute("width") ? parseLength(root->Attribute("width")).value_or(0.0) : 0.0;
        g.height = root->Attribute("height") ? parseLength(root->Attribute("height")).value_or(0.0) : 0.0;
    }
    // the root's own transform and children; the viewBox mapping is a uniform fit TitleGeometry redoes
    for (const tinyxml2::XMLElement* c = root->FirstChildElement(); c; c = c->NextSiblingElement()) {
        reader.visit(c, Reader::inherit({}, reader.ownStyle(root)), Affine(), 0);
    }
    g.ignored.assign(reader.ignored.begin(), reader.ignored.end());
    g.ok = true;
    return g;
}
