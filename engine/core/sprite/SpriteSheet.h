// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A sprite sheet: pages (textures) and frames cut from them, plus the tags, slices and user data
// an Aseprite file gives (docs/specs/S44-aseprite-import.md R45, §6.11). The one runtime record
// every sheet source produces: an imported .aseprite file (S44), a plain texture (one frame), and
// S45's texture sprite sheets and Sprite Atlases. SpriteSheetIO reads and writes it as
// `jasperengine.spritesheet` v1; SpriteSheetPool shares one per path.
//
// Coordinates are pixels, origin top-left, y down (Aseprite's and PNG's); sprites convert once
// per frame (SpriteFrameGeometry). Every value is an integer. A frame name is unique in its sheet.
// Thread-safety: a plain value; a sheet the pool hands out is immutable (shared_ptr<const>).

#ifndef SPRITE_SHEET_H
#define SPRITE_SHEET_H

#include "Export.h"
#include "asset/Guid.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"

#include "json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine{

    struct JASPERENGINE_API SpriteFrame{
        std::string name;                 // display name ("hero 3", "Body/3"), unique in the sheet
        std::string key;                  // the sub-asset key ("frame#3", "frame:Body/3"); S45's texture sheets:
                                          // "rect:<x>,<y>,<w>,<h>", the frame's place in its texture; "" for others
        uint16_t page = 0;
        Rect rect;                        // the frame's pixels in its page (trimmed)
        Vector2 sourceSize;               // the untrimmed frame
        Vector2 offset;                   // where `rect` sits inside the source frame
        Vector2 pivot;                    // in source pixels, from the top-left
        Vector4 border;                   // 9-slice: left, top, right, bottom pixels
        uint32_t durationMs = 100;
    };

    struct JASPERENGINE_API SpritePage{
        std::string texture;              // asset-relative ("je-imported/3f/3fa1.../page0.png")
        int width = 0, height = 0;
    };

    enum class SpriteTagDirection{
        FORWARD,
        REVERSE,
        PING_PONG,
        PING_PONG_REVERSE
    };

    // Aseprite's 16-bit repeat count, and the longest playback a tag unrolls into
    constexpr int SPRITE_TAG_MAX_REPEAT = 65535;
    constexpr size_t SPRITE_TAG_MAX_PLAYBACK = 1u << 20;

    struct JASPERENGINE_API SpriteTag{
        std::string name;
        int from = 0, to = 0;             // frame indices, inclusive
        SpriteTagDirection direction = SpriteTagDirection::FORWARD;
        int repeat = 0;                   // 0: unspecified (loops), N: N times (ping-pong counts directions)
        Vector4 color = Vector4(0.0f, 0.0f, 0.0f, 1.0f);   // 0..1 RGBA
        nlohmann::json userData;          // {text?, color?, properties?, extensions?}; null when none
        // the frames one pass plays, direction applied (a one-frame tag: one entry)
        std::vector<int> sequence() const;
        // the whole playback with `repeat` unrolled (repeat 0: one pass), capped at
        // SPRITE_TAG_MAX_PLAYBACK frames (fewer passes)
        std::vector<int> playback() const;
    };

    struct JASPERENGINE_API SpriteSliceKey{
        int frame = 0;                    // the key holds from this frame until the next key
        Rect bounds;                      // w or h 0: hidden from this frame on
        bool hasCenter = false;
        Rect center;                      // 9-patch centre, relative to bounds
        bool hasPivot = false;
        Vector2 pivot;                    // relative to the bounds' origin

        bool hidden() const { return bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f; }
    };

    struct JASPERENGINE_API SpriteSlice{
        std::string name;
        std::vector<SpriteSliceKey> keys; // ascending frames
        nlohmann::json userData;

        // the key in force on a frame; nullptr before the first key or where the slice is hidden
        const SpriteSliceKey* keyAt(int frame) const;
    };

    struct JASPERENGINE_API SpriteSheetData{
        std::string source;               // the producing asset ("sprites/hero.aseprite"), asset-relative
        Guid sourceGuid;
        float pixelsPerUnit = 1.0f;
        Vector2 pixelRatio = Vector2(1.0f, 1.0f);
        std::vector<SpritePage> pages;
        std::vector<SpriteFrame> frames;
        std::vector<SpriteTag> tags;
        std::vector<SpriteSlice> slices;
        nlohmann::json userData;          // the sprite's (Aseprite file) user data; null when none

        int findFrame(std::string_view name) const;   // -1 when absent
        int findTag(std::string_view name) const;
        int findSlice(std::string_view name) const;
        int frameCount() const { return static_cast<int>(frames.size()); }
    };

    JASPERENGINE_API const char* spriteTagDirectionName(SpriteTagDirection direction);   // "forward", "reverse", "pingpong", "pingpong_reverse"
    JASPERENGINE_API bool spriteTagDirectionFromName(std::string_view name, SpriteTagDirection& out);

}

#endif // SPRITE_SHEET_H
