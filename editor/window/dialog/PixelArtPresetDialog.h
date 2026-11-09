// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Project Settings > Pixel Art > Apply Pixel Art Preset... (docs/specs/S42-pixel-art-mode.md R2,
// R18): the preset's options as fields and checkboxes, every planned change as a row (current ->
// new), the count of settings, scenes, textures and references touched, and Apply (one undo step)
// or Cancel.

#include "pixelart/PixelArtPreset.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class PixelArtPresetDialog {
    public:
        static constexpr const char* POPUP_NAME = "Apply Pixel Art Preset###PixelArtPreset";

        explicit PixelArtPresetDialog(Project* project): project(project) {}

        // opens it (the plan is made on the next draw)
        void open();
        // between the Project Settings window's Begin and End
        void draw();
        bool isOpen() const { return visible; }
        // the preset was applied since the last call
        bool takeApplied() { const bool was = applied; applied = false; return was; }

    private:
        Project* project;
        bool requested = false;
        bool visible = false;
        bool dirty = true;
        bool applied = false;
        int baseIndex = 1;          // 320 x 180
        int customWidth = 320, customHeight = 180;
        int scalingIndex = 0;       // integer, integer_expand
        int pixelsPerUnit = 1;
        int windowScale = 4;
        bool addCameras = true;
        bool convertTextures = true;
        bool pointReferences = true;
        std::vector<pixelart::FixChange> plan;
        std::string result;

        pixelart::PixelArtPresetOptions options() const;
    };

}
