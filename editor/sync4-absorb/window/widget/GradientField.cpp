// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The gradient field (GradientField.h): the inline bar and Unity's popup gradient editor. The
// popup shows the mode and the preset thumbnails, the alpha marker strip, the bar over a
// checkerboard, the colour marker strip, and the selected key's colour (sRGB picker, stored
// linear) or alpha, its Location and a remove button. Every key edit goes through gradientfield
// (GradientFieldLogic.h). Mouse: a click on a strip selects the marker under it or adds a key
// there, a drag moves it. Keys: Delete / Backspace remove the selected key (not the last one).

#include "GradientField.h"
#include "GradientFieldLogic.h"

#include "imgui.h"
#include "external/IconsFontAwesome6.h"
#include "Theme.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::gradientfield;

namespace {

    enum class KeyList : uint8_t{ None, Color, Alpha };

    // the popup's UI state of one field (by ImGui ID)
    struct GradientEditorState{
        KeyList selList = KeyList::None;
        int selIndex = -1;
        bool dragging = false;
        float grabDx = 0.0f;        // the mouse minus the dragged marker, pixels
        bool dragChanged = false;   // the running drag (or the key it added) modified the gradient
        bool fieldPending = false;  // a colour / alpha / location field changed and is still being edited
    };

    std::unordered_map<ImGuiID, GradientEditorState> states;

    const char* const MODE_NAMES[2] = {"Blend", "Fixed"};

    float clamp01(float v){
        return std::isfinite(v) ? std::min(1.0f, std::max(0.0f, v)) : 0.0f;
    }

    // a linear RGBA as the screen shows it (sRGB)
    ImU32 displayColor(const Vector4& linear){
        const Vector3 srgb = Color::linearTosRGB(Vector3(linear.x, linear.y, linear.z));
        return ImGui::ColorConvertFloat4ToU32(ImVec4(clamp01(srgb.x), clamp01(srgb.y), clamp01(srgb.z), clamp01(linear.w)));
    }

    void drawCheckerboard(ImDrawList* drawList, ImVec2 p0, ImVec2 p1, float cell){
        drawList->AddRectFilled(p0, p1, IM_COL32(204, 204, 204, 255));
        int row = 0;
        for (float y = p0.y; y < p1.y; y += cell, row++){
            int column = 0;
            for (float x = p0.x; x < p1.x; x += cell, column++){
                if (((row + column) & 1) == 0) continue;
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, p1.x), std::min(y + cell, p1.y)), IM_COL32(128, 128, 128, 255));
            }
        }
    }

    // the gradient over [p0, p1] on a checkerboard: Blend as horizontal ramps between the key
    // times (subdivided: the screen blends in sRGB, the gradient in linear), Fixed as flat bands
    void drawGradientBar(ImDrawList* drawList, const Gradient& gradient, ImVec2 p0, ImVec2 p1){
        drawCheckerboard(drawList, p0, p1, std::max(2.0f, Theme::dpi(5.0f)));
        std::vector<float> times = {0.0f, 1.0f};
        for (const GradientColorKey& key : gradient.colorKeys) times.push_back(clamp01(key.time));
        for (const GradientAlphaKey& key : gradient.alphaKeys) times.push_back(clamp01(key.time));
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());

        const float width = p1.x - p0.x;
        for (size_t i = 0; i + 1 < times.size(); i++){
            const float t0 = times[i], t1 = times[i + 1];
            if (gradient.mode == GradientMode::Fixed){
                const ImU32 c = displayColor(gradient.evaluate(0.5f * (t0 + t1)));
                drawList->AddRectFilled(ImVec2(p0.x + t0 * width, p0.y), ImVec2(p0.x + t1 * width, p1.y), c);
                continue;
            }
            const int pieces = 8;
            for (int p = 0; p < pieces; p++){
                const float a = t0 + (t1 - t0) * (float)p / (float)pieces;
                const float b = t0 + (t1 - t0) * (float)(p + 1) / (float)pieces;
                const ImU32 ca = displayColor(gradient.evaluate(a));
                const ImU32 cb = displayColor(gradient.evaluate(b));
                drawList->AddRectFilledMultiColor(ImVec2(p0.x + a * width, p0.y), ImVec2(p0.x + b * width, p1.y), ca, cb, cb, ca);
            }
        }
    }

    // a marker at x: a swatch with a tip pointing at the bar (colour markers below it point up,
    // alpha markers above it point down)
    void drawMarker(ImDrawList* drawList, float x, float top, float bottom, bool pointUp, ImU32 fill, bool selected){
        const float half = Theme::dpi(5.0f);
        const float tip = Theme::dpi(5.0f);
        const ImU32 outline = selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(30, 30, 30, 255);
        const float thickness = selected ? Theme::dpi(2.0f) : Theme::dpi(1.0f);
        ImVec2 points[5];
        if (pointUp){
            points[0] = ImVec2(x, top);
            points[1] = ImVec2(x + half, top + tip);
            points[2] = ImVec2(x + half, bottom);
            points[3] = ImVec2(x - half, bottom);
            points[4] = ImVec2(x - half, top + tip);
        }else{
            points[0] = ImVec2(x, bottom);
            points[1] = ImVec2(x - half, bottom - tip);
            points[2] = ImVec2(x - half, top);
            points[3] = ImVec2(x + half, top);
            points[4] = ImVec2(x + half, bottom - tip);
        }
        drawList->AddConvexPolyFilled(points, 5, fill);
        drawList->AddPolyline(points, 5, outline, ImDrawFlags_Closed, thickness);
    }

    void markEdit(GradientFieldResult& result, Gradient& gradient, bool finished){
        gradient.normalize();
        result.changed = true;
        if (finished) result.finished = true;
    }

    bool selectionValid(const GradientEditorState& state, const Gradient& gradient){
        if (state.selIndex < 0) return false;
        if (state.selList == KeyList::Color) return (size_t)state.selIndex < gradient.colorKeys.size();
        if (state.selList == KeyList::Alpha) return (size_t)state.selIndex < gradient.alphaKeys.size();
        return false;
    }

    void removeSelected(GradientEditorState& state, Gradient& gradient, GradientFieldResult& result){
        if (!selectionValid(state, gradient)) return;
        const bool removed = state.selList == KeyList::Color ? removeColorKey(gradient, (size_t)state.selIndex)
                                                             : removeAlphaKey(gradient, (size_t)state.selIndex);
        if (!removed) return;
        state.selList = KeyList::None;
        state.selIndex = -1;
        markEdit(result, gradient, true);
    }

    // one marker strip: select / add on click, move on drag
    void markerStrip(const char* id, KeyList list, GradientEditorState& state, Gradient& gradient, float barX0, float barWidth,
                     ImVec2 size, GradientFieldResult& result){
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        const bool colors = list == KeyList::Color;

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            const std::vector<float> times = colors ? colorTimes(gradient) : alphaTimes(gradient);
            const int hit = hitMarker(times, barX0, barWidth, io.MousePos.x, Theme::dpi(6.0f));
            if (hit >= 0){
                state.selList = list;
                state.selIndex = hit;
                state.grabDx = io.MousePos.x - xAtTime(barX0, barWidth, times[(size_t)hit]);
                state.dragging = true;
                state.dragChanged = false;
            }else{
                const float time = timeAtX(barX0, barWidth, io.MousePos.x);
                const int index = colors ? addColorKey(gradient, time) : addAlphaKey(gradient, time);
                if (index >= 0){
                    state.selList = list;
                    state.selIndex = index;
                    state.grabDx = 0.0f;
                    state.dragging = true;
                    state.dragChanged = true;   // the release commits the add (and any move)
                    markEdit(result, gradient, false);
                }
            }
        }
        if (state.dragging && state.selList == list){
            if (!selectionValid(state, gradient)){
                state.dragging = false;
            }else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)){
                if (io.MouseDelta.x != 0.0f){
                    const float time = timeAtX(barX0, barWidth, io.MousePos.x - state.grabDx);
                    const float before = colors ? gradient.colorKeys[(size_t)state.selIndex].time : gradient.alphaKeys[(size_t)state.selIndex].time;
                    if (time != before){
                        const int to = colors ? moveColorKey(gradient, (size_t)state.selIndex, time)
                                              : moveAlphaKey(gradient, (size_t)state.selIndex, time);
                        if (to >= 0){
                            state.selIndex = to;
                            state.dragChanged = true;
                            markEdit(result, gradient, false);
                        }
                    }
                }
            }else{
                if (state.dragChanged) result.finished = true;
                state.dragging = false;
                state.dragChanged = false;
            }
        }

        // the markers, the selected one last (on top)
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float top = p0.y, bottom = p0.y + size.y;
        const size_t count = colors ? gradient.colorKeys.size() : gradient.alphaKeys.size();
        int selected = (state.selList == list) ? state.selIndex : -1;
        for (int pass = 0; pass < 2; pass++){
            for (size_t i = 0; i < count; i++){
                const bool isSelected = (int)i == selected;
                if (isSelected != (pass == 1)) continue;
                ImU32 fill;
                float time;
                if (colors){
                    const GradientColorKey& key = gradient.colorKeys[i];
                    time = key.time;
                    fill = displayColor(Vector4(key.color.x, key.color.y, key.color.z, 1.0f));
                }else{
                    const GradientAlphaKey& key = gradient.alphaKeys[i];
                    time = key.time;
                    const float grey = clamp01(key.alpha);
                    fill = ImGui::ColorConvertFloat4ToU32(ImVec4(grey, grey, grey, 1.0f));
                }
                drawMarker(drawList, xAtTime(barX0, barWidth, time), top, bottom, colors, fill, isSelected);
            }
        }
        if (hovered && !state.dragging){
            ImGui::SetItemTooltip(colors ? "Click to add a colour key, drag a key to move it" : "Click to add an alpha key, drag a key to move it");
        }
    }

    void editorPopup(GradientEditorState& state, Gradient& gradient, GradientFieldResult& result){
        ImGuiIO& io = ImGui::GetIO();
        if (!selectionValid(state, gradient)){
            state.selList = KeyList::None;
            state.selIndex = -1;
        }

        // mode and presets
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        const int mode = gradient.mode == GradientMode::Fixed ? 1 : 0;
        if (ImGui::BeginCombo("Mode", MODE_NAMES[mode])){
            for (int option = 0; option < 2; option++){
                if (ImGui::Selectable(MODE_NAMES[option], option == mode) && option != mode){
                    gradient.mode = option == 1 ? GradientMode::Fixed : GradientMode::Blend;
                    markEdit(result, gradient, true);
                }
            }
            ImGui::EndCombo();
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 thumb(Theme::dpi(40.0f), ImGui::GetFrameHeight());
        for (int i = 0; i < GRADIENT_PRESET_COUNT; i++){
            ImGui::SameLine(0.0f, i == 0 ? Theme::dpi(12.0f) : -1.0f);
            ImGui::PushID(i);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##preset", thumb);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetItemTooltip("%s", presetName((GradientPreset)i));
            const float pad = Theme::dpi(hovered ? 2.0f : 3.0f);
            drawGradientBar(drawList, presetGradient((GradientPreset)i), ImVec2(p0.x + pad, p0.y + pad), ImVec2(p0.x + thumb.x - pad, p0.y + thumb.y - pad));
            if (hovered) drawList->AddRect(p0, ImVec2(p0.x + thumb.x, p0.y + thumb.y), ImGui::GetColorU32(ImGuiCol_Border));
            if (clicked){
                gradient = presetGradient((GradientPreset)i);
                state.selList = KeyList::None;
                state.selIndex = -1;
                markEdit(result, gradient, true);
            }
            ImGui::PopID();
        }

        // alpha strip, bar, colour strip
        const float width = Theme::dpi(400.0f);
        const float margin = Theme::dpi(6.0f);
        const float stripHeight = Theme::dpi(15.0f);
        const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x, 0.0f));
        const float barX0 = ImGui::GetCursorScreenPos().x + margin;
        const float barWidth = width - 2.0f * margin;
        markerStrip("##alphaStrip", KeyList::Alpha, state, gradient, barX0, barWidth, ImVec2(width, stripHeight), result);
        const ImVec2 b0(barX0, ImGui::GetCursorScreenPos().y);
        const ImVec2 b1(barX0 + barWidth, b0.y + Theme::dpi(28.0f));
        ImGui::Dummy(ImVec2(width, b1.y - b0.y));
        drawGradientBar(drawList, gradient, b0, b1);
        drawList->AddRect(ImVec2(b0.x - 1.0f, b0.y - 1.0f), ImVec2(b1.x + 1.0f, b1.y + 1.0f), IM_COL32(0, 0, 0, 200));
        markerStrip("##colorStrip", KeyList::Color, state, gradient, barX0, barWidth, ImVec2(width, stripHeight), result);
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0.0f, Theme::dpi(4.0f)));

        // Delete / Backspace remove the selected key
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput && !ImGui::IsAnyItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))){
            removeSelected(state, gradient, result);
        }

        // the selected key
        if (selectionValid(state, gradient)){
            const bool colors = state.selList == KeyList::Color;
            const size_t index = (size_t)state.selIndex;
            const float fieldWidth = Theme::dpi(150.0f);
            bool fieldChanged = false;
            bool fieldDone = false;
            if (colors){
                GradientColorKey& key = gradient.colorKeys[index];
                const Vector3 srgb = Color::linearTosRGB(key.color);
                float edit[3] = {clamp01(srgb.x), clamp01(srgb.y), clamp01(srgb.z)};
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::ColorEdit3("Color", edit)){
                    key.color = Color::sRGBToLinear(Vector3(edit[0], edit[1], edit[2]));
                    fieldChanged = true;
                }
                fieldDone |= ImGui::IsItemDeactivatedAfterEdit();
            }else{
                GradientAlphaKey& key = gradient.alphaKeys[index];
                float alpha = clamp01(key.alpha) * 255.0f;
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::SliderFloat("Alpha", &alpha, 0.0f, 255.0f, "%.0f")){
                    key.alpha = clamp01(alpha / 255.0f);
                    fieldChanged = true;
                }
                fieldDone |= ImGui::IsItemDeactivatedAfterEdit();
            }

            ImGui::SameLine(0.0f, Theme::dpi(12.0f));
            const float time = colors ? gradient.colorKeys[index].time : gradient.alphaKeys[index].time;
            float percent = time * 100.0f;
            ImGui::SetNextItemWidth(Theme::dpi(80.0f));
            if (ImGui::DragFloat("Location", &percent, 0.2f, 0.0f, 100.0f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp)){
                const int to = colors ? moveColorKey(gradient, index, percent / 100.0f) : moveAlphaKey(gradient, index, percent / 100.0f);
                if (to >= 0){
                    state.selIndex = to;
                    fieldChanged = true;
                }
            }
            fieldDone |= ImGui::IsItemDeactivatedAfterEdit();

            ImGui::SameLine();
            const size_t listSize = colors ? gradient.colorKeys.size() : gradient.alphaKeys.size();
            ImGui::BeginDisabled(listSize <= 1);
            if (ImGui::Button(ICON_FA_TRASH_CAN)){
                removeSelected(state, gradient, result);
            }
            ImGui::SetItemTooltip("Remove the key (Delete)");
            ImGui::EndDisabled();

            if (fieldChanged){
                markEdit(result, gradient, false);
                state.fieldPending = true;
            }
            if (fieldDone){
                result.finished = true;
                state.fieldPending = false;
            }
        }else{
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Click a marker to edit it; click a strip to add a key");
        }

        // a field edit ended without a deactivation event (the colour picker's own popup)
        if (state.fieldPending && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            result.finished = true;
            state.fieldPending = false;
        }
    }

}

GradientFieldResult editor::gradientField(const char* id, Gradient& gradient, bool mixed, float width){
    GradientFieldResult result;
    ImGui::PushID(id);
    const ImGuiID stateId = ImGui::GetID("##gradientField");
    GradientEditorState& state = states[stateId];

    // the inline bar
    const float w = std::max(Theme::dpi(8.0f), width < 0.0f ? ImGui::GetContentRegionAvail().x : width);
    const ImVec2 size(w, ImGui::GetFrameHeight());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool clicked = ImGui::InvisibleButton("##preview", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float pad = Theme::dpi(2.0f);
    drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    drawGradientBar(drawList, gradient, ImVec2(p0.x + pad, p0.y + pad), ImVec2(p1.x - pad, p1.y - pad));
    if (mixed){
        drawList->AddRectFilled(ImVec2(p0.x + pad, p0.y + pad), ImVec2(p1.x - pad, p1.y - pad), IM_COL32(40, 40, 40, 200));
        const char* dash = "\xE2\x80\x94";
        const ImVec2 ts = ImGui::CalcTextSize(dash);
        drawList->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), dash);
    }
    if (hovered) ImGui::SetItemTooltip("Click to edit the gradient");

    if (clicked){
        state = GradientEditorState();
        ImGui::OpenPopup("##gradientEditor");
    }
    if (ImGui::BeginPopup("##gradientEditor", ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)){
        editorPopup(state, gradient, result);
        ImGui::EndPopup();
    }else if (state.dragging || state.fieldPending){
        // closed during an edit (Escape, a click outside): the edit ends
        if (state.dragChanged || state.fieldPending) result.finished = true;
        state.dragging = false;
        state.dragChanged = false;
        state.fieldPending = false;
    }

    ImGui::PopID();
    return result;
}
