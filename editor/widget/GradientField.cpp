// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GradientField.h"
#include "GradientEditorOps.h"

#include "AppSettings.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "widget/ColorField.h"
#include "widget/ColorMath.h"
#include "widget/ColorPickerWindow.h"

#include "imgui_internal.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteRampTexture.h"
#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::ui;

namespace {

    constexpr int kBarSamples = 48;

    ImU32 srgbColor(const Vector3& srgb, float alpha) {
        return IM_COL32(colormath::toByte(srgb.x), colormath::toByte(srgb.y), colormath::toByte(srgb.z), colormath::toByte(alpha));
    }

    void checker(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
        const float cell = std::max(4.0f, (max.y - min.y) * 0.5f);
        for (float y = min.y; y < max.y; y += cell) {
            for (float x = min.x; x < max.x; x += cell) {
                const bool dark = ((int)((x - min.x) / cell) + (int)((y - min.y) / cell)) % 2 == 0;
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)),
                                        dark ? IM_COL32(110, 110, 110, 255) : IM_COL32(170, 170, 170, 255));
            }
        }
    }

    // the key nearest to `time` within `radius` (in time units); -1 for none
    template <class Keys>
    int nearestKey(const Keys& keys, float time, float radius) {
        int best = -1;
        float bestDistance = radius;
        for (size_t i = 0; i < keys.size(); i++) {
            const float d = std::fabs(keys[i].time - time);
            if (d <= bestDistance) {
                bestDistance = d;
                best = (int)i;
            }
        }
        return best;
    }

}

void ui::drawGradientBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, const Gradient& gradient) {
    checker(drawList, min, max);
    for (int i = 0; i < kBarSamples; i++) {
        const float t0 = (float)i / kBarSamples, t1 = (float)(i + 1) / kBarSamples;
        const float x0 = min.x + t0 * (max.x - min.x), x1 = min.x + t1 * (max.x - min.x);
        const Vector3 c0 = gradient.evaluateEncoded(t0), c1 = gradient.evaluateEncoded(t1);
        const float a0 = gradient.evaluateAlpha(t0), a1 = gradient.evaluateAlpha(t1);
        drawList->AddRectFilledMultiColor(ImVec2(x0, min.y), ImVec2(x1, max.y), srgbColor(c0, a0), srgbColor(c1, a1), srgbColor(c1, a1),
                                          srgbColor(c0, a0));
    }
    drawList->AddRect(min, max, IM_COL32(0, 0, 0, 200));
}

EditPhase ui::gradientField(const char* id, Gradient& gradient, const GradientFieldOptions& options) {
    ImGui::PushID(id);
    const ImGuiID fieldId = ImGui::GetID("##gradientField");
    ImGuiContext& g = *GImGui;
    const float width = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) ? ImGui::CalcItemWidth()
                                                                                     : std::max(ImGui::GetContentRegionAvail().x, 8.0f);
    const ImVec2 size(std::max(width, 8.0f), options.height > 0.0f ? options.height : ImGui::GetFrameHeight());
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##gradientField", size);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    GradientWindow& window = GradientWindow::get();
    EditPhase phase = window.consume(fieldId, gradient);
    drawGradientBar(ImGui::GetWindowDrawList(), min, max, gradient);
    if (options.mixed) ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 4.0f, min.y), IM_COL32(255, 255, 255, 220), "-");
    if (window.targets(fieldId)) ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavCursor));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to edit the gradient");
    if (clicked && !window.targets(fieldId)) {
        window.open(fieldId, gradient, options);
        phase = EditPhase::Begin;
    }
    ImGui::PopID();
    return phase;
}

EditPhase ui::minMaxGradientField(const char* id, particles::MinMaxGradient& value, const GradientFieldOptions& options) {
    using particles::GradientValueMode;
    ImGui::PushID(id);
    EditPhase phase = EditPhase::None;
    auto take = [&](EditPhase p) {
        if (p != EditPhase::None) phase = p;
    };
    const float button = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float avail = std::max(8.0f, ImGui::GetContentRegionAvail().x - button);
    ColorFieldOptions colorOptions;
    colorOptions.alpha = true;
    colorOptions.linearStorage = false;   // the particle colours are sRGB as picked
    colorOptions.label = options.label;
    colorOptions.mixed = options.mixed;
    switch (value.mode) {
        case GradientValueMode::Color: {
            colorOptions.width = avail;
            const EditPhase p = colorField("##color", value.color, colorOptions);
            if (p == EditPhase::Changing || p == EditPhase::Cancelled) value.colorMax = value.color;
            take(p);
            break;
        }
        case GradientValueMode::TwoColors: {
            colorOptions.width = std::max(4.0f, avail * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f);
            take(colorField("##min", value.color, colorOptions));
            ImGui::SameLine();
            take(colorField("##max", value.colorMax, colorOptions));
            break;
        }
        case GradientValueMode::Gradient:
        case GradientValueMode::RandomColor:
        case GradientValueMode::TwoGradients: {
            const bool two = value.mode == GradientValueMode::TwoGradients;
            GradientFieldOptions bar = options;
            const float height = ImGui::GetFrameHeight();
            bar.height = two ? height * 0.5f : height;
            ImGui::SetNextItemWidth(avail);
            const ImVec2 start = ImGui::GetCursorPos();
            take(gradientField("##gradient", value.gradient, bar));
            if (two) {
                ImGui::SetCursorPos(ImVec2(start.x, start.y + bar.height));
                ImGui::SetNextItemWidth(avail);
                take(gradientField("##gradientMax", value.gradientMax, bar));
                ImGui::SetCursorPos(ImVec2(start.x + avail, start.y));
                ImGui::Dummy(ImVec2(0.0f, height));
            }
            break;
        }
        case GradientValueMode::Ramp: {
            // S49 R32: the ramp's steps as a stepped bar; a click edits the reference, a ramp
            // strip dragged from the Palette window (or a .palette from the Project window) sets it
            const std::vector<pixelart::Rgba8> colors = value.resolveRamp();
            const ImVec2 size(avail, ImGui::GetFrameHeight());
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##rampBar", size);
            const ImVec2 max(min.x + size.x, min.y + size.y);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            checker(drawList, min, max);
            const int n = (int)colors.size();
            for (int i = 0; i < n; i++){
                const pixelart::Rgba8& c = colors[(size_t)(value.rampReverse ? n - 1 - i : i)];
                const float x0 = min.x + size.x * (float)i / (float)n, x1 = min.x + size.x * (float)(i + 1) / (float)n;
                drawList->AddRectFilled(ImVec2(x0, min.y), ImVec2(x1, max.y), IM_COL32(c.r, c.g, c.b, c.a));
            }
            if (n == 0) drawList->AddText(ImVec2(min.x + 4.0f, min.y), IM_COL32(255, 120, 120, 255), "drop a ramp");
            drawList->AddRect(min, max, IM_COL32(0, 0, 0, 200));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", value.ramp.empty() ? "inline colours" : value.ramp.c_str());
            // a palette set: its ramp kept when it has one of that name, else its first
            auto setPalette = [&](const std::string& palette){
                std::string current;
                pixelart::parsePaletteRampPath(value.ramp, nullptr, &current);
                std::shared_ptr<const pixelart::Palette> read = palette.empty() ? nullptr : pixelart::PaletteLibrary::get(palette);
                if (read && !read->ramps.empty() && read->findRamp(current) == nullptr) current = read->ramps.front().name;
                value.ramp = palette.empty() ? std::string() : palette + "#ramp:" + current;
                value.rampColors.clear();
                phase = EditPhase::Applied;
            };
            // a .palette dropped from the Project window
            auto acceptPaletteFile = [&](){
                if (!options.paletteOf) return;
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    const std::string first(static_cast<const char*>(payload->Data), strnlen(static_cast<const char*>(payload->Data), (size_t)payload->DataSize));
                    const std::string palette = options.paletteOf(first);
                    if (!palette.empty()) setPalette(palette);
                }
            };
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PALETTE_RAMP")){
                    value.ramp.assign(static_cast<const char*>(payload->Data), (size_t)payload->DataSize);
                    value.rampColors.clear();
                    phase = EditPhase::Applied;
                }
                acceptPaletteFile();
                ImGui::EndDragDropTarget();
            }
            if (clicked) ImGui::OpenPopup("##rampEdit");
            if (ImGui::BeginPopup("##rampEdit")){
                std::string palette, rampName;
                pixelart::parsePaletteRampPath(value.ramp, &palette, &rampName);
                // the palette: an asset field (the project's palettes, or one dropped on it)
                ImGui::SetNextItemWidth(Theme::dpi(240.0f));
                if (ImGui::BeginCombo("Palette", palette.empty() ? "None (inline colours)" : palette.c_str())){
                    const std::vector<std::string> palettes = options.palettes ? options.palettes() : std::vector<std::string>();
                    for (const std::string& candidate : palettes){
                        if (ImGui::Selectable(candidate.c_str(), candidate == palette)) setPalette(candidate);
                    }
                    if (palettes.empty()) ImGui::TextDisabled("No .palette in the project");
                    ImGui::EndCombo();
                }
                if (ImGui::BeginDragDropTarget()){
                    acceptPaletteFile();
                    ImGui::EndDragDropTarget();
                }
                ImGui::SetItemTooltip("A .palette asset: pick one, or drop one from the Project window");
                if (std::shared_ptr<const pixelart::Palette> read = palette.empty() ? nullptr : pixelart::PaletteLibrary::get(palette)){
                    // the ramps, each with its strip
                    const float strip = Theme::dpi(96.0f), height = ImGui::GetTextLineHeight();
                    auto drawStrip = [&](ImVec2 at, const pixelart::PaletteRamp& ramp){
                        const int n = (int)ramp.colors.size();
                        for (int i = 0; i < n; i++){
                            const int index = ramp.colors[(size_t)i];
                            const pixelart::Rgba8 c = read->validIndex(index) ? read->colors[(size_t)index] : pixelart::Rgba8{0, 0, 0, 255};
                            const float x0 = at.x + strip * (float)i / (float)n, x1 = at.x + strip * (float)(i + 1) / (float)n;
                            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, at.y), ImVec2(x1, at.y + height), IM_COL32(c.r, c.g, c.b, 255));
                        }
                    };
                    ImGui::SetNextItemWidth(Theme::dpi(240.0f));
                    if (ImGui::BeginCombo("Ramp", rampName.c_str(), ImGuiComboFlags_HeightLarge)){
                        for (size_t r = 0; r < read->ramps.size(); r++){
                            const pixelart::PaletteRamp& ramp = read->ramps[r];
                            ImGui::PushID((int)r);
                            const ImVec2 at = ImGui::GetCursorScreenPos();
                            if (ImGui::Selectable("##ramp", ramp.name == rampName, 0, ImVec2(0.0f, height))){
                                value.ramp = palette + "#ramp:" + ramp.name;
                                value.rampColors.clear();
                                phase = EditPhase::Applied;
                            }
                            drawStrip(at, ramp);
                            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + strip + Theme::dpi(6.0f), at.y), ImGui::GetColorU32(ImGuiCol_Text), ramp.name.c_str());
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                }else if (!value.rampColors.empty()){
                    ImGui::TextDisabled("%d inline colours", (int)value.rampColors.size());
                }
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                int steps = value.rampSteps;
                if (ImGui::DragInt("Steps", &steps, 0.2f, 0, particles::kMaxRampColors)){
                    value.rampSteps = steps == 1 ? 2 : steps;
                    phase = EditPhase::Changing;
                }
                ImGui::SetItemTooltip("0: the ramp as it is; 2 to 32: resampled, both ends kept");
                if (ImGui::IsItemDeactivatedAfterEdit()) phase = EditPhase::Applied;
                if (ImGui::Checkbox("Reverse", &value.rampReverse)) phase = EditPhase::Applied;
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                if (ImGui::DragFloat("Bias", &value.rampBias, 0.01f, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) phase = EditPhase::Changing;
                ImGui::SetItemTooltip("the driver t becomes t^bias before it picks a step (pixelfx's fire: 0.62)");
                if (ImGui::IsItemDeactivatedAfterEdit()) phase = EditPhase::Applied;
                ImGui::EndPopup();
            }
            break;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CARET_DOWN "##mode")) ImGui::OpenPopup("##minMaxGradientMode");
    if (ImGui::BeginPopup("##minMaxGradientMode")) {
        struct ModeItem {
            const char* label;
            GradientValueMode mode;
        };
        for (const ModeItem& item : {ModeItem{"Color", GradientValueMode::Color}, ModeItem{"Gradient", GradientValueMode::Gradient},
                                     ModeItem{"Random Between Two Colors", GradientValueMode::TwoColors},
                                     ModeItem{"Random Between Two Gradients", GradientValueMode::TwoGradients},
                                     ModeItem{"Random Color", GradientValueMode::RandomColor},
                                     ModeItem{"Palette Ramp", GradientValueMode::Ramp}}) {
            if (ImGui::MenuItem(item.label, nullptr, value.mode == item.mode) && value.mode != item.mode) {
                // a ramp starts from the colour it replaces until a palette's ramp is dropped on it
                if (item.mode == GradientValueMode::Ramp && value.ramp.empty() && value.rampColors.empty())
                {
                    auto byte = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                    value.rampColors = {pixelart::Rgba8{byte(value.color.x), byte(value.color.y), byte(value.color.z), byte(value.color.w)}};
                }
                // a gradient starts from the colour it replaces
                const Vector3 rgb(value.color.x, value.color.y, value.color.z);
                if ((item.mode == GradientValueMode::Gradient || item.mode == GradientValueMode::TwoGradients ||
                     item.mode == GradientValueMode::RandomColor) && value.gradient.colorKeys.empty()) {
                    value.gradient = Gradient::constant(rgb, value.color.w);
                }
                if (item.mode == GradientValueMode::TwoGradients && value.gradientMax.colorKeys.empty()) value.gradientMax = value.gradient;
                value.mode = item.mode;
                phase = EditPhase::Applied;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return phase;
}

// ---- the window

GradientWindow& GradientWindow::get() {
    static GradientWindow window;
    return window;
}

void GradientWindow::beginEdit(CommandHistory* history, const char* label) {
    GradientWindow& window = get();
    window.history = history;
    window.stepName = std::string("Change ") + (label && *label ? label : "Gradient");
}

void GradientWindow::open(ImGuiID id, const Gradient& value, const GradientFieldOptions& fieldOptions) {
    if (target != 0) close();
    setBeginEditHandler(&GradientWindow::beginEdit);
    target = id;
    options = fieldOptions;
    gradient = value;
    title = options.label ? options.label : "Gradient Editor";
    focusOnOpen = true;
    lastSeenFrame = ImGui::GetFrameCount();
    history = nullptr;
    stepName = "Change Gradient";
    changed = false;
    gestureOpen = false;
    commitAfterDelivery = false;
    closeAfterDelivery = false;
    selectedColor = gradient.colorKeys.empty() ? -1 : 0;
    selectedAlpha = -1;
    drag = Drag::None;
    warning.clear();
}

EditPhase GradientWindow::consume(ImGuiID id, Gradient& value) {
    if (id == 0) return EditPhase::None;
    if (endedTarget == id && id != target) {
        endedTarget = 0;
        return EditPhase::Committed;
    }
    if (id != target) return EditPhase::None;
    lastSeenFrame = ImGui::GetFrameCount();
    if (changed) {
        changed = false;
        value = gradient;
        return EditPhase::Changing;
    }
    if (endedTarget == id) {
        endedTarget = 0;
        return EditPhase::Committed;
    }
    // between gestures the window follows the field (an undo, another edit)
    if (!gestureOpen && !commitAfterDelivery && drag == Drag::None) gradient = value;
    return EditPhase::None;
}

void GradientWindow::beginGesture() {
    if (gestureOpen) return;
    if (history) FieldEditSession::begin(history, stepName, this);
    gestureOpen = true;
}

void GradientWindow::deliver(bool ends) {
    changed = true;
    if (ends) commitAfterDelivery = true;
}

void GradientWindow::close() {
    if (target == 0) return;
    if (gestureOpen || commitAfterDelivery) {
        FieldEditSession::commit(this);
        gestureOpen = false;
        commitAfterDelivery = false;
    }
    endedTarget = target;
    target = 0;
    changed = false;
}

void GradientWindow::drawKeys(ImVec2 barMin, ImVec2 barMax) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float width = barMax.x - barMin.x;
    const float strip = Theme::dpi(14.0f);
    const float timeRadius = Theme::dpi(6.0f) / std::max(width, 1.0f);
    auto timeAt = [&](float x) { return std::clamp((x - barMin.x) / std::max(width, 1.0f), 0.0f, 1.0f); };

    for (int row = 0; row < 2; row++) {
        const bool alphaRow = row == 0;
        const ImVec2 min(barMin.x - strip * 0.5f, alphaRow ? barMin.y - strip : barMax.y);
        const ImVec2 max(barMax.x + strip * 0.5f, alphaRow ? barMin.y : barMax.y + strip);
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton(alphaRow ? "##alphaKeys" : "##colorKeys", ImVec2(max.x - min.x, max.y - min.y));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float t = timeAt(io.MousePos.x);
            const int hit = alphaRow ? nearestKey(gradient.alphaKeys, t, timeRadius) : nearestKey(gradient.colorKeys, t, timeRadius);
            beforeDrag = gradient;
            if (hit >= 0) {
                if (alphaRow) {
                    selectedAlpha = hit;
                    selectedColor = -1;
                } else {
                    selectedColor = hit;
                    selectedAlpha = -1;
                }
                drag = alphaRow ? Drag::Alpha : Drag::Color;
                dragIndex = hit;
                dragOffset = (alphaRow ? gradient.alphaKeys[(size_t)hit].time : gradient.colorKeys[(size_t)hit].time) - t;
            } else if (alphaRow ? gradient.alphaKeys.size() < options.maxKeys : gradient.colorKeys.size() < options.maxKeys) {
                // a click on the strip adds a key with the gradient's value there
                beginGesture();
                if (alphaRow) {
                    selectedAlpha = gradientops::addAlphaKey(gradient, t, options.maxKeys);
                    selectedColor = -1;
                } else {
                    selectedColor = gradientops::addColorKey(gradient, t, options.maxKeys);
                    selectedAlpha = -1;
                }
                deliver(true);
            } else {
                warning = "A gradient has at most " + std::to_string(options.maxKeys) + " colour and " + std::to_string(options.maxKeys) + " alpha keys.";
            }
        }
        // the markers: alpha keys point down at the bar, colour keys up
        const size_t count = alphaRow ? gradient.alphaKeys.size() : gradient.colorKeys.size();
        for (size_t i = 0; i < count; i++) {
            const float t = alphaRow ? gradient.alphaKeys[i].time : gradient.colorKeys[i].time;
            const float x = barMin.x + t * width;
            const bool selectedKey = alphaRow ? (int)i == selectedAlpha : (int)i == selectedColor;
            const ImU32 fill = alphaRow ? IM_COL32(colormath::toByte(gradient.alphaKeys[i].alpha), colormath::toByte(gradient.alphaKeys[i].alpha),
                                                   colormath::toByte(gradient.alphaKeys[i].alpha), 255)
                                        : srgbColor(gradient.colorKeys[i].color, 1.0f);
            const float h = strip;
            const ImVec2 tip(x, alphaRow ? barMin.y : barMax.y);
            const ImVec2 a(x - h * 0.45f, alphaRow ? barMin.y - h : barMax.y + h);
            const ImVec2 b(x + h * 0.45f, a.y);
            drawList->AddTriangleFilled(tip, a, b, fill);
            drawList->AddTriangle(tip, a, b, selectedKey ? IM_COL32(255, 220, 80, 255) : IM_COL32(0, 0, 0, 255), selectedKey ? 2.0f : 1.0f);
        }
    }

    // a key's drag: along the bar; off the bar (far up or down) it is deleted while held there
    if (drag != Drag::None) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (gestureOpen) {
                gradient = beforeDrag;
                if (history) {
                    FieldEditSession::cancel(this);
                    gestureOpen = false;
                } else {
                    deliver(true);
                }
            }
            drag = Drag::None;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float t = timeAt(io.MousePos.x + dragOffset * width);
            const bool off = io.MousePos.y < barMin.y - strip * 3.0f || io.MousePos.y > barMax.y + strip * 3.0f;
            Gradient next = beforeDrag;
            if (drag == Drag::Alpha && dragIndex >= 0 && (size_t)dragIndex < next.alphaKeys.size()) {
                if (off && gradientops::removeAlphaKey(next, (size_t)dragIndex)) {
                    selectedAlpha = -1;
                } else {
                    selectedAlpha = gradientops::moveAlphaKey(next, (size_t)dragIndex, t);
                }
            } else if (drag == Drag::Color && dragIndex >= 0 && (size_t)dragIndex < next.colorKeys.size()) {
                if (off && gradientops::removeColorKey(next, (size_t)dragIndex)) {
                    selectedColor = -1;
                } else {
                    selectedColor = gradientops::moveColorKey(next, (size_t)dragIndex, t);
                }
            }
            if (next != gradient) {
                beginGesture();
                gradient = next;
                deliver(false);
            }
        } else {
            drag = Drag::None;
            if (gestureOpen) deliver(true);
        }
    }
}

void GradientWindow::drawSelected() {
    const float labelWidth = ImGui::CalcTextSize("Location").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    if (selectedColor >= 0 && (size_t)selectedColor < gradient.colorKeys.size()) {
        GradientColorKey& key = gradient.colorKeys[(size_t)selectedColor];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Color");
        ImGui::SameLine(labelWidth);
        ColorFieldOptions colorOptions;
        colorOptions.alpha = false;
        colorOptions.linearStorage = false;   // the keys are sRGB
        colorOptions.label = "Gradient Color Key";
        colorOptions.width = Theme::dpi(140.0f);
        Vector3 color = key.color;
        const EditPhase phase = colorField("##keyColor", color, colorOptions);
        // the Color window's session is this window's gesture (one step), ended once the
        // gradient reached the field
        if (phase == EditPhase::Begin) {
            beginGesture();
        } else if (phase == EditPhase::Changing) {
            beginGesture();
            key.color = color;
            deliver(false);
        } else if (phase == EditPhase::Committed) {
            if (gestureOpen) deliver(true);
        } else if (phase == EditPhase::Cancelled) {
            key.color = color;
            if (gestureOpen && history) {
                FieldEditSession::cancel(this);
                gestureOpen = false;
                changed = false;
            } else {
                deliver(true);
            }
        }
    } else if (selectedAlpha >= 0 && (size_t)selectedAlpha < gradient.alphaKeys.size()) {
        GradientAlphaKey& key = gradient.alphaKeys[(size_t)selectedAlpha];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Alpha");
        ImGui::SameLine(labelWidth);
        int alpha = colormath::toByte(key.alpha);
        ImGui::SetNextItemWidth(Theme::dpi(140.0f));
        if (ImGui::SliderInt("##keyAlpha", &alpha, 0, 255)) {
            beginGesture();
            key.alpha = colormath::fromByte((uint8_t)alpha);
            deliver(false);
        }
        if (ImGui::IsItemDeactivated() && gestureOpen) deliver(true);
    } else {
        ImGui::TextDisabled("Select a key: alpha keys above the bar, colour keys below.");
        return;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Location");
    ImGui::SameLine();
    float* time = selectedColor >= 0 ? &gradient.colorKeys[(size_t)selectedColor].time : &gradient.alphaKeys[(size_t)selectedAlpha].time;
    float percent = *time * 100.0f;
    ImGui::SetNextItemWidth(Theme::dpi(80.0f));
    if (ImGui::DragFloat("##keyLocation", &percent, 0.2f, 0.0f, 100.0f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp)) {
        beginGesture();
        *time = std::clamp(percent / 100.0f, 0.0f, 1.0f);
        const float moved = *time;
        gradient.sortKeys();
        if (selectedColor >= 0) selectedColor = nearestKey(gradient.colorKeys, moved, 1e-6f);
        else selectedAlpha = nearestKey(gradient.alphaKeys, moved, 1e-6f);
        deliver(false);
    }
    if (ImGui::IsItemDeactivated() && gestureOpen) deliver(true);
}

bool GradientWindow::applyPreset(const Gradient& preset) {
    if (target == 0) return false;
    // refused whole rather than cut: a preset is a look, and a resampled one is another look
    // (a .procmat ramp holds 32 keys, a particle gradient 8; past its limit the codec refuses the file)
    if (preset.colorKeys.size() > options.maxKeys || preset.alphaKeys.size() > options.maxKeys) {
        warning = "This preset has " + std::to_string(preset.colorKeys.size()) + " colour keys and " + std::to_string(preset.alphaKeys.size()) +
                  " alpha keys; this gradient holds at most " + std::to_string(options.maxKeys) + " of each.";
        return false;
    }
    beginGesture();
    gradient = preset;
    selectedColor = gradient.colorKeys.empty() ? -1 : 0;
    selectedAlpha = -1;
    deliver(true);
    return true;
}

void GradientWindow::drawPresets() {
    ColorPickerWindow& colors = ColorPickerWindow::get();
    const std::vector<SwatchLibrary>& libraries = colors.libraries();
    ImGui::TextUnformatted("Presets");
    for (size_t l = 0; l < libraries.size(); l++) {
        const SwatchLibrary& library = libraries[l];
        for (size_t i = 0; i < library.gradients.size(); i++) {
            ImGui::PushID((int)(l * 1000 + i));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 size(Theme::dpi(56.0f), ImGui::GetFrameHeight());
            if (ImGui::InvisibleButton("##gradientPreset", size)) applyPreset(library.gradients[i].gradient);
            drawGradientBar(ImGui::GetWindowDrawList(), min, ImVec2(min.x + size.x, min.y + size.y), library.gradients[i].gradient);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", library.gradients[i].name.c_str(), library.name.c_str());
            ImGui::PopID();
            ImGui::SameLine();
        }
    }
    // the current gradient into the first library that can be written
    if (ImGui::Button("+##saveGradient")) {
        std::string error = "no writable swatch library";
        for (size_t l = 0; l < libraries.size(); l++) {
            if (libraries[l].readOnly) continue;
            SwatchLibrary library = libraries[l];
            library.gradients.push_back({"Gradient " + std::to_string(library.gradients.size() + 1), gradient});
            if (swatches::write(library, error)) {
                error.clear();
                colors.reloadLibraries();
            }
            break;
        }
        if (!error.empty()) warning = error;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the gradient as a preset (the first user swatch library)");
}

void GradientWindow::show() {
    if (target != 0 && ImGui::GetFrameCount() - lastSeenFrame > 1) close();
    if (target != 0 && commitAfterDelivery && !changed) {
        FieldEditSession::commit(this);
        gestureOpen = false;
        commitAfterDelivery = false;
        endedTarget = target;
    }
    if (target != 0 && closeAfterDelivery && !changed) {
        closeAfterDelivery = false;
        close();
    }
    windowFocused = false;
    if (target == 0) return;

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(420.0f), Theme::dpi(200.0f)), ImGuiCond_FirstUseEver);
    if (focusOnOpen) {
        ImGui::SetNextWindowFocus();
        focusOnOpen = false;
    }
    const std::string windowTitle = title + "###" + WINDOW_NAME;
    if (ImGui::Begin(windowTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // Mode and the blend space
        static const char* const kModes[] = {"Blend", "Fixed"};
        int mode = (int)gradient.mode;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Mode");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        if (ImGui::Combo("##mode", &mode, kModes, 2)) {
            beginGesture();
            gradient.mode = (GradientMode)mode;
            deliver(true);
        }
        ImGui::SameLine();
        bool linear = gradient.linear;
        if (ImGui::Checkbox("Blend in Linear Space", &linear)) {
            beginGesture();
            gradient.linear = linear;
            deliver(true);
        }
        const float strip = Theme::dpi(14.0f);
        ImGui::Dummy(ImVec2(0.0f, strip));
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x - strip;
        const ImVec2 barMin(cursor.x + strip * 0.5f, cursor.y);
        const ImVec2 barMax(barMin.x + std::max(width, 16.0f), barMin.y + Theme::dpi(24.0f));
        drawGradientBar(ImGui::GetWindowDrawList(), barMin, barMax, gradient);
        drawKeys(barMin, barMax);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x, barMax.y + strip + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        drawSelected();
        ImGui::Separator();
        drawPresets();
        ImGui::NewLine();
        ImGui::TextDisabled("%zu / %zu colour keys, %zu / %zu alpha keys", gradient.colorKeys.size(), options.maxKeys,
                            gradient.alphaKeys.size(), options.maxKeys);
        if (!warning.empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", warning.c_str());
    }
    ImGui::End();
    if (!open) {
        if (changed) closeAfterDelivery = true;
        else close();
    }
}
