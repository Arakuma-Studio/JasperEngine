// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Pixel Art preset (docs/specs/S42-pixel-art-mode.md R2, R18): the planned list of changes that
// converts a project, each with its current and new value, applied as one undo step ("Apply Pixel
// Art Preset"). Its project part is pixel_art_mode's fix; the cameras, textures and references
// are integer_scale's, mipmaps_compression's and filtering's fixes on the project as Pixel Art
// Mode sees it, so the preview and check_pixel_art {fix: ["all"]} plan the same changes.

#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtFix.h"

#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    struct PixelArtPresetOptions {
        int baseWidth = 320, baseHeight = 180;
        Scaling scaling = Scaling::INTEGER;      // INTEGER or INTEGER_EXPAND
        int pixelsPerUnit = 1;                   // 1...256
        int windowScale = 4;                     // 2...8
        bool addCameras = true;                  // a Pixel Perfect Camera on every 2D scene's main camera
        bool convertTextures = true;             // pixelArt: true on sprite/UI and drawn textures
        bool pointReferences = true;             // those references Nearest
    };

    // the base resolutions R2 offers (the dialog's and the hub's list): 256x144 ... 640x360
    const std::vector<PixelArtScreen>& presetBaseResolutions();

    // the plan over an input read with Scope::assumeModeOn (deterministic: by kind, then path)
    std::vector<FixChange> planPixelArtPreset(const PixelArtCheckInput& input, const PixelArtPresetOptions& options);
    // the same from the project (main thread)
    std::vector<FixChange> planPixelArtPreset(Project& project, const PixelArtPresetOptions& options);
    // what a plan touches, for the dialog's count
    struct PresetCounts {
        int settings = 0, scenes = 0, textures = 0, references = 0;
    };
    PresetCounts countPresetChanges(const std::vector<FixChange>& plan);

    // applies the plan as one undo step named "Apply Pixel Art Preset"
    bool applyPixelArtPreset(Project& project, const std::vector<FixChange>& plan, FixApplyReport& report, std::string& error);

}
