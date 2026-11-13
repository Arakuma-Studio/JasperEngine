// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S96 vehicles (docs/specs/S96-physics-gaps.md REQ-1, REQ-5): the Wheel Collider and the Vehicle
// Drivetrain with the generic YAML codec. The runtime inputs and read-outs are Transient
// properties (MCP get_runtime_entity / set_runtime_property drive and read a car in Play); the
// vehicle is built again by the physics when a saved field changes, so none needs an update flag.

#include "reflect/Components.h"

#include "Factory.h"
#include "component/VehicleDrivetrainComponent.h"
#include "component/WheelColliderComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // read-outs of the last fixed step: listed for MCP, never saved, drawn by the Inspector's Play foldout
    constexpr uint32_t kReadout = Transient | ReadOnly | NoMcpWrite | NoDefault | NoCodegen | Hidden;
    // inputs a script (or set_runtime_property) gives in Play: not saved
    constexpr uint32_t kInput = Transient | NoCodegen | Hidden;

    constexpr FieldDesc kJointSpringFields[] = {
        JE_FIELD(JointSpring3D, spring, .unit = Unit::Newtons, .min = 0.0f, .tooltip = "N/m: the force pushing the wheel back to its target position."),
        JE_FIELD(JointSpring3D, damper, .min = 0.0f, .tooltip = "N s/m: the force against the suspension's speed."),
        JE_FIELD(JointSpring3D, targetPosition, .flags = Clamped, .min = 0.0f, .max = 1.0f,
                 .tooltip = "Where the wheel rests under the car's weight: 0 full droop, 1 full compression."),
    };

    constexpr FieldDesc kWheelFrictionCurveFields[] = {
        JE_FIELD(WheelFrictionCurve, extremumSlip, .min = 0.0f, .tooltip = "The slip of the curve's peak."),
        JE_FIELD(WheelFrictionCurve, extremumValue, .min = 0.0f, .tooltip = "The grip at the peak (times the wheel's load)."),
        JE_FIELD(WheelFrictionCurve, asymptoteSlip, .min = 0.0f, .tooltip = "The slip from which the grip stays at the asymptote."),
        JE_FIELD(WheelFrictionCurve, asymptoteValue, .min = 0.0f, .tooltip = "The grip of a sliding tire."),
        JE_FIELD(WheelFrictionCurve, stiffness, .min = 0.0f, .tooltip = "Multiplies the curve (lower: a slippery surface)."),
    };

    constexpr FieldDesc kDifferentialFields[] = {
        JE_FIELD(VehicleDifferential, leftWheel, .ops = &ops::Entity, .tooltip = "A Wheel Collider of this vehicle (none: a single-wheel drive)."),
        JE_FIELD(VehicleDifferential, rightWheel, .ops = &ops::Entity),
        JE_FIELD(VehicleDifferential, ratio, .min = 0.0f, .tooltip = "Gear box to wheels."),
        JE_FIELD(VehicleDifferential, leftRightSplit, .flags = Clamped, .min = 0.0f, .max = 1.0f, .tooltip = "0: all torque to the left wheel, 1: to the right."),
        JE_FIELD(VehicleDifferential, limitedSlipRatio, .min = 1.0f, .tooltip = "Max / min wheel speed before the slower wheel gets all the torque."),
        JE_FIELD(VehicleDifferential, engineTorqueRatio, .min = 0.0f, .tooltip = "Its share of the engine's torque (the shares are scaled to sum to 1)."),
    };

    constexpr FieldDesc kAntiRollBarFields[] = {
        JE_FIELD(VehicleAntiRollBar, leftWheel, .ops = &ops::Entity),
        JE_FIELD(VehicleAntiRollBar, rightWheel, .ops = &ops::Entity),
        JE_FIELD(VehicleAntiRollBar, stiffness, .min = 0.0f, .tooltip = "N/m of difference between the two suspensions."),
    };

}

JE_REFLECT_STRUCT_TRAITS(JointSpring3D, kJointSpring3DStruct);
const StructDesc kJointSpring3DStruct = JE_STRUCT_DESC(JointSpring3D, "JointSpring3D", kJointSpringFields);
JE_REFLECT_STRUCT_TRAITS(WheelFrictionCurve, kWheelFrictionCurveStruct);
const StructDesc kWheelFrictionCurveStruct = JE_STRUCT_DESC(WheelFrictionCurve, "WheelFrictionCurve", kWheelFrictionCurveFields);
JE_REFLECT_STRUCT_TRAITS(VehicleDifferential, kVehicleDifferentialStruct);
const StructDesc kVehicleDifferentialStruct = JE_STRUCT_DESC(VehicleDifferential, "VehicleDifferential", kDifferentialFields);
JE_REFLECT_STRUCT_TRAITS(VehicleAntiRollBar, kVehicleAntiRollBarStruct);
const StructDesc kVehicleAntiRollBarStruct = JE_STRUCT_DESC(VehicleAntiRollBar, "VehicleAntiRollBar", kAntiRollBarFields);

namespace {

    // ---------------------------------------------------------------- Wheel Collider

    constexpr FieldDesc kWheelColliderFields[] = {
        JE_FIELD(WheelColliderComponent, enabled),
        JE_FIELD(WheelColliderComponent, mass, .unit = Unit::Kilograms, .min = 0.0f, .tooltip = "The wheel's own mass: its spin inertia is 0.5 mass radius^2."),
        JE_FIELD(WheelColliderComponent, radius, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(WheelColliderComponent, wheelDampingRate, .min = 0.0f, .tooltip = "N m s: slows the wheel's spin."),
        JE_FIELD(WheelColliderComponent, suspensionDistance, .unit = Unit::Meters, .min = 0.0f,
                 .tooltip = "The suspension's travel, down the body's down axis from the wheel's position."),
        JE_FIELD(WheelColliderComponent, forceAppPointDistance, .unit = Unit::Meters, .min = 0.0f,
                 .label = "Force App Point Distance", .tooltip = "Where the forces push, above the resting wheel's base (0: at the contact)."),
        JE_FIELD(WheelColliderComponent, center, .unit = Unit::Meters, .tooltip = "The wheel's centre in this GameObject's space."),
        JE_FIELD(WheelColliderComponent, suspensionSpring),
        JE_FIELD(WheelColliderComponent, forwardFriction, .tooltip = "Grip along the wheel against the slip ratio."),
        JE_FIELD(WheelColliderComponent, sidewaysFriction, .tooltip = "Grip across the wheel against the slip angle (radians)."),
        JE_FIELD(WheelColliderComponent, maxSteerAngle, .tooltip = "The steer a Vehicle Drivetrain's full steer gives."),
        JE_FIELD(WheelColliderComponent, maxBrakeTorque, .min = 0.0f, .tooltip = "N m a Vehicle Drivetrain's full brake gives."),
        JE_FIELD(WheelColliderComponent, maxHandBrakeTorque, .min = 0.0f, .tooltip = "N m a Vehicle Drivetrain's full hand brake gives (0: not braked by it)."),
        JE_FIELD(WheelColliderComponent, driveTransform, .tooltip = "Puts this GameObject at the wheel's pose (suspension, steer, spin) after each step."),
        JE_FIELD(WheelColliderComponent, motorTorque, .flags = kInput),
        JE_FIELD(WheelColliderComponent, brakeTorque, .flags = kInput),
        JE_FIELD(WheelColliderComponent, steerAngle, .flags = kInput),
        JE_FIELD(WheelColliderComponent, isGrounded, .flags = kReadout),
        JE_FIELD(WheelColliderComponent, rpm, .flags = kReadout),
        JE_FIELD(WheelColliderComponent, sprungMass, .flags = kReadout, .unit = Unit::Kilograms),
        JE_FIELD(WheelColliderComponent, suspensionCompression, .flags = kReadout),
        JE_FIELD(WheelColliderComponent, vehicleBody, .ops = &ops::Entity, .flags = kReadout),
    };

    std::string springLiteral(const JointSpring3D& s){
        return "JointSpring3D{" + Factory::formatFloat(s.spring) + ", " + Factory::formatFloat(s.damper) + ", " +
               Factory::formatFloat(s.targetPosition) + "}";
    }

    std::string curveLiteral(const WheelFrictionCurve& c){
        return "WheelFrictionCurve{" + Factory::formatFloat(c.extremumSlip) + ", " + Factory::formatFloat(c.extremumValue) + ", " +
               Factory::formatFloat(c.asymptoteSlip) + ", " + Factory::formatFloat(c.asymptoteValue) + ", " +
               Factory::formatFloat(c.stiffness) + "}";
    }

    // the nested structs (the generic codegen writes scalar fields only), away from their defaults
    void wheelColliderCodegen(const void* comp, const CodegenContext& ctx, std::string& out){
        const WheelColliderComponent& wheel = *static_cast<const WheelColliderComponent*>(comp);
        const WheelColliderComponent defaults;
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        if (wheel.suspensionSpring != defaults.suspensionSpring)
            out += ind + ctx.variable + ".suspensionSpring = " + springLiteral(wheel.suspensionSpring) + ";\n";
        if (wheel.forwardFriction != defaults.forwardFriction)
            out += ind + ctx.variable + ".forwardFriction = " + curveLiteral(wheel.forwardFriction) + ";\n";
        if (wheel.sidewaysFriction != defaults.sidewaysFriction)
            out += ind + ctx.variable + ".sidewaysFriction = " + curveLiteral(wheel.sidewaysFriction) + ";\n";
    }

    // ---------------------------------------------------------------- Vehicle Drivetrain

    constexpr TypeOps kRatiosOps = ops::PushBackListCleared<float>;
    constexpr TypeOps kDifferentialListOps = detail::withStatements(ops::Array<VehicleDifferential>, &detail::structPushBackStatements);
    constexpr TypeOps kAntiRollBarListOps = detail::withStatements(ops::Array<VehicleAntiRollBar>, &detail::structPushBackStatements);

    constexpr FieldDesc kVehicleDrivetrainFields[] = {
        JE_FIELD(VehicleDrivetrainComponent, enabled),
        JE_FIELD(VehicleDrivetrainComponent, maxTorque, .min = 0.0f, .tooltip = "N m at the top of the torque curve.", .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, minRPM, .min = 0.0f, .label = "Min RPM", .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, maxRPM, .min = 0.0f, .label = "Max RPM", .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, torqueCurve, .tooltip = "The share of Max Torque (value) at a share of Max RPM (time 0..1).", .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, engineInertia, .min = 0.0f, .tooltip = "kg m^2", .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, engineDamping, .min = 0.0f, .group = "Engine"),
        JE_FIELD(VehicleDrivetrainComponent, transmission, .enumDesc = &enums::kVehicleTransmissionMode,
                 .tooltip = "Automatic shifts at the shift RPMs (a negative throttle reverses); Manual keeps the gear setGear chose.", .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, gearRatios, .ops = &kRatiosOps, .flags = ListWhole, .tooltip = "Forward gears, first to last.", .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, reverseGearRatios, .ops = &kRatiosOps, .flags = ListWhole, .tooltip = "Negative ratios.", .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, switchTime, .unit = Unit::Seconds, .min = 0.0f, .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, clutchReleaseTime, .unit = Unit::Seconds, .min = 0.0f, .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, switchLatency, .unit = Unit::Seconds, .min = 0.0f, .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, shiftUpRPM, .min = 0.0f, .label = "Shift Up RPM", .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, shiftDownRPM, .min = 0.0f, .label = "Shift Down RPM", .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, clutchStrength, .min = 0.0f, .group = "Transmission"),
        JE_FIELD(VehicleDrivetrainComponent, differentials, .ops = &kDifferentialListOps, .flags = ListWhole | ListTemplate,
                 .tooltip = "The driven wheel pairs (none: every axle, torque shared evenly).", .group = "Differentials"),
        JE_FIELD(VehicleDrivetrainComponent, differentialLimitedSlipRatio, .min = 1.0f, .group = "Differentials"),
        JE_FIELD(VehicleDrivetrainComponent, antiRollBars, .ops = &kAntiRollBarListOps, .flags = ListWhole | ListTemplate, .group = "Anti-Roll Bars"),
        JE_FIELD(VehicleDrivetrainComponent, maxPitchRollAngle, .tooltip = "The body's up stays within this of the world's up (180: off)."),
        JE_FIELD(VehicleDrivetrainComponent, throttle, .flags = kInput),
        JE_FIELD(VehicleDrivetrainComponent, steer, .flags = kInput),
        JE_FIELD(VehicleDrivetrainComponent, brake, .flags = kInput),
        JE_FIELD(VehicleDrivetrainComponent, handBrake, .flags = kInput),
        JE_FIELD(VehicleDrivetrainComponent, rpm, .flags = kReadout),
        JE_FIELD(VehicleDrivetrainComponent, gear, .flags = kReadout),
        JE_FIELD(VehicleDrivetrainComponent, speed, .flags = kReadout, .unit = Unit::MetersPerSecond),
    };

}

const ComponentDesc kWheelColliderDesc{
    .type = ComponentType::WheelColliderComponent, .cppName = "WheelColliderComponent", .key = "wheelcollider",
    .displayName = "Wheel Collider", .icon = ICON_FA_CIRCLE_NOTCH,
    .body = JE_STRUCT_DESC(WheelColliderComponent, "WheelColliderComponent", kWheelColliderFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(WheelColliderComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "wheelCollider",
    .codegenExtra = &wheelColliderCodegen,
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
};

const ComponentDesc kVehicleDrivetrainDesc{
    .type = ComponentType::VehicleDrivetrainComponent, .cppName = "VehicleDrivetrainComponent", .key = "vehicledrivetrain",
    .displayName = "Vehicle Drivetrain", .icon = ICON_FA_GEARS,
    .body = JE_STRUCT_DESC(VehicleDrivetrainComponent, "VehicleDrivetrainComponent", kVehicleDrivetrainFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(VehicleDrivetrainComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "vehicleDrivetrain",
    .codegenGuard = "JASPERENGINE_PHYSICS_3D",
};

}
