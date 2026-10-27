// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_LEGACYPARTICLESYSTEM_H
#define PARTICLES_LEGACYPARTICLESYSTEM_H

// The particle system before S20 (docs/specs/S20-particles.md D9): one initializer and one
// from-to modifier per property, bursts, a rate, a colour gradient. Old scenes are read into it
// (the editor's migration) and the deprecated ParticleSystem setters edit it; the converter
// (LegacyParticleConverter.h) turns it into an effect. The types keep their old names and
// fields, so old code and data that name them still read.

#include "action/Ease.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <algorithm>
#include <vector>

namespace jasperengine{

    enum class ParticleEmitterShape{
        Box = 0,
        Sphere = 1,
        Hemisphere = 2,
        Circle = 3,
        Cone = 4
    };

    struct ParticleLifeInitializer{
        float minLife = 10;
        float maxLife = 10;
    };

    struct ParticlePositionInitializer{
        ParticleEmitterShape shape = ParticleEmitterShape::Box;

        // Box shape (and historical default)
        Vector3 minPosition = Vector3(0,0,0);
        Vector3 maxPosition = Vector3(0,0,0);

        // Sphere / Hemisphere / Circle
        float radius = 1.0f;
        float innerRadius = 0.0f;

        // Cone (apex at origin, axis +Y; half-angle in degrees)
        float coneAngle = 30.0f;
        float coneHeight = 1.0f;
    };

    struct ParticleBurst{
        float time = 0.0f;
        int minCount = 10;
        int maxCount = 10;

        bool operator==(const ParticleBurst& other) const{
            return time == other.time && minCount == other.minCount && maxCount == other.maxCount;
        }
    };

    struct ParticlePositionModifier{
        float fromTime = 0;
        float toTime = 0;

        Vector3 fromPosition = Vector3(0,0,0);
        Vector3 toPosition = Vector3(0,0,0);

        Ease function;
    };

    struct ParticleVelocityInitializer{
        Vector3 minVelocity = Vector3(0,0,0);
        Vector3 maxVelocity = Vector3(0,0,0);
    };

    struct ParticleVelocityModifier{
        float fromTime = 0;
        float toTime = 0;

        Vector3 fromVelocity = Vector3(0,0,0);
        Vector3 toVelocity = Vector3(0,0,0);

        Ease function;
    };

    struct ParticleAccelerationInitializer{
        Vector3 minAcceleration = Vector3(0,0,0);
        Vector3 maxAcceleration = Vector3(0,0,0);
    };

    struct ParticleAccelerationModifier{
        float fromTime = 0;
        float toTime = 0;

        Vector3 fromAcceleration = Vector3(0,0,0);
        Vector3 toAcceleration = Vector3(0,0,0);

        Ease function;
    };

    struct ParticleColorInitializer{
        Vector3 minColor = Vector3(1,1,1);
        Vector3 maxColor = Vector3(1,1,1);

        bool useSRGB = true;
    };

    struct ParticleColorModifier{
        float fromTime = 0;
        float toTime = 0;

        Vector3 fromColor = Vector3(0,0,0);
        Vector3 toColor = Vector3(0,0,0);

        Ease function;

        bool useSRGB = true;
    };

    struct ParticleColorGradientStop{
        float time = 0.0f; // normalized [0,1] over particle lifetime
        Vector3 color = Vector3(1,1,1);

        bool operator==(const ParticleColorGradientStop& other) const{
            return time == other.time && color == other.color;
        }
    };

    struct ParticleColorGradient{
        std::vector<ParticleColorGradientStop> stops;
        bool useSRGB = true;

        void normalize(){
            for (ParticleColorGradientStop& stop : stops){
                if (stop.time < 0.0f) stop.time = 0.0f;
                if (stop.time > 1.0f) stop.time = 1.0f;
            }
            std::stable_sort(stops.begin(), stops.end(), [](const ParticleColorGradientStop& first, const ParticleColorGradientStop& second){
                return first.time < second.time;
            });
        }

        bool operator==(const ParticleColorGradient& other) const{
            return useSRGB == other.useSRGB && stops == other.stops;
        }
    };

    struct ParticleAlphaInitializer{
        float minAlpha = 1;
        float maxAlpha = 1;
    };

    struct ParticleAlphaModifier{
        float fromTime = 0;
        float toTime = 0;

        float fromAlpha = 0;
        float toAlpha= 0;

        Ease function;
    };

    struct ParticleSizeInitializer{
        float minSize = 1;
        float maxSize = 1;
    };

    struct ParticleSizeModifier{
        float fromTime = 0;
        float toTime = 0;

        float fromSize = 0;
        float toSize = 0;

        Ease function;
    };

    struct ParticleSpriteInitializer{
        std::vector<int> frames;
    };

    struct ParticleSpriteModifier{
        float fromTime = 0;
        float toTime = 0;

        std::vector<int> frames;

        Ease function;
    };

    struct ParticleRotationInitializer{
        Quaternion minRotation;
        Quaternion maxRotation;

        bool shortestPath = false;
    };

    struct ParticleRotationModifier{
        float fromTime = 0;
        float toTime = 0;

        Quaternion fromRotation;
        Quaternion toRotation;

        Ease function;

        bool shortestPath = false;
    };

    struct ParticleScaleInitializer{
        Vector3 minScale = Vector3(1,1,1);
        Vector3 maxScale = Vector3(1,1,1);

        bool linearSort = true; // only for initializer
    };

    struct ParticleScaleModifier{
        float fromTime = 0;
        float toTime = 0;

        Vector3 fromScale = Vector3(1,1,1);
        Vector3 toScale = Vector3(1,1,1);

        Ease function;
    };

    // what the old ParticleSystemComponent held as data
    struct LegacyParticleSystem{
        unsigned int maxParticles = 100;
        bool loop = true;
        bool localSpace = false;
        int rate = 5; //per second
        int maxPerUpdate = 100;
        std::vector<ParticleBurst> bursts;

        ParticleLifeInitializer lifeInitializer;

        ParticlePositionInitializer positionInitializer;
        ParticlePositionModifier positionModifier;

        ParticleVelocityInitializer velocityInitializer;
        ParticleVelocityModifier velocityModifier;

        ParticleAccelerationInitializer accelerationInitializer;
        ParticleAccelerationModifier accelerationModifier;

        ParticleColorInitializer colorInitializer;
        ParticleColorModifier colorModifier;
        ParticleColorGradient colorGradient;

        ParticleAlphaInitializer alphaInitializer;
        ParticleAlphaModifier alphaModifier;

        ParticleSizeInitializer sizeInitializer;
        ParticleSizeModifier sizeModifier;

        ParticleSpriteInitializer spriteInitializer;
        ParticleSpriteModifier spriteModifier;

        ParticleRotationInitializer rotationInitializer;
        ParticleRotationModifier rotationModifier;

        ParticleScaleInitializer scaleInitializer;
        ParticleScaleModifier scaleModifier;
    };

}

#endif // PARTICLES_LEGACYPARTICLESYSTEM_H
