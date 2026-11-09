// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The particle system and the force field (docs/specs/S08-reflection.md P3, S20): both keep the
// codecs of S20 (the effect as an asset reference or the inline .particles JSON, the falloff
// curve as key rows), their tables giving the properties, schema, Inspector metadata and
// asset locators. The effect's modules are edited as one document (inlineEffect).

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/ForceFieldComponent.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Particle System

    // a new effect, inline document, exposed value or seed rebinds the system (keeping the live
    // particles when only parameters changed)
    constexpr uint64_t kParticles = UpdateFlags_Particles;

    // the inline effect: a document the tables do not look into (its codec writes it), equal when
    // its canonical .particles JSON is
    using EffectDocument = particles::ParticleEffectDocument;
    bool equalEffects(const void* a, const void* b, float) {
        return particles::ParticleEffectCodec::write(*static_cast<const EffectDocument*>(a)) ==
               particles::ParticleEffectCodec::write(*static_cast<const EffectDocument*>(b));
    }
    constexpr TypeOps kEffectDocumentOps{"value", Kind::Custom, PropertyType::Custom, sizeof(EffectDocument),
        &detail::construct<EffectDocument>, &detail::destroy<EffectDocument>, &detail::copy<EffectDocument>, &equalEffects,
        nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};

    constexpr FieldDesc kParticleSystemFields[] = {
        JE_FIELD(ParticleSystemComponent, enabled),
        JE_FIELD(ParticleSystemComponent, effect, .ops = &ops::AssetRef, .updateFlags = kParticles, .asset = AssetKind::ParticleEffect,
                 .tooltip = "A .particles asset; empty: the inline effect."),
        JE_FIELD(ParticleSystemComponent, inlineEffect, .ops = &kEffectDocumentOps, .flags = NoDefault, .updateFlags = kParticles, .assetForm = AssetForm::InlineEffect),
        JE_FIELD(ParticleSystemComponent, playOnAwake),
        JE_FIELD(ParticleSystemComponent, seed, .updateFlags = kParticles, .tooltip = "0: a new random seed at every play."),
        JE_FIELD(ParticleSystemComponent, playbackSpeed),
        // S52: the 2D draw order and the transparent queue's first keys (a layer id: SortingLayers)
        JE_FIELD(ParticleSystemComponent, sortingLayer, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .label = "Sorting Layer", .drawer = "sortingLayer"),
        JE_FIELD(ParticleSystemComponent, sortingOrder, .flags = OmitIfDefault | CodegenIfChanged, .min = -32768.0f, .max = 32767.0f, .label = "Order in Layer"),
        // written only when on: a file from before it reads as it was, byte for byte
        JE_FIELD(ParticleSystemComponent, useUnscaledTime, .flags = OmitIfDefault | CodegenIfChanged, .label = "Unscaled Time",
                 .tooltip = "Ticks with the unscaled frame time: it plays at Engine.timeScale 0 (a pause menu, UI particles), as the effect's own Unscaled Time does."),
        JE_FIELD(ParticleSystemComponent, exposedValues, .ops = &ops::Opaque<std::vector<ParticleExposedValue>>, .updateFlags = kParticles),
        // GameObjects whose transforms are more collision planes (their up axis is the normal)
        JE_FIELD(ParticleSystemComponent, collisionPlanes, .flags = NoDefault | ListWhole, .elementOps = &ops::Entity),
    };

    const ComponentCodec kParticleSystemCodec = JE_STREAM_CODEC(ParticleSystemComponent, encodeParticleSystemComponent, decodeParticleSystemComponent);

    // the keys its codec reads
    constexpr const char* kParticleSystemKnownKeys[] = {"collisionPlanes", "effect", "effectGuid", "enabled", "exposedValues", "inlineEffect",
                                                        "playOnAwake", "playbackSpeed", "seed", "sortingLayer", "sortingOrder", "useUnscaledTime"};

    // ---------------------------------------------------------------- Force Field

    constexpr FieldDesc kForceFieldFields[] = {
        JE_FIELD(ForceFieldComponent, enabled),
        JE_FIELD(ForceFieldComponent, shape, .enumDesc = &enums::kForceFieldShape),
        // Sphere: x the radius; Box: the half extents; Cylinder: x the radius, y the half height
        JE_FIELD(ForceFieldComponent, size),
        JE_FIELD(ForceFieldComponent, kind, .enumDesc = &enums::kForceFieldKind),
        JE_FIELD(ForceFieldComponent, strength),
        JE_FIELD(ForceFieldComponent, direction),
        JE_FIELD(ForceFieldComponent, turbulence),
        JE_FIELD(ForceFieldComponent, pulseFrequency, .unit = Unit::Hertz),
        JE_FIELD(ForceFieldComponent, frequency),
        // strength over the distance from the centre (0) to the edge (1)
        JE_FIELD(ForceFieldComponent, falloff, .flags = NoDefault),
        JE_FIELD(ForceFieldComponent, layers, .ops = &ops::LayerMask),
    };

    const ComponentCodec kForceFieldCodec = JE_STREAM_CODEC(ForceFieldComponent, encodeForceFieldComponent, decodeForceFieldComponent);

    constexpr const char* kForceFieldKnownKeys[] = {"direction", "enabled", "falloff", "frequency", "kind", "layers", "pulseFrequency", "shape", "size", "strength", "turbulence"};

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

const ComponentDesc kForceFieldDesc{
    .type = ComponentType::ForceFieldComponent, .cppName = "ForceFieldComponent", .key = "forcefield",
    .displayName = "Force Field", .icon = ICON_FA_WIND,
    .body = JE_STRUCT_DESC(ForceFieldComponent, "ForceFieldComponent", kForceFieldFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(ForceFieldComponent),
    .codec = &kForceFieldCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = [](EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createForceFieldComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting);
    },
    .extraKeys = kForceFieldKnownKeys,
};

}
