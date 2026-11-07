// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R7, §6.8)
// - Purpose: the presets of the effect and generator types, editor data files
//   editor/resources/titleart/fx-presets/<type>.yaml embedded in the editor at configure time
//   (title_fx_presets.h, editor/titleart/TitleArtEditorSources.cmake):
//   {presets: [{id, name, aliases, params, cost?, layer?}]}. Effect presets set params only;
//   generator presets may also set the generator layer's blend, opacity, effects and place.
//   add_effect {preset}, apply_recipe, list_title_library and the Add menus read them here.
// - Thread-safety: the embedded files are parsed once, on first use, under a lock; the results
//   are immutable.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleFxPreset {
        std::string id;                     // snake_case, unique per type
        std::string name;                   // "Light Guide (導光板)"
        std::vector<std::string> aliases;
        titleart::Json params = titleart::Json::object();   // over the type's defaults
        std::string cost;                   // "" or the class it is heavier at ("C4")
        titleart::Json layer = nullptr;     // generators: {blend, opacity, place, effects}
    };

    class TitleFxPresets {
    public:
        // a type's presets in file order (none: empty)
        static const std::vector<TitleFxPreset>& forType(const std::string& type);
        static const TitleFxPreset* find(const std::string& type, const std::string& id);
        // every type with a preset file
        static std::vector<std::string> types();
        // one file's text (tests and the embedded files): false with `error`
        static bool parse(const std::string& text, std::vector<TitleFxPreset>& out, std::string& error);
    };

}
