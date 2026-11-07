// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R7, §6.8, §6.11)
// - Purpose: the presets of the effect and generator definitions, read from their files
//   (editor/resources/titleart/fx-presets/<type>.yaml, embedded in the editor at build time:
//   FxPresets.cmake): `{presets: [{id, name, aliases, source?, class?, params, layer?: {blend,
//   opacity, place, effects}}]}`. Effect presets set params only; generator presets may also set
//   the generator layer's blend, opacity, its own effects and its place relative to the followed
//   layer (Pachislop's GeneratorPreset). `source` names the Pachislop preset a ported one comes from
//   ("<pachislopType>/<presetId>": the id map and the parity tests read it); `class` states a
//   preset one cost class heavier than its definition (R34).
// - A definition's file is the file name of its EffectDef / GeneratorDef presetFile; the first
//   preset is what adding the type without one uses.
// - Thread-safety: parsed once on first use (main thread), read-only afterwards.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    struct TitleFxPreset {
        std::string id;
        std::string name;
        std::vector<std::string> aliases;
        std::string source;                 // "<pachislopType>/<presetId>", "" for a new preset
        std::string costClass;              // "" = the definition's class
        titleart::Json params = titleart::Json::object();
        titleart::Json layer = titleart::Json::object();   // generators: blend, opacity, place, effects
    };

    // the presets of the definition `type` (an effect or a generator), file order; empty when it has none
    const std::vector<TitleFxPreset>& titleFxPresets(std::string_view type);
    const TitleFxPreset* findTitleFxPreset(std::string_view type, std::string_view id);
    std::vector<std::string> titleFxPresetIds(std::string_view type);
    // a preset file's problems (unknown params, values outside their ranges, duplicate ids),
    // for the catalogue checks; empty when every preset file is clean
    std::vector<std::string> checkTitleFxPresets();

}
