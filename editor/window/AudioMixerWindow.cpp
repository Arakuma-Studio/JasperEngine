// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioMixerWindow.h"

#include "Project.h"
#include "Theme.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerDocument.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileUtils.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    const char* effectLabel(const AudioMixerEffectDef& effect) {
        const AudioEffectInfo* info = AudioEffects::find(effect.type);
        return info ? info->label : effect.typeName.c_str();
    }

    ImU32 meterColor(float db) {
        if (db > -0.5f) return IM_COL32(230, 70, 60, 255);
        if (db > -12.0f) return IM_COL32(230, 200, 60, 255);
        return IM_COL32(80, 200, 100, 255);
    }

    // 0..1 of the height of a -80..+20 dB scale
    float dbFraction(float db) {
        return std::clamp((db - AudioMath::minDb) / (AudioMath::maxDb - AudioMath::minDb), 0.0f, 1.0f);
    }

    constexpr const char* GROUP_PAYLOAD = "AUDIO_MIXER_GROUP";
    constexpr const char* EFFECT_PAYLOAD = "AUDIO_MIXER_EFFECT";

}

AudioMixerWindow::AudioMixerWindow(Project* project): project(project) {
}

void AudioMixerWindow::open(const fs::path& mixer) {
    chosenFile = mixer;
    loaded = false;
    lastCheck = -1.0;
    setOpen(true);
    focusRequested = true;
}

void AudioMixerWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
    if (!open && (dragging || hasStaged)) {
        finishDrag();
        applyStaged();
    }
}

bool AudioMixerWindow::isOpen() const {
    return windowOpen;
}

void AudioMixerWindow::resetForProjectSwitch() {
    chosenFile.clear();
    file.clear();
    def = AudioMixerDef();
    loaded = false;
    loadError.clear();
    fileText.clear();
    error.clear();
    dragging = false;
    dragEnded = false;
    hasStaged = false;
    stagedWrite = false;
    selectedGroup.clear();
    renaming.clear();
    peaks.clear();
}

bool AudioMixerWindow::isProjectMixer() const {
    const fs::path projectFile = project ? project->getAudioMixerFile() : fs::path();
    return !file.empty() && !projectFile.empty() && file.lexically_normal() == projectFile;
}

void AudioMixerWindow::resolveFile() {
    const fs::path wanted = chosenFile.empty() ? (project ? project->getAudioMixerFile() : fs::path()) : chosenFile;
    if (wanted != file) {
        file = wanted;
        loaded = false;
        fileText.clear();
        selectedGroup.clear();
    }
}

void AudioMixerWindow::reload(bool force) {
    if (dragging) return;
    const double now = ImGui::GetTime();
    if (!force && loaded && now - lastCheck < 0.5) return;
    lastCheck = now;
    std::error_code ec;
    if (file.empty() || !fs::is_regular_file(file, ec)) {
        loaded = false;
        loadError.clear();
        fileText.clear();
        return;
    }
    const std::string text = readText(file);
    if (loaded && text == fileText) return;
    AudioMixerDef next;
    std::string parseError;
    if (!AudioMixerIO::parse(text, next, &parseError)) {
        loadError = parseError;
        loaded = false;
        fileText = text;
        return;
    }
    def = std::move(next);
    fileText = text;
    loaded = true;
    loadError.clear();
    if (!selectedGroup.empty() && !def.findGroupById(selectedGroup)) selectedGroup.clear();
}

bool AudioMixerWindow::commit(const AudioMixerDef& next) {
    // applied at the start of the next frame: the widgets of this one still read the def
    staged = next;
    hasStaged = true;
    stagedWrite = true;
    return true;
}

bool AudioMixerWindow::write(const AudioMixerDef& next) {
    std::string commitError;
    if (!AudioMixerDocument::commit(project, file, next, "", &commitError)) {
        error = commitError;
        // the file is what counts
        loaded = false;
        reload(true);
        if (isProjectMixer() && loaded) AudioMixer::apply(def);
        return false;
    }
    error.clear();
    def = next;
    fileText = readText(file);
    return true;
}

void AudioMixerWindow::preview(const AudioMixerDef& next) {
    staged = next;
    hasStaged = true;
    dragging = true;
}

void AudioMixerWindow::finishDrag() {
    if (dragging) dragEnded = true;
}

void AudioMixerWindow::applyStaged() {
    if (hasStaged) {
        hasStaged = false;
        if (stagedWrite) {
            stagedWrite = false;
            write(staged);
        } else {
            def = staged;
            if (isProjectMixer() && AudioMixerIO::validate(def).empty()) AudioMixer::apply(def);
        }
    }
    if (dragEnded) {
        dragEnded = false;
        dragging = false;
        write(def);
    }
}

std::string AudioMixerWindow::exposedName(const std::string& target) const {
    for (const AudioMixerExposedDef& exposed : def.exposed) {
        if (exposed.target == target) return exposed.name;
    }
    return "";
}

void AudioMixerWindow::exposeMenu(const std::string& target, const std::string& suggestedName) {
    if (!ImGui::BeginPopupContextItem()) return;
    const std::string name = exposedName(target);
    if (name.empty()) {
        if (ImGui::MenuItem("Expose to Script...")) {
            exposeTarget = target;
            std::string suggestion = suggestedName;
            for (int n = 2; def.findExposed(suggestion); n++) suggestion = suggestedName + std::to_string(n);
            snprintf(exposeName, sizeof(exposeName), "%s", suggestion.c_str());
            openExposePopup = true;
        }
    } else {
        ImGui::TextDisabled("Exposed as '%s'", name.c_str());
        if (ImGui::MenuItem("Unexpose")) {
            AudioMixerDef next = def;
            next.exposed.erase(std::remove_if(next.exposed.begin(), next.exposed.end(),
                                              [&](const AudioMixerExposedDef& e) { return e.target == target; }), next.exposed.end());
            commit(next);
        }
    }
    AudioMixerTarget resolved;
    if (AudioMixerIO::resolveTarget(def, target, resolved) && resolved.volume) {
        ImGui::Separator();
        if (ImGui::MenuItem("Reset to 0 dB")) {
            AudioMixerDef next = def;
            AudioMixerIO::setAuthoredValue(next, target, 0.0f);
            commit(next);
        }
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- toolbar

void AudioMixerWindow::drawToolbar() {
    const std::string shown = project ? FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(file)) : "";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_SLIDERS);
    ImGui::SameLine();
    ImGui::Text("%s%s", shown.c_str(), isProjectMixer() ? "  (project mixer)" : "");
    if (!chosenFile.empty() && !isProjectMixer()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show Project Mixer")) open();
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFontSize() * 13.0f));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    int mode = def.unscaled ? 1 : 0;
    const char* modes[] = {"Scaled time", "Unscaled time"};
    if (ImGui::Combo("##AudioMixerUpdateMode", &mode, modes, 2) && (mode == 1) != def.unscaled) {
        AudioMixerDef next = def;
        next.unscaled = mode == 1;
        commit(next);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("What snapshot transitions and timed setFloat follow: Unity's Normal (scaled by Engine.timeScale) or Unscaled Time");
    }

    if (project && project->isAnyScenePlaying() && isProjectMixer()) {
        const std::string current = AudioMixer::getCurrentSnapshot();
        ImGui::TextDisabled("Play: edits are heard and saved. Snapshot now: %s%s", current.empty() ? "none" : current.c_str(),
                            AudioMixer::isTransitioning() ? " (moving)" : "");
    }
    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------- sidebar

void AudioMixerWindow::drawGroupNode(const AudioMixerGroupDef& group) {
    const std::vector<const AudioMixerGroupDef*> children = def.children(group.id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selectedGroup == group.id) flags |= ImGuiTreeNodeFlags_Selected;

    const std::string key = "group:" + group.id;
    bool open = false;
    if (renaming == key) {
        ImGui::SetNextItemWidth(-1);
        if (renameFocus) {
            ImGui::SetKeyboardFocusHere();
            renameFocus = false;
        }
        if (ImGui::InputText(("##rename" + group.id).c_str(), renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            std::vector<std::string> notes;
            AudioMixerDef next = def;
            std::string opError;
            if (std::string(renameBuffer) != group.name &&
                !AudioMixerDocument::applyOps(next, {{{"op", "set_group"}, {"name", group.id}, {"new_name", std::string(renameBuffer)}}}, &opError, &notes)) {
                error = opError;
            } else if (std::string(renameBuffer) != group.name) {
                commit(next);
            }
            renaming.clear();
        } else if (ImGui::IsItemDeactivated()) {
            renaming.clear();
        }
        open = !children.empty();
        if (open) ImGui::TreePush(group.id.c_str());
    } else {
        open = ImGui::TreeNodeEx(group.id.c_str(), flags, "%s", group.name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectedGroup = group.id;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !group.parentId.empty()) {
            renaming = key;
            renameFocus = true;
            snprintf(renameBuffer, sizeof(renameBuffer), "%s", group.name.c_str());
        }
        // drag a group onto another to move it there
        if (!group.parentId.empty() && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(GROUP_PAYLOAD, group.id.c_str(), group.id.size() + 1);
            ImGui::Text("%s", group.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(GROUP_PAYLOAD)) {
                const std::string moved = static_cast<const char*>(payload->Data);
                if (moved != group.id && !def.isUnder(group.id, moved)) {
                    AudioMixerDef next = def;
                    if (AudioMixerGroupDef* target = next.findGroup(moved)) {
                        target->parentId = group.id;
                        commit(next);
                    }
                } else if (moved != group.id) {
                    error = "A group cannot go inside its own subgroups.";
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add Child Group")) {
                AudioMixerDef next = def;
                AudioMixerGroupDef child;
                child.id = AudioMixerIO::newId(next);
                child.name = AudioMixerDocument::uniqueGroupName(next, "Group");
                child.parentId = group.id;
                next.groups.push_back(child);
                if (commit(next)) {
                    selectedGroup = child.id;
                    renaming = "group:" + child.id;
                    renameFocus = true;
                    snprintf(renameBuffer, sizeof(renameBuffer), "%s", child.name.c_str());
                }
            }
            if (ImGui::MenuItem("Rename", nullptr, false, !group.parentId.empty())) {
                renaming = key;
                renameFocus = true;
                snprintf(renameBuffer, sizeof(renameBuffer), "%s", group.name.c_str());
            }
            if (ImGui::MenuItem("Delete", nullptr, false, !group.parentId.empty())) {
                pendingDeleteGroup = group.id;
                openDeletePopup = true;
            }
            ImGui::EndPopup();
        }
    }
    if (open) {
        for (const AudioMixerGroupDef* child : children) drawGroupNode(*child);
        ImGui::TreePop();
    }
}

void AudioMixerWindow::drawSnapshots() {
    for (const AudioMixerSnapshotDef& snapshot : def.snapshots) {
        const std::string key = "snapshot:" + snapshot.name;
        const bool isStart = snapshot.name == def.startSnapshot;
        if (renaming == key) {
            ImGui::SetNextItemWidth(-1);
            if (renameFocus) {
                ImGui::SetKeyboardFocusHere();
                renameFocus = false;
            }
            if (ImGui::InputText(("##renameSnapshot" + snapshot.name).c_str(), renameBuffer, sizeof(renameBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                AudioMixerDef next = def;
                std::string opError;
                if (std::string(renameBuffer) != snapshot.name) {
                    if (AudioMixerDocument::applyOp(next, {{"op", "set_snapshot"}, {"name", snapshot.name}, {"new_name", std::string(renameBuffer)}}, &opError)) commit(next);
                    else error = opError;
                }
                renaming.clear();
            } else if (ImGui::IsItemDeactivated()) {
                renaming.clear();
            }
            continue;
        }
        ImGui::Selectable((std::string(isStart ? ICON_FA_STAR " " : "   ") + snapshot.name + "##snapshot" + snapshot.name).c_str(), false);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s%zu values. Right-click: Save Current Values, Set as Start, Rename, Delete.%s",
                              isStart ? "Start snapshot. " : "", snapshot.values.size(),
                              project && project->isAnyScenePlaying() && isProjectMixer() ? " Double-click: transition to it now (Play)." : "");
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && project && project->isAnyScenePlaying() && isProjectMixer()) {
            AudioMixer::transitionTo(snapshot.name, 0.5f, true);
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Save Current Values")) {
                AudioMixerDef next = def;
                std::string opError;
                if (AudioMixerDocument::applyOp(next, {{"op", "set_snapshot"}, {"name", snapshot.name}, {"capture", true}}, &opError)) commit(next);
                else error = opError;
            }
            if (ImGui::MenuItem("Set as Start", nullptr, isStart)) {
                AudioMixerDef next = def;
                next.startSnapshot = isStart ? "" : snapshot.name;
                commit(next);
            }
            if (ImGui::MenuItem("Rename")) {
                renaming = key;
                renameFocus = true;
                snprintf(renameBuffer, sizeof(renameBuffer), "%s", snapshot.name.c_str());
            }
            if (ImGui::MenuItem("Delete")) {
                AudioMixerDef next = def;
                std::string opError;
                if (AudioMixerDocument::applyOp(next, {{"op", "remove_snapshot"}, {"name", snapshot.name}}, &opError)) commit(next);
                else error = opError;
            }
            ImGui::EndPopup();
        }
    }
    if (ImGui::SmallButton(ICON_FA_PLUS " Snapshot")) {
        AudioMixerDef next = def;
        std::string name = "Snapshot";
        for (int n = 2; next.findSnapshot(name); n++) name = "Snapshot " + std::to_string(n);
        std::string opError;
        if (AudioMixerDocument::applyOp(next, {{"op", "add_snapshot"}, {"name", name}}, &opError)) commit(next);
        else error = opError;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A snapshot of every fader and effect value as they are now");
}

void AudioMixerWindow::drawExposed() {
    if (def.exposed.empty()) {
        ImGui::TextDisabled("None. Right-click a fader or an");
        ImGui::TextDisabled("effect value: Expose to Script.");
        return;
    }
    const bool live = project && project->isAnyScenePlaying() && isProjectMixer();
    for (const AudioMixerExposedDef& exposed : def.exposed) {
        const std::string key = "exposed:" + exposed.name;
        ImGui::PushID(key.c_str());
        if (renaming == key) {
            ImGui::SetNextItemWidth(-1);
            if (renameFocus) {
                ImGui::SetKeyboardFocusHere();
                renameFocus = false;
            }
            if (ImGui::InputText("##renameExposed", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                AudioMixerDef next = def;
                std::string opError;
                if (std::string(renameBuffer) != exposed.name) {
                    if (AudioMixerDocument::applyOp(next, {{"op", "set_exposed"}, {"name", exposed.name}, {"new_name", std::string(renameBuffer)}}, &opError)) commit(next);
                    else error = opError;
                }
                renaming.clear();
            } else if (ImGui::IsItemDeactivated()) {
                renaming.clear();
            }
            ImGui::PopID();
            continue;
        }
        bool remember = exposed.remember;
        if (ImGui::Checkbox("##remember", &remember)) {
            AudioMixerDef next = def;
            for (AudioMixerExposedDef& entry : next.exposed) {
                if (entry.name == exposed.name) entry.remember = remember;
            }
            commit(next);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remember: values set by scripts come back on the next launch (UserSettings \"audio.mixer.%s\")", exposed.name.c_str());
        ImGui::SameLine();
        ImGui::Selectable(exposed.name.c_str(), false);
        if (ImGui::IsItemHovered()) {
            float now = 0.0f;
            const bool hasNow = live && AudioMixer::getTarget(exposed.target, now);
            ImGui::SetTooltip("%s\nAudioMixer.setFloat(\"%s\", value)%s", AudioMixerIO::describeTarget(def, exposed.target).c_str(), exposed.name.c_str(),
                              hasNow ? (std::string("\nNow: ") + std::to_string(now) + (AudioMixer::isTargetOverridden(exposed.target) ? " (set by script)" : "")).c_str() : "");
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Rename")) {
                renaming = key;
                renameFocus = true;
                snprintf(renameBuffer, sizeof(renameBuffer), "%s", exposed.name.c_str());
            }
            if (ImGui::MenuItem("Remove")) {
                AudioMixerDef next = def;
                std::string opError;
                if (AudioMixerDocument::applyOp(next, {{"op", "unexpose"}, {"name", exposed.name}}, &opError)) commit(next);
                else error = opError;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", AudioMixerIO::describeTarget(def, exposed.target).c_str());
        ImGui::PopID();
    }
}

void AudioMixerWindow::drawSidebar() {
    if (Theme::foldoutHeader("Groups", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (const AudioMixerGroupDef* master = def.master()) drawGroupNode(*master);
        if (ImGui::SmallButton(ICON_FA_PLUS " Group")) {
            AudioMixerDef next = def;
            const AudioMixerGroupDef* parent = next.findGroupById(selectedGroup);
            if (!parent) parent = next.master();
            AudioMixerGroupDef child;
            child.id = AudioMixerIO::newId(next);
            child.name = AudioMixerDocument::uniqueGroupName(next, "Group");
            child.parentId = parent->id;
            next.groups.push_back(child);
            if (commit(next)) {
                selectedGroup = child.id;
                renaming = "group:" + child.id;
                renameFocus = true;
                snprintf(renameBuffer, sizeof(renameBuffer), "%s", child.name.c_str());
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A group under the selected one (Master when none). Drag groups to move them; double-click to rename.");
    }
    if (Theme::foldoutHeader("Snapshots", ImGuiTreeNodeFlags_DefaultOpen)) drawSnapshots();
    if (Theme::foldoutHeader("Exposed Parameters", ImGuiTreeNodeFlags_DefaultOpen)) drawExposed();
}

// ---------------------------------------------------------------- strips

void AudioMixerWindow::drawMeter(const std::string& groupId, float height) {
    const bool live = isProjectMixer();
    const float db = live ? AudioMixer::getMeterDb(groupId) : AudioMath::minDb;
    PeakHold& hold = peaks[groupId];
    const double now = ImGui::GetTime();
    if (db >= hold.db || now - hold.time > 1.5) {
        hold.db = db;
        hold.time = now;
    }
    const float width = ImGui::GetFontSize() * 0.6f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(25, 25, 25, 255));
    const float fill = dbFraction(db);
    if (fill > 0.0f) draw->AddRectFilled(ImVec2(min.x, max.y - fill * height), max, meterColor(db));
    const float peak = dbFraction(hold.db);
    if (peak > 0.0f) {
        const float y = max.y - peak * height;
        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), meterColor(hold.db), 2.0f);
    }
    // 0 dB tick
    const float zero = max.y - dbFraction(0.0f) * height;
    draw->AddLine(ImVec2(min.x - 2, zero), ImVec2(max.x + 2, zero), IM_COL32(200, 200, 200, 120));
    ImGui::Dummy(ImVec2(width, height));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(live ? "%.1f dB (peak %.1f)" : "Meters show the project mixer while it plays", db, hold.db);
}

void AudioMixerWindow::drawEffect(const AudioMixerGroupDef& group, size_t index, float width) {
    const AudioMixerEffectDef& effect = group.effects[index];
    ImGui::PushID(effect.id.c_str());
    const bool unknown = effect.type == AudioEffectType::Unknown;
    const AudioEffectInfo* info = AudioEffects::find(effect.type);

    // header: bypass, name (drag to reorder), up/down, remove
    bool active = !effect.bypass;
    if (ImGui::Checkbox("##active", &active) && !unknown) {
        AudioMixerDef next = def;
        next.findGroup(group.id)->effects[index].bypass = !active;
        commit(next);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On / bypassed");
    ImGui::SameLine();
    ImGui::Selectable(unknown ? ("unknown: " + effect.typeName + " (kept)").c_str() : effectLabel(effect), false, 0,
                      ImVec2(width - ImGui::GetFontSize() * 4.2f, 0));
    if (!unknown && ImGui::BeginDragDropSource()) {
        const std::string payload = group.id + "/" + std::to_string(index);
        ImGui::SetDragDropPayload(EFFECT_PAYLOAD, payload.c_str(), payload.size() + 1);
        ImGui::Text("%s", effectLabel(effect));
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EFFECT_PAYLOAD)) {
            const std::string text = static_cast<const char*>(payload->Data);
            const size_t slash = text.find('/');
            if (slash != std::string::npos && text.substr(0, slash) == group.id) {
                const size_t from = (size_t)std::stoul(text.substr(slash + 1));
                AudioMixerDef next = def;
                std::vector<AudioMixerEffectDef>& effects = next.findGroup(group.id)->effects;
                if (from < effects.size() && from != index) {
                    AudioMixerEffectDef moved = effects[from];
                    effects.erase(effects.begin() + (long)from);
                    effects.insert(effects.begin() + (long)index, moved);
                    commit(next);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (info && info->costly && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\ncost: high (about 1-2%% of a mobile core)", info->description);
    else if (info && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDrag to reorder", info->description);
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_UP) && index > 0) {
        AudioMixerDef next = def;
        std::vector<AudioMixerEffectDef>& effects = next.findGroup(group.id)->effects;
        std::swap(effects[index], effects[index - 1]);
        commit(next);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_XMARK)) {
        AudioMixerDef next = def;
        AudioMixerDocument::removeEffect(next, group.id, index);
        commit(next);
        ImGui::PopID();
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");

    if (unknown || !info) {
        ImGui::PopID();
        return;
    }

    const bool live = project && project->isAnyScenePlaying() && isProjectMixer();
    std::vector<AudioEffectParamInfo> params = info->params;
    params.push_back(AudioEffects::wetParam());
    for (const AudioEffectParamInfo& param : params) {
        const std::string target = AudioMixerIO::paramTarget(effect.id, param.name);
        float value = AudioMixerIO::effectParam(effect, param);
        const std::string exposed = exposedName(target);
        char format[48];
        snprintf(format, sizeof(format), "%s%s %%.%df%s%s", exposed.empty() ? "" : "* ", param.label,
                 param.maxValue >= 1000.0f ? 0 : 2, param.unit[0] ? " " : "", param.unit);
        ImGui::SetNextItemWidth(width);
        const ImGuiSliderFlags flags = param.curve == AudioParamCurve::Frequency ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
        if (ImGui::SliderFloat(("##" + std::string(param.name)).c_str(), &value, param.minValue, param.maxValue, format, flags)) {
            AudioMixerDef next = def;
            AudioMixerIO::setAuthoredValue(next, target, value);
            preview(next);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) finishDrag();
        exposeMenu(target, param.name);
        if (ImGui::IsItemHovered()) {
            float now = 0.0f;
            if (live && AudioMixer::getTarget(target, now) && std::fabs(now - value) > 1e-3f) {
                ImGui::SetTooltip("Now %.2f%s%s", now, AudioMixer::isTargetOverridden(target) ? " (set by script)" : "",
                                  exposed.empty() ? "" : ("\nExposed as '" + exposed + "'").c_str());
            } else if (!exposed.empty()) {
                ImGui::SetTooltip("Exposed as '%s'. Right-click to unexpose.", exposed.c_str());
            }
        }
    }
    if (effect.type == AudioEffectType::Duck) {
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##sidechain", effect.sidechainGroup.empty() ? "Side-chain: none" : ("Side-chain: " + effect.sidechainGroup).c_str())) {
            for (const AudioMixerGroupDef* other : def.treeOrder()) {
                if (ImGui::Selectable(other->name.c_str(), other->name == effect.sidechainGroup)) {
                    AudioMixerDef next = def;
                    next.findGroup(group.id)->effects[index].sidechainGroup = other->name;
                    commit(next);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The group whose sound lowers this one");
    }
    ImGui::PopID();
}

void AudioMixerWindow::drawStrip(const AudioMixerGroupDef& group) {
    const float width = ImGui::GetFontSize() * 11.0f;
    ImGui::PushID(group.id.c_str());
    ImGui::BeginGroup();
    ImGui::BeginChild("##strip", ImVec2(width, 0), ImGuiChildFlags_Borders);
    const float inner = ImGui::GetContentRegionAvail().x;

    // name
    if (selectedGroup == group.id) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_PlotHistogram]);
    ImGui::TextUnformatted(group.name.c_str());
    if (selectedGroup == group.id) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) selectedGroup = group.id;
    const AudioMixerGroupDef* parent = def.findGroupById(group.parentId);
    if (parent) {
        ImGui::SameLine();
        ImGui::TextDisabled("> %s", parent->name.c_str());
    }

    // fader + meter
    const std::string volumeTarget = AudioMixerIO::volumeTarget(group.id);
    const float faderHeight = ImGui::GetFontSize() * 11.0f;
    float volume = group.volumeDb;
    const std::string exposed = exposedName(volumeTarget);
    if (ImGui::VSliderFloat("##fader", ImVec2(ImGui::GetFontSize() * 2.2f, faderHeight), &volume, AudioMath::minDb, AudioMath::maxDb, "")) {
        AudioMixerDef next = def;
        AudioMixerIO::setAuthoredValue(next, volumeTarget, volume);
        preview(next);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) finishDrag();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        AudioMixerDef next = def;
        AudioMixerIO::setAuthoredValue(next, volumeTarget, 0.0f);
        commit(next);
    }
    exposeMenu(volumeTarget, group.name + "Volume");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.1f dB%s\nDouble-click: 0 dB. Right-click: Expose to Script.", group.volumeDb,
                          exposed.empty() ? "" : ("\nExposed as '" + exposed + "'").c_str());
    }
    ImGui::SameLine();
    drawMeter(group.id, faderHeight);
    ImGui::SameLine();
    ImGui::BeginGroup();
    // M / S / B
    auto toggle = [&](const char* label, bool value, ImVec4 color, const char* tip, auto apply) {
        if (value) ImGui::PushStyleColor(ImGuiCol_Button, color);
        if (ImGui::Button(label, ImVec2(ImGui::GetFontSize() * 1.8f, 0))) {
            AudioMixerDef next = def;
            apply(*next.findGroup(group.id), !value);
            commit(next);
        }
        if (value) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    toggle("M", group.mute, ImVec4(0.75f, 0.25f, 0.2f, 1.0f), "Mute", [](AudioMixerGroupDef& g, bool v) { g.mute = v; });
    toggle("S", group.solo, ImVec4(0.75f, 0.65f, 0.15f, 1.0f), "Solo: only soloed groups (and the groups above and below them) are heard",
           [](AudioMixerGroupDef& g, bool v) { g.solo = v; });
    toggle("B", group.bypassEffects, ImVec4(0.2f, 0.45f, 0.8f, 1.0f), "Bypass the effects", [](AudioMixerGroupDef& g, bool v) { g.bypassEffects = v; });
    ImGui::EndGroup();

    // the dB field
    ImGui::SetNextItemWidth(inner);
    float typed = group.volumeDb;
    if (ImGui::DragFloat("##db", &typed, 0.1f, AudioMath::minDb, AudioMath::maxDb, typed <= AudioMath::minDb ? "-inf dB" : "%.1f dB")) {
        AudioMixerDef next = def;
        AudioMixerIO::setAuthoredValue(next, volumeTarget, typed);
        preview(next);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) finishDrag();
    if (project && project->isAnyScenePlaying() && isProjectMixer()) {
        float now = 0.0f;
        if (AudioMixer::getTarget(volumeTarget, now) && std::fabs(now - group.volumeDb) > 0.05f) {
            ImGui::TextDisabled("now %.1f dB%s", now, AudioMixer::isTargetOverridden(volumeTarget) ? " (script)" : "");
        }
        if (AudioMixer::getGroupMute(group.id) != group.mute) ImGui::TextDisabled("muted by script: %s", AudioMixer::getGroupMute(group.id) ? "yes" : "no");
    }

    // effects
    ImGui::SeparatorText("Effects");
    for (size_t i = 0; i < group.effects.size(); i++) {
        drawEffect(group, i, inner);
        ImGui::Spacing();
    }
    ImGui::BeginDisabled(group.effects.size() >= (size_t)AudioEffects::maxEffectsPerGroup);
    ImGui::SetNextItemWidth(inner);
    if (ImGui::BeginCombo("##addEffect", ICON_FA_PLUS " Add Effect")) {
        for (const AudioEffectInfo& info : AudioEffects::all()) {
            if (ImGui::Selectable(info.label)) {
                AudioMixerDef next = def;
                std::string opError;
                if (AudioMixerDocument::applyOp(next, {{"op", "add_effect"}, {"group", group.id}, {"type", info.name}}, &opError)) commit(next);
                else error = opError;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", info.description, info.costly ? "\ncost: high" : "");
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::PopID();
}

void AudioMixerWindow::drawStrips() {
    ImGui::BeginChild("##strips", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    bool first = true;
    for (const AudioMixerGroupDef* group : def.treeOrder()) {
        if (!first) ImGui::SameLine();
        first = false;
        drawStrip(*group);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- popups

void AudioMixerWindow::drawPopups() {
    if (openExposePopup) {
        ImGui::OpenPopup("Expose Parameter##AudioMixer");
        openExposePopup = false;
    }
    if (EditorModal::begin("Expose Parameter##AudioMixer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorModal::message("Expose " + AudioMixerIO::describeTarget(def, exposeTarget) + " to scripts as:");
        ImGui::Text("Expose %s to scripts as:", AudioMixerIO::describeTarget(def, exposeTarget).c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = EditorModal::inputText("##exposeName", "name", exposeName, sizeof(exposeName), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = exposeName;
        const bool taken = def.findExposed(name) != nullptr;
        if (taken) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "'%s' is exposed already", name.c_str());
        EditorModal::error(taken ? "'" + name + "' is exposed already" : name.empty() ? "The name is empty." : std::string());
        ImGui::TextDisabled("Lua: AudioMixer.setFloat(\"%s\", value)", name.c_str());
        ImGui::BeginDisabled(name.empty() || taken);
        if (EditorModal::button("Expose", "ok") || (enter && !name.empty() && !taken)) {
            AudioMixerDef next = def;
            next.exposed.push_back({name, exposeTarget, false});
            commit(next);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openDeletePopup) {
        ImGui::OpenPopup("Delete Group##AudioMixer");
        openDeletePopup = false;
    }
    if (EditorModal::begin("Delete Group##AudioMixer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const AudioMixerGroupDef* group = def.findGroupById(pendingDeleteGroup);
        if (!group) {
            ImGui::CloseCurrentPopup();
        } else {
            const size_t count = AudioMixerDocument::subtree(def, group->id).size();
            ImGui::Text("Delete '%s'%s?", group->name.c_str(), count > 1 ? (" and its " + std::to_string(count - 1) + " subgroup(s)").c_str() : "");
            ImGui::TextDisabled("Its Audio Sources play through Master until a group of that name exists again.");
            EditorModal::message("Delete '" + group->name + "'" + (count > 1 ? " and its " + std::to_string(count - 1) + " subgroup(s)" : std::string()) +
                                 "? Its Audio Sources play through Master until a group of that name exists again.");
            if (EditorModal::button("Delete", "yes")) {
                AudioMixerDef next = def;
                AudioMixerDocument::removeGroup(next, group->id);
                if (commit(next) && selectedGroup == pendingDeleteGroup) selectedGroup.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- the window

void AudioMixerWindow::show() {
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }

    if (!project || project->getProjectPath().empty()) {
        ImGui::TextDisabled("Open a project to edit its Audio Mixer.");
        ImGui::End();
        return;
    }

    resolveFile();
    // what last frame's widgets changed, then a drag the mouse let go of elsewhere
    applyStaged();
    if (dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        finishDrag();
        applyStaged();
    }
    reload(false);

    if (!loaded) {
        if (!loadError.empty()) {
            ImGui::TextWrapped("%s does not read: %s", FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(file)).c_str(), loadError.c_str());
            ImGui::TextDisabled("Fix the file (undo, or edit it as text); the game plays through an implicit Master meanwhile.");
        } else if (chosenFile.empty()) {
            ImGui::TextWrapped("The project has no Audio Mixer: every sound plays through an implicit Master at 0 dB.");
            if (ImGui::Button(ICON_FA_SLIDERS " Create Audio Mixer")) {
                fs::path created;
                std::string createError;
                if (!AudioMixerDocument::ensureProjectMixer(project, created, &createError)) error = createError;
                else {
                    file.clear();
                    resolveFile();
                    reload(true);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("assets/%s, set as the project's (Project Settings > Audio)", Project::defaultAudioMixerPath);
        } else {
            ImGui::TextDisabled("%s does not exist.", FileUtils::pathToGenericUtf8(file).c_str());
        }
        if (!error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error.c_str());
        ImGui::End();
        return;
    }

    drawToolbar();
    ImGui::Separator();

    const float sidebar = ImGui::GetFontSize() * 15.0f;
    ImGui::BeginChild("##AudioMixerSidebar", ImVec2(sidebar, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    drawSidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    drawStrips();
    drawPopups();

    ImGui::End();
}
