// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > scene settings > Post-processing (shader library phase 4): the engine's effects
// (PostProcessingSettings) by group, the presets and the cost readout. Every row edits the
// Catalog scene property post_processing.<field> (dragging merges into one undo step); group
// toggles and presets replace the whole struct (post_processing) in one step. Fields the groups
// below do not name are drawn generically under "Other", so a field added to the table in
// SceneSettings.cpp shows up here without code.

#include "InspectorWindow.h"
#include "PaletteAssetField.h"
#include "widget/ColorPickerWindow.h"
#include "log/LogFormatCheck.h"

#include "App.h"
#include "Backend.h"
#include "Catalog.h"
#include "Out.h"
#include "PostProcessingPresets.h"
#include "Theme.h"
#include "imgui_internal.h"
#include "command/CommandHandle.h"
#include "command/type/ScenePropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/Glitch.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteUse.h"
#include "subsystem/RenderSystem.h"
#include "shader/ShaderManifest.h"
#include "util/FileDialogs.h"
#include "util/UIUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>

using namespace jasperengine;

namespace {

    using Field = PostProcessingSettings::Field;
    using FT = PostProcessingSettings::FieldType;

    // a parameter row of an effect: the field's snake name and its row label
    struct ParamDef {
        const char* field;
        const char* label;
    };

    // one effect: its enable flag (null: an effect of neutral values, always shown), its
    // parameters (shown while it is on), what it does, and how to turn on only it (to ask the
    // engine its cost tier)
    struct EffectDef {
        const char* name;
        const char* enable;
        std::vector<ParamDef> params;
        const char* help;
        void (*isolate)(PostProcessingSettings& settings);
        const char* tierText; // replaces the derived tier text (MSAA: part of the scene pass)
    };

    struct GroupDef {
        const char* name;
        std::vector<EffectDef> effects;
    };

    const std::vector<GroupDef>& groups() {
        static const std::vector<GroupDef> table = {
            {"Color", {
                {"Exposure & Tone Mapping", nullptr, {{"exposure", "Exposure"}, {"tonemap", "Tone Mapping"}},
                 "Exposure in stops, then the curve that maps the lit scene to the screen: ACES (filmic), Neutral (Khronos "
                 "PBR: base colours stay as authored), Reinhard, AgX. None clamps, as without post-processing. The scene "
                 "image is 8-bit, so the curve runs in every shader before the write: transparent objects blend "
                 "after it (display values), as in mobile renderers. A painted terrain (PBR layers) with any "
                 "colour effect leaves its base material's emissive texture out (texture budget; the emissive "
                 "factor still applies).",
                 [](PostProcessingSettings& s) { s.tonemap = TonemapMode::ACES; }, nullptr},
                {"Color Grading", "color_grading_enabled",
                 {{"temperature", "Temperature"}, {"tint", "Tint"}, {"contrast", "Contrast"}, {"saturation", "Saturation"},
                  {"lift", "Lift"}, {"gamma", "Gamma"}, {"gain", "Gain"}, {"lut_texture", "LUT Texture"},
                  {"lut_input_encoding", "LUT Input Encoding"}, {"lut_contribution", "LUT Contribution"}, {"lut_size", "LUT Size"}},
                 "White balance, contrast, lift/gamma/gain, saturation and an optional LUT: an image strip (N*N x N, "
                 "Unity's layout) or a .cube file (1D, 3D or both, from Resolve and other grading tools), baked with the "
                 "curve into one small colour LUT whenever a value changes (no GPU cost). A .cube in a log encoding applies "
                 "before the tone mapping; prefer LUT Size 32 with a .cube.",
                 [](PostProcessingSettings& s) { s.colorGradingEnabled = true; s.contrast = 1.2f; }, nullptr},
                {"Monochrome", "monochrome_enabled", {{"monochrome_mode", "Mode"}, {"monochrome_intensity", "Intensity"}},
                 "Grayscale or sepia, baked into the colour LUT (free once any colour effect is on).",
                 [](PostProcessingSettings& s) { s.monochromeEnabled = true; }, nullptr},
                {"Dither", "dither_enabled", {},
                 "Tiny noise before the 8-bit write: removes banding in smooth gradients (skies, fog).",
                 [](PostProcessingSettings& s) { s.ditherEnabled = true; }, nullptr},
            }},
            {"Lens", {
                {"Vignette", "vignette_enabled",
                 {{"vignette_intensity", "Intensity"}, {"vignette_smoothness", "Smoothness"}, {"vignette_roundness", "Roundness"},
                  {"vignette_color", "Color"}},
                 "Darkens the image towards its corners (towards Color).",
                 [](PostProcessingSettings& s) { s.vignetteEnabled = true; }, nullptr},
                {"Chromatic Aberration", "chromatic_aberration_enabled", {{"chromatic_aberration_intensity", "Intensity"}},
                 "Red and blue fringes growing towards the screen's edges (two extra taps). With FXAA, every channel "
                 "stays antialiased where the shift is under a pixel; a stronger fringe at the edges is a shifted "
                 "copy, not antialiased again.",
                 [](PostProcessingSettings& s) { s.chromaticAberrationEnabled = true; }, nullptr},
                {"Film Grain", "film_grain_enabled",
                 {{"film_grain_type", "Type"}, {"film_grain_intensity", "Intensity"}, {"film_grain_response", "Response"},
                  {"film_grain_size", "Size"}, {"film_grain_colored", "Colored"}, {"film_frame_rate", "Frame Rate"}},
                 "Animated noise, weaker in bright areas (Response). Classic: a new hash per pixel and frame. Lens: "
                 "FidelityFX Lens-style cells of Size pixels. Filmic: a 5x5 lattice of soft grains (desktop / high-end; "
                 "Colored grains per channel). Lens and Filmic change once per film frame (Frame Rate) and never push a "
                 "pixel past black or white.",
                 [](PostProcessingSettings& s) { s.filmGrainEnabled = true; }, nullptr},
                {"Halation", "halation_enabled",
                 {{"halation_intensity", "Intensity"}, {"halation_threshold", "Threshold"}, {"halation_radius", "Radius"},
                  {"halation_tint", "Tint"}},
                 "The red-orange glow film shows around bright highlights (light scattered back through the film base). "
                 "With Bloom on it reuses bloom's chain (no pass, bloom's Threshold); without, two small passes. LDR like "
                 "bloom: a white wall halates too.",
                 [](PostProcessingSettings& s) { s.halationEnabled = true; }, nullptr},
            }},
            {"Anti-aliasing", {
                {"FXAA", "fxaa_enabled", {},
                 "FXAA 3.11 (console quality) on the final image: smooths geometry and shader edges for about 5 taps "
                 "per pixel (9 on edges). It and MSAA can both be on; one is usually enough.",
                 [](PostProcessingSettings& s) { s.fxaaEnabled = true; }, nullptr},
                {"MSAA", nullptr, {{"msaa_samples", "MSAA"}},
                 "Multisampling of the main camera's scene (geometry edges only), clamped to what the device supports. "
                 "It and FXAA can both be on; one is usually enough. Not applied to mirrors, other render-to-texture "
                 "cameras, layer scenes or the edit-mode Scene view without Show post-processing; changing the "
                 "count reloads the objects.",
                 [](PostProcessingSettings& s) { s.msaaSamples = 4; },
                 "Part of the scene pass: a tile-based GPU resolves the samples in tile memory (2x is nearly free, 4x costs "
                 "bandwidth; GLES resolves with a blit), into the target the other effects read. In an exported game with "
                 "nothing else offscreen (no uber pass, custom pass or fixed resolution) the scene resolves into a target "
                 "blitted to the screen: one more full-resolution pass. Desktop GPUs pay the multisampled target's memory "
                 "and bandwidth."},
            }},
            {"Stylised", {
                {"Pixelate", "pixelate_enabled", {{"pixelate_size", "Size"}},
                 "Large square pixels (Size in screen pixels).",
                 [](PostProcessingSettings& s) { s.pixelateEnabled = true; }, nullptr},
                {"CRT", "crt_enabled",
                 {{"crt_scanlines", "Scanlines"}, {"crt_mask", "Mask"}, {"crt_curvature", "Curvature"}, {"crt_vignette", "Vignette"}},
                 "An old television: scanlines, aperture-grille mask, curved tube and its darker edges.",
                 [](PostProcessingSettings& s) { s.crtEnabled = true; }, nullptr},
                {"Edge Detect", "edge_detect_enabled",
                 {{"edge_detect_threshold", "Threshold"}, {"edge_detect_intensity", "Intensity"}, {"edge_detect_color", "Color"}},
                 "Outlines where the brightness changes (Sobel on luma): ink or toon outlines.",
                 [](PostProcessingSettings& s) { s.edgeDetectEnabled = true; }, nullptr},
                {"Sharpen", "sharpen_enabled", {{"sharpen_intensity", "Intensity"}},
                 "Crisper detail (shares FXAA's taps); pairs well with a render scale below 1.",
                 [](PostProcessingSettings& s) { s.sharpenEnabled = true; }, nullptr},
                {"VHS", "vhs_enabled",
                 {{"vhs_intensity", "Intensity"}, {"vhs_wobble", "Wobble"}, {"vhs_tracking", "Tracking"},
                  {"vhs_color_bleed", "Color Bleed"}, {"vhs_tape_noise", "Tape Noise"}, {"vhs_chroma_streaks", "Chroma Streaks"},
                  {"vhs_dropouts", "Dropouts"}, {"vhs_scanlines", "Scanlines"}, {"vhs_interference", "Interference"},
                  {"vhs_glitches", "Glitches"}, {"vhs_speed", "Speed"}},
                 "A played video tape: colour bleed and trails of its narrow chroma, line wobble, tracking bands, tape "
                 "noise, chroma streaks, dropouts, interference, glitches and scanlines. It replaces the scene tap (9 "
                 "taps), so FXAA and Sharpen do not run with it.",
                 [](PostProcessingSettings& s) { s.vhsEnabled = true; }, nullptr},
                {"Film Damage", "film_damage_enabled",
                 {{"film_damage_intensity", "Intensity"}, {"film_hair_amount", "Hairs"}, {"film_scratch_amount", "Scratches"},
                  {"film_dust_amount", "Dust"}, {"film_dust_size", "Dust Size"}, {"film_gate_weave", "Gate Weave"},
                  {"film_gate_mask", "Gate Mask"}, {"film_gate_mask_feather", "Gate Mask Feather"}, {"film_frame_rate", "Frame Rate"}},
                 "A projected film print: hairs, scratches and dust (up to 8 of each per film frame), the gate's weave "
                 "and its frame (Academy 1.37, widescreen 1.85, anamorphic 2.39 or rounded corners).",
                 [](PostProcessingSettings& s) { s.filmDamageEnabled = true; }, nullptr},
                {"Glitch", "glitch_enabled",
                 {{"glitch_intensity", "Intensity"}, {"glitch_rgb_split", "RGB Split"}, {"glitch_tears", "Tears"},
                  {"glitch_tear_shift", "Tear Shift"}, {"glitch_interlace", "Interlace"}, {"glitch_burst_rate", "Burst Rate"},
                  {"glitch_burst_length", "Burst Length"}, {"glitch_fps", "Frame Rate"}, {"glitch_seed", "Seed"}},
                 "Digital corruption in bursts: rows torn sideways, red and blue split apart and every other row dark, all "
                 "in whole source texels (art pixels with a Pixel Perfect Camera). Burst Rate 0 corrupts every frame at the "
                 "Intensity; Intensity 0 shows only scripted bursts (PixelFX.glitch). The schedule changes on Frame Rate "
                 "steps and depends only on the time and Seed. Two extra taps.",
                 [](PostProcessingSettings& s) { s.glitchEnabled = true; }, nullptr},
            }},
            {"Palette Quantize", {
                {"Palette Quantize", "palette_enabled",
                 {{"palette", "Palette"}, {"palette_dither", "Dither"}, {"palette_dither_strength", "Dither Strength"},
                  {"palette_distance", "Distance"}, {"palette_precision", "Precision"}},
                 "Every pixel of the frame onto a palette's colours (after the grade, edge and vignette, before CRT, film "
                 "damage and grain), with an ordered dither between a colour's two nearest palette colours per art pixel. "
                 "A colour already in the palette stays exactly as it is. The colour table is baked on the CPU in the "
                 "background whenever the palette or a value changes; the UI drawn after the scene is not quantized.",
                 [](PostProcessingSettings& s) { s.paletteEnabled = true; s.palette = "palette.palette"; }, nullptr},
            }},
            {"Bloom", {
                {"Bloom", "bloom_enabled",
                 {{"bloom_threshold", "Threshold"}, {"bloom_knee", "Knee"}, {"bloom_intensity", "Intensity"},
                  {"bloom_tint", "Tint"}, {"bloom_scatter", "Scatter"}},
                 "Glow around bright areas (above Threshold on the display colour), blurred at a quarter of the resolution. "
                 "LDR: the scene image is 8-bit, so a full-white surface above Threshold glows like a light; keep "
                 "Threshold high for lamps only.",
                 [](PostProcessingSettings& s) { s.bloomEnabled = true; }, nullptr},
            }},
            {"Blur", {
                {"Blur", "blur_enabled", {{"blur_radius", "Radius"}, {"blur_intensity", "Intensity"}},
                 "A blurred copy of the scene from the bloom chain: on screen by Intensity (0 keeps the image sharp and "
                 "only makes the texture, e.g. for frosted UI panels). Panels sample it in a layer scene (a HUD) drawn "
                 "after the post-processed scene; in that scene itself the texture is black.",
                 [](PostProcessingSettings& s) { s.blurEnabled = true; }, nullptr},
            }},
            {"Resolution", {
                {"Render Scale", nullptr, {{"render_scale", "Render Scale"}},
                 "Resolution of the 3D scene (0.5-1 of the output), upscaled by the uber pass: the biggest saving on "
                 "high-density phone screens. UI the main camera draws is part of its image and scales with it; UI drawn "
                 "by a layer scene stays at full resolution. Point sprites keep their size in pixels of the scaled "
                 "image (larger on screen), as with fixed resolution.",
                 [](PostProcessingSettings& s) { s.renderScale = 0.75f; }, nullptr},
                {"Dynamic Resolution", "dynamic_resolution_enabled",
                 {{"dynamic_resolution_min_scale", "Min Scale"}, {"dynamic_resolution_target_fps", "Target FPS"}},
                 "In Play and in the exported game, the render scale moves between Min Scale and Render Scale to hold the "
                 "Target FPS (0: 60): it drops quickly on missed frames and probes upward slowly. Without GPU timing it "
                 "reads the frame period, so it cannot help a CPU-bound game. Never in edit mode; off with Fixed Resolution. "
                 "Each change resizes the scene targets (a step of 0.05, at most twice a second).",
                 [](PostProcessingSettings& s) { s.dynamicResolutionEnabled = true; }, nullptr},
            }},
        };
        return table;
    }

    const Field& field(const char* snake) {
        const Field* found = PostProcessingSettings::findField(snake);
        // the group table names fields of SceneSettings.cpp's table; a rename shows here first
        assert(found);
        return *found;
    }

    template<typename T>
    T& fieldValue(PostProcessingSettings& settings, const Field& f) {
        return *(T*)f.ref(settings);
    }

    template<typename T>
    const T& fieldValue(const PostProcessingSettings& settings, const Field& f) {
        return *(const T*)settings.getFieldRef(f);
    }

    bool fieldIsDefault(const PostProcessingSettings& settings, const Field& f) {
        static const PostProcessingSettings defaults;
        switch (f.type) {
            case FT::Bool: return fieldValue<bool>(settings, f) == fieldValue<bool>(defaults, f);
            case FT::Int:
            case FT::Enum: return fieldValue<int>(settings, f) == fieldValue<int>(defaults, f);
            case FT::Float: return std::fabs(fieldValue<float>(settings, f) - fieldValue<float>(defaults, f)) <= 1e-5f;
            case FT::Vector3: return fieldValue<Vector3>(settings, f) == fieldValue<Vector3>(defaults, f);
            case FT::String: return fieldValue<std::string>(settings, f) == fieldValue<std::string>(defaults, f);
            case FT::Asset: return fieldValue<AssetRef>(settings, f).path == fieldValue<AssetRef>(defaults, f).path;
        }
        return true;
    }

    void resetField(PostProcessingSettings& settings, const Field& f) {
        static const PostProcessingSettings defaults;
        switch (f.type) {
            case FT::Bool: fieldValue<bool>(settings, f) = fieldValue<bool>(defaults, f); break;
            case FT::Int:
            case FT::Enum: fieldValue<int>(settings, f) = fieldValue<int>(defaults, f); break;
            case FT::Float: fieldValue<float>(settings, f) = fieldValue<float>(defaults, f); break;
            case FT::Vector3: fieldValue<Vector3>(settings, f) = fieldValue<Vector3>(defaults, f); break;
            case FT::String: fieldValue<std::string>(settings, f) = fieldValue<std::string>(defaults, f); break;
            case FT::Asset: fieldValue<AssetRef>(settings, f) = fieldValue<AssetRef>(defaults, f); break;
        }
    }

    // an effect is on: its flag, or any of its neutral values changed
    bool effectOn(const PostProcessingSettings& settings, const EffectDef& effect) {
        if (effect.enable) return fieldValue<bool>(settings, field(effect.enable));
        for (const ParamDef& param : effect.params) {
            if (!fieldIsDefault(settings, field(param.field))) return true;
        }
        return false;
    }

    void setEffect(PostProcessingSettings& settings, const EffectDef& effect, bool on) {
        if (effect.enable) {
            fieldValue<bool>(settings, field(effect.enable)) = on;
        } else if (!on) {
            for (const ParamDef& param : effect.params) resetField(settings, field(param.field));
        }
    }

    std::string tierText(const EffectDef& effect) {
        if (effect.tierText) return effect.tierText;
        PostProcessingSettings only;
        effect.isolate(only);
        return editor::getPostProcessingCostTierLabel(editor::getPostProcessingCostTier(only));
    }

    const char* tierTag(const EffectDef& effect) {
        if (effect.tierText) return "scene pass";
        PostProcessingSettings only;
        effect.isolate(only);
        switch (editor::getPostProcessingCostTier(only)) {
            case editor::PostProcessingCostTier::IN_SHADER: return "T0 in shader";
            case editor::PostProcessingCostTier::UBER: return "T1 uber pass";
            case editor::PostProcessingCostTier::CHAIN: return "T2 bloom chain";
            case editor::PostProcessingCostTier::NONE: break;
        }
        return "";
    }

    // Glitch's test row while playing: a scripted burst (PixelFX.glitch) on the running game,
    // never saved
    void drawGlitchBurstRow(editor::SceneProject* sceneProject) {
        if (!sceneProject || sceneProject->playState == editor::ScenePlayState::STOPPED)
            return;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Test");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(ICON_FA_BOLT " Burst now"))
            jasperengine::pixelart::Glitch::burst(0.3f, 1.0f);
        ImGui::SetItemTooltip("A 0.3 s burst at full intensity on the running game, as PixelFX.glitch(0.3, 1) (not saved)");
    }

    // "aces" -> "ACES", "neutral" -> "Neutral (Khronos PBR)"; other enums: capitalized
    std::string enumLabel(const Field& f, int index) {
        const std::string name = f.enumNames[index];
        if (std::strcmp(f.enumType, "TonemapMode") == 0) {
            if (name == "aces") return "ACES";
            if (name == "agx") return "AgX";
            if (name == "neutral") return "Neutral (Khronos PBR)";
        } else if (std::strcmp(f.enumType, "LutInputEncoding") == 0) {
            return editor::getLutInputEncodingName((LutInputEncoding)index);
        } else if (std::strcmp(f.enumType, "FilmGateMask") == 0) {
            if (name == "academy_137") return "Academy (1.37)";
            if (name == "widescreen_185") return "Widescreen (1.85)";
            if (name == "anamorphic_239") return "Anamorphic (2.39)";
        } else if (std::strcmp(f.enumType, "FilmGrainType") == 0) {
            if (name == "filmic") return "Filmic (desktop / high-end)";
        } else if (std::strcmp(f.enumType, "PaletteDither") == 0) {
            if (name == "bayer2") return "Bayer 2\xC3\x97" "2";
            if (name == "bayer4") return "Bayer 4\xC3\x97" "4";
            if (name == "bayer8") return "Bayer 8\xC3\x97" "8";
        } else if (std::strcmp(f.enumType, "PaletteDistance") == 0) {
            if (name == "oklab") return "OKLab";
            if (name == "rgb") return "RGB";
        } else if (std::strcmp(f.enumType, "PalettePrecision") == 0) {
            if (name == "normal") return "Normal (32\xC2\xB3)";
            if (name == "high") return "High (64\xC2\xB3)";
        }
        std::string label = name;
        if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
        return label;
    }

    void postProcessingWrappedTooltip(const std::string& text) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    JE_PRINTF_FORMAT(2, 0) std::string formatFloat(float value, const char* format = "%.2f") {
        char text[32];
        std::snprintf(text, sizeof(text), format, value);
        return text;
    }

    std::string passListText(const editor::PostProcessingCostSide& side) {
        std::string text;
        for (const editor::PostProcessingCostPass& pass : side.passes) {
            text += "\n- " + pass.label + (pass.count > 1 ? " x" + std::to_string(pass.count) : std::string()) +
                    ": " + formatFloat(pass.area) + " screens" + (pass.fullResolution ? " (full resolution)" : "");
        }
        return text.empty() ? std::string("\n(no pass)") : text;
    }

    const float LABEL_INDENT_EM = 1.0f;

}

// One field of PostProcessingSettings as a row: label (with the reset arrow when away from the
// default) and an editor picked from the field's type.
void editor::InspectorWindow::drawPostProcessingFieldRow(SceneProject* sceneProject, const PostProcessingSettings& settings,
                                                         const PostProcessingSettings::Field& f, const char* label,
                                                         const std::string& help, bool indent){
    const uint32_t sceneId = sceneProject->id;
    const std::string property = std::string("post_processing.") + f.snakeName;
    const std::string id = "##pp_" + std::string(f.snakeName);

    // rows that apply only with some values, and rows inactive because of another effect
    const PostProcessingRowState state = getPostProcessingRowState(
        settings, f.snakeName, Catalog::getSceneProperty<bool>(sceneProject->scene, "fixed_resolution_enabled"));
    if (!state.shown)
        return;

    ImGui::PushID(f.snakeName);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (indent) ImGui::Indent(ImGui::GetFontSize() * LABEL_INDENT_EM);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered())
        postProcessingWrappedTooltip(std::string(f.help) + (help.empty() ? std::string() : "\n\n" + help) +
                                     (state.inactiveReason.empty() ? std::string() : "\n\n" + state.inactiveReason));
    if (!fieldIsDefault(settings, f)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##reset")) {
            PostProcessingSettings reset = settings;
            resetField(reset, f);
            CommandHandle::get(sceneId)->addCommandNoMerge(
                new ScenePropertyCmd<PostProcessingSettings>(project, sceneId, "post_processing", reset));
        }
        ImGui::SetItemTooltip("Reset to the default");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }
    if (indent) ImGui::Unindent(ImGui::GetFontSize() * LABEL_INDENT_EM);

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    // still editable (the value is kept), greyed with the reason
    if (!state.inactiveReason.empty())
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ImGui::GetStyle().DisabledAlpha);

    // a changed value: one command, merged while the widget is held (cmd: the inspector's
    // pending command, closed below when the widget is released)
    auto commit = [&](Command* newCmd, bool merge) {
        if (merge) {
            cmd = newCmd;
            CommandHandle::get(sceneId)->addCommand(newCmd);
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(newCmd);
        }
    };

    switch (f.type) {
        case FT::Bool: {
            bool value = fieldValue<bool>(settings, f);
            if (ImGui::Checkbox(id.c_str(), &value))
                commit(new ScenePropertyCmd<bool>(project, sceneId, property, value), false);
            break;
        }
        case FT::Int: {
            int value = fieldValue<int>(settings, f);
            // the fields with a fixed set of values: a combo
            std::vector<std::pair<int, const char*>> choices;
            if (std::strcmp(f.name, "lutSize") == 0) choices = {{16, "16 (low end)"}, {32, "32"}};
            else if (std::strcmp(f.name, "msaaSamples") == 0) choices = {{1, "Off"}, {2, "2x"}, {4, "4x"}};
            if (!choices.empty()) {
                std::string previewText = std::to_string(value);
                for (const auto& choice : choices) if (choice.first == value) previewText = choice.second;
                if (ImGui::BeginCombo(id.c_str(), previewText.c_str())) {
                    for (const auto& choice : choices) {
                        if (ImGui::Selectable(choice.second, choice.first == value) && choice.first != value)
                            commit(new ScenePropertyCmd<int>(project, sceneId, property, choice.first), false);
                    }
                    ImGui::EndCombo();
                }
            } else if (ImGui::DragInt(id.c_str(), &value, 1.0f, (int)f.minValue, (int)f.maxValue)) {
                commit(new ScenePropertyCmd<int>(project, sceneId, property, std::clamp(value, (int)f.minValue, (int)f.maxValue)), true);
            }
            break;
        }
        case FT::Float: {
            float value = fieldValue<float>(settings, f);
            if (UIUtils::sliderFloatInput(id.c_str(), &value, f.minValue, f.maxValue)) {
                value = std::clamp(value, f.minValue, f.maxValue);
                commit(new ScenePropertyCmd<float>(project, sceneId, property, value), true);
            }
            break;
        }
        case FT::Vector3: {
            Vector3 value = fieldValue<Vector3>(settings, f);
            const std::string snake = f.snakeName;
            const bool colour = snake.find("color") != std::string::npos || snake.find("tint") != std::string::npos;
            bool changed;
            if (colour) {
                ui::ColorFieldOptions colorOptions;
                colorOptions.alpha = false;
                colorOptions.linearStorage = false;
                colorOptions.label = label;
                const ui::EditPhase phase = ui::colorField(id.c_str(), value, colorOptions);
                if (phase == ui::EditPhase::Begin) ui::ColorPickerWindow::beginEdit(CommandHandle::get(sceneId), label);
                changed = phase == ui::EditPhase::Changing;
            } else {
                changed = ImGui::DragFloat3(id.c_str(), (float*)&value.x, 0.005f, f.minValue, f.maxValue, "%.3f");
            }
            if (changed) {
                value = Vector3(std::clamp(value.x, f.minValue, f.maxValue), std::clamp(value.y, f.minValue, f.maxValue),
                                std::clamp(value.z, f.minValue, f.maxValue));
                commit(new ScenePropertyCmd<Vector3>(project, sceneId, property, value), true);
            }
            break;
        }
        case FT::Enum: {
            const int value = fieldValue<int>(settings, f);
            if (ImGui::BeginCombo(id.c_str(), enumLabel(f, value).c_str())) {
                for (int i = 0; f.enumNames[i]; i++) {
                    if (ImGui::Selectable(enumLabel(f, i).c_str(), i == value) && i != value)
                        commit(new ScenePropertyCmd<int>(project, sceneId, property, i), false);
                }
                ImGui::EndCombo();
            }
            if (std::strcmp(f.name, "palettePrecision") == 0 && settings.palettePrecision == PalettePrecision::HIGH)
                ImGui::TextDisabled("Slower to bake: for palettes of many close shades");
            if (std::strcmp(f.name, "lutInputEncoding") == 0) {
                // what the .cube's encoding resolves to (Auto: the proposal from its title and range)
                const std::shared_ptr<const CubeLut> lut = getCachedColorLut(settings.lutTexture);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", describeLutInputEncoding(settings.lutInputEncoding, lut.get(),
                                                                   getCachedColorLutError(settings.lutTexture)).c_str());
                ImGui::PopTextWrapPos();
            }
            break;
        }
        case FT::String:
        case FT::Asset: {
            if (std::strcmp(f.name, "palette") == 0) {
                drawPostProcessingPaletteField(sceneProject, settings);
                break;
            }
            // a project image (the LUT strip): drop one, pick one, or clear. An Asset field is set
            // by its path (ScenePropertyCmd<std::string>), which drops a GUID it no longer names
            const std::string current = f.type == FT::Asset ? fieldValue<AssetRef>(settings, f).path
                                                            : fieldValue<std::string>(settings, f);
            auto assign = [&](const std::string& absolutePath) {
                if (!project->isInsideAssetsPath(absolutePath)) {
                    Backend::getApp().registerOutsideAssetsAlert(absolutePath);
                    return;
                }
                commit(new ScenePropertyCmd<std::string>(project, sceneId, property,
                                                         project->normalizeToAssetsRelative(absolutePath).generic_string()), false);
            };
            const float iconWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float buttons = (iconWidth + ImGui::GetStyle().ItemSpacing.x) * (current.empty() ? 1.0f : 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                                                 : ImGui::GetStyle().Colors[ImGuiCol_Text]);
            const std::string shown = current.empty() ? std::string("None") : std::filesystem::path(current).filename().string();
            ImGui::Button((shown + "##slot").c_str(), ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttons), 0));
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%sDrop an image strip or a .cube file here", current.c_str(), current.empty() ? "" : "\n");
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                    std::vector<std::string> received = editor::Util::getStringsFromPayload(payload);
                    if (!received.empty() && (editor::Util::isImageFile(received[0]) || editor::Util::isColorLutFile(received[0])))
                        assign(received[0]);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pick")) {
                const std::string selected = FileDialogs::openFileDialog(project->getAssetsPath().string(),
                                                                         FILE_DIALOG_IMAGE | FILE_DIALOG_COLOR_LUT);
                if (!selected.empty())
                    assign(selected);
            }
            ImGui::SetItemTooltip("Pick an image strip or a .cube LUT");
            if (!current.empty()) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_XMARK "##clear"))
                    commit(new ScenePropertyCmd<std::string>(project, sceneId, property, std::string()), false);
                ImGui::SetItemTooltip("No image");
            }
            break;
        }
    }
    if (!state.inactiveReason.empty()) {
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            postProcessingWrappedTooltip(state.inactiveReason);
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
        cmd->setNoMerge();
        cmd = nullptr;
    }
    ImGui::PopID();
}

void editor::InspectorWindow::applyPostProcessingPreset(SceneProject* sceneProject, const PostProcessingPreset& preset){
    const uint32_t sceneId = sceneProject->id;
    PostProcessingSettings settings;
    bool wroteCopy = false;
    std::string error;
    if (!resolvePostProcessingPreset(*project, preset, settings, wroteCopy, error)) {
        Out::warning("The %s preset's palette: %s", preset.name.c_str(), error.c_str());
        return;
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(makePostProcessingCommand(project, sceneId, settings, "Apply " + preset.name + " Preset"));
    // the built-in palette's copy and the preset: one undo step
    if (wroteCopy)
        editor::pixelart::linkPaletteCopy(*project, sceneId);
}

void editor::InspectorWindow::drawPostProcessingPaletteField(SceneProject* sceneProject, const PostProcessingSettings& settings){
    const uint32_t sceneId = sceneProject->id;
    const std::string current = settings.palette.path;
    std::string picked;
    if (paletteAssetField("##pp_palette", *project, current, picked, true)) {
        editor::pixelart::ResolvedPalette resolved;
        bool wroteCopy = false;
        std::string error;
        if (picked.empty()) {
            CommandHandle::get(sceneId)->addCommandNoMerge(
                new ScenePropertyCmd<std::string>(project, sceneId, "post_processing.palette", std::string()));
        } else if (editor::pixelart::usePalette(*project, picked, resolved, wroteCopy, error)) {
            // set by its path (ScenePropertyCmd<std::string>), which drops a GUID it no longer names
            CommandHandle::get(sceneId)->addCommandNoMerge(
                new ScenePropertyCmd<std::string>(project, sceneId, "post_processing.palette", resolved.assetPath));
            if (wroteCopy)
                editor::pixelart::linkPaletteCopy(*project, sceneId);
        } else {
            Out::warning("Palette Quantize: %s", error.c_str());
        }
    }

    ImGui::PushTextWrapPos(0.0f);
    if (current.empty()) {
        if (settings.paletteEnabled)
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " No palette: Palette Quantize stays off until one is set");
    } else if (std::shared_ptr<const jasperengine::pixelart::Palette> palette = jasperengine::pixelart::PaletteLibrary::get(current)) {
        paletteSwatchStrip("##pp_palette_swatches", palette->colors, -1.0f, ImGui::GetFontSize() * 0.8f, 64);
        // the renderer bakes the colour table in the background; the previous one draws meanwhile
        std::shared_ptr<RenderSystem> render = sceneProject->scene->getSystem<RenderSystem>();
        if (render && render->isPaletteQuantizeBaking())
            ImGui::TextDisabled(ICON_FA_SPINNER " Baking...");
    } else {
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s is not a readable palette", current.c_str());
    }
    ImGui::PopTextWrapPos();
}

void editor::InspectorWindow::drawPostProcessingCost(SceneProject* sceneProject, const PostProcessingSettings& settings){
    const std::vector<PostProcessPass> passes = Catalog::getSceneProperty<std::vector<PostProcessPass>>(sceneProject->scene, "post_process");
    const bool fixedResolution = Catalog::getSceneProperty<bool>(sceneProject->scene, "fixed_resolution_enabled");
    const PostProcessingCost cost = estimatePostProcessingCost(settings, passes, fixedResolution);

    const bool open = ImGui::TreeNodeEx("##pp_cost", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                        ICON_FA_GAUGE_HIGH " Estimated cost");
    ImGui::SetItemTooltip("An estimate from the research cost model (passes counted from the renderers' source, never "
                          "measured on a device): render passes after the scene and the pixels they shade, in screens "
                          "(1 = one full-resolution pass), for these settings here and in Unity URP 14 with the same effects.");
    if (!open)
        return;

    if (ImGui::BeginTable("pp_cost_table", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, getLabelSize("Full resolution", false));
        ImGui::TableSetupColumn("This engine", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Unity URP", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto sideTooltip = [](const char* who, const PostProcessingCostSide& side) {
            if (ImGui::IsItemHovered())
                postProcessingWrappedTooltip(std::string(who) + ":" + passListText(side));
        };
        auto row = [&](const char* label, const std::string& engineText, const std::string& urpText) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(engineText.c_str());
            sideTooltip("This engine (estimate)", cost.engine);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", urpText.c_str());
            sideTooltip("Unity URP 14 (estimate)", cost.urp);
        };
        row("Passes", std::to_string(cost.engine.passCount), std::to_string(cost.urp.passCount));
        row("Full resolution", std::to_string(cost.engine.fullResolutionPasses), std::to_string(cost.urp.fullResolutionPasses));
        row("Screens shaded", formatFloat(cost.engine.fullscreen), formatFloat(cost.urp.fullscreen));
        ImGui::EndTable();
    }

    ImGui::PushTextWrapPos(0.0f);
    for (const std::string& note : cost.notes)
        ImGui::TextDisabled("%s", note.c_str());
    ImGui::PopTextWrapPos();

    // while the game runs: what the engine applies (its tiers), the cheap part of a measurement;
    // the pass list itself is in the log with JASPERENGINE_READY_SIGNAL=1 JASPERENGINE_FRAME_STATS=all
    const bool running = sceneProject->playState == ScenePlayState::PLAYING || sceneProject->playState == ScenePlayState::PAUSED;
    if (running) {
        if (RenderSystem* render = sceneProject->scene->getSystem<RenderSystem>().get()) {
            const PostProcessingTiers& tiers = render->getActivePostProcessingTiers();
            std::string applied = tiers.tailColor ? "in-shader colour" : "";
            if (tiers.uber) {
                const std::string features = describeUberFeatures(tiers.uberFeatures);
                applied += (applied.empty() ? "" : ", ") + std::string("uber pass") + (features.empty() ? "" : " (" + features + ")");
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_PLAY " Applied now: %s", applied.empty() ? "nothing" : applied.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SetItemTooltip("What the running game's renderer applies. Start the editor or the exported game with "
                                  "JASPERENGINE_READY_SIGNAL=1 and JASPERENGINE_FRAME_STATS=all to log the measured pass "
                                  "list of every frame once loaded (=1: three frames); in the editor each extra view "
                                  "(the Scene view while playing) logs its own line.");
            // the scale dynamic resolution applies and what it reads (S13 R16)
            std::vector<std::string> warnings;
            const std::string dynamic = describeDynamicResolution(render->getDynamicResolutionStatus(), warnings);
            if (!dynamic.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_PLAY " %s", dynamic.c_str());
                for (const std::string& warning : warnings)
                    ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", warning.c_str());
                ImGui::PopTextWrapPos();
                ImGui::SetItemTooltip("GPU: the GPU frame time drives it. Frame law (no GPU timing on this backend): it drops on "
                                      "missed frames and probes one step up after a while without one.");
            }
        }
    }

    ImGui::TreePop();
}

void editor::InspectorWindow::drawScenePostProcessing(SceneProject* sceneProject){
    const uint32_t sceneId = sceneProject->id;
    const PostProcessingSettings settings = Catalog::getSceneProperty<PostProcessingSettings>(sceneProject->scene, "post_processing");

    // the whole struct in one undo step, named for the history ("Apply Film Preset (Main)")
    auto setAll = [&](const PostProcessingSettings& value, const std::string& label) {
        CommandHandle::get(sceneId)->addCommandNoMerge(makePostProcessingCommand(project, sceneId, value, label));
    };

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " Only the scene's main camera is post-processed: other cameras (render to "
                        "texture, mirrors), layer scenes (HUD) and the Scene view are not.");
    ImGui::PopTextWrapPos();

    // ---- presets: the whole struct in one step
    {
        const PostProcessingPreset* matched = matchPostProcessingPreset(settings, project);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preset");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##pp_preset", matched ? matched->name.c_str() : "Custom")) {
            for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
                if (ImGui::Selectable(preset.name.c_str(), matched == &preset) && matched != &preset)
                    applyPostProcessingPreset(sceneProject, preset);
                if (ImGui::IsItemHovered()) {
                    const PostProcessingCost cost = estimatePostProcessingCost(preset.settings, {}, false);
                    postProcessingWrappedTooltip(preset.description + "\n\nEstimate: " + std::to_string(cost.engine.passCount) + " passes, " +
                                   formatFloat(cost.engine.fullscreen) + " screens shaded (Unity URP: " +
                                   std::to_string(cost.urp.passCount) + " passes, " + formatFloat(cost.urp.fullscreen) + ").");
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Replace every post-processing value with a preset (one undo step)");
    }

    ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
    const float labelWidth = getLabelSize("Chromatic Aberration") + ImGui::GetFontSize() * LABEL_INDENT_EM;
    std::set<std::string> drawn;

    for (const GroupDef& group : groups()) {
        ImGui::PushID(group.name);

        // the group's checkbox: on when any of its effects is, mixed when only some are;
        // off turns them all off, on turns on the ones with an enable flag
        int flagged = 0;
        int on = 0;
        int flaggedOn = 0;
        for (const EffectDef& effect : group.effects) {
            const bool isOn = effectOn(settings, effect);
            on += isOn ? 1 : 0;
            if (effect.enable) {
                flagged++;
                flaggedOn += isOn ? 1 : 0;
            }
        }
        ImGui::Spacing();
        if (flagged > 0) {
            bool groupOn = on > 0;
            const bool mixed = on > 0 && flaggedOn < flagged;
            if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("##group", &groupOn)) {
                PostProcessingSettings changed = settings;
                for (const EffectDef& effect : group.effects)
                    setEffect(changed, effect, on == 0);
                setAll(changed, std::string(on == 0 ? "Turn On " : "Turn Off ") + group.name);
            }
            if (mixed) ImGui::PopItemFlag();
            // a group of one effect (Bloom, Blur): its checkbox is the effect's, with its help
            const bool single = group.effects.size() == 1;
            if (ImGui::IsItemHovered()) {
                if (single) postProcessingWrappedTooltip(std::string(group.effects[0].help) + "\n\nCost: " + tierText(group.effects[0]));
                else ImGui::SetTooltip(on > 0 ? "Turn every %s effect off" : "Turn every %s effect on", group.name);
            }
            ImGui::SameLine();
        }
        if (group.effects.size() == 1 && group.effects[0].enable) {
            // the tier next to the name: the group has no effect row of its own
            ImGui::SeparatorText((std::string(group.name) + "  (" + tierTag(group.effects[0]) + ")").c_str());
        } else {
            ImGui::SeparatorText(group.name);
        }

        if (ImGui::BeginTable("pp_group", 2, tableFlags)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            for (const EffectDef& effect : group.effects) {
                ImGui::PushID(effect.name);
                const std::string help = std::string(effect.help) + "\n\nCost: " + tierText(effect);
                const bool isOn = effectOn(settings, effect);

                if (effect.enable && group.effects.size() == 1) {
                    // the group's checkbox above is this effect's: only its parameters here
                    drawn.insert(effect.enable);
                    for (const ParamDef& param : effect.params) {
                        drawn.insert(param.field);
                        if (isOn)
                            drawPostProcessingFieldRow(sceneProject, settings, field(param.field), param.label, std::string(), false);
                    }
                } else if (effect.enable) {
                    const Field& enable = field(effect.enable);
                    drawn.insert(effect.enable);
                    // the effect's header row: its checkbox and name, its cost tier on the right
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool value = isOn;
                    if (ImGui::Checkbox(effect.name, &value)) {
                        CommandHandle::get(sceneId)->addCommandNoMerge(new ScenePropertyCmd<bool>(
                            project, sceneId, std::string("post_processing.") + enable.snakeName, value));
                    }
                    if (ImGui::IsItemHovered())
                        postProcessingWrappedTooltip(help);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::AlignTextToFramePadding();
                    // an effect another one turns off (FXAA and Sharpen with VHS, Dynamic Resolution
                    // with Fixed Resolution) says so instead of its tier
                    const PostProcessingRowState state = getPostProcessingRowState(
                        settings, enable.snakeName, Catalog::getSceneProperty<bool>(sceneProject->scene, "fixed_resolution_enabled"));
                    if (state.inactiveReason.empty()) {
                        ImGui::TextDisabled("%s", tierTag(effect));
                        if (ImGui::IsItemHovered())
                            postProcessingWrappedTooltip(help);
                    } else {
                        ImGui::TextColored(Theme::Colors::WarningText, "%s", state.inactiveTag.c_str());
                        if (ImGui::IsItemHovered())
                            postProcessingWrappedTooltip(state.inactiveReason);
                    }
                    if (isOn) {
                        for (const ParamDef& param : effect.params) {
                            drawn.insert(param.field);
                            drawPostProcessingFieldRow(sceneProject, settings, field(param.field), param.label, std::string(), true);
                        }
                        if (std::strcmp(effect.enable, "glitch_enabled") == 0)
                            drawGlitchBurstRow(sceneProject);
                    } else {
                        for (const ParamDef& param : effect.params) drawn.insert(param.field);
                    }
                } else {
                    // neutral values: always shown, the help names the tier
                    for (const ParamDef& param : effect.params) {
                        drawn.insert(param.field);
                        drawPostProcessingFieldRow(sceneProject, settings, field(param.field), param.label, help, false);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    // fields of the table not placed in a group above (added later): generic rows
    bool otherOpen = false;
    for (const Field& f : PostProcessingSettings::getFields()) {
        if (drawn.count(f.snakeName))
            continue;
        if (!otherOpen) {
            ImGui::Spacing();
            ImGui::SeparatorText("Other");
            otherOpen = ImGui::BeginTable("pp_other", 2, tableFlags);
            if (!otherOpen) break;
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        }
        drawPostProcessingFieldRow(sceneProject, settings, f, f.name, std::string(), false);
    }
    if (otherOpen) ImGui::EndTable();

    ImGui::Spacing();
    drawPostProcessingCost(sceneProject, settings);
}

namespace {

    // The user pass's own fields beyond shader, enabled and uniforms (phase 4 section 2.5: a
    // resolution scale, the original scene colour as an input, custom textures). One entry per
    // PostProcessPass member: a field added to the struct becomes a row by adding it here (the
    // MCP post_process JSON and the scene YAML carry it on their side).
    struct PassFieldDef {
        enum class Kind { Bool, Float, Texture };
        const char* label;
        const char* help;
        Kind kind;
        float minValue;
        float maxValue;
        void* (*ref)(PostProcessPass& pass);
    };

    const std::vector<PassFieldDef>& passFields() {
        // (the original scene colour is a shader input, u_originalSceneTexture: no field; the
        // textures are rows by uniform, drawPostProcessPassFields)
        static const std::vector<PassFieldDef> table = {
            {"Scale", "The pass's output resolution, a fraction of the chain's: the next pass (or a copy to the "
                      "destination, after the last) samples it bilinearly. Below 1 costs less.",
             PassFieldDef::Kind::Float, 0.1f, 1.0f, [](PostProcessPass& pass) -> void* { return &pass.scale; }},
        };
        return table;
    }

}

void editor::InspectorWindow::drawPostProcessPassFields(SceneProject* sceneProject, const std::vector<PostProcessPass>& passes, size_t index){
    const uint32_t sceneId = sceneProject->id;
    for (const PassFieldDef& def : passFields()) {
        ImGui::PushID(def.label);
        std::vector<PostProcessPass> changed = passes;
        void* ref = def.ref(changed[index]);
        bool edited = false;
        bool merge = false;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(def.label);
        if (ImGui::IsItemHovered())
            postProcessingWrappedTooltip(def.help);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        switch (def.kind) {
            case PassFieldDef::Kind::Bool:
                edited = ImGui::Checkbox("##value", (bool*)ref);
                break;
            case PassFieldDef::Kind::Float:
                edited = UIUtils::sliderFloatInput("##value", (float*)ref, def.minValue, def.maxValue);
                if (edited) *(float*)ref = std::clamp(*(float*)ref, def.minValue, def.maxValue);
                merge = true;
                break;
            case PassFieldDef::Kind::Texture: {
                std::string& path = *(std::string*)ref;
                const std::string shown = path.empty() ? std::string("None") : std::filesystem::path(path).filename().string();
                ImGui::Button((shown + "##slot").c_str(), ImVec2(-1, 0));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                        std::vector<std::string> received = editor::Util::getStringsFromPayload(payload);
                        if (!received.empty() && editor::Util::isImageFile(received[0]) && project->isInsideAssetsPath(received[0])) {
                            path = project->normalizeToAssetsRelative(received[0]).generic_string();
                            edited = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                break;
            }
        }
        if (edited) {
            Command* passCmd = new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", changed);
            if (merge) {
                cmd = passCmd;
                CommandHandle::get(sceneId)->addCommand(passCmd);
            } else {
                CommandHandle::get(sceneId)->addCommandNoMerge(passCmd);
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
        ImGui::PopID();
    }

    // the pass's textures: the ones its shader's manifest declares (@texture), and any set by name
    // (Lua, MCP; a fork without a manifest); drop an image on the slot, right-click clears it
    const PostProcessPass& pass = passes[index];
    std::vector<std::pair<std::string, std::string>> slots; // uniform, label
    if (std::shared_ptr<const ShaderManifest> manifest = pass.shader.empty() ? nullptr : ShaderManifests::get(pass.shader)) {
        for (const ShaderManifestTexture& texture : manifest->textures)
            slots.push_back({texture.uniform, texture.label.empty() ? texture.uniform : texture.label});
    }
    for (const auto& entry : pass.textures) {
        if (std::none_of(slots.begin(), slots.end(), [&](const auto& slot){ return slot.first == entry.first; }))
            slots.push_back({entry.first, entry.first});
    }
    for (const auto& slot : slots) {
        ImGui::PushID(slot.first.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(slot.second.c_str());
        if (ImGui::IsItemHovered())
            postProcessingWrappedTooltip("Texture " + slot.first + " of the pass's shader (unset: its default)");
        ImGui::TableSetColumnIndex(1);
        const std::string path = pass.getTexture(slot.first);
        const std::string shown = path.empty() ? std::string("None") : std::filesystem::path(path).filename().string();
        ImGui::Button((shown + "##slot").c_str(), ImVec2(-1, 0));
        std::string newPath;
        bool edited = false;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                std::vector<std::string> received = editor::Util::getStringsFromPayload(payload);
                if (!received.empty() && editor::Util::isImageFile(received[0]) && project->isInsideAssetsPath(received[0])) {
                    newPath = project->normalizeToAssetsRelative(received[0]).generic_string();
                    edited = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (!path.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            edited = true;
        }
        if (edited) {
            std::vector<PostProcessPass> changed = passes;
            changed[index].setTexture(slot.first, newPath);
            CommandHandle::get(sceneId)->addCommandNoMerge(new ScenePropertyCmd<std::vector<PostProcessPass>>(project, sceneId, "post_process", changed));
        }
        ImGui::PopID();
    }
}
