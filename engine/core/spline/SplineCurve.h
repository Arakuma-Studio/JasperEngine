// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_SPLINECURVE_H
#define SPLINE_SPLINECURVE_H

// The vector and cubic Bezier arithmetic of the spline sources, inline so it compiles into their
// translation units with their floating-point flags (no contraction, S18 R16). Every segment of
// every spline type is evaluated as one cubic Bezier (SplineData's cache holds the two inner
// control points), so this is the whole curve basis.

#include "math/Vector3.h"
#include <cmath>

namespace jasperengine{ namespace spline{ namespace curve{

    inline float dot(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vector3 cross(const Vector3& a, const Vector3& b){
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    inline float lengthSq(const Vector3& v){
        return v.x * v.x + v.y * v.y + v.z * v.z;
    }

    inline bool isFinite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // v / |v|, or fallback where v has no usable direction (zero, tiny, NaN or overflowing:
    // the negated test also catches NaN, the finiteness test an infinite squared length)
    inline Vector3 normalizedOr(const Vector3& v, const Vector3& fallback, float minLengthSq = 1e-20f){
        const float sq = lengthSq(v);
        if (!(sq >= minLengthSq) || !std::isfinite(sq)){
            return fallback;
        }
        const float r = 1.0f / std::sqrt(sq);
        return Vector3(v.x * r, v.y * r, v.z * r);
    }

    inline Vector3 lerp(const Vector3& a, const Vector3& b, float u){
        const float s = 1.0f - u;
        return Vector3(a.x * s + b.x * u, a.y * s + b.y * u, a.z * s + b.z * u);
    }

    inline float lerp(float a, float b, float u){
        return a * (1.0f - u) + b * u;
    }

    // B(u) in the Bernstein form: exactly p0 at u = 0 and exactly p3 at u = 1
    inline Vector3 bezier(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3, float u){
        const float s = 1.0f - u;
        const float b0 = s * s * s;
        const float b1 = 3.0f * s * s * u;
        const float b2 = 3.0f * s * u * u;
        const float b3 = u * u * u;
        return Vector3(p0.x * b0 + p1.x * b1 + p2.x * b2 + p3.x * b3,
                       p0.y * b0 + p1.y * b1 + p2.y * b2 + p3.y * b3,
                       p0.z * b0 + p1.z * b1 + p2.z * b2 + p3.z * b3);
    }

    // B'(u) = 3 [(1-u)^2 (p1-p0) + 2u(1-u) (p2-p1) + u^2 (p3-p2)]
    inline Vector3 bezierDerivative(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3, float u){
        const float s = 1.0f - u;
        const float a = 3.0f * s * s;
        const float b = 6.0f * s * u;
        const float c = 3.0f * u * u;
        return Vector3((p1.x - p0.x) * a + (p2.x - p1.x) * b + (p3.x - p2.x) * c,
                       (p1.y - p0.y) * a + (p2.y - p1.y) * b + (p3.y - p2.y) * c,
                       (p1.z - p0.z) * a + (p2.z - p1.z) * b + (p3.z - p2.z) * c);
    }

    // B''(u) = 6 [(1-u) (p2 - 2p1 + p0) + u (p3 - 2p2 + p1)]
    inline Vector3 bezierSecondDerivative(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3, float u){
        const float s = 6.0f * (1.0f - u);
        const float t = 6.0f * u;
        return Vector3((p2.x - 2.0f * p1.x + p0.x) * s + (p3.x - 2.0f * p2.x + p1.x) * t,
                       (p2.y - 2.0f * p1.y + p0.y) * s + (p3.y - 2.0f * p2.y + p1.y) * t,
                       (p2.z - 2.0f * p1.z + p0.z) * s + (p3.z - 2.0f * p2.z + p1.z) * t);
    }

    // The four control points of one segment.
    struct Segment{
        Vector3 p0, p1, p2, p3;

        Vector3 position(float u) const { return bezier(p0, p1, p2, p3, u); }
        Vector3 derivative(float u) const { return bezierDerivative(p0, p1, p2, p3, u); }
        Vector3 secondDerivative(float u) const { return bezierSecondDerivative(p0, p1, p2, p3, u); }
        float speed(float u) const { return std::sqrt(lengthSq(derivative(u))); }
    };

    // Arc length of s over [u0, u1]: 3-point Gauss-Legendre (exact for a polynomial speed of
    // degree 5; the LUT intervals are short enough that the square root is smooth over them)
    inline float arcLength(const Segment& s, float u0, float u1){
        const float half = 0.5f * (u1 - u0);
        const float mid = 0.5f * (u0 + u1);
        const float x = 0.7745966692414834f * half;
        return half * (0.5555555555555556f * (s.speed(mid - x) + s.speed(mid + x)) + 0.8888888888888888f * s.speed(mid));
    }

}}}

#endif // SPLINE_SPLINECURVE_H
