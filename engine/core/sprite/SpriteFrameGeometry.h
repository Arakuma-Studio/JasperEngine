// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The quad of one sheet frame (docs/specs/S44-aseprite-import.md R49): a frame is drawn at its
// source size, its trimmed pixels placed at their trim offset, so a trimmed and an untrimmed
// packing of the same frame cover the same pixels at the same place. Pure (CPU, any thread).
// - Units: source pixels times width/height / native size; width = height = 0 means the native
//   size (source size x pixel ratio / pixels per unit: S41's PPU, 1 until it lands).
// - Pivots: PivotPreset::FRAME is the frame's own pivot (source pixels from the top-left); the
//   nine presets keep their meaning, relative to the source size (createSprite's).
// - Orientation: the quad's corners follow createSprite's vertex order (v0 = (x0, y0), v1 =
//   (x1, y0), v2 = (x1, y1), v3 = (x0, y1)); a world camera is y up, a UI camera y down. A
//   sprite whose flipY mirrors the image (automaticFlipY off, flipY set against its camera's
//   convention) mirrors the trimmed area within the source frame too.
// - textureRect: the frame's rect in its page, normalized (y down, as framesRect rects are).
// - Sliced (docs/specs/S45-sprite-atlas.md R6): a bordered frame as a 4 x 4 vertex lattice, its
//   corners at their native size (scaled down only when the sprite is smaller than its two
//   borders), edges and centre stretched to the sprite's width x height; a trimmed frame keeps its
//   trim (its lattice lines clamped to the trimmed rect).

#ifndef SPRITE_FRAME_GEOMETRY_H
#define SPRITE_FRAME_GEOMETRY_H

#include "Export.h"
#include "component/SpriteComponent.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "sprite/SpriteSheet.h"

#include <cstdint>
#include <string_view>

namespace jasperengine{

    struct JASPERENGINE_API SpriteFrameQuad{
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        Rect textureRect = Rect(0.0f, 0.0f, 0.0f, 0.0f);
        // the frame's drawn size (the source size in units): sprites with width/height 0 report it
        float width = 0, height = 0;
        // a frame with no pixels (fully transparent when trimmed): nothing to draw
        bool empty = true;

        // the same vertices (a frame swap to such a frame only changes the texture rect)
        bool sameGeometry(const SpriteFrameQuad& other) const{
            return x0 == other.x0 && y0 == other.y0 && x1 == other.x1 && y1 == other.y1 && empty == other.empty;
        }
    };

    struct JASPERENGINE_API SpriteFrameGeometryInput{
        unsigned int width = 0, height = 0;     // the sprite's size in units, 0 = native
        PivotPreset pivot = PivotPreset::FRAME;
        float pixelsPerUnit = 1.0f;
        Vector2 pixelRatio = Vector2(1.0f, 1.0f);
        bool uiCamera = false;
        bool flipY = true;                      // the sprite's UV flip (createSprite's)
        int pageWidth = 0, pageHeight = 0;
    };

    JASPERENGINE_API SpriteFrameQuad spriteFrameQuad(const SpriteFrame& frame, const SpriteFrameGeometryInput& input);

    // a rect in the frame's source pixels (y down: a slice's box) in the sprite's local units,
    // placed as the frame's pixels are drawn: x, y the corner with the lowest coordinates
    JASPERENGINE_API Rect spriteFrameLocalRect(const SpriteFrame& frame, const SpriteFrameGeometryInput& input, const Rect& pixels);

    // a sheet sprite's slice on its shown frame in local units (Sprite:getSlice, slice colliders):
    // its box, centre (zero without one) and pivot (the box's corner without one); false when the
    // sheet has no such slice or it is hidden on that frame
    JASPERENGINE_API bool spriteSliceLocalRect(const SpriteComponent& sprite, const SpriteSheetData& sheet, bool uiCamera,
                                               std::string_view name, Rect& bounds, Rect* center = nullptr, Vector2* pivot = nullptr);

    // The 9-slice of a bordered frame: 16 vertices, vertex (row r, column c) at (x[c], y[r]) with the
    // frame-rect-local UV (u[c], v[r]) (v down the image, as the texture rect is); rows go up in y
    // and columns right in x, so cells wind as the quad's
    struct JASPERENGINE_API SpriteSlicedMesh{
        float x[4] = {0, 0, 0, 0};
        float y[4] = {0, 0, 0, 0};
        float u[4] = {0, 0, 0, 0};
        float v[4] = {0, 0, 0, 0};
        Rect textureRect = Rect(0.0f, 0.0f, 0.0f, 0.0f);
        float width = 0, height = 0;   // the drawn size in units
        bool empty = true;
    };

    // a border wide enough to slice (any side > 0)
    JASPERENGINE_API bool spriteFrameHasBorder(const SpriteFrame& frame);

    JASPERENGINE_API SpriteSlicedMesh spriteFrameSliced(const SpriteFrame& frame, const SpriteFrameGeometryInput& input);

    // the 54 indices of the 9-slice's 18 triangles (rows of 4 vertices, bottom row first)
    JASPERENGINE_API const uint16_t* spriteSlicedIndices();
    constexpr unsigned int SPRITE_SLICED_VERTICES = 16;
    constexpr unsigned int SPRITE_SLICED_INDICES = 54;

    // a preset's pivot in source pixels, y up from the bottom-left (createSprite's pivotPos);
    // FRAME: the frame's pivot converted to y up
    JASPERENGINE_API Vector2 spriteFramePivot(const SpriteFrame& frame, PivotPreset preset);

}

#endif // SPRITE_FRAME_GEOMETRY_H
