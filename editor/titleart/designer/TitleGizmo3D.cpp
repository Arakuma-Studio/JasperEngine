// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/ui/viewport/Gizmo3D.tsx, gizmoEdit.ts, rotation3d.ts and Handle.tsx
// (commit ecaa136) by SafEight, used with the author's permission.

#include "TitleGizmo3D.h"
#include "TitleGizmo2D.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::editor::titledesigner;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    constexpr double SQRT1_2 = 0.70710678118654752440;

    // drawing (Gizmo3D.tsx)
    constexpr int ARC_STEPS = 18;           // segments of a drawn arc
    constexpr double ARC_INSET = 0.32;      // radians the drawn arc stops short of each end (its tips clear the shafts)
    constexpr float SHAFT_GAP = 11.0f;      // between the centre square and the shafts
    constexpr float HEAD_LEN = 15.0f, HEAD_HALF = 6.5f;
    constexpr float TIP_LEN = 9.0f, TIP_HALF = 5.0f;
    constexpr float LABEL_GAP = 13.0f;      // tip to the centre of its axis letter
    // hit shapes (canvas.css): strokes 20 px wide, the ring 14, heads with a 4 px margin, the centre >= 24 px
    constexpr float STROKE_HIT = 10.0f, RING_HIT = 7.0f, HEAD_MARGIN = 4.0f, CENTRE_HIT = 12.0f;
    constexpr int ARC_HIT_STEPS = 36;
    // an arc's drag: the floor of its on-screen rate (px per radian)
    constexpr double MIN_ARC_RATE = 0.4 * rotation3d::ARC_PX;

    constexpr const char* AXIS_NAMES[3] = {"X", "Y", "Z"};
    constexpr const char* DOT = " \xC2\xB7 ";      // " · "
    constexpr const char* DEGREE = "\xC2\xB0";     // "°"

    double snapTo(double v, double step) { return step > 0.0 ? std::round(v / step) * step : v; }
    double unwrapTo(double v, double toward) { return v + 360.0 * std::round((toward - v) / 360.0); }
    float distance(ImVec2 a, ImVec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

    float segmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float len2 = vx * vx + vy * vy;
        float t = len2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return distance(p, ImVec2(a.x + vx * t, a.y + vy * t));
    }

    // inside triangle abc, or within `margin` of its edges
    bool nearTriangle(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c, float margin) {
        const auto cross = [](ImVec2 o, ImVec2 u, ImVec2 v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
        const float d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (!(neg && pos)) return true;
        return segmentDistance(p, a, b) <= margin || segmentDistance(p, b, c) <= margin || segmentDistance(p, c, a) <= margin;
    }

    // a triangle with its point at `p`, pointing along unit `u`
    void triangleAt(ImVec2 p, ImVec2 u, float len, float half, ImVec2 out[3]) {
        const float bx = p.x - u.x * len, by = p.y - u.y * len;
        out[0] = p;
        out[1] = ImVec2(bx - u.y * half, by + u.x * half);
        out[2] = ImVec2(bx + u.y * half, by - u.x * half);
    }

    ImVec2 unit(ImVec2 v) {
        const float len = std::sqrt(v.x * v.x + v.y * v.y);
        return len > 1e-6f ? ImVec2(v.x / len, v.y / len) : ImVec2(0.0f, 0.0f);
    }

    Vec3d scaled(const Vec3d& v, double s) { return {v.x * s, v.y * s, v.z * s}; }
    Vec3d added(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3d subtracted(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    double length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    // the rotation of `degrees` about unit axis n (three's makeRotationAxis)
    Mat3d axisRotation(const Vec3d& n, double degrees) {
        const double a = degrees * DEG, c = std::cos(a), s = std::sin(a), t = 1.0 - c;
        const double x = n.x, y = n.y, z = n.z;
        Mat3d r;
        r.m[0][0] = t * x * x + c;
        r.m[0][1] = t * x * y - s * z;
        r.m[0][2] = t * x * z + s * y;
        r.m[1][0] = t * x * y + s * z;
        r.m[1][1] = t * y * y + c;
        r.m[1][2] = t * y * z - s * x;
        r.m[2][0] = t * x * z - s * y;
        r.m[2][1] = t * y * z + s * x;
        r.m[2][2] = t * z * z + c;
        return r;
    }

    Mat3d rx(double a) {
        Mat3d r;
        r.m[1][1] = std::cos(a);
        r.m[1][2] = -std::sin(a);
        r.m[2][1] = std::sin(a);
        r.m[2][2] = std::cos(a);
        return r;
    }
    Mat3d ry(double a) {
        Mat3d r;
        r.m[0][0] = std::cos(a);
        r.m[0][2] = std::sin(a);
        r.m[2][0] = -std::sin(a);
        r.m[2][2] = std::cos(a);
        return r;
    }
    Mat3d rz(double a) {
        Mat3d r;
        r.m[0][0] = std::cos(a);
        r.m[0][1] = -std::sin(a);
        r.m[1][0] = std::sin(a);
        r.m[1][1] = std::cos(a);
        return r;
    }

    // three's Euler.setFromRotationMatrix (degrees, [x, y, z]) and the other Tait-Bryan solution:
    // Ra(a) Rb(b) Rc(c) = Ra(a + 180) Rb(180 - b) Rc(c + 180), the middle axis y for ZYX, x for YXZ
    void eulerSolutions(const Mat3d& r, bool zyx, Vec3d& a, Vec3d& b) {
        const auto& m = r.m;
        double x = 0.0, y = 0.0, z = 0.0;
        if (zyx) {
            y = std::asin(-std::clamp(m[2][0], -1.0, 1.0));
            if (std::abs(m[2][0]) < 0.9999999) {
                x = std::atan2(m[2][1], m[2][2]);
                z = std::atan2(m[1][0], m[0][0]);
            } else {
                z = std::atan2(-m[0][1], m[1][1]);
            }
        } else {
            x = std::asin(-std::clamp(m[1][2], -1.0, 1.0));
            if (std::abs(m[1][2]) < 0.9999999) {
                y = std::atan2(m[0][2], m[2][2]);
                z = std::atan2(m[1][0], m[1][1]);
            } else {
                y = std::atan2(-m[2][0], m[0][0]);
            }
        }
        a = {x / DEG, y / DEG, z / DEG};
        b = zyx ? Vec3d{a.x + 180.0, 180.0 - a.y, a.z + 180.0} : Vec3d{180.0 - a.x, a.y + 180.0, a.z + 180.0};
    }

    Gizmo3DHandle moveHandle(int axis) { return (Gizmo3DHandle)((int)Gizmo3DHandle::MoveX + axis); }
    Gizmo3DHandle rotateHandle(int axis) { return (Gizmo3DHandle)((int)Gizmo3DHandle::RotateX + axis); }
    bool isMoveAxis(Gizmo3DHandle h) { return h >= Gizmo3DHandle::MoveX && h <= Gizmo3DHandle::MoveZ; }
    bool isRotateAxis(Gizmo3DHandle h) { return h >= Gizmo3DHandle::RotateX && h <= Gizmo3DHandle::RotateZ; }

    const char* actionOf(Gizmo3DHandle h) {
        switch (h) {
            case Gizmo3DHandle::MoveX: return "Move X";
            case Gizmo3DHandle::MoveY: return "Move Y";
            case Gizmo3DHandle::MoveZ: return "Move Z";
            case Gizmo3DHandle::MovePlane: return "Move";
            case Gizmo3DHandle::RotateX: return "Rotate X";
            case Gizmo3DHandle::RotateY: return "Rotate Y";
            case Gizmo3DHandle::RotateZ: return "Rotate Z";
            case Gizmo3DHandle::FreeRotate: return "Free Rotate";
            case Gizmo3DHandle::None: break;
        }
        return "";
    }

    ImU32 faded(ImU32 color, bool fade) {
        if (!fade) return color;
        const ImU32 a = (color >> IM_COL32_A_SHIFT) & 0xFF;
        return (color & ~IM_COL32_A_MASK) | ((ImU32)(a * 0.35f) << IM_COL32_A_SHIFT);
    }

}

// ---- Mat3d ----

Mat3d Mat3d::operator*(const Mat3d& o) const {
    Mat3d r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
    }
    return r;
}

Vec3d Mat3d::operator*(const Vec3d& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Mat3d Mat3d::transposed() const {
    Mat3d r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
    }
    return r;
}

double Mat3d::determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Mat3d Mat3d::inverse() const {
    const double det = determinant();
    if (std::abs(det) < 1e-300) return Mat3d();
    const double k = 1.0 / det;
    Mat3d r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return r;
}

Mat3d Mat3d::diagonal(double x, double y, double z) {
    Mat3d r;
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = z;
    return r;
}

// ---- rotation3d ----

ImVec2 jasperengine::editor::titledesigner::rotation3d::schematicArm(int axis) {
    if (axis == 0) return ImVec2((float)ARM_PX, 0.0f);
    if (axis == 1) return ImVec2(0.0f, (float)-ARM_PX);
    return ImVec2((float)(-Z_ARM_PX * SQRT1_2), (float)(Z_ARM_PX * SQRT1_2));
}

Vec3d jasperengine::editor::titledesigner::rotation3d::compAxis(int axis) {
    return axis == 0 ? Vec3d{1.0, 0.0, 0.0} : axis == 1 ? Vec3d{0.0, 1.0, 0.0} : Vec3d{0.0, 0.0, 1.0};
}

double jasperengine::editor::titledesigner::rotation3d::armTravel(int axis, double gx, double gy, double k) {
    const ImVec2 a = schematicArm(axis);
    const double len = std::sqrt((double)a.x * a.x + (double)a.y * a.y);
    return (gx * a.x + gy * a.y) / len / k;
}

Mat3d jasperengine::editor::titledesigner::rotation3d::eulerZYX(double x, double y, double z) {
    return rz(z) * ry(y) * rx(x);
}

Mat3d jasperengine::editor::titledesigner::rotation3d::eulerYXZ(double x, double y, double z) {
    return ry(y) * rx(x) * rz(z);
}

Vec3d jasperengine::editor::titledesigner::rotation3d::nearestBranch(const std::vector<Vec3d>& candidates, const Vec3d& previous) {
    Vec3d best = candidates.empty() ? previous : candidates[0];
    double bestDistance = 1e300;
    for (const Vec3d& c : candidates) {
        const Vec3d u{unwrapTo(c.x, previous.x), unwrapTo(c.y, previous.y), unwrapTo(c.z, previous.z)};
        const double d = std::abs(u.x - previous.x) + std::abs(u.y - previous.y) + std::abs(u.z - previous.z);
        if (d < bestDistance) {
            bestDistance = d;
            best = u;
        }
    }
    return best;
}

Mat3d jasperengine::editor::titledesigner::rotation3d::turnAbout(const Mat3d& w0, const Vec3d& n, double degrees) {
    return axisRotation(n, degrees) * w0;
}

Mat3d jasperengine::editor::titledesigner::rotation3d::trackball(const Mat3d& w0, const Vec3d& right, const Vec3d& up, double alpha, double beta) {
    return axisRotation(up, alpha) * axisRotation(right, beta) * w0;
}

Vec3d jasperengine::editor::titledesigner::rotation3d::moveValues(const std::array<Vec3d, 3>& axes, const Vec3d& w) {
    Mat3d basis;
    for (int j = 0; j < 3; j++) {
        basis.m[0][j] = axes[j].x;
        basis.m[1][j] = axes[j].y;
        basis.m[2][j] = axes[j].z;
    }
    return basis.inverse() * w;
}

// ---- RotationModel ----

RotationModel RotationModel::layer() {
    return RotationModel();
}

RotationModel RotationModel::glyph(const Mat3d& base) {
    RotationModel model;
    model.glyphModel = true;
    model.base = base;
    model.baseInverse = base.inverse();
    return model;
}

Mat3d RotationModel::orient(const Vec3d& rot) const {
    if (!glyphModel) return rotation3d::eulerZYX(rot.x * DEG, rot.y * DEG, -rot.z * DEG);
    return base * rotation3d::eulerYXZ(rot.x * DEG, rot.y * DEG, -rot.z * DEG);
}

Vec3d RotationModel::fromOrient(const Mat3d& w, const Vec3d& previous) const {
    Vec3d a, b;
    if (!glyphModel) eulerSolutions(w, true, a, b);
    else eulerSolutions(baseInverse * w, false, a, b);
    // the Z channel is clockwise on screen: the Euler z negated
    return rotation3d::nearestBranch({Vec3d{a.x, a.y, -a.z}, Vec3d{b.x, b.y, -b.z}}, previous);
}

// ---- targets and camera ----

Gizmo3DTarget jasperengine::editor::titledesigner::layerGizmo3DTarget(const std::string& layerId, const Vec3d& origin, const Vec3d& position,
                                                                            const Vec3d& rotation, double moveStep) {
    Gizmo3DTarget t;
    t.id = layerId;
    t.origin = origin;
    t.pos = position;
    t.rot = rotation;
    t.unit = "px";
    t.step = moveStep;
    t.write = [layerId, position, rotation](const Vec3d& dPos, const Vec3d& dRot, Gizmo3DGesture gesture) {
        std::vector<FieldEdit> edits;
        if (gesture == Gizmo3DGesture::Move) {
            edits.push_back({layerId, "transform.position", Json::array({position.x + dPos.x, position.y + dPos.y})});
            edits.push_back({layerId, "transform.z", position.z + dPos.z});
        } else {
            edits.push_back({layerId, "transform.rotation_x", rotation.x + dRot.x});
            edits.push_back({layerId, "transform.rotation_y", rotation.y + dRot.y});
            edits.push_back({layerId, "transform.rotation", rotation.z + dRot.z});
        }
        return edits;
    };
    return t;
}

Gizmo3DCamera Gizmo3DCamera::frontOn(const CanvasXform& view, float pixelRatio) {
    Gizmo3DCamera camera;
    camera.project = [view](const Vec3d& world, ImVec2& screen) {
        screen = view.toScreen(TitleVec2{world.x, world.y});
        return true;
    };
    camera.pixelRatio = pixelRatio;
    return camera;
}

// ---- TitleGizmo3D ----

bool TitleGizmo3D::layout(const Gizmo3DTarget& target, const Gizmo3DCamera& camera) {
    current = target;
    right = camera.right;
    up = camera.up;
    ImVec2 at;
    shown = camera.project && camera.project(target.origin, at);
    if (!shown) return false;
    // on the device pixel grid, so the schematic draws the same pixels wherever it sits
    const float dpr = camera.pixelRatio > 0.0f ? camera.pixelRatio : 1.0f;
    c = ImVec2(std::round(at.x * dpr) / dpr, std::round(at.y * dpr) / dpr);
    // screen px per world unit along the screen's right / up at the pivot (its perspective scale)
    const auto rate = [&](const Vec3d& dir) {
        ImVec2 p;
        if (!camera.project(added(target.origin, dir), p)) return ImVec2(0.0f, 0.0f);
        return ImVec2(p.x - at.x, p.y - at.y);
    };
    ur = rate(right);
    uu = rate(up);
    k = std::sqrt((double)ur.x * ur.x + (double)ur.y * ur.y);
    if (k < 1e-9) k = 1e-9;
    return true;
}

ImVec2 TitleGizmo3D::armEnd(int axis) const {
    const ImVec2 a = rotation3d::schematicArm(axis);
    return ImVec2(c.x + a.x, c.y + a.y);
}

ImVec2 TitleGizmo3D::arcPoint(int axis, double th) const {
    // each arc spanned by the arms of the two axes after its own (cyclic): turning A -> B is positive about its axis
    const ImVec2 a = rotation3d::schematicArm((axis + 1) % 3), b = rotation3d::schematicArm((axis + 2) % 3);
    const double r = rotation3d::ARC_PX / rotation3d::ARM_PX;
    return ImVec2((float)(c.x + r * (std::cos(th) * a.x + std::sin(th) * b.x)), (float)(c.y + r * (std::cos(th) * a.y + std::sin(th) * b.y)));
}

double TitleGizmo3D::nearestOnArc(int axis, double from, double to, ImVec2 p, double* outDistance) const {
    double best = from, bestDistance = 1e300;
    for (int i = 0; i <= 60; i++) {
        const double th = from + (to - from) * i / 60.0;
        const double d = distance(arcPoint(axis, th), p);
        if (d < bestDistance) {
            bestDistance = d;
            best = th;
        }
    }
    if (outDistance) *outDistance = bestDistance;
    return best;
}

Vec3d TitleGizmo3D::plane(double gx, double gy) const {
    // the world displacement in the screen plane through the pivot that moves it (gx, gy) px
    const double det = (double)ur.x * uu.y - (double)ur.y * uu.x;
    if (std::abs(det) < 1e-12) return {};
    const double a = (gx * uu.y - gy * uu.x) / det;
    const double b = (ur.x * gy - ur.y * gx) / det;
    return added(scaled(right, a), scaled(up, b));
}

Gizmo3DHandle TitleGizmo3D::hitTest(ImVec2 s) const {
    if (!shown) return Gizmo3DHandle::None;
    // the reverse of the drawing order: the centre, heads, shafts, arcs, then the ring
    if (distance(s, c) <= CENTRE_HIT) return Gizmo3DHandle::MovePlane;
    for (int axis = 2; axis >= 0; axis--) {
        const ImVec2 arm = rotation3d::schematicArm(axis);
        ImVec2 tri[3];
        triangleAt(armEnd(axis), unit(arm), HEAD_LEN, HEAD_HALF, tri);
        if (nearTriangle(s, tri[0], tri[1], tri[2], HEAD_MARGIN)) return moveHandle(axis);
    }
    for (int axis = 2; axis >= 0; axis--) {
        const ImVec2 arm = rotation3d::schematicArm(axis), u = unit(arm), tip = armEnd(axis);
        const ImVec2 grab(c.x + arm.x * (float)rotation3d::SHAFT_GRAB, c.y + arm.y * (float)rotation3d::SHAFT_GRAB);
        const ImVec2 base(tip.x - u.x * HEAD_LEN, tip.y - u.y * HEAD_LEN);
        if (segmentDistance(s, grab, base) <= STROKE_HIT) return moveHandle(axis);
    }
    for (int axis = 2; axis >= 0; axis--) {
        ImVec2 prev = arcPoint(axis, 0.0);
        for (int i = 1; i <= ARC_HIT_STEPS; i++) {
            const ImVec2 p = arcPoint(axis, PI / 2 * i / ARC_HIT_STEPS);
            if (segmentDistance(s, prev, p) <= STROKE_HIT) return rotateHandle(axis);
            prev = p;
        }
    }
    if (std::abs(distance(s, c) - (float)rotation3d::RING_PX) <= RING_HIT) return Gizmo3DHandle::FreeRotate;
    return Gizmo3DHandle::None;
}

bool TitleGizmo3D::begin(Gizmo3DHandle handle, ImVec2 screen, const GizmoModifiers&) {
    if (handle == Gizmo3DHandle::None || !shown) return false;
    drag = Drag();
    drag.handle = handle;
    drag.target = current;
    drag.press = drag.last = screen;
    drag.w0 = current.rotation.orient(current.rot);
    drag.prev = current.rot;
    drag.nowPos = current.pos;
    drag.nowRot = current.rot;
    if (isRotateAxis(handle)) {
        drag.th = drag.th0 = nearestOnArc((int)handle - (int)Gizmo3DHandle::RotateX, 0.0, PI / 2, screen, nullptr);
    }
    return true;
}

void TitleGizmo3D::end() {
    drag = Drag();
}

std::vector<FieldEdit> TitleGizmo3D::update(ImVec2 screen, const GizmoModifiers& modifiers) {
    if (!active()) return {};
    const Gizmo3DTarget& t = drag.target;
    const double gx = screen.x - drag.press.x, gy = screen.y - drag.press.y;
    const Vec3d none;

    if (isMoveAxis(drag.handle) || drag.handle == Gizmo3DHandle::MovePlane) {
        Vec3d w;
        if (drag.handle == Gizmo3DHandle::MovePlane) {
            w = plane(gx, gy);
        } else {
            const int axis = (int)drag.handle - (int)Gizmo3DHandle::MoveX;
            w = scaled(rotation3d::compAxis(axis), rotation3d::armTravel(axis, gx, gy, k));
        }
        if (modifiers.shift) {
            // the step in world px: that many units of X
            const double step = t.step * length(t.axes[0]);
            w = {snapTo(w.x, step), snapTo(w.y, step), snapTo(w.z, step)};
        }
        const Vec3d d = rotation3d::moveValues(t.axes, w);
        drag.nowPos = added(t.pos, d);
        return t.write ? t.write(d, none, Gizmo3DGesture::Move) : std::vector<FieldEdit>();
    }

    Mat3d turned;
    if (isRotateAxis(drag.handle)) {
        // step along the ring's tangent from the last angle, then settle on the ring point nearest
        // the pointer within ±45° of it, so the angle follows the hand all the way round
        const int axis = (int)drag.handle - (int)Gizmo3DHandle::RotateX;
        const ImVec2 a = rotation3d::schematicArm((axis + 1) % 3), b = rotation3d::schematicArm((axis + 2) % 3);
        const double r = rotation3d::ARC_PX / rotation3d::ARM_PX;
        const double tx = r * (-std::sin(drag.th) * a.x + std::cos(drag.th) * b.x);
        const double ty = r * (-std::sin(drag.th) * a.y + std::cos(drag.th) * b.y);
        drag.th += ((screen.x - drag.last.x) * tx + (screen.y - drag.last.y) * ty) / std::max(tx * tx + ty * ty, MIN_ARC_RATE * MIN_ARC_RATE);
        drag.last = screen;
        double off = 0.0;
        const double th = nearestOnArc(axis, drag.th - PI / 4, drag.th + PI / 4, screen, &off);
        if (off < rotation3d::ARC_PX * 0.6) drag.th = th;
        double deg = (drag.th - drag.th0) / DEG;
        if (modifiers.shift) deg = snapTo(deg, rotation3d::ROTATE_SNAP);
        turned = rotation3d::turnAbout(drag.w0, rotation3d::compAxis(axis), deg);
    } else {
        // the ring: a trackball, right turns about the screen's up axis, down about its right axis
        double alpha = gx * rotation3d::ROTATE_PER_PX, beta = gy * rotation3d::ROTATE_PER_PX;
        if (modifiers.shift) {
            alpha = snapTo(alpha, rotation3d::ROTATE_SNAP);
            beta = snapTo(beta, rotation3d::ROTATE_SNAP);
        }
        turned = rotation3d::trackball(drag.w0, right, up, alpha, beta);
    }
    drag.prev = t.rotation.fromOrient(turned, drag.prev);
    drag.nowRot = drag.prev;
    return t.write ? t.write(none, subtracted(drag.prev, t.rot), Gizmo3DGesture::Rotate) : std::vector<FieldEdit>();
}

std::string TitleGizmo3D::actionName() const {
    return actionOf(drag.handle);
}

std::string TitleGizmo3D::readout(Gizmo3DHandle hovered) const {
    if (!active()) return actionOf(hovered);
    const bool move = isMoveAxis(drag.handle) || drag.handle == Gizmo3DHandle::MovePlane;
    const Vec3d& from = move ? drag.target.pos : drag.target.rot;
    const Vec3d& now = move ? drag.nowPos : drag.nowRot;
    std::string s = actionOf(drag.handle);
    for (int i = 0; i < 3; i++) {
        if (std::abs(now[i] - from[i]) <= 0.05) continue;
        s += std::string(DOT) + AXIS_NAMES[i] + " " + formatReadout(now[i]) + (move ? " " + drag.target.unit : std::string(DEGREE));
    }
    return s;
}

void TitleGizmo3D::draw(ImDrawList* dl, Gizmo3DHandle hovered, Gizmo3DHandle activeHandle) const {
    if (!dl || !shown) return;
    const bool dragging = activeHandle != Gizmo3DHandle::None;
    const auto lit = [&](Gizmo3DHandle h) { return dragging ? h == activeHandle : h == hovered; };
    // while dragging, the other handles step back
    const auto back = [&](Gizmo3DHandle h) { return dragging && h != activeHandle; };

    // the guide of an axis move: its line through the pivot
    if (isMoveAxis(activeHandle)) {
        const int axis = (int)activeHandle - (int)Gizmo3DHandle::MoveX;
        const ImVec2 u = unit(rotation3d::schematicArm(axis));
        dl->AddLine(ImVec2(c.x - u.x * 4000.0f, c.y - u.y * 4000.0f), ImVec2(c.x + u.x * 4000.0f, c.y + u.y * 4000.0f),
                    (overlay::AXIS[axis] & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), 1.0f);
    }

    // the free-rotate ring: readable, quieter than the axis handles
    {
        const bool on = lit(Gizmo3DHandle::FreeRotate), fade = back(Gizmo3DHandle::FreeRotate);
        dl->AddCircle(c, (float)rotation3d::RING_PX, faded(on ? overlay::HALO_COLOR : IM_COL32(7, 7, 10, 128), fade), 0, on ? 6.5f : 4.0f);
        dl->AddCircle(c, (float)rotation3d::RING_PX, faded(on ? overlay::WHITE : IM_COL32(255, 255, 255, 179), fade), 0, on ? 3.0f : 1.5f);
    }

    // arcs: the quarter less ARC_INSET at each end, an arrowhead beyond each end along it
    for (int axis = 0; axis < 3; axis++) {
        const Gizmo3DHandle h = rotateHandle(axis);
        const bool on = lit(h), fade = back(h);
        const ImU32 color = faded(on ? overlay::AXIS_LIT[axis] : overlay::AXIS[axis], fade);
        const ImU32 halo = faded(overlay::HALO_COLOR, fade);
        ImVec2 pts[ARC_STEPS + 1];
        for (int j = 0; j <= ARC_STEPS; j++) pts[j] = arcPoint(axis, ARC_INSET + (PI / 2 - 2 * ARC_INSET) * j / ARC_STEPS);
        ImVec2 tips[2][3];
        const ImVec2 ends[2] = {pts[0], pts[ARC_STEPS]};
        const ImVec2 dirs[2] = {unit(ImVec2(pts[0].x - arcPoint(axis, ARC_INSET + 0.02).x, pts[0].y - arcPoint(axis, ARC_INSET + 0.02).y)),
                                unit(ImVec2(pts[ARC_STEPS].x - arcPoint(axis, PI / 2 - ARC_INSET - 0.02).x,
                                            pts[ARC_STEPS].y - arcPoint(axis, PI / 2 - ARC_INSET - 0.02).y))};
        for (int e = 0; e < 2; e++) {
            triangleAt(ImVec2(ends[e].x + dirs[e].x * TIP_LEN, ends[e].y + dirs[e].y * TIP_LEN), dirs[e], TIP_LEN, TIP_HALF, tips[e]);
        }
        dl->AddPolyline(pts, ARC_STEPS + 1, halo, on ? 9.5f : 8.0f, ImDrawFlags_None);
        for (int e = 0; e < 2; e++) {
            dl->AddTriangle(tips[e][0], tips[e][1], tips[e][2], halo, on ? 6.0f : 4.0f);
            dl->AddTriangleFilled(tips[e][0], tips[e][1], tips[e][2], halo);
        }
        dl->AddPolyline(pts, ARC_STEPS + 1, color, on ? 5.0f : 4.0f, ImDrawFlags_None);
        for (int e = 0; e < 2; e++) dl->AddTriangleFilled(tips[e][0], tips[e][1], tips[e][2], color);
    }

    // shafts first, then every head above the shafts and arcs
    for (int axis = 0; axis < 3; axis++) {
        const Gizmo3DHandle h = moveHandle(axis);
        const bool on = lit(h), fade = back(h);
        const ImVec2 arm = rotation3d::schematicArm(axis), u = unit(arm), tip = armEnd(axis);
        const float len = std::sqrt(arm.x * arm.x + arm.y * arm.y);
        const float gap = std::min(SHAFT_GAP, len / 2);
        const ImVec2 from(c.x + u.x * gap, c.y + u.y * gap), base(tip.x - u.x * HEAD_LEN, tip.y - u.y * HEAD_LEN);
        ImVec2 tri[3];
        triangleAt(tip, u, HEAD_LEN, HEAD_HALF, tri);
        const ImU32 halo = faded(overlay::HALO_COLOR, fade);
        dl->AddLine(from, base, halo, on ? overlay::HOVER_HALO_WIDTH : overlay::HALO_WIDTH);
        dl->AddTriangle(tri[0], tri[1], tri[2], halo, on ? 6.0f : 4.0f);
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], halo);
        dl->AddLine(from, base, faded(on ? overlay::AXIS_LIT[axis] : overlay::AXIS[axis], fade), on ? overlay::HOVER_WIDTH : overlay::STROKE_WIDTH);
    }
    for (int axis = 0; axis < 3; axis++) {
        const Gizmo3DHandle h = moveHandle(axis);
        const bool on = lit(h), fade = back(h);
        const ImVec2 u = unit(rotation3d::schematicArm(axis)), tip = armEnd(axis);
        ImVec2 tri[3];
        triangleAt(tip, u, HEAD_LEN, HEAD_HALF, tri);
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], faded(on ? overlay::AXIS_LIT[axis] : overlay::AXIS[axis], fade));
        // the axis letter on a dark disc ringed in the axis colour
        const ImVec2 at(tip.x + u.x * LABEL_GAP, tip.y + u.y * LABEL_GAP);
        dl->AddCircleFilled(at, 7.5f, faded(on ? overlay::AXIS[axis] : IM_COL32(13, 13, 16, 230), fade));
        dl->AddCircle(at, 7.5f, faded(on ? overlay::AXIS_LIT[axis] : overlay::AXIS[axis], fade), 0, 1.5f);
        const ImVec2 size = ImGui::CalcTextSize(AXIS_NAMES[axis]);
        dl->AddText(ImVec2(std::floor(at.x - size.x * 0.5f), std::floor(at.y - size.y * 0.5f)),
                    faded(on ? IM_COL32(0, 0, 0, 255) : overlay::AXIS_LIT[axis], fade), AXIS_NAMES[axis]);
    }

    // the centre square: moves in the screen plane
    {
        const bool on = lit(Gizmo3DHandle::MovePlane), fade = back(Gizmo3DHandle::MovePlane);
        dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), faded(overlay::WHITE, fade), 2.0f);
        dl->AddRect(ImVec2(c.x - 6.0f, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y + 6.0f), faded(IM_COL32(7, 7, 10, 255), fade), 2.0f, on ? 2.5f : 2.0f);
    }

    const std::string text = readout(hovered);
    if (!text.empty()) overlay::label(dl, ImVec2(c.x, c.y + (float)rotation3d::RING_PX + 18.0f), text, overlay::WHITE, true);
}
