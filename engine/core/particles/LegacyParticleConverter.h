// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_LEGACYPARTICLECONVERTER_H
#define PARTICLES_LEGACYPARTICLECONVERTER_H

// The old particle system to an effect (docs/specs/S20-particles.md D9, R32): every old field
// maps to a module (initializers to start values and Start Velocity / Force over Lifetime,
// shapes to Shape, each enabled modifier to a Set over Age entry with its ease sampled to a
// 16-key curve, the colour gradient to Color over Lifetime replacing the colour); the old
// target (Points or InstancedMesh) maps to the Renderer. The deprecated setters run it back
// and forth: extract() reads the old view of an effect, apply() writes it back.

#include "Export.h"
#include "particles/LegacyParticleSystem.h"
#include "particles/ParticleEffect.h"

#include <string>
#include <vector>

namespace jasperengine { namespace particles {

    // what the old target held that the Renderer takes over
    struct LegacyTarget {
        enum class Kind : uint8_t { Points, Mesh };
        Kind kind = Kind::Points;
        std::string texture;
        std::string textureGuid;
        std::vector<SpriteRect> frames;      // the target's sprite rects (UV)
        std::string shader;                  // the Points' custom shader ("library:particle", a fork, or empty)
        std::vector<std::string> shaderFeatures;
        std::vector<ShaderProperty> shaderProperties;
        std::vector<ShaderTexture> shaderTextures;
        // the Points' blend: -1 Auto, else ParticleBlend; opaque has no particle blend
        int blend = -1;
        bool blendOpaque = false;
        int depthWrite = -1;                 // -1 Auto, 0 off, 1 on
        bool transparent = true;             // the old points were sorted when transparent
    };

    class JASPERENGINE_API LegacyParticleConverter {
    public:
        // the effect of an old system and its target; `notes` gets one line per approximation
        static ParticleEffectDocument convert(const LegacyParticleSystem& legacy, const LegacyTarget& target, std::vector<std::string>* notes = nullptr);
        // writes the fields the old system has into `document`, leaving the others
        static void apply(const LegacyParticleSystem& legacy, ParticleEffectDocument& document, std::vector<std::string>* notes = nullptr);
        // the old view of an effect (what apply() wrote, the old defaults elsewhere)
        static LegacyParticleSystem extract(const ParticleEffectDocument& document);

        // the old emission cycle: maxParticles / rate, and the latest burst
        static float cycleDuration(const LegacyParticleSystem& legacy);
        // how long the old step emitted (the converted duration): a looping system its cycle; one
        // that does not loop until each of its maxParticles slots was used once (bursts take
        // slots, their mean count), or its latest burst
        static float emissionDuration(const LegacyParticleSystem& legacy);
        // an ease over [0, 1] as a curve of 16 keys (slopes from the function, steps kept)
        static Curve sampleEase(const Ease& ease);
    };

}}

#endif // PARTICLES_LEGACYPARTICLECONVERTER_H
