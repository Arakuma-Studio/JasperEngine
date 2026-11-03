// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > Fluid Simulation (Pavel Dobryakov's WebGL fluid as a component), modelled on the
// Post-processing inspector (InspectorPostProcessing.cpp): a preset combo ("Custom" when no
// preset matches, one undo step), groups under separator titles, Bloom and Sunrays with their
// own enable checkbox (their parameters show while on), an Advanced section, label tooltips,
// reset arrows and slider-with-typed-entry rows. Every value goes through the Catalog property
// of the component (PropertyCmd), so undo, multi-selection and prefab overrides (the labels'
// override markers) work as on any component. Above the groups: what keeps the fluid from
// running, and the preview bar (edit-mode Play / Pause / Stop through FluidSystem::setPreview,
// Random Splats, Reset, the Scene view's Paint Dye brush: FluidEditorTools); below them the
// simulator's runtime stats.

#include "InspectorWindow.h"
#include "widget/ColorPickerWindow.h"

#include "App.h"
#include "Catalog.h"
#include "Theme.h"
#include "imgui_internal.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/FluidSimulationComponent.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "external/IconsFontAwesome6.h"
#include "fluid/FluidPresets.h"
#include "fluid/FluidSimulator.h"
#include "util/FluidEditorTools.h"
#include "util/FluidTextureLink.h"
#include "util/UIProbe.h"
#include "util/UIUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <utility>

using namespace jasperengine;

namespace {

    constexpr editor::ComponentType FLUID_TYPE = editor::ComponentType::FluidSimulationComponent;
    const float LABEL_INDENT_EM = 1.0f;

    void wrappedTooltip(const std::string& text) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The selection's values of one property: the first one, whether the others differ, and
    // the Catalog default
    template<typename T>
    struct FluidValues {
        T value{};
        T def{};
        bool valid = false;
        bool mixed = false;
        bool hasDef = false;
        bool changed() const { return hasDef && !(value == def); }
    };

    template<typename T>
    FluidValues<T> readFluidValues(Scene* scene, const std::vector<Entity>& entities, const std::string& property) {
        FluidValues<T> values;
        for (Entity entity : entities) {
            editor::PropertyData prop = editor::Catalog::findProperty(scene, entity, FLUID_TYPE, property);
            if (!prop.ref) continue;
            const T current = *static_cast<const T*>(prop.ref);
            if (!values.valid) {
                values.value = current;
                values.valid = true;
                if (prop.def) {
                    values.def = *static_cast<const T*>(prop.def);
                    values.hasDef = true;
                }
            } else if (!(current == values.value)) {
                values.mixed = true;
            }
        }
        return values;
    }

    // UI tests (UIProbe): a combo's frame, named before BeginCombo (an open one makes its
    // popup the last item)
    void probeFrame(const std::string& name) {
        if (!editor::UIProbe::enabled()) return;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        editor::UIProbe::rect(name, at.x, at.y, at.x + ImGui::CalcItemWidth(), at.y + ImGui::GetFrameHeight());
    }

    std::string formatBytes(size_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        return text;
    }

    // the selection's preset: its id when every one matches the same, "" for Custom, "-" mixed
    std::string matchedPreset(Scene* scene, const std::vector<Entity>& entities) {
        std::string matched;
        bool first = true;
        for (Entity entity : entities) {
            const FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
            if (!fluid) continue;
            const std::string id = matchFluidPreset(*fluid);
            if (first) {
                matched = id;
                first = false;
            } else if (id != matched) {
                return "-";
            }
        }
        return matched;
    }

}

bool editor::InspectorWindow::fluidRowHeader(const std::string& property, const std::string& label, const std::string& help,
                                             bool defChanged, bool indent){
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float indentWidth = ImGui::GetFontSize() * LABEL_INDENT_EM;
    if (indent) ImGui::Indent(indentWidth);

    // the label, bold with the margin bar when a prefab instance overrides it (right click:
    // apply or revert), and its help
    prefabRowProperty = property;
    drawPrefabPropertyLabel(label);
    if (!help.empty() && ImGui::IsItemHovered())
        wrappedTooltip(help);

    bool reset = false;
    if (defChanged) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        reset = ImGui::Button((ICON_FA_ROTATE_LEFT "##reset_" + property).c_str());
        UIProbe::item("fluid.default." + property);
        ImGui::SetItemTooltip("Reset to the default");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }
    if (indent) ImGui::Unindent(indentWidth);

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    return reset;
}

template<typename T>
void editor::InspectorWindow::commitFluidProperty(SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                  const std::string& property, const T& value, bool merge){
    CommandHistory* history = CommandHandle::get(project->getSelectedSceneId());
    if (merge) {
        // held widgets: one command a frame, merged into one undo step until released
        for (Entity entity : entities) {
            cmd = new PropertyCmd<T>(project, sceneProject->id, entity, FLUID_TYPE, property, value);
            history->addCommand(cmd);
        }
    } else {
        MultiPropertyCmd* multi = new MultiPropertyCmd();
        for (Entity entity : entities)
            multi->addPropertyCmd<T>(project, sceneProject->id, entity, FLUID_TYPE, property, value);
        multi->setNoMerge();
        history->addCommand(multi);
    }
}

void editor::InspectorWindow::applyFluidPreset(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& presetId){
    const FluidPreset* preset = findFluidPreset(presetId);
    if (!preset) return;

    // the preset's values by Catalog property name (its fields only)
    FluidSimulationComponent values = preset->values;
    std::map<std::string, PropertyData> props = Catalog::getProperties(FLUID_TYPE, &values);

    MultiPropertyCmd* multi = new MultiPropertyCmd();
    for (Entity entity : entities) {
        if (!sceneProject->scene->findComponent<FluidSimulationComponent>(entity)) continue;
        for (const std::string& field : preset->fields) {
            auto it = props.find(field);
            if (it == props.end() || !it->second.ref) continue;
            void* ref = it->second.ref;
            switch (it->second.type) {
                case PropertyType::Bool:
                    multi->addPropertyCmd<bool>(project, sceneProject->id, entity, FLUID_TYPE, field, *static_cast<bool*>(ref));
                    break;
                case PropertyType::Float:
                    multi->addPropertyCmd<float>(project, sceneProject->id, entity, FLUID_TYPE, field, *static_cast<float*>(ref));
                    break;
                case PropertyType::UInt:
                    multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, FLUID_TYPE, field, *static_cast<unsigned int*>(ref));
                    break;
                case PropertyType::Int:
                case PropertyType::Enum:
                    multi->addPropertyCmd<int>(project, sceneProject->id, entity, FLUID_TYPE, field, *static_cast<int*>(ref));
                    break;
                case PropertyType::Vector3:
                    multi->addPropertyCmd<Vector3>(project, sceneProject->id, entity, FLUID_TYPE, field, *static_cast<Vector3*>(ref));
                    break;
                default:
                    break;
            }
        }
    }
    multi->setNoMerge();
    CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
}

void editor::InspectorWindow::drawFluidStatus(SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    ImGui::PushTextWrapPos(0.0f);

    std::string reason;
    if (!FluidSimulator::isSupported(&reason)) {
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Not drawn: %s",
                           reason.empty() ? "this device cannot run the fluid simulation." : reason.c_str());
        ImGui::SetItemTooltip("The simulation needs half-float render targets. Nothing is drawn here; the exported game "
                              "checks its own device.");
    }

    for (Entity entity : entities) {
        const FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
        if (!fluid) continue;
        if (fluid->simulator && !fluid->simulator->getError().empty()) {
            const std::string who = entities.size() > 1 ? scene->getEntityName(entity) + ": " : std::string();
            ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_CIRCLE_EXCLAMATION " %s%s", who.c_str(),
                               fluid->simulator->getError().c_str());
        }
        if (fluid->output == FluidOutput::MATERIAL && !scene->findComponent<MeshComponent>(entity) &&
            !scene->findComponent<ImageComponent>(entity)) {
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Output Material draws on this "
                               "GameObject's Sprite, Image or Mesh: it has none.");
        }
    }

    ImGui::PopTextWrapPos();
}

void editor::InspectorWindow::drawFluidPreviewBar(SceneProject* sceneProject, const std::vector<Entity>& entities){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const Entity first = entities[0];
    const FluidPreviewState state = FluidEditorTools::getPreviewState(sceneId, first);

    auto each = [&](const std::function<void(Entity, FluidSimulationComponent&)>& fn) {
        for (Entity entity : entities) {
            if (FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity))
                fn(entity, *fluid);
        }
        sceneProject->needUpdateRender = true;
    };

    // Play / Pause, Stop: edit mode only (the game runs its own)
    ImGui::BeginDisabled(!stopped);
    if (state == FluidPreviewState::PLAYING) {
        if (ImGui::Button(ICON_FA_PAUSE "##fluid_pause"))
            each([&](Entity entity, FluidSimulationComponent&) { FluidEditorTools::pausePreview(project, sceneId, entity); });
        UIProbe::item("fluid.preview.pause");
        ImGui::SetItemTooltip("Pause the preview (queued splats still paint)");
    } else {
        if (ImGui::Button(ICON_FA_PLAY "##fluid_play"))
            each([&](Entity entity, FluidSimulationComponent&) { FluidEditorTools::playPreview(project, sceneId, entity); });
        UIProbe::item("fluid.preview.play");
        ImGui::SetItemTooltip(state == FluidPreviewState::PAUSED ? "Resume the preview"
                                                                 : "Preview the fluid in the Scene view without playing the game");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state == FluidPreviewState::STOPPED);
    if (ImGui::Button(ICON_FA_STOP "##fluid_stop"))
        each([&](Entity entity, FluidSimulationComponent&) { FluidEditorTools::stopPreview(project, sceneId, entity); });
    UIProbe::item("fluid.preview.stop");
    ImGui::SetItemTooltip("Stop the preview: the fluid restarts as the game starts it");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!stopped && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The game runs the fluid while it plays");

    // Random Splats and Reset: the preview and the running game
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BURST " Random Splats##fluid_burst")) {
        each([&](Entity entity, FluidSimulationComponent& fluid) {
            fluid.pendingBursts.push_back(fluid.randomSplatCount);
            if (stopped && FluidEditorTools::getPreviewState(sceneId, entity) == FluidPreviewState::STOPPED)
                FluidEditorTools::playPreview(project, sceneId, entity);
        });
    }
    UIProbe::item("fluid.burst");
    ImGui::SetItemTooltip("A burst of random splats (Random Splat Count; 0: Pavel's 5 to 24), as Pavel's space bar. "
                          "Starts the preview in edit mode.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##fluid_reset"))
        each([&](Entity, FluidSimulationComponent& fluid) { fluid.needReset = true; });
    UIProbe::item("fluid.restart");
    ImGui::SetItemTooltip("Restart the fluid: empty fields, the seed, the start burst");

    // Paint Dye: the Scene view's brush on the fluid's Sprite
    ImGui::SameLine();
    std::string reason;
    const bool canPaint = entities.size() == 1 && FluidEditorTools::canPaint(project, sceneId, first, &reason);
    const bool armed = FluidEditorTools::isArmed(sceneId, first);
    if (entities.size() != 1) reason = "Paint Dye paints one fluid: select only it.";
    ImGui::BeginDisabled(!canPaint && !armed);
    if (armed) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
    if (ImGui::Button(ICON_FA_DROPLET " Paint Dye##fluid_paint")) {
        if (armed) FluidEditorTools::disarm();
        else FluidEditorTools::arm(project, sceneId, first);
    }
    UIProbe::item("fluid.paint");
    if (armed) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        wrappedTooltip(std::string("Paint Dye: drag over the fluid's Sprite in the Scene view to stir it with the pointer "
                                   "(previews it). [ and ] change Splat Radius (the circle), Esc or this button stops. "
                                   "Painting is a live preview, not an edit: no undo step and nothing is saved.") +
                       (canPaint || armed ? std::string() : "\n\n" + reason));
    }

    // what runs, when there is room for it on the bar
    if (state != FluidPreviewState::STOPPED || armed) {
        const char* status = armed ? "Painting" : state == FluidPreviewState::PLAYING ? "Previewing" : "Paused";
        if (ImGui::GetContentRegionAvail().x - ImGui::GetItemRectMax().x + ImGui::GetCursorScreenPos().x >
            ImGui::CalcTextSize(status).x + ImGui::GetStyle().ItemSpacing.x * 2.0f) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Theme::Colors::WarningText, "%s", status);
        }
    }
}

void editor::InspectorWindow::drawFluidStats(SceneProject* sceneProject, const std::vector<Entity>& entities){
    const FluidSimulationComponent* fluid = sceneProject->scene->findComponent<FluidSimulationComponent>(entities[0]);
    if (!fluid || !fluid->simulator) return;
    const FluidStats& stats = fluid->simulator->getStats();

    std::string summary = ICON_FA_GAUGE_HIGH " Stats";
    if (stats.simWidth > 0) {
        char text[128];
        std::snprintf(text, sizeof(text), "  %dx%d / %dx%d, %s%s%s", stats.simWidth, stats.simHeight, stats.dyeWidth,
                      stats.dyeHeight, formatBytes(stats.gpuBytes).c_str(), stats.compactFormats ? "" : ", RGBA16F",
                      stats.manualFiltering ? ", manual filtering" : "");
        summary += text;
    }
    const bool open = ImGui::TreeNodeEx("##fluid_stats", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", summary.c_str());
    UIProbe::item("fluid.stats");
    ImGui::SetItemTooltip("The simulator of %s as last drawn (velocity grid / dye image, GPU memory)%s",
                          sceneProject->scene->getEntityName(entities[0]).c_str(),
                          entities.size() > 1 ? "; the first of the selection" : "");
    if (!open) return;

    ImGui::PushTextWrapPos(0.0f);
    if (stats.simWidth <= 0) {
        ImGui::TextDisabled("Not drawn yet: preview it or play the game.");
    } else {
        ImGui::TextDisabled("Velocity %d x %d, dye %d x %d, %s of GPU memory", stats.simWidth, stats.simHeight,
                            stats.dyeWidth, stats.dyeHeight, formatBytes(stats.gpuBytes).c_str());
        ImGui::TextDisabled("Output %d x %d", stats.outputWidth, stats.outputHeight);
        ImGui::TextDisabled("%d passes last frame, %llu steps (%.1f s), %llu splats, seed %u", stats.passesLastFrame,
                            (unsigned long long)stats.steps, stats.time, (unsigned long long)stats.splats, stats.seed);
        ImGui::TextDisabled("Fields %s, %s filtering", stats.compactFormats ? "compact (velocity RG16F, scalars R16F)" : "RGBA16F (Pavel's fallback)",
                            stats.manualFiltering ? "manual" : "hardware");
        ImGui::SetItemTooltip("Compact: the velocity in RG16F and the scalar fields in R16F (less memory); else all "
                              "RGBA16F. Manual filtering: the device cannot filter half floats, so the advection "
                              "filters by hand and the look passes are skipped.");
        std::string fallbacks;
        auto off = [&](bool asked, bool active, const char* name) {
            if (asked && !active) fallbacks += (fallbacks.empty() ? "" : ", ") + std::string(name);
        };
        off(fluid->shading, stats.shadingActive, "shading");
        off(fluid->bloom, stats.bloomActive, "bloom");
        off(fluid->sunrays, stats.sunraysActive, "sunrays");
        if (!fallbacks.empty()) {
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Fallback: %s off on this device",
                               fallbacks.c_str());
            ImGui::SetItemTooltip("The device cannot filter half-float textures: the look passes that need it are skipped "
                                  "(Pavel's fallback).");
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::TreePop();
}

void editor::InspectorWindow::drawFluidSimulationComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities, bool readOnly){
    Scene* scene = sceneProject->scene;
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](Entity entity) {
        return !scene->findComponent<FluidSimulationComponent>(entity);
    }), entities.end());
    if (entities.empty()) return;

    // ---- rows: a label (prefab marker, help, reset arrow) and a value editing the property
    auto endEdit = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
    };

    auto floatRow = [&](const char* property, const char* label, const std::string& help, float sliderMin, float sliderMax,
                        float hardMin, float hardMax, const char* format = "%.2f", bool indent = false, const char* zeroText = nullptr) {
        const FluidValues<float> v = readFluidValues<float>(scene, entities, property);
        if (!v.valid) return;
        ImGui::PushID(property);
        if (fluidRowHeader(property, label, help, v.changed(), indent))
            commitFluidProperty<float>(sceneProject, entities, property, v.def, false);
        float value = v.value;
        if (v.mixed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const char* shown = (zeroText && value == 0.0f) ? zeroText : format;
        if (UIUtils::sliderFloatInput("##value", &value, sliderMin, sliderMax, shown))
            commitFluidProperty<float>(sceneProject, entities, property, std::clamp(value, hardMin, hardMax), true);
        UIProbe::item(std::string("fluid.value.") + property);
        if (v.mixed) ImGui::PopStyleColor();
        endEdit();
        ImGui::PopID();
    };

    auto boolRow = [&](const char* property, const char* label, const std::string& help, bool indent = false) {
        const FluidValues<bool> v = readFluidValues<bool>(scene, entities, property);
        if (!v.valid) return;
        ImGui::PushID(property);
        if (fluidRowHeader(property, label, help, v.changed(), indent))
            commitFluidProperty<bool>(sceneProject, entities, property, v.def, false);
        bool value = v.value;
        if (v.mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("##value", &value))
            commitFluidProperty<bool>(sceneProject, entities, property, v.mixed ? true : value, false);
        UIProbe::item(std::string("fluid.value.") + property);
        if (v.mixed) ImGui::PopItemFlag();
        ImGui::PopID();
    };

    // a fixed set of sizes; another value (Lua, MCP) shows as itself
    auto choiceRow = [&](const char* property, const char* label, const std::string& help,
                         const std::vector<std::pair<unsigned int, const char*>>& choices, bool indent = false) {
        const FluidValues<unsigned int> v = readFluidValues<unsigned int>(scene, entities, property);
        if (!v.valid) return;
        ImGui::PushID(property);
        if (fluidRowHeader(property, label, help, v.changed(), indent))
            commitFluidProperty<unsigned int>(sceneProject, entities, property, v.def, false);
        std::string preview = std::to_string(v.value);
        for (const auto& choice : choices)
            if (choice.first == v.value) preview = choice.second;
        if (v.mixed) preview = "-";
        probeFrame(std::string("fluid.value.") + property);
        if (ImGui::BeginCombo("##value", preview.c_str())) {
            for (const auto& choice : choices) {
                if (ImGui::Selectable(choice.second, !v.mixed && choice.first == v.value) && (v.mixed || choice.first != v.value))
                    commitFluidProperty<unsigned int>(sceneProject, entities, property, choice.first, false);
                UIProbe::item(std::string("fluid.item.") + property + "." + std::to_string(choice.first));
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    };

    auto uintSliderRow = [&](const char* property, const char* label, const std::string& help, unsigned int minValue,
                             unsigned int maxValue, bool indent = false, const char* zeroText = nullptr) {
        const FluidValues<unsigned int> v = readFluidValues<unsigned int>(scene, entities, property);
        if (!v.valid) return;
        ImGui::PushID(property);
        if (fluidRowHeader(property, label, help, v.changed(), indent))
            commitFluidProperty<unsigned int>(sceneProject, entities, property, v.def, false);
        unsigned int value = v.value;
        if (v.mixed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const char* format = (zeroText && value == 0) ? zeroText : "%u";
        if (ImGui::SliderScalar("##value", ImGuiDataType_U32, &value, &minValue, &maxValue, format, ImGuiSliderFlags_AlwaysClamp))
            commitFluidProperty<unsigned int>(sceneProject, entities, property, value, true);
        UIProbe::item(std::string("fluid.value.") + property);
        if (v.mixed) ImGui::PopStyleColor();
        endEdit();
        ImGui::PopID();
    };

    const float labelWidth = getLabelSize("Random Splat Count") + ImGui::GetFontSize() * LABEL_INDENT_EM;
    auto group = [&](const char* name) {
        ImGui::Spacing();
        ImGui::SeparatorText(name);
        beginTable(cpType, labelWidth, name);
    };

    // a group of an effect with its own enable flag (Bloom, Sunrays): the checkbox before the
    // title; its rows show while it is on
    auto effectGroup = [&](const char* name, const char* property, const std::string& help) -> bool {
        const FluidValues<bool> v = readFluidValues<bool>(scene, entities, property);
        ImGui::Spacing();
        ImGui::PushID(property);
        bool on = v.value || v.mixed;
        if (v.mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        if (ImGui::Checkbox("##group", &on))
            commitFluidProperty<bool>(sceneProject, entities, property, v.mixed ? false : on, false);
        UIProbe::item(std::string("fluid.group.") + property);
        if (v.mixed) ImGui::PopItemFlag();
        if (ImGui::IsItemHovered()) wrappedTooltip(help);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::SeparatorText(name);
        if (ImGui::IsItemHovered()) wrappedTooltip(help);
        return on;
    };

    // ---- what keeps it from running, the preview bar (transient, not edits: they also work on
    // a read-only fluid, one the running game made; only the rows below are disabled then)
    drawFluidStatus(sceneProject, entities);
    drawFluidPreviewBar(sceneProject, entities);
    ImGui::BeginDisabled(readOnly);

    // ---- preset: the solver and look fields of a named look, one undo step
    beginTable(cpType, labelWidth, "preset");
    {
        const std::string matched = matchedPreset(scene, entities);
        const FluidPreset* shownPreset = matched.empty() || matched == "-" ? nullptr : findFluidPreset(matched);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preset");
        if (ImGui::IsItemHovered())
            wrappedTooltip("A named look: sets the solver and look values (one undo step). Output, Interactive, Aspect "
                           "Ratio, Transparent, Back Color, the clock, the start burst and the seed stay. Custom: the "
                           "values match no preset.");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        const char* preview = matched == "-" ? "-" : shownPreset ? shownPreset->name.c_str() : "Custom";
        probeFrame("fluid.preset");
        if (ImGui::BeginCombo("##fluid_preset", preview)) {
            for (const FluidPreset& preset : getFluidPresets()) {
                const bool selected = shownPreset == &preset;
                if (ImGui::Selectable(preset.name.c_str(), selected) && !selected)
                    applyFluidPreset(sceneProject, entities, preset.id);
                UIProbe::item("fluid.preset." + preset.id);
                if (ImGui::IsItemHovered())
                    wrappedTooltip(preset.description);
            }
            ImGui::EndCombo();
        }
    }
    endTable();

    // ---- Output
    group("Output");
    {
        const FluidValues<int> v = readFluidValues<int>(scene, entities, "output");
        if (v.valid) {
            ImGui::PushID("output");
            if (fluidRowHeader("output", "Output",
                               "Where the fluid is drawn.\nScreen: over the main camera's image (after post-processing), "
                               "the size of the view.\nMaterial: this GameObject's Sprite, Image or Mesh texture.\nTexture: only "
                               "the texture, for the texture slots linking it (their droplet button) and "
                               "FluidSimulation:getTexture().", v.changed(), false))
                commitFluidProperty<int>(sceneProject, entities, "output", v.def, false);
            static const std::pair<FluidOutput, const char*> outputs[] = {
                {FluidOutput::SCREEN, "Screen"}, {FluidOutput::MATERIAL, "Material"}, {FluidOutput::TEXTURE, "Texture"}};
            const char* preview = "-";
            for (const auto& output : outputs)
                if (!v.mixed && (int)output.first == v.value) preview = output.second;
            probeFrame("fluid.value.output");
            if (ImGui::BeginCombo("##value", preview)) {
                for (const auto& output : outputs) {
                    const bool selected = !v.mixed && (int)output.first == v.value;
                    if (ImGui::Selectable(output.second, selected) && !selected)
                        commitFluidProperty<int>(sceneProject, entities, "output", (int)output.first, false);
                    UIProbe::item(std::string("fluid.item.output.") + output.second);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        boolRow("interactive", "Interactive",
                "The pointer stirs the fluid while the game runs (Pavel's mouse and touches): over the whole view in "
                "Screen, over the Sprite or Image in Material. Scripts splat either way (FluidSimulation:splat).");
        floatRow("aspectRatio", "Aspect Ratio",
                 "Width / height of the fluid. Auto (0): the view in Screen, the Sprite or Image size in Material, else 1.",
                 0.0f, 4.0f, 0.0f, 100.0f, "%.2f", false, "Auto");
    }
    endTable();

    // ---- Simulation
    group("Simulation");
    choiceRow("dyeResolution", "Quality",
              "Dye resolution (short side): the image's detail and most of the fluid's memory. Pavel's quality "
              "setting: High 1024, Medium 512, Low 256, Very Low 128.",
              {{1024, "High (1024)"}, {512, "Medium (512)"}, {256, "Low (256)"}, {128, "Very Low (128)"}});
    choiceRow("simResolution", "Sim Resolution",
              "Velocity grid (short side): the size of the motion's detail. Every solver pass runs on it, so it is the "
              "main cost of a step; Pavel's default 128.",
              {{32, "32"}, {64, "64"}, {128, "128"}, {256, "256"}});
    floatRow("densityDissipation", "Density Diffusion",
             "How fast the dye fades: value / (1 + k dt) a step. 0 keeps it forever; Pavel's 1.", 0.0f, 4.0f, 0.0f, 100.0f);
    floatRow("velocityDissipation", "Velocity Diffusion",
             "How fast the motion dies down: value / (1 + k dt) a step. Pavel's 0.2.", 0.0f, 4.0f, 0.0f, 100.0f);
    floatRow("pressure", "Pressure",
             "The previous pressure kept as the solver's warm start (Pavel's PRESSURE, 0.8): higher holds the "
             "swirls' shape across steps.", 0.0f, 1.0f, 0.0f, 1.0f);
    floatRow("curl", "Vorticity",
             "Vorticity confinement (Pavel's CURL, 30): puts back the small swirls the grid smooths away. 0: none.",
             0.0f, 50.0f, 0.0f, 1000.0f, "%.1f");
    endTable();

    // ---- Splats
    group("Splats");
    floatRow("splatRadius", "Splat Radius",
             "The size of a splat (Pavel's SPLAT_RADIUS, used / 100): the Gaussian exp(-d^2 / r) of the pointer and of "
             "random splats. [ and ] change it while painting.", 0.01f, 1.0f, 0.001f, 10.0f, "%.3f");
    floatRow("splatForce", "Splat Force",
             "The push of a pointer's movement: its delta x this (Pavel's SPLAT_FORCE, 6000).", 0.0f, 20000.0f, 0.0f,
             1000000.0f, "%.0f");
    boolRow("colorful", "Colorful", "Pointer colours cycle through the hues (Pavel's COLORFUL); off keeps one colour a stroke.");
    {
        const FluidValues<bool> colorful = readFluidValues<bool>(scene, entities, "colorful");
        if (colorful.value || colorful.mixed)
            floatRow("colorUpdateSpeed", "Color Speed", "How often the pointer colours change (Pavel's 10).", 0.0f, 50.0f,
                     0.0f, 1000.0f, "%.1f", true);
    }
    boolRow("splatsOnStart", "Splats On Start", "A random burst when the fluid starts (and restarts), like Pavel's page load.");
    uintSliderRow("randomSplatCount", "Random Splat Count",
                  "The splats of a random burst (on start, Random Splats, FluidSimulation:randomSplats). 0: a random "
                  "5 to 24, Pavel's.", 0, 64, false, "Random (5-24)");
    {
        const FluidValues<unsigned int> v = readFluidValues<unsigned int>(scene, entities, "seed");
        if (v.valid) {
            ImGui::PushID("seed");
            if (fluidRowHeader("seed", "Seed",
                               "Every random choice (burst positions, colours, pointer colours) comes from this seed, so "
                               "a seeded fluid starts the same every time. 0: a new seed each start.", v.changed(), false))
                commitFluidProperty<unsigned int>(sceneProject, entities, "seed", v.def, false);
            const float diceWidth = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - diceWidth - ImGui::GetStyle().ItemSpacing.x));
            unsigned int value = v.value;
            if (v.mixed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::DragScalar("##value", ImGuiDataType_U32, &value, 1.0f, nullptr, nullptr,
                                  value == 0 ? "0 (random each start)" : "%u"))
                commitFluidProperty<unsigned int>(sceneProject, entities, "seed", value, true);
            UIProbe::item("fluid.value.seed");
            if (v.mixed) ImGui::PopStyleColor();
            endEdit();
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));
            if (ImGui::Button(ICON_FA_DICE "##dice", ImVec2(diceWidth, 0))) {
                static std::mt19937 generator{std::random_device{}()};
                const unsigned int seed = std::uniform_int_distribution<unsigned int>(1u, 0x7fffffffu)(generator);
                commitFluidProperty<unsigned int>(sceneProject, entities, "seed", seed, false);
            }
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("A new fixed seed");
            ImGui::PopID();
        }
    }
    endTable();

    // ---- Rendering
    group("Rendering");
    boolRow("shading", "Shading", "Lights the dye by its gradient, as a relief (Pavel's SHADING). Needs filterable half floats.");
    boolRow("transparent", "Transparent",
            "No background: the fluid's alpha is its brightest channel, max(r, g, b), so it blends over what is behind "
            "(Pavel's TRANSPARENT).");
    {
        const FluidValues<bool> transparent = readFluidValues<bool>(scene, entities, "transparent");
        const FluidValues<Vector3> v = readFluidValues<Vector3>(scene, entities, "backColor");
        if (v.valid && (!transparent.value || transparent.mixed)) {
            ImGui::PushID("backColor");
            if (fluidRowHeader("backColor", "Back Color",
                               "The background behind the dye (Pavel's BACK_COLOR). Display values like the dye: no "
                               "sRGB to linear conversion.", v.changed(), false))
                commitFluidProperty<Vector3>(sceneProject, entities, "backColor", v.def, false);
            Vector3 value = v.value;
            ui::ColorFieldOptions colorOptions;
            colorOptions.alpha = false;
            colorOptions.linearStorage = false;
            colorOptions.label = "Back Color";
            colorOptions.mixed = v.mixed;
            const ui::EditPhase phase = ui::colorField("##value", value, colorOptions);
            if (phase == ui::EditPhase::Begin)
                ui::ColorPickerWindow::beginEdit(CommandHandle::get(project->getSelectedSceneId()), "Back Color");
            else if (phase == ui::EditPhase::Changing)
                commitFluidProperty<Vector3>(sceneProject, entities, "backColor", value, true);
            UIProbe::item("fluid.value.backColor");
            endEdit();
            ImGui::PopID();
        }
    }
    endTable();

    // ---- Bloom, Sunrays
    if (effectGroup("Bloom", "bloom",
                    "Glow around the bright dye: Pavel's soft-knee bloom on the dye (HDR), blurred down and up a chain of "
                    "Iterations levels. Needs filterable half floats.")) {
        beginTable(cpType, labelWidth, "bloom");
        floatRow("bloomIntensity", "Intensity", "The glow's strength (Pavel's 0.8).", 0.1f, 2.0f, 0.0f, 100.0f);
        floatRow("bloomThreshold", "Threshold", "The brightness the glow starts from (Pavel's 0.6).", 0.0f, 1.0f, 0.0f, 10.0f);
        floatRow("bloomSoftKnee", "Soft Knee", "How gently it starts around the threshold: 0 a hard cut, 1 a wide curve (Pavel's 0.7).",
                 0.0f, 1.0f, 0.0f, 1.0f);
        uintSliderRow("bloomIterations", "Iterations", "Blur levels: a wider glow for two passes a level (Pavel's 8).", 1, 8);
        endTable();
    }
    if (effectGroup("Sunrays", "sunrays",
                    "Light shafts streaming from the dye (Pavel's sunrays): a radial blur of its mask, darkening what "
                    "lies behind. Two passes a frame at Sunrays Resolution.")) {
        beginTable(cpType, labelWidth, "sunrays");
        floatRow("sunraysWeight", "Weight", "The rays' strength (Pavel's 1).", 0.3f, 1.0f, 0.0f, 10.0f);
        endTable();
    }

    // ---- Advanced
    ImGui::Spacing();
    const bool advancedOpen = ImGui::TreeNodeEx("##fluid_advanced", ImGuiTreeNodeFlags_SpanAvailWidth, "Advanced");
    UIProbe::item("fluid.advanced");
    if (advancedOpen) {
        beginTable(cpType, labelWidth, "advanced");
        uintSliderRow("pressureIterations", "Pressure Iterations",
                      "Jacobi iterations of the pressure solve a step: more makes the fluid less compressible and costs "
                      "one pass each (Pavel's 20).", 1, 80);
        floatRow("gradientScale", "Gradient Scale",
                 "The pressure gradient subtracted from the velocity x this. 1 is Pavel's (his gradient omits the 0.5 of "
                 "the central difference, so it projects twice: livelier); 0.5 the textbook projection.",
                 0.25f, 1.5f, 0.0f, 4.0f);
        floatRow("simulationRate", "Simulation Rate",
                 "Fixed steps a second: the fluid moves the same at any frame rate (Pavel steps once a frame).",
                 15.0f, 240.0f, 1.0f, 1000.0f, "%.0f");
        uintSliderRow("maxSubsteps", "Max Substeps",
                      "Steps a frame at most: a slow frame drops the rest of its time instead of falling behind.", 1, 16);
        floatRow("timeScale", "Time Scale", "Slow motion below 1, faster above.", 0.0f, 4.0f, 0.0f, 100.0f);
        boolRow("paused", "Paused", "No steps; splats still paint (Pavel's P key).");
        choiceRow("bloomResolution", "Bloom Resolution", "The bloom chain's first level (short side; Pavel's 256).",
                  {{64, "64"}, {128, "128"}, {256, "256"}, {512, "512"}, {1024, "1024"}});
        choiceRow("sunraysResolution", "Sunrays Resolution", "The sunrays' mask (short side; Pavel's 196).",
                  {{98, "98"}, {196, "196"}, {256, "256"}, {392, "392"}, {512, "512"}});
        endTable();
        ImGui::TreePop();
    }

    ImGui::EndDisabled();

    // ---- runtime stats
    drawFluidStats(sceneProject, entities);
}

void editor::InspectorWindow::applyFluidTexture(Entity fluidEntity, ComponentType cpType, const std::string& id, SceneProject* sceneProject,
                                                std::vector<Entity>& entities, std::function<void()> onValueChanged){
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<FluidSimulationComponent>(fluidEntity))
        return;

    // the same path as a camera link (applyCameraTexture): one undo step for the selection
    Texture texture = FluidTextureLink::make(scene, fluidEntity);
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    for (Entity& entity : entities)
        multiCmd->addPropertyCmd<Texture>(project, sceneProject->id, entity, cpType, id, texture, onValueChanged);
    multiCmd->setNoMerge();
    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
}
