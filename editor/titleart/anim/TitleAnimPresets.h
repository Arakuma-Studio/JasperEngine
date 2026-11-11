// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R21-R23, D13, §6.4, §6.8)
// - Purpose: the Animation Presets library: the built-in presets
//   (editor/resources/titleart/animation-presets/*.json, embedded), the user's
//   (<config dir>/title-library/animation-presets/) and the project's
//   (<project>/EditorSettings/TitleLibrary/AnimationPresets/), format
//   `jasperengine.titleanimpreset` v1; apply_animation_preset (ordinary tracks and markers written
//   into an animation, each tagged `from_preset`); Save Animation as Preset.
// - A preset is data with expressions (D13): times, values and settings may be tinyexpr text over
//   D (the duration), n (the main text's units), fps, S (the main text's size), W and H (the
//   canvas), its `vars` and its parameters (bools 0/1, an enum `p` gives p_<option> 0/1); "$p"
//   is a parameter's value as written (colours, enums, sounds). The only code are the key
//   generators: shake, flicker, beats, ticks (seeded from the document seed and the preset's).
// - Targets: title, main_text (the first target of the exposed `text`, else the topmost text
//   layer), text_layers, layers (the op's `layers`), generator:<type>, add:<id> (what `adds`
//   creates when missing). A part whose S83 type is not registered is skipped with
//   `preset_part_unavailable`.
// - Thread-safety: main thread.

#pragma once

#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtOps.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    constexpr const char* TITLE_ANIM_PRESET_FORMAT = "jasperengine.titleanimpreset";
    constexpr int TITLE_ANIM_PRESET_VERSION = 1;

    struct TitleAnimPresetParam {
        std::string name;               // a tinyexpr identifier
        std::string label;
        std::string type;               // number, bool, enum, color, sound
        TitleJson defaultValue;
        double min = 0.0, max = 0.0;    // number: min == max unbounded
        std::vector<std::string> options;   // enum
    };

    struct TitleAnimPreset {
        std::string id, name, group, role, description;
        std::string scope = "builtin";  // builtin, user, project
        std::string file;               // where it was read
        TitleJson data;                 // the whole preset (tracks, markers, adds, vars, duration, wrap...)
        std::vector<TitleAnimPresetParam> params;
        double defaultDuration = 1.0;   // seconds, -1 automatic
        std::string wrap = "once";
        bool overlay = false;
    };

    class TitleAnimPresets {
    public:
        // built-in, then project, then user (a later scope's id replaces an earlier one)
        static const std::vector<TitleAnimPreset>& all();
        static const TitleAnimPreset* find(const std::string& id);
        static std::vector<std::string> ids();
        // the user and project folders changed (Save as Preset, a project switch): read again
        static void reload();
        // the open project's folder (the tools and the Title Designer set it: ops have no project)
        static void setProjectDir(const std::string& dir);
        // tests: the user folder (else <config dir>/title-library/animation-presets)
        static void setUserDirForTesting(const std::string& dir);
        // one preset file's text; false with `error`
        static bool parse(const std::string& text, TitleAnimPreset& out, std::string& error);
        // where a scope's presets live ("user" or "project")
        static std::string folderOf(const std::string& scope);
    };

    // apply_animation_preset {animation, preset, params?, layers?, at?, mode?}
    bool applyTitleAnimationPresetOp(TitleOpContext& ctx, const TitleJson& op);
    void registerTitleAnimPresetOp();

    // R23: an animation as a preset: absolute tracks relative to their rests (or refused with a
    // reason), times relative to D, targets title / main_text / names. False with `error`.
    bool titleAnimationToPreset(const titleart::TitleArtDoc& doc, const titleanim::TitleAnimation& animation, const std::string& id,
                                const std::string& name, TitleJson& out, std::string& error);
    // writes it into the scope's folder as <id>.json; false with `error`
    bool saveTitleAnimPreset(const TitleJson& preset, const std::string& scope, std::string& path, std::string& error);

    // the main text layer a preset targets (first exposed `text` target, else the topmost text layer): "" none
    std::string mainTextLayerOf(const titleart::TitleArtDoc& doc);

}
