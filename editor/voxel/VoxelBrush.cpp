// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelBrush.h"

#include "shortcuts/CommandCatalog.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int DECAL_RING_POINTS = 48;
    constexpr int DECAL_CIRCLE_POINTS = 32;
    constexpr double DECAL_PI = 3.14159265358979323846;
    constexpr double DECAL_REST_SECONDS = 0.5;
    constexpr double DECAL_FADE_SECONDS = 0.25;

    voxel::Vec3d offsetBy(const voxel::Vec3d& a, const voxel::Vec3d& b, double scale = 1.0) {
        return voxel::Vec3d{a.x + b.x * scale, a.y + b.y * scale, a.z + b.z * scale};
    }

    voxel::Vec3d crossOf(const voxel::Vec3d& a, const voxel::Vec3d& b) {
        return voxel::Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    double lengthOf(const voxel::Vec3d& a) {
        return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    }

    voxel::Vec3d unitOf(const voxel::Vec3d& a) {
        const double size = lengthOf(a);
        return size > 1e-12 ? voxel::Vec3d{a.x / size, a.y / size, a.z / size} : voxel::Vec3d{0.0, 1.0, 0.0};
    }

    // two axes perpendicular to n
    void tangentsOf(const voxel::Vec3d& n, voxel::Vec3d& u, voxel::Vec3d& v) {
        const voxel::Vec3d helper = std::fabs(n.y) < 0.9 ? voxel::Vec3d{0.0, 1.0, 0.0} : voxel::Vec3d{1.0, 0.0, 0.0};
        u = unitOf(crossOf(helper, n));
        v = crossOf(n, u);
    }

    void addWireCircle(std::vector<voxel::Vec3d>& lines, const voxel::Vec3d& center, const voxel::Vec3d& u, const voxel::Vec3d& v, double radius) {
        voxel::Vec3d previous = offsetBy(center, u, radius);
        for (int i = 1; i <= DECAL_CIRCLE_POINTS; i++) {
            const double angle = 2.0 * DECAL_PI * static_cast<double>(i) / DECAL_CIRCLE_POINTS;
            const voxel::Vec3d point = offsetBy(offsetBy(center, u, radius * std::cos(angle)), v, radius * std::sin(angle));
            lines.push_back(previous);
            lines.push_back(point);
            previous = point;
        }
    }

    void addWireBox(std::vector<voxel::Vec3d>& lines, const voxel::Vec3d& center, const voxel::Vec3d& half) {
        const auto corner = [&](int i) {
            return voxel::Vec3d{center.x + ((i & 1) ? half.x : -half.x), center.y + ((i & 2) ? half.y : -half.y), center.z + ((i & 4) ? half.z : -half.z)};
        };
        const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) {
            lines.push_back(corner(edge[0]));
            lines.push_back(corner(edge[1]));
        }
    }

}

const std::vector<VoxelBrushPreset>& editor::voxelBrushPresets() {
    static const std::vector<VoxelBrushPreset> presets = {
        {"Tunnel", "A round bore: drag along the way it should run", VoxelTool::Dig, VoxelBrushShape::Sphere, 3.0f, 0.5f, 0.0f, 0.5f, 1},
        {"Crater", "One wide, soft-edged scoop", VoxelTool::Dig, VoxelBrushShape::Sphere, 8.0f, 3.0f, 0.0f, 0.5f, 1},
        {"Smooth Wall", "Calms a rough surface without moving it far", VoxelTool::Smooth, VoxelBrushShape::Sphere, 4.0f, 0.0f, 0.5f, 0.7f, 3},
        {"Overhang Chisel", "A small hard box for undercutting a face", VoxelTool::Dig, VoxelBrushShape::Box, 1.5f, 0.0f, 0.0f, 0.5f, 1},
    };
    return presets;
}

void editor::applyVoxelBrushPreset(VoxelBrushSettings& settings, const VoxelBrushPreset& preset) {
    settings.tool = preset.tool;
    settings.shape = preset.shape;
    settings.radius = preset.radius;
    settings.blend = preset.blend;
    settings.falloff = preset.falloff;
    settings.amount = preset.amount;
    settings.iterations = preset.iterations;
}

VoxelTool editor::voxelEffectiveTool(VoxelTool tool, bool ctrl, bool shift) {
    if (tool == VoxelTool::Pick) return tool;
    if (shift) return VoxelTool::Smooth;
    if (ctrl && tool == VoxelTool::Dig) return VoxelTool::Add;
    if (ctrl && tool == VoxelTool::Add) return VoxelTool::Dig;
    return tool;
}

VoxelDecalColour editor::voxelDecalColour(VoxelTool effective) {
    switch (effective) {
        case VoxelTool::Add: return {1.0f, 1.0f, 1.0f};
        case VoxelTool::Dig: return {0.2f, 0.2f, 0.2f};
        case VoxelTool::Smooth: return {0.5f, 0.0f, 0.2f};
        case VoxelTool::Paint: return {0.0f, 0.5f, 0.0f};
        case VoxelTool::Pick: return {1.0f, 1.0f, 1.0f};
    }
    return {};
}

const char* editor::voxelToolName(VoxelTool tool) {
    switch (tool) {
        case VoxelTool::Dig: return "Dig";
        case VoxelTool::Add: return "Add";
        case VoxelTool::Smooth: return "Smooth";
        case VoxelTool::Paint: return "Paint";
        case VoxelTool::Pick: return "Pick";
    }
    return "";
}

const char* editor::voxelShapeName(VoxelBrushShape shape) {
    switch (shape) {
        case VoxelBrushShape::Sphere: return "Sphere";
        case VoxelBrushShape::Box: return "Box";
        case VoxelBrushShape::Capsule: return "Capsule";
        case VoxelBrushShape::Mesh: return "Mesh";
        case VoxelBrushShape::Stamp: return "Stamp";
    }
    return "";
}

std::string editor::voxelStrokeName(VoxelTool effective, VoxelBrushShape shape) {
    return std::string(voxelToolName(effective)) + " " + voxelShapeName(shape);
}

std::string editor::voxelBrushLegend(const CommandCatalog& catalog) {
    const auto key = [&](const char* id) { return catalog.bindingsText(id); };
    return key("voxel.tool.dig") + " Dig  " + key("voxel.tool.add") + " Add  " + key("voxel.tool.smooth") + " Smooth  " + key("voxel.tool.paint") +
           " Paint  " + key("voxel.tool.pick") + " Pick  " + key("voxel.brushSmaller") + " " + key("voxel.brushLarger") +
           " Size  Ctrl Invert  Shift Smooth  Alt+Click Pick  Alt+Drag Orbit";
}

double editor::voxelStrokeSpacing(double radius, double voxelSize) {
    return std::max(0.25 * radius, 0.5 * voxelSize);
}

void VoxelStrokeStamper::start(const voxel::Vec3d& point, std::vector<voxel::Vec3d>& stamps) {
    last = point;
    travelled = 0.0;
    anchored = true;
    stamps.push_back(point);
}

void VoxelStrokeStamper::moveTo(const voxel::Vec3d& point, std::vector<voxel::Vec3d>& stamps) {
    if (!anchored) {
        start(point, stamps);
        return;
    }
    const voxel::Vec3d delta{point.x - last.x, point.y - last.y, point.z - last.z};
    const double distance = lengthOf(delta);
    if (distance <= 0.0) return;
    // walking from the segment's start, not from the last stamp, so a stamp's position does not
    // collect the rounding of the frames before it
    double along = spacing - travelled;
    while (along <= distance) {
        const double t = along / distance;
        stamps.push_back(voxel::Vec3d{last.x + delta.x * t, last.y + delta.y * t, last.z + delta.z * t});
        along += spacing;
    }
    travelled = distance - (along - spacing);
    last = point;
}

voxel::EditOp editor::voxelStampOp(const VoxelBrushSettings& settings, VoxelTool effective, const voxel::Vec3d& center, const voxel::Vec3d& end) {
    voxel::EditOp op;
    switch (effective) {
        case VoxelTool::Add: op.kind = voxel::EditKind::Add; break;
        case VoxelTool::Smooth: op.kind = voxel::EditKind::Smooth; break;
        case VoxelTool::Paint: op.kind = voxel::EditKind::Paint; break;
        case VoxelTool::Dig:
        case VoxelTool::Pick: op.kind = voxel::EditKind::Dig; break;
    }
    op.blend = settings.blend;
    op.falloff = settings.falloff;
    op.amount = settings.amount;
    op.iterations = settings.iterations;
    op.weight = settings.weight;
    if (effective == VoxelTool::Add || effective == VoxelTool::Paint) op.material = settings.material;

    voxel::Shape& shape = op.shape;
    shape.center = center;
    shape.radius = settings.radius;
    switch (settings.shape) {
        case VoxelBrushShape::Sphere:
            shape.kind = voxel::ShapeKind::Sphere;
            break;
        case VoxelBrushShape::Box:
            shape.kind = voxel::ShapeKind::Box;
            shape.halfExtents = voxel::Vec3d{settings.radius, settings.radius, settings.radius};
            break;
        case VoxelBrushShape::Capsule:
            shape.kind = voxel::ShapeKind::Capsule;
            shape.from = center;
            shape.to = end;
            break;
        case VoxelBrushShape::Mesh:
        case VoxelBrushShape::Stamp:
            shape.kind = settings.shape == VoxelBrushShape::Mesh ? voxel::ShapeKind::Mesh : voxel::ShapeKind::Stamp;
            shape.field = settings.field;
            shape.scale = settings.stampScale;
            break;
    }
    return op;
}

void VoxelAltGesture::press(float x, float y) {
    startX = x;
    startY = y;
    active = true;
    orbit = false;
}

bool VoxelAltGesture::move(float x, float y, float threshold) {
    if (!active) return false;
    if (!orbit) {
        const float dx = x - startX, dy = y - startY;
        orbit = dx * dx + dy * dy > threshold * threshold;
    }
    return orbit;
}

bool VoxelAltGesture::release() {
    const bool click = active && !orbit;
    active = false;
    orbit = false;
    return click;
}

void editor::buildVoxelDecal(voxel::Store& store, const VoxelBrushSettings& settings, VoxelTool effective, const voxel::RaycastHit& hit,
                             const voxel::Vec3d* capsuleStart, VoxelDecal& decal) {
    decal.ring.clear();
    decal.wire.clear();
    decal.colour = voxelDecalColour(effective);
    const double radius = settings.radius;

    // the ring: rays back down the normal, so it lies on the ground the brush will take
    const voxel::Vec3d normal = unitOf(hit.normal);
    voxel::Vec3d u, v;
    tangentsOf(normal, u, v);
    const double lift = std::max(radius, static_cast<double>(store.desc().voxelSize));
    const voxel::Vec3d down{-normal.x, -normal.y, -normal.z};
    const double surfaceOffset = 0.05 * static_cast<double>(store.desc().voxelSize);
    voxel::Vec3d previous;
    voxel::Vec3d first;
    for (int i = 0; i < DECAL_RING_POINTS; i++) {
        const double angle = 2.0 * DECAL_PI * static_cast<double>(i) / DECAL_RING_POINTS;
        const voxel::Vec3d onPlane = offsetBy(offsetBy(hit.point, u, radius * std::cos(angle)), v, radius * std::sin(angle));
        voxel::Vec3d point = onPlane;
        const voxel::RaycastHit draped = voxel::raycast(store, offsetBy(onPlane, normal, lift), down, lift * 2.0);
        if (draped.hit) point = offsetBy(draped.point, normal, surfaceOffset);
        if (i > 0) {
            decal.ring.push_back(previous);
            decal.ring.push_back(point);
        } else {
            first = point;
        }
        previous = point;
    }
    decal.ring.push_back(previous);
    decal.ring.push_back(first);

    const voxel::Vec3d x{1.0, 0.0, 0.0}, y{0.0, 1.0, 0.0}, z{0.0, 0.0, 1.0};
    switch (settings.shape) {
        case VoxelBrushShape::Sphere:
            addWireCircle(decal.wire, hit.point, x, y, radius);
            addWireCircle(decal.wire, hit.point, y, z, radius);
            addWireCircle(decal.wire, hit.point, x, z, radius);
            break;
        case VoxelBrushShape::Box:
            addWireBox(decal.wire, hit.point, voxel::Vec3d{radius, radius, radius});
            break;
        case VoxelBrushShape::Capsule: {
            const voxel::Vec3d from = capsuleStart ? *capsuleStart : hit.point;
            const voxel::Vec3d axis = unitOf(voxel::Vec3d{hit.point.x - from.x, hit.point.y - from.y, hit.point.z - from.z});
            voxel::Vec3d a, b;
            tangentsOf(axis, a, b);
            addWireCircle(decal.wire, from, a, b, radius);
            addWireCircle(decal.wire, hit.point, a, b, radius);
            for (const voxel::Vec3d& side : {a, b}) {
                decal.wire.push_back(offsetBy(from, side, radius));
                decal.wire.push_back(offsetBy(hit.point, side, radius));
                decal.wire.push_back(offsetBy(from, side, -radius));
                decal.wire.push_back(offsetBy(hit.point, side, -radius));
            }
            break;
        }
        case VoxelBrushShape::Mesh:
        case VoxelBrushShape::Stamp: {
            // the field's box, or the brush's size while it has none
            voxel::Vec3d half{radius, radius, radius};
            if (settings.field) {
                const double extent = settings.field->voxelSize * static_cast<double>(settings.stampScale) * 0.5;
                half = voxel::Vec3d{settings.field->size.x * extent, settings.field->size.y * extent, settings.field->size.z * extent};
            }
            addWireBox(decal.wire, hit.point, half);
            break;
        }
    }
}

float editor::voxelDecalAlpha(double secondsSincePointerMoved, bool cameraMoving) {
    if (cameraMoving) return 0.0f;
    if (secondsSincePointerMoved <= DECAL_REST_SECONDS) return 1.0f;
    const double faded = (secondsSincePointerMoved - DECAL_REST_SECONDS) / DECAL_FADE_SECONDS;
    return static_cast<float>(std::clamp(1.0 - faded, 0.0, 1.0));
}
