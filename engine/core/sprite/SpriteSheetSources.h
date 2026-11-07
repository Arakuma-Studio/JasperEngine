// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S45's sources of SpriteSheetPool (docs/specs/S45-sprite-atlas.md R2, R8, R10, R11, §6.3):
// - a .spriteatlas: its built sheet ("<atlas>#sheet" through ImportedAssets, pages "#image#<n>");
// - a texture an atlas in use packs, or with a Sprite Mode (grid, multiple): "<texture>#sheet"
//   through ImportedAssets (the editor writes it from the texture's import settings, an export
//   ships it), which for a packed texture is its view into the atlas pages (same names, order,
//   sizes, offsets and pivots);
// - a "name@WxH" texture without one: a grid of W x H cells, row-major, empty cells skipped,
//   read from the image itself (a texture loaded from outside a project).
// Every other texture stays the pool's built-in single frame. The texture sheet itself is a pure
// function of the settings and the image (buildTextureSheet), shared with the editor's builder.
// Thread-safety: as SpriteSheetPool (any thread); the inline remap cache takes its own mutex.

#ifndef SPRITE_SHEET_SOURCES_H
#define SPRITE_SHEET_SOURCES_H

#include "Export.h"
#include "component/SpriteComponent.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "sprite/SpriteSheet.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    enum class TextureSpriteMode{
        SINGLE,
        GRID,
        MULTIPLE
    };

    // Grid mode: cells of cellWidth x cellHeight from (offsetX, offsetY), spacingX/Y apart; count
    // 0: every cell that fits; skipEmpty leaves out cells whose pixels are all transparent
    struct JASPERENGINE_API SpriteGridSettings{
        int cellWidth = 0, cellHeight = 0;
        int offsetX = 0, offsetY = 0;
        int spacingX = 0, spacingY = 0;
        int count = 0;
        bool skipEmpty = true;
        std::string prefix;               // frame names: <prefix><i>
    };

    struct JASPERENGINE_API TextureSheetFrame{
        std::string name;
        Rect rect;                        // pixels, top-left origin
        bool hasPivot = false;
        Vector2 pivot;                    // pixels from the frame's top-left
        Vector4 border;                   // left, top, right, bottom pixels
    };

    // A texture's `import.spriteSheet` (§6.4)
    struct JASPERENGINE_API TextureSheetSettings{
        static constexpr int kVersion = 1;
        TextureSpriteMode mode = TextureSpriteMode::SINGLE;
        SpriteGridSettings grid;
        std::vector<TextureSheetFrame> frames;   // multiple
        // the pivot of frames without their own: a preset, or normalized from the bottom-left
        PivotPreset pivotPreset = PivotPreset::CENTER;
        bool customPivot = false;
        Vector2 pivotNormalized = Vector2(0.5f, 0.5f);

        bool operator==(const TextureSheetSettings& other) const;
    };

    class JASPERENGINE_API SpriteSheetSources{
    public:
        // registers the sources with SpriteSheetPool once (the pool calls it before its first load)
        static void ensureRegistered();

        // ".spriteatlas" (any case)
        static bool isAtlasPath(const std::string& path);
        // a texture extension the pool reads
        static bool isTexturePath(const std::string& path);
        // "items/coin@16x16.png": 16, 16 and base "coin"; false without the suffix or with a 0 size
        static bool parseCellSize(const std::string& path, int& cellWidth, int& cellHeight, std::string* base = nullptr);
        // what a "name@WxH" file name means: a grid of those cells, empty ones skipped, "<base>_<i>"
        static TextureSheetSettings cellSizeSettings(int cellWidth, int cellHeight, const std::string& base);

        // true when any pixel of the rect (image pixels, clipped) has alpha > 0
        using OpaqueProbe = std::function<bool(int x, int y, int width, int height)>;
        // The sheet of a texture of width x height with these settings: frames on the texture
        // itself (one page, no trim), pivots resolved, each frame's key "rect:<x>,<y>,<w>,<h>" (its
        // place in the texture). Single: one frame, the whole image. Warnings name what was left
        // out (a cell past the image, a frame outside it, a duplicate name, leftover pixels).
        static SpriteSheetData buildTextureSheet(const std::string& texture, int width, int height, const TextureSheetSettings& settings,
                                                 const OpaqueProbe& opaque, std::vector<std::string>* warnings = nullptr);
        // a frame's place in its texture (its "rect:" key), false for a frame without one
        static bool frameTextureRect(const SpriteFrame& frame, Rect& out);
        static std::string frameTextureKey(const Rect& rect);

        // S45 R8: a legacy inline rect (pixels of `texture`) as drawn from the atlas page that packs
        // the texture: the frame holding it, translated, clipped to the member's trim with the
        // clipped part an offset (sourceSize = the rect's size, pivot its centre). False when no
        // atlas in use packs the texture or no packed frame holds the rect. sourceWidth/Height: the
        // texture's size.
        static bool mapInlineRect(const std::string& texture, const Rect& rect, SpriteFrame& out, int* sourceWidth = nullptr,
                                  int* sourceHeight = nullptr);
        // an atlas in use packs the texture ("<texture>#atlas" resolves)
        static bool isPacked(const std::string& texture);
        // the texture's size from its cook or its header; false when it cannot be read
        static bool textureSize(const std::string& texture, int& width, int& height);
    };

}

#endif // SPRITE_SHEET_SOURCES_H
