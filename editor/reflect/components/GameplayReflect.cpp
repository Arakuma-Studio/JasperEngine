// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S22's gameplay components (docs/specs/S08-reflection.md, S22): the 2D and 3D character
// controllers, the water volume and the buoyancy keep S22's codecs (every key written); their
// tables give the properties, schema, Inspector metadata and the generic copy of a value. The
// controllers' collision filter bits are saved and reached by path, not listed (as before).

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/WaterVolumeComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Character Controller 2D

    // sizes in points (the canvas units 2D bodies use)
    constexpr FieldDesc kCharacterController2DFields[] = {
        JE_FIELD(CharacterController2DComponent, enabled),
        JE_FIELD(CharacterController2DComponent, center, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, height, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, radius, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, slopeLimit, .unit = Unit::Degrees, .tooltip = "Steeper surfaces are not walkable."),
        JE_FIELD(CharacterController2DComponent, stepOffset, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, skinWidth, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, minMoveDistance, .unit = Unit::Pixels),
        JE_FIELD(CharacterController2DComponent, pushStrength, .unit = Unit::Newtons),
        JE_FIELD(CharacterController2DComponent, pushBodies),
        JE_FIELD(CharacterController2DComponent, movingPlatforms),
        JE_FIELD(CharacterController2DComponent, categoryBits, .ops = &ops::UIntAs<uint16_t>, .flags = NotListed | Hidden),
        JE_FIELD(CharacterController2DComponent, maskBits, .ops = &ops::UIntAs<uint16_t>, .flags = NotListed | Hidden),
    };

    const ComponentCodec kCharacterController2DCodec = JE_STREAM_CODEC(CharacterController2DComponent, encodeCharacterController2DComponent,
                                                                       decodeCharacterController2DComponent);

    constexpr const char* kCharacterController2DKnownKeys[] = {"categoryBits", "center", "enabled", "height", "maskBits", "minMoveDistance",
        "movingPlatforms", "pushBodies", "pushStrength", "radius", "skinWidth", "slopeLimit", "stepOffset"};

    // ---------------------------------------------------------------- Character Controller

    constexpr FieldDesc kCharacterControllerFields[] = {
        JE_FIELD(CharacterControllerComponent, enabled),
        JE_FIELD(CharacterControllerComponent, center, .unit = Unit::Meters),
        JE_FIELD(CharacterControllerComponent, height, .unit = Unit::Meters),
        JE_FIELD(CharacterControllerComponent, radius, .unit = Unit::Meters),
        JE_FIELD(CharacterControllerComponent, slopeLimit, .unit = Unit::Degrees, .tooltip = "Steeper surfaces are not walkable."),
        JE_FIELD(CharacterControllerComponent, stepOffset, .unit = Unit::Meters, .tooltip = "Steps up to this high are climbed (0: none)."),
        JE_FIELD(CharacterControllerComponent, skinWidth, .unit = Unit::Meters),
        JE_FIELD(CharacterControllerComponent, minMoveDistance, .unit = Unit::Meters),
        JE_FIELD(CharacterControllerComponent, mass, .unit = Unit::Kilograms),
        JE_FIELD(CharacterControllerComponent, pushStrength, .unit = Unit::Newtons),
        JE_FIELD(CharacterControllerComponent, pushBodies),
        JE_FIELD(CharacterControllerComponent, movingPlatforms),
        JE_FIELD(CharacterControllerComponent, inheritPlatformYaw),
        JE_FIELD(CharacterControllerComponent, detectCollisions),
        JE_FIELD(CharacterControllerComponent, characterCollisions),
        JE_FIELD(CharacterControllerComponent, enhancedEdgeRemoval),
        JE_FIELD(CharacterControllerComponent, categoryBits, .ops = &ops::UIntAs<uint16_t>, .flags = NotListed | Hidden),
        JE_FIELD(CharacterControllerComponent, maskBits, .ops = &ops::UIntAs<uint16_t>, .flags = NotListed | Hidden),
    };

    const ComponentCodec kCharacterControllerCodec = JE_STREAM_CODEC(CharacterControllerComponent, encodeCharacterControllerComponent,
                                                                     decodeCharacterControllerComponent);

    constexpr const char* kCharacterControllerKnownKeys[] = {"categoryBits", "center", "characterCollisions", "detectCollisions", "enabled",
        "enhancedEdgeRemoval", "height", "inheritPlatformYaw", "maskBits", "mass", "minMoveDistance", "movingPlatforms", "pushBodies",
        "pushStrength", "radius", "skinWidth", "slopeLimit", "stepOffset"};

    // ---------------------------------------------------------------- Water Volume

    constexpr FieldDesc kWaterVolumeFields[] = {
        JE_FIELD(WaterVolumeComponent, enabled),
        // XZ the extent, Y the depth under the surface
        JE_FIELD(WaterVolumeComponent, size, .unit = Unit::Meters),
        JE_FIELD(WaterVolumeComponent, surfaceOffset, .unit = Unit::Meters),
        JE_FIELD(WaterVolumeComponent, infinite, .tooltip = "Water everywhere under the surface (an ocean)."),
        JE_FIELD(WaterVolumeComponent, density, .tooltip = "kg/m3 (1000: fresh water)."),
        JE_FIELD(WaterVolumeComponent, flow, .unit = Unit::MetersPerSecond),
        JE_FIELD(WaterVolumeComponent, linearDrag),
        JE_FIELD(WaterVolumeComponent, angularDrag),
        JE_FIELD(WaterVolumeComponent, waves, .enumDesc = &enums::kWaterVolumeWaves),
    };

    const ComponentCodec kWaterVolumeCodec = JE_STREAM_CODEC(WaterVolumeComponent, encodeWaterVolumeComponent, decodeWaterVolumeComponent);

    constexpr const char* kWaterVolumeKnownKeys[] = {"angularDrag", "density", "enabled", "flow", "infinite", "linearDrag", "size",
        "surfaceOffset", "waves"};

    // ---------------------------------------------------------------- Buoyancy

    constexpr FieldDesc kBuoyancyFields[] = {
        JE_FIELD(BuoyancyComponent, enabled),
        JE_FIELD(BuoyancyComponent, columns, .enumDesc = &enums::kBuoyancyColumns),
        // the columns' bottoms in local space, with the CUSTOM columns
        JE_FIELD(BuoyancyComponent, customPoints, .ops = &ops::PushBackList<Vector3>, .flags = NoDefault | ListWhole),
        JE_FIELD(BuoyancyComponent, volumeScale),
        JE_FIELD(BuoyancyComponent, dragScale),
    };

    const ComponentCodec kBuoyancyCodec = JE_STREAM_CODEC(BuoyancyComponent, encodeBuoyancyComponent, decodeBuoyancyComponent);

    constexpr const char* kBuoyancyKnownKeys[] = {"columns", "customPoints", "dragScale", "enabled", "volumeScale"};

}

const ComponentDesc kCharacterController2DDesc{
    .type = ComponentType::CharacterController2DComponent, .cppName = "CharacterController2DComponent", .key = "charactercontroller2d",
    .displayName = "Character Controller 2D", .icon = ICON_FA_PERSON_WALKING,
    .body = JE_STRUCT_DESC(CharacterController2DComponent, "CharacterController2DComponent", kCharacterController2DFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(CharacterController2DComponent),
    .codec = &kCharacterController2DCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = [](EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createCharacterController2DComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting);
    },
    .codegenGuard = "JASPERENGINE_PHYSICS_2D",
    .extraKeys = kCharacterController2DKnownKeys,
};

const ComponentDesc kCharacterControllerDesc{
    .type = ComponentType::CharacterControllerComponent, .cppName = "CharacterControllerComponent", .key = "charactercontroller",
    .displayName = "Character Controller", .icon = ICON_FA_PERSON_WALKING,
    .body = JE_STRUCT_DESC(CharacterControllerComponent, "CharacterControllerComponent", kCharacterControllerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(CharacterControllerComponent),
    .codec = &kCharacterControllerCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = [](EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createCharacterControllerComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting);
    },
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
    .extraKeys = kCharacterControllerKnownKeys,
};

const ComponentDesc kWaterVolumeDesc{
    .type = ComponentType::WaterVolumeComponent, .cppName = "WaterVolumeComponent", .key = "watervolume",
    .displayName = "Water Volume", .icon = ICON_FA_WATER,
    .body = JE_STRUCT_DESC(WaterVolumeComponent, "WaterVolumeComponent", kWaterVolumeFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(WaterVolumeComponent),
    .codec = &kWaterVolumeCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = [](EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createWaterVolumeComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting);
    },
    .extraKeys = kWaterVolumeKnownKeys,
};

const ComponentDesc kBuoyancyDesc{
    .type = ComponentType::BuoyancyComponent, .cppName = "BuoyancyComponent", .key = "buoyancy",
    .displayName = "Buoyancy", .icon = ICON_FA_LIFE_RING,
    .body = JE_STRUCT_DESC(BuoyancyComponent, "BuoyancyComponent", kBuoyancyFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(BuoyancyComponent),
    .codec = &kBuoyancyCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = [](EntityRegistry* registry, Entity entity, const CodegenContext& ctx) {
        return Factory::createBuoyancyComponent(ctx.indentSpaces, registry, entity, ctx.sceneName, ctx.entityName, ctx.assignExisting);
    },
    .extraKeys = kBuoyancyKnownKeys,
};

}
