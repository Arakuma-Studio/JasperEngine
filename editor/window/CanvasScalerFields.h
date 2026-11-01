// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "util/CanvasScaler.h"

#include <string>

namespace jasperengine::editor {

    // Unity's CanvasScaler Inspector as rows of the caller's two-column table (label, value):
    // UI Scale Mode, then only the fields of that mode. True when a value changed this frame;
    // editFinished (optional) is set when an edit ended (a drag released, a combo picked), so an
    // undo command stops merging. The Project Settings (Graphics > UI Scale) and the scene's
    // Canvas Scaler draw it.
    bool drawCanvasScalerFields(const char* id, CanvasScalerSettings& settings, bool* editFinished = nullptr);

    // "At 2400x1080: UI 2400x1080 at scale 1.00" for a view of viewWidth x viewHeight showing a
    // game canvas of canvasWidth x canvasHeight (the Game view's resolution and its dpi)
    std::string canvasScalerPreview(const CanvasScalerSettings& settings, float viewWidth, float viewHeight,
                                    float canvasWidth, float canvasHeight, float dpi);

    // the mode's Inspector label ("Scale With Screen Size")
    const char* canvasScalerModeLabel(UIScaleMode mode);

}
