// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AreaLightMath.h"

#include "LtcLut.h"
#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {
    constexpr float AREA_PI = 3.14159265358979f;

    float areaSaturate(float x){
        return std::clamp(x, 0.0f, 1.0f);
    }

    Vector3 areaSafeNormalize(const Vector3& v){
        const float length = std::sqrt(v.squaredLength());
        return length > 1e-12f ? v / length : Vector3(0.0f, 0.0f, 1.0f);
    }

    // the vector form factor of the shape seen from position, through a matrix applied to each
    // corner first (rows, in the frame t1, t2, n of the receiver when frame is given)
    Vector3 areaVectorFormFactor(const Vector3 corners[], int count, const Vector3& position, const float m[9]){
        Vector3 unit[AreaLightMath::MAX_CORNERS];
        for (int i = 0; i < count; i++){
            Vector3 c = corners[i] - position;
            if (m){
                c = Vector3(m[0] * c.x + m[1] * c.y + m[2] * c.z,
                            m[3] * c.x + m[4] * c.y + m[5] * c.z,
                            m[6] * c.x + m[7] * c.y + m[8] * c.z);
            }
            unit[i] = areaSafeNormalize(c);
        }
        Vector3 sum(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < count; i++){
            sum += AreaLightMath::edgeFormFactor(unit[i], unit[(i + 1) % count]);
        }
        // (the edge fit has the 1 / 2 pi of Lambert's formula in it)
    return sum;
    }

    float ltcBilinear(const uint16_t* table, float u, float v, int channel){
        const float size = (float)LTC_LUT_SIZE;
        // the texel centres span [0.5, 63.5] / 64, as LUT_SCALE and LUT_BIAS map them on the GPU
        const float x = areaSaturate(u) * (size - 1.0f);
        const float y = areaSaturate(v) * (size - 1.0f);
        const int x0 = std::min((int)x, LTC_LUT_SIZE - 2);
        const int y0 = std::min((int)y, LTC_LUT_SIZE - 2);
        const float fx = x - (float)x0;
        const float fy = y - (float)y0;
        auto at = [&](int xi, int yi){
            return halfToFloat(table[(yi * LTC_LUT_SIZE + xi) * 4 + channel]);
        };
        const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
        const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
        return top * (1.0f - fy) + bottom * fy;
    }
}

float AreaLightMath::area(const Shape& shape){
    return shape.disc ? AREA_PI * shape.halfWidth * shape.halfHeight : 4.0f * shape.halfWidth * shape.halfHeight;
}

int AreaLightMath::corners(const Shape& shape, Vector3 out[MAX_CORNERS], int discSides){
    if (!shape.disc){
        out[0] = shape.center - shape.right * shape.halfWidth - shape.up * shape.halfHeight;
        out[1] = shape.center + shape.right * shape.halfWidth - shape.up * shape.halfHeight;
        out[2] = shape.center + shape.right * shape.halfWidth + shape.up * shape.halfHeight;
        out[3] = shape.center - shape.right * shape.halfWidth + shape.up * shape.halfHeight;
        return 4;
    }
    // a regular polygon of the disc's area: circumradius r * sqrt(2 pi / (n sin(2 pi / n))), the
    // shader's steps of a rotation by 2 pi / n
    const int sides = std::clamp(discSides, 3, MAX_CORNERS);
    const float step = 2.0f * AREA_PI / (float)sides;
    const float scale = std::sqrt(2.0f * AREA_PI / ((float)sides * std::sin(step)));
    for (int i = 0; i < sides; i++){
        const float angle = (float)i * step;
        out[i] = shape.center + shape.right * (std::cos(angle) * shape.halfWidth * scale) +
                 shape.up * (std::sin(angle) * shape.halfHeight * scale);
    }
    return sides;
}

Vector3 AreaLightMath::edgeFormFactor(const Vector3& v1, const Vector3& v2){
    const float x = v1.dotProduct(v2);
    const float y = std::abs(x);
    const float a = 0.8543985f + (0.4965155f + 0.0145206f * y) * y;
    const float b = 3.4175940f + (4.1616724f + y) * y;
    const float v = a / b;
    const float thetaSinTheta = (x > 0.0f) ? v : 0.5f / std::sqrt(std::max(1.0f - x * x, 1e-7f)) - v;
    return v1.crossProduct(v2) * thetaSinTheta;
}

float AreaLightMath::sphereFormFactor(float cosTheta, float sinSigmaSqr){
    sinSigmaSqr = std::clamp(sinSigmaSqr, 1e-6f, 1.0f);
    if (cosTheta * cosTheta > sinSigmaSqr){
        return sinSigmaSqr * std::max(cosTheta, 0.0f);
    }
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 1e-7f));
    const float x = std::sqrt(std::max(1.0f / sinSigmaSqr - 1.0f, 1e-7f));
    const float y = std::clamp(-x * cosTheta / sinTheta, -1.0f, 1.0f);
    const float sinThetaSqrtY = sinTheta * std::sqrt(std::max(1.0f - y * y, 0.0f));
    return std::max(((cosTheta * std::acos(y) - x * sinThetaSqrtY) * sinSigmaSqr + std::atan(sinThetaSqrtY / x)) / AREA_PI, 0.0f);
}

bool AreaLightMath::inFront(const Shape& shape, const Vector3& position){
    return (position - shape.center).dotProduct(shape.normal) > 0.0f;
}

float AreaLightMath::diffuseFormFactor(const Shape& shape, const Vector3& position, const Vector3& normal, Vector3& direction){
    direction = areaSafeNormalize(shape.center - position);
    if (!inFront(shape, position))
        return 0.0f;
    Vector3 polygon[MAX_CORNERS];
    const int count = corners(shape, polygon);
    Vector3 f = areaVectorFormFactor(polygon, count, position, nullptr);
    if (f.dotProduct(shape.center - position) < 0.0f)
        f = f * -1.0f;
    const float length = std::sqrt(f.squaredLength());
    if (length <= 1e-9f)
        return 0.0f;
    direction = f / length;
    return sphereFormFactor(direction.dotProduct(normal), length);
}

float AreaLightMath::specularIrradiance(const Shape& shape, const Vector3& position, const Vector3& normal,
                                        const Vector3& view, float alpha, Vector3& direction, float* widenedAlpha){
    direction = areaSafeNormalize(shape.center - position);
    if (!inFront(shape, position))
        return 0.0f;
    const Vector3 reflected = normal * (2.0f * normal.dotProduct(view)) - view;
    const Vector3 toCenter = shape.center - position;
    const float denom = reflected.dotProduct(shape.normal);
    const float t = toCenter.dotProduct(shape.normal) / std::min(denom, -1e-4f);
    // the reflection ray's hit on the light's plane; a ray leaving it takes the plane's point
    // under the ray at the centre's distance
    Vector3 hit = position + reflected * ((denom < -1e-4f && t > 0.0f) ? t : std::sqrt(toCenter.squaredLength()));
    hit = hit - shape.normal * (hit - shape.center).dotProduct(shape.normal);

    // clamped to the shape in its plane
    const Vector3 local = hit - shape.center;
    float x = local.dotProduct(shape.right);
    float y = local.dotProduct(shape.up);
    if (shape.disc){
        const float ex = x / shape.halfWidth;
        const float ey = y / shape.halfHeight;
        const float r = std::sqrt(ex * ex + ey * ey);
        if (r > 1.0f){
            x /= r;
            y /= r;
        }
    }else{
        x = std::clamp(x, -shape.halfWidth, shape.halfWidth);
        y = std::clamp(y, -shape.halfHeight, shape.halfHeight);
    }
    const Vector3 point = shape.center + shape.right * x + shape.up * y;
    const Vector3 toPoint = point - position;
    const float distanceSqr = std::max(toPoint.squaredLength(), 1e-8f);
    direction = toPoint / std::sqrt(distanceSqr);

    // a point light at the representative point with the disc's irradiance of the same area
    // (exact on its axis, finite at contact), its lobe widened by the light's angular size and
    // normalised (Karis 2013)
    const float lightArea = area(shape);
    const float emitter = std::max(shape.normal.dotProduct(direction * -1.0f), 0.0f);
    const float irradiance = lightArea * emitter / (distanceSqr + lightArea / AREA_PI);
    const float sourceRadius = std::sqrt(lightArea / AREA_PI);
    const float a = std::max(alpha, 1e-4f);
    const float widened = areaSaturate(a + sourceRadius / (2.0f * std::sqrt(distanceSqr)));
    const float normalisation = (a / widened) * (a / widened);
    if (widenedAlpha)
        *widenedAlpha = widened;
    return irradiance * normalisation;
}

float AreaLightMath::ltcIntegral(const Shape& shape, const Vector3& position, const Vector3& normal, const Vector3& view,
                                 const float minv[4], float (*sphere)(float z, float length)){
    if (!inFront(shape, position))
        return 0.0f;
    // the frame around the normal, t1 towards the view
    Vector3 t1 = view - normal * view.dotProduct(normal);
    if (t1.squaredLength() < 1e-10f){
        t1 = std::abs(normal.x) < 0.9f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
        t1 = t1 - normal * t1.dotProduct(normal);
    }
    t1 = areaSafeNormalize(t1);
    const Vector3 t2 = normal.crossProduct(t1);
    // inverse matrix rows (m00 0 m02 / 0 1 0 / m20 0 m22, column-major terms as the table holds
    // them: minv = m00, m02, m20, m22 with M[col][row]) times the frame's rows
    const float a = minv[0], b = minv[1], c = minv[2], d = minv[3];
    const float m[9] = {
        a * t1.x + c * normal.x, a * t1.y + c * normal.y, a * t1.z + c * normal.z,
        t2.x, t2.y, t2.z,
        b * t1.x + d * normal.x, b * t1.y + d * normal.y, b * t1.z + d * normal.z,
    };
    Vector3 polygon[MAX_CORNERS];
    const int count = corners(shape, polygon, DISC_SIDES_SPECULAR);
    Vector3 f = areaVectorFormFactor(polygon, count, position, m);
    // the polygon's winding seen from the receiver decides the sign: face it to the light
    const Vector3 toCenter = shape.center - position;
    const Vector3 transformedCenter(m[0] * toCenter.x + m[1] * toCenter.y + m[2] * toCenter.z,
                                    m[3] * toCenter.x + m[4] * toCenter.y + m[5] * toCenter.z,
                                    m[6] * toCenter.x + m[7] * toCenter.y + m[8] * toCenter.z);
    if (f.dotProduct(transformedCenter) < 0.0f)
        f = f * -1.0f;
    const float length = std::sqrt(f.squaredLength());
    if (length <= 1e-9f)
        return 0.0f;
    const float z = f.z / length;
    return sphere ? length * sphere(z, length) : sphereFormFactor(z, length);
}

void AreaLightMath::ltcLookup(float roughness, float cosView, float ltc1[4], float ltc2[4]){
    const float u = areaSaturate(roughness);
    const float v = std::sqrt(std::max(1.0f - areaSaturate(cosView), 0.0f));
    for (int ch = 0; ch < 4; ch++){
        ltc1[ch] = ltcBilinear(ltcLut1, u, v, ch);
        ltc2[ch] = ltcBilinear(ltcLut2, u, v, ch);
    }
}

float AreaLightMath::ltcSphere(float z, float length){
    return ltcBilinear(ltcLut2, z * 0.5f + 0.5f, length, 3);
}
