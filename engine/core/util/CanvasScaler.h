// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef CANVASSCALER_H
#define CANVASSCALER_H

#include "Export.h"
#include "math/Vector2.h"

namespace jasperengine {

    // Unity's CanvasScaler.ScaleMode, plus the engine's default: the UI lays out in the game
    // canvas (Engine.scalingMode), as before the Canvas Scaler existed
    enum class UIScaleMode {
        FollowGameCanvas,
        ConstantPixelSize,
        ScaleWithScreenSize,
        ConstantPhysicalSize
    };

    // CanvasScaler.ScreenMatchMode
    enum class ScreenMatchMode {
        MatchWidthOrHeight,
        Expand,
        Shrink
    };

    // CanvasScaler.Unit
    enum class PhysicalUnit {
        Centimeters,
        Millimeters,
        Inches,
        Points,
        Picas
    };

    // The UI's own coordinate space: what the native UI of a UI scene and the screen HTML UI
    // documents of any scene lay out in (project default, a scene can override it). The game
    // world keeps Engine.scalingMode.
    struct JASPERENGINE_API CanvasScalerSettings {
        UIScaleMode mode = UIScaleMode::FollowGameCanvas;
        float scaleFactor = 1.0f;                           // ConstantPixelSize
        Vector2 referenceResolution = Vector2(1920, 1080);  // ScaleWithScreenSize
        ScreenMatchMode screenMatchMode = ScreenMatchMode::MatchWidthOrHeight;
        float matchWidthOrHeight = 0.0f;                    // 0 width .. 1 height
        PhysicalUnit physicalUnit = PhysicalUnit::Points;   // ConstantPhysicalSize
        float fallbackScreenDPI = 96.0f;

        bool operator==(const CanvasScalerSettings& other) const;
        bool operator!=(const CanvasScalerSettings& other) const { return !(*this == other); }
    };

    // The UI size in UI units and the view pixels per UI unit (x; FollowGameCanvas under a
    // STRETCH game canvas has another factor in y, scaleFactorY)
    struct JASPERENGINE_API CanvasScalerResult {
        float uiWidth = 0.0f;
        float uiHeight = 0.0f;
        float scaleFactor = 1.0f;
        float scaleFactorY = 1.0f;
        // false: FollowGameCanvas (the UI is the game canvas, nothing changes) or nothing computed yet
        bool scaled = false;
    };

    namespace CanvasScaler {

        // Unity's CanvasScaler formulas. viewWidth/Height: the game's view rect in physical
        // pixels (Engine::getViewRect); gameCanvas: Engine canvas; screenDpi 0 = unknown (the
        // fallback DPI then). Invalid inputs (a size or a factor <= 0) return `last`.
        JASPERENGINE_API CanvasScalerResult compute(const CanvasScalerSettings& settings, float viewWidth, float viewHeight,
                                                    float gameCanvasWidth, float gameCanvasHeight, float screenDpi,
                                                    const CanvasScalerResult& last = CanvasScalerResult());

        // "follow_game_canvas", "constant_pixel_size", "scale_with_screen_size", "constant_physical_size"
        // (project.yaml, scene files, MCP); parse returns false for an unknown name
        JASPERENGINE_API const char* modeName(UIScaleMode mode);
        JASPERENGINE_API bool parseMode(const char* name, UIScaleMode& mode);
        // "match_width_or_height", "expand", "shrink"
        JASPERENGINE_API const char* matchModeName(ScreenMatchMode mode);
        JASPERENGINE_API bool parseMatchMode(const char* name, ScreenMatchMode& mode);
        // "centimeters", "millimeters", "inches", "points", "picas"
        JASPERENGINE_API const char* unitName(PhysicalUnit unit);
        JASPERENGINE_API bool parseUnit(const char* name, PhysicalUnit& unit);

    }

}

#endif // CANVASSCALER_H
