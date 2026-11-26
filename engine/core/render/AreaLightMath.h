// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AREALIGHTMATH_H
#define AREALIGHTMATH_H

#include "Export.h"
#include "math/Vector3.h"

namespace jasperengine {

    // The CPU form of the Area Light shading of includes/area_lights.glsl (spec S99 R33), line for
    // line, so the unit tests measure what the shader draws. A light of luminance L lights a
    // receiver with irradiance pi * L * formFactor; the shader multiplies that by Lit's
    // BRDF_lambertian, and the specular terms by the BRDF they stand for.
    namespace AreaLightMath {

        // the emitting shape: it emits from its front (along normal) only
        struct Shape {
            Vector3 center;
            Vector3 normal;       // forward, unit
            Vector3 right;        // unit, in the plane
            Vector3 up;           // unit, in the plane
            float halfWidth = 0.5f;   // a disc's radius, with halfHeight
            float halfHeight = 0.5f;
            bool disc = false;
        };

        // a disc is drawn as a regular polygon of the same area: an octagon for its diffuse (a smooth
        // term), sixteen sides for the LTC specular, whose sharp reflections would show an octagon
        constexpr int DISC_SIDES = 8;
        constexpr int DISC_SIDES_SPECULAR = 16;
        constexpr int MAX_CORNERS = DISC_SIDES_SPECULAR;

        JASPERENGINE_API float area(const Shape& shape);
        // the corners, counter-clockwise seen from the front: 4 for a rectangle, discSides for a disc
        JASPERENGINE_API int corners(const Shape& shape, Vector3 out[MAX_CORNERS], int discSides = DISC_SIDES);

        // the edge's share of the vector form factor of the polygon seen from the origin, between two
        // unit vectors (a rational fit of acos(x) / sin(acos(x)), as Hill and Heitz 2017 do)
        JASPERENGINE_API Vector3 edgeFormFactor(const Vector3& v1, const Vector3& v2);
        // the horizon-clipped form factor of a spherical cap whose vector form factor has length
        // sinSigmaSqr and its direction cosTheta from the normal (the ltcLut2.w table in closed form)
        JASPERENGINE_API float sphereFormFactor(float cosTheta, float sinSigmaSqr);

        // Standard and lower: the form factor of the shape at a receiver (its position and unit
        // normal), and the representative direction its Lambert term uses (the vector form factor's)
        JASPERENGINE_API float diffuseFormFactor(const Shape& shape, const Vector3& position, const Vector3& normal, Vector3& direction);

        // Standard and lower, specular: the most representative point (the reflection ray's hit,
        // clamped to the shape) as a direction, and the irradiance, per unit luminance, a point light
        // there would give, energy normalised for the GGX alpha (perceptual roughness squared).
        // The caller evaluates its GGX towards direction with that irradiance.
        JASPERENGINE_API float specularIrradiance(const Shape& shape, const Vector3& position, const Vector3& normal,
                                                  const Vector3& view, float alpha, Vector3& direction, float* widenedAlpha = nullptr);

        // High: the LTC integral of the shape (Heitz et al. 2016) under the inverse matrix's terms
        // m00, m02, m20, m22 in the frame of the normal and the view; identity terms give the form
        // factor. The horizon clipping takes the sphere approximation, sphere(z, length) being the
        // table the shader samples (sphereFormFactor's closed form when null).
        JASPERENGINE_API float ltcIntegral(const Shape& shape, const Vector3& position, const Vector3& normal, const Vector3& view,
                                           const float minv[4], float (*sphere)(float z, float length) = nullptr);

        // the LTC tables (render/LtcLut.h), bilinear as the GPU filters them, at a perceptual
        // roughness and cos(theta_view): ltc1 the inverse matrix's terms, ltc2 the albedo, Fresnel
        // share, 0 and the sphere table at that (z, length) when sampled through ltcSphere
        JASPERENGINE_API void ltcLookup(float roughness, float cosView, float ltc1[4], float ltc2[4]);
        JASPERENGINE_API float ltcSphere(float z, float length);

        // a point in front of the shape, which is lit by it
        JASPERENGINE_API bool inFront(const Shape& shape, const Vector3& position);
    }

}

#endif //AREALIGHTMATH_H
