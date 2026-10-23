// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShadowCache.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

ShadowRect ShadowRect::united(const ShadowRect& o) const{
    if (o.empty())
        return *this;
    if (empty())
        return o;
    const int x0 = std::min(x, o.x);
    const int y0 = std::min(y, o.y);
    const int x1 = std::max(x + width, o.x + o.width);
    const int y1 = std::max(y + height, o.y + o.height);
    return ShadowRect{x0, y0, x1 - x0, y1 - y0};
}

bool ShadowSlotKey::operator==(const ShadowSlotKey& o) const{
    return viewProjection == o.viewProjection && rect == o.rect && casterMask == o.casterMask &&
        lightRevision == o.lightRevision && atlasGeneration == o.atlasGeneration && cameraCasters == o.cameraCasters;
}

bool ShadowCache::intersects(const Matrix4& viewProjection, const ShadowBounds& bounds){
    if (!bounds.valid)
        return false;
    // the clip volume -w <= x, y, z <= w as six planes of the rows of viewProjection (Gribb and
    // Hartmann); a box is out when its most positive corner is behind one of them
    const Matrix4& m = viewProjection;
    for (int axis = 0; axis < 3; axis++){
        for (int side = -1; side <= 1; side += 2){
            const float a = m[0][3] + (float)side * m[0][axis];
            const float b = m[1][3] + (float)side * m[1][axis];
            const float c = m[2][3] + (float)side * m[2][axis];
            const float d = m[3][3] + (float)side * m[3][axis];
            const float px = (a >= 0.0f) ? bounds.max.x : bounds.min.x;
            const float py = (b >= 0.0f) ? bounds.max.y : bounds.min.y;
            const float pz = (c >= 0.0f) ? bounds.max.z : bounds.min.z;
            if (a * px + b * py + c * pz + d < 0.0f)
                return false;
        }
    }
    return true;
}

ShadowRect ShadowCache::projectedRect(const Matrix4& viewProjection, const ShadowRect& rect, const ShadowBounds& bounds){
    if (!bounds.valid || rect.empty() || !intersects(viewProjection, bounds))
        return ShadowRect();

    float minX = 1.0f, minY = 1.0f, maxX = -1.0f, maxY = -1.0f;
    for (int i = 0; i < 8; i++){
        const Vector4 corner(
            (i & 1) ? bounds.max.x : bounds.min.x,
            (i & 2) ? bounds.max.y : bounds.min.y,
            (i & 4) ? bounds.max.z : bounds.min.z, 1.0f);
        const Vector4 clip = viewProjection * corner;
        if (!(clip.w > 1e-6f)){
            // across the plane of a perspective shadow camera: it can cover any of the slot
            return rect;
        }
        const float x = clip.x / clip.w;
        const float y = clip.y / clip.w;
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    minX = std::max(minX, -1.0f);
    minY = std::max(minY, -1.0f);
    maxX = std::min(maxX, 1.0f);
    maxY = std::min(maxY, 1.0f);
    if (minX > maxX || minY > maxY)
        return ShadowRect();

    // one texel of margin: rasterization of an edge on a texel boundary, depth bias slope
    const int x0 = std::max(0, (int)std::floor((minX * 0.5f + 0.5f) * (float)rect.width) - 1);
    const int y0 = std::max(0, (int)std::floor((minY * 0.5f + 0.5f) * (float)rect.height) - 1);
    const int x1 = std::min(rect.width, (int)std::ceil((maxX * 0.5f + 0.5f) * (float)rect.width) + 1);
    const int y1 = std::min(rect.height, (int)std::ceil((maxY * 0.5f + 0.5f) * (float)rect.height) + 1);
    if (x1 <= x0 || y1 <= y0)
        return ShadowRect();
    return ShadowRect{rect.x + x0, rect.y + y0, x1 - x0, y1 - y0};
}

ShadowSlotDecision ShadowCache::evaluate(ShadowSlotState& state, const ShadowSlotInput& input,
                                         const std::vector<ShadowCasterChange>& changes, uint64_t frame){
    const ShadowSlotKey& key = input.key;
    ShadowSlotDecision decision;

    // a static layer only in the cached frame atlas, and only for Auto lights
    const bool staticLayer = input.caching && input.hasStaticLayer && input.mode == ShadowUpdateMode::AUTO;

    // what changed in the slot's own setup
    ShadowDirtyCause setupCause = ShadowDirtyCause::NONE;
    if (!state.valid){
        setupCause = ShadowDirtyCause::FIRST;
    }else if (key.atlasGeneration != state.key.atlasGeneration || key.rect != state.key.rect){
        setupCause = ShadowDirtyCause::ATLAS;
    }else if (key.lightRevision != state.key.lightRevision || key.casterMask != state.key.casterMask){
        setupCause = ShadowDirtyCause::LIGHT;
    }else if (key.viewProjection != state.key.viewProjection){
        setupCause = ShadowDirtyCause::FIT;
    }else if (key.cameraCasters != state.key.cameraCasters){
        setupCause = ShadowDirtyCause::CAMERA_CASTER;
    }

    // the casters of the slot that changed, per layer
    bool staticChanged = false;
    bool dynamicChanged = false;
    ShadowRect dynamicRect;
    for (const ShadowCasterChange& change : changes){
        if (!(change.layerBits & key.casterMask))
            continue;
        const bool touchesOld = intersects(key.viewProjection, change.oldBounds);
        const bool touchesNew = intersects(key.viewProjection, change.newBounds);
        if (!touchesOld && !touchesNew)
            continue;
        if (change.staticLayer)
            staticChanged = true;
        if (change.dynamicLayer){
            dynamicChanged = true;
            if (touchesOld)
                dynamicRect = dynamicRect.united(projectedRect(key.viewProjection, key.rect, change.oldBounds));
            if (touchesNew)
                dynamicRect = dynamicRect.united(projectedRect(key.viewProjection, key.rect, change.newBounds));
        }
    }

    auto redraw = [&](ShadowDirtyCause cause){
        decision.cause = cause;
        decision.drawStatic = true;
        decision.drawDynamic = true;
        if (staticLayer){
            decision.path = ShadowSlotPath::RENDER_STATIC;
            decision.copyStatic = true;
            decision.dirtyRect = key.rect;
        }else{
            decision.path = ShadowSlotPath::FULL;
            decision.clearSlot = true;
            decision.dirtyRect = key.rect;
        }
    };
    auto uncached = [&](ShadowDirtyCause cause){
        decision.path = ShadowSlotPath::UNCACHED;
        decision.cause = cause;
        decision.drawStatic = true;
        decision.drawDynamic = true;
        decision.clearSlot = true;
        decision.dirtyRect = key.rect;
    };

    if (input.freezeCache && state.valid){
        // debug: hold what is drawn
    }else if (!input.caching){
        // another camera's atlas: kept while its key holds, redrawn whole otherwise
        if (key.cameraCasters){
            uncached(ShadowDirtyCause::CAMERA_CASTER);
        }else if (input.mode == ShadowUpdateMode::EVERY_FRAME){
            uncached(setupCause != ShadowDirtyCause::NONE ? setupCause : ShadowDirtyCause::FORCED);
        }else if (setupCause != ShadowDirtyCause::NONE){
            redraw(setupCause);
        }else if (input.forceRerender || input.requested){
            redraw(ShadowDirtyCause::FORCED);
        }else if (input.mode != ShadowUpdateMode::ON_DEMAND && (staticChanged || dynamicChanged)){
            redraw(staticChanged ? ShadowDirtyCause::STATIC_CASTER : ShadowDirtyCause::DYNAMIC_CASTER);
        }
    }else if (input.mode == ShadowUpdateMode::EVERY_FRAME){
        uncached(setupCause != ShadowDirtyCause::NONE ? setupCause : ShadowDirtyCause::FORCED);
    }else if (key.cameraCasters){
        uncached(setupCause != ShadowDirtyCause::NONE ? setupCause : ShadowDirtyCause::CAMERA_CASTER);
    }else if (setupCause != ShadowDirtyCause::NONE){
        redraw(setupCause);
    }else if (input.forceRerender || input.requested){
        redraw(ShadowDirtyCause::FORCED);
    }else if (input.mode == ShadowUpdateMode::ON_DEMAND){
        // caster changes wait for requestShadowUpdate()
    }else if (staticLayer && (!state.staticValid || state.staticRect != input.staticRect ||
                              state.staticGeneration != input.staticGeneration)){
        // the static layer is new, moved or lost: build it
        redraw(staticChanged ? ShadowDirtyCause::STATIC_CASTER : ShadowDirtyCause::ATLAS);
    }else if (staticChanged){
        redraw(ShadowDirtyCause::STATIC_CASTER);
    }else if (dynamicChanged){
        if (staticLayer && !dynamicRect.empty()){
            decision.path = ShadowSlotPath::RESTORE_DYNAMIC;
            decision.cause = ShadowDirtyCause::DYNAMIC_CASTER;
            decision.dirtyRect = dynamicRect;
            decision.copyStatic = true;
            decision.drawDynamic = true;
        }else{
            redraw(ShadowDirtyCause::DYNAMIC_CASTER);
        }
    }

    state.lastPath = decision.path;
    state.lastCause = decision.cause;
    if (decision.path == ShadowSlotPath::SKIP)
        return decision;

    state.valid = true;
    state.key = key;
    state.lastRender = frame;
    state.lastDirtyRect = decision.dirtyRect;
    if (decision.path == ShadowSlotPath::RENDER_STATIC){
        state.staticValid = true;
        state.staticRect = input.staticRect;
        state.staticGeneration = input.staticGeneration;
    }else if (decision.path != ShadowSlotPath::RESTORE_DYNAMIC){
        state.staticValid = false;
    }
    return decision;
}
