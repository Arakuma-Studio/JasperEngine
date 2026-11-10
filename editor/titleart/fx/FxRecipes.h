// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R8, R28, D4, D5, §6.8, §6.10, §6.11)
// - Purpose: the Add Effect entries that are not types of their own. Recipes are presets of other
//   types (Long Shadow is Extrude, Emboss and Deboss are Material, Icicles are Drips, CRT is
//   Scanlines, Comic Dots and Ben-Day Dots are Halftone) or settings computed for S80's Gradient
//   Overlay (Shine, Rainbow, Vignette: Pachislop recipes.ts glintOverlay, rainbowOverlay); style
//   settings switch a layer's style member on (Dry Brush (かすれ) is style.erode, Glint (3D
//   highlights) S86's style_3d.glint). apply_recipe (TitleArtOps' reserved S83 op) adds a recipe's
//   effect in one undo step named after it; list_title_library lists both kinds with the types.
// - Thread-safety: the tables are immutable; registerTitleFxOps runs on the main thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleFxRecipe {
        const char* id;             // snake_case: apply_recipe's `recipe`
        const char* name;           // the menu entry ("Long Shadow")
        const char* category;       // its Add Effect category
        const char* type;           // the effect it adds
        const char* preset;         // the preset it applies (null: params computed here)
        std::vector<const char*> aliases;
    };
    const std::vector<TitleFxRecipe>& titleFxRecipes();
    const TitleFxRecipe* findTitleFxRecipe(const std::string& id);
    // the params a recipe's effect takes over the type's defaults
    titleart::Json titleFxRecipeParams(const TitleFxRecipe& recipe);

    struct TitleFxStyleSetting {
        const char* id;             // "dry_brush"
        const char* name;           // "Dry Brush (かすれ)"
        const char* category;
        const char* member;         // the style member it switches on ("style.erode.enabled")
        const char* layers;         // which layers have it ("text, shape, vector and image layers")
        std::vector<const char*> aliases;
    };
    const std::vector<TitleFxStyleSetting>& titleFxStyleSettings();

    // apply_recipe {layer?, part?, recipe}: registered into TitleArtOps
    void registerTitleFxOps();

    // add_effect's `preset`: the preset's params over `params` (the type's defaults); false with a
    // message naming the type's presets when it has no such preset
    bool applyTitleFxPreset(const std::string& type, const std::string& preset, titleart::Json& params, std::string& error,
                            std::vector<std::string>& valid);

}
