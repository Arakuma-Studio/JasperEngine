// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's events lane (animation clips spec 4.4, 3.5): markers under the ruler
// (Add Event at the playhead or at a right-click, drag to retime snapped to frames, click to
// select and edit), and the selected event's editor (a popup at the marker: time, function with a
// combo of the methods the root's Lua scripts define, the args list; a function no script defines
// shows in WarningText). The selection is kept by identity (time, function, ordinal), so undo,
// redo and reloads never point it at another event. Each edit is one controller commit (project
// history); a drag merges into one step.

#include "AnimationWindowClip.h"
#include "AnimationWindowClipUtil.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"

#include <cctype>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::clipui;

namespace {

    constexpr float kEventEpsilon = 1e-4f;

    bool isLuaKeyword(const std::string& name){
        static const char* const keywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
                                               "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        for (const char* keyword : keywords){
            if (name == keyword) return true;
        }
        return false;
    }

    bool isIdentifier(const std::string& name){
        if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
        for (char ch : name){
            if (!(std::isalnum((unsigned char)ch) || ch == '_')) return false;
        }
        return true;
    }

    // the index of the event of `id` in `events` (-1 when gone)
    int findEvent(const std::vector<ClipEventData>& events, const AnimationWindowClip::EventId& id){
        int ordinal = 0;
        for (size_t i = 0; i < events.size(); i++){
            if (events[i].function != id.function || std::fabs(events[i].time - id.time) > kEventEpsilon) continue;
            if (ordinal == id.ordinal) return (int)i;
            ordinal++;
        }
        return -1;
    }

    // the event `function` nearest to `time` (after a commit re-sorted the events)
    int findNearest(const std::vector<ClipEventData>& events, const std::string& function, float time){
        int best = -1;
        float bestDistance = 1e9f;
        for (size_t i = 0; i < events.size(); i++){
            if (events[i].function != function) continue;
            const float distance = std::fabs(events[i].time - time);
            if (distance < bestDistance){
                bestDistance = distance;
                best = (int)i;
            }
        }
        return best;
    }

}

std::string AnimationWindowClip::eventProblem(const ClipEventData& event){
    if (!isIdentifier(event.function)) return "an event's function is a Lua identifier (letters, digits, _)";
    if (isLuaKeyword(event.function)) return "'" + event.function + "' is a Lua keyword, not a function name";
    if (event.args.size() > MAX_ANIMATION_EVENT_ARGS) return "an event has at most " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " args";
    for (const AnimationEventArg& arg : event.args){
        if (std::holds_alternative<double>(arg) && !std::isfinite(std::get<double>(arg))) return "a number arg must be finite (not inf or nan)";
    }
    if (!std::isfinite(event.time) || event.time < 0.0f) return "an event's time is 0 or later";
    return std::string();
}

void AnimationWindowClip::selectEvent(int index){
    std::shared_ptr<AnimationClipDocument> doc = controllerUsed ? document() : nullptr;
    if (!doc || index < 0 || index >= (int)doc->data().events.size()){
        selectedEvent = -1;
        eventSelected = false;
        return;
    }
    const std::vector<ClipEventData>& events = doc->data().events;
    selectedEvent = index;
    eventSelected = true;
    selectedEventId.time = events[(size_t)index].time;
    selectedEventId.function = events[(size_t)index].function;
    selectedEventId.ordinal = 0;
    for (int i = 0; i < index; i++){
        if (events[(size_t)i].function == selectedEventId.function && std::fabs(events[(size_t)i].time - selectedEventId.time) <= kEventEpsilon){
            selectedEventId.ordinal++;
        }
    }
    eventRevision = doc->revision();
    eventDocument = doc;
}

void AnimationWindowClip::resolveSelectedEvent(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!eventSelected || !doc){
        selectedEvent = -1;
        eventSelected = false;
        return;
    }
    if (doc == eventDocument.lock() && doc->revision() == eventRevision) return;
    // another revision (an edit, undo, redo, a reload) or another clip: the same event or none
    selectedEvent = doc == eventDocument.lock() ? findEvent(doc->data().events, selectedEventId) : -1;
    if (selectedEvent < 0){
        eventSelected = false;
        return;
    }
    eventRevision = doc->revision();
}

bool AnimationWindowClip::addEvent(float time, const std::string& function, const std::vector<AnimationEventArg>& args, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    ClipEventData event;
    event.time = std::max(0.0f, time);
    event.function = function;
    if (event.function.empty()){
        // the first script method, else a placeholder the user renames
        const std::vector<std::string> functions = scriptFunctions();
        event.function = functions.empty() ? "onAnimationEvent" : functions.front();
    }
    event.args = args;
    const std::string problem = eventProblem(event);
    if (!problem.empty()){
        if (error) *error = problem;
        return false;
    }
    ClipData data = doc->data();
    data.events.push_back(event);
    if (!controller().commit(data, "Add Animation Event", "", error)) return false;
    selection.clear();
    refreshSelection();
    selectEvent(findNearest(document()->data().events, event.function, event.time));
    return true;
}

bool AnimationWindowClip::editEvent(int index, const ClipEventData& event, const std::string& mergeKey, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || index < 0 || index >= (int)doc->data().events.size()){
        if (error) *error = "no such event";
        return false;
    }
    const std::string problem = eventProblem(event);
    if (!problem.empty()){
        if (error) *error = problem;
        return false;
    }
    ClipData data = doc->data();
    data.events[(size_t)index] = event;
    if (!controller().commit(data, "Edit Animation Event", mergeKey, error)) return false;
    selectEvent(findNearest(document()->data().events, event.function, event.time));
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
    selectEvent(-1);
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

void AnimationWindowClip::drawEventsLane(ImDrawList* drawList, ImVec2 origin, float width, float height, float timeOriginX){
    std::shared_ptr<AnimationClipDocument> doc = document();
    ImGuiIO& io = ImGui::GetIO();
    const SheetMetrics m = sheetMetrics();
    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color(Theme::Colors::FoldoutHeader));
    drawList->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                      color(Theme::Colors::ToolbarBorder));
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##clip_events", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (!doc) return;
    const ClipData& data = doc->data();
    auto xAt = [&](float t){ return timeOriginX + t * pixelsPerSecond - scrollX; };
    auto timeAt = [&](float x){ return std::max(0.0f, (x - timeOriginX + scrollX) / pixelsPerSecond); };

    // the handler report of each event (WarningText markers when no script defines it)
    auto handlerMissing = [&](size_t index){
        for (const EventReport& entry : report.events){
            if (entry.event == index) return entry.status == EventHandlerStatus::NotFound;
        }
        return false;
    };

    int hovered = -1;
    const ImVec4 selected = selectedKeyColor();
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    for (size_t i = 0; i < data.events.size(); i++){
        const float x = xAt(data.events[i].time);
        if (x < origin.x - m.eventWidth || x > origin.x + width + m.eventWidth) continue;
        const bool isSelected = (int)i == selectedEvent;
        ImVec4 fill = isSelected ? selected : keyColor();
        if (!isSelected && handlerMissing(i)) fill = Theme::Colors::WarningText;
        // a flag: a pole and an 8 x 12 px marker (Unity's event marker)
        const float w = m.eventWidth;
        const float top = origin.y + 2.0f * m.s;
        drawList->AddRectFilled(ImVec2(x - 1.0f * m.s, top), ImVec2(x + 1.0f * m.s, origin.y + height - 2.0f * m.s), color(fill));
        drawList->AddRectFilled(ImVec2(x, top), ImVec2(x + w, std::min(top + 12.0f * m.s, origin.y + height - 2.0f * m.s)), color(fill));
        if (ImGui::IsItemHovered() && io.MousePos.x >= x - 3.0f * m.s && io.MousePos.x <= x + w + 2.0f * m.s) hovered = (int)i;
    }
    drawList->PopClipRect();

    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if (hovered >= 0){
            selection.clear();
            refreshSelection();
            selectEvent(hovered);
            drag = Drag::PendingEvent;
            dragStart = io.MousePos;
            eventDragBase = data.events[(size_t)hovered].time;
        }else{
            selectEvent(-1);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !doc->isReadOnly()){
        if (drag == Drag::PendingEvent && std::fabs(io.MousePos.x - dragStart.x) > 3.0f * m.s){
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
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag == Drag::PendingEvent || drag == Drag::Event)){
        // a click (no drag) opens the event's editor at the marker
        if (drag == Drag::PendingEvent && selectedEvent >= 0) openEventEditor = true;
        drag = Drag::None;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
        contextTime = snapToFrame(timeAt(io.MousePos.x));
        if (hovered >= 0) selectEvent(hovered);
        ImGui::OpenPopup(hovered >= 0 ? "##clip_event_menu" : "##clip_lane_menu");
    }
    if (hovered >= 0 && drag == Drag::None){
        const ClipEventData& event = data.events[(size_t)hovered];
        ImGui::SetTooltip("%s()%s at %s", event.function.c_str(), handlerMissing((size_t)hovered) ? " (no script defines it)" : "",
                          formatBoth(event.time, frameRate(), showFrames).c_str());
    }
    if (ImGui::BeginPopup("##clip_lane_menu")){
        if (ImGui::MenuItem("Add Event", nullptr, false, !doc->isReadOnly())){
            std::string error;
            if (!addEvent(contextTime, "", {}, &error)) setNotice(error, true);
            else openEventEditor = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##clip_event_menu")){
        if (ImGui::MenuItem("Edit Event...")) openEventEditor = true;
        if (ImGui::MenuItem("Delete Event", "Del", false, !doc->isReadOnly())){
            std::string error;
            if (!deleteEvent(selectedEvent, &error)) setNotice(error, true);
        }
        ImGui::EndPopup();
    }
    // the lane is the item the caller checks for hover (wheel zoom)
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
}

void AnimationWindowClip::drawEventEditor(SceneProject* sceneProject){
    (void)sceneProject;
    std::shared_ptr<AnimationClipDocument> doc = document();
    const bool valid = doc && selectedEvent >= 0 && selectedEvent < (int)doc->data().events.size();
    if (openEventEditor){
        openEventEditor = false;
        if (valid) ImGui::OpenPopup("##clip_event_editor");
    }
    if (!ImGui::BeginPopup("##clip_event_editor")) return;
    if (!valid){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ClipEventData event = doc->data().events[(size_t)selectedEvent];
    const bool readOnly = doc->isReadOnly();
    std::string error;
    ImGui::BeginDisabled(readOnly);
    ImGui::TextUnformatted(ICON_FA_FLAG " Animation Event");
    // the time, in frames or seconds like the window
    const float fps = frameRate();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Time");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(80.0f));
    bool timeChanged = false;
    if (showFrames){
        int frame = (int)std::lround(event.time * fps);
        timeChanged = ImGui::DragInt("##time", &frame, 0.2f, 0, (int)(kMaxTime * fps));
        if (timeChanged) event.time = (float)std::max(0, frame) / fps;
    }else{
        float time = event.time;
        timeChanged = ImGui::DragFloat("##time", &time, 1.0f / fps, 0.0f, kMaxTime, "%.3fs");
        if (timeChanged) event.time = snapToFrame(std::max(0.0f, time));
    }
    if (ImGui::IsItemActivated()) eventMergeKey = "clipevent#" + std::to_string(++eventDragSerial);
    if (timeChanged && !editEvent(selectedEvent, event, eventMergeKey, &error)) setNotice(error, true);
    ImGui::SetItemTooltip("%s", formatBoth(event.time, fps, showFrames).c_str());
    if (event.time > doc->data().effectiveLength() + kEventEpsilon){
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " after the end of the clip (never fires)");
    }

    // the function: the methods the root's scripts define, or any other name
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Function");
    ImGui::SameLine();
    const std::vector<std::string> functions = scriptFunctions();
    static char functionBuffer[128];
    const ImGuiID nameId = ImGui::GetID("##function");
    if (ImGui::GetActiveID() != nameId) std::snprintf(functionBuffer, sizeof(functionBuffer), "%s", event.function.c_str());
    ImGui::SetNextItemWidth(Theme::dpi(170.0f));
    ImGui::InputText("##function", functionBuffer, sizeof(functionBuffer));
    if (ImGui::IsItemDeactivatedAfterEdit() && event.function != functionBuffer){
        event.function = functionBuffer;
        if (!editEvent(selectedEvent, event, "", &error)) setNotice(error, true);
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
    for (const EventReport& entry : report.events){
        if (entry.event == (size_t)selectedEvent && entry.status == EventHandlerStatus::NotFound){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " No script on the root defines %s()",
                               event.function.c_str());
        }
    }

    // the args: number, string or bool each
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Args");
    bool changed = false;
    int removeIndex = -1;
    for (size_t i = 0; i < event.args.size(); i++){
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
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        if (std::holds_alternative<double>(arg)){
            double value = std::get<double>(arg);
            ImGui::InputDouble("##value", &value, 0.0, 0.0, "%g");
            if (ImGui::IsItemDeactivatedAfterEdit()){
                if (std::isfinite(value)){
                    arg = AnimationEvent::numberArg(value);
                    changed = true;
                }else{
                    setNotice("a number arg must be finite (not inf or nan)", true);
                }
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
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_TRASH_CAN " Delete")){
        if (!deleteEvent(selectedEvent, &error)) setNotice(error, true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}
