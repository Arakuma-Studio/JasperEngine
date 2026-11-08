// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The scrub slop, the modifier steps and the logarithmic slider follow Pachislop's NumberField
// and ScrubLabel (src/ui/controls.tsx) by SafEight, used with the author's permission; the
// modifiers are Unity's (Shift faster, Alt slower).

#include "DocRow.h"

#include "external/IconsFontAwesome6.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using namespace jasperengine::editor;
using namespace jasperengine::editor::ui;

namespace {

    std::function<double()>& clockOverride() {
        static std::function<double()> clock;
        return clock;
    }

    double now() {
        return clockOverride() ? clockOverride()() : ImGui::GetTime();
    }

    // per row transient state (ImGuiID of the row)
    struct RowState {
        bool pressed = false;           // the value or the label is held
        bool scrubbing = false;
        float pressX = 0.0f;
        double scrubbed = 0.0;          // the delta reported so far
        bool wheelBurst = false;
        double lastWheel = 0.0;
        double wheelDelta = 0.0;
        double flashUntil = 0.0;        // the clamp flash
        bool editing = false;           // the text input is open
        std::string buffer;
        bool focusNext = false;
        bool sliderHeld = false;
        double sliderStart = 0.0;
    };

    std::map<ImGuiID, RowState>& states() {
        static std::map<ImGuiID, RowState> map;
        return map;
    }

    const ImU32 AMBER = IM_COL32(230, 160, 40, 255);
    const ImU32 FLASH = IM_COL32(80, 140, 230, 110);

    std::string withThousands(double value, int decimals) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(0, decimals), value);
        std::string text = buffer;
        const size_t dot = text.find('.');
        size_t end = dot == std::string::npos ? text.size() : dot;
        size_t start = (text[0] == '-') ? 1 : 0;
        for (long i = (long)end - 3; i > (long)start; i -= 3) text.insert((size_t)i, ",");
        return text;
    }

    int decimalsOf(const DocRowOptions& options) {
        if (options.decimals >= 0) return options.decimals;
        if (options.integer) return 0;
        const double step = docRowStep(options);
        if (step >= 1.0) return 0;
        if (step >= 0.1) return 1;
        if (step >= 0.01) return 2;
        return 3;
    }

    std::string fullTooltip(const DocRowOptions& options) {
        std::string text = options.tooltip;
        auto add = [&text](const std::string& line) {
            if (!text.empty()) text += "\n";
            text += line;
        };
        if (std::isfinite(options.hardMin) || std::isfinite(options.hardMax)) {
            std::string range = "Range: ";
            range += std::isfinite(options.hardMin) ? formatDocNumber(options.hardMin, options) : std::string("-inf");
            range += " to ";
            range += std::isfinite(options.hardMax) ? formatDocNumber(options.hardMax, options) : std::string("inf");
            if (options.slider == SliderMap::Log) range += " (logarithmic slider)";
            add(range);
        }
        if (options.canReset && !options.mixed) add("Default: " + formatDocNumber(options.defaultValue, options));
        if (!options.fieldPath.empty()) add("Path: " + options.fieldPath);
        if (options.readOnly && !options.readOnlyReason.empty()) add(options.readOnlyReason);
        return text;
    }

    // the label column with the gutter (source dot), the context menu and the double-click reset
    DocRowFrame beginFrame(const char* id, const DocRowOptions& options, bool scrubLabel, std::string tooltipOverride = {}) {
        DocRowFrame frame;
        const float full = ImGui::GetContentRegionAvail().x;
        const float labelWidth = options.labelWidth > 0.0f ? options.labelWidth : std::max(80.0f, full * 0.4f);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        const float gutter = ImGui::GetFontSize() * 0.9f;

        if (options.flash) {
            ImGui::GetWindowDrawList()->AddRectFilled(start, ImVec2(start.x + full, start.y + height), FLASH, 3.0f);
        }
        // source dot
        if (options.source != SourceDot::None) {
            const ImVec2 centre(start.x + gutter * 0.5f, start.y + height * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y));
            ImGui::InvisibleButton("##dot", ImVec2(gutter, height));
            if (ImGui::IsItemClicked() && !options.readOnly) frame.dotClicked = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", options.source == SourceDot::SetHere ? "Set here: click to use the source again"
                                                                             : "From the source: click to set it here");
            }
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
            if (options.source == SourceDot::SetHere) ImGui::GetWindowDrawList()->AddCircleFilled(centre, gutter * 0.22f, color);
            else ImGui::GetWindowDrawList()->AddCircle(centre, gutter * 0.22f, color, 0, 1.2f);
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x + gutter, start.y));
        ImGui::InvisibleButton("##label", ImVec2(std::max(1.0f, labelWidth - gutter), height));
        const bool labelHovered = ImGui::IsItemHovered();
        if (scrubLabel && labelHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (scrubLabel && ImGui::IsItemActive()) {
            frame.labelActive = true;
            frame.labelDragX = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
        }
        if (scrubLabel && ImGui::IsItemDeactivated()) frame.labelReleased = true;
        if (labelHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && options.canReset && !options.readOnly) {
            frame.labelDoubleClicked = true;
        }
        const std::string tip = tooltipOverride.empty() ? fullTooltip(options) : tooltipOverride;
        if (labelHovered && !tip.empty() && !frame.labelActive) ImGui::SetTooltip("%s", tip.c_str());
        // label text, dim at its default (changed values stand out)
        ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (options.isDefault && !options.mixed) textColor.w *= 0.62f;
        const ImVec2 textPos(start.x + gutter, start.y + (height - ImGui::GetFontSize()) * 0.5f);
        ImGui::GetWindowDrawList()->PushClipRect(ImVec2(start.x, start.y), ImVec2(start.x + labelWidth - 4.0f, start.y + height), true);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(textColor), options.label);
        ImGui::GetWindowDrawList()->PopClipRect();
        // the row menu
        ImGui::PushID("menu");
        if (ImGui::BeginPopupContextItem("##rowmenu")) {
            if (ImGui::MenuItem("Reset", nullptr, false, options.canReset && !options.readOnly && !options.isDefault)) frame.menuReset = true;
            if (options.source != SourceDot::None && ImGui::MenuItem(options.sourceMenu.c_str(), nullptr, false, !options.readOnly)) frame.menuUseSource = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Value", nullptr, false, !options.mixed)) frame.menuCopyValue = true;
            if (ImGui::MenuItem("Paste Value", nullptr, false, !options.readOnly && ImGui::GetClipboardText() != nullptr)) frame.menuPaste = true;
            if (ImGui::MenuItem("Copy Property Path", nullptr, false, !options.fieldPath.empty())) frame.menuCopyPath = true;
            if (options.canExpose) {
                ImGui::Separator();
                if (ImGui::MenuItem("Expose as Parameter...", nullptr, false, !options.readOnly)) frame.menuExpose = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        (void)id;
        ImGui::SetCursorScreenPos(ImVec2(start.x + labelWidth, start.y));
        const float resetWidth = ImGui::GetFrameHeight();
        frame.valueWidth = std::max(30.0f, full - labelWidth - resetWidth - ImGui::GetStyle().ItemSpacing.x);
        return frame;
    }

    // the reset arrow right of the value; true when clicked
    bool endFrame(const DocRowOptions& options) {
        ImGui::SameLine();
        const float size = ImGui::GetFrameHeight();
        bool clicked = false;
        if (options.canReset && !options.isDefault && !options.mixed && !options.readOnly && options.source == SourceDot::None) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(ICON_FA_ROTATE_LEFT "##reset", ImVec2(size, size))) clicked = true;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to %s", formatDocNumber(options.defaultValue, options).c_str());
        } else {
            ImGui::Dummy(ImVec2(size, size));
        }
        return clicked;
    }

    void applyFrame(DocRowResult& result, const DocRowFrame& frame, bool resetClicked, const std::string& copyText, const DocRowOptions& options) {
        if (frame.labelDoubleClicked || frame.menuReset || resetClicked) result.reset = true;
        if (frame.dotClicked) result.toggleSource = true;
        if (frame.menuUseSource) result.useSource = true;
        if (frame.menuCopyValue) ImGui::SetClipboardText(copyText.c_str());
        if (frame.menuCopyPath) {
            ImGui::SetClipboardText(options.fieldPath.c_str());
            result.copyPath = true;
        }
        if (frame.menuExpose) result.expose = true;
        if (frame.menuPaste) {
            const char* clip = ImGui::GetClipboardText();
            if (clip) {
                result.pasted = true;
                result.text = clip;
            }
        }
    }

    bool parseNumber(const std::string& text, double& out) {
        std::string clean;
        for (char c : text) {
            if (c == ',' || c == ' ') continue;
            clean += c;
        }
        if (clean.empty()) return false;
        char* end = nullptr;
        const double v = std::strtod(clean.c_str(), &end);
        if (end == clean.c_str() || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

}

void jasperengine::editor::ui::setDocRowClockForTesting(std::function<double()> clock) {
    clockOverride() = std::move(clock);
}

double jasperengine::editor::ui::docRowStep(const DocRowOptions& options) {
    if (options.step > 0.0) return options.step;
    if (options.integer) return 1.0;
    if (std::isfinite(options.uiMin) && std::isfinite(options.uiMax) && options.uiMax > options.uiMin) {
        const double span = options.uiMax - options.uiMin;
        // a round step near 1 % of the range
        const double raw = span / 100.0;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
        return magnitude;
    }
    return 0.1;
}

std::string jasperengine::editor::ui::formatDocNumber(double value, const DocRowOptions& options) {
    std::string text = withThousands(value, decimalsOf(options));
    // trim trailing zeros of fractions ("2.50" -> "2.5")
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (options.unit && options.unit[0]) {
        if (std::string(options.unit) != "°") text += " ";
        text += options.unit;
    }
    return text;
}

double jasperengine::editor::ui::sliderToValue(double t, const DocRowOptions& options) {
    t = std::clamp(t, 0.0, 1.0);
    const double lo = options.uiMin, hi = options.uiMax;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) return lo;
    double v = 0.0;
    if (options.slider == SliderMap::Log) {
        // log of (v - lo + 1): works from a minimum of 0
        const double span = std::log(hi - lo + 1.0);
        v = lo + std::exp(t * span) - 1.0;
    } else {
        v = lo + t * (hi - lo);
    }
    if (options.integer) v = std::round(v);
    return std::clamp(v, lo, hi);
}

double jasperengine::editor::ui::valueToSlider(double value, const DocRowOptions& options) {
    const double lo = options.uiMin, hi = options.uiMax;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) return 0.0;
    const double v = std::clamp(value, lo, hi);
    if (options.slider == SliderMap::Log) return std::log(v - lo + 1.0) / std::log(hi - lo + 1.0);
    return (v - lo) / (hi - lo);
}

double jasperengine::editor::ui::scrubDelta(float pixels, const DocRowOptions& options, bool shift, bool alt) {
    double step = docRowStep(options);
    if (shift) step *= 10.0;
    if (alt) step *= 0.1;
    // a pixel is a step; integers move whole steps
    double delta = pixels * step;
    if (options.integer) delta = std::trunc(delta);
    return delta;
}

double jasperengine::editor::ui::clampTyped(double value, const DocRowOptions& options, bool& clamped) {
    const double v = std::clamp(value, options.hardMin, options.hardMax);
    clamped = v != value;
    return options.integer ? std::round(v) : v;
}

std::string jasperengine::editor::ui::clampMessage(double value, const DocRowOptions& options) {
    const bool high = value >= options.hardMax;
    const double limit = high ? options.hardMax : options.hardMin;
    return "Clamped to " + formatDocNumber(limit, options) + (high ? " (the maximum)" : " (the minimum)");
}

DocRowResult jasperengine::editor::ui::docRowNumber(const char* id, double value, const DocRowOptions& options) {
    DocRowResult result;
    ImGui::PushID(id);
    const ImGuiID rowId = ImGui::GetID("##row");
    RowState& state = states()[rowId];
    const double t = now();
    const bool flashing = state.flashUntil > t;
    std::string tip;
    if (flashing) tip = clampMessage(state.wheelDelta, options);
    DocRowFrame frame = beginFrame(id, options, !options.readOnly, tip);
    ImGuiIO& io = ImGui::GetIO();
    const bool shift = io.KeyShift, alt = io.KeyAlt;

    // label scrub
    if (frame.labelActive && !options.readOnly) {
        if (!state.scrubbing && std::abs(frame.labelDragX) >= DOC_ROW_SCRUB_SLOP) {
            state.scrubbing = true;
            state.scrubbed = 0.0;
            state.pressX = io.MousePos.x;
            result.phase = EditPhase::Begin;
            result.kind = DocRowResult::Kind::Shift;
        }
        if (state.scrubbing) {
            const double delta = scrubDelta(io.MousePos.x - state.pressX, options, shift, alt);
            if (delta != state.scrubbed) {
                state.scrubbed = delta;
                if (result.phase == EditPhase::None) result.phase = EditPhase::Changing;
                result.kind = DocRowResult::Kind::Shift;
                result.delta = delta;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.scrubbing = false;
                result.phase = EditPhase::Cancelled;
                ImGui::ClearActiveID();
            }
        }
    }
    if (frame.labelReleased && state.scrubbing) {
        state.scrubbing = false;
        result.phase = EditPhase::Committed;
        result.kind = DocRowResult::Kind::Shift;
        result.delta = state.scrubbed;
    }

    const float width = frame.valueWidth;
    const bool hasSlider = options.slider != SliderMap::None && std::isfinite(options.uiMin) && std::isfinite(options.uiMax) &&
                           options.uiMax > options.uiMin;
    const bool steppers = options.integer && hasSlider && options.uiMax - options.uiMin <= 24.0;
    float fieldWidth = width;
    if (options.readOnly) ImGui::BeginDisabled();

    if (steppers) {
        const float button = ImGui::GetFrameHeight();
        if (ImGui::Button("-##dec", ImVec2(button, button)) && !options.readOnly) {
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Shift;
            result.delta = -docRowStep(options);
        }
        ImGui::SameLine(0.0f, 2.0f);
        fieldWidth = width - 2.0f * (button + 2.0f);
    } else if (hasSlider) {
        // the slider track (absolute: Set) left of the number field
        const float trackWidth = std::max(0.0f, width * 0.55f);
        fieldWidth = width - trackWidth - ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##slider", ImVec2(trackWidth, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = p.y + h * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, y - 2.0f), ImVec2(p.x + trackWidth, y + 2.0f), ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
        const float knobT = options.mixed ? -1.0f : (float)valueToSlider(value, options);
        if (knobT >= 0.0f) {
            dl->AddRectFilled(ImVec2(p.x, y - 2.0f), ImVec2(p.x + trackWidth * knobT, y + 2.0f), ImGui::GetColorU32(ImGuiCol_SliderGrab), 2.0f);
            dl->AddCircleFilled(ImVec2(p.x + trackWidth * knobT, y), h * 0.28f,
                                ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab));
        }
        if (ImGui::IsItemHovered() && options.slider == SliderMap::Log) ImGui::SetTooltip("Logarithmic slider");
        if (ImGui::IsItemActivated()) {
            state.sliderHeld = true;
            result.phase = EditPhase::Begin;
        }
        if (ImGui::IsItemActive() && state.sliderHeld) {
            const double v = sliderToValue((io.MousePos.x - p.x) / std::max(1.0f, trackWidth), options);
            if (result.phase == EditPhase::None) result.phase = EditPhase::Changing;
            result.kind = DocRowResult::Kind::Set;
            result.value = v;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.sliderHeld = false;
                result.phase = EditPhase::Cancelled;
                ImGui::ClearActiveID();
            }
        }
        if (ImGui::IsItemDeactivated() && state.sliderHeld) {
            state.sliderHeld = false;
            result.phase = EditPhase::Committed;
            result.kind = DocRowResult::Kind::Set;
            result.value = sliderToValue((io.MousePos.x - p.x) / std::max(1.0f, trackWidth), options);
        }
        ImGui::SameLine();
    }

    // the number field: a button-like frame that scrubs on drag and turns into a text input on click
    ImGui::SetNextItemWidth(fieldWidth);
    if (state.editing) {
        if (state.focusNext) {
            ImGui::SetKeyboardFocusHere();
            state.focusNext = false;
        }
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", state.buffer.c_str());
        const bool enter = ImGui::InputText("##edit", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        state.buffer = buffer;
        // arrows step while typing focus is on the field
        if (ImGui::IsItemActive() && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
            double step = docRowStep(options) * (shift ? 10.0 : 1.0) * (alt ? 0.1 : 1.0);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step = -step;
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Shift;
            result.delta = step;
            state.editing = false;
        }
        // the wheel edits only the focused field; a burst is one step
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            const double step = docRowStep(options) * (shift ? 10.0 : 1.0) * (alt ? 0.1 : 1.0) * (io.MouseWheel > 0 ? 1.0 : -1.0);
            if (!state.wheelBurst) {
                state.wheelBurst = true;
                state.wheelDelta = 0.0;
                result.phase = EditPhase::Begin;
            } else {
                result.phase = EditPhase::Changing;
            }
            state.wheelDelta += step;
            state.lastWheel = t;
            result.kind = DocRowResult::Kind::Shift;
            result.delta = state.wheelDelta;
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
        }
        const bool cancel = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (cancel) {
            state.editing = false;
        } else if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            double typed = 0.0;
            if (parseNumber(state.buffer, typed)) {
                bool clamped = false;
                const double v = clampTyped(typed, options, clamped);
                result.phase = EditPhase::Applied;
                result.kind = DocRowResult::Kind::Set;
                result.value = v;
                result.clamped = clamped;
                if (clamped) {
                    state.flashUntil = t + DOC_ROW_CLAMP_FLASH_SECONDS;
                    state.wheelDelta = typed;
                }
            }
            state.editing = false;
        } else if (!ImGui::IsItemActive() && !ImGui::IsItemActivated() && !state.focusNext) {
            state.editing = false;
        }
    } else {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##value", ImVec2(fieldWidth, h));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && !options.readOnly) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 frameColor = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        dl->AddRectFilled(p, ImVec2(p.x + fieldWidth, p.y + h), frameColor, ImGui::GetStyle().FrameRounding);
        if (flashing) dl->AddRect(p, ImVec2(p.x + fieldWidth, p.y + h), AMBER, ImGui::GetStyle().FrameRounding, 0, 2.0f);
        const std::string text = options.mixed ? std::string("\xE2\x80\x94") : formatDocNumber(value, options);
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(p.x + std::max(4.0f, (fieldWidth - size.x) * 0.5f), p.y + (h - size.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                    text.c_str());
        if (hovered && flashing) ImGui::SetTooltip("%s", tip.c_str());
        else if (hovered && !options.fieldPath.empty() && !ImGui::IsItemActive()) ImGui::SetTooltip("%s", fullTooltip(options).c_str());
        if (ImGui::IsItemActivated()) {
            state.pressed = true;
            state.pressX = io.MousePos.x;
            state.scrubbed = 0.0;
        }
        if (ImGui::IsItemActive() && state.pressed && !options.readOnly) {
            const float dx = io.MousePos.x - state.pressX;
            if (!state.scrubbing && std::abs(dx) >= DOC_ROW_SCRUB_SLOP) {
                state.scrubbing = true;
                state.pressX = io.MousePos.x;
                result.phase = EditPhase::Begin;
                result.kind = DocRowResult::Kind::Shift;
            }
            if (state.scrubbing) {
                const double delta = scrubDelta(io.MousePos.x - state.pressX, options, shift, alt);
                if (delta != state.scrubbed) {
                    state.scrubbed = delta;
                    if (result.phase == EditPhase::None) result.phase = EditPhase::Changing;
                    result.kind = DocRowResult::Kind::Shift;
                    result.delta = delta;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    state.scrubbing = false;
                    state.pressed = false;
                    result.phase = EditPhase::Cancelled;
                    ImGui::ClearActiveID();
                }
            }
        }
        if (ImGui::IsItemDeactivated() && state.pressed) {
            state.pressed = false;
            if (state.scrubbing) {
                state.scrubbing = false;
                result.phase = EditPhase::Committed;
                result.kind = DocRowResult::Kind::Shift;
                result.delta = state.scrubbed;
            } else if (!options.readOnly) {
                // a click under the slop focuses the field with its text selected
                state.editing = true;
                state.focusNext = true;
                state.buffer = options.mixed ? std::string() : formatDocNumber(value, DocRowOptions{.unit = "", .decimals = decimalsOf(options)});
            }
        }
    }
    // the burst ends 600 ms after its last notch
    if (state.wheelBurst && t - state.lastWheel > DOC_ROW_WHEEL_BURST_SECONDS && result.phase == EditPhase::None) {
        state.wheelBurst = false;
        result.phase = EditPhase::Committed;
        result.kind = DocRowResult::Kind::Shift;
        result.delta = state.wheelDelta;
    }
    if (steppers) {
        ImGui::SameLine(0.0f, 2.0f);
        const float button = ImGui::GetFrameHeight();
        if (ImGui::Button("+##inc", ImVec2(button, button)) && !options.readOnly) {
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Shift;
            result.delta = docRowStep(options);
        }
    }
    if (options.readOnly) ImGui::EndDisabled();
    const bool resetClicked = endFrame(options);
    applyFrame(result, frame, resetClicked, formatDocNumber(value, DocRowOptions{.unit = "", .decimals = 6}), options);
    if (result.pasted) {
        double pasted = 0.0;
        if (parseNumber(result.text, pasted)) {
            bool clamped = false;
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Set;
            result.value = clampTyped(pasted, options, clamped);
            result.clamped = clamped;
        } else {
            result.pasted = false;
        }
    }
    ImGui::PopID();
    return result;
}

DocRowResult jasperengine::editor::ui::docRowBool(const char* id, bool value, const DocRowOptions& options) {
    DocRowResult result;
    ImGui::PushID(id);
    DocRowFrame frame = beginFrame(id, options, false);
    if (options.readOnly) ImGui::BeginDisabled();
    bool v = value;
    if (options.mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::Checkbox("##value", &v)) {
        result.phase = EditPhase::Applied;
        result.kind = DocRowResult::Kind::Set;
        result.value = (options.mixed ? true : v) ? 1.0 : 0.0;
    }
    if (options.mixed) ImGui::PopItemFlag();
    if (options.readOnly) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(std::max(0.0f, frame.valueWidth - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x), ImGui::GetFrameHeight()));
    const bool resetClicked = endFrame(options);
    applyFrame(result, frame, resetClicked, value ? "true" : "false", options);
    if (result.pasted) {
        result.phase = EditPhase::Applied;
        result.kind = DocRowResult::Kind::Set;
        result.value = (result.text == "true" || result.text == "1") ? 1.0 : 0.0;
    }
    ImGui::PopID();
    return result;
}

DocRowResult jasperengine::editor::ui::docRowEnum(const char* id, int index, const std::vector<std::string>& labels, const DocRowOptions& options) {
    DocRowResult result;
    ImGui::PushID(id);
    DocRowFrame frame = beginFrame(id, options, false);
    if (options.readOnly) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(frame.valueWidth);
    const char* preview = (!options.mixed && index >= 0 && index < (int)labels.size()) ? labels[(size_t)index].c_str() : "";
    if (ImGui::BeginCombo("##value", preview)) {
        for (int i = 0; i < (int)labels.size(); i++) {
            const bool selected = !options.mixed && i == index;
            if (ImGui::Selectable(labels[(size_t)i].c_str(), selected)) {
                result.phase = EditPhase::Applied;
                result.kind = DocRowResult::Kind::Set;
                result.value = i;
                result.text = labels[(size_t)i];
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (options.readOnly) ImGui::EndDisabled();
    const bool resetClicked = endFrame(options);
    applyFrame(result, frame, resetClicked, preview, options);
    if (result.pasted) {
        auto it = std::find(labels.begin(), labels.end(), result.text);
        if (it != labels.end()) {
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Set;
            result.value = (double)(it - labels.begin());
        } else {
            result.pasted = false;
        }
    }
    ImGui::PopID();
    return result;
}

DocRowResult jasperengine::editor::ui::docRowText(const char* id, const std::string& value, const DocRowOptions& options, bool multiLine, float height) {
    DocRowResult result;
    ImGui::PushID(id);
    const ImGuiID rowId = ImGui::GetID("##row");
    RowState& state = states()[rowId];
    DocRowFrame frame = beginFrame(id, options, false);
    if (options.readOnly) ImGui::BeginDisabled();
    if (!state.editing) state.buffer = options.mixed ? std::string() : value;
    std::vector<char> buffer(std::max<size_t>(state.buffer.size() + 1024, 4096));
    std::copy(state.buffer.begin(), state.buffer.end(), buffer.begin());
    buffer[state.buffer.size()] = 0;
    bool done = false;
    if (multiLine) {
        const float h = height > 0.0f ? height : ImGui::GetTextLineHeight() * 3.5f;
        ImGui::InputTextMultiline("##value", buffer.data(), buffer.size(), ImVec2(frame.valueWidth, h));
        // Ctrl+Enter is done (Enter is a new line)
        done = ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    } else {
        ImGui::SetNextItemWidth(frame.valueWidth);
        done = ImGui::InputText("##value", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    }
    const std::string edited = buffer.data();
    if (ImGui::IsItemActive()) {
        state.editing = true;
        state.buffer = edited;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.editing = false;
            state.buffer = value;
            ImGui::ClearActiveID();
        }
    }
    if ((done || ImGui::IsItemDeactivatedAfterEdit()) && state.editing) {
        state.editing = false;
        if (edited != value) {
            result.phase = EditPhase::Applied;
            result.kind = DocRowResult::Kind::Set;
            result.text = edited;
        }
        if (done) ImGui::ClearActiveID();
    } else if (!ImGui::IsItemActive()) {
        state.editing = false;
    }
    if (options.readOnly) ImGui::EndDisabled();
    const bool resetClicked = endFrame(options);
    applyFrame(result, frame, resetClicked, value, options);
    if (result.pasted) {
        result.phase = EditPhase::Applied;
        result.kind = DocRowResult::Kind::Set;
    }
    ImGui::PopID();
    return result;
}

DocRowResult jasperengine::editor::ui::docRowCustom(const char* id, const DocRowOptions& options, const std::function<DocRowResult(float width)>& drawValue) {
    ImGui::PushID(id);
    DocRowFrame frame = beginFrame(id, options, false);
    if (options.readOnly) ImGui::BeginDisabled();
    DocRowResult result = drawValue(frame.valueWidth);
    if (options.readOnly) ImGui::EndDisabled();
    const bool resetClicked = endFrame(options);
    applyFrame(result, frame, resetClicked, std::string(), options);
    ImGui::PopID();
    return result;
}

bool jasperengine::editor::ui::docRowLink(const char* id, const DocRowOptions& options, const char* buttonText) {
    ImGui::PushID(id);
    DocRowFrame frame = beginFrame(id, options, false);
    const bool clicked = ImGui::Button(buttonText, ImVec2(frame.valueWidth, 0.0f));
    endFrame(DocRowOptions{.canReset = false});
    ImGui::PopID();
    return clicked;
}

void jasperengine::editor::ui::docRowSourceLegend(const char* sourceName) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Text("\xE2\x97\x8F set here   \xE2\x97\x8B from %s", sourceName);
    ImGui::PopStyleColor();
}
