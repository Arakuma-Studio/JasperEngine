// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The MinMaxCurve / MinMaxGradient fields (MinMaxFields.h): the value widgets of the mode in use
// side by side, then a small arrow button whose popup picks the mode (Unity's labels).

#include "MinMaxFields.h"
#include "GradientField.h"

#include "imgui.h"
#include "external/IconsFontAwesome6.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const CURVE_MODES[4] = {"Constant", "Curve", "Random Between Two Curves", "Random Between Two Constants"};
    const char* const GRADIENT_MODES[5] = {"Color", "Gradient", "Random Between Two Colors", "Random Between Two Gradients", "Random Color"};

    float arrowWidth(){
        return ImGui::GetFrameHeight();
    }

    // the mode popup after the value widgets; true when a mode was picked
    template<typename Mode>
    bool modeArrow(const char* id, const char* const* names, int count, Mode& mode){
        ImGui::SameLine(0.0f, 2.0f);
        const std::string button = std::string(ICON_FA_CARET_DOWN "##mode_") + id;
        const std::string popup = std::string("minmax_mode_") + id;
        if (ImGui::Button(button.c_str(), ImVec2(arrowWidth(), 0.0f))){
            ImGui::OpenPopup(popup.c_str());
        }
        if (ImGui::IsItemHovered()){
            ImGui::SetTooltip("%s", names[(int)mode]);
        }
        bool picked = false;
        if (ImGui::BeginPopup(popup.c_str())){
            for (int i = 0; i < count; i++){
                if (ImGui::MenuItem(names[i], nullptr, (int)mode == i)){
                    mode = (Mode)i;
                    picked = true;
                }
            }
            ImGui::EndPopup();
        }
        return picked;
    }

    // a drag field of a value stored scaled: changed while dragged, finished when let go
    void dragValue(const char* id, float& stored, float scale, const char* format, float step, float width, MinMaxFieldResult& result){
        float shown = stored * scale;
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat(id, &shown, step, 0.0f, 0.0f, format)){
            stored = scale != 0.0f ? shown / scale : shown;
            result.changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) result.finished = true;
    }

    // an sRGB colour button with alpha for a linear colour
    void colorValue(const char* id, Vector4& linear, MinMaxFieldResult& result){
        const Vector3 srgb = Color::linearTosRGB(Vector3(linear.x, linear.y, linear.z));
        float edit[4] = {srgb.x, srgb.y, srgb.z, linear.w};
        if (ImGui::ColorEdit4(id, edit, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)){
            const Vector3 back = Color::sRGBToLinear(Vector3(edit[0], edit[1], edit[2]));
            linear = Vector4(back.x, back.y, back.z, edit[3]);
            result.changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) result.finished = true;
    }

}

MinMaxCurve editor::convertMinMaxCurve(const MinMaxCurve& value, ParticleSystemCurveMode mode){
    if (value.mode == mode) return value;
    MinMaxCurve out = value;
    out.mode = mode;
    // the value it showed: a constant, the curve's start, the two constants' range
    const float high = value.evaluate(0.0f, 1.0f);
    const float low = value.evaluate(0.0f, 0.0f);
    switch (mode){
        case ParticleSystemCurveMode::Constant:
            out.constant = high;
            out.constantMin = high;
            break;
        case ParticleSystemCurveMode::TwoConstants:
            out.constant = high;
            out.constantMin = value.mode == ParticleSystemCurveMode::Constant ? high : low;
            break;
        case ParticleSystemCurveMode::Curve:
            if (value.mode == ParticleSystemCurveMode::TwoCurves){
                out.constant = value.constant;
            }else{
                // a flat curve of 1 times the value: it draws the same
                out.constant = high;
                out.curve = AnimationCurve::constant(1.0f);
            }
            break;
        case ParticleSystemCurveMode::TwoCurves:
            if (value.mode == ParticleSystemCurveMode::Curve){
                out.curveMin = value.curve;
            }else{
                out.constant = high != 0.0f ? high : 1.0f;
                out.curve = AnimationCurve::constant(high != 0.0f ? 1.0f : 0.0f);
                out.curveMin = AnimationCurve::constant(high != 0.0f ? low / high : 0.0f);
            }
            break;
    }
    if (out.curve.keys.empty()) out.curve = AnimationCurve::constant(1.0f);
    if (out.curveMin.keys.empty()) out.curveMin = out.curve;
    return out;
}

MinMaxGradient editor::convertMinMaxGradient(const MinMaxGradient& value, ParticleSystemGradientMode mode){
    if (value.mode == mode) return value;
    MinMaxGradient out = value;
    out.mode = mode;
    const Vector4 first = value.evaluate(0.0f, 1.0f);
    switch (mode){
        case ParticleSystemGradientMode::Color:
            out.color = first;
            break;
        case ParticleSystemGradientMode::TwoColors:
            out.color = first;
            out.colorMin = value.mode == ParticleSystemGradientMode::Color ? first : value.evaluate(0.0f, 0.0f);
            break;
        case ParticleSystemGradientMode::Gradient:
        case ParticleSystemGradientMode::RandomColor:
            if (value.mode == ParticleSystemGradientMode::Color || value.mode == ParticleSystemGradientMode::TwoColors){
                out.gradient = value.mode == ParticleSystemGradientMode::Color ? Gradient::constant(value.color)
                                                                               : Gradient::twoColors(value.colorMin, value.color);
            }
            break;
        case ParticleSystemGradientMode::TwoGradients:
            if (value.mode == ParticleSystemGradientMode::Color){
                out.gradient = out.gradientMin = Gradient::constant(value.color);
            }else if (value.mode == ParticleSystemGradientMode::TwoColors){
                out.gradient = Gradient::constant(value.color);
                out.gradientMin = Gradient::constant(value.colorMin);
            }else{
                out.gradientMin = value.gradient;
            }
            break;
    }
    return out;
}

MinMaxFieldResult editor::minMaxCurveField(const char* id, MinMaxCurve& value, bool mixed, float width, float displayScale,
                                           const char* format, float step){
    MinMaxFieldResult result;
    ImGui::PushID(id);
    const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float fields = std::max(ImGui::GetFontSize() * 3.0f, total - arrowWidth() - 2.0f);
    if (mixed){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    switch (value.mode){
        case ParticleSystemCurveMode::Constant:
            dragValue("##constant", value.constant, displayScale, mixed ? "—" : format, step, fields, result);
            value.constantMin = value.constant;
            break;
        case ParticleSystemCurveMode::TwoConstants: {
            const float half = (fields - spacing) * 0.5f;
            dragValue("##min", value.constantMin, displayScale, mixed ? "—" : format, step, half, result);
            ImGui::SameLine();
            dragValue("##max", value.constant, displayScale, mixed ? "—" : format, step, half, result);
            break;
        }
        case ParticleSystemCurveMode::Curve:
        case ParticleSystemCurveMode::TwoCurves: {
            // the multiplier, then the curve(s)
            const float multiplierWidth = std::min(fields * 0.35f, ImGui::GetFontSize() * 4.0f);
            dragValue("##multiplier", value.constant, displayScale, mixed ? "—" : format, step, multiplierWidth, result);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplier");
            ImGui::SameLine();
            const float curveWidth = std::max(ImGui::GetFontSize() * 2.0f, fields - multiplierWidth - spacing);
            const CurveFieldResult curve = value.mode == ParticleSystemCurveMode::Curve
                ? curveField("##curve", value.curve, nullptr, mixed, curveWidth)
                : curveField("##curves", value.curve, &value.curveMin, mixed, curveWidth);
            result.changed = result.changed || curve.changed;
            result.finished = result.finished || curve.finished;
            break;
        }
    }
    if (mixed){
        ImGui::PopStyleColor();
    }
    ParticleSystemCurveMode mode = value.mode;
    if (modeArrow(id, CURVE_MODES, 4, mode)){
        value = convertMinMaxCurve(value, mode);
        result.changed = true;
        result.finished = true;
    }
    ImGui::PopID();
    return result;
}

MinMaxFieldResult editor::minMaxGradientField(const char* id, MinMaxGradient& value, bool mixed, float width){
    MinMaxFieldResult result;
    ImGui::PushID(id);
    const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float fields = std::max(ImGui::GetFontSize() * 3.0f, total - arrowWidth() - 2.0f);
    auto merge = [&](const GradientFieldResult& r){
        result.changed = result.changed || r.changed;
        result.finished = result.finished || r.finished;
    };
    switch (value.mode){
        case ParticleSystemGradientMode::Color:
            colorValue("##color", value.color, result);
            value.colorMin = value.color;
            break;
        case ParticleSystemGradientMode::TwoColors:
            colorValue("##colorMin", value.colorMin, result);
            ImGui::SameLine();
            colorValue("##colorMax", value.color, result);
            break;
        case ParticleSystemGradientMode::Gradient:
        case ParticleSystemGradientMode::RandomColor:
            merge(gradientField("##gradient", value.gradient, mixed, fields));
            break;
        case ParticleSystemGradientMode::TwoGradients: {
            const float half = (fields - spacing) * 0.5f;
            merge(gradientField("##gradientMin", value.gradientMin, mixed, half));
            ImGui::SameLine();
            merge(gradientField("##gradientMax", value.gradient, mixed, half));
            break;
        }
    }
    if ((value.mode == ParticleSystemGradientMode::Color || value.mode == ParticleSystemGradientMode::TwoColors) && mixed){
        ImGui::SameLine();
        ImGui::TextDisabled("—");
    }
    ParticleSystemGradientMode mode = value.mode;
    if (modeArrow(id, GRADIENT_MODES, 5, mode)){
        value = convertMinMaxGradient(value, mode);
        result.changed = true;
        result.finished = true;
    }
    ImGui::PopID();
    return result;
}
