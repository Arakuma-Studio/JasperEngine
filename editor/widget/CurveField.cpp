// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The field's thumbnail and open-on-click with the field's range follow OpenEngine's
// Apps/Editor/Source/UI/Controls/CurveField.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "CurveField.h"

#include "AppSettings.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "math/CurveJson.h"
#include "particles/ParticleModules.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::ui;

namespace {

    constexpr int kThumbnailSamples = 48;
    const ImU32 kDefaultCurveColor = IM_COL32(110, 220, 110, 255);

    struct Thumbnail {
        Curve curve;
        Curve second;
        bool hasSecond = false;
        curveops::CurveRange range;
        // the polyline in [0,1]x[0,1] of the field (y up)
        std::vector<ImVec2> points;
        std::vector<ImVec2> secondPoints;
        int lastFrame = 0;
    };

    std::unordered_map<ImGuiID, Thumbnail>& thumbnails() {
        static std::unordered_map<ImGuiID, Thumbnail> cache;
        return cache;
    }

    std::vector<ImVec2> sample(const Curve& curve, const curveops::CurveRange& range) {
        std::vector<ImVec2> points;
        points.reserve(kThumbnailSamples + 1);
        const float vs = range.v1 - range.v0;
        for (int i = 0; i <= kThumbnailSamples; i++) {
            const float u = (float)i / kThumbnailSamples;
            const float t = range.t0 + u * (range.t1 - range.t0);
            const float v = vs != 0.0f ? (curve.evaluate(t) - range.v0) / vs : 0.5f;
            points.push_back(ImVec2(u, std::clamp(v, -0.05f, 1.05f)));
        }
        return points;
    }

    curveops::CurveRange rangeFor(const CurveFieldOptions& options, const Curve& curve, const Curve* second) {
        curveops::CurveRange range = options.range;
        if (range.v1 != range.v0) return range;
        // no value range: the curves' own
        float lo, hi;
        curve.valueRange(lo, hi);
        if (second) {
            float a, b;
            second->valueRange(a, b);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        if (hi - lo < 1e-6f) hi = lo + 1.0f;
        range.v0 = lo;
        range.v1 = hi;
        return range;
    }

    // the curve a field's menu set (copy as JSON on the clipboard, paste, reset, presets);
    // true with `out` set when one was picked
    bool curveMenu(const char* popup, const std::vector<Curve>& value, std::vector<Curve>& out, const CurveFieldOptions& options) {
        if (!ImGui::BeginPopup(popup)) return false;
        bool picked = false;
        if (ImGui::MenuItem("Copy")) {
            curvejson::Json j = value.size() == 1 ? curvejson::curveToJson(value[0]) : curvejson::Json::array();
            if (value.size() > 1) {
                for (const Curve& c : value) j.push_back(curvejson::curveToJson(c));
            }
            ImGui::SetClipboardText(j.dump().c_str());
        }
        const char* clip = ImGui::GetClipboardText();
        const curvejson::Json parsed = clip ? curvejson::Json::parse(clip, nullptr, false) : curvejson::Json();
        std::vector<Curve> pasted;
        if (!parsed.is_discarded()) {
            std::vector<curvejson::Issue> issues;
            Curve c;
            if (curvejson::curveFromJson(parsed, c, issues)) {
                pasted.assign(value.size(), c);
            } else if (parsed.is_array() && parsed.size() == value.size()) {
                for (const curvejson::Json& element : parsed) {
                    issues.clear();
                    if (!curvejson::curveFromJson(element, c, issues)) {
                        pasted.clear();
                        break;
                    }
                    pasted.push_back(c);
                }
            }
        }
        if (ImGui::MenuItem("Paste", nullptr, false, !pasted.empty())) {
            out = pasted;
            picked = true;
        }
        if (ImGui::MenuItem("Reset")) {
            const Curve reset = options.resetValue ? *options.resetValue
                                                   : Curve::linear(options.range.t0, options.range.v0, options.range.t1, options.range.v1);
            out.assign(value.size(), reset);
            picked = true;
        }
        if (ImGui::BeginMenu("Presets")) {
            CurveWindow& window = CurveWindow::get();
            for (const CurvePresetLibrary& library : window.libraries()) {
                if (library.presets.empty()) continue;
                if (!ImGui::BeginMenu(library.name.c_str())) continue;
                for (const CurvePreset& preset : library.presets) {
                    if (ImGui::MenuItem(preset.name.c_str())) {
                        out.assign(value.size(), curveops::remap(preset.curve, options.range));
                        picked = true;
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
        return picked;
    }

}

void ui::drawCurveThumbnail(ImGuiID cacheId, ImDrawList* drawList, ImVec2 min, ImVec2 max, const Curve& curve, const Curve* second,
                            const curveops::CurveRange& range, ImU32 color) {
    Thumbnail& thumb = thumbnails()[cacheId];
    const bool stale = thumb.points.empty() || thumb.curve != curve || thumb.hasSecond != (second != nullptr) ||
                       (second && thumb.second != *second) || thumb.range.t0 != range.t0 || thumb.range.t1 != range.t1 ||
                       thumb.range.v0 != range.v0 || thumb.range.v1 != range.v1;
    if (stale) {
        thumb.curve = curve;
        thumb.hasSecond = second != nullptr;
        if (second) thumb.second = *second;
        thumb.range = range;
        thumb.points = sample(curve, range);
        thumb.secondPoints = second ? sample(*second, range) : std::vector<ImVec2>();
    }
    thumb.lastFrame = ImGui::GetFrameCount();

    drawList->AddRectFilled(min, max, IM_COL32(32, 32, 34, 255), 2.0f);
    const float pad = 2.0f;
    const ImVec2 size(max.x - min.x - 2.0f * pad, max.y - min.y - 2.0f * pad);
    auto screen = [&](const ImVec2& p) { return ImVec2(min.x + pad + p.x * size.x, min.y + pad + (1.0f - p.y) * size.y); };
    drawList->PushClipRect(min, max, true);
    if (!thumb.secondPoints.empty()) {
        const ImU32 band = (color & ~IM_COL32_A_MASK) | (64u << IM_COL32_A_SHIFT);
        for (size_t i = 0; i + 1 < thumb.points.size(); i++) {
            const ImVec2 a = screen(thumb.points[i]), b = screen(thumb.points[i + 1]);
            const ImVec2 c = screen(thumb.secondPoints[i + 1]), d = screen(thumb.secondPoints[i]);
            drawList->AddTriangleFilled(a, b, c, band);
            drawList->AddTriangleFilled(a, c, d, band);
        }
    }
    for (const std::vector<ImVec2>* line : {&thumb.points, &thumb.secondPoints}) {
        for (size_t i = 0; i + 1 < line->size(); i++) {
            drawList->AddLine(screen((*line)[i]), screen((*line)[i + 1]), color, 1.5f);
        }
    }
    drawList->PopClipRect();
    drawList->AddRect(min, max, IM_COL32(90, 90, 96, 255), 2.0f);

    // thumbnails of fields gone for a while are dropped
    if ((ImGui::GetFrameCount() & 255) == 0) {
        auto& cache = thumbnails();
        for (auto it = cache.begin(); it != cache.end();) {
            if (ImGui::GetFrameCount() - it->second.lastFrame > 600) it = cache.erase(it);
            else ++it;
        }
    }
}

EditPhase ui::curveField(const char* id, Curve& curve, const CurveFieldOptions& options) {
    ImGui::PushID(id);
    const ImGuiID fieldId = ImGui::GetID("##curveField");
    ImGuiContext& g = *GImGui;
    const float width = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) ? ImGui::CalcItemWidth()
                                                                                     : std::max(ImGui::GetContentRegionAvail().x, 8.0f);
    const ImVec2 size(std::max(width, 8.0f), options.height > 0.0f ? options.height : ImGui::GetFrameHeight());
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##curveField", size);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    CurveWindow& window = CurveWindow::get();
    std::vector<Curve> value{curve};
    EditPhase phase = window.consume(fieldId, value);
    if (phase == EditPhase::Changing || phase == EditPhase::Cancelled) curve = value[0];
    drawCurveThumbnail(fieldId, ImGui::GetWindowDrawList(), min, max, curve, nullptr, rangeFor(options, curve, nullptr),
                       options.color ? options.color : kDefaultCurveColor);
    if (options.mixed) ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 4.0f, min.y), IM_COL32(255, 255, 255, 200), "-");
    if (window.targets(fieldId)) ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavCursor), 2.0f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##curveFieldMenu");
    std::vector<Curve> picked;
    if (curveMenu("##curveFieldMenu", value, picked, options) && !picked.empty()) {
        curve = picked[0];
        phase = EditPhase::Applied;
    }
    if (clicked && !window.targets(fieldId)) {
        window.open(fieldId, {curve}, options);
        phase = EditPhase::Begin;
    }
    ImGui::PopID();
    return phase;
}

EditPhase ui::minMaxCurveField(const char* id, particles::MinMaxCurve& value, const CurveFieldOptions& options) {
    using particles::CurveMode;
    ImGui::PushID(id);
    EditPhase phase = EditPhase::None;
    const float button = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float avail = std::max(8.0f, ImGui::GetContentRegionAvail().x - button);
    auto inline_ = [&](bool edited) {
        if (edited) phase = EditPhase::Changing;
        else if (ImGui::IsItemDeactivatedAfterEdit() && phase == EditPhase::None) phase = EditPhase::Committed;
    };
    switch (value.mode) {
        case CurveMode::Constant: {
            ImGui::SetNextItemWidth(avail);
            const bool edited = ImGui::DragFloat("##constant", &value.constant, 0.01f, 0.0f, 0.0f, "%.3f");
            if (edited) value.constantMax = value.constant;
            inline_(edited);
            break;
        }
        case CurveMode::TwoConstants: {
            const float half = std::max(4.0f, avail * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f);
            ImGui::SetNextItemWidth(half);
            inline_(ImGui::DragFloat("##min", &value.constant, 0.01f, 0.0f, 0.0f, "%.3f"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            inline_(ImGui::DragFloat("##max", &value.constantMax, 0.01f, 0.0f, 0.0f, "%.3f"));
            break;
        }
        case CurveMode::Curve:
        case CurveMode::TwoCurves: {
            const bool two = value.mode == CurveMode::TwoCurves;
            const float multiplierWidth = Theme::dpi(56.0f);
            const float thumbWidth = std::max(8.0f, avail - multiplierWidth - ImGui::GetStyle().ItemSpacing.x);
            const ImGuiID fieldId = ImGui::GetID("##curves");
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 size(thumbWidth, options.height > 0.0f ? options.height : ImGui::GetFrameHeight());
            const bool clicked = ImGui::InvisibleButton("##curves", size);
            const ImVec2 max(min.x + size.x, min.y + size.y);
            CurveWindow& window = CurveWindow::get();
            std::vector<Curve> curves{value.curve};
            if (two) curves.push_back(value.curveMax);
            const EditPhase windowPhase = window.consume(fieldId, curves);
            if (windowPhase == EditPhase::Changing || windowPhase == EditPhase::Cancelled) {
                value.curve = curves[0];
                if (two && curves.size() > 1) value.curveMax = curves[1];
            }
            if (windowPhase != EditPhase::None) phase = windowPhase;
            drawCurveThumbnail(fieldId, ImGui::GetWindowDrawList(), min, max, value.curve, two ? &value.curveMax : nullptr,
                               rangeFor(options, value.curve, two ? &value.curveMax : nullptr), options.color ? options.color : kDefaultCurveColor);
            if (window.targets(fieldId)) ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavCursor), 2.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to edit the curve%s", two ? "s" : "");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##curveFieldMenu");
            std::vector<Curve> picked;
            if (curveMenu("##curveFieldMenu", curves, picked, options) && !picked.empty()) {
                value.curve = picked[0];
                if (two && picked.size() > 1) value.curveMax = picked[1];
                phase = EditPhase::Applied;
            }
            if (clicked && !window.targets(fieldId)) {
                window.open(fieldId, curves, options);
                phase = EditPhase::Begin;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(multiplierWidth);
            inline_(ImGui::DragFloat("##multiplier", &value.multiplier, 0.01f, 0.0f, 0.0f, "x%.2f"));
            break;
        }
        case CurveMode::Ease: {
            // start, end and the preset (each edit makes the curve again), and a multiplier other
            // than 1 (a patch's), which scales what the particles get
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const bool scaled = value.multiplier != 1.0f;
            const float multiplierWidth = scaled ? Theme::dpi(48.0f) + spacing : 0.0f;
            const float third = std::max(4.0f, (avail - multiplierWidth - 2.0f * spacing) / 3.0f);
            ImGui::SetNextItemWidth(third);
            bool edited = ImGui::DragFloat("##start", &value.constant, 0.01f, 0.0f, 0.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start");
            inline_(edited);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(third);
            const bool endEdited = ImGui::DragFloat("##end", &value.constantMax, 0.01f, 0.0f, 0.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("End");
            inline_(endEdited);
            edited = edited || endEdited;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(third);
            const std::vector<particles::EnumOption>& presets = particles::ParticleModules::easeTypes();
            const char* current = presets.front().label;
            for (const particles::EnumOption& preset : presets) {
                if (preset.value == (uint8_t)value.ease) current = preset.label;
            }
            if (ImGui::BeginCombo("##ease", current)) {
                for (const particles::EnumOption& preset : presets) {
                    if (ImGui::Selectable(preset.label, preset.value == (uint8_t)value.ease) && preset.value != (uint8_t)value.ease) {
                        value.ease = (EaseType)preset.value;
                        edited = true;
                        phase = EditPhase::Applied;
                    }
                }
                ImGui::EndCombo();
            }
            if (edited) value.rebuildEase();
            if (scaled) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Theme::dpi(48.0f));
                inline_(ImGui::DragFloat("##multiplier", &value.multiplier, 0.01f, 0.0f, 0.0f, "x%.2f"));
            }
            break;
        }
    }
    // Unity's mode triangle
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CARET_DOWN "##mode")) ImGui::OpenPopup("##minMaxMode");
    if (ImGui::BeginPopup("##minMaxMode")) {
        struct ModeItem {
            const char* label;
            CurveMode mode;
        };
        for (const ModeItem& item : {ModeItem{"Constant", CurveMode::Constant}, ModeItem{"Curve", CurveMode::Curve},
                                     ModeItem{"Random Between Two Constants", CurveMode::TwoConstants},
                                     ModeItem{"Random Between Two Curves", CurveMode::TwoCurves}, ModeItem{"Ease", CurveMode::Ease}}) {
            if (ImGui::MenuItem(item.label, nullptr, value.mode == item.mode) && value.mode != item.mode) {
                if (item.mode == CurveMode::Ease) {
                    // from the value it replaces: a curve's ends, else the constants
                    if (value.usesCurve() && !value.curve.empty()) {
                        value.constant = value.curve.evaluate(0.0f) * value.multiplier;
                        value.constantMax = value.curve.evaluate(1.0f) * value.multiplier;
                    } else if (value.mode == CurveMode::Constant) {
                        value.constantMax = value.constant;
                    }
                    value.multiplier = 1.0f;
                    value.mode = CurveMode::Ease;
                    value.rebuildEase();
                    phase = EditPhase::Applied;
                    continue;
                }
                // a curve starts from the constant it replaces
                if ((item.mode == CurveMode::Curve || item.mode == CurveMode::TwoCurves) && value.curve.empty()) {
                    const float scale = value.constant != 0.0f ? value.constant : 1.0f;
                    value.curve = Curve::constant(1.0f);
                    value.multiplier = scale;
                }
                if (item.mode == CurveMode::TwoCurves && value.curveMax.empty()) value.curveMax = value.curve;
                if (item.mode == CurveMode::TwoConstants && value.mode == CurveMode::Constant) value.constantMax = value.constant;
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

CurveWindow& CurveWindow::get() {
    static CurveWindow window;
    return window;
}

void CurveWindow::beginEdit(CommandHistory* history, const char* label) {
    CurveWindow& window = get();
    window.history = history;
    window.stepName = std::string("Change ") + (label && *label ? label : "Curve");
}

void CurveWindow::setProjectPath(const std::filesystem::path& path) {
    if (path == projectPath) return;
    projectPath = path;
    librariesLoaded = false;
}

std::filesystem::path CurveWindow::userLibraryDirectory() const {
    return curvepresets::userDirectory(AppSettings::getConfigDirectory());
}

std::filesystem::path CurveWindow::projectLibraryDirectory() const {
    return projectPath.empty() ? std::filesystem::path() : curvepresets::projectDirectory(projectPath);
}

void CurveWindow::reloadLibraries() {
    std::vector<std::string> errors;
    const std::string activeName = activeLibrary < loaded.size() ? loaded[activeLibrary].name : std::string();
    loaded = curvepresets::loadAll(userLibraryDirectory(), projectLibraryDirectory(), &errors);
    librariesLoaded = true;
    activeLibrary = 0;
    for (size_t i = 0; i < loaded.size(); i++) {
        if (loaded[i].name == activeName) activeLibrary = i;
    }
    if (!errors.empty()) warning = errors.front();
}

const std::vector<CurvePresetLibrary>& CurveWindow::libraries() {
    if (!librariesLoaded) reloadLibraries();
    return loaded;
}

void CurveWindow::open(ImGuiID id, const std::vector<Curve>& fieldCurves, const CurveFieldOptions& fieldOptions) {
    if (target != 0) close();
    setBeginEditHandler(&CurveWindow::beginEdit);
    target = id;
    options = fieldOptions;
    curves = fieldCurves;
    twoCurves = curves.size() > 1;
    title = options.label ? options.label : "Curve";
    focusOnOpen = true;
    lastSeenFrame = ImGui::GetFrameCount();
    history = nullptr;
    stepName = "Change Curve";
    changed = false;
    gestureOpen = false;
    commitAfterDelivery = false;
    closeAfterDelivery = false;
    warning.clear();
    editor = CurveEditor();
    editor.setRange(&options.range);
    if (!librariesLoaded) reloadLibraries();
}

EditPhase CurveWindow::consume(ImGuiID id, std::vector<Curve>& fieldCurves) {
    if (id == 0) return EditPhase::None;
    if (endedTarget == id && id != target) {
        endedTarget = 0;
        return EditPhase::Committed;
    }
    if (id != target) return EditPhase::None;
    lastSeenFrame = ImGui::GetFrameCount();
    if (changed) {
        changed = false;
        fieldCurves = curves;
        return EditPhase::Changing;
    }
    if (endedTarget == id) {
        endedTarget = 0;
        return EditPhase::Committed;
    }
    // between gestures the window follows the field (an undo, another edit)
    if (!gestureOpen && !commitAfterDelivery && fieldCurves.size() == curves.size()) curves = fieldCurves;
    return EditPhase::None;
}

void CurveWindow::beginGesture() {
    if (history) FieldEditSession::begin(history, stepName, this);
    gestureOpen = true;
}

void CurveWindow::endGesture(bool cancelled) {
    if (cancelled) FieldEditSession::cancel(this);
    else FieldEditSession::commit(this);
    gestureOpen = false;
    commitAfterDelivery = false;
    endedTarget = target;
}

void CurveWindow::close() {
    if (target == 0) return;
    if (gestureOpen || commitAfterDelivery) endGesture(false);
    endedTarget = target;
    target = 0;
    changed = false;
}

void CurveWindow::drawPresets() {
    ImGui::TextUnformatted("Presets");
    ImGui::SameLine();
    const char* preview = activeLibrary < loaded.size() ? loaded[activeLibrary].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    if (ImGui::BeginCombo("##presetLibrary", preview)) {
        for (size_t i = 0; i < loaded.size(); i++) {
            const std::string label = loaded[i].name + (loaded[i].scope == CurveLibraryScope::Project ? " (Project)" : "") + "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), i == activeLibrary)) activeLibrary = i;
        }
        ImGui::EndCombo();
    }
    if (activeLibrary >= loaded.size()) return;
    CurvePresetLibrary& library = loaded[activeLibrary];
    ImGui::SameLine();
    ImGui::BeginDisabled(library.readOnly || curves.empty());
    if (ImGui::Button("Save Current as Preset")) {
        std::string error;
        if (!curvepresets::addPreset(library, std::string(), curveops::normalise(curves[0], options.range), error)) warning = error;
    }
    ImGui::EndDisabled();
    if (library.readOnly && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", library.scope == CurveLibraryScope::BuiltIn ? "Pick a user or project library to save into" : library.warning.c_str());
    }
    // the strip: a thumbnail per preset, a click applies it mapped onto the range
    const float thumb = Theme::dpi(44.0f);
    for (size_t i = 0; i < library.presets.size(); i++) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID((int)i);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##preset", ImVec2(thumb, thumb * 0.6f)) && !curves.empty()) {
            beginGesture();
            curves[0] = curveops::remap(library.presets[i].curve, options.range);
            if (twoCurves && curves.size() > 1) curves[1] = curves[0];
            changed = true;
            commitAfterDelivery = true;
        }
        const ImVec2 max(min.x + thumb, min.y + thumb * 0.6f);
        curveops::CurveRange unit;
        drawCurveThumbnail(ImGui::GetID("##presetThumb"), ImGui::GetWindowDrawList(), min, max, library.presets[i].curve, nullptr, unit,
                           IM_COL32(200, 200, 200, 255));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", library.presets[i].name.c_str());
        if (!library.readOnly && ImGui::BeginPopupContextItem("##presetMenu")) {
            if (ImGui::MenuItem("Delete Preset")) {
                std::string error;
                if (!curvepresets::removePreset(library, i, error)) warning = error;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

std::vector<CurveBinding> CurveWindow::bindings() {
    std::vector<CurveBinding> out;
    static const ImU32 kColors[2] = {IM_COL32(110, 220, 110, 255), IM_COL32(220, 160, 90, 255)};
    for (size_t i = 0; i < curves.size(); i++) {
        CurveBinding b;
        b.curve = &curves[i];
        b.color = i == 0 && options.color ? options.color : kColors[i % 2];
        b.label = curves.size() > 1 ? (i == 0 ? "Min" : "Max") : "Curve";
        b.partner = curves.size() > 1 ? (int)(1 - i) : -1;
        out.push_back(b);
    }
    return out;
}

bool CurveWindow::canRun(CurveEditor::Command command) {
    return target != 0 && editor.canRun(command, bindings());
}

void CurveWindow::run(CurveEditor::Command command) {
    if (target != 0) editor.request(command);
}

CurveWindow::UiState CurveWindow::uiState() const {
    UiState state;
    state.open = target != 0;
    state.focused = isFocused();
    state.selectedKeys = editor.getSelection().size();
    state.clipboardKeys = editor.clipboardSize();
    state.view = editor.getView();
    return state;
}

void CurveWindow::show() {
    // a field not drawn for a frame (its window closed): the window closes, its step committed
    if (target != 0 && ImGui::GetFrameCount() - lastSeenFrame > 1) close();
    // a gesture's last value reached the field: its step is done (and a close waiting for it)
    if (target != 0 && commitAfterDelivery && !changed) endGesture(false);
    if (target != 0 && closeAfterDelivery && !changed) {
        closeAfterDelivery = false;
        close();
    }
    windowFocused = false;
    if (target == 0) return;

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(520.0f), Theme::dpi(380.0f)), ImGuiCond_FirstUseEver);
    if (focusOnOpen) {
        ImGui::SetNextWindowFocus();
        focusOnOpen = false;
    }
    const std::string windowTitle = title + "###" + WINDOW_NAME;
    if (ImGui::Begin(windowTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const float presetsHeight = Theme::dpi(44.0f) * 0.6f + ImGui::GetFrameHeightWithSpacing() * 2.0f;
        const float graphHeight = std::max(Theme::dpi(80.0f), ImGui::GetContentRegionAvail().y - presetsHeight - ImGui::GetFrameHeightWithSpacing() * 2.0f);
        std::vector<CurveBinding> shown = bindings();
        const CurveEditorResult result = editor.draw("##curveEditor", shown, ImVec2(0.0f, graphHeight));
        if (result.began && !gestureOpen) beginGesture();
        if (result.cancelled) {
            if (history) {
                // the session undoes what the field wrote; the editor already put the curves back
                FieldEditSession::cancel(this);
                gestureOpen = false;
                commitAfterDelivery = false;
                changed = false;
            } else {
                changed = true;
                commitAfterDelivery = true;
            }
        } else {
            if (result.changed) changed = true;
            if (result.ended) commitAfterDelivery = true;
        }
        ImGui::Separator();
        drawPresets();
        if (!warning.empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", warning.c_str());
    }
    ImGui::End();
    if (!open) {
        if (changed) closeAfterDelivery = true;
        else close();
    }
}
