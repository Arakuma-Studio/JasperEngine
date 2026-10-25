// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The particle system: its emitter and the initializer and modifier of each particle attribute
// (docs/specs/S08-reflection.md P3; its encoding stays its own, ComponentCodec)

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/ParticleSystemComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr FieldDesc kLifeInitializerFields[] = {
        JE_FIELD(ParticleLifeInitializer, minLife, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(ParticleLifeInitializer, maxLife, .unit = Unit::Seconds, .min = 0.0f),
    };

    constexpr FieldDesc kPositionInitializerFields[] = {
        JE_FIELD(ParticlePositionInitializer, shape, .enumDesc = &enums::kParticleEmitterShape),
        JE_FIELD(ParticlePositionInitializer, minPosition),
        JE_FIELD(ParticlePositionInitializer, maxPosition),
        JE_FIELD(ParticlePositionInitializer, radius, .min = 0.0f),
        JE_FIELD(ParticlePositionInitializer, innerRadius, .min = 0.0f),
        JE_FIELD(ParticlePositionInitializer, coneAngle, .unit = Unit::Degrees),
        JE_FIELD(ParticlePositionInitializer, coneHeight),
    };

    constexpr FieldDesc kPositionModifierFields[] = {
        JE_FIELD(ParticlePositionModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticlePositionModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticlePositionModifier, fromPosition),
        JE_FIELD(ParticlePositionModifier, toPosition),
        JE_FIELD(ParticlePositionModifier, function),
    };

    constexpr FieldDesc kVelocityInitializerFields[] = {
        JE_FIELD(ParticleVelocityInitializer, minVelocity),
        JE_FIELD(ParticleVelocityInitializer, maxVelocity),
    };

    constexpr FieldDesc kVelocityModifierFields[] = {
        JE_FIELD(ParticleVelocityModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleVelocityModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleVelocityModifier, fromVelocity),
        JE_FIELD(ParticleVelocityModifier, toVelocity),
        JE_FIELD(ParticleVelocityModifier, function),
    };

    constexpr FieldDesc kAccelerationInitializerFields[] = {
        JE_FIELD(ParticleAccelerationInitializer, minAcceleration),
        JE_FIELD(ParticleAccelerationInitializer, maxAcceleration),
    };

    constexpr FieldDesc kAccelerationModifierFields[] = {
        JE_FIELD(ParticleAccelerationModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleAccelerationModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleAccelerationModifier, fromAcceleration),
        JE_FIELD(ParticleAccelerationModifier, toAcceleration),
        JE_FIELD(ParticleAccelerationModifier, function),
    };

    constexpr FieldDesc kColorInitializerFields[] = {
        JE_FIELD(ParticleColorInitializer, minColor, .flags = ColorSRGB),
        JE_FIELD(ParticleColorInitializer, maxColor, .flags = ColorSRGB),
        JE_FIELD(ParticleColorInitializer, useSRGB, .label = "Use sRGB"),
    };

    constexpr FieldDesc kColorModifierFields[] = {
        JE_FIELD(ParticleColorModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleColorModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleColorModifier, fromColor, .flags = ColorSRGB),
        JE_FIELD(ParticleColorModifier, toColor, .flags = ColorSRGB),
        JE_FIELD(ParticleColorModifier, function),
        JE_FIELD(ParticleColorModifier, useSRGB, .label = "Use sRGB"),
    };

    constexpr FieldDesc kAlphaInitializerFields[] = {
        JE_FIELD(ParticleAlphaInitializer, minAlpha, .min = 0.0f, .max = 1.0f),
        JE_FIELD(ParticleAlphaInitializer, maxAlpha, .min = 0.0f, .max = 1.0f),
    };

    constexpr FieldDesc kAlphaModifierFields[] = {
        JE_FIELD(ParticleAlphaModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleAlphaModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleAlphaModifier, fromAlpha, .min = 0.0f, .max = 1.0f),
        JE_FIELD(ParticleAlphaModifier, toAlpha, .min = 0.0f, .max = 1.0f),
        JE_FIELD(ParticleAlphaModifier, function),
    };

    constexpr FieldDesc kSizeInitializerFields[] = {
        JE_FIELD(ParticleSizeInitializer, minSize, .min = 0.0f),
        JE_FIELD(ParticleSizeInitializer, maxSize, .min = 0.0f),
    };

    constexpr FieldDesc kSizeModifierFields[] = {
        JE_FIELD(ParticleSizeModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleSizeModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleSizeModifier, fromSize, .min = 0.0f),
        JE_FIELD(ParticleSizeModifier, toSize, .min = 0.0f),
        JE_FIELD(ParticleSizeModifier, function),
    };

    constexpr FieldDesc kSpriteInitializerFields[] = {
        JE_FIELD(ParticleSpriteInitializer, frames, .flags = ListWhole),
    };

    constexpr FieldDesc kSpriteModifierFields[] = {
        JE_FIELD(ParticleSpriteModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleSpriteModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleSpriteModifier, frames, .flags = ListWhole),
        JE_FIELD(ParticleSpriteModifier, function),
    };

    constexpr FieldDesc kRotationInitializerFields[] = {
        JE_FIELD(ParticleRotationInitializer, minRotation),
        JE_FIELD(ParticleRotationInitializer, maxRotation),
        JE_FIELD(ParticleRotationInitializer, shortestPath),
    };

    constexpr FieldDesc kRotationModifierFields[] = {
        JE_FIELD(ParticleRotationModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleRotationModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleRotationModifier, fromRotation),
        JE_FIELD(ParticleRotationModifier, toRotation),
        JE_FIELD(ParticleRotationModifier, function),
        JE_FIELD(ParticleRotationModifier, shortestPath),
    };

    constexpr FieldDesc kScaleInitializerFields[] = {
        JE_FIELD(ParticleScaleInitializer, minScale),
        JE_FIELD(ParticleScaleInitializer, maxScale),
        JE_FIELD(ParticleScaleInitializer, linearSort),
    };

    constexpr FieldDesc kScaleModifierFields[] = {
        JE_FIELD(ParticleScaleModifier, fromTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleScaleModifier, toTime, .unit = Unit::Seconds),
        JE_FIELD(ParticleScaleModifier, fromScale),
        JE_FIELD(ParticleScaleModifier, toScale),
        JE_FIELD(ParticleScaleModifier, function),
    };

}

#define JE_PARTICLE_STRUCT(Type, desc, fields) \
    JE_REFLECT_STRUCT_TRAITS(Type, desc); \
    const StructDesc desc = JE_STRUCT_DESC(Type, #Type, fields)

JE_PARTICLE_STRUCT(ParticleLifeInitializer, kLifeInitializerStruct, kLifeInitializerFields);
JE_PARTICLE_STRUCT(ParticlePositionInitializer, kPositionInitializerStruct, kPositionInitializerFields);
JE_PARTICLE_STRUCT(ParticlePositionModifier, kPositionModifierStruct, kPositionModifierFields);
JE_PARTICLE_STRUCT(ParticleVelocityInitializer, kVelocityInitializerStruct, kVelocityInitializerFields);
JE_PARTICLE_STRUCT(ParticleVelocityModifier, kVelocityModifierStruct, kVelocityModifierFields);
JE_PARTICLE_STRUCT(ParticleAccelerationInitializer, kAccelerationInitializerStruct, kAccelerationInitializerFields);
JE_PARTICLE_STRUCT(ParticleAccelerationModifier, kAccelerationModifierStruct, kAccelerationModifierFields);
JE_PARTICLE_STRUCT(ParticleColorInitializer, kColorInitializerStruct, kColorInitializerFields);
JE_PARTICLE_STRUCT(ParticleColorModifier, kColorModifierStruct, kColorModifierFields);
JE_PARTICLE_STRUCT(ParticleAlphaInitializer, kAlphaInitializerStruct, kAlphaInitializerFields);
JE_PARTICLE_STRUCT(ParticleAlphaModifier, kAlphaModifierStruct, kAlphaModifierFields);
JE_PARTICLE_STRUCT(ParticleSizeInitializer, kSizeInitializerStruct, kSizeInitializerFields);
JE_PARTICLE_STRUCT(ParticleSizeModifier, kSizeModifierStruct, kSizeModifierFields);
JE_PARTICLE_STRUCT(ParticleSpriteInitializer, kSpriteInitializerStruct, kSpriteInitializerFields);
JE_PARTICLE_STRUCT(ParticleSpriteModifier, kSpriteModifierStruct, kSpriteModifierFields);
JE_PARTICLE_STRUCT(ParticleRotationInitializer, kRotationInitializerStruct, kRotationInitializerFields);
JE_PARTICLE_STRUCT(ParticleRotationModifier, kRotationModifierStruct, kRotationModifierFields);
JE_PARTICLE_STRUCT(ParticleScaleInitializer, kScaleInitializerStruct, kScaleInitializerFields);
JE_PARTICLE_STRUCT(ParticleScaleModifier, kScaleModifierStruct, kScaleModifierFields);

#undef JE_PARTICLE_STRUCT

namespace {

    constexpr FieldDesc kParticleSystemFields[] = {
        JE_FIELD(ParticleSystemComponent, enabled),
        JE_FIELD(ParticleSystemComponent, maxParticles, .min = 1.0f),
        JE_FIELD(ParticleSystemComponent, emitter),
        JE_FIELD(ParticleSystemComponent, loop),
        JE_FIELD(ParticleSystemComponent, localSpace),
        JE_FIELD(ParticleSystemComponent, rate, .min = 0.0f, .tooltip = "Particles emitted per second."),
        JE_FIELD(ParticleSystemComponent, maxPerUpdate, .min = 0.0f),
        JE_FIELD(ParticleSystemComponent, bursts, .ops = &ops::Opaque<std::vector<ParticleBurst>>),
        JE_FIELD(ParticleSystemComponent, lifeInitializer),
        JE_FIELD(ParticleSystemComponent, positionInitializer),
        JE_FIELD(ParticleSystemComponent, positionModifier),
        JE_FIELD(ParticleSystemComponent, velocityInitializer),
        JE_FIELD(ParticleSystemComponent, velocityModifier),
        JE_FIELD(ParticleSystemComponent, accelerationInitializer),
        JE_FIELD(ParticleSystemComponent, accelerationModifier),
        JE_FIELD(ParticleSystemComponent, colorInitializer),
        JE_FIELD(ParticleSystemComponent, colorModifier),
        // overrides the colour modifier when it has stops
        JE_FIELD(ParticleSystemComponent, colorGradient, .ops = &ops::Opaque<ParticleColorGradient>),
        JE_FIELD(ParticleSystemComponent, alphaInitializer),
        JE_FIELD(ParticleSystemComponent, alphaModifier),
        JE_FIELD(ParticleSystemComponent, sizeInitializer),
        JE_FIELD(ParticleSystemComponent, sizeModifier),
        JE_FIELD(ParticleSystemComponent, spriteInitializer),
        JE_FIELD(ParticleSystemComponent, spriteModifier),
        JE_FIELD(ParticleSystemComponent, rotationInitializer),
        JE_FIELD(ParticleSystemComponent, rotationModifier),
        JE_FIELD(ParticleSystemComponent, scaleInitializer),
        JE_FIELD(ParticleSystemComponent, scaleModifier),
    };

    const ComponentCodec kParticleSystemCodec = JE_STREAM_CODEC(ParticleSystemComponent, encodeParticleSystemComponent, decodeParticleSystemComponent);

    // the keys its codec reads
    constexpr const char* kParticleSystemKnownKeys[] = {"accelerationInitializer", "accelerationModifier", "alphaInitializer", "alphaModifier", "bursts", "colorGradient", "colorInitializer", "colorModifier", "emitter", "enabled", "lifeInitializer", "localSpace", "loop", "maxParticles", "maxPerUpdate", "positionInitializer", "positionModifier", "rate", "rotationInitializer", "rotationModifier", "scaleInitializer", "scaleModifier", "sizeInitializer", "sizeModifier", "spriteInitializer", "spriteModifier", "velocityInitializer", "velocityModifier"};

}

const ComponentDesc kParticleSystemDesc{
    .type = ComponentType::ParticleSystemComponent, .cppName = "ParticleSystemComponent", .key = "particlesystem",
    .displayName = "Particle System", .icon = ICON_FA_FIRE,
    .body = JE_STRUCT_DESC(ParticleSystemComponent, "ParticleSystemComponent", kParticleSystemFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(ParticleSystemComponent),
    .codec = &kParticleSystemCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createParticleSystemComponent),
    .extraKeys = kParticleSystemKnownKeys,
};

}
