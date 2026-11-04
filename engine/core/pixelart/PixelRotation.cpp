// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelRotation.h"

#include <cmath>

namespace jasperengine::pixelart {

    Vector2 snapRotationStep(const Vector2& axisX){
        const float ax = std::fabs(axisX.x);
        const float ay = std::fabs(axisX.y);
        // as the GLSL twin: x wins its ties only towards 0 degrees (45 and -45 go to 0, 135 and
        // -135 to 90 and -90)
        const bool useX = ax > ay || (ax == ay && axisX.x > 0.0f);
        if (useX)
            return Vector2(axisX.x > 0.0f ? 1.0f : (axisX.x < 0.0f ? -1.0f : 0.0f), 0.0f);
        return Vector2(0.0f, axisX.y > 0.0f ? 1.0f : (axisX.y < 0.0f ? -1.0f : 0.0f));
    }

    bool hasOffGridRotation(const Matrix4& model){
        const float a00 = std::fabs(model[0][0]);
        const float a01 = std::fabs(model[1][0]);
        const float a10 = std::fabs(model[0][1]);
        const float a11 = std::fabs(model[1][1]);
        const float diagonal = a00 + a11;
        const float cross = a01 + a10;
        constexpr float tolerance = 1e-4f;
        return !(cross <= tolerance * diagonal || diagonal <= tolerance * cross);
    }

    Matrix4 roundRotation90(const Matrix4& model){
        // A = [x axis | y axis] of the 2D part: A = R(theta) * S (S symmetric, with the flip F of a
        // negative determinant applied first: A F = R(theta) S)
        const float a = model[0][0];
        const float c = model[0][1];
        float b = model[1][0];
        float d = model[1][1];
        const bool flipped = (a * d - b * c) < 0.0f;
        if (flipped){
            b = -b;
            d = -d;
        }
        // the rotation of the polar decomposition: theta = atan2(c - b, a + d)
        float cosT = a + d;
        float sinT = c - b;
        const float length = std::sqrt(cosT * cosT + sinT * sinT);
        if (length <= 0.0f)
            return model;
        cosT /= length;
        sinT /= length;

        // S = R(-theta) (A F)
        const float s00 = cosT * a + sinT * c;
        const float s01 = cosT * b + sinT * d;
        const float s10 = -sinT * a + cosT * c;
        const float s11 = -sinT * b + cosT * d;

        // the nearest step, exactly 0 or +-1
        const Vector2 step = snapRotationStep(Vector2(cosT, sinT));
        const float cosR = step.x;
        const float sinR = step.y;

        // A' = R(step) S F
        float n00 = cosR * s00 - sinR * s10;
        float n01 = cosR * s01 - sinR * s11;
        float n10 = sinR * s00 + cosR * s10;
        float n11 = sinR * s01 + cosR * s11;
        if (flipped){
            n01 = -n01;
            n11 = -n11;
        }

        Matrix4 result = model;
        result.set(0, 0, n00);
        result.set(0, 1, n10);
        result.set(1, 0, n01);
        result.set(1, 1, n11);
        return result;
    }

}
