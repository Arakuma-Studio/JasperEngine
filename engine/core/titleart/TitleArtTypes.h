// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.1, §6.5, §6.15)
// - Purpose: the classifications the Title Art program shares between the document's member
//   schema (TitleArtSchema) and the effect/generator registry (TitleEffectRegistry), so one
//   vocabulary describes a document member and an effect parameter: unit, animation class
//   (S85), interpolation, combination, glyph safety (S81's Glyph Atlas), and the content hash a
//   member feeds (TitleArtHash, the renderer's caches).
// - Thread-safety: plain enums and pure name functions.

#pragma once

#include "Export.h"

#include <cstdint>
#include <string_view>

namespace jasperengine::titleart {

    // The unit of a number. Percent: of the layer's source `size` (a style dimension transfers
    // across text sizes); Px: canvas pixels, multiplied by the render's px_scale (effect,
    // generator and mask distances); Deg: degrees (clockwise on screen from +x, except light
    // directions, which keep Photoshop's counter-clockwise "comes from"); Factor: a multiplier
    enum class ParamUnit : uint8_t { None, Px, Percent, Deg, Factor };

    // The animation class of a member or a parameter (S85 R11): Live changes without a re-render
    // when the definition has a live variant (transforms, opacity); Rerender is a smooth
    // re-render; Step a stepped re-render (enums, bools, assets); Relayout re-runs layout and
    // analysis (text, size, warp); None is not animatable (ids, structure, outputs)
    enum class FxAnim : uint8_t { None, Live, Rerender, Step, Relayout };
    // how keys interpolate (a seed or an enum is Stepped)
    enum class ParamInterp : uint8_t { Smooth, Stepped };
    // S85's relative tracks: intensities multiply, the rest replace
    enum class ParamCombine : uint8_t { Replace, Multiply };
    // S81 R31's Glyph Atlas classes: Safe renders per glyph exactly as merged; Approximate
    // differs where glyphs meet (shared glows, plates); Unsafe needs the whole text
    enum class GlyphSafety : uint8_t { Safe, Approximate, Unsafe };

    // The content hash a document member feeds (TitleArtHash, §6.3's cascade): a member changes
    // its scope's hash and every later one of the cascade Geometry -> Mask -> Field -> Stack ->
    // Node, so a colour (Stack) never recomputes masks or distance fields and a position
    // (Placement) only re-places the cached stack. Effects and Masks feed the node hash beside
    // the stack. Identity members (ids, names, editor-only flags) feed no hash: rendering never
    // depends on them. Inherit takes the scope of the member that holds the object.
    enum class HashScope : uint8_t { Inherit, Identity, Geometry, Mask, Field, Stack, Effects, Masks, Placement, Node };

    // A rectangle in document space (canvas pixels, origin at the centre, +y up): (x, y) is its
    // lower-left corner
    struct TitleRect {
        double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
        bool empty() const { return width <= 0.0 || height <= 0.0; }
    };
    // A rectangle in image space (pixels, origin top-left, +y down): frames, regions, crops
    struct TitleRectI {
        int x = 0, y = 0, width = 0, height = 0;
        bool empty() const { return width <= 0 || height <= 0; }
        friend bool operator==(const TitleRectI& a, const TitleRectI& b) {
            return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
        }
    };

    JASPERENGINE_API const char* paramUnitName(ParamUnit unit);          // "", "px", "%", "deg", "x"
    JASPERENGINE_API const char* fxAnimName(FxAnim anim);                // "none", "live", "rerender", "step", "relayout"
    JASPERENGINE_API bool fxAnimFromName(std::string_view name, FxAnim& out);
    JASPERENGINE_API const char* glyphSafetyName(GlyphSafety safety);    // "safe", "approximate", "unsafe"
    JASPERENGINE_API const char* hashScopeName(HashScope scope);

}
