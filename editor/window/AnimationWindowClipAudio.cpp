// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's Audio lane (docs/specs/S40-timeline-audio.md R9-R11), under the events
// lane: a row per cue kind in use (Audio, Source, Snapshot). A sound or source cue is a block as
// long as its sound (S36's analysis) showing its cached waveform and its peak; a snapshot cue is a
// flag. Drag to move (snapped to the clip's frames, Alt for free), click for the cue's popover,
// right-click for Align Start / Peak / End to Playhead, Delete; sounds dropped from the Project
// window or the Sound Browser become cues at the drop time. The selection is kept by identity
// (time, kind, ordinal). Every edit is one controller commit named per S24.

#include "AnimationWindowClip.h"
#include "AnimationWindowClipUtil.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ClipAudio.h"
#include "animation/ClipBinding.h"
#include "audio/AudioMixer.h"
#include "audio/SoundDrop.h"
#include "audio/TimelineAudioPreview.h"
#include "component/AudioSourceComponent.h"
#include "util/EntityPath.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::clipui;

namespace {

    constexpr float kCueEpsilon = 1e-4f;

    const char* cueRowLabel(ClipCueKind kind){
        switch (kind){
            case ClipCueKind::Sound: return "Audio";
            case ClipCueKind::Source: return "Source";
            case ClipCueKind::Snapshot: return "Snapshot";
        }
        return "Audio";
    }

    const char* cueNoun(ClipCueKind kind){
        switch (kind){
            case ClipCueKind::Sound: return "Audio Cue";
            case ClipCueKind::Source: return "Source Cue";
            case ClipCueKind::Snapshot: return "Snapshot Cue";
        }
        return "Audio Cue";
    }

    int findCue(const std::vector<ClipAudioCue>& cues, const AnimationWindowClip::CueId& id){
        int ordinal = 0;
        for (size_t i = 0; i < cues.size(); i++){
            if (cues[i].kind != id.kind || std::fabs(cues[i].time - id.time) > kCueEpsilon) continue;
            if (ordinal == id.ordinal) return (int)i;
            ordinal++;
        }
        return -1;
    }

    // the cue of `kind` nearest to `time` (after a commit re-sorted the cues)
    int findNearestCue(const std::vector<ClipAudioCue>& cues, ClipCueKind kind, float time){
        int best = -1;
        float bestDistance = 1e9f;
        for (size_t i = 0; i < cues.size(); i++){
            if (cues[i].kind != kind) continue;
            const float distance = std::fabs(cues[i].time - time);
            if (distance < bestDistance){
                bestDistance = distance;
                best = (int)i;
            }
        }
        return best;
    }

    bool soundPayloadDragged(){
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload && SoundDrop::accepts(payload);
    }

}

bool AnimationWindowClip::alignedStart(CueAlign align, float playhead, double length, double peakMs, float& time, std::string* why){
    switch (align){
        case CueAlign::Start:
            time = playhead;
            break;
        case CueAlign::Peak:
            if (peakMs < 0.0){
                if (why) *why = "the sound has no analysed peak yet";
                return false;
            }
            time = (float)((double)playhead - peakMs / 1000.0);
            break;
        case CueAlign::End:
            if (!(length > 0.0)){
                if (why) *why = "the sound's length is not known yet";
                return false;
            }
            time = (float)((double)playhead - length);
            break;
    }
    if (time < -kCueEpsilon){
        if (why) *why = "it would start before 0 s: move the playhead later";
        return false;
    }
    time = std::max(0.0f, time);
    return true;
}

// ---------------------------------------------------------------- selection and edits

void AnimationWindowClip::selectCue(int index){
    std::shared_ptr<AnimationClipDocument> doc = controllerUsed ? document() : nullptr;
    if (!doc || index < 0 || index >= (int)doc->data().audio.size()){
        selectedCue = -1;
        cueSelected = false;
        return;
    }
    const std::vector<ClipAudioCue>& cues = doc->data().audio;
    selectedCue = index;
    cueSelected = true;
    selectedCueId.time = cues[(size_t)index].time;
    selectedCueId.kind = cues[(size_t)index].kind;
    selectedCueId.ordinal = 0;
    for (int i = 0; i < index; i++){
        if (cues[(size_t)i].kind == selectedCueId.kind && std::fabs(cues[(size_t)i].time - selectedCueId.time) <= kCueEpsilon) selectedCueId.ordinal++;
    }
    cueRevision = doc->revision();
    cueDocument = doc;
}

void AnimationWindowClip::resolveSelectedCue(){
    std::shared_ptr<AnimationClipDocument> doc = controllerUsed ? document() : nullptr;
    if (!cueSelected || !doc){
        selectedCue = -1;
        cueSelected = false;
        return;
    }
    if (doc == cueDocument.lock() && doc->revision() == cueRevision) return;
    selectedCue = doc == cueDocument.lock() ? findCue(doc->data().audio, selectedCueId) : -1;
    if (selectedCue < 0){
        cueSelected = false;
        return;
    }
    cueRevision = doc->revision();
}

bool AnimationWindowClip::addCue(const ClipAudioCue& cue, const std::string& undoName, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    const std::string problem = anim::validateCue(cue);
    if (!problem.empty()){
        if (error) *error = problem;
        return false;
    }
    ClipData data = doc->data();
    data.audio.push_back(cue);
    if (!controller().commit(data, undoName, "", error)) return false;
    selection.clear();
    refreshSelection();
    selectEvent(-1);
    selectCue(findNearestCue(document()->data().audio, cue.kind, std::max(0.0f, cue.time)));
    return true;
}

bool AnimationWindowClip::editCue(int index, const ClipAudioCue& cue, const std::string& undoName, const std::string& mergeKey, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().audio.size()){
        if (error) *error = "no such audio cue";
        return false;
    }
    const std::string problem = anim::validateCue(cue);
    if (!problem.empty()){
        if (error) *error = problem;
        return false;
    }
    ClipData data = doc->data();
    data.audio[(size_t)index] = cue;
    if (!controller().commit(data, undoName, mergeKey, error)) return false;
    selectCue(findNearestCue(document()->data().audio, cue.kind, std::max(0.0f, cue.time)));
    return true;
}

bool AnimationWindowClip::deleteCue(int index, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().audio.size()){
        if (error) *error = "no such audio cue";
        return false;
    }
    ClipData data = doc->data();
    const ClipCueKind kind = data.audio[(size_t)index].kind;
    data.audio.erase(data.audio.begin() + index);
    if (!controller().commit(data, std::string("Delete ") + cueNoun(kind), "", error)) return false;
    selectCue(-1);
    return true;
}

bool AnimationWindowClip::moveCue(int index, float time, const std::string& mergeKey, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().audio.size()){
        if (error) *error = "no such audio cue";
        return false;
    }
    ClipAudioCue cue = doc->data().audio[(size_t)index];
    cue.time = std::max(0.0f, time);
    return editCue(index, cue, std::string("Move ") + cueNoun(cue.kind), mergeKey, error);
}

std::string AnimationWindowClip::cueSound(const ClipAudioCue& cue) const{
    if (cue.kind == ClipCueKind::Sound) return cue.sound.path;
    if (cue.kind != ClipCueKind::Source) return std::string();
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    if (!scene || root == NULL_ENTITY) return std::string();
    const Entity bound = anim::resolvePath(scene, root, cue.path);
    const AudioSourceComponent* audio = bound != NULL_ENTITY ? scene->findComponent<AudioSourceComponent>(bound) : nullptr;
    if (!audio) return std::string();
    return audio->event.empty() ? audio->filename.path : audio->event.path;
}

namespace {

    // a cue's speed in the preview: its source's speed, and the preview's for a follow_speed cue
    double cueSpeedOf(const ClipAudioCue& cue, Scene* scene, Entity root, float playbackSpeed){
        double speed = cue.followSpeed ? std::max(1e-3, (double)std::fabs(playbackSpeed)) : 1.0;
        if (cue.kind == ClipCueKind::Source && scene && root != NULL_ENTITY){
            const Entity bound = anim::resolvePath(scene, root, cue.path);
            const AudioSourceComponent* audio = bound != NULL_ENTITY ? scene->findComponent<AudioSourceComponent>(bound) : nullptr;
            if (audio && audio->speed > 0.0f) speed *= audio->speed;
        }
        return speed;
    }

}

double AnimationWindowClip::cueLength(const ClipAudioCue& cue) const{
    const std::string sound = cueSound(cue);
    if (sound.empty()) return 0.0;
    const TimelineSoundInfo info = TimelineSounds::get(project, sound);
    return info.length / cueSpeedOf(cue, valueScene(), valueRoot(), controller().getPlaybackSpeed());
}

std::string AnimationWindowClip::alignProblem(int index, CueAlign align, float* time) const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().audio.size()) return "select an audio cue";
    if (doc->isReadOnly()) return "read-only clip: extract it to edit it";
    const ClipAudioCue& cue = doc->data().audio[(size_t)index];
    if (cue.kind == ClipCueKind::Snapshot && align != CueAlign::Start) return "a snapshot cue has no sound";
    double length = 0.0;
    double peakMs = -1.0;
    if (cue.kind != ClipCueKind::Snapshot){
        const std::string sound = cueSound(cue);
        if (sound.empty()) return "the cue plays no sound";
        const TimelineSoundInfo info = TimelineSounds::get(project, sound);
        if (info.pending && align != CueAlign::Start) return "the sound is still being analysed";
        const double speed = cueSpeedOf(cue, valueScene(), valueRoot(), controller().getPlaybackSpeed());
        length = info.length / speed;
        peakMs = info.peakMs >= 0.0 ? info.peakMs / speed : -1.0;
    }
    float start = 0.0f;
    std::string why;
    if (!alignedStart(align, controller().getTime(), length, peakMs, start, &why)) return why;
    if (time) *time = start;
    return std::string();
}

bool AnimationWindowClip::alignCue(int index, CueAlign align, std::string* error){
    float time = 0.0f;
    const std::string problem = alignProblem(index, align, &time);
    if (!problem.empty()){
        if (error) *error = problem;
        return false;
    }
    ClipAudioCue cue = document()->data().audio[(size_t)index];
    cue.time = time;
    const char* name = align == CueAlign::Start ? "Align Start to Playhead" : align == CueAlign::Peak ? "Align Peak to Playhead" : "Align End to Playhead";
    return editCue(index, cue, name, "", error);
}

bool AnimationWindowClip::dropSounds(const std::vector<std::string>& assetPaths, float time, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    if (doc->isReadOnly()){
        if (error) *error = "read-only clip: extract it to edit it";
        return false;
    }
    ClipData data = doc->data();
    ClipAudioCue last;
    for (const std::string& path : assetPaths){
        if (!Util::isAudioFile(path) && !Util::isSoundEventFile(path)) continue;
        ClipAudioCue cue;
        cue.time = std::max(0.0f, time);
        cue.kind = ClipCueKind::Sound;
        cue.sound = TimelineSounds::reference(project, path);
        data.audio.push_back(cue);
        last = cue;
    }
    if (data.audio.size() == doc->data().audio.size()){
        if (error) *error = "drop an audio clip or a .soundevent";
        return false;
    }
    if (!controller().commit(data, "Drop Audio Cue", "", error)) return false;
    selection.clear();
    refreshSelection();
    selectCue(findNearestCue(document()->data().audio, ClipCueKind::Sound, last.time));
    return true;
}

// ---------------------------------------------------------------- the lane

std::vector<ClipCueKind> AnimationWindowClip::audioLaneKinds(const ClipData& data) const{
    std::vector<ClipCueKind> kinds;
    for (ClipCueKind kind : {ClipCueKind::Sound, ClipCueKind::Source, ClipCueKind::Snapshot}){
        for (const ClipAudioCue& cue : data.audio){
            if (cue.kind == kind){
                kinds.push_back(kind);
                break;
            }
        }
    }
    // a drop target while sounds are dragged over a clip without sound cues
    if (std::find(kinds.begin(), kinds.end(), ClipCueKind::Sound) == kinds.end() && soundPayloadDragged()){
        kinds.insert(kinds.begin(), ClipCueKind::Sound);
    }
    return kinds;
}

float AnimationWindowClip::audioLaneHeight(const ClipData& data) const{
    const SheetMetrics m = sheetMetrics();
    return (float)audioLaneKinds(data).size() * (m.eventsHeight + 8.0f * m.s);
}

void AnimationWindowClip::drawAudioLaneLabels(ImDrawList* drawList, ImVec2 origin, float width){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    const SheetMetrics m = sheetMetrics();
    const float rowHeight = m.eventsHeight + 8.0f * m.s;
    const std::vector<ClipCueKind> kinds = audioLaneKinds(doc->data());
    for (size_t r = 0; r < kinds.size(); r++){
        const float y = origin.y + (float)r * rowHeight;
        drawList->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + rowHeight), color(Theme::Colors::ToolbarBackground));
        const ImVec4 tint = kinds[r] == ClipCueKind::Snapshot ? Theme::Colors::AnimationSnapshot : Theme::Colors::AnimationAudio;
        drawList->AddRectFilled(ImVec2(origin.x + 4.0f * m.s, y + 4.0f * m.s), ImVec2(origin.x + 7.0f * m.s, y + rowHeight - 4.0f * m.s), color(tint));
        drawList->AddText(ImVec2(origin.x + 12.0f * m.s, y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f), color(Theme::Colors::SubtleText),
                          (std::string(ICON_FA_VOLUME_HIGH " ") + cueRowLabel(kinds[r])).c_str());
    }
}

void AnimationWindowClip::drawAudioLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float timeOriginX){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || height <= 0.0f) return;
    resolveSelectedCue();
    ImGuiIO& io = ImGui::GetIO();
    const SheetMetrics m = sheetMetrics();
    const float rowHeight = m.eventsHeight + 8.0f * m.s;
    const ClipData& data = doc->data();
    const std::vector<ClipCueKind> kinds = audioLaneKinds(data);
    auto xAt = [&](float t){ return timeOriginX + t * pixelsPerSecond - scrollX; };
    auto timeAt = [&](float x){ return std::max(0.0f, (x - timeOriginX + scrollX) / pixelsPerSecond); };
    auto rowOf = [&](ClipCueKind kind){
        for (size_t r = 0; r < kinds.size(); r++){
            if (kinds[r] == kind) return (int)r;
        }
        return -1;
    };

    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color(ImVec4(0.17f, 0.17f, 0.17f, 1.0f)));
    for (size_t r = 1; r < kinds.size(); r++){
        const float y = origin.y + (float)r * rowHeight;
        drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), color(Theme::Colors::ToolbarBorder));
    }
    drawList->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f), color(Theme::Colors::ToolbarBorder));
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##clip_audio", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool laneHovered = ImGui::IsItemHovered();
    UIProbe::item("clip.audio.lane");

    int hovered = -1;
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    for (size_t i = 0; i < data.audio.size(); i++){
        const ClipAudioCue& cue = data.audio[i];
        const int row = rowOf(cue.kind);
        if (row < 0) continue;
        const float top = origin.y + (float)row * rowHeight + 2.0f * m.s;
        const float bottom = top + rowHeight - 4.0f * m.s;
        const float x = xAt(cue.time);
        const bool isSelected = (int)i == selectedCue;
        float right = x + m.eventWidth;
        if (cue.kind == ClipCueKind::Snapshot){
            // a flag with its name
            const ImU32 fill = color(isSelected ? selectedKeyColor() : Theme::Colors::AnimationSnapshot);
            drawList->AddRectFilled(ImVec2(x - 1.0f * m.s, top), ImVec2(x + 1.0f * m.s, bottom), fill);
            const std::string label = anim::cueLabel(cue);
            const float textWidth = ImGui::CalcTextSize(label.c_str()).x;
            drawList->AddRectFilled(ImVec2(x, top), ImVec2(x + textWidth + 8.0f * m.s, top + ImGui::GetTextLineHeight() + 2.0f * m.s), fill, 2.0f);
            drawList->AddText(ImVec2(x + 4.0f * m.s, top + 1.0f * m.s), IM_COL32(255, 255, 255, 240), label.c_str());
            right = x + textWidth + 8.0f * m.s;
        }else{
            const std::string sound = cueSound(cue);
            const TimelineSoundInfo info = TimelineSounds::get(project, sound);
            const double speed = cueSpeedOf(cue, valueScene(), valueRoot(), controller().getPlaybackSpeed());
            const double length = info.length / speed;
            right = xAt(cue.time + (float)std::max(length, 0.05));
            TimelineSoundInfo shown = info;
            if (shown.peakMs >= 0.0) shown.peakMs /= speed;
            std::string label = anim::cueLabel(cue);
            if (sound.empty()) label += " (no sound)";
            else if (!info.error.empty() && !info.pending) label += " (missing)";
            const ImU32 fill = color(sound.empty() || (!info.error.empty() && !info.pending) ? Theme::Colors::WarningText : Theme::Colors::AnimationAudio);
            TimelineSounds::drawBlock(drawList, project, shown, ImVec2(x, top), ImVec2(std::max(right, x + 4.0f * m.s), bottom),
                                      speed / (double)pixelsPerSecond, fill, isSelected, label);
        }
        // the cue's block (UI tests: its edges, drawn whole inside the lane)
        if (x >= origin.x && right <= origin.x + width) UIProbe::rect("clip.audio.cue." + std::to_string(i), x, top, right, bottom);
        if (laneHovered && io.MousePos.y >= top && io.MousePos.y <= bottom && io.MousePos.x >= x - 3.0f * m.s && io.MousePos.x <= right){
            hovered = (int)i;
        }
    }
    drawList->PopClipRect();

    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if (hovered >= 0){
            selection.clear();
            refreshSelection();
            selectEvent(-1);
            selectCue(hovered);
            drag = Drag::PendingCue;
            dragStart = io.MousePos;
            cueDragBase = data.audio[(size_t)hovered].time;
        }else{
            selectCue(-1);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !doc->isReadOnly()){
        if (drag == Drag::PendingCue && std::fabs(io.MousePos.x - dragStart.x) > 3.0f * m.s){
            drag = Drag::Cue;
            cueMergeKey = "clipcue#" + std::to_string(++cueDragSerial);
        }
        if (drag == Drag::Cue && selectedCue >= 0 && selectedCue < (int)data.audio.size()){
            float time = std::max(0.0f, cueDragBase + (io.MousePos.x - dragStart.x) / pixelsPerSecond);
            // snapped to the clip's frames; Alt places it freely
            if (!io.KeyAlt) time = snapToFrame(time);
            if (std::fabs(data.audio[(size_t)selectedCue].time - time) > kCueEpsilon){
                std::string error;
                if (!moveCue(selectedCue, time, cueMergeKey, &error)) setNotice(error, true);
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag == Drag::PendingCue || drag == Drag::Cue)){
        if (drag == Drag::PendingCue && selectedCue >= 0) openCueEditor = true;
        drag = Drag::None;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
        contextTime = snapToFrame(timeAt(io.MousePos.x));
        if (hovered >= 0) selectCue(hovered);
        ImGui::OpenPopup(hovered >= 0 ? "##clip_cue_menu" : "##clip_audio_menu");
    }
    if (hovered >= 0 && drag == Drag::None){
        const ClipAudioCue& cue = data.audio[(size_t)hovered];
        ImGui::SetTooltip("%s: %s at %s", cueNoun(cue.kind), anim::cueLabel(cue).c_str(), formatBoth(cue.time, frameRate(), showFrames).c_str());
    }
    if (ImGui::BeginPopup("##clip_audio_menu")){
        if (ImGui::MenuItem("Add Cue...", nullptr, false, !doc->isReadOnly())){
            controller().setTime(contextTime);
            addPropertyCueKind = (int)ClipCueKind::Sound;
            openAddProperty = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##clip_cue_menu")){
        if (ImGui::MenuItem("Edit Cue...")) openCueEditor = true;
        const struct { CueAlign align; const char* name; } aligns[] = {
            {CueAlign::Start, "Align Start to Playhead"}, {CueAlign::Peak, "Align Peak to Playhead"}, {CueAlign::End, "Align End to Playhead"}};
        for (const auto& entry : aligns){
            const std::string problem = alignProblem(selectedCue, entry.align, nullptr);
            const char* shortcut = entry.align == CueAlign::Peak ? "Shift+A" : nullptr;
            if (ImGui::MenuItem(entry.name, shortcut, false, problem.empty())){
                std::string error;
                if (!alignCue(selectedCue, entry.align, &error)) setNotice(error, true);
            }
            if (!problem.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", problem.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Cue", "Del", false, !doc->isReadOnly())){
            std::string error;
            if (!deleteCue(selectedCue, &error)) setNotice(error, true);
        }
        ImGui::EndPopup();
    }

    // sounds dropped on the lane: a cue each at the drop time (built-in cards imported first)
    if (!doc->isReadOnly() && ImGui::BeginDragDropTargetCustom(ImRect(origin, ImVec2(origin.x + width, origin.y + height)), ImGui::GetID("##clip_audio_drop"))){
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (payload && SoundDrop::accepts(payload)){
            const float dropTime = io.KeyAlt ? timeAt(io.MousePos.x) : snapToFrame(timeAt(io.MousePos.x));
            const float dropX = xAt(dropTime);
            drawList->AddLine(ImVec2(dropX, origin.y), ImVec2(dropX, origin.y + height), color(Theme::Colors::AnimationAudio), 2.0f);
            if (const ImGuiPayload* accepted = ImGui::AcceptDragDropPayload(payload->DataType)){
                std::vector<std::string> paths;
                std::string error;
                if (!SoundDrop::resolve(project, accepted, paths, error) || !dropSounds(paths, dropTime, &error)) setNotice(error, true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
}

// ---------------------------------------------------------------- the cue's popover

void AnimationWindowClip::drawCueEditor(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    resolveSelectedCue();
    const bool valid = doc && selectedCue >= 0 && selectedCue < (int)doc->data().audio.size();
    if (openCueEditor){
        openCueEditor = false;
        if (valid) ImGui::OpenPopup("##clip_cue_editor");
    }
    if (!ImGui::BeginPopup("##clip_cue_editor")) return;
    if (!valid){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ClipAudioCue cue = doc->data().audio[(size_t)selectedCue];
    const ClipAudioCue before = cue;
    std::string undoName = std::string("Edit ") + cueNoun(cue.kind);
    std::string mergeKey;
    ImGui::BeginDisabled(doc->isReadOnly());
    ImGui::Text(ICON_FA_VOLUME_HIGH " %s", cueNoun(cue.kind));
    const float fps = frameRate();
    const float fieldWidth = Theme::dpi(170.0f);
    auto label = [](const char* text){
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(Theme::dpi(110.0f));
    };
    auto dragKey = [&](){
        if (ImGui::IsItemActivated()) cueMergeKey = "clipcue#" + std::to_string(++cueDragSerial);
        mergeKey = cueMergeKey;
    };

    label("Time");
    ImGui::SetNextItemWidth(Theme::dpi(80.0f));
    if (showFrames){
        int frame = (int)std::lround(cue.time * fps);
        if (ImGui::DragInt("##time", &frame, 0.2f, 0, (int)(kMaxTime * fps))) cue.time = (float)std::max(0, frame) / fps;
    }else{
        ImGui::DragFloat("##time", &cue.time, 1.0f / fps, 0.0f, kMaxTime, "%.3fs");
    }
    dragKey();

    Scene* scene = valueScene();
    const Entity root = valueRoot();
    // the GameObjects of the hierarchy (with an AudioSource for a source cue)
    auto pathCombo = [&](const char* id, std::string& path, bool sourcesOnly){
        ImGui::SetNextItemWidth(fieldWidth);
        if (!ImGui::BeginCombo(id, path.empty() ? "(root)" : path.c_str())) return false;
        bool changed = false;
        if (scene && root != NULL_ENTITY){
            std::function<void(Entity)> visit = [&](Entity entity){
                std::string relative;
                if (EntityPath::relative(scene, root, entity, relative) &&
                    (!sourcesOnly || scene->findComponent<AudioSourceComponent>(entity))){
                    if (ImGui::Selectable(relative.empty() ? "(root)" : relative.c_str(), relative == path)){
                        path = relative;
                        changed = true;
                    }
                }
                for (Entity child : anim::getChildren(scene, entity)) visit(child);
            };
            visit(root);
        }
        ImGui::EndCombo();
        return changed;
    };

    if (cue.kind == ClipCueKind::Sound){
        label("Sound");
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::BeginCombo("##sound", cue.sound.path.empty() ? "(none)" : cue.sound.path.c_str())){
            for (const std::string& path : TimelineSounds::projectSounds(project, "")){
                if (ImGui::Selectable(path.c_str(), path == cue.sound.path)) cue.sound = TimelineSounds::reference(project, path);
            }
            ImGui::EndCombo();
        }
        label("Spatial");
        ImGui::Checkbox("##spatial", &cue.spatial);
        ImGui::SetItemTooltip("Follows a GameObject of the animated hierarchy in 3D (off: 2D)");
        if (cue.spatial){
            label("Follow");
            pathCombo("##path", cue.path, false);
        }
        label("Volume (dB)");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("##volume", &cue.volumeDb, anim::CUE_MIN_VOLUME_DB, anim::CUE_MAX_VOLUME_DB, "%.1f dB");
        dragKey();
        label("Pitch");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("##pitch", &cue.pitchSemitones, -anim::CUE_MAX_PITCH_SEMITONES, anim::CUE_MAX_PITCH_SEMITONES, "%.1f st");
        dragKey();
        label("Output Group");
        char group[128];
        std::snprintf(group, sizeof(group), "%s", cue.group.c_str());
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputTextWithHint("##group", "the event's, else Master", group, sizeof(group));
        if (ImGui::IsItemDeactivatedAfterEdit()) cue.group = group;
    }else if (cue.kind == ClipCueKind::Source){
        label("Audio Source");
        pathCombo("##source", cue.path, true);
    }else{
        label("Snapshot");
        ImGui::SetNextItemWidth(fieldWidth);
        const std::vector<std::string> snapshots = AudioMixer::getSnapshotNames();
        const bool known = std::find(snapshots.begin(), snapshots.end(), cue.snapshot) != snapshots.end();
        const std::string shown = known ? cue.snapshot : cue.snapshot + " (missing)";
        if (ImGui::BeginCombo("##snapshot", shown.c_str())){
            for (const std::string& name : snapshots){
                if (ImGui::Selectable(name.c_str(), name == cue.snapshot)) cue.snapshot = name;
            }
            ImGui::EndCombo();
        }
        label("Transition");
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        ImGui::DragFloat("##seconds", &cue.seconds, 0.01f, 0.0f, 60.0f, "%.2fs");
        dragKey();
        label("Time Scale");
        int unscaled = cue.unscaled ? (*cue.unscaled ? 2 : 1) : 0;
        const char* modes[] = {"Mixer's", "Scaled", "Unscaled"};
        ImGui::SetNextItemWidth(Theme::dpi(110.0f));
        if (ImGui::Combo("##unscaled", &unscaled, modes, 3)){
            if (unscaled == 0) cue.unscaled.reset();
            else cue.unscaled = unscaled == 2;
        }
    }
    if (cue.kind != ClipCueKind::Snapshot){
        label("Stop");
        int stop = cue.stop == ClipCueStop::StateExit ? 1 : 0;
        const char* stops[] = {"Plays to its end", "On state exit"};
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::Combo("##stop", &stop, stops, 2)) cue.stop = stop == 1 ? ClipCueStop::StateExit : ClipCueStop::None;
        label("Follow Speed");
        ImGui::Checkbox("##follow", &cue.followSpeed);
        ImGui::SetItemTooltip("The pitch also follows the state's speed");
    }
    label("Name");
    char name[128];
    std::snprintf(name, sizeof(name), "%s", cue.name.c_str());
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputTextWithHint("##name", "a label", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit()) cue.name = name;

    if (cue != before){
        if (cue.time != before.time) undoName = std::string("Move ") + cueNoun(cue.kind);
        std::string error;
        if (!editCue(selectedCue, cue, undoName, mergeKey, &error)) setNotice(error, true);
    }
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_TRASH_CAN " Delete")){
        std::string error;
        if (!deleteCue(selectedCue, &error)) setNotice(error, true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- Add Property

bool AnimationWindowClip::drawAddCueItems(const std::string& needle){
    bool done = false;
    std::string error;
    const float time = snapToFrame(controller().getTime());
    ImGui::PushID("##timeline_audio");
    if (addPropertyCueKind >= 0) ImGui::SetNextItemOpen(true);
    if (ImGui::TreeNodeEx("##audio", 0, ICON_FA_VOLUME_HIGH " Timeline Audio")){
        if (addPropertyCueKind == (int)ClipCueKind::Sound) ImGui::SetNextItemOpen(true);
        if (ImGui::TreeNodeEx("##sound", 0, "Audio Cue")){
            const std::vector<std::string> sounds = TimelineSounds::projectSounds(project, needle);
            if (sounds.empty()) ImGui::TextDisabled("No audio clip or Sound Event in the project");
            for (const std::string& path : sounds){
                if (ImGui::Selectable(path.c_str())){
                    ClipAudioCue cue;
                    cue.time = time;
                    cue.sound = TimelineSounds::reference(project, path);
                    if (!addCue(cue, "Add Audio Cue", &error)) setNotice(error, true);
                    done = true;
                }
            }
            ImGui::TreePop();
        }
        if (addPropertyCueKind == (int)ClipCueKind::Snapshot) ImGui::SetNextItemOpen(true);
        if (ImGui::TreeNodeEx("##snapshot", 0, "Snapshot Cue")){
            const std::vector<std::string> snapshots = AudioMixer::getSnapshotNames();
            if (snapshots.empty()) ImGui::TextDisabled("The project's mixer has no snapshot");
            for (const std::string& name : snapshots){
                if (ImGui::Selectable(name.c_str())){
                    ClipAudioCue cue;
                    cue.time = time;
                    cue.kind = ClipCueKind::Snapshot;
                    cue.snapshot = name;
                    if (!addCue(cue, "Add Snapshot Cue", &error)) setNotice(error, true);
                    done = true;
                }
            }
            ImGui::TreePop();
        }
        if (addPropertyCueKind == (int)ClipCueKind::Source) ImGui::TextDisabled("Source Cue: under a GameObject with an Audio Source below");
        ImGui::TreePop();
    }
    ImGui::PopID();
    return done;
}

bool AnimationWindowClip::drawSourceCueItem(Entity entity, const std::string& path){
    Scene* scene = valueScene();
    if (!scene || !scene->findComponent<AudioSourceComponent>(entity)) return false;
    ImGui::PushID("##source_cue");
    bool done = false;
    if (ImGui::Selectable("Audio Source : Source Cue")){
        ClipAudioCue cue;
        cue.time = snapToFrame(controller().getTime());
        cue.kind = ClipCueKind::Source;
        cue.path = path;
        std::string error;
        if (!addCue(cue, "Add Source Cue", &error)) setNotice(error, true);
        done = true;
    }
    ImGui::SetItemTooltip("Starts this Audio Source at the playhead: its clip or event, its group and 3D settings");
    ImGui::PopID();
    return done;
}
