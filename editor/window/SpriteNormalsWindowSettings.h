// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the Sprite Normal Map window remembers per project and per user (workspace.yaml key
// spriteNormals, Stream::encodeSpriteNormalsSettings / decodeSpriteNormalsSettings): the last
// settings used (the preset and every override, regions included: what an Inspector Shift-click
// generates with), the preview's view and light, and the opt-in auto-regenerate of the
// SpriteNormalsWatcher.

#include "spritenormals/SpriteNormals.h"

#include "math/Vector3.h"

namespace jasperengine::editor {

    struct SpriteNormalsWindowSettings {
        // the shared settings of the window, as last left
        spritenormals::Settings settings = spritenormals::presetSettings(spritenormals::Preset::Default);
        // SpriteNormalsWindow::View: 0 Color, 1 Normal, 2 Lit, 3 Split
        int view = 2;
        // the preview light: 0 = the source's default height (0.6 x the larger cell side)
        float lightHeight = 0.0f;
        Vector3 lightColour = Vector3(1.0f, 1.0f, 1.0f);
        float ambient = 0.25f;
        bool orbit = false;
        bool grid = true;
        bool advancedOpen = false;
        // regenerate the project's normal maps when their sources change (not undoable)
        bool autoRegenerate = false;
    };

}
