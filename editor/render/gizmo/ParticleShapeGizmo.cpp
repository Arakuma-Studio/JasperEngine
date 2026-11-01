// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleShapeGizmo.h"

#include "particles/ParticleSimulation.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    constexpr float PI = 3.14159265358979323846f;
    constexpr float DEG = PI / 180.0f;
    constexpr int SEGMENTS = 48;

    using Segments = std::vector<std::pair<Vector3, Vector3>>;

    // an arc of `degrees` from +X towards +Z around `center`, in the plane of `u` (angle 0) and `v` (90)
    void arc(Segments& out, const Vector3& center, const Vector3& u, const Vector3& v, float radius, float degrees = 360.0f){
        const int steps = std::max(4, (int)std::ceil(SEGMENTS * std::min(360.0f, std::fabs(degrees)) / 360.0f));
        Vector3 previous = center + u * radius;
        for (int i = 1; i <= steps; i++){
            const float a = degrees * DEG * (float)i / (float)steps;
            const Vector3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
            out.push_back({previous, p});
            previous = p;
        }
    }

    void box(Segments& out, const Vector3& half){
        const Vector3 c[8] = {
            Vector3(-half.x, -half.y, -half.z), Vector3(half.x, -half.y, -half.z), Vector3(half.x, -half.y, half.z), Vector3(-half.x, -half.y, half.z),
            Vector3(-half.x, half.y, -half.z), Vector3(half.x, half.y, -half.z), Vector3(half.x, half.y, half.z), Vector3(-half.x, half.y, half.z)};
        for (int i = 0; i < 4; i++){
            out.push_back({c[i], c[(i + 1) % 4]});
            out.push_back({c[4 + i], c[4 + (i + 1) % 4]});
            out.push_back({c[i], c[4 + i]});
        }
    }

}

std::vector<std::pair<Vector3, Vector3>> editor::particleShapeGizmoSegments(const ParticleShapeModule& shape){
    Segments out;
    if (!shape.enabled) return out;
    const float radius = std::max(0.0f, shape.radius);
    const float arcDegrees = std::max(0.0f, std::min(360.0f, shape.arc));
    const Vector3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1), O(0, 0, 0);

    switch (shape.shapeType){
        case ParticleSystemShapeType::Sphere:
            arc(out, O, X, Z, radius, arcDegrees);
            arc(out, O, X, Y, radius);
            arc(out, O, Z, Y, radius);
            break;
        case ParticleSystemShapeType::Hemisphere:
            arc(out, O, X, Z, radius, arcDegrees);
            arc(out, O, X, Y, radius, 180.0f);
            arc(out, O, Z, Y, radius, 180.0f);
            break;
        case ParticleSystemShapeType::Cone:
        case ParticleSystemShapeType::ConeVolume: {
            const float angle = std::max(0.0f, std::min(shape.angle, 89.99f)) * DEG;
            const float length = std::max(0.0f, shape.length);
            const float far = radius + length * std::tan(angle);
            arc(out, O, X, Z, radius, arcDegrees);
            arc(out, Y * length, X, Z, far, arcDegrees);
            const float sides[4] = {0.0f, 90.0f, 180.0f, 270.0f};
            for (float side : sides){
                if (side > arcDegrees + 1e-3f) continue;
                const Vector3 d(std::cos(side * DEG), 0.0f, std::sin(side * DEG));
                out.push_back({d * radius, d * far + Y * length});
            }
            break;
        }
        case ParticleSystemShapeType::Box:
        case ParticleSystemShapeType::BoxShell:
        case ParticleSystemShapeType::BoxEdge:
            box(out, Vector3(0.5f, 0.5f, 0.5f));
            break;
        case ParticleSystemShapeType::Circle:
            arc(out, O, X, Z, radius, arcDegrees);
            if (arcDegrees < 360.0f){
                out.push_back({O, X * radius});
                out.push_back({O, (X * std::cos(arcDegrees * DEG) + Z * std::sin(arcDegrees * DEG)) * radius});
            }
            break;
        case ParticleSystemShapeType::Edge:
            out.push_back({X * -radius, X * radius});
            // the side it emits to
            out.push_back({O, Y * (std::max(radius, 0.1f) * 0.25f)});
            break;
        case ParticleSystemShapeType::Rectangle:
            out.push_back({Vector3(-0.5f, 0, -0.5f), Vector3(0.5f, 0, -0.5f)});
            out.push_back({Vector3(0.5f, 0, -0.5f), Vector3(0.5f, 0, 0.5f)});
            out.push_back({Vector3(0.5f, 0, 0.5f), Vector3(-0.5f, 0, 0.5f)});
            out.push_back({Vector3(-0.5f, 0, 0.5f), Vector3(-0.5f, 0, -0.5f)});
            break;
        case ParticleSystemShapeType::Donut: {
            const float tube = std::max(0.0f, shape.donutRadius);
            arc(out, O, X, Z, radius, arcDegrees);
            arc(out, O, X, Z, radius + tube, arcDegrees);
            arc(out, O, X, Z, std::max(0.0f, radius - tube), arcDegrees);
            arc(out, Y * tube, X, Z, radius, arcDegrees);
            arc(out, Y * -tube, X, Z, radius, arcDegrees);
            break;
        }
    }

    // the shape's own transform, as the simulation applies it
    const Quaternion rotation = particles::eulerRadians(shape.rotation * DEG);
    for (auto& segment : out){
        for (Vector3* p : {&segment.first, &segment.second}){
            *p = rotation * Vector3(p->x * shape.scale.x, p->y * shape.scale.y, p->z * shape.scale.z) + shape.position;
        }
    }
    return out;
}
