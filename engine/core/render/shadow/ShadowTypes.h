// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWTYPES_H
#define SHADOWTYPES_H

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>

namespace jasperengine {

    // Unity's Light.shadows: the user-facing view of LightComponent::shadows + softShadows
    enum class ShadowType : int {
        NONE,
        HARD,
        SOFT
    };

    // when a light's shadow maps are rendered (Unity HDRP's shadow update mode)
    enum class ShadowUpdateMode : int {
        AUTO,         // cached: redrawn only when a light, caster or fit change needs it
        EVERY_FRAME,  // always redrawn in full
        ON_DEMAND     // on enable, on a light or resolution change, and on requestShadowUpdate()
    };

    // how a directional light's cascades split its shadow distance
    enum class CascadeSplitMode : int {
        AUTO,    // log/uniform mix (lambda 0.7)
        MANUAL   // LightComponent::cascadeSplits, fractions of the shadow distance
    };

    // what a cached shadow slot does this frame (ShadowCache)
    enum class ShadowSlotPath : int {
        SKIP,             // its content is current: nothing is drawn
        RESTORE_DYNAMIC,  // static depth copied back inside the dynamic casters' rect, dynamic casters redrawn there
        RENDER_STATIC,    // static casters redrawn into the static cache, copied whole, dynamic casters drawn
        FULL,             // redrawn whole (no static layer)
        UNCACHED          // redrawn whole every frame (camera-dependent casters, Every Frame, caching off)
    };

    // why a slot was redrawn
    enum class ShadowDirtyCause : int {
        NONE,
        FIRST,           // never drawn (or its content was lost)
        LIGHT,           // a light setting the slot depends on
        FIT,             // its shadow camera moved (cascade refit, light moved)
        STATIC_CASTER,   // a static caster it holds changed
        DYNAMIC_CASTER,  // a dynamic caster it holds changed
        CAMERA_CASTER,   // it holds casters drawn per camera (terrain, billboards, culled instances)
        ATLAS,           // its atlas was recreated or its rect moved
        FORCED           // Every Frame, requestShadowUpdate(), the force_rerender debug switch
    };

    // An axis-aligned box as min/max (no allocation, unlike AABB's corner cache); valid false is
    // the empty box
    struct ShadowBounds {
        Vector3 min = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 max = Vector3(0.0f, 0.0f, 0.0f);
        bool valid = false;

        void merge(const ShadowBounds& other){
            if (!other.valid)
                return;
            if (!valid){
                *this = other;
                return;
            }
            min = Vector3(min.x < other.min.x ? min.x : other.min.x, min.y < other.min.y ? min.y : other.min.y, min.z < other.min.z ? min.z : other.min.z);
            max = Vector3(max.x > other.max.x ? max.x : other.max.x, max.y > other.max.y ? max.y : other.max.y, max.z > other.max.z ? max.z : other.max.z);
        }

        bool operator==(const ShadowBounds& other) const{
            if (valid != other.valid)
                return false;
            return !valid || (min == other.min && max == other.max);
        }
        bool operator!=(const ShadowBounds& other) const{ return !(*this == other); }
    };

    // lower-case names (get_shadow_stats): skip, restore, static, full, uncached
    JASPERENGINE_API const char* shadowSlotPathName(ShadowSlotPath path);
    // none, first, light, fit, static_caster, dynamic_caster, camera_caster, atlas, forced
    JASPERENGINE_API const char* shadowDirtyCauseName(ShadowDirtyCause cause);

}

#endif // SHADOWTYPES_H
