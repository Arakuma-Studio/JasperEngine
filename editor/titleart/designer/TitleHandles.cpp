// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Gradient-line maths ported from Pachislop src/ui/viewport/gradient.ts, shape handles from
// shapeHandles.ts and ParamHandles.tsx, distort points from DistortHandles.tsx, the light dial from
// LightDial.tsx and the light sphere from src/ui/LightSphere.tsx, by SafEight, used with the
// author's permission.

#include "TitleHandles.h"
#include "TitleGizmo2D.h"

#include <algorithm>
#include <cmath>
#include <string_view>

using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    constexpr float HIT = 12.0f;            // a handle's hit radius: a 24 px target whatever its drawn size
    constexpr float LINE_HIT = 6.0f;        // a gradient line
    constexpr float SNAP_PX = 8.0f;         // Shift: a point within this many screen px of the layer centre snaps to it
    constexpr float ARROW_PX = 70.0f;       // a direction whose param has no length
    constexpr float LIGHT_DIAL_R = 26.0f;   // LightDial.tsx
    constexpr float ANGLE_DIAL_R = 34.0f;
    constexpr float BEND_STEM = 22.0f;      // the bend handle above the top edge
    constexpr double BEND_PER_PX = 0.5;     // bend % per screen px
    constexpr double POINTS_DRAG_PX = 12.0; // screen px per point / side
    constexpr double INNER_MIN = 0.05, INNER_MAX = 0.98;
    constexpr double MIN_GRADIENT_SCALE = 0.05;
    constexpr ImU32 LIGHT_COLOR = IM_COL32(255, 211, 74, 255);
    constexpr const char* DOT = " \xC2\xB7 ";      // " · "
    constexpr const char* DEGREE = "\xC2\xB0";     // "°"
    // distort corners TL, TR, BR, BL -> their left-right mirror
    constexpr int CORNER_MIRROR[4] = {1, 0, 3, 2};

    double wrap180(double v) {
        double r = std::fmod(v + 180.0, 360.0);
        if (r < 0.0) r += 360.0;
        r -= 180.0;
        return r <= -180.0 ? 180.0 : r;
    }
    double r1(double v) { return std::round(v * 10.0) / 10.0; }
    double r3(double v) { return std::round(v * 1000.0) / 1000.0; }
    double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

    TitleVec2 add(TitleVec2 a, TitleVec2 b) { return {a.x + b.x, a.y + b.y}; }
    TitleVec2 sub(TitleVec2 a, TitleVec2 b) { return {a.x - b.x, a.y - b.y}; }
    TitleVec2 mul(TitleVec2 a, double k) { return {a.x * k, a.y * k}; }
    double dot(TitleVec2 a, TitleVec2 b) { return a.x * b.x + a.y * b.y; }
    double len(TitleVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
    float screenDistance(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
    float segmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float l2 = vx * vx + vy * vy;
        float t = l2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return screenDistance(p, ImVec2(a.x + vx * t, a.y + vy * t));
    }

    // an angle's unit vector in a y-up space, and back
    TitleVec2 dirOf(double angle, AngleConvention c) {
        const double a = angle * DEG;
        return c == AngleConvention::ClockwiseFromRight ? TitleVec2{std::cos(a), -std::sin(a)} : TitleVec2{std::cos(a), std::sin(a)};
    }
    double angleOf(TitleVec2 v, AngleConvention c) {
        return (c == AngleConvention::ClockwiseFromRight ? std::atan2(-v.y, v.x) : std::atan2(v.y, v.x)) / DEG;
    }
    double spaceScale(const Affine2& a) { return std::sqrt(std::abs(a.determinant())); }

    double num(const Json& o, const char* key, double fallback) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }
    bool flagOf(const Json& o, const char* key, bool fallback) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }
    std::string strOf(const Json& o, const char* key, const char* fallback) {
        if (!o.is_object()) return fallback;
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
    }
    const Json& member(const Json& o, const char* key) {
        static const Json none;
        if (!o.is_object()) return none;
        auto it = o.find(key);
        return it != o.end() ? *it : none;
    }
    double defaultNumber(const ParamDef& p) { return p.defaultValue.is_number() ? p.defaultValue.get<double>() : 0.0; }
    double clampTo(double v, double min, double max) { return min < max ? std::clamp(v, min, max) : v; }
    Json vec2Json(TitleVec2 p) { return Json::array({p.x, p.y}); }
    std::string degrees(double v) { return formatReadout(v) + DEGREE; }
    const char* unitSuffix(ParamUnit unit) {
        switch (unit) {
            case ParamUnit::Px: return " px";
            case ParamUnit::Percent: return " %";
            case ParamUnit::Deg: return DEGREE;
            case ParamUnit::Factor: return "x";
            case ParamUnit::None: break;
        }
        return "";
    }

    // ---- gradient.ts ----
    TitleVec2 gradientDir(double angle) { return {std::cos(angle * DEG), -std::sin(angle * DEG)}; }
    double gradientAngle(TitleVec2 d) {
        const double a = std::atan2(-d.y, d.x) / DEG;
        return a < 0.0 ? a + 360.0 : a;
    }
    TitleVec2 halfOf(const TitleRect& b) { return {b.width * 0.5, b.height * 0.5}; }
    TitleVec2 centreOf(const TitleRect& b) { return {b.x + b.width * 0.5, b.y + b.height * 0.5}; }
    double extOf(TitleVec2 d, TitleVec2 hs) { return std::max(1e-3, std::abs(d.x) * hs.x + std::abs(d.y) * hs.y); }
    double clampOffset(double o) { return std::clamp(o, -1.0, 1.0); }
    bool radialLike(const std::string& kind) { return kind == "radial" || kind == "diamond"; }
    // the radius at which t = 1 for scale 1, offset 0
    double radiusOf(const std::string& kind, TitleVec2 hs) { return std::max(1e-3, kind == "radial" ? std::sqrt(hs.x * hs.x + hs.y * hs.y) : hs.x + hs.y); }

    // ---- shapeHandles.ts ----
    struct ShapeFields {
        bool corner = false;
        int points = 0;         // 1 sides, 2 points
        bool inner = false;     // an inner radius handle (Inner, not a ribbon's Notch)
        bool tail = false;      // a speech tail
    };
    ShapeFields shapeFields(const std::string& shape) {
        ShapeFields f;
        if (shape == "rect" || shape == "parallelogram" || shape == "shield") f.corner = true;
        else if (shape == "polygon") f.points = 1;   // its Corner stays a panel control: no flat top-left corner to grab
        else if (shape == "star" || shape == "burst") {
            f.points = 2;
            f.inner = true;
        } else if (shape == "speech") {
            f.corner = true;
            f.tail = true;
        } else if (shape == "shout") {
            f.points = 2;
            f.inner = true;
            f.tail = true;
        }
        return f;
    }

    void boundsOf(const std::vector<TitleVec2>& pts, double& x0, double& y0, double& x1, double& y1) {
        x0 = y0 = 1e300;
        x1 = y1 = -1e300;
        for (const TitleVec2& p : pts) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }

}

// ---- free functions ----

GlobalLight jasperengine::editor::titledesigner::globalLightOf(const Json& root) {
    const Json& light = member(root, "global_light");
    return {num(light, "angle", 120.0), num(light, "altitude", 30.0)};
}

double jasperengine::editor::titledesigner::globalAngleForParam(const GlobalLightBinding& binding, double paramAngle) {
    const double ccw = binding.convention == AngleConvention::ClockwiseFromRight ? -paramAngle : paramAngle;
    // angleFromGlobalLight: From reads G, Away G + 180, Floor -(G + 180)
    const double g = binding.relation == ta::LightRelation::From ? ccw : binding.relation == ta::LightRelation::Away ? ccw - 180.0 : -ccw - 180.0;
    return wrap180(g);
}

void jasperengine::editor::titledesigner::lightFromSphere(double dx, double dy, double radius, AngleConvention convention, bool snap, double& angle,
                                                               double& altitude) {
    const double d = radius > 0.0 ? std::min(1.0, std::sqrt(dx * dx + dy * dy) / radius) : 0.0;
    // screen y is down
    const double sign = convention == AngleConvention::CounterClockwiseFromRight ? -1.0 : 1.0;
    double a = std::atan2(sign * dy, dx) / DEG;
    if (a < 0.0) a += 360.0;
    double alt = std::acos(d) / DEG;
    if (snap) {
        a = std::round(a / 15.0) * 15.0;
        alt = std::round(alt / 5.0) * 5.0;
    }
    angle = std::fmod(std::round(a), 360.0);
    altitude = std::clamp(std::round(alt), 0.0, 90.0);
}

GradientPoints jasperengine::editor::titledesigner::gradientHandles(const GradientParams& g, const TitleRect& box) {
    GradientPoints out;
    const TitleVec2 c = centreOf(box), hs = halfOf(box);
    const double s = std::max(g.scale, 1e-3);
    out.c = c;
    if (radialLike(g.kind)) {
        // angle-free kinds: the handles lie on +x so they never jump while dragging
        const double r = radiusOf(g.kind, hs) * s;
        out.a = {c.x + g.offset * r, c.y};
        out.b = {c.x + (1.0 + g.offset) * r, c.y};
        return out;
    }
    const TitleVec2 d = gradientDir(g.angle);
    const double ext = extOf(d, hs);
    if (g.kind == "linear") {
        out.a = add(c, mul(d, (g.offset - 0.5) * 2.0 * ext * s));
        out.b = add(c, mul(d, (g.offset + 0.5) * 2.0 * ext * s));
    } else if (g.kind == "reflected") {
        out.a = add(c, mul(d, g.offset * ext * s));
        out.b = add(c, mul(d, (1.0 + g.offset) * ext * s));
    } else {
        out.hasA = false;
        out.a = c;
        out.b = add(c, mul(d, ext));
    }
    return out;
}

GradientParams jasperengine::editor::titledesigner::dragGradientHandle(const GradientParams& g, const TitleRect& box, bool start, TitleVec2 p, bool snap) {
    const TitleVec2 c = centreOf(box), hs = halfOf(box);
    const GradientPoints h = gradientHandles(g, box);
    const auto snapAngle = [&](double a) { return snap ? std::fmod(std::round(a / 15.0) * 15.0, 360.0) : r1(a); };
    GradientParams out = g;
    if (g.kind == "linear") {
        TitleVec2 a = start ? p : h.a, b = start ? h.b : p;
        if (snap) {
            // snap the line's direction, keeping the dragged handle's distance from the fixed one
            const TitleVec2 fixed = start ? b : a, moving = start ? a : b;
            const double length = len(sub(moving, fixed));
            const double sgn = start ? -1.0 : 1.0;
            const TitleVec2 d = gradientDir(snapAngle(gradientAngle(mul(sub(moving, fixed), sgn))));
            const TitleVec2 q = add(fixed, mul(d, sgn * length));
            if (start) a = q;
            else b = q;
        }
        const double l = len(sub(b, a));
        if (l < 1e-3) return g;
        const TitleVec2 d = mul(sub(b, a), 1.0 / l);
        const double ext = extOf(d, hs);
        const TitleVec2 m = sub(mul(add(a, b), 0.5), c);
        out.angle = snapAngle(gradientAngle(d));
        out.scale = r3(std::max(MIN_GRADIENT_SCALE, l / (2.0 * ext)));
        out.offset = r3(clampOffset(dot(m, d) / l));
        return out;
    }
    const TitleVec2 v = sub(p, c);
    const double s = std::max(g.scale, 1e-3);
    if (g.kind == "reflected") {
        if (!start) {
            const double l = len(v);
            if (l < 1e-3) return g;
            out.angle = snapAngle(gradientAngle(mul(v, 1.0 / l)));
            const double ext = extOf(gradientDir(out.angle), hs);
            out.scale = r3(std::max(MIN_GRADIENT_SCALE, l / ((1.0 + g.offset) * ext)));
            return out;
        }
        const TitleVec2 d = gradientDir(g.angle);
        out.offset = r3(clampOffset(dot(v, d) / (extOf(d, hs) * s)));
        return out;
    }
    if (radialLike(g.kind)) {
        const double r = radiusOf(g.kind, hs);
        const double dist = g.kind == "radial" ? len(v) : std::abs(v.x) + std::abs(v.y);
        if (!start) {
            out.scale = r3(std::max(MIN_GRADIENT_SCALE, dist / std::max(0.05, 1.0 + g.offset) / r));
            return out;
        }
        out.offset = r3(clampOffset(v.x / (r * s)));
        return out;
    }
    // angle: the handle sets the start direction
    if (len(v) < 1e-3) return g;
    out.angle = snapAngle(gradientAngle(v));
    return out;
}

GradientParams jasperengine::editor::titledesigner::dragGradientLine(const GradientParams& g, const TitleRect& box, TitleVec2 delta) {
    const TitleVec2 hs = halfOf(box);
    const double s = std::max(g.scale, 1e-3);
    GradientParams out = g;
    if (g.kind == "linear" || g.kind == "reflected") {
        const TitleVec2 d = gradientDir(g.angle);
        const double span = (g.kind == "linear" ? 2.0 : 1.0) * extOf(d, hs) * s;
        out.offset = r3(clampOffset(g.offset + dot(delta, d) / span));
    } else if (radialLike(g.kind)) {
        out.offset = r3(clampOffset(g.offset + delta.x / (radiusOf(g.kind, hs) * s)));
    }
    return out;
}

// ---- TitleHandles: building ----

void TitleHandles::clear() {
    // the gesture survives: the canvas rebuilds the handles every frame from the staged document
    handles.clear();
    lightSlots = 0;
}

void TitleHandles::push(Handle h) {
    refreshLabel(h);
    handles.push_back(std::move(h));
}

TitleVec2 TitleHandles::boxCentreDoc(const HandleFrame& frame) const {
    return frame.layerToDoc.apply(centreOf(frame.localBox));
}

void TitleHandles::addHint(const HandleFrame& frame, const GizmoHint& hint, const std::vector<ParamDef>& defs, const Json& params,
                           const std::string& paramsPath, const GlobalLightBinding& binding, const char* owner, const GlobalLight& light) {
    if (!hint.param) return;
    const ParamDef* p = ta::findParam(defs, hint.param);
    if (!p) return;
    // a param its row hides (`when`) has no handle either: a solid glow has no gradient angle
    if (p->when.active()) {
        const Json& gate = member(params, p->when.param);
        const std::string value = gate.is_string() ? gate.get<std::string>() : gate.is_boolean() ? (gate.get<bool>() ? "true" : "false") : std::string();
        if (std::none_of(p->when.values.begin(), p->when.values.end(), [&](const char* v) { return v && value == v; })) return;
    }
    Handle h;
    h.layer = h.layer2 = frame.layerId;
    h.owner = owner ? owner : "";
    h.field = p->label;
    h.frame = frame;
    h.path = paramsPath + "." + hint.param;
    h.min = p->min;
    h.max = p->max;
    h.unit = p->unit;
    const double value = num(params, hint.param, defaultNumber(*p));
    const bool bound = binding.angleKey && std::string_view(binding.angleKey) == hint.param;
    const bool follows = bound && flagOf(params, "use_global_light", false);
    const ParamDef* second = hint.param2 ? ta::findParam(defs, hint.param2) : nullptr;
    switch (hint.kind) {
        case ta::GizmoKind::Point:
            h.kind = HandleKind::Point;
            h.space = frame.pointToDoc;
            h.at = ta::decodeVec2(member(params, hint.param).is_null() ? p->defaultValue : member(params, hint.param));
            break;
        case ta::GizmoKind::Direction:
        case ta::GizmoKind::Light:
            h.kind = hint.kind == ta::GizmoKind::Direction ? HandleKind::Direction : HandleKind::Light;
            // effect and generator directions are screen-fixed, like the light
            h.space = Affine2();
            h.at = boxCentreDoc(frame);
            h.convention = bound ? binding.convention
                                 : (hint.kind == ta::GizmoKind::Light ? AngleConvention::CounterClockwiseFromRight : AngleConvention::ClockwiseFromRight);
            h.binding = binding;
            if (!bound) {
                // an unbound angle keeps its own convention for the global-light maths it never uses
                h.binding.relation = ta::LightRelation::From;
                h.binding.convention = h.convention;
            }
            h.value = follows ? ta::angleFromGlobalLight(binding, light.angle) : value;
            if (follows) {
                h.global = true;
                h.layer.clear();
                h.path = "global_light.angle";
            }
            if (second) {
                h.hasSecond = true;
                h.text = second->label;
                h.min2 = second->min;
                h.max2 = second->max;
                if (hint.kind == ta::GizmoKind::Direction) h.unit = second->unit;
                h.bindingAltitude = hint.kind == ta::GizmoKind::Light && follows && binding.altitudeKey &&
                                    std::string_view(binding.altitudeKey) == hint.param2;
                h.value2 = h.bindingAltitude ? light.altitude : num(params, hint.param2, defaultNumber(*second));
                h.path2 = h.bindingAltitude ? std::string("global_light.altitude") : paramsPath + "." + hint.param2;
                if (h.bindingAltitude) h.layer2.clear();
            }
            if (h.kind == HandleKind::Light) h.slot = lightSlots++;
            break;
        case ta::GizmoKind::Radius:
            h.kind = HandleKind::Radius;
            h.space = frame.layerToDoc;
            h.value = value;
            break;
    }
    push(std::move(h));
}

void TitleHandles::addEffect(const HandleFrame& frame, const EffectDef& def, const Json& params, const std::string& paramsPath, const GlobalLight& light) {
    for (const GizmoHint& hint : def.gizmos) addHint(frame, hint, def.params, params, paramsPath, def.globalLight, def.label, light);
}

void TitleHandles::addGenerator(const HandleFrame& frame, const GeneratorDef& def, const Json& params, const std::string& paramsPath, const GlobalLight& light) {
    for (const GizmoHint& hint : def.gizmos) addHint(frame, hint, def.params, params, paramsPath, def.globalLight, def.label, light);
}

void TitleHandles::addFill(const HandleFrame& frame, const Json& fill, const std::string& fillPath, const std::string& owner) {
    const std::string kind = strOf(fill, "kind", "solid");
    if (kind == "angle") {
        addAngleDial(frame, fillPath + ".angle", num(fill, "angle", 90.0), owner, "Angle", AngleConvention::ClockwiseFromRight, frame.layerToDoc);
        return;
    }
    if (kind != "linear" && kind != "radial" && kind != "reflected" && kind != "diamond") return;
    if (frame.localBox.empty()) return;
    Handle h;
    h.layer = h.layer2 = frame.layerId;
    h.owner = owner;
    h.field = "Gradient";
    h.frame = frame;
    h.space = frame.layerToDoc;
    h.path = fillPath + ".angle";
    h.path2 = fillPath + ".scale";
    h.path3 = fillPath + ".offset";
    h.value = num(fill, "angle", 90.0);
    h.value2 = num(fill, "scale", 1.0);
    h.value3 = num(fill, "offset", 0.0);
    h.text = kind;
    h.box = frame.localBox;
    // the line under its handles: the line, then t = 0, then t = 1
    for (HandleKind k : {HandleKind::GradientLine, HandleKind::GradientStart, HandleKind::GradientEnd}) {
        Handle one = h;
        one.kind = k;
        push(std::move(one));
    }
}

void TitleHandles::addAngleDial(const HandleFrame& frame, const std::string& path, double angle, const std::string& owner, const std::string& field,
                                AngleConvention convention, const Affine2& space) {
    Handle h;
    h.kind = HandleKind::AngleDial;
    h.layer = h.layer2 = frame.layerId;
    h.owner = owner;
    h.field = field;
    h.frame = frame;
    h.space = space;
    h.convention = convention;
    h.path = path;
    h.value = angle;
    // `at` is in the angle's space, like every handle's
    h.at = space.inverse().apply(boxCentreDoc(frame));
    push(std::move(h));
}

void TitleHandles::addBevelLight(const HandleFrame& frame, const Json& bevel, const std::string& path, const GlobalLight& light) {
    Handle h;
    h.kind = HandleKind::Light;
    h.owner = "Bevel";
    h.field = "Light";
    h.frame = frame;
    h.space = Affine2();
    h.convention = AngleConvention::CounterClockwiseFromRight;
    h.binding.relation = ta::LightRelation::From;
    h.binding.convention = AngleConvention::CounterClockwiseFromRight;
    h.hasSecond = true;
    h.text = "Altitude";
    h.min2 = 0.0;
    h.max2 = 90.0;
    h.unit = ParamUnit::Deg;
    // the bevel's light is screen-fixed (the renderer turns it into the layer)
    h.global = h.bindingAltitude = flagOf(bevel, "use_global_light", true);
    if (h.global) {
        h.value = light.angle;
        h.value2 = light.altitude;
        h.path = "global_light.angle";
        h.path2 = "global_light.altitude";
    } else {
        h.layer = h.layer2 = frame.layerId;
        h.value = num(bevel, "light_angle", 120.0);
        h.value2 = num(bevel, "light_altitude", 30.0);
        h.path = path + ".light_angle";
        h.path2 = path + ".light_altitude";
    }
    h.slot = lightSlots++;
    push(std::move(h));
}

void TitleHandles::addSatin(const HandleFrame& frame, const Json& satin, const std::string& path, const GlobalLight& light) {
    Handle h;
    h.kind = HandleKind::Light;
    h.owner = "Satin";
    h.field = "Angle";
    h.frame = frame;
    h.convention = AngleConvention::CounterClockwiseFromRight;
    h.binding.relation = ta::LightRelation::From;
    h.binding.convention = AngleConvention::CounterClockwiseFromRight;
    h.global = flagOf(satin, "use_global_light", false);
    if (h.global) {
        h.space = Affine2();
        h.value = light.angle;
        h.path = "global_light.angle";
    } else {
        // the satin's own angle turns with the layer
        h.layer = h.layer2 = frame.layerId;
        h.space = frame.layerToDoc;
        h.value = num(satin, "angle", 19.0);
        h.path = path + ".angle";
    }
    h.slot = lightSlots++;
    push(std::move(h));
}

void TitleHandles::addDistort(const HandleFrame& frame, const TitleRect& distortFrame, const Json& distort, const std::string& path) {
    if (distortFrame.width < 1e-6 || distortFrame.height < 1e-6) return;
    const std::string mode = strOf(distort, "mode", "none");
    // `none` edits as corners: the first drag switches the distort on
    const bool mesh = mode == "mesh";
    const size_t n = mesh ? 16 : 4;
    std::vector<TitleVec2> offsets(n);
    const Json& list = member(distort, mesh ? "mesh" : "corners");
    for (size_t i = 0; i < n && list.is_array() && i < list.size(); i++) offsets[i] = ta::decodeVec2(list[i]);
    Handle h;
    h.layer = h.layer2 = frame.layerId;
    h.owner = "Distort";
    h.field = mesh ? "Mesh" : "Corners";
    h.frame = frame;
    h.space = frame.layerToDoc;
    h.path = path;
    h.text = mode;
    h.flag = mesh;
    h.box = distortFrame;
    h.list = offsets;
    for (size_t i = 0; i < n; i++) {
        Handle one = h;
        one.kind = HandleKind::DistortPoint;
        one.index = (int)i;
        push(std::move(one));
    }
    if (mode != "none") {
        Handle reset = h;
        reset.kind = HandleKind::DistortReset;
        reset.field = "Reset";
        push(std::move(reset));
    }
}

void TitleHandles::addShape(const HandleFrame& frame, const Json& shape, const std::string& prefix, const std::vector<TitleVec2>& vertices,
                            const std::function<TitleVec2(TitleVec2)>& warp) {
    if (vertices.empty()) return;
    const std::string kind = strOf(shape, "shape", "rect");
    const ShapeFields f = shapeFields(kind);
    const double size = num(shape, "size", 200.0);
    double x0, y0, x1, y1;
    boundsOf(vertices, x0, y0, x1, y1);
    const auto place = [&](TitleVec2 p) { return warp ? warp(p) : p; };
    Handle base;
    base.layer = base.layer2 = frame.layerId;
    base.owner = "Shape";
    base.frame = frame;
    base.space = frame.layerToDoc;
    base.text = kind;
    base.size = size;
    base.aspect = num(shape, "aspect", 1.0);
    if (f.corner) {
        // where the top-left corner arc meets the flat top edge (the leftmost on-curve point on the
        // top line); the corner's virtual vertex is that point less the radius
        const double eps = (y1 - y0) * 1e-4;
        bool found = false;
        TitleVec2 at;
        for (const TitleVec2& v : vertices) {
            if (std::abs(v.y - y1) <= eps && (!found || v.x < at.x)) {
                at = v;
                found = true;
            }
        }
        if (found) {
            const double corner = num(shape, "corner", 0.0);
            const double r = std::min(std::max(0.0, corner) / 100.0 * size, (y1 - y0) / 2.0);
            Handle h = base;
            h.kind = HandleKind::ShapeCorner;
            h.field = "Corner";
            h.path = prefix + "corner";
            h.value = corner;
            h.extra = at.x - r;
            h.at = place(at);
            push(std::move(h));
        }
    }
    if (f.inner) {
        // the deepest valley: the vertex nearest the centre
        TitleVec2 best = vertices[0];
        for (const TitleVec2& v : vertices) {
            if (len(v) < len(best)) best = v;
        }
        Handle h = base;
        h.kind = HandleKind::ShapeInner;
        h.field = "Inner";
        h.path = prefix + "inner";
        h.value = num(shape, "inner", 0.5);
        h.at = place(best);
        push(std::move(h));
    }
    if (f.points) {
        TitleVec2 at{(x0 + x1) * 0.5, y1};
        if (f.points == 2) {
            // the topmost vertex: the first tip points straight up
            at = vertices[0];
            for (const TitleVec2& v : vertices) {
                if (v.y > at.y) at = v;
            }
        }
        Handle h = base;
        h.kind = HandleKind::ShapePoints;
        h.field = f.points == 1 ? "Sides" : "Points";
        h.path = prefix + "points";
        h.value = num(shape, "points", 5.0);
        h.flag = f.points == 1;
        h.at = place(at);
        push(std::move(h));
    }
    if (f.tail) {
        const Json& tail = member(shape, "tail");
        Handle h = base;
        h.kind = HandleKind::ShapeTail;
        h.field = "Tail";
        h.path = prefix + "tail.x";
        h.path2 = prefix + "tail.y";
        h.value = num(tail, "x", 0.0);
        h.value2 = num(tail, "y", -60.0);
        h.at = place({h.value / 100.0 * size, h.value2 / 100.0 * size});
        push(std::move(h));
    }
}

void TitleHandles::addWarpBend(const HandleFrame& frame, const Json& warp, const std::string& path, bool flipX, bool flipY) {
    if (strOf(warp, "style", "none") == "none" || frame.localBox.empty()) return;
    const TitleRect& b = frame.localBox;
    Handle h;
    h.kind = HandleKind::WarpBend;
    h.layer = h.layer2 = frame.layerId;
    h.owner = "Warp";
    h.field = "Bend";
    h.frame = frame;
    h.space = frame.layerToDoc;
    h.path = path + ".bend";
    h.value = num(warp, "bend", 0.0);
    h.flag = flipY;
    // off the top edge at three quarters across (on screen, whatever the flips), clear of the
    // rotate and point-count handles
    h.at = {flipX ? b.x + b.width - b.width * 0.75 : b.x + b.width * 0.75, flipY ? b.y : b.y + b.height};
    push(std::move(h));
}

void TitleHandles::addMask(const HandleFrame& frame, const Json& mask, const std::string& path) {
    const std::string type = strOf(mask, "type", "shape");
    if (type != "gradient" && type != "shape" && type != "image") return;
    const TitleVec2 position = ta::decodeVec2(member(mask, "position"));
    const std::string owner = type == "gradient" ? "Gradient Mask" : type == "shape" ? "Shape Mask" : "Image Mask";
    Handle point;
    point.kind = HandleKind::Point;
    point.layer = point.layer2 = frame.layerId;
    point.owner = owner;
    point.field = "Position";
    point.frame = frame;
    point.space = frame.layerToDoc;
    point.path = path + ".position";
    point.at = position;
    if (type == "gradient") {
        Handle dir;
        dir.kind = HandleKind::Direction;
        dir.layer = dir.layer2 = frame.layerId;
        dir.owner = owner;
        dir.field = "Angle";
        dir.frame = frame;
        dir.space = frame.layerToDoc;
        dir.convention = AngleConvention::ClockwiseFromRight;
        dir.binding.relation = ta::LightRelation::From;
        dir.binding.convention = AngleConvention::ClockwiseFromRight;
        dir.at = position;
        dir.path = path + ".angle";
        dir.value = num(mask, "angle", 0.0);
        dir.min = -360.0;
        dir.max = 360.0;
        dir.hasSecond = true;
        dir.text = "Length";
        dir.unit = ParamUnit::Px;
        dir.path2 = path + ".length";
        dir.value2 = num(mask, "length", 200.0);
        dir.min2 = 1.0;
        dir.max2 = 16384.0;
        push(std::move(dir));
    }
    push(std::move(point));
}

void TitleHandles::addNode(const Json& root, const NodeRef& node, const HandleFrame& frame, const HandleNodeExtras& extras) {
    if (node.kind == NodeKind::Document) return;
    const Json* layer = ta::findLayer(root, node.layer);
    if (!layer) return;
    const GlobalLight light = globalLightOf(root);
    const auto indexById = [](const Json& list, const std::string& id) -> int {
        if (!list.is_array()) return -1;
        for (size_t i = 0; i < list.size(); i++) {
            if (strOf(list[i], "id", "") == id) return (int)i;
        }
        return -1;
    };
    switch (node.kind) {
        case NodeKind::Effect: {
            const Json& stack = node.part.empty() ? member(*layer, "effects") : member(member(*layer, "part_effects"), node.part.c_str());
            const int i = indexById(stack, node.item);
            if (i < 0) return;
            const Json& entry = stack[(size_t)i];
            const ta::EffectDef* def = ta::findEffect(strOf(entry, "type", ""));
            if (!def) return;
            const std::string path = node.part.empty() ? "effects[" + std::to_string(i) + "].params"
                                                       : "part_effects." + node.part + "[" + std::to_string(i) + "].params";
            addEffect(frame, *def, member(entry, "params"), path, light);
            return;
        }
        case NodeKind::Part: {
            std::string path, owner;
            const Json* material = nullptr;
            const Json& style = member(*layer, "style");
            if (node.part == "face") {
                path = "style.face";
                owner = "Face";
                material = &member(style, "face");
            } else if (node.part.rfind("rim:", 0) == 0) {
                const int i = indexById(member(style, "rims"), node.part.substr(4));
                if (i < 0) return;
                path = "style.rims[" + std::to_string(i) + "].material";
                owner = "Rim " + std::to_string(i + 1);
                material = &member(member(style, "rims")[(size_t)i], "material");
            }
            if (!material || !material->is_object()) return;
            addFill(frame, member(*material, "fill"), path + ".fill", owner);
            const Json& bevel = member(*material, "bevel");
            if (flagOf(bevel, "enabled", false)) addBevelLight(frame, bevel, path + ".bevel", light);
            const Json& satin = member(*material, "satin");
            if (flagOf(satin, "enabled", false)) addSatin(frame, satin, path + ".satin", light);
            return;
        }
        case NodeKind::Layer: {
            const ta::LayerType type = ta::layerTypeOf(*layer);
            if (type == ta::LayerType::Generator) {
                const Json& generator = member(*layer, "generator");
                const ta::GeneratorDef* def = ta::findGenerator(strOf(generator, "type", ""));
                if (def) addGenerator(frame, *def, member(generator, "params"), "generator.params", light);
                return;
            }
            if (type == ta::LayerType::Shape && extras.shapeVertices) addShape(frame, *layer, "", *extras.shapeVertices, extras.warp);
            const ta::TitleTransform t = ta::decodeTransform(member(*layer, "transform"));
            const Json& warp = member(*layer, "warp");
            if (warp.is_object()) addWarpBend(frame, warp, "warp", t.flipX, t.flipY);
            const Json& distort = member(*layer, "distort");
            if (strOf(distort, "mode", "none") != "none") addDistort(frame, extras.distortFrame ? *extras.distortFrame : frame.localBox, distort, "distort");
            return;
        }
        case NodeKind::Mask: {
            const Json& masks = member(*layer, "masks");
            const int i = indexById(masks, node.item);
            if (i < 0) return;
            const Json& mask = masks[(size_t)i];
            // linked masks ride the layer; unlinked ones are canvas space
            HandleFrame maskFrame = frame;
            if (!flagOf(mask, "linked", true)) maskFrame.layerToDoc = Affine2();
            addMask(maskFrame, mask, "masks[" + std::to_string(i) + "]");
            return;
        }
        case NodeKind::Document: return;
    }
}

// ---- TitleHandles: labels and positions ----

void TitleHandles::refreshLabel(Handle& h) const {
    switch (h.kind) {
        case HandleKind::Point:
            h.label = h.field + " " + formatReadout(h.at.x) + ", " + formatReadout(h.at.y);
            break;
        case HandleKind::Direction:
        case HandleKind::Light: {
            const std::string angle = h.global ? "Global Light " + degrees(globalAngleForParam(h.binding, h.value)) : h.field + " " + degrees(h.value);
            std::string s = angle;
            if (h.hasSecond) {
                s += h.kind == HandleKind::Light ? " / " + degrees(h.value2) : std::string(DOT) + h.text + " " + formatReadout(h.value2) + unitSuffix(h.unit);
            }
            h.label = s;
            break;
        }
        case HandleKind::Radius:
            h.label = h.field + " " + formatReadout(h.value) + unitSuffix(h.unit);
            break;
        case HandleKind::AngleDial:
            h.label = h.field + " " + degrees(h.value);
            break;
        case HandleKind::GradientEnd:
            h.label = h.owner + (radialLike(h.text) ? std::string(" \xC3\x97") + formatReadout(h.value2, 2) : " " + degrees(h.value));
            break;
        case HandleKind::GradientStart:
        case HandleKind::GradientLine:
            h.label.clear();
            break;
        case HandleKind::DistortPoint: {
            const TitleVec2 o = h.list[(size_t)h.index];
            h.label = formatReadout(o.x * 100.0) + " %, " + formatReadout(o.y * 100.0) + " %";
            break;
        }
        case HandleKind::DistortReset:
            h.label = "Reset";
            break;
        case HandleKind::ShapeCorner:
            h.label = "Corner " + formatReadout(h.value) + " %";
            break;
        case HandleKind::ShapeInner:
            h.label = "Inner " + formatReadout(h.value * 100.0) + " %";
            break;
        case HandleKind::ShapePoints:
            h.label = h.field + " " + formatReadout(h.value, 0);
            break;
        case HandleKind::ShapeTail:
            h.label = "Tail";
            break;
        case HandleKind::WarpBend:
            h.label = "Bend " + formatReadout(h.value) + " %";
            break;
    }
}

ImVec2 TitleHandles::anchorScreen(const Handle& h) const {
    return view.toScreen(h.space.apply(h.at));
}

ImVec2 TitleHandles::dialCentre(const Handle& h) const {
    // just up-right of the box's top-right corner on screen, one dial under the other
    const TitleRect& b = h.frame.localBox;
    const TitleVec2 corners[4] = {{b.x, b.y}, {b.x + b.width, b.y}, {b.x + b.width, b.y + b.height}, {b.x, b.y + b.height}};
    float maxX = -1e30f, minY = 1e30f;
    for (const TitleVec2& c : corners) {
        const ImVec2 s = view.toScreen(h.frame.layerToDoc.apply(c));
        maxX = std::max(maxX, s.x);
        minY = std::min(minY, s.y);
    }
    return ImVec2(maxX + LIGHT_DIAL_R + 10.0f, minY - LIGHT_DIAL_R - 10.0f + h.slot * (2.0f * LIGHT_DIAL_R + 30.0f));
}

ImVec2 TitleHandles::positionOf(const Handle& h) const {
    switch (h.kind) {
        case HandleKind::Point:
            return anchorScreen(h);
        case HandleKind::Direction: {
            const ImVec2 a = anchorScreen(h);
            const TitleVec2 d = h.space.applyLinear(dirOf(h.value, h.convention));
            const double dl = len(d);
            if (dl < 1e-12) return a;
            const float length = h.hasSecond ? (float)(h.value2 * dl * view.zoom) : ARROW_PX;
            return ImVec2(a.x + (float)(d.x / dl) * length, a.y - (float)(d.y / dl) * length);
        }
        case HandleKind::Radius: {
            const double scale = spaceScale(h.space);
            const double r = h.unit == ParamUnit::Percent ? h.value / 100.0 * h.frame.sourceSize : (scale > 1e-12 ? h.value / scale : 0.0);
            const TitleRect& b = h.frame.localBox;
            return view.toScreen(h.space.apply({b.x + b.width + r, b.y + b.height * 0.5}));
        }
        case HandleKind::Light: {
            const ImVec2 c = dialCentre(h);
            const TitleVec2 d = h.space.applyLinear(dirOf(h.value, h.convention));
            const double dl = len(d);
            const float r = LIGHT_DIAL_R * (float)std::cos((h.hasSecond ? std::clamp(h.value2, 0.0, 90.0) : 0.0) * DEG);
            if (dl < 1e-12) return c;
            return ImVec2(c.x + (float)(d.x / dl) * r, c.y - (float)(d.y / dl) * r);
        }
        case HandleKind::AngleDial: {
            const ImVec2 c = anchorScreen(h);
            const TitleVec2 d = h.space.applyLinear(dirOf(h.value, h.convention));
            const double dl = len(d);
            if (dl < 1e-12) return c;
            return ImVec2(c.x + (float)(d.x / dl) * ANGLE_DIAL_R, c.y - (float)(d.y / dl) * ANGLE_DIAL_R);
        }
        case HandleKind::GradientStart:
        case HandleKind::GradientEnd:
        case HandleKind::GradientLine: {
            const GradientPoints g = gradientHandles({h.text, h.value, h.value2, h.value3}, h.box);
            const TitleVec2 p = h.kind == HandleKind::GradientStart ? g.a : h.kind == HandleKind::GradientEnd ? g.b : g.c;
            return view.toScreen(h.space.apply(p));
        }
        case HandleKind::DistortPoint: {
            const TitleRect& f = h.box;
            const int i = h.index;
            TitleVec2 rest;
            if (!h.flag) rest = {i == 0 || i == 3 ? f.x : f.x + f.width, i < 2 ? f.y + f.height : f.y};
            else rest = {f.x + (i % 4) / 3.0 * f.width, f.y + f.height - (i / 4) / 3.0 * f.height};
            const TitleVec2 o = h.list[(size_t)i];
            return view.toScreen(h.space.apply({rest.x + o.x * f.width, rest.y + o.y * f.height}));
        }
        case HandleKind::DistortReset: {
            // centred under the lowest point of the lattice
            Handle probe = h;
            probe.kind = HandleKind::DistortPoint;
            float sumX = 0.0f, maxY = -1e30f;
            const int n = (int)h.list.size();
            for (int i = 0; i < n; i++) {
                probe.index = i;
                const ImVec2 p = positionOf(probe);
                sumX += p.x;
                maxY = std::max(maxY, p.y);
            }
            return ImVec2(n > 0 ? sumX / n : 0.0f, maxY + 23.0f);
        }
        case HandleKind::ShapeCorner:
        case HandleKind::ShapeInner:
        case HandleKind::ShapePoints:
        case HandleKind::ShapeTail:
            return anchorScreen(h);
        case HandleKind::WarpBend: {
            const ImVec2 a = anchorScreen(h);
            return ImVec2(a.x, a.y - BEND_STEM);
        }
    }
    return ImVec2(0.0f, 0.0f);
}

ImVec2 TitleHandles::screenPosition(int index) const {
    return positionOf(handles[(size_t)index]);
}

int TitleHandles::hitTest(ImVec2 s) const {
    // points first (topmost: the last added), then lines and dials' rings
    for (int i = (int)handles.size() - 1; i >= 0; i--) {
        const Handle& h = handles[(size_t)i];
        if (h.kind == HandleKind::GradientLine) continue;
        const ImVec2 p = positionOf(h);
        if (h.kind == HandleKind::DistortReset) {
            if (std::abs(s.x - p.x) <= 24.0f && std::abs(s.y - p.y) <= 9.0f) return i;
            continue;
        }
        if (screenDistance(s, p) <= HIT) return i;
    }
    for (int i = (int)handles.size() - 1; i >= 0; i--) {
        const Handle& h = handles[(size_t)i];
        if (h.kind == HandleKind::GradientLine) {
            const GradientPoints g = gradientHandles({h.text, h.value, h.value2, h.value3}, h.box);
            const ImVec2 from = view.toScreen(h.space.apply(h.text == "linear" ? g.a : g.c)), to = view.toScreen(h.space.apply(g.b));
            if (segmentDistance(s, from, to) <= LINE_HIT) return i;
        } else if (h.kind == HandleKind::AngleDial) {
            if (std::abs(screenDistance(s, anchorScreen(h)) - ANGLE_DIAL_R) <= 6.0f) return i;
        } else if (h.kind == HandleKind::Light) {
            if (screenDistance(s, dialCentre(h)) <= LIGHT_DIAL_R) return i;
        }
    }
    return -1;
}

// ---- TitleHandles: gestures ----

bool TitleHandles::begin(int index, ImVec2 screen, const GizmoModifiers&) {
    if (index < 0 || index >= (int)handles.size()) return false;
    activeIndex = index;
    start = handles[(size_t)index];
    pressDoc = view.toDoc(screen);
    pressScreen = screen;
    return true;
}

void TitleHandles::end() {
    activeIndex = -1;
}

std::string TitleHandles::gestureName() const {
    if (activeIndex < 0) return "";
    if (start.global) return "Change Global Light";
    if (start.kind == HandleKind::DistortReset) return "Reset Distort";
    return "Change " + start.owner + " " + start.field;
}

std::vector<FieldEdit> TitleHandles::update(ImVec2 screen, const GizmoModifiers& modifiers, const SnapContext* snap) {
    if (activeIndex < 0) return {};
    const SnapSettings increments = snap ? snap->settings : SnapSettings();
    const bool snapping = snap && snap->settings.enabled && !modifiers.ctrl;
    const TitleVec2 m = view.toDoc(screen);
    const Handle& s = start;
    Handle now = s;
    std::vector<FieldEdit> edits;
    const auto snapAngle = [&](double a) {
        if (modifiers.shift) return std::round(a / 15.0) * 15.0;
        if (modifiers.ctrl) return incrementAngle(a, increments);
        return r1(a);
    };
    // the angle a handle writes: the param's own, or the global light's that makes it read so
    const auto angleEdit = [&](const Handle& h, double paramAngle) {
        const double v = h.global ? globalAngleForParam(h.binding, paramAngle) : clampTo(wrap180(paramAngle), h.min, h.max);
        edits.push_back({h.layer, h.path, v});
    };
    const Affine2 inv = s.space.inverse();

    switch (s.kind) {
        case HandleKind::Point: {
            const TitleVec2 startDoc = s.space.apply(s.at);
            TitleVec2 next = add(m, sub(startDoc, pressDoc));
            const TitleVec2 centre = boxCentreDoc(s.frame);
            const ImVec2 ns = view.toScreen(next), cs = view.toScreen(centre);
            if (modifiers.shift && screenDistance(ns, cs) <= SNAP_PX) {
                next = centre;
            } else if (snapping) {
                next = add(startDoc, snapPoint(*snap, startDoc, sub(next, startDoc)).delta);
            }
            now.at = inv.apply(next);
            edits.push_back({s.layer, s.path, vec2Json(now.at)});
            break;
        }
        case HandleKind::Direction: {
            const TitleVec2 anchor = s.space.apply(s.at);
            const TitleVec2 v = inv.applyLinear(sub(m, anchor));
            if (len(v) > 1e-9) now.value = wrap180(snapAngle(angleOf(v, s.convention)));
            angleEdit(s, now.value);
            if (s.hasSecond) {
                // the tip keeps its offset from the pointer
                const double pressed = len(inv.applyLinear(sub(pressDoc, anchor)));
                now.value2 = clampTo(r1(len(v) + (s.value2 - pressed)), s.min2, s.max2);
                if (s.min2 >= s.max2) now.value2 = std::max(0.0, now.value2);
                edits.push_back({s.layer2, s.path2, now.value2});
            }
            break;
        }
        case HandleKind::Radius: {
            const double scale = spaceScale(s.space);
            const double pressedLocal = inv.apply(pressDoc).x, nowLocal = inv.apply(m).x;
            const double r0 = s.unit == ParamUnit::Percent ? s.value / 100.0 * s.frame.sourceSize : (scale > 1e-12 ? s.value / scale : 0.0);
            const double r = std::max(0.0, r0 + (nowLocal - pressedLocal));
            double value = s.unit == ParamUnit::Percent ? (s.frame.sourceSize > 1e-12 ? r / s.frame.sourceSize * 100.0 : s.value) : r * scale;
            value = modifiers.ctrl ? snapToIncrement(value, 1.0) : r1(value);
            now.value = clampTo(value, s.min, s.max);
            edits.push_back({s.layer, s.path, now.value});
            break;
        }
        case HandleKind::Light: {
            const ImVec2 c = dialCentre(s);
            const double dx = screen.x - c.x, dy = screen.y - c.y;
            if (modifiers.shift && s.hasSecond) {
                // Shift is the only way to change the altitude, so a sweep never tips it
                now.value2 = std::clamp(std::round(std::acos(std::min(1.0, std::sqrt(dx * dx + dy * dy) / LIGHT_DIAL_R)) / DEG), 0.0, 90.0);
            } else if (std::sqrt(dx * dx + dy * dy) >= 2.0) {
                const TitleVec2 v = inv.applyLinear({dx, -dy});
                double a = angleOf(v, s.convention);
                a = modifiers.ctrl ? incrementAngle(a, increments) : std::round(a);
                now.value = wrap180(a);
            }
            angleEdit(s, now.value);
            if (s.hasSecond) edits.push_back({s.layer2, s.path2, s.bindingAltitude ? now.value2 : clampTo(now.value2, s.min2, s.max2)});
            break;
        }
        case HandleKind::AngleDial: {
            const TitleVec2 v = inv.applyLinear(sub(m, s.space.apply(s.at)));
            if (len(v) > 1e-9) now.value = wrap180(snapAngle(angleOf(v, s.convention)));
            edits.push_back({s.layer, s.path, now.value});
            break;
        }
        case HandleKind::GradientStart:
        case HandleKind::GradientEnd:
        case HandleKind::GradientLine: {
            const GradientParams g{s.text, s.value, s.value2, s.value3};
            const GradientParams r = s.kind == HandleKind::GradientLine ? dragGradientLine(g, s.box, sub(inv.apply(m), inv.apply(pressDoc)))
                                                                         : dragGradientHandle(g, s.box, s.kind == HandleKind::GradientStart, inv.apply(m), modifiers.shift);
            now.value = r.angle;
            now.value2 = r.scale;
            now.value3 = r.offset;
            edits.push_back({s.layer, s.path, r.angle});
            edits.push_back({s.layer, s.path2, r.scale});
            edits.push_back({s.layer, s.path3, r.offset});
            break;
        }
        case HandleKind::DistortPoint: {
            const TitleVec2 p = inv.apply(m), p0 = inv.apply(pressDoc);
            const double dx = (p.x - p0.x) / s.box.width, dy = (p.y - p0.y) / s.box.height;
            std::vector<TitleVec2> list = s.list;
            const int i = s.index;
            list[(size_t)i] = {r4(s.list[(size_t)i].x + dx), r4(s.list[(size_t)i].y + dy)};
            if (modifiers.shift) {
                // the mirrored point (left <-> right) moves the opposite way: symmetric パース
                const int mirror = s.flag ? (i / 4) * 4 + (3 - i % 4) : CORNER_MIRROR[i];
                if (mirror != i) list[(size_t)mirror] = {r4(s.list[(size_t)mirror].x - dx), r4(s.list[(size_t)mirror].y + dy)};
            }
            Json value = Json::array();
            for (const TitleVec2& o : list) value.push_back(vec2Json(o));
            edits.push_back({s.layer, s.path + (s.flag ? ".mesh" : ".corners"), value});
            if (s.text == "none") edits.push_back({s.layer, s.path + ".mode", "corners"});
            now.list = list;
            break;
        }
        case HandleKind::DistortReset: {
            // keeps the mode
            Json corners = Json::array(), mesh = Json::array();
            for (int i = 0; i < 4; i++) corners.push_back(Json::array({0, 0}));
            for (int i = 0; i < 16; i++) mesh.push_back(Json::array({0, 0}));
            edits.push_back({s.layer, s.path + ".corners", corners});
            edits.push_back({s.layer, s.path + ".mesh", mesh});
            break;
        }
        case HandleKind::ShapeCorner: {
            const TitleVec2 p = inv.apply(m);
            const double h = std::max(s.size, 1e-9);
            const double r = std::clamp(p.x - s.extra, 0.0, h / 2.0);
            const double pct = r / h * 100.0;
            now.value = std::min(50.0, modifiers.shift ? std::round(pct / 5.0) * 5.0 : r1(pct));
            edits.push_back({s.layer, s.path, now.value});
            break;
        }
        case HandleKind::ShapeInner: {
            const TitleVec2 p = inv.apply(m), p0 = inv.apply(pressDoc);
            const double d0 = len(p0);
            if (d0 > 1e-3) {
                const double v = std::clamp(s.value * len(p) / d0, INNER_MIN, INNER_MAX);
                now.value = modifiers.shift ? std::round(v * 20.0) / 20.0 : r3(v);
            }
            edits.push_back({s.layer, s.path, now.value});
            break;
        }
        case HandleKind::ShapePoints: {
            const double max = s.flag ? 16.0 : 64.0;
            now.value = std::clamp(std::round(s.value + (screen.x - pressScreen.x) / POINTS_DRAG_PX), 3.0, max);
            edits.push_back({s.layer, s.path, (int)now.value});
            break;
        }
        case HandleKind::ShapeTail: {
            TitleVec2 p = inv.apply(m);
            if (modifiers.shift) {
                // 15° steps around the centre
                const double l = len(p);
                const double a = std::round(std::atan2(p.y, p.x) / (PI / 12.0)) * (PI / 12.0);
                p = {std::cos(a) * l, std::sin(a) * l};
            }
            const double h = std::max(s.size, 1e-9);
            now.value = std::round(p.x / h * 1000.0) / 10.0;
            now.value2 = std::round(p.y / h * 1000.0) / 10.0;
            edits.push_back({s.layer, s.path, now.value});
            edits.push_back({s.layer, s.path2, now.value2});
            break;
        }
        case HandleKind::WarpBend: {
            // up arcs the layer's top upward; mirrored vertically, that is down on screen
            const double k = s.flag ? -BEND_PER_PX : BEND_PER_PX;
            double v = s.value - (screen.y - pressScreen.y) * k;
            v = modifiers.shift ? std::round(v / 10.0) * 10.0 : std::round(v);
            now.value = std::clamp(v, -100.0, 100.0);
            edits.push_back({s.layer, s.path, now.value});
            break;
        }
    }
    // the live label follows (the canvas may also rebuild the handles from the staged document)
    if (activeIndex < (int)handles.size() && handles[(size_t)activeIndex].kind == s.kind) {
        refreshLabel(now);
        handles[(size_t)activeIndex] = now;
    }
    return edits;
}

// ---- drawing ----

void TitleHandles::draw(ImDrawList* dl, int hovered, int activeHandle, bool highlight) const {
    if (!dl) return;
    for (int i = 0; i < (int)handles.size(); i++) {
        const Handle& h = handles[(size_t)i];
        const bool on = highlight || i == hovered || i == activeHandle;
        const ImU32 color = on ? overlay::ACCENT_LIT : overlay::WHITE;
        const ImVec2 p = positionOf(h);
        const ImVec2 labelAt(p.x + 12.0f, p.y - 12.0f);
        switch (h.kind) {
            case HandleKind::Point:
                overlay::line(dl, ImVec2(p.x - 9.0f, p.y), ImVec2(p.x + 9.0f, p.y), color, false);
                overlay::line(dl, ImVec2(p.x, p.y - 9.0f), ImVec2(p.x, p.y + 9.0f), color, false);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            case HandleKind::Direction: {
                const ImVec2 a = anchorScreen(h);
                const float dx = p.x - a.x, dy = p.y - a.y, l = std::sqrt(dx * dx + dy * dy);
                if (l > 1.0f) {
                    const ImVec2 u(dx / l, dy / l);
                    const ImVec2 base(p.x - u.x * 12.0f, p.y - u.y * 12.0f);
                    overlay::line(dl, a, base, color, on);
                    overlay::triangle(dl, p, ImVec2(base.x - u.y * 6.0f, base.y + u.x * 6.0f), ImVec2(base.x + u.y * 6.0f, base.y - u.x * 6.0f), color);
                }
                overlay::disc(dl, a, 3.0f, color, false);
                overlay::label(dl, labelAt, h.label, h.global ? LIGHT_COLOR : color, false);
                break;
            }
            case HandleKind::Radius: {
                const double scale = spaceScale(h.space);
                const double r = h.unit == ParamUnit::Percent ? h.value / 100.0 * h.frame.sourceSize : (scale > 1e-12 ? h.value / scale : 0.0);
                const TitleRect& b = h.frame.localBox;
                const TitleVec2 q[4] = {{b.x - r, b.y - r}, {b.x + b.width + r, b.y - r}, {b.x + b.width + r, b.y + b.height + r}, {b.x - r, b.y + b.height + r}};
                ImVec2 pts[4];
                for (int k = 0; k < 4; k++) pts[k] = view.toScreen(h.space.apply(q[k]));
                dl->AddPolyline(pts, 4, overlay::HALO_COLOR, 3.0f, ImDrawFlags_Closed);
                dl->AddPolyline(pts, 4, on ? overlay::ACCENT_LIT : overlay::ACCENT, 1.5f, ImDrawFlags_Closed);
                overlay::disc(dl, p, 5.0f, color, on);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            }
            case HandleKind::Light: {
                const ImVec2 c = dialCentre(h);
                dl->AddCircleFilled(c, LIGHT_DIAL_R, IM_COL32(27, 27, 33, 176));
                dl->AddCircle(c, LIGHT_DIAL_R, LIGHT_COLOR, 0, on ? 2.0f : 1.0f);
                dl->AddLine(c, p, LIGHT_COLOR, 1.5f);
                dl->AddCircleFilled(c, 2.0f, LIGHT_COLOR);
                dl->AddCircleFilled(p, on ? 7.0f : 6.0f, LIGHT_COLOR);
                dl->AddCircle(p, on ? 7.0f : 6.0f, IM_COL32(0, 0, 0, 255), 0, 1.0f);
                overlay::label(dl, ImVec2(c.x, c.y - LIGHT_DIAL_R - 10.0f), h.label, LIGHT_COLOR, true);
                break;
            }
            case HandleKind::AngleDial: {
                const ImVec2 c = anchorScreen(h);
                dl->AddCircle(c, ANGLE_DIAL_R, overlay::HALO_COLOR, 0, 3.5f);
                dl->AddCircle(c, ANGLE_DIAL_R, on ? overlay::ACCENT_LIT : overlay::ACCENT, 0, 1.5f);
                overlay::line(dl, c, p, color, false);
                overlay::disc(dl, p, 6.0f, color, on);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            }
            case HandleKind::GradientLine: {
                const GradientPoints g = gradientHandles({h.text, h.value, h.value2, h.value3}, h.box);
                const ImVec2 from = view.toScreen(h.space.apply(h.text == "linear" ? g.a : g.c)), to = view.toScreen(h.space.apply(g.b));
                overlay::line(dl, from, to, on ? overlay::ACCENT_LIT : overlay::ACCENT, on);
                if (h.text != "linear") overlay::disc(dl, p, 3.0f, overlay::WHITE, false);
                break;
            }
            case HandleKind::GradientStart:
                overlay::disc(dl, p, on ? 7.0f : 6.0f, color, on);
                break;
            case HandleKind::GradientEnd:
                overlay::disc(dl, p, on ? 7.0f : 6.0f, on ? overlay::ACCENT_LIT : overlay::ACCENT, on);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            case HandleKind::DistortPoint: {
                // the lattice once, with the first point: the frame's outline (corners) or the
                // control polygon's rows and columns (mesh)
                if (h.index == 0) {
                    Handle probe = h;
                    const int n = (int)h.list.size();
                    std::vector<ImVec2> pts((size_t)n);
                    for (int k = 0; k < n; k++) {
                        probe.index = k;
                        pts[(size_t)k] = positionOf(probe);
                    }
                    if (!h.flag && n == 4) {
                        dl->AddPolyline(pts.data(), 4, overlay::HALO_COLOR, 3.0f, ImDrawFlags_Closed);
                        dl->AddPolyline(pts.data(), 4, overlay::ACCENT, 1.0f, ImDrawFlags_Closed);
                    } else if (n == 16) {
                        for (int k = 0; k < 4; k++) {
                            const ImVec2 row[4] = {pts[(size_t)(k * 4)], pts[(size_t)(k * 4 + 1)], pts[(size_t)(k * 4 + 2)], pts[(size_t)(k * 4 + 3)]};
                            const ImVec2 col[4] = {pts[(size_t)k], pts[(size_t)(4 + k)], pts[(size_t)(8 + k)], pts[(size_t)(12 + k)]};
                            dl->AddPolyline(row, 4, overlay::ACCENT, 1.0f, ImDrawFlags_None);
                            dl->AddPolyline(col, 4, overlay::ACCENT, 1.0f, ImDrawFlags_None);
                        }
                    }
                }
                const bool corner = !h.flag || h.index == 0 || h.index == 3 || h.index == 12 || h.index == 15;
                if (corner) overlay::square(dl, p, on ? 12.0f : 10.0f, color, on);
                else overlay::disc(dl, p, on ? 5.5f : 4.5f, color, on);
                if (i == hovered || i == activeHandle) overlay::label(dl, labelAt, h.label, color, false);
                break;
            }
            case HandleKind::DistortReset:
                dl->AddRectFilled(ImVec2(p.x - 24.0f, p.y - 9.0f), ImVec2(p.x + 24.0f, p.y + 9.0f), on ? IM_COL32(60, 60, 70, 235) : IM_COL32(30, 30, 36, 235), 3.0f);
                dl->AddRect(ImVec2(p.x - 24.0f, p.y - 9.0f), ImVec2(p.x + 24.0f, p.y + 9.0f), overlay::ACCENT, 3.0f, 1.0f);
                {
                    const ImVec2 size = ImGui::CalcTextSize("Reset");
                    dl->AddText(ImVec2(std::floor(p.x - size.x * 0.5f), std::floor(p.y - size.y * 0.5f)), overlay::WHITE, "Reset");
                }
                break;
            case HandleKind::ShapeCorner:
            case HandleKind::ShapeInner:
            case HandleKind::ShapePoints:
            case HandleKind::ShapeTail:
                overlay::diamond(dl, p, on ? 13.0f : 11.0f, color, on);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            case HandleKind::WarpBend: {
                const ImVec2 a = anchorScreen(h);
                dl->AddLine(a, p, overlay::HALO_COLOR, 3.0f);
                dl->AddLine(a, p, color, 1.0f);
                overlay::disc(dl, p, on ? 7.0f : 6.0f, color, on);
                overlay::label(dl, labelAt, h.label, color, false);
                break;
            }
        }
    }
}

// ---- the light sphere ----

bool jasperengine::editor::titledesigner::lightSphere(const char* id, double& angle, double& altitude, AngleConvention convention, float size) {
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##light_sphere", ImVec2(size, size));
    const float radius = size * 0.5f - 4.0f;
    const ImVec2 c(origin.x + size * 0.5f, origin.y + size * 0.5f);
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        double a = angle, alt = altitude;
        lightFromSphere(io.MousePos.x - c.x, io.MousePos.y - c.y, radius, convention, io.KeyShift, a, alt);
        if (a != angle || alt != altitude) {
            angle = a;
            altitude = alt;
            changed = true;
        }
    }
    const double sign = convention == AngleConvention::CounterClockwiseFromRight ? -1.0 : 1.0;
    if (ImGui::IsItemFocused()) {
        // left / right turn the light the way the arrow points at the top of the sphere, up / down raise / lower it
        const double step = ImGui::GetIO().KeyShift ? 10.0 : 1.0;
        double da = 0.0, dAlt = 0.0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) da = step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) da = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dAlt = step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dAlt = -step;
        if (da != 0.0 || dAlt != 0.0) {
            angle = std::fmod(std::fmod(std::round(angle) - da * sign, 360.0) + 360.0, 360.0);
            altitude = std::clamp(std::round(altitude) + dAlt, 0.0, 90.0);
            changed = true;
        }
    }
    // a lit sphere: rings brightening toward the highlight
    const float r = (float)std::cos(std::clamp(altitude, 0.0, 90.0) * DEG) * radius;
    const ImVec2 light(c.x + (float)std::cos(angle * DEG) * r, c.y + (float)(sign * std::sin(angle * DEG)) * r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, radius, IM_COL32(27, 22, 12, 255));
    constexpr int RINGS = 14;
    const ImVec4 dark(0.54f, 0.45f, 0.25f, 1.0f), bright(1.0f, 0.91f, 0.66f, 1.0f);
    for (int k = 0; k <= RINGS; k++) {
        const float t = (float)k / RINGS;
        const ImVec2 at(c.x + (light.x - c.x) * t, c.y + (light.y - c.y) * t);
        const float off = std::sqrt((at.x - c.x) * (at.x - c.x) + (at.y - c.y) * (at.y - c.y));
        const float ringRadius = std::max(1.0f, (radius - off) * (1.0f - 0.85f * t));
        const ImVec4 col(dark.x + (bright.x - dark.x) * t, dark.y + (bright.y - dark.y) * t, dark.z + (bright.z - dark.z) * t, 1.0f);
        dl->AddCircleFilled(at, ringRadius, ImGui::ColorConvertFloat4ToU32(col));
    }
    dl->AddCircle(c, radius, IM_COL32(0, 0, 0, 160), 0, 1.0f);
    dl->AddCircleFilled(light, 4.0f, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(light, 4.0f, IM_COL32(0, 0, 0, 255), 0, 1.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Angle %.0f\xC2\xB0 \xC2\xB7 Altitude %.0f\xC2\xB0\nDrag the light (Shift snaps to 15\xC2\xB0 / 5\xC2\xB0); centre = overhead",
                          angle, altitude);
    }
    ImGui::PopID();
    return changed;
}
