// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWCACHE_H
#define SHADOWCACHE_H

#include "Export.h"
#include "math/Matrix4.h"
#include "render/shadow/ShadowCasterRegistry.h"
#include "render/shadow/ShadowTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // A rect of an atlas in texels (bottom-left origin, the viewport convention of the shadow passes)
    struct ShadowRect {
        int x = 0, y = 0, width = 0, height = 0;

        bool empty() const { return width <= 0 || height <= 0; }
        bool operator==(const ShadowRect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
        bool operator!=(const ShadowRect& o) const { return !(*this == o); }
        // the smallest rect holding both (an empty one adds nothing)
        ShadowRect united(const ShadowRect& o) const;
    };

    // What a slot's depth content is made of: equal keys mean an equal image (spec S10 R16).
    // Receiver-only settings (biases, strength, filter, fade) are never part of it.
    struct ShadowSlotKey {
        Matrix4 viewProjection;         // the shadow camera the slot is drawn with (depth range included)
        ShadowRect rect;                // where in the atlas (its size is the resolution)
        uint32_t casterMask = 0;        // the layers drawn into it (camera mask & light culling mask & caster layers)
        uint64_t lightRevision = 0;     // the light settings it depends on (LightComponent::shadowRevision)
        uint64_t atlasGeneration = 0;   // bumped when the atlas is recreated (content lost)
        bool cameraCasters = false;     // it holds casters drawn per camera: never cached

        bool operator==(const ShadowSlotKey& o) const;
        bool operator!=(const ShadowSlotKey& o) const { return !(*this == o); }
    };

    struct ShadowSlotInput {
        ShadowSlotKey key;
        ShadowUpdateMode mode = ShadowUpdateMode::AUTO;
        bool caching = true;            // the frame atlas caches; other cameras' atlases skip or redraw only
        bool hasStaticLayer = false;    // the slot has a rect in the static cache this frame
        ShadowRect staticRect;          // that rect
        uint64_t staticGeneration = 0;  // bumped when the static cache is recreated (content lost)
        bool requested = false;         // requestShadowUpdate(), or an On Demand light enabled
        bool forceRerender = false;     // set_shadow_debug force_rerender
        bool freezeCache = false;       // set_shadow_debug freeze_cache: keep every slot as it is
    };

    // What the shadow pass does with a slot this frame
    struct ShadowSlotDecision {
        ShadowSlotPath path = ShadowSlotPath::SKIP;
        ShadowDirtyCause cause = ShadowDirtyCause::NONE;
        ShadowRect dirtyRect;           // RESTORE_DYNAMIC: the atlas rect restored and redrawn (scissor)
        bool drawStatic = false;        // static casters are drawn (into the static cache under RENDER_STATIC)
        bool drawDynamic = false;       // dynamic casters are drawn into the atlas
        bool clearSlot = false;         // the atlas slot is cleared to far depth first (FULL, UNCACHED)
        bool copyStatic = false;        // the static layer is copied into the atlas (RESTORE_DYNAMIC: dirtyRect only)
    };

    // The per-slot memory of the cache
    struct ShadowSlotState {
        bool valid = false;             // the atlas slot holds key's content
        bool staticValid = false;       // the static cache holds the static casters of key
        ShadowSlotKey key;
        ShadowRect staticRect;
        uint64_t staticGeneration = 0;
        uint64_t lastRender = 0;        // frame of the last draw
        ShadowSlotPath lastPath = ShadowSlotPath::SKIP;
        ShadowDirtyCause lastCause = ShadowDirtyCause::NONE;
        ShadowRect lastDirtyRect;
    };

    // Decides, per shadow slot and frame, whether it is skipped, partly restored or redrawn (spec
    // S10 R17-R20): a pure function of the slot's memory, its key this frame and the casters that
    // changed. Pure CPU, no sokol.
    class JASPERENGINE_API ShadowCache {
    public:
        // decides and updates state as if the decision were carried out at frame
        static ShadowSlotDecision evaluate(ShadowSlotState& state, const ShadowSlotInput& input,
                                           const std::vector<ShadowCasterChange>& changes, uint64_t frame);

        // whether a world box can reach the shadow camera viewProjection draws (its clip volume)
        static bool intersects(const Matrix4& viewProjection, const ShadowBounds& bounds);
        // the atlas texels a world box covers in a slot drawn with viewProjection into rect, one
        // texel of margin around; the whole rect when the box crosses the camera plane of a
        // perspective shadow camera; empty when it misses
        static ShadowRect projectedRect(const Matrix4& viewProjection, const ShadowRect& rect, const ShadowBounds& bounds);
    };

}

#endif // SHADOWCACHE_H
