// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_MATH_H
#define ATMOSPHERE_MATH_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    // The height fog's medium (FogType::HEIGHT) in the form the shaders evaluate it
    // (shaderlib/includes/height_fog.glsl mirrors these functions one to one): extinction
    // sigma = 1 / attenuationDistance at full density; the density is 1 up to baseHeight and
    // exp(-(y - baseHeight) / scaleHeight) above it, scaleHeight = (maximumHeight -
    // baseHeight) / ln(1000); the fog covers the view ray from startDistance to maxDistance.
    struct HeightFogMedium{
        float extinction = 0.0f;
        float baseHeight = 0.0f;
        float scaleHeight = 1.0f;
        float startDistance = 0.0f;
        float maxDistance = 0.0f;
        float maxOpacity = 1.0f;
    };

    // the fog of a part of a view ray: what it adds (in-scattered radiance) and lets through
    struct HeightFogSegment{
        Vector3 scattering = Vector3(0.0f, 0.0f, 0.0f);
        float transmittance = 1.0f;
    };

    // One Local Volumetric Fog volume as the volumetric fog reads it: the world to volume
    // frame (rotation and translation; meters), its half extents in meters (size times the
    // Transform's scale, halved)
    struct FogVolumeData{
        uint32_t entity = 0;
        Matrix4 worldToVolume;
        Vector3 halfExtents;
        Vector3 center;           // world
        float boundingRadius = 0.0f;
        LocalVolumetricFogComponent fog;
    };

    class JASPERENGINE_API AtmosphereMath{
    public:
        static constexpr float LN_1000 = 6.907755279f;
        static constexpr float PI = 3.14159265358979f;

        static HeightFogMedium makeMedium(const FogComponent& fog);
        static float scaleHeight(float baseHeight, float maximumHeight);

        // relative density at a height (1 at and below the base)
        static float density(const HeightFogMedium& medium, float height);
        // the integral of density over height from the base height to height (an
        // antiderivative: negative below the base)
        static float densityIntegral(const HeightFogMedium& medium, float height);
        // the optical depth of origin + t dir (dir a unit vector) for t in [t0, t1], the
        // medium's start and max distances applied; additive over adjoining parts
        static float opticalDepth(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1);
        // the same without the start and max distances
        static float opticalDepthUnclipped(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1);
        // the fog over [t0, t1] lit by a radiance constant along it (inscattered): T = exp(-tau)
        // capped by the max opacity, S = inscattered (1 - T)
        static HeightFogSegment segment(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1,
                                        const Vector3& inscattered);

        // Henyey-Greenstein times 4 pi (1 for isotropic g = 0), g clamped to +-0.95;
        // cosTheta between the view direction and the direction towards the light
        static float phase(float anisotropy, float cosTheta);
        // the radiance the fog scatters towards the camera along dir: fogColor plus the sun's
        // share (sunColor linear times intensity, sunDirection towards the light)
        static Vector3 inscatteredLight(const Vector3& fogColor, const Vector3& sunColor, const Vector3& sunDirection,
                                        float directionalInscattering, float anisotropy, const Vector3& dir, float visibility);
        // the fog colour of the Sky colour mode: the horizon ring (azimuths +Z, +X, -Z, -X)
        // blended by the direction's azimuth
        static Vector3 horizonRingColor(const Vector3 ring[4], const Vector3& dir);
        // how much of the fog colour a sky pixel in direction dir takes (0..maxFill): all of it
        // below the horizon, none above skyFillAngle degrees
        static float skyFillAmount(const Vector3& dir, float skyFillSin, float maxFill);

        // the hemi-octahedral map of the sky (the Sky Map clouds): directions with y >= 0 to
        // uv in [0, 1]^2 and back (below the horizon: the horizon)
        static Vector2 hemiOctahedralEncode(const Vector3& dir);
        static Vector3 hemiOctahedralDecode(const Vector2& uv);

        // the density factor of a Local Volumetric Fog volume at a world point (0 outside, 1
        // inside past the blend distance; no noise)
        static float fogVolumeDensity(const FogVolumeData& volume, const Vector3& worldPoint);

        // The volumes a view's volumetric fog reads: those whose bounds touch the view
        // frustum (cameraFar 0: no far plane) and lie within maxDistance of the camera, the
        // nearest first (distance to the bounding sphere, entity order on ties), at most cap.
        // Writes their indices into selected and returns how many were left out (culled or
        // past the cap).
        static int selectFogVolumes(const std::vector<FogVolumeData>& volumes, const Plane frustumPlanes[6], float cameraFar,
                                    const Vector3& cameraPosition, float maxDistance, int cap, std::vector<int>& selected);
    };

}

#endif // ATMOSPHERE_MATH_H
