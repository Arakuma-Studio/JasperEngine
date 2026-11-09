// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Particle System and Force Field inspectors (docs/specs/S20-particles.md R27). The module
// list is Unity's: Main always open, every other module a foldout with its enable checkbox,
// "Add Module" for the hidden ones. The rows come from the module registry's field tables
// (ParticleModules), so a field added there shows here. Curves and gradients are S27's fields
// (widget/CurveField.h, GradientField.h): a click opens the Curve window or the Gradient Editor,
// their driver rows below them. Every edit is one undo step (a drag merges into one) through
// ParticleEffectEdit: an asset's edit writes the asset (all its users update), an inline
// effect's edit changes that GameObject only; a multi-selection takes the same change.

#include "InspectorWindow.h"
#include "ParticleModuleRows.h"
#include "widget/ColorField.h"
#include "widget/CurveField.h"
#include "widget/GradientField.h"

#include "App.h"
#include "Backend.h"
#include "Catalog.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Widgets.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/CameraComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "Out.h"
#include "particles/AnalyticParticles.h"
#include "pixelart/PaletteUse.h"
#include "pixelart/PixelPerfectLayout.h"
#include "subsystem/RenderSystem.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "particles/ParticlePixel.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteSwapChecks.h"
#include "pixelart/particles/PixelParticlePresets.h"
#include "pixelart/particles/PixelPresetCreate.h"
#include "command/type/SetAssetMetaCmd.h"
#include "command/UndoTimeline.h"
#include "texture/TextureImportSettings.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "reflect/Enums.h"
#include "util/UIProbe.h"
#include "util/EntityPayload.h"
#include "util/ParticleEffectEdit.h"
#include "util/Util.h"
#include "window/ParticleEffectOverlay.h"
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

    // from the enum tables (editor/reflect/Enums.cpp)
    std::vector<editor::EnumEntry>& entriesForceFieldShape = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kForceFieldShape);
    std::vector<editor::EnumEntry>& entriesForceFieldKind = *editor::InspectorWindow::enumEntries(editor::reflect::enums::kForceFieldKind);

    // What the rows of one frame did: a value changed (an undo step to push) and an edit ended
    // (the step stops merging: a drag released, a checkbox clicked)
    struct FieldUi {
        editor::Project* project = nullptr;
        // where the steps go (the scene's history, the project's for an asset preview)
        editor::CommandHistory* history = nullptr;
        std::set<std::string>* expanded = nullptr;
        bool changed = false;
        bool commit = false;
        // S49: the document drawn (the Renderer's rows that depend on the Pixel Art module), the
        // grid a pixel particle uses here, the modules Analytic does not compute, a project step
        // written with this edit (the palette copy of a preset: linked to the edit's step)
        const ParticleEffectDocument* document = nullptr;
        std::string gridStatus;
        std::set<std::string> analyticBlocked;
        bool wroteProjectStep = false;
        uint32_t sceneId = 0;                 // the inspected system (Bake to Sprite Sheet)
        Entity entity = NULL_ENTITY;
        // the fields that are not their defaults (marked), and those a label's menu reset this frame
        std::set<std::string> nonDefault;
        std::vector<std::string> resets;
        // the UI probe's names of the value widgets ("<probe>.<field path>"), empty for none
        std::string probe;
        // the curve field that opened the Curve window this frame
        bool curveOpened = false;
        std::string curveField;
        CurveDriver curveDriver = CurveDriver::NormalizedAge;
        void curveBegan(editor::ui::EditPhase phase, const std::string& path, CurveDriver driver) {
            if (phase != editor::ui::EditPhase::Begin) return;
            curveOpened = true;
            curveField = path;
            curveDriver = driver;
        }

        // a field window's phase (S27 FieldEdit.h): its session in this history, its values
        // merged into the step, a one-go change its own step
        void phase(editor::ui::EditPhase phase, const char* label) {
            using editor::ui::EditPhase;
            switch (phase) {
                case EditPhase::Begin: editor::ui::beginFieldEdit(history, label); break;
                case EditPhase::Changing: changed = true; break;
                case EditPhase::Applied: changed = commit = true; break;
                case EditPhase::Committed: commit = true; break;
                case EditPhase::Cancelled:
                case EditPhase::None: break;
            }
        }

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

    // A field's label cell: marked when the value is not its default (Unity's override bar), with
    // Reset on a right-click
    void effectFieldLabel(FieldUi& ui, const FieldDesc& f, const std::string& path, int indent) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (indent > 0) ImGui::Indent(indentWidth() * indent);
        const bool marked = ui.nonDefault.count(path) > 0;
        // the bar inside the cell (the table clips what is left of it), the label after it
        const float bar = editor::Theme::dpi(2.0f);
        if (marked) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + bar, at.y + ImGui::GetFrameHeight()), ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bar * 3.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(f.label);
        if (ImGui::IsItemHovered()) {
            if (f.tooltip && marked) ImGui::SetTooltip("%s\n(changed from the default: right-click to reset)", f.tooltip);
            else if (f.tooltip) ImGui::SetTooltip("%s", f.tooltip);
            else if (marked) ImGui::SetTooltip("Changed from the default: right-click to reset");
        }
        if (marked && ImGui::BeginPopupContextItem("##field_menu")) {
            if (ImGui::MenuItem("Reset")) ui.resets.push_back(path);
            ImGui::EndPopup();
        }
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

    // ---------------------------------------------------------------- pixel shapes (S49 R31)

    // a 16x16 icon of a shape, drawn by the CPU reference of its rule (ParticlePixel: the same
    // code as the tests' golden), so the icon is the truth
    void pixelShapeIcon(ImDrawList* drawList, ImVec2 at, float side, const RendererModule& renderer, PixelShape shape) {
        PixelShapeSpec spec = PixelShapeSpec::fromRenderer(renderer);
        spec.shape = (int)shape;
        const float n = shape == PixelShape::Puff ? 8.0f : (shape == PixelShape::Chunk ? 10.0f : (shape == PixelShape::Streak ? 1.0f : 11.0f));
        const PixelLayout layout = ParticlePixel::layout(spec, Vector2(8.0f, 8.0f), Vector2(n, n), Vector2(300.0f, 300.0f), 0.6f, true);
        std::vector<PixelCell> cells;
        ParticlePixel::rasterize(spec, layout, 0.2f, 0, cells);
        const float px = side / 16.0f;
        drawList->AddRectFilled(at, ImVec2(at.x + side, at.y + side), IM_COL32(24, 24, 28, 255));
        for (const PixelCell& c : cells) {
            if (c.x < 0 || c.y < 0 || c.x >= 16 || c.y >= 16) continue;
            const ImU32 colour = c.tone == 1 ? IM_COL32(255, 138, 42, 255) : IM_COL32(255, 243, 166, (int)(255.0f * std::clamp(c.cover, 0.25f, 1.0f)));
            const ImVec2 min(at.x + (float)c.x * px, at.y + (float)(15 - c.y) * px);
            drawList->AddRectFilled(min, ImVec2(min.x + px, min.y + px), colour);
        }
    }

    bool pixelShapeCombo(const char* id, const RendererModule& renderer, uint8_t& value) {
        const EnumOption* options = nullptr;
        uint8_t count = 0;
        if (const FieldDesc* field = ParticleModules::get(ModuleId::Renderer).fields.find("pixelShape")) {
            options = field->options;
            count = field->optionCount;
        }
        const char* preview = "";
        for (uint8_t i = 0; i < count; i++) {
            if (options[i].value == value) preview = options[i].label;
        }
        bool picked = false;
        const float side = editor::Theme::dpi(16.0f);
        if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) {
            for (uint8_t i = 0; i < count; i++) {
                ImGui::PushID(i);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##shape", options[i].value == value, 0, ImVec2(0.0f, side))) {
                    picked = options[i].value != value;
                    value = options[i].value;
                }
                pixelShapeIcon(ImGui::GetWindowDrawList(), at, side, renderer, (PixelShape)options[i].value);
                ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + side + editor::Theme::dpi(6.0f), at.y), ImGui::GetColorU32(ImGuiCol_Text), options[i].label);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return picked;
    }

    // ---------------------------------------------------------------- pixel particles (S49)

    void noteRow(const std::string& text, bool warning) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0f);
        if (warning) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
        if (warning) ImGui::TextUnformatted((std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + text).c_str());
        else ImGui::TextDisabled("%s", text.c_str());
        if (warning) ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    bool pixelActive(const ParticleEffectDocument& d) {
        return d.renderer.renderMode == RenderMode::Pixel || (d.pixelArt.present && d.pixelArt.enabled);
    }

    // R30: the grid in use, the Ramp Offset fallback, Rescale to Pixels Per Unit (R20)
    void drawPixelArtNotes(FieldUi& ui, ParticleEffectDocument& edited) {
        if (!ui.gridStatus.empty()) noteRow("Grid: " + ui.gridStatus, false);
        const bool ramp = edited.main.startColor.mode == GradientValueMode::Ramp ||
                          (edited.colorOverLifetime.present && edited.colorOverLifetime.enabled && edited.colorOverLifetime.color.mode == GradientValueMode::Ramp);
        if (edited.pixelArt.secondary == SecondaryColorMode::RampOffset && !ramp)
            noteRow("Ramp Offset: neither Start Color nor Color over Lifetime is a Palette Ramp, so the Secondary colour is drawn", false);
        const int ppu = ui.project ? std::max(ui.project->getPixelsPerUnit(), 1) : 1;
        if (edited.designPixelsPerUnit > 0.0f && (int)edited.designPixelsPerUnit != ppu) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(("Rescale to Pixels Per Unit " + std::to_string(ppu)).c_str())) {
                editor::pixelart::scaleParticleMotion(edited, edited.designPixelsPerUnit / (float)ppu);
                edited.designPixelsPerUnit = (float)ppu;
                ui.track(true, true);
            }
            ImGui::SetItemTooltip("The preset was made for %d pixels per unit; its speeds, forces and shape take the project's %d (one undo step)",
                                  (int)edited.designPixelsPerUnit, ppu);
        }
    }

    // R9, R13: what pixel art ignores, and a texture that cannot keep its colours exact
    void drawRendererPixelNotes(FieldUi& ui, ParticleEffectDocument& edited) {
        if (ui.entity != NULL_ENTITY) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            // the Particle Effect window's Record (S91 R12): its Pixel Grid for pixel art, and an Aseprite JSON beside
            if (ImGui::Button("Bake to Sprite Sheet...")) getEditorHost().openParticleEffectSubtree(ui.sceneId, ui.entity);
            ImGui::SetItemTooltip("Opens the Particle Effect window: its Record writes a sprite sheet (turn on Pixel Grid for pixel art, and Aseprite JSON for a texture's Sprite Mode)");
        }
        if (!pixelActive(edited)) return;
        for (const std::string& feature : edited.renderer.shaderFeatures) {
            if (feature == "EROSION" || feature == "SOFT")
                noteRow(feature + " does nothing for pixel art: its pixels are whole and fade by dither", false);
        }
        if (edited.renderer.renderMode == RenderMode::Pixel || edited.renderer.texture.empty() || !ui.project) return;
        editor::pixelart::PaletteSwapTextureCheck check = editor::pixelart::checkLosslessTexture(*ui.project, edited.renderer.texture);
        check.subject = "Pixel particles";
        if (!check.lossy()) return;
        noteRow(check.message(), true);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button("Fix##particle_texture_lossless")) {
            Material material;
            material.baseColorTexture = Texture(edited.renderer.texture);
            material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
            material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
            editor::pixelart::PaletteSwapTextureFix fix;
            if (editor::pixelart::planPaletteSwapTextureFix(*ui.project, material, check, fix) && fix.importChanged) {
                editor::Command* meta = new editor::SetAssetMetaCmd(ui.project, ui.project->normalizeToProjectRelative(
                    ui.project->getAssetsPath() / editor::FileUtils::pathFromUtf8(fix.texture)), editor::TextureImportSettings::metaKey, fix.importSettings);
                meta->setNoMerge();
                ui.project->getProjectCommandHistory()->addCommand(meta);
            }
        }
        ImGui::SetItemTooltip("Lossless, no mipmaps, a Max Size that keeps the image (one undo step); the particles sample it Nearest");
    }

    // R33: Analytic's unsupported modules and the loop's rate
    void drawAnalyticBanner(FieldUi& ui, ParticleEffectDocument& edited) {
        ui.analyticBlocked.clear();
        if (edited.simulationMode != SimulationMode::Analytic) return;
        std::vector<Diagnostic> diagnostics;
        AnalyticParticles::validate(edited, diagnostics);
        std::vector<std::string> others;
        for (const Diagnostic& d : diagnostics) {
            if (d.path.rfind("modules.", 0) == 0) {
                const std::string rest = d.path.substr(8);
                ui.analyticBlocked.insert(rest.substr(0, rest.find_first_of(".[")));
            }
            others.push_back(d.message);
        }
        if (!diagnostics.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Analytic computes %zu setting(s) of this effect: disable them or use Simulated.", diagnostics.size());
            ImGui::PopStyleColor();
            for (const std::string& line : others) ImGui::BulletText("%s", line.c_str());
            bool any = false;
            for (const std::string& key : ui.analyticBlocked) {
                const ModuleDesc* m = ParticleModules::find(key);
                any = any || (m && m->hasEnable && m->isEnabled(edited) && key != "renderer");
            }
            ImGui::BeginDisabled(!any);
            if (ImGui::Button("Disable unsupported modules")) {
                for (const std::string& key : ui.analyticBlocked) {
                    const ModuleDesc* m = ParticleModules::find(key);
                    if (m && m->hasEnable && key != "renderer") m->setEnabled(edited, false);
                }
                ui.track(true, true);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Unchecks every module Analytic cannot compute (one undo step); switching back to Simulated keeps them as they are");
        }
        if (edited.looping && edited.emission.present && edited.emission.enabled && edited.emission.rateOverTime.isConstant()) {
            const float rate = edited.emission.rateOverTime.constant;
            const float exact = AnalyticParticles::loopRate(rate, edited.duration);
            if (rate > 0.0f) ImGui::TextDisabled("Rate %g -> %.1f/s for a seamless loop of %g s", (double)rate, (double)exact, (double)edited.duration);
        }
    }

    // R20: Replace with Preset and Match Project Palette
    void drawPresetRow(FieldUi& ui, ParticleEffectDocument& edited) {
        if (!ui.project) return;
        ImGui::SetNextItemWidth(editor::Theme::dpi(170.0f));
        if (ImGui::BeginCombo("##replace_preset", "Replace with Preset")) {
            for (const std::string& name : editor::pixelart::pixelParticlePresetNames()) {
                if (!ImGui::Selectable(("Pixel " + editor::pixelart::pixelParticlePresetLabel(name)).c_str())) continue;
                editor::pixelart::ProjectPixelPreset made;
                std::string error;
                if (!editor::pixelart::prepareProjectPixelPreset(*ui.project, name, "Particles/Pixel", made, error)) {
                    editor::Out::error("Replace with Preset: %s", error.c_str());
                    continue;
                }
                if (!made.writes.empty()) {
                    if (!editor::ProjectFileWrites::writeUndoable(ui.project, made.writes, error, nullptr, nullptr, {}, true)) {
                        editor::Out::error("Replace with Preset: %s", error.c_str());
                        continue;
                    }
                    ui.wroteProjectStep = true;
                }
                edited = made.document;
                ui.track(true, true);
            }
            ImGui::EndCombo();
        }
        const std::string palettePath = editor::pixelart::defaultPalettePath(*ui.project);
        if (palettePath.empty()) return;
        ImGui::SameLine();
        if (ImGui::Button("Match Project Palette")) {
            const editor::pixelart::ResolvedPalette palette = editor::pixelart::resolvePaletteName(*ui.project, palettePath);
            if (palette.ok && editor::pixelart::matchPaletteRamps(edited, palette.palette)) ui.track(true, true);
        }
        ImGui::SetItemTooltip("Every ramp step to the nearest colour of the closest ramp of %s (one undo step)", palettePath.c_str());
    }

    // ---------------------------------------------------------------- list rows

    void trashButton(const char* id, bool& clicked) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        clicked = ImGui::Button((std::string(ICON_FA_TRASH_CAN "##") + id).c_str());
        ImGui::PopStyleColor(2);
    }

    float trashWidth() {
        return ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
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

    // a particle curve's frame: time 0 to 1, values from 0 (or below) to 1 (or above)
    editor::curveops::CurveRange particleRange(const Curve& curve, const Curve* second) {
        float lo = 0.0f, hi = 1.0f, a, b;
        if (!curve.empty()) {
            curve.valueRange(a, b);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        if (second && !second->empty()) {
            second->valueRange(a, b);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        return editor::curveops::CurveRange{0.0f, lo, 1.0f, hi, false};
    }

    void drawMinMaxCurve(FieldUi& ui, const FieldDesc& f, MinMaxCurve& value, const std::string& id, int indent) {
        effectFieldLabel(ui, f, id, indent);
        editor::ui::CurveFieldOptions options;
        options.range = particleRange(value.curve, value.mode == CurveMode::TwoCurves ? &value.curveMax : nullptr);
        options.label = f.label;
        const editor::ui::EditPhase phase = editor::ui::minMaxCurveField(id.c_str(), value, options);
        ui.phase(phase, f.label);
        ui.curveBegan(phase, id, value.driver);
        // the curve's driver below it
        if (value.usesCurve()) drawDriver(ui, value, indent + 1);
    }

    // a colour of the effect (sRGB as picked, alpha) through the Color window
    void colorRow(FieldUi& ui, const char* id, Vector4& color, const char* label) {
        editor::ui::ColorFieldOptions options;
        options.linearStorage = false;
        options.label = label;
        ui.phase(editor::ui::colorField(id, color, options), label);
    }

    void drawMinMaxGradient(FieldUi& ui, const FieldDesc& f, MinMaxGradient& value, const std::string& id, int indent) {
        effectFieldLabel(ui, f, id, indent);
        editor::ui::GradientFieldOptions options;
        options.label = f.label;
        // Palette Ramp's palette field: the project's .palette assets (S49 R32)
        editor::Project* project = ui.project;
        options.palettes = [project](){
            std::vector<std::string> found;
            std::error_code ec;
            const std::filesystem::path root = project->getAssetsPath();
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)){
                if (it->is_regular_file(ec) && it->path().extension() == ".palette")
                    found.push_back(project->normalizeToAssetsRelative(it->path().string()).generic_string());
            }
            std::sort(found.begin(), found.end());
            return found;
        };
        options.paletteOf = [project](const std::string& absolute){
            if (std::filesystem::path(absolute).extension() != ".palette" || !project->isInsideAssetsPath(absolute)) return std::string();
            return project->normalizeToAssetsRelative(absolute).generic_string();
        };
        ui.phase(editor::ui::minMaxGradientField(id.c_str(), value, options), f.label);
        if (value.usesGradient()) drawDriver(ui, value, indent + 1);
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
            *guid = path.empty() ? std::string() : ui.project->makeAssetRef(path, editor::AssetPathBase::Assets).guid.str();
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
                effectFieldLabel(ui, f, id, indent);
                bool v = *static_cast<bool*>(p);
                if (ImGui::Checkbox("##v", &v)) {
                    *static_cast<bool*>(p) = v;
                    ui.track(true, true);
                }
                break;
            }
            case FieldType::Int: {
                effectFieldLabel(ui, f, id, indent);
                int& v = *static_cast<int*>(p);
                const bool clamp = ranged(f);
                const bool edited = ImGui::DragInt("##v", &v, 0.2f, clamp ? (int)f.minValue : 0, clamp ? (int)f.maxValue : 0, "%d",
                                                   clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
                ui.track(edited);
                break;
            }
            case FieldType::UInt: {
                effectFieldLabel(ui, f, id, indent);
                uint32_t& v = *static_cast<uint32_t*>(p);
                const uint32_t lo = f.minValue > 0.0f ? (uint32_t)f.minValue : 0u;
                const uint32_t hi = f.maxValue < 4.0e9f ? (uint32_t)f.maxValue : 0xffffffffu;
                const bool edited = ImGui::DragScalar("##v", ImGuiDataType_U32, &v, 0.5f, &lo, &hi, "%u", ImGuiSliderFlags_AlwaysClamp);
                ui.track(edited);
                break;
            }
            case FieldType::Float: {
                effectFieldLabel(ui, f, id, indent);
                ui.track(dragFloat("##v", static_cast<float*>(p), f));
                break;
            }
            case FieldType::Enum: {
                effectFieldLabel(ui, f, id, indent);
                uint8_t& v = *static_cast<uint8_t*>(p);
                // its path as its id ("##effect.simulationMode"): what find_ui_item and the UI tests name
                const std::string comboId = "##" + id;
                if (std::strcmp(f.key, "pixelShape") == 0 && ui.document) {
                    if (pixelShapeCombo(comboId.c_str(), ui.document->renderer, v)) ui.track(true, true);
                    break;
                }
                if (enumCombo(comboId.c_str(), f.options, f.optionCount, v)) ui.track(true, true);
                break;
            }
            case FieldType::Mask: {
                effectFieldLabel(ui, f, id, indent);
                drawMask(ui, *static_cast<uint32_t*>(p));
                break;
            }
            case FieldType::Vector2: {
                effectFieldLabel(ui, f, id, indent);
                ui.track(ImGui::DragFloat2("##v", &static_cast<Vector2*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Vector3: {
                effectFieldLabel(ui, f, id, indent);
                ui.track(ImGui::DragFloat3("##v", &static_cast<Vector3*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Vector4: {
                effectFieldLabel(ui, f, id, indent);
                ui.track(ImGui::DragFloat4("##v", &static_cast<Vector4*>(p)->x, dragSpeed(f), 0.0f, 0.0f, floatFormat(f)));
                break;
            }
            case FieldType::Color: {
                effectFieldLabel(ui, f, id, indent);
                colorRow(ui, "##v", *static_cast<Vector4*>(p), f.label);
                break;
            }
            case FieldType::String: {
                effectFieldLabel(ui, f, id, indent);
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
                effectFieldLabel(ui, f, id, indent);
                Curve& curve = *static_cast<Curve*>(p);
                editor::ui::CurveFieldOptions options;
                options.range = particleRange(curve, nullptr);
                options.label = f.label;
                const editor::ui::EditPhase phase = editor::ui::curveField(id.c_str(), curve, options);
                ui.phase(phase, f.label);
                ui.curveBegan(phase, id, CurveDriver::NormalizedAge);
                break;
            }
            case FieldType::Gradient: {
                effectFieldLabel(ui, f, id, indent);
                Gradient& gradient = *static_cast<Gradient*>(p);
                editor::ui::GradientFieldOptions options;
                options.label = f.label;
                ui.phase(editor::ui::gradientField(id.c_str(), gradient, options), f.label);
                break;
            }
            case FieldType::Struct: {
                effectFieldLabel(ui, f, id, indent);
                if (f.element) drawStruct(ui, *f.element, p, id, indent + 1, Pass::All);
                break;
            }
            case FieldType::Array: {
                effectFieldLabel(ui, f, id, indent);
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
                effectFieldLabel(ui, f, id, indent);
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
                effectFieldLabel(ui, f, id, indent);
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
        if (!ui.probe.empty()) editor::UIProbe::item(ui.probe + "." + id);
        ImGui::PopID();
    }

    bool shown(const FieldDesc& f, const void* owner, Pass pass) {
        if (f.flags & (FieldFlag_Hidden | FieldFlag_Guid)) return false;
        if (pass == Pass::Basic && (f.flags & FieldFlag_Advanced)) return false;
        if (pass == Pass::Advanced && !(f.flags & FieldFlag_Advanced)) return false;
        return !f.visible || f.visible(owner);
    }

    void drawStruct(FieldUi& ui, const StructDesc& desc, void* owner, const std::string& path, int indent, Pass pass) {
        // Sizes in Pixels ignores Min and Max Particle Size (S49 R3)
        const bool pixelSized = ui.document && ui.document->pixelArt.present && ui.document->pixelArt.enabled && ui.document->pixelArt.pixelSizes;
        for (size_t i = 0; i < desc.fields.size(); i++) {
            const FieldDesc& f = desc.fields[i];
            if (!shown(f, owner, pass)) continue;
            if (pixelSized && (std::strcmp(f.key, "minScreenSize") == 0 || std::strcmp(f.key, "maxScreenSize") == 0)) continue;
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
        labelWidth = std::max(labelWidth, std::min(ImGui::GetContentRegionAvail().x * 0.4f, editor::Theme::dpi(180.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, editor::Theme::dpi(1.0f)));
        ImGui::BeginTable(("table_" + id).c_str(), 2, ImGuiTableFlags_Resizable);
        ImGui::PopStyleVar();
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value");
    }

    void endRows() {
        ImGui::EndTable();
    }

    // the module's section of the docs, in the browser
    void helpButton(const ModuleDesc& module) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const bool clicked = ImGui::Button(ICON_FA_CIRCLE_QUESTION "##module_help");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the %s documentation", module.label);
        if (clicked) {
            const std::string url = editor::particlerows::moduleHelpUrl(module);
            auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
            if (!openInShell || !openInShell(ImGui::GetCurrentContext(), url.c_str())) {
                editor::Out::warning("Could not open your browser. Visit %s", url.c_str());
            }
        }
    }

    // A module header's right-click menu: Reset, Copy Module, Paste Module, Paste Values (and Remove
    // Module, which the caller does: true when it was picked)
    bool moduleMenu(FieldUi& ui, ParticleEffectDocument& edited, const ModuleDesc& module, const char* id) {
        bool remove = false;
        if (!ImGui::BeginPopupContextItem(id)) return false;
        if (ImGui::MenuItem("Reset") && editor::particlerows::resetModule(edited, module.id)) ui.track(true, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Module")) ImGui::SetClipboardText(editor::particlerows::copyModule(edited, module.id).c_str());
        const char* clipboard = ImGui::GetClipboardText();
        const std::string text = clipboard ? clipboard : "";
        const ModuleDesc* copied = editor::particlerows::clipboardModule(text);
        std::string error;
        if (ImGui::MenuItem(copied ? (std::string("Paste Module (") + copied->label + ")").c_str() : "Paste Module", nullptr, false, copied != nullptr)) {
            if (editor::particlerows::pasteModule(edited, text, false, &error)) ui.track(true, true);
        }
        if (ImGui::MenuItem("Paste Values", nullptr, false, copied == &module)) {
            if (editor::particlerows::pasteModule(edited, text, true, &error)) ui.track(true, true);
        }
        if (!error.empty()) editor::Out::warning("Particle module: %s", error.c_str());
        if (!module.alwaysPresent) {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Module")) remove = true;
        }
        ImGui::EndPopup();
        return remove;
    }

    // The whole effect: Main always open, a foldout per present module with its checkbox,
    // Add Module, the exposed parameters. Edits go into `edited`; `ui` says what happened.
    void drawEffectDocument(FieldUi& ui, ParticleEffectDocument& edited, const std::string& prefix) {
        const float labelSize = ImGui::CalcTextSize("Simulation Speed X").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const ModuleDesc& mainModule = ParticleModules::get(ModuleId::Main);
        ui.document = &edited;
        drawPresetRow(ui, edited);

        // Main: the effect's own settings and the Main module, always open
        ImGui::SeparatorText(mainModule.label);
        moduleMenu(ui, edited, mainModule, "##main_menu");
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
        drawAnalyticBanner(ui, edited);

        for (const ModuleDesc& module : ParticleModules::all()) {
            if (module.id == ModuleId::Main || !module.isPresent(edited)) continue;
            ImGui::PushID(module.key);
            const bool blocked = ui.analyticBlocked.count(module.key) > 0;

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (module.id == ModuleId::Emission || module.id == ModuleId::Shape || module.id == ModuleId::Renderer) {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            // "##module_<key>": what find_ui_item and the UI tests name a module's header by
            const bool open = ImGui::TreeNodeEx((std::string("##module_") + module.key).c_str(), flags, "%s", "");
            if (module.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", module.tooltip);
            bool remove = moduleMenu(ui, edited, module, "##module_menu");
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
            if (blocked) {
                ImGui::TextDisabled("%s " ICON_FA_TRIANGLE_EXCLAMATION, module.label);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not computed in Analytic mode: disable it or use Simulated");
            } else {
                ImGui::TextUnformatted(module.label);
            }
            ImGui::SameLine(std::max(0.0f, rightEdge - trashWidth() * (module.alwaysPresent ? 1.0f : 2.0f)));
            helpButton(module);
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
                ImGui::BeginDisabled((module.hasEnable && !module.isEnabled(edited)) || (blocked && module.id != ModuleId::Renderer));
                beginRows(prefix + "_module_" + module.key, labelSize);
                drawFieldsWithAdvanced(ui, module.fields, module.module(edited), module.key);
                if (module.id == ModuleId::PixelArt) drawPixelArtNotes(ui, edited);
                if (module.id == ModuleId::Renderer) drawRendererPixelNotes(ui, edited);
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
            // a copied module the effect lacks comes in as it was
            const char* clipboard = ImGui::GetClipboardText();
            const std::string text = clipboard ? clipboard : "";
            const ModuleDesc* copied = editor::particlerows::clipboardModule(text);
            if (copied && !copied->isPresent(edited)) {
                ImGui::Separator();
                if (ImGui::MenuItem((std::string("Paste Module (") + copied->label + ")").c_str()) &&
                    editor::particlerows::pasteModule(edited, text, false)) {
                    ui.track(true, true);
                }
            }
            ImGui::EndPopup();
        }

        // the parameters scripts and the component set by name (setFloat("Intensity", 2))
        if (ImGui::TreeNodeEx(("Exposed Parameters##" + prefix).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            beginRows(prefix + "_exposed", labelSize);
            drawStructList(ui, "Parameters", ParticleModules::exposedFields(), edited.exposed, 0, "exposed");
            endRows();
            ImGui::TreePop();
        }

        // what sendTrigger("name") does besides firing the Trigger sub-emitters of that name
        if (ImGui::TreeNodeEx(("Triggers##" + prefix).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            beginRows(prefix + "_triggers", labelSize);
            drawStructList(ui, "Triggers", ParticleModules::triggerFields(), edited.triggers, (size_t)kMaxTriggers, "triggers");
            endRows();
            ImGui::TreePop();
        }
    }

}

// R30: the grid a pixel particle of this scene draws on
static std::string particleGridStatus(editor::Project* project, Scene* scene) {
    const int ppu = project ? std::max(project->getPixelsPerUnit(), 1) : 1;
    if (RenderSystem* render = scene ? scene->getSystem<RenderSystem>().get() : nullptr) {
        if (const jasperengine::pixelart::PixelPerfectLayout* layout = render->getPixelPerfectLayout())
            return "Pixel Perfect Camera, " + std::to_string(layout->targetWidth) + "x" + std::to_string(layout->targetHeight) + ", PPU " + std::to_string(ppu);
    }
    const CameraComponent* camera = scene ? scene->findComponent<CameraComponent>(scene->getCamera()) : nullptr;
    if (camera && camera->type == CameraType::CAMERA_PERSPECTIVE)
        return "Perspective: screen pixels";
    return "Orthographic, PPU " + std::to_string(ppu);
}

void editor::particlerows::drawEffect(EffectRows& rows, ParticleEffectDocument& edited) {
    FieldUi ui;
    ui.project = rows.project;
    ui.history = rows.history;
    ui.expanded = rows.expanded;
    ui.nonDefault = nonDefaultFields(edited);
    ui.probe = UIProbe::enabled() ? rows.prefix : std::string();
    ui.gridStatus = rows.gridStatus;
    ui.sceneId = rows.sceneId;
    ui.entity = rows.entity;
    drawEffectDocument(ui, edited, rows.prefix);
    rows.wroteProjectStep = rows.wroteProjectStep || ui.wroteProjectStep;
    // a label's Reset is a change of its own, one step
    for (const std::string& path : ui.resets) {
        if (resetField(edited, path)) ui.track(true, true);
    }
    rows.changed = rows.changed || ui.changed;
    rows.commit = rows.commit || ui.commit;
    if (ui.curveOpened) {
        rows.curveOpened = true;
        rows.curveField = ui.curveField;
        rows.curveDriver = ui.curveDriver;
    }
}

void editor::particlerows::drawDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    ::drawDiagnostics(diagnostics);
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
    RowSettings settingsUnscaled;
    settingsUnscaled.help = "Advance on the unscaled frame time: plays while Engine.timeScale is 0 (a pause menu). "
                            "UI particles usually want it. The effect's own Unscaled Time does the same for every system using it.";
    propertyRow(RowPropertyType::Bool, cpType, "useUnscaledTime", "Unscaled Time", sceneProject, entities, settingsUnscaled);
    // particles in a UI (under a RectTransform): drawn in Hierarchy order, clipped by panels, in UI units
    if (entities.size() == 1) {
        bool inUI = false;
        for (Entity e = entities[0]; e != NULL_ENTITY && !inUI;) {
            const Transform* tf = scene->findComponent<Transform>(e);
            e = tf ? tf->parent : NULL_ENTITY;
            inUI = e != NULL_ENTITY && scene->findComponent<RectTransformComponent>(e) != nullptr;
        }
        if (inUI) {
            // (an asset's own Unscaled Time is not read here: its effect is not the GameObject's)
            const bool unscaled = first->useUnscaledTime || (first->effect.empty() && first->inlineEffect.useUnscaledTime);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", unscaled ? "UI particles: drawn between their siblings, clipped by panels, sizes in UI units."
                                               : "UI particles: turn on Unscaled Time so they play in a paused game (menus).");
            ImGui::PopTextWrapPos();
        }
    }
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

    if (entities.size() == 1) {
        if (anyAsset || anyInline) ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FIRE " Open in Particle Effect")) getEditorHost().openParticleEffectSubtree(sceneProject->id, entities[0]);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This GameObject's Particle Systems, and those below it, as one effect: outline, timing, preview");
    }

    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (!ParticleEffectEdit::read(*first, document, &diagnostics)) {
        drawDiagnostics(diagnostics);
        return;
    }
    drawDiagnostics(diagnostics);

    ParticleEffectDocument edited = document;
    particlerows::EffectRows ui;
    ui.project = project;
    ui.history = CommandHandle::get(sceneProject->id);
    ui.expanded = &particleExpanded;
    ui.prefix = "particle";
    ui.gridStatus = particleGridStatus(project, scene);
    if (entities.size() == 1) {
        ui.sceneId = sceneProject->id;
        ui.entity = entities[0];
    }
    particlerows::drawEffect(ui, edited);

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
        // a preset's palette copy (project) and the edit using it (scene): one undo step
        if (ui.wroteProjectStep) pixelart::linkPaletteCopy(*project, sceneProject->id);
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
            case ExposedType::Color: {
                editor::ui::ColorFieldOptions options;
                options.linearStorage = false;
                options.label = parameter.name.c_str();
                const editor::ui::EditPhase phase = editor::ui::colorField("##value", value, options);
                if (phase == editor::ui::EditPhase::Begin) editor::ui::beginFieldEdit(CommandHandle::get(sceneProject->id), parameter.name.c_str());
                edited = phase == editor::ui::EditPhase::Changing;
                if (phase == editor::ui::EditPhase::Committed) commit = true;
                break;
            }
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
            const bool sameScene = sameSceneDropProblem(*project, *entityPayload, sceneProject->id).empty();
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
    ui.history = CommandHandle::get(sceneProject->id);
    ui.expanded = &particleExpanded;
    Curve falloff = field.falloff;
    ImGui::PushID("falloff");
    labelCell("Falloff", "strength over the distance from the centre (0) to the edge (1)", 0);
    editor::ui::CurveFieldOptions falloffOptions;
    falloffOptions.range = particleRange(falloff, nullptr);
    falloffOptions.label = "Falloff";
    ui.phase(editor::ui::curveField("##falloff", falloff, falloffOptions), "Falloff");
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
    particleAssetFrame = ImGui::GetFrameCount();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(ICON_FA_FIRE " %s", std::filesystem::path(path).filename().string().c_str());
    UIProbe::item("asset.particles.header");
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
    particlerows::EffectRows ui;
    ui.project = project;
    ui.history = project->getProjectCommandHistory();
    ui.expanded = &particleExpanded;
    ui.prefix = "particle_asset";
    particlerows::drawEffect(ui, edited);

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
