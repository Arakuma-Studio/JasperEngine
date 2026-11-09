// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Title Art Renderer's Inspector (docs/specs/S81-title-art-runtime.md R60-R62, §6.4): the
// asset with Open in Title Designer, Render Mode and what it costs, the Exposed parameters (a
// checkbox overrides one, as particle exposed values do; the document's value shows greyed while
// it is not overridden), the Live Effects, the Realtime and Glyph Atlas settings, Characters, the
// Status line, and Set Native Size, Add Particles and Re-render. Every row goes through
// propertyRow (prefab marks, multi-edit, "Set <Label> (<GameObject>)" undo); rows a mode does not
// use are disabled with a tooltip, never hidden, so the layout does not jump.

#include "InspectorWindow.h"
#include "InspectorWindowTitleArt.h"

#include "Project.h"
#include "Theme.h"
#include "animation/PreviewOwner.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/AudioSourceComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/TitleArtSystem.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/runtime/TitleArtOverrides.h"
#include "titleart/runtime/TitleParticlePresets.h"
#include "util/Color.h"
#include "util/TitleArtBakeInfo.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::string mib(size_t bytes){
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MiB", bytes / (1024.0 * 1024.0));
        return text;
    }

    // R61: one line on what the mode costs
    std::string costLine(const TitleArtRendererComponent& title, const titleart::TitleArtInstance& instance){
        if (title.renderMode == TitleArtRenderMode::BAKED){
            const int w = instance.bake.frame.width, h = instance.bake.frame.height;
            return "Baked: fixed image" + (w > 0 ? ", " + mib((size_t)w * h * 4) : std::string()) + ", no engine code";
        }
        if (title.renderMode == TitleArtRenderMode::REALTIME){
            return "Realtime: re-renders on change (time sliced), adds the titleart module (~0.5 MiB)";
        }
        const size_t page = (size_t)title.atlasPageSize * title.atlasPageSize * 4;
        return "Glyph Atlas: " + std::to_string(std::max<uint32_t>(1, instance.liveStatus.atlasPages)) + " atlas page(s) (" + mib(page) +
               " each) + plates";
    }

    // the override value shown in the parameter row: the entry's, else the document's
    TitleArtOverride shownValue(const TitleArtRendererComponent& title, const titleart::TitleArtExposedParam& param, bool& overridden){
        const TitleArtOverride* entry = findTitleArtOverride(title, param.name);
        overridden = entry && entry->type == param.type;
        if (overridden) return *entry;
        TitleArtOverride value = param.defaultValue;
        value.name = param.name;
        value.type = param.type;
        return value;
    }

}

void editor::InspectorWindow::drawTitleArtRendererComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const Entity entity = entities.front();
    TitleArtRendererComponent* title = scene->findComponent<TitleArtRendererComponent>(entity);
    if (!title) return;
    titleart::TitleArtInstance& instance = TitleArtSystem::instanceOf(*title);
    TitleArtSystem::parameterNames(scene, entity);
    const bool single = entities.size() == 1;
    const bool baked = title->renderMode == TitleArtRenderMode::BAKED;

    beginTable(cpType, getLabelSize("Prebake Initial State"));
    RowSettings asset;
    asset.help = "The .titleart this GameObject shows (Assets > Create > Title Art; the Title Designer edits it)";
    propertyRow(RowPropertyType::Asset, cpType, "title", "Title Art", sceneProject, entities, asset);

    static std::vector<EnumEntry> modes = {
        {(int)TitleArtRenderMode::BAKED, "Baked"},
        {(int)TitleArtRenderMode::REALTIME, "Realtime"},
        {(int)TitleArtRenderMode::GLYPH_ATLAS, "Glyph Atlas"}};
    RowSettings mode;
    mode.enumEntries = &modes;
    mode.help = "Baked shows the bake (no engine code); Realtime renders the title again when its text or a parameter changes; "
                "Glyph Atlas renders each glyph once and lays out changing text every frame (counters, scores, timers)";
    propertyRow(RowPropertyType::Enum, cpType, "renderMode", "Render Mode", sceneProject, entities, mode);
    // S85 R49: the animation rows (the names are the document's; an unknown name warns once at Play)
    static std::vector<std::string> animationChoices;
    animationChoices = single ? TitleArtSystem::animationNames(scene, entity) : std::vector<std::string>();
    RowSettings animationName;
    animationName.help = "An animation of the title by name (the Title Designer's Animation panel lists them); empty for none";
    animationName.stringSuggestions = &animationChoices;
    propertyRow(RowPropertyType::Bool, cpType, "playAutomatically", "Play Automatically", sceneProject, entities);
    propertyRow(RowPropertyType::String, cpType, "defaultAnimation", "Default Animation", sceneProject, entities, animationName);
    propertyRow(RowPropertyType::Float, cpType, "animationSpeed", "Animation Speed", sceneProject, entities);
    RowSettings unscaled;
    unscaled.help = "Off: the animation follows the scene's time scale; On: unscaled time";
    propertyRow(RowPropertyType::Bool, cpType, "animationUnscaledTime", "Unscaled Time", sceneProject, entities, unscaled);
    RowSettings drive;
    drive.help = "Name of an animation that follows Animation Time instead of the clock (empty: None)";
    drive.stringSuggestions = &animationChoices;
    propertyRow(RowPropertyType::String, cpType, "driveAnimation", "Drive Animation", sceneProject, entities, drive);
    endTable();

    if (!title->title.path.empty() && InspectorWindowTitleArt::canOpenInDesigner()){
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE " Open in Title Designer")) {
            InspectorWindowTitleArt::openInDesigner(project->getProjectPath() / fs::path(title->title.path));
        }
    }
    // S85 R49: the default animation in the Scene and Game views, restored on Stop
    if (single && sceneProject->playState == ScenePlayState::STOPPED && !animationChoices.empty()){
        static PreviewOwner::Token previewToken = 0;
        const bool previewing = TitleArtSystem::isAnimationEditPreviewing(scene, entity);
        if (ImGui::Button(previewing ? ICON_FA_STOP " Stop Preview" : ICON_FA_PLAY " Preview")){
            if (previewing){
                PreviewOwner::release(previewToken);
                previewToken = 0;
                TitleArtSystem::stopAnimationEditPreview(scene, entity);
            }else if (TitleArtSystem::startAnimationEditPreview(scene, entity)){
                previewToken = PreviewOwner::acquire(sceneProject->id, entity, scene, "Title Art preview", [scene, entity](){
                    previewToken = 0;
                    TitleArtSystem::stopAnimationEditPreview(scene, entity);
                });
            }
        }
    }
    // R49: where a marker's sound plays
    if (single && TitleArtSystem::animationHasMarkerSounds(scene, entity) && !(scene->isComponentRegistered<AudioSourceComponent>() && scene->findComponent<AudioSourceComponent>(entity))){
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Marker sounds play 2D on Master without an Audio Source");
        if (ImGui::SmallButton("Add Audio Source"))
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(new AddComponentCmd(project, sceneProject->id, entity, ComponentType::AudioSourceComponent));
    }
    if (single) ImGui::TextColored(Theme::Colors::SubtleText, "%s", costLine(*title, instance).c_str());
    if (single && TitleArtSystem::hasBothCompanions(scene, entity)){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Both an Image and a Sprite: the Image shows the title");
    }
    if (single && TitleArtSystem::spaceOf(scene, entity) == titleart::TitleArtSpace::None){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Add an Image (UI) or a Sprite (world): the title draws through it");
    }
    if (single && !title->title.path.empty() && !instance.hasBake){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s has no bake", fs::path(title->title.path).filename().string().c_str());
        ImGui::SameLine();
        const fs::path document = project->getProjectPath() / fs::path(title->title.path);
        ImGui::BeginDisabled(TitleArtAutoBake::get().baking(document));
        if (ImGui::SmallButton(ICON_FA_FIRE " Bake Now")) TitleArtAutoBake::get().requestBake(project, document, {}, false);
        ImGui::EndDisabled();
    }
    if (single && !instance.liveStatus.glyphFallback.empty()){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Glyph Atlas falls back to Realtime: %s",
                           instance.liveStatus.glyphFallback.c_str());
    }

    // ---- Exposed parameters (R4, R7): a checkbox overrides one ----
    if (single && !instance.exposed.empty()){
        const std::string header = "Parameters (" + std::to_string(instance.exposed.size()) + ")";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)){
            if (baked) ImGui::TextColored(Theme::Colors::SubtleText, "Baked: the image is the asset's bake. Realtime applies overrides.");
            ImGui::BeginDisabled(baked);
            for (const titleart::TitleArtExposedParam& param : instance.exposed){
                bool overridden = false;
                TitleArtOverride value = shownValue(*title, param, overridden);
                const std::string id = "##ta_param_" + param.name;
                const std::string label = param.label.empty() ? param.name : param.label;
                bool checked = overridden;
                if (ImGui::Checkbox((id + "_on").c_str(), &checked)){
                    // one step: the entry made from the document's value, or removed
                    std::vector<TitleArtOverride> next = title->overrides;
                    if (checked){
                        next.push_back(value);
                    }else{
                        next.erase(std::remove_if(next.begin(), next.end(), [&](const TitleArtOverride& o){ return o.name == param.name; }), next.end());
                    }
                    cmd = new PropertyCmd<std::vector<TitleArtOverride>>(project, sceneProject->id, entity, cpType, "overrides", next);
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                }
                ImGui::SetItemTooltip("Override %s on this instance (a prefab variant may override it alone)", label.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(!overridden);
                ImGui::SetNextItemWidth(-1.0f);
                const std::string property = std::string(TITLE_ART_OVERRIDES_PREFIX) + param.name;
                if (param.type == "number"){
                    float v = value.value.x;
                    const float lo = param.hasMin ? (float)param.min : 0.0f, hi = param.hasMax ? (float)param.max : 0.0f;
                    const bool changed = param.hasMin && param.hasMax ? ImGui::SliderFloat((label + id).c_str(), &v, lo, hi)
                                                                      : ImGui::DragFloat((label + id).c_str(), &v, 0.01f);
                    if (changed && overridden){
                        cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, property, v);
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && cmd) cmd->setNoMerge();
                }else if (param.type == "bool"){
                    bool v = value.value.x != 0.0f;
                    if (ImGui::Checkbox((label + id).c_str(), &v) && overridden){
                        cmd = new PropertyCmd<float>(project, sceneProject->id, entity, cpType, property, v ? 1.0f : 0.0f);
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                    }
                }else if (param.type == "color"){
                    // stored linear, shown sRGB (as ui.color)
                    Vector4 srgb = Color::linearTosRGB(value.value);
                    float rgb[3] = {srgb.x, srgb.y, srgb.z};
                    if (ImGui::ColorEdit3((label + id).c_str(), rgb) && overridden){
                        const Vector4 linear = Color::sRGBToLinear(Vector4(rgb[0], rgb[1], rgb[2], 1.0f));
                        cmd = new PropertyCmd<Vector4>(project, sceneProject->id, entity, cpType, property, linear);
                        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && cmd) cmd->setNoMerge();
                }else if (param.type == "color_scheme" && !param.options.empty()){
                    if (ImGui::BeginCombo((label + id).c_str(), value.text.c_str())){
                        for (const std::string& scheme : param.options){
                            if (ImGui::Selectable(scheme.c_str(), scheme == value.text) && overridden){
                                cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, property, scheme);
                                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }else{
                    char buffer[1024];
                    std::snprintf(buffer, sizeof(buffer), "%s", value.text.c_str());
                    if (ImGui::InputText((label + id).c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue) && overridden){
                        cmd = new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, property, std::string(buffer));
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
            // R4: entries the document no longer exposes (or of another type) are kept and named
            for (const TitleArtOverride& entry : title->overrides){
                const titleart::TitleArtExposedParam* param = nullptr;
                for (const titleart::TitleArtExposedParam& p : instance.exposed) if (p.name == entry.name) param = &p;
                if (param && param->type == entry.type) continue;
                if (param) ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Parameter '%s' is a %s, the override a %s",
                                              entry.name.c_str(), param->type.c_str(), entry.type.c_str());
                else ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Missing parameter '%s'", entry.name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(("Remove##ta_missing_" + entry.name).c_str())){
                    std::vector<TitleArtOverride> next = title->overrides;
                    next.erase(std::remove_if(next.begin(), next.end(), [&](const TitleArtOverride& o){ return o.name == entry.name; }), next.end());
                    cmd = new PropertyCmd<std::vector<TitleArtOverride>>(project, sceneProject->id, entity, cpType, "overrides", next);
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(cmd);
                    break;
                }
            }
            ImGui::TreePop();
        }
    }

    // ---- Live Effects (R33-R38) ----
    if (ImGui::TreeNodeEx("Live Effects", ImGuiTreeNodeFlags_None)){
        beginTable(cpType, getLabelSize("Sweep Softness"), "live");
        RowSettings hdr;
        hdr.colorHdr = true;
        propertyRow(RowPropertyType::Bool, cpType, "sweepEnabled", "Sweep", sceneProject, entities);
        propertyRow(RowPropertyType::Float_0_1, cpType, "sweepPosition", "Position", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "sweepWidth", "Width", sceneProject, entities);
        propertyRow(RowPropertyType::Float, cpType, "sweepAngle", "Angle", sceneProject, entities);
        propertyRow(RowPropertyType::Float_0_1, cpType, "sweepSoftness", "Softness", sceneProject, entities);
        propertyRow(RowPropertyType::Color4L, cpType, "sweepColor", "Color", sceneProject, entities, hdr);
        propertyRow(RowPropertyType::Bool, cpType, "sweepAuto", "Auto", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "sweepSpeed", "Speed", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "sweepInterval", "Interval", sceneProject, entities);
        const bool glowAvailable = title->renderMode != TitleArtRenderMode::BAKED || !instance.glowPath.empty();
        if (!glowAvailable) ImGui::BeginDisabled(true);
        propertyRow(RowPropertyType::Bool, cpType, "glowEnabled", "Glow", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "glowPulse", "Pulse", sceneProject, entities);
        propertyRow(RowPropertyType::Color4L, cpType, "glowColor", "Color", sceneProject, entities, hdr);
        if (!glowAvailable) ImGui::EndDisabled();
        propertyRow(RowPropertyType::Bool, cpType, "tintEnabled", "Tint", sceneProject, entities);
        propertyRow(RowPropertyType::Color4L, cpType, "tintColor", "Color", sceneProject, entities);
        propertyRow(RowPropertyType::Float_0_1, cpType, "tintAmount", "Amount", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "glintsEnabled", "Glints", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "glintAmount", "Amount", sceneProject, entities);
        propertyRow(RowPropertyType::Float_0_1, cpType, "glintSize", "Size", sceneProject, entities);
        propertyRow(RowPropertyType::FloatPositive, cpType, "glintRate", "Rate", sceneProject, entities);
        propertyRow(RowPropertyType::UInt, cpType, "glintSeed", "Seed", sceneProject, entities);
        propertyRow(RowPropertyType::Color4L, cpType, "glintColor", "Color", sceneProject, entities, hdr);
        propertyRow(RowPropertyType::FloatPositive, cpType, "liveTime", "Live Time", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "liveUnscaledTime", "Unscaled Time", sceneProject, entities);
        endTable();
        if (single && !glowAvailable) ImGui::TextColored(Theme::Colors::SubtleText, "Enable a Glow output in the Title Designer (Outputs) to pulse the glow.");
        if (single && title->glintsEnabled && instance.anchors.empty())
            ImGui::TextColored(Theme::Colors::SubtleText, "No highlight anchors yet: bake the title (its sidecar lists them).");
        ImGui::TreePop();
    }

    // ---- Realtime / Glyph Atlas (R9-R32) ----
    const char* liveHeader = title->renderMode == TitleArtRenderMode::GLYPH_ATLAS ? "Glyph Atlas" : "Realtime";
    if (ImGui::TreeNodeEx(liveHeader, ImGuiTreeNodeFlags_None)){
        ImGui::BeginDisabled(baked);
        beginTable(cpType, getLabelSize("Prebake Initial State"), "realtime");
        RowSettings scale;
        scale.floatMin = 0.25f;
        scale.floatMax = 2.0f;
        propertyRow(RowPropertyType::Float, cpType, "resolutionScale", "Resolution Scale", sceneProject, entities, scale);
        RowSettings priority;
        priority.intMin = -100;
        priority.intMax = 100;
        propertyRow(RowPropertyType::Int, cpType, "priority", "Priority", sceneProject, entities, priority);
        propertyRow(RowPropertyType::Bool, cpType, "renderWhenHidden", "Render When Hidden", sceneProject, entities);
        propertyRow(RowPropertyType::Bool, cpType, "prebakeInitialState", "Prebake Initial State", sceneProject, entities);
        static std::vector<EnumEntry> transitions = {{(int)TitleArtTransition::CUT, "Cut"}, {(int)TitleArtTransition::CROSSFADE, "Crossfade"}};
        RowSettings transition;
        transition.enumEntries = &transitions;
        propertyRow(RowPropertyType::Enum, cpType, "transition", "Transition", sceneProject, entities, transition);
        propertyRow(RowPropertyType::FloatPositive, cpType, "transitionDuration", "Transition Duration", sceneProject, entities);
        propertyRow(RowPropertyType::String, cpType, "qualityTier", "Quality Tier", sceneProject, entities);
        if (title->renderMode == TitleArtRenderMode::GLYPH_ATLAS){
            static std::vector<int> pageSizes = {512, 1024, 2048};
            RowSettings pages;
            pages.sliderValues = &pageSizes;
            propertyRow(RowPropertyType::IntSlider, cpType, "atlasPageSize", "Atlas Page Size", sceneProject, entities, pages);
            RowSettings maxPages;
            maxPages.intMin = 1;
            maxPages.intMax = 8;
            propertyRow(RowPropertyType::Int, cpType, "atlasMaxPages", "Atlas Max Pages", sceneProject, entities, maxPages);
        }
        endTable();
        if (single && title->renderMode == TitleArtRenderMode::GLYPH_ATLAS && !instance.liveStatus.glyphUnsafe.empty()){
            std::string list;
            for (const std::string& name : instance.liveStatus.glyphUnsafe) list += (list.empty() ? "" : ", ") + name;
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %zu effects don't apply per glyph: %s",
                               instance.liveStatus.glyphUnsafe.size(), list.c_str());
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // ---- Characters (R53, R62) ----
    if (ImGui::TreeNodeEx("Characters", ImGuiTreeNodeFlags_None)){
        ImGui::BeginDisabled(baked);
        beginTable(cpType, getLabelSize("Additional Characters"), "chars");
        RowSettings chars;
        chars.help = "Characters this instance may show besides its title's Characters set (they ship in the font subset)";
        propertyRow(RowPropertyType::String, cpType, "additionalCharacters", "Additional Characters", sceneProject, entities, chars);
        endTable();
        if (single && !instance.liveStatus.missingCharacters.empty()){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Not in the Characters set: \"%s\"",
                               instance.liveStatus.missingCharacters.c_str());
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // ---- Status (R60) ----
    if (single){
        std::string status;
        if (baked) status = instance.hasBake ? "Ready (Baked)" : "No bake";
        else if (instance.moduleMissing) status = "No titleart module in this build: shows its bake";
        else if (instance.failed) status = "Failed: " + instance.error;
        else if (instance.ready) status = "Ready";
        else status = instance.pending.empty() ? std::string("Waiting") : instance.pending;
        if (!baked && instance.liveStatus.lastFrames > 0){
            char last[160];
            std::snprintf(last, sizeof(last), " - last render %d frames, %.1f ms GPU (est.), %.1f ms CPU", instance.liveStatus.lastFrames,
                          instance.liveStatus.lastGpuMsEstimate, instance.liveStatus.lastCpuMs);
            status += last;
        }
        if (!instance.pixelPath.empty()) status += " - " + instance.pixelPath;
        if (instance.liveStatus.displayBytes > 0) status += " - " + mib(instance.liveStatus.displayBytes);
        ImGui::TextColored(instance.failed ? Theme::Colors::ErrorText : Theme::Colors::SubtleText, "Status: %s", status.c_str());
    }

    // ---- actions ----
    if (ImGui::Button(ICON_FA_EXPAND " Set Native Size")){
        // R3: one step "Set Native Size (<GameObject>)"
        auto multi = std::make_unique<MultiPropertyCmd>();
        for (Entity e : entities){
            float width = 0.0f, height = 0.0f, pivotX = 0.5f, pivotY = 0.5f;
            const TitleArtRendererComponent* t = scene->findComponent<TitleArtRendererComponent>(e);
            if (!t || !TitleArtSystem::nativeSize(scene, e, width, height, pivotX, pivotY)) continue;
            if (TitleArtSystem::spaceOf(scene, e) == titleart::TitleArtSpace::UI && scene->findComponent<RectTransformComponent>(e)){
                multi->addPropertyCmd<unsigned int>(project, sceneProject->id, e, ComponentType::RectTransformComponent, "width",
                                                    (unsigned int)std::max(1.0f, std::round(width)));
                multi->addPropertyCmd<unsigned int>(project, sceneProject->id, e, ComponentType::RectTransformComponent, "height",
                                                    (unsigned int)std::max(1.0f, std::round(height)));
                if (t->pivotFromTitle) multi->addPropertyCmd<Vector2>(project, sceneProject->id, e, ComponentType::RectTransformComponent, "pivot", Vector2(pivotX, pivotY));
            }else if (scene->findComponent<SpriteComponent>(e)){
                multi->addPropertyCmd<unsigned int>(project, sceneProject->id, e, ComponentType::SpriteComponent, "width",
                                                    (unsigned int)std::max(1.0f, std::round(width)));
                multi->addPropertyCmd<unsigned int>(project, sceneProject->id, e, ComponentType::SpriteComponent, "height",
                                                    (unsigned int)std::max(1.0f, std::round(height)));
            }
        }
        if (!multi->empty()) CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(multi.release());
    }
    ImGui::SetItemTooltip("Size the %s to the title's bake frame", title->renderMode == TitleArtRenderMode::BAKED && TitleArtSystem::spaceOf(scene, entity) == titleart::TitleArtSpace::World ? "Sprite" : "Image or Sprite");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FIRE " Add Particles")) ImGui::OpenPopup("##ta_particles");
    if (ImGui::BeginPopup("##ta_particles")){
        for (const char* label : {"Sparkle", "Embers", "Confetti", "Coin Shower"}){
            if (ImGui::MenuItem(label)){
                std::string error;
                if (!addTitleParticles(project, sceneProject->id, entity, label, &error)) ImGui::SetTooltip("%s", error.c_str());
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(baked);
    if (ImGui::Button(ICON_FA_ROTATE " Re-render")){
        for (Entity e : entities) TitleArtSystem::refresh(scene, e);
    }
    ImGui::EndDisabled();
}
