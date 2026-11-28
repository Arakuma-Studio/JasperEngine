// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintBrush.h"

#include "shortcuts/CommandCatalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace {

    constexpr float BRUSH_PI = 3.14159265358979f;

    // the corner point of a corner selection: a quarter of the way from its vertex to the face's centroid,
    // so a box around one corner of a face picks that face's corner, not its neighbours'
    Vector3 cornerPoint(const vc::MeshView& mesh, uint32_t corner) {
        const uint32_t tri = corner / 3;
        const Vector3& a = mesh.positions[mesh.indices[tri * 3 + 0]];
        const Vector3& b = mesh.positions[mesh.indices[tri * 3 + 1]];
        const Vector3& c = mesh.positions[mesh.indices[tri * 3 + 2]];
        const Vector3 centroid = (a + b + c) / 3.0f;
        const Vector3& v = mesh.positions[mesh.indices[corner]];
        return v + (centroid - v) * 0.25f;
    }

    Vector3 faceCentroid(const vc::MeshView& mesh, uint32_t tri) {
        return (mesh.positions[mesh.indices[tri * 3 + 0]] + mesh.positions[mesh.indices[tri * 3 + 1]] + mesh.positions[mesh.indices[tri * 3 + 2]]) / 3.0f;
    }

    Vector3 worldNormal(const Matrix4& normalMatrix, const Vector3& local) {
        const Vector4 n = normalMatrix * Vector4(local.x, local.y, local.z, 0.0f);
        Vector3 out(n.x, n.y, n.z);
        const float length = out.length();
        return length > 0.0f ? out / length : Vector3(0.0f, 1.0f, 0.0f);
    }

    // every element of `kind` with its local point and normal
    template<typename Visit>
    void forEachElement(const vc::MeshView& mesh, vc::ElementKind kind, Visit&& visit) {
        switch (kind) {
            case vc::ElementKind::Vertex:
                for (uint32_t l = 0; l < mesh.logicalCount(); l++) {
                    visit(l, mesh.logicalPositions[l], l < mesh.logicalNormals.size() ? mesh.logicalNormals[l] : Vector3(0.0f, 1.0f, 0.0f));
                }
                break;
            case vc::ElementKind::Face:
                for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
                    visit(t, faceCentroid(mesh, t), t < mesh.faceNormals.size() ? mesh.faceNormals[t] : Vector3(0.0f, 1.0f, 0.0f));
                }
                break;
            case vc::ElementKind::Corner:
                for (uint32_t c = 0; c < (uint32_t)mesh.indices.size(); c++) {
                    const uint32_t t = c / 3;
                    visit(c, cornerPoint(mesh, c), t < mesh.faceNormals.size() ? mesh.faceNormals[t] : Vector3(0.0f, 1.0f, 0.0f));
                }
                break;
        }
    }

    template<typename Inside>
    vc::Selection selectRegion(const vc::MeshView& mesh, const Matrix4& model, vc::ElementKind kind, const ProjectFn& project,
                               const VisibleFn& visible, Inside&& inside) {
        vc::Selection out;
        out.kind = kind;
        const Matrix4 normalMatrix = model.inverse().transpose();
        forEachElement(mesh, kind, [&](uint32_t id, const Vector3& local, const Vector3& localNormal) {
            const Vector3 world = model * local;
            Vector2 screen;
            if (!project(world, screen) || !inside(screen)) return;
            if (visible && !visible(world, worldNormal(normalMatrix, localNormal))) return;
            out.ids.push_back(id);
        });
        return out;
    }

}

const char* paintToolName(PaintTool tool) {
    switch (tool) {
        case PaintTool::Select: return "Select";
        case PaintTool::Paint: return "Paint";
        case PaintTool::Smooth: return "Smooth";
        case PaintTool::Fill: return "Fill";
        case PaintTool::Gradient: return "Gradient";
        case PaintTool::Pick: return "Pick";
    }
    return "Paint";
}

const char* paintToolCommand(PaintTool tool) {
    switch (tool) {
        case PaintTool::Select: return "vertexPaint.tool.select";
        case PaintTool::Paint: return "vertexPaint.tool.paint";
        case PaintTool::Smooth: return "vertexPaint.tool.smooth";
        case PaintTool::Fill: return "vertexPaint.tool.fill";
        case PaintTool::Gradient: return "vertexPaint.tool.gradient";
        case PaintTool::Pick: return "vertexPaint.tool.pick";
    }
    return "vertexPaint.tool.paint";
}

vc::StrokeMode effectiveStrokeMode(PaintTool tool, bool ctrl) {
    if (tool == PaintTool::Smooth) return ctrl ? vc::StrokeMode::Sharpen : vc::StrokeMode::Smooth;
    return ctrl ? vc::StrokeMode::Erase : vc::StrokeMode::Paint;
}

std::string strokeUndoName(vc::StrokeMode mode) {
    switch (mode) {
        case vc::StrokeMode::Paint: return "Paint Vertex Colors";
        case vc::StrokeMode::Erase: return "Erase Vertex Colors";
        case vc::StrokeMode::Smooth: return "Smooth Vertex Colors";
        case vc::StrokeMode::Sharpen: return "Sharpen Vertex Colors";
    }
    return "Paint Vertex Colors";
}

const char* elementKindName(vc::ElementKind kind, bool plural) {
    switch (kind) {
        case vc::ElementKind::Vertex: return plural ? "vertices" : "vertex";
        case vc::ElementKind::Face: return plural ? "faces" : "face";
        case vc::ElementKind::Corner: return plural ? "corners" : "corner";
    }
    return plural ? "faces" : "face";
}

std::string applyUndoName(vc::ElementKind kind, size_t count) {
    std::string noun = elementKindName(kind, count != 1);
    noun[0] = (char)std::toupper((unsigned char)noun[0]);
    return "Apply Color to " + std::to_string(count) + " " + noun;
}

std::string selectionCountText(vc::ElementKind kind, size_t count) {
    return std::to_string(count) + " " + elementKindName(kind, count != 1) + " selected";
}

Vector4 strokeValue(const VertexPaintSettings& settings, bool secondary) {
    return secondary ? settings.secondary : settings.primary;
}

std::vector<vc::Filter> maskFilters(const VertexPaintSettings& settings) {
    std::vector<vc::Filter> filters;
    const PaintMasks& masks = settings.masks;
    auto add = [&](vc::FilterKind kind, float min, float max, int channel) {
        vc::Filter filter;
        filter.kind = kind;
        filter.min = min;
        filter.max = max;
        filter.channel = channel;
        filter.invert = masks.invert;
        filters.push_back(filter);
    };
    if (masks.slope) add(vc::FilterKind::Slope, masks.slopeMin, masks.slopeMax, 0);
    if (masks.height) add(vc::FilterKind::Height, masks.heightMin, masks.heightMax, 0);
    if (masks.curvature) add(vc::FilterKind::Curvature, masks.curvatureMin, masks.curvatureMax, 0);
    if (masks.channel) add(vc::FilterKind::Channel, masks.channelMin, masks.channelMax, std::clamp(masks.channelIndex, 0, 3));
    return filters;
}

vc::Filter facingFilter(const Vector3& towardEye) {
    vc::Filter filter;
    filter.kind = vc::FilterKind::Facing;
    filter.direction = towardEye;
    // the side of the surface that faces the eye, grazing included
    filter.angle = 90.0f;
    return filter;
}

vc::Op strokeOp(const VertexPaintSettings& settings, vc::StrokeMode mode, bool secondary, std::span<const vc::StrokePoint> points) {
    vc::Op op;
    op.type = vc::OpType::Stroke;
    op.mode = mode;
    op.points.assign(points.begin(), points.end());
    op.value = strokeValue(settings, secondary);
    op.channels = settings.channels;
    op.blend = settings.blend;
    op.strength = std::clamp(settings.strength, 0.0f, 1.0f);
    op.radius = std::clamp(settings.radius, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
    op.hardness = std::clamp(settings.hardness, 0.0f, 1.0f);
    op.falloff = settings.falloff;
    op.falloffCurve = settings.falloffCurve;
    op.spacing = std::max(0.01f, settings.spacing);
    op.buildup = settings.buildup;
    op.pressureStrength = settings.pressureStrength;
    op.pressureSize = settings.pressureSize;
    op.stencil = settings.stencil;
    op.stencilScale = settings.stencilScale;
    op.stencilRotation = settings.stencilRotation;
    op.target.element = settings.element;
    op.filters = maskFilters(settings);
    return op;
}

vc::Op applyOp(const VertexPaintSettings& settings, const vc::Target& target, bool secondary) {
    vc::Op op;
    const bool exact = settings.blend == vc::Blend::Replace && settings.strength >= 1.0f;
    op.type = exact ? vc::OpType::Set : vc::OpType::Fill;
    op.target = target;
    op.value = strokeValue(settings, secondary);
    op.channels = settings.channels;
    op.blend = settings.blend;
    op.strength = std::clamp(settings.strength, 0.0f, 1.0f);
    return op;
}

vc::Op fillOp(const VertexPaintSettings& settings, const vc::Target& target, bool secondary) {
    vc::Op op = applyOp(settings, target, secondary);
    op.type = vc::OpType::Fill;
    op.filters = maskFilters(settings);
    return op;
}

vc::Op gradientOp(const VertexPaintSettings& settings, const vc::Target& target, const Vector3& from, const Vector3& to) {
    vc::Op op;
    op.type = vc::OpType::Gradient;
    op.target = target;
    op.target.element = settings.element;
    op.gradientType = settings.gradientType == vc::GradientType::Radial ? vc::GradientType::Radial : vc::GradientType::Linear;
    op.gradientFrom = from;
    op.gradientTo = to;
    op.interpolation = settings.interpolation;
    op.extend = vc::Extend::Both;
    op.channels = settings.channels;
    op.blend = settings.blend;
    op.strength = std::clamp(settings.strength, 0.0f, 1.0f);
    vc::GradientStop start;
    start.t = 0.0f;
    start.value = settings.primary;
    vc::GradientStop end;
    end.t = 1.0f;
    end.value = settings.secondary;
    op.stops = {start, end};
    op.filters = maskFilters(settings);
    return op;
}

vc::Op recolorOp(const Vector4& from, const Vector4& to) {
    vc::Op op;
    op.type = vc::OpType::ReplaceColor;
    op.target.kind = vc::TargetKind::Color;
    op.target.color.value = from;
    // the list rounds to 8 bits: half a step on each side finds every value it counted
    op.target.color.tolerance = 0.5f / 255.0f + 1e-5f;
    op.target.color.ignoreAlpha = false;
    op.target.element = vc::Element::Face;
    op.target.color.coverage = 0.0f;
    op.value = to;
    op.channels = vc::CHANNELS_RGBA;
    return op;
}

float pressureOf(float reading) {
    if (!(reading >= 0.0f)) return 1.0f;
    return std::min(reading, 1.0f);
}

void adjustSize(VertexPaintSettings& settings, float factor) {
    settings.radius = std::clamp(settings.radius * factor, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
}

void adjustStrength(VertexPaintSettings& settings, float factor) {
    // a strength of 0 multiplies to 0 forever: a floor lets Stronger bring it back
    settings.strength = std::clamp(std::max(settings.strength, 0.01f) * factor, 0.0f, 1.0f);
}

void applyRightDrag(VertexPaintSettings& settings, bool ctrl, bool shift, float dx, float dy) {
    if (ctrl) {
        // up grows: 1% per pixel, as a multiplier, so small and big brushes feel the same
        adjustSize(settings, std::pow(1.01f, -dy));
        settings.strength = std::clamp(settings.strength + dx * 0.0025f, 0.0f, 1.0f);
    } else if (shift) {
        settings.hardness = std::clamp(settings.hardness - dy * 0.0025f, 0.0f, 1.0f);
        float rotation = settings.stencilRotation + dx * 0.5f;
        rotation = std::fmod(rotation, 360.0f);
        if (rotation < 0.0f) rotation += 360.0f;
        settings.stencilRotation = rotation;
    }
}

void toggleChannel(VertexPaintSettings& settings, int channel) {
    if (channel < 0 || channel > 3) return;
    settings.channels = (vc::ChannelMask)(settings.channels ^ (1u << channel));
}

void swapColors(VertexPaintSettings& settings) {
    std::swap(settings.primary, settings.secondary);
}

bool isLowDensity(const vc::MeshView& mesh, const Matrix4& model, float radius) {
    if (mesh.triangleCount() == 0 || radius <= 0.0f) return false;
    // world area: each face's area under the model's scale (its cross product, transformed)
    double area = 0.0;
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
        const Vector3 a = model * mesh.positions[mesh.indices[t * 3 + 0]];
        const Vector3 b = model * mesh.positions[mesh.indices[t * 3 + 1]];
        const Vector3 c = model * mesh.positions[mesh.indices[t * 3 + 2]];
        area += 0.5 * (double)(b - a).crossProduct(c - a).length();
    }
    const double perRadius2 = (double)mesh.logicalCount() / (area / ((double)radius * (double)radius));
    return area > 0.0 && perRadius2 < 1.0;
}

std::string lowDensityText(const std::string& name, uint32_t logicalVertices) {
    return name + ": " + std::to_string(logicalVertices) + (logicalVertices == 1 ? " vertex" : " vertices") + " \xE2\x80\x94 paint will look blocky";
}

Vector4 cursorTint(const VertexPaintSettings& settings, vc::StrokeMode mode, bool secondary, float alpha) {
    Vector4 value = strokeValue(settings, secondary);
    switch (mode) {
        case vc::StrokeMode::Erase: value = Vector4(0.2f, 0.2f, 0.2f, 1.0f); break;
        case vc::StrokeMode::Smooth: value = Vector4(0.5f, 0.75f, 1.0f, 1.0f); break;
        case vc::StrokeMode::Sharpen: value = Vector4(1.0f, 0.6f, 0.3f, 1.0f); break;
        case vc::StrokeMode::Paint: break;
    }
    // single-channel work: the value of that channel as grey (R103)
    const vc::ChannelMask channels = settings.channels;
    int single = -1;
    for (int c = 0; c < 4; c++) {
        if (channels == (vc::ChannelMask)(1u << c)) single = c;
    }
    if (mode == vc::StrokeMode::Paint && single >= 0) {
        const float v = single == 0 ? value.x : single == 1 ? value.y : single == 2 ? value.z : value.w;
        value = Vector4(v, v, v, 1.0f);
    }
    // a dark colour would vanish on a dark surface: the ring keeps a floor of brightness
    const float brightest = std::max(value.x, std::max(value.y, value.z));
    if (brightest < 0.15f) value = Vector4(value.x + 0.15f, value.y + 0.15f, value.z + 0.15f, 1.0f);
    return Vector4(vc::linearToSrgb(std::clamp(value.x, 0.0f, 1.0f)), vc::linearToSrgb(std::clamp(value.y, 0.0f, 1.0f)),
                   vc::linearToSrgb(std::clamp(value.z, 0.0f, 1.0f)), std::clamp(alpha, 0.0f, 1.0f));
}

void buildCursor(const Vector3& center, const Vector3& normal, float radius, float hardness, float strength, const Vector4& tint,
                              const std::function<Vector3(const Vector3&, const Vector3&)>& drape, PaintCursorLines& out) {
    out.points.clear();
    out.colors.clear();
    Vector3 n = normal;
    if (n.length() <= 0.0f) n = Vector3(0.0f, 1.0f, 0.0f);
    n.normalize();
    Vector3 u = n.perpendicular();
    u.normalize();
    const Vector3 v = n.crossProduct(u).normalized();
    constexpr int SEGMENTS = 48;
    auto ringPoint = [&](float r, float angle) {
        const Vector3 p = center + (u * std::cos(angle) + v * std::sin(angle)) * r;
        return drape ? drape(p, n) : p;
    };
    auto ring = [&](float r, float from, float to, int segments, const Vector4& color) {
        Vector3 previous = ringPoint(r, from);
        for (int i = 1; i <= segments; i++) {
            const Vector3 next = ringPoint(r, from + (to - from) * (float)i / (float)segments);
            out.points.push_back(previous);
            out.points.push_back(next);
            out.colors.push_back(color);
            previous = next;
        }
    };
    ring(radius, 0.0f, 2.0f * BRUSH_PI, SEGMENTS, tint);
    const float inner = radius * std::clamp(hardness, 0.0f, 1.0f);
    if (inner > radius * 0.02f && inner < radius * 0.98f) {
        ring(inner, 0.0f, 2.0f * BRUSH_PI, SEGMENTS, Vector4(tint.x * 0.8f, tint.y * 0.8f, tint.z * 0.8f, tint.w * 0.75f));
    }
    const float s = std::clamp(strength, 0.0f, 1.0f);
    const int arcSegments = std::max(1, (int)std::ceil(SEGMENTS * s));
    // from the top of the ring, clockwise, as a gauge reads
    if (s > 0.0f) ring(radius * 1.08f, BRUSH_PI * 0.5f, BRUSH_PI * 0.5f - 2.0f * BRUSH_PI * s, arcSegments, Vector4(tint.x, tint.y, tint.z, tint.w));
    // the centre tick along the normal: which way the surface faces
    out.points.push_back(center);
    out.points.push_back(center + n * (radius * 0.25f));
    out.colors.push_back(Vector4(tint.x, tint.y, tint.z, tint.w * 0.6f));
}

editor::vertexcolor::MeshHit raycastMesh(const vc::MeshView& mesh, const Matrix4& model, const Vector3& origin, const Vector3& direction, bool twoSided) {
    MeshHit best;
    const Matrix4 inverse = model.inverse();
    const Vector3 localOrigin = inverse * origin;
    const Vector3 localEnd = inverse * (origin + direction);
    const Vector3 localDir = localEnd - localOrigin;
    float bestT = std::numeric_limits<float>::max();
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
        const Vector3& a = mesh.positions[mesh.indices[t * 3 + 0]];
        const Vector3& b = mesh.positions[mesh.indices[t * 3 + 1]];
        const Vector3& c = mesh.positions[mesh.indices[t * 3 + 2]];
        const Vector3 e1 = b - a;
        const Vector3 e2 = c - a;
        const Vector3 p = localDir.crossProduct(e2);
        const float det = e1.dotProduct(p);
        // counter-clockwise faces face the ray when det > 0
        if (twoSided ? std::fabs(det) < 1e-12f : det < 1e-12f) continue;
        const float inv = 1.0f / det;
        const Vector3 s = localOrigin - a;
        const float bu = s.dotProduct(p) * inv;
        if (bu < 0.0f || bu > 1.0f) continue;
        const Vector3 q = s.crossProduct(e1);
        const float bv = localDir.dotProduct(q) * inv;
        if (bv < 0.0f || bu + bv > 1.0f) continue;
        const float hitT = e2.dotProduct(q) * inv;
        if (hitT < 0.0f || hitT >= bestT) continue;
        bestT = hitT;
        best.hit = true;
        best.triangle = t;
        best.barycentric = Vector3(1.0f - bu - bv, bu, bv);
    }
    if (!best.hit) return best;
    best.point = surfacePoint(mesh, model, best.triangle, best.barycentric);
    const float length = direction.length();
    best.distance = length > 0.0f ? (best.point - origin).dotProduct(direction / length) : 0.0f;
    const Matrix4 normalMatrix = inverse.transpose();
    const uint32_t t = best.triangle;
    const Vector3 face = t < mesh.faceNormals.size() ? mesh.faceNormals[t]
        : (mesh.positions[mesh.indices[t * 3 + 1]] - mesh.positions[mesh.indices[t * 3]]).crossProduct(mesh.positions[mesh.indices[t * 3 + 2]] - mesh.positions[mesh.indices[t * 3]]);
    best.normal = worldNormal(normalMatrix, face);
    if (best.normal.dotProduct(direction) > 0.0f) best.normal = -best.normal;
    return best;
}

Vector3 surfacePoint(const vc::MeshView& mesh, const Matrix4& model, uint32_t triangle, const Vector3& barycentric) {
    if ((size_t)triangle * 3 + 2 >= mesh.indices.size()) return model * Vector3();
    const Vector3 local = mesh.positions[mesh.indices[triangle * 3 + 0]] * barycentric.x +
                          mesh.positions[mesh.indices[triangle * 3 + 1]] * barycentric.y +
                          mesh.positions[mesh.indices[triangle * 3 + 2]] * barycentric.z;
    return model * local;
}

uint32_t elementAtHit(const vc::MeshView& mesh, vc::ElementKind kind, uint32_t triangle, const Vector3& barycentric) {
    int k = 0;
    if (barycentric.y > barycentric[(unsigned)k]) k = 1;
    if (barycentric.z > barycentric[(unsigned)k]) k = 2;
    switch (kind) {
        case vc::ElementKind::Face: return triangle;
        case vc::ElementKind::Corner: return triangle * 3 + (uint32_t)k;
        case vc::ElementKind::Vertex: return mesh.logical[mesh.indices[triangle * 3 + (uint32_t)k]];
    }
    return triangle;
}

vc::Selection selectInRect(const vc::MeshView& mesh, const Matrix4& model, vc::ElementKind kind, const Vector2& a, const Vector2& b,
                                        const ProjectFn& project, const VisibleFn& visible) {
    const float minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x);
    const float minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);
    return selectRegion(mesh, model, kind, project, visible,
                        [&](const Vector2& p) { return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY; });
}

vc::Selection selectInLasso(const vc::MeshView& mesh, const Matrix4& model, vc::ElementKind kind, const std::vector<Vector2>& lasso,
                                         const ProjectFn& project, const VisibleFn& visible) {
    if (lasso.size() < 3) {
        vc::Selection none;
        none.kind = kind;
        return none;
    }
    return selectRegion(mesh, model, kind, project, visible, [&](const Vector2& p) { return pointInPolygon(lasso, p); });
}

bool pointInPolygon(const std::vector<Vector2>& polygon, const Vector2& point) {
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vector2& a = polygon[i];
        const Vector2& b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

vc::Selection combineSelection(const vc::Selection& current, const vc::Selection& picked, bool add, bool remove) {
    if (remove) {
        if (current.kind != picked.kind) return current;
        return vc::subtract(current, picked);
    }
    if (add && current.kind == picked.kind) return vc::unite(current, picked);
    return picked;
}

std::string paintLegend(const CommandCatalog& catalog, PaintTool tool) {
    auto key = [&](const char* id) {
        const std::string text = catalog.bindingsText(id);
        return text.empty() ? std::string("-") : text;
    };
    switch (tool) {
        case PaintTool::Select:
            return "Click or drag to select. Shift adds, Ctrl removes. " + key("vertexPaint.apply") + " applies the color.";
        case PaintTool::Paint:
            return "Ctrl erases, Shift paints the secondary color, Alt+click picks. Ctrl+Right drag: size and strength.";
        case PaintTool::Smooth:
            return "Ctrl sharpens. Ctrl+Right drag: size and strength. Shift+Right drag: hardness.";
        case PaintTool::Fill:
            return "Click fills the connected part (or the selection). Ctrl+click fills every face of the clicked color.";
        case PaintTool::Gradient:
            return "Drag from the primary to the secondary color, across the selection or the whole mesh.";
        case PaintTool::Pick:
            return "Click takes the stored color under the cursor; Shift+click into the secondary.";
    }
    return {};
}

}  // namespace jasperengine::editor::vertexcolor
