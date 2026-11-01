// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleShapes.h"
#include "ParticleSimulation.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr float PI = 3.14159265358979323846f;
    constexpr float DEG_TO_RAD = PI / 180.0f;

    float lerp(float a, float b, float f){
        return a + (b - a) * f;
    }

    float clamp01(float v){
        if (!(v >= 0.0f)) return 0.0f;
        return v > 1.0f ? 1.0f : v;
    }

    // the radial fraction of a volume of revolution (power 3) or a disc (power 2) between the
    // inner radius (1 - thickness) and 1, uniform in its volume / area
    float thicknessFraction(float thickness, float u, int power){
        const float inner = 1.0f - clamp01(thickness);
        if (power == 3){
            const float inner3 = inner * inner * inner;
            return std::cbrt(lerp(inner3, 1.0f, u));
        }
        return std::sqrt(lerp(inner * inner, 1.0f, u));
    }

    Vector3 normalizedOr(const Vector3& v, const Vector3& fallback){
        const float length = v.length();
        if (!(length > 1e-8f)) return fallback;
        return v / length;
    }

    // a mode's 0 .. 1 parameter: Random, Loop (phase wraps), PingPong (back and forth),
    // BurstSpread (even within the burst); `spread` quantizes it (0: continuous)
    float multiModeValue(ParticleSystemShapeMultiModeValue mode, float spread, float phase, const ShapeContext& context, Pcg32& rng){
        float value;
        switch (mode){
            case ParticleSystemShapeMultiModeValue::Loop:
                value = phase - std::floor(phase);
                break;
            case ParticleSystemShapeMultiModeValue::PingPong: {
                const float p = phase - 2.0f * std::floor(phase * 0.5f);   // 0 .. 2
                value = p <= 1.0f ? p : 2.0f - p;
                break;
            }
            case ParticleSystemShapeMultiModeValue::BurstSpread:
                // even within a burst; outside one (rate emission) at random
                value = context.spreadCount > 0 ? (float)context.spreadIndex / (float)context.spreadCount : rng.next01();
                break;
            case ParticleSystemShapeMultiModeValue::Random:
            default:
                value = rng.next01();
                break;
        }
        if (spread > 0.0f){
            const float step = std::min(spread, 1.0f);
            value = std::floor(value / step + 1e-4f) * step;
            value = std::min(value, 1.0f);
        }
        return value;
    }

}

float particles::arcAngle(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng){
    const float arc = std::max(0.0f, std::min(shape.arc, 360.0f));
    if (arc <= 0.0f) return 0.0f;
    // the phase is in degrees: one arc length is one wrap
    const float phase = context.arcPhase / arc;
    return multiModeValue(shape.arcMode, shape.arcSpread, phase, context, rng) * arc;
}

float particles::radiusFraction(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng){
    return multiModeValue(shape.radiusMode, shape.radiusSpread, context.radiusPhase, context, rng);
}

Vector3 particles::randomUnitVector(Pcg32& rng){
    const float z = rng.next01() * 2.0f - 1.0f;
    const float phi = rng.next01() * 2.0f * PI;
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vector3(s * std::cos(phi), z, s * std::sin(phi));
}

ShapeSample particles::sampleShape(const ParticleShapeModule& shape, const ShapeContext& context, Pcg32& rng){
    ShapeSample sample;
    if (!shape.enabled){
        return sample;
    }

    const float radius = std::max(0.0f, shape.radius);
    Vector3 p(0, 0, 0);
    Vector3 dir(0, 1, 0);
    // the distance random keeps its own draw so a Loop arc does not move the radius
    const bool radiusByMode = shape.radiusMode != ParticleSystemShapeMultiModeValue::Random;

    switch (shape.shapeType){
        case ParticleSystemShapeType::Sphere:
        case ParticleSystemShapeType::Hemisphere: {
            const float phi = arcAngle(shape, context, rng) * DEG_TO_RAD;
            float y = rng.next01() * 2.0f - 1.0f;
            if (shape.shapeType == ParticleSystemShapeType::Hemisphere) y = std::fabs(y);
            const float s = std::sqrt(std::max(0.0f, 1.0f - y * y));
            dir = Vector3(s * std::cos(phi), y, s * std::sin(phi));
            const float u = radiusByMode ? radiusFraction(shape, context, rng) : rng.next01();
            p = dir * (radius * thicknessFraction(shape.radiusThickness, u, 3));
            break;
        }
        case ParticleSystemShapeType::Cone:
        case ParticleSystemShapeType::ConeVolume: {
            const float phi = arcAngle(shape, context, rng) * DEG_TO_RAD;
            const float u = radiusByMode ? radiusFraction(shape, context, rng) : rng.next01();
            const float f = thicknessFraction(shape.radiusThickness, u, 2);
            const float angle = std::max(0.0f, std::min(shape.angle, 89.99f)) * DEG_TO_RAD * f;
            const float c = std::cos(phi), s = std::sin(phi);
            p = Vector3(radius * f * c, 0.0f, radius * f * s);
            dir = Vector3(std::sin(angle) * c, std::cos(angle), std::sin(angle) * s);
            if (shape.shapeType == ParticleSystemShapeType::ConeVolume){
                p += dir * (std::max(0.0f, shape.length) * rng.next01());
            }
            break;
        }
        case ParticleSystemShapeType::Box:
            p = Vector3(rng.next01() - 0.5f, rng.next01() - 0.5f, rng.next01() - 0.5f);
            break;
        case ParticleSystemShapeType::BoxShell: {
            // a face by its area (the shape's scale sizes the box), then inset by the thickness
            const Vector3 sc(std::fabs(shape.scale.x), std::fabs(shape.scale.y), std::fabs(shape.scale.z));
            const float areas[3] = {sc.y * sc.z, sc.x * sc.z, sc.x * sc.y};
            const float total = areas[0] + areas[1] + areas[2];
            float pick = rng.next01() * (total > 0.0f ? total : 3.0f);
            int axis = 0;
            if (total > 0.0f){
                while (axis < 2 && pick >= areas[axis]){ pick -= areas[axis]; axis++; }
            }else{
                axis = std::min(2, (int)pick);
            }
            const float sign = rng.next01() < 0.5f ? -1.0f : 1.0f;
            const float thickness[3] = {clamp01(shape.boxThickness.x), clamp01(shape.boxThickness.y), clamp01(shape.boxThickness.z)};
            float v[3] = {rng.next01() - 0.5f, rng.next01() - 0.5f, rng.next01() - 0.5f};
            v[axis] = sign * (0.5f - 0.5f * thickness[axis] * rng.next01());
            p = Vector3(v[0], v[1], v[2]);
            break;
        }
        case ParticleSystemShapeType::BoxEdge: {
            // an edge by its length, then inset towards the centre by the thickness
            const Vector3 sc(std::fabs(shape.scale.x), std::fabs(shape.scale.y), std::fabs(shape.scale.z));
            const float lengths[3] = {sc.x, sc.y, sc.z};
            const float total = lengths[0] + lengths[1] + lengths[2];
            float pick = rng.next01() * (total > 0.0f ? total : 3.0f);
            int axis = 0;
            if (total > 0.0f){
                while (axis < 2 && pick >= lengths[axis]){ pick -= lengths[axis]; axis++; }
            }else{
                axis = std::min(2, (int)pick);
            }
            const float thickness[3] = {clamp01(shape.boxThickness.x), clamp01(shape.boxThickness.y), clamp01(shape.boxThickness.z)};
            float v[3];
            for (int a = 0; a < 3; a++){
                const float sign = rng.next01() < 0.5f ? -1.0f : 1.0f;
                v[a] = sign * (0.5f - 0.5f * thickness[a] * rng.next01());
            }
            v[axis] = rng.next01() - 0.5f;
            p = Vector3(v[0], v[1], v[2]);
            break;
        }
        case ParticleSystemShapeType::Circle: {
            const float phi = arcAngle(shape, context, rng) * DEG_TO_RAD;
            const float u = radiusByMode ? radiusFraction(shape, context, rng) : rng.next01();
            const float f = thicknessFraction(shape.radiusThickness, u, 2);
            dir = Vector3(std::cos(phi), 0.0f, std::sin(phi));
            p = dir * (radius * f);
            break;
        }
        case ParticleSystemShapeType::Edge: {
            const float f = radiusFraction(shape, context, rng);
            p = Vector3((f * 2.0f - 1.0f) * radius, 0.0f, 0.0f);
            break;
        }
        case ParticleSystemShapeType::Rectangle:
            p = Vector3(rng.next01() - 0.5f, 0.0f, rng.next01() - 0.5f);
            break;
        case ParticleSystemShapeType::Donut: {
            const float phi = arcAngle(shape, context, rng) * DEG_TO_RAD;
            const Vector3 radial(std::cos(phi), 0.0f, std::sin(phi));
            const Vector3 ring = radial * radius;
            const float theta = rng.next01() * 2.0f * PI;
            const float u = radiusByMode ? radiusFraction(shape, context, rng) : rng.next01();
            const float rho = std::max(0.0f, shape.donutRadius) * thicknessFraction(shape.radiusThickness, u, 2);
            const Vector3 tube = radial * std::cos(theta) + Vector3(0, 1, 0) * std::sin(theta);
            p = ring + tube * rho;
            dir = tube;
            break;
        }
    }

    // the shape's own transform: scale, then rotation, then position
    const Quaternion rotation = eulerRadians(shape.rotation * DEG_TO_RAD);
    p = Vector3(p.x * shape.scale.x, p.y * shape.scale.y, p.z * shape.scale.z);
    p = rotation * p + shape.position;
    dir = rotation * dir;

    if (shape.randomDirectionAmount > 0.0f){
        dir = normalizedOr(dir + (randomUnitVector(rng) - dir) * clamp01(shape.randomDirectionAmount), dir);
    }
    if (shape.sphericalDirectionAmount > 0.0f){
        const Vector3 outward = normalizedOr(p - shape.position, dir);
        dir = normalizedOr(dir + (outward - dir) * clamp01(shape.sphericalDirectionAmount), dir);
    }
    if (shape.randomPositionAmount > 0.0f){
        p += randomUnitVector(rng) * (shape.randomPositionAmount * std::cbrt(rng.next01()));
    }

    sample.position = p;
    sample.direction = normalizedOr(dir, Vector3(0, 1, 0));
    return sample;
}
