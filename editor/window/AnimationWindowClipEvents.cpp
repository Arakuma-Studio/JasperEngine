// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's events lane (animation clips spec 4.4, 3.5): markers under the ruler
// (Add Event at the playhead or at a right-click, drag to retime snapped to frames, select), and
// the selected event's editor under the sheet (function with a combo of the methods the root's
// Lua scripts define, the args list; a function no script defines shows in WarningText). Each
// edit is one controller commit (project history); a drag merges into one step.

#include "AnimationWindowClip.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimationClipDocument.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr float kEventEpsilon = 1e-4f;

    bool isIdentifier(const std::string& name){
        if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
        for (char ch : name){
            if (!(std::isalnum((unsigned char)ch) || ch == '_')) return false;
        }
        return true;
    }

    // the index of the event `function` at `time` in `data` after a commit re-sorted them
    int findEvent(const ClipData& data, const std::string& function, float time){
        int best = -1;
        float bestDistance = 1e9f;
        for (size_t i = 0; i < data.events.size(); i++){
            if (data.events[i].function != function) continue;
            const float distance = std::fabs(data.events[i].time - time);
            if (distance < bestDistance){
                bestDistance = distance;
                best = (int)i;
            }
        }
        return best;
    }

}

std::vector<std::string> AnimationWindowClip::scriptFunctions() const{
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    AnimationClipController& c = controller();
    const uint32_t id = c.isPreviewing() ? c.getPreviewSceneId() : c.getSceneId();
    SceneProject* sceneProject = id ? project->getScene(id) : nullptr;
    if (!scene || !sceneProject || root == NULL_ENTITY) return {};
    return ClipValidation::scriptFunctions(project, sceneProject, root);
}

bool AnimationWindowClip::addEvent(float time, const std::string& function, const std::vector<AnimationEventArg>& args, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    std::string name = function;
    if (name.empty()){
        // the first script method, else a placeholder the user renames
        const std::vector<std::string> functions = scriptFunctions();
        name = functions.empty() ? "onAnimationEvent" : functions.front();
    }
    if (!isIdentifier(name)){
        if (error) *error = "an event's function is a Lua identifier (letters, digits, _)";
        return false;
    }
    if (args.size() > MAX_ANIMATION_EVENT_ARGS){
        if (error) *error = "an event has at most " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " args";
        return false;
    }
    ClipData data = doc->data();
    ClipEventData event;
    event.time = std::max(0.0f, time);
    event.function = name;
    event.args = args;
    data.events.push_back(event);
    if (!controller().commit(data, "Add Animation Event", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
    selectedEvent = findEvent(document()->data(), name, event.time);
    selection.clear();
    return true;
}

bool AnimationWindowClip::editEvent(int index, const ClipEventData& event, const std::string& mergeKey, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().events.size()){
        if (error) *error = "no such event";
        return false;
    }
    if (!isIdentifier(event.function)){
        if (error) *error = "an event's function is a Lua identifier (letters, digits, _)";
        return false;
    }
    if (event.args.size() > MAX_ANIMATION_EVENT_ARGS){
        if (error) *error = "an event has at most " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " args";
        return false;
    }
    ClipData data = doc->data();
    data.events[(size_t)index] = event;
    data.events[(size_t)index].time = std::max(0.0f, event.time);
    if (!controller().commit(data, "Edit Animation Event", mergeKey, error)) return false;
    controller().setLastActionWasClipEdit(true);
    selectedEvent = findEvent(document()->data(), event.function, std::max(0.0f, event.time));
    return true;
}

bool AnimationWindowClip::deleteEvent(int index, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().events.size()){
        if (error) *error = "no such event";
        return false;
    }
    ClipData data = doc->data();
    data.events.erase(data.events.begin() + index);
    if (!controller().commit(data, "Delete Animation Event", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
    selectedEvent = -1;
    return true;
}

bool AnimationWindowClip::moveEvent(int index, float time, const std::string& mergeKey, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().events.size()){
        if (error) *error = "no such event";
        return false;
    }
    ClipEventData event = doc->data().events[(size_t)index];
    event.time = std::max(0.0f, time);
    return editEvent(index, event, mergeKey, error);
}

void AnimationWindowClip::drawEventsLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float keyAreaX){
    std::shared_ptr<AnimationClipDocument> doc = document();
    ImGuiIO& io = ImGui::GetIO();
    const float s = Theme::dpiScale();
    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(44, 44, 44, 255));
    drawList->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                      ImGui::GetColorU32(Theme::Colors::ToolbarBorder));
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##clip_events", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (!doc) return;
    const ClipData& data = doc->data();
    auto xAt = [&](float t){ return keyAreaX + t * pixelsPerSecond - scrollX; };
    auto timeAt = [&](float x){ return std::max(0.0f, (x - keyAreaX + scrollX) / pixelsPerSecond); };

    // the handler report of each event (WarningText markers when no script defines it)
    auto handlerMissing = [&](size_t index){
        for (const EventReport& entry : report.events){
            if (entry.event == index) return entry.status == EventHandlerStatus::NotFound;
        }
        return false;
    };

    int hovered = -1;
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    for (size_t i = 0; i < data.events.size(); i++){
        const float x = xAt(data.events[i].time);
        const bool selected = (int)i == selectedEvent;
        ImU32 fill = selected ? ImGui::GetColorU32(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : IM_COL32(225, 225, 225, 255);
        if (!selected && handlerMissing(i)) fill = ImGui::GetColorU32(Theme::Colors::WarningText);
        // a flag: a thin pole and a rectangle (Unity's event marker)
        const float w = 5.0f * s;
        drawList->AddRectFilled(ImVec2(x - 1.0f * s, origin.y + 2.0f * s), ImVec2(x + 1.0f * s, origin.y + height - 2.0f * s), fill);
        drawList->AddRectFilled(ImVec2(x, origin.y + 2.0f * s), ImVec2(x + w, origin.y + height * 0.55f), fill);
        if (ImGui::IsItemHovered() && std::fabs(io.MousePos.x - (x + w * 0.5f)) <= w + 2.0f * s) hovered = (int)i;
    }
    drawList->PopClipRect();

    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if (hovered >= 0){
            selectedEvent = hovered;
            selection.clear();
            selectedRefs.clear();
            drag = Drag::PendingEvent;
            dragStart = io.MousePos;
            eventDragBase = data.events[(size_t)hovered].time;
        }else{
            selectedEvent = -1;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !doc->isReadOnly()){
        if (drag == Drag::PendingEvent && std::fabs(io.MousePos.x - dragStart.x) > 3.0f * s){
            drag = Drag::Event;
            eventMergeKey = "clipevent#" + std::to_string(++eventDragSerial);
        }
        if (drag == Drag::Event && selectedEvent >= 0){
            const float time = snapToFrame(std::max(0.0f, eventDragBase + (io.MousePos.x - dragStart.x) / pixelsPerSecond));
            if (selectedEvent < (int)data.events.size() && std::fabs(data.events[(size_t)selectedEvent].time - time) > kEventEpsilon){
                std::string error;
                if (!moveEvent(selectedEvent, time, eventMergeKey, &error)) setNotice(error, true);
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag == Drag::PendingEvent || drag == Drag::Event)) drag = Drag::None;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
        contextTime = snapToFrame(timeAt(io.MousePos.x));
        if (hovered >= 0) selectedEvent = hovered;
        ImGui::OpenPopup(hovered >= 0 ? "##clip_event_menu" : "##clip_lane_menu");
    }
    if (hovered >= 0 && drag == Drag::None){
        const ClipEventData& event = data.events[(size_t)hovered];
        ImGui::SetTooltip("%s()%s at %.3fs", event.function.c_str(), handlerMissing((size_t)hovered) ? " (no script defines it)" : "",
                          event.time);
    }
    if (ImGui::BeginPopup("##clip_lane_menu")){
        if (ImGui::MenuItem("Add Event", nullptr, false, !doc->isReadOnly())){
            std::string error;
            if (!addEvent(contextTime, "", {}, &error)) setNotice(error, true);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##clip_event_menu")){
        if (ImGui::MenuItem("Delete Event", "Del", false, !doc->isReadOnly())){
            std::string error;
            if (!deleteEvent(selectedEvent, &error)) setNotice(error, true);
        }
        ImGui::EndPopup();
    }
    // the hit area of the lane is the item the caller checks for hover (wheel zoom)
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
}

float AnimationWindowClip::eventEditorHeight() const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || selectedEvent < 0 || selectedEvent >= (int)doc->data().events.size()) return 0.0f;
    const float line = ImGui::GetFrameHeightWithSpacing();
    return line * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
}

void AnimationWindowClip::drawEventEditor(SceneProject* sceneProject){
    (void)sceneProject;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || selectedEvent < 0 || selectedEvent >= (int)doc->data().events.size()) return;
    ClipEventData event = doc->data().events[(size_t)selectedEvent];
    const bool readOnly = doc->isReadOnly();
    std::string error;
    ImGui::Separator();
    ImGui::PushID("clip_event_editor");
    ImGui::BeginDisabled(readOnly);
    ImGui::AlignTextToFramePadding();
    ImGui::Text(ICON_FA_FLAG " Event");
    ImGui::SameLine();
    float time = event.time;
    ImGui::SetNextItemWidth(Theme::dpi(70.0f));
    if (ImGui::DragFloat("##time", &time, 0.01f, 0.0f, 3600.0f, "%.3fs")){
        if (ImGui::IsItemActivated()) eventMergeKey = "clipevent#" + std::to_string(++eventDragSerial);
        event.time = std::max(0.0f, time);
        if (!editEvent(selectedEvent, event, eventMergeKey, &error)) setNotice(error, true);
    }
    if (ImGui::IsItemActivated()) eventMergeKey = "clipevent#" + std::to_string(++eventDragSerial);
    ImGui::SameLine();
    ImGui::TextUnformatted("Function");
    ImGui::SameLine();
    // the methods the root's scripts define, and free text for any other name
    const std::vector<std::string> functions = scriptFunctions();
    static char functionBuffer[128];
    static int bufferEvent = -1;
    static uint64_t bufferRevision = 0;
    if (bufferEvent != selectedEvent || bufferRevision != doc->revision()){
        std::snprintf(functionBuffer, sizeof(functionBuffer), "%s", event.function.c_str());
    }
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::InputText("##function", functionBuffer, sizeof(functionBuffer));
    const bool editingName = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && event.function != functionBuffer){
        event.function = functionBuffer;
        if (!editEvent(selectedEvent, event, "", &error)) setNotice(error, true);
    }
    if (!editingName){
        bufferEvent = selectedEvent;
        bufferRevision = doc->revision();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() + Theme::dpi(4.0f));
    if (ImGui::BeginCombo("##functions", "", ImGuiComboFlags_NoPreview)){
        if (functions.empty()) ImGui::TextDisabled("No Lua script on the root defines a method");
        for (const std::string& function : functions){
            if (ImGui::Selectable(function.c_str(), function == event.function)){
                event.function = function;
                if (!editEvent(selectedEvent, event, "", &error)) setNotice(error, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The methods the Lua scripts on the bound GameObject define");
    ImGui::SameLine();
    bool missing = false;
    for (const EventReport& entry : report.events){
        if (entry.event == (size_t)selectedEvent && entry.status == EventHandlerStatus::NotFound) missing = true;
    }
    if (missing){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " No script on the root defines %s()",
                           event.function.c_str());
        ImGui::SameLine();
    }
    if (ImGui::Button(ICON_FA_TRASH_CAN "##delete")){
        if (!deleteEvent(selectedEvent, &error)) setNotice(error, true);
        ImGui::EndDisabled();
        ImGui::PopID();
        return;
    }
    ImGui::SetItemTooltip("Delete the event (Del)");

    // the args: number, string or bool each
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Args");
    bool changed = false;
    int removeIndex = -1;
    for (size_t i = 0; i < event.args.size(); i++){
        ImGui::SameLine();
        ImGui::PushID((int)i);
        AnimationEventArg& arg = event.args[i];
        const char* types[] = {"Number", "String", "Bool"};
        int type = (int)arg.index();
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        if (ImGui::Combo("##type", &type, types, 3)){
            if (type == 0) arg = AnimationEvent::numberArg(0.0);
            if (type == 1) arg = AnimationEvent::stringArg("");
            if (type == 2) arg = AnimationEvent::boolArg(false);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        if (std::holds_alternative<double>(arg)){
            double value = std::get<double>(arg);
            ImGui::InputDouble("##value", &value, 0.0, 0.0, "%g");
            if (ImGui::IsItemDeactivatedAfterEdit()){
                arg = AnimationEvent::numberArg(value);
                changed = true;
            }
        }else if (std::holds_alternative<std::string>(arg)){
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s", std::get<std::string>(arg).c_str());
            ImGui::InputText("##value", buffer, sizeof(buffer));
            if (ImGui::IsItemDeactivatedAfterEdit()){
                arg = AnimationEvent::stringArg(buffer);
                changed = true;
            }
        }else{
            bool value = std::get<bool>(arg);
            if (ImGui::Checkbox("##value", &value)){
                arg = AnimationEvent::boolArg(value);
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)) removeIndex = (int)i;
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(event.args.size() >= MAX_ANIMATION_EVENT_ARGS);
    if (ImGui::SmallButton("+ Arg")){
        event.args.push_back(AnimationEvent::numberArg(0.0));
        changed = true;
    }
    ImGui::EndDisabled();
    if (removeIndex >= 0){
        event.args.erase(event.args.begin() + removeIndex);
        changed = true;
    }
    if (changed && !editEvent(selectedEvent, event, "", &error)) setNotice(error, true);
    ImGui::EndDisabled();
    ImGui::PopID();
}
