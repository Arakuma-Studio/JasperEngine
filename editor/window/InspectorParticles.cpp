// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Particle System and Force Field inspectors (docs/specs/S20-particles.md R27). The module
// list is Unity's: Main always open, every other module a foldout with its enable checkbox,
// "Add Module" for the hidden ones. The rows come from the module registry's field tables
// (ParticleModules), so a field added there shows here. Curves and gradients show a preview;
// a click opens their keys and driver below it (S27's shared curve editor replaces the key
// tables when it lands). Every edit is one undo step (a drag merges into one) through
// ParticleEffectEdit: an asset's edit writes the asset (all its users update), an inline
// effect's edit changes that GameObject only; a multi-selection takes the same change.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "Catalog.h"
#include "Project.h"
#include "Widgets.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ForceFieldComponent.h"
#include "component/ParticleSystemComponent.h"
#include "external/IconsFontAwesome6.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "util/EntityPayload.h"
#include "util/ParticleEffectEdit.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    std::vector<editor::EnumEntry> entriesForceFieldShape = {
        { (int)ForceFieldShapeType::Sphere, "Sphere" },
        { (int)ForceFieldShapeType::Box, "Box" },
        { (int)ForceFieldShapeType::Cylinder, "Cylinder" },
        { (int)ForceFieldShapeType::Infinite, "Infinite" }
    };

    std::vector<editor::EnumEntry> entriesForceFieldKind = {
        { (int)ForceFieldKind::Directional, "Directional" },
        { (int)ForceFieldKind::Radial, "Radial" },
        { (int)ForceFieldKind::Vortex, "Vortex" },
        { (int)ForceFieldKind::Turbulence, "Turbulence" },
        { (int)ForceFieldKind::Drag, "Drag" }
    };

    // What the rows of one frame did: a value changed (an undo step to push) and an edit ended
    // (the step stops merging: a drag released, a checkbox clicked)
    struct FieldUi {
        editor::Project* project = nullptr;
        std::set<std::string>* expanded = nullptr;
        bool changed = false;
        bool commit = false;

        void track(bool edited, bool discrete = false) {
            if (edited) {
                changed = true;
                if (discrete) commit = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        }
        bool isExpanded(const std::string& id) const { return expanded->count(id) > 0; }
        void toggle(const std::string& id) {
            if (!expanded->erase(id)) expanded->insert(id);
        }
    };

    float indentWidth() {
        return ImGui::GetStyle().IndentSpacing * 0.6f;
    }

    // The label cell of a new row, then the value cell (full width)
    void labelCell(const std::string& label, const char* tooltip, int indent) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (indent > 0) ImGui::Indent(indentWidth() * indent);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        if (indent > 0) ImGui::Unindent(indentWidth() * indent);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool ranged(const FieldDesc& f) {
        return f.minValue > -1.0e37f || f.maxValue < 1.0e37f;
    }

    float dragSpeed(const FieldDesc& f) {
        if (ranged(f) && f.maxValue - f.minValue <= 10.0f) return 0.005f;
        return 0.05f;
    }

    const char* floatFormat(const FieldDesc& f) {
        return (f.flags & FieldFlag_Degrees) ? "%.2f\xc2\xb0" : "%.3f";
    }

    bool dragFloat(const char* id, float* value, const FieldDesc& f) {
        const bool clamp = ranged(f);
        const bool edited = ImGui::DragFloat(id, value, dragSpeed(f), clamp ? f.minValue : 0.0f, clamp ? f.maxValue : 0.0f, floatFormat(f),
                                             clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
        if (edited && clamp) *value = std::clamp(*value, f.minValue, f.maxValue);
        return edited;
    }

    // A popup of options from a small button; true when one was picked
    bool optionButton(const char* id, const std::vector<EnumOption>& options, uint8_t& value) {
        bool picked = false;
        const std::string popup = std::string("##options_") + id;
        if (ImGui::Button((std::string(ICON_FA_CARET_DOWN "##") + id).c_str())) ImGui::OpenPopup(popup.c_str());
        if (ImGui::IsItemHovered()) {
            for (const EnumOption& o : options) {
                if (o.value == value) ImGui::SetTooltip("%s", o.label);
            }
        }
        if (ImGui::BeginPopup(popup.c_str())) {
            for (const EnumOption& o : options) {
                if (ImGui::MenuItem(o.label, nullptr, o.value == value)) {
                    picked = o.value != value;
                    value = o.value;
                }
            }
            ImGui::EndPopup();
        }
        return picked;
    }

    bool enumCombo(const char* id, const EnumOption* options, uint8_t count, uint8_t& value) {
        const char* preview = "";
        for (uint8_t i = 0; i < count; i++) {
            if (options[i].value == value) preview = options[i].label;
        }
        bool picked = false;
        if (ImGui::BeginCombo(id, preview)) {
            for (uint8_t i = 0; i < count; i++) {
                if (ImGui::Selectable(options[i].label, options[i].value == value)) {
                    picked = options[i].value != value;
                    value = options[i].value;
                }
            }
            ImGui::EndCombo();
        }
        return picked;
    }

    // ---------------------------------------------------------------- previews

    // A curve (two: the band between them) over [0, 1], scaled to its value range; true on a click
    bool curvePreview(const char* id, const Curve& curve, const Curve* curveMax, float multiplier, float width) {
        const float height = ImGui::GetFrameHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(width, 8.0f), height);
        ImGui::InvisibleButton(id, size);
        const bool clicked = ImGui::IsItemClicked();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, ImGui::GetStyle().FrameRounding);

        float lo = 0.0f, hi = 0.0f;
        float a = 0.0f, b = 0.0f;
        curve.valueRange(a, b);
        lo = std::min(lo, a * multiplier);
        hi = std::max(hi, b * multiplier);
        lo = std::min(lo, b * multiplier);
        hi = std::max(hi, a * multiplier);
        if (curveMax) {
            curveMax->valueRange(a, b);
            lo = std::min({lo, a * multiplier, b * multiplier});
            hi = std::max({hi, a * multiplier, b * multiplier});
        }
        if (hi - lo < 1e-6f) hi = lo + 1.0f;
        const float pad = 2.0f;
        auto toScreen = [&](float t, float v) {
            const float y = (v * multiplier - lo) / (hi - lo);
            return ImVec2(pos.x + pad + t * (size.x - 2 * pad), pos.y + size.y - pad - y * (size.y - 2 * pad));
        };
        const int samples = 48;
        const ImU32 line = ImGui::GetColorU32(ImGuiCol_PlotLines);
        if (curveMax && !curveMax->empty()) {
            const ImU32 band = ImGui::GetColorU32(ImGuiCol_PlotLines, 0.25f);
            for (int i = 0; i < samples; i++) {
                const float t0 = (float)i / samples, t1 = (float)(i + 1) / samples;
                dl->AddQuadFilled(toScreen(t0, curve.evaluate(t0)), toScreen(t1, curve.evaluate(t1)),
                                  toScreen(t1, curveMax->evaluate(t1)), toScreen(t0, curveMax->evaluate(t0)), band);
            }
        }
        for (const Curve* c : {&curve, curveMax}) {
            if (!c || c->empty()) continue;
            ImVec2 previous = toScreen(0.0f, c->evaluate(0.0f));
            for (int i = 1; i <= samples; i++) {
                const float t = (float)i / samples;
                const ImVec2 point = toScreen(t, c->evaluate(t));
                dl->AddLine(previous, point, line, 1.5f);
                previous = point;
            }
        }
        return clicked;
    }

    ImU32 colorU32(const Vector3& srgb, float alpha) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(srgb.x, srgb.y, srgb.z, alpha));
    }

    // The gradient's colours (top) and colours over their alpha (bottom); true on a click
    bool gradientPreview(const char* id, const Gradient& gradient, float width, float height) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(width, 8.0f), height);
        ImGui::InvisibleButton(id, size);
        const bool clicked = ImGui::IsItemClicked();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(40, 40, 40, 255));
        const int samples = 32;
        const float mid = pos.y + size.y * 0.6f;
        for (int i = 0; i < samples; i++) {
            const float t0 = (float)i / samples, t1 = (float)(i + 1) / samples;
            const float x0 = pos.x + t0 * size.x, x1 = pos.x + t1 * size.x;
            const Vector3 c0 = gradient.evaluateEncoded(t0), c1 = gradient.evaluateEncoded(t1);
            dl->AddRectFilledMultiColor(ImVec2(x0, pos.y), ImVec2(x1, mid), colorU32(c0, 1.0f), colorU32(c1, 1.0f), colorU32(c1, 1.0f), colorU32(c0, 1.0f));
            const float a0 = gradient.evaluateAlpha(t0), a1 = gradient.evaluateAlpha(t1);
            dl->AddRectFilledMultiColor(ImVec2(x0, mid), ImVec2(x1, pos.y + size.y), colorU32(c0, a0), colorU32(c1, a1), colorU32(c1, a1), colorU32(c0, a0));
        }
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));
        return clicked;
    }

    // ---------------------------------------------------------------- key tables

    void trashButton(const char* id, bool& clicked) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        clicked = ImGui::Button((std::string(ICON_FA_TRASH_CAN "##") + id).c_str());
        ImGui::PopStyleColor(2);
    }

    float trashWidth() {
        return ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    }

    // Keys of a curve over [0, 1]: time, value and tangents, a key's time kept between its
    // neighbours' (the order never changes under a drag), presets for the usual shapes
    void drawCurveKeys(FieldUi& ui, Curve& curve, const std::string& id, const std::string& label, int indent) {
        ImGui::PushID(id.c_str());
        labelCell(label, "time, value, in tangent, out tangent of each key (time 0 to 1)", indent);
        ImGui::Text("%zu", curve.keys.size());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Key")) {
            CurveKey key;
            key.time = curve.keys.empty() ? 0.0f : std::min(1.0f, curve.keys.back().time + 0.25f);
            key.value = curve.keys.empty() ? 1.0f : curve.evaluate(key.time);
            curve.keys.push_back(key);
            curve.sortKeys();
            ui.track(true, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Presets " ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##curve_presets");
        if (ImGui::BeginPopup("##curve_presets")) {
            struct Preset { const char* name; Curve curve; };
            const Preset presets[] = {
                {"Constant", Curve::constant(1.0f)},
                {"Linear 0 to 1", Curve::linear(0.0f, 1.0f)},
                {"Linear 1 to 0", Curve::linear(1.0f, 0.0f)},
                {"Ease In Out 0 to 1", Curve::easeInOut(0.0f, 0.0f, 1.0f, 1.0f)},
                {"Ease In Out 1 to 0", Curve::easeInOut(0.0f, 1.0f, 1.0f, 0.0f)},
            };
            for (const Preset& preset : presets) {
                if (ImGui::MenuItem(preset.name)) {
                    curve = preset.curve;
                    ui.track(true, true);
                }
            }
            ImGui::EndPopup();
        }

        for (size_t i = 0; i < curve.keys.size(); i++) {
            ImGui::PushID((int)i);
            labelCell("Key " + std::to_string(i), nullptr, indent + 1);
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - trashWidth()));
            CurveKey& key = curve.keys[i];
            float values[4] = {key.time, key.value, key.inTangent, key.outTangent};
            const bool edited = ImGui::DragFloat4("##key", values, 0.005f, 0.0f, 0.0f, "%.3f");
            ui.track(edited);
            if (edited) {
                const float lo = i > 0 ? curve.keys[i - 1].time : 0.0f;
                const float hi = i + 1 < curve.keys.size() ? curve.keys[i + 1].time : 1.0f;
                key.time = std::clamp(values[0], lo, hi);
                key.value = values[1];
                key.inTangent = values[2];
                key.outTangent = values[3];
            }
            ImGui::SameLine();
            bool removed = false;
            trashButton("key_delete", removed);
            ImGui::PopID();
            if (removed) {
                curve.keys.erase(curve.keys.begin() + (std::ptrdiff_t)i);
                ui.track(true, true);
                break;
            }
        }
        ImGui::PopID();
    }

    void drawGradientKeys(FieldUi& ui, Gradient& gradient, const std::string& id, const std::string& label, int indent) {
        ImGui::PushID(id.c_str());
        labelCell(label, "colour keys (sRGB) and alpha keys over time 0 to 1, at most 8 of each", indent);
        uint8_t mode = (uint8_t)gradient.mode;
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        const std::vector<EnumOption>& modes = ParticleModules::gradientBlendModes();
        if (enumCombo("##blend", modes.data(), (uint8_t)modes.size(), mode)) {
            gradient.mode = (GradientMode)mode;
            ui.track(true, true);
        }
        ImGui::SameLine();
        bool linear = gradient.linear;
        if (ImGui::Checkbox("Linear Blend", &linear)) {
            gradient.linear = linear;
            ui.track(true, true);
        }

        labelCell("Colors", nullptr, indent + 1);
        ImGui::Text("%zu", gradient.colorKeys.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(gradient.colorKeys.size() >= Gradient::kMaxKeys);
        if (ImGui::Button(ICON_FA_PLUS "##add_color")) {
            GradientColorKey key;
            key.time = gradient.colorKeys.empty() ? 0.0f : 1.0f;
            key.color = gradient.colorKeys.empty() ? Vector3(1, 1, 1) : gradient.evaluateEncoded(key.time);
            gradient.colorKeys.push_back(key);
            gradient.sortKeys();
            ui.track(true, true);
        }
        ImGui::EndDisabled();
        for (size_t i = 0; i < gradient.colorKeys.size(); i++) {
            ImGui::PushID((int)i);
            labelCell("Color " + std::to_string(i), nullptr, indent + 2);
            GradientColorKey& key = gradient.colorKeys[i];
            float color[3] = {key.color.x, key.color.y, key.color.z};
            if (ImGui::ColorEdit3("##color", color, ImGuiColorEditFlags_NoInputs)) {
                key.color = Vector3(color[0], color[1], color[2]);
                ui.track(true);
            } else {
                ui.track(false);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - trashWidth()));
            const float lo = i > 0 ? gradient.colorKeys[i - 1].time : 0.0f;
            const float hi = i + 1 < gradient.colorKeys.size() ? gradient.colorKeys[i + 1].time : 1.0f;
            const bool timeEdited = ImGui::SliderFloat("##time", &key.time, lo, hi, "%.3f");
            ui.track(timeEdited);
            ImGui::SameLine();
            bool removed = false;
            trashButton("color_delete", removed);
            ImGui::PopID();
            if (removed) {
                gradient.colorKeys.erase(gradient.colorKeys.begin() + (std::ptrdiff_t)i);
                ui.track(true, true);
                break;
            }
        }

        labelCell("Alphas", nullptr, indent + 1);
        ImGui::Text("%zu", gradient.alphaKeys.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(gradient.alphaKeys.size() >= Gradient::kMaxKeys);
        if (ImGui::Button(ICON_FA_PLUS "##add_alpha")) {
            GradientAlphaKey key;
            key.time = gradient.alphaKeys.empty() ? 0.0f : 1.0f;
            key.alpha = gradient.alphaKeys.empty() ? 1.0f : gradient.evaluateAlpha(key.time);
            gradient.alphaKeys.push_back(key);
            gradient.sortKeys();
            ui.track(true, true);
        }
        ImGui::EndDisabled();
        for (size_t i = 0; i < gradient.alphaKeys.size(); i++) {
            ImGui::PushID((int)i);
            labelCell("Alpha " + std::to_string(i), nullptr, indent + 2);
            GradientAlphaKey& key = gradient.alphaKeys[i];
            const float w = std::max(1.0f, (ImGui::GetContentRegionAvail().x - trashWidth()) * 0.5f - ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(w);
            const bool alphaEdited = ImGui::SliderFloat("##alpha", &key.alpha, 0.0f, 1.0f, "a %.2f");
            ui.track(alphaEdited);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(w);
            const float lo = i > 0 ? gradient.alphaKeys[i - 1].time : 0.0f;
            const float hi = i + 1 < gradient.alphaKeys.size() ? gradient.alphaKeys[i + 1].time : 1.0f;
            const bool timeEdited = ImGui::SliderFloat("##time", &key.time, lo, hi, "t %.3f");
            ui.track(timeEdited);
            ImGui::SameLine();
            bool removed = false;
            trashButton("alpha_delete", removed);
            ImGui::PopID();
            if (removed) {
                gradient.alphaKeys.erase(gradient.alphaKeys.begin() + (std::ptrdiff_t)i);
                ui.track(true, true);
                break;
            }
        }
        ImGui::PopID();
    }

    // What a curve or gradient is evaluated at, and how values outside its range wrap
    template<class V>
    void drawDriver(FieldUi& ui, V& value, int indent) {
        labelCell("Driven By", "what the curve's time axis follows", indent);
        uint8_t driver = (uint8_t)value.driver;
        const std::vector<EnumOption>& drivers = ParticleModules::curveDrivers();
        if (enumCombo("##driver", drivers.data(), (uint8_t)drivers.size(), driver)) {
            value.driver = (CurveDriver)driver;
            ui.track(true, true);
        }
        if (value.driver == CurveDriver::NormalizedAge || value.driver == CurveDriver::Random) return;
        labelCell("Driver Range", "the driver values at the curve's 0 and 1", indent);
        float range[2] = {value.driverMin, value.driverMax};
        const bool edited = ImGui::DragFloat2("##range", range, 0.05f);
        ui.track(edited);
        if (edited) {
            value.driverMin = range[0];
            value.driverMax = range[1];
        }
        labelCell("Wrap", nullptr, indent);
        uint8_t wrap = (uint8_t)value.wrap;
        const std::vector<EnumOption>& wraps = ParticleModules::curveWraps();
        if (enumCombo("##wrap", wraps.data(), (uint8_t)wraps.size(), wrap)) {
            value.wrap = (CurveWrap)wrap;
            ui.track(true, true);
        }
    }

    // ---------------------------------------------------------------- values

    void drawMinMaxCurve(FieldUi& ui, const FieldDesc& f, MinMaxCurve& value, const std::string& id, int indent) {
        labelCell(f.label, f.tooltip, indent);
        uint8_t mode = (uint8_t)value.mode;
        const float button = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x - button);
        bool modeChanged = false;
        switch (value.mode) {
            case CurveMode::Constant: {
                ImGui::SetNextItemWidth(avail);
                const bool edited = dragFloat("##constant", &value.constant, f);
                ui.track(edited);
                if (edited) value.constantMax = value.constant;
                break;
            }
            case CurveMode::TwoConstants: {
                const float half = std::max(1.0f, avail * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f);
                ImGui::SetNextItemWidth(half);
                ui.track(dragFloat("##min", &value.constant, f));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(half);
                ui.track(dragFloat("##max", &value.constantMax, f));
                break;
            }
            case CurveMode::Curve:
            case CurveMode::TwoCurves: {
                const float multiplierWidth = Theme::dpi(56.0f);
                const float previewWidth = std::max(8.0f, avail - multiplierWidth - ImGui::GetStyle().ItemSpacing.x);
                if (curvePreview("##preview", value.curve, value.mode == CurveMode::TwoCurves ? &value.curveMax : nullptr, value.multiplier, previewWidth)) {
                    ui.toggle(id);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to edit the keys");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(multiplierWidth);
                const bool edited = ImGui::DragFloat("##multiplier", &value.multiplier, 0.01f, 0.0f, 0.0f, "x%.2f");
                ui.track(edited);
                break;
            }
        }
        ImGui::SameLine();
        if (optionButton("mode", ParticleModules::curveModes(), mode)) {
            const CurveMode next = (CurveMode)mode;
            // a curve starts from the constant it replaces
            if ((next == CurveMode::Curve || next == CurveMode::TwoCurves) && value.curve.empty()) {
                const float scale = value.constant != 0.0f ? value.constant : 1.0f;
                value.curve = Curve::constant(1.0f);
                value.multiplier = scale;
            }
            if (next == CurveMode::TwoCurves && value.curveMax.empty()) value.curveMax = value.curve;
            value.mode = next;
            modeChanged = true;
            ui.track(true, true);
            if (value.usesCurve()) ui.expanded->insert(id);
        }
        (void)modeChanged;

        if (value.usesCurve() && ui.isExpanded(id)) {
            drawCurveKeys(ui, value.curve, id + ".curve", value.mode == CurveMode::TwoCurves ? "Min Keys" : "Keys", indent + 1);
            if (value.mode == CurveMode::TwoCurves) drawCurveKeys(ui, value.curveMax, id + ".curveMax", "Max Keys", indent + 1);
            drawDriver(ui, value, indent + 1);
        }
    }

    bool colorEdit(const char* id, Vector4& color, bool compact) {
        float c[4] = {color.x, color.y, color.z, color.w};
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
        if (compact) flags |= ImGuiColorEditFlags_NoInputs;
        const bool edited = ImGui::ColorEdit4(id, c, flags);
        if (edited) color = Vector4(c[0], c[1], c[2], c[3]);
        return edited;
    }

    void drawMinMaxGradient(FieldUi& ui, const FieldDesc& f, MinMaxGradient& value, const std::string& id, int indent) {
        labelCell(f.label, f.tooltip, indent);
        uint8_t mode = (uint8_t)value.mode;
        const float button = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x - button);
        switch (value.mode) {
            case GradientValueMode::Color: {
                ImGui::SetNextItemWidth(avail);
                const bool edited = colorEdit("##color", value.color, false);
                ui.track(edited);
                if (edited) value.colorMax = value.color;
                break;
            }
            case GradientValueMode::TwoColors: {
                ui.track(colorEdit("##min", value.color, true));
                ImGui::SameLine();
                ui.track(colorEdit("##max", value.colorMax, true));
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x - button), 1.0f));
                break;
            }
            case GradientValueMode::Gradient:
            case GradientValueMode::TwoGradients: {
                const bool two = value.mode == GradientValueMode::TwoGradients;
                const float h = ImGui::GetFrameHeight();
                ImGui::BeginGroup();
                bool clicked = gradientPreview("##preview", value.gradient, avail, two ? h * 0.5f : h);
                if (two) clicked = gradientPreview("##previewMax", value.gradientMax, avail, h * 0.5f) || clicked;
                ImGui::EndGroup();
                if (clicked) ui.toggle(id);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to edit the keys");
                break;
            }
        }
        ImGui::SameLine();
        if (optionButton("mode", ParticleModules::gradientModes(), mode)) {
            const GradientValueMode next = (GradientValueMode)mode;
            const Vector3 rgb(value.color.x, value.color.y, value.color.z);
            if ((next == GradientValueMode::Gradient || next == GradientValueMode::TwoGradients) && value.gradient.colorKeys.empty()) {
                value.gradient = Gradient::constant(rgb, value.color.w);
            }
            if (next == GradientValueMode::TwoGradients && value.gradientMax.colorKeys.empty()) value.gradientMax = value.gradient;
            value.mode = next;
            ui.track(true, true);
            if (value.usesGradient()) ui.expanded->insert(id);
        }

        if (value.usesGradient() && ui.isExpanded(id)) {
            drawGradientKeys(ui, value.gradient, id + ".gradient", value.mode == GradientValueMode::TwoGradients ? "Min Gradient" : "Gradient", indent + 1);
            if (value.mode == GradientValueMode::TwoGradients) drawGradientKeys(ui, value.gradientMax, id + ".gradientMax", "Max Gradient", indent + 1);
            drawDriver(ui, value, indent + 1);
        }
    }

    // Unity's layer mask dropdown: Nothing, Everything, then a checkbox per layer
    void drawMask(FieldUi& ui, uint32_t& mask) {
        const std::vector<std::string>& names = ui.project->getLayerNames();
        std::string summary;
        if (mask == 0) summary = "Nothing";
        else if (mask == 0xffffffffu) summary = "Everything";
        else {
            int count = 0;
            for (int i = 0; i < 32; i++) {
                if (mask & (1u << i)) {
                    count++;
                    if (summary.empty()) summary = (i < (int)names.size() && !names[i].empty()) ? names[i] : "Layer " + std::to_string(i);
                }
            }
            if (count > 1) summary = "Mixed...";
        }
        if (ImGui::Button((summary + " " ICON_FA_CARET_DOWN "##mask").c_str(), ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("##mask_popup");
        if (ImGui::BeginPopup("##mask_popup")) {
            if (ImGui::MenuItem("Nothing", nullptr, mask == 0)) {
                mask = 0;
                ui.track(true, true);
            }
            if (ImGui::MenuItem("Everything", nullptr, mask == 0xffffffffu)) {
                mask = 0xffffffffu;
                ui.track(true, true);
            }
            ImGui::Separator();
            for (int i = 0; i < 32; i++) {
                const std::string name = (i < (int)names.size() && !names[i].empty()) ? names[i] : "Layer " + std::to_string(i);
                bool on = (mask & (1u << i)) != 0;
                if (ImGui::MenuItem((name + "##layer" + std::to_string(i)).c_str(), nullptr, &on)) {
                    mask = on ? (mask | (1u << i)) : (mask & ~(1u << i));
                    ui.track(true, true);
                }
            }
            ImGui::EndPopup();
        }
    }

    // A path (assets-relative), typed or dropped from the Project window; the GUID follows it
    void drawAssetPath(FieldUi& ui, std::string& path, std::string* guid) {
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
        bool edited = ImGui::InputText("##path", buffer, sizeof(buffer));
        if (edited) path = buffer;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                std::vector<std::string> dropped = editor::Util::getStringsFromPayload(payload);
                if (!dropped.empty() && ui.project->isInsideAssetsPath(dropped[0])) {
                    path = ui.project->normalizeToAssetsRelative(dropped[0]).generic_string();
                    edited = true;
                    ui.commit = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ui.track(edited);
        if (edited && guid) {
            *guid = path.empty() ? std::string() : ui.project->makeAssetRef(path, AssetPathBase::Assets).guid.str();
        }
        if (ImGui::IsItemHovered() && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
    }

    // which fields of a struct a pass draws
    enum class Pass { Basic, Advanced, All };

    void drawStruct(FieldUi& ui, const StructDesc& desc, void* owner, const std::string& path, int indent, Pass pass);

    void drawField(FieldUi& ui, const FieldDesc& f, void* p, const std::string& id, int indent, std::string* guid) {
        ImGui::PushID(id.c_str());
        switch (f.type) {
            case FieldType::Bool: {
                labelCell(f.label, f.tooltip, indent);
                bool v = *static_cast<bool*>(p);
                if (ImGui::Checkbox("##v", &v)) {
                    *static_cast<bool*>(p) = v;
                    ui.track(true, true);
                }
                break;
            }
            case FieldType::Int: {
                labelCell(f.label, f.tooltip, indent);
                int& v = *static_cast<int*>(p);
                const bool clamp = ranged(f);
                const bool edited = ImGui::DragInt("##v", &v, 0.2f, clamp ? (int)f.minValue : 0, clamp ? (int)f.maxValue : 0, "%d",
                                                   clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
                ui.track(edited);
                break;
            }
            case FieldType::UInt: {
                labelCell(f.label, f.tooltip, indent);
                uint32_t& v = *static_cast<uint32_t*>(p);
                const uint32_t lo = f.minValue > 0.0f ? (uint32_t)f.minValue : 0u;
                const uint32_t hi = f.maxValue < 4.0e9f ? (uint32_t)f.maxValue : 0xffffffffu;
                const bool edited = ImGui::DragScalar("##v", ImGuiDataType_U32, &v, 0.5f, &lo, &hi, "%u", ImGuiSliderFlags_AlwaysClamp);
                ui.track(edited);
                break;
            }
            case FieldType::Float: {
                labelCell(f.label, f.tooltip, indent);
                ui.track(dragFloat("##v", static_cast<float*>(p), f));
                break;
            }
            case FieldType::Enum: {
                labelCell(f.label, f.tooltip, indent);
                uint8_t& v = *static_cast<uint8_t*>(p);
                if (enumCombo("##v", f.options, f.optionCount, v)) ui.track(true, true);
                break;
            }
            case FieldType::Mask: {
                labelCell(f.label, f.tooltip, indent);
                drawMask(ui, *static_cast<uint32_t*>(p));
                break;
            }
            case FieldType::Vector2: {
                labelCell(f.label, f.tooltip, indent);
                ui.track(ImGui::DragFloat2("##v", &static_cast<Vector2*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Vector3: {
                labelCell(f.label, f.tooltip, indent);
                ui.track(ImGui::DragFloat3("##v", &static_cast<Vector3*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Vector4: {
                labelCell(f.label, f.tooltip, indent);
                ui.track(ImGui::DragFloat4("##v", &static_cast<Vector4*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Color: {
                labelCell(f.label, f.tooltip, indent);
                ui.track(colorEdit("##v", *static_cast<Vector4*>(p), false));
                break;
            }
            case FieldType::String: {
                labelCell(f.label, f.tooltip, indent);
                std::string& v = *static_cast<std::string*>(p);
                if (f.flags & FieldFlag_AssetPath) {
                    drawAssetPath(ui, v, guid);
                } else {
                    char buffer[512];
                    std::snprintf(buffer, sizeof(buffer), "%s", v.c_str());
                    const bool edited = ImGui::InputText("##v", buffer, sizeof(buffer));
                    if (edited) v = buffer;
                    ui.track(edited);
                }
                break;
            }
            case FieldType::MinMaxCurve:
                drawMinMaxCurve(ui, f, *static_cast<MinMaxCurve*>(p), id, indent);
                break;
            case FieldType::MinMaxGradient:
                drawMinMaxGradient(ui, f, *static_cast<MinMaxGradient*>(p), id, indent);
                break;
            case FieldType::Curve: {
                labelCell(f.label, f.tooltip, indent);
                Curve& curve = *static_cast<Curve*>(p);
                if (curvePreview("##preview", curve, nullptr, 1.0f, ImGui::GetContentRegionAvail().x)) ui.toggle(id);
                if (ui.isExpanded(id)) drawCurveKeys(ui, curve, id + ".keys", "Keys", indent + 1);
                break;
            }
            case FieldType::Gradient: {
                labelCell(f.label, f.tooltip, indent);
                Gradient& gradient = *static_cast<Gradient*>(p);
                if (gradientPreview("##preview", gradient, ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())) ui.toggle(id);
                if (ui.isExpanded(id)) drawGradientKeys(ui, gradient, id + ".keys", "Keys", indent + 1);
                break;
            }
            case FieldType::Struct: {
                labelCell(f.label, f.tooltip, indent);
                if (f.element) drawStruct(ui, *f.element, p, id, indent + 1, Pass::All);
                break;
            }
            case FieldType::Array: {
                labelCell(f.label, f.tooltip, indent);
                const size_t count = f.array.size(p);
                ImGui::Text("%zu", count);
                ImGui::SameLine();
                ImGui::BeginDisabled(f.maxCount > 0 && count >= f.maxCount);
                if (ImGui::Button(ICON_FA_PLUS " Add")) {
                    f.array.resize(p, count + 1);
                    ui.track(true, true);
                }
                ImGui::EndDisabled();
                for (size_t i = 0; i < f.array.size(p); i++) {
                    ImGui::PushID((int)i);
                    labelCell(std::string(f.label) + " " + std::to_string(i), nullptr, indent + 1);
                    ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x - trashWidth()), 1.0f));
                    ImGui::SameLine();
                    bool removed = false;
                    trashButton("element_delete", removed);
                    if (removed) {
                        f.array.erase(p, i);
                        ui.track(true, true);
                        ImGui::PopID();
                        break;
                    }
                    if (f.element) drawStruct(ui, *f.element, f.array.at(p, i), id + "[" + std::to_string(i) + "]", indent + 2, Pass::All);
                    ImGui::PopID();
                }
                break;
            }
            case FieldType::IntArray: {
                labelCell(f.label, f.tooltip, indent);
                std::vector<int>& v = *static_cast<std::vector<int>*>(p);
                ImGui::Text("%zu", v.size());
                ImGui::SameLine();
                ImGui::BeginDisabled(f.maxCount > 0 && v.size() >= f.maxCount);
                if (ImGui::Button(ICON_FA_PLUS " Add")) {
                    v.push_back(v.empty() ? 0 : v.back() + 1);
                    ui.track(true, true);
                }
                ImGui::EndDisabled();
                for (size_t i = 0; i < v.size(); i++) {
                    ImGui::PushID((int)i);
                    labelCell("#" + std::to_string(i), nullptr, indent + 1);
                    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - trashWidth()));
                    ui.track(ImGui::InputInt("##v", &v[i]));
                    ImGui::SameLine();
                    bool removed = false;
                    trashButton("int_delete", removed);
                    ImGui::PopID();
                    if (removed) {
                        v.erase(v.begin() + (std::ptrdiff_t)i);
                        ui.track(true, true);
                        break;
                    }
                }
                break;
            }
            case FieldType::StringArray: {
                labelCell(f.label, f.tooltip, indent);
                std::vector<std::string>& v = *static_cast<std::vector<std::string>*>(p);
                ImGui::Text("%zu", v.size());
                ImGui::SameLine();
                ImGui::BeginDisabled(f.maxCount > 0 && v.size() >= f.maxCount);
                if (ImGui::Button(ICON_FA_PLUS " Add")) {
                    v.emplace_back();
                    ui.track(true, true);
                }
                ImGui::EndDisabled();
                for (size_t i = 0; i < v.size(); i++) {
                    ImGui::PushID((int)i);
                    labelCell("#" + std::to_string(i), nullptr, indent + 1);
                    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - trashWidth()));
                    char buffer[256];
                    std::snprintf(buffer, sizeof(buffer), "%s", v[i].c_str());
                    const bool edited = ImGui::InputText("##v", buffer, sizeof(buffer));
                    if (edited) v[i] = buffer;
                    ui.track(edited);
                    ImGui::SameLine();
                    bool removed = false;
                    trashButton("string_delete", removed);
                    ImGui::PopID();
                    if (removed) {
                        v.erase(v.begin() + (std::ptrdiff_t)i);
                        ui.track(true, true);
                        break;
                    }
                }
                break;
            }
        }
        ImGui::PopID();
    }

    bool shown(const FieldDesc& f, const void* owner, Pass pass) {
        if (f.flags & (FieldFlag_Hidden | FieldFlag_Guid)) return false;
        if (pass == Pass::Basic && (f.flags & FieldFlag_Advanced)) return false;
        if (pass == Pass::Advanced && !(f.flags & FieldFlag_Advanced)) return false;
        return !f.visible || f.visible(owner);
    }

    void drawStruct(FieldUi& ui, const StructDesc& desc, void* owner, const std::string& path, int indent, Pass pass) {
        for (size_t i = 0; i < desc.fields.size(); i++) {
            const FieldDesc& f = desc.fields[i];
            if (!shown(f, owner, pass)) continue;
            std::string* guid = nullptr;
            if ((f.flags & FieldFlag_AssetPath) && i + 1 < desc.fields.size() && (desc.fields[i + 1].flags & FieldFlag_Guid)) {
                guid = static_cast<std::string*>(desc.fields[i + 1].field(owner));
            }
            drawField(ui, f, f.field(owner), path + "." + f.key, indent, guid);
        }
    }

    bool hasAdvanced(const StructDesc& desc, const void* owner) {
        for (const FieldDesc& f : desc.fields) {
            if (shown(f, owner, Pass::Advanced)) return true;
        }
        return false;
    }

    // A struct's basic rows, then its advanced ones under a foldout row
    void drawFieldsWithAdvanced(FieldUi& ui, const StructDesc& desc, void* owner, const std::string& path) {
        drawStruct(ui, desc, owner, path, 0, Pass::Basic);
        if (!hasAdvanced(desc, owner)) return;
        const std::string id = path + ".advanced";
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool open = ui.isExpanded(id);
        if (ImGui::Selectable(((open ? ICON_FA_CARET_DOWN " Advanced##" : ICON_FA_CARET_RIGHT " Advanced##") + id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
            ui.toggle(id);
        }
        ImGui::TableNextColumn();
        if (open) drawStruct(ui, desc, owner, path, 1, Pass::Advanced);
    }

    // An array of structs outside the module tables (the effect's sub-emitter list, its exposed
    // parameters): element rows with their fields, add and remove
    template<class T>
    void drawStructList(FieldUi& ui, const char* label, const StructDesc& element, std::vector<T>& list, size_t maxCount, const std::string& path) {
        labelCell(label, nullptr, 0);
        ImGui::Text("%zu", list.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(maxCount > 0 && list.size() >= maxCount);
        if (ImGui::Button((std::string(ICON_FA_PLUS " Add##") + path).c_str())) {
            list.emplace_back();
            ui.track(true, true);
        }
        ImGui::EndDisabled();
        for (size_t i = 0; i < list.size(); i++) {
            ImGui::PushID((path + std::to_string(i)).c_str());
            labelCell("#" + std::to_string(i), nullptr, 1);
            ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x - trashWidth()), 1.0f));
            ImGui::SameLine();
            bool removed = false;
            trashButton("list_delete", removed);
            if (removed) {
                list.erase(list.begin() + (std::ptrdiff_t)i);
                ui.track(true, true);
                ImGui::PopID();
                break;
            }
            drawStruct(ui, element, &list[i], path + "[" + std::to_string(i) + "]", 2, Pass::All);
            ImGui::PopID();
        }
    }

    void drawDiagnostics(const std::vector<Diagnostic>& diagnostics) {
        for (const Diagnostic& d : diagnostics) {
            const bool error = d.severity == Diagnostic::Severity::Error;
            ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
            ImGui::TextWrapped("%s %s: %s", error ? ICON_FA_CIRCLE_XMARK : ICON_FA_TRIANGLE_EXCLAMATION, d.path.c_str(), d.message.c_str());
            ImGui::PopStyleColor();
        }
    }

    // A two-column table of rows, like the Inspector's (labels at a fixed width)
    void beginRows(const std::string& id, float labelWidth) {
        labelWidth = std::max(labelWidth, std::min(ImGui::GetContentRegionAvail().x * 0.4f, Theme::dpi(180.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Theme::dpi(1.0f)));
        ImGui::BeginTable(("table_" + id).c_str(), 2, ImGuiTableFlags_Resizable);
        ImGui::PopStyleVar();
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value");
    }

    void endRows() {
        ImGui::EndTable();
    }

    // The whole effect: Main always open, a foldout per present module with its checkbox,
    // Add Module, the exposed parameters. Edits go into `edited`; `ui` says what happened.
    void drawEffectDocument(FieldUi& ui, ParticleEffectDocument& edited, const std::string& prefix) {
        const float labelSize = ImGui::CalcTextSize("Simulation Speed X").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const ModuleDesc& mainModule = ParticleModules::get(ModuleId::Main);

        // Main: the effect's own settings and the Main module, always open
        ImGui::SeparatorText(mainModule.label);
        beginRows(prefix + "_main", labelSize);
        drawStruct(ui, ParticleModules::documentFields(), &edited, "effect", 0, Pass::Basic);
        drawStruct(ui, mainModule.fields, mainModule.module(edited), mainModule.key, 0, Pass::Basic);
        if (hasAdvanced(ParticleModules::documentFields(), &edited) || hasAdvanced(mainModule.fields, mainModule.module(edited))) {
            const std::string id = "main.advanced";
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool open = ui.isExpanded(id);
            if (ImGui::Selectable(open ? ICON_FA_CARET_DOWN " Advanced##main" : ICON_FA_CARET_RIGHT " Advanced##main", false, ImGuiSelectableFlags_SpanAllColumns)) {
                ui.toggle(id);
            }
            ImGui::TableNextColumn();
            if (open) {
                drawStruct(ui, ParticleModules::documentFields(), &edited, "effect", 1, Pass::Advanced);
                drawStruct(ui, mainModule.fields, mainModule.module(edited), mainModule.key, 1, Pass::Advanced);
            }
        }
        endRows();

        for (const ModuleDesc& module : ParticleModules::all()) {
            if (module.id == ModuleId::Main || !module.isPresent(edited)) continue;
            ImGui::PushID(module.key);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (module.id == ModuleId::Emission || module.id == ModuleId::Shape || module.id == ModuleId::Renderer) {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const bool open = ImGui::TreeNodeEx("##module", flags, "%s", "");
            if (module.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", module.tooltip);
            bool remove = false;
            if (!module.alwaysPresent && ImGui::BeginPopupContextItem("##module_menu")) {
                if (ImGui::MenuItem("Remove Module")) remove = true;
                ImGui::EndPopup();
            }
            ImGui::SameLine(ImGui::GetTreeNodeToLabelSpacing());
            if (module.hasEnable) {
                bool enabled = module.isEnabled(edited);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    module.setEnabled(edited, enabled);
                    ui.track(true, true);
                }
                ImGui::PopStyleVar();
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(module.label);
            if (!module.alwaysPresent) {
                ImGui::SameLine(std::max(0.0f, rightEdge - trashWidth()));
                bool clicked = false;
                trashButton("module_remove", clicked);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove Module");
                remove = remove || clicked;
            }
            if (remove) {
                module.setPresent(edited, false);
                ui.track(true, true);
                ImGui::PopID();
                continue;
            }

            if (open) {
                ImGui::BeginDisabled(module.hasEnable && !module.isEnabled(edited));
                beginRows(prefix + "_module_" + module.key, labelSize);
                drawFieldsWithAdvanced(ui, module.fields, module.module(edited), module.key);
                if (module.id == ModuleId::SubEmitters) {
                    drawStructList(ui, "Sub Emitters", ParticleModules::subEmitterFields(), edited.subEmitters, (size_t)kMaxSubEmitters, "subEmitters");
                }
                endRows();
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }

        // "+" adds a hidden module (Unity's module list shows them unchecked)
        if (ImGui::Button(ICON_FA_PLUS " Add Module", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup(("##add_module_" + prefix).c_str());
        if (ImGui::BeginPopup(("##add_module_" + prefix).c_str())) {
            bool any = false;
            for (const ModuleDesc& module : ParticleModules::all()) {
                if (module.alwaysPresent || module.isPresent(edited)) continue;
                any = true;
                if (ImGui::MenuItem(module.label)) {
                    module.setPresent(edited, true);
                    module.setEnabled(edited, true);
                    ui.track(true, true);
                }
                if (module.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", module.tooltip);
            }
            if (!any) ImGui::TextDisabled("Every module is in the list");
            ImGui::EndPopup();
        }

        // the parameters scripts and the component set by name (setFloat("Intensity", 2))
        if (ImGui::TreeNodeEx(("Exposed Parameters##" + prefix).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            beginRows(prefix + "_exposed", labelSize);
            drawStructList(ui, "Parameters", ParticleModules::exposedFields(), edited.exposed, 0, "exposed");
            endRows();
            ImGui::TreePop();
        }
    }

}

// The history step of the last edit, kept merging until the edit ends (a drag released)
void editor::InspectorWindow::pushParticleCommand(CommandHistory* history, Command* command, bool commit) {
    if (command) {
        history->addCommand(command);
        const UndoEntry* top = history->top();
        particleEditCmd = (top && top->cmd.get() == command) ? command : nullptr;
    }
    if (commit) {
        const UndoEntry* top = history->top();
        if (particleEditCmd && top && top->cmd.get() == particleEditCmd) {
            top->cmd->setNoMerge();
        }
        particleEditCmd = nullptr;
    }
}

void editor::InspectorWindow::drawParticleSystemComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    ParticleSystemComponent* first = scene->findComponent<ParticleSystemComponent>(entities[0]);
    if (!first) return;

    RowSettings settingsSeed;
    settingsSeed.help = "0: a new random seed every time the system plays (Unity's Auto Random Seed)";
    RowSettings settingsSpeed;
    settingsSpeed.secondColSize = 6 * ImGui::GetFontSize();

    beginTable(cpType, getLabelSize("Playback Speed"), "particle_component");
    propertyRow(RowPropertyType::Asset, cpType, "effect", "Effect", sceneProject, entities);
    propertyRow(RowPropertyType::Bool, cpType, "playOnAwake", "Play On Awake", sceneProject, entities);
    propertyRow(RowPropertyType::UInt, cpType, "seed", "Seed", sceneProject, entities, settingsSeed);
    propertyRow(RowPropertyType::Float, cpType, "playbackSpeed", "Playback Speed", sceneProject, entities, settingsSpeed);
    endTable();

    bool anyAsset = false;
    bool anyInline = false;
    for (Entity entity : entities) {
        if (ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity)) {
            (particles->effect.empty() ? anyInline : anyAsset) = true;
        }
    }

    if (anyAsset) {
        ImGui::TextDisabled("%s", entities.size() == 1 ? "Edits change the effect asset and every Particle System using it."
                                                        : "Edits change the effect assets and every Particle System using them.");
        if (ImGui::Button(ICON_FA_LINK_SLASH " Make Inline")) {
            pushParticleCommand(CommandHandle::get(sceneProject->id), ParticleEffectEdit::buildMakeInline(project, sceneProject->id, entities), true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies the asset's effect into this GameObject and drops the reference");
    }
    if (anyInline && !anyAsset) {
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save as Asset")) {
            const std::string path = ParticleEffectEdit::freeAssetPath(project, "", scene->getEntityName(entities[0]));
            pushParticleCommand(CommandHandle::get(sceneProject->id), ParticleEffectEdit::buildSaveAsAsset(project, sceneProject->id, entities, path), true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes the effect to a .particles file in the assets folder and plays it from there");
    }

    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (!ParticleEffectEdit::read(*first, document, &diagnostics)) {
        drawDiagnostics(diagnostics);
        return;
    }
    drawDiagnostics(diagnostics);

    ParticleEffectDocument edited = document;
    FieldUi ui;
    ui.project = project;
    ui.expanded = &particleExpanded;
    drawEffectDocument(ui, edited, "particle");

    if (ui.changed) {
        const nlohmann::ordered_json patch = ParticleEffectEdit::diff(ParticleEffectCodec::toKeyedJson(document), ParticleEffectCodec::toKeyedJson(edited));
        const std::string beforeText = ParticleEffectCodec::write(document);
        Command* command = ParticleEffectEdit::build(project, sceneProject->id, entities, [&](ParticleEffectDocument& target) {
            // the effect shown takes the edit as it is; the other selected effects the same change
            if (ParticleEffectCodec::write(target) == beforeText) {
                target = edited;
                return true;
            }
            std::vector<Diagnostic> patchDiagnostics;
            return ParticleEffectCodec::applyMergePatch(target, patch, patchDiagnostics);
        });
        pushParticleCommand(CommandHandle::get(sceneProject->id), command, ui.commit);
    } else if (ui.commit) {
        pushParticleCommand(CommandHandle::get(sceneProject->id), nullptr, true);
    }

    drawParticleExposedValues(cpType, sceneProject, entities, edited);
    if (edited.collision.present && edited.collision.enabled && edited.collision.type == CollisionType::Planes) {
        drawParticleCollisionPlanes(cpType, sceneProject, entities);
    }
}

// The component's own values of the effect's exposed parameters (over the effect's)
void editor::InspectorWindow::drawParticleExposedValues(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                        const ParticleEffectDocument& document) {
    if (document.exposed.empty()) return;
    Scene* scene = sceneProject->scene;
    ParticleSystemComponent& first = scene->getComponent<ParticleSystemComponent>(entities[0]);

    ImGui::SeparatorText("Exposed Values");
    beginTable(cpType, getLabelSize("Simulation Speed X"), "particle_exposed_values");
    bool changed = false;
    bool commit = false;
    std::string changedName;
    bool overridden = false;
    Vector4 newValue;
    for (const ExposedParameter& parameter : document.exposed) {
        ImGui::PushID(parameter.name.c_str());
        labelCell(parameter.name, parameter.path.c_str(), 0);
        auto it = std::find_if(first.exposedValues.begin(), first.exposedValues.end(), [&](const ParticleExposedValue& v) { return v.name == parameter.name; });
        bool has = it != first.exposedValues.end();
        Vector4 value;
        if (has) value = it->value;
        else ParticleModules::getExposed(document, parameter.name, value);
        if (ImGui::Checkbox("##override", &has)) {
            changed = commit = true;
            changedName = parameter.name;
            overridden = has;
            newValue = value;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the effect's value on this Particle System");
        ImGui::SameLine();
        ImGui::BeginDisabled(!has);
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool edited = false;
        switch (parameter.type) {
            case ExposedType::Float: edited = ImGui::DragFloat("##value", &value.x, 0.01f); break;
            case ExposedType::Vector3: edited = ImGui::DragFloat3("##value", &value.x, 0.01f); break;
            case ExposedType::Color: edited = colorEdit("##value", value, false); break;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
        ImGui::EndDisabled();
        if (edited) {
            changed = true;
            changedName = parameter.name;
            overridden = true;
            newValue = value;
        }
        ImGui::PopID();
    }
    endTable();

    if (!changed) {
        if (commit) pushParticleCommand(CommandHandle::get(sceneProject->id), nullptr, true);
        return;
    }
    auto multi = std::make_unique<MultiPropertyCmd>();
    for (Entity entity : entities) {
        ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
        if (!particles) continue;
        std::vector<ParticleExposedValue> values = particles->exposedValues;
        auto it = std::find_if(values.begin(), values.end(), [&](const ParticleExposedValue& v) { return v.name == changedName; });
        if (!overridden) {
            if (it != values.end()) values.erase(it);
        } else if (it != values.end()) {
            it->value = newValue;
        } else {
            values.push_back({changedName, newValue});
        }
        multi->addPropertyCmd<std::vector<ParticleExposedValue>>(project, sceneProject->id, entity, cpType, "exposedValues", values);
    }
    pushParticleCommand(CommandHandle::get(sceneProject->id), multi->empty() ? nullptr : multi.release(), commit);
}

// The GameObjects whose transforms are more collision planes (their up axis the normal)
void editor::InspectorWindow::drawParticleCollisionPlanes(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    Scene* scene = sceneProject->scene;
    const std::vector<Entity> planes = scene->getComponent<ParticleSystemComponent>(entities[0]).collisionPlanes;

    ImGui::SeparatorText("Collision Plane Objects");
    std::vector<Entity> next = planes;
    bool changed = false;
    beginTable(cpType, getLabelSize("Simulation Speed X"), "particle_planes");
    for (size_t i = 0; i < planes.size(); i++) {
        ImGui::PushID((int)i);
        labelCell("Plane " + std::to_string(i), nullptr, 0);
        const std::string name = scene->isEntityCreated(planes[i]) ? scene->getEntityName(planes[i]) : std::string("(missing)");
        const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine(std::max(0.0f, rightEdge - trashWidth()));
        bool removed = false;
        trashButton("plane_delete", removed);
        if (removed) {
            next.erase(next.begin() + (std::ptrdiff_t)i);
            changed = true;
        }
        ImGui::PopID();
        if (removed) break;
    }
    labelCell("Add", "drop a GameObject from the Hierarchy", 0);
    ImGui::Button("Drop a GameObject here", ImVec2(-FLT_MIN, 0));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
            const EntityPayload* entityPayload = reinterpret_cast<const EntityPayload*>(payload->Data);
            const bool sameScene = entityPayload->entitySceneId == 0 || entityPayload->entitySceneId == sceneProject->id;
            if (sameScene && std::find(next.begin(), next.end(), entityPayload->entity) == next.end() &&
                std::find(entities.begin(), entities.end(), entityPayload->entity) == entities.end()) {
                next.push_back(entityPayload->entity);
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    endTable();

    if (!changed) return;
    auto multi = std::make_unique<MultiPropertyCmd>();
    for (Entity entity : entities) {
        multi->addPropertyCmd<std::vector<Entity>>(project, sceneProject->id, entity, cpType, "collisionPlanes", next);
    }
    pushParticleCommand(CommandHandle::get(sceneProject->id), multi.release(), true);
}

void editor::InspectorWindow::drawForceFieldComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;
    const ForceFieldComponent& field = scene->getComponent<ForceFieldComponent>(entities[0]);

    RowSettings settingsShape;
    settingsShape.enumEntries = &entriesForceFieldShape;
    RowSettings settingsKind;
    settingsKind.enumEntries = &entriesForceFieldKind;
    settingsKind.help = "Directional: wind along the direction. Radial: towards the centre (negative: away). Vortex: around the axis. "
                        "Turbulence: curl noise. Drag: slows particles down.";
    RowSettings settingsFloat;
    settingsFloat.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsSize;
    settingsSize.help = "Sphere: x is the radius. Box: the half extents. Cylinder: x the radius, y the half height (along Y).";
    RowSettings settingsLayers;
    settingsLayers.help = "Particle systems feel the field when their External Forces mask shares a layer with it";

    beginTable(cpType, getLabelSize("Pulse Frequency"));
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Shape", sceneProject, entities, settingsShape);
    if (field.shape != ForceFieldShapeType::Infinite) {
        propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settingsSize);
    }
    propertyRow(RowPropertyType::Enum, cpType, "kind", "Kind", sceneProject, entities, settingsKind);
    propertyRow(RowPropertyType::Float, cpType, "strength", "Strength", sceneProject, entities, settingsFloat);
    if (field.kind == ForceFieldKind::Directional || field.kind == ForceFieldKind::Vortex) {
        propertyRow(RowPropertyType::Vector3, cpType, "direction", field.kind == ForceFieldKind::Vortex ? "Axis" : "Direction", sceneProject, entities);
    }
    if (field.kind == ForceFieldKind::Directional) {
        propertyRow(RowPropertyType::Float, cpType, "turbulence", "Turbulence", sceneProject, entities, settingsFloat);
        propertyRow(RowPropertyType::Float, cpType, "pulseFrequency", "Pulse Frequency", sceneProject, entities, settingsFloat);
    }
    if (field.kind == ForceFieldKind::Turbulence || (field.kind == ForceFieldKind::Directional && field.turbulence > 0.0f)) {
        propertyRow(RowPropertyType::Float, cpType, "frequency", "Frequency", sceneProject, entities, settingsFloat);
    }
    propertyRow(RowPropertyType::LayerMask, cpType, "layers", "Layers", sceneProject, entities, settingsLayers);

    // strength over the distance from the centre (0) to the shape's edge (1)
    FieldUi ui;
    ui.project = project;
    ui.expanded = &particleExpanded;
    Curve falloff = field.falloff;
    ImGui::PushID("falloff");
    labelCell("Falloff", "strength over the distance from the centre (0) to the edge (1)", 0);
    if (curvePreview("##preview", falloff, nullptr, 1.0f, ImGui::GetContentRegionAvail().x)) ui.toggle("forcefield.falloff");
    if (ui.isExpanded("forcefield.falloff")) drawCurveKeys(ui, falloff, "forcefield.falloff.keys", "Keys", 1);
    ImGui::PopID();
    endTable();

    if (ui.changed) {
        auto multi = std::make_unique<MultiPropertyCmd>();
        for (Entity entity : entities) {
            multi->addPropertyCmd<Curve>(project, sceneProject->id, entity, cpType, "falloff", falloff);
        }
        pushParticleCommand(CommandHandle::get(sceneProject->id), multi.release(), ui.commit);
    } else if (ui.commit) {
        pushParticleCommand(CommandHandle::get(sceneProject->id), nullptr, true);
    }
}

// A .particles file picked alone in the Project window takes the Inspector; a later change of
// the GameObject selection gives it back
bool editor::InspectorWindow::updateInspectedParticleAsset(SceneProject* sceneProject, const std::vector<Entity>& entities) {
    std::string picked;
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        const std::vector<std::filesystem::path> files = projectWindow->getSelectedFiles();
        if (files.size() == 1 && Util::isParticleEffectFile(files[0].string()) && project->isInsideAssetsPath(files[0].string())) {
            picked = project->normalizeToAssetsRelative(files[0]).generic_string();
        }
    }
    if (picked != lastProjectParticleFile) {
        lastProjectParticleFile = picked;
        inspectedParticleAsset = picked;
        inspectedParticleScene = sceneProject->id;
        inspectedParticleSelection = entities;
    }
    if (!inspectedParticleAsset.empty() && (inspectedParticleScene != sceneProject->id || inspectedParticleSelection != entities)) {
        inspectedParticleAsset.clear();
    }
    if (inspectedParticleAsset.empty()) {
        particleAssetPreview.clear();
        return false;
    }
    return true;
}

// The asset's effect: a live preview, then its modules (edits write the asset, every user updates)
void editor::InspectorWindow::drawParticleAssetInspector() {
    const std::string& path = inspectedParticleAsset;
    ImGui::AlignTextToFramePadding();
    ImGui::Text(ICON_FA_FIRE " %s", std::filesystem::path(path).filename().string().c_str());
    ImGui::TextDisabled("%s", path.c_str());

    const float size = std::min(ImGui::GetContentRegionAvail().x, Theme::dpi(256.0f));
    particleAssetPreview.build(path, (unsigned int)std::max(64.0f, size));
    particleAssetPreview.update();
    Texture preview = particleAssetPreview.getTexture();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - size) * 0.5f));
    Widgets::image(Backend::getImGuiTexture(preview.getRender()), ImVec2(size, size), IM_COL32(30, 30, 33, 255));
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Restart")) particleAssetPreview.restart();
    ImGui::SameLine();
    ImGui::TextDisabled("%u particles", particleAssetPreview.getParticleCount());
    ImGui::Separator();

    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    const bool readable = ParticleEffectEdit::readAsset(path, document, &diagnostics);
    drawDiagnostics(diagnostics);
    if (!readable) return;

    ParticleEffectDocument edited = document;
    FieldUi ui;
    ui.project = project;
    ui.expanded = &particleExpanded;
    drawEffectDocument(ui, edited, "particle_asset");

    CommandHistory* history = project->getProjectCommandHistory();
    if (ui.changed) {
        Command* command = ParticleEffectEdit::buildAsset(project, path, [&](ParticleEffectDocument& target) {
            target = edited;
            return true;
        });
        pushParticleCommand(history, command, ui.commit);
    } else if (ui.commit) {
        pushParticleCommand(history, nullptr, true);
    }
}
