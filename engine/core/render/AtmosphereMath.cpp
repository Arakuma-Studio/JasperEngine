// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The height fog's closed-form optical depth (homogeneous below the base height, exponential
// above it) follows SanielX/Height-Fog (MIT, https://github.com/SanielX/Height-Fog, commit ab6f8278)
// as OpenEngine ported it (Engine/Modules/Rendering/Shaders/Includes/height_fog_term.glsl,
// MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors), rewritten as one
// antiderivative of the density over height so that any part of a ray composes exactly.

#include "AtmosphereMath.h"
#include "component/CameraComponent.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

HeightFogMedium AtmosphereMath::makeMedium(const FogComponent& fog){
    HeightFogMedium medium;
    medium.extinction = 1.0f / std::max(fog.attenuationDistance, 0.01f);
    medium.baseHeight = fog.baseHeight;
    medium.scaleHeight = scaleHeight(fog.baseHeight, fog.maximumHeight);
    medium.startDistance = std::max(fog.startDistance, 0.0f);
    medium.maxDistance = std::max(fog.maxFogDistance, medium.startDistance);
    medium.maxOpacity = std::clamp(fog.maxOpacity, 0.0f, 1.0f);
    return medium;
}

float AtmosphereMath::scaleHeight(float baseHeight, float maximumHeight){
    return std::max((maximumHeight - baseHeight) / LN_1000, 0.01f);
}

float AtmosphereMath::density(const HeightFogMedium& medium, float height){
    if (height <= medium.baseHeight)
        return 1.0f;
    return std::exp(-(height - medium.baseHeight) / medium.scaleHeight);
}

float AtmosphereMath::densityIntegral(const HeightFogMedium& medium, float height){
    const float above = height - medium.baseHeight;
    if (above <= 0.0f)
        return above;
    // H (1 - exp(-x / H)), written with expm1 for the precision of small x
    return -medium.scaleHeight * std::expm1(-above / medium.scaleHeight);
}

float AtmosphereMath::opticalDepthUnclipped(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1){
    const float length = t1 - t0;
    if (length <= 0.0f)
        return 0.0f;
    const float y0 = origin.y + t0 * dir.y;
    const float y1 = origin.y + t1 * dir.y;
    const float rise = y1 - y0;
    // a ray that keeps its height: the density at its middle (the error of the mean of the
    // density over a rise of 1e-3 scale heights is below 1e-7)
    if (std::abs(rise) < 1e-3f * medium.scaleHeight)
        return medium.extinction * length * density(medium, 0.5f * (y0 + y1));
    return medium.extinction * length * (densityIntegral(medium, y1) - densityIntegral(medium, y0)) / rise;
}

float AtmosphereMath::opticalDepth(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1){
    const float a = std::max(t0, medium.startDistance);
    const float b = std::min(t1, medium.maxDistance);
    return opticalDepthUnclipped(medium, origin, dir, a, b);
}

HeightFogSegment AtmosphereMath::segment(const HeightFogMedium& medium, const Vector3& origin, const Vector3& dir, float t0, float t1,
                                         const Vector3& inscattered){
    HeightFogSegment result;
    const float tau = opticalDepth(medium, origin, dir, t0, t1);
    result.transmittance = std::max(std::exp(-tau), 1.0f - medium.maxOpacity);
    result.scattering = inscattered * (1.0f - result.transmittance);
    return result;
}

float AtmosphereMath::phase(float anisotropy, float cosTheta){
    const float g = std::clamp(anisotropy, -0.95f, 0.95f);
    const float g2 = g * g;
    const float denominator = std::max(1.0f + g2 - 2.0f * g * cosTheta, 1e-5f);
    return (1.0f - g2) / (denominator * std::sqrt(denominator));
}

Vector3 AtmosphereMath::inscatteredLight(const Vector3& fogColor, const Vector3& sunColor, const Vector3& sunDirection,
                                         float directionalInscattering, float anisotropy, const Vector3& dir, float visibility){
    const float sun = directionalInscattering * phase(anisotropy, dir.dotProduct(sunDirection)) * visibility;
    return fogColor + sunColor * sun;
}

Vector3 AtmosphereMath::horizonRingColor(const Vector3 ring[4], const Vector3& dir){
    const float flat = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (flat < 1e-5f)
        return (ring[0] + ring[1] + ring[2] + ring[3]) * 0.25f;
    const float x = dir.x / flat;
    const float z = dir.z / flat;
    const float w[4] = {std::max(z, 0.0f), std::max(x, 0.0f), std::max(-z, 0.0f), std::max(-x, 0.0f)};
    const float sum = w[0] + w[1] + w[2] + w[3];
    return (ring[0] * w[0] + ring[1] * w[1] + ring[2] * w[2] + ring[3] * w[3]) / sum;
}

float AtmosphereMath::skyFillAmount(const Vector3& dir, float skyFillSin, float maxFill){
    if (skyFillSin <= 0.0f || maxFill <= 0.0f)
        return 0.0f;
    const float t = std::clamp(dir.y / skyFillSin, 0.0f, 1.0f);
    // 1 - smoothstep(0, sin(angle), y)
    return maxFill * (1.0f - t * t * (3.0f - 2.0f * t));
}

Vector2 AtmosphereMath::hemiOctahedralEncode(const Vector3& dir){
    const float y = std::max(dir.y, 0.0f);
    const float sum = std::abs(dir.x) + y + std::abs(dir.z);
    if (sum <= 1e-8f)
        return Vector2(0.5f, 0.5f);
    const float u = dir.x / sum;
    const float v = dir.z / sum;
    // the diamond |u| + |v| <= 1 turned 45 degrees onto the square
    return Vector2((u + v) * 0.5f + 0.5f, (u - v) * 0.5f + 0.5f);
}

Vector3 AtmosphereMath::hemiOctahedralDecode(const Vector2& uv){
    const float s = uv.x * 2.0f - 1.0f;
    const float t = uv.y * 2.0f - 1.0f;
    const float u = 0.5f * (s + t);
    const float v = 0.5f * (s - t);
    Vector3 dir(u, 1.0f - std::abs(u) - std::abs(v), v);
    return dir.normalized();
}

float AtmosphereMath::fogVolumeDensity(const FogVolumeData& volume, const Vector3& worldPoint){
    const Vector3 p = volume.worldToVolume * worldPoint;
    const Vector3& e = volume.halfExtents;
    float inside;
    if (volume.fog.shape == FogVolumeShape::SPHERE){
        const Vector3 q(p.x / std::max(e.x, 1e-4f), p.y / std::max(e.y, 1e-4f), p.z / std::max(e.z, 1e-4f));
        inside = (1.0f - q.length()) * std::min(e.x, std::min(e.y, e.z));
    }else{
        inside = std::min(e.x - std::abs(p.x), std::min(e.y - std::abs(p.y), e.z - std::abs(p.z)));
    }
    if (inside <= 0.0f)
        return 0.0f;
    const float blend = std::max(volume.fog.blendDistance, 1e-4f);
    const float t = std::min(inside / blend, 1.0f);
    if (volume.fog.falloff == FogVolumeFalloff::EXPONENTIAL){
        // 1 - exp(-4 t), rescaled to reach 1 at the blend distance
        return (1.0f - std::exp(-4.0f * t)) / (1.0f - std::exp(-4.0f));
    }
    return t;
}

int AtmosphereMath::selectFogVolumes(const std::vector<FogVolumeData>& volumes, const Plane frustumPlanes[6], float cameraFar,
                                     const Vector3& cameraPosition, float maxDistance, int cap, std::vector<int>& selected){
    selected.clear();
    struct Candidate{
        int index;
        float distance;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(volumes.size());
    for (int i = 0; i < (int)volumes.size(); i++){
        const FogVolumeData& volume = volumes[i];
        const float distance = std::max((volume.center - cameraPosition).length() - volume.boundingRadius, 0.0f);
        if (distance > maxDistance)
            continue;
        bool visible = true;
        for (int plane = 0; plane < 6 && visible; plane++){
            // without a far clip the far plane is not tested (RenderSystem::isInsideCamera)
            if (plane == FRUSTUM_PLANE_FAR && cameraFar == 0.0f)
                continue;
            if (frustumPlanes[plane].getDistance(volume.center) < -volume.boundingRadius)
                visible = false;
        }
        if (visible)
            candidates.push_back({i, distance});
    }
    std::sort(candidates.begin(), candidates.end(), [&volumes](const Candidate& a, const Candidate& b){
        if (a.distance != b.distance)
            return a.distance < b.distance;
        return volumes[a.index].entity < volumes[b.index].entity;
    });
    const int count = std::min((int)candidates.size(), std::max(cap, 0));
    for (int i = 0; i < count; i++)
        selected.push_back(candidates[i].index);
    return (int)volumes.size() - count;
}
