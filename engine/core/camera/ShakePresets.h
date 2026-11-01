// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 6, "Built-in presets")
// - Purpose: the presets compiled into the engine, by name (CameraShake.getPresetNames()): the
//   13 MilkShake presets of the ported game (Assets/MilkShake/Demo/Presets, the same numbers) and
//   three of EZ Camera Shake's (CameraShakePresets.cs, amplitude converted: strength = magnitude/2).
// - A name never has the .shake extension; names are lower snake_case. Immutable: any thread.

#ifndef SHAKE_PRESETS_H
#define SHAKE_PRESETS_H

#include "Export.h"
#include "camera/ShakePreset.h"

#include <string>
#include <vector>

namespace jasperengine::camera{

    struct JASPERENGINE_API BuiltinShakePreset{
        const char* name;
        const char* description;     // one line for the editor and the docs
        ShakePreset preset;
    };

    // in the documented order (the game's first)
    JASPERENGINE_API const std::vector<BuiltinShakePreset>& builtinShakePresets();
    // nullptr when `name` is not a built-in
    JASPERENGINE_API const ShakePreset* findBuiltinShakePreset(const std::string& name);
    JASPERENGINE_API std::vector<std::string> builtinShakePresetNames();
    // the preset Assets > Create > Shake Preset writes (explosion's values)
    JASPERENGINE_API const char* defaultNewShakePresetName();

}

#endif //SHAKE_PRESETS_H
