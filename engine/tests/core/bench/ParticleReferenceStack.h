// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The reference stack of the particle budgets (docs/specs/S20-particles.md R21): Rate + Cone,
// Start Lifetime/Speed/Size/Color ranges, gravity, Limit Velocity (drag), Noise High (1 octave),
// Color + Size over Lifetime, Texture Sheet 8x8 with frame blending, Billboard, additive.
// je-bench-particles and the web bench of tools/test-projects/particles both measure it.

#pragma once

#include "particles/ParticleEffect.h"

namespace jasperengine { namespace particles { namespace referencestack {

    // `rate` per second, lifetimes 1.5-2.5 s: rate x 2 live particles at the steady state
    inline ParticleEffectDocument referenceStack(bool noise, float rate){
        ParticleEffectDocument d;
        d.maxParticles = 1u << 17;
        d.duration = 5.0f;
        d.looping = true;
        d.simulationSpace = SimulationSpace::World;
        d.seed = 12345;
        d.main.startLifetime = MinMaxCurve::twoConstants(1.5f, 2.5f);
        d.main.startSpeed = MinMaxCurve::twoConstants(2.0f, 4.0f);
        d.main.startSize = MinMaxCurve::twoConstants(0.05f, 0.15f);
        d.main.startColor.mode = GradientValueMode::TwoColors;
        d.main.startColor.color = Vector4(1.0f, 0.6f, 0.2f, 1.0f);
        d.main.startColor.colorMax = Vector4(1.0f, 0.9f, 0.5f, 1.0f);
        d.main.gravityModifier = MinMaxCurve(0.3f);
        d.emission.rateOverTime = MinMaxCurve(rate);
        d.shape.shape = ShapeType::Cone;
        d.shape.emitFrom = ShapeEmitFrom::Base;
        d.shape.radius = 0.5f;
        d.shape.angle = 20.0f;
        d.limitVelocity.present = true;
        d.limitVelocity.drag = MinMaxCurve(0.5f);
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.limit = MinMaxCurve(100.0f);
        if (noise){
            d.noise.present = true;
            d.noise.strength = MinMaxCurve(1.0f);
            d.noise.frequency = 0.5f;
            d.noise.octaves = 1;
            d.noise.quality = NoiseQuality::High;
        }
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.color.mode = GradientValueMode::Gradient;
        d.colorOverLifetime.color.gradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 0.3f, 0.1f), 1.0f, 0.0f);
        d.sizeOverLifetime.present = true;
        d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::polyline({{0.0f, 0.2f}, {0.3f, 1.0f}, {1.0f, 0.5f}}));
        d.textureSheetAnimation.present = true;
        d.textureSheetAnimation.tilesX = 8;
        d.textureSheetAnimation.tilesY = 8;
        d.textureSheetAnimation.frameBlending = true;
        d.renderer.renderMode = RenderMode::Billboard;
        d.renderer.blend = ParticleBlend::Additive;
        return d;
    }

}}}
