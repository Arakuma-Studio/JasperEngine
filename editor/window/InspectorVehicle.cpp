// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector of the S96 vehicle components: the Wheel Collider with Unity's labels and its
// friction curves drawn as small plots, the Vehicle Drivetrain with its gear ratios,
// differentials and anti-roll bars as lists, and a read-only Runtime foldout of each while the
// scene plays. Every edit is one PropertyCmd (undo).

#include "InspectorWindow.h"

#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/VehicleDrivetrainComponent.h"
#include "component/WheelColliderComponent.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Enums.h"
#include "util/Angle.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // the curve's grip at a slip, as the physics reads it (linear between its three points)
    float inspectorWheelCurveAt(const WheelFrictionCurve& curve, float slip){
        const float stiffness = std::max(0.0f, curve.stiffness);
        const float extremumSlip = std::max(1.0e-4f, curve.extremumSlip);
        const float asymptoteSlip = std::max(extremumSlip + 1.0e-4f, curve.asymptoteSlip);
        if (slip <= extremumSlip){
            return slip / extremumSlip * curve.extremumValue * stiffness;
        }
        if (slip <= asymptoteSlip){
            const float t = (slip - extremumSlip) / (asymptoteSlip - extremumSlip);
            return (curve.extremumValue + (curve.asymptoteValue - curve.extremumValue) * t) * stiffness;
        }
        return curve.asymptoteValue * stiffness;
    }

    void inspectorWheelCurvePlot(const char* id, const WheelFrictionCurve& curve){
        constexpr int samples = 48;
        float values[samples];
        const float maxSlip = std::max(1.0e-3f, std::max(curve.extremumSlip, curve.asymptoteSlip) * 1.5f);
        float top = 0.0f;
        for (int i = 0; i < samples; i++){
            values[i] = inspectorWheelCurveAt(curve, maxSlip * (float)i / (float)(samples - 1));
            top = std::max(top, values[i]);
        }
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "slip 0..%.2f", maxSlip);
        ImGui::PlotLines(id, values, samples, 0, overlay, 0.0f, std::max(0.01f, top * 1.1f),
                         ImVec2(ImGui::GetContentRegionAvail().x, 3.0f * ImGui::GetFrameHeight()));
    }

}

void editor::InspectorWindow::drawWheelColliderComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const WheelColliderComponent& wheel = scene->getComponent<WheelColliderComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsMass = settings;
    settingsMass.help = "The wheel's own mass (kg): its spin inertia is 0.5 mass radius^2";
    RowSettings settingsDamping = settings;
    settingsDamping.help = "N m s: slows the wheel's spin";
    RowSettings settingsTravel = settings;
    settingsTravel.help = "The suspension's travel (m), down the body's down axis from the wheel's position";
    RowSettings settingsForcePoint = settings;
    settingsForcePoint.help = "Where the suspension and tire forces push, in metres above the resting wheel's base (0: at the contact)";
    RowSettings settingsCenter = settings;
    settingsCenter.help = "The wheel's centre in this GameObject's space";
    RowSettings settingsDrive;
    settingsDrive.help = "Puts this GameObject at the wheel's pose (suspension, steer, spin) after each step, so a wheel mesh below it follows";

    beginTable(cpType, getLabelSize("Force App Point Distance"));
    propertyRow(RowPropertyType::FloatPositive, cpType, "mass", "Mass", sceneProject, entities, settingsMass);
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "wheelDampingRate", "Wheel Damping Rate", sceneProject, entities, settingsDamping);
    propertyRow(RowPropertyType::FloatPositive, cpType, "suspensionDistance", "Suspension Distance", sceneProject, entities, settingsTravel);
    propertyRow(RowPropertyType::FloatPositive, cpType, "forceAppPointDistance", "Force App Point Distance", sceneProject, entities, settingsForcePoint);
    propertyRow(RowPropertyType::Vector3, cpType, "center", "Center", sceneProject, entities, settingsCenter);
    propertyRow(RowPropertyType::Bool, cpType, "driveTransform", "Drive Transform", sceneProject, entities, settingsDrive);
    endTable();

    RowSettings settingsSpring = settings;
    settingsSpring.help = "N/m: the force pushing the wheel back to its target position";
    RowSettings settingsDamper = settings;
    settingsDamper.help = "N s/m: the force against the suspension's speed";
    RowSettings settingsTarget = settings;
    settingsTarget.floatMin = 0.0f;
    settingsTarget.floatMax = 1.0f;
    settingsTarget.help = "Where the wheel rests under the car's weight: 0 full droop, 1 full compression";
    separatorText("Suspension Spring");
    beginTable(cpType, getLabelSize("Target Position"), "spring");
    propertyRow(RowPropertyType::FloatPositive, cpType, "suspensionSpring.spring", "Spring", sceneProject, entities, settingsSpring);
    propertyRow(RowPropertyType::FloatPositive, cpType, "suspensionSpring.damper", "Damper", sceneProject, entities, settingsDamper);
    propertyRow(RowPropertyType::Float_0_1, cpType, "suspensionSpring.targetPosition", "Target Position", sceneProject, entities, settingsTarget);
    endTable();

    auto frictionCurve = [&](const char* title, const char* key, const WheelFrictionCurve& curve, const char* slipHelp){
        separatorText(title);
        const std::string prefix = std::string(key) + ".";
        RowSettings settingsSlip = settings;
        settingsSlip.help = slipHelp;
        RowSettings settingsValue = settings;
        settingsValue.help = "The grip, times the wheel's load";
        RowSettings settingsStiffness = settings;
        settingsStiffness.help = "Multiplies the whole curve (lower: a slippery surface)";
        beginTable(cpType, getLabelSize("Asymptote Value"), key);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "extremumSlip", "Extremum Slip", sceneProject, entities, settingsSlip);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "extremumValue", "Extremum Value", sceneProject, entities, settingsValue);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "asymptoteSlip", "Asymptote Slip", sceneProject, entities, settingsSlip);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "asymptoteValue", "Asymptote Value", sceneProject, entities, settingsValue);
        propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "stiffness", "Stiffness", sceneProject, entities, settingsStiffness);
        endTable();
        inspectorWheelCurvePlot((std::string("##plot_") + key).c_str(), curve);
    };
    frictionCurve("Forward Friction", "forwardFriction", wheel.forwardFriction, "The slip ratio (0: rolling, 1: locked or spinning at twice the ground's speed)");
    frictionCurve("Sideways Friction", "sidewaysFriction", wheel.sidewaysFriction, "The slip angle in radians (0.2: about 11 degrees)");

    RowSettings settingsSteer = settings;
    settingsSteer.help = "The steer a Vehicle Drivetrain's full steer input gives";
    RowSettings settingsBrake = settings;
    settingsBrake.help = "N m a Vehicle Drivetrain's full brake gives";
    RowSettings settingsHandBrake = settings;
    settingsHandBrake.help = "N m a Vehicle Drivetrain's full hand brake gives (0: this wheel is not hand braked)";
    separatorText("Vehicle Drivetrain Limits");
    beginTable(cpType, getLabelSize("Max Hand Brake Torque"), "limits");
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxSteerAngle", "Max Steer Angle", sceneProject, entities, settingsSteer);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxBrakeTorque", "Max Brake Torque", sceneProject, entities, settingsBrake);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxHandBrakeTorque", "Max Hand Brake Torque", sceneProject, entities, settingsHandBrake);
    endTable();

    // a wheel goes below the vehicle's body, never on it
    for (Entity entity : entities){
        bool hasBody = false;
        const Transform* transform = scene->findComponent<Transform>(entity);
        for (int depth = 0; transform && transform->parent != NULL_ENTITY && depth < 256; depth++){
            if (const RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(transform->parent)){
                hasBody = body->type == BodyType::DYNAMIC;
                break;
            }
            transform = scene->findComponent<Transform>(transform->parent);
        }
        const char* problem = nullptr;
        if (scene->findComponent<RigidbodyComponent>(entity)){
            problem = "A Wheel Collider does nothing on a GameObject with a Rigidbody: put it on a child of the vehicle's body.";
        }else if (!hasBody){
            problem = "A Wheel Collider needs a dynamic Rigidbody on a GameObject above it (the vehicle's body).";
        }
        if (problem){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", problem);
            break;
        }
    }

    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Grounded: %s   RPM: %.0f   Sprung Mass: %.1f kg", wheel.isGrounded ? "yes" : "no", wheel.rpm, wheel.sprungMass);
        ImGui::TextDisabled("Motor: %.0f N m   Brake: %.0f N m   Steer: %.1f", wheel.motorTorque, wheel.brakeTorque, wheel.steerAngle);
        ImGui::TextDisabled("Compression: %.0f%%", wheel.suspensionCompression * 100.0f);
        if (wheel.isGrounded){
            ImGui::TextDisabled("Forward Slip: %.3f   Sideways Slip: %.3f   Force: %.0f N", wheel.hit.forwardSlip, wheel.hit.sidewaysSlip, wheel.hit.force);
            ImGui::TextDisabled("Ground: %s", wheel.hit.colliderEntity != NULL_ENTITY && scene->isEntityCreated(wheel.hit.colliderEntity)
                                ? scene->getEntityName(wheel.hit.colliderEntity).c_str() : "(none)");
        }
    }
}

void editor::InspectorWindow::drawVehicleDrivetrainComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const VehicleDrivetrainComponent& drivetrain = scene->getComponent<VehicleDrivetrainComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    settings.stepSize = 10.0f;
    settings.format = "%.0f";
    RowSettings settingsSmall;
    settingsSmall.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsCurve;
    settingsCurve.help = "The share of Max Torque (value) at a share of Max RPM (time 0..1)";
    RowSettings settingsTransmission;
    settingsTransmission.enumEntries = enumEntries(reflect::enums::kVehicleTransmissionMode);
    settingsTransmission.help = "Automatic shifts at the shift RPMs (a negative throttle reverses); Manual keeps the gear setGear chose";
    RowSettings settingsPitch = settingsSmall;
    settingsPitch.help = "The body's up stays within this of the world's up (180: off)";

    separatorText("Engine");
    beginTable(cpType, getLabelSize("Engine Inertia"), "engine");
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxTorque", "Max Torque", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "minRPM", "Min RPM", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxRPM", "Max RPM", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Curve, cpType, "torqueCurve", "Torque Curve", sceneProject, entities, settingsCurve);
    propertyRow(RowPropertyType::FloatPositive, cpType, "engineInertia", "Engine Inertia", sceneProject, entities, settingsSmall);
    propertyRow(RowPropertyType::FloatPositive, cpType, "engineDamping", "Engine Damping", sceneProject, entities, settingsSmall);
    endTable();

    separatorText("Transmission");
    beginTable(cpType, getLabelSize("Clutch Release Time"), "transmission");
    propertyRow(RowPropertyType::Enum, cpType, "transmission", "Mode", sceneProject, entities, settingsTransmission);
    propertyRow(RowPropertyType::FloatPositive, cpType, "shiftUpRPM", "Shift Up RPM", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "shiftDownRPM", "Shift Down RPM", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "switchTime", "Switch Time", sceneProject, entities, settingsSmall);
    propertyRow(RowPropertyType::FloatPositive, cpType, "clutchReleaseTime", "Clutch Release Time", sceneProject, entities, settingsSmall);
    propertyRow(RowPropertyType::FloatPositive, cpType, "switchLatency", "Switch Latency", sceneProject, entities, settingsSmall);
    propertyRow(RowPropertyType::FloatPositive, cpType, "clutchStrength", "Clutch Strength", sceneProject, entities, settingsSmall);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxPitchRollAngle", "Max Pitch Roll Angle", sceneProject, entities, settingsPitch);
    endTable();

    if (entities.size() == 1){
        const Entity entity = entities[0];
        auto addCommand = [&](Command* command){
            command->setNoMerge();
            CommandHandle::get(sceneProject->id)->addCommand(command);
        };
        const ImGuiStyle& style = ImGui::GetStyle();
        const float iconWidth = ImGui::GetFrameHeight();
        auto removeButton = [&](const char* id, const char* tooltip) -> bool {
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - iconWidth));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            const bool pressed = ImGui::Button(id, ImVec2(iconWidth, 0));
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("%s", tooltip);
            return pressed;
        };

        // the gear ratios: forward then reverse
        auto ratioList = [&](const char* key, const char* title, const std::vector<float>& ratios, float added){
            separatorText(title);
            for (size_t i = 0; i < ratios.size(); i++){
                ImGui::PushID(key);
                ImGui::PushID((int)i);
                beginTable(cpType, getLabelSize("Gear 10"), std::string(key) + std::to_string(i));
                RowSettings settingsRatio = settingsSmall;
                propertyRow(RowPropertyType::Float, cpType, std::string(key) + "[" + std::to_string(i) + "]", "Gear " + std::to_string(i + 1),
                            sceneProject, entities, settingsRatio);
                endTable();
                if (removeButton(ICON_FA_TRASH_CAN "##remove", "Remove gear") && ratios.size() > 1){
                    std::vector<float> next = ratios;
                    next.erase(next.begin() + (std::ptrdiff_t)i);
                    addCommand(new PropertyCmd<std::vector<float>>(project, sceneProject->id, entity, cpType, key, next));
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            if (ImGui::Button((std::string(ICON_FA_PLUS " Add Gear##") + key).c_str())){
                std::vector<float> next = ratios;
                next.push_back(next.empty() ? added : next.back() * (added > 0.0f ? 0.8f : 1.0f));
                addCommand(new PropertyCmd<std::vector<float>>(project, sceneProject->id, entity, cpType, key, next));
            }
        };
        ratioList("gearRatios", "Gear Ratios", drivetrain.gearRatios, 1.0f);
        ratioList("reverseGearRatios", "Reverse Gear Ratios", drivetrain.reverseGearRatios, -2.9f);

        RowSettings settingsWheel;
        settingsWheel.help = "A Wheel Collider of this vehicle";
        RowSettings settingsSplit = settingsSmall;
        settingsSplit.floatMin = 0.0f;
        settingsSplit.floatMax = 1.0f;
        settingsSplit.help = "0: all torque to the left wheel, 1: to the right";

        separatorText("Differentials");
        if (drivetrain.differentials.empty()){
            ImGui::TextDisabled("None: every axle is driven, the torque shared evenly.");
        }
        for (size_t i = 0; i < drivetrain.differentials.size(); i++){
            ImGui::PushID("differential");
            ImGui::PushID((int)i);
            const std::string prefix = "differentials[" + std::to_string(i) + "].";
            ImGui::TextDisabled("Differential %zu", i + 1);
            if (removeButton(ICON_FA_TRASH_CAN "##remove", "Remove differential")){
                std::vector<VehicleDifferential> next = drivetrain.differentials;
                next.erase(next.begin() + (std::ptrdiff_t)i);
                addCommand(new PropertyCmd<std::vector<VehicleDifferential>>(project, sceneProject->id, entity, cpType, "differentials", next));
                ImGui::PopID();
                ImGui::PopID();
                break;
            }
            beginTable(cpType, getLabelSize("Engine Torque Ratio"), "differential" + std::to_string(i));
            propertyRow(RowPropertyType::LocalEntity, cpType, prefix + "leftWheel", "Left Wheel", sceneProject, entities, settingsWheel);
            propertyRow(RowPropertyType::LocalEntity, cpType, prefix + "rightWheel", "Right Wheel", sceneProject, entities, settingsWheel);
            propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "ratio", "Ratio", sceneProject, entities, settingsSmall);
            propertyRow(RowPropertyType::Float_0_1, cpType, prefix + "leftRightSplit", "Left Right Split", sceneProject, entities, settingsSplit);
            propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "limitedSlipRatio", "Limited Slip Ratio", sceneProject, entities, settingsSmall);
            propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "engineTorqueRatio", "Engine Torque Ratio", sceneProject, entities, settingsSmall);
            endTable();
            ImGui::PopID();
            ImGui::PopID();
        }
        if (ImGui::Button(ICON_FA_PLUS " Add Differential")){
            std::vector<VehicleDifferential> next = drivetrain.differentials;
            next.push_back(VehicleDifferential());
            addCommand(new PropertyCmd<std::vector<VehicleDifferential>>(project, sceneProject->id, entity, cpType, "differentials", next));
        }
        beginTable(cpType, getLabelSize("Limited Slip Ratio"), "differentialsSlip");
        propertyRow(RowPropertyType::FloatPositive, cpType, "differentialLimitedSlipRatio", "Limited Slip Ratio", sceneProject, entities, settingsSmall);
        endTable();

        separatorText("Anti-Roll Bars");
        for (size_t i = 0; i < drivetrain.antiRollBars.size(); i++){
            ImGui::PushID("antiRollBar");
            ImGui::PushID((int)i);
            const std::string prefix = "antiRollBars[" + std::to_string(i) + "].";
            ImGui::TextDisabled("Anti-Roll Bar %zu", i + 1);
            if (removeButton(ICON_FA_TRASH_CAN "##remove", "Remove anti-roll bar")){
                std::vector<VehicleAntiRollBar> next = drivetrain.antiRollBars;
                next.erase(next.begin() + (std::ptrdiff_t)i);
                addCommand(new PropertyCmd<std::vector<VehicleAntiRollBar>>(project, sceneProject->id, entity, cpType, "antiRollBars", next));
                ImGui::PopID();
                ImGui::PopID();
                break;
            }
            beginTable(cpType, getLabelSize("Right Wheel"), "antiRollBar" + std::to_string(i));
            propertyRow(RowPropertyType::LocalEntity, cpType, prefix + "leftWheel", "Left Wheel", sceneProject, entities, settingsWheel);
            propertyRow(RowPropertyType::LocalEntity, cpType, prefix + "rightWheel", "Right Wheel", sceneProject, entities, settingsWheel);
            propertyRow(RowPropertyType::FloatPositive, cpType, prefix + "stiffness", "Stiffness", sceneProject, entities, settings);
            endTable();
            ImGui::PopID();
            ImGui::PopID();
        }
        if (ImGui::Button(ICON_FA_PLUS " Add Anti-Roll Bar")){
            std::vector<VehicleAntiRollBar> next = drivetrain.antiRollBars;
            next.push_back(VehicleAntiRollBar());
            addCommand(new PropertyCmd<std::vector<VehicleAntiRollBar>>(project, sceneProject->id, entity, cpType, "antiRollBars", next));
        }
    }

    if (!scene->findComponent<RigidbodyComponent>(entities[0])){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("A Vehicle Drivetrain goes on the vehicle's body: the GameObject with the dynamic Rigidbody its Wheel Colliders are under.");
    }

    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1){
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Gear: %d   RPM: %.0f   Speed: %.1f m/s", drivetrain.gear, drivetrain.rpm, drivetrain.speed);
        ImGui::TextDisabled("Throttle: %.2f   Steer: %.2f   Brake: %.2f   Hand Brake: %.2f", drivetrain.throttle, drivetrain.steer,
                            drivetrain.brake, drivetrain.handBrake);
    }
}
