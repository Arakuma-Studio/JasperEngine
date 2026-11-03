// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCAL_VOLUMETRIC_FOG_COMPONENT_H
#define LOCAL_VOLUMETRIC_FOG_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    enum class FogVolumeShape{
        BOX,
        SPHERE
    };

    // how the density fades over the blend distance at the volume's edge
    enum class FogVolumeFalloff{
        LINEAR,
        EXPONENTIAL
    };

    // A volume of extra fog (Unity HDRP's Local Volumetric Fog): a box or a sphere of size
    // (local, times the Transform's scale, rotated with it) whose fog adds to the scene's
    // volumetric fog. It shows only through volumetric fog (a Fog of type Height with
    // Volumetric on); the volumes nearest the camera within the volumetric distance are used,
    // as many as the atmosphere quality allows (2, 4 or 8).
    struct JASPERENGINE_API LocalVolumetricFogComponent{
        FogVolumeShape shape = FogVolumeShape::BOX;
        Vector3 size = Vector3(10.0f, 10.0f, 10.0f);
        // the distance light travels in the fog before 63% of it is absorbed (meters)
        float attenuationDistance = 10.0f;
        // the colour the fog scatters (linear)
        Vector3 albedo = Vector3(1.0f, 1.0f, 1.0f);
        // the density fades in over this distance inside the edge (meters)
        float blendDistance = 1.0f;
        FogVolumeFalloff falloff = FogVolumeFalloff::LINEAR;
        // density noise: 0 none .. 1 full, its feature size (meters) and scroll (m/s)
        float noiseIntensity = 0.0f;
        float noiseScale = 5.0f;
        Vector3 scrollSpeed = Vector3(0.0f, 0.0f, 0.0f);
    };

}

#endif // LOCAL_VOLUMETRIC_FOG_COMPONENT_H
