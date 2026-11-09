// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector shadow settings (docs/rendering/shadows.md), in Unity's order: the light's Shadows
// section (Shadow Type, Strength, Resolution, Bias, Normal Bias, Near Plane, Update Mode, then
// the directional light's distance, cascades and split bar or a local light's source radius,
// then Contact Shadows) and the scene's Shadows settings. Every row is an undo step (dragging merges into one); a
// row that cannot apply on this platform's tier says why.

#include "InspectorWindow.h"

#include "Catalog.h"
#include "reflect/Enums.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "component/LightComponent.h"
#include "external/IconsFontAwesome6.h"
#include "render/shadow/CascadeFit.h"
#include "render/shadow/ShadowTiers.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace jasperengine;

namespace {

    // the dropdowns of the light's enums: their reflection tables (editor/reflect/Enums.cpp)
    std::vector<editor::EnumEntry>& entriesShadowUpdateMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kShadowUpdateMode);
    std::vector<editor::EnumEntry>& entriesCascadeSplitMode = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kCascadeSplitMode);

    const char* const shadowTypeNames[] = { "No Shadows", "Hard Shadows", "Soft Shadows" };
    const unsigned int resolutionValues[] = { 0, 256, 512, 1024, 2048, 4096 };

    std::string tierLabel(ShadowTier tier){
        std::string name = ShadowTiers::getName(tier);
        if (!name.empty())
            name[0] = (char)std::toupper((unsigned char)name[0]);
        return name;
    }

    // the cascade colours of the Cascades debug view (includes/lights.glsl shadowDebugColor)
    const ImU32 cascadeColors[4] = {
        IM_COL32(222, 64, 64, 255), IM_COL32(64, 200, 64, 255), IM_COL32(77, 115, 230, 255), IM_COL32(220, 210, 60, 255)
    };
}

void editor::InspectorWindow::drawLightShadows(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    LightComponent& light = scene->getComponent<LightComponent>(entities[0]);
    const ShadowTier tier = scene->getEffectiveShadowTier();
    const ShadowTierSettings& tierSettings = ShadowTiers::getSettings(tier);
    const bool hardOnly = scene->getShadowQuality() == ShadowQuality::NONE;

    separatorText("Shadows");
    beginTable(cpType, getLabelSize("Angular Diameter"), "shadow_settings_table");

    // Shadow Type: shadows and softShadows together, one undo step
    {
        const ShadowType current = light.getShadowType();
        bool mixed = false;
        for (Entity entity : entities){
            if (LightComponent* other = scene->findComponent<LightComponent>(entity))
                mixed = mixed || other->getShadowType() != current;
        }
        propertyHeader("Shadow Type");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##shadow_type", mixed ? "—" : shadowTypeNames[(int)current])){
            for (int t = 0; t < 3; t++){
                if (ImGui::Selectable(shadowTypeNames[t], !mixed && (int)current == t)){
                    auto* multi = new MultiPropertyCmd();
                    for (Entity entity : entities){
                        multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "shadows", t != (int)ShadowType::NONE);
                        if (t != (int)ShadowType::NONE)
                            multi->addPropertyCmd<bool>(project, sceneProject->id, entity, cpType, "softShadows", t == (int)ShadowType::SOFT);
                    }
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(multi);
                }
            }
            ImGui::EndCombo();
        }
    }

    if (!light.shadows){
        endTable();
        return;
    }

    RowSettings settingsStrength;
    settingsStrength.help = "How dark the shadows are: 0 casts none, 1 full shadow.";
    propertyRow(RowPropertyType::Float_0_1, cpType, "shadowStrength", "Strength", sceneProject, entities, settingsStrength);

    // Resolution: the tier's, or a fixed size
    {
        const unsigned int current = light.mapResolution;
        auto label = [&](unsigned int value) -> std::string{
            if (value == 0){
                const unsigned int tierValue = (light.type == LightType::DIRECTIONAL) ? tierSettings.slotResolution :
                    (light.type == LightType::SPOT ? tierSettings.spotResolution : tierSettings.pointResolution);
                return "Use Quality Settings (" + std::to_string(tierValue) + ")";
            }
            return std::to_string(value);
        };
        propertyHeader("Resolution");
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::BeginCombo("##shadow_resolution", label(current).c_str())){
            for (unsigned int value : resolutionValues){
                const bool overTier = value > tierSettings.slotResolution;
                const std::string text = label(value) + (overTier ? " (tier " + tierLabel(tier) + " caps it at " + std::to_string(tierSettings.slotResolution) + ")" : "");
                if (ImGui::Selectable(text.c_str(), value == current)){
                    auto* multi = new MultiPropertyCmd();
                    for (Entity entity : entities)
                        multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, "mapResolution", value);
                    CommandHandle::get(sceneProject->id)->addCommandNoMerge(multi);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        helpMarker("Texels per side of each of the light's shadow maps (a cube face of a point light). Use Quality Settings takes the "
                   "quality tier's; the tier caps every size, and the shadow memory budget may lower the least important lights' maps.");
    }

    RowSettings settingsBias;
    settingsBias.stepSize = 0.05f;
    settingsBias.format = "%.2f";
    settingsBias.help = "Depth offset towards the light, in shadow texels (whatever the resolution or cascade). Raise it against "
                        "shadow acne; too much detaches shadows from their casters.";
    propertyRow(RowPropertyType::FloatPositive, cpType, "shadowBias", "Bias", sceneProject, entities, settingsBias);
    RowSettings settingsNormalBias = settingsBias;
    settingsNormalBias.help = "Offset of the receiver along its normal, in shadow texels, larger the more it faces away from the light. "
                              "Fixes acne on slopes without detaching contact shadows.";
    propertyRow(RowPropertyType::FloatPositive, cpType, "shadowNormalBias", "Normal Bias", sceneProject, entities, settingsNormalBias);

    if (light.type != LightType::DIRECTIONAL){
        RowSettings settingsAuto;
        settingsAuto.help = "The shadow camera's near plane is the scene camera's and its far plane the light's range. "
                            "Off: set them yourself.";
        propertyRow(RowPropertyType::Bool, cpType, "automaticShadowCamera", "Auto Near Plane", sceneProject, entities, settingsAuto);
        if (!light.automaticShadowCamera){
            RowSettings settingsPlane;
            settingsPlane.stepSize = 0.01f;
            propertyRow(RowPropertyType::FloatPositive, cpType, "shadowCameraNear", "Near Plane", sceneProject, entities, settingsPlane);
            propertyRow(RowPropertyType::FloatPositive, cpType, "shadowCameraFar", "Far Plane", sceneProject, entities, settingsPlane);
        }
    }

    RowSettings settingsUpdate;
    settingsUpdate.enumEntries = &entriesShadowUpdateMode;
    settingsUpdate.help = "Auto: the maps are cached and redrawn only when this light, the casters they hold or their fit changes. "
                          "Every Frame: redrawn always. On Demand: drawn when the light is enabled or changes, and on "
                          "requestShadowUpdate() (Lua) or Request Update; moving casters wait for that.";
    propertyRow(RowPropertyType::Enum, cpType, "shadowUpdateMode", "Update Mode", sceneProject, entities, settingsUpdate);
    if (light.shadowUpdateMode == ShadowUpdateMode::ON_DEMAND){
        propertyHeader("");
        if (ImGui::Button(ICON_FA_ROTATE " Request Update")){
            for (Entity entity : entities){
                if (LightComponent* other = scene->findComponent<LightComponent>(entity))
                    other->shadowUpdateRequested = true;
            }
        }
    }

    const bool pcssTier = tier == ShadowTier::ULTRA && !hardOnly;
    const std::string pcssNote = hardOnly ? "Quality is Hard Only: every light's shadows are hard."
        : ("Contact hardening (PCSS) needs the Ultra quality tier; this platform uses " + tierLabel(tier) + ".");

    if (light.type == LightType::DIRECTIONAL){
        RowSettings settingsDistance;
        settingsDistance.stepSize = 1.0f;
        settingsDistance.format = "%.1f";
        const float sceneDistance = scene->getShadowDistance();
        settingsDistance.help = "Metres from the camera directional shadows reach (fading out over the scene's Shadow Fade). 0 takes the "
                                "scene's Shadow Distance, else the quality tier's (" +
                                std::to_string((int)(sceneDistance > 0.0f ? sceneDistance : tierSettings.shadowDistance)) + " m now).";
        propertyRow(RowPropertyType::FloatPositive, cpType, "shadowDistance", "Shadow Distance", sceneProject, entities, settingsDistance);

        // Cascade Count: the tier's, or 1 to 4 (capped by the tier)
        {
            const unsigned int current = light.numShadowCascades;
            const std::string qualityLabel = "Use Quality Settings (" + std::to_string(tierSettings.maxCascades) + ")";
            propertyHeader("Cascade Count");
            ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x));
            const std::string preview = (current == 0) ? qualityLabel : std::to_string(current);
            if (ImGui::BeginCombo("##shadow_cascades", preview.c_str())){
                for (unsigned int value = 0; value <= MAX_SHADOWCASCADES; value++){
                    std::string text = (value == 0) ? qualityLabel : std::to_string(value);
                    if (value > tierSettings.maxCascades)
                        text += " (tier " + tierLabel(tier) + " draws " + std::to_string(tierSettings.maxCascades) + ")";
                    if (ImGui::Selectable(text.c_str(), value == current)){
                        auto* multi = new MultiPropertyCmd();
                        for (Entity entity : entities)
                            multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, "numShadowCascades", value);
                        CommandHandle::get(sceneProject->id)->addCommandNoMerge(multi);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            helpMarker("How many shadow maps share the shadow distance, finer near the camera. An orthographic camera draws one.");
        }

        RowSettings settingsSplit;
        settingsSplit.enumEntries = &entriesCascadeSplitMode;
        settingsSplit.help = "Auto spreads the cascades between even and logarithmic. Manual takes the split bar below (drag its handles).";
        propertyRow(RowPropertyType::Enum, cpType, "cascadeSplitMode", "Split Mode", sceneProject, entities, settingsSplit);

        drawShadowSplitBar(sceneProject, entities, light);

        RowSettings settingsAngle;
        settingsAngle.stepSize = 0.01f;
        settingsAngle.format = "%.2f";
        settingsAngle.help = "Degrees of the light's disc (the sun: 0.53). Widens soft shadows with distance from their caster; with PCSS "
                             "(Ultra) they harden at contact. " + (pcssTier ? std::string() : pcssNote);
        propertyRow(RowPropertyType::FloatPositive, cpType, "shadowAngularDiameter", "Angular Diameter", sceneProject, entities, settingsAngle);
    }else{
        RowSettings settingsRadius;
        settingsRadius.stepSize = 0.01f;
        settingsRadius.format = "%.2f";
        settingsRadius.help = "Metres of the light's emitting sphere: how soft its shadows get away from their caster. " +
            std::string(light.type == LightType::POINT ? "Point lights filter with the Vogel disk at High and Ultra (no PCSS across cube faces)."
                                                       : (pcssTier ? "" : pcssNote));
        propertyRow(RowPropertyType::FloatPositive, cpType, "shadowSourceRadius", "Source Radius", sceneProject, entities, settingsRadius);
    }

    if (light.softShadows && (hardOnly || (!pcssTier && light.type != LightType::POINT))){
        propertyHeader("");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", pcssNote.c_str());
        ImGui::PopStyleColor();
    }

    // contact shadows (S57 R3): the screen-space march towards a directional or spot light
    if (light.type != LightType::POINT){
        RowSettings settingsContact;
        settingsContact.help = "Screen-space shadows of the small details the shadow map misses (creases, feet on the ground). Costs a "
                               "fullscreen pass after the depth pre-pass (which it turns on) marching 8-16 depth samples a pixel; full "
                               "resolution on High, half on Medium, off on Low.";
        propertyRow(RowPropertyType::Bool, cpType, "contactShadows", "Contact Shadows", sceneProject, entities, settingsContact);
        if (light.contactShadows){
            RowSettings settingsLength;
            settingsLength.indent = true;
            settingsLength.stepSize = 0.01f;
            settingsLength.help = "Metres the march reaches towards the light: longer finds larger occluders, and misses more thin ones.";
            propertyRow(RowPropertyType::FloatPositive, cpType, "contactShadowLength", "Length", sceneProject, entities, settingsLength);
            RowSettings settingsThickness = settingsLength;
            settingsThickness.help = "Metres a surface in the depth buffer is taken to be thick: thicker shadows more behind thin objects, "
                                     "and may shadow through gaps.";
            propertyRow(RowPropertyType::FloatPositive, cpType, "contactShadowThickness", "Thickness", sceneProject, entities, settingsThickness);
            RowSettings settingsSamples;
            settingsSamples.indent = true;
            settingsSamples.intMin = 8;
            settingsSamples.intMax = 16;
            settingsSamples.help = "Depth samples each pixel marches (8-16): more are smoother and cost more.";
            propertyRow(RowPropertyType::Int, cpType, "contactShadowSampleCount", "Sample Count", sceneProject, entities, settingsSamples);
        }
    }

    endTable();
}

// The cascades as coloured segments of the shadow distance, with their far edges in metres;
// under Manual the handles between them drag the splits (one undo step per drag)
void editor::InspectorWindow::drawShadowSplitBar(SceneProject* sceneProject, std::vector<Entity> entities, const LightComponent& light){
    Scene* scene = sceneProject->scene;
    const ShadowTierSettings& tierSettings = ShadowTiers::getSettings(scene->getEffectiveShadowTier());
    const unsigned int cascades = std::clamp(light.numShadowCascades == 0 ? tierSettings.maxCascades
                                             : std::min(light.numShadowCascades, tierSettings.maxCascades), 1u, (unsigned int)MAX_SHADOWCASCADES);
    float distance = light.shadowDistance;
    if (!(distance > 0.0f))
        distance = scene->getShadowDistance();
    if (!(distance > 0.0f))
        distance = tierSettings.shadowDistance;

    // the splits as the fit computes them, from a 0.1 m near plane
    CascadeFitInput input;
    input.nearClip = 0.1f;
    input.shadowDistance = distance;
    input.cascades = cascades;
    input.mode = light.cascadeSplitMode;
    input.splits[0] = light.cascadeSplits.x;
    input.splits[1] = light.cascadeSplits.y;
    input.splits[2] = light.cascadeSplits.z;
    float splitFar[CASCADE_FIT_MAX];
    CascadeFit::computeSplits(input, splitFar);

    propertyHeader("Cascade Splits");
    const float width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    float start = 0.0f;
    for (unsigned int c = 0; c < cascades; c++){
        const float end = splitFar[c] / distance;
        const ImVec2 a(origin.x + start * width, origin.y);
        const ImVec2 b(origin.x + end * width, origin.y + height);
        drawList->AddRectFilled(a, b, cascadeColors[c]);
        const std::string text = std::to_string((int)std::round(splitFar[c])) + " m";
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        if (size.x + 4.0f < b.x - a.x)
            drawList->AddText(ImVec2(b.x - size.x - 3.0f, a.y + (height - size.y) * 0.5f), IM_COL32(15, 15, 15, 255), text.c_str());
        start = end;
    }
    drawList->AddRect(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_Border));

    if (light.cascadeSplitMode == CascadeSplitMode::MANUAL){
        for (unsigned int c = 0; c + 1 < cascades; c++){
            const float x = origin.x + (splitFar[c] / distance) * width;
            ImGui::SetCursorScreenPos(ImVec2(x - 4.0f, origin.y));
            ImGui::PushID((int)c);
            ImGui::InvisibleButton("##split_handle", ImVec2(8.0f, height));
            const bool active = ImGui::IsItemActive();
            if (ImGui::IsItemHovered() || active){
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(255, 255, 255, 255), 2.0f);
            }
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)){
                // between its neighbours, a hair apart
                const float lower = (c == 0) ? 0.005f : light.cascadeSplits[c - 1] + 0.005f;
                const float upper = (c + 2 < cascades) ? light.cascadeSplits[c + 1] - 0.005f : 0.995f;
                const float fraction = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width, lower, std::max(lower, upper));
                Vector3 splits = light.cascadeSplits;
                splits[c] = fraction;
                if (splits != light.cascadeSplits){
                    auto* multi = new MultiPropertyCmd();
                    for (Entity entity : entities)
                        multi->addPropertyCmd<Vector3>(project, sceneProject->id, entity, ComponentType::LightComponent, "cascadeSplits", splits);
                    CommandHandle::get(sceneProject->id)->addCommand(multi);
                    splitBarCmd = multi;
                }
            }
            if (ImGui::IsItemDeactivated() && splitBarCmd){
                splitBarCmd->setNoMerge();
                splitBarCmd = nullptr;
            }
            ImGui::PopID();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

void editor::InspectorWindow::drawSceneShadows(SceneProject* sceneProject){
    Scene* scene = sceneProject->scene;
    const float labelWidth = std::max(getLabelSize("Effective Tier"), getLabelSize("Capsule Intensity")) + ImGui::GetStyle().CellPadding.x * 2.0f;
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("scene_shadow_settings_table", 2, tableFlags))
        return;
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    const float comboWidth = ImGui::CalcTextSize("Hard Only").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    drawScenePropertyRow<ShadowQuality>(sceneProject, "shadows_quality", "Quality", ScenePropertyInputType::Combo, comboWidth, 0.0f, 1.0f,
        "The shadow quality tier (docs/rendering/shadows.md):\n"
        "- Auto: the project's tier of the platform (Project Settings > Graphics > Shadows)\n"
        "- Hard Only: that tier with every light's shadows hard\n"
        "- Low: 2 cascades, 3x3 filter, dithered cascade blend (mobile)\n"
        "- Medium: 3 cascades, 5x5 filter, receiver-plane bias (web)\n"
        "- High: 4 cascades, 16-tap Vogel disk (desktop)\n"
        "- Ultra: 4096 maps, PCSS contact hardening (opt-in)");
    drawScenePropertyRow<float>(sceneProject, "shadow_distance", "Shadow Distance", ScenePropertyInputType::DragFloat, -1.0f, 0.0f, 1.0f,
        "Metres from the camera directional shadows reach; 0 takes the quality tier's. A light's own Shadow Distance overrides it.");
    drawScenePropertyRow<float>(sceneProject, "shadow_fade_fraction", "Shadow Fade", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 1.0f,
        "The last fraction of the shadow distance over which directional shadows fade to lit, so they end without a line.");
    drawScenePropertyRow<bool>(sceneProject, "shadow_cascade_blend", "Cascade Blend", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
        "Blend each shadow cascade into the next over a band before its split (Low dithers instead), so the change of detail shows no seam.");
    drawScenePropertyRow<bool>(sceneProject, "capsule_shadows_enabled", "Capsule Shadows", ScenePropertyInputType::Checkbox, -1.0f, 0.0f, 1.0f,
        "Soft shadows of characters from the main directional light, cast by the capsules of the 4 Capsule Shadow Casters nearest the "
        "camera, without a shadow map. Costs per lit pixel, up to 32 capsule tests; Capsules Only casters keep the static shadow cache valid.");
    // the two below mean nothing while it is off
    ImGui::BeginDisabled(!scene->isCapsuleShadowsEnabled());
    drawScenePropertyRow<float>(sceneProject, "capsule_shadow_intensity", "Capsule Intensity", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 1.0f,
        "How dark capsule shadows are: 0 casts none, 1 full shadow.");
    drawScenePropertyRow<float>(sceneProject, "capsule_shadow_softness", "Capsule Softness", ScenePropertyInputType::SliderFloat, -1.0f, 0.0f, 2.0f,
        "How fast a capsule shadow's penumbra widens per metre along the light: 0 is hard, higher is softer further from the character.");
    ImGui::EndDisabled();

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Effective Tier");
    ImGui::TableSetColumnIndex(1);
    const ShadowTier tier = scene->getEffectiveShadowTier();
    std::string tierText = tierLabel(tier) + " on " + std::string(Project::getShadowTierPlatformName(static_cast<int>(Engine::getPlatform())));
    if (scene->getShadowQuality() == ShadowQuality::NONE)
        tierText += ", hard only";
    ImGui::TextDisabled("%s", tierText.c_str());

    ImGui::EndTable();
}
