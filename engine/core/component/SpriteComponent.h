// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_COMPONENT_H
#define SPRITE_COMPONENT_H

#include "util/SpriteFrameData.h"
#include "util/GrowArray.h"
#include "asset/AssetRef.h"
#include "math/Rect.h"
#include "Engine.h"
#include "pixelart/PixelPerfectLayout.h"
#include "sprite/PivotPreset.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct SpriteSheetData;   // sprite/SpriteSheet.h

    // the point a sprite sorts by along the Transparency Sort Axis (spec S52 R4, Unity's Sprite
    // Sort Point): its world bounds' centre, or its pivot
    enum class SpriteSortPoint{
        CENTER,
        PIVOT
    };

    // How a frame fills the sprite (docs/specs/S45-sprite-atlas.md R6): Simple stretches the frame
    // over width x height; Sliced draws it as 9 quads from its border, corners unscaled, edges and
    // centre stretched. A frame without a border draws Simple.
    enum class SpriteDrawMode{
        SIMPLE,
        SLICED
    };

    // A legacy sprite drawn from the atlas page that packs its texture (S45 R8): the saved texture
    // and frame rect stay the source's, the mesh shows the page. Replaced whole, never changed in
    // place, so copies of a component may share it.
    struct SpriteInlineAtlas{
        std::string source;               // the texture the sprite saves (asset path)
        Rect sourceRect;                  // the frame shown, normalized in the source texture
        std::string page;                 // the page the mesh draws
        Rect pageRect;                    // the texture rect set on the mesh (a different one: someone else changed it)
        int sourceWidth = 0, sourceHeight = 0;
        uint64_t revision = 0;            // SpriteSheetPool::revision() it was mapped at
        // page "": the texture is packed but this rect is not inside one packed frame, so the
        // sprite draws from its own texture until the rect changes
        mutable std::atomic<uint32_t> refs{0};
    };

    // The component's handle on its SpriteInlineAtlas: one pointer (a shared_ptr is two), counted
    // in the object, so copies of a component share it
    class SpriteInlineAtlasRef{
    public:
        SpriteInlineAtlasRef() = default;
        // takes a new object (refs 0)
        explicit SpriteInlineAtlasRef(SpriteInlineAtlas* atlas): p(atlas){ retain(); }
        SpriteInlineAtlasRef(const SpriteInlineAtlasRef& o): p(o.p){ retain(); }
        SpriteInlineAtlasRef(SpriteInlineAtlasRef&& o) noexcept: p(o.p){ o.p = nullptr; }
        SpriteInlineAtlasRef& operator=(const SpriteInlineAtlasRef& o){
            if (o.p != p){ o.retain(); release(); p = o.p; }
            return *this;
        }
        SpriteInlineAtlasRef& operator=(SpriteInlineAtlasRef&& o) noexcept{
            if (&o != this){ release(); p = o.p; o.p = nullptr; }
            return *this;
        }
        ~SpriteInlineAtlasRef(){ release(); }
        void reset(){ release(); p = nullptr; }
        const SpriteInlineAtlas* get() const { return p; }
        const SpriteInlineAtlas* operator->() const { return p; }
        explicit operator bool() const { return p != nullptr; }

    private:
        const SpriteInlineAtlas* p = nullptr;
        void retain() const { if (p) p->refs.fetch_add(1, std::memory_order_relaxed); }
        void release(){
            if (p && p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete p;
        }
    };

    struct JASPERENGINE_API SpriteComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        unsigned int width = 0;
        unsigned int height = 0;

        bool automaticFlipY = true;
        bool flipY = false;

        float textureScaleFactor = 0.0;

        unsigned int numFramesRect = 0;
        GrowArray<SpriteFrameData> framesRect;
        // what its arrays hold outside the component (ComponentArray::arrayHeapBytes)
        size_t arrayHeapBytes() const { return framesRect.storageBytes(); }

        PivotPreset pivotPreset = PivotPreset::BOTTOM_LEFT;

        // S41: texels per world unit; 0 = the project's Pixels Per Unit (Engine::getPixelsPerUnit).
        // width and height stay in pixels; the quad is their size divided by it (spriteWorldSize)
        float pixelsPerUnit = 0.0f;
        // under a Pixel Perfect Camera: the camera's rotation policy, or one of its own
        PixelRotation pixelRotation = PixelRotation::CAMERA;
        // S52 R2: what Custom Axis sorting measures (beside pixelRotation, in its padding)
        SpriteSortPoint spriteSortPoint = SpriteSortPoint::CENTER;

        // Frame chosen before the texture size was known, in pixels
        Rect pendingFrameRect;
        bool needUpdateFrameRect = false;

        bool needUpdateSprite = true;
        // S45 R8, runtime: the pool revision (low 32 bits) a legacy sprite last looked for an atlas
        // at (in the bytes before `sheet`)
        uint32_t inlineAtlasChecked = 0;
        // S48 R7, runtime: the texels the built quad grew by for the material's Pixel FX (left,
        // bottom, right, top, a byte each; MeshSystem's @expand), in the padding before `sheet`
        uint32_t builtFxExpand = 0;
        // MeshComponent::customUniformWrites at the last compare of the expand
        uint32_t fxWritesSeen = 0;

        // A sheet sprite (docs/specs/S44-aseprite-import.md R48): its frames come from a sprite
        // sheet (an .aseprite file's import, a texture; S45: atlases and sliced textures), the
        // inline frames and the mesh texture are not used and not saved. "" = a legacy sprite.
        AssetRef sheet;
        int frame = 0;                    // index into the sheet's frames
        // S45 R6: written only when not Simple (beside `frame`: the 4 bytes before frameName)
        SpriteDrawMode drawMode = SpriteDrawMode::SIMPLE;
        std::string frameName;            // the frame's name when saved: found again by name on load

        // runtime, not saved
        std::shared_ptr<const SpriteSheetData> sheetData;
        std::string loadedSheet;          // the `sheet` path sheetData was loaded for
        uint64_t sheetGeneration = 0;     // SpriteSheetPool's generation of sheetData
        uint64_t sheetRevision = 0;       // the pool revision last checked (a cheap per-frame "anything new?")
        int builtFrame = -1;              // the frame whose geometry the buffer holds
        bool resolveFrameName = true;     // find `frame` by `frameName` on the next sheet load
        bool builtSliced = false;         // the buffer holds the 9-slice mesh (16 vertices, not a 3x3 grid)
        // S45 R8: set while a legacy sprite draws from an atlas page; null otherwise (no cost)
        SpriteInlineAtlasRef inlineAtlas;

        // Sprite:playTag (R38): a sheet tag stepped by MeshSystem with the sheet's durations, no
        // frame limit; runtime, not saved
        std::string playingTag;           // "" = none
        std::vector<int> tagFrames;       // the tag's playback (direction and repeat unrolled)
        bool tagLoop = false;             // repeat 0: the sequence over and over
        bool tagPlaying = false;          // false: finished or stopped (playingTag stays the last tag)
        size_t tagStep = 0;
        double tagElapsedMs = 0.0;
        float tagSpeed = 1.0f;
    };

    // the texels per world unit a pixelsPerUnit of a sprite or a tilemap stands for (0: the
    // project's), always positive
    JASPERENGINE_API float resolvePixelsPerUnit(float pixelsPerUnit);
    // the sprite's quad in world units (its pixel size over its Pixels Per Unit): what bounds,
    // colliders fitted to it and world-size readers use
    JASPERENGINE_API Vector2 spriteWorldSize(const SpriteComponent& sprite);
    
}

#endif //SPRITE_COMPONENT_H