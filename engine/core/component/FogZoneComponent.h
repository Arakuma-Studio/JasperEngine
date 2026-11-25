// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_ZONE_COMPONENT_H
#define FOG_ZONE_COMPONENT_H

#include "math/Vector3.h"
#include "asset/AssetRef.h"

namespace jasperengine{

    enum class FogZoneShape{
        BOX,
        SPHERE
    };

    // A place with a fog look of its own (docs/specs/S101-vapor.md R9, R40): a box or a sphere of size
    // (local, times the Transform's scale, rotated with it, as Local Volumetric Fog) whose
    // profile's overrides blend over the scene's fog by where the camera is. The weight is
    // weight x clamp(1 - d / blendDistance, 0, 1), d the camera's distance to the shape (0
    // inside); blendDistance 0 switches at the surface. Zones apply in ascending priority, ties
    // by entity id. CPU only: the renderer sees the resolved values (render/FogResolve.h).
    struct JASPERENGINE_API FogZoneComponent{
        FogZoneShape shape = FogZoneShape::BOX;
        Vector3 size = Vector3(10.0f, 10.0f, 10.0f);
        float blendDistance = 5.0f;
        int priority = 0;
        float weight = 1.0f;
        // a Fog Profile (.fogprofile, assets-relative, or builtin:<id>); none: no effect
        AssetRef profile;
    };

}

#endif // FOG_ZONE_COMPONENT_H
