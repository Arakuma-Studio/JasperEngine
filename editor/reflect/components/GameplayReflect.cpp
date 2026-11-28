// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S22's gameplay components (docs/specs/S08-reflection.md, S22): the 2D and 3D character
// controllers, the water volume and the buoyancy keep S22's codecs (every key written); their
// tables give the properties, schema, Inspector metadata and the generic copy of a value. The
// controllers' collision filter bits are saved but no property (as before: Derived).

#include "reflect/Components.h"

#include "Stream.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "component/SimulationCullingComponent.h"
#include "component/DialogueRunnerComponent.h"

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
        JE_FIELD(CharacterController2DComponent, categoryBits, .ops = &ops::UIntAs<uint16_t>, .flags = Derived | Hidden),
        JE_FIELD(CharacterController2DComponent, maskBits, .ops = &ops::UIntAs<uint16_t>, .flags = Derived | Hidden),
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
        JE_FIELD(CharacterControllerComponent, categoryBits, .ops = &ops::UIntAs<uint16_t>, .flags = Derived | Hidden),
        JE_FIELD(CharacterControllerComponent, maskBits, .ops = &ops::UIntAs<uint16_t>, .flags = Derived | Hidden),
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
        JE_FIELD(BuoyancyComponent, customPoints, .ops = &ops::Array<Vector3>, .flags = NoDefault | ListWhole),
        JE_FIELD(BuoyancyComponent, volumeScale),
        JE_FIELD(BuoyancyComponent, dragScale),
    };

    const ComponentCodec kBuoyancyCodec = JE_STREAM_CODEC(BuoyancyComponent, encodeBuoyancyComponent, decodeBuoyancyComponent);

    constexpr const char* kBuoyancyKnownKeys[] = {"columns", "customPoints", "dragScale", "enabled", "volumeScale"};

    // ---------------------------------------------------------------- Water Excluder, Water Wake

    bool waterExcluderHasBox(const void* owner){
        return static_cast<const WaterExcluderComponent*>(owner)->shape != WaterExcluderShape::CONVEX_HULL;
    }

    constexpr FieldDesc kWaterExcluderFields[] = {
        JE_FIELD(WaterExcluderComponent, enabled),
        JE_FIELD(WaterExcluderComponent, shape, .enumDesc = &enums::kWaterExcluderShape,
                 .tooltip = "Convex Hull: the hull of this GameObject's mesh (or its model's meshes); Box and Sphere: Center and Size."),
        JE_FIELD(WaterExcluderComponent, center, .unit = Unit::Meters, .tooltip = "Local centre of the Box or Sphere.",
                 .visibleIf = &waterExcluderHasBox),
        JE_FIELD(WaterExcluderComponent, size, .unit = Unit::Meters, .tooltip = "Local size of the Box (a Sphere fills it).",
                 .visibleIf = &waterExcluderHasBox),
    };

    constexpr FieldDesc kWaterWakeFields[] = {
        JE_FIELD(WaterWakeComponent, enabled),
        JE_FIELD(WaterWakeComponent, foam, .min = 0.0f, .max = 2.0f, .uiMin = 0.0f, .uiMax = 2.0f, .step = 0.05f,
                 .tooltip = "Churn behind the stern, spray along the hull and breaking bow waves."),
        JE_FIELD(WaterWakeComponent, waves, .min = 0.0f, .max = 2.0f, .uiMin = 0.0f, .uiMax = 2.0f, .step = 0.05f,
                 .tooltip = "The diverging and transverse waves of the wake."),
        JE_FIELD(WaterWakeComponent, width, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Waterline width along local X; 0: from the Water Excluder or the mesh."),
        JE_FIELD(WaterWakeComponent, length, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Waterline length along local Z; 0: from the Water Excluder or the mesh (or as Width)."),
    };

    // ---------------------------------------------------------------- Simulation Culling

    bool simulationCullingHasReducedBand(const void* owner){
        return static_cast<const SimulationCullingComponent*>(owner)->reducedRateDistance > 0.0f;
    }

    // the runtime state (state, reason, focusDistance) is never saved; the Inspector shows it in Play
    constexpr FieldDesc kSimulationCullingFields[] = {
        JE_FIELD(SimulationCullingComponent, enabled),
        JE_FIELD(SimulationCullingComponent, simulationDistance, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Beyond this distance from every simulation focus (the main camera by default) the GameObject is culled."),
        JE_FIELD(SimulationCullingComponent, reducedRateDistance, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Between this and Simulation Distance its scripts update every Reduced Update Interval frames; 0: no reduced band."),
        JE_FIELD(SimulationCullingComponent, reducedUpdateInterval, .flags = Clamped, .min = 2.0f, .max = 60.0f, .uiMin = 2.0f, .uiMax = 60.0f,
                 .tooltip = "Frames between two onUpdate calls in the reduced band (Engine.deltatime covers them all).",
                 .visibleIf = &simulationCullingHasReducedBand),
        JE_FIELD(SimulationCullingComponent, hysteresis, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "Metres past an edge before it moves to a farther state."),
        JE_FIELD(SimulationCullingComponent, boundsRadius, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "0 = automatic: the rendered or collider bounds when it starts, else 0.5 m."),
        JE_FIELD(SimulationCullingComponent, simulateWhenVisible, .tooltip = "Beyond the distance but on screen: keeps updating (at the reduced rate)."),
        JE_FIELD(SimulationCullingComponent, freezeWhenGroundNotLoaded,
                 .tooltip = "Frozen over Terrain World tiles that are not loaded, or where SimulationCulling.setGroundCheck says so."),
        JE_FIELD(SimulationCullingComponent, cullScripts, .label = "Scripts",
                 .tooltip = "Skips onUpdate, onPostUpdate and onFixedUpdate (other events, coroutines and timers run).", .group = "Cull"),
        JE_FIELD(SimulationCullingComponent, cullAnimator, .label = "Animator", .tooltip = "Stops the Animator (as Cull Completely).", .group = "Cull"),
        JE_FIELD(SimulationCullingComponent, cullPhysics, .label = "Physics",
                 .tooltip = "Freezes dynamic Rigidbodies in place (kinematic, still collided with); they resume with their velocity.", .group = "Cull"),
        JE_FIELD(SimulationCullingComponent, state, .enumDesc = &enums::kSimulationState, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite),
        JE_FIELD(SimulationCullingComponent, reason, .enumDesc = &enums::kSimulationCullReason, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite),
        JE_FIELD(SimulationCullingComponent, focusDistance, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite, .unit = Unit::Meters),
    };

    // ---------------------------------------------------------------- Dialogue Runner

    // the runtime state (isRunning, state, currentNode) is never saved; the Inspector shows it in Play
    constexpr FieldDesc kDialogueRunnerFields[] = {
        JE_FIELD(DialogueRunnerComponent, dialogue, .asset = AssetKind::Dialogue, .tooltip = "The .dialogue this runner plays."),
        JE_FIELD(DialogueRunnerComponent, startEntry, .label = "Start Entry",
                 .tooltip = "The entry start() begins at when none is given; empty: the entry named start, else the first."),
        JE_FIELD(DialogueRunnerComponent, autoStart, .tooltip = "Starts on Play or when instantiated, after every script's init of that frame."),
        JE_FIELD(DialogueRunnerComponent, seed, .tooltip = "Seeds the random variants, random() and chance(); 0: from the GameObject's id."),
        JE_FIELD(DialogueRunnerComponent, view, .ops = &ops::Entity, .flags = OmitIfDefault | EmptyIsDefault,
                 .tooltip = "The GameObject whose UI Document shows the lines (the dialogue template's view); None: the game's own."),
        JE_FIELD(DialogueRunnerComponent, isRunning, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite),
        JE_FIELD(DialogueRunnerComponent, state, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite),
        JE_FIELD(DialogueRunnerComponent, currentNode, .flags = NoSave | ReadOnly | Hidden | NoMcpWrite),
    };

}

const ComponentDesc kCharacterController2DDesc{
    .type = ComponentType::CharacterController2DComponent, .cppName = "CharacterController2DComponent", .key = "charactercontroller2d",
    .displayName = "Character Controller 2D", .icon = ICON_FA_PERSON_WALKING,
    .body = JE_STRUCT_DESC(CharacterController2DComponent, "CharacterController2DComponent", kCharacterController2DFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(CharacterController2DComponent),
    .codec = &kCharacterController2DCodec,
    .decodeApply = DecodeApply::MarkOnly,
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
    .extraKeys = kWaterVolumeKnownKeys,
};

const ComponentDesc kWaterExcluderDesc{
    .type = ComponentType::WaterExcluderComponent, .cppName = "WaterExcluderComponent", .key = "waterexcluder",
    .displayName = "Water Excluder", .icon = ICON_FA_SHIP,
    .body = JE_STRUCT_DESC(WaterExcluderComponent, "WaterExcluderComponent", kWaterExcluderFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(WaterExcluderComponent),
};

const ComponentDesc kWaterWakeDesc{
    .type = ComponentType::WaterWakeComponent, .cppName = "WaterWakeComponent", .key = "waterwake",
    .displayName = "Water Wake", .icon = ICON_FA_WATER,
    .body = JE_STRUCT_DESC(WaterWakeComponent, "WaterWakeComponent", kWaterWakeFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(WaterWakeComponent),
};

const ComponentDesc kBuoyancyDesc{
    .type = ComponentType::BuoyancyComponent, .cppName = "BuoyancyComponent", .key = "buoyancy",
    .displayName = "Buoyancy", .icon = ICON_FA_LIFE_RING,
    .body = JE_STRUCT_DESC(BuoyancyComponent, "BuoyancyComponent", kBuoyancyFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(BuoyancyComponent),
    .codec = &kBuoyancyCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .extraKeys = kBuoyancyKnownKeys,
};

const ComponentDesc kSimulationCullingDesc{
    .type = ComponentType::SimulationCullingComponent, .cppName = "SimulationCullingComponent", .key = "simulationculling",
    .displayName = "Simulation Culling", .icon = ICON_FA_EYE_SLASH,
    .body = JE_STRUCT_DESC(SimulationCullingComponent, "SimulationCullingComponent", kSimulationCullingFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SimulationCullingComponent),
};

const ComponentDesc kDialogueRunnerDesc{
    .type = ComponentType::DialogueRunnerComponent, .cppName = "DialogueRunnerComponent", .key = "dialoguerunner",
    .displayName = "Dialogue Runner", .icon = ICON_FA_COMMENTS,
    .body = JE_STRUCT_DESC(DialogueRunnerComponent, "DialogueRunnerComponent", kDialogueRunnerFields),
    .structural = false, JE_COMPONENT_ACCESS(DialogueRunnerComponent),
};

}
