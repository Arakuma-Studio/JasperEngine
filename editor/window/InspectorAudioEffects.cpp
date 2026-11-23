// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Audio Source Inspector's Effects section (docs/specs/S97-audio-gaps.md REQ-38): the source's
// own effects with their bypass, Wet and parameters (the Audio Mixer's labels and units), Add
// Effect, reorder and remove, each edit one undo step of the component's effects; and Add
// Component > Audio Low Pass Filter and its siblings (REQ-5).

#include "InspectorWindow.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "audio/AudioEffects.h"
#include "audio/SourceEffectEdit.h"
#include "audio/SourceEffects.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/PropertyCmd.h"
#include "component/AudioSourceComponent.h"
#include "external/IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using EffectList = std::vector<AudioSourceEffectDef>;

    // the command of a slider being dragged: later drags merge into it, its release closes it
    // (only while it is still the history's last step: a drag back to the start leaves none)
    PropertyCmd<EffectList>* audioEffectDrag = nullptr;
    uint32_t audioEffectDragScene = 0;

    void commitAudioEffects(Project* project, uint32_t sceneId, Entity entity, ComponentType cpType, const EffectList& effects, bool dragging) {
        auto* cmd = new PropertyCmd<EffectList>(project, sceneId, entity, cpType, "effects", effects);
        if (dragging) {
            CommandHandle::get(sceneId)->addCommand(cmd);
            audioEffectDrag = cmd;
            audioEffectDragScene = sceneId;
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
            audioEffectDrag = nullptr;
        }
    }

    void finishAudioEffectDrag() {
        const UndoEntry* top = audioEffectDrag ? CommandHandle::get(audioEffectDragScene)->top() : nullptr;
        if (top && top->cmd.get() == audioEffectDrag) audioEffectDrag->setNoMerge();
        audioEffectDrag = nullptr;
    }

}

bool editor::InspectorWindow::addAudioEffectToSelection(const std::string& effect, std::string* why) {
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene || project->isAnyScenePlaying()) {
        if (why) *why = "Select GameObjects first (not while playing).";
        return false;
    }
    const std::vector<Entity>& entities = project->getSelectedEntities(sceneProject->id);
    if (entities.empty()) {
        if (why) *why = "Select GameObjects first.";
        return false;
    }
    std::string reason;
    Command* cmd = SourceEffectEdit::addEffectCommand(project, sceneProject->id, entities, effect, reason);
    if (!cmd) {
        if (why) *why = reason;
        return false;
    }
    CommandHandle::get(sceneProject->id)->addCommandNoMerge(cmd);
    sceneProject->isModified = true;
    return true;
}

void editor::InspectorWindow::drawAudioEffectRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    separatorText("Effects");
    if (entities.size() != 1) {
        ImGui::TextDisabled("Select one GameObject to edit its Audio Source's effects");
        return;
    }
    const Entity entity = entities[0];
    const uint32_t sceneId = sceneProject->id;
    const AudioSourceComponent& audio = sceneProject->scene->getComponent<AudioSourceComponent>(entity);
    const EffectList effects = audio.effects;
    const float width = ImGui::GetContentRegionAvail().x;
    const float labelWidth = getLabelSize("Sample Rate");

    if (effects.empty()) {
        ImGui::TextDisabled("None: the voices play the clip directly (no cost)");
    }
    for (size_t index = 0; index < effects.size(); index++) {
        const AudioSourceEffectDef& effect = effects[index];
        const AudioEffectInfo* info = AudioEffects::findPerSource(effect.type);
        ImGui::PushID((int)index);

        // header: on (not bypassed), name, cost badge, up / down, remove
        bool on = !effect.bypass;
        ImGui::BeginDisabled(!info);
        if (ImGui::Checkbox("##on", &on)) {
            EffectList next = effects;
            next[index].bypass = !on;
            commitAudioEffects(project, sceneId, entity, cpType, next, false);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("On / bypassed (a bypassed effect costs nothing on a voice started while every effect is off)");
        ImGui::SameLine();
        if (info) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(info->label);
            ImGui::SetItemTooltip("%s", info->description);
            if (info->costly) {
                ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_GAUGE_HIGH " cost: high");
                ImGui::SetItemTooltip("About 1-2%% of a mobile core per voice. Many sources sharing one: put it on a mixer group instead.");
            }
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Theme::Colors::WarningText, "unknown: %s (kept, inactive)", effect.type.c_str());
        }
        const float buttons = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), width - buttons));
        ImGui::BeginDisabled(index == 0);
        if (ImGui::Button(ICON_FA_ARROW_UP)) {
            EffectList next = effects;
            std::swap(next[index], next[index - 1]);
            commitAudioEffects(project, sceneId, entity, cpType, next, false);
        }
        ImGui::SetItemTooltip("Move Up (the chain runs top to bottom)");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1 >= effects.size());
        if (ImGui::Button(ICON_FA_ARROW_DOWN)) {
            EffectList next = effects;
            std::swap(next[index], next[index + 1]);
            commitAudioEffects(project, sceneId, entity, cpType, next, false);
        }
        ImGui::SetItemTooltip("Move Down");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK)) {
            EffectList next = effects;
            next.erase(next.begin() + (std::ptrdiff_t)index);
            commitAudioEffects(project, sceneId, entity, cpType, next, false);
            ImGui::PopID();
            break;
        }
        ImGui::SetItemTooltip("Remove");

        if (info && ImGui::BeginTable("##effect_params", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            std::vector<AudioEffectParamInfo> params = info->params;
            params.push_back(AudioEffects::wetParam());
            for (const AudioEffectParamInfo& param : params) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(param.label);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                float value = SourceEffects::paramValue(effect, info->type, param);
                auto store = [&](float v) {
                    EffectList next = effects;
                    if (std::string(param.name) == "wet") next[index].wet = v;
                    else next[index].params[param.name] = v;
                    return next;
                };
                const std::string id = std::string("##") + param.name;
                if (param.curve == AudioParamCurve::Switch) {
                    bool enabled = value >= 0.5f;
                    if (ImGui::Checkbox(id.c_str(), &enabled)) commitAudioEffects(project, sceneId, entity, cpType, store(enabled ? 1.0f : 0.0f), false);
                    continue;
                }
                char format[32];
                snprintf(format, sizeof(format), "%%.%df%s%s", param.maxValue >= 1000.0f ? 0 : 2, param.unit[0] ? " " : "", param.unit);
                const ImGuiSliderFlags flags = param.curve == AudioParamCurve::Frequency ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
                if (ImGui::SliderFloat(id.c_str(), &value, param.minValue, param.maxValue, format, flags)) {
                    commitAudioEffects(project, sceneId, entity, cpType, store(value), true);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) finishAudioEffectDrag();
                if (std::string(param.name) == "cutoff") {
                    ImGui::SetItemTooltip("%s", info->type == AudioEffectType::LowPass ? "22000 Hz (from 0.45 x the clip's rate) is off: no cost on a voice started there" :
                                                info->type == AudioEffectType::HighPass ? "10 Hz is off: no cost on a voice started there" : "Hz");
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    // Add Effect: the mixer's catalogue without the group-only ones
    ImGui::BeginDisabled((int)effects.size() >= AudioSourceComponent::maxEffects);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##audio_add_effect", ICON_FA_PLUS " Add Effect")) {
        for (const AudioEffectInfo& info : AudioEffects::all()) {
            if (!info.perSource) continue;
            if (ImGui::Selectable(info.label)) {
                EffectList next = effects;
                AudioSourceEffectDef added;
                added.type = info.name;
                next.push_back(added);
                commitAudioEffects(project, sceneId, entity, cpType, next, false);
            }
            ImGui::SetItemTooltip("%s%s", info.description, info.costly ? "\ncost: high" : "");
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if ((int)effects.size() >= AudioSourceComponent::maxEffects) {
        ImGui::SetItemTooltip("A source holds up to %d effects: route it through a mixer group for more", AudioSourceComponent::maxEffects);
    } else {
        ImGui::SetItemTooltip("Effects on this source's voices only, before its Output group (Unity's Audio Low Pass Filter and its siblings)");
    }
}
