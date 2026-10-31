// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "CanvasScalerFields.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

namespace {

    const char* const modeLabels[] = {"Follow Game Canvas", "Constant Pixel Size", "Scale With Screen Size", "Constant Physical Size"};
    const char* const matchLabels[] = {"Match Width Or Height", "Expand", "Shrink"};
    const char* const unitLabels[] = {"Centimeters", "Millimeters", "Inches", "Points", "Picas"};

    void label(const char* text, const char* help) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        if (help && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(help);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
    }

    template<typename T, int N>
    bool combo(const char* id, const char* const (&labels)[N], T& value) {
        int index = std::clamp((int)value, 0, N - 1);
        bool changed = false;
        if (ImGui::BeginCombo(id, labels[index])) {
            for (int i = 0; i < N; i++) {
                if (ImGui::Selectable(labels[i], i == index)) {
                    value = (T)i;
                    changed = i != index;
                }
                if (i == index) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

}

const char* editor::canvasScalerModeLabel(UIScaleMode mode) {
    const int index = std::clamp((int)mode, 0, 3);
    return modeLabels[index];
}

bool editor::drawCanvasScalerFields(const char* id, CanvasScalerSettings& settings, bool* editFinished) {
    bool changed = false;
    bool finished = false;
    auto ended = [&]() { finished |= ImGui::IsItemDeactivatedAfterEdit(); };
    ImGui::PushID(id);

    label("UI Scale Mode",
          "Follow Game Canvas: the UI lays out in the game canvas (Scaling Mode above), as before.\n"
          "Constant Pixel Size: one UI unit is Scale Factor screen pixels.\n"
          "Scale With Screen Size: the UI is designed at the Reference Resolution and scaled to the screen.\n"
          "Constant Physical Size: UI units are a physical length (points by default) at the screen's dpi.");
    if (combo("##mode", modeLabels, settings.mode)) changed = finished = true;

    switch (settings.mode) {
        case UIScaleMode::FollowGameCanvas:
            break;
        case UIScaleMode::ConstantPixelSize: {
            label("Scale Factor", "Screen pixels per UI unit.");
            float factor = settings.scaleFactor;
            if (ImGui::DragFloat("##scaleFactor", &factor, 0.01f, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                settings.scaleFactor = std::max(factor, 0.01f);
                changed = true;
            }
            ended();
            break;
        }
        case UIScaleMode::ScaleWithScreenSize: {
            label("Reference Resolution", "The resolution the UI is designed for, in UI units.");
            float reference[2] = {settings.referenceResolution.x, settings.referenceResolution.y};
            if (ImGui::DragFloat2("##reference", reference, 1.0f, 1.0f, 16384.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
                settings.referenceResolution = Vector2(std::max(reference[0], 1.0f), std::max(reference[1], 1.0f));
                changed = true;
            }
            ended();
            label("Screen Match Mode",
                  "Match Width Or Height: blends between scaling by the width and by the height.\n"
                  "Expand: the UI is never smaller than the reference (a wider or taller screen adds space).\n"
                  "Shrink: the UI is never larger than the reference (part of it may be cut).");
            if (combo("##matchMode", matchLabels, settings.screenMatchMode)) changed = finished = true;
            if (settings.screenMatchMode == ScreenMatchMode::MatchWidthOrHeight) {
                label("Match", "0 scales by the width, 1 by the height, between them a log-space blend.");
                ImGui::TextDisabled("Width");
                ImGui::SameLine();
                const float heightLabel = ImGui::CalcTextSize("Height").x + ImGui::GetStyle().ItemSpacing.x;
                ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - heightLabel, 40.0f));
                float match = settings.matchWidthOrHeight;
                if (ImGui::SliderFloat("##match", &match, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                    settings.matchWidthOrHeight = std::clamp(match, 0.0f, 1.0f);
                    changed = true;
                }
                ended();
                ImGui::SameLine();
                ImGui::TextDisabled("Height");
            }
            break;
        }
        case UIScaleMode::ConstantPhysicalSize: {
            label("Physical Unit", "The length one UI unit is on screen.");
            if (combo("##unit", unitLabels, settings.physicalUnit)) changed = finished = true;
            label("Fallback Screen DPI", "Used when the platform does not report the screen's dpi (Screen.dpi = 0).");
            float dpi = settings.fallbackScreenDPI;
            if (ImGui::DragFloat("##fallbackDpi", &dpi, 1.0f, 1.0f, 1000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
                settings.fallbackScreenDPI = std::max(dpi, 1.0f);
                changed = true;
            }
            ended();
            break;
        }
    }

    ImGui::PopID();
    if (editFinished)
        *editFinished = finished;
    return changed;
}

std::string editor::canvasScalerPreview(const CanvasScalerSettings& settings, float viewWidth, float viewHeight,
                                        float canvasWidth, float canvasHeight, float dpi) {
    const CanvasScalerResult result = CanvasScaler::compute(settings, viewWidth, viewHeight, canvasWidth, canvasHeight, dpi);
    char text[160];
    std::snprintf(text, sizeof(text), "At %.0fx%.0f: UI %gx%g at scale %.2f", viewWidth, viewHeight,
                  std::round(result.uiWidth * 10.0f) / 10.0f, std::round(result.uiHeight * 10.0f) / 10.0f, result.scaleFactor);
    return text;
}
