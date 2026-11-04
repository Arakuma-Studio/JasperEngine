// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainSurface.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::terrain;

float jasperengine::terrain::sampleHeightTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY) {
    if (!pixels || width <= 0 || height <= 0 || channels <= 0) {
        return 0.0f;
    }

    texelX = std::clamp(texelX, 0.0f, static_cast<float>(width - 1));
    texelY = std::clamp(texelY, 0.0f, static_cast<float>(height - 1));
    const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, width - 1);
    const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, height - 1);
    const int upperX = std::min(lowerX + 1, width - 1);
    const int upperY = std::min(lowerY + 1, height - 1);
    const float blendX = texelX - static_cast<float>(lowerX);
    const float blendY = texelY - static_cast<float>(lowerY);

    auto texel = [&](int x, int y) {
        return heightTexelAt(pixels, static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x), channels, bytesPerChannel);
    };

    const float lower = texel(lowerX, lowerY) + (texel(upperX, lowerY) - texel(lowerX, lowerY)) * blendX;
    const float upper = texel(lowerX, upperY) + (texel(upperX, upperY) - texel(lowerX, upperY)) * blendX;
    return lower + (upper - lower) * blendY;
}

float jasperengine::terrain::sampleHeightLocal(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ) {
    if (terrainSize <= std::numeric_limits<float>::epsilon()) {
        return 0.0f;
    }
    return sampleHeightTexels(pixels, width, height, channels, bytesPerChannel,
                              surfaceTexelCoord(localX, terrainSize, width), surfaceTexelCoord(localZ, terrainSize, height));
}

namespace {

    bool hasMap(const SurfaceView& view) {
        return view.pixels && view.width > 0 && view.height > 0 && view.channels > 0 &&
               view.size > std::numeric_limits<float>::epsilon();
    }

    // A NaN position would reach an int cast through the clamp, which is undefined
    float finite(float value) {
        return value == value ? value : 0.0f;
    }

    bool holeAt(const SurfaceView& view, float localX, float localZ) {
        if (!view.holes.texels || view.size <= std::numeric_limits<float>::epsilon()) return false;
        const float half = view.size * 0.5f;
        return isHole(view.holes, (localX + half) / view.size, (localZ + half) / view.size);
    }

    float heightAtLocal(const SurfaceView& view, float x, float z) {
        return view.maxHeight * sampleHeightLocal(view.pixels, view.width, view.height, view.channels, view.bytesPerChannel, view.size, x, z);
    }

}

SurfaceSample jasperengine::terrain::sampleSurface(const SurfaceView& view, float localX, float localZ) {
    SurfaceSample result;
    localX = finite(localX);
    localZ = finite(localZ);

    if (view.size > std::numeric_limits<float>::epsilon()) {
        const float half = view.size * 0.5f;
        result.inside = localX >= -half && localX <= half && localZ >= -half && localZ <= half;
        result.hole = holeAt(view, localX, localZ);
    }
    if (!hasMap(view)) {
        return result;
    }

    result.present = true;
    const float delta = view.size / static_cast<float>(view.width);
    result.height = heightAtLocal(view, localX, localZ);
    result.normal = Vector3(result.height - heightAtLocal(view, localX + delta, localZ), delta,
                            result.height - heightAtLocal(view, localX, localZ + delta)).normalized();
    return result;
}

float jasperengine::terrain::sampleHeight(const SurfaceView& view, float localX, float localZ) {
    if (!hasMap(view)) {
        return 0.0f;
    }
    return heightAtLocal(view, finite(localX), finite(localZ));
}

bool jasperengine::terrain::raycastSurface(const SurfaceView& view, const Vector3& origin, const Vector3& direction, float maxT,
                                           bool skipHoles, Vector3& point, float& t) {
    if (view.size <= std::numeric_limits<float>::epsilon()) {
        return false;
    }
    const bool map = hasMap(view);
    const float halfSize = view.size * 0.5f;

    auto surfaceHeight = [&](float localX, float localZ, float& sampled) {
        if (localX < -halfSize || localX > halfSize || localZ < -halfSize || localZ > halfSize) {
            return false;
        }
        sampled = map ? heightAtLocal(view, localX, localZ) : 0.0f;
        return true;
    };

    const float surfaceMinHeight = std::min(0.0f, view.maxHeight);
    const float surfaceMaxHeight = std::max(0.0f, view.maxHeight);

    float rayEntry = 0.0f;
    float rayExit = maxT;
    auto clipAxis = [&](float axisOrigin, float axisDirection, float minValue, float maxValue) {
        if (std::abs(axisDirection) <= std::numeric_limits<float>::epsilon()) {
            return axisOrigin >= minValue && axisOrigin <= maxValue;
        }

        float nearParameter = (minValue - axisOrigin) / axisDirection;
        float farParameter = (maxValue - axisOrigin) / axisDirection;
        if (nearParameter > farParameter) {
            std::swap(nearParameter, farParameter);
        }
        rayEntry = std::max(rayEntry, nearParameter);
        rayExit = std::min(rayExit, farParameter);
        return rayEntry <= rayExit;
    };

    if (!clipAxis(origin.x, direction.x, -halfSize, halfSize) ||
        !clipAxis(origin.y, direction.y, surfaceMinHeight, surfaceMaxHeight) ||
        !clipAxis(origin.z, direction.z, -halfSize, halfSize)) {
        return false;
    }

    auto pointAt = [&](float parameter) {
        return Vector3(origin.x + direction.x * parameter, origin.y + direction.y * parameter, origin.z + direction.z * parameter);
    };

    auto signedDistanceToSurface = [&](float rayParameter, float& signedDistance, Vector3& at) {
        at = pointAt(rayParameter);
        float sampled = 0.0f;
        if (!surfaceHeight(at.x, at.z, sampled)) {
            return false;
        }
        signedDistance = at.y - sampled;
        return true;
    };

    auto skipped = [&](const Vector3& at) {
        return skipHoles && holeAt(view, at.x, at.z);
    };

    const int raycastSteps = 160;
    const float surfaceEpsilon = std::max(0.001f, std::abs(surfaceMaxHeight - surfaceMinHeight) * 0.0005f);
    float previousParameter = rayEntry;
    float previousDistance = 0.0f;
    Vector3 previousPoint;
    if (!signedDistanceToSurface(previousParameter, previousDistance, previousPoint)) {
        return false;
    }

    // A point over a hole is no candidate when holes are skipped
    bool haveClosest = !skipped(previousPoint);
    float closestDistance = previousDistance;
    float closestParameter = previousParameter;
    Vector3 closestPoint = previousPoint;
    if (haveClosest && std::abs(previousDistance) <= surfaceEpsilon) {
        closestDistance = 0.0f;
    }

    for (int sampleIndex = 1; sampleIndex <= raycastSteps; sampleIndex++) {
        const float rayParameter = rayEntry + (rayExit - rayEntry) * (static_cast<float>(sampleIndex) / static_cast<float>(raycastSteps));
        float currentDistance = 0.0f;
        Vector3 currentPoint;
        if (!signedDistanceToSurface(rayParameter, currentDistance, currentPoint)) {
            continue;
        }

        if (!skipped(currentPoint) && (!haveClosest || std::abs(currentDistance) < std::abs(closestDistance))) {
            haveClosest = true;
            closestDistance = currentDistance;
            closestParameter = rayParameter;
            closestPoint = currentPoint;
        }

        const bool crossedSurface = (previousDistance <= 0.0f && currentDistance >= 0.0f) ||
                                    (previousDistance >= 0.0f && currentDistance <= 0.0f);
        if (crossedSurface) {
            float lowerParameter = previousParameter;
            float upperParameter = rayParameter;
            float lowerDistance = previousDistance;
            for (int refineIndex = 0; refineIndex < 12; refineIndex++) {
                const float midParameter = (lowerParameter + upperParameter) * 0.5f;
                float midDistance = 0.0f;
                Vector3 midPoint;
                if (!signedDistanceToSurface(midParameter, midDistance, midPoint)) {
                    break;
                }
                const bool sameSide = (lowerDistance <= 0.0f && midDistance <= 0.0f) ||
                                      (lowerDistance >= 0.0f && midDistance >= 0.0f);
                if (sameSide) {
                    lowerParameter = midParameter;
                    lowerDistance = midDistance;
                } else {
                    upperParameter = midParameter;
                }
            }
            const float crossing = (lowerParameter + upperParameter) * 0.5f;
            const Vector3 crossingPoint = pointAt(crossing);
            if (!skipped(crossingPoint)) {
                haveClosest = true;
                closestParameter = crossing;
                closestPoint = crossingPoint;
                closestDistance = 0.0f;
                break;
            }
            // through the hole: the ray goes on, and what it finds beyond is what it hit
        }

        previousParameter = rayParameter;
        previousDistance = currentDistance;
    }

    float localHeight = 0.0f;
    if (!haveClosest || std::abs(closestDistance) > surfaceEpsilon * 4.0f || !surfaceHeight(closestPoint.x, closestPoint.z, localHeight)) {
        return false;
    }

    point = Vector3(closestPoint.x, localHeight, closestPoint.z);
    t = closestParameter;
    return true;
}

SurfaceSample SurfaceViewQuery::sample(float localX, float localZ) const {
    return sampleSurface(view, localX, localZ);
}

float SurfaceViewQuery::heightAt(float localX, float localZ) const {
    return sampleHeight(view, localX, localZ);
}

bool SurfaceViewQuery::raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const {
    return raycastSurface(view, origin, direction, maxT, skipHoles, point, t);
}
