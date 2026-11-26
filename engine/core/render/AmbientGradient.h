// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AMBIENTGRADIENT_H
#define AMBIENTGRADIENT_H

#include "math/Vector3.h"
#include "math/Vector4.h"

namespace jasperengine{

    // Environment Lighting > Source: Gradient (SceneSettings::AmbientMode) as the L1 SH ambient
    // every lit shader already reads (includes/lights.glsl ambientLight: rgb_c(n) = dot(sh_c.xyz,
    // n) + sh_c.w), so the gradient needs no shader, variant or uniform of its own.
    //
    // The gradient is an environment of radiance R(d) over the sphere, linear in the direction's
    // height y: equator + (sky - equator) y above the horizon, equator + (ground - equator) (-y)
    // below it. Projected to SH bands 0 and 1 and convolved with the clamped cosine (Ramamoorthi
    // and Hanrahan: A0 = pi, A1 = 2 pi / 3), divided by pi as every light lights.glsl hands a
    // shader is (light to multiply by albedo):
    //   E(n) / pi = (equator / 2 + (sky + ground) / 4) + n.y (sky - ground) / 3
    // A uniform colour c gives c everywhere, as the flat Ambient Color does.
    namespace AmbientGradient{

        // the SH of a gradient of linear colours, one Vector4 per channel (xyz the direction
        // term, w the constant term)
        inline void project(const Vector3& sky, const Vector3& equator, const Vector3& ground, Vector4 sh[3]){
            for (int c = 0; c < 3; c++){
                const float constant = 0.5f * equator[c] + 0.25f * (sky[c] + ground[c]);
                const float up = (sky[c] - ground[c]) / 3.0f;
                sh[c] = Vector4(0.0f, up, 0.0f, constant);
            }
        }

        // the gradient's light averaged over every facing: what flat consumers (particles, the
        // clouds' ambient) take for it
        inline Vector3 average(const Vector3& sky, const Vector3& equator, const Vector3& ground){
            return equator * 0.5f + (sky + ground) * 0.25f;
        }

    }

}

#endif //AMBIENTGRADIENT_H
