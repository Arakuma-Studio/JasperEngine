// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fluid simulation and the lattice deformer components (docs/specs/S08-reflection.md): the
// fluid and the lattice follower on the generic codec; the lattice, its modifier and its tracks
// keep their codecs (offsets sized by the resolution, flow-style empty lists, clamps on read),
// their tables giving the properties, schema, Inspector metadata and asset locators.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/FluidSimulationComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeTracksComponent.h"

namespace jasperengine::editor::reflect {

    JE_REFLECT_STRUCT_TRAITS(LatticeBinding, kLatticeBindingStruct);

namespace {

    // ---------------------------------------------------------------- Fluid Simulation

    // only what differs from the defaults is written; a missing (or empty) key is the default
    constexpr uint32_t kFluid = OmitIfDefault | EmptyIsDefault;
    constexpr uint32_t kFluidFloat = OmitIfDefault | OmitIfNonFinite | EmptyIsDefault;

    // what the start depends on (the seed, the start burst) restarts the fluid
    constexpr uint64_t kReset = UpdateFlags_Fluid_Reset;

    constexpr FieldDesc kFluidFields[] = {
        JE_FIELD(FluidSimulationComponent, enabled),
        JE_FIELD(FluidSimulationComponent, output, .enumDesc = &enums::kFluidOutput, .flags = kFluid,
                 .tooltip = "Screen: drawn over the scene. Material: the base colour of this GameObject's Mesh or Sprite. Texture: a "
                            "texture other materials show (fluid|<GameObject>)."),
        JE_FIELD(FluidSimulationComponent, interactive, .flags = kFluid, .tooltip = "Pointer drags splat dye."),
        JE_FIELD(FluidSimulationComponent, aspectRatio, .ops = &ops::FiniteFloat, .flags = kFluidFloat, .min = 0.0f,
                 .tooltip = "Width over height of the simulation. 0: its output's."),
        JE_FIELD(FluidSimulationComponent, simResolution, .flags = kFluid, .tooltip = "The velocity grid's short side."),
        JE_FIELD(FluidSimulationComponent, dyeResolution, .flags = kFluid, .tooltip = "The dye image's short side."),
        JE_FIELD(FluidSimulationComponent, densityDissipation, .ops = &ops::FiniteFloat, .flags = kFluidFloat, .min = 0.0f),
        JE_FIELD(FluidSimulationComponent, velocityDissipation, .ops = &ops::FiniteFloat, .flags = kFluidFloat, .min = 0.0f),
        JE_FIELD(FluidSimulationComponent, pressure, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, pressureIterations, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, curl, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, gradientScale, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, simulationRate, .ops = &ops::FiniteFloat, .flags = kFluidFloat, .unit = Unit::Hertz),
        JE_FIELD(FluidSimulationComponent, maxSubsteps, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, timeScale, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, paused, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, splatRadius, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, splatForce, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, colorful, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, colorUpdateSpeed, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, splatsOnStart, .flags = kFluid, .updateFlags = kReset),
        JE_FIELD(FluidSimulationComponent, randomSplatCount, .flags = kFluid, .updateFlags = kReset),
        JE_FIELD(FluidSimulationComponent, seed, .flags = kFluid, .updateFlags = kReset),
        JE_FIELD(FluidSimulationComponent, shading, .flags = kFluid),
        // display-space values like the dye (no linear conversion)
        JE_FIELD(FluidSimulationComponent, backColor, .ops = &ops::FiniteVector3Always, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, transparent, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, bloom, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, bloomIterations, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, bloomResolution, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, bloomIntensity, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, bloomThreshold, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, bloomSoftKnee, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
        JE_FIELD(FluidSimulationComponent, sunrays, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, sunraysResolution, .flags = kFluid),
        JE_FIELD(FluidSimulationComponent, sunraysWeight, .ops = &ops::FiniteFloat, .flags = kFluidFloat),
    };

    // the old one's runtime state (its simulator, what is queued for it) stays; a new seed or
    // start burst restarts it
    void fluidPostDecode(void* comp, const DecodeContext& ctx) {
        FluidSimulationComponent& fluid = *static_cast<FluidSimulationComponent*>(comp);
        const FluidSimulationComponent* old = static_cast<const FluidSimulationComponent*>(ctx.old);
        if (old && (old->seed != fluid.seed || old->splatsOnStart != fluid.splatsOnStart || old->randomSplatCount != fluid.randomSplatCount)) {
            fluid.needReset = true;
        }
    }

    // ---------------------------------------------------------------- Lattice

    constexpr uint64_t kLattice = UpdateFlags_Lattice;

    // the saved poses: a value the tables do not look into (its codec writes it), equal when their
    // names and offsets are
    using Poses = std::vector<LatticePose>;
    bool equalPoses(const void* a, const void* b, float) {
        const Poses& x = *static_cast<const Poses*>(a);
        const Poses& y = *static_cast<const Poses*>(b);
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++) {
            if (x[i].name != y[i].name || x[i].offsets != y[i].offsets) return false;
        }
        return true;
    }
    constexpr TypeOps kPosesOps{"value", Kind::Custom, PropertyType::Custom, sizeof(Poses),
        &detail::construct<Poses>, &detail::destroy<Poses>, &detail::copy<Poses>, &equalPoses,
        nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};

    // resolution, subdivisions and binding weight: an edit outside the range is clamped (Catalog::updateEntity),
    // as before reflection, not refused
    constexpr FieldDesc kLatticeFields[] = {
        JE_FIELD(LatticeComponent, enabled),
        JE_FIELD(LatticeComponent, resolutionX, .flags = Clamped, .updateFlags = kLattice, .min = 2.0f, .max = 16.0f),
        JE_FIELD(LatticeComponent, resolutionY, .flags = Clamped, .updateFlags = kLattice, .min = 2.0f, .max = 16.0f),
        JE_FIELD(LatticeComponent, resolutionZ, .flags = Clamped, .updateFlags = kLattice, .min = 2.0f, .max = 16.0f),
        // the whole list, and each point (a prefab instance overrides points one by one)
        JE_FIELD(LatticeComponent, offsets, .flags = ListWhole | ListElements | ListTemplate, .updateFlags = kLattice),
        JE_FIELD(LatticeComponent, interpolation, .enumDesc = &enums::kLatticeInterpolation, .updateFlags = kLattice),
        JE_FIELD(LatticeComponent, outside, .enumDesc = &enums::kLatticeOutside, .updateFlags = kLattice),
        JE_FIELD(LatticeComponent, fadeDistance, .updateFlags = kLattice, .min = 0.0f),
        // poses only matter when applied, which writes offsets
        JE_FIELD(LatticeComponent, poses, .ops = &kPosesOps, .flags = ListWhole),
        JE_FIELD(LatticeComponent, jiggle, .updateFlags = kLattice),
        JE_FIELD(LatticeComponent, jiggleStiffness, .updateFlags = kLattice, .min = 0.0f),
        JE_FIELD(LatticeComponent, jiggleDamping, .updateFlags = kLattice, .min = 0.0f),
        JE_FIELD(LatticeComponent, jiggleInertia, .updateFlags = kLattice, .min = 0.0f),
        JE_FIELD(LatticeComponent, jiggleAnchor, .enumDesc = &enums::kLatticeJiggleAnchor, .updateFlags = kLattice),
        JE_FIELD(LatticeComponent, jiggleMaxOffset, .updateFlags = kLattice, .min = 0.0f),
    };

    const ComponentCodec kLatticeCodec = JE_STREAM_CODEC(LatticeComponent, encodeLatticeComponent, decodeLatticeComponent);

    constexpr FieldDesc kLatticeBindingFields[] = {
        JE_FIELD(LatticeBinding, lattice, .ops = &ops::Entity, .updateFlags = kLattice),
        JE_FIELD(LatticeBinding, weight, .flags = Clamped, .updateFlags = kLattice, .min = 0.0f, .max = 2.0f),
        JE_FIELD(LatticeBinding, stage, .enumDesc = &enums::kLatticeStage, .updateFlags = kLattice),
        JE_FIELD(LatticeBinding, submesh, .updateFlags = kLattice, .min = -1.0f, .tooltip = "Only this submesh (material slot); -1: all."),
        JE_FIELD(LatticeBinding, mask, .enumDesc = &enums::kLatticeMaskChannel, .updateFlags = kLattice),
    };

}

const StructDesc kLatticeBindingStruct = JE_STRUCT_DESC(LatticeBinding, "LatticeBinding", kLatticeBindingFields);

namespace {

    constexpr FieldDesc kLatticeModifierFields[] = {
        JE_FIELD(LatticeModifierComponent, enabled),
        JE_FIELD(LatticeModifierComponent, bindings, .flags = ListWhole | ListTemplate, .updateFlags = kLattice),
        JE_FIELD(LatticeModifierComponent, includeChildren, .updateFlags = kLattice),
        JE_FIELD(LatticeModifierComponent, subdivisions, .flags = Clamped, .updateFlags = kLattice, .min = 1.0f, .max = 64.0f),
    };

    const ComponentCodec kLatticeModifierCodec = JE_STREAM_CODEC(LatticeModifierComponent, encodeLatticeModifierComponent,
                                                                 decodeLatticeModifierComponent);

    // read by LatticeSystem::lateUpdate every frame (no update flag)
    constexpr FieldDesc kLatticeFollowerFields[] = {
        JE_FIELD(LatticeFollowerComponent, enabled),
        JE_FIELD(LatticeFollowerComponent, lattice, .ops = &ops::Entity),
        JE_FIELD(LatticeFollowerComponent, rest, .ops = &ops::Entity),
        JE_FIELD(LatticeFollowerComponent, applyPosition),
        JE_FIELD(LatticeFollowerComponent, applyRotation),
        JE_FIELD(LatticeFollowerComponent, applyScale),
        JE_FIELD(LatticeFollowerComponent, weight, .ops = &ops::FiniteFloat, .flags = OmitIfNonFinite),
    };

    // points[N]: the flat point index of column N of every key; values[N]: the offsets of key N
    constexpr FieldDesc kLatticeTracksFields[] = {
        JE_FIELD(LatticeTracksComponent, points, .flags = ListWhole | ListElements | ListTemplate),
        JE_FIELD(LatticeTracksComponent, values, .flags = ListWhole | ListElements | ListTemplate),
        // Hermite tangents, kept mirrored with values by key-edit commands
        JE_FIELD(LatticeTracksComponent, inTangents, .flags = ListWhole),
        JE_FIELD(LatticeTracksComponent, outTangents, .flags = ListWhole),
    };

    const ComponentCodec kLatticeTracksCodec = JE_STREAM_CODEC(LatticeTracksComponent, encodeLatticeTracksComponent, decodeLatticeTracksComponent);

    constexpr const char* kLatticeKnownKeys[] = {"enabled", "fadeDistance", "interpolation", "jiggle", "jiggleAnchor", "jiggleDamping",
        "jiggleInertia", "jiggleMaxOffset", "jiggleStiffness", "offsets", "outside", "poses", "resolutionX", "resolutionY", "resolutionZ"};
    constexpr const char* kLatticeModifierKnownKeys[] = {"bindings", "enabled", "includeChildren", "subdivisions"};
    constexpr const char* kLatticeTracksKnownKeys[] = {"inTangents", "outTangents", "points", "values"};

}

const ComponentDesc kFluidSimulationDesc{
    .type = ComponentType::FluidSimulationComponent, .cppName = "FluidSimulationComponent", .key = "fluidsimulation",
    .displayName = "Fluid Simulation", .icon = ICON_FA_DROPLET,
    .body = JE_STRUCT_DESC(FluidSimulationComponent, "FluidSimulationComponent", kFluidFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(FluidSimulationComponent),
    .postDecode = &fluidPostDecode,
    .codegen = JE_FACTORY_CODEGEN(createFluidSimulationComponent),
};

const ComponentDesc kLatticeDesc{
    .type = ComponentType::LatticeComponent, .cppName = "LatticeComponent", .key = "lattice", .displayName = "Lattice",
    .icon = ICON_FA_BORDER_NONE,
    .body = JE_STRUCT_DESC(LatticeComponent, "LatticeComponent", kLatticeFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(LatticeComponent),
    .codec = &kLatticeCodec,
    .codegen = JE_FACTORY_CODEGEN(createLatticeComponent),
    .extraKeys = kLatticeKnownKeys,
};

const ComponentDesc kLatticeModifierDesc{
    .type = ComponentType::LatticeModifierComponent, .cppName = "LatticeModifierComponent", .key = "latticemodifier",
    .displayName = "Lattice Modifier", .icon = ICON_FA_WAND_MAGIC_SPARKLES,
    .body = JE_STRUCT_DESC(LatticeModifierComponent, "LatticeModifierComponent", kLatticeModifierFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(LatticeModifierComponent),
    .codec = &kLatticeModifierCodec,
    .codegen = JE_FACTORY_CODEGEN(createLatticeModifierComponent),
    .extraKeys = kLatticeModifierKnownKeys,
};

const ComponentDesc kLatticeFollowerDesc{
    .type = ComponentType::LatticeFollowerComponent, .cppName = "LatticeFollowerComponent", .key = "latticefollower",
    .displayName = "Lattice Follower", .icon = ICON_FA_LOCATION_ARROW,
    .body = JE_STRUCT_DESC(LatticeFollowerComponent, "LatticeFollowerComponent", kLatticeFollowerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(LatticeFollowerComponent),
    .codegen = JE_FACTORY_CODEGEN(createLatticeFollowerComponent),
};

const ComponentDesc kLatticeTracksDesc{
    .type = ComponentType::LatticeTracksComponent, .cppName = "LatticeTracksComponent", .key = "latticetracks",
    .displayName = "Lattice Tracks", .icon = ICON_FA_CUBES_STACKED,
    .body = JE_STRUCT_DESC(LatticeTracksComponent, "LatticeTracksComponent", kLatticeTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(LatticeTracksComponent),
    .codec = &kLatticeTracksCodec,
    .codegen = JE_FACTORY_CODEGEN(createLatticeTracksComponent),
    .extraKeys = kLatticeTracksKnownKeys,
};

}
