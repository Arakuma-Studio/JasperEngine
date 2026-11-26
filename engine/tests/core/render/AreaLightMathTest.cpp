// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Area Light shading (render/AreaLightMath.h, the CPU form of includes/area_lights.glsl)
// against numeric integrals over the shape (spec S99 AC8): the Standard tier's form factor and the
// High tier's LTC diffuse and GGX specular within 5 % (15 % where the receiver's horizon cuts the
// light), the representative point's specular energy
// in the point-light limit.

#include <gtest/gtest.h>

#include "render/AreaLightMath.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace AreaLightMathTest_cpp {

using namespace jasperengine;
using namespace jasperengine::AreaLightMath;

namespace {
    constexpr double TEST_PI = 3.14159265358979;

    Shape makeShape(float halfWidth, float halfHeight, bool disc){
        Shape shape;
        shape.center = Vector3(0.0f, 0.0f, 0.0f);
        shape.normal = Vector3(0.0f, 0.0f, 1.0f);
        shape.right = Vector3(1.0f, 0.0f, 0.0f);
        shape.up = Vector3(0.0f, 1.0f, 0.0f);
        shape.halfWidth = halfWidth;
        shape.halfHeight = halfHeight;
        shape.disc = disc;
        return shape;
    }

    // the integral over the shape's area of f(direction to the element) times its solid angle
    template<class F>
    double integrateShape(const Shape& shape, const Vector3& position, F f, int n){
        double sum = 0.0;
        const double dA = (2.0 * shape.halfWidth / n) * (2.0 * shape.halfHeight / n);
        for (int j = 0; j < n; j++){
            for (int i = 0; i < n; i++){
                const double u = (i + 0.5) / n * 2.0 - 1.0;
                const double v = (j + 0.5) / n * 2.0 - 1.0;
                if (shape.disc && u * u + v * v > 1.0)
                    continue;
                const Vector3 element = shape.center + shape.right * (float)(u * shape.halfWidth) + shape.up * (float)(v * shape.halfHeight);
                const Vector3 toElement = element - position;
                const double r2 = toElement.squaredLength();
                const Vector3 l = toElement / (float)std::sqrt(r2);
                const double emitter = std::max(-(double)l.dotProduct(shape.normal), 0.0);
                sum += f(l) * emitter / r2 * dA;
            }
        }
        return sum;
    }

    // Lit's GGX with F = 1 (D * Vis), times n.l
    double ggxTimesCos(const Vector3& n, const Vector3& l, const Vector3& v, double alpha){
        const Vector3 h = (l + v).normalized();
        const double NdotL = std::max((double)n.dotProduct(l), 0.0);
        const double NdotV = std::max((double)n.dotProduct(v), 1e-4);
        const double NdotH = std::max((double)n.dotProduct(h), 0.0);
        const double a2 = alpha * alpha;
        const double f = NdotH * NdotH * (a2 - 1.0) + 1.0;
        const double D = a2 / (TEST_PI * f * f);
        const double vis = 0.5 / std::max(NdotL * std::sqrt(NdotV * NdotV * (1.0 - a2) + a2) + NdotV * std::sqrt(NdotL * NdotL * (1.0 - a2) + a2), 1e-7);
        return D * vis * NdotL;
    }

    struct Receiver{
        Vector3 position;
        Vector3 normal;
    };

    // in front of the light, from half its size to eight times it, on and off its axis, facing it,
    // facing down and tilted
    std::vector<Receiver> receivers(){
        std::vector<Receiver> out;
        for (float z : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f}){
            for (auto offset : std::vector<std::pair<float, float>>{{0.0f, 0.0f}, {0.3f, 0.2f}, {0.5f, 0.0f}, {1.0f, 0.0f}, {2.0f, 1.0f}}){
                const Vector3 position(offset.first, offset.second, z);
                out.push_back({position, (position * -1.0f).normalized()});
                out.push_back({position, Vector3(0.0f, 0.0f, -1.0f)});
                out.push_back({position, Vector3(0.5f, 0.3f, -1.0f).normalized()});
            }
        }
        return out;
    }

    const std::vector<std::pair<float, float>> kSizes = {{0.5f, 0.5f}, {1.0f, 0.25f}};

    // the tolerance: 5 % where the receiver's horizon leaves the light whole (the vector form factor
    // is exact there), 15 % where it cuts it (the sphere approximation of the clipped part)
    double tolerance(const Shape& shape, const Receiver& r){
        Vector3 polygon[MAX_CORNERS];
        const int count = corners(shape, polygon);
        for (int i = 0; i < count; i++){
            if (r.normal.dotProduct(polygon[i] - r.position) <= 0.0f)
                return 0.15;
        }
        return 0.05;
    }
}

TEST(AreaLightMath, StandardFormFactorMatchesTheIntegral){
    for (bool disc : {false, true}){
        for (auto size : kSizes){
            const Shape shape = makeShape(size.first, size.second, disc);
            for (const Receiver& r : receivers()){
                const double reference = integrateShape(shape, r.position, [&](const Vector3& l){
                    return std::max((double)r.normal.dotProduct(l), 0.0) / TEST_PI;
                }, 256);
                if (reference < 1e-3)
                    continue;
                Vector3 direction;
                const double standard = diffuseFormFactor(shape, r.position, r.normal, direction);
                EXPECT_NEAR(standard / reference, 1.0, tolerance(shape, r)) << (disc ? "disc " : "rectangle ") << size.first << "x" << size.second
                    << " at (" << r.position.x << ", " << r.position.y << ", " << r.position.z << ")";
            }
        }
    }
}

TEST(AreaLightMath, LtcDiffuseMatchesTheIntegral){
    const float identity[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    for (bool disc : {false, true}){
        for (auto size : kSizes){
            const Shape shape = makeShape(size.first, size.second, disc);
            for (const Receiver& r : receivers()){
                const double reference = integrateShape(shape, r.position, [&](const Vector3& l){
                    return std::max((double)r.normal.dotProduct(l), 0.0) / TEST_PI;
                }, 256);
                if (reference < 1e-3)
                    continue;
                const Vector3 view = (r.normal + Vector3(0.3f, 0.0f, 0.0f)).normalized();
                const double ltc = ltcIntegral(shape, r.position, r.normal, view, identity, ltcSphere);
                EXPECT_NEAR(ltc / reference, 1.0, tolerance(shape, r)) << (disc ? "disc " : "rectangle ") << size.first << "x" << size.second
                    << " at (" << r.position.x << ", " << r.position.y << ", " << r.position.z << ")";
            }
        }
    }
}

TEST(AreaLightMath, LtcSpecularMatchesTheGgxIntegral){
    for (bool disc : {false, true}){
        const Shape shape = makeShape(0.5f, 0.5f, disc);
        for (float roughness : {0.25f, 0.5f, 0.8f}){
            for (float z : {0.5f, 1.0f, 2.0f, 4.0f}){
                for (float viewX : {0.0f, 0.5f}){
                    const Vector3 position(0.3f, 0.1f, z);
                    const Vector3 normal(0.0f, 0.0f, -1.0f);
                    const Vector3 view = Vector3(viewX, 0.1f, -1.0f).normalized();
                    const double alpha = roughness * roughness;
                    const double reference = integrateShape(shape, position, [&](const Vector3& l){
                        return ggxTimesCos(normal, l, view, alpha);
                    }, 400);
                    float ltc1[4], ltc2[4];
                    ltcLookup(roughness, normal.dotProduct(view), ltc1, ltc2);
                    // f0 = 1: the albedo term alone
                    const double ltc = ltcIntegral(shape, position, normal, view, ltc1, ltcSphere) * ltc2[0];
                    EXPECT_NEAR(ltc / reference, 1.0, 0.05) << (disc ? "disc" : "rectangle") << " roughness " << roughness
                        << " z " << z << " view x " << viewX;
                }
            }
        }
    }
}

// The representative point's specular, integrated over the view hemisphere (cosine weighted),
// against the same integral of the exact GGX over the shape: the energy normalisation keeps it
// within 5 % for a light small in the receiver's sky, and within the measured band for larger ones
TEST(AreaLightMath, RepresentativePointSpecularEnergy){
    const int views = 16;
    auto energyRatio = [&](const Shape& shape, const Vector3& position, double roughness){
        const Vector3 normal(0.0f, 0.0f, -1.0f);
        const double alpha = roughness * roughness;
        double exact = 0.0;
        double representative = 0.0;
        for (int i = 0; i < views; i++){
            for (int j = 0; j < 2 * views; j++){
                const double cosTheta = 1.0 - (i + 0.5) / views;
                const double theta = std::acos(cosTheta);
                const double phi = (j + 0.5) / (2.0 * views) * 2.0 * TEST_PI;
                const Vector3 view((float)(std::sin(theta) * std::cos(phi)), (float)(std::sin(theta) * std::sin(phi)), (float)-cosTheta);
                exact += integrateShape(shape, position, [&](const Vector3& l){ return ggxTimesCos(normal, l, view, alpha); }, 48) * cosTheta;
                Vector3 direction;
                const double irradiance = specularIrradiance(shape, position, normal, view, (float)alpha, direction);
                representative += irradiance * ggxTimesCos(normal, direction, view, alpha) * cosTheta;
            }
        }
        return representative / exact;
    };
    for (bool disc : {false, true}){
        for (double roughness : {0.4, 0.7}){
            // the point-light limit: 0.04 m across at 2 m
            EXPECT_NEAR(energyRatio(makeShape(0.02f, 0.02f, disc), Vector3(0.1f, 0.0f, 2.0f), roughness), 1.0, 0.05)
                << (disc ? "disc" : "rectangle") << " roughness " << roughness;
            // 1 m at 2 m and at 4 m
            for (float z : {2.0f, 4.0f}){
                const double ratio = energyRatio(makeShape(0.5f, 0.5f, disc), Vector3(0.3f, 0.1f, z), roughness);
                EXPECT_GT(ratio, 0.6) << (disc ? "disc" : "rectangle") << " roughness " << roughness << " z " << z;
                EXPECT_LT(ratio, 1.3) << (disc ? "disc" : "rectangle") << " roughness " << roughness << " z " << z;
            }
        }
    }
}

TEST(AreaLightMath, BehindTheLightIsUnlit){
    const Shape shape = makeShape(0.5f, 0.5f, false);
    const Vector3 behind(0.0f, 0.0f, -1.0f);
    const Vector3 up(0.0f, 0.0f, 1.0f);
    Vector3 direction;
    EXPECT_EQ(diffuseFormFactor(shape, behind, up, direction), 0.0f);
    EXPECT_EQ(specularIrradiance(shape, behind, up, up, 0.25f, direction), 0.0f);
    const float identity[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    EXPECT_EQ(ltcIntegral(shape, behind, up, up, identity, ltcSphere), 0.0f);
}

TEST(AreaLightMath, StaysFiniteAtContact){
    // a receiver touching the light's centre and edge, facing it: finite, at most the full hemisphere
    for (bool disc : {false, true}){
        const Shape shape = makeShape(0.5f, 0.5f, disc);
        for (const Vector3& position : {Vector3(0.0f, 0.0f, 1e-5f), Vector3(0.5f, 0.0f, 1e-5f)}){
            Vector3 direction;
            const float formFactor = diffuseFormFactor(shape, position, Vector3(0.0f, 0.0f, -1.0f), direction);
            EXPECT_TRUE(std::isfinite(formFactor));
            EXPECT_LE(formFactor, 1.0f + 1e-3f);
            const float specular = specularIrradiance(shape, position, Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 0.0f, -1.0f), 0.1f, direction);
            EXPECT_TRUE(std::isfinite(specular));
            EXPECT_TRUE(std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z));
        }
    }
}

}  // namespace AreaLightMathTest_cpp
